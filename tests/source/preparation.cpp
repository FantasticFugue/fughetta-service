import std;
import boost.ut;
import coroutine;
import native;
import protocol;
import language;
import commands;
import reflection;
import support;
import fixture;
import content;
import nodes;

namespace {
    using namespace boost::ut;

    template <class Request> auto call(net::ip::tcp::endpoint endpoint, Request request) -> jobs::reply_t<Request> {
        native::context context;
        auto io = context.get_scheduler();
        auto task = [&]() -> coroutine::task<jobs::reply_t<Request>> {
            auto socket = co_await io::connect(io, endpoint);
            transport::channel<native::socket> channel{socket};
            co_return co_await coroutine::with_scope([&](coroutine::scope& tasks) -> coroutine::task<jobs::reply_t<Request>> {
                tasks.spawn(channel.pump([](const transport::frame&) {}));
                co_await channel.post(jobs::hello{.name = "preparation-test", .auth = jobs::source_token(), .protocol = jobs::protocol_version});
                auto result = co_await channel.call(std::move(request));
                if (!result)
                    transport::raise_call_error(std::move(result.error()));
                co_return std::move(*result);
            });
        };
        return *cores::run(context, task());
    }

    auto configuration() -> language::configuration {
        const std::string code = "import json,sys; from pathlib import Path; p=Path(sys.argv[1]); "
                                 "p.parent.mkdir(parents=True,exist_ok=True); "
                                 "p.write_text(json.dumps([{'directory':sys.argv[2],'file':'main.c',"
                                 "'arguments':['cc','-D'+sys.argv[3],'-c','main.c']}]))";
        builds::config<"command"> command;
        for (const auto& name : {"FIRST", "LAST"})
            command.operations[name] = {.argv = {"python3", "-c", code, "${tree}/build/commands.json", "${tree}", name},
                                        .outputs = {{"db", {"compilation_database", "build/commands.json"}}}, .build_dirs = {"build"}};
        language::configuration config;
        config.build.builders["main"] = builds::with<"command">{.options = command};
        config.build.tasks["first"] = {.builder = "main", .operation = "FIRST"};
        config.build.tasks["last"] = {.builder = "main", .operation = "LAST", .after = {"first"}};
        config.build.operations["prepare"] = {.tasks = {"last"}};
        config.build.outputs["main"] = {.task = "first", .output = "db"};
        config.inputs = {"main"};
        return config;
    }

    const worlds::actor actor{.issuer = "source", .user = "preparation-test", .caller = "suite", .role = "service"};

    // The source lives in the node: a change is one batch that takes the global scope for itself.
    auto node(net::ip::tcp::endpoint endpoint, nodes::node_request request, bool reading = false) -> nodes::node_reply {
        auto action = request.action;
        auto reply = call(endpoint, jobs::node_call{.project = "app", .actor = actor, .request = std::move(request), .reading = reading});
        if (reply.code != "ok")
            throw std::runtime_error(action + ": " + reply.code + ": " + reply.reason);
        return reply;
    }
    auto write(net::ip::tcp::endpoint endpoint, std::string text, std::string operation, std::string path = "main.c", std::string worldline = "main") -> nodes::node_reply {
        auto tree = node(endpoint, {.action = "tree", .worldline = worldline}, true);
        bool exists = std::ranges::contains(tree.files, path, &nodes::file_view::path);
        return node(endpoint, {.action = "apply", .worldline = worldline, .operation = content::hash_of(operation).str(), .expected = tree.frontier,
                               .edits = {{.kind = exists ? "write" : "create", .path = std::move(path), .text = std::move(text)}}, .scopes = {"::"}});
    }
    auto source_text(net::ip::tcp::endpoint endpoint, std::string path, std::string worldline = "main") -> std::string {
        auto read = node(endpoint, {.action = "read", .worldline = std::move(worldline), .target = path}, true);
        auto file = std::ranges::find(read.files, path, &nodes::file_view::path);
        if (file == read.files.end())
            throw std::runtime_error(path + " is not in the source");
        return file->text;
    }

    static constexpr auto cases = [] {
        "source mixed graph runs Bazel CMake Bazel and native nested commands"_test = [] {
            auto bazel = std::getenv("TEST_BAZEL");
            if (!bazel || !*bazel) {
                std::println("SKIP: source mixed build graph requires Bazel (TEST_BAZEL or bazel on PATH)");
                return;
            }
#ifdef _WIN32
            std::println("SKIP: source mixed build fixture requires a POSIX shell for the Bazel executable rule");
            return;
#endif
            test::scratch tmp{"systems"};
            language::configuration config;
            builds::config<"cmake"> cmake{.build_dir = "build/source-cmake", .c_compiler = "cc", .export_compile_commands = true};
            cmake.targets = {"app", "native_bazel"};
            cmake.inputs = {{"header", "cxx_header"}};
            cmake.variables = {{"GENERATED_HEADER", "${input:header}"}, {"BAZEL", "${tool:bazel}"},
                               {"BAZEL_BASE", "${tree}/build/source-bazel"}, {"NATIVE_PROOF", "${tree}/build/source-cmake/native-proof"}};
            cmake.outputs = {{"binary", {"executable", "build/source-cmake/app"}}, {"proof", {"marker", "build/source-cmake/native-proof"}}};
            builds::config<"bazel"> bazel_config{.build_dir = "build/source-bazel", .startup_options = {"--host_jvm_args=-Xmx512m"}};
            auto options = std::vector<std::string>{"--jobs=2", "--host_platform=//:local", "--platforms=//:local"};
            bazel_config.operations["generate"] = {.command = "run", .targets = {"//:bridge"}, .options = options,
                .args = {"generate", "${tree}/build/source-generated/generated.h"}, .outputs = {{"header", {"cxx_header", "build/source-generated/generated.h"}}}};
            bazel_config.operations["package"] = {.command = "run", .targets = {"//:bridge"}, .options = options,
                .args = {"package", "${input:binary}", "${tree}/build/source-package.bin"}, .inputs = {{"binary", "executable"}},
                .outputs = {{"package", {"archive", "build/source-package.bin"}}}};
            config.build.builders["cmake"] = builds::with<"cmake">{.options = cmake, .needs = builds::requirements{.tools = {"bazel"}}};
            config.build.builders["bazel"] = builds::with<"bazel">{.options = bazel_config};
            config.build.tasks = {{"generate", {.builder = "bazel", .operation = "generate"}},
                {"configure", {.builder = "cmake", .operation = "configure", .inputs = {{"header", {"generate", "header"}}}}},
                {"compile", {.builder = "cmake", .operation = "build", .after = {"configure"}}},
                {"package", {.builder = "bazel", .operation = "package", .inputs = {{"binary", {"compile", "binary"}}}}}};
            config.build.operations["prepare"] = {.tasks = {"package"}};
            config.build.outputs["database"] = {.task = "configure", .output = "compilation_database"};
            config.build.env["BUILD_OWNER"] = "source";
            config.build.limits.timeout_s = 90;
            config.inputs = {"database"};
            config.server = fixture::clice("service-not-installed");
            language::request request{.project = "app", .operation_id = "mixed", .config = config};
            language::result prepared;
            {
                test::source_service source{tmp.dir, {{"bazel", bazel}}};
                auto endpoint = source.endpoint();
                write(endpoint, "module(name = \"fixture\")\n", "module", "MODULE.bazel");
                write(endpoint, "load(\":bridge.bzl\", \"bridge\")\nplatform(name = \"local\")\nbridge(name = \"bridge\")\n", "build", "BUILD.bazel");
                write(endpoint, R"bzl(def _bridge_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(output, """#!/bin/sh
set -eu
case "$1" in
generate)
    mkdir -p "$(dirname "$2")"
    printf '#define GREETING "from bazel"\\n' > "$2"
    ;;
package)
    cmake -E copy "$2" "$3"
    "$2"
    ;;
native-cmake)
    cmake -E touch "$2"
    ;;
esac
""", is_executable=True)
    return [DefaultInfo(executable=output)]
bridge = rule(implementation=_bridge_impl, executable=True)
)bzl", "bridge", "bridge.bzl");
                write(endpoint, R"(cmake_minimum_required(VERSION 3.20)
project(mixed C)
if(NOT "$ENV{BUILD_OWNER}" STREQUAL "source")
  message(FATAL_ERROR "source environment is missing")
endif()
get_filename_component(HEADER_DIR "${GENERATED_HEADER}" DIRECTORY)
add_executable(app main.c "${GENERATED_HEADER}")
target_include_directories(app PRIVATE "${HEADER_DIR}")
add_custom_target(native_bazel
  COMMAND "${BAZEL}" --batch "--output_base=${BAZEL_BASE}" "--output_user_root=${BAZEL_BASE}/user"
    --host_jvm_args=-Xmx512m run --repo_contents_cache= --jobs=2 --host_platform=//:local --platforms=//:local //:bridge -- native-cmake "${NATIVE_PROOF}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
)", "cmake", "CMakeLists.txt");
                write(endpoint, "#include <stdio.h>\n#include \"generated.h\"\nint main(void) { puts(GREETING); return 0; }\n", "main", "main.c");
                prepared = call(endpoint, request);
                expect((prepared.status == "prepared" && prepared.inputs.size() == 1u) >> fatal) << prepared.error
                    << rfl::json::write(prepared.steps);
                expect(prepared.inputs[0].units == 1u && prepared.steps.size() == 4u);
                expect(prepared.steps.back().output.contains("from bazel"));
                auto tree = std::filesystem::path{prepared.tree};
                expect(std::filesystem::path{prepared.inputs[0].path}.parent_path().filename() == "source-cmake");
                expect(std::filesystem::exists(tree / "build/source-cmake/native-proof"));
                expect(test::slurp(tree / "build/source-package.bin") == test::slurp(tree / "build/source-cmake/app"));
                expect(!std::filesystem::exists(tmp.dir / "acme/app/build"));
                auto replay = call(endpoint, request);
                expect(replay.replayed && rfl::json::write(replay.inputs) == rfl::json::write(prepared.inputs));
                auto header = tree / "build/source-generated/generated.h";
                auto original = test::slurp(header);
                test::spit(header, original + "// changed\n");
                auto receipt = request;
                receipt.action = "receipt";
                expect(call(endpoint, receipt).status == "stale");
                test::spit(header, original);
            }
            test::source_service source{tmp.dir};
            request.action = "receipt";
            auto recovered = call(source.endpoint(), request);
            expect(recovered.status == "prepared" && rfl::json::write(recovered.inputs) == rfl::json::write(prepared.inputs));
        };

        "native preset and toolchain prepare an isolated database"_test = [] {
            test::scratch tmp{"native"};
            test::source_service source{tmp.dir};
            auto endpoint = source.endpoint();
            write(endpoint, "int main(void) { return 0; }\n", "main");
            write(endpoint, R"(cmake_minimum_required(VERSION 3.20)
project(native C)
if(NOT ANALYSIS_PRESET STREQUAL "loaded" OR NOT ANALYSIS_TOOLCHAIN STREQUAL "loaded")
  message(FATAL_ERROR "preset or toolchain was not applied")
endif()
if(NOT "$ENV{ANALYSIS_FLAG}" STREQUAL "registered")
  message(FATAL_ERROR "registered environment was not applied")
endif()
add_executable(app main.c)
add_custom_target(generated COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/generated.h)
)", "cmake", "CMakeLists.txt");
            write(endpoint, R"({"version":3,"configurePresets":[{"name":"analysis","generator":"Ninja","binaryDir":"${sourceDir}/build/preset-output","cacheVariables":{"ANALYSIS_PRESET":"loaded"}}]})",
                  "preset", "CMakePresets.json");
            write(endpoint, "set(ANALYSIS_TOOLCHAIN loaded CACHE STRING \"\")\n", "toolchain", "cmake/toolchain.cmake");
            language::configuration config;
            config.build = fixture::cmake("build/analysis", "cc");
            auto& options = fixture::options<"cmake">(config.build.builders.at("main"));
            options.preset = "analysis";
            options.generator = "ignored-by-preset";
            options.toolchain_file = "cmake/toolchain.cmake";
            options.export_compile_commands = true;
            config.build.env["ANALYSIS_FLAG"] = "registered";
            config.build.tasks["generate"] = {.builder = "main", .operation = "build", .after = {"configure"}, .parameters = {.targets = {"generated"}}};
            config.build.operations["prepare"].tasks = {"generate"};
            config.build.outputs["main"] = {.task = "configure", .output = "compilation_database"};
            config.inputs = {"main"};
            config.server = fixture::clice("service-not-installed");
            auto result = call(endpoint, language::request{.project = "app", .operation_id = "native", .config = config});
            expect((result.status == "prepared" && result.inputs.size() == 1u) >> fatal) << result.error;
            expect(result.inputs[0].units == 1u);
            expect(std::filesystem::path{result.inputs[0].path}.parent_path().filename() == "analysis");
            auto tree = std::filesystem::path{result.tree};
            expect(std::filesystem::exists(tree / "build/analysis/generated.h"));
            expect(!std::filesystem::exists(tree / "build/preset-output"));
            expect(!std::filesystem::exists(tmp.dir / "acme/app/build"));
            expect(test::slurp(tree / "build/analysis/CMakeCache.txt").contains("ANALYSIS_TOOLCHAIN:STRING=loaded"));
        };

        "preparation rejects source rewritten by configure"_test = [] {
            test::scratch tmp{"mutation"};
            test::source_service source{tmp.dir};
            auto endpoint = source.endpoint();
            const std::string original = "int main(void) { return 0; }\n";
            write(endpoint, original, "main");
            write(endpoint, R"(cmake_minimum_required(VERSION 3.20)
project(mutation C)
add_executable(app main.c)
file(APPEND ${CMAKE_CURRENT_SOURCE_DIR}/main.c "// modified by preparation\n")
)", "cmake", "CMakeLists.txt");
            language::configuration config;
            config.build = fixture::cmake("build/analysis", "cc");
            auto& options = fixture::options<"cmake">(config.build.builders.at("main"));
            options.export_compile_commands = true;
            config.build.outputs["main"] = {.task = "configure", .output = "compilation_database"};
            config.inputs = {"main"};
            config.server = fixture::clice("service-not-installed");
            language::request request{.project = "app", .operation_id = "mutation", .config = config};
            auto result = call(endpoint, request);
            expect(result.status == "failed" && result.error.contains("source")) << result.error;
            expect(result.steps.size() == 1u && result.steps[0].exit_code == 0);
            expect(test::slurp(std::filesystem::path{result.tree} / "main.c").contains("modified by preparation"));
            expect(source_text(endpoint, "main.c") == original);
            request.action = "receipt";
            expect(call(endpoint, request).status == "failed");
        };

        "final database measurements and receipts survive edits, restoration and source restart"_test = [] {
            test::scratch tmp{"preparation"};
            language::request request{.project = "app", .operation_id = "final", .config = configuration()};
            std::string database, original, tree;
            {
                test::source_service source{tmp.dir};
                auto endpoint = source.endpoint();
                write(endpoint, "int main(void) { return 0; }\n", "initial");
                auto result = call(endpoint, request);
                expect((result.status == "prepared" && result.inputs.size() == 1u) >> fatal) << result.error;
                expect(result.steps.size() == 2u && result.artifacts.size() == 2u);
                database = result.inputs[0].path;
                tree = result.tree;
                original = test::slurp(database);
                expect(original.contains("LAST") && !original.contains("FIRST"));
                for (const auto& artifact : result.artifacts)
                    expect(artifact.digest == result.inputs[0].digest);
                expect(result.inputs[0].units == 1u && result.snapshot == content::manifest{{
                    {.path = "main.c", .digest = content::hash_of("int main(void) { return 0; }\n"), .size = 29}}}.id());
                auto replayed = call(endpoint, request);
                expect(replayed.status == "prepared" && replayed.replayed && replayed.tree == tree);
                auto changed = request;
                changed.ignore = {"other"};
                expect(throws<std::runtime_error>([&] { call(endpoint, changed); }));
                auto receipt = request;
                receipt.action = "receipt";
                expect(call(endpoint, receipt).status == "prepared");
                test::spit(database, "[]");
                expect(call(endpoint, receipt).status == "stale");
                test::spit(database, original);
                expect(call(endpoint, receipt).status == "prepared");
                auto retained = std::filesystem::path{tree} / "main.c";
                auto stamp = std::filesystem::last_write_time(retained);
                test::spit(retained, "int main(void) { return 1; }\n");
                std::filesystem::last_write_time(retained, stamp);
                expect(call(endpoint, receipt).status == "stale");
                test::spit(retained, "int main(void) { return 0; }\n");
                expect(call(endpoint, receipt).status == "prepared");
                expect(source_text(endpoint, "main.c") == "int main(void) { return 0; }\n");
            }
            {
                test::source_service source{tmp.dir};
                auto receipt = request;
                receipt.action = "receipt";
                auto recovered = call(source.endpoint(), receipt);
                expect(recovered.status == "prepared" && !recovered.replayed && recovered.tree == tree);
                auto replayed = call(source.endpoint(), request);
                expect(replayed.status == "prepared" && replayed.replayed);
                test::spit(tmp.dir / ".analysis/receipts/app/interrupted.json",
                           std::format(R"({{"input":"pending","until":0,"result":{}}})",
                                       rfl::json::write(language::result{.operation_id = "interrupted", .status = "preparing"})));
                receipt.operation_id = "interrupted";
                expect(call(source.endpoint(), receipt).status == "interrupted");
                receipt.operation_id = "missing";
                expect(throws<std::runtime_error>([&] { call(source.endpoint(), receipt); }));
            }
        };

        "worldline preparation retains the requested source snapshot"_test = [] {
            test::scratch tmp{"preparation-world"};
            test::source_service source{tmp.dir};
            auto endpoint = source.endpoint();
            write(endpoint, "int initial;\n", "initial");
            auto main = node(endpoint, {.action = "tree", .worldline = "main"}, true);
            node(endpoint, {.action = "lock", .worldline = "main", .scopes = {"::"}});
            auto branch = node(endpoint, {.action = "fork", .worldline = "analysis", .operation = content::hash_of("fork").str(), .source = main.frontier});
            node(endpoint, {.action = "unlock", .worldline = "main", .scopes = {"::"}});
            write(endpoint, "int changed;\n", "change");
            language::request request{.project = "app", .operation_id = "branch", .worldline = "analysis", .revision = branch.version,
                                        .config = configuration()};
            auto prepared = call(endpoint, request);
            expect((prepared.status == "prepared") >> fatal) << prepared.error;
            expect(prepared.worldline == "analysis" && prepared.revision == branch.version);
            expect(test::slurp(std::filesystem::path{prepared.tree} / "main.c") == "int initial;\n");
            expect(source_text(endpoint, "main.c") == "int changed;\n");
        };

        "a language server that never answers ends its analysis in time and leaves the next preparation free"_test = [] {
            test::scratch tmp{"preparation-silent"};
            test::source_service source{tmp.dir};
            auto endpoint = source.endpoint();
            write(endpoint, "int main(void) { return 0; }\n", "initial", "main.cpp");
            write(endpoint, "import time\ntime.sleep(30)\n", "silent server", "serve");
            auto config = configuration();
            config.server = fixture::clice("python3");
            config.build.limits.timeout_s = 3;
            auto began = std::chrono::steady_clock::now();
            auto silent = call(endpoint, language::request{.project = "app", .operation_id = "silent", .config = config, .environment = "env"});
            expect(silent.status == "prepared" && silent.analysis == "failed") << silent.analysis;
            expect(silent.analysis_error.value_or("").contains("did not start")) << silent.analysis_error.value_or("");
            expect(std::chrono::steady_clock::now() - began < std::chrono::seconds{15});
            auto next = call(endpoint, language::request{.project = "app", .operation_id = "next", .config = configuration()});
            expect(next.status == "prepared") << next.error;
        };

        "a link left in a staged tree never takes a later checkout outside it"_test = [] {
            test::scratch tmp{"preparation-link"};
            test::source_service source{tmp.dir};
            auto endpoint = source.endpoint();
            write(endpoint, "int main(void) { return 0; }\n", "initial");
            write(endpoint, "int helper;\n", "helper", "src/helper.c");
            auto first = call(endpoint, language::request{.project = "app", .operation_id = "first", .config = configuration()});
            expect((first.status == "prepared") >> fatal) << first.error;
            auto tree = std::filesystem::path{first.tree};
            auto outside = tmp.dir / "outside";
            std::filesystem::create_directories(outside);
            std::filesystem::remove_all(tree / "src");
            std::filesystem::create_directory_symlink(outside, tree / "src");
            write(endpoint, "int helper = 1;\n", "helper-changed", "src/helper.c");
            auto second = call(endpoint, language::request{.project = "app", .operation_id = "second", .config = configuration()});
            expect(second.status == "prepared") << second.error;
            expect(std::filesystem::is_empty(outside)) << "nothing was written through the link";
            expect(!std::filesystem::is_symlink(std::filesystem::path{second.tree} / "src"));
            expect(test::slurp(std::filesystem::path{second.tree} / "src/helper.c") == "int helper = 1;\n");
        };

        "a staged tree survives a link planted at its root and a path that switched between file and directory"_test = [] {
            test::scratch tmp{"preparation-swap"};
            test::source_service source{tmp.dir};
            auto endpoint = source.endpoint();
            write(endpoint, "int main(void) { return 0; }\n", "initial");
            write(endpoint, "a file\n", "lib-file", "lib");
            auto first = call(endpoint, language::request{.project = "app", .operation_id = "first", .config = configuration()});
            expect((first.status == "prepared") >> fatal) << first.error;
            auto tree = std::filesystem::path{first.tree};
            auto outside = tmp.dir / "outside";
            std::filesystem::create_directories(outside);
            std::filesystem::rename(tree, tmp.dir / "moved");
            std::filesystem::create_directory_symlink(outside, tree);
            auto main = node(endpoint, {.action = "tree", .worldline = "main"}, true);
            node(endpoint, {.action = "apply", .worldline = "main", .operation = content::hash_of("swap").str(), .expected = main.frontier,
                            .edits = {{.kind = "remove", .path = "lib"}}, .scopes = {"::"}});
            write(endpoint, "int helper;\n", "lib-dir", "lib/helper.c");
            auto second = call(endpoint, language::request{.project = "app", .operation_id = "second", .config = configuration()});
            expect(second.status == "prepared") << second.error;
            expect(std::filesystem::is_empty(outside)) << "nothing was written through a link at the tree's root";
            expect(test::slurp(std::filesystem::path{second.tree} / "lib/helper.c") == "int helper;\n") << "a file became a directory";
        };

        "failed preparation retains reports and protects tracked outputs"_test = [] {
            test::scratch tmp{"preparation-errors"};
            test::source_service source{tmp.dir};
            auto endpoint = source.endpoint();
            write(endpoint, "int source;\n", "initial");
            auto config = configuration();
            fixture::options<"command">(config.build.builders.at("main")).operations.at("FIRST").argv = {
                "python3", "-c", "print('failed output'); raise SystemExit(3)"};
            language::request request{.project = "app", .operation_id = "failed", .config = config};
            auto failed = call(endpoint, request);
            expect(failed.status == "failed" && failed.steps.size() == 1u);
            expect(failed.steps[0].exit_code == 3 && failed.steps[0].output.contains("failed output"));
            expect(call(endpoint, request).replayed);
            fixture::options<"command">(request.config.build.builders.at("main")).operations.at("FIRST").argv = {
                "python3", "-c", "import time; print('started',flush=True); time.sleep(30)"};
            request.config.build.limits.timeout_s = 1;
            request.operation_id = "timeout";
            auto timeout = call(endpoint, request);
            expect(timeout.status == "failed" && timeout.steps.size() == 1u && timeout.steps[0].timed_out);
            request.config = configuration();
            request.config.build.enabled = false;
            request.operation_id = "disabled";
            expect(throws<std::runtime_error>([&] { call(endpoint, request); }));
            request.config = configuration();
            request.operation_id = "overlap";
            write(endpoint, "tracked", "tracked-output", "build/commands.json");
            auto overlap = call(endpoint, request);
            expect(overlap.status == "failed" && overlap.error.contains("overlaps tracked source"));
            expect(source_text(endpoint, "build/commands.json") == "tracked");
            expect(source_text(endpoint, "main.c") == "int source;\n");
        };
    };
} // namespace

int main(int argc, const char** argv) {
    boost::ut::suite<"preparation">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
