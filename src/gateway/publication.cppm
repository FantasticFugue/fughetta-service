export module publication;
import std;
import coroutine;
import registry;
import knowledge;
import protocol;
import channels;
export import tool;

export namespace publishing {
    using tools::tool_error;
    struct committed {
        std::optional<language::session_state> session;
        std::set<semantic::unit_id> keys;
        std::map<semantic::unit_id, std::vector<semantic::shard_id>> contributions;
        std::vector<semantic::shard> shards;
        std::vector<semantic::missing> missing;
        std::uint64_t reused = 0;
    };
    struct bound {

        storage::registry* files = nullptr;
        std::size_t core = 0;
        std::string project;
        worlds::actor actor;
        semantic::snapshot_vector snapshots;
        jobs::analysis_plan plan;
        semantic::retention_roots roots;
        semantic::source_vector sources;
        std::set<semantic::shard_id> allowed;
        semantic::snapshot_id expected_snapshot;
        reuse::channel_values input_channels;
        reuse::collection_values input_collections;
        std::vector<reuse::key> approved;
        void receive(language::evaluated_unit unit);
        auto validate(jobs::reuse_request request) -> jobs::reuse_reply;
        auto read(jobs::shard_read request) -> coroutine::task<jobs::shard_piece>;
        auto serve(jobs::analysis_link inputs) -> coroutine::task<>;
        auto finish() -> coroutine::task<std::optional<semantic::view_id>>;
        std::shared_ptr<committed> units = std::make_shared<committed>();
    };
    void expect_session(const jobs::analysis_plan& plan, const language::session_state& session, const std::string& project,
                        const semantic::snapshot_id& snapshot, const std::optional<language::session_state>& previous) {
        if (session.id.empty() || session.phase != "ready" || !session.epoch || session.project != project || session.stage != plan.execution.stage_id ||
            session.provider != plan.provider || session.options != plan.options || session.slot != plan.slot_id || session.snapshot != snapshot ||
            (previous && (previous->id != session.id || previous->epoch != session.epoch || previous->inputs != session.inputs)))
            throw tool_error("stale_analysis", "provider session does not belong to the requested inputs");
    }
    auto bind(storage::registry& files, std::size_t core, const std::string& project, semantic::snapshot_vector snapshots, worlds::actor actor,
              jobs::analysis_plan& analysis, semantic::source_vector sources, semantic::retention_roots roots) -> coroutine::task<bound> {
        const auto& pin = semantic::snapshot_for(snapshots, project);
        const auto expected_snapshot = pin.snapshot;
        if (!roots)
            roots = co_await files.with_semantic(core, [](semantic::store& store) { return store.protect(); });
        analysis.candidates =
            co_await files.with_semantic(core, [snapshots, project, options = analysis.options, sources, retained = analysis.retained,
                                                roots](semantic::store& store) { return store.offer(snapshots, project, options, sources, retained, roots); });
        std::set<semantic::shard_id> allowed;
        for (const auto& [port, ids] : analysis.source_facts)
            allowed.insert(ids.begin(), ids.end());
        auto input_channels = co_await files.with_semantic(core, [ports = analysis.source_facts, candidates = analysis.candidates](semantic::store& store) {
            reuse::channel_values channels;
            reuse::collection_values collections;
            auto selected_ports = ports;
            for (const auto& candidate : candidates)
                for (const auto& unit : candidate.units)
                    for (const auto& read : unit.observed_dependencies)
                        if (read.kind == semantic::dependency_kind::record_set)
                            selected_ports.try_emplace(read.key);
            for (const auto& [port, ids] : selected_ports) {
                std::vector<semantic::unit> units;
                std::set<std::pair<std::string, semantic::record_selection>> selections;
                for (const auto& id : ids) {
                    units.push_back(store.object_unit(id));
                    for (const auto& channel : units.back().output_channels) {
                        channels[{units.back().id, channel.name}] = channel.value;
                        selections.insert({channel.name, {}});
                    }
                }
                for (const auto& candidate : candidates)
                    for (const auto& unit : candidate.units)
                        for (const auto& read : unit.observed_dependencies)
                            if (read.kind == semantic::dependency_kind::record_set && read.key == port && read.channel)
                                selections.insert({*read.channel, read.records.value_or(semantic::record_selection{})});
                std::vector<const semantic::unit*> selected;
                for (const auto& unit : units)
                    selected.push_back(&unit);
                for (const auto& [name, selection] : selections) {
                    auto value = channels::collection_value(selected, name, selection);
                    if (!value.empty())
                        collections[{port, name, selection}] = std::move(value);
                }
            }
            return std::pair{std::move(channels), std::move(collections)};
        });
        co_return bound{.files = &files,
                        .core = core,
                        .project = project,
                        .actor = actor,
                        .snapshots = std::move(snapshots),
                        .plan = analysis,
                        .roots = std::move(roots),
                        .sources = std::move(sources),
                        .allowed = std::move(allowed),
                        .expected_snapshot = expected_snapshot,
                        .input_channels = std::move(input_channels.first),
                        .input_collections = std::move(input_channels.second)};
    }
    auto bound::read(jobs::shard_read request) -> coroutine::task<jobs::shard_piece> {
        if (!allowed.contains(request.id))
            throw tool_error("invalid_worker_request", "semantic object is not an input of this analysis");
        co_return co_await files->with_semantic(core, [request](semantic::store& store) {
            auto [total, bytes] = store.object_bytes(request.id, request.offset, request.limit);
            return jobs::shard_piece{request.offset, total, std::move(bytes)};
        });
    }
    auto bound::validate(jobs::reuse_request request) -> jobs::reuse_reply {
        const auto& session = request.session;
        expect_session(plan, session, project, expected_snapshot, units->session);
        if (request.execution != plan.execution)
            throw tool_error("stale_analysis", "reuse inputs do not belong to this analysis");
        units->session = session;
        const auto& validators = language::providers::validators();
        reuse::environment current{sources, request.inputs, input_channels, project, validators, input_collections};
        jobs::reuse_reply reply;
        for (const auto& candidate : plan.candidates)
            if (reuse::accept(candidate, session, plan.demand, current, true))
                reply.accepted.push_back({candidate.id, semantic::recipe_identifier(candidate.recipe)});
        approved = reply.accepted;
        return reply;
    }
    void bound::receive(language::evaluated_unit unit) {
        const auto& session = unit.session;
        expect_session(plan, session, project, expected_snapshot, units->session);
        if (unit.request.execution != plan.execution || unit.request.slot_id != plan.slot_id || unit.request.epoch != session.epoch ||
            unit.request.inputs != session.inputs || unit.request.demand != plan.demand || unit.request.unit.project != project ||
            unit.request.unit.stage != plan.execution.stage_id || unit.request.unit.provider != plan.provider ||
            !(plan.registration ? std::ranges::contains(plan.registration->operations, unit.request.unit.operation)
                                : language::providers::dispatch(plan.provider,
                                                                [&]<class Provider> {
                                                                    const auto operations = language::operations<Provider>();
                                                                    return std::ranges::contains(operations, unit.request.unit.operation);
                                                                })) ||
            !units->keys.insert(semantic::unit_identifier(unit.request.unit)).second)
            throw tool_error("stale_analysis", "provider unit does not belong to the requested inputs");
        units->session = session;
        if (unit.reused) {
            if (!unit.shards.empty() ||
                !std::ranges::contains(approved, reuse::key{*unit.reused, semantic::recipe_identifier(semantic::recipe{unit.request.unit, plan.options})}))
                throw tool_error("invalid_worker_reply", "provider reused an unoffered unit");
            const auto candidate = std::ranges::find(plan.candidates, *unit.reused, &semantic::candidate::id);
            if (candidate == plan.candidates.end())
                throw tool_error("invalid_worker_reply", "unknown reused contribution");
            units->contributions[semantic::unit_identifier(unit.request.unit)] = candidate->shards;
            ++units->reused;
            return;
        }
        if (unit.shards.empty())
            throw tool_error("invalid_worker_reply", "an evaluated unit requires its record shard");
        const auto channels =
            plan.registration ? plan.registration->channels : language::providers::dispatch(plan.provider, []<class Provider> { return Provider::channels(); });
        for (auto& shard : unit.shards) {
            if (shard.unit.recipe != semantic::recipe{unit.request.unit, plan.options})
                throw tool_error("invalid_worker_reply", "provider records belong to another unit recipe");
            shard.unit.channel_definitions = channels;
            units->shards.push_back(std::move(shard));
        }
        return;
    }
    auto bound::serve(jobs::analysis_link inputs) -> coroutine::task<> {
        while (auto item = co_await inputs->next()) {
            auto release = jobs::analysis_exchange::completion{item};
            try {
                if (auto* unit = std::get_if<language::evaluated_unit>(&item->data)) {
                    receive(std::move(*unit));
                    item->answer(std::monostate{});
                } else if (auto* request = std::get_if<jobs::reuse_request>(&item->data)) {
                    item->answer(validate(std::move(*request)));
                } else if (auto* request = std::get_if<jobs::shard_read>(&item->data)) {
                    item->answer(co_await read(std::move(*request)));
                } else {
                    item->answer(co_await files->read_model(core, project, std::move(std::get<jobs::model_read>(item->data))));
                }
            } catch (...) {
                item->fail(std::current_exception());
            }
        }
    }
    auto bound::finish() -> coroutine::task<std::optional<semantic::view_id>> {
        if (!files || units->keys.empty())
            co_return std::nullopt;
        const auto& pin = semantic::snapshot_for(snapshots, project);
        const auto& worldline = pin.worldline;
        const auto& version = pin.version;
        if (units->keys.empty())
            co_return std::nullopt;
        nodes::node_request registration{.action = "scope_register", .worldline = worldline, .revision = version};
        for (const auto& shard : units->shards)
            for (const auto& occurrence : shard.occurrences)
                // Implicit declarations share their owner's source range;
                // their identity comes from the owner and relative native key.
                if (occurrence.location.source && !std::ranges::contains(occurrence.roles, semantic::role{"implicit"}))
                    registration.declaration_ranges.try_emplace(occurrence.symbol, occurrence.selection.value_or(*occurrence.location.source));
        std::map<std::string, semantic::source_anchor> anchors;
        if (!registration.declaration_ranges.empty()) {
            auto reply = co_await files->node(core, project, std::move(registration), actor, false);
            if (reply.code != "ok")
                throw tool_error(reply.code, reply.reason);
            anchors = std::move(reply.declaration_anchors);
        }
        semantic::view selected{
            .snapshots = snapshots, .configuration = plan.configuration, .effective_profile = units->session->inputs, .units = units->contributions};
        selected.coverage.requested = plan.demand;
        selected.coverage.missing = units->missing;
        co_return co_await files->with_semantic(
            core, [selected = std::move(selected), units = units, anchors = std::move(anchors), roots = roots](semantic::store& store) mutable {
                return store.publish_records(std::move(selected), std::move(units->shards), anchors, roots);
            });
    }
} // namespace publishing
