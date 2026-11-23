import std;
import async;
import configuration;
import coroutine;
import host_api;
import host;
import host_operation;
import http;
import native;
import support;

namespace {
    using namespace host_operation;
    using namespace std::chrono_literals;

    void require(bool condition, std::string_view message) {
        if (!condition)
            throw std::runtime_error(std::string{message});
    }

    auto request(native::scheduler& io, const native::listener& listener, http::method method, std::string path,
                 std::string body = {}, std::string token = "Bearer host-token") -> coroutine::task<web::reply> {
        auto socket = co_await io::connect(io, listener.local_endpoint());
        web::stream<native::socket> transport{socket};
        http::v1::client client{transport};
        std::vector<http::header> headers;
        if (!token.empty())
            headers.push_back({"authorization", std::move(token)});
        http::request head{.method = method, .target = path, .scheme = "http",
                           .authority = "localhost", .fields = std::move(headers)};
        try {
            auto response = co_await http::coroutine::fetch(client, std::move(head), std::string_view{body});
            co_return web::reply{std::move(response.head), std::string{response.text()}};
        } catch (const std::exception& failure) {
            throw std::runtime_error(std::format("{} {}: {}", method.token(), path, failure.what()));
        }
    }

    auto exercise(native::scheduler& io, const std::filesystem::path& storage) -> coroutine::task<> {
        constexpr auto get = http::method::GET;
        constexpr auto post = http::method::POST;
        config::host_settings settings;
        settings.host_id = "example";
        settings.token = "host-token";
        settings.storage = storage.string();
        settings.isolation = {{std::string{config::runc}}, true, {}};
        settings.capacity = {1000, 128u << 20, 64u << 20};
        auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};

        operation start;
        start.workspace_id = "example-workspace";
        start.host_id = settings.host_id;
        start.kind = environment_kind::core;
        start.desired = environment_state::running;
        start.environment_id = name(start.workspace_id, start.kind);
        start.operation_id = start.environment_id + "-1";
        start.contract = {.os = "linux", .arch = std::string{host_api::linux_arch()}, .toolchain = "test",
                          .boundary = {.kind = "container", .shared_kernel = true, .network = "none"},
                          .limits = {600, 64u << 20, 32u << 20}, .toolchain_root = "/tools", .work_root = "/work/core"};
        start.work_volume = volume{"/dev/disk/by-id/scsi-0HC_Volume_123", 32u << 20};
        auto stop = start;
        stop.operation_id = stop.environment_id + "-2";
        stop.desired = environment_state::stopped;
        stop.work_volume.reset();
        auto changed = start;
        changed.contract.limits.cpu_millis = 500;
        const auto path = "/v1/operations/" + start.operation_id;
        std::string recorded;
        unsigned calls = 0;
        bool release = false;
        bool fail_claim = false;
        auto provision = [&](const operation& op) -> coroutine::task<observed> {
            ++calls;
            while (!release)
                co_await execution::schedule_after(io, 5ms);
            if (fail_claim && op.action == verb::claim)
                throw std::runtime_error("fixture could not publish the claim");
            observed result;
            result.environment_id = op.environment_id;
            result.operation_id = op.operation_id;
            result.state = op.desired;
            result.volume_fenced = true;
            if (op.desired == environment_state::running) {
                result.instance_id = "fixture";
                result.toolchain_read_only = true;
                result.applied = op.contract;
                result.work_volume = op.work_volume;
            }
            co_return result;
        };
        {
            host_api::server api{io, settings, provision, 20ms};
            auto run = [&](coroutine::scope& scope) -> coroutine::task<> {
                scope.spawn(api.run(listener));
                require((co_await request(io, listener, get, "/healthz", {}, "")).head.status == 200, "health is public");
                require((co_await request(io, listener, get, "/v1/inventory", {}, "")).head.status == 401, "missing bearer was accepted");
                require((co_await request(io, listener, post, "/v1/operations", "{}", "Bearer bad-token")).head.status == 401,
                        "invalid bearer was accepted");
                require((co_await request(io, listener, get, path)).head.status == 404, "unknown operation was found");
                require((co_await request(io, listener, post, "/v1/operations", "{}")).head.status == 400, "invalid operation was accepted");
                auto invalid = start;
                for (const auto* device : {"scsi-volume", "/dev/disk/by-id-extra/scsi-volume", "/dev/disk/by-id",
                                           "/dev/disk/by-id/../scsi-volume", "/dev/disk/by-id/./scsi-volume"}) {
                    invalid.work_volume->device = device;
                    require((co_await request(io, listener, post, "/v1/operations", config::write(invalid))).head.status == 400,
                            "invalid device path was accepted");
                }
                invalid = start;
                invalid.work_volume->size_bytes = 0;
                require((co_await request(io, listener, post, "/v1/operations", config::write(invalid))).head.status == 400,
                        "zero-size volume was accepted");
                invalid = stop;
                invalid.work_volume = start.work_volume;
                require((co_await request(io, listener, post, "/v1/operations", config::write(invalid))).head.status == 400,
                        "stop accepted a volume attachment");
                const auto accepted = co_await request(io, listener, post, "/v1/operations", config::write(start));
                require(accepted.head.status == 202 && config::parse_as<std::map<std::string, std::string>>(accepted.body, "pending").at("operation_id") ==
                        start.operation_id, "start did not return its pending identity");
                require((co_await request(io, listener, get, path)).head.status == 202, "in-flight operation was not visible");
                require((co_await request(io, listener, post, "/v1/operations", config::write(start))).head.status == 202 && calls == 1,
                        "concurrent replay dispatched the provisioner again");
                require((co_await request(io, listener, post, "/v1/operations", config::write(changed))).head.status == 409, "different input reused its ID");
                require((co_await request(io, listener, post, "/v1/operations", config::write(stop))).head.status == 409, "stop overtook its start");
                release = true;
                auto complete = co_await request(io, listener, post, "/v1/operations", config::write(start));
                require(complete.head.status == 200 && calls == 1, "accepted operation did not finish");
                recorded = std::move(complete.body);
            };
            co_await coroutine::with_scope(std::move(run));
        }

        host_api::server restarted{io, settings, provision};
        auto run = [&](coroutine::scope& scope) -> coroutine::task<> {
            scope.spawn(restarted.run(listener));
            const auto replay = co_await request(io, listener, post, "/v1/operations", config::write(start));
            require(replay.head.status == 200 && replay.body == recorded && calls == 1, "durable replay changed the result");
            require((co_await request(io, listener, get, path)).body == recorded, "lookup did not return the durable result");
            require((co_await request(io, listener, post, "/v1/operations", config::write(changed))).head.status == 409, "durable inputs changed");
            auto attached = co_await request(io, listener, get, "/v1/inventory");
            require(config::parse_as<host_api::inventory>(attached.body, "inventory").environments.front().work_volume == start.work_volume,
                    "inventory omitted the attached volume");
            require((co_await request(io, listener, post, "/v1/operations", config::write(stop))).head.status == 200 && calls == 2,
                    "stop failed after restart");
            const auto response = co_await request(io, listener, get, "/v1/inventory");
            const auto inventory = config::parse_as<host_api::inventory>(response.body, "inventory");
            require(response.head.status == 200 && inventory.host_id == settings.host_id && inventory.capacity == settings.capacity &&
                    inventory.warm.empty() && inventory.environments.size() == 1 &&
                    inventory.environments.front().operation_id == stop.operation_id &&
                    inventory.environments.front().state == environment_state::stopped &&
                    !inventory.environments.front().work_volume, "inventory did not reflect the latest observation");
            require((co_await request(io, listener, get, path)).body == recorded, "stop erased the start result");

            auto warm = start;
            warm.work_volume.reset();
            warm.workspace_id.clear();
            warm.kind = environment_kind::overflow;
            warm.action = verb::warm;
            warm.profile = "linux";
            warm.environment_id = "warm-" + std::string(64, 'a');
            warm.operation_id = warm.environment_id + "-1";
            warm.contract.boundary.network = "private";
            warm.contract.work_root.clear();
            auto claim = warm;
            claim.action = verb::claim;
            claim.operation_id = claim.environment_id + "-2";
            claim.workspace_id = start.workspace_id;
            claim.contract.work_root = "/work/worker";
            claim.core_address = "10.203.0.6:9090";
            auto other = claim;
            other.workspace_id = "another-workspace";
            auto destroy = claim;
            destroy.action = verb::destroy;
            destroy.operation_id = destroy.environment_id + "-3";
            destroy.desired = environment_state::stopped;
            auto submit = [&](const operation& op) {
                return request(io, listener, post, "/v1/operations", config::write(op));
            };
            auto read_inventory = [&]() -> coroutine::task<host_api::inventory> {
                auto response = co_await request(io, listener, get, "/v1/inventory");
                require(response.head.status == 200, "inventory request failed");
                co_return config::parse_as<host_api::inventory>(response.body, "inventory");
            };
            require((co_await submit(claim)).head.status == 409, "claim without warm was accepted");
            require((co_await submit(warm)).head.status == 200, "warm failed");
            const auto idle = co_await read_inventory();
            require(idle.warm.size() == 1 && idle.warm.front().environment_id == warm.environment_id &&
                    idle.warm.front().profile == warm.profile && idle.warm.front().state == environment_state::running,
                    "warm inventory omitted the Worker");
            const auto claimed = co_await submit(claim);
            require(claimed.head.status == 200 && (co_await read_inventory()).warm.empty(), "claim did not remove the warm entry");
            require((co_await submit(other)).head.status == 409, "claimed Worker accepted a second workspace");
            require((co_await submit(claim)).body == claimed.body && calls == 4, "claim replay called the provisioner");
            require((co_await submit(destroy)).head.status == 200 && calls == 5, "claimed Worker destroy failed");
            require((co_await submit(other)).head.status == 409 && (co_await read_inventory()).warm.empty(),
                    "destroyed Worker could be reassigned");

            warm.environment_id = "warm-" + std::string(64, 'b');
            warm.operation_id = warm.environment_id + "-1";
            destroy = warm;
            destroy.action = verb::destroy;
            destroy.operation_id = destroy.environment_id + "-2";
            destroy.desired = environment_state::stopped;
            require((co_await submit(warm)).head.status == 200 && (co_await submit(destroy)).head.status == 200 &&
                    (co_await read_inventory()).warm.empty(), "unclaimed Worker destroy failed");

            warm.environment_id = "warm-" + std::string(64, 'c');
            warm.operation_id = warm.environment_id + "-1";
            claim.environment_id = warm.environment_id;
            claim.operation_id = claim.environment_id + "-2";
            destroy = claim;
            destroy.action = verb::destroy;
            destroy.operation_id = claim.environment_id + "-3";
            destroy.desired = environment_state::stopped;
            fail_claim = true;
            require((co_await submit(warm)).head.status == 200 && (co_await submit(claim)).head.status == 500 &&
                    (co_await read_inventory()).warm.empty(), "failed claim left the Worker available");
            require((co_await submit(destroy)).head.status == 200, "failed claim could not be destroyed");
            const auto complete_calls = calls;
            require((co_await submit(claim)).head.status == 500 && calls == complete_calls, "replay retried a destroyed Worker");
        };
        co_await coroutine::with_scope(std::move(run));
    }
}

int main() {
    try {
        const test::scratch scratch{"host-api"};
        native::context context;
        auto io = context.get_scheduler();
        cores::run(context, exercise(io, scratch.dir));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
