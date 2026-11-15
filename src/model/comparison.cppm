module indexing;
import std;
import reflection;
import content;

namespace semantic {
    namespace comparison {
        using key = std::pair<symbol_id, context_id>;
        struct state {
            std::vector<const records::symbol*> symbols;
            std::set<std::string> declaration, metadata, relations;
            std::map<unit_id, std::set<std::string>> inputs;
            std::map<type_id, std::set<std::string>> types;
        };
        auto type_shapes(const indexed_view& view, const type_id& id) -> std::set<std::string> {
            std::set<std::string> out;
            const auto [first, last] = view.types.equal_range(id);
            for (auto at = first; at != last; ++at) {
                const auto& values = view.pieces.at(at->second.shard)->index->type_shapes.at(id);
                out.insert(values.begin(), values.end());
            }
            return out;
        }
        auto type_closure(const indexed_view& view, std::vector<type_id> pending) -> std::map<type_id, std::set<std::string>> {
            std::map<type_id, std::set<std::string>> out;
            while (!pending.empty()) {
                auto id = std::move(pending.back());
                pending.pop_back();
                if (out.contains(id))
                    continue;
                out.emplace(id, type_shapes(view, id));
                const auto [first, last] = view.types.equal_range(id);
                for (auto at = first; at != last; ++at) {
                    const auto& children = view.pieces.at(at->second.shard)->index->type_children.at(id);
                    pending.insert(pending.end(), children.begin(), children.end());
                }
            }
            return out;
        }
        auto type_users(const indexed_view& view, std::vector<type_id> pending) -> std::set<key> {
            std::set<key> out;
            std::set<type_id> visited;
            while (!pending.empty()) {
                auto id = std::move(pending.back());
                pending.pop_back();
                if (!visited.insert(id).second)
                    continue;
                const auto [first, last] = view.type_declarations.equal_range(id);
                for (auto at = first; at != last; ++at) {
                    const auto& symbol = view.symbol(at->second);
                    out.emplace(symbol.id, symbol.ownership.context);
                }
                if (const auto parents = view.type_parents.find(id); parents != view.type_parents.end())
                    pending.insert(pending.end(), parents->second.begin(), parents->second.end());
            }
            return out;
        }
        auto changed_keys(const indexed_view& before, const indexed_view& after) -> std::set<key> {
            std::unordered_set<shard_id> old, current;
            for (const auto& piece : before.pieces)
                old.insert(piece->id);
            for (const auto& piece : after.pieces)
                current.insert(piece->id);
            std::set<key> out;
            std::set<type_id> types;
            auto append = [&](const indexed_view& view, const auto& shared) {
                for (const auto& piece : view.pieces)
                    if (!shared.contains(piece->id)) {
                        for (const auto& [symbol, contexts] : piece->index->comparisons)
                            for (const auto& [context, _] : contexts)
                                out.emplace(symbol, context);
                        for (const auto& [id, _] : piece->index->type_shapes)
                            types.insert(id);
                    }
            };
            append(before, current);
            append(after, old);
            std::vector<type_id> pending;
            for (const auto& id : types)
                if (type_shapes(before, id) != type_shapes(after, id))
                    pending.push_back(id);
            for (const auto* view : {&before, &after}) {
                const auto users = type_users(*view, pending);
                out.insert(users.begin(), users.end());
            }
            return out;
        }
        auto states(const indexed_view& view, const std::set<key>& selected) -> std::map<key, state> {
            std::map<key, state> out;
            for (const auto& key : selected) {
                state value;
                std::vector<type_id> roots;
                const auto [first, last] = view.symbols.equal_range(key.first);
                for (auto at = first; at != last; ++at) {
                    const auto& row = view.symbol(at->second);
                    if (row.ownership.context == key.second) {
                        value.symbols.push_back(&row);
                        if (row.type)
                            roots.push_back(*row.type);
                    }
                }
                if (value.symbols.empty())
                    continue;
                // A changed part can share an entity with unchanged alternatives
                // or with relation-only parts. Merge all indexed projections of
                // that entity without visiting unrelated records.
                const auto [begin, end] = view.comparison_parts.equal_range(key);
                for (auto at = begin; at != end; ++at) {
                    const auto& index = *view.pieces.at(at->second)->index;
                    const auto& record = index.comparisons.at(key.first).at(key.second);
                    value.declaration.insert(record.declarations.begin(), record.declarations.end());
                    value.metadata.insert(record.metadata.begin(), record.metadata.end());
                    value.relations.insert(record.relations.begin(), record.relations.end());
                    if (record.inputs)
                        value.inputs[view.pieces.at(at->second)->unit.id].insert(index.comparison_inputs);
                }
                value.types = type_closure(view, std::move(roots));
                out.emplace(key, std::move(value));
            }
            return out;
        }
        struct cursor {
            view_id before, after;
            std::string query;
            std::uint64_t offset = 0;
            unsigned side = 0;
            std::optional<std::string> traversal;
            bool announced = false;
        };
    } // namespace comparison
    auto compare_views(const indexed_view& before, const indexed_view& after, semantic::request request) -> semantic::result {
        if (request.action != "impact")
            throw std::invalid_argument("view comparison requires impact");
        if (request.selector)
            semantic::validate(*request.selector);
        if (!request.depth)
            throw std::invalid_argument("impact depth must be positive");
        if (!request.limit || request.limit > 1000)
            throw std::invalid_argument("comparison limit must be between 1 and 1000");
        request.from_view = before.description.id;
        request.to_view = after.description.id;
        request.view.reset();
        const auto page = std::exchange(request.cursor, {});
        auto identity = request;
        identity.budget = {};
        identity.limit = 1;
        const auto query = content::hash_of(rfl::json::write(identity)).str();
        comparison::cursor progress{before.description.id, after.description.id, query};
        std::uint64_t offset = 0;
        if (page) {
            const auto decoded = rfl::json::read<comparison::cursor>(*page);
            if (!decoded || decoded->before != before.description.id || decoded->after != after.description.id || decoded->query != query)
                throw std::invalid_argument("cursor belongs to another view comparison");
            progress = *decoded;
            offset = progress.offset;
        }
        const auto keys = comparison::changed_keys(before, after);
        const auto old = comparison::states(before, keys), current = comparison::states(after, keys);
        auto eligible_symbol = [&](const indexed_view& view, const records::symbol& row) {
            if (!request.scope.projects.empty() && !std::ranges::contains(request.scope.projects, view.unit_projects.at(row.ownership.owner_unit)))
                return false;
            if (!request.scope.modules.empty() && !std::ranges::contains(request.scope.modules, row.module))
                return false;
            if (!request.scope.contexts.empty() && !std::ranges::contains(request.scope.contexts, row.ownership.context))
                return false;
            if (request.selector && request.selector->entity) {
                const auto& entity = *request.selector->entity;
                if (entity.symbol_id != row.id || (entity.context && *entity.context != row.ownership.context))
                    return false;
            }
            if (request.selector && request.selector->name) {
                const auto& name = *request.selector->name;
                if ((name.text != row.name && name.text != row.qualified_name) || (name.module && *name.module != row.module) ||
                    (name.kind && *name.kind != row.kind))
                    return false;
            }
            auto files = request.scope.files;
            if (request.selector && request.selector->name && request.selector->name->path)
                files.push_back(*request.selector->name->path);
            if (files.empty())
                return true;
            const auto [first, last] = view.occurrences.equal_range(row.id);
            return std::any_of(first, last, [&](const auto& entry) {
                const auto& occurrence = view.occurrence(entry.second);
                return occurrence.ownership.context == row.ownership.context && occurrence.location.source &&
                       std::ranges::contains(files, occurrence.location.source->path);
            });
        };
        auto eligible = [&](const indexed_view& view, const comparison::state& value) {
            return std::ranges::any_of(value.symbols, [&](const auto* row) { return eligible_symbol(view, *row); });
        };
        std::vector<view_change> changes;
        for (const auto& key : keys) {
            const auto a = old.find(key), b = current.find(key);
            if (!(a != old.end() && eligible(before, a->second)) && !(b != current.end() && eligible(after, b->second)))
                continue;
            view_change change{.subject = {key.first, key.second}};
            if (a == old.end())
                change.kinds.push_back(std::string{"added"});
            else if (b == current.end())
                change.kinds.push_back(std::string{"removed"});
            else {
                if (a->second.declaration != b->second.declaration || a->second.types != b->second.types)
                    change.kinds.push_back(std::string{"declaration"});
                if (a->second.metadata != b->second.metadata)
                    change.kinds.push_back(std::string{"metadata"});
                if (a->second.relations != b->second.relations)
                    change.kinds.push_back(std::string{"relations"});
                if (a->second.inputs != b->second.inputs)
                    change.kinds.push_back(std::string{"inputs"});
            }
            if (!change.kinds.empty())
                changes.push_back(std::move(change));
        }
        auto reanalysis = reanalysis_between(before, after);
        if (request.selector || !request.scope.projects.empty() || !request.scope.modules.empty() || !request.scope.contexts.empty() ||
            !request.scope.files.empty()) {
            std::set<unit_id> selected;
            for (const auto* view : {&before, &after}) {
                auto retain = [&](const records::symbol& row) {
                    if (!eligible_symbol(*view, row))
                        return;
                    selected.insert(row.ownership.owner_unit);
                    const auto [first, last] = view->comparison_parts.equal_range({row.id, row.ownership.context});
                    for (auto part = first; part != last; ++part) {
                        const auto& piece = *view->pieces.at(part->second);
                        if (piece.index->comparisons.at(row.id).at(row.ownership.context).inputs)
                            selected.insert(piece.unit.id);
                    }
                    if (row.type)
                        for (const auto& [id, _] : comparison::type_closure(*view, {*row.type})) {
                            const auto [begin, end] = view->types.equal_range(id);
                            for (auto part = begin; part != end; ++part)
                                selected.insert(view->pieces.at(part->second.shard)->unit.id);
                        }
                };
                if (request.selector && request.selector->entity) {
                    const auto [first, last] = view->symbols.equal_range(request.selector->entity->symbol_id);
                    for (auto at = first; at != last; ++at)
                        retain(view->symbol(at->second));
                } else if (request.selector && request.selector->name) {
                    const auto [first, last] = view->names.equal_range(request.selector->name->text);
                    for (auto at = first; at != last; ++at)
                        retain(view->symbol(at->second));
                } else if (!request.scope.files.empty()) {
                    for (const auto& file : request.scope.files) {
                        const auto [first, last] = view->positions.equal_range(file);
                        for (auto at = first; at != last; ++at) {
                            const auto& occurrence = view->occurrence(at->second);
                            const auto [begin, end] = view->symbols.equal_range(occurrence.symbol);
                            for (auto symbol = begin; symbol != end; ++symbol)
                                retain(view->symbol(symbol->second));
                        }
                    }
                } else {
                    for (const auto& [id, parts] : view->unit_parts) {
                        const auto& unit = view->pieces.at(parts.front())->unit;
                        if ((!request.scope.projects.empty() && !std::ranges::contains(request.scope.projects, unit.recipe.unit.project)) ||
                            (!request.scope.contexts.empty() && !std::ranges::contains(request.scope.contexts, unit.context)) ||
                            (!request.scope.modules.empty() &&
                             !std::ranges::any_of(unit.modules, [&](const auto& module) { return std::ranges::contains(request.scope.modules, module); })))
                            continue;
                        selected.insert(id);
                    }
                }
            }
            // Symbol-free file/type parts still belong to the selected
            // declarations. Inspect only reanalysis candidates, using their
            // owner/type indices instead of scanning every view symbol.
            for (const auto& change : reanalysis)
                if (!selected.contains(change.id))
                    for (const auto* view : {&before, &after}) {
                        const auto parts = view->unit_parts.find(change.id);
                        if (parts == view->unit_parts.end())
                            continue;
                        std::set<comparison::key> owners;
                        for (const auto part : parts->second) {
                            const auto& index = *view->pieces.at(part)->index;
                            for (const auto& [id, contexts] : index.comparisons)
                                for (const auto& [context, _] : contexts)
                                    owners.emplace(id, context);
                            std::vector<type_id> types;
                            for (const auto& [id, _] : index.type_shapes)
                                types.push_back(id);
                            const auto users = comparison::type_users(*view, std::move(types));
                            owners.insert(users.begin(), users.end());
                        }
                        for (const auto& [id, context] : owners) {
                            const auto [first, last] = view->symbols.equal_range(id);
                            for (auto at = first; at != last; ++at) {
                                const auto& row = view->symbol(at->second);
                                if (row.ownership.context == context && eligible_symbol(*view, row)) {
                                    selected.insert(change.id);
                                    break;
                                }
                            }
                            if (selected.contains(change.id))
                                break;
                        }
                    }
            bool expanded;
            do {
                expanded = false;
                for (const auto& change : reanalysis)
                    if (!selected.contains(change.id))
                        for (const auto& dependency : change.dependencies)
                            if (std::ranges::any_of(dependency.producers, [&](const auto& producer) { return selected.contains(producer); })) {
                                expanded |= selected.insert(change.id).second;
                                break;
                            }
            } while (expanded);
            std::erase_if(reanalysis, [&](const auto& change) { return !selected.contains(change.id); });
        }
        const auto count = std::max(changes.size(), reanalysis.size());
        if (offset > count)
            throw std::invalid_argument("cursor is beyond the view changes");
        result out{.view = after.description.id, .snapshots = after.description.snapshots, .status = std::string{"complete"}};
        out.comparison = impact_comparison{before.description.id, after.description.id};
        const auto end = offset + std::min<std::uint64_t>(request.limit, count - offset);
        const auto started = std::chrono::steady_clock::now();
        bool exhausted = false;
        const auto nodes = request.budget.nodes.value_or(1000);
        while (progress.offset < end && !exhausted) {
            const auto at = progress.offset;
            if (!progress.announced) {
                if (at < reanalysis.size())
                    out.comparison->reanalysis.push_back(reanalysis[at]);
                if (at < changes.size())
                    out.comparison->changes.push_back(changes[at]);
                progress.announced = true;
            }
            while (at < changes.size() && progress.side < 2) {
                const auto& change = changes[at];
                const auto* view = progress.side == 0 ? &before : &after;
                const auto& states = progress.side == 0 ? old : current;
                if (!states.contains({change.subject.symbol_id, *change.subject.context})) {
                    ++progress.side;
                    continue;
                }
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                if (out.query_nodes >= nodes || (request.budget.milliseconds && elapsed >= *request.budget.milliseconds)) {
                    exhausted = true;
                    break;
                }
                auto selected = request;
                selected.view = view->description.id;
                selected.from_view.reset();
                selected.to_view.reset();
                selected.selector = selection{.entity = change.subject};
                selected.scope = {};
                selected.limit = 1000;
                selected.cursor = progress.traversal;
                selected.budget.nodes = nodes - out.query_nodes;
                if (request.budget.milliseconds)
                    selected.budget.milliseconds = *request.budget.milliseconds - elapsed;
                if (selected.changes.empty() && change.kinds.size() == 1 && change.kinds.front() == "inputs")
                    selected.changes = {"body"};
                auto affected = view->query(selected);
                out.query_nodes += affected.query_nodes;
                if (affected.status != "complete")
                    out.status = std::string{"partial"};
                std::ranges::move(affected.coverage.missing, std::back_inserter(out.coverage.missing));
                std::ranges::move(affected.impact, std::back_inserter(out.impact));
                out.comparison->evidence.push_back({view->description.id, view->description.snapshots, std::move(affected.items), std::move(affected.relations),
                                                    std::move(affected.references), std::move(affected.occurrences), std::move(affected.symbols),
                                                    std::move(affected.types), std::move(affected.typed_ranges), std::move(affected.scopes),
                                                    std::move(affected.diagnostics)});
                progress.traversal = std::move(affected.cursor);
                if (!progress.traversal)
                    ++progress.side;
            }
            if (!exhausted) {
                ++progress.offset;
                progress.side = 0;
                progress.announced = false;
            }
        }
        if (exhausted) {
            out.status = std::string{"partial"};
            out.coverage.missing.push_back({.reason = "view impact comparison budget exhausted", .phase = std::string{"query"}});
        }
        if (progress.offset < count)
            out.cursor = rfl::json::write(progress);
        return out;
    }
} // namespace semantic
