module indexing;
import std;
import json;
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
        auto duplicates(const indexed_view& before, const indexed_view& after, const std::vector<view_change>& changes,
                        const std::map<key, state>& current) -> std::vector<duplicate_type> {
            using shape_key = std::pair<std::string, std::string>; // Project and shape.
            std::map<shape_key, std::vector<duplicate_type>> added;
            for (const auto& change : changes) {
                if (!std::ranges::contains(change.kinds, std::string{"added"}) || before.symbols.contains(change.subject.symbol_id))
                    continue;
                const auto* owner = current.at({change.subject.symbol_id, *change.subject.context}).symbols.front();
                if (const auto shape = after.record_shape(*owner))
                    added[{after.unit_projects.at(owner->ownership.owner_unit), shape->first}].push_back(
                        {.subject = change.subject,
                         .subject_name = owner->qualified_name.empty() ? owner->name : owner->qualified_name,
                         .structural_shape_hash = shape->first,
                         .field_count = shape->second});
            }
            if (added.empty())
                return {};
            std::vector<duplicate_type> out;
            std::set<key> examined;
            for (auto member = after.members.begin(); member != after.members.end(); member = after.members.upper_bound(member->first)) {
                if (!before.symbols.contains(member->first))
                    continue;
                const auto [begin, end] = after.symbols.equal_range(member->first);
                for (auto at = begin; at != end; ++at) {
                    const auto& owner = after.symbol(at->second);
                    if (!after.languages.public_visibility(owner.language, owner.visibility) || !examined.emplace(owner.id, owner.ownership.context).second)
                        continue;
                    if (const auto shape = after.record_shape(owner))
                        if (const auto matches = added.find({after.unit_projects.at(owner.ownership.owner_unit), shape->first}); matches != added.end())
                            for (auto notice : matches->second) {
                                notice.existing = {owner.id, owner.ownership.context};
                                notice.existing_name = owner.qualified_name.empty() ? owner.name : owner.qualified_name;
                                out.push_back(std::move(notice));
                            }
                }
            }
            auto order = [](const duplicate_type& notice) {
                return std::tie(notice.subject.symbol_id, notice.subject.context, notice.existing.symbol_id, notice.existing.context);
            };
            std::ranges::sort(out, {}, order);
            return out;
        }
        auto relation_kind_of(const semantic::unit& unit, std::string_view kind) -> std::string_view {
            for (const auto& convention : unit.conventions)
                if (const auto found = convention.relation_traits.find(std::string{kind}); found != convention.relation_traits.end())
                    return found->second;
            return kind;
        }

        auto module_of(const indexed_view& view, const records::symbol& row) -> dependency_module {
            dependency_module out{view.unit_projects.at(row.ownership.owner_unit), row.module};
            if (!row.module.empty() && row.module != out.project)
                return out;
            out.name = out.project;
            if (row.language != "cpp")
                return out;
            std::set<std::string> definitions, declarations;
            const auto [first, last] = view.occurrences.equal_range(row.id);
            for (auto at = first; at != last; ++at) {
                const auto& found = view.occurrence(at->second);
                if (!found.location.source)
                    continue;
                if (std::ranges::contains(found.roles, std::string{"definition"}))
                    definitions.insert(found.location.source->path);
                else if (found.ownership.context == row.ownership.context && std::ranges::contains(found.roles, std::string{"declaration"}))
                    declarations.insert(found.location.source->path);
            }
            const auto& files = definitions.empty() ? declarations : definitions;
            if (files.size() == 1)
                out.file = *files.begin();
            return out;
        }

        using module_graph = std::map<dependency_module, std::set<dependency_module>>;
        auto dependency_graph(const indexed_view& view) -> module_graph {
            module_graph graph;
            std::map<std::pair<symbol_id, std::optional<context_id>>, std::optional<dependency_module>> endpoints;
            auto module = [&](const symbol_id& id, std::optional<context_id> context) -> std::optional<dependency_module> {
                const auto [entry, inserted] = endpoints.try_emplace({id, context});
                if (!inserted)
                    return entry->second;
                const auto [first, last] = view.symbols.equal_range(id);
                std::set<dependency_module> alternatives;
                bool unknown = false;
                for (auto at = first; at != last; ++at) {
                    const auto& symbol = view.symbol(at->second);
                    if (!context || symbol.ownership.context == *context) {
                        auto candidate = module_of(view, symbol);
                        if (symbol.language == "cpp" && symbol.domain == "source" && candidate.name == candidate.project && !candidate.file)
                            unknown = true;
                        else
                            alternatives.insert(std::move(candidate));
                    }
                }
                if (!unknown && alternatives.size() == 1)
                    entry->second = *alternatives.begin();
                return entry->second;
            };
            auto edge = [&](const auto& from, const auto& to) {
                if (from && to && from != to) {
                    graph[*from].insert(*to);
                    graph.try_emplace(*to);
                }
            };
            for (const auto& piece : view.pieces) {
                for (const auto& row : piece->relations()) {
                    const auto kind = relation_kind_of(piece->unit, row.kind);
                    if (row.resolution != "resolved" || (kind != "calls" && kind != "inherits" && kind != "imports"))
                        continue;
                    edge(module(row.from, row.from_context.value_or(row.ownership.context)), module(row.to, row.to_context));
                }
                for (const auto& row : piece->references())
                    if (row.resolution == "resolved" && row.from && row.target &&
                        (std::ranges::contains(row.roles, std::string{"call"}) || std::ranges::contains(row.roles, std::string{"import"})))
                        edge(module(*row.from, row.ownership.context), module(*row.target, row.target_context));
            }
            return graph;
        }

        // Tarjan's stack tracks open components independently of the DFS walk.
        // Frames retain the next edge so dependency depth does not consume the C++ stack.
        auto cyclic_components(const module_graph& graph) -> std::vector<std::vector<dependency_module>> {
            struct node { std::size_t index, low; bool open = true; };
            struct frame {
                const dependency_module* module;
                std::set<dependency_module>::const_iterator next, end;
            };
            std::map<dependency_module, node> visited;
            std::vector<const dependency_module*> open;
            std::vector<frame> walk;
            std::vector<std::vector<dependency_module>> cycles;
            auto enter = [&](const auto& row) {
                const auto index = visited.size();
                visited.emplace(row.first, node{index, index});
                open.push_back(&row.first);
                walk.push_back({&row.first, row.second.begin(), row.second.end()});
            };
            for (const auto& root : graph) {
                if (visited.contains(root.first))
                    continue;
                enter(root);
                while (!walk.empty()) {
                    auto& step = walk.back();
                    auto& current = visited.at(*step.module);
                    if (step.next != step.end) {
                        const auto& target = *step.next++;
                        const auto found = visited.find(target);
                        if (found == visited.end())
                            enter(*graph.find(target));
                        else if (found->second.open)
                            current.low = std::min(current.low, found->second.index);
                        continue;
                    }
                    if (current.low == current.index) {
                        std::vector<dependency_module> component;
                        for (;;) {
                            const auto* member = open.back();
                            open.pop_back();
                            visited.at(*member).open = false;
                            component.push_back(*member);
                            if (member == step.module)
                                break;
                        }
                        if (component.size() > 1) {
                            std::ranges::sort(component);
                            cycles.push_back(std::move(component));
                        }
                    }
                    walk.pop_back();
                    if (!walk.empty()) {
                        auto& parent = visited.at(*walk.back().module);
                        parent.low = std::min(parent.low, current.low);
                    }
                }
            }
            std::ranges::sort(cycles);
            return cycles;
        }

        auto topology_changes(const indexed_view& before, const indexed_view& after, const std::set<dependency_module>& scope, bool scoped)
            -> std::vector<topology_warning> {
            const auto previous = dependency_graph(before), current = dependency_graph(after);
            const auto old_cycles = cyclic_components(previous);
            std::map<dependency_module, std::size_t> old_component;
            for (std::size_t i = 0; i < old_cycles.size(); ++i)
                for (const auto& module : old_cycles[i])
                    old_component.emplace(module, i);
            std::vector<topology_warning> out;
            auto eligible = [&](const auto& module) { return !scoped || scope.contains(module); };
            for (auto& cycle : cyclic_components(current)) {
                const auto first = old_component.find(cycle.front());
                const bool already_connected = first != old_component.end() && std::ranges::all_of(cycle, [&](const auto& module) {
                    const auto found = old_component.find(module);
                    return found != old_component.end() && found->second == first->second;
                });
                if (!already_connected && std::ranges::any_of(cycle, eligible))
                    out.push_back({.kind = std::string{"cycle"}, .modules = std::move(cycle)});
            }
            for (const auto& [module, links] : current) {
                const auto found = previous.find(module);
                const auto old = found == previous.end() ? 0 : found->second.size();
                if (eligible(module) && links.size() >= old + 3 && links.size() - old >= old)
                    out.push_back({.kind = std::string{"fan_out"}, .modules = {module}, .before = old, .after = links.size()});
            }
            return out;
        }

        struct slot_type {
            std::string name;
            bool escape = false;
            bool concrete = true;
            auto operator<=>(const slot_type&) const = default;
        };
        auto spelling(std::string name) -> std::string {
            if (name.starts_with('['))
                if (const auto end = name.find(']'); end != name.npos)
                    name.erase(0, end + 1);
            std::erase_if(name, [](char c) { return c == ' ' || c == '\t'; });
            return name;
        }
        auto classify_type(const indexed_view& view, const type_id& id, const std::set<std::string>& escapes, std::set<type_id>& path) -> std::optional<slot_type> {
            if (!path.insert(id).second)
                return {};
            std::set<slot_type> alternatives;
            const auto [first, last] = view.types.equal_range(id);
            for (auto at = first; at != last; ++at) {
                const auto& row = view.type(at->second);
                const auto kind = view.languages.core_kind(view.languages.type_traits, row.domain, row.kind);
                std::optional<slot_type> value;
                const auto name = spelling(row.name.value_or(""));
                if (escapes.contains(name))
                    value = slot_type{name, true};
                else if (kind == "alias" && row.arguments.size() == 1)
                    value = classify_type(view, row.arguments.front(), escapes, path);
                else if (kind == "primitive" || kind == "named" || kind == "application") {
                    if (!name.empty() && name != "any" && name != "unknown" && name != "dynamic" && name != "void")
                        value = slot_type{name};
                    else if (name == "void")
                        value = slot_type{name, false, false};
                    if (value && escapes.contains(name + "?"))
                        value->concrete = false;
                } else if ((kind == "pointer" || kind == "reference" || kind == "nullable" || kind == "array") && row.arguments.size() == 1) {
                    if (const auto inner = classify_type(view, row.arguments.front(), escapes, path)) {
                        const auto suffix = kind == "pointer" ? "*" : kind == "reference" ? "&" : kind == "nullable" ? "?" : "[]";
                        const auto put = inner->name + suffix;
                        value = slot_type{put, escapes.contains(put), inner->concrete};
                    }
                }
                if (!value) {
                    alternatives.clear();
                    break;
                }
                alternatives.insert(std::move(*value));
            }
            path.erase(id);
            return alternatives.size() == 1 ? std::optional{*alternatives.begin()} : std::nullopt;
        }

        struct signature {
            std::optional<slot_type> result;
            std::vector<std::vector<std::optional<slot_type>>> groups;
            auto operator<=>(const signature&) const = default;
        };
        auto signature_of(const indexed_view& view, const state& state) -> std::optional<signature> {
            std::set<signature> alternatives;
            for (const auto* symbol : state.symbols) {
                if (!symbol->type)
                    continue;
                std::set<std::string> escapes;
                if (const auto found = view.languages.escape_types.find(symbol->language); found != view.languages.escape_types.end())
                    for (const auto& name : found->second)
                        escapes.insert(spelling(name));
                if (escapes.empty())
                    continue;
                auto classify = [&](const auto& id) {
                    std::set<type_id> path;
                    return classify_type(view, id, escapes, path);
                };
                const auto [first, last] = view.types.equal_range(*symbol->type);
                for (auto at = first; at != last; ++at) {
                    const auto& type = view.type(at->second);
                    if (view.languages.core_kind(view.languages.type_traits, type.domain, type.kind) != "function")
                        continue;
                    signature shape;
                    if (type.return_type)
                        shape.result = classify(*type.return_type);
                    for (const auto& group : type.parameter_groups) {
                        auto& parameters = shape.groups.emplace_back();
                        for (const auto& parameter : group.parameters)
                            parameters.push_back(classify(parameter.type));
                    }
                    alternatives.insert(std::move(shape));
                }
            }
            return alternatives.size() == 1 ? std::optional{*alternatives.begin()} : std::nullopt;
        }

        auto unsafe_sites(const indexed_view& view, const key& subject) {
            std::map<std::pair<std::string, std::optional<std::string>>, std::set<locator>> out;
            const auto [first, last] = view.outgoing.equal_range(subject.first);
            for (auto at = first; at != last; ++at) {
                const auto& row = view.relation(at->second);
                const auto& unit = view.pieces.at(at->second.shard)->unit;
                if (row.resolution != "resolved" || row.from_context.value_or(row.ownership.context) != subject.second || !row.ownership.origin.location)
                    continue;
                const auto kind = relation_kind_of(unit, row.kind);
                if (kind == "unsafe_cast") {
                    out[{row.kind, row.fingerprint}].insert(*row.ownership.origin.location);
                    continue;
                }
                if (kind != "calls")
                    continue;
                const auto [begin, end] = view.symbols.equal_range(row.to);
                for (auto candidate = begin; candidate != end; ++candidate) {
                    const auto& target = view.symbol(candidate->second);
                    if (row.to_context && target.ownership.context != *row.to_context)
                        continue;
                    if (std::ranges::any_of(unit.conventions, [&](const auto& rules) {
                            return rules.unsafe_casts && std::ranges::contains(*rules.unsafe_casts, target.qualified_name);
                        }))
                        out[{target.qualified_name, row.fingerprint}].insert(*row.ownership.origin.location);
                }
            }
            return out;
        }

        auto degradations(const indexed_view& before, const indexed_view& after, const std::vector<view_change>& changes,
                          const std::map<key, state>& old, const std::map<key, state>& current) -> std::vector<type_degradation> {
            std::vector<type_degradation> out;
            for (const auto& change : changes) {
                const key subject{change.subject.symbol_id, *change.subject.context};
                const auto now = current.find(subject);
                if (now == current.end())
                    continue;
                const auto& symbol = *now->second.symbols.front();
                const auto& name = symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
                if (const auto previous = old.find(subject);
                    previous != old.end() && std::ranges::contains(change.kinds, std::string{"declaration"})) {
                    const auto a = signature_of(before, previous->second), b = signature_of(after, now->second);
                    if (a && b) {
                        auto compare = [&](const auto& from, const auto& to, std::optional<std::uint64_t> group = {}, std::optional<std::uint64_t> index = {}) {
                            if (from && to && from->concrete && !from->escape && to->escape)
                                out.push_back({.subject = change.subject, .name = name, .kind = std::string{group ? "parameter" : "return_value"},
                                               .group = group, .index = index, .before = from->name, .after = to->name});
                        };
                        compare(a->result, b->result);
                        if (a->groups.size() == b->groups.size())
                            for (std::size_t group = 0; group < a->groups.size(); ++group)
                                if (a->groups[group].size() == b->groups[group].size())
                                    for (std::size_t index = 0; index < a->groups[group].size(); ++index)
                                        compare(a->groups[group][index], b->groups[group][index], group, index);
                    }
                }
                const auto prior = unsafe_sites(before, subject);
                for (const auto& [cast, locations] : unsafe_sites(after, subject)) {
                    const auto found = prior.find(cast);
                    const auto count = found == prior.end() ? 0 : found->second.size();
                    if (locations.size() > count) {
                        const auto added = std::ranges::find_if(locations, [&](const auto& location) {
                            return found == prior.end() || !found->second.contains(location);
                        });
                        out.push_back({.subject = change.subject, .name = name, .kind = std::string{"unsafe_cast"}, .before = std::to_string(count),
                                       .after = cast.first, .location = *added});
                    }
                }
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
        const auto query = content::hash_of(json::write(identity)).str();
        comparison::cursor progress{before.description.id, after.description.id, query};
        std::uint64_t offset = 0;
        if (page) {
            const auto decoded = json::read<comparison::cursor>(*page);
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
        std::vector<view_change> changes, critic_subjects;
        const bool scoped = request.selector || !request.scope.projects.empty() || !request.scope.modules.empty() || !request.scope.contexts.empty() ||
                            !request.scope.files.empty();
        std::set<dependency_module> module_scope;
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
            critic_subjects.push_back(change);
            if (!change.kinds.empty())
                changes.push_back(std::move(change));
        }
        if (scoped)
            for (const auto* view : {&before, &after}) {
                auto collect = [&](const auto& rows) {
                    for (const auto& [_, address] : rows)
                        if (const auto& row = view->symbol(address); eligible_symbol(*view, row))
                            module_scope.insert(comparison::module_of(*view, row));
                };
                if (request.selector && request.selector->entity) {
                    const auto [first, last] = view->symbols.equal_range(request.selector->entity->symbol_id);
                    collect(std::ranges::subrange(first, last));
                } else if (request.selector && request.selector->name) {
                    const auto [first, last] = view->names.equal_range(request.selector->name->text);
                    collect(std::ranges::subrange(first, last));
                } else
                    collect(view->symbols);
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
        const auto duplicates = comparison::duplicates(before, after, changes, current);
        const auto topology = before.description.id == after.description.id || (scoped && module_scope.empty())
                                  ? std::vector<topology_warning>{}
                                  : comparison::topology_changes(before, after, module_scope, scoped);
        const auto degraded = comparison::degradations(before, after, critic_subjects, old, current);
        const auto count = std::max({changes.size(), reanalysis.size(), duplicates.size(), topology.size(), degraded.size()});
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
                if (at < duplicates.size())
                    out.comparison->duplicate_types.push_back(duplicates[at]);
                if (at < topology.size())
                    out.comparison->topology.push_back(topology[at]);
                if (at < degraded.size())
                    out.comparison->degraded_types.push_back(degraded[at]);
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
            out.cursor = json::write(progress);
        return out;
    }
} // namespace semantic
