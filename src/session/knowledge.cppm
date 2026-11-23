module;
#include <sqlite3.h>
export module knowledge;
import utility;
import std;
import json;
import constants;
import content;
import codec;
import channels;
import enumeration;
import reuse;
import durability;
export import indexing;
export import objects;

export namespace semantic {
    namespace persistence {
        enum class table { identities, views, recipes, leases, shards };
        class database {
            struct statement {
                sqlite3* database;
                sqlite3_stmt* handle = nullptr;
                statement(sqlite3* database, const std::string& sql) : database(database) {
                    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &handle, nullptr) != SQLITE_OK)
                        throw std::runtime_error(sqlite3_errmsg(database));
                }
                ~statement() { sqlite3_finalize(handle); }
                statement(const statement&) = delete;
                auto operator=(const statement&) -> statement& = delete;
                void bind(int slot, std::string_view value) {
                    if (sqlite3_bind_text64(handle, slot, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK)
                        throw std::runtime_error(sqlite3_errmsg(database));
                }
                auto step() -> bool {
                    const auto status = sqlite3_step(handle);
                    if (status == SQLITE_ROW)
                        return true;
                    if (status == SQLITE_DONE)
                        return false;
                    throw std::runtime_error(sqlite3_errmsg(database));
                }
                auto text(int column) const -> std::string_view {
                    return {reinterpret_cast<const char*>(sqlite3_column_text(handle, column)), static_cast<std::size_t>(sqlite3_column_bytes(handle, column))};
                }
            };

        public:
            explicit database(const std::filesystem::path& path) {
                const auto filename = path.u8string();
                if (sqlite3_open_v2(reinterpret_cast<const char*>(filename.c_str()), &handle_,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
                    const std::string message = sqlite3_errmsg(handle_);
                    sqlite3_close(handle_);
                    throw std::runtime_error(message);
                }
                try {
                    execute("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
                    for (const auto item : {table::identities, table::views, table::leases, table::shards})
                        execute("CREATE TABLE IF NOT EXISTS " + name(item) + " (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
                    execute("CREATE TABLE IF NOT EXISTS shard_refs (owner TEXT NOT NULL, shard TEXT NOT NULL, PRIMARY KEY(owner,shard)) WITHOUT ROWID");
                    execute("CREATE INDEX IF NOT EXISTS refs_shard ON shard_refs(shard)");
                    execute("CREATE TABLE IF NOT EXISTS shard_identities (shard TEXT NOT NULL, symbol TEXT NOT NULL, PRIMARY KEY(shard,symbol)) WITHOUT ROWID");
                    execute("CREATE INDEX IF NOT EXISTS identity_shards ON shard_identities(symbol)");
                    execute("CREATE TABLE IF NOT EXISTS shard_objects (shard TEXT NOT NULL, object TEXT NOT NULL, PRIMARY KEY(shard,object)) WITHOUT ROWID");
                    execute("CREATE TABLE IF NOT EXISTS staging (shard TEXT PRIMARY KEY) WITHOUT ROWID");
                    execute("CREATE TABLE IF NOT EXISTS garbage (shard TEXT PRIMARY KEY) WITHOUT ROWID");
                    execute("CREATE TABLE IF NOT EXISTS storage (id INTEGER PRIMARY KEY CHECK(id=1), bytes INTEGER NOT NULL)");
                    execute("INSERT OR IGNORE INTO storage VALUES (1,0)");
                    execute("CREATE TABLE IF NOT EXISTS orphan_symbols (symbol TEXT PRIMARY KEY) WITHOUT ROWID");
                    execute("CREATE INDEX IF NOT EXISTS identity_symbol ON identities(value)");
                    execute("CREATE TABLE IF NOT EXISTS recipes (key TEXT PRIMARY KEY, recipe_id TEXT NOT NULL, project TEXT NOT NULL, "
                            "options TEXT NOT NULL, accessed INTEGER NOT NULL, dirty INTEGER NOT NULL DEFAULT 1, source_valid INTEGER NOT NULL DEFAULT 0, "
                            "value TEXT NOT NULL) WITHOUT ROWID");
                    execute("CREATE INDEX IF NOT EXISTS recipe_options ON recipes(project,options,source_valid,accessed DESC)");
                    execute("CREATE INDEX IF NOT EXISTS recipe_dirty ON recipes(project,options,dirty)");
                    execute("CREATE INDEX IF NOT EXISTS recipe_unit ON recipes(recipe_id,accessed DESC)");
                    execute("CREATE INDEX IF NOT EXISTS recipe_access ON recipes(accessed)");
                    execute("CREATE TABLE IF NOT EXISTS candidate_inputs (project TEXT NOT NULL, input_key TEXT NOT NULL, candidate_id TEXT NOT NULL, "
                            "PRIMARY KEY(project,input_key,candidate_id)) WITHOUT ROWID");
                    execute("CREATE INDEX IF NOT EXISTS candidate_owner ON candidate_inputs(candidate_id)");
                    execute("CREATE TABLE IF NOT EXISTS source_inputs (owner TEXT NOT NULL, project TEXT NOT NULL, path TEXT NOT NULL, object TEXT NOT NULL, "
                            "PRIMARY KEY(owner,project,path)) WITHOUT ROWID");
                    execute(
                        "CREATE TABLE IF NOT EXISTS source_projects (owner TEXT NOT NULL, project TEXT NOT NULL, PRIMARY KEY(owner,project)) WITHOUT ROWID");
                    execute("CREATE TABLE IF NOT EXISTS source_reclaims (owner TEXT PRIMARY KEY) WITHOUT ROWID");
                    execute("CREATE TABLE IF NOT EXISTS python_test_reports (project TEXT NOT NULL, worldline TEXT NOT NULL, version TEXT NOT NULL, "
                            "snapshot TEXT NOT NULL, name TEXT NOT NULL, digest TEXT NOT NULL, report TEXT NOT NULL, expires INTEGER NOT NULL, "
                            "PRIMARY KEY(project,worldline,version,snapshot,name)) WITHOUT ROWID");
                    execute("CREATE TEMP TABLE next_sources (project TEXT NOT NULL, path TEXT NOT NULL, object TEXT NOT NULL, "
                            "PRIMARY KEY(project,path)) WITHOUT ROWID");
                    execute("CREATE TEMP TABLE next_projects (project TEXT PRIMARY KEY) WITHOUT ROWID");
                    execute("CREATE TEMP TABLE changed_inputs (project TEXT NOT NULL, input_key TEXT NOT NULL, PRIMARY KEY(project,input_key)) WITHOUT ROWID");
                    execute("CREATE INDEX IF NOT EXISTS lease_view ON leases(json_extract(value,'$.view'))");
                    execute("CREATE INDEX IF NOT EXISTS lease_expiry ON leases(json_extract(value,'$.kind'),json_extract(value,'$.value.expires'))");
                } catch (...) {
                    sqlite3_close(handle_);
                    throw;
                }
            }
            ~database() { sqlite3_close(handle_); }
            database(const database&) = delete;
            auto operator=(const database&) -> database& = delete;
            struct transaction {
                database& owner;
                bool committed = false;
                explicit transaction(database& owner) : owner(owner) { owner.execute("BEGIN IMMEDIATE"); }
                ~transaction() {
                    if (!committed)
                        sqlite3_exec(owner.handle_, "ROLLBACK", nullptr, nullptr, nullptr);
                }
                void commit() {
                    owner.execute("COMMIT");
                    committed = true;
                }
            };
            void put(table item, std::string_view key, std::string_view value) {
                statement query{handle_, "INSERT INTO " + name(item) + " (key,value) VALUES (?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value"};
                query.bind(1, key);
                query.bind(2, value);
                query.step();
            }
            void insert(table item, std::string_view key, std::string_view value) {
                statement query{handle_, "INSERT INTO " + name(item) + " (key,value) VALUES (?1,?2)"};
                query.bind(1, key);
                query.bind(2, value);
                query.step();
            }
            void erase(table item, std::string_view key) {
                statement query{handle_, "DELETE FROM " + name(item) + " WHERE key=?1"};
                query.bind(1, key);
                query.step();
            }
            auto rows(const std::string& sql, const std::vector<std::string>& parameters = {}) const -> std::vector<std::vector<std::string>> {
                statement query{handle_, sql};
                for (std::size_t i = 0; i < parameters.size(); ++i)
                    query.bind(i + 1, parameters[i]);
                std::vector<std::vector<std::string>> result;
                while (query.step()) {
                    auto& row = result.emplace_back();
                    for (int i = 0; i < sqlite3_column_count(query.handle); ++i)
                        row.emplace_back(query.text(i));
                }
                return result;
            }
            auto get(table item, const std::string& key) const -> std::optional<std::string> {
                auto result = rows("SELECT value FROM " + name(item) + " WHERE key=?1", {key});
                return result.empty() ? std::nullopt : std::optional{std::move(result.front().front())};
            }

        private:
            static auto name(table item) -> std::string {
                switch (item) {
                case table::identities:
                    return "identities";
                case table::views:
                    return "views";
                case table::recipes:
                    return "recipes";
                case table::leases:
                    return "leases";
                case table::shards:
                    return "shards";
                }
                std::unreachable();
            }
            void execute(const std::string& sql) {
                if (sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
                    throw std::runtime_error(sqlite3_errmsg(handle_));
            }
            sqlite3* handle_ = nullptr;
        };
    } // namespace persistence

    struct candidate_payload {
        snapshot_vector snapshots;
        std::vector<shard_id> shards;
        std::vector<semantic::unit> units;
        std::uint64_t nodes = 0;
        bool cacheable = true;
    };

    struct expired_view : std::runtime_error, util::retained_exception {
        using std::runtime_error::runtime_error;
        auto retain() const noexcept -> std::exception_ptr override;
    };
    auto expired_view::retain() const noexcept -> std::exception_ptr { return util::copy_current_exception(*this); }

    struct expired_symbol : std::runtime_error, util::retained_exception {
        using std::runtime_error::runtime_error;
        auto retain() const noexcept -> std::exception_ptr override;
    };
    auto expired_symbol::retain() const noexcept -> std::exception_ptr { return util::copy_current_exception(*this); }

    struct retention {
        std::chrono::milliseconds lifetime = constants::semantic_view_retention;
        std::uint64_t bytes = constants::semantic_cache_bytes;
        std::uint64_t hot_bytes = constants::semantic_hot_bytes;
    };

    // The registry serializes access. New objects become durable before one database
    // transaction binds their declarations and publishes the view.
    class store {
        struct shard_metadata {
            semantic::unit unit;
            std::uint64_t bytes = 0, index_bytes = 0, nodes = 0;
            std::vector<std::string> scope_ids, scope_reads;
        };
        struct lifetime {
            std::int64_t accessed = 0, expires = 0;
        };
        // Source validity is a pure function of a candidate's immutable dependency
        // state and the source vector it is resolved against. The resolved state is
        // keyed by that exact source vector, never by a content hash, so switching
        // back to a worldline state that was already resolved reuses its results
        // instead of decoding and re-validating every affected candidate.
        struct validated_state {
            std::string project;
            std::string sources;
            std::map<candidate_id, bool> results;
        };
        struct identity_hash {
            auto operator()(const declaration_identity& key) const -> std::size_t {
                std::size_t value = key.index();
                auto add = [&](std::string_view text) { value ^= std::hash<std::string_view>{}(text) + 0x9e3779b9u + (value << 6) + (value >> 2); };
                std::visit(
                    [&](const auto& identity) {
                        add(identity.project);
                        add(identity.module);
                        add(identity.provider);
                        if constexpr (std::same_as<std::decay_t<decltype(identity)>, native_identity>) {
                            add(identity.domain);
                            add(identity.native_key);
                            if (identity.owner)
                                add(*identity.owner);
                        } else {
                            add(identity.anchor.file);
                            add(identity.anchor.declaration);
                            if (identity.anchor.owner)
                                add(*identity.anchor.owner);
                        }
                    },
                    key);
                return value;
            }
        };
        using identity_index = std::unordered_map<declaration_identity, symbol_id, identity_hash>;
        struct query_lease {
            json::literal<"query"> kind = std::string{"query"};
            view_id view;
            lifetime value;
        };
        struct current_lease {
            json::literal<"current"> kind = std::string{"current"};
            std::string project, worldline, configuration;
            view_id view;
        };
        struct continuation_lease {
            json::literal<"continuation"> kind = std::string{"continuation"};
            view_id view;
            std::string cursor;
            semantic::request query;
        };

    public:
        explicit store(
            std::filesystem::path directory, retention limits = {},
            std::function<std::chrono::system_clock::time_point()> clock = [] { return std::chrono::system_clock::now(); })
            : directory_(std::move(directory)), limits_(limits), clock_(std::move(clock)) {
            for (const auto* part : {"shards", "indexes"})
                durability::durable_directories(directory_ / part);
            database_ = std::make_unique<persistence::database>(directory_ / "semantic.db");
            recover();
        }

        ~store() {
            try {
                flush();
            } catch (...) {
            }
        }

        // selected.units contains only directly reused contributions. Each updated
        // unit replaces its entire shard list; omitted units contribute nothing.
        auto publish_records(semantic::view selected, std::vector<semantic::shard> updates, const std::map<std::string, source_anchor>& anchors,
                             const retention_roots& roots = {}, std::vector<std::optional<shard_receipt>> receipts = {}) -> view_id {
            std::map<std::string, declaration_identity> declarations;
            std::map<std::string, const source_anchor*> retained_sources;
            std::map<std::string, std::optional<std::string>> parents;
            for (const auto& shard : updates) {
                for (const auto& binding : shard.identities) {
                    if (binding.source)
                        retained_sources.emplace(binding.symbol, &*binding.source);
                    if (const auto* native = std::get_if<native_identity>(&binding.declaration); native && native->owner)
                        parents.emplace(binding.symbol, native->owner);
                    auto [found, inserted] = declarations.emplace(binding.symbol, binding.declaration);
                    if (!inserted && found->second != binding.declaration)
                        throw std::invalid_argument("one local declaration ID has conflicting identities");
                }
                for (const auto& symbol : shard.symbols)
                    parents.emplace(symbol.id, symbol.owner);
            }
            identity_index pending;
            std::map<std::string, symbol_id> ids;
            std::set<std::string> inferred;
            for (const auto& shard : updates)
                for (const auto& symbol : shard.symbols)
                    if (!declarations.contains(symbol.id)) {
                        if (symbol.native_key)
                            throw std::invalid_argument("a native declaration requires an identity binding");
                        if (auto source = anchors.find(symbol.id); source != anchors.end()) {
                            declarations.emplace(
                                symbol.id, anchored_identity{shard.unit.recipe.unit.project, symbol.module, shard.unit.recipe.unit.provider, source->second});
                            inferred.insert(symbol.id);
                        } else
                            // An unbound symbol is the same only within the same unit inputs.
                            ids.try_emplace(symbol.id, "sym-" + content::hash_of(json::write(std::tuple{shard.unit.id, shard.unit.context,
                                                                                                             shard.unit.direct_inputs, symbol.id}))
                                                                    .str());
                    }
            const auto unbound = ids.size();
            std::map<std::string, declaration_identity> bound;
            std::map<std::string, std::optional<source_anchor>> bound_sources;
            std::map<std::string, std::vector<std::string>> children;
            std::vector<std::string> ready;
            for (const auto& [local, declaration] : declarations) {
                const auto parent = parents.find(local);
                if (parent == parents.end() || !parent->second || ids.contains(*parent->second))
                    ready.push_back(local);
                else {
                    if (!declarations.contains(*parent->second))
                        throw std::invalid_argument("semantic declaration has an unknown owner");
                    children[*parent->second].push_back(local);
                }
            }
            auto known = [&](const declaration_identity& key) -> std::optional<symbol_id> {
                if (const auto found = pending.find(key); found != pending.end())
                    return found->second;
                if (auto found = identity(key))
                    return found;
                return {};
            };
            for (std::size_t at = 0; at < ready.size(); ++at) {
                const auto local = ready[at];
                auto declaration = declarations.at(local);
                if (auto* native = std::get_if<native_identity>(&declaration); native && native->owner)
                    native->owner = ids.at(*native->owner);
                std::vector<declaration_identity> keys;
                const auto retained = known(declaration);
                if (std::holds_alternative<native_identity>(declaration) || retained)
                    keys.push_back(declaration);
                std::optional<source_anchor> source;
                if (retained) {
                    if (const auto* anchored = std::get_if<anchored_identity>(&declaration))
                        source = anchored->anchor;
                    if (auto previous = retained_sources.find(local); previous != retained_sources.end()) {
                        auto alias = std::visit(
                            [&](const auto& identity) -> anchored_identity {
                                return {identity.project, identity.module, identity.provider, *previous->second};
                            },
                            declaration);
                        if (auto bound = known(alias)) {
                            if (bound != retained)
                                throw std::invalid_argument("declaration source anchor belongs to another symbol");
                            source = *previous->second;
                        }
                    }
                }
                if (auto found = anchors.find(local); found != anchors.end()) {
                    source = found->second;
                    if (auto parent = parents.find(local); parent != parents.end() && parent->second)
                        source->owner = ids.at(*parent->second);
                    keys.emplace_back(
                        std::visit([&](const auto& identity) -> anchored_identity { return {identity.project, identity.module, identity.provider, *source}; },
                                   declaration));
                }
                if (keys.empty())
                    throw std::invalid_argument("an anchored declaration requires a source anchor");
                std::optional<symbol_id> selected;
                for (const auto& key : keys)
                    if (auto previous = known(key)) {
                        if (selected && *selected != *previous)
                            throw std::invalid_argument("declaration identities are already bound to different symbols");
                        selected = std::move(previous);
                    }
                // A new symbol is named by its declaration, so identical records get the
                // same ID, and so the same shard ID, in every publication.
                auto id = selected ? *selected : "sym-" + content::hash_of(json::write(keys.front())).str();
                for (const auto& key : keys)
                    if (!known(key))
                        pending.emplace(key, id);
                ids.emplace(local, id);
                bound.emplace(local, keys.front());
                bound_sources.emplace(local, std::move(source));
                if (auto found = children.find(local); found != children.end())
                    ready.insert(ready.end(), found->second.begin(), found->second.end());
            }
            if (ids.size() != declarations.size() + unbound)
                throw std::invalid_argument("semantic declarations contain an ownership cycle");
            auto remap = [&](std::string& id) {
                if (!id.empty())
                    id = ids.at(id);
            };
            auto optional = [&](std::optional<std::string>& id) {
                if (id)
                    remap(*id);
            };
            for (auto& shard : updates) {
                for (const auto& symbol : shard.symbols)
                    if (inferred.contains(symbol.id))
                        shard.identities.push_back({declarations.at(symbol.id), symbol.id});
                auto scope_id = [&](const std::string& native) {
                    return "scope-" + content::hash_of(json::write(std::pair{shard.unit.context, native})).str();
                };
                for (auto& binding : shard.identities) {
                    binding.declaration = bound.at(binding.symbol);
                    binding.source = bound_sources.at(binding.symbol);
                    remap(binding.symbol);
                }
                if (shard.unit.resolution)
                    for (auto& target : shard.unit.resolution->targets)
                        remap(target.symbol_id);
                for (auto& symbol : shard.symbols) {
                    if (symbol.native_key)
                        symbol.native_key = bound.at(symbol.id);
                    remap(symbol.id);
                    optional(symbol.owner);
                    if (symbol.declaring_scope)
                        symbol.declaring_scope = scope_id(*symbol.declaring_scope);
                }
                for (auto& type : shard.types) {
                    optional(type.declaration);
                    optional(type.binder);
                    auto arguments = [&](this auto&& self, auto& values) -> void {
                        for (auto& argument : values) {
                            optional(argument.declaration);
                            self(argument.elements);
                        }
                    };
                    arguments(type.application_arguments);
                }
                for (auto& occurrence : shard.occurrences)
                    remap(occurrence.symbol);
                for (auto& reference : shard.references) {
                    optional(reference.from);
                    optional(reference.target);
                    for (auto& candidate : reference.candidates)
                        remap(candidate);
                }
                for (auto& scope : shard.scopes) {
                    scope.id = scope_id(scope.id);
                    if (scope.lexical_parent)
                        scope.lexical_parent = scope_id(*scope.lexical_parent);
                    optional(scope.semantic_owner);
                    for (auto& member : scope.declared_members)
                        remap(member);
                    for (auto& edge : scope.lookup)
                        remap(edge.target);
                }
                for (auto& relation : shard.relations) {
                    remap(relation.from);
                    remap(relation.to);
                }
                for (auto& range : shard.typed_ranges) {
                    optional(range.overload);
                    if (range.observation)
                        remap(range.observation->function);
                }
            }
            return publish_impl(std::move(selected), std::move(updates), true, roots, std::move(pending), true, std::move(receipts));
        }

        auto publish(semantic::view selected, std::vector<semantic::shard> updates, bool collect_coverage = false, const retention_roots& roots = {})
            -> view_id {
            return publish_impl(std::move(selected), std::move(updates), collect_coverage, roots, {});
        }

    private:
        auto publish_impl(semantic::view selected, std::vector<semantic::shard> updates, bool collect_coverage, const retention_roots& roots,
                          identity_index pending, bool remember_units = false, std::vector<std::optional<shard_receipt>> receipts = {}) -> view_id {
            normalize(selected.snapshots);
            std::set<unit_id> replaced;
            for (auto& shard : updates) {
                const auto& project = shard.unit.recipe.unit.project;
                snapshot_for(selected.snapshots, project);
                auto cover = [&](semantic::extent& scope) {
                    if (scope.projects.empty())
                        scope.projects = {project};
                    if (scope.contexts.empty())
                        scope.contexts = {shard.unit.context};
                };
                for (auto& scope : shard.unit.coverage.available)
                    cover(scope);
                for (auto& missing : shard.unit.coverage.missing)
                    cover(missing.scope);
                auto qualify = [&](semantic::locator& location) {
                    if (location.source && !location.project)
                        location.project = project;
                };
                for (auto& row : shard.occurrences)
                    qualify(row.location);
                for (auto& row : shard.references)
                    qualify(row.location);
                for (auto& row : shard.scopes)
                    qualify(row.location);
                for (auto& row : shard.diagnostics) {
                    qualify(row.location);
                    for (auto& location : row.related_locations)
                        qualify(location);
                }
                if (shard.unit.id != unit_identifier(shard.unit.recipe.unit) || shard.unit.context.empty())
                    throw std::invalid_argument("a semantic unit requires identity and semantic context");
                check_ownership(shard);
                check_identities(shard, pending);
                contributions(shard);
                if (replaced.insert(shard.unit.id).second)
                    selected.units.erase(shard.unit.id);
            }
            std::map<unit_id, std::vector<const semantic::shard*>> grouped;
            for (const auto& shard : updates)
                grouped[shard.unit.id].push_back(&shard);
            std::map<unit_id, std::vector<semantic::channel>> outputs;
            std::map<unit_id, std::set<std::string>> modules;
            for (const auto& [unit, shards] : grouped) {
                outputs[unit] = channels::outputs(channels::definitions(shards), shards);
                for (const auto* shard : shards) {
                    modules[unit].insert(shard->unit.modules.begin(), shard->unit.modules.end());
                    for (const auto& symbol : shard->symbols)
                        if (!symbol.module.empty())
                            modules[unit].insert(symbol.module);
                }
            }
            // An ID names exact content: a shard already stored under it, or repeated
            // in this batch, is selected again without writing another copy.
            std::vector<semantic::shard*> fresh;
            if (!receipts.empty() && receipts.size() != updates.size())
                throw std::logic_error("semantic receipts do not cover the submitted shards");
            std::size_t receipt_index = 0;
            for (auto& shard : updates) {
                shard.unit.output_channels = outputs.at(shard.unit.id);
                const auto& names = modules[shard.unit.id];
                shard.unit.modules.assign(names.begin(), names.end());
                shard.id = semantic::shard_key(shard);
                if (!receipts.empty()) {
                    if (auto& receipt = receipts[receipt_index++]) {
                        receipt->published = shard.id;
                        selected.receipts.push_back(std::move(*receipt));
                    }
                }
                auto& listed = selected.units[shard.unit.id];
                if (std::ranges::contains(listed, shard.id))
                    continue;
                listed.push_back(shard.id);
                if (!database_->get(persistence::table::shards, shard.id))
                    fresh.push_back(&shard);
            }
            if (!selected.receipts.empty()) {
                std::set<shard_id> included;
                for (const auto& [_, shards] : selected.units)
                    included.insert(shards.begin(), shards.end());
                std::erase_if(selected.receipts, [&](const auto& receipt) { return !included.contains(receipt.published); });
            }
            if (!fresh.empty()) {
                persistence::database::transaction journal{*database_};
                for (const auto* shard : fresh)
                    database_->rows("INSERT INTO staging VALUES (?1)", {shard->id});
                journal.commit();
            }
            struct pending_files {
                store& owner;
                bool committed = false;
                ~pending_files() {
                    if (!committed) {
                        try {
                            owner.recover();
                        } catch (...) {
                        }
                    }
                }
            } staged{*this};
            persistence::database::transaction transaction{*database_};
            for (auto* shard : fresh)
                save_shard(std::move(*shard));
            // Reused shards are selected by unit, never by a workspace-wide union.
            if (collect_coverage) {
                selected.coverage.available.clear();
            }
            std::set<std::string> scope_ids;
            std::vector<std::pair<std::string, semantic::context_id>> scope_reads;
            for (const auto& [unit, shards] : selected.units)
                for (const auto& id : shards) {
                    const auto& metadata = describe_shard(id);
                    scope_ids.insert(metadata.scope_ids.begin(), metadata.scope_ids.end());
                    for (const auto& scope : metadata.scope_reads)
                        scope_reads.emplace_back(scope, metadata.unit.context);
                    snapshot_for(selected.snapshots, metadata.unit.recipe.unit.project);
                    if (metadata.unit.id != unit)
                        throw std::invalid_argument("semantic shard belongs to another unit");
                    if (collect_coverage) {
                        selected.coverage.available.insert(selected.coverage.available.end(), metadata.unit.coverage.available.begin(),
                                                           metadata.unit.coverage.available.end());
                        selected.coverage.missing.insert(selected.coverage.missing.end(), metadata.unit.coverage.missing.begin(),
                                                         metadata.unit.coverage.missing.end());
                    }
                }
            for (const auto& [scope, context] : scope_reads)
                if (!scope_ids.contains(scope) && !std::ranges::any_of(selected.coverage.missing, [&](const auto& missing) {
                        return missing.deferred && (missing.scope.contexts.empty() || std::ranges::contains(missing.scope.contexts, context));
                    }))
                    throw std::invalid_argument("semantic scope reference has no declaration in the view");
            selected.id = identifier("view");
            selected.state = std::string{"published"};
            auto id = selected.id;
            const auto now = timestamp();
            const lifetime life{now, now + (roots ? 0 : limits_.lifetime.count())};
            insert_identities(pending);
            database_->insert(persistence::table::views, id, json::write(selected));
            reference_shards(id, selected.units);
            reference_shards(id, selected.checkpoint);
            if (remember_units)
                for (const auto& [owner, shards] : selected.units) {
                    auto candidate = contribution(selected.snapshots, shards);
                    if (eligible(candidate))
                        save_candidate(std::move(candidate));
                }
            write_leases();
            put_lease(query_lease{.view = id, .value = life});
            database_->rows("DELETE FROM staging");
            transaction.commit();
            staged.committed = true;

            pending_leases_.clear();
            pending_accesses_.clear();
            if (roots)
                roots->views.insert(id);
            return id;
        }

    public:
        // Runtime roots follow the query/task that owns the shared handle. They are
        // distinct from durable pagination leases and each worldline's current view.
        auto protect(std::set<view_id> views = {}) -> retention_roots {
            for (const auto& id : views)
                describe(id);
            std::erase_if(active_, [](const auto& reference) { return reference.expired(); });
            auto roots = std::make_shared<retained_objects>(retained_objects{.views = std::move(views)});
            active_.push_back(roots);
            return roots;
        }

        void retain(const retention_roots& roots, const view_id& id) {
            describe(id);
            roots->views.insert(id);
        }

        auto remember_query(const view_id& id, semantic::request query) -> semantic::continuation {
            describe(id);
            if (query.from_view)
                renew(*query.from_view);
            query.view = id;
            auto cursor = content::hash_of(json::write(query)).str();
            put_lease(continuation_lease{.view = id, .cursor = cursor, .query = std::move(query)});
            renew(id);
            return {id, std::move(cursor)};
        }
        auto resume(const semantic::continuation& token) -> semantic::request {
            describe(token.view);
            auto bytes = database_->get(persistence::table::leases, lease_key(continuation_lease{.view = token.view, .cursor = token.cursor}));
            if (!bytes)
                throw std::invalid_argument("unknown semantic continuation");
            renew(token.view);
            auto query = decode<continuation_lease>(*bytes).query;
            if (query.from_view)
                renew(*query.from_view);
            return query;
        }
        void renew(const view_id& id) {
            describe(id);
            const auto now = timestamp();
            pending_leases_[id] = {now, now + limits_.lifetime.count()};
        }
        void flush() {
            if (pending_leases_.empty() && pending_accesses_.empty())
                return;
            persistence::database::transaction transaction{*database_};
            write_leases();
            transaction.commit();
            pending_leases_.clear();
            pending_accesses_.clear();
        }
        auto current(const std::string& project, const std::string& worldline, const std::string& configuration) const -> std::optional<view_id> {
            auto value = database_->get(persistence::table::leases,
                                        lease_key(current_lease{.project = project, .worldline = worldline, .configuration = configuration}));
            return value ? std::optional{decode<current_lease>(*value).view} : std::nullopt;
        }
        auto currents(const std::string& project, const std::string& worldline) const -> std::vector<view_id> {
            std::vector<view_id> ids;
            for (auto& row : database_->rows("SELECT json_extract(value,'$.view') FROM leases WHERE json_extract(value,'$.kind')='current' "
                                            "AND json_extract(value,'$.project')=?1 AND json_extract(value,'$.worldline')=?2", {project, worldline}))
                ids.push_back(std::move(row.front()));
            return ids;
        }
        struct python_test_report {
            std::string name, digest, bytes;
        };
        void remember_python_test(const semantic::snapshot& pin, const python_test_report& report) {
            database_->rows("INSERT INTO python_test_reports VALUES(?1,?2,?3,?4,?5,?6,?7,CAST(?8 AS INTEGER)) "
                            "ON CONFLICT(project,worldline,version,snapshot,name) DO UPDATE SET digest=excluded.digest,report=excluded.report,expires=excluded.expires",
                            {pin.project, pin.worldline, pin.version, pin.snapshot, report.name, report.digest, report.bytes,
                             std::to_string(timestamp() + limits_.lifetime.count())});
        }
        auto python_tests(const semantic::snapshot& pin) const -> std::vector<python_test_report> {
            std::vector<python_test_report> out;
            for (auto& row : database_->rows("SELECT name,digest,report FROM python_test_reports WHERE project=?1 AND worldline=?2 AND version=?3 "
                                            "AND snapshot=?4 AND expires>CAST(?5 AS INTEGER) ORDER BY name",
                                            {pin.project, pin.worldline, pin.version, pin.snapshot, std::to_string(timestamp())}))
                out.push_back({std::move(row[0]), std::move(row[1]), std::move(row[2])});
            return out;
        }
        void make_current(const view_id& id) {
            const auto view = describe(id);
            persistence::database::transaction transaction{*database_};
            const auto project = view.computation ? view.computation->project : view.snapshots.size() == 1 ? view.snapshots.front().project : std::string{};
            if (project.empty())
                throw std::invalid_argument("current view requires a primary project");
            const auto& pin = snapshot_for(view.snapshots, project);
            put_lease(current_lease{.project = project, .worldline = pin.worldline, .configuration = view.configuration, .view = id});
            write_leases();
            transaction.commit();
            pending_leases_.clear();
            pending_accesses_.clear();
        }
        void release_current(const std::string& project, const std::string& worldline, const std::string& configuration) {
            database_->erase(persistence::table::leases, lease_key(current_lease{.project = project, .worldline = worldline, .configuration = configuration}));
        }
        void sweep() {
            flush();
            database_->rows("DELETE FROM python_test_reports WHERE expires<=CAST(?1 AS INTEGER)", {std::to_string(timestamp())});
            retained_objects roots;
            for (const auto& row : database_->rows("SELECT json_extract(value,'$.view') FROM leases WHERE json_extract(value,'$.kind')='current' OR "
                                                   "(json_extract(value,'$.kind')='query' AND json_extract(value,'$.value.expires') > CAST(?1 AS INTEGER))",
                                                   {std::to_string(timestamp())}))
                roots.views.insert(row[0]);
            std::erase_if(active_, [&](const auto& reference) {
                if (auto held = reference.lock()) {
                    roots.views.insert(held->views.begin(), held->views.end());
                    roots.candidates.insert(held->candidates.begin(), held->candidates.end());
                    roots.shards.insert(held->shards.begin(), held->shards.end());
                    return false;
                }
                return true;
            });
            std::set<view_id> removed;
            for (const auto& row : database_->rows("SELECT key FROM views"))
                if (!roots.views.contains(row[0]))
                    removed.insert(row[0]);
            if (!removed.empty())
                withdraw_views(removed);
            auto total = std::stoull(database_->rows("SELECT CAST(bytes AS TEXT) FROM storage WHERE id=1").front().front());
            std::set<shard_id> reclaimable;
            auto discount = [&](const std::string& shard, const std::string& bytes) {
                if (!roots.shards.contains(shard) && reclaimable.insert(shard).second)
                    total -= std::min(total, std::stoull(bytes));
            };
            for (const auto& row :
                 database_->rows("SELECT s.key,CAST(json_extract(s.value,'$.bytes')+json_extract(s.value,'$.index_bytes') AS TEXT) "
                                 "FROM garbage g JOIN shards s ON s.key=g.shard WHERE NOT EXISTS (SELECT 1 FROM shard_refs r WHERE r.shard=g.shard)"))
                discount(row[0], row[1]);
            if (total > limits_.bytes) {
                persistence::database::transaction transaction{*database_};
                struct candidate_shard {
                    shard_id shard;
                    std::string bytes;
                    std::size_t owners = 0;
                };
                // Candidates are reclaimed oldest first, in bounded batches. The cursor
                // rides on (accessed,key), which erasing a candidate never changes, so
                // deleting rows behind the cursor cannot reorder or skip the rest.
                std::optional<std::pair<std::int64_t, std::string>> cursor;
                while (true) {
                    const auto page =
                        cursor ? database_->rows("SELECT key,CAST(accessed AS TEXT) FROM recipes WHERE accessed>CAST(?1 AS INTEGER) OR "
                                                 "(accessed=CAST(?1 AS INTEGER) AND key>?2) ORDER BY accessed,key LIMIT " +
                                                     std::to_string(reclaim_batch),
                                                 {std::to_string(cursor->first), cursor->second})
                               : database_->rows("SELECT key,CAST(accessed AS TEXT) FROM recipes ORDER BY accessed,key LIMIT " + std::to_string(reclaim_batch));
                    if (page.empty())
                        break;
                    std::vector<std::string> wanted;
                    for (const auto& row : page)
                        if (!roots.candidates.contains(row[0]))
                            wanted.push_back(row[0]);
                    // One read resolves the whole batch. owners counts every other durable
                    // reference; batch-local candidates that are erased go away as the batch
                    // is walked, which is exactly the point-in-time test a per-candidate query
                    // would make after those deletions.
                    std::map<std::string, std::vector<candidate_shard>> references;
                    std::map<shard_id, std::vector<std::string>> shared;
                    if (!wanted.empty())
                        for (auto& row :
                             database_->rows("SELECT r.owner,r.shard,CAST(json_extract(s.value,'$.bytes')+json_extract(s.value,'$.index_bytes') AS TEXT),"
                                             "(SELECT COUNT(*) FROM shard_refs other WHERE other.shard=r.shard AND other.owner<>r.owner) "
                                             "FROM shard_refs r JOIN shards s ON s.key=r.shard WHERE r.owner IN (SELECT value FROM json_each(?1))",
                                             {json::write(wanted)})) {
                            auto shard = std::move(row[1]);
                            references[row[0]].push_back({shard, std::move(row[2]), static_cast<std::size_t>(std::stoull(row[3]))});
                            shared[shard].push_back(row[0]);
                        }
                    std::set<std::string> erased;
                    bool satisfied = false;
                    for (const auto& key : wanted) {
                        const auto found = references.find(key);
                        if (found == references.end())
                            continue;
                        std::vector<const candidate_shard*> released;
                        for (const auto& shard : found->second) {
                            auto remaining = shard.owners;
                            for (const auto& holder : shared.at(shard.shard))
                                if (holder != key && erased.contains(holder))
                                    --remaining;
                            if (remaining == 0)
                                released.push_back(&shard);
                        }
                        // Only shards whose last durable reference is this candidate can reduce storage.
                        const bool frees_bytes = std::ranges::any_of(released, [&](const auto* shard) {
                            return !roots.shards.contains(shard->shard) && !reclaimable.contains(shard->shard) && std::stoull(shard->bytes) != 0;
                        });
                        if (!frees_bytes)
                            continue;
                        erase_candidate(key);
                        erased.insert(key);
                        for (const auto* shard : released)
                            discount(shard->shard, shard->bytes);
                        if (total <= limits_.bytes) {
                            satisfied = true;
                            break;
                        }
                    }
                    if (satisfied)
                        break;
                    cursor = {std::stoll(page.back()[1]), page.back()[0]};
                }
                transaction.commit();
            }
            collect();
        }
        void remember(const view_id& id, const unit_key& key, bool cacheable = true) {
            const auto view = describe(id);
            const auto found = view.units.find(unit_identifier(key));
            if (found == view.units.end())
                return;
            auto value = contribution(view.snapshots, found->second);
            value.cacheable &= cacheable;
            if (!eligible(value))
                return;
            persistence::database::transaction transaction{*database_};
            save_candidate(std::move(value));
            write_accesses();
            transaction.commit();
            pending_accesses_.clear();
        }
        auto candidates(const std::string& project, const std::string& options, const source_vector& sources) -> std::vector<semantic::candidate> {
            if (database_->rows("SELECT 1 FROM recipes WHERE project=?1 AND options=?2 LIMIT 1", {project, options}).empty())
                return {};
            persistence::database::transaction transaction{*database_};
            auto& state = validation_state(project, update_sources(project, sources));
            std::vector<std::string> dirty;
            for (auto& row : database_->rows("SELECT key FROM recipes WHERE project=?1 AND options=?2 AND dirty=1", {project, options}))
                dirty.push_back(std::move(row[0]));
            std::vector<std::string> unresolved;
            for (const auto& key : dirty)
                if (!state.results.contains(key))
                    unresolved.push_back(key);
            if (!unresolved.empty()) {
                const semantic::input_vector inputs;
                const reuse::channel_values channels;
                const reuse::environment current{sources, inputs, channels, project};
                for (const auto& row : database_->rows("SELECT key,CAST(accessed AS TEXT),value FROM recipes WHERE key IN (SELECT value FROM json_each(?1))",
                                                       {json::write(unresolved)}))
                    state.results.emplace(row[0], reuse::sources_match(candidate_record(row), current));
            }
            std::vector<std::string> valid, invalid;
            for (auto& key : dirty) {
                const bool matched = state.results.at(key);
                (matched ? valid : invalid).push_back(std::move(key));
            }
            settle(valid, true);
            settle(invalid, false);
            transaction.commit();
            std::vector<semantic::candidate> out;
            for (const auto& row : database_->rows("SELECT key,CAST(accessed AS TEXT),value FROM recipes WHERE project=?1 AND options=?2 AND source_valid=1 "
                                                   "ORDER BY accessed DESC,key",
                                                   {project, options}))
                out.push_back(candidate_record(row));
            return out;
        }
        auto offer(const snapshot_vector& targets, const std::string& project, const std::string& options, const source_vector& inputs,
                   const std::map<unit_id, std::vector<shard_id>>& checkpoint, const retention_roots& roots) -> std::vector<semantic::candidate> {
            auto available = candidates(project, options, inputs);
            const semantic::input_vector installed;
            const reuse::channel_values channels;
            const reuse::environment current{inputs, installed, channels, project};
            std::vector<semantic::candidate> reconstructed;
            std::set<std::vector<shard_id>> offered;
            for (const auto& value : available)
                offered.insert(value.shards);
            for (const auto& [unit, shards] : checkpoint) {
                if (shards.empty() || offered.contains(shards))
                    continue;
                auto value = contribution(targets, shards);
                if (value.recipe.unit.project != project || value.recipe.options != options || !eligible(value) || !reuse::sources_match(value, current))
                    continue;
                offered.insert(shards);
                reconstructed.push_back(value);
                available.push_back(std::move(value));
            }
            if (!reconstructed.empty()) {
                persistence::database::transaction transaction{*database_};
                for (auto& value : reconstructed)
                    save_candidate(std::move(value));
                write_accesses();
                transaction.commit();
                pending_accesses_.clear();
            }
            for (auto& candidate : available) {
                if (roots) {
                    roots->candidates.insert(candidate.id);
                    roots->shards.insert(candidate.shards.begin(), candidate.shards.end());
                }
                auto found = checkpoint.find(unit_identifier(candidate.recipe.unit));
                candidate.retained = found != checkpoint.end() && found->second == candidate.shards;
            }
            std::stable_partition(available.begin(), available.end(), [](const auto& item) { return item.retained; });
            return available;
        }

        void require_symbol(const symbol_id& id) const {
            if (database_->rows("SELECT 1 FROM shard_identities WHERE symbol=?1 LIMIT 1", {id}).empty())
                throw expired_symbol("symbol identity is no longer retained; select the declaration again by name");
        }

        auto object_unit(const shard_id& id) const -> semantic::unit { return describe_shard(id).unit; }
        auto object_records(const shard_id& id) const -> std::shared_ptr<const semantic::shard> { return load_shard(id)->materialized(); }
        auto object_bytes(const shard_id& id, std::uint64_t offset, std::uint64_t limit) const -> std::pair<std::uint64_t, std::vector<std::byte>> {
            const auto total = describe_shard(id).bytes;
            if (offset > total || !limit || limit > constants::semantic_batch_bytes)
                throw std::invalid_argument("invalid semantic object range");
            std::vector<std::byte> bytes(std::min(limit, total - offset));
            std::ifstream input{shard_path(id), std::ios::binary};
            input.seekg(offset);
            input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
            if (!input)
                throw std::runtime_error("semantic object could not be read");
            return {total, std::move(bytes)};
        }

        auto describe(const view_id& id) const -> semantic::view {
            const auto bytes = database_->get(persistence::table::views, id);
            if (!bytes)
                throw expired_view("semantic view is no longer retained: " + id);
            return decode<semantic::view>(*bytes);
        }

        auto artifact_roots() const -> std::set<std::string> {
            std::set<std::string> result;
            // A cached candidate never needs its old container: installation captures
            // the current container before inventory and member reuse. Views do need it.
            for (const auto& row :
                 database_->rows("SELECT DISTINCT o.object FROM shard_objects o JOIN shard_refs r ON r.shard=o.shard JOIN views v ON v.key=r.owner"))
                result.insert(row[0]);
            auto shard = [&](const shard_id& id) {
                for (const auto& row : database_->rows("SELECT object FROM shard_objects WHERE shard=?1", {id}))
                    result.insert(row[0]);
            };
            for (const auto& reference : active_)
                if (auto held = reference.lock()) {
                    for (const auto& id : held->shards)
                        shard(id);
                    for (const auto& id : held->views)
                        for (const auto& row :
                             database_->rows("SELECT o.object FROM shard_objects o JOIN shard_refs r ON r.shard=o.shard WHERE r.owner=?1", {id}))
                            result.insert(row[0]);
                }
            // A query may still hold an immutable view after its durable lease retires.
            for (const auto& [_, reference] : live_views_)
                if (auto held = reference.lock())
                    for (const auto& [_, object] : held->artifact_inputs)
                        result.insert(object);
            std::erase_if(retired_views_, [](const auto& reference) { return reference.expired(); });
            for (const auto& reference : retired_views_)
                if (auto held = reference.lock())
                    for (const auto& [_, object] : held->artifact_inputs)
                        result.insert(object);
            return result;
        }

        auto load(const view_id& id) const -> std::shared_ptr<const indexed_view> {
            if (auto found = views_.find(id))
                return found;
            if (auto found = live_views_[id].lock())
                return found;
            const auto description = describe(id);
            auto out = std::make_shared<indexed_view>();
            out->description = description;
            std::uint64_t bytes = 0;
            for (const auto& [unit, shards] : description.units)
                for (const auto& id : shards) {
                    const auto piece = load_shard(id);
                    const auto index = out->pieces.size();
                    out->pieces.push_back(piece);
                    out->unit_projects.emplace(unit, piece->unit.recipe.unit.project);
                    out->languages.add(piece->unit.conventions);
                    out->unit_parts[unit].push_back(index);
                    for (const auto& input : piece->unit.direct_inputs)
                        if (input.kind == input_kind::artifact && !input.object.empty()) {
                            artifact_input key{piece->unit.recipe.unit.project, piece->unit.recipe.unit.stage, input.name};
                            const auto [found, inserted] = out->artifact_inputs.emplace(std::move(key), input.object);
                            if (!inserted && found->second != input.object)
                                throw std::invalid_argument("view has conflicting artifact input bindings");
                        }
                    for (const auto& [symbol, contexts] : piece->index->comparisons)
                        for (const auto& [context, _] : contexts)
                            out->comparison_parts.emplace(comparison_key{symbol, context}, index);
                    for (const auto& read : piece->unit.observed_dependencies) {
                        if (read.producer)
                            out->unit_consumers[*read.producer].insert(unit);
                        for (const auto& producer : read.producers)
                            out->unit_consumers[producer].insert(unit);
                    }
                    const auto& metadata = describe_shard(id);
                    bytes += metadata.bytes + 2 * metadata.index_bytes;
                    auto append = [&](auto& into, const auto& from) {
                        for (const auto& [key, rows] : from)
                            for (const auto row : rows)
                                into.emplace(key, record_address{index, row});
                    };
                    append(out->names, piece->index->names);
                    append(out->symbols, piece->index->symbols);
                    append(out->positions, piece->index->positions);
                    append(out->references, piece->index->references);
                    append(out->outgoing, piece->index->outgoing);
                    append(out->incoming, piece->index->incoming);
                    append(out->occurrences, piece->index->occurrences);
                    append(out->occurrence_ids, piece->index->occurrence_ids);
                    append(out->reference_ids, piece->index->reference_ids);
                    append(out->reference_files, piece->index->reference_files);
                    append(out->unresolved, piece->index->unresolved);
                    append(out->types, piece->index->types);
                    append(out->type_declarations, piece->index->type_declarations);
                    for (const auto& [parent, children] : piece->index->type_children)
                        for (const auto& child : children)
                            out->type_parents[child].insert(parent);
                    append(out->typed_overloads, piece->index->typed_overloads);
                    append(out->typed_range_files, piece->index->typed_range_files);
                    if (piece->index->typed_observations)
                        append(out->typed_observations, *piece->index->typed_observations);
                    append(out->scopes, piece->index->scopes);
                    append(out->scope_owners, piece->index->scope_owners);
                    append(out->members, piece->index->members);
                    append(out->type_producers, piece->index->type_producers);
                    append(out->type_consumers, piece->index->type_consumers);
                    append(out->diagnostics, piece->index->diagnostics);
                    if (piece->unit.resolution) {
                        const auto& resolution = *piece->unit.resolution;
                        out->resolutions.emplace(json::write(std::tuple{resolution.action.str(), std::optional{resolution.selector}}), index);
                    }
                }
            for (const auto& [function, address] : out->typed_observations) {
                const auto& sample = out->typed_range(address);
                const auto& observed = *sample.observation;
                if (observed.role != "return" && observed.role != "first_parameter")
                    continue;
                const auto [types, end_types] = out->types.equal_range(sample.type);
                for (auto at = types; at != end_types; ++at) {
                    if (at->second.shard != address.shard)
                        continue;
                    const auto& type = out->type(at->second);
                    if (!type.declaration)
                        continue;
                    auto& capabilities = observed.role == "return" ? out->type_producers : out->type_consumers;
                    const auto [first, last] = out->symbols.equal_range(function);
                    for (auto symbol = first; symbol != last; ++symbol)
                        if (out->symbol(symbol->second).ownership.context == observed.context)
                            capabilities.emplace(*type.declaration, symbol->second);
                }
            }
            std::erase_if(live_views_, [](const auto& entry) { return entry.second.expired(); });
            live_views_[id] = out;
            views_.put(id, out, bytes, limits_.hot_bytes);
            return out;
        }

        // First withdraw the view atomically; only then collect unreferenced shards.
        // An in-flight reader already owns its immutable index and remains valid.
        void retire(const view_id& id) {
            describe(id);
            withdraw_views({id});
            collect();
        }

        void collect() {
            auto abandoned = database_->rows("SELECT shard FROM garbage WHERE NOT EXISTS (SELECT 1 FROM shard_refs WHERE shard=garbage.shard)");
            std::set<std::string> retained;
            for (const auto& reference : active_)
                if (auto roots = reference.lock())
                    retained.insert(roots->shards.begin(), roots->shards.end());
            std::erase_if(abandoned, [&](const auto& row) { return retained.contains(row[0]); });
            persistence::database::transaction transaction{*database_};
            for (const auto& row : abandoned) {
                const auto& id = row[0];
                database_->rows("INSERT OR IGNORE INTO orphan_symbols SELECT symbol FROM shard_identities WHERE shard=?1", {id});
                database_->rows("DELETE FROM shard_identities WHERE shard=?1", {id});
                database_->rows("DELETE FROM shard_objects WHERE shard=?1", {id});
                database_->rows("UPDATE storage SET bytes=bytes-COALESCE((SELECT json_extract(value,'$.bytes')+json_extract(value,'$.index_bytes') FROM shards "
                                "WHERE key=?1),0) WHERE id=1",
                                {id});
                database_->erase(persistence::table::shards, id);
                database_->rows("DELETE FROM garbage WHERE shard=?1", {id});
            }
            for (const auto& row :
                 database_->rows("SELECT symbol FROM orphan_symbols WHERE NOT EXISTS (SELECT 1 FROM shard_identities WHERE symbol=orphan_symbols.symbol)")) {
                const auto encoded = json::write(row[0]);
                for (const auto& key : database_->rows("SELECT key FROM identities WHERE value=?1", {encoded}))
                    identities_.erase(key[0]);
                database_->rows("DELETE FROM identities WHERE value=?1", {encoded});
            }
            database_->rows("DELETE FROM source_inputs WHERE owner IN (SELECT owner FROM source_reclaims q WHERE NOT EXISTS (SELECT 1 FROM recipes r WHERE "
                            "r.project=q.owner))");
            database_->rows("DELETE FROM source_projects WHERE owner IN (SELECT owner FROM source_reclaims q WHERE NOT EXISTS (SELECT 1 FROM recipes r WHERE "
                            "r.project=q.owner))");
            database_->rows("DELETE FROM source_reclaims");
            // A project with no remaining recipes has no candidates to resolve, so any
            // remembered validation state for it is dead weight.
            std::erase_if(validation_states_,
                          [&](const auto& state) { return database_->rows("SELECT 1 FROM recipes WHERE project=?1 LIMIT 1", {state.project}).empty(); });
            database_->rows("DELETE FROM orphan_symbols");
            database_->rows("DELETE FROM garbage WHERE EXISTS (SELECT 1 FROM shard_refs WHERE shard=garbage.shard)");
            transaction.commit();
            for (const auto& row : abandoned) {
                objects_.erase(row[0]);
                live_objects_.erase(row[0]);
                durability::durable_remove(shard_path(row[0]));
                durability::durable_remove(object_path("indexes", row[0]));
            }
        }

    private:
        void recover() {
            const auto pending = database_->rows("SELECT shard FROM staging");
            if (pending.empty())
                return;
            for (const auto& row : pending) {
                objects_.erase(row[0]);
                live_objects_.erase(row[0]);
                durability::durable_remove(shard_path(row[0]));
                durability::durable_remove(object_path("indexes", row[0]));
            }
            database_->rows("DELETE FROM staging");
        }
        auto timestamp() const -> std::int64_t { return std::chrono::duration_cast<std::chrono::milliseconds>(clock_().time_since_epoch()).count(); }
        auto identity(const declaration_identity& declaration) const -> std::optional<symbol_id> {
            auto key = json::write(declaration);
            if (auto cached = identities_.find(key))
                return *cached;
            auto bytes = database_->get(persistence::table::identities, key);
            if (!bytes)
                return {};
            auto value = std::make_shared<const symbol_id>(decode<symbol_id>(*bytes));
            identities_.put(key, value, key.size() + value->size() + 128, limits_.hot_bytes / 8);
            return *value;
        }
        void reference_shards(const std::string& owner, const std::map<unit_id, std::vector<shard_id>>& units) {
            for (const auto& [unit, shards] : units)
                for (const auto& id : shards) {
                    database_->rows("INSERT OR IGNORE INTO shard_refs VALUES (?1,?2)", {owner, id});
                    database_->rows("DELETE FROM garbage WHERE shard=?1", {id});
                }
        }
        auto contribution(const snapshot_vector& snapshots, const std::vector<shard_id>& shards) const -> semantic::candidate {
            semantic::candidate value{.id = identifier("candidate"), .snapshots = snapshots, .shards = shards, .accessed = timestamp()};
            for (const auto& id : shards) {
                const auto metadata = describe_shard(id);
                if (!value.units.empty() &&
                    (value.recipe != metadata.unit.recipe || value.units.front().channel_definitions != metadata.unit.channel_definitions))
                    throw std::invalid_argument("candidate shards have different recipes or channel definitions");
                value.recipe = metadata.unit.recipe;
                value.cacheable &= metadata.unit.cacheable &&
                                   (!metadata.unit.direct_inputs.empty() || !metadata.unit.observed_dependencies.empty()) &&
                                   metadata.unit.coverage.missing.empty();
                value.units.push_back(metadata.unit);
                value.nodes += metadata.nodes;
            }
            value.cacheable &= !shards.empty();
            return value;
        }
        void erase_candidate(const std::string& id) {
            database_->rows("INSERT OR IGNORE INTO source_reclaims SELECT project FROM recipes WHERE key=?1", {id});
            database_->erase(persistence::table::recipes, id);
            database_->rows("DELETE FROM candidate_inputs WHERE candidate_id=?1", {id});
            pending_accesses_.erase(id);
            candidates_.erase(id);
            for (auto& state : validation_states_)
                state.results.erase(id);
            database_->rows("INSERT OR IGNORE INTO garbage SELECT shard FROM shard_refs WHERE owner=?1", {id});
            database_->rows("DELETE FROM shard_refs WHERE owner=?1", {id});
        }
        static auto eligible(const semantic::candidate& value) -> bool {
            return !value.units.empty() && std::ranges::all_of(value.units, [](const auto& unit) {
                return (!unit.direct_inputs.empty() || !unit.observed_dependencies.empty()) && unit.coverage.missing.empty();
            });
        }
        static auto input_state(const semantic::candidate& value) -> std::string {
            std::vector<std::tuple<std::vector<input_object>, std::vector<dependency>, semantic::coverage>> inputs;
            for (const auto& unit : value.units)
                inputs.emplace_back(unit.direct_inputs, unit.observed_dependencies, unit.coverage);
            return json::write(inputs);
        }
        auto candidate_record(const std::vector<std::string>& row) const -> semantic::candidate {
            auto cached = candidates_.find(row[0]);
            if (!cached) {
                auto payload = decode<candidate_payload>(row[2]);
                auto value = std::make_shared<semantic::candidate>();
                value->id = row[0];
                value->snapshots = std::move(payload.snapshots);
                value->shards = std::move(payload.shards);
                value->units = std::move(payload.units);
                value->recipe = value->units.front().recipe;
                value->nodes = payload.nodes;
                value->cacheable = payload.cacheable;
                candidates_.put(row[0], value, row[2].size(), limits_.hot_bytes / 8);
                cached = std::move(value);
            }
            auto result = *cached;
            result.accessed = std::stoll(row[1]);
            return result;
        }
        void index_candidate(const semantic::candidate& value) {
            // Register the set of watched inputs, not one insertion attempt per read:
            // the same file is commonly read by several units of a candidate, and the
            // project-level wildcard only needs one row per project. update_sources only
            // joins on (project,input_key), so this set is the complete dirty contract.
            std::set<std::pair<std::string, std::string>> watched;
            std::set<std::string> projects;
            auto watch = [&](const std::string& project, const std::string& key) {
                watched.emplace(project, key);
                projects.insert(project);
            };
            for (const auto& unit : value.units) {
                const auto& owner = unit.recipe.unit.project;
                for (const auto& input : unit.direct_inputs)
                    if (input.kind == input_kind::file)
                        watch(owner, "file:" + input.name);
                for (const auto& read : unit.observed_dependencies) {
                    const auto& project = read.project.empty() ? owner : read.project;
                    if (read.kind == dependency_kind::source)
                        watch(project, "file:" + read.key);
                    else if (read.kind == dependency_kind::negative_lookup && !read.scope) {
                        auto path = read.key;
                        while (path.ends_with('/'))
                            path.pop_back();
                        watch(project, "member:" + path);
                        watch(project, "tree:" + path);
                    } else if (read.kind == dependency_kind::enumeration && !read.scope && read.selection) {
                        watch(project, "project");
                        for (const auto& path : read.selection->files)
                            watch(project, "member:" + path);
                        for (const auto& path : read.selection->directories)
                            watch(project, "tree:" + (path == "." ? std::string{} : path));
                    }
                }
            }
            std::vector<std::tuple<std::string, std::string, std::string>> rows;
            rows.reserve(watched.size() + projects.size());
            for (const auto& [project, key] : watched)
                rows.emplace_back(project, key, value.id);
            for (const auto& project : projects)
                if (!watched.contains({project, "project"}))
                    rows.emplace_back(project, "project", value.id);
            if (rows.empty())
                return;
            database_->rows("INSERT OR IGNORE INTO candidate_inputs(project,input_key,candidate_id) "
                            "SELECT json_extract(value,'$[0]'),json_extract(value,'$[1]'),json_extract(value,'$[2]') FROM json_each(?1)",
                            {json::write(rows)});
        }
        auto update_sources(const std::string& owner, const source_vector& sources) -> std::string {
            database_->rows("DELETE FROM next_sources");
            database_->rows("DELETE FROM next_projects");
            database_->rows("DELETE FROM changed_inputs");
            auto encoded = json::write(sources);
            database_->rows("INSERT INTO next_projects SELECT key FROM json_each(?1)", {encoded});
            database_->rows("INSERT INTO next_sources SELECT p.key,f.key,f.value FROM json_each(?1) p,json_each(p.value) f", {encoded});
            auto mark = [&](const std::string& project, const std::string& key) {
                database_->rows("INSERT OR IGNORE INTO changed_inputs VALUES (?1,?2)", {project, key});
            };
            for (const auto& row :
                 database_->rows("SELECT n.project,n.path,CASE WHEN s.path IS NULL THEN 'membership' ELSE 'content' END FROM next_sources n "
                                 "LEFT JOIN source_inputs s ON s.owner=?1 AND s.project=n.project AND s.path=n.path WHERE s.path IS NULL OR s.object<>n.object "
                                 "UNION ALL SELECT s.project,s.path,'membership' FROM source_inputs s LEFT JOIN next_sources n "
                                 "ON n.project=s.project AND n.path=s.path WHERE s.owner=?1 AND n.path IS NULL",
                                 {owner})) {
                mark(row[0], "file:" + row[1]);
                if (row[2] == "membership") {
                    mark(row[0], "member:" + row[1]);
                    auto path = std::filesystem::path{row[1]}.parent_path();
                    for (;;) {
                        mark(row[0], "tree:" + path.generic_string());
                        if (path.empty())
                            break;
                        path = path.parent_path();
                    }
                }
            }
            for (const auto& row :
                 database_->rows("SELECT project FROM next_projects WHERE project NOT IN (SELECT project FROM source_projects WHERE owner=?1) "
                                 "UNION SELECT project FROM source_projects WHERE owner=?1 AND project NOT IN (SELECT project FROM next_projects)",
                                 {owner}))
                mark(row[0], "project");
            database_->rows("UPDATE recipes SET dirty=1 WHERE project=?1 AND key IN (SELECT c.candidate_id FROM changed_inputs d "
                            "JOIN candidate_inputs c ON c.project=d.project AND c.input_key=d.input_key)",
                            {owner});
            database_->rows("DELETE FROM source_inputs WHERE owner=?1 AND NOT EXISTS (SELECT 1 FROM next_sources n WHERE n.project=source_inputs.project AND "
                            "n.path=source_inputs.path)",
                            {owner});
            database_->rows(
                "INSERT INTO source_inputs SELECT ?1,n.* FROM next_sources n LEFT JOIN source_inputs s ON s.owner=?1 AND s.project=n.project AND s.path=n.path "
                "WHERE s.path IS NULL OR s.object<>n.object ON CONFLICT(owner,project,path) DO UPDATE SET object=excluded.object",
                {owner});
            database_->rows("DELETE FROM source_projects WHERE owner=?1 AND project NOT IN (SELECT project FROM next_projects)", {owner});
            database_->rows("INSERT OR IGNORE INTO source_projects SELECT ?1,project FROM next_projects", {owner});
            return std::move(encoded);
        }
        auto validation_state(const std::string& project, std::string sources) -> validated_state& {
            auto found = std::ranges::find_if(validation_states_, [&](const auto& state) { return state.project == project && state.sources == sources; });
            if (found != validation_states_.end()) {
                if (found != validation_states_.begin())
                    std::rotate(validation_states_.begin(), found, std::next(found));
                return validation_states_.front();
            }
            if (validation_states_.size() == validation_state_limit)
                validation_states_.pop_back();
            validation_states_.push_front({project, std::move(sources)});
            return validation_states_.front();
        }
        void settle(const std::vector<std::string>& keys, bool valid) {
            if (keys.empty())
                return;
            database_->rows("UPDATE recipes SET dirty=0,source_valid=?2 WHERE key IN (SELECT value FROM json_each(?1))",
                            {json::write(keys), valid ? "1" : "0"});
        }
        void save_candidate(semantic::candidate value) {
            const auto key = recipe_identifier(value.recipe);
            auto previous = database_->rows("SELECT key,CAST(accessed AS TEXT),value FROM recipes WHERE recipe_id=?1 ORDER BY accessed DESC,key", {key});
            std::size_t kept = 1;
            const auto inputs = input_state(value);
            for (const auto& row : previous) {
                const auto old = candidate_record(row);
                if (old.shards == value.shards) {
                    pending_accesses_.insert(old.id);
                    return;
                }
                if (input_state(old) == inputs || kept >= constants::semantic_candidates_per_unit)
                    erase_candidate(row[0]);
                else
                    ++kept;
            }
            candidate_payload payload{value.snapshots, value.shards, value.units, value.nodes, value.cacheable};
            database_->rows("INSERT INTO recipes(key,recipe_id,project,options,accessed,value) VALUES (?1,?2,?3,?4,?5,?6)",
                            {value.id, key, value.recipe.unit.project, value.recipe.options, std::to_string(value.accessed), json::write(payload)});
            index_candidate(value);
            reference_shards(value.id, {{unit_identifier(value.recipe.unit), value.shards}});
        }
        void withdraw_views(const std::set<view_id>& ids) {
            persistence::database::transaction transaction{*database_};
            for (const auto& id : ids) {
                database_->erase(persistence::table::views, id);
                database_->rows("DELETE FROM leases WHERE json_extract(value,'$.view')=?1", {id});
                database_->rows("INSERT OR IGNORE INTO garbage SELECT shard FROM shard_refs WHERE owner=?1", {id});
                database_->rows("DELETE FROM shard_refs WHERE owner=?1", {id});
                pending_leases_.erase(id);
                if (const auto found = live_views_.find(id); found != live_views_.end())
                    retired_views_.push_back(found->second);
                views_.erase(id);
                live_views_.erase(id);
            }
            write_leases();
            transaction.commit();
            pending_leases_.clear();
            pending_accesses_.clear();
        }

        static auto identifier(std::string_view kind) -> std::string { return std::string{kind} + "-" + nodes::hex(nodes::random_bytes(16)); }

        auto object_path(std::string_view part, const shard_id& id) const -> std::filesystem::path {
            content::checked_name(id);
            return directory_ / part / id;
        }
        auto shard_path(const shard_id& id) const -> std::filesystem::path { return object_path("shards", id); }
        auto describe_shard(const shard_id& id) const -> shard_metadata {
            auto value = database_->get(persistence::table::shards, id);
            if (!value)
                throw std::invalid_argument("unknown semantic shard: " + id);
            return decode<shard_metadata>(*value);
        }
        auto load_shard(const shard_id& id) const -> std::shared_ptr<const indexed_shard> {
            if (auto found = objects_.find(id))
                return found;
            if (auto found = live_objects_[id].lock())
                return found;
            const auto metadata = describe_shard(id);
            auto value = std::make_shared<indexed_shard>();
            value->id = id;
            value->unit = metadata.unit;
            value->index = std::make_shared<const shard_index>(read<shard_index>(object_path("indexes", id)));
            value->document = document(shard_path(id));
            std::erase_if(live_objects_, [](const auto& entry) { return entry.second.expired(); });
            live_objects_[id] = value;
            objects_.put(id, value, metadata.bytes + metadata.index_bytes, limits_.hot_bytes);
            return value;
        }
        void save_shard(semantic::shard shard) {
            const auto id = shard.id;
            const auto index = std::make_shared<const shard_index>(index_shard(shard));
            auto persist = [&](const std::filesystem::path& path, const auto& value) {
                auto bytes = json::write(value);
                if (bytes.size() > constants::semantic_object_bytes)
                    throw std::length_error("semantic object exceeds storage limit");
                durability::durable_write(path, bytes);
                return bytes;
            };
            auto bytes = persist(shard_path(id), shard);
            shard_metadata metadata{.unit = shard.unit,
                                    .bytes = bytes.size(),
                                    .index_bytes = persist(object_path("indexes", id), *index).size(),
                                    .nodes = shard.identities.size() + shard.symbols.size() + shard.types.size() + shard.occurrences.size() +
                                             shard.references.size() + shard.scopes.size() + shard.relations.size() + shard.diagnostics.size() +
                                             shard.typed_ranges.size()};
            // Keep the unit's file membership with the metadata; the record-id
            // lists repeat columns the shard document and index already carry.
            metadata.unit.contributions = {.files = shard.unit.contributions.files};
            std::set<std::string> scopes, reads;
            for (const auto& scope : shard.scopes) {
                scopes.insert(scope.id);
                if (scope.lexical_parent)
                    reads.insert(*scope.lexical_parent);
            }
            for (const auto& symbol : shard.symbols)
                if (symbol.declaring_scope)
                    reads.insert(*symbol.declaring_scope);
            metadata.scope_ids.assign(scopes.begin(), scopes.end());
            metadata.scope_reads.assign(reads.begin(), reads.end());
            database_->put(persistence::table::shards, id, json::write(metadata));
            database_->rows("UPDATE storage SET bytes=bytes+CAST(?1 AS INTEGER) WHERE id=1", {std::to_string(metadata.bytes + metadata.index_bytes)});
            database_->rows("INSERT INTO garbage VALUES (?1)", {id});
            for (const auto& symbol : index->identities)
                database_->rows("INSERT INTO shard_identities VALUES (?1,?2)", {id, symbol});
            for (const auto& input : shard.unit.direct_inputs)
                if (input.kind == input_kind::artifact && !input.object.empty())
                    database_->rows("INSERT OR IGNORE INTO shard_objects VALUES (?1,?2)", {id, input.object});
            auto value = std::make_shared<indexed_shard>();
            value->id = id;
            value->unit = metadata.unit;
            value->index = index;
            value->document = std::make_shared<const std::string>(std::move(bytes));
            std::erase_if(live_objects_, [](const auto& entry) { return entry.second.expired(); });
            live_objects_[id] = value;
            objects_.put(id, value, metadata.bytes + metadata.index_bytes, limits_.hot_bytes);
        }

        static auto document(const std::filesystem::path& path) -> std::shared_ptr<const std::string> {
            files::mapping mapped{path};
            const auto bytes = mapped.bytes();
            if (bytes.size() > constants::semantic_object_bytes)
                throw std::length_error("semantic object exceeds storage limit");
            return std::make_shared<const std::string>(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }

        template <class T> static auto read(const std::filesystem::path& path) -> T {
            files::mapping mapped{path};
            const auto bytes = mapped.bytes();
            if (bytes.size() > constants::semantic_object_bytes)
                throw std::length_error("semantic object exceeds storage limit");
            auto value = json::read<T>(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            if (!value)
                throw std::runtime_error("invalid semantic object: " + path.string() + ": " + value.error().what());
            return std::move(*value);
        }

        template <class T> static auto decode(std::string_view bytes) -> T {
            auto value = json::read<T>(bytes);
            if (!value)
                throw std::runtime_error("invalid semantic database record: " + value.error().what());
            return std::move(*value);
        }
        static auto lease_key(const query_lease& lease) -> std::string { return json::write(std::tuple{std::string{"query"}, lease.view}); }
        static auto lease_key(const current_lease& lease) -> std::string {
            return json::write(std::tuple{std::string{"current"}, lease.project, lease.worldline, lease.configuration});
        }
        static auto lease_key(const continuation_lease& lease) -> std::string {
            return json::write(std::tuple{std::string{"continuation"}, lease.view, lease.cursor});
        }
        void put_lease(const auto& lease) { database_->put(persistence::table::leases, lease_key(lease), json::write(lease)); }
        void write_accesses() {
            if (!pending_accesses_.empty())
                database_->rows("UPDATE recipes SET accessed=MAX(accessed,CAST(?1 AS INTEGER)) WHERE key IN (SELECT value FROM json_each(?2))",
                                {std::to_string(timestamp()), json::write(pending_accesses_)});
        }
        void write_leases(const std::set<view_id>& excluded = {}) {
            write_accesses();
            for (const auto& [id, life] : pending_leases_)
                if (!excluded.contains(id))
                    put_lease(query_lease{.view = id, .value = life});
        }
        void insert_identities(const identity_index& pending) {
            for (const auto& [declaration, symbol] : pending) {
                database_->insert(persistence::table::identities, json::write(declaration), json::write(symbol));
                database_->rows("INSERT OR IGNORE INTO orphan_symbols VALUES (?1)", {symbol});
            }
        }
        void check_identities(const semantic::shard& shard, const identity_index& pending) const {
            for (const auto& binding : shard.identities) {
                const auto found = identity(binding.declaration);
                const auto staged = pending.find(binding.declaration);
                if (!((found && *found == binding.symbol) || (staged != pending.end() && staged->second == binding.symbol)))
                    throw std::invalid_argument("semantic declaration identity was not assigned by the store");
            }
        }

        static void check_ownership(const semantic::shard& shard) {
            auto check = [&](const auto& rows) {
                for (const auto& row : rows)
                    if (row.ownership.owner_unit != shard.unit.id || row.ownership.context != shard.unit.context)
                        throw std::invalid_argument("semantic record belongs to another unit or semantic context");
            };
            check(shard.symbols);
            check(shard.occurrences);
            check(shard.references);
            check(shard.scopes);
            check(shard.relations);
            check(shard.diagnostics);
            check(shard.typed_ranges);
        }

        std::filesystem::path directory_;
        retention limits_;
        std::function<std::chrono::system_clock::time_point()> clock_;
        std::vector<std::weak_ptr<retained_objects>> active_;
        std::unique_ptr<persistence::database> database_;
        static constexpr std::size_t validation_state_limit = 4;
        std::deque<validated_state> validation_states_;
        // A reclaim batch keeps the owner IN list near 16 KiB and its shard metadata
        // in a few hundred rows while amortizing statement preparation over many
        // candidates. It only sizes a scan; reclamation order and predicates are
        // independent of it.
        static constexpr std::size_t reclaim_batch = 256;
        std::map<view_id, lifetime> pending_leases_;
        std::set<candidate_id> pending_accesses_;
        mutable hot_cache<semantic::candidate> candidates_;
        mutable hot_cache<symbol_id> identities_;
        mutable hot_cache<indexed_shard> objects_;
        mutable hot_cache<indexed_view> views_;
        mutable std::unordered_map<shard_id, std::weak_ptr<const indexed_shard>> live_objects_;
        mutable std::unordered_map<view_id, std::weak_ptr<const indexed_view>> live_views_;
        mutable std::vector<std::weak_ptr<const indexed_view>> retired_views_;
    };
} // namespace semantic
