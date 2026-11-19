export module kmetadata;
import std;
import records;
import json;

export namespace kotlin_metadata {
    struct identity {
        std::string id, key;
        std::optional<std::string> owner_key;
    };
    struct member {
        std::string owner, kind, name, descriptor;
    };
    struct declaration {
        std::string id, key;
        std::optional<std::string> owner_key, containing_key;
        std::string name, qualified_name;
        semantic::symbol_kind kind;
        std::string visibility;
        std::vector<std::string> modifiers;
        std::optional<semantic::type_id> type;
        std::vector<semantic::records::parameter_detail> parameters;
        std::vector<member> jvm_members;
    };
    struct klass {
        std::string owner;
        std::uint32_t metadata_kind = 0;
        std::string package;
        std::vector<declaration> declarations;
        std::vector<identity> identities;
        std::vector<semantic::records::type> types;
    };
    struct report {
        std::vector<klass> classes;
    };
    using member_key = std::tuple<std::string, std::string, std::string, std::string>;
    struct member_target {
        semantic::identity_binding identity;
        semantic::context_id context;
        semantic::locator location;
    };
    using member_targets = std::map<member_key, member_target>;

    class projector {
        const report& decoded_;
        const member_targets& members_;
        std::string library_module_, source_provider_;
        semantic::context_id context_;
        std::map<std::string, const klass*> classes_;
        std::map<std::string, const identity*> identities_;
        std::map<std::pair<std::string, std::optional<std::string>>, std::string> ids_by_key_;
        std::set<std::string> owned_;
        std::deque<semantic::identity_binding> source_identities_;
        std::map<std::string, std::vector<const semantic::identity_binding*>> source_keys_;
        std::map<semantic::symbol_id, std::set<semantic::context_id>> source_contexts_;
        std::map<std::string, std::optional<semantic::identity_binding>> resolved_;

    public:
        projector(const projector&) = delete;
        auto operator=(const projector&) -> projector& = delete;
        projector(const report& decoded, std::string library_module, std::string source_provider, semantic::context_id context, const member_targets& members,
                  const std::vector<const semantic::shard*>& source_facts)
            : decoded_(decoded), members_(members), library_module_(std::move(library_module)), source_provider_(std::move(source_provider)),
              context_(std::move(context)) {
            auto& identities = identities_;
            auto& ids_by_key = ids_by_key_;
            auto& owned = owned_;
            auto& source_keys = source_keys_;
            auto& source_contexts = source_contexts_;
            for (const auto& klass : decoded.classes) {
                classes_.emplace(klass.owner, &klass);
                for (const auto& value : klass.identities) {
                    identities.emplace(value.id, &value);
                    ids_by_key.emplace(std::pair{value.key, value.owner_key}, value.id);
                }
                for (const auto& value : klass.declarations)
                    owned.insert(value.id);
            }
            for (const auto* shard : source_facts) {
                for (const auto& binding : shard->identities)
                    if (const auto* native = std::get_if<semantic::native_identity>(&binding.declaration); native && native->domain == "kotlin") {
                        source_identities_.push_back(binding);
                        source_keys[native->native_key].push_back(&source_identities_.back());
                    }
                for (const auto& symbol : shard->symbols)
                    if (symbol.domain == "kotlin")
                        source_contexts[symbol.id].insert(symbol.ownership.context);
            }
        }
        auto records(const semantic::recipe& recipe, const std::optional<std::string>& selected_owner = {}) -> semantic::shard {
            const auto& library_module = library_module_;
            const auto& source_provider = source_provider_;
            const auto& context = context_;
            const auto& members = members_;
            const auto& identities = identities_;
            const auto& ids_by_key = ids_by_key_;
            const auto& owned = owned_;
            const auto& source_keys = source_keys_;
            const auto& source_contexts = source_contexts_;
            std::vector<const klass*> selected_classes;
            if (selected_owner) {
                if (const auto found = classes_.find(*selected_owner); found != classes_.end())
                    selected_classes.push_back(found->second);
            } else
                for (const auto& klass : decoded_.classes)
                    selected_classes.push_back(&klass);
            semantic::shard out;
            out.unit = semantic::make_unit(recipe, context);
            out.unit.modules = {library_module};
            out.unit.coverage.available = {{.modules = {library_module},
                                            .contexts = {context},
                                            .facts = {std::string{"symbols"}, std::string{"types"}, std::string{"definitions"}, std::string{"relations"}}}};
            auto& resolved = resolved_;
            std::set<std::string> used;
            std::set<std::string> visiting;
            auto bind = [&](this auto&& self, const std::string& local) -> std::optional<semantic::identity_binding> {
                used.insert(local);
                if (const auto found = resolved.find(local); found != resolved.end()) {
                    const auto metadata = identities.find(local);
                    if (metadata != identities.end() && metadata->second->owner_key)
                        if (const auto parent = ids_by_key.find({*metadata->second->owner_key, std::nullopt}); parent != ids_by_key.end())
                            self(parent->second);
                    return found->second;
                }
                const auto found = identities.find(local);
                if (found == identities.end())
                    return {};
                if (!visiting.insert(local).second)
                    throw std::invalid_argument("Kotlin metadata declaration owners form a cycle");
                const auto& value = *found->second;
                std::optional<semantic::symbol_id> owner;
                if (value.owner_key) {
                    const auto parent = ids_by_key.find({*value.owner_key, std::nullopt});
                    if (parent == ids_by_key.end())
                        throw std::invalid_argument("Kotlin metadata declaration owner is absent");
                    const auto bound = self(parent->second);
                    if (!bound)
                        throw std::invalid_argument("Kotlin metadata declaration owner has no identity");
                    owner = bound->symbol;
                }
                std::vector<const semantic::identity_binding*> candidates;
                if (const auto sources = source_keys.find(value.key); sources != source_keys.end())
                    for (const auto* candidate : sources->second) {
                        const auto& native = std::get<semantic::native_identity>(candidate->declaration);
                        if (native.owner == owner &&
                            !std::ranges::contains(candidates, candidate->declaration, [](const auto* binding) -> const auto& { return binding->declaration; }))
                            candidates.push_back(candidate);
                    }
                // An explicit logical library identity disambiguates classpaths;
                // actual source declarations otherwise retain their installed
                // module/provider identity, including configured helper providers.
                if (owned.contains(local) && std::ranges::any_of(candidates, [&](const auto* binding) {
                        return std::get<semantic::native_identity>(binding->declaration).module == library_module;
                    }))
                    std::erase_if(candidates,
                                  [&](const auto* binding) { return std::get<semantic::native_identity>(binding->declaration).module != library_module; });
                if (candidates.size() > 1 && std::ranges::count_if(candidates, [&](const auto* binding) {
                                                 return std::get<semantic::native_identity>(binding->declaration).provider == source_provider;
                                             }) == 1)
                    std::erase_if(candidates,
                                  [&](const auto* binding) { return std::get<semantic::native_identity>(binding->declaration).provider != source_provider; });
                if (candidates.size() > 1)
                    throw std::invalid_argument("Kotlin library declaration has ambiguous source identities: " + value.key);
                std::optional<semantic::identity_binding> result;
                if (!candidates.empty())
                    result = *candidates.front();
                else if (owned.contains(local))
                    result = semantic::identity_binding{
                        semantic::native_identity{recipe.unit.project, library_module, source_provider, "kotlin", value.key, owner}, context + ":" + local};
                resolved.emplace(local, result);
                visiting.erase(local);
                return result;
            };
            std::set<semantic::symbol_id> retained;
            auto retain = [&](const semantic::identity_binding& binding) {
                if (retained.insert(binding.symbol).second)
                    out.identities.push_back(binding);
            };
            auto declaration_id = [&](std::optional<semantic::symbol_id>& value) {
                if (!value)
                    return;
                if (const auto binding = bind(*value))
                    *value = binding->symbol;
                else
                    value.reset();
            };
            auto type_id = [&](std::string& value) { value = context + ":kotlin:" + value; };
            auto optional_type = [&](std::optional<std::string>& value) {
                if (value)
                    type_id(*value);
            };
            std::set<semantic::type_id> type_ids;
            for (const auto* selected : selected_classes) {
                const auto& klass = *selected;
                for (auto value : klass.types) {
                    if (!type_ids.insert(value.id).second)
                        continue;
                    type_id(value.id);
                    declaration_id(value.declaration);
                    declaration_id(value.binder);
                    for (auto* values : {&value.arguments, &value.constraints, &value.elements, &value.type_parameters})
                        for (auto& id : *values)
                            type_id(id);
                    for (auto* id : {&value.receiver, &value.return_type, &value.lower_bound, &value.upper_bound})
                        optional_type(*id);
                    for (auto& group : value.parameter_groups)
                        for (auto& parameter : group.parameters)
                            type_id(parameter.type);
                    auto arguments = [&](this auto&& self, auto& values) -> void {
                        for (auto& argument : values) {
                            optional_type(argument.type);
                            declaration_id(argument.declaration);
                            self(argument.elements);
                        }
                    };
                    arguments(value.application_arguments);
                    if (const auto found = value.extensions.find("kotlin.projections"); found != value.extensions.end()) {
                        struct projection {
                            std::string variance;
                            std::optional<semantic::type_id> type;
                        };
                        auto projections = json::from_value<std::vector<projection>>(found->second).value();
                        for (auto& projection : projections)
                            optional_type(projection.type);
                        found->second = json::to_value(projections);
                    }
                    out.types.push_back(std::move(value));
                }
            }
            for (const auto* selected : selected_classes) {
                const auto& klass = *selected;
                for (const auto& value : klass.declarations) {
                    const auto binding = bind(value.id);
                    if (!binding)
                        throw std::invalid_argument("Kotlin metadata declaration cannot be identified");
                    const semantic::ownership ownership{out.unit.id, context, {.provider = recipe.unit.provider, .kind = std::string{"metadata"}}};
                    const auto& native = std::get<semantic::native_identity>(binding->declaration);
                    const auto already_declared = source_contexts.contains(binding->symbol);
                    if (!already_declared) {
                        semantic::records::symbol symbol{
                            .id = binding->symbol,
                            .ownership = ownership,
                            .language = "kotlin",
                            .domain = "kotlin",
                            .module = native.module,
                            .name = value.name,
                            .qualified_name = value.qualified_name,
                            .kind = value.kind,
                            .type = value.type,
                            .visibility = value.visibility,
                            .modifiers = value.modifiers,
                            .parameters = value.parameters,
                            .native_key = binding->declaration,
                            .extensions = {{"kotlin.metadata_owner", klass.owner}, {"kotlin.metadata_kind", static_cast<std::int64_t>(klass.metadata_kind)}}};
                        optional_type(symbol.type);
                        if (value.containing_key) {
                            if (const auto parent = ids_by_key.find({*value.containing_key, std::nullopt}); parent != ids_by_key.end())
                                if (const auto identified = bind(parent->second))
                                    symbol.owner = identified->symbol;
                        } else if (native.owner)
                            symbol.owner = native.owner;
                        out.symbols.push_back(std::move(symbol));
                        if (value.jvm_members.empty())
                            if (const auto container = members.find({klass.owner, "class", "", ""}); container != members.end()) {
                                auto origin = ownership;
                                origin.origin.location = container->second.location;
                                out.occurrences.push_back({.id = out.unit.id + ":metadata-declaration:" + value.id,
                                                           .ownership = origin,
                                                           .symbol = binding->symbol,
                                                           .location = container->second.location,
                                                           .roles = {std::string{"declaration"}}});
                            }
                    }
                    std::set<member_key> emitted;
                    for (const auto& member : value.jvm_members) {
                        const member_key key{member.owner, member.kind, member.name, member.descriptor};
                        if (!emitted.insert(key).second)
                            continue;
                        const auto found = members.find(key);
                        if (found == members.end()) {
                            ++out.unit.coverage.unresolved_count;
                            out.unit.coverage.missing.push_back(
                                {.scope = {.modules = {library_module}, .facts = {std::string{"relations"}}},
                                 .reason = "Kotlin metadata JVM member is absent: " + member.owner + "." + member.name + member.descriptor});
                            continue;
                        }
                        const auto& target = found->second;
                        retain(target.identity);
                        auto origin = ownership;
                        origin.origin.location = target.location;
                        const auto id = out.unit.id + ":metadata:" + value.id + ":" + json::write(key);
                        if (!already_declared)
                            out.occurrences.push_back(
                                {.id = id, .ownership = origin, .symbol = binding->symbol, .location = target.location, .roles = {std::string{"definition"}}});
                        const auto from_contexts = already_declared ? source_contexts.at(binding->symbol) : std::set<semantic::context_id>{context};
                        for (const auto& source_context : from_contexts)
                            out.relations.push_back({.id = id + ":" + source_context,
                                                     .kind = std::string{"emits"},
                                                     .ownership = origin,
                                                     .from = binding->symbol,
                                                     .to = target.identity.symbol,
                                                     .resolution = std::string{"resolved"},
                                                     .from_context = source_context,
                                                     .to_context = target.context});
                    }
                }
            }
            for (const auto& id : used)
                if (const auto binding = resolved.find(id); binding != resolved.end() && binding->second)
                    retain(*binding->second);
            return out;
        }
    };
    inline auto records(const report& decoded, const semantic::recipe& recipe, const std::string& library_module, const std::string& source_provider,
                        const semantic::context_id& context, const member_targets& members, const std::vector<const semantic::shard*>& source_facts,
                        const std::optional<std::string>& selected_owner = {}) -> semantic::shard {
        return projector{decoded, library_module, source_provider, context, members, source_facts}.records(recipe, selected_owner);
    }
} // namespace kotlin_metadata
