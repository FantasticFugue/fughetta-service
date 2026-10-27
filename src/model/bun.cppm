export module bun;
import std;
import content;
export import provider;

namespace builds {
    // Bun builds or tests a project. Both operations install the locked manifest first; there is no install or run
    // operation and no arbitrary command/argument field. Callers can select test files and a test-name pattern, nothing executable.
    template <> struct adapter<"bun"> {
        struct options {
            std::string executable = "bun";
            std::string project_dir = ".";
            // `bun build` writes bundles here. Dependencies go to the node_modules of project_dir, where Bun itself puts them.
            std::string build_dir;
            std::optional<std::string> cache_dir;
            std::vector<std::string> entrypoints;
            // Test files or directories checked by every test operation, relative to the tree.
            std::vector<std::string> test_paths;
            bool offline = false;
            bool production = false;
            // Lifecycle scripts are disabled unless the project configuration trusts them.
            bool ignore_scripts = true;
            // Bundler settings; absent target, format, and sourcemap leave Bun's defaults in effect.
            std::optional<std::string> target;
            std::optional<std::string> format;
            std::optional<std::string> sourcemap;
            std::vector<std::string> external;
            bool minify = false;
            bool splitting = false;
            // Test settings. A zero-bail run is not allowed; absent leaves Bun's failure behavior unchanged.
            std::optional<std::uint32_t> bail;
        };
        static void validate(const options& o);
        static auto describe(const options& o) -> std::map<std::string, operation_spec>;
        static auto expand(const options& o, std::string_view operation, const parameters& p) -> std::vector<step>;
    };
    static_assert(system<adapter<"bun">>);
} // namespace builds

namespace {
    void directory(const std::string& value, std::string_view field) {
        if (value.empty() || value == ".")
            throw std::invalid_argument(std::string{field} + " is required: the configuration names the directory");
        builds::relative(value);
    }
    void argument(const std::string& value, std::string_view field) {
        if (value.empty() || value.contains('\0') || value.contains('\n') || value.contains('\r') || value.contains("${"))
            throw std::invalid_argument(std::string{field} + " holds one value without line breaks or step references");
    }
    void choice(const std::optional<std::string>& value, std::initializer_list<std::string_view> choices, std::string_view field) {
        if (value && !std::ranges::contains(choices, std::string_view{*value}))
            throw std::invalid_argument(std::string{"invalid bun "} + std::string{field} + ": " + *value);
    }
    auto tree_path(const std::string& path) -> std::string { return "${tree}/" + path; }
    auto dependencies(const builds::adapter<"bun">::options& o) -> std::string {
        return o.project_dir == "." ? "node_modules" : o.project_dir + "/node_modules";
    }
    auto install(const builds::adapter<"bun">::options& o) -> builds::step {
        builds::step out{.name = "install", .cwd = o.project_dir};
        out.argv = {builds::tool(o.executable), "install", "--no-progress", "--frozen-lockfile"};
        if (o.offline)
            out.argv.push_back("--offline");
        if (o.production)
            out.argv.push_back("--production");
        if (o.ignore_scripts)
            out.argv.push_back("--ignore-scripts");
        if (o.cache_dir)
            out.argv.insert(out.argv.end(), {"--cache-dir", tree_path(*o.cache_dir)});
        return out;
    }
} // namespace

namespace builds {
    void adapter<"bun">::validate(const options& o) {
        if (o.executable.empty() || o.executable.contains('\0') || o.executable.contains("${"))
            throw std::invalid_argument("bun executable is required");
        tools({o.executable});
        if (o.executable.find_first_of("/\\") != std::string::npos && !std::filesystem::path(o.executable).is_absolute())
            content::checked_path(o.executable);
        relative(o.project_dir, true);
        auto dependency_dir = dependencies(o);
        std::vector<std::pair<const std::string*, std::string_view>> dirs{{&dependency_dir, "node_modules"}, {&o.build_dir, "build_dir"}};
        if (o.cache_dir)
            dirs.emplace_back(&*o.cache_dir, "cache_dir");
        for (auto [dir, field] : dirs)
            directory(*dir, field);
        for (std::size_t i = 0; i < dirs.size(); ++i)
            for (std::size_t j = i + 1; j < dirs.size(); ++j) {
                auto &a = *dirs[i].first, &b = *dirs[j].first;
                if (a == b || a.starts_with(b + "/") || b.starts_with(a + "/"))
                    throw std::invalid_argument(std::string{dirs[i].second} + " and " + std::string{dirs[j].second} + " must be separate directories");
            }
        for (auto& entry : o.entrypoints) {
            relative(entry);
            if (entry.starts_with('-'))
                throw std::invalid_argument("bun entrypoints are paths, not options");
        }
        for (auto& path : o.test_paths) {
            relative(path);
            if (path.starts_with('-'))
                throw std::invalid_argument("bun test_paths are paths, not options");
        }
        choice(o.target, {"bun", "browser", "node"}, "target");
        choice(o.format, {"esm", "cjs", "iife"}, "format");
        choice(o.sourcemap, {"none", "linked", "inline", "external"}, "sourcemap");
        for (auto& name : o.external)
            argument(name, "bun external");
        if (o.bail && !*o.bail)
            throw std::invalid_argument("bun bail must be positive");
    }

    auto adapter<"bun">::describe(const options& o) -> std::map<std::string, operation_spec> {
        std::vector<std::string> installed{dependencies(o)};
        if (o.cache_dir)
            installed.push_back(*o.cache_dir);
        auto written = installed;
        written.push_back(o.build_dir);
        operation_spec build{.outputs = {{"bundle", {"javascript", o.build_dir, "directory"}}},
                             .needs = tools({o.executable}),
                             .source_dirs = {o.project_dir},
                             .build_dirs = std::move(written)};
        operation_spec test{
            .parameters = {"targets", "filter"}, .needs = tools({o.executable}), .source_dirs = {o.project_dir}, .build_dirs = std::move(installed)};
        return {{"build", std::move(build)}, {"test", std::move(test)}};
    }

    auto adapter<"bun">::expand(const options& o, std::string_view operation, const parameters& p) -> std::vector<step> {
        if (operation != "build" && operation != "test")
            throw std::out_of_range("no such operation");
        if (operation == "build" && o.entrypoints.empty())
            throw std::invalid_argument("bun build has no entrypoints");
        auto installed = install(o);
        step out{.name = std::string{operation}, .cwd = o.project_dir};
        out.argv = {tool(o.executable), std::string{operation}};
        auto& argv = out.argv;
        if (operation == "build") {
            argv.insert(argv.end(), {"--outdir", tree_path(o.build_dir)});
            if (o.target)
                argv.insert(argv.end(), {"--target", *o.target});
            if (o.format)
                argv.insert(argv.end(), {"--format", *o.format});
            if (o.sourcemap)
                argv.push_back("--sourcemap=" + *o.sourcemap);
            if (o.production)
                argv.push_back("--production");
            if (o.minify)
                argv.push_back("--minify");
            if (o.splitting)
                argv.push_back("--splitting");
            for (auto& name : o.external)
                argv.insert(argv.end(), {"--external", name});
            for (auto& entry : o.entrypoints)
                argv.push_back(tree_path(entry));
        } else {
            argv.insert(argv.end(), {"--reporter=junit", "--reporter-outfile=${reports}/bun.xml"});
            out.reports = {{"junit", "bun.xml"}};
            if (p.filter) {
                if (p.filter->empty() || p.filter->contains('\0') || p.filter->contains('\n') || p.filter->contains('\r'))
                    throw std::invalid_argument("bun test filter is a nonempty test-name pattern");
                argv.insert(argv.end(), {"--test-name-pattern", *p.filter});
            }
            if (o.bail)
                argv.push_back("--bail=" + std::to_string(*o.bail));
            for (auto& path : o.test_paths)
                argv.push_back(tree_path(path));
            for (auto& path : p.targets) {
                relative(path);
                argv.push_back(tree_path(path));
            }
        }
        return {std::move(installed), std::move(out)};
    }
} // namespace builds
