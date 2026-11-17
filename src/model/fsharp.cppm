export module fsharp;
import std;
import content;
export import helper;
export import provider;

export namespace language {
    template <> struct server<"fsharp"> {
        using adapter = helper_adapter;
        struct options {
            std::string executable = "dotnet", host = "fughetta-clr", adapter = "fughetta-fsharp";
            builds::environment env;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> documents{{{"fs", "fsharp"}, {"fsi", "fsharp"}, {"fsx", "fsharp"}}};
        static constexpr std::array<std::string_view, 1> inputs{"project_model"};
        static constexpr std::array<std::string_view, 1> operations{"evaluate"};
        static constexpr std::array<std::string_view, 8> facts{"symbols",    "types",  "definitions", "occurrences",
                                                               "references", "scopes", "diagnostics", "typed_ranges"};
        static constexpr semantic::unit_kind unit_kind = semantic::unit_kind::module;
        static constexpr bool negotiated = false, dependency_reads = true;
        static constexpr std::string_view update_granularity = "ordered-project";
        static auto channels() -> std::vector<semantic::channel_definition> {
            auto out = source_channels();
            out.front().projections = {std::string{"declarations"}};
            out.push_back(linkage_channel());
            return out;
        }
        using emits = std::tuple<domain<"clr">>;
        static auto requirements(const options& o) -> builds::requirements { return builds::tools({o.executable, o.host, o.adapter}); }
        // The compiler service runs only the type providers and analyzers the project model names; a model without them runs no project code.
        static constexpr bool model_code = true;
        static auto executes_project_code(const options&) -> bool { return false; }
        static void validate(const options& o) {
            (void)requirements(o);
            for (const auto& value : {o.executable, o.host, o.adapter}) {
                if (value.empty() || value.contains('\0') || value.contains("${"))
                    throw std::invalid_argument("F# requires dotnet, fughetta-clr host and F# adapter DLL tool mappings");
                if (value.find_first_of("/\\") != std::string::npos)
                    content::checked_path(value);
            }
        }
        static auto launch(const options& o, const std::vector<builds::context>& inputs) -> program {
            if (inputs.size() != 1 || inputs.front().type != "project_model")
                throw std::invalid_argument("F# analysis requires one project_model input");
            return {.provider = "fsharp",
                    .argv = {o.executable, builds::tool(o.host), "--adapter", builds::tool(o.adapter)},
                    .env = o.env,
                    .executables = {o.host, o.adapter}};
        }
    };
    static_assert(frontend<server<"fsharp">>);
} // namespace language
