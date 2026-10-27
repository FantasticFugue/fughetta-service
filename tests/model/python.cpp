import std;
import boost.ut;
import meta;
import language;
import build;
import reflection;

namespace {
    using namespace boost::ut;

    template <meta::fixed_string Name> auto options(builds::configuration& c, const std::string& builder) -> builds::config<Name>& {
        return rfl::get<builds::with<Name>>(c.builders.at(builder).variant()).options;
    }
    void add(builds::configuration& c, const std::string& task, const std::string& builder, const std::string& operation, std::vector<std::string> after = {}) {
        c.tasks[task] = {.builder = builder, .operation = operation, .after = std::move(after)};
        c.operations[task] = {.tasks = {task}};
    }
    auto has(const builds::step& s, std::string_view arg) -> bool { return std::ranges::contains(s.argv, std::string{arg}); }
    auto has_pair(const builds::step& s, std::string_view flag, std::string_view value) -> bool {
        for (std::size_t i = 0; i + 1 < s.argv.size(); ++i)
            if (s.argv[i] == flag && s.argv[i + 1] == value)
                return true;
        return false;
    }
    auto step_of(const builds::plan& plan, std::string_view name) -> const builds::step& {
        auto found = std::ranges::find(plan.steps, name, &builds::step::name);
        if (found == plan.steps.end())
            throw std::out_of_range("no step " + std::string{name});
        return *found;
    }

    // A uv project whose environment ty reads and whose sources ruff checks.
    auto project() -> builds::configuration {
        builds::configuration c;
        c.builders["uv"] = builds::with<"uv">{.options = {.env_dir = "build/venv",
                                                          .build_dir = "build/dist",
                                                          .cache_dir = "build/cache",
                                                          .groups = {"dev"},
                                                          .extras = {"cli"},
                                                          .run_command = {"python", "-m", "svc"}}};
        c.builders["ty"] = builds::with<"ty">{.options = {.python = "build/venv", .paths = {"src"}, .error = {"unresolved-import"}}};
        c.builders["ruff"] = builds::with<"ruff">{.options = {.paths = {"src"}, .select = {"E", "F"}}};
        for (auto operation : {"lock", "sync", "build", "run"})
            add(c, operation, "uv", operation);
        add(c, "types", "ty", "check", {"sync"});
        add(c, "lint", "ruff", "check", {"sync"});
        add(c, "format", "ruff", "format");
        c.operations["verify"] = {.tasks = {"types", "lint", "format"}};
        return c;
    }

    static constexpr auto cases = [] {
        "uv gives each command only the flags it accepts"_test = [] {
            auto c = project();
            auto sync = step_of(builds::prepare(c, {.operation = "sync"}), "sync");
            expect(sync.argv.front() == "${tool:uv}" && sync.argv[1] == "sync" && has(sync, "--locked"));
            expect(has_pair(sync, "--group", "dev") && has_pair(sync, "--extra", "cli"));
            expect(sync.env.at("UV_PROJECT_ENVIRONMENT") == "${tree}/build/venv" && sync.env.at("UV_CACHE_DIR") == "${tree}/build/cache");
            auto lock = step_of(builds::prepare(c, {.operation = "lock"}), "lock");
            expect(lock.argv[1] == "lock" && has(lock, "--locked") && !has(lock, "--group") && !has(lock, "--extra"));
            auto build = step_of(builds::prepare(c, {.operation = "build"}), "build");
            expect(has_pair(build, "--out-dir", "${tree}/build/dist") && !has(build, "--locked") && !has(build, "--group") && !has(build, "--extra"));
            auto run = step_of(builds::prepare(c, {.operation = "run", .parameters = {.args = {"--port", "0"}}}), "run");
            expect(run.argv[1] == "run" && has(run, "--no-sync") && !has(run, "--locked") && !has(run, "--group") && !has(run, "--"));
            expect((run.argv.size() >= 5u) >> fatal);
            expect(std::vector<std::string>(run.argv.end() - 5, run.argv.end()) == std::vector<std::string>{"python", "-m", "svc", "--port", "0"});
            auto local = builds::locate(sync, "/work/project", {{"uv", "/opt/uv/bin/uv"}}, 2);
            expect(local.argv.front() == "/opt/uv/bin/uv" && local.env.at("UV_PROJECT_ENVIRONMENT") == "/work/project/build/venv");
            auto offline = project();
            options<"uv">(offline, "uv").offline = true;
            expect(has(step_of(builds::prepare(offline, {.operation = "sync"}), "sync"), "--no-build"));
            expect(!has(step_of(builds::prepare(offline, {.operation = "build"}), "build"), "--no-build"));
        };

        "uv refuses what the configuration must name and what would overlap"_test = [] {
            auto reject = [](auto mutate) {
                auto c = project();
                auto& uv = options<"uv">(c, "uv");
                mutate(uv);
                expect(throws<std::invalid_argument>([&] { builds::adapter<"uv">::validate(uv); }));
            };
            reject([](auto& uv) { uv.env_dir.clear(); });
            reject([](auto& uv) { uv.build_dir.clear(); });
            reject([](auto& uv) { uv.env_dir = "/home/someone/.venv"; });
            reject([](auto& uv) { uv.build_dir = "build/venv/dist"; });
            reject([](auto& uv) { uv.cache_dir = "build"; });
            reject([](auto& uv) { uv.groups = {"dev,docs"}; });
            reject([](auto& uv) { uv.run_command = {"--no-sync"}; });
            auto c = project();
            options<"uv">(c, "uv").run_command.clear();
            expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "run"}); })) << "a run starts a program";
        };

        "ty and ruff read the environment uv synced and change no source"_test = [] {
            auto plan = builds::prepare(project(), {.operation = "verify"});
            auto& sync = step_of(plan, "sync");
            auto& types = step_of(plan, "types");
            expect(sync.env.at("UV_PROJECT_ENVIRONMENT") == "${tree}/build/venv");
            expect(types.argv.front() == "${tool:ty}" && types.argv[1] == "check" && has_pair(types, "--python", "${tree}/build/venv"));
            expect(has_pair(types, "--error", "unresolved-import") && !has(types, "--warn") && has(types, "--error-on-warning"));
            expect(types.argv.back() == "${tree}/src");
            auto& lint = step_of(plan, "lint");
            expect(lint.argv[1] == "check" && has(lint, "--no-fix") && !has(lint, "--fix") && has_pair(lint, "--select", "E"));
            auto& format = step_of(plan, "format");
            expect(format.argv[1] == "format" && has(format, "--check") && !has(format, "--no-fix"));
            for (auto* s : {&types, &lint, &format})
                expect(s->env.at("CI_PROJECT_DIR") == "${tree}/" + s->cwd) << s->name;
            for (auto tool : {"uv", "ty", "ruff"})
                expect(std::ranges::contains(plan.needs.tools, std::string{tool})) << tool;
            expect(std::ranges::contains(plan.build_dirs, std::string{"build/venv"}) && std::ranges::contains(plan.build_dirs, std::string{".ruff_cache"}));
            auto reject = [](auto mutate) {
                auto c = project();
                mutate(c);
                expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
            };
            reject([](auto& c) { options<"ty">(c, "ty").executable = "/usr/local/bin/ty"; });
            reject([](auto& c) { options<"ty">(c, "ty").python_version = "latest"; });
            reject([](auto& c) { options<"ty">(c, "ty").error = {"unresolved import"}; });
            reject([](auto& c) { options<"ruff">(c, "ruff").select = {"E 501"}; });
            reject([](auto& c) { options<"ruff">(c, "ruff").line_length = 0; });
            auto filtered = project();
            expect(throws<std::invalid_argument>([&] { builds::prepare(filtered, {.operation = "lint", .parameters = {.filter = "E501"}}); }))
                << "ruff has no filter";
        };

        "the ty and ruff servers get their own settings as initializationOptions.settings"_test = [] {
            language::config<"ty"> ty{.python = "build/venv"};
            ty.server.diagnostic_mode = "workspace";
            ty.server.configuration["rules"] = rfl::json::read<rfl::Generic>(R"({"unresolved-import":"warn"})").value();
            auto plan = language::prepare({.server = language::with<"ty">{.options = std::move(ty)}});
            expect(plan.server.argv == std::vector<std::string>{"ty", "server"});
            expect(rfl::json::write(plan.server.initialization) ==
                   R"({"settings":{"configuration":{"rules":{"unresolved-import":"warn"}},"diagnosticMode":"workspace"}})");
            expect(plan.server.env.at("VIRTUAL_ENV") == "${tree}/build/venv") << "ty finds the uv environment by its variable";
            auto local = language::locate(plan.server, "/work/project", {{"ty", "/opt/ty/bin/ty"}}, 1);
            expect(local.argv == std::vector<std::string>{"/opt/ty/bin/ty", "server"} && local.env.at("VIRTUAL_ENV") == "/work/project/build/venv");
            expect(language::providers::document("ty", "src/app.py") == std::optional<std::string_view>{"python"});

            language::config<"ruff"> ruff;
            ruff.server.linter.select = std::vector<std::string>{"ANN", "I"};
            ruff.server.line_length = 100;
            ruff.server.formatter.backend = "uv";
            ruff.server.actions.fix_violation.enable = false;
            ruff.server.exclude = {"build/**"};
            auto launched = language::prepare({.server = language::with<"ruff">{.options = std::move(ruff)}}).server;
            expect(launched.argv == std::vector<std::string>{"ruff", "server"});
            expect(rfl::json::write(launched.initialization) == R"({"settings":{"lineLength":100,"exclude":["build/**"],"lint":{"select":["ANN","I"]},)"
                                                                R"("format":{"backend":"uv"},"codeAction":{"fixViolation":{"enable":false}}}})");

            auto reject = [](auto provider) { expect(throws<std::invalid_argument>([&] { language::prepare({.server = provider}); })); };
            reject(language::with<"ty">{.options = {.executable = "/usr/local/bin/ty"}});
            reject(language::with<"ty">{.options = {.env = {{"VIRTUAL_ENV", "other"}}, .python = "build/venv"}});
            reject(language::with<"ty">{.options = {.server = {.diagnostic_mode = "always"}}});
            reject(language::with<"ruff">{.options = {.server = {.formatter = {.backend = "external"}}}});
            reject(language::with<"ruff">{.options = {.server = {.line_length = 0}}});
        };

        "ty and ruff report in Code Quality JSON and pytest in JUnit"_test = [] {
            auto plan = builds::prepare(project(), {.operation = "verify"});
            auto& types = step_of(plan, "types");
            expect(has_pair(types, "--output-format", "gitlab") && types.standard_output == "ty.json");
            expect(types.reports == std::vector<builds::report>{{"codequality", "ty.json"}});
            for (auto name : {"lint", "format"})
                expect(has_pair(step_of(plan, name), "--output-format", "gitlab") && step_of(plan, name).standard_output == "ruff.json") << name;

            auto checked = builds::report_protocols::read("codequality", R"([
  {"check_name": "invalid-assignment", "description": "invalid-assignment: Object of type `str` is not assignable to `int`", "severity": "major",
   "fingerprint": "3d32e7f2ef0b9f8c", "location": {"path": "src/m.py", "positions": {"begin": {"line": 2, "column": 10}, "end": {"line": 2, "column": 13}}}},
  {"check_name": "unused-ignore-comment", "description": "unused-ignore-comment: Unused blanket `type: ignore`", "severity": "minor",
   "location": {"path": "src/w.py", "positions": {"begin": {"line": 3, "column": 6}}}},
  {"check_name": "unformatted", "description": "unformatted: File would be reformatted", "severity": "info",
   "location": {"path": "src/f.py", "lines": {"begin": 4}}}
])");
            expect((checked.diagnostics.size() == 3u && !checked.tests) >> fatal);
            expect(checked.diagnostics[0].file == "src/m.py" && checked.diagnostics[0].line == 2 && checked.diagnostics[0].column == 10);
            expect(checked.diagnostics[0].severity == "error" && checked.diagnostics[0].message.starts_with("invalid-assignment: Object of type"));
            expect(checked.diagnostics[1].severity == "warning" && checked.diagnostics[2].severity == "info" && checked.diagnostics[2].line == 4);
            expect(builds::report_protocols::read("codequality", "[]").diagnostics.empty());

            auto pytest = builds::report_protocols::read("junit", R"(<?xml version="1.0" encoding="utf-8"?><testsuites name="pytest tests">
<testsuite name="pytest" errors="1" failures="1" skipped="1" tests="5" time="0.31">
<testcase classname="tests.test_a" name="test_sum" time="0.001"><failure message="assert 3 == 4">def test_sum():
&gt;       assert 1 + 2 == 4
E       assert 3 == 4</failure><error message="failed on teardown with &quot;RuntimeError: teardown&quot;">teardown</error></testcase>
<testcase classname="tests.test_a" name="test_ok" time="0.001" />
<testcase classname="tests.test_a" name="test_skip" time="0.001"><skipped type="pytest.skip" message="later">skip</skipped></testcase>
<testcase classname="" name="tests.test_b" time="0.0"><error message="collection failure">ModuleNotFoundError: No module named 'svc'</error></testcase>
</testsuite></testsuites>)");
            expect((pytest.tests.has_value()) >> fatal);
            expect(pytest.tests->total == 4 && pytest.tests->failed == 2);
            expect(pytest.tests->failed_tests == std::vector<std::string>{"tests.test_a.test_sum", "tests.test_b"});
            auto none = builds::report_protocols::read("junit", R"(<testsuites><testsuite name="pytest" tests="0"></testsuite></testsuites>)");
            expect(none.tests && none.tests->total == 0);
        };

        "a task adds the report of a program it starts"_test = [] {
            auto c = project();
            options<"uv">(c, "uv").run_command = {"pytest", "-q", "--junit-xml=${reports}/pytest.xml"};
            c.tasks["run"].reports = {{"junit", "pytest.xml"}};
            auto run = step_of(builds::prepare(c, {.operation = "run"}), "run");
            expect(run.reports == std::vector<builds::report>{{"junit", "pytest.xml"}});
            expect(has(builds::locate(run, "/work/project", {}, 1, "/work/reports"), "--junit-xml=/work/reports/pytest.xml"));
            expect(step_of(builds::prepare(c, {.operation = "sync"}), "sync").reports.empty());
            options<"uv">(c, "uv").run_command = {"pytest", "${tree}/tests"};
            expect(throws<std::invalid_argument>([&] { builds::validate(c); })) << "${reports} is the one reference a program takes";
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"python_tooling">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
