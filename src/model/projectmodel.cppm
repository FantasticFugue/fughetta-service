export module projectmodel;
import std;
import json;
import content;
import toolchain;
export import provider;

export namespace projectmodel {
    struct object_ref {
        std::string hash, name;
    };
    struct project_reference {
        json::literal<"project"> kind;
        std::string project, unit, path;
        std::string role;
        std::map<std::string, std::string> attributes;
    };
    struct artifact_reference {
        json::literal<"artifact"> kind;
        object_ref object;
        std::string role;
    };
    struct sdk_reference {
        json::literal<"sdk"> kind;
        std::string name, path, role;
    };
    struct tree_reference {
        json::literal<"tree"> kind;
        std::string path, role;
    };
    using reference = json::tagged<"kind", project_reference, artifact_reference, sdk_reference, tree_reference>;
    struct unit {
        std::string id, project, language, directory;
        // Source order is significant for F#. Generated text is a content object,
        // while its key keeps the logical source path in the build tree.
        std::vector<std::string> sources;
        std::map<std::string, object_ref> generated;
        std::vector<reference> references;
        std::vector<std::string> outputs, compiler_arguments;
        std::vector<std::map<std::string, std::string>> variants;
        std::map<std::string, json::value> extensions;
    };
    struct document {
        std::uint32_t schema = 1;
        std::string producer, producer_version;
        std::vector<unit> units;
    };
    inline void check(const object_ref& object) {
        if (!content::is_hash(object.hash) || object.name.empty() || object.name.find_first_of("/\\") != std::string::npos)
            throw std::invalid_argument("invalid project model artifact reference");
    }
    inline auto resolve(const document& model, const project_reference& ref) -> const unit& {
        const unit* selected = nullptr;
        for (const auto& unit : model.units) {
            if (unit.project != ref.project)
                continue;
            const bool matches = ref.unit.empty() ? (!ref.attributes.empty() && std::ranges::contains(unit.variants, ref.attributes)) : unit.id == ref.unit;
            if (!matches)
                continue;
            if (selected)
                throw std::invalid_argument("ambiguous project model reference: " + ref.project);
            selected = &unit;
        }
        if (!selected)
            throw std::invalid_argument("project model reference requires a matching unit or variant: " + ref.project);
        return *selected;
    }
    inline auto read(std::string_view text) -> document {
        auto model = json::read<document>(text).value();
        if (model.schema != 1)
            throw std::invalid_argument("unknown project model schema");
        if (model.producer.empty() || model.producer_version.empty() || model.units.empty())
            throw std::invalid_argument("project model requires producer and units");
        std::set<std::string> ids;
        for (const auto& unit : model.units) {
            if (unit.id.empty() || unit.project.empty() || unit.language.empty() || !ids.insert(unit.id).second)
                throw std::invalid_argument("invalid or duplicate project model unit");
            builds::relative(unit.directory, true);
            for (const auto& source : unit.sources)
                builds::relative(source);
            for (const auto& output : unit.outputs)
                builds::relative(output);
            for (const auto& [path, object] : unit.generated) {
                if (!std::ranges::contains(unit.sources, path))
                    throw std::invalid_argument("generated input is not a model source");
                check(object);
            }
            for (const auto& ref : unit.references)
                ref.visit([](const auto& value) {
                    using T = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<T, artifact_reference>)
                        check(value.object);
                    else if constexpr (std::same_as<T, sdk_reference>) {
                        if (value.name.empty())
                            throw std::invalid_argument("unnamed model SDK");
                        builds::relative(value.path, true);
                    } else if constexpr (std::same_as<T, tree_reference>) {
                        builds::relative(value.path);
                    } else {
                        if (value.project.empty())
                            throw std::invalid_argument("unnamed model project reference");
                        builds::relative(value.path, true);
                    }
                });
        }
        // Exporters classify project outputs by component identity, including
        // jars and reference assemblies not listed here. This is a final check
        // for tree references under the outputs the model declares.
        std::set<std::filesystem::path> outputs;
        for (const auto& unit : model.units)
            for (const auto& path : unit.outputs)
                outputs.insert(std::filesystem::path{path}.lexically_normal());
        for (const auto& unit : model.units)
            for (const auto& ref : unit.references)
                ref.visit([&](const auto& value) {
                    using T = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<T, project_reference>)
                        (void)resolve(model, value);
                    else if constexpr (std::same_as<T, tree_reference>)
                        for (auto path = std::filesystem::path{value.path}.lexically_normal(); !path.empty(); path = path.parent_path())
                            if (outputs.contains(path))
                                throw std::invalid_argument("tree reference names a project output: " + value.path);
                });
        return model;
    }
    inline auto objects(const document& model) -> std::set<std::string> {
        std::set<std::string> result;
        for (const auto& unit : model.units) {
            for (const auto& [_, ref] : unit.generated)
                result.insert(ref.hash);
            for (const auto& ref : unit.references)
                ref.visit([&](const auto& value) {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, artifact_reference>)
                        result.insert(value.object.hash);
                });
        }
        return result;
    }
    // Whether a compiler reading the model loads code the model names: compiler plugins, analyzers or type providers.
    // KGP puts Kotlin's scripting registrar and the libraries it needs on every compilation's plugin classpath; the
    // analysis host loads it like the compiler does, so it runs no code the project chose.
    inline auto builtin_plugin(std::string_view name) -> bool {
        constexpr std::array<std::string_view, 6> prefixes{"kotlin-scripting-", "kotlin-script-runtime-",  "kotlin-stdlib",
                                                           "kotlin-reflect-",   "kotlinx-coroutines-core", "annotations-"};
        return name.ends_with(".jar") && std::ranges::any_of(prefixes, [&](std::string_view prefix) { return name.starts_with(prefix); });
    }
    inline auto loads_code(const document& model) -> bool {
        // `-Xplugin=a.jar,b.jar` and `-Xcompiler-plugin=a.jar,b.jar=option=value`.
        auto passed = [](std::string_view argument) {
            for (std::string_view flag : {"-Xplugin=", "-Xcompiler-plugin="})
                if (argument.starts_with(flag)) {
                    auto paths = argument.substr(flag.size());
                    paths = paths.substr(0, paths.find('='));
                    return std::ranges::any_of(paths | std::views::split(','),
                                               [](auto path) { return !builtin_plugin(std::filesystem::path{std::string_view{path}}.filename().string()); });
                }
            return false;
        };
        auto csharp_analyzer = [](std::string_view argument) {
            const auto begin = argument.find_first_not_of(" \t\"");
            if (begin == argument.npos)
                return false;
            argument.remove_prefix(begin);
            if (argument.starts_with('@'))
                return true;
            if (argument.empty() || (argument.front() != '/' && argument.front() != '-'))
                return false;
            auto option = std::string{argument.substr(1, argument.find(':') - 1)};
            std::erase(option, '"');
            std::ranges::transform(option, option.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return option == "analyzer" || option == "a";
        };
        return std::ranges::any_of(model.units, [&](const unit& unit) {
            if (const auto found = unit.extensions.find("type_providers"); found != unit.extensions.end() && found->second.to_bool().value_or(false))
                return true;
            if (std::ranges::any_of(unit.compiler_arguments, passed) ||
                (unit.language == "csharp" && std::ranges::any_of(unit.compiler_arguments, csharp_analyzer)))
                return true;
            return std::ranges::any_of(unit.references, [csharp = unit.language == "csharp"](const reference& ref) {
                return ref.visit([csharp](const auto& value) {
                    if (value.role != "plugin")
                        return false;
                    if (csharp)
                        return true;
                    if constexpr (requires { value.object; })
                        return !builtin_plugin(value.object.name);
                    return true;
                });
            });
        });
    }
    inline auto object_path(const std::filesystem::path& objects, const object_ref& ref) -> std::filesystem::path {
        check(ref);
        return objects / ref.hash;
    }
    inline auto sdk_path(std::span<const toolchain::binding> bindings, const sdk_reference& ref) -> std::filesystem::path {
        builds::relative(ref.path, true);
        auto found = std::ranges::find_if(bindings, [&](const toolchain::binding& binding) { return binding.name == ref.name; });
        if (found == bindings.end())
            throw std::runtime_error("project model requires SDK mapping: " + ref.name);
        for (const auto& root : found->roots) {
            auto path = std::filesystem::path{root} / ref.path;
            if (std::filesystem::exists(path))
                return path;
        }
        throw std::runtime_error("mapped SDK has no model input: " + ref.name + "/" + ref.path);
    }
    inline void installed(const document& model, const std::filesystem::path& objects, std::span<const toolchain::binding> bindings) {
        auto object = [&](const object_ref& ref) {
            if (!std::filesystem::is_regular_file(object_path(objects, ref)))
                throw std::runtime_error("missing project model object: " + ref.hash);
        };
        for (const auto& unit : model.units) {
            for (const auto& [_, ref] : unit.generated)
                object(ref);
            for (const auto& ref : unit.references)
                ref.visit([&](const auto& value) {
                    using T = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<T, artifact_reference>)
                        object(value.object);
                    else if constexpr (std::same_as<T, sdk_reference>)
                        (void)sdk_path(bindings, value);
                });
        }
    }
    struct format {
        static constexpr std::string_view kind = "file";
        static constexpr std::uint64_t max_bytes = 64u << 20;
        static auto check(std::string_view text) -> std::uint64_t { return read(text).units.size(); }
        static void installed(std::string_view text, const std::filesystem::path& objects, std::span<const toolchain::binding> bindings) {
            projectmodel::installed(read(text), objects, bindings);
        }
    };
} // namespace projectmodel
namespace builds {
    template <> struct format<"project_model"> : projectmodel::format {};
} // namespace builds
