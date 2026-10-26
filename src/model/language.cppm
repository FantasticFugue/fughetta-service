export module language;
import std;
import meta;
import reflection;
import content;
export import build;
export import clice;
export import ty;
export import ruff;

export namespace language {
    // The language server of an analysis: a provider by name and that provider's options.
    template <meta::fixed_string Name> struct with {
        using implementation = server<Name>;
        static constexpr std::string_view name = Name.view();
        builds::literal<Name> provider;
        config<Name> options;
    };

    // The language servers this program knows; an analysis names one of them or is refused when parsed.
    template <meta::fixed_string... Names> struct catalog {
        static_assert((speaker<server<Names>> && ...));
        static_assert(meta::distinct(std::array{std::string_view{Names}...}));
        using choice = rfl::TaggedUnion<"provider", with<Names>...>;
        // The language ID provider `name` tells the server a document at `path` is in; none when it does not read the document.
        static auto document(std::string_view name, std::string_view path) -> std::optional<std::string_view> {
            auto extension = std::filesystem::path(path).extension().string();
            if (extension.empty())
                return std::nullopt;
            std::optional<std::string_view> out;
            auto look = [&](const auto& documents) {
                for (auto& [suffix, id] : documents)
                    if (suffix == std::string_view{extension}.substr(1))
                        out = id;
            };
            (void)((name == Names.view() && (look(server<Names>::documents), true)) || ...);
            return out;
        }
    };
    using providers = catalog<"clice", "ty", "ruff">;
    using provider = providers::choice;

    struct configuration {
        provider server;
        builds::configuration build;
        std::string operation = "prepare";
        // The build outputs the server reads, each of a type its provider accepts.
        std::vector<std::string> inputs;
    };
    struct plan {
        builds::plan build;
        program server;
        std::vector<builds::context> inputs;
        content::hash fingerprint;
        content::hash recipe;
    };
    struct report {
        std::string name;
        std::int32_t exit_code = -1;
        bool timed_out = false;
        double seconds = 0;
        std::string output;
        bool truncated = false;
    };
    struct result {
        std::string operation_id;
        std::string status = "not_prepared";
        std::string worldline;
        std::string revision;
        content::hash snapshot;
        content::hash configuration;
        std::string tree;
        std::vector<builds::artifact_result> inputs;
        std::vector<builds::artifact_result> artifacts;
        std::vector<std::string> build_dirs;
        std::vector<report> steps;
        std::string error;
        bool replayed = false;
        // What the language server found in the prepared tree, published into the node for this version and environment:
        // "analyzed", "failed" (see analysis_error) or "not_analyzed" when no server is configured.
        std::string analysis = "not_analyzed";
        std::optional<std::string> analysis_error;
        std::uint64_t scopes = 0, symbols = 0;
        // The worker that prepared it; empty when the source did, which alone can check its tree afterwards.
        std::string worker;
        std::optional<std::string> worker_id;
    };
    struct request {
        std::string project;
        std::vector<std::string> ignore;
        std::string action = "prepare";
        std::string operation_id;
        std::string worldline;
        std::string revision;
        language::configuration config;
        // The analysis environment the gateway names this configuration by; scope locks and queries use it.
        std::string environment;
    };

    auto provider_of(const provider& p) -> std::string_view;
    void validate(const configuration& c);
    auto prepare(const configuration& c) -> plan;
    auto locate(program value, const std::filesystem::path& tree, const builds::toolset& tools, std::uint32_t jobs) -> program;
    // Refuses a plan whose build directories or outputs overlap the tracked files of the tree to prepare.
    void check(const plan& p, const std::vector<content::entry>& files);
    auto timeout(const configuration& c) -> std::uint32_t;
    auto output_limit(const configuration& c) -> std::uint64_t;
} // namespace language
