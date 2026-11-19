export module clice;
import evaluation;
import coroutine;
import lsp;
import runner;
import constants;
import toolchain;
import std;
import meta;
import json;
import records;
import enumeration;
export import provider;

export namespace language {
    class clice_adapter;
}
namespace language {
    // The clice C and C++ language server. It is started as `clice serve [args]` in the project directory, which it takes as its
    // workspace, and is told through initializationOptions what clice.toml documents: `rules`, holding the compile_commands of the
    // build outputs and flags appended to or removed from their commands, and `project.cache_dir`. Rule paths are relative to the
    // project, and the server resolves them from the prepared tree, which a worker may hold elsewhere. The cache holds the PCH, PCM
    // and index of a session and the logs beneath it; the worker keeps it in the session's own directory, outside the tree.
    template <> struct server<"clice"> {
        static auto conventions() -> std::vector<semantic::language_conventions> {
            return {
                {.language = "cpp",
                 .visibility = {"public", "protected", "private", "local"},
                 .value_wrappers = {"std::optional", "std::expected", "std::unique_ptr", "std::shared_ptr"},
                 .wrapper_arguments = "<",
                 .scope_edges = {"base", "using_namespace", "using_declaration", "using_enum", "inline_namespace", "anonymous_namespace", "unscoped_enum"},
                 .relation_traits = {{"cpp:reinterpret_cast", "unsafe_cast"}, {"cpp:const_cast", "unsafe_cast"}, {"cpp:unsafe_c_style_cast", "unsafe_cast"}},
                 .escape_types = {{"void*", "std::any"}}}};
        }
        static auto channels() -> std::vector<semantic::channel_definition> {
            auto out = source_channels();
            out.front().projections = {std::string{"declarations"}};
            out.push_back(linkage_channel());
            return out;
        }
        static auto input_validators() -> std::vector<input_validator> {
            return {{"clice/configuration",
                     [](const semantic::input_object& required, std::span<const semantic::input_object> installed) {
                         return !required.object.empty() && std::ranges::contains(installed, required);
                     }},
                    {"clice/contribution", [](const semantic::input_object& required, std::span<const semantic::input_object> installed) {
                         return !required.object.empty() && std::ranges::contains(installed, required);
                     }}};
        }
        using adapter = clice_adapter;
        struct options {
            // `clice`, or the path of a clice build inside the project; elsewhere, map 'clice' in the tools of each worker and the studio.
            std::string executable = "clice";
            // What follows `serve`.
            std::vector<std::string> args;
            builds::environment env;
            // A rule's compile-time flags, appended to every compilation command.
            std::vector<std::string> append;
            // A rule's compile-time flags, removed from every command.
            std::vector<std::string> remove;
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 16> documents{{
            {"c", "c"},
            {"cc", "cpp"},
            {"cpp", "cpp"},
            {"cxx", "cpp"},
            {"c++", "cpp"},
            {"h", "cpp"},
            {"hh", "cpp"},
            {"hpp", "cpp"},
            {"hxx", "cpp"},
            {"h++", "cpp"},
            {"ipp", "cpp"},
            {"tpp", "cpp"},
            {"cppm", "cpp"},
            {"ixx", "cpp"},
            {"mpp", "cpp"},
            {"cu", "cpp"},
        }};
        static constexpr std::array<std::string_view, 1> inputs{"compilation_database"};
        static constexpr std::array<std::string_view, 8> facts{"symbols", "types",       "definitions", "references",
                                                               "scopes",  "diagnostics", "relations",   "typed_ranges"};
        static constexpr std::array<std::string_view, 2> operations{"evaluate", "resolve"};
        static constexpr semantic::unit_kind unit_kind = semantic::unit_kind::group;
        static constexpr bool negotiated = false, dependency_reads = true, multiple_inputs = true;
        using emits = std::tuple<domain<"native">>;
        static auto requirements(const options& o) -> builds::requirements { return builds::tools({o.executable}); }
        // Toolchain discovery can run the driver named by the project's compilation database.
        static auto executes_project_code(const options&) -> bool { return true; }

        static void configure(program& value, std::uint32_t jobs, const std::filesystem::path& cache) {
            auto initialization = value.initialization.to_object().value_or(json::object{});
            json::object project;
            project["cache_dir"] = (cache / "clice").generic_string();
            project["stateless_worker_count"] = std::int64_t{std::max(1u, jobs)};
            project["min_stateless_worker_count"] = std::int64_t{1};
            project["max_stateless_worker_count"] = std::int64_t{std::max(1u, jobs)};
            initialization["project"] = std::move(project);
            value.initialization = std::move(initialization);
        }

        static void validate(const options& o) {
            if (o.executable.empty() || o.executable.find('\0') != std::string::npos)
                throw std::invalid_argument("clice executable is required");
            if (!builds::absolute_path(o.executable))
                builds::tools({o.executable});
            builds::validate_environment(o.env);
            for (auto& arg : o.args)
                if (arg.empty() || arg == "--" || arg.find('\0') != std::string::npos || arg.contains("${"))
                    throw std::invalid_argument("clice argument is empty or a step reference");
            for (auto& flag : o.append) {
                if (flag.empty() || flag.contains("${") || flag.find('\0') != std::string::npos)
                    throw std::invalid_argument("clice appended flag is empty or a step reference");
            }
            for (auto& flag : o.remove)
                if (flag.empty() || flag.contains("${") || flag.find('\0') != std::string::npos)
                    throw std::invalid_argument("clice removed flag is empty or a step reference");
        }
        static auto launch(const options& o, const std::vector<builds::context>& databases) -> program {
            if (databases.empty())
                throw std::invalid_argument("clice requires a compilation database to read");
            program out{.argv = {o.executable, "serve"}, .env = o.env};
            out.argv.insert(out.argv.end(), o.args.begin(), o.args.end());
            json::array paths;
            for (auto& database : databases)
                paths.push_back(database.path);
            json::object rule, init;
            rule["compile_commands"] = std::move(paths);
            if (!o.append.empty()) {
                json::array flags;
                for (auto& flag : o.append)
                    flags.push_back(flag);
                rule["append"] = std::move(flags);
            }
            if (!o.remove.empty()) {
                json::array flags;
                for (auto& flag : o.remove)
                    flags.push_back(flag);
                rule["remove"] = std::move(flags);
            }
            init["rules"] = json::array{std::move(rule)};
            out.initialization = std::move(init);
            out.commands = {{"query", {o.executable, "query"}}};
            return out;
        }
    };
    static_assert(frontend<server<"clice">>);
} // namespace language

export namespace language::cpp {
    struct range {
        std::string path;
        std::uint64_t begin = 0, end = 0;
    };
    struct argument {
        std::string kind, value;
        std::optional<std::string> type, declaration;
        std::vector<argument> elements;
    };
    struct type {
        std::string id, kind, name;
        std::optional<std::string> declaration, binder, return_type;
        std::optional<std::uint64_t> parameter_index, array_size;
        std::vector<std::string> arguments, parameters, qualifiers, type_parameters;
        std::vector<argument> template_arguments;
    };
    struct parameter {
        std::uint64_t group = 0, index = 0;
        std::optional<std::string> name, default_value;
        std::vector<std::string> attributes;
    };
    struct symbol {
        std::string id, name, qualified_name, kind;
        std::optional<std::string> owner, type;
        std::vector<std::string> modifiers;
        std::optional<std::string> declaring_scope;
        std::string visibility;
        std::vector<parameter> parameters;
        std::vector<std::string> attributes;
        std::optional<std::string> constant_digest, definition_digest;
        std::map<std::string, std::string> extensions;
    };
    struct occurrence {
        std::optional<std::string> symbol;
        std::string role;
        range location;
        std::optional<range> spelling, expansion, extent;
        std::optional<std::string> owner;
        std::vector<std::string> candidates;
        std::string resolution = "resolved";
        std::optional<std::string> dispatch;
        std::vector<semantic::role> roles;
        std::optional<std::string> declaration_kind;
    };
    struct scope {
        std::string id;
        std::optional<std::string> parent, owner;
        range location;
        std::vector<std::string> declared_members;
        std::vector<semantic::records::scope_edge> lookup;
        std::string kind;
    };
    struct relation {
        std::string from, to, kind;
        std::optional<range> location;
        std::string resolution = "resolved";
        std::optional<std::string> dispatch;
        std::optional<std::string> fingerprint;
    };
    struct diagnostic {
        std::string code, severity, message;
        std::optional<range> location;
    };
    struct typed_range {
        range range;
        std::string type;
        std::optional<std::string> overload;
        std::map<std::string, std::string> substitutions;
    };
    struct configuration_reads {
        std::map<std::string, std::string> commands;
        bool complete = false;
    };

    struct tool_reads {
        std::vector<std::string> executables;
        bool complete = false;
    };
    struct publication {
        std::string column_encoding;
        bool complete = true;
        std::vector<std::string> dependencies, absent, directories;
        tool_reads tools;
        configuration_reads configurations;
        std::vector<symbol> symbols;
        std::vector<type> types;
        std::vector<occurrence> occurrences;
        std::vector<scope> scopes;
        std::vector<relation> relations;
        std::vector<diagnostic> diagnostics;
        std::vector<typed_range> typed_ranges;
        std::vector<std::string> identities;
    };
    struct contribution_key {
        std::string translation_unit, contribution;
    };
    struct contribution_info {
        std::string key, kind, file, value;
    };

    inline auto lower(std::string value) -> std::string {
        for (auto& c : value)
            c = std::tolower(static_cast<unsigned char>(c));
        return value;
    }
    inline auto kind(std::string value) -> std::string {
        value = lower(std::move(value));
        if (value == "struct" || value == "class" || value == "enum" || value == "union")
            return "type";
        if (value == "enummember")
            return "enum_member";
        if (value == "invalid" || value.empty())
            return "other";
        return value;
    }
    // A callable declaration's body is a function scope.
    inline auto scope_kind(std::string value) -> std::string {
        value = kind(std::move(value));
        return value == "method" || value == "constructor" || value == "destructor" || value == "operator" ? "function" : value;
    }
    // The adapter receives the exact text installed in the worker slot. Positions cross
    // the provider boundary once, from UTF-8 bytes to source Unicode scalar offsets.
    inline auto translate(const publication& input, const std::filesystem::path& root, const std::map<std::string, std::string>& texts,
                          const std::string& project, const std::string& options, const std::string& stage, const std::string& path,
                          const std::vector<semantic::input_object>& inputs, const std::map<std::string, std::string>& source_files,
                          std::span<const toolchain::binding> external = {}, const std::map<std::string, std::string>& executables = {},
                          bool scoped_configurations = false) -> semantic::shard {
        if (input.column_encoding != "utf8")
            throw std::invalid_argument("clice export requires UTF-8 byte ranges");
        semantic::shard out;
        semantic::recipe recipe{{project, stage, "clice", "evaluate", semantic::unit_kind::file, path}, options};
        out.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe, path));
        out.unit.modules = {project};
        const auto main = source_files.find(path);
        if (main == source_files.end())
            throw std::invalid_argument("clice translation unit has no installed file object");
        out.unit.direct_inputs.push_back({semantic::input_kind::file, main->first, main->second});
        for (const auto& value : inputs)
            if ((!scoped_configurations && value.kind == semantic::input_kind::artifact) || value.kind == semantic::input_kind::runtime)
                out.unit.direct_inputs.push_back(value);
        if (scoped_configurations)
            for (const auto& [file, value] : input.configurations.commands)
                out.unit.direct_inputs.push_back({semantic::input_kind::provider, file, value, "clice/configuration"});
        out.unit.cacheable = input.tools.complete;
        std::map<std::string, std::string> selected_tools;
        for (const auto& executable : input.tools.executables) {
            const auto name = std::filesystem::path{executable}.lexically_normal().generic_string();
            const auto found = executables.find(name);
            auto& object = selected_tools[name];
            if (found != executables.end())
                object = found->second;
            out.unit.cacheable = out.unit.cacheable && !object.empty();
        }
        for (const auto& [name, object] : selected_tools)
            out.unit.direct_inputs.push_back({semantic::input_kind::tool, name, object});
        auto own = [&] {
            return semantic::ownership{.owner_unit = out.unit.id, .context = out.unit.context, .origin = {.provider = "clice", .kind = std::string{"source"}}};
        };
        auto relative = [&](const std::string& name) {
            return std::filesystem::path(name).is_absolute() ? std::filesystem::path(name).lexically_relative(root).generic_string() : name;
        };
        auto locate = [&](const range& range) {
            semantic::locator result;
            auto name = relative(range.path);
            auto found = texts.find(name);
            if (found == texts.end()) {
                if (const auto member = toolchain::locate(external, root / range.path))
                    result.artifact = semantic::artifact_location{
                        .artifact = member->input->object, .module = member->input->name, .member = member->path, .offset = range.begin};
                else
                    result.artifact = semantic::artifact_location{.artifact = range.path, .module = project, .offset = range.begin};
                return result;
            }
            const auto& text = found->second;
            if (range.end > text.size() || range.begin > range.end)
                throw std::invalid_argument("clice range exceeds its input text");
            auto scalar = [&](std::uint64_t offset) {
                std::uint64_t count = 0;
                for (std::uint64_t byte = 0; byte < offset; ++byte)
                    if ((static_cast<unsigned char>(text[byte]) & 0xc0) != 0x80)
                        ++count;
                return count;
            };
            result.source = semantic::place{name, scalar(range.begin), scalar(range.end)};
            return result;
        };
        auto type_id = [&](const std::string& id) { return out.unit.id + ":type:" + id; };
        auto application_argument = [&](this auto&& self, const argument& value) -> semantic::records::application_argument {
            semantic::records::application_argument result{.kind = value.kind, .value = value.value, .declaration = value.declaration};
            if (value.type)
                result.type = type_id(*value.type);
            for (const auto& element : value.elements)
                result.elements.push_back(self(element));
            return result;
        };
        std::set<std::string> identities(input.identities.begin(), input.identities.end());
        for (const auto& symbol : input.symbols)
            identities.insert(symbol.id);
        for (const auto& id : identities)
            out.identities.push_back({semantic::native_identity{project, project, "clice", "source", id}, id, {}});
        for (const auto& symbol : input.symbols) {
            auto identity = semantic::native_identity{project, project, "clice", "source", symbol.id};
            const auto module = symbol.extensions.find("cpp.module");
            out.symbols.push_back({.id = symbol.id,
                                   .ownership = own(),
                                   .language = "cpp",
                                   .domain = "source",
                                   .module = module == symbol.extensions.end() ? project : module->second,
                                   .name = symbol.name,
                                   .qualified_name = symbol.qualified_name,
                                   .kind = kind(symbol.kind),
                                   .owner = symbol.owner,
                                   .declaring_scope = symbol.declaring_scope,
                                   .type = symbol.type ? std::optional{type_id(*symbol.type)} : std::nullopt,
                                   .visibility = symbol.visibility,
                                   .modifiers = symbol.modifiers,
                                   .native_key = identity});
            auto annotations = [](const std::vector<std::string>& attributes) {
                semantic::extensions result;
                if (!attributes.empty()) {
                    json::array values;
                    for (const auto& attribute : attributes)
                        values.emplace_back(attribute);
                    result.emplace("cpp.attributes", std::move(values));
                }
                return result;
            };
            auto& record = out.symbols.back();
            record.extensions = annotations(symbol.attributes);
            if (const auto linkage = symbol.extensions.find("native.linkage_name"); linkage != symbol.extensions.end())
                record.extensions.emplace(linkage->first, linkage->second);
            if (symbol.visibility == "local")
                record.modifiers.push_back("local");
            if (symbol.constant_digest)
                record.extensions["cpp.constant_digest"] = *symbol.constant_digest;
            if (symbol.definition_digest)
                record.extensions["cpp.definition_digest"] = *symbol.definition_digest;
            for (const auto& parameter : symbol.parameters)
                record.parameters.push_back({.group = parameter.group,
                                             .index = parameter.index,
                                             .name = parameter.name,
                                             .default_value = parameter.default_value,
                                             .annotations = annotations(parameter.attributes)});
        }
        for (const auto& value : input.types) {
            semantic::records::type type{.id = type_id(value.id),
                                         .domain = "cpp",
                                         .kind = value.kind,
                                         .name = value.name,
                                         .declaration = value.declaration,
                                         .binder = value.binder,
                                         .parameter_index = value.parameter_index,
                                         .array_size = value.array_size,
                                         .qualifiers = value.qualifiers};
            for (const auto& argument : value.arguments)
                type.arguments.push_back(type_id(argument));
            for (const auto& parameter : value.type_parameters)
                type.type_parameters.push_back(type_id(parameter));
            if (value.return_type)
                type.return_type = type_id(*value.return_type);
            if (value.kind == "function") {
                type.parameter_groups.emplace_back();
                for (const auto& parameter : value.parameters)
                    type.parameter_groups.back().parameters.push_back({type_id(parameter), {}});
            }
            for (const auto& argument : value.template_arguments)
                type.application_arguments.push_back(application_argument(argument));
            out.types.push_back(std::move(type));
        }
        // A function's body is the first block its own declaration scope opens.
        std::map<std::string, range> bodies;
        std::map<std::string, const scope*> scopes;
        for (const auto& value : input.scopes)
            scopes.emplace(value.id, &value);
        for (const auto& value : input.scopes)
            if (value.kind == "block" && value.owner && value.parent)
                if (const auto parent = scopes.find(*value.parent);
                    parent != scopes.end() && parent->second->owner == value.owner && scope_kind(parent->second->kind) == "function")
                    bodies.try_emplace(*value.owner, value.location);
        std::vector<const occurrence*> reference_sources;
        std::vector<const occurrence*> declaration_sources;
        for (const auto& value : input.occurrences) {
            auto location = locate(value.location);
            if (value.spelling)
                location.spelling = locate(*value.spelling).source;
            if (value.expansion)
                location.expansion = locate(*value.expansion).source;
            if (value.role == "Declaration" || value.role == "Definition") {
                if (!value.symbol)
                    throw std::invalid_argument("clice declaration occurrence has no symbol");
                out.occurrences.push_back(
                    {.id = out.unit.id + ":occ:" + std::to_string(out.occurrences.size()),
                     .ownership = own(),
                     .symbol = *value.symbol,
                     .location = location,
                     .roles = {lower(value.role)},
                     .selection = location.source,
                     .body = value.role == "Definition" && bodies.contains(*value.symbol) ? locate(bodies.at(*value.symbol)).source
                             : value.extent                                               ? locate(*value.extent).source
                                                                                          : std::nullopt,
                     .declaration_kind = value.declaration_kind ? std::optional<semantic::symbol_kind>{kind(*value.declaration_kind)} : std::nullopt});
                declaration_sources.push_back(&value);
            } else {
                out.references.push_back({.id = out.unit.id + ":ref:" + std::to_string(out.references.size()),
                                          .ownership = own(),
                                          .from = value.owner.value_or(""),
                                          .location = location,
                                          .target = value.symbol,
                                          .candidates = value.candidates,
                                          .roles = value.roles,
                                          .resolution = value.resolution});
                if (value.symbol)
                    out.references.back().target_context = out.unit.context;
                if (value.dispatch)
                    out.references.back().dispatch = *value.dispatch;
                reference_sources.push_back(&value);
            }
        }
        for (const auto& value : input.scopes) {
            out.scopes.push_back({.id = value.id,
                                  .ownership = own(),
                                  .lexical_parent = value.parent,
                                  .semantic_owner = value.owner,
                                  .location = locate(value.location),
                                  .declared_members = value.declared_members,
                                  .lookup = value.lookup,
                                  .kind = scope_kind(value.kind)});
        }
        for (const auto& value : input.relations) {
            semantic::records::relation relation{.id = out.unit.id + ":edge:" + std::to_string(out.relations.size()),
                                                 .kind = value.kind,
                                                 .ownership = own(),
                                                 .from = value.from,
                                                 .to = value.to,
                                                 .resolution = value.resolution,
                                                 .fingerprint = value.fingerprint};
            if (value.location) {
                relation.ownership.origin.location = locate(*value.location);
                for (std::size_t i = 0; i < out.references.size(); ++i) {
                    auto& reference = out.references[i];
                    const auto& source = reference_sources[i]->location;
                    const bool target = value.resolution == "candidate" ? std::ranges::contains(reference.candidates, value.to) : reference.target == value.to;
                    if (reference.from == value.from && target && source.path == value.location->path && source.begin >= value.location->begin &&
                        source.end <= value.location->end) {
                        relation.reference = reference.id;
                        if (value.kind == "calls") {
                            if (!std::ranges::contains(reference.roles, std::string{"call"}))
                                reference.roles.push_back(std::string{"call"});
                            if (value.dispatch)
                                reference.dispatch = *value.dispatch;
                        }
                        break;
                    }
                }
                for (std::size_t i = 0; i < out.occurrences.size(); ++i) {
                    const auto& source = declaration_sources[i]->location;
                    if (out.occurrences[i].symbol == value.from && source.path == value.location->path && source.begin >= value.location->begin &&
                        source.end <= value.location->end) {
                        relation.occurrence = out.occurrences[i].id;
                        break;
                    }
                }
            }
            out.relations.push_back(std::move(relation));
        }
        for (const auto& value : input.diagnostics)
            out.diagnostics.push_back({.id = out.unit.id + ":diag:" + std::to_string(out.diagnostics.size()),
                                       .producer = "clice",
                                       .code = value.code,
                                       .severity = lower(value.severity),
                                       .message = value.message,
                                       .ownership = own(),
                                       .location = value.location ? locate(*value.location) : semantic::locator{}});
        for (const auto& value : input.typed_ranges) {
            auto location = locate(value.range).source;
            if (!location)
                continue;
            semantic::records::typed_range range{.ownership = own(), .range = std::move(*location), .type = type_id(value.type), .overload = value.overload};
            for (const auto& [parameter, argument] : value.substitutions)
                range.substitutions.emplace(type_id(parameter), type_id(argument));
            out.typed_ranges.push_back(std::move(range));
        }
        auto artifact_read = [&](const std::string& dependency) {
            const auto name = relative(dependency);
            semantic::dependency read{.kind = semantic::dependency_kind::artifact, .project = project, .key = name};
            const semantic::input_object* bound = nullptr;
            for (const auto& value : inputs) {
                if ((value.kind == semantic::input_kind::artifact || value.kind == semantic::input_kind::generated) &&
                    (value.name == name || name.starts_with(value.name + "/")) && (!bound || value.name.size() > bound->name.size()))
                    bound = &value;
            }
            if (bound) {
                if (bound->kind == semantic::input_kind::generated)
                    read.kind = semantic::dependency_kind::generated;
                read.value = bound->object;
                read.scope = bound->name;
            } else if (const auto member = toolchain::locate(external, root / dependency)) {
                read.value = member->input->object;
                read.scope = member->input->name;
                read.key = member->path;
            }
            return read;
        };
        for (const auto& dependency : input.dependencies) {
            if (const auto source = source_files.find(relative(dependency)); source != source_files.end())
                out.unit.observed_dependencies.push_back(
                    {.kind = semantic::dependency_kind::source, .project = project, .key = source->first, .value = source->second});
            else
                out.unit.observed_dependencies.push_back(artifact_read(dependency));
        }
        for (const auto& directory : input.directories) {
            auto read = artifact_read(directory);
            const auto path = std::filesystem::path{read.key}.lexically_normal();
            if (!read.scope && !path.is_absolute() && !path.empty() && *path.begin() != "..")
                read = enumeration::read(project, read.key, source_files, {.directories = {path.generic_string()}});
            out.unit.observed_dependencies.push_back(std::move(read));
        }
        for (const auto& absent : input.absent) {
            semantic::dependency read{.kind = semantic::dependency_kind::negative_lookup, .project = project, .key = relative(absent)};
            if (const auto member = toolchain::locate(external, root / absent)) {
                read.value = member->input->object;
                read.scope = member->input->name;
                read.key = member->path;
            }
            out.unit.observed_dependencies.push_back(std::move(read));
        }
        out.unit.coverage.requested.facts = {std::string{"symbols"}, std::string{"types"},       std::string{"definitions"}, std::string{"references"},
                                             std::string{"scopes"},  std::string{"diagnostics"}, std::string{"relations"},   std::string{"typed_ranges"}};
        out.unit.coverage.requested.contexts = {out.unit.context};
        if (input.complete)
            out.unit.coverage.available.push_back(out.unit.coverage.requested);
        else
            out.unit.coverage.missing.push_back({.scope = out.unit.coverage.requested, .reason = "clice compilation is incomplete"});
        return out;
    }
} // namespace language::cpp

export namespace language {
    class clice_adapter : public lsp_adapter {
    public:
        static constexpr adapter_kind transport = adapter_kind::clice;
        using lsp_adapter::lsp_adapter;
        auto deferred(const semantic::unit_key& key, const semantic::extent& demand, std::string reason = "semantic unit budget exhausted") const
            -> semantic::missing override {
            auto path = key.name;
            if (const auto found = indexed_parts_.find(key.name); found != indexed_parts_.end())
                path = found->second.first.translation_unit;
            else
                path = json::read<cpp::contribution_key>(key.name).value().translation_unit;
            semantic::extent scope{.files = {path}};
            for (const auto fact : server<"clice">::facts)
                scope.facts.push_back(std::string{fact});
            for (const auto& [file, owners] : indexed_owners_)
                if (file != path && std::ranges::contains(owners, path))
                    scope.files.push_back(file);
            return {.scope = std::move(scope), .units = {semantic::unit_identifier(key)}, .reason = std::move(reason), .deferred = true};
        }

    protected:
        auto do_open(json::value initialization) -> coroutine::task<> override {
            overlay_ = json::write(initialization);
            co_await lsp_adapter::do_open(std::move(initialization));
        }
        auto do_update(language::installation input, const std::vector<std::string>& changed) -> coroutine::task<> override {
            install(std::move(input));
            const auto& documents = installed_.documents;
            if (commands_.contains("query"))
                co_await server_.synchronize({}, changed);
            else
                co_await server_.synchronize(documents, changed);
        }
        auto prepare(const semantic::extent& demand) -> coroutine::task<> override {
            if (commands_.contains("query") && !(demand.facts.size() == 1 && demand.facts.front() == "diagnostics"))
                co_await index();
        }
        auto input_value(const semantic::input_object& input) const -> std::optional<std::string> override {
            if (input.kind == semantic::input_kind::provider && input.type == "clice/configuration" && indexed_epoch_ == state_.epoch)
                if (const auto found = indexed_configurations_.find(input.name); found != indexed_configurations_.end())
                    return found->second;
            if (input.kind == semantic::input_kind::provider && input.type == "clice/contribution" && indexed_epoch_ == state_.epoch)
                if (const auto found = indexed_parts_.find(input.name); found != indexed_parts_.end())
                    return found->second.second.value;
            return std::nullopt;
        }
        auto do_units(const semantic::extent& demand) -> coroutine::task<std::vector<semantic::unit_key>> override {
            const bool indexed = indexed_epoch_ == state_.epoch && !(demand.facts.size() == 1 && demand.facts.front() == "diagnostics");
            std::set<std::string> installed;
            for (const auto& document : installed_.documents)
                installed.insert(document.path);
            std::set<std::string> selected;
            for (const auto& path : demand.files) {
                bool owned = false;
                if (indexed)
                    if (auto owners = indexed_owners_.find(path); owners != indexed_owners_.end())
                        for (const auto& owner : owners->second)
                            if (indexed_units_.contains(owner) && installed.contains(owner)) {
                                selected.insert(owner);
                                owned = true;
                            }
                if (!owned || indexed_units_.contains(path))
                    selected.insert(path);
            }
            std::vector<semantic::unit_key> result;
            std::vector<std::string> paths;
            for (const auto& document : installed_.documents) {
                if (!demand.files.empty() && !selected.contains(document.path))
                    continue;
                const auto owners = indexed_owners_.find(document.path);
                const bool covered =
                    indexed && !indexed_units_.contains(document.path) && owners != indexed_owners_.end() &&
                    std::ranges::any_of(owners->second, [&](const auto& owner) { return indexed_units_.contains(owner) && installed.contains(owner); });
                if (!covered)
                    paths.push_back(document.path);
            }
            if (indexed && std::ranges::any_of(paths, [&](const auto& path) { return indexed_units_.contains(path) && !indexed_loaded_.contains(path); })) {
                struct inventory {
                    std::string path;
                    std::vector<cpp::contribution_info> contributions;
                };
                std::vector<std::string> arguments{"semantic-contributions"};
                if (demand.files.empty())
                    arguments.push_back("--all");
                else
                    for (const auto& path : paths)
                        if (indexed_units_.contains(path) && !indexed_loaded_.contains(path))
                            arguments.push_back(path);
                auto inventories = co_await query<std::vector<inventory>>(std::move(arguments));
                for (auto& entry : inventories) {
                    auto path = std::filesystem::path{entry.path};
                    auto name = (path.is_absolute() ? path.lexically_relative(root_) : path).generic_string();
                    if (!indexed_loaded_.insert(name).second)
                        continue;
                    auto& names = indexed_by_unit_[name];
                    for (auto& part : entry.contributions) {
                        cpp::contribution_key key{name, part.key};
                        auto id = json::write(key);
                        names.push_back(id);
                        indexed_parts_.emplace(std::move(id), std::pair{std::move(key), std::move(part)});
                    }
                }
            }
            for (const auto& path : paths) {
                if (indexed && indexed_units_.contains(path)) {
                    for (const bool declarations : {true, false})
                        for (const auto& name : indexed_by_unit_.at(path))
                            if ((indexed_parts_.at(name).second.kind == "declarations") == declarations)
                                result.push_back({state_.project, state_.stage, state_.provider, "evaluate", semantic::unit_kind::group, name});
                } else
                    result.push_back({state_.project, state_.stage, state_.provider, "evaluate", semantic::unit_kind::group,
                                      json::write(cpp::contribution_key{path, "immediate"})});
            }
            co_return result;
        }

        auto do_evaluate(const evaluation_request& request) -> coroutine::task<evaluated_unit> override {
            const auto key = json::read<cpp::contribution_key>(request.unit.name).value();
            auto path = key.translation_unit;
            const auto part = indexed_parts_.find(request.unit.name);
            if (part != indexed_parts_.end())
                path = part->second.first.translation_unit;
            const auto document = std::ranges::find(installed_.documents, path, &language::document::path);
            if (document == installed_.documents.end())
                throw std::invalid_argument("C++ document is not installed");
            evaluated_unit result{.request = request};
            std::optional<cpp::publication> exported;
            const auto& facts = result.request.demand.facts;
            const bool immediate = facts.size() == 1 && facts.front() == "diagnostics";
            if (!immediate && commands_.contains("query")) {
                co_await index();
                if (indexed_units_.contains(document->path)) {
                    std::vector<std::string> arguments{"semantic-export", document->path};
                    if (part != indexed_parts_.end())
                        arguments.insert(arguments.end(), {"--contribution", part->second.first.contribution});
                    auto answer = co_await query<cpp::publication>(std::move(arguments));
                    exported = std::move(answer);
                }
            }
            if (!exported) {
                auto raw = co_await server_.document_request("clice/semanticExport", *document);
                auto parsed = json::from_value<cpp::publication, json::camel_case>(raw);
                if (!parsed)
                    throw std::runtime_error("clice semantic export failed: " + parsed.error().what());
                exported = std::move(*parsed);
            }
            const bool scoped_configurations = indexed_epoch_ == state_.epoch && exported->configurations.complete &&
                                               !exported->configurations.commands.empty() &&
                                               std::ranges::all_of(exported->configurations.commands, [&](const auto& entry) {
                                                   const auto found = indexed_configurations_.find(entry.first);
                                                   return found != indexed_configurations_.end() && !entry.second.empty() && found->second == entry.second;
                                               });
            result.shards.push_back(cpp::translate(*exported, root_, texts_, state_.project, state_.options, state_.stage, document->path, state_.inputs,
                                                   installed_.source_files, runtime_.inputs, runtime_.executables, scoped_configurations));
            auto& unit = result.shards.front().unit;
            {
                const auto parent_id = unit.id;
                unit.recipe = {request.unit, state_.options};
                unit.id = semantic::unit_identifier(request.unit);
                if (part != indexed_parts_.end()) {
                    std::erase_if(unit.direct_inputs, [](const auto& input) { return input.kind == semantic::input_kind::file; });
                    unit.direct_inputs.push_back({semantic::input_kind::provider, request.unit.name, part->second.second.value, "clice/contribution"});
                }
                auto own = [&](auto& rows) {
                    for (auto& row : rows)
                        row.ownership.owner_unit = unit.id;
                };
                auto& shard = result.shards.front();
                own(shard.symbols);
                own(shard.occurrences);
                own(shard.references);
                own(shard.scopes);
                own(shard.relations);
                own(shard.diagnostics);
                own(shard.typed_ranges);
                auto record_id = [&](std::string& id) {
                    if (id.starts_with(parent_id))
                        id = unit.id + id.substr(parent_id.size());
                };
                for (auto& occurrence : shard.occurrences)
                    record_id(occurrence.id);
                for (auto& reference : shard.references)
                    record_id(reference.id);
                for (auto& relation : shard.relations) {
                    record_id(relation.id);
                    if (relation.reference)
                        record_id(*relation.reference);
                    if (relation.occurrence)
                        record_id(*relation.occurrence);
                }
                for (auto& diagnostic : shard.diagnostics)
                    record_id(diagnostic.id);
            }
            unit.cacheable =
                unit.cacheable && !runtime_.execution.empty() && exported->complete && std::ranges::none_of(unit.observed_dependencies, [](const auto& read) {
                    return read.value.empty() && (read.kind == semantic::dependency_kind::artifact ||
                                                  (read.kind == semantic::dependency_kind::negative_lookup &&
                                                   (std::filesystem::path(read.key).is_absolute() || read.key.starts_with("../"))));
                });
            co_return result;
        }
        auto resolves() const -> bool override { return false; }

    private:
        // These subprocesses inherit the slot's allocation, environment and outer
        // evaluation deadline. The live server remains the sole index writer.
        auto command(std::string name, std::vector<std::string> arguments) -> coroutine::task<std::string> {
            auto command = command_;
            command.argv = commands_.at(name);
            command.argv.insert(command.argv.end(), arguments.begin(), arguments.end());
            command.argv.insert(command.argv.end(), {"--workspace", root_.string(), "--config-overlay", overlay_});
            command.stdio = io::process_stdio::blocking;
            auto report = co_await runner::run_step(io_, std::move(command), std::nullopt, constants::semantic_object_bytes, name);
            if (report.truncated)
                throw std::length_error("clice " + name + " exceeds the semantic object limit");
            if (report.exit_code != 0)
                throw std::runtime_error("clice " + name + " failed (exit " + std::to_string(report.exit_code) + "): " + report.output);
            co_return std::move(report.output);
        }

        template <class T, class... Processors> auto query(std::vector<std::string> arguments) -> coroutine::task<T> {
            struct response {
                T result;
                std::vector<std::string> stale;
            };
            auto output = co_await command("query", std::move(arguments));
            auto answer = json::read<response, Processors...>(output);
            if (!answer)
                throw std::runtime_error("clice query returned an invalid result: " + answer.error().what());
            if (!answer->stale.empty())
                throw std::runtime_error("clice query still has stale inputs after indexing");
            co_return std::move(answer->result);
        }

        auto index() -> coroutine::task<> {
            if (indexed_epoch_ == state_.epoch)
                co_return;
            co_await server_.synchronize({});
            (void)co_await server_.workspace_request("clice/index");
            struct file {
                std::string path;
                bool translation_unit = false;
                std::vector<std::string> translation_units;
                std::map<std::string, std::string> configuration;
            };
            struct inventory {
                std::vector<file> files;
            };
            auto files = co_await query<inventory, json::camel_case>({"project-files"});
            indexed_units_.clear();
            indexed_owners_.clear();
            indexed_configurations_.clear();
            indexed_parts_.clear();
            indexed_loaded_.clear();
            indexed_by_unit_.clear();
            auto relative = [&](const std::string& name) {
                auto path = std::filesystem::path(name);
                return (path.is_absolute() ? path.lexically_relative(root_) : path).generic_string();
            };
            for (const auto& file : files.files) {
                auto path = relative(file.path);
                indexed_configurations_.insert(file.configuration.begin(), file.configuration.end());
                if (file.translation_unit)
                    indexed_units_.insert(path);
                for (const auto& owner : file.translation_units)
                    indexed_owners_[path].push_back(relative(owner));
            }
            indexed_epoch_ = state_.epoch;
        }

        std::string overlay_;
        std::set<std::string> indexed_units_, indexed_loaded_;
        std::map<std::string, std::vector<std::string>> indexed_owners_;
        std::map<std::string, std::string> indexed_configurations_;
        std::map<std::string, std::pair<cpp::contribution_key, cpp::contribution_info>> indexed_parts_;
        std::map<std::string, std::vector<std::string>> indexed_by_unit_;
        std::uint64_t indexed_epoch_ = 0;
    };
} // namespace language
