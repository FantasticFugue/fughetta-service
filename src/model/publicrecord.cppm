export module publicrecord;
import std;
import records;
import content;
import json;
import archive;
import constants;

export namespace public_shards {
    using inputs = std::vector<semantic::input_object>;
    using units = std::map<semantic::unit_key, inputs>;
    inline constexpr std::string_view domain = "fughetta/public-shard";
    inline constexpr std::string_view neutral_project = "public";

    inline auto eligible(const inputs& values) -> bool {
        using enum semantic::input_kind;
        return !values.empty() && std::ranges::all_of(values, [](const auto& value) {
            return (value.kind == tool || value.kind == sdk || value.kind == platform || value.kind == package) &&
                   !value.name.empty() && value.type.empty() && content::is_hash(value.object);
        });
    }
    inline auto canonical(inputs values) -> inputs {
        std::ranges::sort(values, {}, [](const auto& value) { return std::tie(value.kind, value.name, value.object); });
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }
    inline auto lookup_key(const semantic::recipe& recipe, const inputs& values) -> std::string {
        if (!eligible(values))
            return {};
        const auto& unit = recipe.unit;
        return content::hash_of(json::write(std::tuple{domain, unit.stage, unit.provider, unit.operation, unit.kind, unit.name,
                                                       recipe.options, canonical(values)})).str();
    }
    inline auto shard_name(std::string_view id) -> bool { return id.starts_with("shard-") && content::is_hash(id.substr(6)); }

    struct entry {
        std::string key, shard_id, shard_digest;
        inputs input_set;
        std::string publisher, signature;
        auto payload() const -> std::string { return json::write(std::tie(domain, key, shard_id, shard_digest, input_set, publisher)); }
        auto trusted(const std::vector<std::string>& keys) const -> bool {
            return content::is_hash(key) && shard_name(shard_id) && content::is_hash(shard_digest) && eligible(input_set) &&
                   input_set == canonical(input_set) && std::ranges::contains(keys, publisher) &&
                   content::receipt_key::verify(publisher, payload(), signature);
        }
    };

    // Public records must form a closed graph. Rebind their ownership and native
    // identities when moving between the neutral recipe and a workspace recipe.
    inline auto rebind(semantic::shard& shard, const std::string& project) -> bool {
        auto& unit = shard.unit;
        if (!eligible(unit.direct_inputs) || !unit.observed_dependencies.empty() || !unit.cacheable || unit.resolution ||
            !unit.coverage.missing.empty() || !shard.typed_ranges.empty() || !shard.diagnostics.empty() ||
            !unit.contributions.files.empty() || unit.id != semantic::unit_identifier(unit.recipe.unit))
            return false;
        const auto previous_project = unit.recipe.unit.project;
        const auto previous_unit = unit.id;
        const auto previous_context = unit.context;
        if (previous_context != semantic::context_identifier(unit.recipe))
            return false;
        unit.recipe.unit.project = project;
        unit.id = semantic::unit_identifier(unit.recipe.unit);
        unit.context = semantic::context_identifier(unit.recipe);
        bool valid = true;
        auto scope = [&](semantic::extent& extent) {
            valid &= extent.files.empty() && !extent.selector;
            for (auto& name : extent.projects) {
                valid &= name == previous_project;
                name = project;
            }
            for (auto& context : extent.contexts) {
                valid &= context == previous_context;
                context = unit.context;
            }
        };
        scope(unit.coverage.requested);
        for (auto& extent : unit.coverage.available)
            scope(extent);
        std::set<semantic::symbol_id> symbols;
        std::set<semantic::type_id> types;
        std::set<std::string> scopes;
        for (const auto& row : shard.symbols)
            valid &= symbols.insert(row.id).second && !row.id.empty();
        for (const auto& row : shard.types)
            valid &= types.insert(row.id).second && !row.id.empty();
        for (const auto& row : shard.scopes)
            valid &= scopes.insert(row.id).second && !row.id.empty();
        auto location = [&](const semantic::locator& place) {
            valid &= !place.source && !place.spelling && !place.expansion && !place.project &&
                      (!place.artifact || (!place.artifact->input && std::ranges::any_of(unit.direct_inputs, [&](const auto& input) {
                          return (input.kind == semantic::input_kind::sdk || input.kind == semantic::input_kind::package) &&
                                 input.object == place.artifact->artifact;
                      })));
        };
        auto context = [&](std::optional<semantic::context_id>& value) {
            if (value) {
                valid &= *value == previous_context;
                value = unit.context;
            }
        };
        auto ownership = [&](semantic::ownership& value) {
            valid &= value.owner_unit == previous_unit &&
                      value.context == previous_context && !value.origin.producer &&
                      value.origin.kind != "source" && value.origin.kind != "generated" && value.origin.kind != "debug";
            value.owner_unit = unit.id;
            value.context = unit.context;
            if (value.origin.location)
                location(*value.origin.location);
        };
        auto symbol = [&](const std::optional<semantic::symbol_id>& id) { valid &= !id || symbols.contains(*id); };
        auto identity = [&](semantic::declaration_identity& value) {
            if (auto* native = std::get_if<semantic::native_identity>(&value)) {
                valid &= native->project == previous_project && native->provider == unit.recipe.unit.provider;
                native->project = project;
                symbol(native->owner);
            } else {
                valid = false;
            }
        };
        std::set<semantic::symbol_id> bindings;
        for (auto& row : shard.identities) {
            valid &= !row.source && symbols.contains(row.symbol) && bindings.insert(row.symbol).second;
            identity(row.declaration);
        }
        for (auto& row : shard.symbols) {
            ownership(row.ownership);
            symbol(row.owner);
            valid &= (!row.declaring_scope || scopes.contains(*row.declaring_scope)) && (!row.type || types.contains(*row.type));
            if (row.native_key) {
                valid &= bindings.contains(row.id);
                identity(*row.native_key);
            }
        }
        for (auto& row : shard.types) {
            symbol(row.declaration);
            symbol(row.binder);
            semantic::records::each_type_reference(row, [&](const auto& id) { valid &= types.contains(id); });
            auto arguments = [&](this auto&& self, const auto& values) -> void {
                for (const auto& argument : values) {
                    symbol(argument.declaration);
                    self(argument.elements);
                }
            };
            arguments(row.application_arguments);
        }
        for (auto& row : shard.occurrences) {
            ownership(row.ownership);
            location(row.location);
            valid &= symbols.contains(row.symbol) && !row.selection && !row.body;
        }
        for (auto& row : shard.references) {
            ownership(row.ownership);
            location(row.location);
            symbol(row.from);
            symbol(row.target);
            context(row.target_context);
            for (const auto& candidate : row.candidates)
                valid &= symbols.contains(candidate);
        }
        for (auto& row : shard.scopes) {
            ownership(row.ownership);
            location(row.location);
            symbol(row.semantic_owner);
            valid &= !row.lexical_parent || scopes.contains(*row.lexical_parent);
            for (const auto& member : row.declared_members)
                valid &= symbols.contains(member);
            for (const auto& edge : row.lookup)
                valid &= symbols.contains(edge.target);
        }
        for (auto& row : shard.relations) {
            ownership(row.ownership);
            context(row.from_context);
            context(row.to_context);
            valid &= symbols.contains(row.from) && symbols.contains(row.to) && !row.detail;
        }
        return valid;
    }

    struct object {
        entry index;
        std::string bytes;
    };
    inline auto encode(semantic::shard shard, const inputs& expected, const content::receipt_key& signer) -> std::optional<object> {
        if (canonical(shard.unit.direct_inputs) != canonical(expected) || !rebind(shard, std::string{neutral_project}))
            return {};
        shard.unit.direct_inputs = canonical(std::move(shard.unit.direct_inputs));
        shard.unit.contributions = {};
        shard.unit.output_channels.clear();
        shard.id = semantic::shard_key(shard);
        auto plain = json::write(shard);
        if (plain.size() > constants::semantic_object_bytes)
            return {};
        auto bytes = nodes::archive::pack(plain);
        entry index{lookup_key(shard.unit.recipe, shard.unit.direct_inputs), shard.id, content::hash_of(bytes).str(), shard.unit.direct_inputs,
                    signer.public_key()};
        index.signature = signer.sign(index.payload());
        return object{std::move(index), std::move(bytes)};
    }
    inline auto decode_shard(std::string_view id, std::string_view bytes) -> semantic::shard {
        auto shard = json::read<semantic::shard>(nodes::archive::unpack(bytes, constants::semantic_object_bytes)).value();
        if (shard.id != id || semantic::shard_key(shard) != id || shard.unit.recipe.unit.project != neutral_project)
            throw std::invalid_argument("invalid public shard identity");
        return shard;
    }
    inline auto decode(const entry& index, std::string_view bytes, const semantic::recipe& recipe, const inputs& expected) -> std::optional<semantic::shard> {
        if (index.key != lookup_key(recipe, expected) || index.input_set != canonical(expected) ||
            content::hash_of(bytes).str() != index.shard_digest)
            return {};
        auto shard = decode_shard(index.shard_id, bytes);
        if (shard.unit.direct_inputs != index.input_set || !rebind(shard, recipe.unit.project) || shard.unit.recipe != recipe)
            return {};
        shard.id.clear();
        return shard;
    }
}
