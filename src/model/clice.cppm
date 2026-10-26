export module clice;
import std;
import meta;
import reflection;
export import provider;

namespace language {
    // The clice C and C++ language server, given its compilation databases through initializationOptions.
    template <> struct server<"clice"> {
        struct options {
            std::string executable = "clice";
            std::vector<std::string> args;
            builds::environment env;
            // Where clice keeps its caches, relative to the project; omitted, a clice directory beside the first database.
            std::optional<std::string> cache_dir;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 15> documents{{
            {"c", "c"},    {"cc", "cpp"},  {"cpp", "cpp"}, {"cxx", "cpp"}, {"c++", "cpp"}, {"h", "cpp"},   {"hh", "cpp"},  {"hpp", "cpp"},
            {"hxx", "cpp"}, {"h++", "cpp"}, {"ipp", "cpp"}, {"tpp", "cpp"}, {"cppm", "cpp"}, {"ixx", "cpp"}, {"mpp", "cpp"},
        }};
        static constexpr std::array<std::string_view, 1> inputs{"compilation_database"};

        static void validate(const options& o) {
            if (o.executable.empty())
                throw std::invalid_argument("clice executable is required");
            if (o.cache_dir)
                builds::relative(*o.cache_dir);
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
            auto beside = first.kind == "directory" ? std::filesystem::path(first.path) : std::filesystem::path(first.path).parent_path();
            auto cache = o.cache_dir.value_or((beside / "clice").generic_string());
            rfl::Generic::Object rule, project, init;
            rule["compile_commands"] = std::move(paths);
            project["cache_dir"] = std::move(cache);
            init["rules"] = rfl::Generic::Array{std::move(rule)};
            init["project"] = std::move(project);
            out.initialization = std::move(init);
            return out;
        }
    };
    static_assert(speaker<server<"clice">>);
} // namespace language
