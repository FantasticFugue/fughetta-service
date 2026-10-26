module cmake;

namespace builds {
    void adapter<"cmake">::validate(const options& c) {
        relative(c.source_dir, true);
        relative(c.build_dir);
        if (c.source_dir == c.build_dir || c.source_dir.starts_with(c.build_dir + "/"))
            throw std::invalid_argument("source and build directories must be separate; the build directory cannot contain the source directory");
        if (c.executable.empty() || c.ctest.empty())
            throw std::invalid_argument("CMake and CTest executables are required");
        if (!c.toolchain_file.empty())
            relative(c.toolchain_file);
        if (c.export_compile_commands && c.preset.empty() && c.generator != "Ninja" && c.generator != "Ninja Multi-Config" && c.generator != "Unix Makefiles")
            throw std::invalid_argument("selected CMake generator does not export a compilation database");
        for (auto& [key, value] : c.variables) {
            if (key.empty() || !std::ranges::all_of(key, [](unsigned char v) { return std::isalnum(v) || v == '_'; }))
                throw std::invalid_argument("invalid CMake variable name");
            if (key == "CMAKE_EXPORT_COMPILE_COMMANDS" || key == "CMAKE_C_COMPILER" || key == "CMAKE_CXX_COMPILER" || key == "CMAKE_TOOLCHAIN_FILE" ||
                key == "CMAKE_BUILD_TYPE" || key == "CMAKE_GENERATOR")
                throw std::invalid_argument("use the dedicated field for reserved CMake variables");
        }
        for (auto& arg : c.configure_options)
            if (arg == "-S" || arg == "-B" || arg == "--preset" || arg == "-G" || arg.starts_with("-DCMAKE_EXPORT_COMPILE_COMMANDS"))
                throw std::invalid_argument("use dedicated CMake source, build, preset, generator and export fields");
    }
    auto adapter<"cmake">::describe(const options& c) -> std::map<std::string, operation_spec> {
        operation_spec configure{.inputs = c.inputs, .source_dirs = {c.source_dir}, .build_dirs = {c.build_dir}};
        configure.needs = tools({c.executable, c.c_compiler, c.cxx_compiler});
        if (c.preset.empty() && c.generator.starts_with("Ninja"))
            combine(configure.needs, tools({"ninja"}));
        if (c.preset.empty() && c.generator == "Unix Makefiles")
            combine(configure.needs, tools({"make"}));
        configure.outputs["build_tree"] = {"directory", c.build_dir, "directory"};
        if (c.export_compile_commands)
            configure.outputs["compilation_database"] = {"compilation_database", c.build_dir + "/compile_commands.json"};
        operation_spec build{.parameters = {"targets"}, .outputs = c.outputs, .source_dirs = {c.source_dir}, .build_dirs = {c.build_dir}};
        build.needs = tools({c.executable});
        if (!build.outputs.emplace("build_tree", artifact{"directory", c.build_dir, "directory"}).second)
            throw std::invalid_argument("CMake build_tree output is reserved");
        return {{"configure", std::move(configure)},
                {"build", std::move(build)},
                {"test",
                 {.parameters = {"args", "filter", "labels", "parallel"},
                  .needs = tools({c.ctest}),
                  .source_dirs = {c.source_dir},
                  .build_dirs = {c.build_dir}}}};
    }
    auto adapter<"cmake">::expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step> {
        auto path = [](const std::string& p) { return "${tree}/" + p; };
        builds::step step{.name = std::string{operation}};
        if (operation == "configure") {
            step.cwd = c.source_dir;
            step.argv = {tool(c.executable), "-S", path(c.source_dir), "-B", path(c.build_dir), "--no-warn-unused-cli"};
            if (!c.preset.empty())
                step.argv.insert(step.argv.end(), {"--preset", c.preset});
            else
                step.argv.insert(step.argv.end(), {"-G", c.generator, "-DCMAKE_BUILD_TYPE=" + c.build_type});
            step.argv.push_back(std::string{"-DCMAKE_EXPORT_COMPILE_COMMANDS="} + (c.export_compile_commands ? "ON" : "OFF"));
            if (!c.c_compiler.empty())
                step.argv.push_back("-DCMAKE_C_COMPILER=" + tool(c.c_compiler));
            if (!c.cxx_compiler.empty())
                step.argv.push_back("-DCMAKE_CXX_COMPILER=" + tool(c.cxx_compiler));
            if (!c.toolchain_file.empty())
                step.argv.push_back("-DCMAKE_TOOLCHAIN_FILE=" + path(c.toolchain_file));
            for (auto& [key, value] : c.variables)
                step.argv.push_back("-D" + key + "=" + value);
            step.argv.insert(step.argv.end(), c.configure_options.begin(), c.configure_options.end());
        } else if (operation == "build") {
            step.argv = {tool(c.executable), "--build", path(c.build_dir)};
            step.env["CMAKE_BUILD_PARALLEL_LEVEL"] = "${jobs}";
            if (!c.configuration.empty())
                step.argv.insert(step.argv.end(), {"--config", c.configuration});
            auto& targets = p.targets.empty() ? c.targets : p.targets;
            if (!targets.empty()) {
                step.argv.push_back("--target");
                step.argv.insert(step.argv.end(), targets.begin(), targets.end());
            }
            if (!c.build_options.empty()) {
                step.argv.push_back("--");
                step.argv.insert(step.argv.end(), c.build_options.begin(), c.build_options.end());
            }
        } else if (operation == "test") {
            step.argv = {tool(c.ctest), "--test-dir", path(c.build_dir), "--output-on-failure"};
            step.env["CTEST_PARALLEL_LEVEL"] = "${jobs}";
            if (!c.configuration.empty())
                step.argv.insert(step.argv.end(), {"-C", c.configuration});
            if (p.filter)
                step.argv.insert(step.argv.end(), {"-R", *p.filter});
            if (p.labels)
                step.argv.insert(step.argv.end(), {"-L", *p.labels});
            if (p.parallel)
                step.argv.insert(step.argv.end(), {"-j", std::to_string(*p.parallel)});
            step.argv.insert(step.argv.end(), p.args.begin(), p.args.end());
        } else
            throw std::invalid_argument("unknown CMake operation");
        return {std::move(step)};
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
