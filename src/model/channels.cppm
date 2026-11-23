export module channels;
import std;
import json;
import content;
import records;

export namespace channels {
    // A port's complete producer set matters for negative lookups: adding an
    // export can resolve an import even when every previous producer is unchanged.
    inline auto selects(const semantic::unit& unit, const semantic::record_selection& selection) -> bool {
        return (selection.projects.empty() || std::ranges::contains(selection.projects, unit.recipe.unit.project)) &&
               (selection.units.empty() || std::ranges::contains(selection.units, unit.id)) &&
               (selection.modules.empty() ||
                std::ranges::any_of(unit.modules, [&](const auto& module) { return std::ranges::contains(selection.modules, module); }));
    }
    inline auto collection_value(const std::vector<const semantic::unit*>& units, const std::string& name, const semantic::record_selection& selection = {})
        -> std::string {
        std::map<semantic::unit_id, std::string> values;
        for (const auto* unit : units) {
            if (!selects(*unit, selection))
                continue;
            const auto found = std::ranges::find_if(unit->output_channels, [&](const semantic::channel& channel) { return channel.name == name; });
            if (found == unit->output_channels.end())
                return {};
            auto [previous, inserted] = values.emplace(unit->id, found->value);
            if (!inserted && previous->second != found->value)
                throw std::invalid_argument("one input unit has conflicting output channels");
        }
        return content::hash_of(json::write(values)).str();
    }
    inline auto collection(const std::string& project, const std::string& port, const std::vector<const semantic::unit*>& units, const std::string& name,
                           semantic::record_selection selection = {}) -> semantic::dependency {
        for (auto* dimension : {&selection.projects, &selection.modules, &selection.units}) {
            std::ranges::sort(*dimension);
            dimension->erase(std::unique(dimension->begin(), dimension->end()), dimension->end());
        }
        auto value = collection_value(units, name, selection);
        if (value.empty())
            throw std::invalid_argument("input collection does not provide channel " + name);
        std::set<semantic::unit_id> producers;
        for (const auto* unit : units)
            if (selects(*unit, selection))
                producers.insert(unit->id);
        return {.kind = semantic::dependency_kind::record_set,
                .project = project,
                .key = port,
                .value = std::move(value),
                .channel = name,
                .producers = {producers.begin(), producers.end()},
                .records = std::move(selection)};
    }
    inline auto definitions(const std::vector<const semantic::shard*>& shards) -> const std::vector<semantic::channel_definition>& {
        if (shards.empty())
            throw std::invalid_argument("semantic unit has no shards");
        const auto& unit = shards.front()->unit;
        for (const auto* shard : shards)
            if (shard->unit.recipe != unit.recipe || shard->unit.channel_definitions != unit.channel_definitions)
                throw std::invalid_argument("semantic unit shards have different recipes or channel definitions");
        return unit.channel_definitions;
    }

    struct definition_body {
        semantic::context_id context;
        semantic::identity_binding identity;
        semantic::locator location;
        semantic::place body;
        semantic::symbol_kind kind;
        std::optional<std::string> linkage_name;
    };

    inline auto source_definitions(const std::vector<const semantic::shard*>& shards, const std::vector<semantic::symbol_kind>& kinds,
                                   const std::string& linkage_extension = {})
        -> std::vector<definition_body> {
        using key = std::pair<std::string, semantic::symbol_id>;
        auto key_for = [&](const semantic::unit& unit, const semantic::symbol_id& symbol) -> key {
            return {linkage_extension.empty() ? unit.id : unit.context, symbol};
        };
        std::map<key, const semantic::records::symbol*> symbols;
        std::map<key, const semantic::identity_binding*> identities;
        for (const auto* shard : shards) {
            for (const auto& symbol : shard->symbols)
                symbols.try_emplace(key_for(shard->unit, symbol.id), &symbol);
            for (const auto& identity : shard->identities)
                identities.try_emplace(key_for(shard->unit, identity.symbol), &identity);
        }
        std::vector<definition_body> out;
        for (const auto* shard : shards)
            for (const auto& occurrence : shard->occurrences) {
                if (!std::ranges::contains(occurrence.roles, std::string{"definition"}))
                    continue;
                const auto& extent = linkage_extension.empty() ? occurrence.body : occurrence.location.source;
                if (!extent)
                    continue;
                const auto id = key_for(shard->unit, occurrence.symbol);
                const auto symbol = symbols.find(id);
                auto kind = occurrence.declaration_kind;
                if (!kind && symbol != symbols.end())
                    kind = symbol->second->kind;
                if (!kind || (!kinds.empty() && !std::ranges::contains(kinds, *kind)))
                    continue;
                std::optional<std::string> linkage_name;
                if (!linkage_extension.empty()) {
                    if (symbol == symbols.end())
                        continue;
                    const auto name = symbol->second->extensions.find(linkage_extension);
                    if (name == symbol->second->extensions.end())
                        continue;
                    auto parsed = json::from_value<std::string>(name->second);
                    if (!parsed || parsed->empty())
                        continue;
                    linkage_name = std::move(*parsed);
                }
                const auto identity = identities.find(id);
                if (identity != identities.end())
                    out.push_back({shard->unit.context, *identity->second, occurrence.location, *extent, *kind, std::move(linkage_name)});
            }
        return out;
    }

    template <class Ports> auto definition_bodies(const Ports& ports, const std::string& channel) -> std::vector<definition_body> {
        std::map<semantic::unit_id, std::vector<const semantic::shard*>> groups;
        for (const auto& [port, objects] : ports)
            for (const auto& object : objects)
                groups[object->data->unit.id].push_back(object->data.get());
        std::vector<definition_body> out;
        for (const auto& [unit, shards] : groups) {
            const auto& definitions = channels::definitions(shards);
            const auto selected = std::ranges::find(definitions, channel, &semantic::channel_definition::name);
            if (selected == definitions.end())
                throw std::invalid_argument("semantic unit did not declare channel " + channel);
            auto values = source_definitions(shards, selected->symbol_kinds);
            std::ranges::move(values, std::back_inserter(out));
        }
        return out;
    }

    inline auto definition_bodies(const std::vector<const semantic::shard*>& shards, const std::vector<semantic::symbol_kind>& kinds)
        -> std::vector<definition_body> {
        return source_definitions(shards, kinds);
    }

    template <class Ports> auto linkage_definitions(const Ports& ports, const std::string& extension) -> std::vector<definition_body> {
        std::vector<const semantic::shard*> shards;
        for (const auto& [port, objects] : ports)
            for (const auto& object : objects)
                if (std::ranges::contains(object->data->unit.channel_definitions, std::string{"linkage"}, &semantic::channel_definition::name))
                    shards.push_back(object->data.get());
        // clice separates declarations and definition sites into different
        // contributions in the same translation-unit context.
        return source_definitions(shards, {}, extension);
    }

    inline auto declarations(const std::vector<const semantic::shard*>& shards) -> std::vector<semantic::records::symbol> {
        std::map<semantic::symbol_id, std::vector<semantic::symbol_id>> children;
        std::set<semantic::symbol_id> local;
        std::vector<semantic::symbol_id> pending;
        for (const auto* shard : shards)
            for (const auto& symbol : shard->symbols) {
                if (symbol.owner)
                    children[*symbol.owner].push_back(symbol.id);
                if (std::ranges::contains(symbol.modifiers, "local") && local.insert(symbol.id).second)
                    pending.push_back(symbol.id);
            }
        while (!pending.empty()) {
            auto id = std::move(pending.back());
            pending.pop_back();
            for (const auto& child : children[id])
                if (local.insert(child).second)
                    pending.push_back(child);
        }
        std::vector<semantic::records::symbol> out;
        for (const auto* shard : shards)
            for (auto symbol : shard->symbols)
                if (!local.contains(symbol.id)) {
                    symbol.declaring_scope.reset();
                    out.push_back(std::move(symbol));
                }
        return out;
    }

    // Expression-only types do not contribute to a declaration channel. Resolve
    // the signature closure across all shards, independent of their layout.
    inline auto declaration_types(const std::vector<const semantic::shard*>& shards, const std::vector<semantic::records::symbol>& declarations)
        -> std::vector<semantic::records::type> {
        std::map<semantic::type_id, const semantic::records::type*> types;
        std::vector<semantic::type_id> pending;
        for (const auto* shard : shards) {
            for (const auto& type : shard->types)
                types.emplace(type.id, &type);
        }
        for (const auto& symbol : declarations)
            if (symbol.type)
                pending.push_back(*symbol.type);
        std::set<semantic::type_id> visited;
        std::vector<semantic::records::type> out;
        while (!pending.empty()) {
            auto id = std::move(pending.back());
            pending.pop_back();
            if (!visited.insert(id).second)
                continue;
            const auto found = types.find(id);
            if (found == types.end())
                continue;
            const auto& type = *found->second;
            out.push_back(type);
            semantic::records::each_type_reference(type, [&](const auto& child) { pending.push_back(child); });
        }
        return out;
    }

    inline auto outputs(const std::vector<semantic::channel_definition>& definitions, const std::vector<const semantic::shard*>& shards)
        -> std::vector<semantic::channel> {
        std::vector<semantic::channel> out;
        for (const auto& definition : definitions) {
            std::vector<std::pair<std::string, std::vector<std::string>>> projections;
            for (const auto& projection : definition.projections) {
                auto& values = projections.emplace_back(projection.str(), std::vector<std::string>{}).second;
                auto append = [&](const auto& rows) {
                    for (auto row : rows) {
                        if constexpr (requires { row.ownership.origin; })
                            row.ownership.origin = {};
                        if constexpr (std::same_as<std::decay_t<decltype(row)>, semantic::records::symbol>)
                            if (definition.symbol_extensions)
                                std::erase_if(row.extensions,
                                              [&](const auto& value) { return !std::ranges::contains(*definition.symbol_extensions, value.first); });
                        values.push_back(json::write(row));
                    }
                };
                if (projection == "definition_bodies")
                    append(definition_bodies(shards, definition.symbol_kinds));
                else if (projection == "declarations") {
                    const auto symbols = declarations(shards);
                    append(symbols);
                    append(declaration_types(shards, symbols));
                } else
                    for (const auto* shard : shards) {
                        if (projection == "symbols")
                            append(shard->symbols);
                        else if (projection == "types")
                            append(shard->types);
                        else if (projection == "occurrences")
                            append(shard->occurrences);
                        else if (projection == "references")
                            append(shard->references);
                        else if (projection == "scopes")
                            append(shard->scopes);
                        else if (projection == "relations")
                            append(shard->relations);
                        else if (projection == "diagnostics")
                            append(shard->diagnostics);
                        else if (projection == "typed_ranges")
                            append(shard->typed_ranges);
                    }
                std::ranges::sort(values);
                values.erase(std::unique(values.begin(), values.end()), values.end());
            }
            std::ranges::sort(projections, {}, [](const auto& value) { return value.first; });
            out.push_back({definition.name, content::hash_of(json::write(std::pair{definition, projections})).str()});
        }
        return out;
    }

    auto consume(const std::string& project, const std::string& port, const semantic::shard& shard, const std::string& name, semantic::dependency_kind kind)
        -> semantic::dependency {
        auto output = std::ranges::find(shard.unit.output_channels, name, &semantic::channel::name);
        if (output == shard.unit.output_channels.end())
            throw std::invalid_argument("semantic unit did not publish channel " + name);
        return {.kind = kind, .project = project, .key = port, .value = output->value, .producer = shard.unit.id, .channel = name};
    }

    // A body range that encompasses the symbol's declared name is a whole declaration, not a function body.
    inline auto is_body_range(const semantic::records::occurrence& row) -> bool {
        if (!row.body || row.body->end <= row.body->begin + 1)
            return false;
        if (row.selection && row.selection->path == row.body->path && row.body->begin <= row.selection->begin && row.selection->end <= row.body->end)
            return false;
        return std::ranges::contains(row.roles, semantic::role::make<"definition">());
    }

    // Conservative soundness check: edits strictly inside a sealed body cannot change any external declaration,
    // type signature, template instantiation, constexpr value, or cross-TU interface.
    inline auto is_sealed_body(const semantic::records::symbol& symbol, const semantic::records::occurrence& row,
                               const std::map<semantic::type_id, const semantic::records::type*>& types) -> bool {
        if (!is_body_range(row))
            return false;
        const auto kind = symbol.kind.str();
        if (kind != "function" && kind != "method" && kind != "constructor" && kind != "destructor")
            return false;
        const std::string_view path = row.body->path;
        static constexpr std::array<std::string_view, 13> interface_suffixes{
            ".h", ".hh", ".hpp", ".hxx", ".h++", ".ipp", ".tpp", ".inl", ".cppm", ".ixx", ".mpp", ".fsi", ".d.ts"};
        if (std::ranges::any_of(interface_suffixes, [&](std::string_view suffix) { return path.ends_with(suffix); }))
            return false;
        static constexpr std::array<std::string_view, 8> penetrating_modifiers{
            "inline", "constexpr", "consteval", "template", "generic", "macro", "const", "extern_inline"};
        for (const auto& modifier : symbol.modifiers)
            if (std::ranges::contains(penetrating_modifiers, std::string_view{modifier}))
                return false;
        auto has_deduced_spelling = [](const std::optional<std::string>& spelling) {
            return spelling && (spelling->contains("auto") || spelling->contains("decltype") || spelling->contains("impl "));
        };
        if (symbol.type) {
            if (const auto found = types.find(*symbol.type); found != types.end()) {
                const auto& type = *found->second;
                if (!type.type_parameters.empty() || has_deduced_spelling(type.name))
                    return false;
                if (type.return_type) {
                    if (const auto ret = types.find(*type.return_type); ret != types.end() && has_deduced_spelling(ret->second->name))
                        return false;
                }
            }
        }
        return true;
    }

    // Detects cross-language FFI contract endpoints (JNI, P/Invoke, pybind / C ABI exports) whose signature
    // changes break cross-language bindings and warrant immediate zero-compile warnings.
    inline auto is_ffi_boundary(const semantic::records::symbol& symbol) -> bool {
        if (symbol.kind != semantic::symbol_kind::make<"function">() && symbol.kind != semantic::symbol_kind::make<"method">())
            return false;
        auto ext_str = [&](std::string_view key) -> std::string {
            if (auto it = symbol.extensions.find(std::string{key}); it != symbol.extensions.end())
                return json::from_value<std::string>(it->second).value_or(std::string{});
            return {};
        };
        const bool is_external = std::ranges::contains(symbol.modifiers, "external");
        const bool is_native = std::ranges::contains(symbol.modifiers, "native");
        const bool is_pinvoke = std::ranges::contains(symbol.modifiers, "pinvoke");
        if (is_pinvoke || !ext_str("clr.pinvoke.entry_point").empty())
            return true;
        if ((symbol.domain == "kotlin" && is_external) || ((is_external || is_native) && (symbol.domain == "scala" || !ext_str("jvm.class").empty())))
            return true;
        const auto linkage = ext_str("native.linkage_name");
        const std::string_view effective_name = linkage.empty() ? std::string_view{symbol.name} : std::string_view{linkage};
        if (effective_name.starts_with("Java_"))
            return true;
        if (symbol.language == "rust" &&
            (std::ranges::contains(symbol.modifiers, "no_mangle") || std::ranges::contains(symbol.modifiers, "export_name")))
            return true;
        if (!ext_str("native.jni_registrations").empty())
            return true;
        return false;
    }
} // namespace channels
