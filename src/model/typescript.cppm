export module typescript;
import std;
import meta;
import reflection;
import content;
export import provider;

namespace builds {
    // The TypeScript compiler checks a project without emitting or writes its JavaScript and declarations to build_dir. Compiler
    // diagnostics stay in the step output: tsc has no stable structured diagnostic report, so its human-readable output is not parsed.
    template <> struct adapter<"tsc"> {
        struct options {
            // `tsc`, or a compiler inside the project; elsewhere map this name in the tools of each worker and the studio.
            std::string executable = "tsc";
            std::string project_dir = ".";
            // The tsconfig passed to --project, relative to the tree.
            std::string config_file = "tsconfig.json";
            // Required output of build. check always passes --noEmit and never writes here.
            std::string build_dir;
            std::optional<std::string> tsbuildinfo_file;
            // Command-line equivalents of their tsconfig compilerOptions. Absent leaves the choice to tsconfig.
            std::optional<std::string> target;
            std::optional<std::string> module;
            std::optional<std::string> module_resolution;
            std::optional<std::string> jsx;
            std::vector<std::string> lib;
            std::optional<bool> strict;
            std::optional<bool> no_unchecked_indexed_access;
            std::optional<bool> exact_optional_property_types;
            std::optional<bool> verbatim_module_syntax;
            std::optional<bool> source_map;
            std::optional<bool> declaration;
            std::optional<bool> declaration_map;
            std::optional<bool> no_emit_on_error;
        };
        static void validate(const options& o);
        static auto describe(const options& o) -> std::map<std::string, operation_spec>;
        static auto expand(const options& o, std::string_view operation, const parameters& p) -> std::vector<step>;
    };
    static_assert(system<adapter<"tsc">>);
} // namespace builds

namespace language {
    // typescript-language-server owns tsserver and speaks LSP to the source service. Its documented initialization options are sent
    // unchanged where they are open-ended (preferences and implicitProjectConfiguration); the other settings retain typed fields.
    template <> struct server<"typescript"> {
        struct options {
            std::string executable = "typescript-language-server";
            builds::environment env;
            // A mapped executable name, an absolute machine path, or a tsserver inside the project. Absent uses the server's own.
            std::optional<std::string> tsserver_path;
            std::optional<std::string> locale;
            std::optional<std::uint32_t> max_tsserver_memory;
            std::optional<bool> disable_automatic_typing_acquisition;
            std::map<std::string, rfl::Generic> preferences;
            std::map<std::string, rfl::Generic> implicit_project_configuration;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 8> documents{{
            {"ts", "typescript"},
            {"tsx", "typescriptreact"},
            {"mts", "typescript"},
            {"cts", "typescript"},
            {"js", "javascript"},
            {"jsx", "javascriptreact"},
            {"mjs", "javascript"},
            {"cjs", "javascript"},
        }};
        static constexpr std::array<std::string_view, 0> inputs{};

        static void validate(const options& o);
        static auto launch(const options& o, const std::vector<builds::context>& inputs) -> program;
    };
    static_assert(speaker<server<"typescript">>);
} // namespace language

namespace {
    void executable(const std::string& name, std::string_view adapter) {
        if (name.empty() || name.contains('\0') || name.contains("${"))
            throw std::invalid_argument(std::string{adapter} + " executable is required");
        builds::tools({name});
        if (name.find_first_of("/\\") != std::string::npos && !builds::absolute_path(name))
            content::checked_path(name);
    }
    void word(const std::string& value, std::string_view field) {
        if (value.empty() || value.starts_with('-') || value.contains("${") || (value.contains('\0') || value.find_first_of("\n\r") != std::string::npos))
            throw std::invalid_argument(std::string{field} + " is one compiler option value: " + value);
    }
    void object(const std::map<std::string, rfl::Generic>& values, std::string_view field) {
        for (auto& [key, value] : values)
            if (key.empty() || std::holds_alternative<std::nullopt_t>(value.get()))
                throw std::invalid_argument(std::string{field} + " keys are nonempty and their values are not null: " + key);
    }
    auto tree_path(const std::string& path) -> std::string { return builds::absolute_path(path) ? path : "${tree}/" + path; }
    void boolean(std::vector<std::string>& argv, std::string_view flag, const std::optional<bool>& value) {
        if (value) {
            argv.emplace_back(flag);
            argv.push_back(*value ? "true" : "false");
        }
    }
} // namespace

namespace builds {
    void adapter<"tsc">::validate(const options& o) {
        executable(o.executable, "tsc");
        relative(o.project_dir, true);
        relative(o.config_file);
        if (o.build_dir.empty() || o.build_dir == ".")
            throw std::invalid_argument("tsc build_dir is required: the configuration names the output directory");
        relative(o.build_dir);
        if (o.tsbuildinfo_file) {
            relative(*o.tsbuildinfo_file);
            if (std::filesystem::path(*o.tsbuildinfo_file).parent_path().empty())
                throw std::invalid_argument("tsc tsbuildinfo_file must name a file inside a build directory");
            if (*o.tsbuildinfo_file == o.build_dir || o.tsbuildinfo_file->starts_with(o.build_dir + "/"))
                throw std::invalid_argument("tsc tsbuildinfo_file must be outside build_dir, which is emptied before a build");
        }
        for (auto [field, value] :
             {std::pair{"target", &o.target}, std::pair{"module", &o.module}, std::pair{"module_resolution", &o.module_resolution}, std::pair{"jsx", &o.jsx}})
            if (*value)
                word(**value, field);
        for (auto& entry : o.lib)
            word(entry, "lib");
    }

    auto adapter<"tsc">::describe(const options& o) -> std::map<std::string, operation_spec> {
        operation_spec check{.needs = tools({o.executable}), .source_dirs = {o.project_dir}};
        operation_spec build = check;
        build.outputs = {{"output", {"javascript", o.build_dir, "directory"}}};
        build.build_dirs = {o.build_dir};
        if (o.tsbuildinfo_file)
            build.build_dirs.push_back(std::filesystem::path(*o.tsbuildinfo_file).parent_path().generic_string());
        return {{"check", std::move(check)}, {"build", std::move(build)}};
    }

    auto adapter<"tsc">::expand(const options& o, std::string_view operation, const parameters&) -> std::vector<step> {
        if (operation != "check" && operation != "build")
            throw std::out_of_range("no such operation");
        step out{.name = std::string{operation}, .cwd = o.project_dir};
        out.argv = {tool(o.executable), "--project", tree_path(o.config_file), "--pretty", "false"};
        if (operation == "check")
            out.argv.insert(out.argv.end(), {"--noEmit", "--incremental", "false"});
        else {
            out.argv.insert(out.argv.end(), {"--outDir", tree_path(o.build_dir)});
            if (o.tsbuildinfo_file)
                out.argv.insert(out.argv.end(), {"--tsBuildInfoFile", tree_path(*o.tsbuildinfo_file)});
        }
        for (auto [flag, value] : {std::pair{"--target", &o.target}, std::pair{"--module", &o.module}, std::pair{"--moduleResolution", &o.module_resolution},
                                   std::pair{"--jsx", &o.jsx}})
            if (*value)
                out.argv.insert(out.argv.end(), {flag, **value});
        if (!o.lib.empty()) {
            std::string joined;
            for (auto& entry : o.lib)
                joined += (joined.empty() ? "" : ",") + entry;
            out.argv.insert(out.argv.end(), {"--lib", std::move(joined)});
        }
        for (auto [flag, value] :
             {std::pair{"--strict", &o.strict}, std::pair{"--noUncheckedIndexedAccess", &o.no_unchecked_indexed_access},
              std::pair{"--exactOptionalPropertyTypes", &o.exact_optional_property_types}, std::pair{"--verbatimModuleSyntax", &o.verbatim_module_syntax},
              std::pair{"--sourceMap", &o.source_map}, std::pair{"--declaration", &o.declaration}, std::pair{"--declarationMap", &o.declaration_map},
              std::pair{"--noEmitOnError", &o.no_emit_on_error}})
            boolean(out.argv, flag, *value);
        return {std::move(out)};
    }
} // namespace builds

namespace language {
    void server<"typescript">::validate(const options& o) {
        executable(o.executable, "typescript-language-server");
        builds::validate_environment(o.env);
        if (o.tsserver_path) {
            if (o.tsserver_path->empty() || o.tsserver_path->contains("${") ||
                (o.tsserver_path->contains('\0') || o.tsserver_path->find_first_of("\n\r") != std::string::npos))
                throw std::invalid_argument("typescript tsserver_path is one executable or project path");
            if (!builds::absolute_path(*o.tsserver_path) && o.tsserver_path->find_first_of("/\\") != std::string::npos)
                builds::relative(*o.tsserver_path);
            else if (!builds::absolute_path(*o.tsserver_path))
                builds::tools({*o.tsserver_path});
        }
        if (o.locale)
            word(*o.locale, "typescript locale");
        if (o.max_tsserver_memory && (!*o.max_tsserver_memory || *o.max_tsserver_memory > 1048576))
            throw std::invalid_argument("typescript max_tsserver_memory is a positive number of MiB, at most 1048576");
        object(o.preferences, "typescript preferences");
        object(o.implicit_project_configuration, "typescript implicit_project_configuration");
    }

    auto server<"typescript">::launch(const options& o, const std::vector<builds::context>&) -> program {
        program out{.argv = {o.executable, "--stdio"}, .env = o.env};
        if (o.tsserver_path) {
            out.argv.push_back("--tsserver-path");
            out.argv.push_back(builds::absolute_path(*o.tsserver_path) || o.tsserver_path->find_first_of("/\\") != std::string::npos
                                   ? tree_path(*o.tsserver_path)
                                   : builds::tool(*o.tsserver_path));
        }
        rfl::Generic::Object init;
        init["hostInfo"] = "fughetta";
        if (o.locale)
            init["locale"] = *o.locale;
        if (o.max_tsserver_memory)
            init["maxTsServerMemory"] = rfl::Generic{static_cast<std::int64_t>(*o.max_tsserver_memory)};
        if (o.disable_automatic_typing_acquisition)
            init["disableAutomaticTypingAcquisition"] = rfl::Generic{*o.disable_automatic_typing_acquisition};
        if (!o.preferences.empty()) {
            rfl::Generic::Object preferences;
            for (auto& [key, value] : o.preferences)
                preferences[key] = value;
            init["preferences"] = std::move(preferences);
        }
        if (!o.implicit_project_configuration.empty()) {
            rfl::Generic::Object configuration;
            for (auto& [key, value] : o.implicit_project_configuration)
                configuration[key] = value;
            init["implicitProjectConfiguration"] = std::move(configuration);
        }
        out.initialization = std::move(init);
        return out;
    }
} // namespace language
