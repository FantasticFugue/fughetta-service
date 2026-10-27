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
import settings;
import keyring;
import usage;
import support;

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
        test::scratch tmp{"settings"};
        cores::io_context<native::context> rt{1};
        routing::router router{rt};
        execution::thread_pool pool{2};
        front::config_hub hub{rt,
                              config::store{config::parse(std::format(R"({{
      "server": {{ "allowed_origins": ["http://ok.example"], "max_request_bytes": 1048576, "storage": "{}",
                   "auth": {{ "tokens": [{{ "name": "tester", "hash": "{}" }}, {{ "name": "owner", "hash": "{}" }}, {{ "name": "member", "hash": "{}", "role": "member" }}] }} }}
    }})",
                                                                      (tmp.dir / "storage").generic_string(), keyring::hash("sk_test"),
                                                                      keyring::hash("sk_owner"), keyring::hash("sk_member"))),
                                            tmp.dir / "gateway.json"},
                              pool};
        test::source_service source{tmp.dir / "source"};
        storage::registry files{rt, tmp.dir / "storage", hub, pool, source.endpoint()};
        tools::services services{hub.slot(0), router, files, 0};
        tools::backlog queue{rt, services};
        front::server server{front::context{hub, files, router, rt, 0, nullptr, nullptr, nullptr, &services}, services};
        harness() { services.queue = &queue; }
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
            auto at = current.status == 200 ? current.body.find(R"("revision":)") : std::string::npos;
            if (at == std::string::npos) {
                auto ws = std::string{path}.substr(0, std::string{path}.find("/projects/"));
                current = co_await ask(io, GET, ws);
                at = current.body.find(R"("revision":)");
            }
            auto revision = at == std::string::npos ? std::uint64_t{0} : std::stoull(current.body.substr(at + 11));
            co_return co_await ask(io, PUT, std::move(path), std::format(R"({{"revision":{},"value":{}}})", revision, value));
        }

        auto mcp(ctx& io, std::string_view json, std::vector<http::header> fields = {}) -> coroutine::task<answer> {
            fields.push_back({"content-type", "application/json"});
            co_return co_await ask(io, http::method::POST, "/mcp", std::string{json}, std::move(fields));
        }
    };

    auto result(const answer& response) -> mcp::tool_result {
        auto rpc = rfl::json::read<mcp::response>(response.body).value();
        return rfl::from_generic<mcp::tool_result>(rpc.result.value()).value();
    }
    auto arguments(std::string_view project, rfl::Generic body) -> rfl::Generic {
        return mcp::object({{"project", std::string{project}}, {"body", std::move(body)}});
    }
    auto invoke(harness& h, ctx& io, std::string name, rfl::Generic args = rfl::Generic::Object{}, std::string token = "sk_test",
                std::string path = "/mcp/config") -> coroutine::task<answer> {
        auto rpc = rfl::json::write(mcp::request{.jsonrpc = "2.0",
                                                 .method = "tools/call",
                                                 .id = 1,
                                                 .params = rfl::to_generic(mcp::call_params{.name = std::move(name), .arguments = std::move(args)})});
        co_return co_await h.ask(io, http::method::POST, std::move(path), std::move(rpc), {{"authorization", "Bearer " + token}}, false);
    }
    auto rest(harness& h, ctx& io, http::method method, std::string path, std::string body = "", std::string token = "sk_test") -> coroutine::task<answer> {
        co_return co_await h.ask(io, method, std::move(path), std::move(body), {{"authorization", "Bearer " + token}}, false);
    }
    template <class T> auto body(std::uint64_t revision, const T& value) -> rfl::Generic {
        return mcp::object({{"revision", rfl::to_generic(revision)}, {"value", rfl::to_generic(value)}});
    }

    auto exercise(harness& h, ctx& io) -> coroutine::task<> {
        const std::string project = "/api/v1/projects/app";
        const std::string list = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
        auto business = co_await h.ask(io, http::method::POST, "/mcp", list);
        auto configuration = co_await h.ask(io, http::method::POST, "/mcp/config", list);
        auto extract = [](const answer& a) {
            auto rpc = rfl::json::read<mcp::response>(a.body).value();
            struct listing {
                std::vector<mcp::tool> tools;
            };
            return rfl::from_generic<listing>(rpc.result.value()).value().tools;
        };
        auto business_tools = extract(business);
        auto config_tools = extract(configuration);
        expect(config_tools.size() == settings::catalog().size());
        for (const auto& tool : config_tools) {
            expect(!std::ranges::contains(business_tools, tool.name, &mcp::tool::name)) << tool.name;
            if (tool.name == "save_project") {
                auto schema = rfl::json::write(tool.inputSchema);
                expect(schema.contains("default_configurations") && schema.contains("Revision returned") && schema.size() < 2000) << schema.size();
            }
        }
        expect(std::ranges::contains(business_tools, std::string{"configurations"}, &mcp::tool::name));
        expect(result(co_await invoke(h, io, "save_project", {}, "sk_test", "/mcp")).isError);
        expect(result(co_await invoke(h, io, "read")).isError);

        auto compare = [&](std::string tool, rfl::Generic args, std::string path, std::string token = "sk_test") -> coroutine::task<> {
            auto via_mcp = result(co_await invoke(h, io, std::move(tool), std::move(args), token));
            auto via_rest = co_await rest(h, io, GET, std::move(path), "", token);
            expect(!via_mcp.isError && via_rest.status == 200) << via_mcp.content.front().text;
            expect(rfl::json::write(via_mcp.structuredContent.value()) == via_rest.body);
        };
        co_await compare("configuration", rfl::Generic::Object{}, "/api/v1/config", "sk_member");
        co_await compare("server", rfl::Generic::Object{}, "/api/v1/config/server");
        co_await compare("limits", rfl::Generic::Object{}, "/api/v1/config/limits");
        co_await compare("schema", rfl::Generic::Object{}, "/api/v1/settings-schema");
        expect((co_await rest(h, io, GET, "/api/v1/settings-schema", "", "sk_member")).body.contains("language server caches"));

        auto server = h.hub.current(0)->server;
        server.max_request_bytes = 2u << 20;
        auto saved = result(co_await invoke(h, io, "save_server", mcp::object({{"body", body(0, server)}})));
        expect(!saved.isError) << saved.content.front().text;
        co_await compare("server", rfl::Generic::Object{}, "/api/v1/config/server");
        expect(h.hub.current(0)->server.max_request_bytes == server.max_request_bytes);
        config::limits limits{.max_projects = 20, .read_max_lines = 80};
        saved = result(co_await invoke(h, io, "save_limits", mcp::object({{"body", body(0, limits)}})));
        expect(!saved.isError);
        auto read = co_await rest(h, io, GET, "/api/v1/config/limits");
        expect(saved.content.front().text == read.body);

        config::settings settings;
        settings.ignore = {"generated", "*.o"};
        settings.env = {{"CC", "clang"}};
        settings.build_configurations["Debug"] = fixture::cmake("build/debug");
        settings.run_configurations["tests"] = {.operation = "test", .configurations = {"Debug"}};
        settings.default_configurations = {"Debug"};
        settings.analysis["Cpp"] = {.server = fixture::clice("clice", {"--log-level", "off"})};
        std::uint64_t revision = 0;
        auto input = arguments("app", body(revision, settings));
        saved = result(co_await invoke(h, io, "save_project", input, "sk_member"));
        expect(!saved.isError) << saved.content.front().text;
        read = co_await rest(h, io, GET, project, "", "sk_member");
        expect(saved.content.front().text == read.body);
        auto decoded = rfl::json::read<front::api::section_view<config::settings>>(read.body).value();
        expect(config::write(decoded.value) == config::write(settings));
        co_await compare("project", arguments("app", {}), project, "sk_member");
        co_await compare("projects", mcp::object({}), "/api/v1/projects", "sk_member");
        co_await compare("effective", arguments("app", {}), project + "/effective", "sk_member");
        // Reusing an old revision returns the same complete error as REST.
        saved = result(co_await invoke(h, io, "save_project", input));
        auto failed = co_await rest(h, io, PUT, project, rfl::json::write(body(revision, settings)));
        expect(saved.isError && failed.status == 409 && saved.content.front().text == failed.body);

        auto selection = mcp::object({{"project", "app"}, {"name", "Debug"}, {"body", rfl::to_generic(builds::request{.operation = "build"})}});
        auto planned = result(co_await invoke(h, io, "preview", selection));
        auto rest_plan = co_await rest(h, io, http::method::POST, project + "/build-configurations/Debug/plan", R"({"operation":"build","parameters":{}})");
        expect(!planned.isError && rest_plan.status == 200 && planned.content.front().text == rest_plan.body);
        auto analysis = mcp::object({{"project", "app"}, {"name", "Cpp"}});
        planned = result(co_await invoke(h, io, "analysis", analysis));
        rest_plan = co_await rest(h, io, http::method::POST, project + "/analysis/Cpp/plan", "{}");
        expect(planned.isError && rest_plan.status == 400 && planned.content.front().text == rest_plan.body)
            << "the configuration names no language service: " << rest_plan.body;

        struct rejection {
            std::string tool, path, token;
            http::method method;
            rfl::Generic args, payload;
        };
        for (const auto& check :
             std::vector<rejection>{{"save_server", "/api/v1/config/server", "sk_member", PUT, mcp::object({{"body", body(1, server)}}), body(1, server)},
                                    {"save_limits", "/api/v1/config/limits", "sk_member", PUT, mcp::object({{"body", body(0, limits)}}), body(0, limits)}}) {
            auto rejected = co_await invoke(h, io, check.tool, check.args, check.token);
            auto denied = co_await rest(h, io, check.method, check.path, rfl::json::write(check.payload), check.token);
            auto rpc = rfl::json::read<mcp::response>(rejected.body).value();
            if (rpc.error) {
                expect(denied.body.contains(rpc.error->message));
            } else {
                auto outcome = result(rejected);
                expect(outcome.isError && outcome.content.front().text == denied.body);
            }
            expect(denied.status == 403);
        }

        // Invalid values and missing revisions are parsed by the REST handlers.
        auto invalid_server = server;
        invalid_server.http_bind = "localhost";
        auto invalid_project = settings;
        invalid_project.ignore = {""};
        for (const auto& check : std::vector<rejection>{
                 {"save_server", "/api/v1/config/server", "sk_test", PUT, mcp::object({{"body", body(h.hub.current(0)->revisions.server, invalid_server)}}),
                  body(h.hub.current(0)->revisions.server, invalid_server)},
                 {"save_project", project, "sk_member", PUT, arguments("app", body(decoded.revision, invalid_project)),
                  body(decoded.revision, invalid_project)},
                 {"save_limits", "/api/v1/config/limits", "sk_owner", PUT, mcp::object({{"body", mcp::object({{"value", rfl::to_generic(limits)}})}}),
                  mcp::object({{"value", rfl::to_generic(limits)}})}}) {
            auto outcome = result(co_await invoke(h, io, check.tool, check.args, check.token));
            auto response = co_await rest(h, io, check.method, check.path, rfl::json::write(check.payload), check.token);
            expect(outcome.isError && response.status == 400 && outcome.content.front().text == response.body) << outcome.content.front().text;
        }
        auto removed = result(co_await invoke(h, io, "remove_project", arguments("app", {}), "sk_member"));
        expect(!removed.isError);
        expect((co_await rest(h, io, GET, project)).status == 404);

        auto method = co_await h.ask(io, GET, "/mcp/config");
        expect(method.status == 405);
        auto bad_json = co_await h.ask(io, http::method::POST, "/mcp/config", "{");
        expect(bad_json.status == 400 && bad_json.body.contains("-32700"));
        auto origin = co_await h.ask(io, http::method::POST, "/mcp/config", list, {{"origin", "http://ok.example"}});
        expect(origin.status == 200 && origin.allow_origin == "http://ok.example");
        auto blocked = co_await h.ask(io, http::method::POST, "/mcp/config", list, {{"origin", "http://bad.example"}});
        expect(blocked.status == 403);
    }
    const auto cases = [] {
        "configuration MCP shares REST operations and separates tool catalogs"_test = [] {
            harness h;
            h.rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                return [](harness& h, ctx& io) -> coroutine::task<> {
                    auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    h.port = listener.local_endpoint().port();
                    co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                        children.spawn([](harness& h, native::listener& listener, coroutine::scope& children) -> coroutine::task<> {
                            for (;;)
                                children.spawn(
                                    h.server.serve(co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); })));
                        }(h, listener, children));
                        co_await exercise(h, io);
                    });
                }(h, io);
            });
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"settings">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
