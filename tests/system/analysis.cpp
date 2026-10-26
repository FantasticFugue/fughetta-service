import launch;
import boost.ut;
import client;
import protocol;
import language;
import fixture;
import content;
import nodes;
import semantic;

namespace {
    using namespace boost::ut;
    const worlds::actor actor{.issuer = "source", .user = "analysis-test", .caller = "suite", .role = "service"};

    auto node(net::ip::tcp::endpoint endpoint, nodes::node_request request, bool reading = false) -> nodes::node_reply {
        auto action = request.action;
        auto reply = source::call(endpoint, jobs::node_call{.project = "app", .actor = actor, .request = std::move(request), .reading = reading});
        test::require(reply.code == "ok", action + ": " + reply.code + ": " + reply.reason);
        return reply;
    }
    // A change in main as one batch that takes the global scope for itself.
    auto create(net::ip::tcp::endpoint endpoint, std::string path, std::string text) -> nodes::node_reply {
        auto tree = node(endpoint, {.action = "tree", .worldline = "main"}, true);
        return node(endpoint, {.action = "apply", .worldline = "main", .operation = content::hash_of("create " + path).str(), .expected = tree.frontier,
                               .edits = {{.kind = "create", .path = std::move(path), .text = std::move(text)}}, .scopes = {"::"}});
    }
    auto start_source(const std::filesystem::path& root, std::uint16_t port, const builds::toolset& tools = {}) {
        std::vector<std::string> args{"--port", std::to_string(port), "--storage", (root / "source").string()};
        for (auto& [name, executable] : tools)
            args.insert(args.end(), {"--tool", name + "=" + executable});
        auto process = std::make_unique<test::child>(test::source_command(SERVICE_EXECUTABLE, root, std::move(args)));
        test::eventually([&] {
            test::require(!process->finished(), process->output());
            return process->output().contains("source: internal file service");
        });
        test::ready(*process, port);
        return process;
    }

    static constexpr auto cases = [] {
        "a prepared C++ tree is analyzed, published into its version, and its scopes are locked and edited"_test = [] {
            auto clice = std::getenv("TEST_CLICE");
            if (!clice) {
                std::println("skipped: TEST_CLICE names no clice executable");
                return;
            }
            test::scratch tmp{"survey"};
            auto port = test::port();
            auto endpoint = net::ip::tcp::endpoint{net::ip::address_v4{{127, 0, 0, 1}}, port};
            auto process = start_source(tmp.dir, port);
            const std::string code = "namespace demo {\nint twice(int x) {\n    return x * 2;\n}\nint thrice(int x) {\n    return x * 3;\n}\n}\n";
            create(endpoint, "main.cpp", code);
            auto prepared_at = create(endpoint, "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(survey CXX)\nadd_executable(app main.cpp)\n");
            language::configuration config;
            config.build = fixture::cmake("build/analysis", "c++");
            auto& options = fixture::options<"cmake">(config.build.builders.at("main"));
            options.export_compile_commands = true;
            options.cxx_compiler = "c++";
            config.build.outputs["main"] = {.task = "configure", .output = "compilation_database"};
            config.inputs = {"main"};
            config.server = fixture::clice(clice, {"--log-level", "off"});
            language::request request{.project = "app", .operation_id = "survey", .worldline = "main", .config = config, .environment = "test-environment"};
            auto prepared = source::call(endpoint, request);
            expect((prepared.status == "prepared") >> fatal) << prepared.error << rfl::json::write(prepared.steps);
            if (prepared.analysis != "analyzed")
                std::println("analysis: {}", rfl::json::write(prepared));
            expect((prepared.analysis == "analyzed") >> fatal);
            expect(prepared.revision == prepared_at.version && prepared.scopes >= 3u) << prepared.scopes;

            auto found = node(endpoint, {.action = "semantic", .worldline = "main", .environment = "test-environment"}, true);
            expect((found.semantic.has_value() && !found.stale) >> fatal);
            auto twice = std::ranges::find_if(found.semantic->symbols, [](auto& s) { return s.name == "twice"; });
            expect((twice != found.semantic->symbols.end()) >> fatal);
            auto scope = std::ranges::find(found.semantic->scopes, twice->id, &semantic::scope::id);
            expect((scope != found.semantic->scopes.end() && scope->range.place) >> fatal);
            expect(scope->range.place->path == "main.cpp" && scope->kind == "function");

            node(endpoint, {.action = "lock", .worldline = "main", .environment = "test-environment", .scopes = {twice->id}});
            auto inside = static_cast<std::uint64_t>(code.find("x * 2"));
            auto edited = node(endpoint, {.action = "apply", .worldline = "main", .operation = content::hash_of("inside").str(), .expected = prepared_at.frontier,
                                          .edits = {{.kind = "edit", .path = "main.cpp", .text = "x + x", .offset = inside, .count = 5}}});
            auto outside = static_cast<std::uint64_t>(code.find("x * 3"));
            auto refused = source::call(endpoint, jobs::node_call{.project = "app", .actor = actor, .request = {
                .action = "apply", .worldline = "main", .operation = content::hash_of("outside").str(), .expected = edited.frontier,
                .edits = {{.kind = "edit", .path = "main.cpp", .text = "x + x + x", .offset = outside, .count = 5}}}});
            expect(refused.code == "scope_locked") << "a change outside the locked function needs its scope: " << refused.reason;
            auto later = node(endpoint, {.action = "semantic", .worldline = "main", .environment = "test-environment"}, true);
            expect(later.stale) << "the analysis is older than the edited version";
            auto moved = std::ranges::find(later.semantic->scopes, twice->id, &semantic::scope::id);
            expect(moved != later.semantic->scopes.end() && moved->range.place && moved->range.place->path == "main.cpp") << "the scope follows the edit";
            expect(process->stop() == 0);
        };


        "interrupted preparation survives a source process restart"_test = [] {
            test::scratch tmp{"interruption"};
            auto port = test::port();
            auto endpoint = net::ip::tcp::endpoint{net::ip::address_v4{{127, 0, 0, 1}}, port};
            auto start = [&] {
                auto process = std::make_unique<test::child>(test::source_command(SERVICE_EXECUTABLE, tmp.dir,
                    {"--port", std::to_string(port), "--storage", (tmp.dir / "source").string()}));
                test::eventually([&] {
                    test::require(!process->finished(), process->output());
                    return process->output().contains("source: internal file service");
                });
                return process;
            };
            auto process = start();
            test::ready(*process, port);
            create(endpoint, "main.c", "int main(void) { return 0; }\n");
            create(endpoint, "CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.20)
project(interruption C)
add_executable(app main.c)
file(WRITE ${CMAKE_BINARY_DIR}/started "started")
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 30)
)");
            language::configuration config;
            config.build = fixture::cmake("build/analysis", "cc");
            auto& options = fixture::options<"cmake">(config.build.builders.at("main"));
            options.export_compile_commands = true;
            config.build.outputs["main"] = {.task = "configure", .output = "compilation_database"};
            config.inputs = {"main"};
            config.server = fixture::clice("service-not-installed");
            language::request request{.project = "app", .operation_id = "interrupted", .config = config};
            auto pending = std::async(std::launch::async, [=] {
                try { return source::call(endpoint, request).status; }
                catch (const std::exception&) { return std::string{"disconnected"}; }
            });
            std::filesystem::path marker;
            test::eventually([&] {
                auto trees = tmp.dir / "source/.analysis/trees/app";
                if (!std::filesystem::exists(trees))
                    return false;
                for (const auto& tree : std::filesystem::directory_iterator{trees}) {
                    auto path = tree.path() / "build/analysis/started";
                    if (std::filesystem::exists(path)) {
                        marker = path;
                        return true;
                    }
                }
                return false;
            });
            auto stamp = std::filesystem::last_write_time(marker);
            expect(process->stop() == 0);
            expect(pending.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
            auto outcome = pending.get();
            expect(outcome != "prepared");
            process = start();
            test::ready(*process, port);
            auto receipt = request;
            receipt.action = "receipt";
            auto recovered = source::call(endpoint, receipt);
            expect(recovered.status == "interrupted" && !recovered.error.empty());
            expect(!recovered.replayed && recovered.operation_id == "interrupted");
            auto replay = source::call(endpoint, request);
            expect(replay.status == "interrupted" && replay.replayed);
            expect(std::filesystem::last_write_time(marker) == stamp);
            expect(!std::filesystem::exists(marker.parent_path() / "compile_commands.json"));
            auto source = node(endpoint, {.action = "read", .worldline = "main", .target = "main.c"}, true);
            expect(source.files.size() == 1u && source.files[0].text == "int main(void) { return 0; }\n");
            expect(process->stop() == 0);
        };
        "Python tools execute on the prepared tree and diagnostics survive restart"_test = [] {
            auto uv = std::getenv("TEST_UV"), ty = std::getenv("TEST_TY"), ruff = std::getenv("TEST_RUFF");
            if (!uv || !ty || !ruff) {
                std::println("skipped: TEST_UV, TEST_TY and TEST_RUFF name the Python tool executables");
                return;
            }
            test::scratch tmp{"python-toolchain"};
            auto port = test::port();
            net::ip::tcp::endpoint endpoint{net::ip::address_v4{{127, 0, 0, 1}}, port};
            builds::toolset tools{{"uv", uv}, {"ty", ty}, {"ruff", ruff}};
            auto process = start_source(tmp.dir, port, tools);
            create(endpoint, "pyproject.toml", "[project]\nname = \"toolchain-probe\"\nversion = \"0\"\nrequires-python = \">=3.11\"\n"
                                               "[build-system]\nrequires = [\"uv_build\"]\nbuild-backend = \"uv_build\"\n");
            create(endpoint, "src/toolchain_probe/__init__.py", "def answer() -> int:\n    return 42\n");
            auto version = create(endpoint, "bad.py", "import os\nanswer: int = \"wrong\"\n").version;
            builds::configuration build;
            build.builders["environment"] = builds::with<"uv">{.options = {
                .env_dir = "build/env", .build_dir = "build/dist", .cache_dir = "build/cache", .python = "python3",
                .run_command = {"python", "-c", "from toolchain_probe import answer; print(answer())"}, .locked = false}};
            build.builders["types"] = builds::with<"ty">{.options = {.python = "build/env", .paths = {"bad.py"}}};
            build.builders["lint"] = builds::with<"ruff">{.options = {.paths = {"bad.py"}, .select = {"F401"}}};
            build.tasks = {{"sync", {.builder = "environment", .operation = "sync"}},
                           {"package", {.builder = "environment", .operation = "build", .after = {"sync"}}},
                           {"run", {.builder = "environment", .operation = "run", .after = {"package"}}},
                           {"check", {.builder = "types", .operation = "check", .after = {"sync"}}},
                           {"lint", {.builder = "lint", .operation = "check", .after = {"sync"}}}};
            build.operations = {{"prepare", {.tasks = {"run"}}}, {"check", {.tasks = {"check"}}}, {"lint", {.tasks = {"lint"}}}};
            build.limits.timeout_s = 90;
            language::configuration config{.server = language::with<"ty">{.options = {.python = "build/env"}}, .build = build};
            language::request request{.project = "app", .operation_id = "python-prepare", .worldline = "main", .revision = version,
                                      .config = config, .environment = "python-types"};
            auto prepared = source::call(endpoint, request);
            expect((prepared.status == "prepared") >> fatal) << prepared.error << rfl::json::write(prepared.steps);
            expect(prepared.steps.size() == 3u && prepared.steps.back().output.contains("42"));
            expect((prepared.analysis == "analyzed") >> fatal) << prepared.analysis_error.value_or("");
            auto semantic = node(endpoint, {.action = "semantic", .worldline = "main", .environment = "python-types"}, true);
            expect((semantic.semantic.has_value() && !semantic.semantic->diagnostics.empty()) >> fatal);
            expect(std::ranges::any_of(semantic.semantic->diagnostics, [](auto& d) {
                return d.range.place && d.range.place->path == "bad.py" && !d.range.document.empty();
            }));
            auto replay = source::call(endpoint, request);
            expect(replay.replayed && replay.tree == prepared.tree);
            for (auto operation : {"check", "lint"}) {
                request.operation_id = std::string{"python-"} + operation;
                request.config.operation = operation;
                auto failed = source::call(endpoint, request);
                expect(failed.status == "failed") << rfl::json::write(failed);
                expect(!failed.steps.empty() && failed.steps.back().exit_code != 0);
            }
            request.operation_id = "python-ruff-language";
            request.config = {.server = language::with<"ruff">{}};
            request.environment = "python-lint";
            auto linted = source::call(endpoint, request);
            expect((linted.status == "prepared" && linted.analysis == "analyzed") >> fatal) << rfl::json::write(linted);
            auto lint = node(endpoint, {.action = "semantic", .worldline = "main", .environment = "python-lint"}, true);
            expect(lint.semantic.has_value() && !lint.semantic->diagnostics.empty());
            expect(process->stop() == 0);
            process = start_source(tmp.dir, port, tools);
            auto restored = node(endpoint, {.action = "semantic", .worldline = "main", .environment = "python-types"}, true);
            expect(restored.semantic.has_value() && restored.semantic->version == version && restored.semantic->diagnostics.size() == semantic.semantic->diagnostics.size());
            expect(process->stop() == 0);
        };
    };
}

int main(int argc, const char** argv) {
    boost::ut::suite<"analysis">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
