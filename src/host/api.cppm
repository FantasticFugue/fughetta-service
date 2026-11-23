export module host_api;
import std;
import async;
import configuration;
import constants;
import content;
import coroutine;
import host_operation;
import host_journal;
import http;
import host;
import json;
import keyring;
import native;

export namespace host_api {
    using namespace host_operation;

    inline auto linux_arch() -> std::string_view {
        return host::arch == "arm64" ? "aarch64" : host::arch;
    }

    using provisioner = std::function<coroutine::task<observed>(const operation&)>;

    auto error(http::status status, std::string_view message) -> web::reply {
        return web::json_reply(status, json::write(json::object{{"error", message}}));
    }

    auto pending(std::string_view id) -> web::reply {
        return web::json_reply(202, json::write(json::object{{"operation_id", id}}));
    }

    struct inventory {
        std::string host_id;
        config::host_limits capacity;
        std::vector<observed> environments;
        std::vector<warm_environment> warm;
    };

    class server {
        native::scheduler io_;
        const config::host_settings& settings_;
        std::string digest_;
        host_journal::journal journal_;
        provisioner provision_;
        coroutine::mutex resources_;
        std::set<std::string, std::less<>> active_;
        std::chrono::milliseconds answer_limit_;

        auto execute(const operation& request) -> coroutine::task<> {
            const auto& id = request.operation_id;
            struct tracked {
                std::set<std::string, std::less<>>& active;
                const std::string& id;
                ~tracked() { active.erase(id); }
            } running{active_, id};
            try {
                auto lock = co_await resources_.lock();
                auto actual = co_await coroutine::deadline(io_, std::chrono::seconds{settings_.operation_timeout_seconds}, provision_(request));
                if (!actual)
                    throw std::runtime_error("host operation timed out");
                if (actual->environment_id != request.environment_id || actual->operation_id != id ||
                    actual->state != request.desired || !actual->volume_fenced ||
                    actual->work_volume.has_value() != request.work_volume.has_value() ||
                    (request.work_volume && (actual->work_volume->device != request.work_volume->device ||
                     actual->work_volume->size_bytes < std::max(request.work_volume->size_bytes, request.contract.limits.storage_bytes))) ||
                    (actual->state == environment_state::running &&
                     (actual->instance_id.empty() || !actual->toolchain_read_only || actual->applied != std::optional{request.contract} ||
                      (request.kind == environment_kind::core && request.contract.boundary.network == "private" && actual->address.empty()))))
                    throw std::runtime_error("provisioner did not confirm the requested environment");
                if (request.action == verb::claim) {
                    const auto* warm = journal_.find(request.environment_id + "-1");
                    if (actual->instance_id != warm->result->instance_id)
                        throw std::runtime_error("claim replaced the warm Worker task");
                }
                journal_.complete(id, std::move(*actual));
            } catch (const conflict& failure) {
                journal_.fail(id, {409, failure.what()});
            } catch (const std::invalid_argument& failure) {
                journal_.fail(id, {400, failure.what()});
            } catch (const std::exception& failure) {
                journal_.fail(id, {500, failure.what()});
            }
        }

        void launch(const host_journal::record& entry, coroutine::scope& operations) {
            const auto& id = entry.request.operation_id;
            if (journal_.latest(entry) && !entry.result && !active_.contains(id)) {
                journal_.retry(id);
                active_.insert(id);
                operations.spawn(execute(entry.request));
            }
        }

        auto result(const host_journal::record& entry) const -> web::reply {
            if (entry.result)
                return web::json_reply(200, config::write(*entry.result));
            if (entry.failure)
                return error(http::status::from_number(entry.failure->status).value_or(http::status{500}), entry.failure->message);
            return pending(entry.request.operation_id);
        }

        auto submit(std::string_view body, std::chrono::steady_clock::time_point until, coroutine::scope& operations) -> coroutine::task<web::reply> {
            try {
                auto op = config::parse_as<operation>(body, "host operation");
                validate(op);
                if (op.host_id != settings_.host_id)
                    throw std::invalid_argument("operation names a different host");
                if (op.work_volume)
                    validate_device(*op.work_volume, settings_.volume_devices);
                if (op.desired == environment_state::running) {
                    const auto& contract = op.contract;
                    const auto& limits = contract.limits;
                    const auto& capacity = settings_.capacity;
                    if (limits.cpu_millis > capacity.cpu_millis || limits.memory_bytes > capacity.memory_bytes ||
                        limits.storage_bytes > capacity.storage_bytes)
                        throw std::invalid_argument("environment limits exceed host capacity");
                    if (contract.os != "linux" || contract.arch != linux_arch() || runtime_for(contract.boundary, settings_.isolation).empty() ||
                        (contract.boundary.network != "none" && contract.boundary.network != "private") ||
                        limits.storage_bytes < constants::host_minimum_storage || op.overflow_wait_seconds > 600)
                        throw std::invalid_argument("host cannot enforce the requested environment");
                    if (op.kind == environment_kind::overflow && op.action != verb::warm &&
                        (contract.boundary.network != "private" || op.core_address.empty() || io::parse_endpoint(op.core_address).port() == 0))
                        throw std::invalid_argument("overflow Worker needs a private Core endpoint");
                    for (const auto& endpoint : op.egress)
                        config::tcp_egress(endpoint);
                    if (!op.core_address.empty())
                        config::tcp_egress(op.core_address);
                }
                const auto& entry = journal_.accept(std::move(op));
                launch(entry, operations);
                while (active_.contains(entry.request.operation_id)) {
                    const auto remaining = until - std::chrono::steady_clock::now();
                    if (remaining <= std::chrono::steady_clock::duration::zero())
                        break;
                    co_await execution::schedule_after(io_, std::min(remaining, std::chrono::steady_clock::duration{std::chrono::milliseconds{25}}));
                }
                co_return result(entry);
            } catch (const conflict& failure) {
                co_return error(409, failure.what());
            } catch (const std::invalid_argument& failure) {
                co_return error(400, failure.what());
            } catch (const std::exception& failure) {
                co_return error(500, failure.what());
            }
        }

        auto authorized(const http::request& request) const -> bool {
            const auto token = keyring::bearer(web::header(request, "authorization").value_or(""));
            return token && content::secure_equal(digest_, keyring::hash(*token));
        }

        template <class Body>
        auto route(const http::request& request, Body& input, coroutine::scope& operations) -> coroutine::task<web::reply> {
            const auto until = std::chrono::steady_clock::now() + answer_limit_;
            const auto method = request.method.token();
            const auto target = std::string_view{request.target};
            if (method == "GET" && target == "/healthz")
                co_return web::json_reply(200, "{}");
            if (!authorized(request))
                co_return error(401, "send the host bearer token");
            if (method == "GET" && target == "/v1/inventory")
                co_return web::json_reply(200, config::write(inventory{settings_.host_id, settings_.capacity, journal_.inventory(), journal_.warm()}));
            if (method == "GET" && target.starts_with('/'))
                if (const auto args = web::path<"/v1/operations/{operation_id}">::match(target))
                    if (const auto* entry = journal_.find(args->front()))
                        co_return result(*entry);
            if (method == "POST" && target == "/v1/operations") {
                try {
                    auto body = co_await web::read_all(input);
                    co_return co_await submit(body, until, operations);
                } catch (const std::length_error& failure) {
                    co_return error(400, failure.what());
                }
            }
            co_return error(404, "not found");
        }

        auto serve(native::socket peer, coroutine::scope& operations) -> coroutine::task<> {
            web::stream<native::socket> transport{peer};
            http::v1::server<web::stream<native::socket>> connection{transport};
            try {
                auto in = co_await http::coroutine::receive(connection);
                auto reply = co_await route(in.head, in.body, operations);
                reply.head.fields.push_back({"connection", "close"});
                co_await web::respond(connection, std::move(in.token), std::move(reply.head), std::move(reply.body));
                // Let the client receive the response even if it is still sending request content.
                peer.shutdown(net::socket_base::shutdown_send);
                auto drain = [&peer]() -> coroutine::task<> {
                    std::array<std::byte, 4096> buffer;
                    while (co_await io::async_read_some(peer, net::buffer(buffer))) {}
                };
                co_await coroutine::deadline(io_, std::chrono::seconds{1}, drain());
            } catch (const std::exception&) {
            }
        }

    public:
        server(native::scheduler io, const config::host_settings& settings, provisioner provision,
               std::chrono::milliseconds answer_limit = constants::host_answer_limit)
            : io_(io), settings_(settings), digest_(keyring::hash(settings.token)),
              journal_(std::filesystem::path{settings.storage} / "operations"), provision_(std::move(provision)), answer_limit_(answer_limit) {}

        auto run(native::listener& listener) -> coroutine::task<> {
            auto accept = [&](coroutine::scope& operations) -> coroutine::task<> {
                for (const auto* entry : journal_.pending())
                    launch(*entry, operations);
                auto connections = [&](coroutine::scope& clients) -> coroutine::task<> {
                    for (;;) {
                        auto [peer, endpoint] = co_await net::async_accept(listener);
                        clients.spawn(serve(std::move(peer), operations));
                    }
                };
                co_await coroutine::with_scope(std::move(connections));
            };
            co_await coroutine::with_scope(std::move(accept));
        }
    };
}
