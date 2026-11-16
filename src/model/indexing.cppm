export module indexing;
import std;
import reflection;
import content;
export import semantic;

export namespace semantic {
    struct record_address {
        std::size_t shard = 0, row = 0;
    };

    using comparison_key = std::pair<symbol_id, context_id>;
    struct comparison_record {
        std::set<std::string> declarations, metadata, relations;
        bool inputs = false;
    };
    struct shard_index {
        std::map<std::string, std::vector<std::size_t>> names, symbols, positions, references, outgoing, incoming, occurrences, occurrence_ids, reference_ids,
            reference_files, unresolved, types, scopes, scope_owners, diagnostics, typed_overloads, members;
        // Typed ranges by path, ordered by their start offset.
        std::map<std::string, std::vector<std::size_t>> typed_range_files;
        std::set<symbol_id> identities;
        // Immutable semantic projections are computed with the shard index,
        // persisted and shared by every view and comparison page.
        std::map<symbol_id, std::map<context_id, comparison_record>> comparisons;
        std::string comparison_inputs;
        std::vector<std::string> dependency_keys;
        std::map<type_id, std::set<std::string>> type_shapes;
        std::map<type_id, std::set<type_id>> type_children;
        std::map<type_id, std::vector<std::size_t>> type_declarations;
        // Callables keyed by the nominal type they produce, and by the one they
        // take as receiver or first argument.
        std::map<symbol_id, std::vector<std::size_t>> type_producers, type_consumers;
    };
    // Return the JSON text of one top-level member of a compact JSON object.
    // Persisted shards are one such object, so a projection reads its own column
    // without decoding the record tables it does not use.
    inline auto shard_document_member(std::string_view document, std::string_view name) -> std::string_view {
        auto skip = [&](std::size_t& at) {
            while (at < document.size() && (document[at] == ' ' || document[at] == '\n' || document[at] == '\r' || document[at] == '\t'))
                ++at;
        };
        auto string_text = [&](std::size_t& at) -> std::string_view {
            if (at >= document.size() || document[at] != '"')
                throw std::invalid_argument("semantic shard document is not an object");
            const auto first = ++at;
            while (at < document.size()) {
                if (document[at] == '\\') {
                    at += 2;
                    continue;
                }
                if (document[at] == '"')
                    return document.substr(first, (at++) - first);
                ++at;
            }
            throw std::invalid_argument("semantic shard document ends inside a name");
        };
        std::size_t at = 0;
        skip(at);
        if (at >= document.size() || document[at] != '{')
            throw std::invalid_argument("semantic shard document is not an object");
        ++at;
        while (true) {
            skip(at);
            if (at < document.size() && document[at] == '}')
                break;
            const auto key = string_text(at);
            skip(at);
            if (at >= document.size() || document[at] != ':')
                throw std::invalid_argument("semantic shard document has a malformed member");
            skip(++at);
            const auto begin = at;
            int depth = 0;
            bool quoted = false, escaped = false;
            while (at < document.size()) {
                const char ch = document[at];
                if (quoted) {
                    if (escaped)
                        escaped = false;
                    else if (ch == '\\')
                        escaped = true;
                    else if (ch == '"')
                        quoted = false;
                } else if (ch == '"')
                    quoted = true;
                else if (ch == '{' || ch == '[')
                    ++depth;
                else if (ch == '}' || ch == ']') {
                    if (!depth)
                        break;
                    --depth;
                } else if (ch == ',' && !depth)
                    break;
                ++at;
            }
            if (key == name)
                return document.substr(begin, at - begin);
            if (at < document.size() && document[at] == ',')
                ++at;
        }
        throw std::invalid_argument("semantic shard document has no " + std::string{name} + " column");
    }
    // The resident index addresses immutable records; the columns are decoded
    // from the persisted document the first time a query projects them.
    struct indexed_shard {
        shard_id id;
        semantic::unit unit;
        std::shared_ptr<const shard_index> index;
        std::shared_ptr<const std::string> document;
        auto identities() const -> const std::vector<identity_binding>& { return column(identities_, "identities"); }
        auto symbols() const -> const std::vector<records::symbol>& { return column(symbols_, "symbols"); }
        auto types() const -> const std::vector<records::type>& { return column(types_, "types"); }
        auto occurrences() const -> const std::vector<records::occurrence>& { return column(occurrences_, "occurrences"); }
        auto references() const -> const std::vector<records::reference>& { return column(references_, "references"); }
        auto scopes() const -> const std::vector<records::scope>& { return column(scopes_, "scopes"); }
        auto relations() const -> const std::vector<records::relation>& { return column(relations_, "relations"); }
        auto diagnostics() const -> const std::vector<records::diagnostic>& { return column(diagnostics_, "diagnostics"); }
        auto typed_ranges() const -> const std::vector<records::typed_range>& { return column(typed_ranges_, "typed_ranges"); }
        // Paths that hand a shard to another module's record walker need the
        // whole value; only they pay for decoding every column.
        auto materialized() const -> const std::shared_ptr<const semantic::shard>& {
            std::scoped_lock guard{lock_};
            if (!full_) {
                auto decoded = rfl::json::read<semantic::shard>(*document);
                if (!decoded)
                    throw std::runtime_error("invalid semantic shard document: " + decoded.error().what());
                full_ = std::make_shared<const semantic::shard>(std::move(*decoded));
            }
            return full_;
        }

    private:
        template <class T> auto column(std::shared_ptr<const std::vector<T>>& slot, std::string_view name) const -> const std::vector<T>& {
            std::scoped_lock guard{lock_};
            if (!slot) {
                auto decoded = rfl::json::read<std::vector<T>>(shard_document_member(*document, name));
                if (!decoded)
                    throw std::runtime_error("invalid semantic shard column " + std::string{name} + ": " + decoded.error().what());
                slot = std::make_shared<const std::vector<T>>(std::move(*decoded));
            }
            return *slot;
        }
        mutable std::mutex lock_;
        mutable std::shared_ptr<const std::vector<identity_binding>> identities_;
        mutable std::shared_ptr<const std::vector<records::symbol>> symbols_;
        mutable std::shared_ptr<const std::vector<records::type>> types_;
        mutable std::shared_ptr<const std::vector<records::occurrence>> occurrences_;
        mutable std::shared_ptr<const std::vector<records::reference>> references_;
        mutable std::shared_ptr<const std::vector<records::scope>> scopes_;
        mutable std::shared_ptr<const std::vector<records::relation>> relations_;
        mutable std::shared_ptr<const std::vector<records::diagnostic>> diagnostics_;
        mutable std::shared_ptr<const std::vector<records::typed_range>> typed_ranges_;
        mutable std::shared_ptr<const semantic::shard> full_;
    };
    // Providers spell visibility in their own case: clice "public", Kotlin "PUBLIC".
    inline auto spelled(std::string_view value, std::string_view lower) -> bool {
        return std::ranges::equal(value, lower, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
    }
    inline auto visibility_rank(std::string_view value) -> std::ptrdiff_t {
        static constexpr std::array<std::string_view, 4> order{"public", "internal", "protected", "private"};
        return std::ranges::find_if(order, [&](std::string_view known) { return spelled(value, known); }) - order.begin();
    }
    // Strip indirections, aliases and library value wrappers (optional, task, result)
    // down to the declaration that names the value's type. Providers name
    // applications fully qualified: "std::optional", "kotlin.Result",
    // "System.Threading.Tasks.Task<int>" or F#'s postfix "int Microsoft.FSharp.Core.option".
    template <class Lookup> auto nominal_declaration(type_id id, Lookup&& lookup) -> std::optional<symbol_id> {
        static const std::set<std::string_view> wrappers{"std::optional",
                                                         "std::expected",
                                                         "std::unique_ptr",
                                                         "std::shared_ptr",
                                                         "kotlin.Result",
                                                         "kotlinx.coroutines.Deferred",
                                                         "System.Nullable",
                                                         "System.Threading.Tasks.Task",
                                                         "System.Threading.Tasks.ValueTask",
                                                         "Microsoft.FSharp.Core.option",
                                                         "Microsoft.FSharp.Core.Option",
                                                         "Microsoft.FSharp.Core.voption",
                                                         "Microsoft.FSharp.Core.ValueOption",
                                                         "Microsoft.FSharp.Core.Result",
                                                         "Microsoft.FSharp.Control.Async",
                                                         "Microsoft.FSharp.Core.byref",
                                                         "Microsoft.FSharp.Core.inref",
                                                         "Microsoft.FSharp.Core.outref",
                                                         "Microsoft.FSharp.NativeInterop.nativeptr"};
        // F# spells `T option` or `Option<T>`; CLR metadata spells
        // `[System.Runtime,8.0.0.0]System.Threading.Tasks.Task`1`.
        auto base = [](std::string_view name) {
            if (name.ends_with('>'))
                name = name.substr(0, name.find('<'));
            else
                name = name.substr(name.rfind(' ') + 1);
            if (name.starts_with('['))
                name = name.substr(name.find(']') + 1);
            if (const auto tick = name.rfind('`'); tick != std::string_view::npos)
                name = name.substr(0, tick);
            return name;
        };
        std::set<type_id> seen;
        while (seen.insert(id).second) {
            const records::type* type = lookup(id);
            if (!type)
                return std::nullopt;
            if ((type->kind == "pointer" || type->kind == "reference" || type->kind == "nullable" || type->kind == "alias") && !type->arguments.empty())
                id = type->arguments.front();
            else if (type->kind == "flexible" && type->lower_bound)
                id = *type->lower_bound;
            else if (type->kind == "application" && type->name && (type->arguments.size() == 1 || type->arguments.size() == 2) &&
                     wrappers.contains(base(*type->name)))
                id = type->arguments.front();
            else if (type->kind == "named" || type->kind == "application")
                return type->declaration;
            else
                return std::nullopt;
        }
        return std::nullopt;
    }

    inline auto index_shard(const semantic::shard& shard) -> shard_index {
        shard_index out;
        out.comparison_inputs = content::hash_of(rfl::json::write(std::tuple{shard.unit.direct_inputs, shard.unit.observed_dependencies})).str();
        for (auto read : shard.unit.observed_dependencies) {
            read.value.clear();
            read.producers.clear();
            if (read.project.empty())
                read.project = shard.unit.recipe.unit.project;
            out.dependency_keys.push_back(content::hash_of(rfl::json::write(read)).str());
        }
        auto retain = [&](const std::optional<symbol_id>& id) {
            if (id)
                out.identities.insert(*id);
        };
        for (const auto& identity : shard.identities) {
            out.identities.insert(identity.symbol);
            if (identity.source)
                retain(identity.source->owner);
        }
        for (const auto& type : shard.types) {
            retain(type.declaration);
            retain(type.binder);
            auto arguments = [&](this auto&& self, const auto& values) -> void {
                for (const auto& argument : values) {
                    retain(argument.declaration);
                    self(argument.elements);
                }
            };
            arguments(type.application_arguments);
        }
        for (const auto& scope : shard.scopes) {
            retain(scope.semantic_owner);
            out.identities.insert(scope.declared_members.begin(), scope.declared_members.end());
            for (const auto& edge : scope.lookup)
                out.identities.insert(edge.target);
        }
        for (std::size_t row = 0; row < shard.typed_ranges.size(); ++row) {
            const auto& range = shard.typed_ranges[row];
            retain(range.overload);
            if (range.overload)
                out.typed_overloads[*range.overload].push_back(row);
            out.typed_range_files[range.range.path].push_back(row);
        }
        for (auto& [path, rows] : out.typed_range_files)
            std::ranges::stable_sort(rows, {}, [&](std::size_t row) { return shard.typed_ranges[row].range.begin; });
        if (shard.unit.resolution)
            for (const auto& target : shard.unit.resolution->targets)
                out.identities.insert(target.symbol_id);
        for (std::size_t row = 0; row < shard.symbols.size(); ++row) {
            const auto& symbol = shard.symbols[row];
            out.identities.insert(symbol.id);
            retain(symbol.owner);
            out.names[symbol.name].push_back(row);
            if (symbol.qualified_name != symbol.name)
                out.names[symbol.qualified_name].push_back(row);
            out.symbols[symbol.id].push_back(row);
            auto& compared = out.comparisons[symbol.id][symbol.ownership.context];
            compared.declarations.insert(
                content::hash_of(rfl::json::write(std::tuple{symbol.language, symbol.domain, symbol.module, symbol.name, symbol.qualified_name, symbol.kind,
                                                             symbol.type, symbol.owner, symbol.visibility, symbol.modifiers, symbol.parameters}))
                    .str());
            compared.metadata.insert(content::hash_of(rfl::json::write(symbol.extensions)).str());
            compared.inputs = true;
            if (symbol.type)
                out.type_declarations[*symbol.type].push_back(row);
            if (symbol.owner)
                out.members[*symbol.owner].push_back(row);
        }
        for (std::size_t row = 0; row < shard.occurrences.size(); ++row) {
            out.occurrences[shard.occurrences[row].symbol].push_back(row);
            out.occurrence_ids[shard.occurrences[row].id].push_back(row);
            out.identities.insert(shard.occurrences[row].symbol);
            const auto& occurrence = shard.occurrences[row];
            if (occurrence.body && std::ranges::contains(occurrence.roles, std::string{"definition"}))
                out.comparisons[occurrence.symbol][occurrence.ownership.context].inputs = true;
            if (const auto& source = shard.occurrences[row].location.source)
                out.positions[source->path].push_back(row);
        }
        for (std::size_t row = 0; row < shard.references.size(); ++row) {
            const auto& reference = shard.references[row];
            out.reference_ids[reference.id].push_back(row);
            const auto file = reference.location.source ? reference.location.source->path : "";
            out.reference_files[file].push_back(row);
            if (!reference.target && reference.candidates.empty())
                out.unresolved[file].push_back(row);
            retain(reference.from);
            if (reference.from)
                out.comparisons[*reference.from][reference.ownership.context].relations.insert(
                    content::hash_of(rfl::json::write(std::tuple{reference.target, reference.target_context, reference.candidates, reference.roles,
                                                                 reference.resolution, reference.dispatch}))
                        .str());
            retain(reference.target);
            out.identities.insert(reference.candidates.begin(), reference.candidates.end());
            if (reference.target)
                out.references[*reference.target].push_back(row);
            for (const auto& candidate : reference.candidates)
                out.references[candidate].push_back(row);
        }
        for (std::size_t row = 0; row < shard.relations.size(); ++row) {
            out.identities.insert(shard.relations[row].from);
            out.identities.insert(shard.relations[row].to);
            out.outgoing[shard.relations[row].from].push_back(row);
            out.incoming[shard.relations[row].to].push_back(row);
            const auto& relation = shard.relations[row];
            out.comparisons[relation.from][relation.from_context.value_or(relation.ownership.context)].relations.insert(
                content::hash_of(rfl::json::write(std::tuple{relation.kind, relation.from, relation.to, relation.resolution, relation.detail,
                                                             relation.from_context, relation.to_context}))
                    .str());
        }
        for (std::size_t row = 0; row < shard.types.size(); ++row) {
            const auto& type = shard.types[row];
            out.types[type.id].push_back(row);
            out.type_shapes[type.id].insert(content::hash_of(rfl::json::write(type)).str());
            auto& children = out.type_children[type.id];
            for (const auto* list : {&type.arguments, &type.constraints, &type.elements, &type.type_parameters})
                children.insert(list->begin(), list->end());
            for (const auto* child : {&type.receiver, &type.return_type, &type.lower_bound, &type.upper_bound})
                if (*child)
                    children.insert(**child);
            for (const auto& group : type.parameter_groups)
                for (const auto& parameter : group.parameters)
                    children.insert(parameter.type);
            auto arguments = [&](this auto&& self, const auto& values) -> void {
                for (const auto& argument : values) {
                    if (argument.type)
                        children.insert(*argument.type);
                    self(argument.elements);
                }
            };
            arguments(type.application_arguments);
        }
        auto local = [&](const type_id& id) -> const records::type* {
            const auto found = out.types.find(id);
            return found == out.types.end() ? nullptr : &shard.types[found->second.front()];
        };
        for (std::size_t row = 0; row < shard.symbols.size(); ++row) {
            const auto& symbol = shard.symbols[row];
            const auto* signature = symbol.type ? local(*symbol.type) : nullptr;
            if (!signature || signature->kind != "function" || std::ranges::contains(symbol.modifiers, "local") || spelled(symbol.visibility, "local") ||
                (symbol.kind.str() != "function" && symbol.kind.str() != "method" && symbol.kind.str() != "constructor"))
                continue;
            // C++ constructors return void; every constructor produces its owner.
            const auto produced = symbol.kind.str() == "constructor" ? symbol.owner
                                  : signature->return_type           ? nominal_declaration(*signature->return_type, local)
                                                                     : std::nullopt;
            if (produced)
                out.type_producers[*produced].push_back(row);
            // Instance methods without a receiver (C++ and Kotlin members) take their owner.
            std::optional<symbol_id> consumed;
            if (signature->receiver)
                consumed = nominal_declaration(*signature->receiver, local);
            else if (symbol.kind.str() == "method" && symbol.owner && !std::ranges::contains(symbol.modifiers, "static"))
                consumed = symbol.owner;
            else if (!signature->parameter_groups.empty() && !signature->parameter_groups.front().parameters.empty())
                consumed = nominal_declaration(signature->parameter_groups.front().parameters.front().type, local);
            if (consumed)
                out.type_consumers[*consumed].push_back(row);
        }
        for (std::size_t row = 0; row < shard.scopes.size(); ++row) {
            const auto& location = shard.scopes[row].location;
            out.scopes[location.source ? location.source->path : ""].push_back(row);
            if (shard.scopes[row].semantic_owner)
                out.scope_owners[*shard.scopes[row].semantic_owner].push_back(row);
        }
        for (std::size_t row = 0; row < shard.diagnostics.size(); ++row) {
            const auto& location = shard.diagnostics[row].location;
            out.diagnostics[location.source ? location.source->path : ""].push_back(row);
        }
        return out;
    }

    // An index owns the immutable records it addresses. Readers can retain it while a
    // later view replaces a unit or the store retires an older view.
    struct indexed_view {
        semantic::view description;
        std::vector<std::shared_ptr<const indexed_shard>> pieces;
        std::map<unit_id, std::string> unit_projects;
        std::map<artifact_input, std::string> artifact_inputs;
        std::multimap<std::string, record_address> names, symbols, positions, references;
        std::multimap<std::string, record_address> outgoing, incoming, occurrences, occurrence_ids, reference_ids, reference_files, unresolved, types, scopes,
            scope_owners, diagnostics, typed_overloads, typed_range_files, members;
        std::multimap<std::string, std::size_t> resolutions;
        std::multimap<comparison_key, std::size_t> comparison_parts;
        std::map<unit_id, std::vector<std::size_t>> unit_parts;
        std::map<unit_id, std::set<unit_id>> unit_consumers;
        std::map<type_id, std::set<type_id>> type_parents;
        std::multimap<type_id, record_address> type_declarations;
        std::multimap<symbol_id, record_address> type_producers, type_consumers;

        auto symbol(record_address address) const -> const records::symbol& { return pieces.at(address.shard)->symbols().at(address.row); }
        auto type(record_address address) const -> const records::type& { return pieces.at(address.shard)->types().at(address.row); }
        auto occurrence(record_address address) const -> const records::occurrence& { return pieces.at(address.shard)->occurrences().at(address.row); }
        auto reference(record_address address) const -> const records::reference& { return pieces.at(address.shard)->references().at(address.row); }
        auto relation(record_address address) const -> const records::relation& { return pieces.at(address.shard)->relations().at(address.row); }
        auto scope(record_address address) const -> const records::scope& { return pieces.at(address.shard)->scopes().at(address.row); }
        auto diagnostic(record_address address) const -> const records::diagnostic& { return pieces.at(address.shard)->diagnostics().at(address.row); }
        auto typed_range(record_address address) const -> const records::typed_range& { return pieces.at(address.shard)->typed_ranges().at(address.row); }

        auto reference_contexts(const records::reference& reference, const symbol_id& id) const -> std::set<context_id> {
            if (reference.target == id && reference.target_context)
                return {*reference.target_context};
            std::set<context_id> result;
            const auto [first, last] = symbols.equal_range(id);
            for (auto at = first; at != last; ++at)
                result.insert(symbol(at->second).ownership.context);
            return result;
        }
        auto reference_targets(const records::reference& reference) const -> std::set<comparison_key> {
            std::set<comparison_key> result;
            auto append = [&](const symbol_id& id) {
                for (const auto& context : reference_contexts(reference, id))
                    result.emplace(id, context);
            };
            if (reference.target)
                append(*reference.target);
            for (const auto& id : reference.candidates)
                append(id);
            return result;
        }
        auto reference_uncertain(const records::reference& reference) const -> bool {
            return reference.resolution != "resolved" || !reference.target || reference_contexts(reference, *reference.target).size() != 1;
        }
        auto project_reference(records::reference reference) const -> records::reference {
            if (reference.target && !reference.target_context) {
                const auto contexts = reference_contexts(reference, *reference.target);
                if (contexts.size() == 1)
                    reference.target_context = *contexts.begin();
                else if (reference.resolution == "resolved")
                    reference.resolution = contexts.empty() ? std::string{"unresolved"} : std::string{"ambiguous"};
            }
            return reference;
        }

        auto query(semantic::request request) const -> semantic::result;
    };

    auto compare_views(const indexed_view& before, const indexed_view& after, semantic::request request) -> semantic::result;
    auto reanalysis_between(const indexed_view& before, const indexed_view& after) -> std::vector<reanalysis_change>;
    namespace querying {
        struct execution;
        inline void dispatch(execution&);
        inline void relations(execution&);
        inline void references(execution&);
        inline void types(execution&, std::vector<type_id> pending = {});
        inline void scopes(execution&);
        inline void diagnostics(execution&);
        inline void declarations(execution&);
        inline void inspect(execution&);
        void impact(execution&);
        struct execution {
            using symbol_key = std::pair<symbol_id, context_id>;
            struct walk_edge {
                symbol_key from, to;
                record_address address;
                bool reference = false;
            };
            struct walk_path {
                symbol_key seed, target;
                std::vector<walk_edge> edges;
                bool tentative = false, semantic = false;
            };
            struct walk_state {
                std::vector<symbol_key> seeds;
                std::size_t seed = 0;
                bool semantic = false, done = false, initialized = false, missing_owner = false;
                std::deque<walk_path> queue;
                std::set<symbol_key> discovered;
                std::set<std::pair<std::size_t, std::size_t>> emitted;
                std::optional<walk_path> current, pending;
                std::size_t phase = 0, scanned = 0;
            };
            struct page {
                view_id view;
                std::string query;
                std::uint64_t offset;
                std::optional<walk_state> traversal;
            };
            const indexed_view& view;
            semantic::request request;
            semantic::result out;
            std::string query_key;
            std::uint64_t offset = 0;
            std::optional<walk_state> traversal;
            bool graph = false, page_references = false, page_relations = false;
            std::map<symbol_key, const records::symbol*> selected;
            std::vector<const records::symbol*> ordered;
            std::set<std::string> origin_references, origin_occurrences;

            auto in_scope(const auto& row) const -> bool {
                return (request.scope.contexts.empty() || std::ranges::contains(request.scope.contexts, row.ownership.context)) &&
                       (request.scope.projects.empty() || std::ranges::contains(request.scope.projects, view.unit_projects.at(row.ownership.owner_unit)));
            }
            auto in_files(const locator& location) const -> bool {
                return (request.scope.projects.empty() || (location.project && std::ranges::contains(request.scope.projects, *location.project))) &&
                       (request.scope.files.empty() || (location.source && std::ranges::contains(request.scope.files, location.source->path)));
            }
            auto in_modules(const records::symbol& row) const -> bool {
                return request.scope.modules.empty() || std::ranges::contains(request.scope.modules, row.module);
            }
            auto declared_in_files(const records::symbol& row) const -> bool {
                if (request.scope.files.empty())
                    return true;
                const auto [first, last] = view.occurrences.equal_range(row.id);
                return std::any_of(first, last, [&](const auto& entry) {
                    const auto& occurrence = view.occurrence(entry.second);
                    return occurrence.ownership.context == row.ownership.context && occurrence.location.source &&
                           std::ranges::contains(request.scope.files, occurrence.location.source->path);
                });
            }
            auto contains(const symbol_id& id, const context_id& context) const -> bool { return selected.contains({id, context}); }
            void prepare() {

                if (request.selector)
                    semantic::validate(*request.selector);
                if (request.limit == 0 || request.limit > 1000)
                    throw std::invalid_argument("language limit must be between 1 and 1000");
                graph = request.action == "relations" || request.action == "impact" || std::ranges::contains(request.include, "relations");
                if (graph && request.depth == 0)
                    throw std::invalid_argument("relation depth must be positive");
                if (request.view && *request.view != view.description.id)
                    throw std::invalid_argument("query belongs to another view");
                request.view = view.description.id;
                request.scope.selector = request.selector;
                if (request.action != "impact" && request.action != "references" && !std::ranges::contains(request.include, "references") &&
                    request.scope.contexts.empty() && request.selector && request.selector->entity && request.selector->entity->context)
                    request.scope.contexts = {*request.selector->entity->context};
                auto cursor = std::exchange(request.cursor, {});
                auto identity = request;
                if (request.action == "impact" || request.action == "relations") {
                    identity.budget = {};
                    identity.limit = 1;
                }
                query_key = content::hash_of(rfl::json::write(identity)).str();
                if (cursor) {
                    auto decoded = rfl::json::read<page>(*cursor);
                    if (!decoded || decoded->view != view.description.id || decoded->query != query_key)
                        throw std::invalid_argument("cursor belongs to another query or view");
                    offset = decoded->offset;
                    traversal = std::move(decoded->traversal);
                }
                out = {.view = view.description.id,
                       .snapshots = view.description.snapshots,
                       .status = std::string{"complete"},
                       .coverage = view.description.coverage};
                out.coverage.requested = request.scope;
            }
            void cover() {
                const auto& prepared_files = view.description.coverage.requested.files;
                if (!prepared_files.empty()) {
                    auto absent = request.scope.files;
                    std::erase_if(absent, [&](const auto& file) { return std::ranges::contains(prepared_files, file); });
                    if (request.scope.files.empty() || !absent.empty())
                        out.coverage.missing.push_back({.scope = {.files = std::move(absent)}, .reason = "the view was prepared for selected files"});
                }
                for (auto fact : semantic::required_facts(request.action.str(), request.include))
                    if (!std::ranges::contains(out.coverage.requested.facts, fact))
                        out.coverage.requested.facts.push_back(std::move(fact));
                auto overlaps = [](const auto& a, const auto& b) {
                    return a.empty() || b.empty() || std::ranges::any_of(a, [&](const auto& value) { return std::ranges::contains(b, value); });
                };
                auto relevant = [&](const auto& scope) {
                    return (!scope.selector || scope.selector == request.selector) && overlaps(scope.files, request.scope.files) &&
                           overlaps(scope.modules, request.scope.modules) && overlaps(scope.contexts, request.scope.contexts) &&
                           overlaps(scope.projects, request.scope.projects);
                };
                std::erase_if(out.coverage.available, [&](const auto& scope) { return !relevant(scope); });
                std::erase_if(out.coverage.missing, [&](auto& missing) {
                    if (!relevant(missing.scope))
                        return true;
                    if (missing.scope.facts.empty())
                        return false;
                    std::erase_if(missing.scope.facts, [&](const auto& fact) { return !std::ranges::contains(out.coverage.requested.facts, fact); });
                    return missing.scope.facts.empty();
                });
                struct domain {
                    std::set<std::string> projects, modules, files;
                };
                std::map<context_id, domain> domains;
                for (const auto& piece : view.pieces) {
                    const auto& unit = piece->unit;
                    auto& domain = domains[unit.context];
                    auto append = [](auto& to, const auto& from) { to.insert(from.begin(), from.end()); };
                    append(domain.modules, unit.modules);
                    append(domain.files, unit.contributions.files);
                    domain.projects.insert(unit.recipe.unit.project);
                }
                auto scoped_domain = [&](const semantic::extent& scope) {
                    for (const auto& context : scope.contexts) {
                        auto& domain = domains[context];
                        domain.projects.insert(scope.projects.begin(), scope.projects.end());
                        domain.modules.insert(scope.modules.begin(), scope.modules.end());
                        domain.files.insert(scope.files.begin(), scope.files.end());
                    }
                };
                for (const auto& scope : out.coverage.available)
                    scoped_domain(scope);
                for (const auto& missing : out.coverage.missing)
                    scoped_domain(missing.scope);
                const bool explicit_contexts = !request.scope.contexts.empty();
                std::set<context_id> requested_contexts;
                for (const auto& fact : out.coverage.requested.facts) {
                    std::set<context_id> contexts{request.scope.contexts.begin(), request.scope.contexts.end()};
                    if (!explicit_contexts) {
                        // A resolver which exports only relations does not owe
                        // symbol facts. Deferred capabilities still participate.
                        for (const auto& scope : out.coverage.available)
                            if (std::ranges::contains(scope.facts, fact))
                                contexts.insert(scope.contexts.begin(), scope.contexts.end());
                        for (const auto& missing : out.coverage.missing)
                            if (missing.scope.facts.empty() || std::ranges::contains(missing.scope.facts, fact))
                                contexts.insert(missing.scope.contexts.begin(), missing.scope.contexts.end());
                    }
                    if (contexts.empty())
                        contexts.insert("");
                    bool selected_context = false;
                    for (const auto& context : contexts) {
                        auto selected_scope = request.scope;
                        if (!context.empty())
                            selected_scope.contexts = {context};
                        if (const auto found = domains.find(context); found != domains.end()) {
                            auto intersect = [](auto& requested, const auto& domain) {
                                if (requested.empty() || domain.empty())
                                    return true;
                                std::erase_if(requested, [&](const auto& value) { return !domain.contains(value); });
                                return !requested.empty();
                            };
                            if (!intersect(selected_scope.projects, found->second.projects) || !intersect(selected_scope.modules, found->second.modules) ||
                                !intersect(selected_scope.files, found->second.files))
                                continue;
                        }
                        selected_context = true;
                        if (!context.empty())
                            requested_contexts.insert(context);
                        auto in_context = [&](const auto& scope) {
                            return context.empty() || scope.contexts.empty() || std::ranges::contains(scope.contexts, context);
                        };
                        std::vector<const semantic::extent*> scopes;
                        for (const auto& scope : out.coverage.available)
                            if (std::ranges::contains(scope.facts, fact) && in_context(scope) && overlaps(scope.projects, selected_scope.projects) &&
                                overlaps(scope.modules, selected_scope.modules) && overlaps(scope.files, selected_scope.files))
                                scopes.push_back(&scope);
                        auto covered = [&](const auto& wanted, auto member) {
                            // Empty filters mean the prepared view's whole domain;
                            // completed units cover it together, not one wildcard.
                            return std::ranges::all_of(wanted, [&](const auto& value) {
                                return std::ranges::any_of(scopes, [&](const auto* scope) {
                                    const auto& values = scope->*member;
                                    return values.empty() || std::ranges::contains(values, value);
                                });
                            });
                        };
                        const bool available = !scopes.empty() && covered(selected_scope.files, &semantic::extent::files) &&
                                               covered(selected_scope.modules, &semantic::extent::modules) &&
                                               covered(selected_scope.projects, &semantic::extent::projects);
                        const bool missing = std::ranges::any_of(out.coverage.missing, [&](const auto& entry) {
                            return (entry.scope.facts.empty() || std::ranges::contains(entry.scope.facts, fact)) && in_context(entry.scope) &&
                                   overlaps(entry.scope.projects, selected_scope.projects) && overlaps(entry.scope.modules, selected_scope.modules) &&
                                   overlaps(entry.scope.files, selected_scope.files);
                        });
                        if (!available && !missing) {
                            selected_scope.facts = {fact};
                            out.coverage.missing.push_back({.scope = std::move(selected_scope), .reason = "the view has no coverage for this fact"});
                        }
                    }
                    if (!selected_context) {
                        auto missing = request.scope;
                        missing.facts = {fact};
                        out.coverage.missing.push_back({.scope = std::move(missing), .reason = "the view has no coverage for this fact"});
                    }
                }
                if (!explicit_contexts)
                    out.coverage.requested.contexts.assign(requested_contexts.begin(), requested_contexts.end());
            }
            void select() {
                if (!request.selector && request.include.empty() && (request.action == "diagnostics" || request.action == "scopes"))
                    return;
                std::optional<std::set<symbol_key>> resolved;
                const auto resolution_key = rfl::json::write(std::tuple{request.action.str(), request.selector});
                const auto [begin, end] = view.resolutions.equal_range(resolution_key);
                for (auto at = begin; at != end; ++at) {
                    const auto& unit = view.pieces[at->second]->unit;
                    if (!resolved)
                        resolved.emplace();
                    for (const auto& target : unit.resolution->targets)
                        resolved->emplace(target.symbol_id, target.context.value_or(unit.context));
                }
                std::set<symbol_key> file_members, named_members;
                auto members = [&](const std::string& path, auto& result) {
                    const auto [first, last] = view.positions.equal_range(path);
                    for (auto it = first; it != last; ++it) {
                        const auto& row = view.occurrence(it->second);
                        result.emplace(row.symbol, row.ownership.context);
                    }
                };
                for (const auto& path : request.scope.files)
                    members(path, file_members);
                const auto selection_path = request.selector && request.selector->name ? request.selector->name->path : std::nullopt;
                if (selection_path && !resolved) {
                    members(*selection_path, named_members);
                    const auto [first, last] = view.reference_files.equal_range(*selection_path);
                    for (auto it = first; it != last; ++it) {
                        const auto& row = view.reference(it->second);
                        const auto targets = view.reference_targets(row);
                        named_members.insert(targets.begin(), targets.end());
                    }
                }
                std::vector<record_address> candidates;
                auto add = [&](const auto& index, const auto& key) {
                    const auto [first, last] = index.equal_range(key);
                    for (auto it = first; it != last; ++it)
                        candidates.push_back(it->second);
                };
                if (resolved) {
                    for (const auto& [id, context] : *resolved)
                        add(view.symbols, id);
                } else if (request.selector && request.selector->entity)
                    add(view.symbols, request.selector->entity->symbol_id);
                else if (request.selector && request.selector->name) {
                    const auto& text = request.selector->name->text;
                    if (request.action == "symbols") {
                        for (auto it = view.names.lower_bound(text); it != view.names.end() && it->first.starts_with(text); ++it)
                            candidates.push_back(it->second);
                    } else
                        add(view.names, text);
                } else if (!request.scope.files.empty() && request.action != "references" && request.action != "relations") {
                    for (const auto& [id, context] : file_members)
                        add(view.symbols, id);
                } else
                    for (const auto& [id, address] : view.symbols)
                        candidates.push_back(address);
                for (const auto& address : candidates) {
                    const auto& row = view.symbol(address);
                    if (!in_scope(row) || (!request.scope.modules.empty() && !std::ranges::contains(request.scope.modules, row.module)))
                        continue;
                    if (resolved && !resolved->contains({row.id, row.ownership.context}))
                        continue;
                    if (selection_path && !resolved && !named_members.contains({row.id, row.ownership.context}))
                        continue;
                    if (request.selector) {
                        if (const auto& entity = request.selector->entity;
                            entity && (entity->symbol_id != row.id || (entity->context && *entity->context != row.ownership.context)))
                            continue;
                        if (const auto& name = request.selector->name; name) {
                            const auto matches = request.action == "symbols" ? row.name.starts_with(name->text) || row.qualified_name.starts_with(name->text)
                                                                             : row.name == name->text || row.qualified_name == name->text;
                            if ((!resolved && !matches) || (name->module && *name->module != row.module) || (name->kind && *name->kind != row.kind))
                                continue;
                        }
                    }
                    if (!request.scope.files.empty() && request.action != "references" && request.action != "relations" &&
                        !file_members.contains({row.id, row.ownership.context}))
                        continue;
                    selected.emplace(std::pair{row.id, row.ownership.context}, &row);
                }
                for (const auto& [id, row] : selected)
                    ordered.push_back(row);
                std::ranges::sort(ordered, [](const auto* a, const auto* b) {
                    return std::tie(a->qualified_name, a->id, a->ownership.context) < std::tie(b->qualified_name, b->id, b->ownership.context);
                });
                if (request.action != "symbols" && request.selector && selected.size() > 1) {
                    out.status = std::string{"ambiguous"};
                    out.code = "ambiguous_symbol";
                    out.message = "select a symbol ID and context from the candidates";
                }
            }
            void project_symbols() {
                // A resolved reference query pages uses of the selected declaration,
                // while an ambiguous query still pages its declaration candidates.
                const bool row_action = (request.action == "scopes" || request.action == "diagnostics") && out.status != "ambiguous";
                const bool page_records = page_references || page_relations || row_action;
                const auto begin = page_records ? 0 : offset;
                if (begin > ordered.size())
                    throw std::invalid_argument("cursor is beyond the query result");
                const auto end = page_records ? ordered.size() : begin + std::min<std::uint64_t>(request.limit, ordered.size() - begin);
                selected.clear();
                for (auto index = begin; index < end; ++index) {
                    const auto& row = *ordered[index];
                    selected.emplace(std::pair{row.id, row.ownership.context}, &row);
                    if (!row_action)
                        out.items.push_back({.symbol_id = row.id,
                                             .context = row.ownership.context,
                                             .language = row.language,
                                             .domain = row.domain,
                                             .module = row.module,
                                             .name = row.name,
                                             .kind = row.kind,
                                             .type_id = row.type});
                }
                if (!page_records && end < ordered.size())
                    out.cursor = rfl::json::write(page{view.description.id, query_key, end});
            }

            template <class F> void each_file(const auto& index, F&& visit) const {
                if (request.scope.files.empty()) {
                    for (const auto& [key, address] : index)
                        visit(address);
                } else
                    for (const auto& file : request.scope.files) {
                        const auto [first, last] = index.equal_range(file);
                        for (auto it = first; it != last; ++it)
                            visit(it->second);
                    }
            }

            void project() {
                if (request.action != "inspect") {
                    if (std::ranges::contains(request.include, "declarations"))
                        querying::declarations(*this);
                    if (std::ranges::contains(request.include, "type"))
                        querying::types(*this);
                }
                std::set<std::string> included;
                for (const auto& row : out.references)
                    included.insert(row.id);
                std::set<std::pair<std::size_t, std::size_t>> seen;
                auto occurrences = [&](const auto& index, const auto& key, bool selected_only) {
                    const auto [first, last] = index.equal_range(key);
                    for (auto it = first; it != last; ++it) {
                        const auto address = it->second;
                        const auto& row = view.occurrence(address);
                        if (!selected_only || contains(row.symbol, row.ownership.context))
                            seen.emplace(address.shard, address.row);
                    }
                };
                for (const auto& [key, row] : selected)
                    occurrences(view.occurrences, key.first, true);
                for (const auto& id : origin_occurrences)
                    occurrences(view.occurrence_ids, id, false);
                for (const auto& [shard, row] : seen)
                    out.occurrences.push_back(view.occurrence({shard, row}));
                for (const auto& id : origin_references) {
                    const auto [first, last] = view.reference_ids.equal_range(id);
                    for (auto it = first; it != last; ++it) {
                        const auto& row = view.reference(it->second);
                        if (included.insert(row.id).second)
                            out.references.push_back(row);
                    }
                }
                if (!out.coverage.missing.empty())
                    out.status = std::string{"partial"};
            }
            // A graph cursor is the next edge to inspect, not an offset into a
            // wall-clock-dependent prefix. All addresses refer to the pinned view.
            auto traverse(bool impact) -> std::vector<walk_path> {
                if (!traversal) {
                    traversal.emplace();
                    for (const auto& [key, row] : selected)
                        traversal->seeds.push_back(key);
                }
                auto& state = *traversal;
                const auto started = std::chrono::steady_clock::now();
                std::uint64_t work = 0;
                bool exhausted = false;
                std::vector<walk_path> result;
                if (state.pending) {
                    result.push_back(std::move(*state.pending));
                    state.pending.reset();
                }
                auto stop = [&] {
                    if (result.size() > request.limit)
                        return true;
                    exhausted =
                        work >= request.budget.nodes.value_or(1000) ||
                        (request.budget.milliseconds && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds{*request.budget.milliseconds});
                    return exhausted;
                };
                while (!state.done && !stop()) {
                    if (!state.initialized) {
                        state.discovered.clear();
                        if (impact) {
                            if (state.seed == state.seeds.size()) {
                                if (!state.semantic && (request.changes.empty() || std::ranges::contains(request.changes, "declaration"))) {
                                    state.semantic = true;
                                    state.seed = 0;
                                } else {
                                    state.done = true;
                                    break;
                                }
                            }
                            if (state.seeds.empty()) {
                                state.done = true;
                                break;
                            }
                            auto seed = state.seeds[state.seed];
                            state.queue.push_back({seed, seed, {}, false, state.semantic});
                            state.discovered.insert(seed);
                        } else {
                            for (const auto& seed : state.seeds) {
                                state.queue.push_back({seed, seed, {}, false, false});
                                state.discovered.insert(seed);
                            }
                        }
                        state.initialized = true;
                    }
                    if (!state.current) {
                        if (state.queue.empty()) {
                            if (impact) {
                                ++state.seed;
                                state.initialized = false;
                                continue;
                            }
                            state.done = true;
                            break;
                        }
                        ++work;
                        state.current = std::move(state.queue.front());
                        state.queue.pop_front();
                        state.phase = state.scanned = 0;
                        if (state.current->edges.size() >= request.depth) {
                            state.current.reset();
                            continue;
                        }
                    }
                    const auto key = state.current->target;
                    const auto phases = impact && state.semantic ? 3u : 2u;
                    if (state.phase == phases) {
                        state.current.reset();
                        continue;
                    }
                    if (!impact && ((state.phase == 0 && request.direction == "outgoing") || (state.phase == 1 && request.direction == "incoming"))) {
                        ++state.phase;
                        state.scanned = 0;
                        continue;
                    }
                    const auto& index = state.phase == 2 ? view.references : state.phase == 0 ? view.incoming : view.outgoing;
                    const auto [begin, end] = index.equal_range(key.first);
                    auto at = begin;
                    for (std::size_t skip = 0; skip < state.scanned && at != end; ++skip)
                        ++at;
                    for (; at != end && !stop(); ++at) {
                        ++work;
                        ++state.scanned;
                        const auto address = at->second;
                        walk_edge edge;
                        edge.from = key;
                        edge.address = address;
                        edge.reference = state.phase == 2;
                        bool tentative = false;
                        if (edge.reference) {
                            const auto& row = view.reference(address);
                            if (!view.reference_contexts(row, key.first).contains(key.second))
                                continue;
                            if (!row.from) {
                                state.missing_owner = true;
                                continue;
                            }
                            edge.to = {*row.from, row.ownership.context};
                            tentative = view.reference_uncertain(row);
                        } else {
                            const auto& row = view.relation(address);
                            const auto from = row.from_context.value_or(row.ownership.context), to = row.to_context.value_or(row.ownership.context);
                            const bool incoming = state.phase == 0;
                            if ((incoming ? to : from) != key.second || (!request.relations.empty() && !std::ranges::contains(request.relations, row.kind)))
                                continue;
                            if (impact) {
                                const bool follows =
                                    row.kind == "emits" ||
                                    (incoming && (state.semantic ? (row.kind == "inherits" || row.kind == "implements" || row.kind == "overrides" ||
                                                                    row.kind == "aliases" || row.kind == "instantiates")
                                                                 : (row.kind == "calls" || row.kind == "binds")));
                                if (!follows)
                                    continue;
                            } else if ((!in_scope(row) && !std::ranges::contains(request.scope.contexts, from) &&
                                        !std::ranges::contains(request.scope.contexts, to)) ||
                                       (row.ownership.origin.location ? !in_files(*row.ownership.origin.location) : !request.scope.files.empty()))
                                continue;
                            edge.to = {incoming ? row.from : row.to, incoming ? from : to};
                            tentative = row.resolution != "resolved";
                        }
                        auto path = *state.current;
                        path.target = edge.to;
                        path.edges.push_back(edge);
                        path.tentative |= tentative;
                        const bool fresh = state.discovered.insert(edge.to).second;
                        if (fresh)
                            state.queue.push_back(path);
                        if (impact ? fresh : state.emitted.emplace(address.shard, address.row).second)
                            result.push_back(std::move(path));
                    }
                    if (at == end) {
                        ++state.phase;
                        state.scanned = 0;
                    }
                }
                if (result.size() > request.limit) {
                    state.pending = std::move(result.back());
                    result.pop_back();
                }
                out.query_nodes += work;
                if (!state.done && page_relations)
                    out.cursor = rfl::json::write(page{view.description.id, query_key, offset + result.size(), state});
                if (exhausted)
                    out.coverage.missing.push_back(
                        {.scope = {.facts = {std::string{"relations"}}}, .reason = "graph traversal budget exhausted", .phase = std::string{"query"}});
                if (state.missing_owner)
                    out.coverage.missing.push_back({.scope = {.facts = {std::string{"references"}}},
                                                    .reason = "some references lack an owning declaration for impact traversal",
                                                    .phase = std::string{"analysis"}});
                return result;
            }
            auto run() -> semantic::result {
                prepare();
                cover();
                select();
                querying::dispatch(*this);
                if (out.status != "ambiguous")
                    project();
                return std::move(out);
            }
        };
        inline void relations(execution& self) {

            using key = execution::symbol_key;
            const auto matches = self.traverse(false);
            if (!self.request.selector) {
                self.selected.clear();
                self.out.items.clear();
            }
            auto endpoint = [&](const symbol_id& id, const context_id& context) {
                const auto [first, last] = self.view.symbols.equal_range(id);
                for (auto it = first; it != last; ++it) {
                    const auto& row = self.view.symbol(it->second);
                    if (row.ownership.context == context && self.selected.emplace(key{id, context}, &row).second)
                        self.out.items.push_back({.symbol_id = row.id,
                                                  .context = context,
                                                  .language = row.language,
                                                  .domain = row.domain,
                                                  .module = row.module,
                                                  .name = row.name,
                                                  .kind = row.kind,
                                                  .type_id = row.type});
                }
            };
            for (const auto& match : matches) {
                const auto& row = self.view.relation(match.edges.back().address);
                self.out.relations.push_back(row);
                endpoint(row.from, row.from_context.value_or(row.ownership.context));
                endpoint(row.to, row.to_context.value_or(row.ownership.context));
                if (row.reference)
                    self.origin_references.insert(*row.reference);
                if (row.occurrence)
                    self.origin_occurrences.insert(*row.occurrence);
                if (row.resolution != "resolved")
                    ++self.out.coverage.unresolved_count;
            }
        }
        inline void references(execution& self) {

            std::set<std::pair<std::size_t, std::size_t>> visited;
            std::vector<const records::reference*> matches;
            for (const auto& [key, symbol] : self.selected) {
                const auto [first, last] = self.view.references.equal_range(key.first);
                for (auto entry = first; entry != last; ++entry) {
                    const auto address = entry->second;
                    const auto& row = self.view.reference(address);
                    if (self.view.reference_contexts(row, key.first).contains(key.second) && self.in_scope(row) && self.in_files(row.location) &&
                        visited.emplace(address.shard, address.row).second)
                        matches.push_back(&row);
                }
            }
            // A query without a target also includes names for which the
            // provider found no candidates, so unresolved uses stay visible.
            if (!self.request.selector) {
                auto include = [&](record_address address) {
                    const auto& row = self.view.reference(address);
                    if (self.in_scope(row) && self.in_files(row.location) &&
                        (self.request.scope.modules.empty() || (row.from && self.contains(*row.from, row.ownership.context))))
                        matches.push_back(&row);
                };
                self.each_file(self.view.unresolved, include);
            }
            std::ranges::sort(matches, [](const auto* a, const auto* b) {
                return std::tie(a->ownership.context, a->ownership.owner_unit, a->id) < std::tie(b->ownership.context, b->ownership.owner_unit, b->id);
            });
            const auto first = self.page_references ? self.offset : 0;
            if (first > matches.size())
                throw std::invalid_argument("cursor is beyond the query result");
            const auto last = self.page_references ? first + std::min<std::uint64_t>(self.request.limit, matches.size() - first) : matches.size();
            std::set<std::pair<std::size_t, std::size_t>> declarations;
            const auto include_declarations = std::ranges::contains(self.request.include, "declarations");
            for (auto index = first; index < last; ++index) {
                const auto& reference = *matches[index];
                self.out.references.push_back(reference);
                if (reference.resolution != "resolved")
                    ++self.out.coverage.unresolved_count;
                if (include_declarations && reference.from) {
                    const auto [begin, end] = self.view.symbols.equal_range(*reference.from);
                    for (auto at = begin; at != end; ++at) {
                        const auto& declaration = self.view.symbol(at->second);
                        if (declaration.ownership.context == reference.ownership.context)
                            declarations.emplace(at->second.shard, at->second.row);
                    }
                }
            }
            const auto budget = self.request.budget.nodes.value_or(1000);
            for (const auto& [shard, row] : declarations) {
                if (self.out.symbols.size() >= budget) {
                    self.out.coverage.missing.push_back(
                        {.scope = {.facts = {std::string{"symbols"}}}, .reason = "declaration budget exhausted", .phase = std::string{"query"}});
                    break;
                }
                self.out.symbols.push_back(self.view.symbol({shard, row}));
            }
            if (self.page_references && last < matches.size())
                self.out.cursor = rfl::json::write(execution::page{self.view.description.id, self.query_key, last});
        }
        inline void types(execution& self, std::vector<type_id> pending) {
            using use_key = std::tuple<symbol_id, context_id, std::string, std::string>;
            std::map<use_key, std::multimap<std::uint64_t, std::uint64_t>> uses;
            std::set<symbol_id> targets;
            for (const auto& reference : self.out.references) {
                if (!reference.location.source || !reference.target)
                    continue;
                const auto& source = *reference.location.source;
                const auto& project = self.view.unit_projects.at(reference.ownership.owner_unit);
                uses[{*reference.target, reference.ownership.context, project, source.path}].emplace(source.begin, source.end);
                targets.insert(*reference.target);
            }
            std::set<std::pair<std::size_t, std::size_t>> added;
            auto add = [&](record_address address) {
                if (!added.emplace(address.shard, address.row).second)
                    return;
                const auto& range = self.view.typed_range(address);
                self.out.typed_ranges.push_back(range);
                pending.push_back(range.type);
                for (const auto& [parameter, argument] : range.substitutions) {
                    pending.push_back(parameter);
                    pending.push_back(argument);
                }
            };
            for (const auto& target : targets) {
                const auto [first, last] = self.view.typed_overloads.equal_range(target);
                for (auto it = first; it != last; ++it) {
                    const auto& range = self.view.typed_range(it->second);
                    if (!self.in_scope(range))
                        continue;
                    const auto& project = self.view.unit_projects.at(range.ownership.owner_unit);
                    const auto found = uses.find({target, range.ownership.context, project, range.range.path});
                    if (found == uses.end())
                        continue;
                    auto use = found->second.lower_bound(range.range.begin);
                    while (use != found->second.end() && use->first < range.range.end && use->second > range.range.end)
                        ++use;
                    if (use == found->second.end() || use->first >= range.range.end)
                        continue;
                    add(it->second);
                }
            }
            // Inferred types written inside a selected definition's body, or
            // inside a returned scope.
            auto within = [&](const context_id& context, const unit_id& unit, const place& region) {
                const auto& project = self.view.unit_projects.at(unit);
                const auto [first, last] = self.view.typed_range_files.equal_range(region.path);
                for (auto it = first; it != last; ++it) {
                    const auto& range = self.view.typed_range(it->second);
                    if (range.ownership.context == context && range.range.begin >= region.begin && range.range.end <= region.end &&
                        self.view.unit_projects.at(range.ownership.owner_unit) == project && self.in_scope(range))
                        add(it->second);
                }
            };
            if (self.request.action == "inspect")
                for (const auto& item : self.out.items) {
                    const auto [first, last] = self.view.occurrences.equal_range(item.symbol_id);
                    for (auto it = first; it != last; ++it)
                        if (const auto& occurrence = self.view.occurrence(it->second); occurrence.ownership.context == item.context && occurrence.body)
                            within(item.context, occurrence.ownership.owner_unit, *occurrence.body);
                }
            for (const auto& scope : self.out.scopes)
                if (scope.location.source)
                    within(scope.ownership.context, scope.ownership.owner_unit, *scope.location.source);
            for (const auto& item : self.out.items)
                if (item.type_id)
                    pending.push_back(*item.type_id);
            std::set<type_id> visited;
            const auto budget = self.request.budget.nodes.value_or(1000);
            while (!pending.empty()) {
                auto id = std::move(pending.back());
                pending.pop_back();
                if (!visited.insert(id).second)
                    continue;
                auto found = self.view.types.find(id);
                if (found == self.view.types.end()) {
                    ++self.out.coverage.unresolved_count;
                    continue;
                }
                if (self.out.types.size() + self.out.symbols.size() + self.out.producers.size() + self.out.consumers.size() >= budget) {
                    pending.push_back(std::move(id));
                    break;
                }
                const auto& type = self.view.type(found->second);
                self.out.types.push_back(type);
                // An alias or abbreviation declares the type it stands for.
                if (type.declaration) {
                    const auto [first, last] = self.view.symbols.equal_range(*type.declaration);
                    for (auto at = first; at != last; ++at)
                        if (const auto& declared = self.view.symbol(at->second).type)
                            pending.push_back(*declared);
                }
                for (const auto* list : {&type.arguments, &type.constraints, &type.elements, &type.type_parameters})
                    pending.insert(pending.end(), list->begin(), list->end());
                for (const auto* child : {&type.receiver, &type.return_type, &type.lower_bound, &type.upper_bound})
                    if (*child)
                        pending.push_back(**child);
                for (const auto& group : type.parameter_groups)
                    for (const auto& parameter : group.parameters)
                        pending.push_back(parameter.type);
                auto arguments = [&](this auto&& self, const auto& values) -> void {
                    for (const auto& argument : values) {
                        if (argument.type)
                            pending.push_back(*argument.type);
                        self(argument.elements);
                    }
                };
                arguments(type.application_arguments);
            }
            if (!pending.empty())
                self.out.coverage.missing.push_back(
                    {.scope = {.facts = {std::string{"types"}}}, .reason = "type node budget exhausted", .phase = std::string{"query"}});
        }
        template <class Rows> void page_rows(execution& self, Rows& rows) {
            if (self.offset > rows.size())
                throw std::invalid_argument("cursor is beyond the query result");
            const auto end = self.offset + std::min<std::uint64_t>(self.request.limit, rows.size() - self.offset);
            if (end < rows.size())
                self.out.cursor = rfl::json::write(execution::page{self.view.description.id, self.query_key, end});
            rows.erase(rows.begin() + end, rows.end());
            rows.erase(rows.begin(), rows.begin() + self.offset);
        }
        inline void scopes(execution& self) {
            auto include = [&](record_address address) {
                const auto& row = self.view.scope(address);
                if (self.in_scope(row) && self.in_files(row.location) &&
                    (!self.request.selector || (row.semantic_owner && self.contains(*row.semantic_owner, row.ownership.context))))
                    self.out.scopes.push_back(row);
            };
            if (!self.request.selector)
                self.each_file(self.view.scopes, include);
            else {
                std::set<symbol_id> owners;
                for (const auto& [key, row] : self.selected)
                    owners.insert(key.first);
                for (const auto& id : owners) {
                    const auto [first, last] = self.view.scope_owners.equal_range(id);
                    for (auto at = first; at != last; ++at)
                        include(at->second);
                }
            }
            if (self.request.action == "scopes")
                page_rows(self, self.out.scopes);
        }
        inline void diagnostics(execution& self) {
            self.each_file(self.view.diagnostics, [&](record_address address) {
                const auto& row = self.view.diagnostic(address);
                if (self.in_scope(row) && self.in_files(row.location))
                    self.out.diagnostics.push_back(row);
            });
            if (self.request.action == "diagnostics")
                page_rows(self, self.out.diagnostics);
        }
        inline void declarations(execution& self) {
            std::set<std::pair<symbol_id, context_id>> included;
            for (const auto& row : self.out.symbols)
                included.emplace(row.id, row.ownership.context);
            const auto budget = self.request.budget.nodes.value_or(1000);
            for (const auto& [key, row] : self.selected) {
                if (included.contains(key))
                    continue;
                if (self.out.symbols.size() >= budget) {
                    self.out.coverage.missing.push_back(
                        {.scope = {.facts = {std::string{"symbols"}}}, .reason = "declaration budget exhausted", .phase = std::string{"query"}});
                    break;
                }
                included.insert(key);
                self.out.symbols.push_back(*row);
            }
        }
        inline void inspect(execution& self) {
            declarations(self);
            // One hop of type surface: the public members of the nominal types a
            // selected callable's signature names, so a caller sees how to build
            // its arguments and what its result offers.
            auto lookup = [&](const type_id& id) -> const records::type* {
                const auto found = self.view.types.find(id);
                return found == self.view.types.end() ? nullptr : &self.view.type(found->second);
            };
            std::set<symbol_id> owners;
            for (const auto& item : self.out.items) {
                const auto* signature = item.type_id ? lookup(*item.type_id) : nullptr;
                if (!signature || signature->kind != "function")
                    continue;
                std::vector<type_id> named;
                for (const auto& group : signature->parameter_groups)
                    for (const auto& parameter : group.parameters)
                        named.push_back(parameter.type);
                for (const auto* end : {&signature->receiver, &signature->return_type})
                    if (*end)
                        named.push_back(**end);
                for (const auto& id : named)
                    if (auto declaration = nominal_declaration(id, lookup))
                        owners.insert(std::move(*declaration));
            }
            auto surface = [](const records::symbol& row) {
                static const std::set<std::string_view> kinds{"constructor", "field", "property", "enum_member", "function", "method"};
                return kinds.contains(row.kind.str()) && spelled(row.visibility, "public");
            };
            std::set<std::pair<symbol_id, context_id>> included;
            for (const auto& row : self.out.symbols)
                included.emplace(row.id, row.ownership.context);
            std::vector<type_id> pending;
            const auto budget = self.request.budget.nodes.value_or(1000);
            bool exhausted = false;
            for (const auto& owner : owners) {
                const auto [first, last] = self.view.members.equal_range(owner);
                for (auto at = first; at != last && !exhausted; ++at) {
                    const auto& row = self.view.symbol(at->second);
                    if (!self.in_scope(row) || !surface(row) || included.contains({row.id, row.ownership.context}))
                        continue;
                    if (self.out.symbols.size() >= budget) {
                        exhausted = true;
                        break;
                    }
                    included.emplace(row.id, row.ownership.context);
                    self.out.symbols.push_back(row);
                    if (row.type)
                        pending.push_back(*row.type);
                }
            }
            if (exhausted)
                self.out.coverage.missing.push_back(
                    {.scope = {.facts = {std::string{"symbols"}}}, .reason = "type surface budget exhausted", .phase = std::string{"query"}});
            // A selected type lists the callables that produce it and those that
            // take it, ordered by module and visibility.
            auto capabilities = [&](std::string_view include, const std::multimap<symbol_id, record_address>& index, std::vector<records::symbol>& into) {
                if (!std::ranges::contains(self.request.include, include))
                    return;
                std::set<std::pair<symbol_id, context_id>> seen;
                for (const auto& item : self.out.items) {
                    if (item.kind.str() != "type")
                        continue;
                    const auto [first, last] = index.equal_range(item.symbol_id);
                    for (auto at = first; at != last; ++at)
                        if (const auto& row = self.view.symbol(at->second);
                            self.in_scope(row) && self.in_modules(row) && self.declared_in_files(row) && seen.emplace(row.id, row.ownership.context).second)
                            into.push_back(row);
                }
                std::ranges::sort(into, {},
                                  [](const records::symbol& row) { return std::tuple{row.module, visibility_rank(row.visibility), row.qualified_name}; });
                const auto used = self.out.symbols.size() + self.out.producers.size() + self.out.consumers.size() - into.size();
                if (used + into.size() > budget) {
                    into.resize(budget > used ? budget - used : 0);
                    self.out.coverage.missing.push_back(
                        {.scope = {.facts = {std::string{"symbols"}}}, .reason = std::format("{} budget exhausted", include), .phase = std::string{"query"}});
                }
                for (const auto& row : into)
                    if (row.type)
                        pending.push_back(*row.type);
            };
            capabilities("producers", self.view.type_producers, self.out.producers);
            capabilities("consumers", self.view.type_consumers, self.out.consumers);
            types(self, std::move(pending));
        }
        enum class paging { symbols, references, relations };
        struct handler {
            query_action action;
            std::string_view include;
            paging page;
            void (*run)(execution&);
        };
        inline void dispatch(execution& self) {
            static const std::array handlers{handler{query_action::make<"relations">(), "relations", paging::relations, relations},
                                             handler{query_action::make<"references">(), "references", paging::references, references},
                                             handler{query_action::make<"scopes">(), "scope", paging::symbols, scopes},
                                             handler{query_action::make<"diagnostics">(), "diagnostics", paging::symbols, diagnostics},
                                             handler{query_action::make<"symbols">(), "", paging::symbols, nullptr},
                                             handler{query_action::make<"definitions">(), "", paging::symbols, nullptr},
                                             handler{query_action::make<"inspect">(), "", paging::symbols, inspect},
                                             handler{query_action::make<"impact">(), "", paging::relations, impact}};
            const auto primary = std::ranges::find(handlers, self.request.action, &handler::action);
            if (primary == handlers.end())
                throw std::logic_error("query action has no registered handler");
            self.page_references = primary->page == paging::references && self.out.status != "ambiguous";
            self.page_relations = primary->page == paging::relations && self.out.status != "ambiguous";
            self.project_symbols();
            if (self.out.status == "ambiguous")
                return;
            for (const auto& handler : handlers)
                if (handler.run &&
                    (self.request.action == handler.action || (!handler.include.empty() && std::ranges::contains(self.request.include, handler.include))))
                    handler.run(self);
        }
    } // namespace querying
    inline auto indexed_view::query(semantic::request request) const -> semantic::result {
        auto out = querying::execution{.view = *this, .request = std::move(request)}.run();
        for (auto& reference : out.references) {
            const auto was_resolved = reference.resolution == "resolved";
            reference = project_reference(std::move(reference));
            if (was_resolved && reference.resolution != "resolved")
                ++out.coverage.unresolved_count;
        }
        std::set<artifact_input> missing;
        auto artifact = [&](artifact_location& location) {
            if (!location.input)
                return;
            const auto found = artifact_inputs.find(*location.input);
            if (found == artifact_inputs.end()) {
                location.artifact.clear();
                missing.insert(*location.input);
            } else
                location.artifact = found->second;
        };
        auto locator = [&](semantic::locator& location) {
            if (location.artifact)
                artifact(*location.artifact);
        };
        auto ownership = [&](semantic::ownership& owner) {
            if (owner.origin.location)
                locator(*owner.origin.location);
        };
        for (auto& item : out.items) {
            for (auto& location : item.declarations)
                locator(location);
            for (auto& location : item.definitions)
                locator(location);
        }
        for (auto* rows : {&out.symbols, &out.producers, &out.consumers})
            for (auto& row : *rows)
                ownership(row.ownership);
        for (auto& row : out.typed_ranges)
            ownership(row.ownership);
        for (auto& row : out.occurrences) {
            ownership(row.ownership);
            locator(row.location);
        }
        for (auto& row : out.references) {
            ownership(row.ownership);
            locator(row.location);
        }
        for (auto& row : out.scopes) {
            ownership(row.ownership);
            locator(row.location);
        }
        for (auto& row : out.diagnostics) {
            ownership(row.ownership);
            locator(row.location);
            for (auto& related : row.related_locations)
                locator(related);
        }
        for (auto& row : out.relations) {
            ownership(row.ownership);
            if (row.detail)
                row.detail->visit([&](auto& detail) {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(detail)>, records::binding_detail>) {
                        for (auto& candidate : detail.library_selection.candidates)
                            artifact(candidate);
                        if (detail.library_selection.selected)
                            artifact(*detail.library_selection.selected);
                    }
                });
        }
        if (!missing.empty()) {
            out.status = std::string{"partial"};
            out.coverage.missing.push_back({.reason = "artifact container is absent from the selected view", .phase = std::string{"query"}});
        }
        return out;
    }
} // namespace semantic
