export module tools:analysis;
import utility;
import constants;
import std;
import knowledge;
import json;
import coroutine;
import meta;
import protocol;
import mcp;
import configuration;
import usage;
import world;
import codec;
import archive;
import registry;
import fleet;
import commands;
import channels;
export import templates;
export import publication;
import native;
import syntax;

export import :execution;

export namespace tools {
    struct language_query
        : tool<"language", "Query scopes, symbols, definitions, references or diagnostics for a worldline version; schema reads semantic contracts. "
                           "Analysis is prepared automatically within the call when missing or stale. Inspecting a callable returns its signature, "
                           "one-hop type surface, input producers and result consumers. inspect returns and registers body text so a later worldline "
                           "apply locks the enclosing function and reports its reach. Without analysis configuration, inspect returns a syntax outline."> {
        struct scope_filter {
            std::optional<std::vector<std::string>> modules, files, contexts, projects;
            auto extent() const -> semantic::extent {
                semantic::extent result;
                result.modules = modules.value_or(std::vector<std::string>{});
                result.files = files.value_or(std::vector<std::string>{});
                result.contexts = contexts.value_or(std::vector<std::string>{});
                result.projects = projects.value_or(std::vector<std::string>{});
                return result;
            }
        };
        struct input {
            json::described<"Project; supplied by the execution context when absent", std::optional<std::string>> project;
            json::described<"Worldline; task binding when absent; explicit selection required without a task", std::optional<std::string>> worldline;
            json::described<"Version to query; the current one when absent", std::optional<std::string>> version;
            json::described<"Analysis configuration; the project's only one when absent", std::optional<std::string>> configuration;
            json::described<"scopes, symbols, definitions, references, relations, impact, diagnostics (default), inspect, plan, describe or schema",
                             std::optional<semantic::tool_action>>
                action;
            json::described<"Filter describe by provider ID; all registered providers when absent", std::optional<std::string>> provider;
            std::optional<scope_filter> scope;
            json::described<"name selects a symbol prefix; entity identifies a declaration in a published view", std::optional<semantic::selection>> selector;
            json::described<"Continue a published immutable analysis view", std::optional<std::string>> view;
            std::optional<std::string> from_view, to_view;
            std::optional<std::vector<std::string>> outputs;
            std::optional<std::vector<std::string>> include;
            json::described<"Include verified Worker execution receipts for this view", std::optional<bool>> execution_receipts;
            std::optional<std::uint64_t> limit;
            std::optional<std::string> cursor;
            json::described<"Resume the query retained by a partial result; budget may be overridden", std::optional<std::string>> continuation;
            std::optional<std::vector<semantic::relation_kind>> relations;
            json::described<"Impact change kinds: declaration and/or body; both when omitted", std::optional<std::vector<std::string>>> changes;
            std::optional<json::literal<"outgoing", "incoming", "both">> direction;
            std::optional<std::uint64_t> depth;
            json::described<"Analysis time in milliseconds, document units and returned semantic nodes; view queries use nodes for type expansion",
                             std::optional<semantic::budget>>
                budget;
        };
        struct outline_item {
            std::string name, kind;
            semantic::place source;
        };
        struct output {
            builds::timings timings;
            std::uint64_t reused_units = 0;
            std::string configuration, worldline;
            std::optional<std::string> environment, version, view;
            bool stale = false;
            bool analyzed = false;
            std::optional<json::value> schema;
            std::optional<std::vector<language::description>> providers;
            std::optional<language::graph> plan;
            semantic::snapshot_vector snapshots;
            std::string status = "complete";
            semantic::coverage coverage;
            std::vector<semantic::item> items;
            std::optional<std::vector<outline_item>> outline;
            std::vector<semantic::records::symbol> symbols, producers, consumers;
            std::vector<semantic::records::type> types;
            std::vector<semantic::records::typed_range> typed_ranges;
            std::vector<semantic::records::occurrence> occurrences;
            std::vector<semantic::records::reference> references;
            std::vector<semantic::records::scope> scopes;
            std::vector<semantic::records::relation> relations;
            std::vector<semantic::records::diagnostic> diagnostics;
            std::vector<semantic::impact_path> impact;
            std::optional<semantic::impact_comparison> comparison;
            std::uint64_t query_nodes = 0;
            std::optional<std::string> cursor, code, message;
            std::optional<std::string> continuation, next_view;
            std::string column_encoding = "unicode_scalar";
            std::vector<semantic::shard_receipt> shard_receipts;
            std::vector<semantic::artifact_receipt> artifact_receipts;
        };
        struct dependency_execution {
            std::map<std::string, semantic::snapshot> pins;
            std::set<std::pair<std::string, std::string>> active;
            std::map<std::string, output> completed;
            std::vector<semantic::retention_roots> roots;
            std::map<semantic::unit_id, std::vector<semantic::shard_id>> retained;
            std::map<std::string, language::dependency_computation> computations;
        };
        static auto declared_dependencies(services& s, const std::string& project, const std::string& configuration) -> std::vector<language::project_input> {
            std::vector<language::project_input> result;
            std::set<std::pair<std::string, std::string>> active;
            auto visit = [&](this auto&& self, const std::string& owner, const std::string& name, const std::vector<std::string>& outputs) -> void {
                if (!active.emplace(owner, name).second)
                    throw tool_error(nodes::code::invalid_record, "cross-project analysis dependency cycle: " + owner + "/" + name);
                const auto settings = config::resolve(*s.snapshot(), owner);
                const auto found = settings.analysis.find(name);
                if (found == settings.analysis.end())
                    throw tool_error(nodes::code::invalid_record, "dependency analysis configuration not found: " + owner + "/" + name);
                std::map<std::string, builds::configuration> builds;
                for (const auto& [id, config] : settings.configs)
                    builds.emplace(id, config.build);
                const auto graph = language::expand(found->second, builds, outputs, settings.analysis_defaults);
                for (const auto& stage : graph.stages)
                    for (const auto& [port, producers] : found->second.stages.at(stage).inputs)
                        for (const auto& producer : producers)
                            if (const auto* dependency = std::get_if<language::project_input>(&producer)) {
                                result.push_back(*dependency);
                                self(dependency->project, dependency->configuration, {dependency->output});
                            }
                active.erase({owner, name});
            };
            visit(project, configuration, {});
            return result;
        }
        static auto read_actor(const worlds::actor& actor, const std::string& project, const semantic::snapshot& pin,
                               const std::vector<language::project_input>& declarations) -> worlds::actor {
            if (pin.project == project) {
                if (actor.dependency && (actor.dependency->project != project || actor.dependency->worldline != pin.worldline))
                    throw tool_error(nodes::code::permission_denied, "view differs from dependency read grant");
                auto result = actor;
                if (result.dependency)
                    result.dependency->version = pin.version;
                return result;
            }
            if (!std::ranges::any_of(declarations, [&](const auto& dependency) {
                    return dependency.project == pin.project && dependency.worldline == pin.worldline &&
                           (!dependency.version || *dependency.version == pin.version);
                }))
                throw tool_error(nodes::code::permission_denied, "view contains an undeclared project dependency: " + pin.project);
            auto result = actor;
            if (!result.execution_token.empty())
                result.dependency = worlds::dependency_read{pin.project, pin.worldline, pin.version};
            return result;
        }
        static void compact_coverage(semantic::coverage& coverage) {
            std::set<std::string> scopes;
            std::erase_if(coverage.available, [&](const auto& scope) { return !scopes.insert(json::write(scope)).second; });

            std::map<std::string, std::size_t> groups;
            auto entries = std::exchange(coverage.missing, {});
            for (auto& entry : entries) {
                auto key = json::write(std::tie(entry.scope, entry.reason, entry.phase, entry.deferred));
                auto [found, inserted] = groups.try_emplace(std::move(key), coverage.missing.size());
                if (inserted)
                    coverage.missing.push_back(std::move(entry));
                else {
                    auto& units = coverage.missing[found->second].units;
                    std::ranges::move(entry.units, std::back_inserter(units));
                }
            }
            for (auto& entry : coverage.missing) {
                std::ranges::sort(entry.units);
                entry.units.erase(std::ranges::unique(entry.units).begin(), entry.units.end());
            }
        }
        static auto read_view(services& s, const std::string& project, const std::string& id, const input& in, const worlds::actor& actor,
                              std::optional<std::string> reproject_version = std::nullopt) -> coroutine::task<output> {
            semantic::view description;
            std::optional<semantic::view> earlier;
            semantic::retention_roots reading;
            try {
                std::tie(description, earlier, reading) = co_await s.files.with_semantic(s.core, [id, from = in.from_view](semantic::store& store) {
                    std::set<semantic::view_id> ids{id};
                    std::optional<semantic::view> previous;
                    if (from) {
                        ids.insert(*from);
                        previous = store.describe(*from);
                    }
                    return std::tuple{store.describe(id), std::move(previous), store.protect(ids)};
                });
            } catch (const semantic::expired_view& e) {
                co_return output{.view = id, .status = "expired", .code = "view_expired", .message = e.what()};
            }
            const auto found = std::ranges::find(description.snapshots, project, &semantic::snapshot::project);
            if (!actor.run.empty()) {
                if (found == description.snapshots.end())
                    throw tool_error(nodes::code::permission_denied, "view is outside the task project");
                execution_line(actor, found->worldline);
                if (earlier) {
                    auto pin = std::ranges::find(earlier->snapshots, project, &semantic::snapshot::project);
                    if (pin == earlier->snapshots.end())
                        throw tool_error(nodes::code::permission_denied, "comparison view is outside the task project");
                    execution_line(actor, pin->worldline);
                }
            }
            if (found == description.snapshots.end())
                throw tool_error("not_found", "semantic view not found in this project");
            const auto& snapshot = *found;
            if ((in.worldline.value() && *in.worldline.value() != snapshot.worldline) || (in.version.value() && *in.version.value() != snapshot.version) ||
                (in.configuration.value() && *in.configuration.value() != description.configuration))
                throw tool_error(nodes::code::invalid_record, "view already fixes worldline, version and configuration");
            // Source applies the same project and task authorization as a fresh query.
            const auto declarations =
                description.snapshots.size() > 1 ? declared_dependencies(s, project, description.configuration) : std::vector<language::project_input>{};
            for (const auto& pin : description.snapshots)
                co_await selected(s, pin.project, pin.worldline, pin.version, read_actor(actor, project, pin, declarations));
            if (earlier) {
                const auto previous_declarations =
                    earlier->snapshots.size() > 1 ? declared_dependencies(s, project, earlier->configuration) : std::vector<language::project_input>{};
                for (const auto& pin : earlier->snapshots)
                    co_await selected(s, pin.project, pin.worldline, pin.version, read_actor(actor, project, pin, previous_declarations));
            }
            semantic::request request{.project = project,
                                      .worldline = snapshot.worldline,
                                      .version = snapshot.version,
                                      .configuration = description.configuration,
                                      .view = id,
                                      .from_view = in.from_view,
                                      .to_view = in.from_view ? std::optional{id} : std::nullopt,
                                      .action = in.action.value().value_or(semantic::tool_action::make<"diagnostics">()).str(),
                                      .selector = in.selector.value(),
                                      .include = in.include.value_or(std::vector<std::string>{}),
                                      .relations = in.relations.value_or(std::vector<semantic::relation_kind>{}),
                                      .changes = in.changes.value().value_or(std::vector<std::string>{}),
                                      .direction = in.direction.value_or(json::literal<"outgoing", "incoming", "both">{}),
                                      .depth = in.depth.value_or(description.defaults.relation_depth),
                                      .limit = in.limit.value_or(description.defaults.fact_limit),
                                      .cursor = in.cursor,
                                      .budget = semantic::tighten(description.defaults.budget, in.budget.value().value_or(semantic::budget{}))};
            request.scope = in.scope.value_or(scope_filter{}).extent();
            semantic::result result;
            try {
                result = co_await s.files.with_semantic(s.core, [id, request](semantic::store& store) {
                    store.renew(id);
                    if (request.selector && request.selector->entity)
                        store.require_symbol(request.selector->entity->symbol_id);
                    if (request.from_view) {
                        store.renew(*request.from_view);
                        return semantic::compare_views(*store.load(*request.from_view), *store.load(id), request);
                    }
                    return store.load(id)->query(request);
                });
            } catch (const semantic::expired_symbol& e) {
                co_return output{.view = id, .status = "expired", .code = "symbol_expired", .message = e.what()};
            } catch (const std::invalid_argument& e) {
                throw tool_error(nodes::code::invalid_record, e.what());
            }
            const bool compiler_errors_only = !result.coverage.missing.empty() && std::ranges::all_of(result.coverage.missing, [](const auto& missing) {
                return missing.reason == semantic::tentative_cpp_types;
            });
            output out{.configuration = description.configuration,
                       .worldline = snapshot.worldline,
                       .version = snapshot.version,
                       .view = id,
                       .stale = result.status == "partial" && !compiler_errors_only,
                       .snapshots = result.snapshots,
                       .status = result.status.str(),
                       .coverage = std::move(result.coverage),
                       .items = std::move(result.items),
                       .symbols = std::move(result.symbols),
                       .producers = std::move(result.producers),
                       .consumers = std::move(result.consumers),
                       .types = std::move(result.types),
                       .typed_ranges = std::move(result.typed_ranges),
                       .occurrences = std::move(result.occurrences),
                       .references = std::move(result.references),
                       .scopes = std::move(result.scopes),
                       .relations = std::move(result.relations),
                       .diagnostics = std::move(result.diagnostics),
                       .impact = std::move(result.impact),
                       .comparison = std::move(result.comparison),
                       .query_nodes = result.query_nodes,
                       .cursor = std::move(result.cursor),
                       .code = std::move(result.code),
                       .message = std::move(result.message)};
            if (in.execution_receipts.value().value_or(false)) {
                out.shard_receipts = std::move(description.receipts);
                out.artifact_receipts = std::move(description.artifact_receipts);
            }
            if (auto profile = std::ranges::find_if(
                    description.effective_profile,
                    [](const auto& value) { return value.kind == semantic::input_kind::project_model && value.name == "analysis/configuration"; });
                profile != description.effective_profile.end())
                out.environment = profile->object;
            if (out.status == "partial" && !compiler_errors_only && ((!request.from_view && description.computation) ||
                                            std::ranges::all_of(out.coverage.missing, [](const auto& missing) { return missing.phase == "query"; }))) {
                out.continuation = co_await s.files.with_semantic(s.core, [id, request, cursor = out.cursor](semantic::store& store) mutable {
                    request.cursor = std::move(cursor);
                    return json::write(store.remember_query(id, std::move(request)));
                });
            }
            struct source_positions {
                std::vector<std::uint64_t> lines{0};
                std::uint64_t size = 0;
            };
            std::map<std::pair<std::string, std::string>, source_positions> positions;
            const bool sources = request.action == "inspect" || std::ranges::contains(request.include, "source");
            std::map<std::pair<std::string, std::string>, std::string> texts;
            // Semantic offsets count code points.
            auto slice = [](std::string_view text, const semantic::place& range) {
                std::size_t byte = 0, begin = text.size();
                for (std::uint64_t point = 0; byte < text.size() && point < range.end; ++point) {
                    if (point == range.begin)
                        begin = byte;
                    const auto c = static_cast<unsigned char>(text[byte]);
                    byte += c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
                }
                return std::string{text.substr(begin, std::min(byte, text.size()) - std::min(begin, byte))};
            };
            auto enlist = [&](const semantic::locator& at) {
                if (at.source)
                    positions.try_emplace(std::pair{at.project.value_or(project), at.source->path});
            };
            for (const auto& row : out.occurrences) {
                enlist(row.location);
                if (sources && row.body && std::ranges::contains(out.items, std::pair{row.symbol, row.ownership.context}, [](const auto& item) {
                        return std::pair{item.symbol_id, item.context};
                    }))
                    positions.try_emplace(texts.try_emplace(std::pair{row.location.project.value_or(project), row.body->path}).first->first);
            }
            for (const auto& row : out.references)
                enlist(row.location);
            for (const auto& row : out.scopes)
                enlist(row.location);
            for (const auto& row : out.diagnostics) {
                enlist(row.location);
                for (const auto& at : row.related_locations)
                    enlist(at);
            }
            const bool reprojecting = reproject_version && *reproject_version != snapshot.version;
            if (reprojecting) {
                std::vector<semantic::place*> targets;
                std::vector<semantic::place> places;
                auto track = [&](std::optional<semantic::place>& place, const std::optional<std::string>& owner) {
                    if (place && owner.value_or(project) == project) {
                        targets.push_back(&*place);
                        places.push_back(*place);
                    }
                };
                for (auto& row : out.occurrences) {
                    track(row.location.source, row.location.project);
                    track(row.selection, row.location.project);
                    track(row.body, row.location.project);
                }
                for (auto& row : out.references)
                    track(row.location.source, row.location.project);
                for (auto& row : out.scopes)
                    track(row.location.source, row.location.project);
                for (auto& row : out.diagnostics) {
                    track(row.location.source, row.location.project);
                    for (auto& at : row.related_locations)
                        track(at.source, at.project);
                }
                if (!places.empty()) {
                    auto reply = co_await node_call(
                        s, project,
                        {.action = "scope_classify",
                         .worldline = snapshot.worldline,
                         .revision = *reproject_version,
                         .target = snapshot.version,
                         .scope_hints = {.reproject_places = std::move(places)}},
                        read_actor(actor, project, snapshot, declarations), true);
                    if (!reply.classification ||
                        (!reply.classification->sealed_body_only && !reply.classification->comment_or_whitespace_only) ||
                        reply.reprojected.size() != targets.size() ||
                        std::ranges::any_of(reply.reprojected, [](const auto& item) { return !item.has_value(); }))
                        throw tool_error("reprojection_stale", "CRDT anchor reprojection unavailable; full analysis required");
                    for (std::size_t i = 0; i < targets.size(); ++i)
                        *targets[i] = *reply.reprojected[i];
                }
                out.version = *reproject_version;
                for (auto& pin : out.snapshots)
                    if (pin.project == project)
                        pin.version = *reproject_version;
            }
            for (auto& [key, index] : positions) {
                const auto& [input_project, path] = key;
                auto pin = semantic::snapshot_for(description.snapshots, input_project);
                if (reprojecting && input_project == project)
                    pin.version = *reproject_version;
                auto source = co_await node_call(s, input_project, {.action = "read", .worldline = pin.worldline, .revision = pin.version, .target = path},
                                                 read_actor(actor, project, pin, declarations), true);
                const auto found = std::ranges::find(source.files, path, &nodes::file_view::path);
                if (found == source.files.end())
                    throw tool_error("invalid_semantic_range", "semantic location has no file in its snapshot");
                const auto text = found->encoding == "hex" ? lines::unhex(found->text) : found->text;
                if (const auto wanted = texts.find(key); wanted != texts.end())
                    wanted->second = text;
                for (std::size_t byte = 0; byte < text.size();) {
                    const auto c = static_cast<unsigned char>(text[byte]);
                    ++index.size;
                    if (c == '\n' || (c == '\r' && (byte + 1 == text.size() || text[byte + 1] != '\n')))
                        index.lines.push_back(index.size);
                    byte += c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
                }
            }
            auto locate = [&](semantic::locator& at) {
                if (!at.project)
                    at.project = project;
                if (!at.source)
                    return;
                const auto& range = *at.source;
                const auto& index = positions.at({at.project.value_or(project), range.path});
                if (range.begin > range.end || range.end > index.size)
                    throw tool_error("invalid_semantic_range", "semantic range exceeds its source snapshot");
                auto point = [&](std::uint64_t offset) {
                    const auto line = std::upper_bound(index.lines.begin(), index.lines.end(), offset) - index.lines.begin();
                    return std::pair{static_cast<std::uint64_t>(line), offset - index.lines.at(line - 1) + 1};
                };
                const auto [line, column] = point(range.begin);
                const auto [end_line, end_column] = point(range.end);
                at.position = semantic::coordinates{line, column, end_line, end_column};
            };
            std::map<std::pair<std::string, std::string>, std::vector<const semantic::records::occurrence*>> occurrences;
            for (auto& row : out.occurrences) {
                locate(row.location);
                occurrences[{row.symbol, row.ownership.context}].push_back(&row);
            }
            for (auto& row : out.references)
                locate(row.location);
            for (auto& row : out.scopes)
                locate(row.location);
            for (auto& row : out.diagnostics) {
                locate(row.location);
                for (auto& at : row.related_locations)
                    locate(at);
            }
            for (auto& item : out.items) {
                std::set<semantic::locator> definitions, declarations;
                for (const auto* occurrence : occurrences[{item.symbol_id, item.context}]) {
                    if (std::ranges::contains(occurrence->roles, semantic::role::make<"definition">())) {
                        definitions.insert(occurrence->location);
                        const auto text = occurrence->body ? texts.find({occurrence->location.project.value_or(project), occurrence->body->path}) : texts.end();
                        if (!item.source && text != texts.end())
                            item.source = slice(text->second, *occurrence->body);
                    }
                    if (std::ranges::contains(occurrence->roles, semantic::role::make<"declaration">()))
                        declarations.insert(occurrence->location);
                }
                item.definitions.assign(definitions.begin(), definitions.end());
                item.declarations.assign(declarations.begin(), declarations.end());
            }
            // Bodies and scopes that inspect or definitions return register
            // silently, so a later text edit snaps to them; only a scope
            // request replies with their references.
            const bool reveal = request.action == "scopes" || std::ranges::contains(request.include, "scope");
            if (reveal || request.action == "inspect" || request.action == "definitions") {
                std::map<std::string, nodes::node_request> grouped;
                std::map<std::string, std::size_t> offsets;
                auto group = [&](const std::string& input_project) -> nodes::node_request& {
                    auto [found, created] = grouped.try_emplace(input_project);
                    if (created) {
                        const auto& pin = semantic::snapshot_for(description.snapshots, input_project);
                        found->second = {.action = "scope_register", .worldline = pin.worldline, .revision = pin.version};
                        found->second.configuration = description.configuration;
                        found->second.environment = out.environment.value_or("");
                    }
                    return found->second;
                };
                for (std::size_t i = 0; i < out.scopes.size(); ++i) {
                    const auto& row = out.scopes[i];
                    if (row.location.source) {
                        group(row.location.project.value_or(project)).analyzed_scopes.push_back(row);
                        offsets.emplace(row.id, i);
                    }
                }
                std::map<semantic::symbol_id, const semantic::records::symbol*> symbols_by_id;
                for (const auto& sym : out.symbols)
                    symbols_by_id.try_emplace(sym.id, &sym);
                std::map<semantic::type_id, const semantic::records::type*> types_by_id;
                for (const auto& type : out.types)
                    types_by_id.try_emplace(type.id, &type);
                for (const auto& row : out.occurrences)
                    if (channels::is_body_range(row) &&
                        std::ranges::contains(out.items, std::pair{row.symbol, row.ownership.context},
                                              [](const auto& item) { return std::pair{item.symbol_id, item.context}; })) {
                        auto& reg = group(row.location.project.value_or(project));
                        reg.body_ranges.try_emplace(row.symbol, *row.body);
                        if (const auto sym = symbols_by_id.find(row.symbol);
                            sym != symbols_by_id.end() && channels::is_sealed_body(*sym->second, row, types_by_id))
                            reg.scope_hints.sealed_bodies.insert(row.symbol);
                    }
                for (auto& [input_project, registration] : grouped) {
                    auto pin = semantic::snapshot_for(description.snapshots, input_project);
                    if (reprojecting && input_project == project) {
                        pin.version = *reproject_version;
                        registration.revision = *reproject_version;
                    }
                    auto reply = co_await node_call(s, input_project, std::move(registration), read_actor(actor, project, pin, declarations));
                    if (reveal)
                        for (auto& row : reply.scopes)
                            out.scopes.at(offsets.at(row.id)) = std::move(row);
                }
            }
            compact_coverage(out.coverage);
            co_return out;
        }
        struct restoration {
            semantic::view previous;
            language::computation computation;
        };
        static auto resolve_target(const semantic::result& selected, const semantic::entity& entity, bool same_revision)
            -> std::optional<semantic::resolution_target> {
            if (selected.items.size() != 1)
                return {};
            const auto found = std::ranges::find(selected.symbols, entity.symbol_id, &semantic::records::symbol::id);
            if (found == selected.symbols.end())
                return {};
            const auto& declaration = *found;
            for (const auto& occurrence : selected.occurrences)
                if (occurrence.symbol == entity.symbol_id && occurrence.location.source) {
                    const auto& at = *occurrence.location.source;
                    semantic::resolution_target target{.name = {.text = declaration.qualified_name.empty() ? declaration.name : declaration.qualified_name,
                                                                .kind = declaration.kind,
                                                                .path = at.path}};
                    if (same_revision)
                        target.occurrence = at;
                    return target;
                }
            return {};
        }

        static auto syntax_query(const input& in, const std::map<std::string, language::configuration>& profiles) -> bool {
            if (in.action.value().value_or(semantic::tool_action::make<"diagnostics">()).str() != "inspect" ||
                in.configuration.value() || in.outputs || in.cursor || (in.include && !in.include->empty()) || (in.selector.value() && in.selector.value()->entity))
                return false;
            const auto filters = in.scope.value_or(scope_filter{}).extent();
            const auto name = in.selector.value() ? in.selector.value()->name : std::nullopt;
            if (!filters.modules.empty() || !filters.contexts.empty() || (name && name->module))
                return false;
            if (profiles.empty())
                return true;

            auto files = filters.files;
            if (name && name->path)
                files = {*name->path};
            if (files.empty())
                return false;

            std::set<std::string_view> families;
            for (const auto& path : files) {
                const auto family = syntax::outline_provider_family(path);
                if (family.empty())
                    return false;
                families.insert(family);
            }
            return std::ranges::none_of(profiles, [&](const auto& profile) {
                return std::ranges::any_of(profile.second.stages, [&](const auto& stage) {
                    const auto provider = language::describe_provider(stage.second.server).id;
                    return provider == "typescript" ? families.contains("typescript")
                                                     : (provider == "ty" || provider == "ruff") && families.contains("python");
                });
            });
        }

        static auto syntax_outline(services& s, const std::string& project, const input& in, const worlds::actor& actor)
            -> coroutine::task<output> {
            const auto line = execution_line(actor, in.worldline.value());
            const auto snapshot = co_await selected(s, project, line, in.version.value().value_or(""), actor);
            const auto filters = in.scope.value_or(scope_filter{}).extent();
            if (in.selector.value()) {
                try {
                    semantic::validate(*in.selector.value());
                } catch (const std::invalid_argument& error) {
                    throw tool_error(nodes::code::invalid_record, error.what());
                }
            }
            const auto name = in.selector.value() ? in.selector.value()->name : std::nullopt;
            output out{.worldline = line, .version = snapshot.version, .outline = std::vector<outline_item>{}};
            const auto budget = in.budget.value().value_or(semantic::budget{});
            const auto limit = std::min(in.limit.value_or(semantic::query_defaults{}.fact_limit),
                                        budget.nodes.value_or(std::numeric_limits<std::uint64_t>::max()));
            const auto started = std::chrono::steady_clock::now();
            std::uint64_t bytes = 0, units = 0;
            for (const auto& file : snapshot.files) {
                if (file.kind != "text" || !syntax::supports(file.path) ||
                    (!filters.files.empty() && !std::ranges::contains(filters.files, file.path)) ||
                    (!filters.projects.empty() && !std::ranges::contains(filters.projects, project)) ||
                    (name && name->path && *name->path != file.path))
                    continue;
                if (out.outline->size() >= limit || (budget.units && units >= *budget.units) ||
                    file.size > constants::semantic_object_bytes - bytes ||
                    (budget.milliseconds &&
                     std::cmp_greater_equal(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(),
                                            *budget.milliseconds))) {
                    out.status = "partial";
                    out.message = "Narrow the source scope or increase the outline budget to inspect more declarations";
                    break;
                }
                bytes += file.size;
                ++units;
                auto fetched = co_await node_call(s, project, {.action = "read", .worldline = line, .revision = snapshot.version, .target = file.path},
                                                  actor, true);
                const auto entry = std::ranges::find(fetched.files, file.path, &nodes::file_view::path);
                if (entry == fetched.files.end())
                    continue;
                auto parsed = syntax::parsed_file{entry->path, entry->text}.analyze(true);
                for (auto& scope : parsed.scopes) {
                    if (name && (!scope.name.starts_with(name->text) || (name->kind && name->kind->view() != scope.kind)))
                        continue;
                    if (out.outline->size() == limit) {
                        out.status = "partial";
                        out.message = "Narrow the source scope or increase the outline budget to inspect more declarations";
                        co_return out;
                    }
                    out.outline->push_back({std::move(scope.name), std::move(scope.kind), {file.path, scope.begin, scope.end}});
                }
            }
            co_return out;
        }

        static auto run(services& s, mcp::call_context context, input in, mcp::notifier& note, worlds::actor actor, std::optional<restoration> restored = {},
                        std::shared_ptr<dependency_execution> dependencies = {}, std::optional<language::dependency_computation> pinned = {})
            -> coroutine::task<output> {
            if (in.continuation.value()) {
                auto token = json::read<semantic::continuation>(*in.continuation.value());
                if (!token)
                    throw tool_error(nodes::code::invalid_record, "invalid analysis continuation");
                auto query = co_await s.files.with_semantic(s.core, [token = *token](semantic::store& store) { return store.resume(token); });
                auto pending = std::make_unique<input>();
                pending->project = query.project;
                pending->worldline = query.worldline;
                pending->version = query.version;
                pending->configuration = query.configuration;
                pending->action = query.action.str();
                pending->selector = query.selector;
                pending->view = query.view;
                pending->cursor = query.cursor;
                pending->from_view = query.from_view;
                pending->to_view = query.to_view;
                pending->include = query.include;
                pending->relations = query.relations;
                pending->changes = query.changes;
                pending->direction = query.direction;
                pending->depth = query.depth;
                pending->limit = query.limit;
                pending->budget = query.budget;
                pending->scope = scope_filter{
                    .modules = query.scope.modules, .files = query.scope.files, .contexts = query.scope.contexts, .projects = query.scope.projects};
                if (!actor.run.empty() && query.project != actor.project)
                    throw tool_error(nodes::code::permission_denied, "continuation is outside the task project");
                if (in.project.value() && in.project.value() != pending->project.value())
                    throw tool_error(nodes::code::invalid_record, "continuation belongs to another project");
                if (in.budget.value()) {
                    auto budget = pending->budget.value().value_or(semantic::budget{});
                    if (in.budget.value()->milliseconds)
                        budget.milliseconds = in.budget.value()->milliseconds;
                    if (in.budget.value()->units)
                        budget.units = in.budget.value()->units;
                    if (in.budget.value()->nodes)
                        budget.nodes = in.budget.value()->nodes;
                    pending->budget = budget;
                }
                const auto project = project_of(*pending);
                auto available = co_await read_view(s, project, *pending->view.value(), *pending, actor);
                if (pending->from_view || available.status != "partial" ||
                    std::ranges::all_of(available.coverage.missing, [](const auto& missing) { return missing.phase == "query"; }))
                    co_return available;
                auto [previous, continuing] = co_await s.files.with_semantic(
                    s.core, [id = *pending->view.value()](semantic::store& store) { return std::pair{store.describe(id), store.protect({id})}; });
                if (!previous.computation)
                    throw tool_error(nodes::code::invalid_record, "view has no analysis computation");
                auto computation = *previous.computation;
                pending->view.value().reset();
                pending->cursor.reset();
                pending->outputs.reset();
                const auto& pin = semantic::snapshot_for(previous.snapshots, project);
                pending->worldline = pin.worldline;
                pending->version = pin.version;
                pending->configuration = previous.configuration;
                auto next = co_await run(s, context, std::move(*pending), note, actor, restoration{std::move(previous), std::move(computation)});
                next.next_view = next.view;
                co_return next;
            }
            if (!dependencies) {
                dependencies = std::make_shared<dependency_execution>();
                if (restored) {
                    for (const auto& pin : restored->previous.snapshots)
                        dependencies->pins.emplace(pin.project, pin);
                    dependencies->retained = restored->previous.checkpoint;
                    dependencies->computations = restored->computation.dependencies;
                }
            }
            auto action = in.action.value().value_or(semantic::tool_action::make<"diagnostics">()).str();
            if (action == "schema") {
                static const auto schema = [] {
                    auto value = semantic::schemas().to_object().value();
                    value["providers"] = json::to_value(language::providers::describe());
                    return json::value{std::move(value)};
                }();
                co_return output{.schema = schema};
            }
            if (action == "describe") {
                auto descriptions = language::providers::describe();
                if (!project_name(in).empty()) {
                    const auto settings = config::resolve(*s.snapshot(), project_of(in));
                    for (const auto& [name, profile] : settings.analysis) {
                        if (in.configuration.value() && name != *in.configuration.value())
                            continue;
                        for (const auto& [_, stage] : profile.stages) {
                            auto value = language::describe_provider(stage.server);
                            if (!std::ranges::contains(descriptions, value.id, &language::description::id))
                                descriptions.push_back(std::move(value));
                        }
                    }
                }
                if (in.provider.value()) {
                    std::erase_if(descriptions, [&](const auto& value) { return value.id != *in.provider.value(); });
                    if (descriptions.empty())
                        throw tool_error(nodes::code::invalid_record, "unknown language provider");
                }
                co_return output{.providers = std::move(descriptions)};
            }
            auto project = project_of(in);
            if (in.from_view || in.to_view) {
                if (action != "impact" || !in.from_view || (!in.to_view && !in.view.value()))
                    throw tool_error(nodes::code::invalid_record, "impact comparison requires from_view and to_view");
                if (in.to_view && in.view.value() && *in.to_view != *in.view.value())
                    throw tool_error(nodes::code::invalid_record, "view and to_view select different targets");
                co_return co_await read_view(s, project, in.to_view.value_or(in.view.value().value_or("")), in, actor);
            }
            if (in.view.value())
                co_return co_await read_view(s, project, *in.view.value(), in, actor);
            auto name = in.configuration.value().value_or("");
            language::computation computation;
            language::defaults defaults;
            if (restored)
                computation = restored->computation;
            else if (pinned) {
                computation.profile = pinned->profile;
                computation.builds = pinned->builds;
                computation.toolchains = pinned->toolchains;
                computation.project = pinned->project;
            } else {
                auto settings = config::resolve(*s.snapshot(), project);
                if (syntax_query(in, settings.analysis))
                    co_return co_await syntax_outline(s, project, in, actor);
                if (name.empty()) {
                    if (settings.analysis.size() != 1)
                        throw tool_error(nodes::code::invalid_record,
                                         "name the analysis configuration; this project has " + std::to_string(settings.analysis.size()));
                    name = settings.analysis.begin()->first;
                }
                auto found = settings.analysis.find(name);
                if (found == settings.analysis.end())
                    throw tool_error(nodes::code::invalid_record, "analysis configuration not found: " + name);
                computation.profile = found->second;
                for (const auto& [id, config] : settings.configs)
                    computation.builds.emplace(id, config.build);
                defaults = settings.analysis_defaults;
            }
            output out{.configuration = name};
            computation.project = project;
            auto line = execution_line(actor, in.worldline.value());
            out.worldline = line;
            auto& builds = computation.builds;
            auto& profile = computation.profile;
            language::graph graph;
            try {
                graph = language::expand(profile, builds, in.outputs.value_or(std::vector<std::string>{}), defaults);
            } catch (const std::invalid_argument& error) {
                throw tool_error(nodes::code::invalid_record, error.what());
            }
            if (action == "plan")
                co_return output{.configuration = name, .worldline = line, .plan = graph};
            const auto declarations = declared_dependencies(s, project, name);
            if (!dependencies->active.emplace(project, name).second)
                throw tool_error(nodes::code::invalid_record, "cross-project analysis dependency cycle: " + project + "/" + name);
            struct active_dependency {
                std::shared_ptr<dependency_execution> state;
                std::pair<std::string, std::string> key;
                ~active_dependency() { state->active.erase(key); }
            } active{dependencies, {project, name}};
            std::vector<std::pair<std::string, std::string>> fingerprints;
            for (const auto& id : graph.stages) {
                const auto stage = language::resolve(profile.stages.at(id), graph.defaults);
                fingerprints.emplace_back(id, language::fingerprint(stage, builds).str());
            }
            out.environment = content::hash_of(json::write(std::tuple{fingerprints, graph.outputs})).str();
            semantic::request query{.action = action, .selector = in.selector.value()};
            if (query.selector) {
                try {
                    semantic::validate(*query.selector);
                } catch (const std::invalid_argument& e) {
                    throw tool_error(nodes::code::invalid_record, e.what());
                }
            }
            query.scope = in.scope.value_or(scope_filter{}).extent();
            for (const auto& path : query.scope.files)
                content::checked_path(path);
            if ((action == "definitions" || action == "references") && !query.selector)
                throw tool_error(nodes::code::invalid_record, "definitions and references require a selector");
            if ((action == "definitions" || action == "references") && query.selector->name && !query.selector->name->path)
                throw tool_error(nodes::code::invalid_record, "a named definition or reference selector requires its source path");
            const auto fixed_pin = dependencies->pins.find(project);
            if (fixed_pin != dependencies->pins.end() &&
                (fixed_pin->second.worldline != line || (in.version.value() && *in.version.value() != fixed_pin->second.version)))
                throw tool_error(nodes::code::invalid_record, "a project has conflicting snapshot selections: " + project);
            const auto source =
                co_await selected(s, project, line, fixed_pin == dependencies->pins.end() ? in.version.value().value_or("") : fixed_pin->second.version, actor);
            dependencies->pins.insert_or_assign(project, semantic::snapshot{project, line, source.version, ""});
            out.version = source.version;
            if (actor.dependency)
                actor.dependency->version = source.version;
            if (query.selector && query.selector->entity) {
                try {
                    co_await s.files.with_semantic(s.core, [id = query.selector->entity->symbol_id](semantic::store& store) { store.require_symbol(id); });
                } catch (const semantic::expired_symbol& e) {
                    out.status = "expired";
                    out.code = "symbol_expired";
                    out.message = e.what();
                    co_return out;
                }
            }
            semantic::request calculation;
            if ((action == "definitions" || action == "references") && query.selector) {
                calculation.action = query.action;
                calculation.selector = query.selector;
                if (query.selector->entity) {
                    calculation.target = co_await s.files.with_semantic(
                        s.core,
                        [project, line, name, version = source.version, entity = *query.selector->entity,
                         previous = restored ? std::optional{restored->previous.id}
                                             : std::nullopt](semantic::store& store) -> std::optional<semantic::resolution_target> {
                            auto id = previous ? previous : store.current(project, line, name);
                            if (!id)
                                return {};
                            auto view = store.load(*id);
                            auto selected = view->query({.action = std::string{"definitions"},
                                                         .selector = semantic::selection{.entity = entity},
                                                         .scope = {.projects = {project}},
                                                         .include = {"declarations"}});
                            return resolve_target(selected, entity, semantic::snapshot_for(view->description.snapshots, project).version == version);
                        });
                }
            }
            semantic::view selected_view{.snapshots = {{project, line, source.version, ""}},
                                         .configuration = name,
                                         .effective_profile = {{semantic::input_kind::project_model, "analysis/configuration", *out.environment}},
                                         .defaults = {*graph.defaults.fact_limit, *graph.defaults.relation_depth, graph.defaults.execution.budget}};
            selected_view.checkpoint = dependencies->retained;
            std::set<std::string> exposed;
            if (restored) {
                selected_view.checkpoint = restored->previous.checkpoint;
            }
            for (const auto& [_, producers] : graph.outputs)
                for (const auto& producer : producers)
                    exposed.insert(producer.stage);
            std::vector<language::request> requests;
            const auto file_scope = query.scope.files;
            for (const auto& id : graph.stages) {
                const auto stage = language::resolve(profile.stages.at(id), graph.defaults);
                const auto descriptor = language::describe_provider(stage.server);
                auto facts = semantic::required_facts(action, in.include.value_or(std::vector<std::string>{}));
                std::erase_if(facts, [&](const auto& fact) { return !std::ranges::contains(descriptor.facts, fact); });
                if (facts.empty())
                    facts = descriptor.facts;
                requests.push_back({.config = stage,
                                    .stage = id,
                                    .builds = language::select_builds(stage, builds),
                                    .query = calculation,
                                    .budget = semantic::tighten(stage.execution.budget, in.budget.value().value_or(semantic::budget{})),
                                    .demand = {.files = file_scope,
                                               .facts = std::move(facts),
                                               .projects = {project},
                                               .selector = (action == "definitions" || action == "references") ? query.selector : std::nullopt},
                                    .retained = selected_view.checkpoint});
            }
            using project_input_keys = std::map<std::string, std::set<std::tuple<semantic::input_kind, std::string, std::string, std::string>>>;
            for (auto& consumer : requests) {
                project_input_keys observed_inputs;
                const auto descriptor = language::describe_provider(consumer.config.server);
                for (const auto& [port, producers] : consumer.config.inputs)
                    for (const auto& producer : producers)
                        if (const auto* dependency = std::get_if<language::project_input>(&producer)) {
                            const auto cache_key =
                                json::write(std::tuple{dependency->project, dependency->worldline, dependency->configuration, dependency->output});
                            if (const auto pin = dependencies->pins.find(dependency->project);
                                pin != dependencies->pins.end() &&
                                (pin->second.worldline != dependency->worldline || (dependency->version && *dependency->version != pin->second.version)))
                                throw tool_error(nodes::code::invalid_record, "a project has conflicting dependency selections: " + dependency->project);
                            auto completed = dependencies->completed.find(cache_key);
                            if (completed == dependencies->completed.end()) {
                                input upstream;
                                upstream.project = dependency->project;
                                upstream.configuration = dependency->configuration;
                                upstream.worldline = dependency->worldline;
                                upstream.version = dependency->version;
                                upstream.outputs = std::vector<std::string>{dependency->output};
                                upstream.action = semantic::tool_action::make<"symbols">();
                                upstream.budget = in.budget.value();
                                auto fixed = dependencies->pins.find(dependency->project);
                                semantic::snapshot pin{dependency->project, dependency->worldline,
                                                       fixed == dependencies->pins.end() ? dependency->version.value_or("") : fixed->second.version, ""};
                                auto delegated = read_actor(actor, project, pin, declarations);
                                const auto fixed_computation = dependencies->computations.find(cache_key);
                                auto result = co_await run(s, context, std::move(upstream), note, std::move(delegated), {}, dependencies,
                                                           fixed_computation == dependencies->computations.end() ? std::nullopt
                                                                                                                 : std::optional{fixed_computation->second});
                                completed = dependencies->completed.emplace(cache_key, std::move(result)).first;
                            }
                            const auto& result = completed->second;
                            if (!result.view)
                                throw tool_error("analysis_failed", "dependency analysis produced no view: " + dependency->project);
                            auto [description, held, units] = co_await s.files.with_semantic(s.core, [id = *result.view](semantic::store& store) {
                                auto description = store.describe(id);
                                std::map<semantic::unit_id, semantic::unit> units;
                                for (const auto& [owner, shards] : description.units)
                                    if (!shards.empty())
                                        units.emplace(owner, store.object_unit(shards.front()));
                                return std::tuple{std::move(description), store.protect({id}), std::move(units)};
                            });
                            dependencies->roots.push_back(std::move(held));
                            if (description.computation) {
                                const auto& child = *description.computation;
                                language::dependency_computation child_computation{child.profile, child.builds, child.toolchains, child.project};
                                dependencies->computations.insert_or_assign(cache_key, child_computation);
                                computation.dependencies.insert_or_assign(cache_key, std::move(child_computation));
                                computation.dependencies.insert(child.dependencies.begin(), child.dependencies.end());
                            }
                            consumer.project_views.push_back(*result.view);
                            auto& facts = consumer.source_facts[port];
                            std::set<semantic::shard_id> selected_shards{facts.begin(), facts.end()};
                            std::set<std::string> producing_stages;
                            if (description.computation)
                                for (const auto& [output, producers] : description.computation->profile.outputs)
                                    for (const auto& producer : producers)
                                        producing_stages.insert(producer.stage);
                            for (const auto& [owner, shards] : description.units) {
                                dependencies->retained[owner] = shards;
                                for (const auto& shard : shards) {
                                    const auto& unit = units.at(owner);
                                    if (unit.recipe.unit.project == dependency->project && producing_stages.contains(unit.recipe.unit.stage) &&
                                        selected_shards.insert(shard).second)
                                        facts.push_back(shard);
                                }
                            }
                            for (const auto& [owner, unit] : units) {
                                auto& inputs = consumer.project_inputs[unit.recipe.unit.project];
                                for (const auto& value : unit.direct_inputs)
                                    if (value.kind != semantic::input_kind::tool && value.kind != semantic::input_kind::environment &&
                                        observed_inputs[unit.recipe.unit.project].emplace(value.kind, value.name, value.object, value.type).second)
                                        inputs.push_back(value);
                            }
                            for (const auto& pin : description.snapshots) {
                                auto selected_pin = std::ranges::find(selected_view.snapshots, pin.project, &semantic::snapshot::project);
                                if (selected_pin == selected_view.snapshots.end())
                                    selected_view.snapshots.push_back(pin);
                                else if (std::tie(selected_pin->worldline, selected_pin->version, selected_pin->snapshot) !=
                                         std::tie(pin.worldline, pin.version, pin.snapshot))
                                    throw tool_error(nodes::code::invalid_record, "dependency view has conflicting project snapshots: " + pin.project);
                                const auto tree = co_await selected(s, pin.project, pin.worldline, pin.version, read_actor(actor, project, pin, declarations));
                                auto& sources = consumer.sources[pin.project];
                                for (const auto& file : tree.files)
                                    sources.emplace(file.path, file.digest);
                            }
                            selected_view.coverage.missing.insert(selected_view.coverage.missing.end(), description.coverage.missing.begin(),
                                                                  description.coverage.missing.end());
                        }
                consumer.snapshots = selected_view.snapshots;
                for (const auto& [port, producers] : consumer.config.inputs)
                    for (const auto& producer : producers)
                        if (const auto* ref = std::get_if<language::reference>(&producer)) {
                            auto& upstream = *std::ranges::find(requests, ref->stage, &language::request::stage);
                            upstream.demand.files.clear();
                            const auto input = std::ranges::find(descriptor.ports, port, &language::port::name);
                            for (const auto& fact : input->facts)
                                if (!std::ranges::contains(upstream.demand.facts, fact))
                                    upstream.demand.facts.push_back(fact);
                        }
            }
            semantic::source_vector dependency_sources;
            semantic::input_vector dependency_inputs;
            project_input_keys collected_inputs;
            for (const auto& request : requests) {
                for (const auto& [owner, files] : request.sources)
                    dependency_sources[owner].insert(files.begin(), files.end());
                for (const auto& [owner, inputs] : request.project_inputs)
                    for (const auto& input : inputs)
                        if (collected_inputs[owner].emplace(input.kind, input.name, input.object, input.type).second)
                            dependency_inputs[owner].push_back(input);
            }
            for (auto& request : requests) {
                request.snapshots = selected_view.snapshots;
                request.sources = dependency_sources;
                request.project_inputs = dependency_inputs;
            }
            // Zero-compilation CRDT reprojection fast path:
            // - comment/whitespace-only edits preserve full semantic truth for every read query (including diagnostics/references);
            // - sealed body-only edits (without FFI boundary breaks) preserve declaration/structure queries.
            const bool reprojectable_query =
                !restored && !pinned && selected_view.snapshots.size() == 1 &&
                (action == "symbols" || action == "inspect" || action == "definitions" || action == "scopes" ||
                 action == "diagnostics" || action == "references" || action == "relations");
            const bool declaration_only_query =
                reprojectable_query &&
                (action == "symbols" || action == "inspect" || action == "definitions" || action == "scopes") &&
                (!in.include ||
                 std::ranges::none_of(*in.include, [](const std::string& item) {
                     return item == "diagnostics" || item == "typed_ranges" || item == "references";
                 }));
            if (reprojectable_query) {
                const auto wanted_facts = semantic::required_facts(action, in.include.value_or(std::vector<std::string>{}));
                auto candidate = co_await s.files.with_semantic(
                    s.core,
                    [project, line, name, env = *out.environment, target_version = source.version, file_scope, wanted_facts,
                     selector = query.selector](semantic::store& store) -> std::optional<std::tuple<semantic::view_id, std::string, std::size_t>> {
                        auto id = store.current(project, line, name);
                        if (!id)
                            return std::nullopt;
                        const auto view = store.describe(*id);
                        if (view.snapshots.size() != 1 || !view.coverage.missing.empty())
                            return std::nullopt;
                        const auto& pin = view.snapshots.front();
                        if (pin.project != project || pin.worldline != line || pin.version == target_version)
                            return std::nullopt;
                        const auto env_match = std::ranges::any_of(view.effective_profile, [&](const auto& value) {
                            return value.kind == semantic::input_kind::project_model && value.name == "analysis/configuration" && value.object == env;
                        });
                        if (!env_match)
                            return std::nullopt;
                        if (view.coverage.requested.selector && view.coverage.requested.selector != selector)
                            return std::nullopt;
                        if (!view.coverage.requested.files.empty() &&
                            (file_scope.empty() || !std::ranges::all_of(file_scope, [&](const auto& f) {
                                 return std::ranges::contains(view.coverage.requested.files, f);
                             })))
                            return std::nullopt;
                        if (!view.coverage.requested.facts.empty() &&
                            !std::ranges::all_of(wanted_facts, [&](const auto& f) {
                                return std::ranges::contains(view.coverage.requested.facts, f);
                            }))
                            return std::nullopt;
                        return std::tuple{*id, pin.version, view.units.size()};
                    });
                if (candidate) {
                    const auto& [v0_id, v0_version, v0_units] = *candidate;
                    try {
                        auto classified = co_await node_call(
                            s, project,
                            {.action = "scope_classify", .worldline = line, .revision = source.version, .target = v0_version},
                            actor, true);
                        const bool can_reproject =
                            classified.classification &&
                            (classified.classification->comment_or_whitespace_only ||
                             (declaration_only_query && classified.classification->sealed_body_only && !classified.classification->ffi_boundary_touched));
                        if (can_reproject) {
                            auto fixed = in;
                            fixed.version.value().reset();
                            auto queried = co_await read_view(s, project, v0_id, fixed, actor, source.version);
                            queried.analyzed = false;
                            queried.reused_units = v0_units;
                            co_return queried;
                        }
                    } catch (const tool_error& error) {
                        if (error.code != "reprojection_stale")
                            throw;
                    }
                }
            }
            auto prepared = co_await prepare(s, context, project, name, line, source.version, std::move(requests), std::move(exposed), actor, &note, query,
                                             computation.toolchains);
            if (prepared.status != "prepared" || prepared.analysis != "analyzed")
                throw tool_error(prepared.status == "prepared" || prepared.status == "failed" ? prepared.analysis_error_code : prepared.status,
                                 prepared.analysis_error.value_or(prepared.error));
            out.timings = prepared.timings;
            std::ranges::find(selected_view.snapshots, project, &semantic::snapshot::project)->snapshot = prepared.snapshot.str();
            selected_view.coverage.requested.files = file_scope;
            selected_view.coverage.requested.facts = semantic::required_facts(action, in.include.value_or(std::vector<std::string>{}));
            selected_view.coverage.requested.selector = query.selector;
            if (restored) {
                selected_view.units = restored->previous.units;
                for (const auto& missing : restored->previous.coverage.missing)
                    if (missing.deferred)
                        selected_view.coverage.missing.push_back(missing);
                for (const auto& fact : restored->previous.coverage.requested.facts)
                    if (!std::ranges::contains(selected_view.coverage.requested.facts, fact))
                        selected_view.coverage.requested.facts.push_back(fact);
                auto& scope = selected_view.coverage.requested.files;
                const auto& previous = restored->previous.coverage.requested.files;
                if (previous.empty())
                    scope.clear();
                else if (!scope.empty())
                    for (const auto& file : previous)
                        if (!std::ranges::contains(scope, file))
                            scope.push_back(file);
            }
            profile.defaults = graph.defaults;
            profile.outputs = graph.outputs;
            std::erase_if(profile.stages, [&](const auto& entry) { return !std::ranges::contains(graph.stages, entry.first); });
            std::set<std::string> used_builds;
            for (const auto& [name, stage] : profile.stages) {
                used_builds.insert(stage.build_configuration);
                for (const auto& [port, producers] : stage.inputs)
                    for (const auto& producer : producers)
                        if (const auto* build = std::get_if<language::build_input>(&producer))
                            used_builds.insert(build->configuration);
            }
            std::erase_if(builds, [&](const auto& entry) { return !used_builds.contains(entry.first); });
            computation.toolchains = prepared.toolchains;
            auto units = std::move(prepared.stage_views);
            std::map<std::string, std::string> runtime_sources;
            if (!restored && std::ranges::any_of(profile.stages, [](const auto& stage) { return language::provider_of(stage.second.server) == "ty"; }))
                for (const auto& file : source.files)
                    runtime_sources.emplace(file.path, file.digest);
            out.view = co_await s.files.with_semantic(s.core, [selected_view = std::move(selected_view), units = std::move(units),
                                                               computation = std::move(computation), checkpoints = std::move(prepared.checkpoint_views),
                                                               missing = std::move(prepared.missing), runtime_sources = std::move(runtime_sources)](semantic::store& store) mutable {
                std::set<semantic::unit_id> touched;
                for (const auto& id : units) {
                    auto unit = store.describe(id);
                    std::ranges::move(unit.receipts, std::back_inserter(selected_view.receipts));
                    std::ranges::move(unit.artifact_receipts, std::back_inserter(selected_view.artifact_receipts));
                    for (const auto& [owner, shards] : unit.units) {
                        selected_view.units[owner] = shards;
                        touched.insert(owner);
                    }
                }
                for (const auto& pending : missing)
                    touched.insert(pending.units.begin(), pending.units.end());
                for (const auto& id : checkpoints)
                    for (const auto& [owner, shards] : store.describe(id).units) {
                        selected_view.checkpoint[owner] = shards;
                        touched.insert(owner);
                    }
                std::erase_if(selected_view.coverage.missing, [&](const auto& pending) {
                    return std::ranges::all_of(pending.units, [&](const auto& unit) { return touched.contains(unit); });
                });
                selected_view.coverage.missing.insert(selected_view.coverage.missing.end(), missing.begin(), missing.end());
                selected_view.computation = computation;
                auto observations = runtime_sources.empty() ? std::vector<semantic::shard>{} : publishing::python_test_updates(
                    store, selected_view, semantic::snapshot_for(selected_view.snapshots, computation.project), runtime_sources);
                auto view = observations.empty() ? store.publish(std::move(selected_view), {}, true)
                                                : store.publish_records(std::move(selected_view), std::move(observations), {});
                store.make_current(view);
                return view;
            });
            auto fixed = in;
            fixed.version.value().reset();
            auto queried = co_await read_view(s, project, *out.view, fixed, actor);
            queried.timings = out.timings;
            queried.analyzed = true;
            queried.reused_units = prepared.reused_units;
            co_return queried;
        }
    };

} // namespace tools
