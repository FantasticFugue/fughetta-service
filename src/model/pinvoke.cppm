export module pinvoke;
import std;
import evaluation;
import coroutine;
import reflection;
import records;
import channels;
import objects;
export import provider;

export namespace pinvoke {
    class resolver;
    struct group {
        std::string project, module, library, native;
        auto operator<=>(const group&) const = default;
    };
    struct options {
        // DLL import name -> native artifact module. Selection is explicit;
        // target OS loader search paths do not come from the worker host.
        std::map<std::string, std::string> libraries;
    };
    inline auto property(const semantic::records::symbol& symbol, const std::string& key) -> std::string {
        auto found = symbol.extensions.find(key);
        return found == symbol.extensions.end() ? "" : rfl::from_generic<std::string>(found->second).value();
    }
    inline auto resolve(const semantic::recipe& recipe, const options& config, const std::vector<const semantic::shard*>& inputs,
                        const std::optional<group>& selected_group = {}) -> semantic::shard {
        semantic::shard out;
        out.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
        // The adapter supplies collection reads and a runtime identity before
        // allowing reuse. Direct projection callers do not have installed ports.
        out.unit.cacheable = false;
        out.unit.coverage.available = {{.contexts = {out.unit.context}, .facts = {std::string{"relations"}, std::string{"diagnostics"}}}};
        struct target {
            const semantic::shard* shard;
            const semantic::records::symbol* symbol;
            semantic::artifact_location location;
        };
        std::unordered_map<const semantic::shard*, semantic::record_lookup> lookups;
        for (const auto* shard : inputs)
            lookups.try_emplace(shard, *shard);
        std::map<std::pair<std::string, std::string>, std::vector<target>> exports;
        for (const auto* shard : inputs) {
            for (const auto& symbol : shard->symbols) {
                if (symbol.domain != "native" || !std::ranges::contains(symbol.modifiers, "export"))
                    continue;
                for (const auto* occurrence : lookups.at(shard).positions(symbol.id))
                    if (occurrence->location.artifact && std::ranges::contains(occurrence->roles, std::string{"definition"}))
                        exports[{symbol.module, property(symbol, "native.linkage_name")}].push_back({shard, &symbol, *occurrence->location.artifact});
            }
        }
        std::set<std::string> identities;
        auto retain = [&](const semantic::shard& shard, const std::string& symbol) {
            if (!identities.insert(symbol).second)
                return;
            const auto* found = lookups.at(&shard).identity(symbol);
            if (!found)
                throw std::invalid_argument("binding endpoint has no declaration identity");
            out.identities.push_back(*found);
        };
        for (const auto* shard : inputs)
            for (const auto& symbol : shard->symbols) {
                if (symbol.domain != "clr" || !std::ranges::contains(symbol.modifiers, "pinvoke"))
                    continue;
                if (selected_group && (shard->unit.recipe.unit.project != selected_group->project || symbol.module != selected_group->module ||
                                       property(symbol, "clr.pinvoke.library") != selected_group->library))
                    continue;
                const auto library = property(symbol, "clr.pinvoke.library"), entry = property(symbol, "clr.pinvoke.entry_point");
                const auto selected = config.libraries.find(library);
                semantic::records::library_selection selection{.name = library, .strategy = std::string{"exact"}};
                std::vector<target> targets;
                if (selected != config.libraries.end()) {
                    if (auto found = exports.find({selected->second, entry}); found != exports.end())
                        targets = found->second;
                }
                for (const auto& target : targets)
                    selection.candidates.push_back(target.location);
                const auto occurrences = lookups.at(shard).positions(symbol.id);
                const auto location = occurrences.empty() ? semantic::locator{} : occurrences.front()->location;
                const semantic::ownership owner{out.unit.id, out.unit.context, {.provider = "pinvoke", .kind = std::string{"resolver"}, .location = location}};
                const auto id = out.unit.id + ":" + symbol.id;
                if (targets.size() == 1)
                    selection.selected = targets.front().location;
                if (targets.size() != 1 && (!selected_group || selected_group->native.empty())) {
                    ++out.unit.coverage.unresolved_count;
                    out.diagnostics.push_back({.id = id,
                                               .producer = "pinvoke",
                                               .code = selected == config.libraries.end() ? "library_unmapped"
                                                       : targets.empty()                  ? "entry_missing"
                                                                                          : "entry_ambiguous",
                                               .severity = "warning",
                                               .message = "P/Invoke " + library + "!" + entry + " requires one native export",
                                               .ownership = owner,
                                               .location = location});
                }
                for (const auto& target : targets) {
                    if (selected_group && (selected_group->native.empty() || target.shard->unit.id != selected_group->native))
                        continue;
                    retain(*shard, symbol.id);
                    retain(*target.shard, target.symbol->id);
                    semantic::records::binding_detail detail{
                        .mechanism = std::string{"pinvoke"},
                        .convention = decltype(semantic::records::binding_detail::convention){property(symbol, "clr.pinvoke.convention")},
                        .library_selection = selection,
                        .entry_point = entry};
                    if (const auto policy = symbol.extensions.find("clr.pinvoke.marshalling"); policy != symbol.extensions.end())
                        detail.marshalling = rfl::from_generic<semantic::records::import_marshalling>(policy->second).value();
                    struct metadata {
                        std::uint32_t sequence = 0;
                        semantic::records::parameter_marshalling value;
                    };
                    std::map<std::uint32_t, semantic::records::parameter_marshalling> parameters;
                    if (const auto found = symbol.extensions.find("clr.pinvoke.parameters"); found != symbol.extensions.end()) {
                        auto decoded = rfl::from_generic<std::vector<metadata>>(found->second).value();
                        for (auto& parameter : decoded)
                            parameters.emplace(parameter.sequence, std::move(parameter.value));
                    }
                    if (symbol.type) {
                        const auto* signature = lookups.at(shard).type(*symbol.type);
                        if (signature) {
                            auto mapping = [&](const std::string& type, semantic::records::parameter_slot slot, std::uint32_t sequence) {
                                static const std::set<std::string> scalars{"System.SByte",  "System.Byte",   "System.Int16",  "System.UInt16",
                                                                           "System.Int32",  "System.UInt32", "System.Int64",  "System.UInt64",
                                                                           "System.Single", "System.Double", "System.IntPtr", "System.UIntPtr"};
                                const auto* value = lookups.at(shard).type(type);
                                const auto name = value ? value->name.value_or("") : "";
                                if (slot.kind == "return_value" && name == "System.Void")
                                    return;
                                semantic::records::parameter_mapping result{slot, slot, std::string{scalars.contains(name) ? "identity" : "custom"}};
                                if (const auto found = parameters.find(sequence); found != parameters.end()) {
                                    result.marshalling = found->second;
                                    const auto& metadata = found->second;
                                    if (metadata.native) {
                                        const auto& native = metadata.native->name;
                                        static const std::set<std::string> strings{"BStr",      "LPStr",    "LPWStr", "LPTStr",
                                                                                   "ByValTStr", "AnsiBStr", "TBStr",  "LPUTF8Str"};
                                        static const std::set<std::string> arrays{"LPArray", "SafeArray", "ByValArray"};
                                        // Preserve MarshalAs exactly. Only matching
                                        // scalar representations can be identity;
                                        // narrowing, bools and custom marshalers
                                        // remain explicit marshaling operations.
                                        static const std::map<std::string, std::string> scalar_native{
                                            {"System.SByte", "I1"},  {"System.Byte", "U1"},   {"System.Int16", "I2"},      {"System.UInt16", "U2"},
                                            {"System.Int32", "I4"},  {"System.UInt32", "U4"}, {"System.Int64", "I8"},      {"System.UInt64", "U8"},
                                            {"System.Single", "R4"}, {"System.Double", "R8"}, {"System.IntPtr", "SysInt"}, {"System.UIntPtr", "SysUInt"}};
                                        const auto scalar = scalar_native.find(metadata.managed_type);
                                        result.conversion = std::string{strings.contains(native)                                    ? "string"
                                                                        : arrays.contains(native)                                   ? "array"
                                                                        : scalar != scalar_native.end() && scalar->second == native ? "identity"
                                                                                                                                    : "custom"};
                                    } else if (metadata.layout)
                                        result.conversion = std::string{"custom"};
                                    else if (metadata.by_reference || (value && value->kind == "pointer"))
                                        result.conversion = std::string{"pointer"};
                                    else if (metadata.managed_type == "System.String")
                                        result.conversion = std::string{"string"};
                                    else if (value && value->kind == "array")
                                        result.conversion = std::string{"array"};
                                }
                                detail.parameter_mapping.push_back(std::move(result));
                            };
                            std::uint64_t index = 0;
                            for (const auto& group : signature->parameter_groups)
                                for (const auto& parameter : group.parameters) {
                                    mapping(parameter.type, {.kind = std::string{"parameter"}, .index = index}, static_cast<std::uint32_t>(index + 1));
                                    ++index;
                                }
                            if (signature->return_type)
                                mapping(*signature->return_type, {.kind = std::string{"return_value"}}, 0);
                        }
                    }
                    out.relations.push_back({.id = id + ":" + target.symbol->id,
                                             .kind = std::string{"binds"},
                                             .ownership = owner,
                                             .from = symbol.id,
                                             .to = target.symbol->id,
                                             .resolution = std::string{targets.size() == 1 ? "resolved" : "candidate"},
                                             .detail = std::move(detail),
                                             .from_context = symbol.ownership.context,
                                             .to_context = target.symbol->ownership.context});
                }
            }
        return out;
    }
} // namespace pinvoke

export namespace language {
    template <> struct server<"pinvoke"> {
        using adapter = pinvoke::resolver;
        using options = pinvoke::options;
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 0> documents{};
        static constexpr std::array<std::string_view, 2> inputs{"managed", "native"};
        static constexpr std::array<std::string_view, 1> operations{"evaluate"}, roles{"relation-resolver"};
        static constexpr std::array<std::string_view, 2> facts{"relations", "diagnostics"};
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
        static auto requirements(const options&) -> builds::requirements { return {}; }
        static void validate(const options& value) {
            for (const auto& [library, module] : value.libraries)
                if (library.empty() || module.empty())
                    throw std::invalid_argument("P/Invoke library mappings require names and modules");
        }
        static auto launch(const options& value, const std::vector<builds::context>&) -> program {
            return {.provider = "pinvoke", .initialization = rfl::to_generic(value)};
        }
    };
} // namespace language

export namespace pinvoke {
    class resolver : public language::reader_adapter {
    public:
        using language::reader_adapter::reader_adapter;

    protected:
        auto do_open(rfl::Generic value) -> coroutine::task<> override {
            config_ = rfl::from_generic<options>(value).value();
            co_return;
        }
        auto do_units(const semantic::extent&) -> coroutine::task<std::vector<semantic::unit_key>> override {
            std::set<group> groups;
            if (const auto managed = installed_.source_facts.find("managed"); managed != installed_.source_facts.end())
                for (const auto& object : managed->second)
                    for (const auto& symbol : object->data->symbols)
                        if (symbol.domain == "clr" && std::ranges::contains(symbol.modifiers, "pinvoke"))
                            groups.insert({object->data->unit.recipe.unit.project, symbol.module, property(symbol, "clr.pinvoke.library")});
            const auto managed_groups = groups;
            for (const auto& group : managed_groups) {
                semantic::record_selection native;
                if (const auto found = config_.libraries.find(group.library); found != config_.libraries.end())
                    native.modules = {found->second};
                if (native.modules.empty())
                    continue;
                if (const auto objects = installed_.source_facts.find("native"); objects != installed_.source_facts.end())
                    for (const auto& object : objects->second)
                        if (channels::selects(object->data->unit, native)) {
                            auto selected = group;
                            selected.native = object->data->unit.id;
                            groups.insert(std::move(selected));
                        }
            }
            std::vector<semantic::unit_key> out;
            for (const auto& group : groups)
                out.push_back({state_.project, state_.stage, state_.provider, "evaluate", semantic::unit_kind::group, rfl::json::write(group)});
            co_return out;
        }
        auto do_evaluate(const language::evaluation_request& request) -> coroutine::task<language::evaluated_unit> override {
            const auto selected = rfl::json::read<group>(request.unit.name).value();
            semantic::record_selection managed{.projects = {selected.project}, .modules = {selected.module}};
            semantic::record_selection native;
            if (const auto library = config_.libraries.find(selected.library); library != config_.libraries.end())
                native.modules.push_back(library->second);
            std::vector<const semantic::shard*> shards;
            std::vector<semantic::dependency> reads;
            std::vector<const semantic::unit*> native_units;
            for (const std::string port : {"managed", "native"}) {
                if (port == "native" && native.modules.empty())
                    continue;
                const auto& selection = port == "managed" ? managed : native;
                std::vector<const semantic::unit*> units;
                if (const auto objects = installed_.source_facts.find(port); objects != installed_.source_facts.end())
                    for (const auto& object : objects->second) {
                        units.push_back(&object->data->unit);
                        if (channels::selects(object->data->unit, selection))
                            shards.push_back(object->data.get());
                    }
                // Keep the complete selected membership, including an empty
                // lookup. A new export in a configured library must invalidate it.
                reads.push_back(channels::collection(state_.project, port, units, port == "native" ? "exports" : "linkage", selection));
                if (port == "native")
                    native_units = std::move(units);
            }
            auto shard = pinvoke::resolve({request.unit, state_.options}, config_, shards, selected);
            if (!selected.native.empty()) {
                auto selected_library = native;
                // Ambiguous bindings expose every candidate's location, so they
                // depend on all those libraries. Resolved contributions only
                // read their selected library's addresses and record bodies.
                if (std::ranges::none_of(shard.relations, [](const auto& row) { return row.resolution == "candidate"; }))
                    selected_library.units = {selected.native};
                reads.push_back(channels::collection(state_.project, "native", native_units, "linkage", selected_library));
            }
            shard.unit.modules = {selected.module};
            shard.unit.observed_dependencies = std::move(reads);
            shard.unit.direct_inputs.push_back({semantic::input_kind::runtime, "toolchain", runtime_.execution});
            shard.unit.cacheable = !runtime_.execution.empty();
            shard.unit.coverage.requested = request.demand;
            language::evaluated_unit result;
            result.shards.push_back(std::move(shard));
            co_return result;
        }

    private:
        options config_;
    };
} // namespace pinvoke
