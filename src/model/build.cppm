export module build;
import std;
import meta;
import reflection;
import content;
export import provider;
export import report;
export import cmake;
export import bazel;
export import commands;
export import vcpkg;
export import typescript;
export import bun;
export import uv;
export import ty;
export import ruff;

export namespace builds {
    struct timings {
        std::optional<double> sync_seconds, queue_seconds, dependency_seconds, worker_seconds;
    };

    // One builder of a configuration: a build system by name and that system's options.
    template <meta::fixed_string Name> struct with {
        using implementation = builds::adapter<Name>;
        static constexpr std::string_view name = Name.view();
        literal<Name> adapter;
        config<Name> options;
        rfl::Rename<"requires", requirements> needs;
        environment env;
    };

    // The build systems and artifact formats this program knows; a configuration names one of them or is refused when parsed.
    template <meta::fixed_string... Names> struct catalog {
        static_assert((system<adapter<Names>> && ...));
        static_assert(meta::distinct(std::array{std::string_view{Names}...}));
        using builder = rfl::TaggedUnion<"adapter", with<Names>...>;
    };
    template <meta::fixed_string... Names> struct formats {
        static_assert((layout<format<Names>> && ...));
        // Calls `f` with the format named `name`; false when no format has that name.
        template <class F> static auto find(std::string_view name, F&& f) -> bool { return ((name == Names.view() && (f(format<Names>{}), true)) || ...); }
    };
    // The report protocols; a step's report names one of them.
    template <meta::fixed_string... Names> struct protocols {
        static_assert((parses<protocol<Names>> && ...));
        static auto known(std::string_view name) -> bool { return ((name == Names.view()) || ...); }
        static auto read(std::string_view name, std::string_view text) -> reading {
            reading out;
            (void)((name == Names.view() && (out = protocol<Names>::read(text), true)) || ...);
            return out;
        }
        static constexpr std::array names{Names.view()...};
    };
    using adapters = catalog<"cmake", "bazel", "command", "vcpkg", "tsc", "bun", "uv", "ty", "ruff">;
    using artifacts = formats<"compilation_database">;
    using report_protocols = protocols<"junit", "bep", "codequality">;
    using builder = adapters::builder;

    struct task {
        std::string builder;
        std::string operation;
        std::vector<std::string> after;
        std::map<std::string, reference> inputs;
        builds::parameters parameters;
        // Reports the task's steps write besides the ones their adapter declares: a command or `uv run` that starts pytest with
        // --junit-xml=${reports}/pytest.xml names {"protocol": "junit", "file": "pytest.xml"}.
        std::vector<report> reports;
    };
    struct operation {
        std::vector<std::string> tasks;
        std::optional<std::string> receiver;
        // Named files or directories, relative to the working tree, delivered after this operation.
        std::map<std::string, std::string> exports;
    };
    struct configuration {
        std::map<std::string, builder> builders;
        std::map<std::string, task> tasks;
        std::map<std::string, operation, std::less<>> operations;
        std::map<std::string, reference> outputs;
        rfl::Rename<"requires", requirements> needs;
        environment env;
        bool enabled = true;
        builds::limits limits;
    };
    struct request {
        std::string operation;
        builds::parameters parameters;
    };

    auto adapter_of(const builder& b) -> std::string_view;
    // The option schema of every build system, by name.
    auto schemas() -> const std::map<std::string, rfl::Generic>&;
    void validate(const configuration& c);
    auto prepare(const configuration& c, const request& request) -> plan;
    // Selects only the tasks and prerequisites that produce the artifact types a consumer needs.
    auto prepare_inputs(const configuration& c, const std::vector<std::string>& types, const std::map<std::string, std::string>& paths = {}) -> plan;
    // Binds a step to a tree on this machine, its mapped tools, the parallel jobs it may use and the directory of its reports.
    auto locate(step value, const std::filesystem::path& tree, const toolset& tools, std::uint32_t jobs, const std::filesystem::path& reports = {},
                const std::filesystem::path& dependency_cache = {}, const std::filesystem::path& dependency_upstream = {}) -> step;
    // What the reports a bound step left in `reports` say, with paths made relative to `tree`; a missing report adds nothing,
    // and one that cannot be read says so as a diagnostic.
    auto collect(const step& bound, const std::filesystem::path& reports, const std::filesystem::path& tree) -> reading;
    auto inspect(const context& artifact, const std::filesystem::path& tree) -> artifact_result;
} // namespace builds
