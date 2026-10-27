export module ruff;
import std;
import meta;
import reflection;
import content;
export import provider;

namespace builds {
    // ruff as the build adapter of a Python project: `ruff check` and `ruff format`, each run in its project_dir. Neither writes a source
    // file: check passes --no-fix, so a `fix = true` of the configuration does not rewrite either, and format passes --check. The exit
    // code is ruff's: a violation, or a file format would change, fails the step.
    template <> struct adapter<"ruff"> {
        struct options {
            // `ruff`, or a build of ruff inside the project; anywhere else, map the name in the tools of each worker and the studio.
            std::string executable = "ruff";
            std::string project_dir = ".";
            // --config, a ruff.toml or pyproject.toml of the tree
            std::optional<std::string> config_file;
            // --target-version, such as py312
            std::optional<std::string> target_version;
            // --line-length
            std::optional<std::uint32_t> line_length;
            // --cache-dir, a directory of the tree; absent, ruff writes .ruff_cache in the project.
            std::optional<std::string> cache_dir;
            // The paths checked or formatted, of the tree; empty, the project.
            std::vector<std::string> paths;
            // --exclude globs
            std::vector<std::string> exclude;
            // --preview
            bool preview = false;
            // --select, --ignore and --extend-select of check; rule codes or prefixes.
            std::vector<std::string> select;
            std::vector<std::string> ignore;
            std::vector<std::string> extend_select;
        };
        static void validate(const options& c);
        static auto describe(const options& c) -> std::map<std::string, operation_spec>;
        static auto expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step>;
    };
    static_assert(system<adapter<"ruff">>);

} // namespace builds

namespace language {
    // ruff's language server, `ruff server`, started in the prepared tree. Its settings are the ones ruff documents, sent as
    // initializationOptions.settings and again for every workspace/configuration request:
    //   - configuration: the keys of a ruff.toml as an object, which sets the configuration of the workspace;
    //   - configurationPreference: editorFirst, filesystemFirst or editorOnly;
    //   - lint {select, extendSelect, ignore, preview}, format {preview, backend}, exclude and lineLength;
    //   - fixAll, organizeImports, showSyntaxErrors and codeAction {disableRuleComment, fixViolation} with enable;
    //   - logLevel and logFile.
    template <> struct server<"ruff"> {
        struct lint {
            std::optional<std::vector<std::string>> select;
            std::optional<std::vector<std::string>> extend_select;
            std::optional<std::vector<std::string>> ignore;
            std::optional<bool> preview;
        };
        struct format {
            std::optional<bool> preview;
            // internal, or uv to format through uv
            std::optional<std::string> backend;
        };
        struct toggle {
            std::optional<bool> enable;
        };
        struct code_action {
            toggle disable_rule_comment;
            toggle fix_violation;
        };
        struct settings {
            std::map<std::string, rfl::Generic> configuration;
            std::optional<std::string> configuration_preference;
            std::vector<std::string> exclude;
            lint linter;
            format formatter;
            std::optional<bool> fix_all;
            std::optional<bool> organize_imports;
            code_action actions;
            std::optional<bool> show_syntax_errors;
            std::optional<std::uint32_t> line_length;
            std::optional<std::string> log_level;
            std::optional<std::string> log_file;
        };
        struct options {
            std::string executable = "ruff";
            builds::environment env;
            settings server;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> documents{{{"py", "python"}, {"pyi", "python"}, {"pyw", "python"}}};
        static constexpr std::array<std::string_view, 0> inputs{};

        static void validate(const options& o);
        static auto launch(const options& o, const std::vector<builds::context>& inputs) -> program;
    };
    static_assert(speaker<server<"ruff">>);
} // namespace language

namespace {
    void executable(const std::string& name) {
        if (name.empty() || name.contains('\0') || name.contains("${"))
            throw std::invalid_argument("ruff executable is required");
        builds::tools({name});
        if (name.find_first_of("/\\") != std::string::npos)
            content::checked_path(name);
    }
    void glob(const std::string& value, std::string_view field) {
        if (value.empty() || value.find_first_of("\n\r") != std::string::npos || value.contains('\0') || value.contains("${"))
            throw std::invalid_argument(std::string{field} + " holds one glob per entry: " + value);
    }
    void selectors(const std::vector<std::string>& codes, std::string_view field) {
        for (auto& code : codes)
            if (code.empty() || !std::ranges::all_of(code, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '*' || c == '.'; }))
                throw std::invalid_argument(std::string{field} + " holds rule codes such as ANN or ANN001: " + code);
    }
    void columns(const std::optional<std::uint32_t>& value, std::string_view field) {
        if (value && (!*value || *value > 320))
            throw std::invalid_argument(std::string{field} + " is a positive number of columns, at most 320");
    }
} // namespace

namespace builds {
    namespace {
        void command(const adapter<"ruff">::options& c, std::string_view what) {
            executable(c.executable);
            relative(c.project_dir, true);
            for (auto* path : {&c.config_file, &c.cache_dir})
                if (*path)
                    relative(**path);
            if (c.target_version && !(c.target_version->starts_with("py3") && c.target_version->size() > 3 &&
                                      std::ranges::all_of(c.target_version->substr(3), [](unsigned char ch) { return std::isdigit(ch); })))
                throw std::invalid_argument(std::string{what} + " target_version is a version such as py312: " + *c.target_version);
            columns(c.line_length, std::string{what} + " line_length");
            for (auto& path : c.paths)
                relative(path);
            for (auto& pattern : c.exclude)
                glob(pattern, std::string{what} + " exclude");
        }
        auto flags(const adapter<"ruff">::options& c, std::string_view operation) -> step {
            // Both operations print their Code Quality report on standard output; format has no --output-file. Its paths are relative
            // to CI_PROJECT_DIR when it is set, so it names the step's directory.
            step out{.name = std::string{operation}, .cwd = c.project_dir, .reports = {{"codequality", "ruff.json"}}, .standard_output = "ruff.json"};
            out.env["NO_COLOR"] = "1";
            out.env["CI_PROJECT_DIR"] = "${tree}/" + c.project_dir;
            out.argv = {tool(c.executable), std::string{operation}, "--output-format", "gitlab"};
            auto& argv = out.argv;
            argv.push_back(operation == "check" ? "--no-fix" : "--check");
            if (c.config_file)
                argv.insert(argv.end(), {"--config", "${tree}/" + *c.config_file});
            if (c.target_version)
                argv.insert(argv.end(), {"--target-version", *c.target_version});
            if (c.line_length)
                argv.insert(argv.end(), {"--line-length", std::to_string(*c.line_length)});
            if (c.cache_dir)
                argv.insert(argv.end(), {"--cache-dir", "${tree}/" + *c.cache_dir});
            if (c.preview)
                argv.push_back("--preview");
            for (auto& pattern : c.exclude)
                argv.insert(argv.end(), {"--exclude", pattern});
            return out;
        }
    } // namespace

    void adapter<"ruff">::validate(const options& c) {
        command(c, "ruff");
        selectors(c.select, "select");
        selectors(c.ignore, "ignore");
        selectors(c.extend_select, "extend_select");
    }

    auto adapter<"ruff">::describe(const options& c) -> std::map<std::string, operation_spec> {
        auto spec = [](const options& run) {
            return operation_spec{.needs = tools({run.executable}),
                                  .source_dirs = {run.project_dir},
                                  .build_dirs = {run.cache_dir.value_or(run.project_dir == "." ? ".ruff_cache" : run.project_dir + "/.ruff_cache")}};
        };
        return {{"check", spec(c)}, {"format", spec(c)}};
    }

    auto adapter<"ruff">::expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step> {
        if (operation != "check" && operation != "format")
            throw std::out_of_range("no such operation");
        auto& run = c;
        auto out = flags(run, operation);
        if (operation == "check") {
            for (auto [flag, codes] : {std::pair{"--select", &c.select}, std::pair{"--extend-select", &c.extend_select}, std::pair{"--ignore", &c.ignore}})
                for (auto& code : *codes)
                    out.argv.insert(out.argv.end(), {flag, code});
        }
        for (auto& path : run.paths)
            out.argv.push_back("${tree}/" + path);

        return {std::move(out)};
    }

} // namespace builds

namespace language {
    void server<"ruff">::validate(const options& o) {
        executable(o.executable);
        builds::validate_environment(o.env);
        auto& s = o.server;
        for (auto& [key, value] : s.configuration)
            if (key.empty() || std::holds_alternative<std::nullopt_t>(value.get()))
                throw std::invalid_argument("a ruff configuration key is a nonempty name with a value: " + key);
        if (s.configuration_preference && *s.configuration_preference != "editorFirst" && *s.configuration_preference != "filesystemFirst" &&
            *s.configuration_preference != "editorOnly")
            throw std::invalid_argument("ruff configurationPreference is editorFirst, filesystemFirst or editorOnly: " + *s.configuration_preference);
        for (auto& pattern : s.exclude)
            glob(pattern, "ruff exclude");
        for (auto* codes : {&s.linter.select, &s.linter.extend_select, &s.linter.ignore})
            if (*codes)
                selectors(**codes, "ruff lint");
        if (s.formatter.backend && *s.formatter.backend != "internal" && *s.formatter.backend != "uv")
            throw std::invalid_argument("ruff format backend is internal or uv: " + *s.formatter.backend);
        columns(s.line_length, "ruff lineLength");
        if (s.log_level && *s.log_level != "error" && *s.log_level != "warn" && *s.log_level != "info" && *s.log_level != "debug" && *s.log_level != "trace")
            throw std::invalid_argument("ruff logLevel is error, warn, info, debug or trace: " + *s.log_level);
        if (s.log_file && (s.log_file->empty() || s.log_file->contains('\0')))
            throw std::invalid_argument("ruff logFile names a file");
    }

    auto server<"ruff">::launch(const options& o, const std::vector<builds::context>&) -> program {
        auto& s = o.server;
        auto strings = [](const std::vector<std::string>& values) {
            rfl::Generic::Array list;
            for (auto& value : values)
                list.emplace_back(value);
            return list;
        };
        rfl::Generic::Object settings, lint, format, actions;
        if (!s.configuration.empty()) {
            rfl::Generic::Object configuration;
            for (auto& [key, value] : s.configuration)
                configuration[key] = value;
            settings["configuration"] = std::move(configuration);
        }
        if (s.configuration_preference)
            settings["configurationPreference"] = *s.configuration_preference;
        if (s.line_length)
            settings["lineLength"] = rfl::Generic{static_cast<std::int64_t>(*s.line_length)};
        if (s.fix_all)
            settings["fixAll"] = rfl::Generic{*s.fix_all};
        if (s.organize_imports)
            settings["organizeImports"] = rfl::Generic{*s.organize_imports};
        if (s.show_syntax_errors)
            settings["showSyntaxErrors"] = rfl::Generic{*s.show_syntax_errors};
        if (!s.exclude.empty())
            settings["exclude"] = strings(s.exclude);
        if (s.linter.select)
            lint["select"] = strings(*s.linter.select);
        if (s.linter.extend_select)
            lint["extendSelect"] = strings(*s.linter.extend_select);
        if (s.linter.ignore)
            lint["ignore"] = strings(*s.linter.ignore);
        if (s.linter.preview)
            lint["preview"] = rfl::Generic{*s.linter.preview};
        if (!lint.empty())
            settings["lint"] = std::move(lint);
        if (s.formatter.preview)
            format["preview"] = rfl::Generic{*s.formatter.preview};
        if (s.formatter.backend)
            format["backend"] = *s.formatter.backend;
        if (!format.empty())
            settings["format"] = std::move(format);
        for (auto [name, toggle] : {std::pair{"disableRuleComment", &s.actions.disable_rule_comment}, std::pair{"fixViolation", &s.actions.fix_violation}})
            if (toggle->enable) {
                rfl::Generic::Object entry;
                entry["enable"] = rfl::Generic{*toggle->enable};
                actions[name] = std::move(entry);
            }
        if (!actions.empty())
            settings["codeAction"] = std::move(actions);
        if (s.log_level)
            settings["logLevel"] = *s.log_level;
        if (s.log_file)
            settings["logFile"] = *s.log_file;
        rfl::Generic::Object init;
        init["settings"] = std::move(settings);
        return {.argv = {o.executable, "server"}, .env = o.env, .initialization = std::move(init)};
    }
} // namespace language
