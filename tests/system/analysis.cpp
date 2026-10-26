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
    auto start_source(const std::filesystem::path& root, std::uint16_t port) {
        auto process = std::make_unique<test::child>(test::source_command(SERVICE_EXECUTABLE, root, {"--port", std::to_string(port), "--storage", (root / "source").string()}));
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
    };
}

int main(int argc, const char** argv) {
    boost::ut::suite<"analysis">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
