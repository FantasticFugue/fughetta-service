import std;
import boost.ut;
import coroutine;
import native;
import http;
import configuration;
import content;
import json;
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

    struct rpc_result {
        json::value result;
    };

    auto result(const answer& response) -> mcp::tool_result {
        auto rpc = json::read<rpc_result>(response.body).value();
        return json::from_value<mcp::tool_result>(rpc.result).value();
    }
    auto arguments(std::string_view project, json::value body) -> json::value {
        return json::object({{"project", std::string{project}}, {"body", std::move(body)}});
    }
    auto invoke(harness& h, ctx& io, std::string name, json::value args = json::object{}, std::string token = "sk_test",
                std::string path = "/mcp/config") -> coroutine::task<answer> {
        auto rpc = json::write(mcp::request{.jsonrpc = "2.0",
                                                 .method = "tools/call",
                                                 .id = 1,
                                                 .params = json::to_value(mcp::call_params{.name = std::move(name), .arguments = std::move(args)})});
        co_return co_await h.ask(io, http::method::POST, std::move(path), std::move(rpc), {{"authorization", "Bearer " + token}}, false);
    }
    auto rest(harness& h, ctx& io, http::method method, std::string path, std::string body = "", std::string token = "sk_test") -> coroutine::task<answer> {
        co_return co_await h.ask(io, method, std::move(path), std::move(body), {{"authorization", "Bearer " + token}}, false);
    }
    template <class T> auto body(std::uint64_t revision, const T& value) -> json::value {
        return json::object({{"revision", json::to_value(revision)}, {"value", json::to_value(value)}});
    }

    auto exercise(harness& h, ctx& io) -> coroutine::task<> {
        const std::string project = "/api/v1/projects/app";
        const std::string list = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
        auto business = co_await h.ask(io, http::method::POST, "/mcp", list);
        auto configuration = co_await h.ask(io, http::method::POST, "/mcp/config", list);
        auto extract = [](const answer& a) {
            auto rpc = json::read<rpc_result>(a.body).value();
            struct listing {
                std::vector<mcp::tool> tools;
            };
            return json::from_value<listing>(rpc.result).value().tools;
        };
        auto business_tools = extract(business);
        auto config_tools = extract(configuration);
        expect(config_tools.size() == 4u);
        for (const auto& tool : config_tools) {
            expect(std::ranges::contains(business_tools, tool.name, &mcp::tool::name)) << tool.name;
            if (tool.name == "save_project") {
                auto schema = json::write(tool.inputSchema);
                expect(schema.contains("default_configurations") && schema.contains("Revision returned") && schema.size() < 2000) << schema.size();
            }
        }
        expect(std::ranges::contains(business_tools, std::string{"configurations"}, &mcp::tool::name));
        expect(result(co_await invoke(h, io, "save_project", {}, "sk_test", "/mcp")).isError);
        expect(result(co_await invoke(h, io, "read")).isError);

        auto compare = [&](std::string tool, json::value args, std::string path, std::string token = "sk_test") -> coroutine::task<> {
            auto via_mcp = result(co_await invoke(h, io, std::move(tool), std::move(args), token));
            auto via_rest = co_await rest(h, io, GET, std::move(path), "", token);
            expect(!via_mcp.isError && via_rest.status == 200) << via_mcp.content.front().text;
            expect(json::write(via_mcp.structuredContent.value()) == via_rest.body);
        };
        co_await compare("schema", json::object{}, "/api/v1/settings-schema");

        config::settings settings;
        settings.ignore = {"generated", "*.o"};
        settings.env = {{"CC", "clang"}};
        settings.build_configurations["Debug"] = fixture::cmake("build/debug");
        settings.run_configurations["tests"] = {.operation = "test", .configurations = {"Debug"}};
        settings.default_configurations = {"Debug"};
        fixture::options<"cmake">(settings.build_configurations["Debug"].builders.at("main")).export_compile_commands = true;
        settings.analysis["Cpp"] =
            fixture::profile({.server = fixture::clice("clice"),
                              .build_configuration = "Debug",
                              .inputs = {{"compilation_database", {language::build_input{"Debug", "configure", "compilation_database"}}}}});
        std::uint64_t revision = 0;
        auto input = arguments("app", body(revision, settings));
        auto saved = result(co_await invoke(h, io, "save_project", input, "sk_member"));
        expect(!saved.isError) << saved.content.front().text;
        auto read = co_await rest(h, io, GET, project, "", "sk_member");
        expect(saved.content.front().text == read.body);
        auto decoded = json::read<front::api::section_view<config::settings>>(read.body).value();
        expect(config::write(decoded.value) == config::write(settings));
        co_await compare("project", arguments("app", {}), project, "sk_member");
        co_await compare("effective", arguments("app", {}), project + "/effective", "sk_member");
        // Reusing an old revision returns the same complete error as REST.
        saved = result(co_await invoke(h, io, "save_project", input));
        auto failed = co_await rest(h, io, PUT, project, json::write(body(revision, settings)));
        expect(saved.isError && failed.status == 409 && saved.content.front().text == failed.body);

        auto invalid = settings;
        invalid.analysis.at("Cpp").stages.at("main").build_configuration = "missing";
        auto payload = body(decoded.revision, invalid);
        auto rejected = result(co_await invoke(h, io, "save_project", arguments("app", payload), "sk_member"));
        auto denied = co_await rest(h, io, PUT, project, json::write(payload), "sk_member");
        expect(rejected.isError && denied.status == 400 && rejected.content.front().text == denied.body);
        expect(denied.body.contains("build_configuration"));

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
        "a configuration tool answers a non-JSON core response as an MCP error"_test = [] {
            // The dispatch stands for a REST route that failed before framing JSON.
            static constexpr auto run_operation = [](const worlds::actor&, std::string, std::string, std::string) -> coroutine::task<web::reply> {
                co_return web::reply{.head = {.status = 200}, .body = "<html>gateway busy</html>"};
            };
            settings::bound bound{run_operation};
            cores::io_context<native::context> rt{1};
            auto run_core = [&](ctx&, std::size_t) -> coroutine::task<> {
                mcp::notifier drop;
                const worlds::actor who{.issuer = "token", .user = "tester", .caller = "settings-suite", .role = "owner"};
                auto outcome = co_await bound.call("schema", json::object{}, drop, who);
                expect(outcome.error);
                expect(outcome.text.contains("not JSON")) << outcome.text;
            };
            rt.run(std::move(run_core));
        };

        "configuration MCP shares REST operations and joins the workspace tool catalog"_test = [] {
            harness h;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                static constexpr auto run_operation = [](harness& h, ctx& io) -> coroutine::task<> {
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
                        co_await exercise(h, io);
                    };
                    co_await coroutine::with_scope(std::move(run_children));
                };
                return run_operation(h, io);
            };
            h.rt.run(std::move(run_core));
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"settings">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
