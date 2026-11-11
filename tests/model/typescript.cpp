import std;
import support;
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
    void add(builds::configuration& c, const std::string& task, const std::string& builder, const std::string& operation) {
        c.tasks[task] = {.builder = builder, .operation = operation};
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

    auto project() -> builds::configuration {
        builds::configuration c;
        c.builders["tsc"] = builds::with<"tsc">{.options = {.build_dir = "build/types",
                                                            .tsbuildinfo_file = "build/info/app.tsbuildinfo",
                                                            .target = "es2022",
                                                            .module = "nodenext",
                                                            .strict = true,
                                                            .no_unchecked_indexed_access = true,
                                                            .declaration = true}};
        c.builders["bun"] = builds::with<"bun">{.options = {.build_dir = "dist",
                                                            .cache_dir = "build/bun-cache",
                                                            .entrypoints = {"src/index.ts"},
                                                            .test_paths = {"tests"},
                                                            .target = "bun",
                                                            .sourcemap = "linked",
                                                            .external = {"react"},
                                                            .minify = true,
                                                            .bail = 2}};
        add(c, "typecheck", "tsc", "check");
        add(c, "compile", "tsc", "build");
        add(c, "bundle", "bun", "build");
        add(c, "test", "bun", "test");
        c.outputs = {{"javascript", {"compile", "output"}}, {"bundle", {"bundle", "bundle"}}};
        return c;
    }

    static constexpr auto cases = [] {
        "tsc checks without emitting and builds into its declared directory"_test = [] {
            auto c = project();
            auto check = step_of(builds::prepare(c, {.operation = "typecheck"}), "typecheck");
            expect(check.argv.front() == "${tool:tsc}" && has_pair(check, "--project", "${tree}/tsconfig.json"));
            expect(has(check, "--noEmit") && has_pair(check, "--incremental", "false") && !has(check, "--outDir") && !has(check, "--tsBuildInfoFile") &&
                   has_pair(check, "--pretty", "false"));
            expect(has_pair(check, "--strict", "true") && has_pair(check, "--noUncheckedIndexedAccess", "true"));
            auto compile = step_of(builds::prepare(c, {.operation = "compile"}), "compile");
            expect(!has(compile, "--noEmit") && has_pair(compile, "--outDir", "${tree}/build/types"));
            expect(has_pair(compile, "--tsBuildInfoFile", "${tree}/build/info/app.tsbuildinfo"));
            expect(has_pair(compile, "--module", "nodenext") && has_pair(compile, "--declaration", "true"));
            auto local = builds::locate(compile, ::test::absolute("/work/project"), {{"tsc", ::test::absolute("/opt/node/bin/tsc")}}, 2);
            expect(local.argv.front() == ::test::absolute("/opt/node/bin/tsc") && has_pair(local, "--outDir", ::test::absolute("/work/project/build/types")));
            expect(builds::prepare(c, {.operation = "compile"}).contexts.front().path == "build/types");
        };

        "Bun automatically installs without offering a command execution operation"_test = [] {
            auto c = project();
            auto plan = builds::prepare(c, {.operation = "bundle"});
            expect(plan.steps.size() == 2u);
            auto& install = step_of(plan, "bundle/install");
            auto& bundle = step_of(plan, "bundle/build");
            expect(install.argv == std::vector<std::string>{"${tool:bun}", "install", "--no-progress", "--frozen-lockfile", "--ignore-scripts", "--cache-dir",
                                                            "${tree}/build/bun-cache"});
            expect(bundle.argv.front() == "${tool:bun}" && bundle.argv[1] == "build" && has_pair(bundle, "--outdir", "${tree}/dist"));
            expect(has_pair(bundle, "--target", "bun") && has(bundle, "--sourcemap=linked"));
            expect(has_pair(bundle, "--external", "react") && has(bundle, "--minify") && bundle.argv.back() == "${tree}/src/index.ts");
            expect(bundle.outputs.front().path == "dist");
            expect(std::ranges::contains(plan.build_dirs, std::string{"node_modules"}) &&
                   std::ranges::contains(plan.build_dirs, std::string{"build/bun-cache"}));
            auto& bun = options<"bun">(c, "bun");
            auto operations = builds::adapter<"bun">::describe(bun);
            expect(!operations.contains("install") && !operations.contains("run"));
            expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "test", .parameters = {.args = {"sh", "-c", "id"}}}); }));
        };

        "Bun tests select only tests and emit native JUnit"_test = [] {
            auto plan = builds::prepare(project(), {.operation = "test", .parameters = {.targets = {"tests/api.test.ts"}, .filter = "creates.*project"}});
            expect(plan.steps.size() == 2u && step_of(plan, "test/install").argv[1] == "install");
            auto& test = step_of(plan, "test/test");
            expect(has(test, "--reporter-outfile=${reports}/bun.xml") && has(test, "--reporter=junit"));
            expect(has_pair(test, "--test-name-pattern", "creates.*project") && has(test, "--bail=2"));
            expect(test.argv[test.argv.size() - 2] == "${tree}/tests" && test.argv.back() == "${tree}/tests/api.test.ts");
            expect(test.reports == std::vector<builds::report>{{"junit", "bun.xml"}});
            auto local = builds::locate(test, ::test::absolute("/work/project"), {}, 1, ::test::absolute("/work/reports"));
            expect(has(local, ("--reporter-outfile=" + ::test::absolute("/work/reports/bun.xml"))));
        };

        "unsafe paths and incomplete outputs are refused"_test = [] {
            auto reject = [](auto mutate) {
                auto c = project();
                mutate(c);
                expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
            };
            reject([](auto& c) { options<"tsc">(c, "tsc").build_dir.clear(); });
            reject([](auto& c) { options<"tsc">(c, "tsc").build_dir = "/tmp/output"; });
            reject([](auto& c) { options<"tsc">(c, "tsc").tsbuildinfo_file = "app.tsbuildinfo"; });
            reject([](auto& c) { options<"tsc">(c, "tsc").tsbuildinfo_file = "build/types/app.tsbuildinfo"; });
            reject([](auto& c) { options<"bun">(c, "bun").build_dir = "node_modules/dist"; });
            reject([](auto& c) { options<"bun">(c, "bun").entrypoints = {"--compile"}; });
            reject([](auto& c) { options<"bun">(c, "bun").test_paths = {"--watch"}; });
            reject([](auto& c) { options<"bun">(c, "bun").target = "shell"; });
            reject([](auto& c) { options<"bun">(c, "bun").bail = 0; });
            auto c = project();
            options<"bun">(c, "bun").entrypoints.clear();
            expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "bundle"}); }));
        };

        "the TypeScript language server gets typed initialization and mapped tools"_test = [] {
            language::config<"typescript"> typescript;
            typescript.tsserver_path = "tsserver";
            typescript.locale = "en";
            typescript.max_tsserver_memory = 4096;
            typescript.disable_automatic_typing_acquisition = true;
            typescript.preferences["includeInlayParameterNameHints"] = rfl::Generic{"all"};
            typescript.implicit_project_configuration["checkJs"] = rfl::Generic{true};
            auto plan = language::prepare({.server = language::with<"typescript">{.options = std::move(typescript)}, .build_configuration = "native"}, {});
            expect(plan.server.argv == std::vector<std::string>{"typescript-language-server", "--stdio", "--tsserver-path", "${tool:tsserver}"});
            auto json = rfl::json::write(plan.server.initialization);
            expect(json.contains(R"("hostInfo":"fughetta")") && json.contains(R"("maxTsServerMemory":4096)"));
            expect(json.contains(R"("includeInlayParameterNameHints":"all")") && json.contains(R"("checkJs":true)"));
            auto local = language::locate(plan.server, ::test::absolute("/work/project"),
                                          {{"typescript-language-server", ::test::absolute("/opt/ts/language-server")}, {"tsserver", ::test::absolute("/opt/ts/tsserver")}}, 1);
            expect(local.argv.front() == ::test::absolute("/opt/ts/language-server") && local.argv.back() == ::test::absolute("/opt/ts/tsserver"));
            expect(language::providers::document("typescript", "src/app.tsx") == std::optional<std::string_view>{"typescriptreact"});
            expect(language::providers::document("typescript", "src/worker.mts") == std::optional<std::string_view>{"typescript"});
            expect(language::providers::document("typescript", "src/app.py") == std::nullopt);

            language::config<"typescript"> invalid;
            invalid.max_tsserver_memory = 0;
            expect(throws<std::invalid_argument>(
                [&] { language::prepare({.server = language::with<"typescript">{.options = invalid}, .build_configuration = "native"}, {}); }));
            invalid.max_tsserver_memory.reset();
            invalid.preferences[""] = rfl::Generic{true};
            expect(throws<std::invalid_argument>(
                [&] { language::prepare({.server = language::with<"typescript">{.options = invalid}, .build_configuration = "native"}, {}); }));
        };

        "TypeScript and Bun are in the published adapter schemas"_test = [] {
            auto& schemas = builds::schemas();
            expect(schemas.contains("tsc") && schemas.contains("bun"));
            auto text = rfl::json::write(schemas.at("bun"));
            expect(!text.contains("run_command") && !text.contains("append_args"));
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"typescript_tooling">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
