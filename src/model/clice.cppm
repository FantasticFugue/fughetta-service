export module clice;
import std;
import meta;
import reflection;
export import provider;

namespace language {
    // The clice C and C++ language server. It is started as `clice serve [args]` in the project directory, which it takes as its
    // workspace, and is told through initializationOptions what clice.toml documents: `rules`, holding the compile_commands of the
    // build outputs and flags appended to or removed from their commands, and `project.cache_dir`. Paths are relative to the project,
    // and the server resolves them from the prepared tree, which a worker may hold elsewhere. The cache holds the PCH, PCM and index
    // of a session and the logs beneath it, inside the project.
    template <> struct server<"clice"> {
        struct options {
            // `clice`, or the path of a clice build inside the project; elsewhere, map 'clice' in the tools of each worker and the studio.
            std::string executable = "clice";
            // What follows `serve`, such as --log-level and its value.
            std::vector<std::string> args;
            builds::environment env;
            // Where clice keeps its caches and logs; omitted, a clice directory beside the first database.
            std::optional<std::string> cache_dir;
            // A rule's compile-time flags, appended to every compilation command.
            std::vector<std::string> append;
            // A rule's compile-time flags, removed from every command.
            std::vector<std::string> remove;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 16> documents{{
            {"c", "c"},     {"cc", "cpp"},   {"cpp", "cpp"}, {"cxx", "cpp"}, {"c++", "cpp"}, {"h", "cpp"},    {"hh", "cpp"},  {"hpp", "cpp"},
            {"hxx", "cpp"}, {"h++", "cpp"}, {"ipp", "cpp"}, {"tpp", "cpp"}, {"cppm", "cpp"}, {"ixx", "cpp"}, {"mpp", "cpp"}, {"cu", "cpp"},
        }};
        static constexpr std::array<std::string_view, 1> inputs{"compilation_database"};

        static void validate(const options& o) {
            if (o.executable.empty() || o.executable.find('\0') != std::string::npos)
                throw std::invalid_argument("clice executable is required");
            if (!std::filesystem::path(o.executable).is_absolute())
                builds::tools({o.executable});
            if (o.cache_dir)
                builds::relative(*o.cache_dir);
            builds::validate_environment(o.env);
            for (auto& arg : o.args)
                if (arg.empty() || arg == "--" || arg.find('\0') != std::string::npos || arg.contains("${"))
                    throw std::invalid_argument("clice argument is empty or a step reference");
            for (auto& flag : o.append) {
                if (flag.empty() || flag.contains("${") || flag.find('\0') != std::string::npos)
                    throw std::invalid_argument("clice appended flag is empty or a step reference");
            }
            for (auto& flag : o.remove)
                if (flag.empty() || flag.contains("${") || flag.find('\0') != std::string::npos)
                    throw std::invalid_argument("clice removed flag is empty or a step reference");
        }
        static auto launch(const options& o, const std::vector<builds::context>& databases) -> program {
            if (databases.empty())
                throw std::invalid_argument("clice requires a compilation database to read");
            program out{.argv = {o.executable, "serve"}, .env = o.env};
            out.argv.insert(out.argv.end(), o.args.begin(), o.args.end());
            rfl::Generic::Array paths;
            for (auto& database : databases)
                paths.push_back(database.path);
            auto& first = databases.front();
            auto beside = first.kind == "directory" ? first.path : std::filesystem::path(first.path).parent_path().generic_string();
            rfl::Generic::Object rule, project, init;
            rule["compile_commands"] = std::move(paths);
                if (!o.append.empty()) {
                    rfl::Generic::Array flags;
                    for (auto& flag : o.append)
                        flags.push_back(flag);
                    rule["append"] = std::move(flags);
                }
                if (!o.remove.empty()) {
                    rfl::Generic::Array flags;
                    for (auto& flag : o.remove)
                        flags.push_back(flag);
                    rule["remove"] = std::move(flags);
                }
            init["rules"] = rfl::Generic::Array{std::move(rule)};
            project["cache_dir"] = o.cache_dir ? *o.cache_dir : beside + "/clice";
            init["project"] = std::move(project);
            out.initialization = std::move(init);
            return out;
        }
    };
    static_assert(speaker<server<"clice">>);
} // namespace language
