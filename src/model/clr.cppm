export module clr;
import std;
import evaluation;
import coroutine;
import json;
import records;
import content;
import channels;
import enumeration;
export import provider;

export namespace clr {
    class reader;
    struct point {
        std::string file;
        std::uint64_t offset = 0, line = 0, column = 0, end_line = 0, end_column = 0;
    };
    struct entity {
        struct parameter_metadata {
            std::uint32_t sequence = 0;
            semantic::records::parameter_marshalling value;
        };
        std::string id, key, name, kind, owner;
        std::uint32_t flags = 0;
        bool imported = false;
        std::string return_type;
        std::vector<std::string> parameters;
        std::vector<point> locations;
        std::string library, entry_point, convention;
        std::optional<semantic::records::structure_layout> layout;
        std::optional<semantic::records::import_marshalling> marshalling;
        std::vector<parameter_metadata> marshalling_parameters;
        struct mapping {
            std::int32_t flags = 0;
            std::string kind;
            std::optional<std::int32_t> variant, sequence;
        };
        std::string documentation_id, fsharp_documentation_id, accessor_role, visibility;
        std::optional<mapping> compilation_mapping;
    };
    struct call {
        std::string from, target;
        std::uint64_t offset = 0;
        bool call = true;
        std::string dispatch;
    };
    struct relationship {
        std::string kind, from, target, resolution;
    };
    struct object {
        std::string assembly;
        std::vector<entity> entities;
        std::vector<semantic::records::type> types;
        std::vector<call> calls;
        std::vector<relationship> relations;
        std::vector<std::string> unresolved;
    };
    inline auto records(object object, const semantic::recipe& recipe, const std::string& module, const std::vector<semantic::input_object>& inputs)
        -> semantic::shard {
        semantic::shard out;
        out.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
        out.unit.direct_inputs = inputs;
        out.unit.coverage.available = {
            {.modules = {module},
             .contexts = {out.unit.context},
             .facts = {std::string{"symbols"}, std::string{"types"}, std::string{"definitions"}, std::string{"references"}, std::string{"relations"}}}};
        out.types = std::move(object.types);
        const auto artifact = std::ranges::find_if(inputs, [&](const semantic::input_object& input) { return input.name == recipe.unit.name; });
        if (artifact == inputs.end())
            throw std::invalid_argument("CLR records require their artifact input");
        const auto ownership = semantic::ownership{out.unit.id, out.unit.context, {.provider = "clr-artifact", .kind = std::string{"artifact"}}};
        std::set<std::string> entities;
        for (const auto& value : object.entities)
            entities.insert(value.id);
        for (const auto& value : object.entities) {
            semantic::native_identity identity{recipe.unit.project, module, "clr-artifact", "clr", recipe.options + ":" + recipe.unit.name + ":" + value.key};
            out.identities.push_back({identity, value.id, {}});
            semantic::records::symbol symbol{.id = value.id,
                                             .ownership = ownership,
                                             .language = "clr",
                                             .domain = "clr",
                                             .module = module,
                                             .name = value.name,
                                             .qualified_name = value.key,
                                             .kind = semantic::symbol_kind{value.kind},
                                             .native_key = identity};
            if (!value.owner.empty() && entities.contains(value.owner))
                symbol.owner = value.owner;
            if (value.imported)
                symbol.modifiers.push_back("import");
            symbol.visibility = value.visibility;
            if (value.kind == "type") {
                if (value.flags & 128)
                    symbol.modifiers.push_back("abstract");
                if (value.flags & 256)
                    symbol.modifiers.push_back("sealed");
                if (value.flags & 32)
                    symbol.modifiers.push_back("interface");
            } else {
                if (value.flags & 16)
                    symbol.modifiers.push_back("static");
                if (value.kind == "field") {
                    if (value.flags & 32)
                        symbol.modifiers.push_back("readonly");
                    if (value.flags & 64)
                        symbol.modifiers.push_back("const");
                } else {
                    if (value.flags & 1024)
                        symbol.modifiers.push_back("abstract");
                    if (value.flags & 64)
                        symbol.modifiers.push_back("virtual");
                    if (value.flags & 32)
                        symbol.modifiers.push_back("final");
                }
            }
            if (!value.accessor_role.empty())
                symbol.modifiers.push_back(value.accessor_role);
            if (value.kind == "method" || value.kind == "constructor" || (value.kind == "property" && !value.parameters.empty())) {
                auto id = "signature:" + value.id;
                semantic::records::type signature{.id = id, .domain = "clr", .kind = std::string{"function"}, .return_type = value.return_type};
                signature.parameter_groups.emplace_back();
                for (const auto& parameter : value.parameters)
                    signature.parameter_groups.back().parameters.push_back({.type = parameter});
                out.types.push_back(std::move(signature));
                symbol.type = std::move(id);
            } else if (!value.return_type.empty())
                symbol.type = value.return_type;
            symbol.extensions = {{"clr.assembly", object.assembly}, {"clr.token", value.id}, {"clr.sequence_points", json::to_value(value.locations)}};
            if (value.layout)
                symbol.extensions["clr.layout"] = json::to_value(*value.layout);
            if (!value.documentation_id.empty())
                symbol.extensions["clr.documentation_id"] = value.documentation_id;
            if (!value.fsharp_documentation_id.empty())
                symbol.extensions["clr.fsharp_documentation_id"] = value.fsharp_documentation_id;
            if (!value.accessor_role.empty())
                symbol.extensions["clr.accessor_role"] = value.accessor_role;
            if (value.compilation_mapping)
                symbol.extensions["clr.compilation_mapping"] = json::to_value(*value.compilation_mapping);
            if (!value.library.empty()) {
                symbol.modifiers.push_back("pinvoke");
                symbol.extensions["clr.pinvoke.library"] = value.library;
                symbol.extensions["clr.pinvoke.entry_point"] = value.entry_point;
                symbol.extensions["clr.pinvoke.convention"] = value.convention;
                if (value.marshalling)
                    symbol.extensions["clr.pinvoke.marshalling"] = json::to_value(*value.marshalling);
                symbol.extensions["clr.pinvoke.parameters"] = json::to_value(value.marshalling_parameters);
            }
            out.symbols.push_back(std::move(symbol));
            out.occurrences.push_back({.id = "definition:" + value.id,
                                       .ownership = ownership,
                                       .symbol = value.id,
                                       .location = {.artifact = semantic::artifact_location{artifact->object, module, value.key, 0}},
                                       .roles = {std::string{value.imported ? "declaration" : "definition"}}});
        }
        for (const auto& value : object.relations) {
            out.relations.push_back({.id = "metadata:" + value.kind + ":" + value.from + ":" + value.target,
                                     .kind = semantic::relation_kind{value.kind},
                                     .ownership = ownership,
                                     .from = value.from,
                                     .to = value.target,
                                     .resolution = json::literal<"resolved", "candidate", "unresolved">{value.resolution},
                                     .from_context = out.unit.context,
                                     .to_context = out.unit.context});
        }
        out.unit.coverage.unresolved_count += object.unresolved.size();
        for (const auto& value : object.calls) {
            auto id = value.from + ":il:" + std::to_string(value.offset);
            const auto resolved = !value.target.empty();
            semantic::records::reference reference{.id = id,
                                                   .ownership = ownership,
                                                   .from = value.from,
                                                   .location = {.artifact = semantic::artifact_location{artifact->object, module, value.from, value.offset}},
                                                   .roles = {std::string{"reference"}},
                                                   .resolution = std::string{resolved ? "resolved" : "unresolved"},
                                                   .dispatch = json::literal<"direct", "virtual", "interface", "indirect">{value.dispatch},
                                                   .target_context = resolved ? std::optional{out.unit.context} : std::nullopt};
            if (resolved)
                reference.target = value.target;
            else
                ++out.unit.coverage.unresolved_count;
            if (value.call)
                reference.roles.push_back(std::string{"call"});
            out.references.push_back(std::move(reference));
            if (resolved && value.call)
                out.relations.push_back({.id = id,
                                         .kind = std::string{"calls"},
                                         .ownership = ownership,
                                         .from = value.from,
                                         .to = value.target,
                                         .resolution = std::string{"resolved"},
                                         .reference = id,
                                         .from_context = out.unit.context,
                                         .to_context = out.unit.context});
        }
        return out;
    }
    inline void associate(semantic::shard& shard, const object& object, const std::vector<const semantic::shard*>& source_shards,
                          const std::map<std::string, std::string>& documents) {
        struct source {
            const semantic::records::symbol* symbol;
            const semantic::identity_binding* identity;
            semantic::locator location;
        };
        std::unordered_map<semantic::symbol_id, const semantic::records::occurrence*> occurrences;
        for (const auto& row : shard.occurrences)
            occurrences.try_emplace(row.symbol, &row);
        std::unordered_set<semantic::symbol_id> retained;
        for (const auto& row : shard.identities)
            retained.insert(row.symbol);
        std::vector<source> sources;
        auto extension = [](const semantic::records::symbol& symbol, const std::string& name) -> std::string {
            const auto found = symbol.extensions.find(name);
            return found == symbol.extensions.end() ? std::string{} : json::from_value<std::string>(found->second).value();
        };
        auto sequence = [](const semantic::records::symbol& symbol, const std::string& name) -> std::optional<std::int32_t> {
            const auto found = symbol.extensions.find(name);
            return found == symbol.extensions.end() ? std::nullopt : std::optional{json::from_value<std::int32_t>(found->second).value()};
        };
        for (const auto* part : source_shards) {
            std::map<semantic::symbol_id, std::vector<const semantic::identity_binding*>> identities;
            for (const auto& identity : part->identities)
                identities[identity.symbol].push_back(&identity);
            for (const auto& symbol : part->symbols) {
                // Accessors may share a source anchor and canonical symbol while retaining distinct native signatures.
                const auto aliases = identities.find(symbol.id);
                if (aliases == identities.end())
                    continue;
                const auto found = std::ranges::find_if(
                    aliases->second, [&](const auto* identity) { return !symbol.native_key || identity->declaration == *symbol.native_key; });
                if (found == aliases->second.end())
                    continue;
                const auto assembly = extension(symbol, "fsharp.assembly");
                if (!assembly.empty() && assembly != object.assembly)
                    continue;
                sources.push_back({&symbol, *found, symbol.ownership.origin.location.value_or(semantic::locator{})});
            }
        }
        std::map<std::string, std::vector<const source*>> documentation;
        for (const auto& source : sources)
            if (const auto* identity = std::get_if<semantic::native_identity>(&source.identity->declaration); identity && identity->domain == "fsharp")
                documentation[identity->native_key].push_back(&source);
        std::map<std::string, const entity*> entities;
        for (const auto& entity : object.entities)
            entities[entity.id] = &entity;
        std::map<std::string, std::vector<const source*>> mapped;
        std::set<std::tuple<std::string, std::string, std::string>> emitted;
        auto emit = [&](const entity& entity, const std::vector<const source*>& candidates, const std::string& evidence) {
            std::map<std::pair<std::string, std::string>, const source*> unique;
            for (const auto* source : candidates)
                unique.emplace(std::pair{source->identity->symbol, source->symbol->ownership.context}, source);
            for (const auto& [key, source] : unique) {
                if (!emitted.emplace(key.first, key.second, entity.id).second)
                    continue;
                if (retained.insert(source->identity->symbol).second)
                    shard.identities.push_back(*source->identity);
                const auto* occurrence = occurrences.at(entity.id);
                auto ownership = occurrence->ownership;
                ownership.origin.kind = std::string{evidence == "debug_location"                                           ? "debug"
                                                    : evidence == "compilation_mapping" || evidence == "metadata_accessor" ? "generated"
                                                                                                                           : "metadata"};
                ownership.origin.location = evidence == "debug_location" ? source->location : semantic::locator{};
                ownership.origin.location->artifact = occurrence->location.artifact;
                shard.relations.push_back(
                    {.id = "emits:" + key.first + ":" + key.second + ":" + entity.id,
                     .kind = std::string{"emits"},
                     .ownership = std::move(ownership),
                     .from = key.first,
                     .to = entity.id,
                     .resolution = std::string{unique.size() == 1 ? "resolved" : "candidate"},
                     .detail =
                         semantic::records::emission_detail{
                             .evidence = decltype(semantic::records::emission_detail::evidence){evidence},
                             .file = evidence == "debug_location" && source->location.source ? source->location.source->path : std::string{}},
                     .from_context = key.second,
                     .to_context = shard.unit.context});
                mapped[entity.id].push_back(source);
            }
        };
        for (const auto& entity : object.entities) {
            std::vector<const source*> candidates;
            bool generated = false;
            const auto& id = entity.fsharp_documentation_id.empty() ? entity.documentation_id : entity.fsharp_documentation_id;
            if (const auto found = documentation.find(id); found != documentation.end())
                candidates = found->second;
            if (candidates.empty() && entity.compilation_mapping && entity.compilation_mapping->sequence) {
                std::set<std::string> owners, visited;
                auto owner = entity.owner;
                while (!owner.empty() && visited.insert(owner).second) {
                    const auto found = entities.find(owner);
                    if (found == entities.end())
                        break;
                    owners.insert(found->second->documentation_id);
                    owner = found->second->owner;
                }
                const auto& mapping = *entity.compilation_mapping;
                for (const auto& source : sources) {
                    const auto kind = source.symbol->kind;
                    if ((mapping.kind == "UnionCase" && kind != "enum_member") || (mapping.kind == "Field" && kind != "field") ||
                        (mapping.kind != "UnionCase" && mapping.kind != "Field"))
                        continue;
                    if (!owners.contains(extension(*source.symbol, "fsharp.declaring_entity")) ||
                        sequence(*source.symbol, "fsharp.sequence") != mapping.sequence)
                        continue;
                    if (mapping.kind == "Field" && sequence(*source.symbol, "fsharp.variant") != mapping.variant)
                        continue;
                    candidates.push_back(&source);
                }
                generated = !candidates.empty();
            }
            if (!candidates.empty())
                emit(entity, candidates, generated ? "compilation_mapping" : "metadata_signature");
        }
        for (const auto& entity : object.entities)
            if (!entity.accessor_role.empty() && !mapped.contains(entity.id))
                if (const auto owner = mapped.find(entity.owner); owner != mapped.end())
                    emit(entity, owner->second, "metadata_accessor");

        const auto bodies =
            channels::definition_bodies(source_shards, {std::string{"function"}, std::string{"method"}, std::string{"constructor"}, std::string{"property"}});
        std::map<std::string, std::vector<std::vector<std::uint64_t>>> positions;
        for (const auto& [path, text] : documents) {
            auto& lines = positions[path];
            lines.push_back({0});
            std::uint64_t scalar = 0;
            for (std::size_t at = 0; at < text.size();) {
                const auto byte = static_cast<unsigned char>(text[at]);
                const auto width = byte < 0x80 ? 1u : byte < 0xe0 ? 2u : byte < 0xf0 ? 3u : 4u;
                if (byte == '\n')
                    lines.push_back({++scalar});
                else {
                    if (width == 4)
                        lines.back().push_back(scalar);
                    lines.back().push_back(++scalar);
                }
                at += width;
            }
        }
        for (const auto& entity : object.entities)
            for (const auto& point : entity.locations) {
                auto debug = std::filesystem::path{point.file}.lexically_normal().generic_string();
                std::vector<const channels::definition_body*> selected;
                std::uint64_t length = std::numeric_limits<std::uint64_t>::max();
                for (const auto& body : bodies) {
                    if (debug != body.body.path && !debug.ends_with("/" + body.body.path))
                        continue;
                    const auto found = positions.find(body.body.path);
                    if (found == positions.end() || !point.line || point.line > found->second.size())
                        continue;
                    const auto& columns = found->second[point.line - 1];
                    if (!point.column || point.column > columns.size())
                        continue;
                    const auto offset = columns[point.column - 1];
                    if (offset < body.body.begin || offset >= body.body.end)
                        continue;
                    const auto size = body.body.end - body.body.begin;
                    if (size < length) {
                        selected.clear();
                        length = size;
                    }
                    if (size == length)
                        selected.push_back(&body);
                }
                std::vector<const source*> candidates;
                for (const auto* body : selected)
                    for (const auto& source : sources)
                        if (source.identity->symbol == body->identity.symbol && source.symbol->ownership.context == body->context)
                            candidates.push_back(&source);
                if (!candidates.empty())
                    emit(entity, candidates, "debug_location");
            }
    }

} // namespace clr

export namespace language {
    // Assemblies and their portable debug symbols.
    template <> struct domain<"clr"> {
        static constexpr std::string_view name = "clr", artifact = "clr_artifact";
    };
    template <> struct server<"clr-artifact"> : reads<domain<"clr">, optional_input<"symbols", "portable_pdb">> {
        using adapter = clr::reader;
        struct options {
            std::string executable = "dotnet", reader = "fughetta-clr", module;
            builds::environment env;
        };
        static constexpr std::array<std::string_view, 5> facts{"symbols", "types", "definitions", "references", "relations"};
        static constexpr std::string_view update_granularity = "artifact";
        // Metadata names a generic application `[assembly]Namespace.Name`arity`.
        static auto conventions() -> std::vector<semantic::language_conventions> {
            return {{.language = "clr",
                     .visibility = {"public", "protected_internal", "protected", "internal", "private_protected", "private"},
                     .value_wrappers = {"System.Nullable", "System.Threading.Tasks.Task", "System.Threading.Tasks.ValueTask"},
                     .wrapper_prefix = "[", .wrapper_prefix_end = "]", .wrapper_arguments = "`",
                     .escape_types = {{"System.Object"}}}};
        }
        static auto channels() -> std::vector<semantic::channel_definition> {
            return {{.name = "declarations", .projections = {std::string{"symbols"}, std::string{"types"}}},
                    {.name = "relations", .projections = {std::string{"references"}, std::string{"relations"}}},
                    linkage_channel()};
        }
        static auto requirements(const options& o) -> builds::requirements { return builds::tools({o.executable, o.reader}); }
        static auto executes_project_code(const options& o) -> bool { return builds::in_project({o.executable, o.reader}); }
        static void validate(const options& o) {
            (void)requirements(o);
            if (o.module.empty())
                throw std::invalid_argument("CLR artifact module is required");
        }
        static auto launch(const options& o, const std::vector<builds::context>& inputs) -> program {
            std::string artifact, symbols;
            for (const auto& input : inputs) {
                auto& target = input.type == "clr_artifact"   ? artifact
                               : input.type == "portable_pdb" ? symbols
                                                              : throw std::invalid_argument("unknown CLR input");
                if (!target.empty())
                    throw std::invalid_argument("CLR reader requires unique assembly and PDB inputs");
                target = input.path;
            }
            if (artifact.empty())
                throw std::invalid_argument("CLR reader requires one clr_artifact");
            program out{.provider = "clr-artifact",
                        .argv = {o.executable, builds::tool(o.reader), "--clr", "${tree}/" + artifact},
                        .env = o.env,
                        .initialization =
                            json::to_value(std::map<std::string, std::string>{{"artifact", artifact}, {"symbols", symbols}, {"module", o.module}}),
                        .executables = {o.reader}};
            if (!symbols.empty())
                out.argv.push_back("${tree}/" + symbols);
            return out;
        }
    };
    static_assert(domain_reader<server<"clr-artifact">>);
} // namespace language

export namespace clr {
    class reader : public language::reader_adapter {
    public:
        using language::reader_adapter::reader_adapter;

    protected:
        auto do_open(json::value value) -> coroutine::task<> override {
            settings_ = json::from_value<std::map<std::string, std::string>>(value).value();
            co_return;
        }
        auto do_units(const semantic::extent&) -> coroutine::task<std::vector<semantic::unit_key>> override {
            co_return std::vector<semantic::unit_key>{
                {state_.project, state_.stage, state_.provider, "evaluate", semantic::unit_kind::artifact, settings_.at("artifact")}};
        }
        auto do_evaluate(const language::evaluation_request& request) -> coroutine::task<language::evaluated_unit> override {
            std::vector<semantic::input_object> inputs;
            inputs.push_back({semantic::input_kind::runtime, "toolchain", runtime_.execution});
            std::vector<semantic::dependency> consumed;
            for (const auto name : {"artifact", "symbols"}) {
                const auto path = settings_.at(name);
                if (path.empty())
                    continue;
                const auto found = installed_.model.find(path);
                if (found == installed_.model.end())
                    throw std::invalid_argument("CLR input has no measured build output");
                inputs.push_back({semantic::input_kind::artifact, path, found->second});
                consumed.push_back({.kind = semantic::dependency_kind::artifact, .project = state_.project, .key = path, .value = found->second});
            }
            const auto object = json::read<clr::object>(co_await run(command_.argv)).value();
            auto shard = records(object, {request.unit, state_.options}, settings_.at("module"), inputs);
            shard.unit.cacheable = !runtime_.execution.empty();
            if (!installed_.source_facts.empty()) {
                const auto definitions = channels::definition_bodies(installed_.source_facts, "definition_bodies");
                std::set<std::string> files;
                for (const auto& definition : definitions)
                    if (installed_.source_files.contains(definition.body.path))
                        files.insert(definition.body.path);
                auto documents = co_await loader_.read(files);
                std::vector<const semantic::shard*> source_shards;
                for (const auto& [_, parts] : installed_.source_facts)
                    for (const auto& part : parts)
                        source_shards.push_back(part->data.get());
                associate(shard, object, source_shards, documents);
                for (const auto& file : files)
                    consumed.push_back(
                        {.kind = semantic::dependency_kind::source, .project = state_.project, .key = file, .value = installed_.source_files.at(file)});
                std::set<semantic::unit_id> producers;
                for (const auto& [_, objects] : installed_.source_facts)
                    for (const auto& source : objects)
                        if (producers.insert(source->data->unit.id).second) {
                            consumed.push_back(
                                channels::consume(state_.project, state_.stage, *source->data, "definition_bodies", semantic::dependency_kind::body));
                            consumed.push_back(
                                channels::consume(state_.project, state_.stage, *source->data, "declarations", semantic::dependency_kind::declaration));
                        }
                std::map<std::string, std::string> selected;
                for (const auto& file : files)
                    selected[file] = installed_.source_files.at(file);
                consumed.push_back(enumeration::read(state_.project, "source_documents", selected, {.files = {files.begin(), files.end()}}));
            }
            shard.unit.observed_dependencies = std::move(consumed);
            shard.unit.coverage.requested = request.demand;
            language::evaluated_unit result;
            result.shards.push_back(std::move(shard));
            co_return result;
        }

    private:
        std::map<std::string, std::string> settings_;
    };
} // namespace clr
