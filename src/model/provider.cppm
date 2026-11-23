export module provider;
import utility;
import std;
import meta;
import json;
import content;
import toolchain;
export import records;

// What every build system and language server shares, and the templates each one specializes by name. The set of
// built-in providers is listed by the build and language modules; configured helpers share the language transport.
export namespace builds {
    inline auto absolute_path(std::string_view value) -> bool {
        return value.starts_with('/') || value.starts_with("\\\\") ||
               (value.size() > 2 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':' && (value[2] == '/' || value[2] == '\\'));
    }

    using environment = std::map<std::string, std::string>;
    using toolset = std::map<std::string, std::string>;
    struct requirements {
        std::vector<std::string> tools;
        struct platform {
            std::optional<std::string> os;
            std::optional<std::string> arch;
            friend auto operator==(const platform&, const platform&) -> bool = default;
        } platform;
        std::vector<toolchain::sdk_constraint> sdks;
        friend auto operator==(const requirements&, const requirements&) -> bool = default;
    };
    struct limits {
        std::optional<std::uint32_t> timeout_s;
        std::optional<std::uint64_t> output_limit;
        std::optional<std::uint64_t> max_memory_bytes;
        std::optional<std::uint32_t> max_cpu_seconds;
    };
    struct context {
        std::string name;
        std::string type;
        std::string path;
        std::string kind = "file";
    };
    // A report a step writes in a fixed protocol: `file`, a plain name, in `${reports}`.
    struct report {
        std::string protocol;
        std::string file;
        bool directory = false;
        friend auto operator==(const report&, const report&) -> bool = default;
    };
    // `${tree}` is the build tree, `${tool:name}` a mapped tool, `${jobs}` the parallel jobs the runner allows and `${reports}` the
    // directory, empty when the step starts, that its reports go to. standard_output names a report file that receives the step's
    // standard output, for a tool that prints its report there; the step's output is then its standard error alone.
    struct dependency_input {
        std::string manifest_dir;
        std::string triplet;
        std::string host_triplet;
        std::vector<std::string> features;
        bool default_features = true;
    };
    struct generation {
        std::string producer;
        // Empty until the producer's final step; a file in ${reports} thereafter.
        std::string manifest;
        std::vector<context> outputs;
    };
    struct step {
        std::string name;
        std::vector<std::string> argv;
        std::string cwd;
        environment env;
        std::vector<context> outputs;
        std::vector<report> reports;
        std::optional<std::string> standard_output;
        // A provider-authored description, never an executable command for the publisher.
        std::optional<dependency_input> dependencies;
        bool preparation = false;
        std::optional<builds::generation> generation;
        std::vector<context> inputs;
        // The provider permits adjacent tasks to share one invocation when
        // these leading argv entries, the directory and environment agree.
        // Remaining argv entries are ordered tasks for that same build instance.
        std::optional<std::size_t> batch_prefix;
        // A model step may be reused when only the contents of existing sources here change.
        std::optional<std::vector<std::string>> model_source_dirs;
        bool gradle = false;
        bool cargo = false;
    };
    struct invocation {
        step command;
        std::vector<std::string> tasks;
    };
    inline auto invocations(const std::vector<step>& steps) -> std::vector<invocation> {
        std::vector<invocation> out;
        for (const auto& step : steps) {
            if (step.batch_prefix && (!*step.batch_prefix || *step.batch_prefix >= step.argv.size()))
                throw std::invalid_argument("build step has an invalid task batch prefix");
            auto standalone = [](const auto& command) {
                return command.batch_prefix && command.reports.empty() && !command.standard_output && !command.dependencies && !command.generation;
            };
            bool joined = false;
            if (!out.empty()) {
                auto& previous = out.back();
                auto& command = previous.command;
                if (standalone(command) && standalone(step) && command.batch_prefix == step.batch_prefix && command.cwd == step.cwd &&
                    command.env == step.env && command.preparation == step.preparation && command.gradle == step.gradle && command.cargo == step.cargo &&
                    std::equal(command.argv.begin(), command.argv.begin() + *command.batch_prefix, step.argv.begin())) {
                    command.argv.insert(command.argv.end(), step.argv.begin() + *step.batch_prefix, step.argv.end());
                    for (const auto& input : step.inputs)
                        if (!std::ranges::any_of(command.outputs, [&](const auto& output) { return output.path == input.path; }))
                            command.inputs.push_back(input);
                    command.outputs.insert(command.outputs.end(), step.outputs.begin(), step.outputs.end());
                    previous.tasks.push_back(step.name);
                    command.name = "batch";
                    joined = true;
                }
            }
            if (!joined)
                out.push_back({step, {step.name}});
        }
        return out;
    }
    struct artifact {
        std::string type;
        std::string path;
        std::string kind = "file";
    };
    struct reference {
        std::string task;
        std::string output;
    };
    struct input {
        reference producer;
        std::string type;
    };
    struct parameters {
        std::vector<std::string> targets;
        std::vector<std::string> args;
        std::optional<std::string> filter;
        std::optional<std::string> labels;
        std::optional<std::uint32_t> parallel;
    };
    struct operation_spec {
        std::set<std::string> parameters;
        std::map<std::string, std::string> inputs;
        std::map<std::string, artifact> outputs;
        requirements needs;
        std::vector<std::string> source_dirs;
        std::vector<std::string> build_dirs;
    };
    struct artifact_result {
        std::string name;
        std::string type;
        std::string path;
        content::hash digest;
        std::uint64_t units = 0;
        std::string kind = "file";
    };
    struct exported_object {
        content::hash digest;
        std::uint64_t size = 0;
    };
    struct exported_file {
        std::string name;
        std::string path;
        content::hash digest;
        std::uint64_t size = 0;
        bool executable = false;
        std::string delivery_error;
        std::vector<exported_object> objects;
    };
    struct task_plan {
        std::string name;
        std::vector<std::string> dependencies;
        std::size_t first_step = 0;
        std::size_t step_count = 0;
        std::vector<context> outputs;
    };
    struct plan {
        std::vector<step> steps;
        std::vector<task_plan> tasks;
        std::vector<context> contexts;
        requirements needs;
        environment env;
        std::vector<std::string> source_dirs;
        std::vector<std::string> build_dirs;
    };

    // A build system: `options` is what a builder of it is configured with.
    template <meta::fixed_string Name> struct adapter;
    template <meta::fixed_string Name> using config = typename adapter<Name>::options;
    template <class T>
    concept system = requires(const typename T::options& options, std::string_view operation, const parameters& parameters) {
        { T::validate(options) } -> std::same_as<void>;
        { T::describe(options) } -> std::same_as<std::map<std::string, operation_spec>>;
        { T::expand(options, operation, parameters) } -> std::same_as<std::vector<step>>;
    };

    // An artifact type with a fixed form: its kind, its size bound and what one holds, counted in units.
    template <meta::fixed_string Name> struct format;
    template <class T>
    concept layout = requires(std::string_view text) {
        { T::kind } -> std::convertible_to<std::string_view>;
        { T::max_bytes } -> std::convertible_to<std::uint64_t>;
        { T::check(text) } -> std::same_as<std::uint64_t>;
    };

    // What a step's reports say: its diagnostics, with paths relative to the tree, and the tests it ran.
    struct finding {
        std::string file;
        std::uint32_t line = 0;
        std::uint32_t column = 0;
        std::string severity;
        std::string message;
    };
    // Every case the run named, how many of those failed, and how many it skipped instead of running: a JUnit writer marks a
    // case it did not start with a <skipped> element, so a total with no failures no longer reads as a run that covered it.
    struct tally {
        std::uint32_t total = 0;
        std::uint32_t failed = 0;
        std::uint32_t skipped = 0;
        std::vector<std::string> failed_tests;
        std::vector<std::string> skipped_tests;
    };
    struct reading {
        std::vector<finding> diagnostics;
        std::optional<tally> tests;
    };

    // A report protocol: a format that a build system, test runner or checker writes by its own contract, read the same way
    // whatever version of the tool wrote it. Paths in a reading are as the report holds them.
    template <meta::fixed_string Name> struct protocol;
    template <class T>
    concept parses = requires(std::string_view text) {
        { T::read(text) } -> std::same_as<reading>;
    };

    template <meta::fixed_string Name> using literal = json::literal<Name>;

    void relative(std::string_view value, bool dot = false);
    void validate_environment(const environment& env);
    void validate(const requirements& needs);
    void combine(requirements& into, const requirements& from);
    auto tools(const std::vector<std::string>& executables) -> requirements;
    auto tool(const std::string& executable) -> std::string;
    // Whether any of these executables is a file of the project instead of a mapped tool or a machine path.
    auto in_project(const std::vector<std::string>& executables) -> bool;
    auto requested(const parameters& p) -> std::set<std::string>;
    void require(const operation_spec& available, const parameters& requested);
    void tool_name(std::string_view name);
} // namespace builds

export namespace language {
    struct resolution_error : std::invalid_argument, util::retained_exception {
        using std::invalid_argument::invalid_argument;
        auto retain() const noexcept -> std::exception_ptr override;
    };
    auto resolution_error::retain() const noexcept -> std::exception_ptr { return util::copy_current_exception(*this); }

    struct document {
        std::string path, language, text;
        friend auto operator==(const document&, const document&) -> bool = default;
    };
    struct session_state {
        std::string id, slot, project, provider, options, stage, snapshot;
        std::uint64_t epoch = 0;
        std::vector<semantic::input_object> inputs;
        json::literal<"created", "opening", "ready", "updating", "evaluating", "resolving", "failed", "closing", "closed"> phase;
    };
    struct evaluation_identity {
        std::string request_id, computation_id, stage_id;
        friend auto operator==(const evaluation_identity&, const evaluation_identity&) -> bool = default;
    };
    struct evaluation_request {
        evaluation_identity execution;
        std::string slot_id;
        semantic::unit_key unit;
        std::uint64_t epoch = 0;
        std::vector<semantic::input_object> inputs;
        semantic::extent demand;
        semantic::budget budget;
        bool retained_only = false;
    };
    struct evaluated_unit {
        session_state session;
        evaluation_request request;
        std::vector<semantic::shard> shards;
        std::optional<semantic::candidate_id> reused;
        bool deferred = false;
        std::vector<semantic::execution_receipt> receipts;
    };

    enum class port_direction : std::uint8_t { input, output, unspecified = 255 };
    struct port {
        std::string name, type;
        port_direction direction = port_direction::unspecified;
        bool required = false, multiple = false;
        std::vector<semantic::fact_kind> facts;
    };
    inline auto source_channels() -> std::vector<semantic::channel_definition> {
        return {{.name = "declarations", .projections = {std::string{"symbols"}, std::string{"types"}}},
                {.name = "definition_bodies",
                 .projections = {std::string{"definition_bodies"}},
                 .symbol_kinds = {semantic::symbol_kind::make<"function">(), semantic::symbol_kind::make<"method">(),
                                  semantic::symbol_kind::make<"constructor">(), semantic::symbol_kind::make<"destructor">()}}};
    }
    // The symbols, types and occurrences a binding resolver reads from one unit.
    // Source providers whose declarations bind across languages publish it too.
    inline auto linkage_channel() -> semantic::channel_definition {
        return {.name = "linkage", .projections = {std::string{"symbols"}, std::string{"types"}, std::string{"occurrences"}}};
    }
    template <class Provider> auto operations() -> std::vector<std::string> {
        if constexpr (requires { Provider::operations; })
            return {Provider::operations.begin(), Provider::operations.end()};
        else
            return {"evaluate", "resolve"};
    }
    template <class Provider> auto conventions() -> std::vector<semantic::language_conventions> {
        if constexpr (requires { Provider::conventions(); })
            return Provider::conventions();
        else
            return {};
    }
    struct input_validator {
        std::string_view type;
        bool (*validate)(const semantic::input_object&, std::span<const semantic::input_object>);
    };
    template <class Provider> auto input_validators() -> std::vector<input_validator> {
        if constexpr (requires { Provider::input_validators(); })
            return Provider::input_validators();
        else
            return {};
    }
    enum class adapter_kind { lsp, clice, helper, reader };
    using provider_role = json::literal<"source-analysis", "artifact-reader", "relation-resolver">;
    struct description {
        std::string id;
        std::vector<provider_role> roles;
        std::vector<std::string> languages;
        // The link domains this provider's declarations are built into.
        std::vector<std::string> emits;
        // The link domain whose artifacts this provider reads.
        std::optional<std::string> reads;
        json::value configuration_schema;
        std::vector<port> ports;
        std::vector<semantic::channel_definition> channels;
        // How each language the provider reports spells visibility, value wrappers and scope edges.
        std::vector<semantic::language_conventions> conventions;
        // The adapter's export surface; a session may supply a subset after negotiation.
        std::vector<semantic::fact_kind> facts;
        std::vector<std::string> operations{"evaluate", "resolve"};
        bool negotiated = true;
        adapter_kind adapter = adapter_kind::lsp;
        semantic::unit_kind unit_kind = semantic::unit_kind::file;
        std::string update_granularity = "document";
        bool dependency_reads = false, cancellation = true, exclusive_directory = true;
        // Whether a session under this configuration runs code the project supplies.
        bool executes_project_code = false;
        // Whether a session loads the compiler plugins or type providers its project model names, which may be the project's own.
        bool model_code = false;
        builds::requirements needs;
    };
    struct program {
        std::string provider;
        std::vector<std::string> argv;
        builds::environment env;
        // Sent as the initialize request's initializationOptions.
        json::value initialization;
        // Additional provider operations; each argv is resolved in the same worker slot.
        std::map<std::string, std::vector<std::string>> commands;
        // Auxiliary executable inputs selected by the provider, such as tsserver.
        std::vector<std::string> executables;
        // Configured helpers carry their public contract independently of the
        // built-in catalog entry used to configure the transport.
        std::optional<description> registration;
        std::map<std::string, std::string> documents;
    };

    // A language server: `options` is what an analysis configures it with, `documents` maps file extensions to the
    // language IDs it is told, `inputs` names the artifact types it reads, and `launch` starts it on them.
    // `executes_project_code` says whether a session under these options runs code the project supplies.
    template <meta::fixed_string Name> struct server;
    template <meta::fixed_string Name> using config = typename server<Name>::options;
    template <class T>
    concept speaker = requires(const typename T::options& options, const std::vector<builds::context>& inputs) {
        typename T::adapter;
        { T::validate(options) } -> std::same_as<void>;
        { T::launch(options, inputs) } -> std::same_as<program>;
        { T::requirements(options) } -> std::same_as<builds::requirements>;
        { T::executes_project_code(options) } -> std::same_as<bool>;
        { T::documents.size() } -> std::convertible_to<std::size_t>;
        { T::inputs.size() } -> std::convertible_to<std::size_t>;
        { T::facts.size() } -> std::convertible_to<std::size_t>;
        { T::channels() } -> std::same_as<std::vector<semantic::channel_definition>>;
        { T::unit_kind } -> std::convertible_to<semantic::unit_kind>;
    };

    // A link domain: the family of built artifacts in which a declaration gets the identity a linker or loader
    // resolves. `artifact` is the input type a reader of the domain takes. A domain that bindings target also names
    // `linkage`, the symbol extension holding that identity.
    template <meta::fixed_string Name> struct domain;
    template <class T>
    concept link_domain = requires {
        { T::name } -> std::convertible_to<std::string_view>;
        { T::artifact } -> std::convertible_to<std::string_view>;
    };

    // A provider is exactly one of three things. A frontend gives source-level semantics and lists, as a tuple of
    // domains, where its declarations are built into. A domain reader gives the symbols of one domain's artifacts. A
    // bridge resolves the bindings from one domain into another.
    template <class T>
    concept frontend = speaker<T> && requires { typename T::emits; } && !requires { typename T::domain; } && !requires { typename T::to; };
    template <class T>
    concept domain_reader = speaker<T> && link_domain<typename T::domain> && !requires { typename T::emits; } && !requires { typename T::to; };
    template <class T>
    concept bridge = speaker<T> && link_domain<typename T::from> && link_domain<typename T::to> && requires {
        { T::to::linkage } -> std::convertible_to<std::string_view>;
    } && !requires { typename T::emits; } && !requires { typename T::domain; };
    namespace roles {
        inline constexpr auto frontend = provider_role::make<"source-analysis">();
        inline constexpr auto reader = provider_role::make<"artifact-reader">();
        inline constexpr auto bridge = provider_role::make<"relation-resolver">();
    }
    template <class T>
        requires frontend<T> || domain_reader<T> || bridge<T>
    constexpr provider_role role = frontend<T> ? roles::frontend : domain_reader<T> ? roles::reader : roles::bridge;

    template <class... Domains> auto domain_names(std::tuple<Domains...>*) -> std::vector<std::string> { return {std::string{Domains::name}...}; }
    template <frontend T> auto emitted() -> std::vector<std::string> { return domain_names(static_cast<typename T::emits*>(nullptr)); }

    // The ports every reader of a domain has: its artifact, the source facts whose definitions it joins to the
    // artifact's debug locations, and between them the optional inputs a reader lists by name and type.
    template <meta::fixed_string Name, meta::fixed_string Type> struct optional_input {};
    template <link_domain Domain, class... Inputs> struct reads;
    template <link_domain Domain, meta::fixed_string... Names, meta::fixed_string... Types> struct reads<Domain, optional_input<Names, Types>...> {
        using domain = Domain;
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 0> documents{};
        static constexpr std::array<std::string_view, sizeof...(Names) + 2> inputs{"artifact", Names.view()..., "source_facts"};
        static constexpr std::array<std::string_view, 1> operations{"evaluate"};
        static constexpr semantic::unit_kind unit_kind = semantic::unit_kind::artifact;
        static constexpr bool negotiated = false;
        static auto multiple_input(std::string_view port) -> bool { return port == "source_facts"; }
        static auto required_inputs(const auto&) -> std::vector<std::string> { return {"artifact"}; }
        static auto input_type(std::string_view port) -> std::string_view {
            std::string_view type = port == "artifact" ? std::string_view{Domain::artifact} : "semantic_shard";
            (void)((port == Names.view() && (type = Types.view(), true)) || ...);
            return type;
        }
        static auto consumed_facts(std::string_view port) -> std::vector<semantic::fact_kind> {
            return port == "source_facts" ? std::vector<semantic::fact_kind>{std::string{"symbols"}, std::string{"definitions"}}
                                          : std::vector<semantic::fact_kind>{};
        }
    };

    // The ports, facts and channel every bridge has: the declarations of the domain bindings start in, the
    // definitions of the domain they land in, and the relations between them.
    template <link_domain From, link_domain To> struct binds {
        using from = From;
        using to = To;
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 0> documents{};
        static constexpr std::array<std::string_view, 2> inputs{"managed", "native"}, facts{"relations", "diagnostics"};
        static constexpr std::array<std::string_view, 1> operations{"evaluate"};
        static constexpr semantic::unit_kind unit_kind = semantic::unit_kind::group;
        static constexpr bool negotiated = false, multiple_inputs = true;
        static auto input_type(std::string_view) -> std::string_view { return "semantic_shard"; }
        static auto consumed_facts(std::string_view port) -> std::vector<semantic::fact_kind> {
            std::vector<semantic::fact_kind> out{std::string{"symbols"}, std::string{"definitions"}};
            if (port == "managed")
                out.push_back(std::string{"types"});
            return out;
        }
        static auto channels() -> std::vector<semantic::channel_definition> {
            return {{.name = "bindings", .projections = {std::string{"relations"}, std::string{"diagnostics"}}}};
        }
        static auto requirements(const auto&) -> builds::requirements { return {}; }
        static auto executes_project_code(const auto&) -> bool { return false; }
    };
} // namespace language

namespace builds {
    void relative(std::string_view value, bool dot) {
        if (dot && (value.empty() || value == "."))
            return;
        content::checked_path(value);
        if (value.find(':') != std::string_view::npos)
            throw std::invalid_argument("build paths must be relative to the project");
    }
    void validate_environment(const environment& env) {
        for (auto& [key, value] : env)
            if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
                throw std::invalid_argument("invalid build environment variable");
    }
    void validate(const requirements& needs) {
        for (const auto& tool : needs.tools)
            toolchain::require_tool(tool);
        for (const auto& sdk : needs.sdks)
            toolchain::require_sdk(sdk);
        if ((needs.platform.os && needs.platform.os->empty()) || (needs.platform.arch && needs.platform.arch->empty()))
            throw std::invalid_argument("platform requirement is empty");
    }
    void combine(requirements& into, const requirements& from) {
        validate(from);
        for (auto& tool : from.tools)
            if (!std::ranges::contains(into.tools, tool))
                into.tools.push_back(tool);
        for (const auto& sdk : from.sdks)
            if (!std::ranges::contains(into.sdks, sdk))
                into.sdks.push_back(sdk);
        auto merge = [](auto& a, const auto& b) {
            if (a && b && a != b)
                throw std::invalid_argument("build tasks require conflicting platforms");
            if (b)
                a = b;
        };
        merge(into.platform.os, from.platform.os);
        merge(into.platform.arch, from.platform.arch);
    }
    auto requested(const parameters& p) -> std::set<std::string> {
        std::set<std::string> out;
        if (!p.targets.empty())
            out.insert("targets");
        if (!p.args.empty())
            out.insert("args");
        if (p.filter)
            out.insert("filter");
        if (p.labels)
            out.insert("labels");
        if (p.parallel)
            out.insert("parallel");
        return out;
    }
    void require(const operation_spec& c, const parameters& p) {
        for (auto& name : requested(p))
            if (!c.parameters.contains(name))
                throw std::invalid_argument("operation does not accept the " + name + " parameter");
        if (p.parallel && !*p.parallel)
            throw std::invalid_argument("parallel must be positive");
        for (auto& target : p.targets)
            if (target.empty() || target.front() == '-')
                throw std::invalid_argument("invalid build target");
    }
    namespace {
        auto project_path(std::string_view executable) -> bool { return executable.find_first_of("/\\") != std::string_view::npos; }
    } // namespace
    void tool_name(std::string_view name) {
        if (!toolchain::valid_name(name))
            throw std::invalid_argument("invalid tool name: " + std::string{name});
    }
    auto tools(const std::vector<std::string>& executables) -> requirements {
        requirements out;
        for (auto& executable : executables) {
            if (executable.empty() || executable.contains("${"))
                continue;
            if (builds::absolute_path(executable) || (executable.size() > 1 && executable[1] == ':'))
                throw std::invalid_argument("'" + executable + "' is a path on one machine; name the tool (such as '" +
                                            std::filesystem::path(executable).stem().string() + "') and map it in the tools of each worker and the studio");
            if (project_path(executable))
                continue;
            tool_name(executable);
            if (!std::ranges::contains(out.tools, executable))
                out.tools.push_back(executable);
        }
        return out;
    }
    auto in_project(const std::vector<std::string>& executables) -> bool {
        return std::ranges::any_of(executables, [](const std::string& executable) { return !absolute_path(executable) && project_path(executable); });
    }
    auto tool(const std::string& executable) -> std::string {
        if (executable.empty() || executable.contains("${") || project_path(executable))
            return executable;
        return "${tool:" + executable + "}";
    }
} // namespace builds
