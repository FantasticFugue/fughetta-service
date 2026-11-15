export module kotlin;
import std;
import content;
export import helper;
export import provider;

export namespace language {
    template <> struct server<"kotlin"> {
        using adapter = helper_adapter;
        struct options {
            std::string executable = "java";
            std::string host = "fughetta-jvm";
            std::string adapter = "fughetta-kotlin";
            builds::environment env;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> documents{{{"kt", "kotlin"}, {"java", "java"}}};
        static constexpr std::array<std::string_view, 1> inputs{"project_model"};
        static constexpr std::array<std::string_view, 1> operations{"evaluate"};
        static constexpr std::array<std::string_view, 9> facts{"symbols", "types",     "definitions", "occurrences", "references",
                                                               "scopes",  "relations", "diagnostics", "typed_ranges"};
        static constexpr semantic::unit_kind unit_kind = semantic::unit_kind::module;
        static constexpr bool negotiated = false, dependency_reads = true;
        static constexpr std::string_view update_granularity = "module";
        static auto channels() -> std::vector<semantic::channel_definition> {
            auto out = source_channels();
            out.front().projections = {std::string{"declarations"}};
            return out;
        }
        static auto requirements(const options& o) -> builds::requirements { return builds::tools({o.executable, o.host, o.adapter}); }
        static void validate(const options& o) {
            (void)requirements(o);
            for (const auto& value : {o.executable, o.host, o.adapter}) {
                if (value.empty() || value.contains('\0') || value.contains("${"))
                    throw std::invalid_argument("Kotlin requires Java, fughetta-jvm host and Kotlin adapter JAR tool mappings");
                if (value.find_first_of("/\\") != std::string::npos)
                    content::checked_path(value);
            }
        }
        static auto launch(const options& o, const std::vector<builds::context>& inputs) -> program {
            if (inputs.size() != 1)
                throw std::invalid_argument("Kotlin analysis requires one project_model input");
            return {.provider = "kotlin",
                    .argv = {o.executable, "-jar", builds::tool(o.host), "--adapter", builds::tool(o.adapter)},
                    .env = o.env,
                    .executables = {o.host, o.adapter}};
        }
    };
    static_assert(speaker<server<"kotlin">>);
} // namespace language
