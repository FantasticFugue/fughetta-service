export module scala;
import std;
import content;
export import helper;
export import provider;

export namespace language {
    template <> struct server<"scala"> {
        using adapter = helper_adapter;
        struct options {
            std::string executable = "java";
            std::string host = "fughetta-jvm";
            std::string adapter = "fughetta-scala";
            builds::environment env;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> documents{{{"scala", "scala"}, {"sc", "scala"}, {"java", "java"}}};
        static constexpr std::array<std::string_view, 1> inputs{"project_model"};
        static constexpr std::array<std::string_view, 1> operations{"evaluate"};
        static constexpr std::array<std::string_view, 9> facts{"symbols", "types",     "definitions", "occurrences", "references",
                                                               "scopes",  "relations", "diagnostics", "typed_ranges"};
        static constexpr semantic::unit_kind unit_kind = semantic::unit_kind::module;
        static constexpr bool negotiated = false, dependency_reads = true;
        static constexpr std::string_view update_granularity = "module";
        static auto conventions() -> std::vector<semantic::language_conventions> {
            return {{.language = "scala",
                     .visibility = {"public", "protected", "package", "private"},
                     .value_wrappers = {"scala.Option", "scala.Some", "scala.util.Try", "scala.util.Either", "scala.concurrent.Future"},
                     .wrapper_arguments = "[",
                     .scope_edges = {"base", "import"},
                     .type_traits = {{"scala:curried_signature", "function"}, {"scala:context_function", "function"}, {"scala:by_name", "reference"}},
                     .symbol_traits = {{"scala:trait", "type"}, {"scala:object", "type"}, {"scala:enum", "type"}, {"scala:given", "variable"},
                                       {"scala:type_member", "type"}},
                     .escape_types = {{"scala.Any", "Any", "scala.AnyRef", "AnyRef", "java.lang.Object"}},
                     .unsafe_casts = {{"scala.Any.asInstanceOf"}}}};
        }
        static auto channels() -> std::vector<semantic::channel_definition> {
            auto out = source_channels();
            out.front().projections = {std::string{"declarations"}};
            out.push_back(linkage_channel());
            return out;
        }
        using emits = std::tuple<domain<"jvm">>;
        static auto requirements(const options& o) -> builds::requirements { return builds::tools({o.executable, o.host, o.adapter}); }
        // The compiler may execute macros from plain classpath references.
        static auto executes_project_code(const options&) -> bool { return true; }
        static void validate(const options& o) {
            for (const auto& value : {o.executable, o.host, o.adapter}) {
                if (value.empty() || value.contains('\0') || value.contains("${"))
                    throw std::invalid_argument("Scala requires Java, fughetta-jvm host and Scala adapter JAR tool mappings");
                if (value.find_first_of("/\\") != std::string::npos)
                    content::checked_path(value);
            }
            (void)requirements(o);
        }
        static auto launch(const options& o, const std::vector<builds::context>& inputs) -> program {
            if (inputs.size() != 1 || inputs.front().type != "project_model")
                throw std::invalid_argument("Scala analysis requires one project_model input");
            return {.provider = "scala",
                    .argv = {o.executable, "-jar", builds::tool(o.host), "--adapter", builds::tool(o.adapter)},
                    .env = o.env,
                    .executables = {o.host, o.adapter}};
        }
    };
    static_assert(frontend<server<"scala">>);
} // namespace language
