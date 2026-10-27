export module provider;
import std;
import meta;
import reflection;
import content;

// What every build system and language server shares, and the templates each one specializes by name. The set of
// providers is closed when the program is compiled; the build and language modules list them.
export namespace builds {
    using environment = std::map<std::string, std::string>;
    using toolset = std::map<std::string, std::string>;
    struct requirements {
        std::vector<std::string> tools;
        struct platform {
            std::optional<std::string> os;
            std::optional<std::string> arch;
            friend auto operator==(const platform&, const platform&) -> bool = default;
        } platform;
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
        friend auto operator==(const report&, const report&) -> bool = default;
    };
    // `${tree}` is the build tree, `${tool:name}` a mapped tool, `${jobs}` the parallel jobs the runner allows and `${reports}` the
    // directory, empty when the step starts, that its reports go to. standard_output names a report file that receives the step's
    // standard output, for a tool that prints its report there; the step's output is then its standard error alone.
    struct step {
        std::string name;
        std::vector<std::string> argv;
        std::string cwd;
        environment env;
        std::vector<context> outputs;
        std::vector<report> reports;
        std::optional<std::string> standard_output;
    };
    struct artifact {
        std::string type;
        std::string path;
        std::string kind = "file";
    };
    struct reference {
        std::string task;
        std::string output;
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
    struct plan {
        std::vector<step> steps;
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
    struct tally {
        std::uint32_t total = 0;
        std::uint32_t failed = 0;
        std::vector<std::string> failed_tests;
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

    template <meta::fixed_string Name> using literal = rfl::Literal<rfl::internal::StringLiteral<sizeof(Name.chars)>{Name.chars}>;

    void relative(std::string_view value, bool dot = false);
    void validate_environment(const environment& env);
    void validate(const requirements& needs);
    void combine(requirements& into, const requirements& from);
    auto tools(const std::vector<std::string>& executables) -> requirements;
    auto tool(const std::string& executable) -> std::string;
    auto requested(const parameters& p) -> std::set<std::string>;
    void require(const operation_spec& available, const parameters& requested);
    void tool_name(std::string_view name);
} // namespace builds

export namespace language {
    struct program {
        std::string provider;
        std::vector<std::string> argv;
        builds::environment env;
        // Sent as the initialize request's initializationOptions.
        rfl::Generic initialization;
    };

    // A language server: `options` is what an analysis configures it with, `documents` maps file extensions to the
    // language IDs it is told, `inputs` names the artifact types it reads, and `launch` starts it on them.
    template <meta::fixed_string Name> struct server;
    template <meta::fixed_string Name> using config = typename server<Name>::options;
    template <class T>
    concept speaker = requires(const typename T::options& options, const std::vector<builds::context>& inputs) {
        { T::validate(options) } -> std::same_as<void>;
        { T::launch(options, inputs) } -> std::same_as<program>;
        { T::documents.size() } -> std::convertible_to<std::size_t>;
        { T::inputs.size() } -> std::convertible_to<std::size_t>;
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
        for (auto& tool : needs.tools)
            if (tool.empty())
                throw std::invalid_argument("requires.tools contains an empty name");
        if ((needs.platform.os && needs.platform.os->empty()) || (needs.platform.arch && needs.platform.arch->empty()))
            throw std::invalid_argument("platform requirement is empty");
    }
    void combine(requirements& into, const requirements& from) {
        validate(from);
        for (auto& tool : from.tools)
            if (!std::ranges::contains(into.tools, tool))
                into.tools.push_back(tool);
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
        if (name.empty() || !std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '+'; }))
            throw std::invalid_argument("invalid tool name: " + std::string{name});
    }
    auto tools(const std::vector<std::string>& executables) -> requirements {
        requirements out;
        for (auto& executable : executables) {
            if (executable.empty() || executable.contains("${"))
                continue;
            if (std::filesystem::path(executable).is_absolute() || (executable.size() > 1 && executable[1] == ':'))
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
    auto tool(const std::string& executable) -> std::string {
        if (executable.empty() || executable.contains("${") || project_path(executable))
            return executable;
        return "${tool:" + executable + "}";
    }
} // namespace builds
