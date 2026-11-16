export module profiles;
import std;
import native;
import async;
import meta;
import reflection;
import content;
import records;
export import evaluation;
export import helper;
import toolchain;
export import build;
export import clice;
export import artifact;
export import jvm;
export import clr;
export import pinvoke;
export import jni;
export import typescript;
export import ty;
export import ruff;
export import kotlin;
export import fsharp;

export namespace language {
    template <class Implementation> auto port_type(std::string_view port) -> std::string_view {
        if constexpr (requires { Implementation::input_type(port); })
            return Implementation::input_type(port);
        else
            return port;
    }

    // The language server of an analysis: a provider by name and that provider's options.
    template <meta::fixed_string Name> struct with {
        using implementation = server<Name>;
        static constexpr std::string_view name = Name.view();
        builds::literal<Name> provider;
        config<Name> options;
    };

    // The language servers this program knows; an analysis names one of them or is refused when parsed.
    template <meta::fixed_string... Names> struct catalog {
        static_assert(((frontend<server<Names>> + domain_reader<server<Names>> + bridge<server<Names>> == 1) && ...));
        static_assert(meta::distinct(std::array{std::string_view{Names}...}));
        using choice = rfl::TaggedUnion<"provider", with<Names>...>;
        static auto validators() -> const std::vector<input_validator>& {
            static const auto registry = [] {
                std::vector<input_validator> out;
                auto add = [&]<meta::fixed_string Name>() {
                    for (auto validator : language::input_validators<server<Name>>()) {
                        if (std::ranges::any_of(out, [&](const input_validator& known) { return known.type == validator.type; }))
                            throw std::invalid_argument("duplicate provider input validator: " + std::string{validator.type});
                        out.push_back(std::move(validator));
                    }
                };
                (add.template operator()<Names>(), ...);
                return out;
            }();
            return registry;
        }
        template <class Visitor> static auto dispatch(std::string_view provider, Visitor&& visitor) {
            using first = std::tuple_element_t<0, std::tuple<server<Names>...>>;
            using result = decltype(visitor.template operator()<first>());
            using entry = std::pair<std::string_view, result (*)(std::remove_reference_t<Visitor>&)>;
            static constexpr std::array<entry, sizeof...(Names)> table{
                {{Names.view(), +[](std::remove_reference_t<Visitor>& call) -> result { return call.template operator()<server<Names>>(); }}...}};
            const auto found = std::ranges::find(table, provider, &entry::first);
            if (found == table.end())
                throw std::invalid_argument("unknown analysis provider: " + std::string{provider});
            return found->second(visitor);
        }
        static auto describe() -> std::vector<description> {
            std::vector<description> out;
            auto add = [&]<meta::fixed_string Name>() {
                using implementation = server<Name>;
                typename implementation::options options;
                description value{.id = std::string{Name.view()}, .roles = {std::string{role<implementation>}}};
                if constexpr (frontend<implementation>)
                    value.emits = emitted<implementation>();
                if constexpr (domain_reader<implementation>)
                    value.reads = std::string{implementation::domain::name};
                value.configuration_schema = rfl::json::read<rfl::Generic>(rfl::json::to_schema<typename implementation::options>()).value();
                for (const auto name : implementation::facts)
                    value.facts.emplace_back(std::string{name});
                value.channels = implementation::channels();
                value.operations = language::operations<implementation>();
                value.unit_kind = implementation::unit_kind;
                value.adapter = implementation::adapter::transport;
                if constexpr (requires { implementation::update_granularity; })
                    value.update_granularity = implementation::update_granularity;
                value.needs = implementation::requirements(options);
                value.executes_project_code = implementation::executes_project_code(options);
                if constexpr (requires { implementation::negotiated; })
                    value.negotiated = implementation::negotiated;
                if constexpr (requires { implementation::dependency_reads; })
                    value.dependency_reads = implementation::dependency_reads;
                value.ports.push_back({"source", "source_snapshot", "input", true});
                for (auto input : implementation::inputs) {
                    bool required = true, multiple = false;
                    if constexpr (requires { implementation::required_inputs(options); })
                        required = std::ranges::contains(implementation::required_inputs(options), input);
                    if constexpr (requires { implementation::multiple_inputs; })
                        multiple = implementation::multiple_inputs;
                    if constexpr (requires { implementation::multiple_input(input); })
                        multiple = implementation::multiple_input(input);
                    value.ports.push_back({std::string{input}, std::string{port_type<implementation>(input)}, "input", required, multiple});
                    if constexpr (requires { implementation::consumed_facts(input); })
                        value.ports.back().facts = implementation::consumed_facts(input);
                }
                value.ports.push_back({"facts", "semantic_shard", "output", true, true});
                for (const auto& [suffix, language] : implementation::documents)
                    if (!std::ranges::contains(value.languages, language))
                        value.languages.emplace_back(language);
                out.push_back(std::move(value));
            };
            (add.template operator()<Names>(), ...);
            return out;
        }
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
    using providers =
        catalog<"clice", "typescript", "ty", "ruff", "native-artifact", "jvm-artifact", "clr-artifact", "pinvoke", "jni", "kotlin", "fsharp", "external">;
    using provider = providers::choice;
    inline auto describe_provider(const provider& configured) -> description {
        return configured.visit([](const auto& value) -> description {
            using implementation = typename std::remove_cvref_t<decltype(value)>::implementation;
            if constexpr (requires { implementation::registration(value.options); }) {
                auto out = implementation::registration(value.options);
                // A registered helper is a frontend unless it names the domain it reads.
                out.roles = {std::string{out.reads ? roles::reader : roles::frontend}};
                out.adapter = adapter_kind::helper;
                out.needs = implementation::requirements(value.options);
                out.executes_project_code = implementation::executes_project_code(value.options);
                for (const auto& [_, language] : value.options.documents)
                    if (!std::ranges::contains(out.languages, language))
                        out.languages.push_back(language);
                return out;
            } else {
                const auto entries = providers::describe();
                auto out = *std::ranges::find_if(entries, [](const description& entry) { return entry.id == std::remove_cvref_t<decltype(value)>::name; });
                out.needs = implementation::requirements(value.options);
                out.executes_project_code = implementation::executes_project_code(value.options);
                if constexpr (requires { implementation::required_inputs(value.options); }) {
                    const auto required = implementation::required_inputs(value.options);
                    for (auto& port : out.ports)
                        if (port.direction == "input" && port.type != "source_snapshot")
                            port.required = std::ranges::contains(required, port.name);
                }
                return out;
            }
        });
    }
    inline auto make_adapter(native::scheduler scheduler, io::command command, session_state identity,
                             std::map<std::string, std::vector<std::string>> commands = {}, toolchain::identity runtime = {}, document_loader loader = {},
                             std::optional<description> registration = {}, object_loader objects = {}) -> std::unique_ptr<adapter> {
        if (registration) {
            if (registration->id != identity.provider || registration->adapter != adapter_kind::helper)
                throw std::invalid_argument("external provider registration does not describe its session");
            auto result = std::make_unique<helper_adapter>(scheduler, std::move(command), std::move(identity), std::move(commands), std::move(runtime),
                                                           std::move(loader));
            result->dependency_reads(registration->dependency_reads);
            result->objects(std::move(objects));
            return result;
        }
        return providers::dispatch(identity.provider, [&]<class Implementation>() -> std::unique_ptr<adapter> {
            auto result = std::make_unique<typename Implementation::adapter>(scheduler, std::move(command), std::move(identity), std::move(commands),
                                                                             std::move(runtime), std::move(loader));
            if constexpr (std::same_as<typename Implementation::adapter, helper_adapter> && requires { Implementation::dependency_reads; })
                result->dependency_reads(Implementation::dependency_reads);
            result->objects(std::move(objects));
            return result;
        });
    }

    struct execution {
        rfl::Rename<"requires", builds::requirements> needs;
        builds::limits limits;
        semantic::budget budget;
        std::optional<std::string> directory_group;
    };
    struct defaults {
        std::optional<std::uint64_t> fact_limit, relation_depth;
        language::execution execution;
    };
    struct build_input {
        std::string configuration, task, output;
    };
    struct reference {
        std::string stage, output;
    };
    // A semantic output in another Fughetta project. A call fixes its version
    // before preparing any consumer; continuation retains that same selection.
    struct project_input {
        std::string project, worldline, configuration, output;
        std::optional<std::string> version;
    };
    using input = std::variant<build_input, reference, project_input>;
    struct stage {
        provider server;
        std::string build_configuration;
        std::map<std::string, std::vector<language::input>> inputs;
        std::vector<std::string> after;
        language::execution execution;
    };
    struct configuration {
        language::defaults defaults;
        std::map<std::string, stage> stages;
        std::map<std::string, std::vector<reference>> outputs;
    };
    struct dependency_computation {
        configuration profile;
        std::map<std::string, builds::configuration> builds;
        std::map<std::string, std::string> toolchains;
        std::string project;
    };
    struct computation {
        configuration profile;
        std::map<std::string, builds::configuration> builds;
        std::map<std::string, std::string> toolchains;
        std::string project;
        // Flattened by explicit project/worldline/configuration/output selection.
        std::map<std::string, dependency_computation> dependencies;
    };
} // namespace language
