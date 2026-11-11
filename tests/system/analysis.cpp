import launch;
import boost.ut;
import client;
import protocol;
import language;
import fixture;
import content;
import nodes;
import semantic;
import tools;

namespace {
    using namespace boost::ut;
    auto start_source(const std::filesystem::path& root, std::uint16_t port) {
        std::vector<std::string> args{"--port", std::to_string(port), "--storage", (root / "source").string()};
        auto process = std::make_unique<test::child>(test::source_command(SERVICE_EXECUTABLE, root, std::move(args)));
        test::eventually([&] {
            test::require(!process->finished(), process->output());
            return process->output().contains("source: internal file service");
        });
        test::ready(*process, port);
        return process;
    }

    static constexpr auto cases = [] {
        "task credentials bind concurrent runs, survive restart and revoke source access"_test = [] {
            test::scratch tmp{"task-context"};
            auto source_port = test::port(), port = test::port();
            auto source = start_source(tmp.dir, source_port);
            auto doc = test::document(tmp.dir, port, source_port);
            doc.projects["app.task"] = doc.projects.at("app");
            doc.projects.erase("app");
            test::spit(tmp.dir / "config.json", rfl::json::write(doc));
            test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {"config.json"})};
            test::ready(gateway, port);
            auto initial = test::decode<tools::worldline::output>(test::tool(port, "worldline", R"({"project":"app.task","action":"inspect"})"));
            auto missing = test::request(port, http::method::POST, "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"worldline","arguments":{"project":"app.task","action":"inspect"}}})");
            expect(missing.body.contains("context_required"));
            expect(test::request(port, http::method::POST, "/api/v1/projects/app.task/node",
                R"({"action":"scope_register","analyzed_scopes":[]})").status == 403);
            auto fork = "{\"project\":\"app.task\",\"action\":\"fork\",\"task\":\"parser\",\"worldline\":\"task-parser\",\"from\":\"main\",\"version\":" + rfl::json::write(initial.version) + ",\"operation_id\":\"task-fork\"}";
            test::tool(port, "task", fork);
            test::tool(port, "task", fork);
            auto start = [&](std::string operation) {
                return test::decode<tools::task::output>(test::tool(port, "task", "{\"project\":\"app.task\",\"action\":\"start\",\"task\":\"parser\",\"operation_id\":" + rfl::json::write(operation) + "}"));
            };
            auto a = start("run-a"), b = start("run-b");
            expect(a.run != b.run && a.credential != b.credential && start("run-a").credential == a.credential);
            auto agent = [&](std::string name, std::string args, bool error = false, bool second = false) {
                return test::tool(port, name, args, error, *(second ? b.credential : a.credential));
            };
            auto tree = test::decode<tools::worldline::output>(agent("worldline", R"({"action":"inspect"})"));
            expect(tree.worldline == "task-parser");
            expect(agent("worldline", R"({"action":"inspect","worldline":"main"})", true).contains("permission_denied"));
            expect(agent("worldline", R"({"action":"inspect","project":"elsewhere"})", true).contains("permission_denied"));
            expect(agent("task", R"({"project":"app.task","action":"start","task":"parser","operation_id":"escape"})", true).contains("permission_denied"));
            auto rest = test::request(port, http::method::POST, "/api/v1/projects/app.task/worldline", R"({"project":"app.task","action":"inspect","worldline":"main"})", *a.credential);
            expect(rest.status == 403);
            auto acquired = test::decode<tools::scope::output>(agent("scope", "{\"action\":\"acquire\",\"entries\":[\"new.txt\"],\"version\":" + rfl::json::write(tree.version) + ",\"operation_id\":\"entry\"}"));
            auto mutation = "{\"action\":\"apply\",\"observed\":" + rfl::json::write(tree.version) + ",\"operation_id\":\"create\",\"leases\":[" + rfl::json::write(acquired.leases[0].token) + "],\"changes\":[{\"kind\":\"create\",\"path\":\"new.txt\",\"text\":\"\"}]}";
            expect(agent("worldline", mutation, true, true).contains("scope_expired"));
            auto created = test::decode<tools::worldline::output>(agent("worldline", mutation));
            expect(created.version.has_value());
            auto observer = test::decode<tools::task::output>(test::tool(port, "task",
                R"({"project":"app.task","action":"start","task":"parser","operation_id":"observer","writable":false})"));
            auto observed = test::decode<tools::worldline::output>(test::tool(port, "worldline", R"({"action":"inspect"})", false, *observer.credential));
            expect(observed.version == created.version);
            expect(test::tool(port, "worldline", "{\"action\":\"apply\",\"observed\":" + rfl::json::write(created.version) +
                ",\"operation_id\":\"readonly-write\",\"changes\":[{\"kind\":\"create\",\"path\":\"readonly.txt\",\"text\":\"forbidden\"}]}", true, *observer.credential).contains("permission_denied"));
            expect(source->stop() == 0);
            source = start_source(tmp.dir, source_port);
            expect(test::decode<tools::worldline::output>(agent("worldline", R"({"action":"read","path":"new.txt"})")).text == "");
            auto main = test::decode<tools::worldline::output>(test::tool(port, "worldline", R"({"project":"app.task","action":"inspect"})"));
            expect(main.version == initial.version);
            test::tool(port, "task", "{\"project\":\"app.task\",\"action\":\"stop\",\"task\":\"parser\",\"run\":" + rfl::json::write(a.run) + "}");
            auto revoked = test::request(port, http::method::POST, "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", *a.credential);
            expect(revoked.status == 401);
            auto remaining = test::decode<tools::scope::output>(agent("scope", R"({"action":"list"})", false, true));
            expect(remaining.leases.empty());
            auto file = test::decode<tools::scope::output>(agent("scope", "{\"action\":\"acquire\",\"files\":[\"new.txt\"],\"version\":" + rfl::json::write(created.version) + ",\"operation_id\":\"empty-file\"}", false, true));
            expect(file.leases.size() == 1u && file.leases[0].kind == "file");
            test::tool(port, "task", R"({"project":"app.task","action":"close","task":"parser"})");
            expect(test::request(port, http::method::POST, "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", *b.credential).status == 401);
            expect(gateway.stop() == 0 && source->stop() == 0);
        };

        "language preparation requires a worker and never executes on source"_test = [] {
            test::scratch tmp{"analysis-routing"};
            auto source_port = test::port(), port = test::port();
            auto source = start_source(tmp.dir, source_port);
            auto doc = test::document(tmp.dir, port, source_port);
            auto& settings = doc.projects.at("app");
            settings.build_configurations["native"] = fixture::cmake("build/native");
            fixture::options<"cmake">(settings.build_configurations["native"].builders.at("main")).export_compile_commands = true;
            settings.analysis["cpp"] = {.build_configuration = "native"};
            test::spit(tmp.dir / "config.json", rfl::json::write(doc));
            test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {"config.json"})};
            test::ready(gateway, port);
            auto error = test::tool(port, "language", R"({"project":"app","configuration":"cpp"})", true);
            expect(error.contains("no_worker")) << error;
            expect(!std::filesystem::exists(tmp.dir / "source/.analysis"));
            for (auto action : {"plan", "prepare", "receipt"}) {
                auto response = test::request(port, action == std::string_view{"receipt"} ? http::method::GET : http::method::POST,
                                              std::string{test::project} + "/analysis/cpp/" + action);
                expect(response.status == 404);
            }
            expect(gateway.stop() == 0 && source->stop() == 0);
        };

        "one MCP connection edits, analyzes, builds, tests and retrieves a local executable"_test = [] {
            auto clice = std::getenv("TEST_CLICE");
            if (!clice) {
                skip / "real worker analysis requires TEST_CLICE"_test = [] {};
                return;
            }
            test::scratch tmp{"worker-analysis"};
            auto source_port = test::port(), port = test::port();
            auto source = start_source(tmp.dir, source_port);
            auto doc = test::document(tmp.dir, port, source_port);
            auto& settings = doc.projects.at("app");
            settings.build_configurations["native"] = fixture::cmake("build/native");
            auto& cmake = fixture::options<"cmake">(settings.build_configurations["native"].builders.at("main"));
            cmake.export_compile_commands = true;
            settings.analysis["cpp"] = {.server = fixture::clice("clice", {"--log-level", "off"}), .build_configuration = "native"};
            settings.default_configurations = {"native"};
            settings.build_configurations["native"].operations.at("build").exports = {{"app", "build/native/app"}};
            settings.build_configurations["native"].operations.at("test").exports = {{"app", "build/native/app"}};
            auto project_settings = settings;
            settings = {};
            test::spit(tmp.dir / "config.json", rfl::json::write(doc));
            test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {"config.json"})};
            test::ready(gateway, port);
            config::worker worker_config;
            worker_config.gateway = std::format("127.0.0.1:{}", doc.server.worker_port);
            worker_config.work_root = (tmp.dir / "worker").string();
            worker_config.tools["clice"] = clice;
            test::spit(tmp.dir / "worker.json", rfl::json::write(worker_config));
            test::child worker{test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {"worker.json"})};
            test::eventually([&] {
                test::require(!worker.finished(), worker.output());
                auto workers = test::request(port, http::method::GET, "/api/v1/workers");
                return workers.status == 200 && workers.body != "[]";
            });
            // Only MCP requests supply configuration and source; no local source sync or source REST writes.
            auto current = test::tool(port, "project", R"({"project":"app"})");
            struct revision { std::uint64_t revision; };
            test::tool(port, "save_project", "{\"project\":\"app\",\"body\":{\"revision\":" +
                std::to_string(test::decode<revision>(current).revision) + ",\"value\":" + config::write(project_settings) + "}}");
            std::string credential = "sk_test";
            auto agent = [&](std::string name, std::string args, bool error = false) {
                return test::tool(port, name, args, error, credential);
            };
            auto line = [&](std::string args) {
                return test::decode<tools::worldline::output>(agent("worldline", std::move(args)));
            };
            auto initial = line(R"({"project":"app","action":"inspect"})");
            const std::string before = "// project code\nint twice(int x) { return x * 3; }\nint main() { return twice(21) == 42 ? 0 : 1; }\n";
            const std::string after = "// project code\nint twice(int x) { return x * 2; }\nint main() { return twice(21) == 42 ? 0 : 1; }\n";
            const std::string cmake_source = "cmake_minimum_required(VERSION 3.20)\nproject(mcp_loop CXX)\nadd_executable(app main.cpp)\nenable_testing()\nadd_test(NAME answer COMMAND app)\n";
            auto created = line("{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + rfl::json::write(initial.version) +
                ",\"operation_id\":\"create\",\"changes\":[{\"kind\":\"create\",\"path\":\"main.cpp\",\"text\":" + rfl::json::write(before) +
                "},{\"kind\":\"create\",\"path\":\"CMakeLists.txt\",\"text\":" + rfl::json::write(cmake_source) + "}]}");
            test::tool(port, "task", R"({"project":"app","action":"create","task":"fix","worldline":"main","operation_id":"bind"})");
            auto execution_run = test::decode<tools::task::output>(test::tool(port, "task", R"({"project":"app","action":"start","task":"fix","operation_id":"start"})"));
            credential = *execution_run.credential;
            auto read = line(R"({"project":"app","action":"read","path":"main.cpp"})");
            expect(read.text == before);
            auto symbols = agent("language", R"({"project":"app","configuration":"cpp","action":"symbols"})");
            expect(symbols.contains("twice") && symbols.contains(R"("analyzed":true)")) << symbols;
            auto scopes = test::decode<tools::language_query::output>(agent("language",
                R"({"project":"app","configuration":"cpp","action":"scopes","path":"main.cpp"})"));
            test::require(scopes.scopes.has_value(), "no semantic scopes");
            const auto replacement = before.find('3');
            auto selected = std::ranges::find_if(*scopes.scopes, [&](const auto& scope) {
                return scope.kind == "function" && scope.range.place && scope.range.place->begin <= replacement && scope.range.place->end > replacement;
            });
            test::require(selected != scopes.scopes->end(), "function scope missing");
            auto range = *selected->range.place;
            auto scoped_text = line("{\"project\":\"app\",\"action\":\"read\",\"path\":\"main.cpp\",\"version\":" +
                rfl::json::write(scopes.version) + ",\"offset\":" + std::to_string(range.begin) + ",\"count\":" + std::to_string(range.end - range.begin) + "}");
            expect(scoped_text.text == before.substr(range.begin, range.end - range.begin));
            tools::scope::input acquire{.project = "app", .action = "acquire", .scopes = std::vector<std::string>{*selected->reference}, .operation_id = "claim"};
            auto leased = test::decode<tools::scope::output>(agent("scope", rfl::json::write(acquire)));
            auto replay = test::decode<tools::scope::output>(agent("scope", rfl::json::write(acquire)));
            expect(leased.version == *created.version && replay.leases.at(0).token == leased.leases.at(0).token);
            expect(leased.leases.at(0).begin == range.begin && leased.leases.at(0).end == range.end);
            auto token = leased.leases.at(0).token;
            auto edit = [&](std::string worldline, std::string operation, bool held) {
                return "{\"project\":\"app\",\"action\":\"apply\",\"worldline\":" + rfl::json::write(worldline) +
                    ",\"observed\":" + rfl::json::write(created.version) + ",\"operation_id\":" + rfl::json::write(operation) +
                    (held ? ",\"leases\":[" + rfl::json::write(token) + "]" : "") +
                    ",\"changes\":[{\"kind\":\"edit\",\"path\":\"main.cpp\",\"offset\":" + std::to_string(replacement) + ",\"count\":1,\"text\":\"2\"}]}";
            };
            // A lease persists across a source restart and fences writes that omit its token.
            expect(source->stop() == 0);
            source = start_source(tmp.dir, source_port);
            auto refused = agent("worldline", edit("main", "unclaimed", false), true);
            expect(refused.contains("scope_conflict"));
            test::tool(port, "worldline", R"({"project":"app","action":"fork","name":"parallel","operation_id":"fork"})");
            auto independent = test::decode<tools::worldline::output>(test::tool(port, "worldline", edit("parallel", "parallel-fix", false)));
            expect(independent.version.has_value());
            // A disjoint edit in this file moves the function without invalidating its anchored lease.
            const std::string prefix = "// concurrent note\n";
            line("{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + rfl::json::write(created.version) +
                ",\"operation_id\":\"notes\",\"changes\":[{\"kind\":\"edit\",\"path\":\"main.cpp\",\"offset\":0,\"count\":0,\"text\":" + rfl::json::write(prefix) + "}]}");
            auto edited = line(edit("main", "fix", true));
            auto reread = line("{\"project\":\"app\",\"action\":\"read\",\"path\":\"main.cpp\",\"version\":" +
                rfl::json::write(edited.version) + ",\"known_digest\":" + rfl::json::write(read.digest) + "}");
            expect(reread.text == prefix + after);
            auto compared = line("{\"project\":\"app\",\"action\":\"compare\",\"observed\":" + rfl::json::write(created.version) +
                ",\"version\":" + rfl::json::write(edited.version) + "}");
            expect(compared.changes && compared.changes->size() == 1u && compared.files && compared.files->size() == 1u);
            expect(edited.files && edited.files->size() == 1u && edited.files->at(0).path == "main.cpp");
            auto unchanged = line("{\"project\":\"app\",\"action\":\"read\",\"path\":\"main.cpp\",\"known_digest\":" + rfl::json::write(reread.digest) + "}");
            expect(unchanged.unchanged.value_or(false) && !unchanged.text && !unchanged.data_base64);
            auto renewed = test::decode<tools::scope::output>(agent("scope", rfl::json::write(tools::scope::input{.project = "app", .action = "renew", .token = token})));
            expect(renewed.version == *edited.version && renewed.leases.at(0).begin == range.begin + prefix.size());
            agent("scope", rfl::json::write(tools::scope::input{.project = "app", .action = "release", .token = token}));
            expect(agent("worldline", edit("main", "expired-fix", true), true).contains("scope_expired"));
            auto pinned = "{\"project\":\"app\",\"configurations\":[\"native\"],\"version\":" + rfl::json::write(edited.version) + "}";
            struct build_output { std::vector<builds::exported_file> exports; };
            struct execution { bool ok; std::string revision; std::vector<build_output> results; };
            auto built = test::decode<execution>(agent("build", pinned));
            expect(built.ok && built.revision == *edited.version);
            auto tested = test::decode<execution>(agent("test", pinned));
            expect(tested.ok && tested.revision == *edited.version) << config::write(tested);
            test::require(tested.ok && !tested.results.at(0).exports.empty(), "test produced no artifact");
            auto exported = tested.results.at(0).exports.at(0);
            expect(exported.executable && exported.delivery_error.empty());
            auto chunk_args = [&](std::string project, std::uint64_t offset, std::uint64_t length) {
                return "{\"project\":" + rfl::json::write(project) + ",\"digest\":" + rfl::json::write(exported.digest) +
                    ",\"offset\":" + std::to_string(offset) + ",\"length\":" + std::to_string(length) + "}";
            };
            agent("artifact", chunk_args("app", 0, 0), true);
            agent("artifact", chunk_args("app", exported.size + 1, 1024), true);
            std::string binary;
            for (std::uint64_t offset = 0;;) {
                auto chunk = test::decode<tools::artifact::output>(agent("artifact", chunk_args("app", offset, 4096)));
                test::require(chunk.offset == offset && chunk.total == exported.size, "artifact range differs");
                auto bytes = tools::lines::unbase64(chunk.data_base64);
                test::require(chunk.next_offset == offset + bytes.size(), "artifact did not advance");
                binary += bytes;
                if (chunk.eof) break;
                offset = chunk.next_offset;
            }
            expect(binary.size() == exported.size);
            auto local = tmp.dir / "downloaded-app";
            test::spit(local, binary);
            std::filesystem::permissions(local, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
            test::child deployed{test::command(local, tmp.dir)};
            expect(deployed.wait() == 0) << "the retrieved executable runs locally";
            expect(!std::filesystem::exists(tmp.dir / "source/.analysis"));
            expect(worker.stop() == 0 && gateway.stop() == 0 && source->stop() == 0);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"analysis">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
