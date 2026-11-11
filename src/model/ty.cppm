export module ty;
import std;
import meta;
import reflection;
import content;
export import provider;

namespace builds {
    // ty as the build adapter of a Python project: `ty check`, run in project_dir. python is the environment ty resolves imports from,
    // a directory of the tree such as the one `uv sync` creates, or an absolute path of the machine. The exit code is ty's: an
    // error-level diagnostic fails the step, and a warning does too only when error_on_warning is set.
    template <> struct adapter<"ty"> {
        struct options {
            // `ty`, or a build of ty inside the project; anywhere else, map the name in the tools of each worker and the studio.
            std::string executable = "ty";
            std::string project_dir = ".";
            // --python
            std::optional<std::string> python;
            // --python-version, such as 3.12
            std::optional<std::string> python_version;
            // --config-file, a ty.toml of the tree
            std::optional<std::string> config_file;
            // The paths to check, of the tree; empty, the project.
            std::vector<std::string> paths;
            // --extra-search-path, directories of the tree
            std::vector<std::string> extra_search_paths;
            // --exclude globs
            std::vector<std::string> exclude;
            // --error, --warn and --ignore, one rule name each; `all` names every rule.
            std::vector<std::string> error;
            std::vector<std::string> warn;
            std::vector<std::string> ignore;
            // --error-on-warning
            bool error_on_warning = true;
        };
        static void validate(const options& o);
        static auto describe(const options& o) -> std::map<std::string, operation_spec>;
        static auto expand(const options& o, std::string_view operation, const parameters& p) -> std::vector<step>;
    };
    static_assert(system<adapter<"ty">>);

} // namespace builds

namespace language {
    // ty's language server, `ty server`, started in the prepared tree. Its settings are the editor settings ty documents, sent as
    // initializationOptions.settings and again for every workspace/configuration request:
    //   - configuration: inline ty configuration, the keys of ty.toml (such as rules), which takes precedence over files;
    //   - configurationFile: a ty.toml of the tree, read instead of the discovered ones;
    //   - diagnosticMode: off, openFilesOnly or workspace; showSyntaxErrors; disableLanguageServices;
    //   - inlayHints {variableTypes, callArgumentNames} and completions {autoImport}.
    // python names the environment ty resolves imports from, as the adapter's does. ty finds it in VIRTUAL_ENV when no configuration
    // names one, so the server is started with that variable, bound to the tree like any other path of its environment.
    template <> struct server<"ty"> {
        struct hint_settings {
            std::optional<bool> variable_types;
            std::optional<bool> call_argument_names;
        };
        struct completion_settings {
            std::optional<bool> auto_import;
        };
        struct settings {
            std::map<std::string, rfl::Generic> configuration;
            std::optional<std::string> configuration_file;
            std::optional<std::string> diagnostic_mode;
            std::optional<bool> show_syntax_errors;
            std::optional<bool> disable_language_services;
            hint_settings inlay_hints;
            completion_settings completions;
        };
        struct options {
            std::string executable = "ty";
            builds::environment env;
            std::optional<std::string> python;
            settings server;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> documents{{{"py", "python"}, {"pyi", "python"}, {"pyw", "python"}}};
        static constexpr std::array<std::string_view, 1> inputs{"python_environment"};
        static auto input_paths(const options& o) -> std::map<std::string, std::string> {
            if (o.python && !builds::absolute_path(*o.python))
                return {{"python_environment", *o.python}};
            return {};
        }
        static auto required_inputs(const options& o) -> std::vector<std::string> {
            if (o.python && !builds::absolute_path(*o.python))
                return {"python_environment"};
            return {};
        }

        static void validate(const options& o);
        static auto launch(const options& o, const std::vector<builds::context>& inputs) -> program;
    };
    static_assert(speaker<server<"ty">>);
} // namespace language

namespace {
    void executable(const std::string& name) {
        if (name.empty() || name.contains('\0') || name.contains("${"))
            throw std::invalid_argument("ty executable is required");
        builds::tools({name});
        if (name.find_first_of("/\\") != std::string::npos)
            content::checked_path(name);
    }
    // An environment of the tree, or an absolute path of the machine that holds one; it is read, never written.
    void python_path(const std::string& python) {
        if (python.empty() || python.contains('\0') || python.contains("${") || python.starts_with('-') || python.find_first_of("\n\r") != std::string::npos)
            throw std::invalid_argument("python is one path to an environment, or is omitted: " + python);
        if (!builds::absolute_path(python))
            builds::relative(python);
    }
    auto bound(const std::string& python) -> std::string { return builds::absolute_path(python) ? python : "${tree}/" + python; }
    void version(const std::string& value) {
        if (!value.starts_with("3.") || value.size() < 3 || value.size() > 4 ||
            !std::ranges::all_of(value.substr(2), [](char c) { return c >= '0' && c <= '9'; }))
            throw std::invalid_argument("python_version is a version such as 3.12: " + value);
    }
    void pattern(const std::string& value, std::string_view field) {
        if (value.empty() || value.find_first_of("\n\r") != std::string::npos || value.contains('\0') || value.contains("${"))
            throw std::invalid_argument(std::string{field} + " holds one entry each: " + value);
    }
} // namespace

namespace builds {
    void adapter<"ty">::validate(const options& o) {
        executable(o.executable);
        relative(o.project_dir, true);
        if (o.python)
            python_path(*o.python);
        if (o.python_version)
            version(*o.python_version);
        if (o.config_file)
            relative(*o.config_file);
        for (auto& path : o.paths)
            relative(path);
        for (auto& path : o.extra_search_paths)
            relative(path);
        for (auto& glob : o.exclude)
            pattern(glob, "exclude");
        for (auto* rules : {&o.error, &o.warn, &o.ignore})
            for (auto& rule : *rules)
                if (rule != "all" && (rule.empty() || !std::ranges::all_of(rule, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; })))
                    throw std::invalid_argument("a ty rule is its name, such as unresolved-import, or all: " + rule);
    }

    auto adapter<"ty">::describe(const options& o) -> std::map<std::string, operation_spec> {
        return {{"check", {.needs = tools({o.executable}), .source_dirs = {o.project_dir}}}};
    }

    auto adapter<"ty">::expand(const options& o, std::string_view operation, const parameters& p) -> std::vector<step> {
        if (operation != "check")
            throw std::out_of_range("no such operation");
        // ty prints its Code Quality report on standard output, with paths relative to CI_PROJECT_DIR when it is set; pinning it to
        // the step's directory keeps them relative to where the report is read from.
        step out{.name = "check", .cwd = o.project_dir, .reports = {{"codequality", "ty.json"}}, .standard_output = "ty.json"};
        out.env["CI_PROJECT_DIR"] = "${tree}/" + o.project_dir;
        out.argv = {tool(o.executable), "check", "--no-progress", "--color", "never", "--output-format", "gitlab"};
        auto& argv = out.argv;
        if (o.python)
            argv.insert(argv.end(), {"--python", bound(*o.python)});
        if (o.python_version)
            argv.insert(argv.end(), {"--python-version", *o.python_version});
        if (o.config_file)
            argv.insert(argv.end(), {"--config-file", "${tree}/" + *o.config_file});
        for (auto& path : o.extra_search_paths)
            argv.insert(argv.end(), {"--extra-search-path", "${tree}/" + path});
        for (auto& glob : o.exclude)
            argv.insert(argv.end(), {"--exclude", glob});
        for (auto [flag, rules] : {std::pair{"--error", &o.error}, std::pair{"--warn", &o.warn}, std::pair{"--ignore", &o.ignore}})
            for (auto& rule : *rules)
                argv.insert(argv.end(), {flag, rule});
        argv.push_back(o.error_on_warning ? "--error-on-warning" : "--exit-zero-on-warning");
        for (auto& path : o.paths)
            argv.push_back("${tree}/" + path);

        return {std::move(out)};
    }

} // namespace builds

namespace language {
    void server<"ty">::validate(const options& o) {
        executable(o.executable);
        builds::validate_environment(o.env);
        if (o.python) {
            python_path(*o.python);
            if (o.env.contains("VIRTUAL_ENV"))
                throw std::invalid_argument("ty python and the VIRTUAL_ENV of env both name the environment");
        }
        auto& s = o.server;
        if (s.configuration_file)
            builds::relative(*s.configuration_file);
        if (s.diagnostic_mode && *s.diagnostic_mode != "off" && *s.diagnostic_mode != "openFilesOnly" && *s.diagnostic_mode != "workspace")
            throw std::invalid_argument("ty diagnosticMode is off, openFilesOnly or workspace: " + *s.diagnostic_mode);
        for (auto& [key, value] : s.configuration)
            if (key.empty() || std::holds_alternative<std::nullopt_t>(value.get()))
                throw std::invalid_argument("a ty configuration key is a nonempty name with a value: " + key);
    }

    auto server<"ty">::launch(const options& o, const std::vector<builds::context>& inputs) -> program {
        if (o.python && !builds::absolute_path(*o.python) &&
            !std::ranges::any_of(inputs, [&](const auto& input) { return input.type == "python_environment" && input.path == *o.python; }))
            throw std::invalid_argument("the bound build does not produce the ty Python environment: " + *o.python);
        auto& s = o.server;
        rfl::Generic::Object settings;
        if (!s.configuration.empty()) {
            rfl::Generic::Object configuration;
            for (auto& [key, value] : s.configuration)
                configuration[key] = value;
            settings["configuration"] = std::move(configuration);
        }
        if (s.configuration_file)
            settings["configurationFile"] = *s.configuration_file;
        if (s.diagnostic_mode)
            settings["diagnosticMode"] = *s.diagnostic_mode;
        if (s.show_syntax_errors)
            settings["showSyntaxErrors"] = rfl::Generic{*s.show_syntax_errors};
        if (s.disable_language_services)
            settings["disableLanguageServices"] = rfl::Generic{*s.disable_language_services};
        rfl::Generic::Object hints, completions;
        if (s.inlay_hints.variable_types)
            hints["variableTypes"] = rfl::Generic{*s.inlay_hints.variable_types};
        if (s.inlay_hints.call_argument_names)
            hints["callArgumentNames"] = rfl::Generic{*s.inlay_hints.call_argument_names};
        if (!hints.empty())
            settings["inlayHints"] = std::move(hints);
        if (s.completions.auto_import)
            completions["autoImport"] = rfl::Generic{*s.completions.auto_import};
        if (!completions.empty())
            settings["completions"] = std::move(completions);
        rfl::Generic::Object init;
        init["settings"] = std::move(settings);
        program out{.argv = {o.executable, "server"}, .env = o.env, .initialization = std::move(init)};
        if (o.python)
            out.env["VIRTUAL_ENV"] = bound(*o.python);
        return out;
    }
} // namespace language
