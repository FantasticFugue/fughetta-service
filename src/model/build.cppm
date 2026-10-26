export module build;
import std;
import meta;
import reflection;
import content;
export import provider;
export import cmake;
export import bazel;
export import commands;
export import vcpkg;
export import uv;
export import ty;
export import ruff;

export namespace builds {
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
    using adapters = catalog<"cmake", "bazel", "command", "vcpkg", "uv", "ty", "ruff">;
    using artifacts = formats<"compilation_database">;
    using builder = adapters::builder;

    struct task {
        std::string builder;
        std::string operation;
        std::vector<std::string> after;
        std::map<std::string, reference> inputs;
        builds::parameters parameters;
    };
    struct operation {
        std::vector<std::string> tasks;
        std::optional<std::string> receiver;
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
    // Binds a step to a tree on this machine, its mapped tools and the parallel jobs it may use.
    auto locate(step value, const std::filesystem::path& tree, const toolset& tools, std::uint32_t jobs) -> step;
    auto inspect(const context& artifact, const std::filesystem::path& tree) -> artifact_result;
} // namespace builds
