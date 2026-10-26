import std;
import boost.ut;
import language;
import configuration;
import reflection;
import support;
import fixture;
import protocol;

namespace {
    using namespace boost::ut;

    auto mixed() -> builds::configuration {
        auto c = fixture::cmake("build/cmake", "cc");
        auto& cm = fixture::options<"cmake">(c.builders.at("main"));
        cm.inputs = {{"header", "cxx_header"}};
        cm.variables["GENERATED_HEADER"] = "${input:header}";
        cm.export_compile_commands = true;
        cm.outputs["binary"] = {"executable", "build/cmake/app"};
        fixture::needs(c.builders.at("main")).platform.os = "linux";
        builds::config<"bazel"> bz{.build_dir = "build/bazel"};
        bz.operations["generate"] = {.command = "run", .targets = {"//:generate"}, .outputs = {{"header", {"cxx_header", "build/generated.h"}}}};
        bz.operations["package"] = {.command = "run",
                                    .targets = {"//:package"},
                                    .args = {"${input:binary}"},
                                    .inputs = {{"binary", "executable"}},
                                    .outputs = {{"package", {"archive", "build/package.bin"}}}};
        c.builders["bazel"] = builds::with<"bazel">{.options = std::move(bz)};
        fixture::needs(c.builders.at("bazel")).tools = {"sdk"};
        c.tasks["generate"] = {.builder = "bazel", .operation = "generate"};
        c.tasks.at("configure").inputs = {{"header", {"generate", "header"}}};
        c.tasks["package"] = {.builder = "bazel", .operation = "package", .inputs = {{"binary", {"build", "binary"}}}};
        c.operations["build"] = {.tasks = {"package"}};
        c.outputs = {{"database", {"configure", "compilation_database"}}, {"archive", {"package", "package"}}};
        return c;
    }

    static constexpr auto cases = [] {
        "worker requests route and preserve binary payloads"_test = [] {
            static_assert(std::same_as<jobs::reply_t<jobs::source_bytes>, jobs::authority_ack>);
            static_assert(std::same_as<jobs::reply_t<jobs::sync_offer>, jobs::sync_plan>);
            static_assert(std::same_as<jobs::reply_t<jobs::build_request>, jobs::build_report>);
            static_assert(std::same_as<jobs::reply_t<language::request>, language::result>);
            static_assert(jobs::code<jobs::heartbeat> == jobs::kind<"heartbeat">);
            jobs::hash_file file{.path = "dataset/file.cpp"};
            expect(jobs::locate(file) == "dataset");
            std::vector<std::byte> data{std::byte{0}, std::byte{255}};
            jobs::source_bytes push{{{.digest = content::hash_of(data), .offset = 2, .total = 4, .bytes = data}}};
            transport::frame frame{.payload = transport::encode(push)};
            auto decoded = transport::decode<jobs::source_bytes>(frame);
            expect(std::ranges::equal(decoded.parts.front().bytes, data) && decoded.parts.front().offset == 2u);
            frame.payload.pop_back();
            expect(throws<std::length_error>([&] { transport::decode<jobs::source_bytes>(frame); }));
            jobs::build_report report{.worker = "local", .tree = "work", .steps = {{.name = "build", .exit_code = 0}}};
            auto restored = transport::decode<jobs::build_report>({.payload = transport::encode(report)});
            expect(restored.ok() && restored.worker == "local");
            restored.steps.front().timed_out = true;
            expect(!restored.ok() && !jobs::build_report{}.ok());
            language::request request{.project = "app", .config = {.build = mixed(), .inputs = {"database"}}};
            auto prepared = transport::decode<language::request>({.payload = transport::encode(request)});
            expect(language::prepare(prepared.config).inputs.size() == 1u);
        };

        "CMake and Bazel compose by typed inputs"_test = [] {
            auto c = mixed();
            auto plan = builds::prepare(c, {.operation = "build"});
            std::vector<std::string> order;
            for (auto& step : plan.steps)
                order.push_back(step.name);
            expect(order == std::vector<std::string>{"generate", "configure", "build", "package"});
            expect(plan.needs.platform.os == std::optional<std::string>{"linux"});
            for (auto tool : {"bazel", "cmake", "cc", "ninja", "sdk"})
                expect(std::ranges::contains(plan.needs.tools, tool));
            expect(plan.contexts.size() == 2u);
            expect(std::ranges::contains(plan.steps[1].argv, "-DGENERATED_HEADER=${tree}/build/generated.h"));
            expect(std::ranges::contains(plan.steps[3].argv, "${tree}/build/cmake/app"));
            expect(plan.steps[0].argv[1] == "--batch");
            expect(plan.steps[0].argv[2] == "--output_base=${tree}/build/bazel");
            c.operations["prepare"] = {.tasks = {"configure", "build"}};
            auto once = builds::prepare(c, {.operation = "prepare"});
            expect(once.steps.size() == 3u) << "shared dependencies expand once";
            auto prepared = language::prepare({.build = mixed(), .inputs = {"database"}});
            expect(prepared.build.steps.size() == 2u && prepared.inputs[0].type == "compilation_database");
            expect(prepared.server.provider == "clice" && prepared.server.argv == std::vector<std::string>{"clice", "serve"});
            expect(rfl::json::write(prepared.server.initialization) ==
                   R"({"rules":[{"compile_commands":["build/cmake/compile_commands.json"]}],"project":{"cache_dir":"build/cmake/clice"}})");
            auto build = builds::locate(plan.steps[2], "/tree", {}, 3);
            expect(build.env.at("CMAKE_BUILD_PARALLEL_LEVEL") == "3") << "a step uses the jobs its runner allows";
            expect(std::ranges::contains(builds::locate(plan.steps[0], "/tree", {}, 3).argv, "--jobs=3"));
        };

        "graph and type failures are rejected before any execution"_test = [] {
            auto baseline = mixed();
            auto reject = [&](auto mutate) {
                auto c = baseline;
                mutate(c);
                expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "build"}); }));
            };
            reject([](auto& c) { c.tasks.at("configure").inputs.clear(); });
            reject([](auto& c) { c.tasks.at("configure").inputs.at("header").output = "missing"; });
            reject([](auto& c) { c.tasks.at("configure").inputs.at("header") = {"build", "binary"}; });
            reject([](auto& c) { c.tasks.at("generate").after = {"package"}; });
            reject([](auto& c) { c.tasks.at("package").builder = "missing"; });
            reject([](auto& c) { c.tasks.at("package").operation = "missing"; });
            reject([](auto& c) { fixture::needs(c.builders.at("bazel")).platform.os = "windows"; });
            reject([](auto& c) { c.outputs.at("archive").output = "missing"; });
            expect(throws<std::invalid_argument>(
                [&] { builds::prepare(baseline, {.operation = "prepare", .parameters = {.targets = {"app"}}}); }));
            expect(throws<std::invalid_argument>(
                [&] { builds::prepare(baseline, {.operation = "build", .parameters = {.labels = "unit"}}); }));
            expect(throws<std::invalid_argument>([&] { language::prepare({.build = baseline, .inputs = {"archive"}}); })) << "clice reads no archive";
            expect(throws<std::invalid_argument>([&] { language::prepare({.build = baseline}); })) << "clice needs a compilation database";
            expect(!rfl::json::read<builds::builder>(R"({"adapter":"missing","options":{}})")) << "an unknown adapter is refused when parsed";
            expect(!rfl::json::read<language::provider>(R"({"provider":"clangd"})")) << "so is an unknown language provider";
        };

        "independent configurations preserve argv boundaries and local output identities"_test = [] {
            test::scratch tmp{"build-model"};
            auto c = mixed();
            c.env = {{"ROOT_ENV", "worker"}};
            fixture::env(c.builders.at("main")) = {{"NODE_ENV", "source"}};
            auto plan = builds::prepare(c, {.operation = "prepare"});
            auto local = builds::locate(plan.steps[1], tmp.dir / "tree with spaces", {{"cc", "/opt/toolchain/bin/cc"}}, 1);
            expect(local.argv[0] == "cmake") << "an unmapped tool is found on PATH";
            expect(std::ranges::contains(local.argv, "-DCMAKE_C_COMPILER=/opt/toolchain/bin/cc")) << "a mapped tool is this machine's executable";
            expect(std::ranges::contains(local.argv,
                                         "-DGENERATED_HEADER=" + (std::filesystem::absolute(tmp.dir / "tree with spaces") / "build/generated.h").string()));
            expect(local.env.at("NODE_ENV") == "source" && plan.env.at("ROOT_ENV") == "worker");
            auto db = tmp.dir / "build/commands.json";
            test::spit(db, R"([{"directory":"/workspace","file":"main.c","arguments":["cc","-c","main.c"]}])");
            auto result = builds::inspect({"db", "compilation_database", "build/commands.json"}, tmp.dir);
            expect(result.units == 1u && !result.digest.empty());
            test::spit(db, R"([{"directory":"/workspace","file":"main.c","arguments":[]}])");
            expect(throws<std::runtime_error>([&] { builds::inspect({"db", "compilation_database", "build/commands.json"}, tmp.dir); }));
            test::spit(tmp.dir / "build/generated.h", "first");
            auto first = builds::inspect({"header", "cxx_header", "build/generated.h"}, tmp.dir);
            test::spit(tmp.dir / "build/generated.h", "second");
            expect(builds::inspect({"header", "cxx_header", "build/generated.h"}, tmp.dir).digest != first.digest);
            expect(builds::inspect({"sdk", "acme.sdk", "build", "directory"}, tmp.dir).kind == "directory");
        };

        "request parameters go to the operation's receiver only"_test = [] {
            auto c = mixed();
            c.operations["both"] = {.tasks = {"build", "generate"}};
            builds::validate(c);
            expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "both", .parameters = {.targets = {"app"}}}); }))
                << "several final tasks and no receiver";
            c.operations["both"].receiver = "build";
            auto plan = builds::prepare(c, {.operation = "both", .parameters = {.targets = {"app"}}});
            auto step = [&](std::string_view name) { return *std::ranges::find(plan.steps, name, &builds::step::name); };
            expect(std::ranges::contains(step("build").argv, "app"));
            expect(!std::ranges::contains(step("generate").argv, "app") && std::ranges::contains(step("generate").argv, "//:generate"));
            c.operations["both"].receiver = "missing";
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
        };

        "configurations name tools; each machine maps them"_test = [] {
            auto cmake = fixture::cmake("build/x", "/usr/bin/cc");
            expect(throws<std::invalid_argument>([&] { builds::validate(cmake); })) << "a host path";
            auto script = fixture::commands({"scripts/gen.sh", "--fast"});
            auto plan = builds::prepare(script, {.operation = "build"});
            expect(plan.needs.tools.empty()) << "a project file is no tool";
            expect(builds::locate(plan.steps[0], "/tree", {}, 1).argv[0] == "/tree/scripts/gen.sh");
            auto python = fixture::commands({"python", "-c", "pass"});
            plan = builds::prepare(python, {.operation = "build"});
            expect(plan.needs.tools == std::vector<std::string>{"python"} && plan.steps[0].argv[0] == "${tool:python}");
            expect(builds::locate(plan.steps[0], "/tree", {{"python", "/opt/py/bin/python3"}}, 1).argv[0] == "/opt/py/bin/python3");
            expect(throws<std::invalid_argument>([] { builds::validate(fixture::commands({"/usr/bin/python3"})); }));
        };

        "an artifact format brings its own rules"_test = [] {
            builds::config<"command"> wrong{
                .operations = {{"build", {.argv = {"make"}, .outputs = {{"db", {"compilation_database", "build/db", "directory"}}}}}}};
            builds::configuration c;
            c.builders["main"] = builds::with<"command">{.options = std::move(wrong)};
            c.tasks["build"] = {.builder = "main", .operation = "build"};
            c.operations["build"] = {.tasks = {"build"}};
            expect(throws<std::invalid_argument>([&] { builds::validate(c); })) << "a database is a file";
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"build_model">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
