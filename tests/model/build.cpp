import std;
import boost.ut;
import language;
import configuration;
import reflection;
import support;
import fixture;
import protocol;
import semantic;

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
            expect(throws<std::invalid_argument>([&] { builds::prepare(baseline, {.operation = "prepare", .parameters = {.targets = {"app"}}}); }));
            expect(throws<std::invalid_argument>([&] { builds::prepare(baseline, {.operation = "build", .parameters = {.labels = "unit"}}); }));
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

        "CMake presets and CTest selection become executable plans"_test = [] {
            auto c = fixture::cmake("build/cmake", "cc");
            auto& o = fixture::options<"cmake">(c.builders.at("main"));
            o.preset = "debug";
            o.toolchain_file = "/opt/vcpkg/scripts/buildsystems/vcpkg.cmake";
            o.configuration = "Release";
            o.test.include = "^unit";
            o.test.labels = "fast";
            o.test.exclude_labels = "network";
            o.test.timeout = 30;
            auto p = builds::prepare(c, {.operation = "test", .parameters = {.filter = "^gateway", .parallel = 3}});
            auto& configure = p.steps.front().argv;
            expect(std::ranges::contains(configure, "--preset"));
            expect(std::ranges::contains(configure, "-DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake"));
            expect(!std::ranges::contains(configure, "-G"));
            auto local = builds::locate(p.steps.back(), "/work/project", {{"ctest", "/opt/cmake/bin/ctest"}}, 2, "/work/reports");
            auto after = [&](std::string_view flag) {
                auto at = std::ranges::find(local.argv, flag);
                return at == local.argv.end() || ++at == local.argv.end() ? std::string{} : *at;
            };
            expect(local.argv.front() == "/opt/cmake/bin/ctest");
            expect(after("--test-dir") == "/work/project/build/cmake" && after("-R") == "^gateway");
            expect(after("-L") == "fast" && after("-LE") == "network" && after("-j") == "3");
            expect(after("--timeout") == "30" && after("-C") == "Release");
            expect(after("--output-junit") == "/work/reports/ctest.xml" && local.reports == std::vector<builds::report>{{"junit", "ctest.xml"}});
            expect(std::ranges::contains(local.argv, "--no-tests=error"));
            expect(local.env.at("CTEST_PARALLEL_LEVEL") == "2");
        };
        "extra CMake and CTest options cannot replace structured settings"_test = [] {
            for (auto option : {"-Belsewhere", "-Selsewhere", "-GMakefiles", "--preset=other", "-DCMAKE_C_COMPILER=other", "-DCMAKE_BUILD_TYPE:STRING=Release",
                                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"}) {
                auto c = fixture::cmake("build/cmake");
                fixture::options<"cmake">(c.builders.at("main")).configure_options = {option};
                expect(throws<std::invalid_argument>([&] { builds::validate(c); })) << option;
            }
            for (auto option : {"--no-tests=ignore", "-j4", "--tests-regex=skip", "--test-dir=elsewhere", "-N", "--output-junit=other.xml"}) {
                auto c = fixture::cmake("build/cmake");
                expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "test", .parameters = {.args = {option}}}); })) << option;
            }
            auto c = fixture::cmake("build/cmake");
            auto& o = fixture::options<"cmake">(c.builders.at("main"));
            o.test.no_tests_action = "ignore";
            o.build_options = {"--", "-k", "0"};
            builds::validate(c);
            expect(std::ranges::contains(builds::prepare(c, {.operation = "test"}).steps.back().argv, "--no-tests=ignore"));
            o.test.parallel = 0;
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
        };
        "CTest and Bazel tests report in JUnit and the Build Event Protocol"_test = [] {
            auto c = fixture::cmake("build/cmake");
            auto p = builds::prepare(c, {.operation = "test"});
            expect(p.steps.front().reports.empty() && p.steps.back().reports.size() == 1u) << "only the test step writes a report";

            auto ctest = builds::report_protocols::read("junit", R"(<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="Linux-c++" tests="3" failures="1" disabled="0" skipped="1" hostname="" time="0" timestamp="2026-10-27T10:00:00">
	<testcase name="unit" classname="unit" time="0.01" status="run"><system-out>ok</system-out></testcase>
	<testcase name="gateway" classname="gateway" time="0.2" status="fail"><failure message="Failed"/><system-out>assert</system-out></testcase>
	<testcase name="network" classname="network" time="0" status="notrun"><skipped message="Disabled"/></testcase>
</testsuite>)");
            expect((ctest.tests.has_value()) >> fatal);
            expect(ctest.tests->total == 3 && ctest.tests->failed == 1 && ctest.tests->failed_tests == std::vector<std::string>{"gateway"});

            auto bazel = builds::report_protocols::read(
                "bep", R"({"id":{"started":{}},"started":{"uuid":"1"}})"
                       "\n"
                       R"({"id":{"testSummary":{"label":"//pkg:unit","configuration":{"id":"k8"}}},"testSummary":{"overallStatus":"PASSED","totalRunCount":1}})"
                       "\n"
                       R"({"id":{"testSummary":{"label":"//pkg:slow","configuration":{"id":"k8"}}},"testSummary":{"overallStatus":"TIMEOUT"}})"
                       "\n"
                       R"({"id":{"testSummary":{"label":"//pkg:flaky","configuration":{"id":"k8"}}},"testSummary":{"overallStatus":"FLAKY"}})"
                       "\n"
                       R"({"id":{"testSummary":{"label":"//pkg:broken","configuration":{"id":"k8"}}},"testSummary":{"totalRunCount":0}})"
                       "\n");
            expect((bazel.tests.has_value()) >> fatal);
            expect(bazel.tests->total == 4 && bazel.tests->failed == 2);
            expect(bazel.tests->failed_tests == std::vector<std::string>{"//pkg:slow", "//pkg:broken"});
            auto suites = builds::report_protocols::read("junit", R"(<testsuites><testsuite name="unit"><testcase name="t" classname="m"/></testsuite>
<testsuite name="integration"><testcase name="t" classname="m"><failure/></testcase></testsuite></testsuites>)");
            expect(suites.tests->total == 2 && suites.tests->failed_tests == std::vector<std::string>{"m.t"}) << "cases of two suites stay apart";
            expect(throws([] { builds::report_protocols::read("junit", "<testsuite"); }));
        };
        "a worker reads the reports a step left, relative to the tree"_test = [] {
            test::scratch tmp{"build-reports"};
            auto c = fixture::cmake("build/cmake");
            auto& o = fixture::options<"cmake">(c.builders.at("main"));
            o.source_dir = "app";
            auto p = builds::prepare(c, {.operation = "test"});
            auto step = p.steps.back();
            step.reports.push_back({"codequality", "lint.json"});
            auto local = builds::locate(step, tmp.dir / "tree", {}, 1, tmp.dir / "reports");
            test::spit(tmp.dir / "reports/ctest.xml", R"(<testsuite><testcase name="a" classname="a"><error message="crash"/></testcase></testsuite>)");
            test::spit(tmp.dir / "reports/lint.json",
                       R"([{"description":"F401: unused","check_name":"F401","severity":"major",)"
                       R"("location":{"path":"src/m.py","positions":{"begin":{"line":1,"column":8}}}},)"
                       R"({"description":"outside","severity":"minor","location":{"path":"../../elsewhere.py","lines":{"begin":3}}}])");
            auto found = builds::collect(local, tmp.dir / "reports", tmp.dir / "tree");
            expect((found.tests && found.diagnostics.size() == 2u) >> fatal);
            expect(found.tests->failed_tests == std::vector<std::string>{"a"});
            expect(found.diagnostics[0].file == "app/src/m.py" && found.diagnostics[0].column == 8 && found.diagnostics[0].severity == "error");
            expect(found.diagnostics[1].file == "../../elsewhere.py" && found.diagnostics[1].line == 3 && found.diagnostics[1].severity == "warning");
            test::spit(tmp.dir / "reports/ctest.xml", "not xml");
            std::filesystem::remove(tmp.dir / "reports/lint.json");
            found = builds::collect(local, tmp.dir / "reports", tmp.dir / "tree");
            expect((found.diagnostics.size() == 1u && !found.tests) >> fatal) << "a missing report adds no result";
            expect(found.diagnostics[0].message.starts_with("the junit report ctest.xml could not be read"));
        };
        "a step's report is a plain file name of a known protocol"_test = [] {
            for (auto r : std::vector<builds::report>{{"junit", "../x.xml"}, {"junit", "a/b.xml"}, {"junit", ""}, {"tap", "t.tap"}, {"junit", "ctest.xml"}}) {
                auto c = fixture::cmake("build/cmake");
                c.tasks.at("test").reports = {r};
                expect(throws<std::invalid_argument>([&] { builds::validate(c); })) << r.file;
            }
        };
        "worker reports carry language diagnostics to source publication"_test = [] {
            jobs::build_report report;
            report.outline = semantic::outline{};
            report.outline->diagnostics.push_back({.range = {.place = semantic::place{"app.py", 2, 5}}, .severity = "error", .message = "invalid assignment"});
            auto decoded = transport::codec<jobs::build_report>::decode(transport::encode(report));
            expect((decoded.outline.has_value()) >> fatal);
            expect(decoded.outline->diagnostics.size() == 1u);
            auto& diagnostic = decoded.outline->diagnostics.front();
            expect(diagnostic.message == "invalid assignment" && diagnostic.range.place->path == "app.py");
            expect(diagnostic.range.place->begin == 2u && diagnostic.range.place->end == 5u);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"build_model">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
