export module csharp;
import std;
import content;
export import helper;
export import provider;

export namespace language {
    template <> struct server<"csharp"> {
        using adapter = helper_adapter;
        struct options {
            std::string executable = "dotnet", host = "fughetta-clr", adapter = "fughetta-csharp";
            builds::environment env;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> documents{{{"cs", "csharp"}, {"csx", "csharp"}}};
        static constexpr std::array<std::string_view, 1> inputs{"project_model"};
        static constexpr std::array<std::string_view, 1> operations{"evaluate"};
        static constexpr std::array<std::string_view, 9> facts{"symbols", "types",     "definitions", "occurrences", "references",
                                                               "scopes",  "relations", "diagnostics", "typed_ranges"};
        static constexpr semantic::unit_kind unit_kind = semantic::unit_kind::module;
        static constexpr bool negotiated = false, dependency_reads = true;
        static constexpr std::string_view update_granularity = "module";
        static constexpr bool model_code = true;
        static auto executes_project_code(const options& o) -> bool {
            return !o.env.empty() || builds::in_project({o.executable, o.host, o.adapter});
        }
        static auto conventions() -> std::vector<semantic::language_conventions> {
            return {{.language = "csharp",
                     .visibility = {"public", "protected_internal", "protected", "internal", "private_protected", "private", "local"},
                     .value_wrappers = {"System.Nullable", "System.Threading.Tasks.Task", "System.Threading.Tasks.ValueTask"},
                     .wrapper_arguments = "<",
                     .scope_edges = {"base", "import"},
                     .symbol_traits = {{"csharp:record", "type"}, {"csharp:struct", "type"}, {"csharp:interface", "type"},
                                       {"csharp:delegate", "type"}, {"csharp:event", "variable"}},
                     .escape_types = {{"System.Object", "object", "object?", "dynamic"}},
                     .unsafe_casts = {{"System.Runtime.CompilerServices.Unsafe.As"}}}};
        }
        static auto channels() -> std::vector<semantic::channel_definition> {
            auto out = source_channels();
            out.front().projections = {std::string{"declarations"}};
            out.push_back(linkage_channel());
            return out;
        }
        using emits = std::tuple<domain<"clr">>;
        static auto requirements(const options& o) -> builds::requirements { return builds::tools({o.executable, o.host, o.adapter}); }
        static void validate(const options& o) {
            for (const auto* value : {&o.executable, &o.host, &o.adapter}) {
                if (value->empty() || value->contains('\0') || value->contains("${"))
                    throw std::invalid_argument("C# requires dotnet, fughetta-clr host and C# adapter DLL tool mappings");
                if (value->find_first_of("/\\") != std::string::npos)
                    content::checked_path(*value);
            }
            (void)requirements(o);
        }
        static auto launch(const options& o, const std::vector<builds::context>& inputs) -> program {
            if (inputs.size() != 1 || inputs.front().type != "project_model")
                throw std::invalid_argument("C# analysis requires one project_model input");
            return {.provider = "csharp",
                    .argv = {o.executable, builds::tool(o.host), "--adapter", builds::tool(o.adapter)},
                    .env = o.env,
                    .executables = {o.host, o.adapter}};
        }
    };
    static_assert(frontend<server<"csharp">>);
} // namespace language
