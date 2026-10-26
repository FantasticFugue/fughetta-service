module cmake;

namespace builds {
    namespace {
        auto flag(std::string_view arg, std::string_view name, bool joined = false) -> bool {
            return arg == name || (arg.starts_with(name) && arg.size() > name.size() && (joined || arg[name.size()] == '='));
        }
        auto owned_variable(std::string_view name) -> bool {
            return name == "CMAKE_EXPORT_COMPILE_COMMANDS" || name == "CMAKE_C_COMPILER" || name == "CMAKE_CXX_COMPILER" ||
                   name == "CMAKE_TOOLCHAIN_FILE" || name == "CMAKE_BUILD_TYPE" || name == "CMAKE_GENERATOR";
        }
        void test_arguments(const std::vector<std::string>& args) {
            for (auto& arg : args) {
                for (auto name : {"-R", "-E", "-L", "-j", "-C"})
                    if (flag(arg, name, true))
                        throw std::invalid_argument("use the dedicated CTest field for " + arg);
                for (auto name : {"--tests-regex", "--exclude-regex", "--label-regex", "--label-exclude", "--parallel", "--timeout",
                                  "--output-junit", "--preset", "--no-tests", "--output-on-failure", "--stop-on-failure", "--test-dir",
                                  "--build-config", "--show-only"})
                    if (flag(arg, name))
                        throw std::invalid_argument("use the dedicated CTest field for " + arg);
                if (arg == "-N")
                    throw std::invalid_argument("the test operation must execute tests");
            }
        }
        auto path(const std::string& value) -> std::string {
            return std::filesystem::path(value).is_absolute() ? value : "${tree}/" + value;
        }
    }

    void adapter<"cmake">::validate(const options& c) {
        relative(c.source_dir, true);
        relative(c.build_dir);
        if (c.source_dir == c.build_dir || c.source_dir.starts_with(c.build_dir + "/"))
            throw std::invalid_argument("the build directory cannot contain the source directory");
        if (c.executable.empty() || c.test.executable.empty())
            throw std::invalid_argument("CMake and CTest executables are required");
        if (!c.toolchain_file.empty() && !std::filesystem::path(c.toolchain_file).is_absolute())
            relative(c.toolchain_file);
        if (c.export_compile_commands && c.preset.empty() && c.generator != "Ninja" && c.generator != "Ninja Multi-Config" && c.generator != "Unix Makefiles")
            throw std::invalid_argument("selected CMake generator does not export a compilation database");
        for (auto& [key, value] : c.variables) {
            if (key.empty() || !std::ranges::all_of(key, [](unsigned char v) { return std::isalnum(v) || v == '_'; }))
                throw std::invalid_argument("invalid CMake variable name");
            if (owned_variable(key))
                throw std::invalid_argument("use the dedicated field for reserved CMake variables");
        }
        for (std::size_t i = 0; i < c.configure_options.size(); ++i) {
            auto& arg = c.configure_options[i];
            if (flag(arg, "-S", true) || flag(arg, "-B", true) || flag(arg, "-G", true) || flag(arg, "--preset") ||
                flag(arg, "--toolchain") || flag(arg, "--build") || flag(arg, "-U", true))
                throw std::invalid_argument("use the dedicated CMake field for " + arg);
            if (arg.starts_with("-D")) {
                std::string_view variable = arg;
                variable.remove_prefix(2);
                if (variable.empty() && i + 1 < c.configure_options.size())
                    variable = c.configure_options[++i];
                variable = variable.substr(0, variable.find_first_of(":="));
                if (owned_variable(variable))
                    throw std::invalid_argument("use the dedicated CMake field for " + std::string{variable});
            }
        }
        // Build options are CMake options; arguments after -- belong to the native build tool.
        for (auto& arg : c.build_options) {
            if (arg == "--") break;
            if (flag(arg, "--build") || flag(arg, "--config") || flag(arg, "--target") || flag(arg, "--preset") ||
                flag(arg, "-t", true) || flag(arg, "-j", true) || flag(arg, "--parallel"))
                throw std::invalid_argument("use the dedicated CMake field for " + arg);
        }
        auto& t = c.test;
        if ((t.parallel && !*t.parallel) || (t.timeout && !*t.timeout))
            throw std::invalid_argument("CTest parallelism and timeout must be positive");
        if (t.no_tests_action != "error" && t.no_tests_action != "ignore")
            throw std::invalid_argument("CTest no_tests_action must be error or ignore");
        if (t.output_junit)
            relative(*t.output_junit);
        test_arguments(t.args);
    }
    auto adapter<"cmake">::describe(const options& c) -> std::map<std::string, operation_spec> {
        operation_spec configure{.inputs = c.inputs, .source_dirs = {c.source_dir}, .build_dirs = {c.build_dir}};
        configure.needs = tools({c.executable, c.c_compiler, c.cxx_compiler});
        if (c.preset.empty() && c.generator.starts_with("Ninja")) combine(configure.needs, tools({"ninja"}));
        if (c.preset.empty() && c.generator == "Unix Makefiles") combine(configure.needs, tools({"make"}));
        configure.outputs["build_tree"] = {"directory", c.build_dir, "directory"};
        if (c.export_compile_commands)
            configure.outputs["compilation_database"] = {"compilation_database", c.build_dir + "/compile_commands.json"};
        operation_spec build{.parameters = {"targets"}, .outputs = c.outputs, .needs = tools({c.executable}),
                             .source_dirs = {c.source_dir}, .build_dirs = {c.build_dir}};
        if (!build.outputs.emplace("build_tree", artifact{"directory", c.build_dir, "directory"}).second)
            throw std::invalid_argument("CMake build_tree output is reserved");
        operation_spec test{.parameters = {"args", "filter", "labels", "parallel"}, .needs = tools({c.test.executable}),
                            .source_dirs = {c.source_dir}, .build_dirs = {c.build_dir}};
        if (c.test.output_junit) test.outputs["test_report"] = {"report", *c.test.output_junit};
        return {{"configure", std::move(configure)}, {"build", std::move(build)}, {"test", std::move(test)}};
    }
    auto adapter<"cmake">::expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step> {
        step out{.name = std::string{operation}};
        if (operation == "configure") {
            out.cwd = c.source_dir;
            out.argv = {tool(c.executable), "-S", path(c.source_dir), "-B", path(c.build_dir), "--no-warn-unused-cli"};
            if (!c.preset.empty()) out.argv.insert(out.argv.end(), {"--preset", c.preset});
            else out.argv.insert(out.argv.end(), {"-G", c.generator, "-DCMAKE_BUILD_TYPE=" + c.build_type});
            out.argv.push_back(std::string{"-DCMAKE_EXPORT_COMPILE_COMMANDS="} + (c.export_compile_commands ? "ON" : "OFF"));
            if (!c.c_compiler.empty()) out.argv.push_back("-DCMAKE_C_COMPILER=" + tool(c.c_compiler));
            if (!c.cxx_compiler.empty()) out.argv.push_back("-DCMAKE_CXX_COMPILER=" + tool(c.cxx_compiler));
            if (!c.toolchain_file.empty()) out.argv.push_back("-DCMAKE_TOOLCHAIN_FILE=" + path(c.toolchain_file));
            for (auto& [key, value] : c.variables) out.argv.push_back("-D" + key + "=" + value);
            out.argv.insert(out.argv.end(), c.configure_options.begin(), c.configure_options.end());
        } else if (operation == "build") {
            out.argv = {tool(c.executable), "--build", path(c.build_dir)};
            out.env["CMAKE_BUILD_PARALLEL_LEVEL"] = "${jobs}";
            if (!c.configuration.empty()) out.argv.insert(out.argv.end(), {"--config", c.configuration});
            auto& targets = p.targets.empty() ? c.targets : p.targets;
            if (!targets.empty()) {
                out.argv.push_back("--target");
                for (auto& target : targets) {
                    if (target.empty() || target.front() == '-') throw std::invalid_argument("invalid CMake target");
                    out.argv.push_back(target);
                }
            }
            out.argv.insert(out.argv.end(), c.build_options.begin(), c.build_options.end());
        } else if (operation == "test") {
            auto& t = c.test;
            test_arguments(p.args);
            if (p.parallel && !*p.parallel) throw std::invalid_argument("CTest parallelism must be positive");
            out.cwd = c.source_dir;
            out.argv = {tool(t.executable), "--test-dir", path(c.build_dir)};
            out.env["CTEST_PARALLEL_LEVEL"] = "${jobs}";
            auto add = [&](std::string flag, const std::string& value) { if (!value.empty()) out.argv.insert(out.argv.end(), {std::move(flag), value}); };
            add("-R", p.filter.value_or(t.include)); add("-E", t.exclude);
            add("-L", p.labels.value_or(t.labels)); add("-LE", t.exclude_labels);
            if (auto parallel = p.parallel ? p.parallel : t.parallel) add("-j", std::to_string(*parallel));
            if (t.timeout) add("--timeout", std::to_string(*t.timeout));
            add("--preset", t.preset);
            add("-C", c.configuration);
            if (t.output_on_failure) out.argv.push_back("--output-on-failure");
            if (t.stop_on_failure) out.argv.push_back("--stop-on-failure");
            out.argv.push_back("--no-tests=" + t.no_tests_action);
            if (t.output_junit) add("--output-junit", path(*t.output_junit));
            out.argv.insert(out.argv.end(), t.args.begin(), t.args.end());
            out.argv.insert(out.argv.end(), p.args.begin(), p.args.end());
        } else throw std::invalid_argument("unknown CMake operation");
        return {std::move(out)};
    }
    auto format<"compilation_database">::check(std::string_view text) -> std::uint64_t {
        auto data = rfl::json::read<rfl::Generic>(std::string{text});
        auto array = data ? data->to_array() : rfl::Generic{}.to_array();
        if (!array || array->empty())
            throw std::runtime_error("expected a nonempty compilation database");
        for (auto& unit : *array) {
            auto object = unit.to_object();
            if (!object)
                throw std::runtime_error("invalid compilation database entry");
            auto file = object->get("file"), directory = object->get("directory");
            if (!file || !directory || file->to_string().value_or("").empty() || directory->to_string().value_or("").empty())
                throw std::runtime_error("compilation database entry has no file or directory");
            auto command = object->get("command"), arguments = object->get("arguments");
            bool valid = command && !command->to_string().value_or("").empty();
            if (arguments) {
                auto argv = arguments->to_array();
                valid = argv && !argv->empty();
                if (valid)
                    for (auto& arg : *argv)
                        if (!arg.to_string())
                            valid = false;
            }
            if (!valid)
                throw std::runtime_error("compilation database entry has no command");
        }
        return array->size();
    }
} // namespace builds
