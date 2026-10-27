export module fixture;
import std;
import meta;
export import reflection;
export import configuration;

export namespace fixture {
    template <meta::fixed_string Name> auto options(builds::builder& b) -> builds::config<Name>& { return rfl::get<builds::with<Name>>(b.variant()).options; }
    template <meta::fixed_string Name> auto options(const builds::builder& b) -> const builds::config<Name>& {
        return rfl::get<builds::with<Name>>(b.variant()).options;
    }
    inline auto needs(builds::builder& b) -> builds::requirements& {
        return b.visit([](auto& w) -> builds::requirements& { return w.needs.get(); });
    }
    inline auto env(builds::builder& b) -> builds::environment& {
        return b.visit([](auto& w) -> builds::environment& { return w.env; });
    }
    // The clice server of an analysis, started as `executable serve args...`.
    inline auto clice(std::string executable, std::vector<std::string> args = {}) -> language::provider {
        return language::with<"clice">{.options = {.executable = std::move(executable), .args = std::move(args)}};
    }
    inline void command(builds::configuration& build, std::string operation, std::vector<std::string> argv) {
        options<"command">(build.builders.at("main")).operations.at(operation).argv = std::move(argv);
    }
    inline auto commands(std::vector<std::string> build, std::vector<std::string> prepare = {}, std::vector<std::string> test = {}, std::string cwd = "",
                         std::vector<std::string> run = {}) -> builds::configuration {
        builds::config<"command"> c;
        builds::configuration out;
        c.operations["build"] = {.argv = std::move(build), .working_dir = cwd};
        out.tasks["build"] = {.builder = "main", .operation = "build"};
        out.operations["build"] = {.tasks = {"build"}};
        if (!prepare.empty()) {
            c.operations["prepare"] = {.argv = std::move(prepare), .working_dir = cwd};
            out.tasks["prepare"] = {.builder = "main", .operation = "prepare"};
            out.tasks["build"].after = {"prepare"};
            out.operations["prepare"] = {.tasks = {"prepare"}};
        }
        if (!test.empty()) {
            c.operations["test"] = {.argv = std::move(test), .working_dir = cwd};
            out.tasks["test"] = {.builder = "main", .operation = "test", .after = {"build"}};
            out.operations["test"] = {.tasks = {"test"}};
        }
        if (!run.empty()) {
            c.operations["run"] = {.argv = std::move(run), .working_dir = cwd, .append_args = true};
            out.tasks["run"] = {.builder = "main", .operation = "run", .after = {"build"}};
            out.operations["run"] = {.tasks = {"run"}};
        }
        out.builders["main"] = builds::with<"command">{.options = std::move(c)};
        return out;
    }
    inline auto cmake(std::string dir, std::string compiler = "") -> builds::configuration {
        builds::configuration out;
        out.builders["main"] = builds::with<"cmake">{.options = {.build_dir = std::move(dir), .c_compiler = std::move(compiler)}};
        out.tasks = {{"configure", {.builder = "main", .operation = "configure"}},
                     {"build", {.builder = "main", .operation = "build", .after = {"configure"}}},
                     {"test", {.builder = "main", .operation = "test", .after = {"build"}}}};
        out.operations = {{"prepare", {.tasks = {"configure"}}}, {"build", {.tasks = {"build"}}}, {"test", {.tasks = {"test"}}}};
        return out;
    }
    inline void launcher(builds::configuration& build, std::string executable) {
        build.builders["launcher"] = builds::with<"command">{
            .options = {
                .operations = {{"run", {.argv = {"${input:binary_dir}/" + executable}, .inputs = {{"binary_dir", "directory"}}, .append_args = true}}}}};
        build.tasks["run"] = {.builder = "launcher", .operation = "run", .inputs = {{"binary_dir", {"build", "build_tree"}}}};
        build.operations["run"] = {.tasks = {"run"}};
    }
} // namespace fixture
