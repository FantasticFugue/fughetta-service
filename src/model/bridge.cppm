export module bridge;
import std;
import evaluation;
import coroutine;
import reflection;
import records;
import channels;
import objects;
export import provider;

// What the bridges between two link domains share: the index of the definitions a binding can land on, and the
// adapter that groups a domain's importing declarations by the libraries they load.
export namespace binding {
    inline auto property(const semantic::records::symbol& symbol, const std::string& key) -> std::string {
        auto found = symbol.extensions.find(key);
        return found == symbol.extensions.end() ? "" : rfl::from_generic<std::string>(found->second).value();
    }
    // An artifact unit publishes its exports; a source unit does not.
    inline auto artifact(const semantic::unit& unit) { return std::ranges::contains(unit.output_channels, std::string{"exports"}, &semantic::channel::name); }
    struct target {
        const semantic::shard* shard;
        const semantic::records::symbol* symbol;
        semantic::artifact_location location;
        bool registered = false, source = false;
    };

    // The definitions of the domain bindings land in: its artifacts' exports by module and link identity, and,
    // before any artifact exists, the source definitions `defines` selects, by name.
    template <language::link_domain To> struct endpoints {
        std::unordered_map<const semantic::shard*, semantic::record_lookup> lookups;
        std::map<std::pair<std::string, std::string>, std::vector<target>> exports;
        std::map<std::string, std::vector<target>> definitions;

        // `defined` sees every artifact definition of the domain, exported or not.
        template <class Defines, class Defined> endpoints(const std::vector<const semantic::shard*>& inputs, Defines defines, Defined defined) {
            auto is_definition = [](const auto* occurrence) { return std::ranges::contains(occurrence->roles, std::string{"definition"}); };
            for (const auto* shard : inputs)
                lookups.try_emplace(shard, *shard);
            for (const auto* shard : inputs)
                for (const auto& symbol : shard->symbols) {
                    if (defines(symbol)) {
                        if (std::ranges::any_of(lookups.at(shard).positions(symbol.id), is_definition))
                            definitions[symbol.name].push_back({shard, &symbol, {}, false, true});
                        continue;
                    }
                    if (symbol.domain != To::name)
                        continue;
                    for (const auto* occurrence : lookups.at(shard).positions(symbol.id))
                        if (occurrence->location.artifact && is_definition(occurrence)) {
                            const target found{shard, &symbol, *occurrence->location.artifact};
                            if (std::ranges::contains(symbol.modifiers, "export"))
                                exports[{symbol.module, property(symbol, std::string{To::linkage})}].push_back(found);
                            defined(found);
                        }
                }
        }
        explicit endpoints(const std::vector<const semantic::shard*>& inputs, auto defines) : endpoints(inputs, defines, [](const target&) {}) {}

        // Copies the declaration identity of a binding's end into the bridge's own unit, once.
        void retain(semantic::shard& out, const semantic::shard& shard, const std::string& symbol) {
            if (!retained_.insert(symbol).second)
                return;
            const auto* found = lookups.at(&shard).identity(symbol);
            if (!found)
                throw std::invalid_argument("binding endpoint has no declaration identity");
            out.identities.push_back(*found);
        }

    private:
        std::set<std::string> retained_;
    };

    // The adapter of a bridge. `Mechanism` names the bridge's `options` and its `group`, the unit of one importing
    // module and the library it loads; `grouped` places an importing declaration in its group, `libraries` names the
    // target modules a group reads, and `resolve` binds a group. A group marked `source` binds source declarations
    // to the source definitions of its own project, before any artifact exists.
    template <class Mechanism> class resolver : public language::reader_adapter {
        using group = typename Mechanism::group;

    public:
        using language::reader_adapter::reader_adapter;

    protected:
        auto do_open(rfl::Generic value) -> coroutine::task<> override {
            config_ = rfl::from_generic<typename Mechanism::options>(value).value();
            co_return;
        }
        auto do_units(const semantic::extent&) -> coroutine::task<std::vector<semantic::unit_key>> override {
            std::set<group> groups;
            if (const auto managed = installed_.source_facts.find("managed"); managed != installed_.source_facts.end())
                for (const auto& object : managed->second)
                    for (const auto& symbol : object->data->symbols)
                        if (auto found = Mechanism::grouped(object->data->unit.recipe.unit.project, symbol))
                            groups.insert(std::move(*found));
            const auto managed_groups = groups;
            for (const auto& group : managed_groups) {
                const auto native = selection(group);
                if (!group.source && native.modules.empty())
                    continue;
                if (const auto objects = installed_.source_facts.find("native"); objects != installed_.source_facts.end())
                    for (const auto& object : objects->second)
                        if (artifact(object->data->unit) != group.source && channels::selects(object->data->unit, native)) {
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
            const auto native = this->selection(selected);
            std::vector<const semantic::shard*> shards;
            std::vector<semantic::dependency> reads;
            std::vector<const semantic::unit*> native_units;
            for (const std::string port : {"managed", "native"}) {
                if (port == "native" && !selected.source && native.modules.empty())
                    continue;
                const auto& selection = port == "managed" ? managed : native;
                std::vector<const semantic::unit*> units;
                if (const auto objects = installed_.source_facts.find(port); objects != installed_.source_facts.end())
                    for (const auto& object : objects->second) {
                        // Artifact and source natives may share a port; each group reads its own kind.
                        if (port == "native" && artifact(object->data->unit) == selected.source)
                            continue;
                        units.push_back(&object->data->unit);
                        if (channels::selects(object->data->unit, selection))
                            shards.push_back(object->data.get());
                    }
                // Keep the complete selected membership, including an empty
                // lookup. A new export in a configured library must invalidate it.
                reads.push_back(channels::collection(state_.project, port, units, port == "native" && !selected.source ? "exports" : "linkage", selection));
                if (port == "native")
                    native_units = std::move(units);
            }
            auto shard = Mechanism::resolve({request.unit, state_.options}, config_, shards, selected);
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
        // The target units a group reads: its configured libraries, or the sources of its own project.
        auto selection(const group& value) const -> semantic::record_selection {
            semantic::record_selection out;
            if (value.source)
                out.projects = {value.project};
            else
                out.modules = Mechanism::libraries(config_, value);
            return out;
        }
        typename Mechanism::options config_;
    };
} // namespace binding
