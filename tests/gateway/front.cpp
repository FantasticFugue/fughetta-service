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
        front::server server{front::context{hub, files, router, rt, 0, nullptr, nullptr, nullptr, &services}, services};
        explicit harness(std::string mode = "token") : mode(std::move(mode)) { services.queue = &queue; }
        std::uint16_t port = 0;

        auto ask(ctx& io, http::method method, std::string target, std::string body = {}, std::vector<http::header> fields = {}, bool authenticated = true)
            -> coroutine::task<answer> {
            auto s = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, port});
            web::stream<socket> transport{s};
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
        return std::format(R"({{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{{"name":"{}","arguments":{}{}}}}})", tool, args,
                           meta.empty() ? "" : std::format(R"(,"_meta":{})", meta));
    }

    static constexpr auto front_suite = [] {
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
            rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
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
            });
        };
        "Workspace SSE streams committed configuration changes before the response ends"_test = [] {
            harness h;
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                h.port = l.local_endpoint().port();
                co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn([](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                        for (;;)
                            children.spawn(h.server.serve(co_await execution::then(net::async_accept(l), [](auto socket, auto) { return std::move(socket); })));
                    }(h, l, children));
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
                });
            });
        };
        "a trusted front names the caller and its role in headers"_test = [] {
            harness h{"trusted"};
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                return [](harness& h, ctx& io) -> coroutine::task<> {
                    auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = l.local_endpoint().port();
                    co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                        children.spawn([](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                            for (;;)
                                children.spawn(
                                    h.server.serve(co_await execution::then(net::async_accept(l), [](auto socket, auto) { return std::move(socket); })));
                        }(h, l, children));
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
                    });
                }(h, io);
            });
        };
        "token mode ignores caller headers and takes any case of the bearer scheme"_test = [] {
            harness h;
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                return [](harness& h, ctx& io) -> coroutine::task<> {
                    auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = l.local_endpoint().port();
                    co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                        children.spawn([](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                            for (;;)
                                children.spawn(
                                    h.server.serve(co_await execution::then(net::async_accept(l), [](auto socket, auto) { return std::move(socket); })));
                        }(h, l, children));
                        auto forged = co_await h.ask(io, GET, "/api/v1/me", {}, {{"x-fughetta-user", "intruder"}, {"x-fughetta-role", "owner"}}, false);
                        expect(forged.status == 401) << forged.body;
                        auto own = co_await h.ask(io, GET, "/api/v1/me", {}, {{"x-fughetta-user", "intruder"}});
                        expect(own.status == 200 && own.body.contains(R"("user":"tester")") && !own.body.contains("intruder")) << own.body;
                        auto lower = co_await h.ask(io, GET, "/api/v1/me", {}, {{"authorization", "bearer sk_test"}}, false);
                        expect(lower.status == 200) << lower.body;
                        children.request_stop();
                    });
                }(h, io);
            });
        };
        "the admin API configures limits and projects"_test = [] {
            harness h;
            std::vector<answer> a;
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                return [](harness& h, ctx& io, std::vector<answer>& a) -> coroutine::task<> {
                    auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = l.local_endpoint().port();
                    co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                        children.spawn([](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                            for (;;)
                                children.spawn(
                                    h.server.serve(co_await execution::then(net::async_accept(l), [](auto socket, auto) { return std::move(socket); })));
                        }(h, l, children));
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
                    });
                }(h, io, a);
            });
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
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                std::vector<socket> clients;
                clients.reserve(2);
                co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                    for (const auto& credential : {first, second}) {
                        clients.push_back(co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()}));
                        auto peer = co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
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
                });
            });
        };

        "same-name managed workers report one summed inflight series"_test = [] {
            harness h;
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                std::vector<socket> clients;
                clients.reserve(2);
                co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                    for (std::size_t n = 0; n < 2; ++n) {
                        clients.push_back(co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()}));
                        auto peer = co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
                        transport::channel<socket> channel{clients.back()};
                        co_await channel.post(jobs::hello{.name = "builder", .protocol = jobs::protocol_version});
                        children.spawn(h.router.workers(0).admit(io, std::move(peer)));
                        while (h.router.workers(0).describe(0).size() != n + 1)
                            co_await execution::schedule_after(io, std::chrono::milliseconds{1});
                    }
                    const worlds::actor actor{.issuer = "token", .user = "tester", .role = "owner"};
                    front::context context{h.hub, h.files, h.router, h.rt, 0, nullptr, &actor, &h.pool, &h.services};
                    auto& fleet = h.router.workers(0);
                    co_await fleet.with({}, [&](auto&) -> coroutine::task<> {
                        co_await fleet.with({}, [&](auto&) -> coroutine::task<> {
                            auto metrics = co_await front::route(context, "GET", web::target::parse("/metrics"), "");
                            const std::string series = "gateway_worker_inflight{worker=\"builder\",worker_id=\"\"} 2\n";
                            auto at = metrics.body.find(series);
                            expect(at != std::string::npos) << metrics.body;
                            expect(metrics.body.find("gateway_worker_inflight{worker=\"builder\"", at == std::string::npos ? 0 : at + series.size()) ==
                                   std::string::npos);
                            expect(metrics.body.contains("gateway_workers 2\n"));
                        });
                    });
                });
            });
        };

        "build plans, worker status, history, and analysis preparation have REST routes"_test = [] {
            harness h;
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                co_await h.hub.edit(0, [](config::store& store) {
                    config::settings project;
                    project.build_configurations["Debug"] = fixture::commands({"python3", "-c", "print('built')"});
                    language::configuration analysis;
                    builds::config<"command"> command;
                    command.operations["prepare"] = {
                        .argv = {"python3", "-c",
                                 "import json; from pathlib import Path; p=Path('commands.json'); "
                                 "p.write_text(json.dumps([{'directory':str(Path.cwd()),'file':'main.c','arguments':['cc','-c','main.c']}]))"},
                        .outputs = {{"db", {"compilation_database", "commands.json"}}}};
                    analysis.build.builders["main"] = builds::with<"command">{.options = command};
                    analysis.build.tasks["db"] = {.builder = "main", .operation = "prepare"};
                    analysis.build.operations["prepare"] = {.tasks = {"db"}};
                    analysis.build.outputs["db"] = {.task = "db", .output = "db"};
                    analysis.inputs = {"db"};
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
                auto plan = co_await request("POST", base + "/analysis/Cpp/plan");
                expect(plan.head.status == 200 && plan.body.contains("commands.json")) << plan.body;
                mcp::notifier note;
                const worlds::actor actor{.issuer = "token", .user = "tester", .role = "owner"};
                co_await tools::worldline::run(h.services, {.project = "app", .action = "lock", .scopes = std::vector<std::string>{"::"}}, note, actor);
                co_await tools::write::run(
                    h.services, {.project = "app", .path = "main.c", .content = "int main(void) { return 0; }", .operation_id = "write-25"}, note, actor);
                auto prepared = co_await request("POST", base + "/analysis/Cpp/prepare", R"({"operation_id":"rest-prepare"})");
                expect(prepared.head.status == 200 && prepared.body.contains(R"("status":"prepared")")) << prepared.body;
                auto repeated = co_await request("POST", base + "/analysis/Cpp/prepare", R"({"operation_id":"rest-prepare"})");
                expect(repeated.head.status == 200 && repeated.body.contains(R"("replayed":true)"))
                    << "a repeated preparation answers its receipt: " << repeated.body;
                auto queued = co_await h.queue.list(0);
                expect(std::ranges::any_of(queued, [](auto& v) { return v.operation == "prepare" && v.state == "done" && v.ok == std::optional<bool>{true}; }))
                    << "a preparation takes its turn in the build queue";
                auto receipt = co_await request("GET", base + "/analysis/Cpp/receipt?operation_id=rest-prepare");
                expect(receipt.head.status == 200 && receipt.body.contains(R"("status":"prepared")")) << receipt.body;
                auto language = co_await tools::language_query::run(
                    h.services, {.project = "app", .worldline = "", .configuration = "Cpp", .action = "prepare", .operation_id = "mcp-prepare"}, note, actor);
                expect(language.preparation && language.preparation->status == "prepared");
                auto again = co_await tools::language_query::run(
                    h.services, {.project = "app", .worldline = "", .configuration = "Cpp", .action = "receipt", .operation_id = "mcp-prepare"}, note, actor);
                expect(again.preparation && again.preparation->status == "prepared");
            });
        };

        "node source browsing pins files to a durable causal revision"_test = [] {
            harness h;
            std::vector<answer> replies;
            std::filesystem::create_directories(h.tmp.dir / "source/app");
            std::ofstream{h.tmp.dir / "source/app/captured.txt"} << "captured file\n";
            std::ofstream{h.tmp.dir / "source/app/captured.bin", std::ios::binary}.write("\0\xff\x01\x80", 4);
            std::ofstream{h.tmp.dir / "source/app/long.txt"} << std::string(1024 * 1024 - 2, 'x') << "😀tail";
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                return [](harness& h, ctx& io, std::vector<answer>& replies) -> coroutine::task<> {
                    auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = listener.local_endpoint().port();
                    co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                        children.spawn([](harness& h, native::listener& listener, coroutine::scope& children) -> coroutine::task<> {
                            for (;;)
                                children.spawn(
                                    h.server.serve(co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); })));
                        }(h, listener, children));
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

                        nodes::node_request hold{.action = "lock", .scopes = {"::"}};
                        auto locked = co_await h.ask(io, http::method::POST, base, rfl::json::write(hold));
                        expect(locked.status == 200) << locked.body;
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
                    });
                }(h, io, replies);
            });
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
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                return [](harness& h, ctx& io, std::vector<answer>& a, std::uint64_t& first) -> coroutine::task<> {
                    auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = l.local_endpoint().port();
                    co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                        children.spawn([](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                            for (;;)
                                children.spawn(
                                    h.server.serve(co_await execution::then(net::async_accept(l), [](auto socket, auto) { return std::move(socket); })));
                        }(h, l, children));
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
                    });
                }(h, io, a, first);
            });
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
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                return [](harness& h, ctx& io, std::vector<answer>& a) -> coroutine::task<> {
                    auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = l.local_endpoint().port();
                    co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                        children.spawn([](harness& h, native::listener& l, coroutine::scope& children) -> coroutine::task<> {
                            for (;;)
                                children.spawn(
                                    h.server.serve(co_await execution::then(net::async_accept(l), [](auto socket, auto) { return std::move(socket); })));
                        }(h, l, children));
                        co_await h.save(io, "/api/v1/config/limits", R"({"max_file_bytes": 100})");
                        co_await h.save(io, "/api/v1/projects/app", "{}");
                        auto at = R"({"project":"app")";
                        a.push_back(co_await h.mcp(io, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));
                        a.push_back(co_await h.mcp(io, call("worldline", std::format(R"({},"action":"lock","scopes":["::"]}})", at))));
                        a.push_back(co_await h.mcp(io, call("write", std::format(R"({},"path":"a.txt","content":"hi\n","operation_id":"op-a"}})", at))));
                        a.push_back(co_await h.mcp(
                            io, call("write", std::format(R"({},"path":"b.txt","content":"{}","operation_id":"op-b"}})", at, std::string(200, 'x')))));
                        a.push_back(co_await h.mcp(
                            io, call("edit", std::format(R"({},"path":"a.txt","old_string":"absent","new_string":"x","operation_id":"op-e"}})", at))));
                        a.push_back(co_await h.ask(io, GET, "/mcp"));
                        a.push_back(co_await h.mcp(io, "{not json"));
                        a.push_back(co_await h.mcp(io, R"({"jsonrpc":"2.0","id":1,"method":"ping"})", {{"mcp-protocol-version", "1999-01-01"}}));
                        a.push_back(co_await h.mcp(io, R"({"jsonrpc":"2.0","id":1,"method":"ping"})", {{"origin", "http://evil.example"}}));
                        a.push_back(co_await h.mcp(io, R"({"jsonrpc":"2.0","id":1,"method":"ping"})", {{"origin", "http://ok.example"}}));
                        a.push_back(co_await h.mcp(io, std::string(5000, ' ')));
                        a.push_back(co_await h.mcp(io, call("read", std::format(R"({},"path":"a.txt"}})", at), R"({"progressToken":"p1"})"),
                                                   {{"accept", "application/json, text/event-stream"}}));
                        a.push_back(co_await h.ask(io, GET, "/api/v1/usage"));
                        a.push_back(co_await h.ask(io, GET, "/api/v1/stats?days=1"));
                        a.push_back(co_await h.ask(io, GET, "/metrics"));
                        a.push_back(co_await h.mcp(io, call("worldline", std::format(R"({},"action":"fork","name":"wl","operation_id":"op-1"}})", at))));
                        a.push_back(co_await h.mcp(io, call("worldline", std::format(R"({},"action":"history","worldline":"wl"}})", at))));
                        a.push_back(co_await h.mcp(io, call("write", std::format(R"({},"path":"c.txt","content":"receipt\n","operation_id":"op-c"}})", at))));
                        a.push_back(co_await h.mcp(io, call("worldline", std::format(R"({},"action":"receipt","operation_id":"op-c"}})", at))));
                        a.push_back(co_await h.mcp(io, call("write", std::format(R"({},"path":"c.txt","content":"receipt\n","operation_id":"op-c"}})", at))));
                        a.push_back(co_await h.mcp(io, call("write", std::format(R"({},"path":"c.txt","content":"changed\n","operation_id":"op-c"}})", at))));
                        a.push_back(co_await h.mcp(
                            io,
                            call(
                                "worldline",
                                std::format(
                                    R"({},"action":"apply","observed":"{}","operation_id":"op-d","changes":[{{"kind":"create","path":"d.txt","text":"d\n"}}]}})",
                                    at, std::string(64, '0')))));
                    });
                }(h, io, a);
            });
            expect((a.size() == 22u) >> fatal);
            expect(a[0].status == 200 && a[0].body.contains(R"("name":"language")") && a[0].body.contains("outputSchema")) << a[0].body;
            expect(a[1].body.contains(R"("isError":false)")) << a[1].body;
            expect(a[2].body.contains(R"("isError":false)")) << a[2].body;
            expect(a[3].body.contains(R"("isError":true)") && a[3].body.contains("100 byte file limit") && a[3].body.contains("resource_limit")) << a[3].body;
            expect(a[4].body.contains(R"("isError":true)") && a[4].body.contains("does not occur")) << a[4].body;
            expect(a[5].status == 405);
            expect(a[6].status == 400 && a[6].body.contains("-32700")) << a[6].body;
            expect(a[7].status == 400);
            expect(a[8].status == 403);
            expect(a[9].status == 200 && a[9].allow_origin == "http://ok.example");
            expect(a[10].status == 413);
            expect(a[11].content_type == "text/event-stream") << a[11].content_type;
            expect(a[11].body.starts_with("event: message\ndata: {") && a[11].body.contains(R"(\"content\":\"hi\\n\")")) << a[11].body;
            expect(a[12].status == 200) << a[12].body;
            expect(a[13].body.contains(R"("tool_calls":5)") && a[13].body.contains(R"("tool_errors":2)")) << a[13].body;
            expect(a[14].body.contains(R"(gateway_tool_calls_total{tool="write",result="error"} 1)")) << a[14].body;
            expect(a[15].body.contains(R"("isError":false)")) << a[15].body;
            expect(a[16].body.contains(R"("actor":"token:tester")")) << "history names the authenticated caller" << a[16].body;
            expect(a[17].body.contains(R"("isError":false)") && a[17].body.contains(R"("replayed":false)")) << a[17].body;
            expect(a[18].body.contains(R"("isError":false)") && a[18].body.contains(content::hash_of("receipt\n"))) << a[18].body;
            expect(a[19].body.contains(R"("isError":false)") && a[19].body.contains(R"("replayed":true)")) << a[19].body;
            expect(a[20].body.contains(R"("isError":true)") && a[20].body.contains("identity_conflict")) << a[20].body;
            expect(a[21].body.contains(R"("isError":true)") && a[21].body.contains("invalid_record")) << a[21].body;
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"front">{front_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
