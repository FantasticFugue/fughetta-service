export module uv;
import std;
export import provider;

namespace builds {
    // uv as the build adapter of a Python project: `uv lock`, `uv sync`, `uv build` and `uv run`. env_dir, build_dir and cache_dir are
    // directories of the tree, as every build directory is; the environment reaches every command as UV_PROJECT_ENVIRONMENT.
    template <> struct adapter<"uv"> {
        struct options {
            std::string executable = "uv";
            std::string project_dir = ".";
            // The environment `uv sync` creates and `uv run` uses; there is no default.
            std::string env_dir;
            // Where `uv build` writes the distributions; there is no default.
            std::string build_dir;
            // uv's cache; absent, uv decides.
            std::optional<std::string> cache_dir;
            // The interpreter lock, sync and build resolve for; absent, uv discovers one.
            std::optional<std::string> python;
            // Dependency groups and extras of the sync.
            std::vector<std::string> groups;
            std::vector<std::string> extras;
            // The program `uv run` starts, followed by the operation's arguments.
            std::vector<std::string> run_command;
            // lock and sync fail if uv.lock needs an update.
            bool locked = true;
            // Only the cache is read, and no source distribution is built.
            bool offline = false;
            // The sync leaves out the groups the project installs by default.
            bool no_default_groups = false;
        };
        static void validate(const options&);
        static auto describe(const options& c) -> std::map<std::string, operation_spec>;
        static auto expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step>;
    };
    static_assert(system<adapter<"uv">>);
} // namespace builds

namespace builds {
    namespace {
        void directory(const std::string& value, std::string_view field) {
            if (value.empty() || value == ".")
                throw std::invalid_argument(std::string{field} + " is required: the configuration names the directory");
            relative(value);
        }
        void name(const std::string& value, std::string_view what) {
            if (value.empty() || !std::ranges::all_of(value, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; }))
                throw std::invalid_argument(std::string{"invalid uv "} + std::string{what} + ": " + value);
        }
    } // namespace

    void adapter<"uv">::validate(const options& c) {
        if (c.executable.empty() || c.executable.contains('\0'))
            throw std::invalid_argument("uv executable is required");
        relative(c.project_dir, true);
        std::vector<std::pair<const std::string*, std::string_view>> dirs;
        dirs.emplace_back(&c.env_dir, "env_dir");
        dirs.emplace_back(&c.build_dir, "build_dir");
        if (c.cache_dir)
            dirs.emplace_back(&*c.cache_dir, "cache_dir");
        for (auto [dir, field] : dirs)
            directory(*dir, field);
        for (std::size_t i = 0; i < dirs.size(); ++i)
            for (std::size_t j = i + 1; j < dirs.size(); ++j) {
                auto &a = *dirs[i].first, &b = *dirs[j].first;
                if (a == b || a.starts_with(b + "/") || b.starts_with(a + "/"))
                    throw std::invalid_argument(std::string{dirs[i].second} + " and " + std::string{dirs[j].second} + " must be separate directories");
            }
        if (c.python && (c.python->empty() || c.python->contains('\0')))
            throw std::invalid_argument("python names an interpreter or is omitted");
        for (auto& group : c.groups)
            name(group, "group");
        for (auto& extra : c.extras)
            name(extra, "extra");
        for (auto& arg : c.run_command)
            if (arg.empty() || arg.contains('\0') || arg.contains('\n') || arg.contains("${"))
                throw std::invalid_argument("run_command holds arguments without line breaks or step references");
        if (!c.run_command.empty() && c.run_command.front().starts_with('-'))
            throw std::invalid_argument("run_command names the program uv run starts: " + c.run_command.front());
    }

    auto adapter<"uv">::describe(const options& c) -> std::map<std::string, operation_spec> {
        std::vector<std::string> cache;
        if (c.cache_dir)
            cache.push_back(*c.cache_dir);
        operation_spec lock{.needs = tools({c.executable}), .source_dirs = {c.project_dir}, .build_dirs = cache};
        operation_spec sync = lock, build = lock, run = lock;
        sync.outputs = {{"environment", {"directory", c.env_dir, "directory"}}};
        sync.build_dirs.push_back(c.env_dir);
        build.outputs = {{"distributions", {"python_distributions", c.build_dir, "directory"}}};
        build.build_dirs.push_back(c.build_dir);
        run.parameters = {"args"};
        return {{"lock", std::move(lock)}, {"sync", std::move(sync)}, {"build", std::move(build)}, {"run", std::move(run)}};
    }

    auto adapter<"uv">::expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step> {
        if (operation != "lock" && operation != "sync" && operation != "build" && operation != "run")
            throw std::out_of_range("no such operation");
        if (operation == "run" && c.run_command.empty())
            throw std::invalid_argument("uv run has no run_command: name the program it starts");
        step out{.name = std::string{operation}, .cwd = c.project_dir};
        out.env["UV_PROJECT_ENVIRONMENT"] = "${tree}/" + c.env_dir;
        if (c.cache_dir)
            out.env["UV_CACHE_DIR"] = "${tree}/" + *c.cache_dir;
        out.argv = {tool(c.executable), std::string{operation}, "--color", "never"};
        auto& argv = out.argv;
        // Only lock and sync resolve: --locked and --group do not exist for build, and a run without a sync has nothing to lock.
        bool resolves = operation == "lock" || operation == "sync";
        if (c.offline) {
            argv.push_back("--offline");
            if (resolves)
                argv.push_back("--no-build");
        }
        if (c.python && operation != "run")
            argv.insert(argv.end(), {"--python", *c.python});
        if (c.locked && resolves)
            argv.push_back("--locked");
        if (operation == "sync") {
            if (c.no_default_groups)
                argv.push_back("--no-default-groups");
            for (auto& group : c.groups)
                argv.insert(argv.end(), {"--group", group});
            for (auto& extra : c.extras)
                argv.insert(argv.end(), {"--extra", extra});
        }
        if (operation == "build")
            argv.insert(argv.end(), {"--out-dir", "${tree}/" + c.build_dir});
        if (operation == "run") {
            // The environment is the one sync made; everything after the program is the program's own.
            argv.push_back("--no-sync");
            argv.insert(argv.end(), c.run_command.begin(), c.run_command.end());
            argv.insert(argv.end(), p.args.begin(), p.args.end());
        }
        return {std::move(out)};
    }
} // namespace builds
