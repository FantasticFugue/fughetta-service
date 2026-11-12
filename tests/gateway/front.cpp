import std;
import boost.ut;
import coroutine;
import native;
import http;
import configuration;
import content;
import reflection;
import nodes;
import registry;
import tools;
import mcp;
import fixture;
import commands;
import fleet;
import front;
import world;
import keyring;
import usage;
import support;
import enrollment;
import protocol;

namespace {
    using namespace boost::ut;
    using ctx = io::scheduler_of_t<native::context>;
    using socket = native::socket;

    struct answer {
        http::status status = 0;
        std::string body;
        std::string content_type;
        std::string allow_origin;
    };

    constexpr auto PUT = http::method::PUT;
    constexpr auto GET = http::method::GET;
    constexpr auto DELETE = http::method::DELETE_;

    struct harness {
        std::string mode;
        test::scratch tmp{"front"};
        cores::io_context<native::context> rt{1};
        routing::router router{rt};
        execution::thread_pool pool{2};
        front::config_hub hub{rt,
                              config::store{config::parse(std::format(R"({{
      "server": {{ "allowed_origins": ["http://ok.example"], "max_request_bytes": 4096, "storage": "{}",
                   "auth": {{ "mode": "{}", "workspace": "{}", "tokens": [{{ "name": "tester", "hash": "{}" }}] }} }}
    }})",
                                                                      (tmp.dir / "storage").generic_string(), mode, mode == "trusted" ? "acme-id" : "",
                                                                      keyring::hash("sk_test"))),
                                            tmp.dir / "gateway.json"},
                              pool};
        test::source_service source{tmp.dir / "source"};
        storage::registry files{rt, tmp.dir / "storage", hub, pool, source.endpoint()};
        tools::services services{hub.slot(0), router, files, 0};
        tools::backlog queue{rt, services};
        std::chrono::milliseconds heartbeat;
        front::server server{front::context{hub, files, router, rt, 0, nullptr, nullptr, nullptr, &services}, services, heartbeat};
        explicit harness(std::string mode = "token", std::chrono::milliseconds interval = std::chrono::seconds{15})
            : mode(std::move(mode)), heartbeat(interval) {
            services.queue = &queue;
        }
        std::uint16_t port = 0;

        auto ask(ctx& io, http::method method, std::string target, std::string body = {}, std::vector<http::header> fields = {}, bool authenticated = true)
            -> coroutine::task<answer> {
            auto s = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, port});
            test::http_stream transport{s};
            if (authenticated)
                fields.push_back({"authorization", "Bearer sk_test"});
            http::request req{.method = method, .target = std::move(target), .scheme = "http", .authority = "localhost", .fields = std::move(fields)};
            http::v1::client client{transport};
            auto got = co_await http::coroutine::fetch(client, std::move(req), std::string_view{body});
            s.close();
            co_return answer{got.head.status, std::string{got.text()}, std::string{http::find_header(got.head.fields, "content-type").value_or("")},
                             std::string{http::find_header(got.head.fields, "access-control-allow-origin").value_or("")}};
        }

        auto save(ctx& io, std::string path, std::string_view value, std::vector<http::header> fields = {}) -> coroutine::task<answer> {
            auto current = co_await ask(io, GET, std::string{path}, {}, std::move(fields));
            // A project that does not exist yet is created from revision 0.
            auto at = current.status == 200 ? current.body.find(R"("revision":)") : std::string::npos;
            auto revision = at == std::string::npos ? std::uint64_t{0} : std::stoull(current.body.substr(at + 11));
            co_return co_await ask(io, PUT, std::move(path), std::format(R"({{"revision":{},"value":{}}})", revision, value));
        }

        auto mcp(ctx& io, std::string_view json, std::vector<http::header> fields = {}) -> coroutine::task<answer> {
            fields.push_back({"content-type", "application/json"});
            co_return co_await ask(io, http::method::POST, "/mcp", std::string{json}, std::move(fields));
        }
    };

    auto call(std::string_view tool, std::string_view args, std::string_view meta = "") -> std::string {
        std::string selected{args};
        if (tool == "worldline" || tool == "language" || tool == "scope" || tool == "build" || tool == "test" || tool == "run") {
            auto object = rfl::json::read<rfl::Generic>(selected)->to_object().value();
            if (!object.get("worldline"))
                object["worldline"] = "main";
            selected = rfl::json::write(object);
        }
        return std::format(R"({{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{{"name":"{}","arguments":{}{}}}}})", tool, selected,
                           meta.empty() ? "" : std::format(R"(,"_meta":{})", meta));
    }

    static constexpr auto front_suite = [] {
        "export downloads require project grants including shared build cache hits"_test = [] {
            using namespace std::chrono_literals;
            harness h;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                h.port = listener.local_endpoint().port();
                auto workers = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto connection = co_await io::connect(io, workers.local_endpoint());
                auto [peer, endpoint] = co_await net::async_accept(workers);
                transport::channel<socket> channel{connection};
                transport::dispatcher<socket> dispatch;
                const std::string data = "verified artifact";
                const auto digest = content::hash_of(data);
                const std::string log = "complete compiler log";
                const auto log_digest = content::hash_of(log);
                int builds = 0, downloads = 0;
                bool fail_download = false;
                bool remove_project = false;
                static constexpr auto handle_sync_offer = [](jobs::sync_offer offer, auto&) -> coroutine::task<jobs::sync_plan> {
                    co_return jobs::sync_plan{.snapshot = offer.tree.id()};
                };
                dispatch.on<jobs::sync_offer>(handle_sync_offer);
                auto handle_build_request = [&](jobs::build_request request, auto&) -> coroutine::task<jobs::build_report> {
                    ++builds;
                    expect(request.exports.at("binary") == "out/app");
                    if (remove_project)
                        expect((co_await h.ask(io, DELETE, "/api/v1/projects/app")).status == 200);
                    co_return jobs::build_report{.steps = {{.name = "compile",
                                                            .exit_code = 0,
                                                            .output = "log",
                                                            .truncated = true,
                                                            .log = builds::exported_file{"step-0.log", "0.log", log_digest, log.size(), false}}},
                                                 .exports = {{"binary", "app", digest, data.size(), true}}};
                };
                dispatch.on<jobs::build_request>(std::move(handle_build_request));
                auto handle_export_read = [&](jobs::export_read request, auto&) -> coroutine::task<jobs::source_bytes> {
                    ++downloads;
                    if (fail_download)
                        throw std::runtime_error("artifact transport unavailable");
                    const auto& value = request.digest == log_digest ? log : data;
                    auto part = std::as_bytes(std::span{value}).subspan(request.offset, request.length);
                    co_return jobs::source_bytes{{{request.digest, request.offset, value.size(), {part.begin(), part.end()}}}};
                };
                dispatch.on<jobs::export_read>(std::move(handle_export_read));
                co_await channel.post(
                    jobs::hello{.name = "export-builder", .protocol = jobs::protocol_version, .tools = {"true"}, .toolchain_id = "export-toolchain"});
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    static constexpr auto run_child_2 = [](harness& h, native::listener& listener, coroutine::scope& children) -> coroutine::task<> {
                        for (;;) {
                            auto [connection, peer] = co_await net::async_accept(listener);
                            children.spawn(h.server.serve(std::move(connection)));
                        }
                    };
                    children.spawn(run_child_2(h, listener, children));
                    children.spawn(h.router.workers(0).admit(io, std::move(peer)));
                    static constexpr auto run_child = [](auto& channel, auto& dispatch, coroutine::scope& children) -> coroutine::task<> {
                        for (;;)
                            children.spawn(dispatch.handle(channel, co_await channel.recv()));
                    };
                    children.spawn(run_child(channel, dispatch, children));
                    children.spawn(h.queue.serve(io));
                    while (h.router.workers(0).describe(0).empty())
                        co_await execution::schedule_after(io, 1ms);
                    config::settings project;
                    project.build_configurations["native"] = fixture::commands({"true"});
                    project.build_configurations["native"].operations["build"].exports = {{"binary", "out/app"}};
                    project.default_configurations = {"native"};
                    for (auto name : {"app", "other"}) {
                        const auto saved = co_await h.save(io, std::string{"/api/v1/projects/"} + name, config::write(project));
                        expect(saved.status == 200) << saved.body;
                    }
                    const auto path = "/api/v1/projects/app/exports/" + digest.str();
                    expect((co_await h.ask(io, GET, path)).status != 200);
                    const auto first = co_await h.mcp(io, call("build", R"({"project":"app"})"));
                    expect(first.status == 200 && first.body.contains(digest.str())) << first.body;
                    expect(builds == 1);
                    const auto log_reply = co_await h.ask(io, GET, "/api/v1/projects/app/exports/" + log_digest.str());
                    expect(log_reply.status == 200 && log_reply.body.contains(tools::lines::base64(log)));
                    const auto part = co_await h.ask(io, GET, path + "?offset=9&length=8");
                    expect(part.status == 200 && part.body.contains(tools::lines::base64("artifact"))) << part.body;
                    expect((co_await h.ask(io, GET, path, {}, {}, false)).status == 401);
                    const auto other = "/api/v1/projects/other/exports/" + digest.str();
                    expect((co_await h.ask(io, GET, other)).status != 200) << "knowing a digest grants no access";
                    const auto cached = co_await h.mcp(io, call("build", R"({"project":"other"})"));
                    expect(cached.status == 200 && cached.body.contains(digest.str())) << cached.body;
                    expect(builds == 1) << "identical inputs reuse the existing result";
                    expect((co_await h.ask(io, GET, other)).status == 200) << "cache recipients gain download access";
                    expect((co_await h.ask(io, GET, "/api/v1/projects/missing/exports/" + digest.str())).status != 200);
                    expect((co_await h.ask(io, GET, path + "?offset=99")).status != 200);
                    fail_download = true;
                    const auto previous_downloads = downloads;
                    const auto reused = co_await h.mcp(io, call("build", R"({"project":"app","fresh":true})"));
                    expect(reused.status == 200 && !reused.body.contains("artifact transport unavailable")) << reused.body;
                    expect(builds == 2 && downloads == previous_downloads) << "fresh execution reuses stored artifact and log bytes";
                    std::filesystem::remove(h.tmp.dir / "storage/.exports/blobs" / digest.view());
                    const auto failed = co_await h.mcp(io, call("build", R"({"project":"app","fresh":true})"));
                    expect(failed.status == 200 && failed.body.contains("artifact transport unavailable") && failed.body.contains(R"("exit_code":0)"))
                        << failed.body;
                    expect(!failed.body.contains("outcome_unknown")) << "execution results survive delivery failures";
                    expect(builds == 3);
                    fail_download = false;
                    const auto rebuilt = co_await h.mcp(io, call("build", R"({"project":"app"})"));
                    expect(rebuilt.status == 200 && builds == 4) << "failed delivery invalidates the result cache";
                    std::filesystem::remove(h.tmp.dir / "storage/.exports/blobs" / digest.view());
                    const auto stale = co_await h.mcp(io, call("build", R"({"project":"app"})"));
                    struct execution_reply {
                        struct payload {
                            tools::results structuredContent;
                        };
                        payload result;
                    };
                    const auto stale_result = rfl::json::read<execution_reply>(stale.body).value();
                    expect(stale.status == 200 && builds == 4 && !stale_result.result.structuredContent.results.at(0).exports.at(0).delivery_error.empty());
                    const auto repaired = co_await h.mcp(io, call("build", R"({"project":"app"})"));
                    expect(repaired.status == 200 && builds == 5) << "missing objects do not trap later calls in a stale cache";
                    remove_project = true;
                    const auto removed = co_await h.mcp(io, call("build", R"({"project":"app","fresh":true})"));
                    const auto removed_result = rfl::json::read<execution_reply>(removed.body).value();
                    const auto& completed = removed_result.result.structuredContent.results.at(0);
                    expect(removed.status == 200 && completed.steps.at(0).exit_code == 0 && completed.error.empty());
                    expect(!completed.exports.at(0).delivery_error.empty());
                    expect(completed.steps.at(0).log && !completed.steps.at(0).log->delivery_error.empty());
                    expect((co_await h.ask(io, GET, path)).status != 200);
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            h.rt.run(std::move(run_core));
        };
        "trusted authentication requires a configured workspace identity"_test = [] {
            expect(throws<std::invalid_argument>([] { config::parse(R"({"server":{"auth":{"mode":"trusted"}}})"); }))
                << "trusted mode must name the workspace it serves";
            expect(throws<std::invalid_argument>([] { config::parse(R"({"server":{"auth":{"mode":"trusted","workspace":""}}})"); }))
                << "an empty workspace identity is refused";
            auto configured = config::parse(R"({"server":{"auth":{"mode":"trusted","workspace":"acme-id"}}})");
            expect(configured.server.auth.workspace == "acme-id") << "a configured workspace identity is accepted";
        };
        "Workspace event subscriptions reset, coalesce and bound slow consumers"_test = [] {
            cores::io_context<native::context> rt{1};
            front::workspace_events events{rt, "workspace-id"};
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto first = events.listen(0);
                expect(first->body.take().contains(R"("workspace_id":"workspace-id","changes":15)"));
                events.publish(1);
                events.publish(6);
                co_await execution::schedule_after(io, std::chrono::milliseconds{1});
                expect(first->body.take().contains(R"("changes":7)"));
                auto second = events.listen(0);
                expect(second->body.take().contains("event: reset")) << "every reconnect rereads current state";
                first->send(std::string(65537, 'x'));
                expect(first->closed && first->body.pending_bytes() == 0u) << "slow subscribers have bounded memory";
                std::vector<std::shared_ptr<front::workspace_events::subscription>> held{first, second};
                while (auto sub = events.listen(0))
                    held.push_back(std::move(sub));
                expect(held.size() == 64u);
                held.clear();
                first.reset();
                second.reset();
                expect(static_cast<bool>(events.listen(0))) << "closed connections release their slots";
            };
            rt.run(std::move(run_core));
        };
        "Workspace SSE streams committed configuration changes before the response ends"_test = [] {
            harness h;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                h.port = l.local_endpoint().port();
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    static constexpr auto run_child = [](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                        for (;;) {
                            auto [connection, peer] = co_await net::async_accept(l);
                            children.spawn(h.server.serve(std::move(connection)));
                        }
                    };
                    children.spawn(run_child(h, l, children));
                    auto denied = co_await h.ask(io, GET, "/api/v1/events", {}, {}, false);
                    expect(denied.status == 401);
                    auto socket = co_await io::connect(io, l.local_endpoint());
                    web::stream<decltype(socket)> transport{socket};
                    http::v1::client client{transport};
                    http::request req{.method = GET,
                                      .target = "/api/v1/events",
                                      .scheme = "http",
                                      .authority = "localhost",
                                      .fields = {{"authorization", "Bearer sk_test"}, {"origin", "http://ok.example"}, {"last-event-id", "old-process"}}};
                    req.fields.push_back({"host", "localhost"});
                    http::span_body empty{std::span<const std::byte>{}};
                    auto response = co_await http::coroutine::request(client, std::move(req), empty);
                    expect(response.head.status == 200);
                    expect(http::find_header(response.head.fields, "content-type") == std::optional<std::string_view>{"text/event-stream"});
                    expect(http::find_header(response.head.fields, "x-accel-buffering") == std::optional<std::string_view>{"no"});
                    expect(http::find_header(response.head.fields, "access-control-allow-origin") == std::optional<std::string_view>{"http://ok.example"});
                    std::array<std::byte, 4096> buffer;
                    auto n = co_await http::coroutine::read(response.body, buffer);
                    std::string first{reinterpret_cast<const char*>(buffer.data()), n};
                    expect(first.contains("event: reset"));
                    auto saved = co_await h.save(io, "/api/v1/projects/sse", "{}");
                    expect(saved.status == 200) << saved.body;
                    std::string update;
                    while (!update.contains("event: change")) {
                        auto count = co_await coroutine::deadline(io, std::chrono::seconds{2}, http::coroutine::read(response.body, buffer));
                        if (!count || !*count)
                            break;
                        update.append(reinterpret_cast<const char*>(buffer.data()), *count);
                    }
                    expect(update.contains(R"("changes":8)")) << update;
                    socket.close();
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            h.rt.run(std::move(run_core));
        };
        "MCP keepalives detect a disconnected caller and release its queued ticket"_test = [] {
            using namespace std::chrono_literals;
            harness h{"token", 10ms};
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                h.port = listener.local_endpoint().port();
                auto worker_listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto worker_socket = co_await io::connect(io, worker_listener.local_endpoint());
                auto [worker_peer, endpoint] = co_await net::async_accept(worker_listener);
                transport::channel<socket> worker_channel{worker_socket};
                co_await worker_channel.post(
                    jobs::hello{.name = "queued-builder", .protocol = jobs::protocol_version, .tools = {"true"}, .toolchain_id = "test-toolchain"});
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    static constexpr auto run_child = [](harness& h, native::listener& listener, coroutine::scope& children) -> coroutine::task<> {
                        for (;;) {
                            auto [connection, peer] = co_await net::async_accept(listener);
                            children.spawn(h.server.serve(std::move(connection)));
                        }
                    };
                    children.spawn(run_child(h, listener, children));
                    children.spawn(h.router.workers(0).admit(io, std::move(worker_peer)));
                    while (h.router.workers(0).describe(0).empty())
                        co_await execution::schedule_after(io, 1ms);
                    children.spawn(h.queue.serve(io));
                    co_await h.save(io, "/api/v1/config/limits", R"({"concurrent_builds":1})");
                    config::settings project;
                    project.build_configurations["test"] = fixture::commands({"true"});
                    project.default_configurations = {"test"};
                    co_await h.save(io, "/api/v1/projects/app", config::write(project));
                    co_await h.ask(
                        io, http::method::POST, "/api/v1/projects/app/worldline",
                        R"({"project":"app","worldline":"main","action":"apply","observed":"","operation_id":"seed","changes":[{"kind":"create","path":"a.txt","text":"a"}]})");
                    auto run_operation = [&io](tools::services&, jobs::progress_log&, std::string) -> coroutine::task<std::optional<tools::backlog::outcome>> {
                        co_await execution::schedule_after(io, 30s);
                        co_return tools::config_result{.ok = true};
                    };
                    auto blocker = co_await h.queue.submit(
                        0, {.key = "blocker", .project = "app", .configuration = "blocker", .operation = "blocker", .actor = "blocker", .trees = "blocker"},
                        std::move(run_operation));
                    struct release {
                        std::shared_ptr<tools::backlog::ticket> ticket;
                        ~release() { ticket->leave(); }
                    } guard{blocker};
                    auto socket = co_await io::connect(io, listener.local_endpoint());
                    web::stream<decltype(socket)> transport{socket};
                    http::v1::client client{transport};
                    http::request request{
                        .method = http::method::POST,
                        .target = "/mcp",
                        .scheme = "http",
                        .authority = "localhost",
                        .fields = {{"authorization", "Bearer sk_test"}, {"accept", "text/event-stream"}, {"content-type", "application/json"}}};
                    auto body = call("build", R"({"project":"app"})", R"({"progressToken":"heartbeat"})");
                    request.fields.push_back({"content-length", std::to_string(body.size())});
                    request.fields.push_back({"host", "localhost"});
                    http::span_body payload{std::as_bytes(std::span{body.data(), body.size()})};
                    auto response = co_await http::coroutine::request(client, std::move(request), payload);
                    std::array<std::byte, 4096> buffer;
                    std::string frames;
                    while (!frames.contains(": keepalive\n\n") || !frames.contains("queued, ")) {
                        auto count = co_await coroutine::deadline(io, 2s, http::coroutine::read(response.body, buffer));
                        if (!count || !*count)
                            break;
                        frames.append(reinterpret_cast<const char*>(buffer.data()), *count);
                    }
                    expect(frames.contains(": keepalive\n\n")) << frames;
                    socket.close();
                    bool cancelled = false;
                    for (int attempt = 0; attempt < 100 && !cancelled; ++attempt) {
                        co_await execution::schedule_after(io, 20ms);
                        auto jobs = co_await h.queue.list(0);
                        cancelled = std::ranges::any_of(jobs, [](auto& job) { return job.configuration == "test" && job.state == "cancelled"; });
                    }
                    expect(cancelled) << "a disconnected request must release its backlog ticket";
                    blocker->leave();
                    while (!h.queue.idle())
                        co_await execution::schedule_after(io, 20ms);
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            h.rt.run(std::move(run_core));
        };
        "a trusted front names the caller and its role in headers"_test = [] {
            harness h{"trusted"};
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                static constexpr auto run_operation = [](harness& h, ctx& io) -> coroutine::task<> {
                    auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = l.local_endpoint().port();
                    auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                        static constexpr auto run_child = [](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                            for (;;) {
                                auto [connection, peer] = co_await net::async_accept(l);
                                children.spawn(h.server.serve(std::move(connection)));
                            }
                        };
                        children.spawn(run_child(h, l, children));
                        auto anonymous = co_await h.ask(io, GET, "/api/v1/me");
                        expect(anonymous.status == 401) << "a bearer token means nothing behind a trusted front: " << anonymous.body;
                        for (const auto& role : {"member", "owner", "host"}) {
                            std::vector<http::header> identity{{"x-fughetta-user", "user-2"}, {"x-fughetta-role", role}};
                            auto missing = co_await h.ask(io, GET, "/api/v1/me", {}, identity, false);
                            expect(missing.status == 401) << role << " without a workspace identity: " << missing.body;
                            identity.push_back({"x-fughetta-workspace", "other-id"});
                            auto wrong = co_await h.ask(io, GET, "/api/v1/me", {}, identity, false);
                            expect(wrong.status == 401) << role << " with another workspace identity: " << wrong.body;
                            identity.back().value = "acme";
                            auto slug = co_await h.ask(io, GET, "/api/v1/me", {}, identity, false);
                            expect(slug.status == 401) << role << " with a slug instead of the immutable identity: " << slug.body;
                            identity.back().value = "acme-id";
                            auto matched = co_await h.ask(io, GET, "/api/v1/me", {}, identity, false);
                            expect(matched.status == 200 && matched.body.contains(std::format(R"("role":"{}")", role)))
                                << role << " with the configured workspace identity: " << matched.body;
                        }
                        std::vector<http::header> member{
                            {"x-fughetta-workspace", "acme-id"}, {"x-fughetta-user", "user-2"}, {"x-fughetta-role", "member"}, {"x-fughetta-issuer", "cloud"}};
                        auto me = co_await h.ask(io, GET, "/api/v1/me", {}, member, false);
                        expect(me.status == 200 && me.body.contains(R"("user":"user-2")") && me.body.contains(R"("role":"member")") &&
                               me.body.contains(R"("issuer":"cloud")"))
                            << me.body;
                        auto limited = co_await h.ask(io, PUT, "/api/v1/config/limits", R"({"revision":0,"value":{}})", member, false);
                        expect(limited.status == 403) << limited.body;
                        auto config = co_await h.ask(io, GET, "/api/v1/config", {}, member, false);
                        expect(config.status == 200 && !config.body.contains(keyring::hash("sk_test"))) << "a member sees no token hashes";
                        auto owner = co_await h.ask(io, PUT, "/api/v1/config/limits", R"({"revision":0,"value":{}})",
                                                    {{"x-fughetta-workspace", "acme-id"}, {"x-fughetta-user", "user-1"}, {"x-fughetta-role", "owner"}}, false);
                        expect(owner.status == 200) << owner.body;
                        auto unknown =
                            co_await h.ask(io, GET, "/api/v1/me", {},
                                           {{"x-fughetta-workspace", "acme-id"}, {"x-fughetta-user", "user-3"}, {"x-fughetta-role", "operator"}}, false);
                        expect(unknown.status == 401) << unknown.body;
                        std::vector<http::header> owner_headers{
                            {"x-fughetta-workspace", "acme-id"}, {"x-fughetta-user", "user-1"}, {"x-fughetta-role", "owner"}};
                        std::vector<http::header> host_headers{{"x-fughetta-workspace", "acme-id"}, {"x-fughetta-user", "cloud"}, {"x-fughetta-role", "host"}};
                        auto grant = R"({"revision":0,"value":{"max_projects":3}})";
                        auto by_owner = co_await h.ask(io, PUT, "/api/v1/config/granted", grant, owner_headers, false);
                        expect(by_owner.status == 403) << "an owner cannot raise what the host grants: " << by_owner.body;
                        auto by_member = co_await h.ask(io, PUT, "/api/v1/config/granted", grant, member, false);
                        expect(by_member.status == 403) << by_member.body;
                        auto by_host = co_await h.ask(io, PUT, "/api/v1/config/granted", grant, host_headers, false);
                        expect(by_host.status == 200 && by_host.body.contains(R"("max_projects":3)")) << by_host.body;
                        auto stale = co_await h.ask(io, PUT, "/api/v1/config/granted", R"({"revision":0,"value":{"max_projects":9}})", host_headers, false);
                        expect(stale.status == 409 && stale.body.contains(R"("max_projects":3)"))
                            << "a stale grant is refused with the current one: " << stale.body;
                        expect(config::load(h.tmp.dir / "gateway.json").granted->max_projects == std::optional<std::uint32_t>{3}) << "a grant is saved";
                        auto server = co_await h.ask(io, GET, "/api/v1/config/server", {}, owner_headers, false);
                        auto server_revision = std::stoull(server.body.substr(server.body.find(R"("revision":)") + 11));
                        auto to_tokens = co_await h.ask(io, PUT, "/api/v1/config/server",
                                                        std::format(R"({{"revision":{},"value":{{"storage":"{}","auth":{{"mode":"token"}}}}}})",
                                                                    server_revision, (h.tmp.dir / "storage").generic_string()),
                                                        owner_headers, false);
                        expect(to_tokens.status == 403) << "no caller changes how callers are checked: " << to_tokens.body;
                        auto effective =
                            co_await h.ask(io, PUT, "/api/v1/config/limits", R"({"revision":1,"value":{"max_projects":50}})", owner_headers, false);
                        expect(effective.status == 200) << effective.body;
                        expect(config::resolve_limits(*h.hub.current(0)).max_projects == 3u) << "the grant caps the owner's own limit";
                        auto events = co_await h.ask(io, GET, "/api/v1/usage/events?after=0", {}, member, false);
                        expect(events.status == 403) << "a member does not read the event log";
                        events = co_await h.ask(io, GET, "/api/v1/usage/events?after=0", {}, host_headers, false);
                        expect(events.status == 200 && events.body.contains(R"("oldest":)")) << events.body;
                        auto granted = co_await h.ask(io, GET, "/api/v1/config/granted", {}, member, false);
                        auto granted_revision = std::stoull(granted.body.substr(granted.body.find(R"("revision":)") + 11));
                        auto cleared = co_await h.ask(io, PUT, "/api/v1/config/granted", std::format(R"({{"revision":{},"value":null}})", granted_revision),
                                                      host_headers, false);
                        expect(cleared.status == 200 && !config::load(h.tmp.dir / "gateway.json").granted.has_value()) << cleared.body;
                        auto named = co_await h.ask(io, GET, "/api/v1/me", {},
                                                    {{"x-fughetta-workspace", "acme-id"},
                                                     {"x-fughetta-user", "user-2"},
                                                     {"x-fughetta-role", "member"},
                                                     {"x-fughetta-caller", "agent-a"},
                                                     {"x-fughetta-email", "two@example.com"}},
                                                    false);
                        expect(named.status == 200 && named.body.contains(R"("caller":"agent-a")") && named.body.contains(R"("email":"two@example.com")") &&
                               named.body.contains(R"("issuer":"front")"))
                            << named.body;
                        children.request_stop();
                    };
                    co_await coroutine::with_scope(std::move(run_children));
                };
                return run_operation(h, io);
            };
            h.rt.run(std::move(run_core));
        };
        "token mode ignores caller headers and takes any case of the bearer scheme"_test = [] {
            harness h;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                static constexpr auto run_operation = [](harness& h, ctx& io) -> coroutine::task<> {
                    auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = l.local_endpoint().port();
                    auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                        static constexpr auto run_child = [](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                            for (;;) {
                                auto [connection, peer] = co_await net::async_accept(l);
                                children.spawn(h.server.serve(std::move(connection)));
                            }
                        };
                        children.spawn(run_child(h, l, children));
                        auto forged = co_await h.ask(io, GET, "/api/v1/me", {}, {{"x-fughetta-user", "intruder"}, {"x-fughetta-role", "owner"}}, false);
                        expect(forged.status == 401) << forged.body;
                        auto own = co_await h.ask(io, GET, "/api/v1/me", {}, {{"x-fughetta-user", "intruder"}});
                        expect(own.status == 200 && own.body.contains(R"("user":"tester")") && !own.body.contains("intruder")) << own.body;
                        auto lower = co_await h.ask(io, GET, "/api/v1/me", {}, {{"authorization", "bearer sk_test"}}, false);
                        expect(lower.status == 200) << lower.body;
                        children.request_stop();
                    };
                    co_await coroutine::with_scope(std::move(run_children));
                };
                return run_operation(h, io);
            };
            h.rt.run(std::move(run_core));
        };
        "the admin API configures limits and projects"_test = [] {
            harness h;
            std::vector<answer> a;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                static constexpr auto run_operation = [](harness& h, ctx& io, std::vector<answer>& a) -> coroutine::task<> {
                    auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = l.local_endpoint().port();
                    auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                        static constexpr auto run_child = [](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                            for (;;) {
                                auto [connection, peer] = co_await net::async_accept(l);
                                children.spawn(h.server.serve(std::move(connection)));
                            }
                        };
                        children.spawn(run_child(h, l, children));
                        auto read = co_await h.ask(io, GET, "/api/v1/config/limits");
                        auto at = std::stoull(read.body.substr(read.body.find(R"("revision":)") + 11));
                        a.push_back(
                            co_await h.ask(io, PUT, "/api/v1/config/limits", std::format(R"({{"revision":{},"value":{{"max_file_bytes": 100}}}})", at)));
                        a.push_back(co_await h.ask(io, PUT, "/api/v1/projects/app", R"({"revision":0})"));
                        a.push_back(co_await h.ask(io, PUT, "/api/v1/projects/bad%20name", R"({"revision":0})"));
                        a.push_back(co_await h.ask(io, GET, "/api/v1/projects"));
                        a.push_back(co_await h.ask(io, GET, "/api/v1/projects/app/effective"));
                        a.push_back(co_await h.ask(io, GET, "/api/v1/projects/nope"));
                        a.push_back(co_await h.ask(io, DELETE, "/api/v1/projects"));
                        a.push_back(co_await h.ask(io, GET, "/nothing/here"));
                        a.push_back(co_await h.ask(io, PUT, "/api/v1/config/limits", "{not json"));
                        a.push_back(co_await h.ask(io, GET, "/healthz"));
                        a.push_back(co_await h.ask(io, DELETE, "/api/v1/projects/app"));
                        a.push_back(co_await h.ask(io, GET, "/api/v1/projects/app"));
                        a.push_back(co_await h.ask(io, GET, "/"));
                        a.push_back(co_await h.ask(io, GET, "/metrics"));
                        a.push_back(co_await h.ask(io, http::method::OPTIONS, "/api/v1/projects/app", "",
                                                   {{"origin", "http://ok.example"}, {"access-control-request-method", "PUT"}}));
                        a.push_back(co_await h.ask(io, PUT, "/api/v1/projects/web", R"({"revision":0})", {{"origin", "http://ok.example"}}));
                        a.push_back(co_await h.ask(io, PUT, "/api/v1/projects/web", R"({"revision":0})", {{"origin", "http://localhost:5173"}}));
                    };
                    co_await coroutine::with_scope(std::move(run_children));
                };
                return run_operation(h, io, a);
            };
            h.rt.run(std::move(run_core));
            expect((a.size() == 17u) >> fatal);

            expect(a[14].status == 204 && a[14].allow_origin == "http://ok.example");
            expect(a[15].status == 200 && a[15].allow_origin == "http://ok.example") << a[15].body;
            expect(a[16].status == 403) << "an origin not in allowed_origins";
            expect(a[0].status == 200 && a[1].status == 200) << a[0].body << a[1].body;
            expect(a[2].status == 400) << a[2].body;
            expect(a[3].body == R"(["app"])") << a[3].body;
            expect(a[4].status == 200 && a[4].body.contains(R"("max_file_bytes":100)")) << a[4].body;
            expect(a[5].status == 404 && a[6].status == 405 && a[7].status == 404);
            expect(a[8].status == 400);
            expect(a[9].status == 200 && a[9].body.contains(R"("status":"ok")"));
            expect(a[10].status == 200 && a[11].status == 404);
            expect(a[12].status == 404 && a[12].content_type.starts_with("application/json")) << "only the API: the console is a separate app";
            expect(a[13].content_type.starts_with("text/plain") && a[13].body.contains("gateway_up 1\n") &&
                   a[13].body.contains("# TYPE gateway_tool_calls_total counter"))
                << a[13].body;
            expect(config::load(h.tmp.dir / "gateway.json").limits.max_file_bytes == std::optional<std::uint64_t>{100}) << "edits are saved";
        };

        "same-name registered workers have distinct metric series"_test = [] {
            harness h;
            enrollment::store registry{h.tmp.dir / "identities", "workspace-a"};
            auto first = registry.add("same machine", "owner");
            auto second = registry.add("same machine", "owner");
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                std::vector<socket> clients;
                clients.reserve(2);
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    for (const auto& credential : {first, second}) {
                        clients.push_back(co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()}));
                        auto [peer, endpoint] = co_await net::async_accept(listener);
                        transport::channel<socket> channel{clients.back()};
                        co_await channel.post(jobs::hello{.name = "same machine",
                                                          .auth = credential.secret,
                                                          .protocol = jobs::protocol_version,
                                                          .worker_id = credential.worker_id,
                                                          .workspace_id = credential.workspace_id,
                                                          .heartbeat_seconds = 15});
                        children.spawn(h.router.workers(0).admit(io, std::move(peer), &registry, &h.pool, [] { return true; }));
                        auto admitted = transport::decode<jobs::admission>(co_await channel.recv());
                        expect(admitted.accepted);
                    }
                    const worlds::actor actor{.issuer = "token", .user = "tester", .role = "owner"};
                    front::context context{h.hub, h.files, h.router, h.rt, 0, nullptr, &actor, &h.pool, &h.services};
                    auto metrics = co_await front::route(context, "GET", web::target::parse("/metrics"), "");
                    expect(metrics.head.status == 200);
                    for (const auto& credential : {first, second})
                        expect(
                            metrics.body.contains(std::format("gateway_worker_inflight{{worker=\"same machine\",worker_id=\"{}\"}} 0\n", credential.worker_id)))
                            << metrics.body;
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            h.rt.run(std::move(run_core));
        };

        "same-name managed workers report one summed inflight series"_test = [] {
            harness h;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                std::vector<socket> clients;
                clients.reserve(2);
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    for (std::size_t n = 0; n < 2; ++n) {
                        clients.push_back(co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()}));
                        auto [peer, endpoint] = co_await net::async_accept(listener);
                        transport::channel<socket> channel{clients.back()};
                        co_await channel.post(jobs::hello{.name = "builder", .protocol = jobs::protocol_version});
                        children.spawn(h.router.workers(0).admit(io, std::move(peer)));
                        while (h.router.workers(0).describe(0).size() != n + 1)
                            co_await execution::schedule_after(io, std::chrono::milliseconds{1});
                    }
                    const worlds::actor actor{.issuer = "token", .user = "tester", .role = "owner"};
                    front::context context{h.hub, h.files, h.router, h.rt, 0, nullptr, &actor, &h.pool, &h.services};
                    auto& fleet = h.router.workers(0);
                    auto run_operation_2 = [&](auto&) -> coroutine::task<> {
                        auto run_operation = [&](auto&) -> coroutine::task<> {
                            auto metrics = co_await front::route(context, "GET", web::target::parse("/metrics"), "");
                            const std::string series = "gateway_worker_inflight{worker=\"builder\",worker_id=\"\"} 2\n";
                            auto at = metrics.body.find(series);
                            expect(at != std::string::npos) << metrics.body;
                            expect(metrics.body.find("gateway_worker_inflight{worker=\"builder\"", at == std::string::npos ? 0 : at + series.size()) ==
                                   std::string::npos);
                            expect(metrics.body.contains("gateway_workers 2\n"));
                        };
                        co_await fleet.with({}, std::move(run_operation));
                    };
                    co_await fleet.with({}, std::move(run_operation_2));
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            h.rt.run(std::move(run_core));
        };

        "build plans and status remain available while manual analysis routes are removed"_test = [] {
            harness h;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                co_await h.hub.edit(0, [](config::store& store) {
                    config::settings project;
                    project.build_configurations["Debug"] = fixture::commands({"python3", "-c", "print('built')"});
                    language::configuration analysis{.build_configuration = "Analysis"};
                    builds::configuration analysis_build;
                    builds::config<"command"> command;
                    command.operations["prepare"] = {
                        .argv = {"python3", "-c",
                                 "import json; from pathlib import Path; p=Path('commands.json'); "
                                 "p.write_text(json.dumps([{'directory':str(Path.cwd()),'file':'main.c','arguments':['cc','-c','main.c']}]))"},
                        .outputs = {{"db", {"compilation_database", "commands.json"}}}};
                    analysis_build.builders["main"] = builds::with<"command">{.options = command};
                    analysis_build.tasks["db"] = {.builder = "main", .operation = "prepare"};
                    analysis_build.operations["prepare"] = {.tasks = {"db"}};
                    analysis_build.outputs["db"] = {.task = "db", .output = "db"};
                    project.build_configurations["Analysis"] = analysis_build;
                    project.analysis["Cpp"] = analysis;
                    store.put_project("app", project, 0);
                });
                const worlds::actor principal{.issuer = "token", .user = "tester", .caller = "tester", .role = "owner"};
                front::context context{h.hub, h.files, h.router, h.rt, 0, nullptr, &principal, nullptr, &h.services};
                const std::string base = "/api/v1/projects/app";
                auto request = [&](std::string method, std::string path, std::string body = "") -> coroutine::task<web::reply> {
                    auto target = web::target::parse(path);
                    co_return co_await front::route(context, method, target, body);
                };
                auto adapters = co_await request("GET", "/api/v1/build-adapters");
                expect(adapters.head.status == 200 && adapters.body.contains("command"));
                auto build = co_await request("POST", base + "/build-configurations/Debug/plan", R"({"operation":"build"})");
                expect(build.head.status == 200 && build.body.contains("built")) << build.body;
                auto missing = co_await request("POST", base + "/build-configurations/Missing/plan", R"({"operation":"build"})");
                expect(missing.head.status == 404);
                auto workers = co_await request("GET", "/api/v1/workers");
                expect(workers.head.status == 200 && workers.body == "[]");
                auto health = co_await request("GET", "/healthz");
                expect(health.body.contains(R"("workers":0)"));
                co_await h.files.with_ledger(0, [](usage::ledger& ledger, const config::document& doc) {
                    auto id = ledger.admit_build("app", config::resolve(doc, "app").limits);
                    ledger.record(id,
                                  {.project = "app", .target = "Debug", .worker = "test-worker", .ok = true, .exit_code = 0, .seconds = 2.0, .bytes_sent = 7});
                });
                auto history = co_await request("GET", "/api/v1/builds?project=app&limit=1");
                expect(history.head.status == 200 && history.body.contains("test-worker")) << history.body;
                auto used = co_await request("GET", "/api/v1/usage");
                expect(used.head.status == 200 && used.body.contains(R"("build_seconds_today":2)")) << used.body;
                auto metrics = co_await request("GET", "/metrics");
                expect(metrics.body.contains("gateway_builds_total") && metrics.body.contains("gateway_synced_bytes_total"));
                expect(metrics.body.contains(R"(result="unknown"} 0)") && metrics.body.contains(R"(result="cancelled"} 0)"));
                for (auto action : {"plan", "prepare", "receipt"}) {
                    auto removed = co_await request(action == std::string_view{"receipt"} ? "GET" : "POST", base + "/analysis/Cpp/" + action);
                    expect(removed.head.status == 404);
                }
                mcp::notifier note;
                const worlds::actor actor{.issuer = "token", .user = "tester", .role = "owner"};
                for (auto action : {"plan", "prepare", "receipt", "scopes"}) {
                    bool rejected = false;
                    try {
                        co_await tools::language_query::run(h.services, {}, {.project = "app", .configuration = "Cpp", .action = action}, note, actor);
                    } catch (const tools::tool_error&) {
                        rejected = true;
                    }
                    expect(rejected) << action;
                }
            };
            h.rt.run(std::move(run_core));
        };

        "node source browsing pins files to a durable causal revision"_test = [] {
            harness h;
            std::vector<answer> replies;
            std::filesystem::create_directories(h.tmp.dir / "source/app");
            test::spit(h.tmp.dir / "source/app/captured.txt", "captured file\n");
            std::ofstream{h.tmp.dir / "source/app/captured.bin", std::ios::binary}.write("\0\xff\x01\x80", 4);
            std::ofstream{h.tmp.dir / "source/app/long.txt"} << std::string(1024 * 1024 - 2, 'x') << "😀tail";
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                static constexpr auto run_operation = [](harness& h, ctx& io, std::vector<answer>& replies) -> coroutine::task<> {
                    auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = listener.local_endpoint().port();
                    auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                        static constexpr auto run_child = [](harness& h, native::listener& listener, coroutine::scope& children) -> coroutine::task<> {
                            for (;;) {
                                auto [connection, peer] = co_await net::async_accept(listener);
                                children.spawn(h.server.serve(std::move(connection)));
                            }
                        };
                        children.spawn(run_child(h, listener, children));
                        co_await h.save(io, "/api/v1/projects/app", "{}");
                        std::string base = "/api/v1/projects/app/node";
                        auto empty = co_await h.ask(io, GET, base + "/source");
                        expect(empty.status == 200) << "a project's node starts with an empty main" << empty.body;
                        auto capture_body = rfl::json::write(front::api::node_capture{content::hash_of("browser-capture").str(), {}, "Capture project files"});
                        auto captured = co_await h.ask(io, http::method::POST, base + "/capture", capture_body);
                        expect(captured.status == 200) << captured.body;
                        auto replay = co_await h.ask(io, http::method::POST, base + "/capture", capture_body);
                        auto first = rfl::json::read<nodes::node_reply>(captured.body), second = rfl::json::read<nodes::node_reply>(replay.body);
                        expect(bool(first) && bool(second) >> fatal);
                        expect(first->receipt == second->receipt);
                        expect(second->replayed);
                        auto changed_capture =
                            rfl::json::write(front::api::node_capture{content::hash_of("browser-capture").str(), {}, "Changed capture intent"});
                        auto rejected = co_await h.ask(io, http::method::POST, base + "/capture", changed_capture);
                        expect(rejected.status == 409 && rejected.body.contains("identity_conflict")) << rejected.body;
                        auto original = co_await h.ask(io, GET, base + "/source/file?revision=" + first->version + "&path=captured.txt");
                        expect(original.status == 200 && original.body.contains(R"("content":"captured file\n")")) << original.body;
                        auto binary = co_await h.ask(io, GET, base + "/source/file?revision=" + first->version + "&path=captured.bin");
                        auto binary_view = rfl::json::read<front::api::node_source_file>(binary.body);
                        expect(binary.status == 200 && bool(binary_view) >> fatal) << binary.body;
                        expect(binary_view->binary && binary_view->content.empty());
                        auto recapture =
                            rfl::json::write(front::api::node_capture{content::hash_of("browser-recapture").str(), first->frontier, "Capture project files"});
                        auto occupied = co_await h.ask(io, http::method::POST, base + "/capture", recapture);
                        expect(occupied.status == 409 && occupied.body.contains("source_conflict")) << occupied.body;
                        auto large = co_await h.ask(io, GET, base + "/source/file?revision=" + first->version + "&path=long.txt");
                        auto bounded = rfl::json::read<front::api::node_source_file>(large.body);
                        expect(large.status == 200 && bool(bounded) >> fatal) << large.body.substr(0, 300);
                        expect(bounded->truncated && bounded->lines == 1u && bounded->total_lines == 1u && bounded->content.size() == 1024 * 1024 - 2u);

                        replies.push_back(std::move(captured));

                        nodes::node_request request{.action = "apply",
                                                    .operation = content::hash_of("browser-seed").str(),
                                                    .expected = first->frontier,
                                                    .edits = {{.kind = "create", .path = "src/hello.txt", .text = "one\ntwo\nthree\n"}}};
                        auto accepted = co_await h.ask(io, http::method::POST, base, rfl::json::write(request));
                        auto parsed = rfl::json::read<nodes::node_reply>(accepted.body);
                        expect(bool(parsed) >> fatal) << accepted.body;
                        auto head_reply = co_await h.ask(io, GET, base + "/source");
                        auto head = rfl::json::read<front::api::node_source_head>(head_reply.body);
                        expect(head_reply.status == 200 && bool(head) >> fatal);
                        expect(head->node == parsed->node);
                        auto concurrent = request;
                        concurrent.operation = content::hash_of("browser-concurrent-save").str();
                        concurrent.expected = first->frontier;
                        concurrent.edits = {{.kind = "create", .path = "src/other.txt", .text = "other\n"}};
                        auto joined = co_await h.ask(io, http::method::POST, base, rfl::json::write(concurrent));
                        auto both = rfl::json::read<nodes::node_reply>(joined.body);
                        expect(joined.status == 200 && bool(both) >> fatal) << joined.body;
                        expect(both->frontier.size() == 2u) << "a save from an older state of the worldline joins it";
                        auto outside = request;
                        outside.operation = content::hash_of("browser-outside-save").str();
                        outside.expected = {content::hash_of("unknown-event").str()};
                        auto refused = co_await h.ask(io, http::method::POST, base, rfl::json::write(outside));
                        expect(refused.status == 503 && refused.body.contains("missing_objects")) << refused.body;
                        auto revision = both->version;
                        request.operation = content::hash_of("browser-next").str();
                        request.expected = both->frontier;
                        request.edits = {{.kind = "write", .path = "src/hello.txt", .text = "changed\n"}};
                        replies.push_back(co_await h.ask(io, http::method::POST, base, rfl::json::write(request)));
                        replies.push_back(co_await h.ask(io, GET, base + "/source?revision=" + revision));
                        replies.push_back(co_await h.ask(io, GET, base + "/source/file?revision=" + revision + "&path=src%2Fhello.txt&offset=2&limit=1"));
                        replies.push_back(co_await h.ask(io, GET, base + "/source/file?path=src%2Fhello.txt"));
                        replies.push_back(co_await h.ask(io, GET, base + "/source/file?revision=" + revision + "&path=missing"));
                    };
                    co_await coroutine::with_scope(std::move(run_children));
                };
                return run_operation(h, io, replies);
            };
            h.rt.run(std::move(run_core));
            expect((replies.size() == 6u) >> fatal);
            expect(replies[0].status == 200) << replies[0].body;
            expect(replies[1].status == 200) << replies[1].body;
            expect(replies[2].status == 200 && replies[2].body.contains("src/hello.txt")) << replies[2].body;
            expect(replies[3].status == 200 && replies[3].body.contains(R"("content":"two\n")") && replies[3].body.contains(R"("total_lines":3)"))
                << replies[3].body;
            expect(replies[4].status == 400);
            expect(replies[5].status == 404);
            expect(std::filesystem::exists(h.tmp.dir / "source/.nodes/app"));
            expect(!std::filesystem::exists(h.tmp.dir / "storage/.nodes"));
            expect(!std::filesystem::exists(h.tmp.dir / "storage/app"));
        };

        "saves name the revision they read"_test = [] {
            harness h;
            std::vector<answer> a;
            std::uint64_t first = 0;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                static constexpr auto run_operation = [](harness& h, ctx& io, std::vector<answer>& a, std::uint64_t& first) -> coroutine::task<> {
                    auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = l.local_endpoint().port();
                    auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                        static constexpr auto run_child = [](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                            for (;;) {
                                auto [connection, peer] = co_await net::async_accept(l);
                                children.spawn(h.server.serve(std::move(connection)));
                            }
                        };
                        children.spawn(run_child(h, l, children));
                        auto read = co_await h.ask(io, GET, "/api/v1/config/limits");
                        first = std::stoull(read.body.substr(read.body.find(R"("revision":)") + 11));
                        a.push_back(std::move(read));
                        a.push_back(co_await h.ask(io, PUT, "/api/v1/config/limits", "{}"));
                        a.push_back(
                            co_await h.ask(io, PUT, "/api/v1/config/limits", std::format(R"({{"revision":{},"value":{{"max_file_bytes": 100}}}})", first)));
                        a.push_back(
                            co_await h.ask(io, PUT, "/api/v1/config/limits", std::format(R"({{"revision":{},"value":{{"search_max_matches": 9}}}})", first)));
                        a.push_back(co_await h.ask(io, PUT, "/api/v1/config/limits",
                                                   std::format(R"({{"revision":{},"value":{{"search_max_matches": 9}}}})", first + 1)));
                        a.push_back(co_await h.ask(io, GET, "/api/v1/config/limits"));
                    };
                    co_await coroutine::with_scope(std::move(run_children));
                };
                return run_operation(h, io, a, first);
            };
            h.rt.run(std::move(run_core));
            expect((a.size() == 6u) >> fatal);
            expect(a[0].status == 200) << "read the section before saving it";
            expect(a[1].status == 400 && a[1].body.contains(R"("code":"missing_revision")")) << "a save with no revision is refused: " << a[1].body;
            expect(a[2].status == 200);
            expect(a[3].status == 409 && a[3].body.contains(std::string{R"("code":"revision_conflict")"}) &&
                   a[3].body.contains(std::format(R"("revision":{})", first + 1)) && a[3].body.contains(R"("max_file_bytes":100)"))
                << a[3].body;
            expect(a[4].status == 200 && a[4].body.contains(std::string{R"("search_max_matches":9)"}));
            expect(a[5].status == 200 && a[5].body.contains(std::string{R"("search_max_matches":9)"}));
        };

        "the MCP endpoint, over HTTP"_test = [] {
            harness h;
            std::vector<answer> a;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::
                                                          task<> {
                                                              static constexpr auto run_operation =
                                                                  [](harness& h, ctx& io, std::vector<answer>& a) -> coroutine::
                                                                                                                      task<> {
                                                                                                                          auto l = native::listener{
                                                                                                                              io,
                                                                                                                              {net::ip::address_v4::loopback(),
                                                                                                                               0}};
                                                                                                                          h.port = l.local_endpoint().port();
                                                                                                                          auto run_children = [&](coroutine::scope& children) -> coroutine::
                                                                                                                                                                                  task<> {
                                                                                                                                                                                      static constexpr auto
                                                                                                                                                                                          run_child =
                                                                                                                                                                                              [](harness&
                                                                                                                                                                                                     h,
                                                                                                                                                                                                 native::listener&
                                                                                                                                                                                                     l,
                                                                                                                                                                                                 coroutine::
                                                                                                                                                                                                     scope&
                                                                                                                                                                                                         children) -> coroutine::
                                                                                                                                                                                                                       task<> {
                                                                                                                                                                                                                           for (
                                                                                                                                                                                                                               ;
                                                                                                                                                                                                                               ;) {
                                                                                                                                                                                                                               auto [connection,
                                                                                                                                                                                                                                     peer] =
                                                                                                                                                                                                                                   co_await net::
                                                                                                                                                                                                                                       async_accept(
                                                                                                                                                                                                                                           l);
                                                                                                                                                                                                                               children
                                                                                                                                                                                                                                   .spawn(
                                                                                                                                                                                                                                       h.server
                                                                                                                                                                                                                                           .serve(std::move(
                                                                                                                                                                                                                                               connection)));
                                                                                                                                                                                                                           }
                                                                                                                                                                                                                       };
                                                                                                                                                                                      children
                                                                                                                                                                                          .spawn(run_child(
                                                                                                                                                                                              h,
                                                                                                                                                                                              l,
                                                                                                                                                                                              children));
                                                                                                                                                                                      co_await h
                                                                                                                                                                                          .save(
                                                                                                                                                                                              io,
                                                                                                                                                                                              "/api/v1/config/limits",
                                                                                                                                                                                              R"({"max_file_bytes": 100})");
                                                                                                                                                                                      co_await h
                                                                                                                                                                                          .save(
                                                                                                                                                                                              io,
                                                                                                                                                                                              "/api/v1/projects/app",
                                                                                                                                                                                              "{}");
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .mcp(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));
                                                                                                                                                                                      for (
                                                                                                                                                                                          const auto
                                                                                                                                                                                              name :
                                                                                                                                                                                          {"read",
                                                                                                                                                                                           "write",
                                                                                                                                                                                           "edit",
                                                                                                                                                                                           "remove",
                                                                                                                                                                                           "move",
                                                                                                                                                                                           "list",
                                                                                                                                                                                           "glob",
                                                                                                                                                                                           "search",
                                                                                                                                                                                           "generate",
                                                                                                                                                                                           "jobs"}) {
                                                                                                                                                                                          auto refused =
                                                                                                                                                                                              co_await h
                                                                                                                                                                                                  .mcp(
                                                                                                                                                                                                      io,
                                                                                                                                                                                                      call(
                                                                                                                                                                                                          name,
                                                                                                                                                                                                          R"({"project":"app"})"));
                                                                                                                                                                                          expect(
                                                                                                                                                                                              refused
                                                                                                                                                                                                  .body
                                                                                                                                                                                                  .contains(
                                                                                                                                                                                                      "unknown tool"))
                                                                                                                                                                                              << refused
                                                                                                                                                                                                     .body;
                                                                                                                                                                                      }
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .ask(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  GET,
                                                                                                                                                                                                  "/mcp"));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .mcp(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  "{not json"));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .mcp(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
                                                                                                                                                                                                  {{"mcp-protocol-version",
                                                                                                                                                                                                    "1999-01-01"}}));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .mcp(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
                                                                                                                                                                                                  {{"origin",
                                                                                                                                                                                                    "http://evil.example"}}));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .mcp(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
                                                                                                                                                                                                  {{"origin",
                                                                                                                                                                                                    "http://ok.example"}}));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .mcp(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  std::string(
                                                                                                                                                                                                      5000,
                                                                                                                                                                                                      ' ')));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .mcp(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  call(
                                                                                                                                                                                                      "configurations",
                                                                                                                                                                                                      R"({"project":"app"})",
                                                                                                                                                                                                      R"({"progressToken":"p1"})"),
                                                                                                                                                                                                  {{"accept",
                                                                                                                                                                                                    "application/json, text/event-stream"}}));
                                                                                                                                                                                      const auto
                                                                                                                                                                                          endpoint =
                                                                                                                                                                                              "/api/v1/projects/app/worldline";
                                                                                                                                                                                      const auto inspected =
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .ask(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  http::method::
                                                                                                                                                                                                      POST,
                                                                                                                                                                                                  endpoint,
                                                                                                                                                                                                  R"({"project":"app","worldline":"main","action":"inspect"})");
                                                                                                                                                                                      const auto initial =
                                                                                                                                                                                          rfl::json::read<
                                                                                                                                                                                              tools::worldline::
                                                                                                                                                                                                  output>(
                                                                                                                                                                                              inspected
                                                                                                                                                                                                  .body)
                                                                                                                                                                                              .value()
                                                                                                                                                                                              .version
                                                                                                                                                                                              .value();
                                                                                                                                                                                      const auto apply =
                                                                                                                                                                                          std::format(
                                                                                                                                                                                              R"({{"project":"app","worldline":"main","action":"apply","observed":"{}","operation_id":"op-a","changes":[{{"kind":"create","path":"a.txt","text":"hi\n"}}]}})",
                                                                                                                                                                                              initial);
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .ask(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  http::method::
                                                                                                                                                                                                      POST,
                                                                                                                                                                                                  endpoint,
                                                                                                                                                                                                  apply));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .ask(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  http::method::
                                                                                                                                                                                                      POST,
                                                                                                                                                                                                  endpoint,
                                                                                                                                                                                                  R"({"project":"app","worldline":"main","action":"receipt","operation_id":"op-a"})"));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .ask(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  http::method::
                                                                                                                                                                                                      POST,
                                                                                                                                                                                                  endpoint,
                                                                                                                                                                                                  apply));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .ask(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  http::method::
                                                                                                                                                                                                      POST,
                                                                                                                                                                                                  endpoint,
                                                                                                                                                                                                  R"({"project":"app","worldline":"main","action":"fork","name":"wl","operation_id":"op-fork"})"));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .ask(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  http::method::
                                                                                                                                                                                                      POST,
                                                                                                                                                                                                  endpoint,
                                                                                                                                                                                                  R"({"project":"app","action":"history","worldline":"wl"})"));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .ask(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  http::method::
                                                                                                                                                                                                      POST,
                                                                                                                                                                                                  endpoint,
                                                                                                                                                                                                  R"({"project":"app","worldline":"main","action":"read","path":"a.txt"})"));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .ask(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  GET,
                                                                                                                                                                                                  "/api/v1/usage"));
                                                                                                                                                                                      a.push_back(
                                                                                                                                                                                          co_await h
                                                                                                                                                                                              .ask(
                                                                                                                                                                                                  io,
                                                                                                                                                                                                  GET,
                                                                                                                                                                                                  "/metrics"));
                                                                                                                                                                                  };
                                                                                                                          co_await coroutine::with_scope(
                                                                                                                              std::move(run_children));
                                                                                                                      };
                                                              return run_operation(h, io, a);
                                                          };
            h.rt.run(std::move(run_core));
            expect((a.size() == 16u) >> fatal);
            for (const auto name : {"configurations", "build", "test", "run", "language"})
                expect(a[0].body.contains(std::format(R"("name":"{}")", name)));
            for (const auto name : {"read", "write", "edit", "remove", "move", "list", "glob", "search", "generate", "jobs"})
                expect(!a[0].body.contains(std::format(R"("name":"{}")", name)));
            expect(a[0].status == 200 && a[0].body.contains("outputSchema"));
            for (auto name : {"worldline", "artifact", "project", "save_project", "schema", "effective"})
                expect(a[0].body.contains(std::format(R"("name":"{}")", name)));
            expect(a[1].status == 405);
            expect(a[2].status == 400 && a[2].body.contains("-32700"));
            expect(a[3].status == 400);
            expect(a[4].status == 403);
            expect(a[5].status == 200 && a[5].allow_origin == "http://ok.example");
            expect(a[6].status == 413);
            expect(a[7].content_type == "text/event-stream" && a[7].body.starts_with("event: message\ndata: {"));
            for (std::size_t i = 8; i < a.size(); ++i)
                expect(a[i].status == 200) << a[i].body;
            expect(a[8].body.contains(R"("replayed":false)"));
            expect(a[9].body.contains(content::hash_of("hi\n")));
            expect(a[10].body.contains(R"("replayed":true)"));
            expect(a[11].body.contains(R"("worldline":"wl")"));
            expect(a[12].body.contains(R"("actor":"token:tester")"));
            expect(a[13].body.contains(R"("data_base64":"aGkK")"));
            expect(a[15].body.contains("gateway_tool_calls_total"));
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"front">{front_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
