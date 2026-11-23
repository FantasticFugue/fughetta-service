export module publication;
import utility;
import native;
import std;
import coroutine;
import registry;
import knowledge;
import protocol;
import channels;
import enrollment;
import pybind;
import constants;
import publiccache;
import publicrecord;
export import tool;

export namespace publishing {
    using tools::tool_error;

    auto python_test_updates(semantic::store& store, semantic::view& view, const semantic::snapshot& pin,
                             const std::map<std::string, std::string>& files) -> std::vector<semantic::shard> {
        const auto reports = store.python_tests(pin);
        if (reports.empty())
            return {};
        std::vector<std::shared_ptr<const semantic::shard>> sources;
        std::vector<semantic::unit_id> replaced;
        for (const auto& [unit, objects] : view.units)
            for (const auto& id : objects) {
                const auto metadata = store.object_unit(id);
                const auto& key = metadata.recipe.unit;
                if (key.project != pin.project)
                    continue;
                if (key.provider == "ty")
                    sources.push_back(store.object_records(id));
                if (key.provider == "python-runtime" && metadata.recipe.options == "test-observations")
                    replaced.push_back(unit);
            }
        for (const auto& unit : replaced)
            view.units.erase(unit);
        if (sources.empty())
            return {};
        std::vector<semantic::shard> updates;
        for (const auto& report : reports)
            if (auto shard = language::project_python_test(report.bytes, pin.project, report.name, report.digest, files, sources))
                updates.push_back(std::move(*shard));
        return updates;
    }

    auto publish_python_tests(storage::registry& registry, std::size_t core, const semantic::snapshot& pin,
                              const std::string& configuration, const std::string& execution,
                              const std::map<std::string, std::string>& expected, const std::vector<builds::exported_file>& exports,
                              const content::manifest& snapshot) -> coroutine::task<std::vector<semantic::view_id>> {
        std::vector<semantic::store::python_test_report> reports;
        for (const auto& [name, path] : expected) {
            const auto file = std::ranges::find(exports, name, &builds::exported_file::name);
            if (file == exports.end() || !file->delivery_error.empty() || file->path != std::filesystem::path{path}.filename().generic_string() ||
                !file->size || file->size > constants::semantic_object_bytes)
                throw std::invalid_argument("test observation report unavailable: " + path);
            std::string bytes;
            while (bytes.size() < file->size) {
                auto result = co_await registry.read_export(core, pin.project, {.digest = file->digest, .offset = bytes.size()});
                if (result.parts.size() != 1)
                    throw std::runtime_error("incomplete Python test observation report");
                const auto& part = result.parts.front();
                if (part.packed || part.offset != bytes.size() || part.total != file->size || part.bytes.empty() || part.bytes.size() > file->size - bytes.size())
                    throw std::runtime_error("invalid Python test observation report part");
                bytes.append(reinterpret_cast<const char*>(part.bytes.data()), part.bytes.size());
            }
            language::validate_python_test_report(bytes, execution);
            reports.push_back({configuration + "/" + path, file->digest.str(), std::move(bytes)});
        }
        std::map<std::string, std::string> files;
        for (const auto& file : snapshot.files)
            files.emplace(file.path, file.digest.str());
        co_return co_await registry.with_semantic(core, [pin, reports = std::move(reports), files = std::move(files)](semantic::store& store) {
            for (const auto& report : reports)
                store.remember_python_test(pin, report);
            std::vector<semantic::view_id> views;
            for (const auto& id : store.currents(pin.project, pin.worldline)) {
                auto view = store.describe(id);
                const auto& previous = semantic::snapshot_for(view.snapshots, pin.project);
                if (previous.version != pin.version || previous.snapshot != pin.snapshot)
                    continue;
                const auto before = view.units.size();
                auto updates = python_test_updates(store, view, pin, files);
                if (updates.empty() && view.units.size() == before)
                    continue;
                auto published = store.publish_records(std::move(view), std::move(updates), {});
                store.make_current(published);
                views.push_back(std::move(published));
            }
            return views;
        });
    }

    struct receipt_authority {
        const enrollment::store* registry = nullptr;
        std::string worker;

        auto verify(const semantic::execution_receipt& claim, const semantic::execution_receipt& received) const -> std::string {
            if (registry && claim.payload() == received.payload())
                if (auto key = registry->verify(worker, received))
                    return *key;
            throw tool_error("invalid_execution_receipt", "Worker receipt does not match the submitted execution");
        }

        template <class Request> void artifacts(const Request& request, jobs::build_report& report) const {
            report.artifact_audit.clear();
            report.stage_views.clear();
            auto claim = registry ? jobs::artifact_claim(report, request.snapshot.str(), jobs::artifact_recipe(request), registry->workspace_id(), worker)
                                  : std::nullopt;
            if (report.receipts.size() != static_cast<std::size_t>(claim.has_value()))
                throw tool_error("invalid_execution_receipt", "Worker artifact manifest has an unexpected receipt count");
            if (claim) {
                auto& received = report.receipts.front();
                auto key = verify(claim->receipt, received);
                report.artifact_audit.push_back({std::move(received), std::move(key), std::move(claim->manifest)});
            }
            report.receipts.clear();
        }
    };
    struct committed {
        std::optional<language::session_state> session;
        std::set<semantic::unit_id> keys;
        std::map<semantic::unit_id, std::vector<semantic::shard_id>> contributions;
        std::vector<semantic::shard> shards;
        std::vector<semantic::missing> missing;
        std::uint64_t reused = 0;
        std::vector<std::optional<semantic::shard_receipt>> receipts;
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
        receipt_authority signer;
        std::vector<semantic::artifact_receipt> artifacts;
        void receive(language::evaluated_unit unit);
        public_shards::units public_units;
        std::map<semantic::candidate_id, semantic::shard> public_hits;
        std::vector<std::size_t> public_outputs;
        auto validate(jobs::reuse_request request) -> coroutine::task<jobs::reuse_reply>;
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
        analysis.public_lookup = files.shard_cache() != nullptr;
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
    auto bound::validate(jobs::reuse_request request) -> coroutine::task<jobs::reuse_reply> {
        const auto& session = request.session;
        expect_session(plan, session, project, expected_snapshot, units->session);
        if (request.execution != plan.execution)
            throw tool_error("stale_analysis", "reuse inputs do not belong to this analysis");
        units->session = session;
        const auto& validators = language::providers::validators();
        reuse::environment current{sources, request.inputs, input_channels, project, validators, input_collections, std::move(request.processes)};
        jobs::reuse_reply reply;
        for (const auto& candidate : plan.candidates)
            if (reuse::accept(candidate, session, plan.demand, current, true))
                reply.accepted.push_back({candidate.id, semantic::recipe_identifier(candidate.recipe)});
        approved = reply.accepted;
        if (auto* cache = files->shard_cache()) {
            for (auto& [unit, inputs] : request.public_units) {
                if (unit.project != project || unit.stage != session.stage || unit.provider != session.provider || unit.operation != "evaluate" ||
                    !public_shards::eligible(inputs))
                    continue;
                const auto id = semantic::unit_identifier(unit);
                const auto process = current.processes.find(id);
                if ((process != current.processes.end() && !std::ranges::all_of(process->second, [&](const auto& input) {
                        return std::ranges::contains(inputs, input);
                    })) || !std::ranges::all_of(inputs, [&](const auto& input) {
                        if (input.kind == semantic::input_kind::tool)
                            return process != current.processes.end() && std::ranges::contains(process->second, input);
                        return reuse::input(input, project, current);
                    }))
                    continue;
                if (!public_units.emplace(unit, inputs).second)
                    continue;
                const semantic::recipe recipe{unit, plan.options};
                if (std::ranges::any_of(reply.accepted, [&](const auto& accepted) { return accepted.recipe == semantic::recipe_identifier(recipe); }))
                    continue;
                auto shard = co_await cache->lookup(files->scheduler(core), files->disk_pool(), recipe, std::move(inputs));
                if (!shard)
                    continue;
                semantic::candidate candidate{.id = "public-" + public_shards::lookup_key(recipe, shard->unit.direct_inputs),
                                              .recipe = recipe, .units = {shard->unit}};
                auto public_inputs = request.inputs;
                public_inputs[project].append_range(shard->unit.direct_inputs);
                reuse::environment installed{sources, public_inputs, input_channels, project, validators, input_collections, current.processes};
                if (!reuse::accept(candidate, session, plan.demand, installed, true))
                    continue;
                candidate.units.clear();
                candidate.nodes = shard->identities.size() + shard->symbols.size() + shard->types.size() + shard->occurrences.size() +
                                  shard->references.size() + shard->scopes.size() + shard->relations.size();
                approved.push_back({candidate.id, semantic::recipe_identifier(recipe)});
                public_hits.emplace(candidate.id, std::move(*shard));
                reply.public_candidates.push_back(std::move(candidate));
            }
        }
        co_return reply;
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
            units->keys.contains(semantic::unit_identifier(unit.request.unit)))
            throw tool_error("stale_analysis", "provider unit does not belong to the requested inputs");
        if (unit.receipts.size() != (signer.registry ? unit.shards.size() : 0))
            throw tool_error("invalid_execution_receipt", "Worker semantic unit has an unexpected receipt count");
        std::vector<semantic::shard_receipt> verified;
        if (signer.registry) {
            const auto workspace = signer.registry->workspace_id();
            for (std::size_t index = 0; index < unit.shards.size(); ++index) {
                auto claim = jobs::shard_claim(unit, unit.shards[index], workspace, signer.worker);
                auto& received = unit.receipts[index];
                auto key = signer.verify(claim, received);
                verified.push_back({std::move(received), std::move(key), {}});
            }
        }
        const auto key = semantic::unit_identifier(unit.request.unit);
        if (unit.reused) {
            if (!unit.shards.empty() ||
                !std::ranges::contains(approved, reuse::key{*unit.reused, semantic::recipe_identifier(semantic::recipe{unit.request.unit, plan.options})}))
                throw tool_error("invalid_worker_reply", "provider reused an unoffered unit");
            if (auto hit = public_hits.find(*unit.reused); hit != public_hits.end()) {
                units->shards.push_back(std::move(hit->second));
                units->receipts.emplace_back();
                public_hits.erase(hit);
                units->keys.insert(key);
                ++units->reused;
                return;
            }
            const auto candidate = std::ranges::find(plan.candidates, *unit.reused, &semantic::candidate::id);
            if (candidate == plan.candidates.end())
                throw tool_error("invalid_worker_reply", "unknown reused contribution");
            units->session = session;
            units->keys.insert(key);
            units->contributions[key] = candidate->shards;
            ++units->reused;
            return;
        }
        if (unit.shards.empty())
            throw tool_error("invalid_worker_reply", "an evaluated unit requires its record shard");
        const auto channels =
            plan.registration ? plan.registration->channels : language::providers::dispatch(plan.provider, []<class Provider> { return Provider::channels(); });
        const auto conventions = plan.registration
                                     ? plan.registration->conventions
                                     : language::providers::dispatch(plan.provider, []<class Provider> { return language::conventions<Provider>(); });
        for (auto& shard : unit.shards) {
            if (shard.unit.recipe != semantic::recipe{unit.request.unit, plan.options})
                throw tool_error("invalid_worker_reply", "provider records belong to another unit recipe");
            shard.unit.channel_definitions = channels;
            shard.unit.conventions = conventions;
            for (const auto& scope : shard.scopes)
                for (const auto& edge : scope.lookup)
                    if (std::ranges::none_of(conventions,
                                             [&](const semantic::language_conventions& value) { return std::ranges::contains(value.scope_edges, edge.kind); }))
                        throw tool_error("invalid_worker_reply", "provider wrote an undeclared scope edge kind: " + edge.kind);
        }
        units->session = session;
        units->keys.insert(key);
        if (verified.size() == 1 && public_units.contains(unit.request.unit))
            public_outputs.push_back(units->shards.size());
        std::ranges::move(unit.shards, std::back_inserter(units->shards));
        if (verified.empty())
            units->receipts.resize(units->shards.size());
        else
            std::ranges::move(verified, std::back_inserter(units->receipts));
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
                    item->answer(co_await validate(std::move(*request)));
                } else if (auto* request = std::get_if<jobs::shard_read>(&item->data)) {
                    item->answer(co_await read(std::move(*request)));
                } else {
                    item->answer(co_await files->read_model(core, project, std::move(std::get<jobs::model_read>(item->data))));
                }
            } catch (...) {
                item->fail(util::capture_current_exception());
            }
        }
    }
    auto bound::finish() -> coroutine::task<std::optional<semantic::view_id>> {
        if (!files || units->keys.empty())
            co_return std::nullopt;
        const auto& pin = semantic::snapshot_for(snapshots, project);
        const auto& worldline = pin.worldline;
        const auto& version = pin.version;
        nodes::node_request registration{.action = "scope_register", .worldline = worldline, .revision = version};
        const auto project_sources = sources.find(project);
        for (const auto& shard : units->shards) {
            std::map<semantic::symbol_id, const semantic::records::symbol*> symbols;
            for (const auto& symbol : shard.symbols)
                symbols.try_emplace(symbol.id, &symbol);
            std::map<semantic::type_id, const semantic::records::type*> types;
            for (const auto& type : shard.types)
                types.try_emplace(type.id, &type);
            for (const auto& occurrence : shard.occurrences) {
                // Implicit declarations share their owner's source range;
                // their identity comes from the owner and relative native key.
                if (occurrence.location.source && !std::ranges::contains(occurrence.roles, semantic::role{"implicit"}) &&
                    registration.declaration_ranges.size() < constants::scope_reference_limit)
                    registration.declaration_ranges.try_emplace(occurrence.symbol, occurrence.selection.value_or(*occurrence.location.source));
                if (channels::is_body_range(occurrence) && occurrence.location.project.value_or(project) == project &&
                    project_sources != sources.end() && project_sources->second.contains(occurrence.body->path) &&
                    registration.body_ranges.size() + registration.scope_hints.ffi_boundaries.size() < constants::scope_reference_limit) {
                    registration.body_ranges.try_emplace(occurrence.symbol, *occurrence.body);
                    if (const auto sym = symbols.find(occurrence.symbol); sym != symbols.end() && channels::is_sealed_body(*sym->second, occurrence, types))
                        registration.scope_hints.sealed_bodies.insert(occurrence.symbol);
                }
                if (occurrence.location.source && occurrence.location.project.value_or(project) == project &&
                    project_sources != sources.end() && project_sources->second.contains(occurrence.location.source->path) &&
                    !std::ranges::contains(occurrence.roles, semantic::role{"implicit"}) &&
                    (std::ranges::contains(occurrence.roles, semantic::role::make<"declaration">()) ||
                     std::ranges::contains(occurrence.roles, semantic::role::make<"definition">())) &&
                    registration.body_ranges.size() + registration.scope_hints.ffi_boundaries.size() < constants::scope_reference_limit) {
                    if (const auto sym = symbols.find(occurrence.symbol); sym != symbols.end() && channels::is_ffi_boundary(*sym->second))
                        registration.scope_hints.ffi_boundaries.try_emplace(occurrence.symbol, occurrence.selection.value_or(*occurrence.location.source));
                }
            }
        }
        std::map<std::string, semantic::source_anchor> anchors;
        if (!registration.declaration_ranges.empty() || !registration.body_ranges.empty() || !registration.scope_hints.ffi_boundaries.empty()) {
            auto reply = co_await files->node(core, project, std::move(registration), actor, false);
            if (reply.code != "ok")
                throw tool_error(reply.code, reply.reason);
            anchors = std::move(reply.declaration_anchors);
        }
        semantic::view selected{
            .snapshots = snapshots, .configuration = plan.configuration, .effective_profile = units->session->inputs, .units = units->contributions};
        selected.coverage.requested = plan.demand;
        selected.coverage.missing = units->missing;
        selected.artifact_receipts = std::move(artifacts);
        auto* cache = files->shard_cache();
        std::vector<public_shards::object> uploads;
        if (cache && cache->signer() && !public_outputs.empty()) {
            uploads = co_await io::blocking(files->scheduler(core), files->disk_pool(), [this, cache] {
                std::vector<public_shards::object> objects;
                for (auto index : public_outputs) {
                    const auto& shard = units->shards[index];
                    try {
                        if (auto value = public_shards::encode(shard, public_units.at(shard.unit.recipe.unit), *cache->signer()))
                            objects.push_back(std::move(*value));
                    } catch (const std::exception& error) {
                        logging::warn(std::format("public shard encoding failed: {}", error.what()));
                    }
                }
                return objects;
            });
        }
        auto published = co_await files->with_semantic(
            core, [selected = std::move(selected), units = units, anchors = std::move(anchors), roots = roots](semantic::store& store) mutable {
                return store.publish_records(std::move(selected), std::move(units->shards), anchors, roots, std::move(units->receipts));
            });
        for (const auto& value : uploads) {
            try {
                co_await cache->publish(files->scheduler(core), value);
            } catch (const std::exception& error) {
                logging::warn(std::format("public shard upload failed: {}", error.what()));
            }
        }
        co_return published;
    }
} // namespace publishing
