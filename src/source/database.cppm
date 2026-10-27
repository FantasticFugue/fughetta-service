module;
#include <sqlite3.h>
module engine;
import std;
import content;
import codec;
import kernel;
import durability;
import archive;
import index;
import reflection;
import utility;

namespace nodes {
    namespace {
        struct database {
            sqlite3* handle = nullptr;
            std::map<std::string, std::vector<sqlite3_stmt*>, std::less<>> statements;
            struct pending_value {
                std::string packed;
                std::vector<std::string> links;
            };
            std::unordered_map<std::string, std::string> pending_nodes;
            std::unordered_map<std::string, pending_value> pending_values;
            nodes::commit_check* confirm = nullptr;
            std::uint64_t state_nodes_read = 0, state_values_read = 0, state_nodes_written = 0, state_values_written = 0, state_bytes_written = 0,
                          state_bytes_read = 0, state_bytes_decoded = 0, causal_blocks_read = 0;
            explicit database(const std::filesystem::path& file) {
                if (sqlite3_open_v2(file.string().c_str(), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
                    std::string why = handle ? sqlite3_errmsg(handle) : "allocation failed";
                    if (handle)
                        sqlite3_close(handle);
                    handle = nullptr;
                    fail(nodes::code::storage_failure, why);
                }
                sqlite3_busy_timeout(handle, 5000);
                if (sqlite3_db_config(handle, SQLITE_DBCONFIG_LOOKASIDE, nullptr, 2048, 256) != SQLITE_OK)
                    fail(nodes::code::storage_failure, "cannot configure database scratch memory");
                run("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
                run("CREATE TABLE IF NOT EXISTS objects(id TEXT PRIMARY KEY, data BLOB NOT NULL, kind TEXT NOT NULL);"
                    "CREATE INDEX IF NOT EXISTS objects_kind ON objects(kind);"
                    "CREATE TABLE IF NOT EXISTS metadata(name TEXT PRIMARY KEY, data BLOB NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS roots(name TEXT PRIMARY KEY, data BLOB NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS revisions(id TEXT PRIMARY KEY, data BLOB NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS semantics(seq INTEGER PRIMARY KEY AUTOINCREMENT, environment TEXT NOT NULL, service TEXT NOT NULL, "
                    "version TEXT NOT NULL, frontier BLOB NOT NULL, data TEXT NOT NULL, UNIQUE(environment, service, version));"
                    "CREATE INDEX IF NOT EXISTS semantics_lookup ON semantics(environment, service, seq);"
                    "CREATE TABLE IF NOT EXISTS locks(id INTEGER PRIMARY KEY AUTOINCREMENT, line TEXT NOT NULL, holder TEXT NOT NULL, scope TEXT NOT NULL, "
                    "symbol TEXT NOT NULL, environment TEXT NOT NULL, document TEXT NOT NULL, first_run TEXT NOT NULL, first_offset INTEGER NOT NULL, "
                    "last_run TEXT NOT NULL, last_offset INTEGER NOT NULL, acquired INTEGER NOT NULL, UNIQUE(line, holder, scope, environment));"
                    "CREATE TABLE IF NOT EXISTS bindings(author TEXT NOT NULL, operation TEXT NOT NULL, digest TEXT NOT NULL, receipt TEXT NOT NULL, PRIMARY "
                    "KEY(author,operation));"
                    "CREATE TABLE IF NOT EXISTS staging(id TEXT PRIMARY KEY, total INTEGER NOT NULL, data BLOB NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS rejected(id TEXT PRIMARY KEY, reason TEXT NOT NULL, data BLOB NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS state_nodes(id TEXT PRIMARY KEY,data BLOB NOT NULL,refs INTEGER NOT NULL DEFAULT 0);"
                    "CREATE TABLE IF NOT EXISTS state_values(id TEXT PRIMARY KEY,data BLOB NOT NULL,refs INTEGER NOT NULL DEFAULT 0);"
                    "CREATE TABLE IF NOT EXISTS state_links(value_id TEXT NOT NULL,root_id TEXT NOT NULL,PRIMARY KEY(value_id,root_id));"
                    "CREATE TABLE IF NOT EXISTS state_heads(name TEXT PRIMARY KEY,id TEXT NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS state_payloads(id TEXT PRIMARY KEY,data BLOB NOT NULL,refs INTEGER NOT NULL DEFAULT 0);");
            }
            ~database() {
                for (auto& [_, idle] : statements)
                    for (auto* query : idle)
                        sqlite3_finalize(query);
                if (handle)
                    sqlite3_close_v2(handle);
            }
            void run(std::string_view sql) {
                char* message = nullptr;
                if (sqlite3_exec(handle, std::string(sql).c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
                    std::string why = message ? message : sqlite3_errmsg(handle);
                    sqlite3_free(message);
                    fail(nodes::code::storage_failure, why);
                }
            }
        };
        struct statement {
            database& owner;
            sqlite3* db;
            std::vector<sqlite3_stmt*>* idle;
            sqlite3_stmt* handle = nullptr;
            statement(database& d, std::string_view query) : owner(d), db(d.handle) {
                auto found = d.statements.find(query);
                if (found == d.statements.end())
                    found = d.statements.try_emplace(std::string{query}).first;
                idle = &found->second;
                if (!idle->empty()) {
                    handle = idle->back();
                    idle->pop_back();
                } else if (sqlite3_prepare_v3(db, query.data(), static_cast<int>(query.size()), SQLITE_PREPARE_PERSISTENT, &handle, nullptr) != SQLITE_OK)
                    fail(nodes::code::storage_failure, sqlite3_errmsg(db));
            }
            ~statement() {
                reset();
                idle->push_back(handle);
            }
            void reset() {
                sqlite3_reset(handle);
                sqlite3_clear_bindings(handle);
            }
            void bind(int n, std::string_view data) {
                if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
                    sqlite3_bind_blob(handle, n, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT) != SQLITE_OK)
                    fail(nodes::code::storage_failure, "cannot bind object bytes");
            }
            void text(int n, std::string_view data) {
                if (sqlite3_bind_text(handle, n, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT) != SQLITE_OK)
                    fail(nodes::code::storage_failure, "cannot bind object key");
            }
            void borrow_blob(int n, std::string_view data) {
                if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
                    sqlite3_bind_blob(handle, n, data.data(), static_cast<int>(data.size()), SQLITE_STATIC) != SQLITE_OK)
                    fail(nodes::code::storage_failure, "cannot bind tree bytes");
            }
            void borrow_text(int n, std::string_view data) {
                if (sqlite3_bind_text(handle, n, data.data(), static_cast<int>(data.size()), SQLITE_STATIC) != SQLITE_OK)
                    fail(nodes::code::storage_failure, "cannot bind tree key");
            }
            void number(int n, std::uint64_t v) {
                if (v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || sqlite3_bind_int64(handle, n, v) != SQLITE_OK)
                    fail(nodes::code::resource_limit, "invalid storage integer");
            }
            auto next() -> bool {
                int code = sqlite3_step(handle);
                if (code == SQLITE_ROW)
                    return true;
                if (code == SQLITE_DONE)
                    return false;
                fail(nodes::code::storage_failure, sqlite3_errmsg(db));
            }
            auto data(int n) -> std::string {
                auto p = static_cast<const char*>(sqlite3_column_blob(handle, n));
                return p ? std::string(p, sqlite3_column_bytes(handle, n)) : std::string{};
            }
            auto number(int n) -> std::uint64_t {
                auto v = sqlite3_column_int64(handle, n);
                if (v < 0)
                    fail(nodes::code::storage_failure, "negative stored length");
                return static_cast<std::uint64_t>(v);
            }
        };
        struct transaction {
            database& db;
            bool committed = false;
            explicit transaction(database& d) : db(d) { db.run("BEGIN IMMEDIATE"); }
            ~transaction() {
                if (!committed) {
                    sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr);
                    db.pending_nodes.clear();
                    db.pending_values.clear();
                }
            }
            // A caller that waited for the write lock commits only if the confirmation still passes.
            void commit() {
                if (db.confirm)
                    try {
                        db.confirm->check();
                    } catch (const error&) {
                        throw;
                    } catch (const std::exception& refused) {
                        fail(nodes::code::permission_denied, refused.what());
                    }
                db.run("COMMIT");
                committed = true;
            }
        };
        struct read_snapshot {
            database& db;
            bool owned, committed = false;
            explicit read_snapshot(database& d) : db(d), owned(sqlite3_get_autocommit(d.handle) != 0) {
                if (owned)
                    db.run("BEGIN");
            }
            ~read_snapshot() {
                if (owned && !committed)
                    sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr);
            }
            void commit() {
                if (owned)
                    db.run("COMMIT");
                committed = true;
            }
        };
        class state_tree {
            struct node {
                std::string key, value, left, right;
                content::sha256::digest priority;
                std::uint64_t count = 1;
            };
            database& db_;
            bool owner_ = false;
            mutable std::string root_;
            std::string saved_;
            mutable std::map<std::string, std::optional<std::string>, std::less<>> updates_;
            std::unordered_map<std::string, std::int64_t> node_refs_, value_refs_;
            mutable std::unordered_map<std::string, std::shared_ptr<const node>> cache_;
            mutable std::deque<std::string> lru_;
            static auto priority_of(std::string_view key) -> content::sha256::digest {
                content::sha256 digest;
                digest.update(key);
                return digest.finish();
            }
            auto load(const std::string& id) const -> std::shared_ptr<const node> {
                if (auto found = cache_.find(id); found != cache_.end())
                    return found->second;
                std::string bytes;
                if (auto found = db_.pending_nodes.find(id); found != db_.pending_nodes.end())
                    bytes = found->second;
                else {
                    statement q{db_, "SELECT data FROM state_nodes WHERE id=?1"};
                    q.borrow_text(1, id);
                    if (!q.next())
                        fail(nodes::code::missing_objects, "state index branch is absent");
                    ++db_.state_nodes_read;
                    bytes = q.data(0);
                    db_.state_bytes_read += bytes.size();
                }
                db_.state_bytes_decoded += bytes.size();
                if (digest_of(bytes) != id || !bytes.starts_with("DCI1"))
                    fail(nodes::code::storage_failure, "state index branch hash differs");
                std::size_t offset = 4;
                auto integer = [&]() {
                    if (bytes.size() - offset < 8)
                        fail(nodes::code::invalid_record, "state index integer");
                    std::uint64_t n = 0;
                    for (int i = 7; i >= 0; --i)
                        n = (n << 8) | static_cast<unsigned char>(bytes[offset + i]);
                    offset += 8;
                    return n;
                };
                auto text = [&]() {
                    auto size = integer();
                    if (size > bytes.size() - offset)
                        fail(nodes::code::invalid_record, "state index text");
                    auto out = bytes.substr(offset, size);
                    offset += size;
                    return out;
                };
                auto out = std::make_shared<node>();
                out->key = text();
                out->value = text();
                out->left = text();
                out->right = text();
                out->count = integer();
                out->priority = priority_of(out->key);
                if (offset != bytes.size() || !out->count || !content::is_hash(out->value) || (!out->left.empty() && !content::is_hash(out->left)) ||
                    (!out->right.empty() && !content::is_hash(out->right)))
                    fail(nodes::code::invalid_record, "state index fields");
                cache_[id] = out;
                lru_.push_back(id);
                while ((cache_.size() > 4096 || lru_.size() > 8192) && !lru_.empty()) {
                    cache_.erase(lru_.front());
                    lru_.pop_front();
                }
                return out;
            }
            void reference(std::string_view table, const std::string& id, int delta) {
                if (!id.empty())
                    (table == "state_nodes" ? node_refs_ : value_refs_)[id] += delta;
            }
            auto make(const std::string& key, const std::string& value, const std::string& left, const std::string& right) const -> std::string {
                auto count = 1 + (left.empty() ? 0 : load(left)->count) + (right.empty() ? 0 : load(right)->count);
                std::string bytes{"DCI1"};
                bytes.reserve(44 + key.size() + value.size() + left.size() + right.size());
                for (auto* text : {&key, &value, &left, &right}) {
                    codec::integer(bytes, text->size());
                    bytes += *text;
                }
                codec::integer(bytes, count);
                auto id = digest_of(bytes);
                db_.pending_nodes.try_emplace(id, std::move(bytes));
                auto cached = std::make_shared<node>(node{key, value, left, right, priority_of(key), count});
                cache_[id] = std::move(cached);
                lru_.push_back(id);
                while ((cache_.size() > 4096 || lru_.size() > 8192) && !lru_.empty()) {
                    cache_.erase(lru_.front());
                    lru_.pop_front();
                }
                return id;
            }
            auto split(const std::string& id, std::string_view key) const -> std::pair<std::string, std::string> {
                if (id.empty())
                    return {};
                auto p = load(id);
                if (p->key < key) {
                    auto [a, b] = split(p->right, key);
                    return {make(p->key, p->value, p->left, a), std::move(b)};
                }
                auto [a, b] = split(p->left, key);
                return {std::move(a), make(p->key, p->value, b, p->right)};
            }
            auto insert(const std::string& id, const std::string& key, const std::string& value, const content::sha256::digest& priority) const -> std::string {
                if (id.empty())
                    return make(key, value, {}, {});
                auto p = load(id);
                if (key == p->key)
                    return value == p->value ? id : make(key, value, p->left, p->right);
                if (std::tie(priority, key) < std::tie(p->priority, p->key)) {
                    auto [a, b] = split(id, key);
                    return make(key, value, a, b);
                }
                if (key < p->key)
                    return make(p->key, p->value, insert(p->left, key, value, priority), p->right);
                return make(p->key, p->value, p->left, insert(p->right, key, value, priority));
            }
            auto merge(const std::string& left, const std::string& right) const -> std::string {
                if (left.empty())
                    return right;
                if (right.empty())
                    return left;
                auto a = load(left), b = load(right);
                if (std::tie(a->priority, a->key) < std::tie(b->priority, b->key))
                    return make(a->key, a->value, a->left, merge(a->right, right));
                return make(b->key, b->value, merge(left, b->left), b->right);
            }
            auto remove(const std::string& id, std::string_view key) const -> std::string {
                if (id.empty())
                    return id;
                auto p = load(id);
                if (key == p->key)
                    return merge(p->left, p->right);
                if (key < p->key) {
                    auto child = remove(p->left, key);
                    return child == p->left ? id : make(p->key, p->value, child, p->right);
                }
                auto child = remove(p->right, key);
                return child == p->right ? id : make(p->key, p->value, p->left, child);
            }
            void flush_updates() const {
                if (updates_.empty())
                    return;
                struct entry {
                    const std::string* key;
                    const std::optional<std::string>* value;
                    content::sha256::digest priority;
                };
                std::vector<entry> entries;
                entries.reserve(updates_.size());
                for (auto& [key, id] : updates_)
                    entries.push_back({&key, &id, priority_of(key)});
                using bound = std::optional<std::string_view>;
                auto clip = [&](this const auto& clip, std::string id, bound lower, bound upper) -> std::string {
                    if (id.empty() || (!lower && !upper))
                        return id;
                    auto p = load(id);
                    if (lower && p->key <= *lower)
                        return clip(p->right, lower, upper);
                    if (upper && p->key >= *upper)
                        return clip(p->left, lower, upper);
                    auto left = lower ? clip(p->left, lower, {}) : p->left;
                    auto right = upper ? clip(p->right, {}, upper) : p->right;
                    return left == p->left && right == p->right ? id : make(p->key, p->value, left, right);
                };
                auto rewrite = [&](this const auto& rewrite, std::string id, std::size_t first, std::size_t last, bound lower, bound upper) -> std::string {
                    if (first == last)
                        return clip(std::move(id), lower, upper);
                    std::shared_ptr<const node> old;
                    while (!id.empty()) {
                        old = load(id);
                        if (lower && old->key <= *lower)
                            id = old->right;
                        else if (upper && old->key >= *upper)
                            id = old->left;
                        else
                            break;
                    }
                    if (id.empty())
                        old.reset();
                    auto middle = last;
                    for (auto index = first; index < last; ++index)
                        if (*entries[index].value && (middle == last || std::tie(entries[index].priority, *entries[index].key) <
                                                                            std::tie(entries[middle].priority, *entries[middle].key)))
                            middle = index;
                    if (middle != last && (!old || std::tie(entries[middle].priority, *entries[middle].key) < std::tie(old->priority, old->key))) {
                        auto& change = entries[middle];
                        auto left = rewrite(id, first, middle, lower, *change.key);
                        auto right = rewrite(id, middle + 1, last, *change.key, upper);
                        return make(*change.key, **change.value, left, right);
                    }
                    if (!old)
                        return {};
                    auto split = std::lower_bound(entries.begin() + first, entries.begin() + last, old->key,
                                                  [](const entry& item, const std::string& key) { return *item.key < key; }) -
                                 entries.begin();
                    bool replaced = split < last && *entries[split].key == old->key;
                    auto right_first = split + (replaced ? 1 : 0);
                    auto left = rewrite(old->left, first, split, lower, {});
                    auto right = rewrite(old->right, right_first, last, {}, upper);
                    if (replaced && !*entries[split].value)
                        return merge(left, right);
                    auto& value = replaced ? **entries[split].value : old->value;
                    return value == old->value && left == old->left && right == old->right ? id : make(old->key, value, left, right);
                };
                root_ = rewrite(root_, 0, entries.size(), {}, {});
                updates_.clear();
            }
            auto value(const std::string& id) const -> std::string {
                std::string packed;
                if (auto found = db_.pending_values.find(id); found != db_.pending_values.end())
                    packed = found->second.packed;
                else {
                    statement q{db_, "SELECT data FROM state_values WHERE id=?1"};
                    q.borrow_text(1, id);
                    if (!q.next())
                        fail(nodes::code::missing_objects, "state index value is absent");
                    ++db_.state_values_read;
                    packed = q.data(0);
                    db_.state_bytes_read += packed.size();
                }
                std::string out;
                try {
                    out = archive::unpack(packed);
                } catch (const error& e) {
                    fail(nodes::code::storage_failure, "stored index payload is corrupt: " + std::string(e.what()));
                }
                db_.state_bytes_decoded += out.size();
                if (digest_of(out) != id)
                    fail(nodes::code::storage_failure, "state index value hash differs");
                return out;
            }
            template <class F> void visit(const std::string& id, std::string_view first, std::string_view last, F& f) const {
                if (id.empty())
                    return;
                auto p = load(id);
                if (p->key >= first)
                    visit(p->left, first, last, f);
                if (p->key >= first && p->key < last)
                    f(p->key, value(p->value));
                if (p->key < last)
                    visit(p->right, first, last, f);
            }

        public:
            explicit state_tree(database& db, bool owner = false) : db_(db), owner_(owner) {}
            void adopt(std::string root) {
                if (owner_) {
                    db_.pending_nodes.clear();
                    db_.pending_values.clear();
                }
                updates_.clear();
                root_ = std::move(root);
                saved_ = root_;
                if (owner_) {
                    statement q{db_, "SELECT id FROM state_heads WHERE name='current'"};
                    saved_ = q.next() ? q.data(0) : std::string{};
                }
                cache_.clear();
                lru_.clear();
                if (!root_.empty())
                    (void)load(root_);
            }
            auto root() const -> const std::string& {
                flush_updates();
                return root_;
            }
            void set(const std::string& key, std::string_view bytes, const std::vector<std::string>& links = {}) {
                auto id = digest_of(bytes);
                if (!db_.pending_values.contains(id))
                    db_.pending_values.emplace(id, database::pending_value{archive::pack(bytes), links});
                updates_[key] = std::move(id);
            }
            void erase(std::string_view key) { updates_[std::string{key}] = std::nullopt; }
            auto get(std::string_view key) const -> std::optional<std::string> {
                if (auto queued = updates_.find(key); queued != updates_.end()) {
                    if (queued->second && key.starts_with("k/"))
                        ++db_.causal_blocks_read;
                    return queued->second ? std::optional{value(*queued->second)} : std::nullopt;
                }
                auto id = root_;
                while (!id.empty()) {
                    auto p = load(id);
                    if (p->key == key) {
                        if (key.starts_with("k/"))
                            ++db_.causal_blocks_read;
                        return value(p->value);
                    }
                    id = key < p->key ? p->left : p->right;
                }
                return {};
            }
            auto contains_range(std::string_view first, std::string_view last) const -> bool {
                flush_updates();
                auto id = root_;
                std::optional<std::string> candidate;
                while (!id.empty()) {
                    auto p = load(id);
                    if (p->key >= first) {
                        candidate = p->key;
                        id = p->left;
                    } else
                        id = p->right;
                }
                return candidate && *candidate < last;
            }
            template <class F> void each(std::string prefix, F f) const {
                flush_updates();
                auto last = prefix;
                if (last.empty())
                    fail(nodes::code::invalid_record, "state index range prefix");
                ++last.back();
                visit(root_, prefix, last, f);
            }
            void finish() {
                flush_updates();
                node_refs_.clear();
                value_refs_.clear();
                std::unordered_set<std::string> written_nodes, written_values;
                // Writes a pending node, or a pending value when `held` is set, after everything it reaches.
                auto write = [&](this const auto& write, const std::string& id, bool held) -> void {
                    if (held) {
                        auto found = db_.pending_values.find(id);
                        if (found == db_.pending_values.end() || !written_values.insert(id).second)
                            return;
                        auto& v = found->second;
                        for (auto& root : v.links)
                            write(root, false);
                        statement q{db_, "INSERT OR IGNORE INTO state_values(id,data) VALUES(?1,?2)"};
                        q.borrow_text(1, id);
                        q.borrow_blob(2, v.packed);
                        q.next();
                        if (sqlite3_changes(db_.handle)) {
                            ++db_.state_values_written;
                            db_.state_bytes_written += v.packed.size();
                            for (auto& root : v.links)
                                if (!root.empty()) {
                                    statement link{db_, "INSERT OR IGNORE INTO state_links(value_id,root_id) VALUES(?1,?2)"};
                                    link.borrow_text(1, id);
                                    link.borrow_text(2, root);
                                    link.next();
                                    if (sqlite3_changes(db_.handle))
                                        reference("state_nodes", root, 1);
                                }
                        }
                        return;
                    }
                    auto found = db_.pending_nodes.find(id);
                    if (found == db_.pending_nodes.end() || !written_nodes.insert(id).second)
                        return;
                    auto p = load(id);
                    write(p->left, false);
                    write(p->right, false);
                    write(p->value, true);
                    statement q{db_, "INSERT OR IGNORE INTO state_nodes(id,data) VALUES(?1,?2)"};
                    q.borrow_text(1, id);
                    q.borrow_blob(2, found->second);
                    q.next();
                    if (sqlite3_changes(db_.handle)) {
                        ++db_.state_nodes_written;
                        db_.state_bytes_written += found->second.size();
                        reference("state_nodes", p->left, 1);
                        reference("state_nodes", p->right, 1);
                        reference("state_values", p->value, 1);
                    }
                };
                write(root_, false);
                if (root_ != saved_) {
                    reference("state_nodes", root_, 1);
                    reference("state_nodes", saved_, -1);
                }
                {
                    statement q{db_, "INSERT INTO state_heads(name,id) VALUES('current',?1) ON CONFLICT(name) DO UPDATE SET id=excluded.id"};
                    q.borrow_text(1, root_);
                    q.next();
                }
                std::deque<std::string> node_queue, value_queue;
                std::unordered_set<std::string> deleted_nodes, deleted_values;
                for (auto& [id, delta] : node_refs_)
                    if (delta < 0)
                        node_queue.push_back(id);
                for (auto& [id, delta] : value_refs_)
                    if (delta < 0)
                        value_queue.push_back(id);
                auto unreferenced = [&](std::string_view table, const std::string& id, auto& deltas) {
                    auto found = deltas.find(id);
                    if (found == deltas.end() || found->second >= 0)
                        return false;
                    statement q{db_, table == "state_nodes" ? "SELECT refs FROM state_nodes WHERE id=?1" : "SELECT refs FROM state_values WHERE id=?1"};
                    q.borrow_text(1, id);
                    if (!q.next())
                        fail(nodes::code::storage_failure, "state reference target is absent");
                    auto refs = static_cast<std::int64_t>(q.number(0)) + found->second;
                    if (refs < 0)
                        fail(nodes::code::storage_failure, "state reference count is negative");
                    return refs == 0;
                };
                while (!node_queue.empty() || !value_queue.empty()) {
                    while (!node_queue.empty()) {
                        auto id = std::move(node_queue.front());
                        node_queue.pop_front();
                        if (deleted_nodes.contains(id) || !unreferenced("state_nodes", id, node_refs_))
                            continue;
                        auto p = load(id);
                        statement remove{db_, "DELETE FROM state_nodes WHERE id=?1"};
                        remove.borrow_text(1, id);
                        remove.next();
                        node_refs_.erase(id);
                        deleted_nodes.insert(id);
                        reference("state_values", p->value, -1);
                        value_queue.push_back(p->value);
                        for (auto* child : {&p->left, &p->right})
                            if (!child->empty()) {
                                reference("state_nodes", *child, -1);
                                node_queue.push_back(*child);
                            }
                        cache_.erase(id);
                    }
                    while (!value_queue.empty()) {
                        auto id = std::move(value_queue.front());
                        value_queue.pop_front();
                        if (deleted_values.contains(id) || !unreferenced("state_values", id, value_refs_))
                            continue;
                        statement links{db_, "SELECT root_id FROM state_links WHERE value_id=?1"};
                        links.borrow_text(1, id);
                        while (links.next()) {
                            auto root = links.data(0);
                            reference("state_nodes", root, -1);
                            node_queue.push_back(std::move(root));
                        }
                        statement remove_links{db_, "DELETE FROM state_links WHERE value_id=?1"};
                        remove_links.borrow_text(1, id);
                        remove_links.next();
                        statement remove{db_, "DELETE FROM state_values WHERE id=?1"};
                        remove.borrow_text(1, id);
                        remove.next();
                        value_refs_.erase(id);
                        deleted_values.insert(id);
                    }
                }
                auto flush_refs = [&](std::string_view table, const auto& deltas) {
                    statement q{db_, table == "state_nodes" ? "UPDATE state_nodes SET refs=refs+?2 WHERE id=?1"
                                                            : "UPDATE state_values SET refs=refs+?2 WHERE id=?1"};
                    for (auto& [id, delta] : deltas)
                        if (delta) {
                            q.reset();
                            q.borrow_text(1, id);
                            if (sqlite3_bind_int64(q.handle, 2, delta) != SQLITE_OK)
                                fail(nodes::code::storage_failure, "state reference binding");
                            q.next();
                        }
                };
                flush_refs("state_nodes", node_refs_);
                flush_refs("state_values", value_refs_);
                node_refs_.clear();
                value_refs_.clear();
                saved_ = root_;
                db_.pending_nodes.clear();
                db_.pending_values.clear();
                while ((cache_.size() > 4096 || lru_.size() > 8192) && !lru_.empty()) {
                    cache_.erase(lru_.front());
                    lru_.pop_front();
                }
            }
        };
        auto vector_set(std::vector<std::string> ids) -> std::vector<std::string> { return canonical_set(std::move(ids)); }
        auto base_body(std::string kind, std::string project) -> value::map {
            return {{"kind", std::move(kind)}, {"project", std::move(project)}, {"schema", std::uint64_t{1}}};
        }
        auto effects_digest(const value::array& effects) -> std::string {
            if (effects.size() > codec::collection_limit)
                fail(nodes::code::resource_limit, "record collection limit");
            std::string encoded{"DCN1"};
            encoded += '\7';
            codec::integer(encoded, 1);
            codec::text(encoded, "effects");
            encoded += '\6';
            codec::integer(encoded, effects.size());
            for (auto& effect : effects)
                codec::encode_value(encoded, effect, 2);
            if (encoded.size() > codec::record_limit)
                fail(nodes::code::resource_limit, "record byte limit");
            return digest_of(encoded);
        }
        auto request_body(const node_request& in, const std::string& project) -> value {
            value::array edits;
            for (auto& e : in.edits)
                edits.emplace_back(value::map{{"count", e.count},
                                              {"document", e.document},
                                              {"executable", e.executable},
                                              {"kind", e.kind},
                                              {"offset", e.offset},
                                              {"path", e.path},
                                              {"target", e.target},
                                              {"text", e.text}});
            return value::map{{"action", in.action},
                              {"activation", in.activation ? value{*in.activation} : value{}},
                              {"actor", in.actor},
                              {"caller_operation", in.caller_operation},
                              {"configuration", in.configuration},
                              {"decisions", strings(in.decisions)},
                              {"dependencies", strings(in.dependencies)},
                              {"edits", std::move(edits)},
                              {"expected", strings(in.expected)},
                              {"intent", digest_of(in.intent)},
                              {"operation", in.operation},
                              {"owner", in.owner},
                              {"project", project},
                              {"result", digest_of(in.result)},
                              {"source", strings(in.source)},
                              {"target", in.target},
                              {"tool", in.tool},
                              {"worldline", in.worldline}};
        }
        auto decode_request(const value& input, std::string intent, std::string result) -> node_request {
            fields(input, {"action", "activation", "actor", "caller_operation", "configuration", "decisions", "dependencies", "edits", "expected", "intent",
                           "operation", "owner", "project", "result", "source", "target", "tool", "worldline"});
            node_request out;
            out.action = input.at("action").text();
            out.actor = input.at("actor").text();
            out.caller_operation = input.at("caller_operation").text();
            out.configuration = input.at("configuration").text();
            out.decisions = string_list(input.at("decisions"));
            out.dependencies = string_list(input.at("dependencies"));
            if (input.at("intent").text() != digest_of(intent) || input.at("result").text() != digest_of(result))
                fail(nodes::code::invalid_record, "request attachments do not match their input fingerprints");
            out.expected = string_list(input.at("expected"));
            out.intent = std::move(intent);
            out.operation = input.at("operation").text();
            out.result = std::move(result);
            out.source = string_list(input.at("source"));
            out.target = input.at("target").text();
            out.worldline = input.at("worldline").text();
            out.owner = input.at("owner").text();
            out.tool = input.at("tool").text();
            if (!std::holds_alternative<std::monostate>(input.at("activation").data))
                out.activation = input.at("activation").boolean();
            for (auto& e : input.at("edits").list()) {
                fields(e, {"count", "document", "executable", "kind", "offset", "path", "target", "text"});
                out.edits.push_back({e.at("kind").text(), e.at("path").text(), e.at("text").text(), e.at("target").text(), e.at("offset").number(),
                                     e.at("count").number(), e.at("executable").boolean(), e.at("document").text()});
            }
            return out;
        }
        auto tool_name(std::string_view declared, std::string_view action) -> std::string {
            auto name = declared.empty() ? "source." + std::string(action) + "/1" : std::string(declared);
            auto separator = name.find('/');
            if (name.size() > 128 || separator == std::string::npos || separator == 0 || separator + 1 == name.size() ||
                name.find('/', separator + 1) != std::string::npos || std::ranges::any_of(name, [](unsigned char c) {
                    return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/');
                }))
                fail(nodes::code::invalid_record, "tool identity must contain a name and version");
            return name;
        }
    } // namespace

    struct local_node::impl {
        struct body_view {
            const value* body;
            operator const value&() const { return *body; }
            auto at(std::string_view name) const -> const value& { return body->at(name); }
            auto object() const -> const value::map& { return body->object(); }
        };
        std::filesystem::path root;
        archive::files io_files;
        key author, node;
        std::unique_ptr<database> db;
        std::string project;
        bool read_only = false, archive_audited = false;
        std::string observed_snapshot;
        mutable std::map<std::string, value> objects;
        mutable std::map<std::string, body_view> bodies;
        std::map<std::string, value> decoded_inputs;
        mutable std::deque<std::string> object_order;
        mutable std::unordered_map<std::string, std::size_t> object_sizes;
        mutable std::size_t cached_object_bytes = 0;
        std::map<std::string, std::vector<std::string>> receipt_index;
        mutable std::set<std::string> validated;
        std::map<std::string, std::pair<std::string, std::string>> refused;
        std::map<std::string, std::set<std::string>> operation_claims;
        std::map<std::string, std::string> identity_conflicts;
        std::vector<std::string> policies;
        kernel source;
        std::map<std::string, std::vector<std::string>> roots;
        std::map<std::string, std::string> root_admissions;
        // Worldline names bound by validated acceptances, and the lines with accepted source, built on first use after each reload.
        struct line_index {
            std::map<std::string, std::set<std::string>> names;
            std::set<std::string> active;
        };
        mutable std::optional<line_index> line_names;
        bool dirty = false;
        std::uint64_t data_version = 0;
        std::uint64_t snapshot_watermark = 0, snapshot_events = 0, replayed_events = 0;
        mutable std::uint64_t loaded_records = 0;
        std::string state_snapshot;
        std::unique_ptr<state_tree> pages;
        bool snapshot_ready = false, kernel_loaded = true, status_loaded = true, pending_loaded = true, kernel_aliases = false;
        std::uint64_t causal_headers_read = 0, causal_effects_read = 0, file_histories_read = 0, file_payloads_read = 0, projected_before_eviction = 0,
                      sequence_pages_read = 0, sequence_pages_written = 0, insertion_payloads_read = 0;
        std::set<std::string> cached_payload_files;
        std::uint64_t cached_payload_bytes = 0, cache_evictions = 0;
        std::deque<std::pair<std::string, std::string>> projection_garbage;
        // A projection store; `Codec` names its pages and gives `item`, `encode`, `decode` and `hash` for what it holds.
        template <class Codec> struct projection_index : persistent_index<typename Codec::item>::backend {
            using T = typename Codec::item;
            static constexpr std::string_view name = Codec::name;
            impl& self;
            state_tree& pages;
            std::uint64_t* generated = nullptr;
            projection_index(impl& self, state_tree& pages) : self(self), pages(pages) {}
            using data = persistent_index<T>::node_data;
            auto digest(const T& item) const -> std::string override { return Codec::hash(item); }
            auto load(std::string_view id) -> data override {
                auto bytes = pages.get("j/" + std::string(name) + "/" + std::string(id));
                if (!bytes)
                    fail(nodes::code::missing_objects, "required projection branch is absent");
                if (digest_of(*bytes) != id)
                    fail(nodes::code::storage_failure, "projection object identity differs");
                auto v = codec::decode(*bytes);
                fields(v, {"count", "item", "key", "left", "left_count", "left_mass", "left_object", "mass", "right", "right_count", "right_mass",
                           "right_object", "value"});
                return {.key = v.at("key").text(),
                        .item = Codec::decode(self, v.at("item")),
                        .value_digest = v.at("value").text(),
                        .left = v.at("left").text(),
                        .right = v.at("right").text(),
                        .left_object = v.at("left_object").text(),
                        .right_object = v.at("right_object").text(),
                        .count = v.at("count").number(),
                        .left_count = v.at("left_count").number(),
                        .right_count = v.at("right_count").number(),
                        .mass = v.at("mass").number(),
                        .left_mass = v.at("left_mass").number(),
                        .right_mass = v.at("right_mass").number(),
                        .bound = true};
            }
            auto store(const data& node) -> std::string override {
                if (!node.bound || node.value_digest != Codec::hash(node.item))
                    fail(nodes::code::invalid_record, "projection value digest differs");
                auto item = Codec::encode(self, node.item);
                auto bytes = codec::encode(value::map{{"key", node.key},
                                                      {"item", item},
                                                      {"value", node.value_digest},
                                                      {"left", node.left},
                                                      {"right", node.right},
                                                      {"left_object", node.left_object},
                                                      {"right_object", node.right_object},
                                                      {"count", static_cast<std::uint64_t>(node.count)},
                                                      {"left_count", static_cast<std::uint64_t>(node.left_count)},
                                                      {"right_count", static_cast<std::uint64_t>(node.right_count)},
                                                      {"mass", node.mass},
                                                      {"left_mass", node.left_mass},
                                                      {"right_mass", node.right_mass}});
                auto id = digest_of(bytes), key = "j/" + std::string(name) + "/" + id;
                if (auto previous = pages.get(key)) {
                    if (*previous != bytes)
                        fail(nodes::code::identity_conflict, "projection object has differing content");
                    return id;
                }
                pages.set(key, bytes);
                if (generated)
                    ++*generated;
                pages.set("jrefs/" + std::string(name) + "/" + id, codec::encode(value::map{{"refs", std::uint64_t{0}}}));
                self.projection_reference(std::string(name), node.left_object, 1);
                self.projection_reference(std::string(name), node.right_object, 1);
                if constexpr (name == "files" || name == "hidden") {
                    self.payload_reference(item.at("data").text(), 1);
                    auto& sequence = item.at("sequence");
                    self.projection_reference("sequence", sequence.at("pieces").at("object").text(), 1);
                    self.projection_reference("anchors", sequence.at("anchors").at("object").text(), 1);
                }
                return id;
            }
        };
        struct anchor_span {
            std::string key;
            std::uint64_t first = 0, count = 0;
            auto operator==(const anchor_span&) const -> bool = default;
        };
        static auto fragment_value(const fragment& f) -> value { return value::map{{"count", f.count}, {"first", f.first}, {"run", f.run}}; }
        static auto span_value(const anchor_span& f) -> value { return value::map{{"count", f.count}, {"first", f.first}, {"key", f.key}}; }
        struct sequence_pages {
            using item = fragment;
            static constexpr std::string_view name = "sequence";
            static auto encode(impl&, const fragment& f) -> value { return fragment_value(f); }
            static auto decode(impl& self, const value& v) -> fragment {
                ++self.sequence_pages_read;
                fields(v, {"count", "first", "run"});
                auto count = v.at("count").number();
                if (!count || count > 1024)
                    fail(nodes::code::invalid_record, "sequence scalar page bound differs");
                return fragment{v.at("run").text(), v.at("first").number(), count};
            }
            static auto hash(const fragment& f) -> std::string { return digest_of(codec::encode(fragment_value(f))); }
        };
        struct anchor_pages {
            using item = anchor_span;
            static constexpr std::string_view name = "anchors";
            static auto encode(impl&, const anchor_span& f) -> value { return span_value(f); }
            static auto decode(impl& self, const value& v) -> anchor_span {
                ++self.sequence_pages_read;
                fields(v, {"count", "first", "key"});
                return anchor_span{v.at("key").text(), v.at("first").number(), v.at("count").number()};
            }
            static auto hash(const anchor_span& f) -> std::string { return digest_of(codec::encode(span_value(f))); }
        };
        std::shared_ptr<projection_index<sequence_pages>> sequence_index;
        std::shared_ptr<projection_index<anchor_pages>> anchor_index;
        struct sequence_view : sequence_state {
            persistent_index<fragment> pieces;
            persistent_index<anchor_span> anchors;
            std::shared_ptr<projection_index<sequence_pages>> piece_storage;
            std::shared_ptr<projection_index<anchor_pages>> anchor_storage;
            static auto anchor_key(const std::string& run, std::uint64_t first) -> std::string { return run + "/" + std::format("{:016x}", first); }
            static auto between(const std::string& left, const std::string& right) -> std::string {
                if (!right.empty() && left >= right)
                    fail(nodes::code::invalid_record, "sequence ordering interval differs");
                std::string out;
                bool bounded = !right.empty();
                for (std::size_t i = 0; i < 1024; ++i) {
                    unsigned lo = i < left.size() ? static_cast<unsigned char>(left[i]) : 32;
                    unsigned hi = bounded && i < right.size() ? static_cast<unsigned char>(right[i]) : 127;
                    if (hi > lo + 1) {
                        out.push_back(static_cast<char>((hi + lo) / 2));
                        return out;
                    }
                    out.push_back(static_cast<char>(lo));
                    if (lo < hi)
                        bounded = false;
                }
                fail(nodes::code::resource_limit, "sequence ordering precision exhausted");
            }
            auto size() const -> std::uint64_t override { return pieces.mass(); }
            auto at(std::uint64_t offset) const -> element override {
                auto [it, relative] = pieces.at_mass(offset);
                return {it->second.run, it->second.first + relative};
            }
            auto position(const element& selected) const -> std::optional<std::uint64_t> override {
                auto found = extent(selected);
                if (!found)
                    return {};
                return found->first;
            }
            auto extent(const element& selected) const -> std::optional<std::pair<std::uint64_t, std::uint64_t>> override {
                auto it = anchors.floor(anchor_key(selected.run, selected.offset));
                auto prefix = selected.run + "/";
                if (it == anchors.end() || !it->first.starts_with(prefix) || selected.offset < it->second.first ||
                    selected.offset - it->second.first >= it->second.count)
                    return {};
                auto piece = pieces.find(it->second.key);
                if (piece == pieces.end() || piece->second.run != selected.run || piece->second.first != it->second.first ||
                    piece->second.count != it->second.count)
                    fail(nodes::code::storage_failure, "sequence anchor and counted piece differ");
                auto inside = selected.offset - piece->second.first;
                return std::pair{pieces.mass_before(piece->first) + inside, piece->second.count - inside};
            }
            void insert(const std::string& key, fragment f) {
                if (!f.count || f.count > 1024)
                    fail(nodes::code::invalid_record, "sequence piece scalar bound differs");
                pieces.set(key, f, digest_of(codec::encode(fragment_value(f))));
                anchor_span span{key, f.first, f.count};
                anchors.set(anchor_key(f.run, f.first), span, digest_of(codec::encode(span_value(span))));
            }
            auto splice(std::uint64_t offset, std::uint64_t count, std::string run, std::uint64_t inserted) const
                -> std::shared_ptr<const sequence_state> override {
                if (offset > size() || count > size() - offset || (inserted && run.empty()))
                    fail(nodes::code::invalid_record, "sequence splice bounds differ");
                auto next = std::make_shared<sequence_view>(*this);
                std::vector<std::string> removed;
                std::vector<fragment> replacement;
                std::optional<fragment> suffix;
                std::string left, right;
                if (offset < size()) {
                    auto [it, first] = pieces.at_mass(offset);
                    auto before = pieces.floor(it->first, true);
                    if (before != pieces.end())
                        left = before->first;
                    std::uint64_t remaining = count;
                    if (first)
                        replacement.push_back({it->second.run, it->second.first, first});
                    for (;;) {
                        auto f = it->second;
                        removed.push_back(it->first);
                        auto taken = std::min(remaining, f.count - first);
                        if (first + taken < f.count)
                            suffix = fragment{f.run, f.first + first + taken, f.count - first - taken};
                        remaining -= taken;
                        ++it;
                        if (!remaining) {
                            if (it != pieces.end())
                                right = it->first;
                            break;
                        }
                        if (it == pieces.end())
                            fail(nodes::code::storage_failure, "sequence region is incomplete");
                        first = 0;
                    }
                } else {
                    auto before = pieces.floor(std::string(1, static_cast<char>(127)));
                    if (before != pieces.end())
                        left = before->first;
                }
                for (auto& key : removed) {
                    auto f = next->pieces.find(key)->second;
                    next->anchors.erase(anchor_key(f.run, f.first));
                    next->pieces.erase(key);
                }
                for (std::uint64_t first = 0; first < inserted;) {
                    auto length = std::min<std::uint64_t>(1024, inserted - first);
                    replacement.push_back({run, first, length});
                    first += length;
                }
                if (suffix)
                    replacement.push_back(*suffix);
                for (auto& f : replacement) {
                    auto key = between(left, right);
                    next->insert(key, f);
                    left = std::move(key);
                }
                if (next->size() != size() - count + inserted)
                    fail(nodes::code::storage_failure, "sequence splice scalar count differs");
                return next;
            }
            auto all() const -> std::vector<fragment> override {
                std::vector<fragment> out;
                for (auto& [_, f] : pieces)
                    out.push_back(f);
                return out;
            }
            auto span(std::uint64_t offset, std::uint64_t count) const -> std::vector<fragment> override {
                std::vector<fragment> out;
                if (!count)
                    return out;
                auto [it, first] = pieces.at_mass(offset);
                for (; count; ++it, first = 0) {
                    if (it == pieces.end())
                        fail(nodes::code::storage_failure, "sequence region is incomplete");
                    auto taken = std::min(count, it->second.count - first);
                    out.push_back({it->second.run, it->second.first + first, taken});
                    count -= taken;
                }
                return out;
            }
            auto save() const -> value override {
                pieces.persist(piece_storage);
                anchors.persist(anchor_storage);
                auto root = [](const auto& index) -> value {
                    return value::map{
                        {"root", index.digest()}, {"object", index.object()}, {"count", static_cast<std::uint64_t>(index.size())}, {"mass", index.mass()}};
                };
                return value::map{{"pieces", root(pieces)}, {"anchors", root(anchors)}};
            }
        };
        void projection_reference(const std::string& type, const std::string& id, int delta) {
            if (id.empty())
                return;
            auto key = "jrefs/" + type + "/" + id;
            auto bytes = pages->get(key);
            if (!bytes)
                fail(nodes::code::missing_objects, "required projection reference state is absent");
            auto count = codec::decode(*bytes).at("refs").number();
            if (delta < 0 && !count)
                fail(nodes::code::storage_failure, "projection reference count underflow");
            count = delta > 0 ? count + 1 : count - 1;
            pages->set(key, codec::encode(value::map{{"refs", count}}));
            if (!count)
                projection_garbage.emplace_back(type, id);
        }
        void payload_reference(const std::string& id, int delta) {
            statement q{*db, "SELECT refs FROM state_payloads WHERE id=?1"};
            q.text(1, id);
            if (!q.next())
                fail(nodes::code::missing_objects, "required payload reference state is absent");
            auto count = q.number(0);
            if (delta < 0 && !count)
                fail(nodes::code::storage_failure, "payload reference count underflow");
            count = delta > 0 ? count + 1 : count - 1;
            statement update{*db, count ? "UPDATE state_payloads SET refs=?2 WHERE id=?1" : "DELETE FROM state_payloads WHERE id=?1"};
            update.text(1, id);
            if (count)
                update.number(2, count);
            update.next();
        }
        void projection_roots(const std::string& previous, const std::string& current) {
            auto before = previous.empty() ? value::map{} : codec::decode(previous).object();
            auto after = codec::decode(current);
            for (auto name : {"files", "hidden", "paths", "file_conflicts", "path_conflicts", "control_conflicts", "identity_conflicts", "dependency_conflicts",
                              "activation", "resolutions"}) {
                std::string type = std::string(name).ends_with("conflicts")                                  ? "conflicts"
                                   : std::string(name) == "activation" || std::string(name) == "resolutions" ? "booleans"
                                                                                                             : name;
                auto prior = before.find(name);
                auto old = prior == before.end() ? std::string{} : prior->second.at("object").text();
                auto next = after.at(name).at("object").text();
                if (old != next) {
                    projection_reference(type, next, 1);
                    projection_reference(type, old, -1);
                }
            }
        }
        void collect_projection_garbage() {
            while (!projection_garbage.empty()) {
                auto [type, id] = std::move(projection_garbage.front());
                projection_garbage.pop_front();
                auto refs = pages->get("jrefs/" + type + "/" + id);
                if (!refs || codec::decode(*refs).at("refs").number())
                    continue;
                auto bytes = pages->get("j/" + type + "/" + id);
                if (!bytes || digest_of(*bytes) != id)
                    fail(nodes::code::storage_failure, "projection collection object differs");
                auto v = codec::decode(*bytes);
                projection_reference(type, v.at("left_object").text(), -1);
                projection_reference(type, v.at("right_object").text(), -1);
                if (type == "files" || type == "hidden") {
                    auto& item = v.at("item");
                    payload_reference(item.at("data").text(), -1);
                    projection_reference("sequence", item.at("sequence").at("pieces").at("object").text(), -1);
                    projection_reference("anchors", item.at("sequence").at("anchors").at("object").text(), -1);
                }
                pages->erase("j/" + type + "/" + id);
                pages->erase("jrefs/" + type + "/" + id);
            }
        }
        auto sequence_from(const value& roots) -> std::shared_ptr<sequence_view> {
            projection_stores();
            auto out = std::make_shared<sequence_view>();
            out->piece_storage = sequence_index;
            out->anchor_storage = anchor_index;
            fields(roots, {"anchors", "pieces"});
            auto load = [](auto& index, const value& v, auto storage) {
                fields(v, {"count", "mass", "object", "root"});
                index.adopt(v.at("root").text(), v.at("object").text(), v.at("count").number(), true, v.at("mass").number(), storage);
            };
            load(out->pieces, roots.at("pieces"), sequence_index);
            load(out->anchors, roots.at("anchors"), anchor_index);
            if (out->pieces.size() != out->anchors.size() || out->pieces.mass() != out->anchors.mass())
                fail(nodes::code::storage_failure, "sequence and anchor root counts differ");
            return out;
        }
        auto sequence_for(const source_file& file) -> std::shared_ptr<const sequence_state> {
            if (file.sequence)
                return file.sequence;
            projection_stores();
            auto out = std::make_shared<sequence_view>();
            out->piece_storage = sequence_index;
            out->anchor_storage = anchor_index;
            std::uint64_t index = 0;
            for (auto& f : file.fragments)
                for (std::uint64_t first = 0; first < f.count;) {
                    auto length = std::min<std::uint64_t>(1024, f.count - first);
                    out->insert(ordinal_key(++index), {f.run, f.first + first, length});
                    first += length;
                }
            file.sequence = out;
            return out;
        }
        auto retain_payload(std::string data) -> std::string {
            auto id = digest_of(data), packed = archive::pack(data);
            statement q{*db, "INSERT OR IGNORE INTO state_payloads(id,data) VALUES(?1,?2)"};
            q.text(1, id);
            q.bind(2, packed);
            q.next();
            if (sqlite3_changes(db->handle)) {
                ++db->state_values_written;
                db->state_bytes_written += packed.size();
            }
            return id;
        }
        auto read_payload(const std::string& id) -> std::string {
            statement q{*db, "SELECT data FROM state_payloads WHERE id=?1"};
            q.text(1, id);
            if (!q.next())
                fail(nodes::code::missing_objects, "required content payload is absent");
            auto packed = q.data(0);
            ++db->state_values_read;
            db->state_bytes_read += packed.size();
            std::string data;
            try {
                data = archive::unpack(packed);
            } catch (const error& e) {
                fail(nodes::code::storage_failure, "stored content payload is corrupt: " + std::string(e.what()));
            }
            db->state_bytes_decoded += data.size();
            if (digest_of(data) != id)
                fail(nodes::code::storage_failure, "content payload hash differs");
            return data;
        }
        using projected_file = std::shared_ptr<const source_file>;
        using path_owners = std::shared_ptr<const std::set<std::string>>;
        struct visible_files {
            using item = projected_file;
            static constexpr std::string_view name = "files";
            static auto encode(impl& self, const projected_file& f) -> value { return codec::decode(self.file_bytes(*f)); }
            static auto decode(impl& self, const value& v) -> projected_file { return self.file_header(codec::encode(v), v.at("file").text()); }
            static auto hash(const projected_file& f) -> std::string { return f->metadata_digest(); }
        };
        struct hidden_files : visible_files {
            static constexpr std::string_view name = "hidden";
            static auto decode(impl& self, const value& v) -> projected_file {
                auto hidden = std::make_shared<source_file>(*visible_files::decode(self, v));
                hidden->present = false;
                return hidden;
            }
        };
        struct path_pages {
            using item = path_owners;
            static constexpr std::string_view name = "paths";
            static auto encode(impl&, const path_owners& owners) -> value { return strings({owners->begin(), owners->end()}); }
            static auto decode(impl&, const value& v) -> path_owners {
                auto ids = string_list(v);
                return std::make_shared<const std::set<std::string>>(ids.begin(), ids.end());
            }
            static auto hash(const path_owners& owners) -> std::string {
                return digest_of(codec::encode(value::map{{"ids", strings({owners->begin(), owners->end()})}}));
            }
        };
        struct conflict_pages {
            using item = std::vector<std::string>;
            static constexpr std::string_view name = "conflicts";
            static auto encode(impl&, const item& ids) -> value { return strings(ids); }
            static auto decode(impl&, const value& v) -> item { return string_list(v); }
            static auto hash(const item& ids) -> std::string { return digest_of(codec::encode(value::map{{"ids", strings(ids)}})); }
        };
        struct boolean_pages {
            using item = bool;
            static constexpr std::string_view name = "booleans";
            static auto encode(impl&, bool b) -> value { return b; }
            static auto decode(impl&, const value& v) -> bool { return v.boolean(); }
            static auto hash(bool b) -> std::string { return digest_of(codec::encode(value::map{{"value", b}})); }
        };
        std::shared_ptr<projection_index<visible_files>> visible_index;
        std::shared_ptr<projection_index<hidden_files>> hidden_index;
        std::shared_ptr<projection_index<path_pages>> path_index;
        std::shared_ptr<projection_index<conflict_pages>> conflict_index;
        std::shared_ptr<projection_index<boolean_pages>> boolean_index;
        std::uint64_t object_count = 0, object_watermark = 0, archive_bytes = 0, kernel_block = 0;
        std::string kernel_tail;
        struct saved_view {
            std::vector<std::string> frontier, conflicts;
            std::string admission, version, digest, files, projection;
        };
        std::map<std::string, saved_view> views;
        mutable std::set<std::string> pending_objects;
        mutable std::deque<std::string> new_candidates;
        std::deque<std::string> completed_objects, refused_objects;
        std::map<std::string, std::size_t> remaining_dependencies;
        std::map<std::string, std::vector<std::string>> waiting_dependencies;
        mutable std::set<std::string> dirty_status;
        std::set<std::string> dirty_claims, dirty_receipts, dirty_conflicts, dirty_roots, dirty_cursors;
        std::map<std::string, causal_set> checkpoint_cursors;

        static auto checked_root(std::filesystem::path path) -> std::filesystem::path {
            path = std::filesystem::absolute(path).lexically_normal();
            auto current = path.root_path();
            for (auto part : path.relative_path()) {
                current /= part;
                auto status = std::filesystem::symlink_status(current);
                if (std::filesystem::is_symlink(status))
                    fail(nodes::code::storage_failure, "node path contains a symbolic link");
            }
            std::filesystem::create_directories(path);
            std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
            return path;
        }
        static auto load_key(const std::filesystem::path& file, archive::files& files) -> key {
            auto status = std::filesystem::symlink_status(file);
            if (std::filesystem::is_symlink(status))
                fail(nodes::code::storage_failure, "key file is a symbolic link");
            if (std::filesystem::exists(status)) {
                return key::from_secret(files.read(file));
            }
            auto out = key::generate();
            files.write(file, out.secret);
            std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace);
            return out;
        }
        impl(std::filesystem::path path, std::string trust, bool audit, bool reading)
            : root(checked_root(std::move(path))), author(load_key(root / "author.key", io_files)), node(load_key(root / "node.key", io_files)) {
            read_only = reading;
            for (auto name : {"repository.sqlite", "repository.sqlite-wal", "repository.sqlite-shm"})
                if (std::filesystem::is_symlink(std::filesystem::symlink_status(root / name)))
                    fail(nodes::code::storage_failure, "node database is a symbolic link");
            db = std::make_unique<database>(root / "repository.sqlite");
            pages = std::make_unique<state_tree>(*db, true);
            {
                statement q{*db, "SELECT data FROM metadata WHERE name='project'"};
                if (q.next()) {
                    project = q.data(0);
                    if (!trust.empty() && trust != project)
                        fail("wrong_project", "trusted project differs from local genesis");
                } else if (!trust.empty()) {
                    if (!content::is_hash(trust))
                        fail("wrong_project", "invalid trusted genesis digest");
                    project = trust;
                    metadata("project", project);
                }
            }
            reload(audit);
        }
        void metadata(std::string_view name, std::string_view data) {
            statement q{*db, "INSERT INTO metadata(name,data) VALUES(?1,?2) ON CONFLICT(name) DO UPDATE SET data=excluded.data"};
            q.text(1, name);
            q.bind(2, data);
            q.next();
            if (name == "state_snapshot") {
                observed_snapshot = digest_of(data);
                ++db->state_values_written;
                db->state_bytes_written += data.size();
            }
        }
        void trim_kernel() {
            if (!kernel_loaded) {
                cached_payload_files.clear();
                cached_payload_bytes = 0;
                return;
            }
            if (!dirty && snapshot_ready && kernel_loaded && source.size() == snapshot_events &&
                (source.loaded_files() > 8 || source.loaded_events() > 512 || cached_payload_files.size() > 8 || cached_payload_bytes > 8 * 1024 * 1024)) {
                ++cache_evictions;
                archive_audited = false;
                cached_payload_files.clear();
                cached_payload_bytes = 0;
                projected_before_eviction += source.projected_files();
                kernel_aliases = source.aliases();
                source = kernel{};
                kernel_tail.clear();
                kernel_loaded = snapshot_events == 0;
            }
        }
        auto is_validated(const std::string& id) const -> bool {
            if (validated.contains(id))
                return true;
            if (snapshot_ready) {
                auto saved = pages->get("m/o/" + id);
                if (saved && *saved == "V") {
                    validated.insert(id);
                    return true;
                }
            }
            return false;
        }
        void queue_object(const std::string& id) const {
            if (!is_validated(id) && !refused.contains(id) && pending_objects.insert(id).second) {
                new_candidates.push_back(id);
                dirty_status.insert(id);
            }
        }
        void mark_validated(const std::string& id) {
            validated.insert(id);
            pending_objects.erase(id);
            remaining_dependencies.erase(id);
            dirty_status.insert(id);
            completed_objects.push_back(id);
        }
        void ensure_status() {
            if (status_loaded)
                return;
            status_loaded = true;
            pages->each("m/x/", [&](const std::string& key, std::string bytes) {
                auto body = codec::decode(bytes);
                refused[key.substr(4)] = {body.at("code").text(), body.at("reason").text()};
            });
            pages->each("m/i/", [&](const std::string& key, std::string bytes) { identity_conflicts[key.substr(4)] = codec::decode(bytes).at("id").text(); });
        }
        auto claims(const std::string& logical) -> std::set<std::string>& {
            if (!operation_claims.contains(logical))
                if (auto bytes = pages->get("m/c/" + logical)) {
                    auto ids = string_list(codec::decode(*bytes).at("ids"));
                    operation_claims[logical] = {ids.begin(), ids.end()};
                }
            return operation_claims[logical];
        }
        auto receipts(const std::string& logical) -> std::vector<std::string>& {
            if (!receipt_index.contains(logical))
                if (auto bytes = pages->get("m/r/" + logical))
                    receipt_index[logical] = string_list(codec::decode(*bytes).at("ids"));
            return receipt_index[logical];
        }
        void trim_objects() {
            if (objects.size() <= 512 && cached_object_bytes <= 512 * 1024)
                return;
            auto remaining = object_order.size();
            while ((objects.size() > 256 || cached_object_bytes > 256 * 1024) && remaining-- && !object_order.empty()) {
                auto id = std::move(object_order.front());
                object_order.pop_front();
                if (!objects.contains(id))
                    continue;
                if (!is_validated(id) && !refused.contains(id)) {
                    object_order.push_back(std::move(id));
                    continue;
                }
                cached_object_bytes -= object_sizes.at(id);
                object_sizes.erase(id);
                decoded_inputs.erase(id);
                bodies.erase(id);
                objects.erase(id);
            }
        }
        auto put(value body, const key& signer) -> std::string {
            auto kind = body.at("kind").text();
            auto id = retain(seal(std::move(body), signer));
            if (kind == "blob" || kind == "intent" || kind == "attempt" || kind == "failure" || kind == "review")
                validate_object(id);
            return id;
        }
        auto retain(value object) -> std::string {
            auto data = codec::encode(object);
            auto id = digest_of(data);
            (void)open(object);
            persist(std::move(object), id, data);
            return id;
        }
        void persist(value object, const std::string& id, std::string_view data) {
            archive_audited = false;
            check_schema(object.at("body"));
            auto packed = archive::pack(data);
            statement q{*db, "INSERT OR IGNORE INTO objects(id,data,kind) VALUES(?1,?2,?3)"};
            q.text(1, id);
            q.bind(2, packed);
            q.text(3, object.at("body").at("kind").text());
            q.next();
            if (sqlite3_changes(db->handle)) {
                ++object_count;
                archive_bytes += packed.size();
                object_watermark = static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db->handle));
            }
            auto [stored, inserted] = objects.emplace(id, std::move(object));
            if (inserted) {
                object_order.push_back(id);
                object_sizes.emplace(id, data.size());
                cached_object_bytes += data.size();
            }
            bodies.emplace(id, body_view{&stored->second.at("body")});
            queue_object(id);
        }
        auto blob(std::string bytes_value) -> std::string {
            auto body = base_body("blob", project);
            body["data"] = bytes{std::move(bytes_value)};
            return put(std::move(body), author);
        }
        auto get(const std::string& id, std::string_view kind = {}) const -> const value& {
            auto it = bodies.find(id);
            if (it == bodies.end()) {
                auto data = object_bytes(id);
                auto object = codec::decode(data);
                if (!is_validated(id))
                    check_schema(open(object));
                auto [stored, inserted] = objects.emplace(id, std::move(object));
                if (inserted) {
                    object_order.push_back(id);
                    object_sizes.emplace(id, data.size());
                    cached_object_bytes += data.size();
                }
                it = bodies.emplace(id, body_view{&stored->second.at("body")}).first;
                ++loaded_records;
                queue_object(id);
            }
            if (!kind.empty() && it->second.at("kind").text() != kind)
                fail(nodes::code::invalid_record, "reference has incorrect kind");
            return it->second;
        }
        auto has_object(const std::string& id) const -> bool {
            statement q{*db, "SELECT 1 FROM objects WHERE id=?1"};
            q.text(1, id);
            return q.next();
        }
        auto object_bytes(const std::string& id) const -> std::string {
            statement q{*db, "SELECT data FROM objects WHERE id=?1"};
            q.text(1, id);
            if (!q.next()) {
                error e{nodes::code::missing_objects, "required object unavailable"};
                e.missing.push_back(id);
                throw e;
            }
            auto data = archive::unpack(q.data(0));
            if (digest_of(data) != id)
                fail(nodes::code::storage_failure, "stored object hash mismatch");
            return data;
        }
        auto object_ids(std::string_view kind = {}) const -> std::vector<std::string> {
            statement q{*db, kind.empty() ? "SELECT id FROM objects ORDER BY id" : "SELECT id FROM objects WHERE kind=?1 ORDER BY id"};
            if (!kind.empty())
                q.text(1, kind);
            std::vector<std::string> ids;
            while (q.next())
                ids.push_back(q.data(0));
            return ids;
        }
        auto request_input(const std::string& id) -> const value& {
            if (auto it = decoded_inputs.find(id); it != decoded_inputs.end())
                return it->second;
            return decoded_inputs.emplace(id, codec::decode(get(id, "blob").at("data").raw())).first->second;
        }
        struct id_less {
            auto operator()(const std::string* a, const std::string* b) const -> bool { return *a < *b; }
        };
        auto ref_ids(const value& b) const -> std::vector<const std::string*> {
            const auto& kind = b.at("kind").text();
            std::vector<const std::string*> out;
            out.reserve(16);
            auto one = [&](std::string_view k) {
                auto& v = b.at(k);
                if (!std::holds_alternative<std::monostate>(v.data))
                    out.push_back(&v.text());
            };
            auto many = [&](std::string_view k) {
                for (auto& v : b.at(k).list())
                    out.push_back(&v.text());
            };
            if (kind != "genesis")
                out.push_back(&b.at("project").text());
            if (kind == "batch") {
                many("parents");
                one("line");
                one("policy");
                one("intent");
                one("attempt");
                many("required");
            } else if (kind == "policy") {
                many("previous");
                many("settled");
            } else if (kind == "intent") {
                many("parents");
                many("messages");
            } else if (kind == "attempt") {
                one("intent");
                one("input");
                one("result");
                many("observed");
            } else if (kind == "line") {
                many("base");
            } else if (kind == "admission") {
                one("attempt");
                one("line");
                one("policy");
                one("previous");
                many("accepted_dependencies");
                many("prior");
                many("frontier");
                one("result");
                one("review");
            } else if (kind == "receipt") {
                one("admission");
                one("result");
            } else if (kind == "review") {
                one("policy");
                many("frontier");
            } else if (kind == "failure") {
                one("input");
                one("intent");
                one("result");
            } else if (kind == "inventory") {
                many("objects");
                one("base");
            } else if (kind == "checkpoint") {
                many("frontier");
                many("policies");
                many("admissions");
                many("evidence");
                many("receipts");
                many("retained");
                one("configuration");
                one("manifest");
                one("inventory");
            } else if (kind != "blob" && kind != "genesis")
                fail(nodes::code::invalid_record, "unknown object kind");
            std::ranges::sort(out, [](auto* a, auto* b) { return canonical_less(*a, *b); });
            out.erase(std::unique(out.begin(), out.end(), [](auto* a, auto* b) { return *a == *b; }), out.end());
            return out;
        }
        auto refs(const value& b) const -> std::vector<std::string> {
            auto ids = ref_ids(b);
            std::vector<std::string> out;
            out.reserve(ids.size());
            for (auto* id : ids)
                out.push_back(*id);
            return out;
        }
        void check_schema(const value& b) const {
            auto kind = b.at("kind").text();
            if (kind == "genesis")
                fields(b, {"authors", "founding_key", "kind", "maintainers", "nodes", "nonce", "owner", "protected", "schema"});
            else if (kind == "blob")
                fields(b, {"data", "kind", "project", "schema"});
            else if (kind == "intent")
                fields(b, {"author", "kind", "messages", "parents", "project", "schema", "summary"});
            else if (kind == "attempt")
                fields(b, {"author", "executor", "input", "intent", "kind", "observed", "operation", "outcome", "project", "result", "schema", "tool"});
            else if (kind == "batch")
                fields(b, {"attempt", "author", "dependencies", "effect_digest", "effects", "expected", "intent", "kind", "lamport", "line", "operation",
                           "parents", "policy", "project", "request_digest", "required", "schema"});
            else if (kind == "line")
                fields(b, {"base", "creator", "kind", "name", "operation", "project", "schema"});
            else if (kind == "policy")
                fields(b, {"authors", "epoch", "kind", "maintainers", "nodes", "previous", "project", "protected", "schema", "settled"});
            else if (kind == "admission")
                fields(b, {"accepted_dependencies", "attempt", "author_override", "frontier", "issuer", "kind", "line", "operation", "policy", "previous",
                           "prior", "project", "result", "review", "schema", "target"});
            else if (kind == "receipt")
                fields(b, {"admission", "author", "effect_digest", "issuer", "kind", "operation", "project", "request_digest", "result", "schema", "target",
                           "version"});
            else if (kind == "review")
                fields(b, {"author", "frontier", "identities", "kind", "policy", "project", "schema", "target"});
            else if (kind == "failure")
                fields(b, {"author", "code", "input", "intent", "kind", "operation", "project", "reason", "request_digest", "result", "schema"});
            else if (kind == "inventory")
                fields(b, {"base", "complete", "kind", "missing", "objects", "owner", "project", "schema", "worldlines"});
            else if (kind == "checkpoint")
                fields(b, {"admissions", "configuration", "evidence", "frontier", "inventory", "kind", "manifest", "owner", "policies", "project", "receipts",
                           "retained", "schema", "worldline"});
            else
                fail(nodes::code::invalid_record, "unrecognized object kind");
            for (auto name : {"accepted_dependencies", "admissions", "authors", "dependencies", "evidence", "frontier", "identities", "maintainers", "missing",
                              "nodes", "objects", "parents", "policies", "protected", "receipts", "required", "retained", "settled"}) {
                auto it = b.object().find(name);
                if (it != b.object().end()) {
                    auto& set = it->second.list();
                    for (std::size_t i = 0; i < set.size(); ++i) {
                        auto& id = set[i].text();
                        if (i && !canonical_less(set[i - 1].text(), id))
                            fail(nodes::code::invalid_record, "noncanonical set field: " + std::string(name));
                        if (std::string_view{name} != "dependencies" && std::string_view{name} != "identities" && std::string_view{name} != "protected" &&
                            !content::is_hash(id))
                            fail(nodes::code::invalid_record, "invalid identity or object address");
                    }
                }
            }
            if (kind == "policy") {
                auto previous = string_list(b.at("previous"));
                if (previous != vector_set(previous))
                    fail(nodes::code::invalid_record, "noncanonical policy predecessors");
                for (auto& id : previous)
                    if (!content::is_hash(id))
                        fail(nodes::code::invalid_record, "invalid policy predecessor address");
            }
            if (kind == "line") {
                auto& base = b.at("base").list();
                for (std::size_t i = 0; i < base.size(); ++i)
                    if (!content::is_hash(base[i].text()) || (i && !canonical_less(base[i - 1].text(), base[i].text())))
                        fail(nodes::code::invalid_record, "noncanonical worldline base");
                checked_name(b.at("name").text());
                if (!content::is_hash(b.at("creator").text()) || !content::is_hash(b.at("operation").text()))
                    fail(nodes::code::invalid_record, "invalid worldline creation identity");
            }
            if (kind == "admission" && !std::holds_alternative<std::monostate>(b.at("previous").data) && !content::is_hash(b.at("previous").text()))
                fail(nodes::code::invalid_record, "invalid previous acceptance address");
            if (kind == "admission" || kind == "receipt" || kind == "review") {
                auto& target = b.at("target");
                fields(target, {"name", "owner", "reviewer"});
                checked_name(target.at("name").text());
                if (!content::is_hash(target.at("owner").text()) || !content::is_hash(target.at("reviewer").text()))
                    fail(nodes::code::invalid_record, "invalid worldline ownership identity");
            }
            for (auto name : {"project", "policy", "intent", "attempt", "input", "result", "admission", "configuration", "manifest", "inventory", "base",
                              "review", "line"}) {
                auto it = b.object().find(name);
                if (kind == "line" && std::string_view{name} == "base")
                    continue;
                if (it != b.object().end() && !std::holds_alternative<std::monostate>(it->second.data) && !content::is_hash(it->second.text()))
                    fail(nodes::code::invalid_record, "invalid object reference");
            }
            if (kind == "blob")
                (void)b.at("data").raw();
        }
        auto issuer(const std::string& id) const -> std::string {
            (void)get(id);
            return digest_of(objects.at(id).at("public_key").raw());
        }
        auto policy_before(const std::string& a, const std::string& b) const -> bool {
            if (a == b)
                return true;
            std::set<std::string> seen;
            std::vector<std::string> work{b};
            while (!work.empty()) {
                auto id = std::move(work.back());
                work.pop_back();
                if (!seen.insert(id).second)
                    continue;
                if (id == a)
                    return true;
                auto& x = get(id);
                if (x.at("kind").text() == "policy")
                    for (auto& p : string_list(x.at("previous")))
                        work.push_back(p);
            }
            return false;
        }
        void refresh_policies() {
            std::vector<std::string> all;
            if (has_object(project))
                all.push_back(project);
            for (auto& id : object_ids("policy"))
                if (is_validated(id))
                    all.push_back(id);
            policies.clear();
            for (auto& id : all)
                if (std::ranges::none_of(all, [&](auto& other) { return id != other && policy_before(id, other); }))
                    policies.push_back(id);
            std::ranges::sort(policies);
        }
        auto allowed(const value& policy, std::string_view field, const std::string& who) const -> bool {
            return std::ranges::any_of(policy.at(field).list(), [&](auto& identity) { return identity.text() == who; });
        }
        auto protected_target(const value& policy, std::string_view worldline) const -> bool {
            for (auto& name : string_list(policy.at("protected")))
                if (name == worldline)
                    return true;
            return false;
        }
        auto line_of(const std::string& event) const -> std::string {
            auto& body = get(event);
            if (body.at("kind").text() != "batch")
                fail("worldline_boundary", "selected source is not a source batch");
            return body.at("line").text();
        }
        // The line a root name stands for: main is the genesis line, every other name is bound by its creating line object.
        // A line takes its name only once an accepted fork created a root for it.
        auto lines() const -> const line_index& {
            if (!line_names) {
                line_names.emplace();
                for (auto& id : object_ids("admission"))
                    if (is_validated(id))
                        register_line(get(id));
            }
            return *line_names;
        }
        auto registered_lines(const std::string& name) const -> const std::set<std::string>& {
            static const std::set<std::string> none;
            auto found = lines().names.find(name);
            return found == lines().names.end() ? none : found->second;
        }
        void register_line(const value& admission) const {
            if (!line_names)
                return;
            auto& line = admission.at("line").text();
            line_names->names[line == project ? std::string{"main"} : get(line, "line").at("name").text()].insert(line);
            if (!admission.at("frontier").list().empty())
                line_names->active.insert(line);
        }
        // A local root keeps its name only while its line is the one every node resolves the name to.
        auto canonical(const std::string& name) const -> bool {
            auto admission = root_admissions.find(name);
            if (admission == root_admissions.end() || !is_validated(admission->second))
                return admission == root_admissions.end();
            auto& line = get(admission->second, "admission").at("line").text();
            if (name == "main")
                return line == project;
            auto& found = registered_lines(name);
            return !found.empty() && *found.begin() == line;
        }
        auto worldlines() const -> std::map<std::string, std::vector<std::string>> {
            std::map<std::string, std::vector<std::string>> out;
            for (auto& [name, frontier] : roots)
                if (canonical(name))
                    out.emplace(name, frontier);
            return out;
        }
        // Forks made apart under one name resolve to the smallest line identity on every node.
        auto named_line(const std::string& name) const -> std::string {
            if (name == "main")
                return project;
            auto& found = registered_lines(name);
            if (found.empty())
                fail(nodes::code::invalid_record, "worldline not found");
            return *found.begin();
        }
        auto shown_operation(const value& attempt) -> std::string {
            auto& input = request_input(attempt.at("input").text());
            auto& caller = input.at("caller_operation").text();
            return caller.empty() ? attempt.at("operation").text() : caller;
        }
        auto line_base(const std::string& line) const -> std::vector<std::string> {
            return line == project ? std::vector<std::string>{} : string_list(get(line, "line").at("base"));
        }
        // An event belongs to a worldline when the line produced it or the line was forked after it.
        auto in_line(const std::string& line, const std::string& event) const -> bool {
            return line_of(event) == line || source.includes(line_base(line), event);
        }
        void within_line(const std::string& line, const std::vector<std::string>& heads) const {
            for (auto& head : heads)
                if (!in_line(line, head))
                    fail("worldline_boundary", "selected source belongs to another worldline");
        }
        static auto selection(std::string_view action) -> bool { return action == "fork" || action == "integrate" || action == "admit"; }
        // A source acceptance adds one batch to the root: the frontier event outside the prior root that binds the accepted attempt.
        auto admitted_batch(const value& admission) const -> std::string {
            auto prior = string_list(admission.at("prior"));
            for (auto& event : string_list(admission.at("frontier")))
                if (get(event).at("kind").text() == "batch" && get(event).at("attempt") == admission.at("attempt") && !source.includes(prior, event))
                    return event;
            fail(nodes::code::invalid_record, "acceptance does not name its executed batch");
        }
        auto root_line(const std::string& name) const -> std::string {
            auto admission = root_admissions.find(name);
            if (admission == root_admissions.end())
                return named_line(name);
            auto line = get(admission->second, "admission").at("line").text();
            if (line != named_line(name))
                fail("worldline_boundary", "an earlier fork owns this worldline name; fork this state under another name");
            return line;
        }
        // An observation is a state of the line only when it contains the state the line was forked from.
        void observes_line(const std::string& line, const std::vector<std::string>& observed) const {
            within_line(line, observed);
            for (auto& event : line_base(line))
                if (!source.includes(observed, event))
                    fail("stale_frontier", "observed source does not contain the worldline base");
        }
        void verify_policy(const std::string& id, const value& b) const {
            auto prev = string_list(b.at("previous"));
            if (prev.empty() || prev != vector_set(prev))
                fail(nodes::code::invalid_record, "invalid policy predecessors");
            std::uint64_t epoch = 0;
            auto signer = issuer(id);
            for (auto& p : prev) {
                auto& old = get(p);
                auto kind = old.at("kind").text();
                if (kind != "genesis" && kind != "policy")
                    fail(nodes::code::invalid_record, "invalid policy reference");
                if (!allowed(old, "maintainers", signer))
                    fail(nodes::code::policy_denied, "policy signer is not a predecessor maintainer");
                auto maintainers = string_list(old.at("maintainers"));
                if (maintainers.size() != 1)
                    fail(nodes::code::policy_denied, "policy requires every predecessor maintainer signature");
                if (kind == "policy")
                    epoch = std::max(epoch, old.at("epoch").number());
            }
            if (epoch == std::numeric_limits<std::uint64_t>::max() || b.at("epoch").number() != epoch + 1)
                fail(nodes::code::invalid_record, "invalid policy epoch");
            if (string_list(b.at("maintainers")).empty())
                fail(nodes::code::invalid_record, "policy has no maintainer");
            for (auto& field : {"authors", "nodes", "maintainers"})
                for (auto& who : string_list(b.at(field)))
                    if (!content::is_hash(who))
                        fail(nodes::code::invalid_record, "invalid policy identity");
            for (auto& name : string_list(b.at("protected")))
                checked_name(name);
            for (auto& a : string_list(b.at("settled")))
                get(a, "admission");
        }
        void validate_object(const std::string& id) {
            if (is_validated(id))
                return;
            auto& b = get(id);
            auto kind = b.at("kind").text();
            if (kind == "genesis") {
                if (id != project || b.at("nonce").raw().size() != 32 || b.at("founding_key").raw() != objects.at(id).at("public_key").raw() ||
                    !allowed(b, "maintainers", issuer(id)))
                    fail("wrong_project", "invalid trusted genesis");
                mark_validated(id);
                return;
            }
            if (b.at("project").text() != project)
                fail("wrong_project", "object is bound to another project");
            for (auto* r : ref_ids(b))
                if (!is_validated(*r))
                    get(*r);
            if (kind == "policy")
                verify_policy(id, b);
            else if (kind == "batch") {
                if (issuer(id) != b.at("author").text())
                    fail("invalid_signature", "batch author does not match signature");
                if (b.at("parents") != b.at("expected"))
                    fail(nodes::code::invalid_record, "batch base does not match parent frontier");
                auto digest = effects_digest(b.at("effects").list());
                if (digest != b.at("effect_digest").text())
                    fail(nodes::code::invalid_record, "effect digest mismatch");
                get(b.at("policy").text());
                get(b.at("intent").text(), "intent");
                auto& attempt = get(b.at("attempt").text(), "attempt");
                if (attempt.at("author") != b.at("author") || attempt.at("operation") != b.at("operation") || attempt.at("intent") != b.at("intent") ||
                    digest_of(get(attempt.at("input").text(), "blob").at("data").raw()) != b.at("request_digest").text())
                    fail(nodes::code::invalid_record, "batch does not bind its signed attempt");
                auto request = decode_request(request_input(attempt.at("input").text()), get(attempt.at("intent").text(), "intent").at("summary").text(),
                                              get(attempt.at("result").text(), "blob").at("data").raw());
                for (auto& effect : b.at("effects").list())
                    if (effect.at("type").text() == "binary") {
                        auto digest = effect.at("value").text();
                        bool declared = false;
                        for (auto& required : string_list(b.at("required"))) {
                            auto& object = get(required);
                            if (object.at("kind").text() == "blob" && digest_of(object.at("data").raw()) == digest) {
                                source.add_blob(digest, object.at("data").raw());
                                declared = true;
                                break;
                            }
                        }
                        if (!declared)
                            fail(nodes::code::invalid_record, "binary effect omits its required content object");
                    }
                auto observed_effects = effects(request, request.expected, b.at("author").text());
                if (request.expected != string_list(b.at("parents")) ||
                    source.dependencies(request.expected, observed_effects, request.dependencies) != string_list(b.at("dependencies")) ||
                    observed_effects != b.at("effects").list())
                    fail(nodes::code::invalid_record, "source effects differ from the observed signed request");
                auto line = b.at("line").text();
                if (line != project)
                    get(line, "line");
                observes_line(line, string_list(b.at("parents")));
                source.add(id, b);
            } else if (kind == "line") {
                if (issuer(id) != b.at("creator").text())
                    fail("invalid_signature", "worldline creator differs from signature");
                if (b.at("name").text() == "main")
                    fail("worldline_boundary", "main is the genesis worldline");
                std::set<std::string> lines;
                for (auto& event : string_list(b.at("base")))
                    lines.insert(line_of(event));
                if (lines.size() > 1)
                    fail("worldline_boundary", "worldline base spans several worldlines");
            } else if (kind == "admission") {
                auto& p = get(b.at("policy").text());
                bool protected_line = protected_target(p, b.at("target").at("name").text());
                if (issuer(id) != b.at("issuer").text() || (protected_line && !allowed(p, "nodes", issuer(id))))
                    fail(nodes::code::policy_denied, "unauthorized accepting node");
                auto& attempt = get(b.at("attempt").text(), "attempt");
                auto& input = request_input(attempt.at("input").text());
                if (attempt.at("executor") != b.at("issuer") || attempt.at("operation") != b.at("operation") || attempt.at("result") != b.at("result") ||
                    b.at("target").at("owner") != b.at("issuer") || b.at("target").at("reviewer") != attempt.at("author") ||
                    input.at("owner") != b.at("issuer") || input.at("project") != b.at("project") || input.at("worldline") != b.at("target").at("name") ||
                    (selection(input.at("action").text()) && input.at("expected") != b.at("prior")))
                    fail(nodes::code::invalid_record, "acceptance does not bind its signed request");
                if (protected_line && !allowed(p, "authors", attempt.at("author").text()))
                    fail(nodes::code::policy_denied, "accepting request author is unauthorized");
                auto same_target = [&](const value& prior) {
                    return prior.at("target").at("owner") == b.at("target").at("owner") && prior.at("target").at("name") == b.at("target").at("name");
                };
                if (!std::holds_alternative<std::monostate>(b.at("previous").data)) {
                    auto& prior = get(b.at("previous").text(), "admission");
                    if (!same_target(prior) || prior.at("frontier") != b.at("prior"))
                        fail(nodes::code::invalid_record, "previous acceptance does not bind the prior worldline root");
                } else if (!b.at("prior").list().empty())
                    fail(nodes::code::invalid_record, "nonempty prior source requires its retained acceptance");
                std::vector<std::string> covered;
                for (auto& dependency : string_list(b.at("accepted_dependencies"))) {
                    auto& prior = get(dependency, "admission");
                    if (prior.at("policy") != b.at("policy") || !same_target(prior))
                        fail(nodes::code::policy_denied, "admission dependency is outside this target and policy");
                    auto selected = string_list(prior.at("frontier"));
                    covered.insert(covered.end(), selected.begin(), selected.end());
                }
                auto action = input.at("action").text();
                auto accepted = string_list(b.at("frontier"));
                auto expected = string_list(b.at("prior"));
                auto line = b.at("line").text();
                if (line == project ? b.at("target").at("name").text() != "main" : get(line, "line").at("name") != b.at("target").at("name"))
                    fail("worldline_boundary", "acceptance binds a worldline name to another worldline");
                bool first = std::holds_alternative<std::monostate>(b.at("previous").data);
                if (action == "fork") {
                    auto& created = get(line, "line");
                    if (!first || !expected.empty() || string_list(created.at("base")) != vector_set(accepted) ||
                        created.at("operation") != b.at("operation") || created.at("creator") != attempt.at("author"))
                        fail(nodes::code::invalid_record, "fork acceptance does not bind its created worldline");
                } else if (!first && get(b.at("previous").text(), "admission").at("line") != b.at("line"))
                    fail("worldline_boundary", "acceptance changes the worldline of its root");
                if (action == "fork" || action == "integrate" || action == "admit") {
                    auto selected = string_list(input.at("source"));
                    if (action != "fork") {
                        selected.insert(selected.end(), expected.begin(), expected.end());
                        observes_line(line, accepted);
                    }
                    if (source.frontier(std::move(selected)) != accepted)
                        fail(nodes::code::invalid_record, "acceptance selects source outside the signed request");
                } else {
                    auto event = admitted_batch(b);
                    auto& batch = get(event, "batch");
                    auto grown = expected;
                    grown.push_back(event);
                    if (batch.at("operation") != b.at("operation") || batch.at("line") != b.at("line") || source.includes(expected, event) ||
                        source.frontier(std::move(grown)) != accepted)
                        fail(nodes::code::invalid_record, "acceptance does not bind the executed batch");
                    auto parents = string_list(batch.at("parents"));
                    if (parents.empty() && !expected.empty())
                        fail(nodes::code::invalid_record, "batch observed no state of the accepted worldline");
                    for (auto& parent : parents)
                        if (!source.includes(expected, parent))
                            fail(nodes::code::invalid_record, "batch observed source outside the accepted worldline");
                }
                if (b.at("author_override").boolean()) {
                    auto& review = get(b.at("review").text(), "review");
                    if (!allowed(p, "maintainers", review.at("author").text()) || issuer(b.at("review").text()) != review.at("author").text() ||
                        review.at("policy") != b.at("policy") || review.at("frontier") != b.at("frontier") || review.at("target") != b.at("target"))
                        fail(nodes::code::policy_denied, "override lacks a signed maintainer decision");
                } else if (!std::holds_alternative<std::monostate>(b.at("review").data))
                    fail(nodes::code::invalid_record, "unexpected override decision");
                if (protected_line && !b.at("author_override").boolean())
                    for (auto& event : source.uncovered(string_list(b.at("frontier")), covered)) {
                        if (!allowed(p, "authors", source.event_author(event))) {
                            bool settled = false;
                            for (auto& prior : string_list(p.at("kind").text() == "policy" ? p.at("settled") : value{value::array{}})) {
                                auto accepted = source.reachable(string_list(get(prior, "admission").at("frontier")));
                                if (std::ranges::find(accepted, event) != accepted.end())
                                    settled = true;
                            }
                            if (!settled)
                                fail(nodes::code::policy_denied, "admission contains an unauthorized author");
                        }
                    }
                register_line(b);
            } else if (kind == "attempt") {
                if (issuer(id) != b.at("author").text())
                    fail("invalid_signature", "attempt author differs from signature");
                auto& input = request_input(b.at("input").text());
                if (input.at("operation") != b.at("operation") || input.at("owner") != b.at("executor") || input.at("project") != b.at("project"))
                    fail(nodes::code::invalid_record, "attempt input identity mismatch");
                (void)decode_request(input, get(b.at("intent").text(), "intent").at("summary").text(), get(b.at("result").text(), "blob").at("data").raw());
                static const std::set<std::string> actions{"apply", "fork", "integrate", "admit", "reverse", "redo", "resolve"};
                if (!actions.contains(input.at("action").text()) || b.at("tool").text() != tool_name(input.at("tool").text(), input.at("action").text()) ||
                    b.at("outcome").text() != "accepted" || !content::is_hash(b.at("operation").text()) || !content::is_hash(b.at("executor").text()) ||
                    get(b.at("intent").text(), "intent").at("author") != b.at("author"))
                    fail(nodes::code::invalid_record, "invalid source tool attempt definition");
                auto logical = b.at("author").text() + ":" + b.at("operation").text();
                auto& claims = this->claims(logical);
                dirty_claims.insert(logical);
                claims.insert(digest_of(get(b.at("input").text(), "blob").at("data").raw()));
                dirty_conflicts.insert(logical);
                if (claims.size() > 1) {
                    auto ids = std::vector<std::string>{claims.begin(), claims.end()};
                    ids.push_back(logical);
                    identity_conflicts[logical] =
                        "identity:" + digest_of(codec::encode(value::map{{"effects", strings(vector_set(std::move(ids)))}, {"type", "identity"}}));
                }
            } else if (kind == "intent") {
                if (issuer(id) != b.at("author").text())
                    fail("invalid_signature", "intent author differs from signature");
            } else if (kind == "review") {
                if (issuer(id) != b.at("author").text() || b.at("target").at("reviewer") != b.at("author") ||
                    !allowed(get(b.at("policy").text()), "maintainers", issuer(id)))
                    fail(nodes::code::policy_denied, "invalid maintainer review");
            } else if (kind == "failure") {
                if (issuer(id) != b.at("author").text() || !content::is_hash(b.at("operation").text()))
                    fail(nodes::code::invalid_record, "invalid failed attempt identity");
                auto raw = get(b.at("input").text(), "blob").at("data").raw();
                if (digest_of(raw) != b.at("request_digest").text())
                    fail(nodes::code::invalid_record, "failed attempt input mismatch");
                (void)decode_request(codec::decode(raw), get(b.at("intent").text(), "blob").at("data").raw(),
                                     get(b.at("result").text(), "blob").at("data").raw());
            } else if (kind == "receipt") {
                auto& a = get(b.at("admission").text(), "admission");
                if (issuer(id) != b.at("issuer").text() || b.at("issuer") != a.at("issuer") || b.at("operation") != a.at("operation") ||
                    b.at("target") != a.at("target") || b.at("result") != a.at("result"))
                    fail(nodes::code::invalid_record, "receipt does not bind original acceptance");
                auto& attempt = get(a.at("attempt").text(), "attempt");
                if (b.at("author") != attempt.at("author") ||
                    digest_of(get(attempt.at("input").text(), "blob").at("data").raw()) != b.at("request_digest").text())
                    fail(nodes::code::invalid_record, "receipt request digest mismatch");
                auto action = request_input(attempt.at("input").text()).at("action").text();
                auto expected_digest = selection(action) ? digest_of(codec::encode(value::map{{"frontier", a.at("frontier")}, {"prior", a.at("prior")}}))
                                                         : get(admitted_batch(a), "batch").at("effect_digest").text();
                if (b.at("effect_digest").text() != expected_digest)
                    fail(nodes::code::invalid_record, "receipt effect digest mismatch");
                if (b.at("version").text() != version(string_list(a.at("frontier"))))
                    fail(nodes::code::invalid_record, "receipt version mismatch");
                auto logical = b.at("author").text() + ":" + b.at("operation").text();
                receipts(logical).push_back(id);
                dirty_receipts.insert(logical);
            } else if (kind == "checkpoint") {
                auto state_owner = source.projection(string_list(b.at("frontier")));
                const auto& state = *state_owner;
                if (!state.conflicts.empty())
                    fail(nodes::code::source_conflict, "checkpoint has unresolved source");
                value::array expected;
                for (auto& [file, f] : state.files)
                    if (f->present)
                        expected.emplace_back(value::map{{"digest", f->digest},
                                                         {"executable", f->executable},
                                                         {"file", file},
                                                         {"kind", f->kind},
                                                         {"path", f->path},
                                                         {"size", static_cast<std::uint64_t>(f->bytes())}});
                auto manifest = codec::decode(get(b.at("manifest").text(), "blob").at("data").raw());
                if (manifest.at("files").list() != expected)
                    fail(nodes::code::invalid_record, "checkpoint manifest does not match fixed source");
                auto inventory = codec::decode(get(b.at("inventory").text(), "blob").at("data").raw());
                if (inventory.at("objects") != b.at("retained"))
                    fail(nodes::code::invalid_record, "checkpoint inventory mismatch");
                auto roots = string_list(b.at("policies"));
                if (roots.size() != 1)
                    fail("policy_conflict", "checkpoint must bind one admission policy");
                auto permissions = get(roots.front());
                if (issuer(id) != b.at("owner").text())
                    fail("invalid_signature", "checkpoint owner differs from signer");
                if (protected_target(permissions, b.at("worldline").text()) && !allowed(permissions, "nodes", issuer(id)))
                    fail(nodes::code::policy_denied, "checkpoint node lacks target authority");
                bool accepted = false;
                std::vector<std::string> covered;
                for (auto& a : string_list(b.at("admissions"))) {
                    auto& admission = get(a, "admission");
                    if (admission.at("issuer") == b.at("owner") && admission.at("target").at("name") == b.at("worldline")) {
                        if (admission.at("frontier") == b.at("frontier")) {
                            within_line(admission.at("line").text(), string_list(b.at("frontier")));
                            accepted = true;
                        }
                        if (admission.at("policy") == b.at("policies").list().front()) {
                            auto heads = string_list(admission.at("frontier"));
                            covered.insert(covered.end(), heads.begin(), heads.end());
                        }
                    }
                    trim_objects();
                }
                if (!accepted && !(b.at("frontier").list().empty() && allowed(permissions, "nodes", b.at("owner").text())))
                    fail(nodes::code::policy_denied, "checkpoint lacks a retained owner admission");
                if (protected_target(permissions, b.at("worldline").text()))
                    for (auto& event : source.uncovered(string_list(b.at("frontier")), covered)) {
                        if (!allowed(permissions, "authors", source.event_author(event))) {
                            bool settled = false;
                            if (permissions.at("kind").text() == "policy")
                                for (auto& prior : string_list(permissions.at("settled")))
                                    if (source.includes(string_list(get(prior, "admission").at("frontier")), event))
                                        settled = true;
                            if (!settled)
                                fail("policy_pending", "checkpoint source requires current-policy admission");
                        }
                        trim_objects();
                    }
            } else if (kind == "inventory") {
                if (issuer(id) != b.at("owner").text())
                    fail("invalid_signature", "inventory owner differs from origin");
                auto objects = string_list(b.at("objects"));
                if (objects != vector_set(objects))
                    fail(nodes::code::invalid_record, "noncanonical inventory objects");
                if (!std::holds_alternative<std::monostate>(b.at("base").data)) {
                    auto& base = get(b.at("base").text(), "checkpoint");
                    auto retained = string_list(base.at("retained"));
                    objects.insert(objects.end(), retained.begin(), retained.end());
                    objects = vector_set(std::move(objects));
                }
                for (auto& line : b.at("worldlines").list()) {
                    fields(line, {"frontier", "name"});
                    checked_name(line.at("name").text());
                    for (auto& event : string_list(line.at("frontier")))
                        if (!std::ranges::binary_search(objects, event))
                            fail(nodes::code::invalid_record, "inventory omits a worldline root");
                }
            }
            mark_validated(id);
        }
        auto missing(const std::vector<std::string>& initial) const -> std::vector<std::string> {
            std::set<std::string> seen, absent;
            std::vector<std::string> work = initial;
            while (!work.empty()) {
                auto id = std::move(work.back());
                work.pop_back();
                if (!seen.insert(id).second)
                    continue;
                if (is_validated(id))
                    continue;
                if (!has_object(id)) {
                    absent.insert(id);
                    continue;
                }
                for (auto* r : ref_ids(get(id)))
                    work.push_back(*r);
            }
            return {absent.begin(), absent.end()};
        }
        void refuse(const std::string& id, const error& reason) {
            auto encoded = codec::encode(objects.at(id));
            auto status = codec::encode(value::map{{"code", reason.code}, {"reason", std::string(reason.what())}});
            statement q{*db, "INSERT OR REPLACE INTO rejected(id,reason,data) VALUES(?1,?2,?3)"};
            q.text(1, id);
            q.bind(2, status);
            q.bind(3, encoded);
            q.next();
            refused[id] = {reason.code, reason.what()};
            pending_objects.erase(id);
            remaining_dependencies.erase(id);
            dirty_status.insert(id);
            refused_objects.push_back(id);
        }
        auto validate_available() -> std::vector<std::string> {
            ensure_status();
            if (!pending_loaded) {
                pending_loaded = true;
                pages->each("m/p/", [&](const std::string& key, std::string) { queue_object(key.substr(4)); });
            }
            std::deque<std::string> ready;
            std::vector<std::string> rejected;
            std::size_t processed = 0;
            auto release = [&](const std::string& id, bool rejected_dependency) {
                auto found = waiting_dependencies.find(id);
                if (found == waiting_dependencies.end())
                    return;
                auto waiting = std::move(found->second);
                waiting_dependencies.erase(found);
                for (auto& dependent : waiting)
                    if (pending_objects.contains(dependent)) {
                        if (rejected_dependency) {
                            refuse(dependent, error{nodes::code::policy_denied, "a required object was refused"});
                            rejected.push_back(dependent);
                        } else if (auto count = remaining_dependencies.find(dependent);
                                   count != remaining_dependencies.end() && count->second && --count->second == 0)
                            ready.push_back(dependent);
                    }
            };
            while (!new_candidates.empty() || !completed_objects.empty() || !refused_objects.empty() || !ready.empty()) {
                if (!completed_objects.empty()) {
                    auto id = std::move(completed_objects.front());
                    completed_objects.pop_front();
                    release(id, false);
                    continue;
                }
                if (!refused_objects.empty()) {
                    auto id = std::move(refused_objects.front());
                    refused_objects.pop_front();
                    release(id, true);
                    continue;
                }
                if (!ready.empty()) {
                    auto id = std::move(ready.front());
                    ready.pop_front();
                    if (!pending_objects.contains(id))
                        continue;
                    try {
                        validate_object(id);
                    } catch (const error& reason) {
                        if (reason.code == nodes::code::storage_failure || reason.code == nodes::code::resource_limit)
                            throw;
                        refuse(id, reason);
                        rejected.push_back(id);
                    }
                    if (++processed % 128 == 0)
                        trim_objects();
                    continue;
                }
                auto id = std::move(new_candidates.front());
                new_candidates.pop_front();
                if (!pending_objects.contains(id) || remaining_dependencies.contains(id) || refused.contains(id))
                    continue;
                std::size_t count = 0;
                bool blocked = false;
                for (auto* reference : ref_ids(get(id))) {
                    auto dependency = *reference;
                    if (is_validated(dependency))
                        continue;
                    if (refused.contains(dependency)) {
                        refuse(id, error{nodes::code::policy_denied, "a required object was refused"});
                        rejected.push_back(id);
                        blocked = true;
                        break;
                    }
                    waiting_dependencies[dependency].push_back(id);
                    ++count;
                    if (has_object(dependency))
                        (void)get(dependency);
                }
                if (!blocked) {
                    remaining_dependencies[id] = count;
                    if (!count)
                        ready.push_back(id);
                }
            }
            return rejected;
        }
        void load_objects(std::uint64_t after = 0) {
            struct record {
                std::string id, data;
                value object;
                std::exception_ptr failure;
                std::size_t bytes = 0;
            };
            std::vector<record> batch;
            statement query{*db, "SELECT id,data FROM objects WHERE rowid>?1 ORDER BY rowid"};
            query.number(1, after);
            bool exhausted = false;
            auto fill = [&] {
                batch.clear();
                std::size_t buffered = 0;
                while (!exhausted && batch.size() < 256 && buffered < 8 * 1024 * 1024) {
                    if (!query.next()) {
                        exhausted = true;
                        break;
                    }
                    auto data = archive::unpack(query.data(1));
                    buffered += data.size();
                    batch.push_back({query.data(0), std::move(data), {}, {}});
                    batch.back().bytes = batch.back().data.size();
                }
            };
            auto decode = [&](record& row) {
                if (digest_of(row.data) != row.id)
                    fail(nodes::code::storage_failure, "stored object hash mismatch");
                auto object = codec::decode(row.data);
                if (!validated.contains(row.id))
                    check_schema(open(object));
                row.object = std::move(object);
                std::string{}.swap(row.data);
            };
            auto merge = [&] {
                for (auto& row : batch) {
                    if (row.failure)
                        std::rethrow_exception(row.failure);
                    auto [stored, inserted] = objects.emplace(std::move(row.id), std::move(row.object));
                    if (inserted) {
                        object_order.push_back(stored->first);
                        object_sizes.emplace(stored->first, row.bytes);
                        cached_object_bytes += row.bytes;
                    }
                    bodies.emplace(stored->first, body_view{&stored->second.at("body")});
                    ++loaded_records;
                    queue_object(stored->first);
                }
            };
            fill();
            if (batch.empty())
                return;
            // Decoding runs on the source's assigned disk worker, without an extra pool.
            do {
                for (auto& row : batch)
                    decode(row);
                merge();
                validate_available();
                trim_objects();
                fill();
            } while (!batch.empty());
        }
        auto block_key(std::uint64_t index) const -> std::string { return std::format("k/{:016x}", index); }
        auto ordinal_key(std::uint64_t index) const -> std::string { return std::format("{:016x}", index); }
        // The kernel's view of this node's causal storage.
        struct causal_storage final : causal_reader {
            impl& self;
            explicit causal_storage(impl& self) : self(self) {}
            auto header(std::size_t index) -> value override {
                auto bytes = self.pages->get("q/header/" + self.ordinal_key(index));
                if (!bytes)
                    fail(nodes::code::missing_objects, "required causal proof is absent");
                ++self.causal_headers_read;
                return codec::decode(*bytes);
            }
            auto text(const std::string& id) -> std::string override {
                ++self.insertion_payloads_read;
                auto v = codec::decode(self.read_payload(id));
                fields(v, {"text"});
                return v.at("text").raw();
            }
            auto file_text(const source_file& file) -> std::string override {
                ++self.file_payloads_read;
                self.cached_payload_files.insert(file.id);
                self.cached_payload_bytes += file.stored_bytes;
                auto v = codec::decode(self.read_payload(file.content_object));
                fields(v, {"text"});
                auto text = v.at("text").raw();
                if (text.size() != file.stored_bytes || digest_of(text) != file.digest)
                    fail(nodes::code::storage_failure, "state file content differs");
                return text;
            }
            auto locate(const std::string& id) -> std::optional<std::size_t> override {
                auto bytes = self.pages->get("q/id/" + id);
                if (!bytes)
                    return {};
                return codec::decode(*bytes).at("index").number();
            }
            auto records(const std::string& kind, const std::string& key) -> std::vector<value> override {
                if (kind == "file") {
                    if (!self.pages->get("q/file_state/" + key))
                        fail(nodes::code::missing_objects, "required file causal index is absent: " + key);
                    ++self.file_histories_read;
                }
                std::vector<value> out;
                self.pages->each("q/" + kind + "/" + key + "/", [&](const std::string&, std::string bytes) {
                    out.push_back(codec::decode(bytes));
                    if (kind != "dependent" && kind != "identity")
                        ++self.causal_effects_read;
                });
                if (kind == "file") {
                    auto count = codec::decode(*self.pages->get("q/file_state/" + key)).at("count").number();
                    if (out.empty() || out.size() != count)
                        fail(nodes::code::missing_objects, "file causal membership is incomplete");
                }
                return out;
            }
            auto projection(const std::vector<std::string>& heads) -> std::shared_ptr<const source_state> override {
                if (auto name = self.matching_view(heads)) {
                    auto& saved = self.views.at(*name);
                    return self.restore_projection(saved.projection, saved.digest);
                }
                return {};
            }
            auto file_changed(const std::string& file, const causal_set& missing) -> bool override {
                for (auto [first, last] : missing.ranges)
                    for (auto kind : {"file", "file_control"}) {
                        auto prefix = "q/" + std::string(kind) + "/" + file + "/";
                        if (self.pages->contains_range(prefix + self.ordinal_key(first) + "/", prefix + self.ordinal_key(last) + "/"))
                            return true;
                    }
                return false;
            }
            auto base(const causal_set& wanted) -> std::pair<std::vector<std::string>, std::shared_ptr<const source_state>> override {
                const saved_view* closest = nullptr;
                std::uint64_t score = std::numeric_limits<std::uint64_t>::max();
                for (auto& [_, saved] : self.views) {
                    causal_set known;
                    auto v = codec::decode(saved.projection);
                    for (auto& range : v.at("causal").list())
                        known.ranges.emplace_back(range.at("first").number(), range.at("last").number());
                    auto delta = known.without(wanted);
                    delta.unite(wanted.without(known));
                    std::uint64_t size = 0;
                    for (auto [first, last] : delta.ranges)
                        size += last - first;
                    if (size < score) {
                        score = size;
                        closest = &saved;
                    }
                }
                if (!closest)
                    return {};
                return {closest->frontier, self.restore_projection(closest->projection, closest->digest)};
            }
        } causal{*this};
        void ensure_kernel() {
            if (kernel_loaded)
                return;
            // A missing derived index is an unavailable state, never an empty history.
            auto count = pages->get("q/count");
            if (!count || codec::decode(*count).at("count").number() != snapshot_events)
                fail(nodes::code::missing_objects, "authenticated causal index is absent; reconstruct from the retained archive");
            source = kernel{};
            source.demand(snapshot_events, kernel_aliases, &causal);
            kernel_tail.clear();
            kernel_loaded = true;
        }
        void persist_event(std::size_t index) {
            auto header = source.event_header(index);
            auto key = ordinal_key(index);
            pages->set("q/header/" + key, codec::encode(header));
            pages->set("q/id/" + header.at("id").text(), codec::encode(value::map{{"index", static_cast<std::uint64_t>(index)}}));
            auto event_ref = codec::encode(value::map{{"event", static_cast<std::uint64_t>(index)}});
            pages->set("q/identity/" + header.at("author").text() + ":" + header.at("operation").text() + "/" + key, event_ref);
            for (auto& dependency : string_list(header.at("dependencies")))
                pages->set("q/dependent/" + dependency + "/" + key, event_ref);
            for (auto record : source.event_records(index)) {
                auto& effect = record.at("effect");
                auto type = effect.at("type").text();
                if (type == "insert") {
                    auto text = effect.at("text").text();
                    auto payload = retain_payload(codec::encode(value::map{{"text", bytes{text}}}));
                    payload_reference(payload, 1);
                    auto& input = std::get<value::map>(std::get<value::map>(record.data)["effect"].data);
                    input.erase(input.find("text"));
                    input["text_id"] = payload;
                    input["scalars"] = static_cast<std::uint64_t>(scalars(text).size());
                }
                auto ordinal = record.at("ordinal").number();
                auto location = key + "/" + ordinal_key(ordinal);
                auto bytes = codec::encode(record);
                auto id = source.operation_effect(header.at("author").text(), header.at("operation").text(), ordinal);
                pages->set("q/event/" + location, bytes);
                pages->set("q/effect/" + id + "/" + location, bytes);
                if (type == "activate" || type == "resolve") {
                    for (auto& selected : string_list(effect.at("selections"))) {
                        pages->set("q/control/" + selected + "/" + location, bytes);
                        if (type == "activate")
                            for (auto& file : source.effect_files(selected))
                                pages->set("q/file_control/" + file + "/" + location, bytes);
                    }
                } else {
                    auto file = effect.at("file").text();
                    std::uint64_t count = 0;
                    if (auto prior = pages->get("q/file_state/" + file))
                        count = codec::decode(*prior).at("count").number();
                    pages->set("q/file_state/" + file, codec::encode(value::map{{"count", count + 1}}));
                    pages->set("q/file/" + file + "/" + location, bytes);
                    if (type == "presence" || type == "path")
                        pages->set("q/presence/" + file + "/" + location, bytes);
                }
            }
        }
        void projection_stores() {
            if (visible_index)
                return;
            sequence_index = std::make_shared<projection_index<sequence_pages>>(*this, *pages);
            anchor_index = std::make_shared<projection_index<anchor_pages>>(*this, *pages);
            visible_index = std::make_shared<projection_index<visible_files>>(*this, *pages);
            hidden_index = std::make_shared<projection_index<hidden_files>>(*this, *pages);
            path_index = std::make_shared<projection_index<path_pages>>(*this, *pages);
            conflict_index = std::make_shared<projection_index<conflict_pages>>(*this, *pages);
            boolean_index = std::make_shared<projection_index<boolean_pages>>(*this, *pages);
            sequence_index->generated = &sequence_pages_written;
            anchor_index->generated = &sequence_pages_written;
        }
        auto persist_projection(const source_state& state) -> std::string {
            projection_stores();
            auto save = []<class T, class C>(const persistent_index<T>& index, const std::shared_ptr<projection_index<C>>& storage) -> value {
                index.persist(storage);
                return value::map{
                    {"root", index.digest()}, {"object", index.object()}, {"count", static_cast<std::uint64_t>(index.size())}, {"mass", index.mass()}};
            };
            value::array causal;
            for (auto [first, last] : state.causal.ranges)
                causal.emplace_back(value::map{{"first", static_cast<std::uint64_t>(first)}, {"last", static_cast<std::uint64_t>(last)}});
            return codec::encode(value::map{{"files", save(state.files, visible_index)},
                                            {"hidden", save(state.hidden, hidden_index)},
                                            {"paths", save(state.paths, path_index)},
                                            {"file_conflicts", save(state.file_conflicts, conflict_index)},
                                            {"path_conflicts", save(state.path_conflicts, conflict_index)},
                                            {"control_conflicts", save(state.control_conflicts, conflict_index)},
                                            {"identity_conflicts", save(state.identity_conflicts, conflict_index)},
                                            {"dependency_conflicts", save(state.dependency_conflicts, conflict_index)},
                                            {"activation", save(state.activation, boolean_index)},
                                            {"resolutions", save(state.resolutions, boolean_index)},
                                            {"causal", std::move(causal)},
                                            {"conflicts", strings(state.conflicts)}});
        }
        auto restore_projection(const std::string& bytes, const std::string& expected_digest) -> std::shared_ptr<const source_state> {
            projection_stores();
            auto state = std::make_shared<source_state>();
            auto v = codec::decode(bytes);
            fields(v, {"activation", "causal", "conflicts", "control_conflicts", "dependency_conflicts", "file_conflicts", "files", "hidden",
                       "identity_conflicts", "path_conflicts", "paths", "resolutions"});
            auto load = []<class T, class C>(persistent_index<T>& index, const value& root, const std::shared_ptr<projection_index<C>>& storage) {
                fields(root, {"count", "mass", "object", "root"});
                index.adopt(root.at("root").text(), root.at("object").text(), root.at("count").number(), true, root.at("mass").number(), storage);
            };
            load(state->files, v.at("files"), visible_index);
            load(state->hidden, v.at("hidden"), hidden_index);
            load(state->paths, v.at("paths"), path_index);
            load(state->file_conflicts, v.at("file_conflicts"), conflict_index);
            load(state->path_conflicts, v.at("path_conflicts"), conflict_index);
            load(state->control_conflicts, v.at("control_conflicts"), conflict_index);
            load(state->identity_conflicts, v.at("identity_conflicts"), conflict_index);
            load(state->dependency_conflicts, v.at("dependency_conflicts"), conflict_index);
            load(state->activation, v.at("activation"), boolean_index);
            load(state->resolutions, v.at("resolutions"), boolean_index);
            for (auto& r : v.at("causal").list()) {
                auto first = r.at("first").number(), last = r.at("last").number();
                if (first >= last || last > snapshot_events || (!state->causal.ranges.empty() && first <= state->causal.ranges.back().second))
                    fail(nodes::code::invalid_record, "projection causal intervals differ");
                state->causal.ranges.emplace_back(first, last);
            }
            state->conflicts = string_list(v.at("conflicts"));
            state->digest = digest_of(codec::encode(value::map{{"conflicts", strings(state->conflicts)}, {"files", state->files.digest()}}));
            if (state->digest != expected_digest)
                fail(nodes::code::storage_failure, "projection digest differs from the selected revision");
            return state;
        }
        auto file_header(std::string_view encoded, const std::string& id) -> std::shared_ptr<const source_file> {
            auto body = codec::decode(encoded);
            fields(body, {"data", "digest", "executable", "file", "kind", "path", "scalars", "sequence", "size"});
            if (body.at("file").text() != id)
                fail(nodes::code::invalid_record, "state file identity differs");
            auto file = std::make_shared<source_file>();
            file->id = id;
            file->path = body.at("path").text();
            file->kind = body.at("kind").text();
            file->digest = body.at("digest").text();
            file->executable = body.at("executable").boolean();
            file->stored_bytes = body.at("size").number();
            file->stored_scalars = body.at("scalars").number();
            file->storage_header = std::string(encoded);
            file->content_object = body.at("data").text();
            file->sequence = sequence_from(body.at("sequence"));
            file->fragments_ready = false;
            if (file->sequence->size() != file->stored_scalars)
                fail(nodes::code::storage_failure, "sequence root scalar count differs");
            auto payload = body.at("data").text();
            checked_path(file->path);
            if ((file->kind != "text" && file->kind != "binary") || !content::is_hash(file->digest) || !content::is_hash(payload))
                fail(nodes::code::invalid_record, "state file metadata");
            file->stored = &causal;
            return file;
        }
        auto file_bytes(const source_file& file) -> std::string {
            if (!file.storage_header.empty())
                return file.storage_header;
            if (file.content_object.empty()) {
                cached_payload_files.insert(file.id);
                cached_payload_bytes += file.bytes();
                file.content_object = retain_payload(codec::encode(value::map{{"text", bytes{file.content()}}}));
            }
            auto data = file.content_object;
            auto sequence = sequence_for(file)->save();
            file.storage_header = codec::encode(value::map{{"data", data},
                                                           {"digest", file.digest},
                                                           {"executable", file.executable},
                                                           {"file", file.id},
                                                           {"kind", file.kind},
                                                           {"path", file.path},
                                                           {"scalars", file.size()},
                                                           {"sequence", std::move(sequence)},
                                                           {"size", file.bytes()}});
            return file.storage_header;
        }
        auto matching_view(const std::vector<std::string>& heads) -> std::optional<std::string> {
            for (auto& [name, view] : views)
                if (view.frontier == heads)
                    return name;
            return {};
        }
        auto ready_reply(const std::string& name, bool include_text, std::string_view path) -> node_reply {
            auto& saved = views.at(name);
            auto out = identity();
            out.frontier = saved.frontier;
            out.version = saved.version;
            out.source_digest = saved.digest;
            out.conflicts = saved.conflicts;
            out.worldlines = worldlines();
            state_tree files{*db};
            files.adopt(saved.files);
            auto append = [&](const std::string& id, const std::string& bytes) {
                auto file = file_header(bytes, id);
                out.files.push_back({id, file->path, file->kind, file->digest,
                                     include_text ? (file->kind == "binary" ? hex(file->content()) : file->content()) : "", file->bytes(), file->executable,
                                     file->kind == "binary" ? "hex" : "utf8"});
            };
            if (path.empty())
                files.each("f/", [&](const std::string& key, std::string bytes) { append(key.substr(2), bytes); });
            else if (auto owners = files.get("p/" + std::string(path)))
                for (auto& id : string_list(codec::decode(*owners).at("ids"))) {
                    auto bytes = files.get("f/" + id);
                    if (!bytes)
                        fail(nodes::code::missing_objects, "state path has no file");
                    append(id, *bytes);
                }
            return out;
        }
        void persist_view(const std::string& name) {
            auto heads = roots.at(name);
            auto state = source.projection(heads);
            saved_view base;
            bool has_base = false;
            if (auto old = views.find(name); old != views.end()) {
                base = old->second;
                has_base = true;
            } else if (!views.empty()) {
                base = views.begin()->second;
                if (auto same = matching_view(heads))
                    base = views.at(*same);
                has_base = true;
            }
            state_tree files{*db};
            files.adopt(base.files);
            auto owner = [&](const std::string& path, const std::string& id, bool add) {
                auto key = "p/" + path;
                std::vector<std::string> owners;
                if (auto bytes = files.get(key))
                    owners = string_list(codec::decode(*bytes).at("ids"));
                if (add)
                    owners.push_back(id);
                else
                    std::erase(owners, id);
                owners = vector_set(std::move(owners));
                if (owners.empty())
                    files.erase(key);
                else
                    files.set(key, codec::encode(value::map{{"ids", strings(owners)}}));
            };
            auto before = has_base ? source.projection(base.frontier) : std::make_shared<const source_state>();
            before->files.difference(state->files, [&](const std::string& id, const auto* previous, const auto* current) {
                std::string old_path = previous ? (*previous)->path : std::string{};
                if (previous && (!current || old_path != (*current)->path))
                    owner(old_path, id, false);
                if (!current)
                    files.erase("f/" + id);
                else {
                    auto& file = **current;
                    if (!previous || old_path != file.path)
                        owner(file.path, id, true);
                    files.set("f/" + id, file_bytes(file));
                }
            });
            saved_view saved{heads,
                             state->conflicts,
                             root_admissions.contains(name) ? root_admissions.at(name) : std::string{},
                             version(heads),
                             state->digest,
                             files.root(),
                             persist_projection(*state)};
            auto original = views.find(name);
            projection_roots(original == views.end() ? std::string{} : original->second.projection, saved.projection);
            auto bytes = codec::encode(value::map{{"admission", saved.admission},
                                                  {"conflicts", strings(saved.conflicts)},
                                                  {"digest", saved.digest},
                                                  {"files", saved.files},
                                                  {"frontier", strings(heads)},
                                                  {"projection", nodes::bytes{saved.projection}},
                                                  {"version", saved.version}});
            pages->set("r/" + name, bytes, saved.files.empty() ? std::vector<std::string>{} : std::vector<std::string>{saved.files});
            views[name] = std::move(saved);
        }
        void reset_state_storage() {
            db->run("DELETE FROM state_links; DELETE FROM state_heads; DELETE FROM state_nodes; DELETE FROM state_values; DROP TABLE state_payloads; CREATE "
                    "TABLE state_payloads(id TEXT PRIMARY KEY,data BLOB NOT NULL,refs INTEGER NOT NULL DEFAULT 0);");
            pages->adopt({});
            views.clear();
            projection_garbage.clear();
            snapshot_ready = false;
            snapshot_events = 0;
            kernel_block = 0;
            kernel_tail.clear();
            for (auto& [name, _] : roots)
                dirty_roots.insert(name);
        }
        void save_snapshot() {
            if (project.empty() || !has_object(project))
                return;
            ensure_kernel();
            bool changed = false;
            for (auto index = snapshot_events; index < source.size(); ++index) {
                persist_event(index);
                auto frame = source.snapshot(index, 1);
                if (kernel_tail.empty()) {
                    if (snapshot_events)
                        ++kernel_block;
                    kernel_tail = "DCK1";
                    codec::integer(kernel_tail, index);
                }
                if (kernel_tail.size() > 12 && kernel_tail.size() + 8 + frame.size() > 16 * 1024) {
                    if (changed)
                        pages->set(block_key(kernel_block), kernel_tail);
                    ++kernel_block;
                    kernel_tail = "DCK1";
                    codec::integer(kernel_tail, index);
                    changed = false;
                }
                codec::integer(kernel_tail, frame.size());
                kernel_tail += frame;
                changed = true;
            }
            if (changed)
                pages->set(block_key(kernel_block), kernel_tail);
            for (auto& id : dirty_status) {
                if (auto refused_object = refused.find(id); refused_object != refused.end()) {
                    auto bytes = codec::encode(value::map{{"code", refused_object->second.first}, {"reason", refused_object->second.second}});
                    pages->set("m/o/" + id, "R");
                    pages->set("m/x/" + id, bytes);
                    pages->erase("m/p/" + id);
                } else if (validated.contains(id)) {
                    pages->set("m/o/" + id, "V");
                    pages->erase("m/p/" + id);
                    pages->erase("m/x/" + id);
                } else {
                    pages->set("m/o/" + id, "P");
                    pages->set("m/p/" + id, "P");
                }
            }
            for (auto& key : dirty_claims) {
                auto& ids = operation_claims.at(key);
                pages->set("m/c/" + key, codec::encode(value::map{{"ids", strings({ids.begin(), ids.end()})}}));
            }
            for (auto& key : dirty_receipts)
                pages->set("m/r/" + key, codec::encode(value::map{{"ids", strings(vector_set(receipt_index.at(key)))}}));
            for (auto& key : dirty_conflicts)
                if (identity_conflicts.contains(key))
                    pages->set("m/i/" + key, codec::encode(value::map{{"id", identity_conflicts.at(key)}}));
            for (auto& key : dirty_cursors) {
                value::array ranges;
                for (auto [first, last] : checkpoint_cursors.at(key).ranges)
                    ranges.emplace_back(value::map{{"first", static_cast<std::uint64_t>(first)}, {"last", static_cast<std::uint64_t>(last)}});
                pages->set("m/t/" + key, codec::encode(value::map{{"ranges", std::move(ranges)}}));
            }
            for (auto& name : dirty_roots)
                persist_view(name);
            pages->set("q/count", codec::encode(value::map{{"count", static_cast<std::uint64_t>(source.size())}}));
            pages->set("policies", codec::encode(value::map{{"ids", strings(policies)}}));
            source.discard_projections();
            collect_projection_garbage();
            pages->finish();
            auto body = value::map{{"aliases", source.aliases()},
                                   {"archive_bytes", archive_bytes},
                                   {"events", static_cast<std::uint64_t>(source.size())},
                                   {"kernel_block", kernel_block},
                                   {"kind", "state_snapshot"},
                                   {"node", node.id()},
                                   {"object_count", object_count},
                                   {"project", project},
                                   {"root", pages->root()},
                                   {"schema", std::uint64_t{1}},
                                   {"watermark", object_watermark}};
            auto encoded = codec::encode(seal(std::move(body), node));
            metadata("state_snapshot", encoded);
            state_snapshot = digest_of(encoded);
            snapshot_events = source.size();
            snapshot_watermark = object_watermark;
            snapshot_ready = true;
            if (kernel_tail.size() > 16 * 1024)
                kernel_tail.clear();
            if (!source.has_reader())
                source.attach_reader(&causal);
            dirty_status.clear();
            dirty_claims.clear();
            dirty_receipts.clear();
            dirty_conflicts.clear();
            dirty_roots.clear();
            dirty_cursors.clear();
        }
        auto restore_snapshot() -> bool {
            statement q{*db, "SELECT data FROM metadata WHERE name='state_snapshot'"};
            if (!q.next())
                return false;
            auto encoded = q.data(0);
            auto sealed = codec::decode(encoded);
            const auto& body = open(sealed);
            fields(body, {"aliases", "archive_bytes", "events", "kernel_block", "kind", "node", "object_count", "project", "root", "schema", "watermark"});
            if (body.at("schema").number() != 1 || body.at("kind").text() != "state_snapshot" || body.at("project").text() != project ||
                body.at("node").text() != node.id() || digest_of(sealed.at("public_key").raw()) != node.id())
                fail("invalid_signature", "state snapshot identity differs");
            kernel_aliases = body.at("aliases").boolean();
            snapshot_events = body.at("events").number();
            snapshot_watermark = body.at("watermark").number();
            object_count = body.at("object_count").number();
            archive_bytes = body.at("archive_bytes").number();
            object_watermark = snapshot_watermark;
            kernel_block = body.at("kernel_block").number();
            statement count{*db, "SELECT COUNT(*) FROM objects WHERE rowid<=?1"};
            count.number(1, snapshot_watermark);
            count.next();
            if (count.number(0) != object_count)
                fail(nodes::code::missing_objects, "state snapshot archive coverage is damaged; run an explicit audit");
            pages->adopt(body.at("root").text());
            snapshot_ready = true;
            kernel_loaded = snapshot_events == 0;
            status_loaded = false;
            pending_loaded = false;
            if (auto bytes = pages->get("policies"))
                policies = string_list(codec::decode(*bytes).at("ids"));
            pages->each("r/", [&](const std::string& key, std::string bytes) {
                auto saved = codec::decode(bytes);
                fields(saved, {"admission", "conflicts", "digest", "files", "frontier", "projection", "version"});
                auto name = key.substr(2);
                checked_name(name);
                saved_view view{string_list(saved.at("frontier")), string_list(saved.at("conflicts")), saved.at("admission").text(), saved.at("version").text(),
                                saved.at("digest").text(),         saved.at("files").text(),           saved.at("projection").raw()};
                roots[name] = view.frontier;
                if (!view.admission.empty())
                    root_admissions[name] = view.admission;
                views[name] = std::move(view);
            });
            state_snapshot = digest_of(encoded);
            return true;
        }
        void reload(bool audit = false) {
            read_snapshot snapshot{*db};
            {
                statement identity{*db, "SELECT data FROM metadata WHERE name='project'"};
                if (identity.next()) {
                    auto id = identity.data(0);
                    if ((!project.empty() && id != project) || !content::is_hash(id))
                        fail("wrong_project", "durable project identity differs");
                    project = std::move(id);
                }
            }
            {
                statement root{*db, "SELECT data FROM metadata WHERE name='state_snapshot'"};
                observed_snapshot = root.next() ? digest_of(root.data(0)) : std::string{};
            }
            std::uint64_t observed_version = data_version;
            {
                statement generation{*db, "PRAGMA data_version"};
                if (generation.next())
                    observed_version = generation.number(0);
            }
            decoded_inputs.clear();
            bodies.clear();
            objects.clear();
            object_order.clear();
            object_sizes.clear();
            cached_object_bytes = 0;
            receipt_index.clear();
            validated.clear();
            refused.clear();
            operation_claims.clear();
            identity_conflicts.clear();
            roots.clear();
            root_admissions.clear();
            line_names.reset();
            views.clear();
            projection_garbage.clear();
            projected_before_eviction += source.projected_files();
            source = kernel{};
            cached_payload_files.clear();
            cached_payload_bytes = 0;
            policies.clear();
            pending_objects.clear();
            new_candidates.clear();
            completed_objects.clear();
            refused_objects.clear();
            remaining_dependencies.clear();
            waiting_dependencies.clear();
            dirty_status.clear();
            dirty_claims.clear();
            dirty_receipts.clear();
            dirty_conflicts.clear();
            dirty_roots.clear();
            dirty_cursors.clear();
            checkpoint_cursors.clear();
            snapshot_watermark = 0;
            snapshot_events = 0;
            state_snapshot.clear();
            loaded_records = 0;
            object_count = object_watermark = archive_bytes = kernel_block = 0;
            kernel_tail.clear();
            snapshot_ready = false;
            kernel_loaded = status_loaded = pending_loaded = true;
            pages->adopt({});
            bool restored = !audit && restore_snapshot();
            std::uint64_t latest = 0;
            {
                statement q{*db, "SELECT COALESCE(MAX(rowid),0) FROM objects"};
                q.next();
                latest = q.number(0);
            }
            if (!restored || (!read_only && latest > snapshot_watermark)) {
                ensure_kernel();
                ensure_status();
                statement rejected{*db, "SELECT id,reason FROM rejected"};
                while (rejected.next()) {
                    auto status = codec::decode(rejected.data(1));
                    refused[rejected.data(0)] = {status.at("code").text(), status.at("reason").text()};
                    dirty_status.insert(rejected.data(0));
                }
                load_objects(snapshot_watermark);
                if (!project.empty() && has_object(project)) {
                    validate_available();
                    refresh_policies();
                }
                statement q{*db, "SELECT name,data FROM roots ORDER BY name"};
                while (q.next()) {
                    auto data = codec::decode(q.data(1));
                    auto name = q.data(0);
                    roots[name] = string_list(data.at("frontier"));
                    if (!std::holds_alternative<std::monostate>(data.at("admission").data))
                        root_admissions[name] = data.at("admission").text();
                    dirty_roots.insert(name);
                }
                statement totals{*db, "SELECT COALESCE(MAX(rowid),0),COUNT(*),COALESCE(SUM(length(data)),0) FROM objects"};
                totals.next();
                object_watermark = totals.number(0);
                object_count = totals.number(1);
                archive_bytes = totals.number(2);
                replayed_events = source.size() - snapshot_events;
            } else
                replayed_events = 0;
            snapshot.commit();
            data_version = observed_version;
            archive_audited = audit;
            dirty = false;
        }
        void refresh_external() {
            if (dirty) {
                reload();
                return;
            }
            statement snapshot{*db, "SELECT data FROM metadata WHERE name='state_snapshot'"};
            bool changed = snapshot.next() ? digest_of(snapshot.data(0)) != observed_snapshot : !observed_snapshot.empty();
            statement objects{*db, "SELECT COALESCE(MAX(rowid),0) FROM objects"};
            objects.next();
            if (changed || (!read_only && objects.number(0) != object_watermark))
                reload();
        }
        auto version(const std::vector<std::string>& frontier) -> std::string {
            if (frontier.empty())
                return digest_of(codec::encode(value::map{{"frontier", strings({})}, {"project_id", project}}));
            if (auto name = matching_view(frontier))
                return views.at(*name).version;
            ensure_kernel();
            return digest_of(codec::encode(value::map{{"frontier", strings(source.frontier(frontier))}, {"project_id", project}}));
        }
        void root_at(const std::string& name, const std::vector<std::string>& frontier, std::string admission = {}) {
            {
                statement revision{*db, "INSERT OR IGNORE INTO revisions(id,data) VALUES(?1,?2)"};
                revision.text(1, version(frontier));
                revision.bind(2, codec::encode(value::map{{"frontier", strings(frontier)}}));
                revision.next();
            }
            auto data = codec::encode(value::map{{"admission", admission.empty() ? value{} : value{admission}}, {"frontier", strings(frontier)}});
            statement q{*db, "INSERT INTO roots(name,data) VALUES(?1,?2) ON CONFLICT(name) DO UPDATE SET data=excluded.data"};
            q.text(1, name);
            q.bind(2, data);
            q.next();
            roots[name] = frontier;
            dirty_roots.insert(name);
            if (!admission.empty())
                root_admissions[name] = std::move(admission);
        }
        auto identity() -> node_reply {
            ensure_status();
            node_reply out;
            out.project = project;
            out.node = node.id();
            out.author = author.id();
            out.policies = policies;
            out.state_snapshot = state_snapshot;
            out.loaded_records = loaded_records;
            out.replayed_events = replayed_events;
            out.snapshot_events = snapshot_events;
            for (auto& [_, id] : identity_conflicts)
                out.identity_conflicts.push_back(id);
            for (auto& [id, _] : refused)
                out.refused.push_back(id);
            return out;
        }
        auto view(const std::vector<std::string>& frontier, bool include_text = true, std::string_view path = {}) -> node_reply {
            if (frontier.empty()) {
                auto out = identity();
                out.version = version(frontier);
                out.source_digest = digest_of(codec::encode(value::map{{"conflicts", strings({})}, {"files", ""}}));
                out.worldlines = worldlines();
                return out;
            }
            if (auto name = matching_view(frontier))
                return ready_reply(*name, include_text, path);
            ensure_kernel();
            auto out = identity();
            out.frontier = source.frontier(frontier);
            out.version = version(frontier);
            auto state_owner = source.projection(frontier);
            const auto& state = *state_owner;
            out.source_digest = state.digest;
            out.conflicts = state.conflicts;
            auto append = [&](const std::string& id, const std::shared_ptr<const source_file>& f) {
                out.files.push_back({id, f->path, f->kind, f->digest, include_text ? (f->kind == "binary" ? hex(f->content()) : f->content()) : "", f->bytes(),
                                     f->executable, f->kind == "binary" ? "hex" : "utf8"});
            };
            if (path.empty()) {
                for (auto& [id, file] : state.files)
                    append(id, file);
            } else if (auto owners = state.paths.find(path); owners != state.paths.end())
                for (auto& id : *owners->second)
                    append(id, state.files.find(id)->second);
            out.worldlines = worldlines();
            return out;
        }
        auto compare(const node_request& in, const std::vector<std::string>& heads) -> node_reply {
            within_line(root_line(in.worldline), in.expected);
            auto current_owner = source.projection(heads);
            auto out = identity();
            out.base_version = version(in.expected);
            out.version = version(heads);
            out.frontier = source.frontier(heads);
            out.source_digest = current_owner->digest;
            out.conflicts = current_owner->conflicts;
            out.changes = differences(in.expected, heads);
            return out;
        }
        // The files that differ between two states, with their full contents on both sides.
        // A budget for shown contents: text up to `per_side` bytes a side within `total`, cut between characters; binary content not at all.
        struct shown {
            std::uint64_t per_side, total;
        };
        auto differences(const std::vector<std::string>& before_heads, const std::vector<std::string>& after_heads, std::optional<shown> budget = std::nullopt)
            -> std::vector<source_difference> {
            auto old_owner = source.projection(before_heads), current_owner = source.projection(after_heads);
            const auto& old = *old_owner;
            const auto& current = *current_owner;
            std::vector<source_difference> out;
            auto make = [&budget](const source_file& f) -> file_view {
                std::string text;
                if (!budget)
                    text = f.kind == "binary" ? hex(f.content()) : f.content();
                else if (f.kind == "text") {
                    const auto& all = f.content();
                    auto most = std::min<std::uint64_t>({budget->per_side, budget->total, all.size()});
                    while (most > 0 && most < all.size() && (static_cast<unsigned char>(all[most]) & 0xC0) == 0x80)
                        --most;
                    text = all.substr(0, most);
                    budget->total -= most;
                }
                return {f.id, f.path, f.kind, f.digest, std::move(text), f.bytes(), f.executable, f.kind == "binary" ? "hex" : "utf8"};
            };
            old.files.difference(current.files, [&](const std::string&, const auto* before, const auto* after) {
                if (before && after && (*before)->path == (*after)->path && (*before)->digest == (*after)->digest &&
                    (*before)->executable == (*after)->executable && (*before)->kind == (*after)->kind)
                    return;
                source_difference change;
                change.type = !before ? "added" : !after ? "removed" : (*before)->path != (*after)->path ? "moved" : "modified";
                if (before)
                    change.before = make(**before);
                if (after)
                    change.after = make(**after);
                out.push_back(std::move(change));
            });
            return out;
        }
        auto history(const node_request& in, const std::vector<std::string>& heads) -> node_reply {
            if (in.limit == 0 || in.limit > 1000)
                fail(nodes::code::resource_limit, "history page must contain 1 through 1000 records");
            auto selected = source.reachable(heads);
            std::ranges::sort(selected, [&](auto& a, auto& b) {
                auto& x = get(a);
                auto& y = get(b);
                return std::tuple{x.at("lamport").number(), x.at("author").text(), x.at("operation").text(), a} <
                       std::tuple{y.at("lamport").number(), y.at("author").text(), y.at("operation").text(), b};
            });
            if (!in.target.empty()) {
                auto cursor = std::ranges::find(selected, in.target);
                if (cursor == selected.end())
                    fail(nodes::code::invalid_record, "history cursor is outside selected version");
                selected.erase(selected.begin(), cursor + 1);
            }
            auto out = identity();
            out.frontier = source.frontier(heads);
            out.version = version(heads);
            out.truncated = selected.size() > in.limit;
            if (selected.size() > in.limit)
                selected.resize(in.limit);
            std::map<std::string, std::vector<std::string>> receipts;
            for (auto& event : selected) {
                auto& batch = get(event, "batch");
                for (auto& id : this->receipts(batch.at("author").text() + ":" + batch.at("operation").text())) {
                    auto& b = get(id, "receipt");
                    receipts[get(b.at("admission").text(), "admission").at("attempt").text()].push_back(id);
                }
            }
            for (auto& id : selected)
                out.history.push_back(entry_of(id, receipts[get(id, "batch").at("attempt").text()]));
            return out;
        }
        // The history entry of batch `id`, given the receipts that accepted its attempt.
        auto entry_of(const std::string& id, std::vector<std::string> proof) -> history_entry {
            auto& b = get(id, "batch");
            auto& attempt = get(b.at("attempt").text(), "attempt");
            auto& intent = get(b.at("intent").text(), "intent");
            std::string outcome = identity_conflicts.contains(b.at("author").text() + ":" + b.at("operation").text())
                                      ? std::string{nodes::code::identity_conflict}
                                  : proof.empty() ? "unconfirmed"
                                                  : "accepted";
            // Only this node's own signed receipt vouches for the caller it recorded.
            bool local = std::ranges::any_of(proof, [&](auto& receipt) { return is_validated(receipt) && get(receipt).at("issuer").text() == node.id(); });
            auto actor = local ? request_input(attempt.at("input").text()).at("actor").text() : std::string{};
            return {id,
                    shown_operation(attempt),
                    b.at("author").text(),
                    std::move(actor),
                    attempt.at("executor").text(),
                    attempt.at("tool").text(),
                    intent.at("summary").text(),
                    get(attempt.at("result").text(), "blob").at("data").raw(),
                    outcome,
                    b.at("policy").text(),
                    b.at("line").text(),
                    string_list(b.at("parents")),
                    string_list(b.at("dependencies")),
                    std::move(proof),
                    b.at("lamport").number()};
        }
        // One batch as a contribution: its history entry, what it changed against its parents, and the worldlines that contain it.
        auto contribution(const node_request& in) -> node_reply {
            if (in.target.empty())
                fail(nodes::code::invalid_record, "a contribution names a history entry");
            ensure_kernel();
            if (!has_object(in.target) || !is_validated(in.target) || get(in.target).at("kind").text() != "batch")
                fail(nodes::code::missing_objects, "no batch has this history entry");
            auto& b = get(in.target, "batch");
            auto parents = string_list(b.at("parents"));
            auto after = parents;
            after.push_back(in.target);
            after = source.frontier(std::move(after));
            std::vector<std::string> proof;
            for (auto& id : receipts(b.at("author").text() + ":" + b.at("operation").text())) {
                auto& r = get(id, "receipt");
                if (get(r.at("admission").text(), "admission").at("attempt").text() == b.at("attempt").text())
                    proof.push_back(id);
            }
            auto out = identity();
            out.history.push_back(entry_of(in.target, std::move(proof)));
            out.base_version = version(parents);
            out.version = version(after);
            // A shown text shorter than its file's size is cut.
            out.changes = differences(parents, after, shown{in.max_bytes, 16u << 20});
            for (auto& [name, frontier] : worldlines())
                if (out.lines.size() < 1000 && source.includes(frontier, in.target))
                    out.lines.push_back({.name = name, .version = version(frontier)});
            return out;
        }
        auto policy(std::string_view worldline = {}) -> std::string {
            if (policies.empty())
                fail(nodes::code::missing_objects, "project policy is unavailable");
            if (policies.size() != 1 && (worldline.empty() || std::ranges::any_of(policies, [&](auto& id) { return protected_target(get(id), worldline); })))
                fail("policy_conflict", "protected target requires a joined-parent resolution");
            return policies.front();
        }
        auto target(const std::string& name) -> value { return value::map{{"name", name}, {"owner", node.id()}, {"reviewer", author.id()}}; }
        auto unresolved_identities(const std::vector<std::string>& heads, const std::string& p, const std::string& worldline) -> std::vector<std::string> {
            if (identity_conflicts.empty())
                return {};
            std::set<std::string> blocked;
            auto& permissions = get(p);
            std::vector<std::string> preserved;
            if (permissions.at("kind").text() == "policy")
                preserved = string_list(permissions.at("settled"));
            for (auto conflict = identity_conflicts.begin(); conflict != identity_conflicts.end(); ++conflict)
                for (auto& event : source.identity_events(conflict->first)) {
                    if (!source.includes(heads, event))
                        continue;
                    bool settled = false;
                    for (auto& admission : preserved)
                        if (source.includes(string_list(get(admission, "admission").at("frontier")), event)) {
                            settled = true;
                            break;
                        }
                    if (!settled)
                        for (auto& id : object_ids("review"))
                            if (is_validated(id)) {
                                auto& b = get(id);
                                if (b.at("policy").text() != p || b.at("target").at("name").text() != worldline ||
                                    b.at("target").at("owner").text() != node.id())
                                    continue;
                                auto choices = string_list(b.at("identities"));
                                if (std::ranges::find(choices, conflict->second) != choices.end() && source.includes(string_list(b.at("frontier")), event))
                                    settled = true;
                            }
                    if (!settled)
                        blocked.insert(conflict->second);
                }
            return {blocked.begin(), blocked.end()};
        }
        auto receipt(const std::string& id, bool replayed) -> node_reply {
            if (!is_validated(id))
                fail(nodes::code::outcome_unknown, "original receipt has not been verified");
            auto& r = get(id, "receipt");
            auto& a = get(r.at("admission").text(), "admission");
            auto out = identity();
            out.frontier = string_list(a.at("frontier"));
            out.version = version(out.frontier);
            if (auto name = matching_view(out.frontier)) {
                out.source_digest = views.at(*name).digest;
                out.conflicts = views.at(*name).conflicts;
            } else {
                ensure_kernel();
                auto state_owner = source.projection(out.frontier);
                const auto& state = *state_owner;
                out.source_digest = state.digest;
                out.conflicts = state.conflicts;
            }
            out.receipt = id;
            out.issuer = r.at("issuer").text();
            out.worldline = r.at("target").at("name").text();
            out.receipt_status = out.issuer == node.id() ? "local_acceptance" : "peer_received";
            out.author = r.at("author").text();
            out.operation = shown_operation(get(a.at("attempt").text(), "attempt"));
            out.result = get(r.at("result").text(), "blob").at("data").raw();
            out.replayed = replayed;
            return out;
        }
        void binding(const std::string& operation, const std::string& digest, const std::string& result) {
            statement q{*db, "INSERT INTO bindings(author,operation,digest,receipt) VALUES(?1,?2,?3,?4) ON CONFLICT(author,operation) DO UPDATE SET "
                             "receipt=excluded.receipt WHERE digest=excluded.digest"};
            q.text(1, author.id());
            q.text(2, operation);
            q.text(3, digest);
            q.text(4, result);
            q.next();
        }
        // The receipt of `operation` as this node executed it for its author, if it ran.
        auto executed(const std::string& operation) -> std::optional<std::string> {
            statement q{*db, "SELECT receipt FROM bindings WHERE author=?1 AND operation=?2"};
            q.text(1, author.id());
            q.text(2, operation);
            if (q.next() && !q.data(0).empty())
                return q.data(0);
            for (auto& id : receipts(author.id() + ":" + operation))
                if (is_validated(id)) {
                    auto& b = get(id);
                    if (b.at("operation").text() == operation && b.at("author").text() == author.id() && b.at("issuer").text() == node.id())
                        return id;
                }
            return {};
        }
        auto retry(const node_request& in, const std::string& digest) -> std::optional<node_reply> {
            statement q{*db, "SELECT digest,receipt FROM bindings WHERE author=?1 AND operation=?2"};
            q.text(1, author.id());
            q.text(2, in.operation);
            if (q.next()) {
                if (q.data(0) != digest && in.owner == node.id())
                    fail(nodes::code::identity_conflict, "operation ID has different input");
                if (q.data(0) == digest && !q.data(1).empty())
                    return receipt(q.data(1), true);
            }
            bool different = false;
            auto& indexed = receipts(author.id() + ":" + in.operation);
            for (auto& id : indexed) {
                auto& b = get(id, "receipt");
                if (b.at("request_digest").text() == digest && b.at("target").at("owner").text() == in.owner)
                    return receipt(id, true);
                different = true;
            }
            if (different)
                fail(nodes::code::identity_conflict, "retained receipt has different canonical input");
            return {};
        }
        auto accept(const node_request& in, const std::string& line, const std::string& request_digest, const std::string& effect_digest,
                    const std::vector<std::string>& prior, const std::vector<std::string>& frontier, const std::string& result_id,
                    const std::string& attempt_id, bool override_author = false) -> node_reply {
            auto p = policy(in.worldline);
            auto a = base_body("admission", project);
            a["issuer"] = node.id();
            a["operation"] = in.operation;
            a["target"] = target(in.worldline);
            a["policy"] = p;
            auto identities = protected_target(get(p), in.worldline) ? unresolved_identities(frontier, p, in.worldline) : std::vector<std::string>{};
            if (!identities.empty() &&
                (!override_author || std::ranges::any_of(identities, [&](auto& id) { return std::ranges::find(in.decisions, id) == in.decisions.end(); }))) {
                error e{nodes::code::identity_conflict, "target admission requires a maintainer choice of a fixed source frontier"};
                e.conflicts = std::move(identities);
                throw e;
            }
            if (override_author)
                for (auto& id : in.decisions)
                    if (std::ranges::find(identities, id) == identities.end())
                        fail(nodes::code::identity_conflict, "admission decision is outside the observed identity conflicts");
            a["prior"] = strings(prior);
            a["frontier"] = strings(frontier);
            a["result"] = result_id;
            a["author_override"] = override_author;
            a["attempt"] = attempt_id;
            a["line"] = line;
            a["previous"] = value{};
            std::vector<std::string> accepted;
            if (auto previous = root_admissions.find(in.worldline); previous != root_admissions.end()) {
                a["previous"] = previous->second;
                if (get(previous->second, "admission").at("policy").text() == p)
                    accepted.push_back(previous->second);
            }
            a["accepted_dependencies"] = strings(accepted);
            a["review"] = value{};
            if (override_author) {
                auto review = base_body("review", project);
                review["author"] = author.id();
                review["frontier"] = strings(frontier);
                review["target"] = target(in.worldline);
                review["policy"] = p;
                review["identities"] = strings(vector_set(in.decisions));
                a["review"] = put(std::move(review), author);
            }
            auto admission = put(std::move(a), node);
            validate_object(admission);
            auto r = base_body("receipt", project);
            r["issuer"] = node.id();
            r["author"] = author.id();
            r["operation"] = in.operation;
            r["target"] = target(in.worldline);
            r["admission"] = admission;
            r["version"] = version(frontier);
            r["result"] = result_id;
            r["request_digest"] = request_digest;
            r["effect_digest"] = effect_digest;
            auto id = put(std::move(r), node);
            validate_object(id);
            binding(in.operation, request_digest, id);
            root_at(in.worldline, frontier, admission);
            return receipt(id, false);
        }
        static void interrupt_at(std::string_view boundary) {
            if (auto value = std::getenv("NODE_FAULT"); value && boundary == value)
                std::_Exit(86);
        }
        auto effects(const node_request& in, const std::vector<std::string>& heads, const std::string& creating_author = {}) -> value::array {
            auto state_owner = source.projection(heads);
            const auto& state = *state_owner;
            value::array effects;
            auto property = [&](std::string file, std::string type, value v) {
                effects.emplace_back(value::map{{"file", std::move(file)}, {"type", std::move(type)}, {"value", std::move(v)}});
            };
            auto insert = [&](const std::string& file, element left, element right, const std::string& text) {
                if (!text.empty())
                    effects.emplace_back(value::map{{"file", file}, {"left", left.encoded()}, {"right", right.encoded()}, {"text", text}, {"type", "insert"}});
            };
            auto erase = [&](const source_file& file, std::uint64_t offset, std::uint64_t count) {
                value::array selected;
                for (auto& f : file.span(offset, count))
                    selected.push_back(value::map{{"count", f.count}, {"first", f.first}, {"run", f.run}});
                if (selected.size() > codec::collection_limit)
                    fail(nodes::code::resource_limit, "deletion target collection limit");
                if (!selected.empty())
                    effects.emplace_back(value::map{{"file", file.id}, {"targets", std::move(selected)}, {"type", "erase"}});
            };
            if (in.action == "reverse" || in.action == "redo") {
                if (!source.includes(heads, in.target))
                    fail(nodes::code::invalid_record, "compensation target is outside selected history");
                auto selected = source.effect_ids(in.target);
                if (selected.empty())
                    fail(nodes::code::invalid_record, "batch has no reversible effects");
                bool desired = in.action == "redo";
                for (auto& id : selected)
                    if (source.active(heads, id) == desired)
                        fail(nodes::code::source_conflict, "compensation precondition does not hold");
                if (!desired) {
                    auto dependencies = source.compensation_dependencies(heads, selected);
                    if (std::ranges::any_of(dependencies, [&](auto& id) { return std::ranges::find(in.decisions, id) == in.decisions.end(); })) {
                        error e{"dependency_conflict", "reversal requires a fixed-version dependency decision"};
                        e.conflicts = std::move(dependencies);
                        throw e;
                    }
                }
                effects.emplace_back(value::map{{"selections", strings(selected)}, {"type", "activate"}, {"value", desired}});
            }
            if (!in.decisions.empty()) {
                auto conflicts = state.conflicts;
                if ((in.action == "reverse" || in.action == "resolve") && !in.target.empty()) {
                    auto forecast = source.compensation_dependencies(heads, source.effect_ids(in.target));
                    conflicts.insert(conflicts.end(), forecast.begin(), forecast.end());
                }
                for (auto& id : in.decisions)
                    if (std::ranges::find(conflicts, id) == conflicts.end())
                        fail(nodes::code::source_conflict, "resolution names an unobserved conflict");
                effects.emplace_back(value::map{{"selections", strings(vector_set(in.decisions))}, {"type", "resolve"}, {"value", true}});
            }
            if (in.action == "resolve" && !in.target.empty()) {
                if (!source.includes(heads, in.target))
                    fail(nodes::code::invalid_record, "compensation target is outside selected history");
                if (!in.activation)
                    fail(nodes::code::invalid_record, "activation resolution requires an explicit choice");
                effects.emplace_back(value::map{{"selections", strings(source.effect_ids(in.target))}, {"type", "activate"}, {"value", *in.activation}});
            }
            std::size_t created = 0;
            std::set<std::string> touched;
            // A file is placed only on a path that is free once the batch is done: one this batch empties, or one nobody holds.
            std::set<std::string> freed, placed;
            for (auto& e : in.edits) {
                if (!e.document.empty() && e.kind != "edit" && e.kind != "write" && e.kind != "binary")
                    fail(nodes::code::invalid_record, "only a content change names a file identity");
                if (e.kind == "remove" || e.kind == "move")
                    freed.insert(e.path);
                else if (e.kind == "directory_move")
                    for (auto it = state.paths.lower_bound(e.path + "/"); it != state.paths.end() && it->first.starts_with(e.path + "/"); ++it)
                        freed.insert(it->first);
            }
            auto claim = [&](const std::string& path) {
                auto held = state.paths.find(path);
                if ((held != state.paths.end() && !held->second->empty() && !freed.contains(path)) || !placed.insert(path).second)
                    fail(nodes::code::source_conflict, path + " already exists");
            };
            for (auto& e : in.edits) {
                checked_path(e.path);
                if (!touched.insert(e.path).second)
                    fail(nodes::code::invalid_record, "multiple edits of one path require separate observed batches");
                if (e.kind == "create" || e.kind == "binary_create") {
                    claim(e.path);
                    auto file = (creating_author.empty() ? author.id() : creating_author) + ":" + in.operation + ":file:" + std::to_string(created++);
                    property(file, "path", e.path);
                    property(file, "presence", true);
                    property(file, "mode", e.executable);
                    if (e.kind == "binary_create")
                        property(file, "binary", digest_of(unhex(e.text)));
                    else
                        insert(file, {}, {}, e.text);
                    continue;
                }
                if (e.kind == "directory_move") {
                    checked_path(e.target);
                    bool found = false;
                    std::vector<std::pair<std::string, std::string>> moved;
                    auto prefix = e.path + "/";
                    for (auto it = state.paths.lower_bound(prefix); it != state.paths.end() && it->first.starts_with(prefix); ++it)
                        for (auto& id : *it->second)
                            moved.emplace_back(id, it->first);
                    std::ranges::sort(moved);
                    for (auto& [id, path] : moved) {
                        auto destination = e.target + path.substr(e.path.size());
                        claim(destination);
                        property(id, "path", destination);
                        found = true;
                    }
                    if (!found)
                        fail(nodes::code::invalid_record, "directory has no source files");
                    continue;
                }
                const source_file* selected = nullptr;
                if (!e.document.empty()) {
                    auto it = state.files.find(e.document);
                    if (it != state.files.end())
                        selected = it->second.get();
                    else {
                        auto hidden = state.hidden.find(e.document);
                        if (hidden == state.hidden.end())
                            fail(nodes::code::invalid_record, "unknown selected file identity");
                        selected = hidden->second.get();
                    }
                }
                auto& file = selected ? *selected : state.path(e.path);
                if (e.kind == "move") {
                    checked_path(e.target);
                    // A move never replaces a file; a concurrent move onto one path stays a recorded path conflict.
                    claim(e.target);
                    property(file.id, "path", e.target);
                } else if (e.kind == "remove")
                    property(file.id, "presence", false);
                else if (e.kind == "mode")
                    property(file.id, "mode", e.executable);
                else if (e.kind == "binary") {
                    if (file.kind != "binary")
                        fail(nodes::code::invalid_record, "binary replacement requires a binary document");
                    property(file.id, "binary", digest_of(unhex(e.text)));
                } else if (e.kind == "edit" || e.kind == "write") {
                    if (file.kind != "text")
                        fail(nodes::code::invalid_record, "text edit requires a text document");
                    auto offset = e.kind == "write" ? 0 : e.offset, count = e.kind == "write" ? file.size() : e.count;
                    if (offset > file.size() || count > file.size() - offset)
                        fail(nodes::code::invalid_record, "text edit outside observed source");
                    auto left = offset ? file.at(offset - 1) : element{};
                    auto right = offset + count < file.size() ? file.at(offset + count) : element{};
                    erase(file, offset, count);
                    insert(file.id, left, right, e.text);
                } else
                    fail(nodes::code::invalid_record, "unsupported source edit kind");
            }
            return effects;
        }
        auto publish(const node_request& in) -> node_reply {
            auto declared_tool = tool_name(in.tool, in.action);
            checked_name(in.worldline);
            if (!content::is_hash(in.operation))
                fail(nodes::code::invalid_record, "operation ID must be 32-byte lowercase hex");
            if (!in.genesis_nonce.empty() || !in.authors.empty() || !in.nodes.empty() || !in.policies.empty() || !in.settled.empty() || !in.want.empty() ||
                !in.parts.empty() || !in.configuration.empty() || !in.environment.empty() || in.generation || !in.spans.empty() || in.force ||
                (!in.scopes.empty() &&
                 (in.action != "apply" || in.scopes != std::vector<std::string>{std::string{nodes::global_scope}} || in.holder.empty())) ||
                !in.revision.empty() || !in.snapshot.empty() || in.protected_worldlines != std::vector<std::string>{"main"} || in.max_bytes != 1024 * 1024 ||
                in.limit != 100)
                fail(nodes::code::invalid_record, "source publication contains fields for another operation");
            if (in.activation && in.action != "resolve")
                fail(nodes::code::invalid_record, "activation choices belong to conflict resolution");
            if (!in.source.empty() && in.action != "fork" && in.action != "integrate" && in.action != "admit")
                fail(nodes::code::invalid_record, "source selection belongs to fork or integration");
            if ((in.action == "fork" || in.action == "integrate" || in.action == "admit") &&
                (!in.edits.empty() || (in.action != "admit" && !in.decisions.empty()) || !in.dependencies.empty() || !in.target.empty()))
                fail(nodes::code::invalid_record, "selection operations cannot contain source effects");
            if (!in.target.empty() && in.action != "reverse" && in.action != "redo" && in.action != "resolve")
                fail(nodes::code::invalid_record, "unexpected compensation target");
            if (in.action == "resolve" && !in.target.empty() && in.decisions.empty())
                fail(nodes::code::source_conflict, "activation resolution requires observed conflict decisions");
            auto request_digest = digest_of(codec::encode(request_body(in, project)));
            if (auto result = retry(in, request_digest))
                return *result;
            if (!content::is_hash(in.owner))
                fail(nodes::code::invalid_record, "source publication requires an explicit target node identity");
            if (in.owner != node.id())
                fail(nodes::code::outcome_unknown, "original target is another node and its verified result is unavailable");
            interrupt_at("before_transaction");
            transaction tx{*db};
            refresh_external();
            if (auto result = retry(in, request_digest))
                return *result;
            ensure_kernel();
            ensure_status();
            dirty = true;
            auto found = roots.find(in.worldline);
            if (in.action == "fork" && (found != roots.end() || in.worldline == "main" || !registered_lines(in.worldline).empty()))
                fail(nodes::code::source_conflict, "worldline already exists");
            std::vector<std::string> prior = found == roots.end() ? std::vector<std::string>{} : found->second;
            // Writers in one worldline may observe an older state of it; selections must observe the current root.
            if (selection(in.action) ? in.expected != prior : in.expected != source.frontier(in.expected) || std::ranges::any_of(in.expected, [&](auto& event) {
                                                                  return !source.includes(prior, event);
                                                              }))
                fail("stale_frontier", "expected source frontier is not part of this worldline");
            auto p = policy(in.worldline);
            auto& permissions = get(p);
            bool protected_line = protected_target(permissions, in.worldline);
            if (protected_line && !allowed(permissions, "nodes", node.id()))
                fail(nodes::code::policy_denied, "local node cannot accept source");
            if (protected_line && !allowed(permissions, "authors", author.id()))
                fail(nodes::code::policy_denied, "local author cannot publish source");
            auto heads = prior;
            std::string line;
            if (in.action == "fork") {
                heads = source.frontier(in.source);
                std::set<std::string> lines;
                for (auto& event : heads)
                    lines.insert(line_of(event));
                if (lines.size() > 1)
                    fail("worldline_boundary", "fork source spans several worldlines");
                auto created = base_body("line", project);
                created["base"] = strings(vector_set(heads));
                created["creator"] = author.id();
                created["name"] = in.worldline;
                created["operation"] = in.operation;
                line = put(std::move(created), author);
                validate_object(line);
            } else {
                line = root_line(in.worldline);
                if (in.action == "integrate" || in.action == "admit") {
                    heads.insert(heads.end(), in.source.begin(), in.source.end());
                    heads = source.frontier(std::move(heads));
                    observes_line(line, heads);
                    if (!in.holder.empty())
                        check_locks(in, prior);
                } else {
                    if (found == roots.end() && in.worldline != "main")
                        fail(nodes::code::invalid_record, "join this worldline by integration first");
                    observes_line(line, in.expected);
                    if (in.expected.empty() && !prior.empty())
                        fail("stale_frontier", "an edit must observe a state of the worldline");
                    heads = in.expected;
                    if (!in.holder.empty())
                        check_locks(in, heads);
                }
            }
            if (in.action == "admit" && !allowed(permissions, "maintainers", author.id()))
                fail(nodes::code::policy_denied, "explicit admission requires maintainer review");
            auto conflicts = source.projection(heads)->conflicts;
            if (std::ranges::any_of(conflicts, [](auto& id) { return id.starts_with("identity:"); }) && in.action != "integrate") {
                error e{nodes::code::identity_conflict, "select a coherent source frontier before publishing"};
                e.conflicts = std::move(conflicts);
                throw e;
            }
            auto result_id = blob(in.result);
            std::string effect_digest;
            auto intent = base_body("intent", project);
            intent["author"] = author.id();
            intent["parents"] = value::array{};
            intent["messages"] = value::array{};
            intent["summary"] = in.intent;
            auto intent_id = put(std::move(intent), author);
            auto input_id = blob(codec::encode(request_body(in, project)));
            auto attempt = base_body("attempt", project);
            attempt["author"] = author.id();
            attempt["executor"] = node.id();
            attempt["intent"] = intent_id;
            attempt["operation"] = in.operation;
            attempt["input"] = input_id;
            attempt["observed"] = strings(heads);
            attempt["outcome"] = "accepted";
            attempt["result"] = result_id;
            attempt["tool"] = declared_tool;
            auto attempt_id = put(std::move(attempt), author);
            if (prior.empty() && heads.empty() && in.action != "fork" && lines().active.contains(line))
                fail("stale_frontier", "this worldline has accepted source; integrate it before editing");
            if (in.action != "fork" && in.action != "integrate" && in.action != "admit") {
                std::vector<std::string> required{intent_id, attempt_id, input_id, result_id};
                for (auto& edit : in.edits)
                    if (edit.kind == "binary" || edit.kind == "binary_create")
                        required.push_back(blob(unhex(edit.text)));
                auto changes = effects(in, heads);
                effect_digest = effects_digest(changes);
                auto b = base_body("batch", project);
                b["author"] = author.id();
                b["operation"] = in.operation;
                b["parents"] = strings(heads);
                b["expected"] = strings(heads);
                b["lamport"] = source.clock(heads);
                b["line"] = line;
                b["policy"] = p;
                b["request_digest"] = request_digest;
                b["effect_digest"] = effect_digest;
                b["intent"] = intent_id;
                b["attempt"] = attempt_id;
                b["dependencies"] = strings(source.dependencies(heads, changes, in.dependencies));
                b["effects"] = std::move(changes);
                b["required"] = strings(vector_set(std::move(required)));
                auto event = put(std::move(b), author);
                validate_object(event);
                heads = prior;
                heads.push_back(event);
                heads = source.frontier(std::move(heads));
                auto projected = source.projection(heads);
                check_limits(in, *source.projection(prior), *projected);
                if ((in.action == "reverse" || in.action == "redo") &&
                    std::ranges::any_of(projected->conflicts, [](auto& c) { return c.starts_with("dependency:"); }))
                    fail("dependency_conflict", "reversal requires a recorded dependency decision");
            }
            // A batch that takes "::" for itself held nothing while its caller read the source, so what it creates must still be free now.
            if (!in.scopes.empty()) {
                auto now = source.projection(prior);
                for (auto& e : in.edits)
                    for (const auto& path :
                         {e.kind == "create" || e.kind == "binary_create" ? e.path : std::string{}, e.kind == "move" ? e.target : std::string{}})
                        if (auto held = now->paths.find(path); !path.empty() && held != now->paths.end() && !held->second->empty())
                            fail(nodes::code::source_conflict, path + " was created since this batch read the source");
            }
            // A fork copies a state that exists; an integration or admission joins states, and that may grow the target.
            if (in.action == "integrate" || in.action == "admit")
                check_limits(in, *source.projection(prior), *source.projection(heads));
            interrupt_at("after_objects");
            interrupt_at("before_acceptance");
            if (effect_digest.empty())
                effect_digest = digest_of(codec::encode(value::map{{"frontier", strings(heads)}, {"prior", strings(prior)}}));
            auto out = accept(in, line, request_digest, effect_digest, prior, heads, result_id, attempt_id, in.action == "admit");
            save_snapshot();
            out.state_snapshot = state_snapshot;
            out.snapshot_events = snapshot_events;
            out.replayed_events = replayed_events;
            interrupt_at("before_commit");
            tx.commit();
            dirty = false;
            interrupt_at("after_commit");
            return out;
        }
        // Language data is derived from source, so it stays local to this node and is checked against the version it describes.
        auto selected_heads(const node_request& in) -> std::vector<std::string> {
            auto found = roots.find(in.worldline);
            if (found == roots.end() && in.worldline != "main")
                fail(nodes::code::invalid_record, "worldline not found");
            auto line = root_line(in.worldline);
            std::vector<std::string> heads = found == roots.end() ? std::vector<std::string>{} : found->second;
            if (!in.revision.empty()) {
                statement revision{*db, "SELECT data FROM revisions WHERE id=?1"};
                revision.text(1, in.revision);
                if (!revision.next())
                    fail(nodes::code::invalid_record, "revision not found");
                heads = string_list(codec::decode(revision.data(0)).at("frontier"));
                within_line(line, heads);
            }
            return heads;
        }
        static constexpr std::size_t semantic_records = 200000, semantic_text = 4096, semantic_bytes = 16 * 1024 * 1024, anchor_spans = 10000;
        template <class F> static void each_span(semantic::generation& g, F&& f) {
            for (auto& scope : g.scopes)
                f(scope.range);
            for (auto& symbol : g.symbols) {
                for (auto& span : symbol.declarations)
                    f(span);
                for (auto& span : symbol.definitions)
                    f(span);
            }
            for (auto& reference : g.references)
                f(reference.range);
            for (auto& diagnostic : g.diagnostics)
                f(diagnostic.range);
        }
        auto semantic_publish(const node_request& in) -> node_reply {
            if (!in.generation || !in.environment.empty() || !in.expected.empty() || !in.source.empty() || !in.target.empty() || !in.snapshot.empty() ||
                !in.revision.empty() || !in.spans.empty() || !in.edits.empty() || !in.operation.empty() || in.activation || !in.configuration.empty() ||
                !in.decisions.empty() || !in.dependencies.empty() || !in.intent.empty() || !in.result.empty() || !in.tool.empty() ||
                !in.genesis_nonce.empty() || !in.authors.empty() || !in.nodes.empty() || !in.policies.empty() || !in.settled.empty() || !in.want.empty() ||
                !in.parts.empty() || !in.caller_operation.empty() || in.force || in.capture || !in.scopes.empty() || in.max_bytes != 1024 * 1024 ||
                in.limit != 100 || in.protected_worldlines != std::vector<std::string>{"main"})
                fail(nodes::code::invalid_record, "semantic publication takes one generation and its worldline");
            auto generation = *in.generation;
            for (auto* field : {&generation.environment, &generation.service, &generation.service_version, &generation.version})
                if (field->empty() || field->size() > 256)
                    fail(nodes::code::invalid_record, "semantic generation identity is incomplete");
            std::size_t records = generation.covered.size() + generation.scopes.size() + generation.symbols.size() + generation.references.size() +
                                  generation.relations.size() + generation.diagnostics.size();
            std::size_t text = 0;
            // Counted as JSON text: a quote, backslash or control character takes up to six bytes.
            auto measure = [&](const std::string& value) {
                if (value.size() > semantic_text)
                    fail(nodes::code::resource_limit, "semantic text field exceeds 4 KiB");
                text += value.size() +
                        5 * static_cast<std::size_t>(std::ranges::count_if(value, [](unsigned char c) { return c < 0x20 || c == '"' || c == '\\'; }));
            };
            for (auto& scope : generation.scopes)
                for (auto* v : {&scope.id, &scope.parent, &scope.kind, &scope.symbol})
                    measure(*v);
            for (auto& symbol : generation.symbols) {
                for (auto* v : {&symbol.id, &symbol.name, &symbol.kind, &symbol.scope})
                    measure(*v);
                records += symbol.declarations.size() + symbol.definitions.size();
            }
            for (auto& reference : generation.references)
                for (auto* v : {&reference.target, &reference.kind})
                    measure(*v);
            for (auto& relation : generation.relations)
                for (auto* v : {&relation.source, &relation.target, &relation.kind})
                    measure(*v);
            for (auto& diagnostic : generation.diagnostics)
                for (auto* v : {&diagnostic.severity, &diagnostic.message})
                    measure(*v);
            for (auto& covered : generation.covered)
                for (auto* v : {&covered.document, &covered.digest})
                    measure(*v);
            each_span(generation, [&](semantic::span& span) {
                for (auto* v : {&span.document, &span.first.run, &span.last.run})
                    measure(*v);
            });
            if (records > semantic_records || text > semantic_bytes)
                fail(nodes::code::resource_limit, "semantic generation is too large");
            ensure_kernel();
            auto heads = selected_heads({.action = "inspect", .worldline = in.worldline, .revision = generation.version});
            auto state_owner = source.projection(heads);
            const auto& state = *state_owner;
            each_span(generation, [&](semantic::span& span) {
                span.place.reset();
                auto found = state.files.find(span.document);
                if (found == state.files.end() || found->second->kind != "text")
                    fail(nodes::code::invalid_record, "semantic span names no text document of this version");
                auto& file = *found->second;
                auto first = file.position({span.first.run, span.first.offset}), last = file.position({span.last.run, span.last.offset});
                if (!first || !last || *first > *last)
                    fail(nodes::code::invalid_record, "semantic span is outside its document");
            });
            std::map<std::string, std::string> parents;
            std::set<std::string> symbols;
            for (auto& scope : generation.scopes)
                if (scope.id.empty() || !parents.emplace(scope.id, scope.parent).second)
                    fail(nodes::code::invalid_record, "semantic scope identities must be unique");
            for (auto& symbol : generation.symbols)
                if (symbol.id.empty() || !symbols.insert(symbol.id).second)
                    fail(nodes::code::invalid_record, "semantic symbol identities must be unique");
            for (auto& scope : generation.scopes) {
                if (!scope.parent.empty() && !parents.contains(scope.parent))
                    fail(nodes::code::invalid_record, "semantic scope parent is unknown");
                if (!scope.symbol.empty() && !symbols.contains(scope.symbol))
                    fail(nodes::code::invalid_record, "semantic scope symbol is unknown");
            }
            // Each scope is visited once: a walk stops at a scope already known to reach the top.
            std::set<std::string> rooted;
            for (auto& [id, _] : parents) {
                std::vector<std::string> walk;
                std::set<std::string> on_walk;
                for (auto at = id; !at.empty() && !rooted.contains(at); at = parents.at(at)) {
                    if (!on_walk.insert(at).second)
                        fail(nodes::code::invalid_record, "semantic scopes form a cycle");
                    walk.push_back(at);
                }
                rooted.insert(walk.begin(), walk.end());
            }
            // A child scope lies inside its parent, so a lock on the parent holds the child.
            std::map<std::string, std::tuple<std::string, std::uint64_t, std::uint64_t>> extents;
            for (auto& scope : generation.scopes) {
                auto& file = *state.files.find(scope.range.document)->second;
                extents[scope.id] = {scope.range.document, *file.position({scope.range.first.run, scope.range.first.offset}),
                                     *file.position({scope.range.last.run, scope.range.last.offset})};
            }
            for (auto& scope : generation.scopes)
                if (!scope.parent.empty()) {
                    auto& [document, begin, end] = extents.at(scope.id);
                    auto& [outer, first, last] = extents.at(scope.parent);
                    if (document != outer || begin < first || end > last)
                        fail(nodes::code::invalid_record, "semantic scope lies outside its parent");
                }
            for (auto& symbol : generation.symbols)
                if (!symbol.scope.empty() && !parents.contains(symbol.scope))
                    fail(nodes::code::invalid_record, "semantic symbol scope is unknown");
            for (auto& reference : generation.references)
                if (!symbols.contains(reference.target))
                    fail(nodes::code::invalid_record, "semantic reference target is unknown");
            for (auto& relation : generation.relations)
                if (!symbols.contains(relation.source) || !symbols.contains(relation.target))
                    fail(nodes::code::invalid_record, "semantic relation names an unknown symbol");
            for (auto& covered : generation.covered) {
                auto found = state.files.find(covered.document);
                if (found == state.files.end() || found->second->digest != covered.digest)
                    fail(nodes::code::invalid_record, "semantic coverage does not match this version");
            }
            auto data = rfl::json::write(generation);
            if (data.size() > semantic_bytes)
                fail(nodes::code::resource_limit, "semantic generation is too large");
            transaction tx{*db};
            // A new publication for the same version replaces the old row, so it also becomes the newest.
            statement upsert{*db, "INSERT OR REPLACE INTO semantics(environment,service,version,frontier,data) VALUES(?1,?2,?3,?4,?5)"};
            upsert.text(1, generation.environment);
            upsert.text(2, generation.service);
            upsert.text(3, generation.version);
            upsert.bind(4, codec::encode(value::map{{"frontier", strings(heads)}}));
            upsert.text(5, data);
            upsert.next();
            tx.commit();
            auto out = identity();
            out.version = generation.version;
            out.frontier = heads;
            return out;
        }
        // Turns path and character offsets of the selected version into character identities, so providers can publish spans.
        auto anchor(const node_request& in) -> node_reply {
            if (in.spans.size() > anchor_spans)
                fail(nodes::code::resource_limit, "anchor request exceeds 10000 spans");
            ensure_kernel();
            auto heads = selected_heads(in);
            auto state_owner = source.projection(heads);
            const auto& state = *state_owner;
            auto out = identity();
            out.frontier = heads;
            out.version = version(heads);
            for (auto span : in.spans) {
                if (!span.place)
                    fail(nodes::code::invalid_record, "anchor span needs a place");
                auto& at = *span.place;
                auto& file = state.path(at.path);
                if (file.kind != "text" || at.begin >= at.end || at.end > file.size())
                    fail(nodes::code::invalid_record, "anchor span is outside its text document");
                auto first = file.at(at.begin), last = file.at(at.end - 1);
                span.document = file.id;
                span.first = {first.run, first.offset};
                span.last = {last.run, last.offset};
                out.spans.push_back(std::move(span));
            }
            return out;
        }
        // The generation for the selected version, or else the causally newest one the selected source contains.
        auto newest(const std::vector<std::string>& heads, const std::string& environment, const std::string& service)
            -> std::optional<std::pair<semantic::generation, std::size_t>> {
            statement rows{*db, "SELECT version,frontier,data FROM semantics WHERE environment=?1 AND (?2='' OR service=?2) ORDER BY seq DESC"};
            rows.text(1, environment);
            rows.text(2, service);
            auto exact = version(heads);
            std::optional<std::pair<std::vector<std::string>, std::string>> best;
            auto selected = source.within(heads);
            while (rows.next()) {
                auto frontier = string_list(codec::decode(rows.data(1)).at("frontier"));
                if (rows.data(0) == exact) {
                    best = {std::move(frontier), rows.data(2)};
                    break;
                }
                if ((frontier.empty() && !heads.empty()) || !std::ranges::all_of(frontier, selected))
                    continue;
                if (!best || (best->first != frontier && std::ranges::all_of(best->first, source.within(frontier))))
                    best = {std::move(frontier), rows.data(2)};
            }
            if (!best)
                return std::nullopt;
            auto parsed = rfl::json::read<semantic::generation>(best->second);
            if (!parsed)
                fail(nodes::code::storage_failure, "stored semantic generation is unreadable");
            return std::pair{std::move(*parsed), best->second.size()};
        }
        auto semantic_query(const node_request& in) -> node_reply {
            if (in.environment.empty())
                fail(nodes::code::invalid_record, "semantic query needs an environment");
            ensure_kernel();
            auto heads = selected_heads(in);
            auto out = identity();
            out.frontier = heads;
            out.version = version(heads);
            auto found = newest(heads, in.environment, in.target);
            if (!found)
                return out;
            auto& [generation, stored] = *found;
            auto state_owner = source.projection(heads);
            const auto& state = *state_owner;
            std::size_t placed = stored;
            each_span(generation, [&](semantic::span& span) {
                span.place.reset();
                auto file = state.files.find(span.document);
                if (file == state.files.end() || file->second->kind != "text")
                    return;
                auto first = file->second->position({span.first.run, span.first.offset}), last = file->second->position({span.last.run, span.last.offset});
                if (first && last && *first <= *last) {
                    auto& path = file->second->path;
                    if ((placed += path.size() + 5 * std::ranges::count_if(path, [](unsigned char c) { return c < 0x20 || c == '"' || c == '\\'; }) + 96) >
                        2 * semantic_bytes)
                        fail(nodes::code::resource_limit, "semantic reply exceeds its byte budget");
                    span.place = semantic::place{file->second->path, *first, *last + 1};
                }
            });
            out.stale = generation.version != out.version;
            out.semantic = std::move(generation);
            return out;
        }
        // Scope locks are local to the node that accepts writes; they are not replicated objects. A lock is a syntax
        // scope from language analysis; it holds the scope's characters, its children included, and "::" holds all.
        // range is what the lock keeps from others; intact says whether its scope is still whole, the condition that lets its
        // holder edit inside it.
        struct held {
            nodes::lock lock;
            std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
            bool intact = true;
            auto global() const -> bool { return lock.scope == nodes::global_scope; }
        };
        auto line_locks(const std::string& line) -> std::vector<nodes::lock> {
            std::vector<nodes::lock> out;
            statement rows{*db, "SELECT id,holder,scope,symbol,environment,document,first_run,first_offset,last_run,last_offset,acquired FROM locks "
                                "WHERE line=?1 ORDER BY id"};
            rows.text(1, line);
            while (rows.next()) {
                nodes::lock l{.id = rows.number(0),
                              .holder = rows.data(1),
                              .scope = rows.data(2),
                              .symbol = rows.data(3),
                              .environment = rows.data(4),
                              .document = rows.data(5),
                              .acquired = static_cast<std::int64_t>(rows.number(10))};
                if (!rows.data(6).empty()) {
                    l.first = semantic::anchor{rows.data(6), rows.number(7)};
                    l.last = semantic::anchor{rows.data(8), rows.number(9)};
                }
                out.push_back(std::move(l));
            }
            return out;
        }
        // Where a scope lock is in one source state. A scope whose end characters were removed holds its whole document.
        static auto resolve(const nodes::lock& l, const source_state& state) -> held {
            held out{l, std::nullopt};
            if (out.global())
                return out;
            if (auto found = state.files.find(l.document); found != state.files.end() && found->second->present) {
                auto& file = *found->second;
                auto b = file.position({l.first->run, l.first->offset}), e = file.position({l.last->run, l.last->offset});
                out.intact = b && e && *b <= *e;
                out.range = out.intact ? std::pair{*b, *e + 1} : std::pair{std::uint64_t{0}, file.size()};
                out.lock.place = semantic::place{file.path, out.range->first, out.range->second};
            }
            return out;
        }
        static auto overlaps(const held& a, const held& b) -> bool {
            if (a.global() || b.global())
                return true;
            return a.range && b.range && a.lock.document == b.lock.document && a.range->first < b.range->second && b.range->first < a.range->second;
        }
        static auto holding(const held& h) -> std::string { return std::format("{} holds {}", h.lock.holder, h.lock.scope); }
        static auto encloses(std::string_view kind) -> bool { return kind == "namespace" || kind == "type" || kind == "function" || kind == "module"; }
        static void lock_request(const node_request& in) {
            if (in.holder.empty())
                fail(nodes::code::invalid_record, "a lock request needs its holder");
            if (!in.edits.empty() || !in.expected.empty() || !in.source.empty() || !in.decisions.empty() || in.generation || !in.spans.empty() ||
                !in.operation.empty() || !in.revision.empty() || !in.snapshot.empty() || in.capture)
                fail(nodes::code::invalid_record, "a lock request takes only scopes, an environment and a target");
        }
        auto lock(const node_request& in) -> node_reply {
            lock_request(in);
            if (in.scopes.empty())
                fail(nodes::code::invalid_record, "a lock names its scopes");
            if (in.scopes.size() > anchor_spans)
                fail(nodes::code::resource_limit, "a lock names at most 10000 scopes");
            transaction tx{*db};
            refresh_external();
            ensure_kernel();
            auto line = root_line(in.worldline);
            auto heads = selected_heads({.action = "inspect", .worldline = in.worldline});
            auto state_owner = source.projection(heads);
            const auto& state = *state_owner;
            std::vector<held> existing;
            for (auto& l : line_locks(line))
                existing.push_back(resolve(l, state));
            std::optional<semantic::generation> analysis;
            if (std::ranges::any_of(in.scopes, [](auto& s) { return s != nodes::global_scope; })) {
                if (in.environment.empty())
                    fail(nodes::code::invalid_record, "a scope lock names the analysis environment of its scopes");
                auto found = newest(heads, in.environment, in.target);
                if (!found)
                    fail(nodes::code::invalid_record, "this worldline has no analysis to name scopes from");
                analysis = std::move(found->first);
            }
            for (auto& id : in.scopes) {
                nodes::lock l{.holder = in.holder,
                              .scope = id,
                              .environment = id == nodes::global_scope ? std::string{} : in.environment,
                              .acquired = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
                if (id != nodes::global_scope) {
                    auto scope = std::ranges::find(analysis->scopes, id, &semantic::scope::id);
                    if (scope == analysis->scopes.end())
                        fail(nodes::code::invalid_record, "the analysis has no scope " + id);
                    if (!encloses(scope->kind))
                        fail(nodes::code::invalid_record, "only a namespace, type, function or module scope can be locked");
                    l.symbol = scope->symbol;
                    l.document = scope->range.document;
                    l.first = scope->range.first;
                    l.last = scope->range.last;
                }
                auto wanted = resolve(l, state);
                if (!wanted.global() && (!wanted.range || !wanted.intact))
                    fail(nodes::code::invalid_record, "scope " + id + " is not whole in this worldline's source; analyze it again");
                for (auto& other : existing)
                    if (other.lock.holder != in.holder && overlaps(wanted, other))
                        fail("scope_locked", holding(other));
                // Locking a scope again takes it as the newest analysis names it.
                statement insert{*db, "INSERT INTO locks(line,holder,scope,symbol,environment,document,first_run,first_offset,last_run,last_offset,"
                                      "acquired) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) ON CONFLICT(line,holder,scope,environment) DO UPDATE SET "
                                      "symbol=excluded.symbol,document=excluded.document,first_run=excluded.first_run,first_offset=excluded.first_offset,"
                                      "last_run=excluded.last_run,last_offset=excluded.last_offset,acquired=excluded.acquired"};
                insert.text(1, line);
                insert.text(2, l.holder);
                insert.text(3, l.scope);
                insert.text(4, l.symbol);
                insert.text(5, l.environment);
                insert.text(6, l.document);
                insert.text(7, l.first ? l.first->run : std::string{});
                insert.number(8, l.first ? l.first->offset : 0);
                insert.text(9, l.last ? l.last->run : std::string{});
                insert.number(10, l.last ? l.last->offset : 0);
                insert.number(11, static_cast<std::uint64_t>(l.acquired));
                insert.next();
                existing.push_back(std::move(wanted));
            }
            tx.commit();
            return locks(in);
        }
        // Releases the named scopes of the caller, or one lock by number; a forced release may name another holder's lock.
        auto unlock(const node_request& in) -> node_reply {
            lock_request(in);
            if (in.scopes.empty() == in.target.empty())
                fail(nodes::code::invalid_record, "an unlock names either scopes or one lock number");
            transaction tx{*db};
            auto line = root_line(in.worldline);
            if (!in.scopes.empty()) {
                for (auto& scope : in.scopes) {
                    statement remove{*db, "DELETE FROM locks WHERE line=?1 AND holder=?2 AND scope=?3 AND (?4='' OR environment=?4)"};
                    remove.text(1, line);
                    remove.text(2, in.holder);
                    remove.text(3, scope);
                    remove.text(4, scope == nodes::global_scope ? std::string{} : in.environment);
                    remove.next();
                }
            } else {
                std::uint64_t id = 0;
                auto [end, error] = std::from_chars(in.target.data(), in.target.data() + in.target.size(), id);
                if (error != std::errc{} || end != in.target.data() + in.target.size())
                    fail(nodes::code::invalid_record, "unlock target is a lock number");
                statement holder{*db, "SELECT holder FROM locks WHERE line=?1 AND id=?2"};
                holder.text(1, line);
                holder.number(2, id);
                if (!holder.next())
                    fail(nodes::code::invalid_record, "no such lock");
                if (holder.data(0) != in.holder && !in.force)
                    fail(nodes::code::permission_denied, "another writer holds this lock");
                statement remove{*db, "DELETE FROM locks WHERE line=?1 AND id=?2"};
                remove.text(1, line);
                remove.number(2, id);
                remove.next();
            }
            tx.commit();
            return locks(in);
        }
        // Byte ranges of files of one version, named by the SHA-256 of their bytes; builds sync workers from these.
        auto content(const node_request& in) -> node_reply {
            if (in.parts.empty() || in.parts.size() > anchor_spans)
                fail(nodes::code::resource_limit, "a content request names 1 through 10000 parts");
            ensure_kernel();
            auto heads = selected_heads(in);
            auto state_owner = source.projection(heads);
            std::map<std::string, const source_file*> files;
            for (auto& [id, file] : state_owner->files)
                if (file->present)
                    files.emplace(file->digest, file.get());
            auto out = identity();
            out.frontier = heads;
            out.version = version(heads);
            std::uint64_t budget = 16 * 1024 * 1024;
            for (auto& part : in.parts) {
                auto found = files.find(part.id);
                if (found == files.end())
                    fail(nodes::code::missing_objects, "no file of this version has digest " + part.id);
                auto& bytes = found->second->content();
                if (part.offset > bytes.size() || part.total > bytes.size() - part.offset || part.total > budget)
                    fail(nodes::code::resource_limit, "content part is outside its file or the reply budget");
                budget -= part.total;
                out.parts.push_back({part.id, part.offset, bytes.size(), hex(std::string_view{bytes}.substr(part.offset, part.total))});
            }
            return out;
        }
        auto worldline_list() -> node_reply {
            ensure_kernel();
            auto out = identity();
            for (auto& [name, frontier] : worldlines()) {
                nodes::line_view line{.name = name, .version = version(frontier)};
                if (auto line_id = root_line(name); line_id != project) {
                    auto& created = get(line_id, "line");
                    line.base_version = version(string_list(created.at("base")));
                    line.creator = created.at("creator").text();
                }
                out.lines.push_back(std::move(line));
            }
            return out;
        }
        auto locks(const node_request& in) -> node_reply {
            ensure_kernel();
            auto line = root_line(in.worldline);
            auto heads = selected_heads({.action = "inspect", .worldline = in.worldline});
            auto state_owner = source.projection(heads);
            auto out = identity();
            out.frontier = heads;
            out.version = version(heads);
            for (auto& l : line_locks(line))
                out.locks.push_back(resolve(l, *state_owner).lock);
            return out;
        }
        // An attributed write must lie inside the caller's own scope locks, measured in the state it observed, and
        // never inside another holder's. Text edits need a scope that contains them; every other change needs "::".
        // Limits hold for the state a batch publishes, concurrent changes joined: no file it grows or brings back exceeds the file limit,
        // and it may not grow a project beyond the project limit. Zero is no limit.
        template <class State> void check_limits(const node_request& in, const State& before, const State& after) {
            auto total = [](const State& state) {
                std::uint64_t n = 0;
                for (auto& [_, file] : state.files)
                    n += file->bytes();
                return n;
            };
            if (in.file_limit)
                for (auto& [id, file] : after.files)
                    if (auto size = file->bytes(); size > in.file_limit) {
                        auto was = before.files.find(id);
                        if (was == before.files.end() || was->second->bytes() < size)
                            fail(nodes::code::resource_limit, std::format("{} would exceed the {} byte file limit", file->path, in.file_limit));
                    }
            if (in.project_limit)
                if (auto bytes = total(after); bytes > in.project_limit && bytes > total(before))
                    fail(nodes::code::resource_limit, std::format("the project would exceed its {} byte limit", in.project_limit));
        }
        void check_locks(const node_request& in, const std::vector<std::string>& heads) {
            auto line = root_line(in.worldline);
            auto state_owner = source.projection(heads);
            const auto& state = *state_owner;
            std::vector<held> current;
            for (auto& l : line_locks(line))
                current.push_back(resolve(l, state));
            auto require = [&](const held& wanted, bool point) {
                for (auto& h : current)
                    if (h.lock.holder != in.holder && overlaps(h, wanted))
                        fail("scope_locked", holding(h));
                for (auto& h : current)
                    if (h.lock.holder == in.holder &&
                        (h.global() ||
                         (h.intact && wanted.range && h.range && h.lock.document == wanted.lock.document && h.range->first <= wanted.range->first &&
                          (point ? wanted.range->first <= h.range->second : wanted.range->second <= h.range->second))))
                        return;
                fail("scope_locked", "lock the scope that contains this change first");
            };
            const held everything{nodes::lock{.scope = std::string{nodes::global_scope}}, std::nullopt};
            // An apply may take "::" for itself alone: it needs no other holder's lock in the way, and leaves nothing held behind.
            if (in.action == "apply" && !in.scopes.empty()) {
                if (in.scopes != std::vector<std::string>{std::string{nodes::global_scope}})
                    fail(nodes::code::invalid_record, "an apply takes only the global scope for itself");
                for (auto& h : current)
                    if (h.lock.holder != in.holder)
                        fail("scope_locked", holding(h));
                return;
            }
            if (in.capture) {
                if (!heads.empty() || std::ranges::any_of(in.edits, [](auto& e) { return e.kind != "create" && e.kind != "binary_create"; }))
                    fail(nodes::code::invalid_record, "a capture only creates the files of an empty worldline");
                for (auto& h : current)
                    if (h.lock.holder != in.holder)
                        fail("scope_locked", holding(h));
                return;
            }
            if (in.action != "apply" || !in.decisions.empty())
                return require(everything, false);
            for (auto& e : in.edits) {
                const source_file* file = nullptr;
                if (!e.document.empty()) {
                    if (auto it = state.files.find(e.document); it != state.files.end())
                        file = it->second.get();
                } else if (auto it = state.paths.find(e.path); it != state.paths.end() && it->second->size() == 1)
                    file = state.files.find(*it->second->begin())->second.get();
                if (e.kind != "edit" || !file || file->kind != "text") {
                    require(everything, false);
                    continue;
                }
                held target{nodes::lock{.document = file->id}, std::pair{e.offset, e.offset + std::max<std::uint64_t>(e.count, 1)}};
                require(target, e.count == 0);
            }
        }
        auto create_policy(const node_request& in) -> node_reply {
            auto previous = in.policies.empty() ? policies : vector_set(in.policies);
            if (previous != policies)
                fail("policy_conflict", "policy change must observe all current policy heads");
            auto body = base_body("policy", project);
            body["previous"] = strings(previous);
            body["authors"] = strings(vector_set(in.authors));
            body["nodes"] = strings(vector_set(in.nodes));
            body["maintainers"] = strings({author.id()});
            body["settled"] = strings(vector_set(in.settled));
            body["protected"] = strings(vector_set(in.protected_worldlines));
            std::uint64_t epoch = 0;
            for (auto& old : previous)
                if (get(old).at("kind").text() == "policy")
                    epoch = std::max(epoch, get(old).at("epoch").number());
            body["epoch"] = epoch + 1;
            transaction tx{*db};
            dirty = true;
            auto id = put(std::move(body), author);
            validate_object(id);
            refresh_policies();
            save_snapshot();
            tx.commit();
            dirty = false;
            auto out = identity();
            out.objects = {id};
            return out;
        }
        auto inventory(const std::string& known_checkpoint = {}) -> node_reply {
            auto out = identity();
            out.worldlines = worldlines();
            out.completeness_scope = "repository";
            if (known_checkpoint.empty())
                out.objects = object_ids();
            else {
                if (!checkpoint_cursors.contains(known_checkpoint)) {
                    causal_set covered;
                    if (auto bytes = pages->get("m/t/" + known_checkpoint)) {
                        auto body = codec::decode(*bytes);
                        for (auto& range : body.at("ranges").list()) {
                            auto first = range.at("first").number(), last = range.at("last").number();
                            if (first >= last || last > object_watermark + 1)
                                fail(nodes::code::invalid_record, "checkpoint row range");
                            covered.ranges.emplace_back(first, last);
                        }
                    } else {
                        auto& base = get(known_checkpoint, "checkpoint");
                        if (!is_validated(known_checkpoint))
                            fail(nodes::code::invalid_record, "checkpoint is unverified");
                        std::unordered_set<std::string> excluded;
                        excluded.insert(known_checkpoint);
                        for (auto& id : string_list(base.at("retained")))
                            excluded.insert(id);
                        for (auto* ref : ref_ids(base))
                            excluded.insert(*ref);
                        statement rows{*db, "SELECT rowid,id FROM objects ORDER BY rowid"};
                        while (rows.next())
                            if (excluded.contains(rows.data(1)))
                                covered.add(rows.number(0));
                        dirty_cursors.insert(known_checkpoint);
                    }
                    checkpoint_cursors[known_checkpoint] = std::move(covered);
                }
                causal_set available;
                available.ranges.emplace_back(1, object_watermark + 1);
                for (auto [first, last] : available.without(checkpoint_cursors.at(known_checkpoint)).ranges) {
                    statement rows{*db, "SELECT id FROM objects WHERE rowid>=?1 AND rowid<?2"};
                    rows.number(1, first);
                    rows.number(2, last);
                    while (rows.next())
                        out.objects.push_back(rows.data(0));
                }
                out.objects = vector_set(std::move(out.objects));
            }
            out.missing = missing(out.objects);
            statement pending{*db, "SELECT id FROM staging ORDER BY id"};
            while (pending.next())
                out.missing.push_back(pending.data(0));
            out.missing = vector_set(std::move(out.missing));
            if (!project.empty() && !has_object(project))
                out.missing.push_back(project);
            out.complete = out.missing.empty();
            out.storage_bytes = archive_bytes;
            return out;
        }
        auto receive(const node_request& in, bool detailed = true, bool raw_parts = false, bool validate = true, bool snapshot = true) -> node_reply {
            transaction tx{*db};
            if (!dirty)
                refresh_external();
            ensure_kernel();
            ensure_status();
            dirty = true;
            for (auto& part : in.parts) {
                if (!content::is_hash(part.id) || part.total > 128 * 1024 * 1024 || part.data.size() > 2 * 1024 * 1024)
                    fail(nodes::code::resource_limit, "invalid transfer object or chunk size");
                std::string decoded;
                if (!raw_parts)
                    decoded = unhex(part.data);
                std::string_view data = raw_parts ? std::string_view{part.data} : std::string_view{decoded};
                if (data.size() > 1024 * 1024 || part.offset > part.total || data.size() > part.total - part.offset)
                    fail(nodes::code::invalid_record, "invalid transfer bounds");
                if (has_object(part.id)) {
                    auto existing = object_bytes(part.id);
                    if (existing.size() != part.total || existing.compare(part.offset, data.size(), data) != 0)
                        fail(nodes::code::identity_conflict, "received bytes differ from retained object");
                    continue;
                }
                statement prior{*db, "SELECT total,data FROM staging WHERE id=?1"};
                prior.text(1, part.id);
                std::string bytes_value;
                if (prior.next()) {
                    if (prior.number(0) != part.total)
                        fail(nodes::code::identity_conflict, "object transfer length changed");
                    bytes_value = prior.data(1);
                }
                if (part.offset > bytes_value.size())
                    fail(nodes::code::missing_objects, "transfer has an unfilled gap");
                auto overlap = std::min<std::size_t>(data.size(), bytes_value.size() - part.offset);
                if (bytes_value.compare(part.offset, overlap, data, 0, overlap) != 0)
                    fail(nodes::code::identity_conflict, "transfer retry has different bytes");
                bytes_value += data.substr(overlap);
                if (bytes_value.size() == part.total) {
                    if (digest_of(bytes_value) != part.id)
                        fail(nodes::code::invalid_record, "object digest mismatch");
                    auto object = codec::decode(bytes_value);
                    auto& body = open(object);
                    if (body.at("kind").text() == "genesis") {
                        if (part.id != project)
                            fail("wrong_project", "unexpected genesis");
                    } else if (body.at("project").text() != project)
                        fail("wrong_project", "transferred object belongs to another project");
                    persist(std::move(object), part.id, bytes_value);
                    statement remove{*db, "DELETE FROM staging WHERE id=?1"};
                    remove.text(1, part.id);
                    remove.next();
                } else {
                    statement save{*db, "INSERT INTO staging(id,total,data) VALUES(?1,?2,?3) ON CONFLICT(id) DO UPDATE SET data=excluded.data"};
                    save.text(1, part.id);
                    save.number(2, part.total);
                    save.bind(3, bytes_value);
                    save.next();
                }
            }
            // Bulk streams validate the closure after their final chunk, before exposing an inventory.
            std::vector<std::string> rejected;
            if (validate) {
                rejected = validate_available();
                refresh_policies();
                if (snapshot)
                    save_snapshot();
            }
            tx.commit();
            dirty = !validate;
            trim_objects();
            auto out = identity();
            out.completeness_scope = "received_objects";
            if (detailed) {
                for (auto& part : in.parts)
                    out.objects.push_back(part.id);
                out.objects = vector_set(std::move(out.objects));
                out.missing = missing(out.objects);
                out.complete = out.missing.empty();
            }
            statement pending{*db, "SELECT id,total,length(data) FROM staging ORDER BY id"};
            while (pending.next()) {
                out.parts.push_back({pending.data(0), pending.number(2), pending.number(1), {}});
                out.complete = false;
                out.missing.push_back(pending.data(0));
            }
            out.missing = vector_set(std::move(out.missing));
            if (!rejected.empty()) {
                out.code = refused.at(rejected.front()).first;
                out.reason = refused.at(rejected.front()).second;
            }
            return out;
        }
        auto checkpoint(const node_request& in) -> node_reply {
            if (!in.target.empty()) {
                auto& checkpoint = get(in.target, "checkpoint");
                auto out = identity();
                out.checkpoint = in.target;
                out.issuer = checkpoint.at("owner").text();
                out.worldline = checkpoint.at("worldline").text();
                out.frontier = string_list(checkpoint.at("frontier"));
                out.missing = missing({in.target});
                out.complete = out.missing.empty() && is_validated(in.target);
                out.completeness_scope = "checkpoint";
                if (out.complete) {
                    out.version = version(out.frontier);
                    out.source_digest = source.projection(out.frontier)->digest;
                } else if (refused.contains(in.target)) {
                    out.code = refused.at(in.target).first;
                    out.reason = refused.at(in.target).second;
                }
                return out;
            }
            auto checkpoint_policy = policy(in.worldline);
            auto found = roots.find(in.worldline);
            if (found == roots.end() && in.worldline != "main")
                fail(nodes::code::invalid_record, "worldline not found");
            auto heads = !in.source.empty() ? source.frontier(in.source) : found == roots.end() ? std::vector<std::string>{} : found->second;
            within_line(root_line(in.worldline), heads);
            auto state_owner = source.projection(heads);
            const auto& state = *state_owner;
            if (!state.conflicts.empty())
                fail(nodes::code::source_conflict, "checkpoint source has unresolved conflicts");
            auto identities = protected_target(get(checkpoint_policy), in.worldline) ? unresolved_identities(heads, checkpoint_policy, in.worldline)
                                                                                     : std::vector<std::string>{};
            if (!identities.empty()) {
                error e{nodes::code::identity_conflict, "checkpoint requires an identity admission decision"};
                e.conflicts = std::move(identities);
                throw e;
            }
            value::array files;
            for (auto& [id, f] : state.files)
                if (f->present)
                    files.emplace_back(value::map{{"digest", f->digest},
                                                  {"executable", f->executable},
                                                  {"file", id},
                                                  {"kind", f->kind},
                                                  {"path", f->path},
                                                  {"size", static_cast<std::uint64_t>(f->bytes())}});
            transaction tx{*db};
            dirty = true;
            auto manifest = blob(codec::encode(value::map{{"files", std::move(files)}}));
            auto b = base_body("checkpoint", project);
            b["frontier"] = strings(heads);
            b["policies"] = strings({checkpoint_policy});
            b["manifest"] = manifest;
            b["owner"] = node.id();
            b["worldline"] = in.worldline;
            std::vector<std::string> admissions, receipts, evidence;
            if (in.source.empty())
                if (auto current = root_admissions.find(in.worldline); current != root_admissions.end()) {
                    auto& accepted = get(current->second, "admission");
                    if (accepted.at("issuer").text() == node.id() && accepted.at("target").at("name").text() == in.worldline &&
                        string_list(accepted.at("frontier")) == heads)
                        admissions.push_back(current->second);
                }
            if (admissions.empty() && !heads.empty())
                for (auto& id : object_ids("admission"))
                    if (is_validated(id))
                        admissions.push_back(id);
            for (auto& kind : {"receipt", "intent", "attempt"})
                for (auto& id : object_ids(kind))
                    if (is_validated(id)) {
                        if (kind == std::string_view{"receipt"})
                            receipts.push_back(id);
                        else
                            evidence.push_back(id);
                    }
            b["admissions"] = strings(admissions);
            b["receipts"] = strings(receipts);
            b["evidence"] = strings(vector_set(std::move(evidence)));
            b["configuration"] = in.configuration.empty() ? value{} : value{in.configuration};
            if (!in.configuration.empty())
                get(in.configuration, "blob");
            std::vector<std::string> retained(validated.begin(), validated.end());
            b["retained"] = strings(retained);
            b["inventory"] = blob(codec::encode(value::map{{"objects", strings(retained)}}));
            auto id = put(std::move(b), node);
            validate_object(id);
            trim_objects();
            checkpoint_cursors[id].ranges = {{1, static_cast<std::size_t>(object_watermark + 1)}};
            dirty_cursors.insert(id);
            save_snapshot();
            tx.commit();
            dirty = false;
            auto out = view(heads, false);
            out.checkpoint = id;
            return out;
        }
        auto recover() -> node_reply {
            reload(true);
            transaction tx{*db};
            reset_state_storage();
            for (auto& id : object_ids("receipt")) {
                auto& b = get(id);
                if (b.at("issuer").text() != node.id())
                    continue;
                statement q{*db, "INSERT INTO bindings(author,operation,digest,receipt) VALUES(?1,?2,?3,?4) ON CONFLICT(author,operation) DO UPDATE SET "
                                 "receipt=excluded.receipt WHERE digest=excluded.digest"};
                q.text(1, b.at("author").text());
                q.text(2, b.at("operation").text());
                q.text(3, b.at("request_digest").text());
                q.text(4, id);
                q.next();
            }
            for (auto& id : object_ids("failure")) {
                auto& b = get(id);
                if (b.at("author").text() != author.id())
                    continue;
                statement q{*db, "INSERT OR IGNORE INTO bindings(author,operation,digest,receipt) VALUES(?1,?2,?3,'')"};
                q.text(1, author.id());
                q.text(2, b.at("operation").text());
                q.text(3, b.at("request_digest").text());
                q.next();
            }
            for (auto& [_, heads] : roots) {
                for (auto& h : heads)
                    get(h, "batch");
                (void)source.projection(heads);
            }
            save_snapshot();
            tx.commit();
            auto visible = worldlines();
            for (auto& [name, heads] : roots)
                if (!visible.contains(name)) {
                    auto directory = root / "working" / name;
                    if (std::filesystem::is_directory(std::filesystem::symlink_status(directory)))
                        std::filesystem::remove_all(directory);
                }
            for (auto& [name, heads] : visible)
                materialize(name, heads);
            return inventory();
        }
        void materialize(const std::string& name, const std::vector<std::string>& heads) {
            checked_name(name);
            auto snapshot_owner = source.projection(heads);
            const auto& snapshot = *snapshot_owner;
            if (!snapshot.conflicts.empty())
                fail(nodes::code::source_conflict, "cannot materialize unresolved source");
            auto directory = root / "working" / name;
            checked_root(directory);
            auto marker = directory / ".fughetta-version";
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(marker)))
                fail(nodes::code::storage_failure, "working marker is a symbolic link");
            std::set<std::filesystem::path> keep;
            for (auto& [_, file] : snapshot.files) {
                if (file->path == ".fughetta-version" || file->path.starts_with(".fughetta-version/"))
                    fail(nodes::code::source_conflict, "source collides with materialization marker");
                keep.insert((directory / file->path).lexically_normal());
            }
            durability::durable_remove(marker);
            std::vector<std::filesystem::path> removed, directories;
            for (auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                if (entry.is_symlink())
                    fail(nodes::code::storage_failure, "working tree contains a symbolic link");
                if (entry.is_directory())
                    directories.push_back(entry.path());
                else if (!keep.contains(entry.path().lexically_normal()))
                    removed.push_back(entry.path());
            }
            for (auto& path : removed)
                durability::durable_remove(path);
            std::ranges::sort(directories, [](auto& a, auto& b) { return a.native().size() > b.native().size(); });
            for (auto& path : directories)
                if (std::filesystem::is_empty(path))
                    durability::durable_remove(path);
            for (auto& [_, file] : snapshot.files) {
                auto destination = directory / file->path;
                checked_root(destination.parent_path());
                bool current = false;
                if (std::filesystem::is_regular_file(destination) && std::filesystem::file_size(destination) == file->bytes() &&
                    durability::is_executable(destination) == file->executable)
                    current = digest_of(io_files.read(destination)) == file->digest;
                if (!current)
                    io_files.write(destination, file->content(), file->executable);
                interrupt_at("during_materialization");
            }
            io_files.write(marker, version(heads));
        }
        void record_failure(const node_request& in, const error& failure) {
            if (dirty)
                reload();
            auto digest = digest_of(codec::encode(request_body(in, project)));
            transaction tx{*db};
            dirty = true;
            auto input = blob(codec::encode(request_body(in, project)));
            auto b = base_body("failure", project);
            b["author"] = author.id();
            b["operation"] = in.operation;
            b["request_digest"] = digest;
            b["input"] = input;
            b["code"] = failure.code;
            b["reason"] = std::string(failure.what());
            b["intent"] = blob(in.intent);
            b["result"] = blob(in.result);
            put(std::move(b), author);
            statement q{*db, "INSERT OR IGNORE INTO bindings(author,operation,digest,receipt) VALUES(?1,?2,?3,'')"};
            q.text(1, author.id());
            q.text(2, in.operation);
            q.text(3, digest);
            q.next();
            save_snapshot();
            tx.commit();
            dirty = false;
        }
    };

    local_node::local_node(std::filesystem::path root, std::string trusted_project, bool audit, bool read_only)
        : state_(std::make_unique<impl>(std::move(root), std::move(trusted_project), audit, read_only)) {}
    local_node::~local_node() = default;
    auto local_node::project_id() const -> std::string { return state_->project; }
    auto local_node::node_id() const -> std::string { return state_->node.id(); }
    auto local_node::author_id() const -> std::string { return state_->author.id(); }
    auto local_node::statistics() const -> node_work {
        auto& s = *state_;
        return {s.projected_before_eviction + s.source.projected_files(),
                s.db->state_nodes_read,
                s.db->state_values_read,
                s.db->state_nodes_written,
                s.db->state_values_written,
                s.db->state_bytes_written,
                s.kernel_loaded && s.source.fully_loaded(),
                s.causal_headers_read,
                s.causal_effects_read,
                s.file_histories_read,
                s.file_payloads_read,
                s.db->state_bytes_read,
                s.kernel_loaded ? (s.source.has_reader() ? "indexed" : "complete") : "none",
                s.sequence_pages_read,
                s.sequence_pages_written,
                s.insertion_payloads_read,
                s.cache_evictions,
                s.db->state_bytes_decoded,
                s.db->causal_blocks_read};
    }
    void local_node::use_author(key signer) { state_->author = std::move(signer); }
    void local_node::confirm_with(commit_check* check) { state_->db->confirm = check; }
    auto local_node::initialize(std::string genesis_nonce) -> node_reply {
        if (!genesis_nonce.empty() && !content::is_hash(genesis_nonce))
            fail(nodes::code::invalid_record, "genesis nonce must be 32-byte lowercase hex");
        auto& s = *state_;
        if (!s.project.empty()) {
            if (!s.has_object(s.project))
                fail(nodes::code::missing_objects, "trusted genesis has not been received");
            if (!genesis_nonce.empty() && s.get(s.project, "genesis").at("nonce").raw() != unhex(genesis_nonce))
                fail(nodes::code::identity_conflict, "repository has another genesis nonce");
            return s.identity();
        }
        auto genesis = value::map{{"authors", strings({s.author.id()})},
                                  {"founding_key", bytes{s.author.public_key}},
                                  {"kind", "genesis"},
                                  {"maintainers", strings({s.author.id()})},
                                  {"nodes", strings({s.node.id()})},
                                  {"nonce", bytes{genesis_nonce.empty() ? random_bytes(32) : unhex(genesis_nonce)}},
                                  {"owner", s.author.id()},
                                  {"protected", strings({"main"})},
                                  {"schema", std::uint64_t{1}}};
        transaction tx{*s.db};
        s.dirty = true;
        s.project = s.put(std::move(genesis), s.author);
        s.validate_object(s.project);
        s.metadata("project", s.project);
        s.root_at("main", {});
        s.refresh_policies();
        s.save_snapshot();
        tx.commit();
        s.dirty = false;
        return s.identity();
    }
    auto local_node::objects(const std::vector<std::string>& ids) -> std::vector<object_part> {
        std::vector<object_part> out;
        for (auto& id : ids) {
            auto data = state_->object_bytes(id);
            for (std::size_t offset = 0; offset < data.size(); offset += 1024 * 1024)
                out.push_back({id, offset, data.size(), hex(std::string_view(data).substr(offset, 1024 * 1024))});
        }
        return out;
    }
    auto local_node::receive_packet(std::string_view packet) -> node_reply {
        try {
            return call({.action = "receive", .parts = decode_packet(packet)});
        } catch (const error& e) {
            auto out = state_->identity();
            out.code = e.code;
            out.reason = e.what();
            return out;
        }
    }
    namespace {
        void write_frame(std::ostream& output, std::string_view bytes) {
            auto packed = archive::pack(bytes);
            std::string header;
            codec::integer(header, packed.size());
            output.write(header.data(), header.size());
            output.write(packed.data(), packed.size());
            if (!output)
                fail(nodes::code::storage_failure, "exchange stream write failed");
        }
        auto read_frame(std::istream& input) -> std::optional<std::string> {
            std::array<char, 8> header{};
            input.read(header.data(), header.size());
            if (input.gcount() == 0 && input.eof())
                return {};
            if (input.gcount() != 8)
                fail(nodes::code::invalid_record, "truncated exchange frame header");
            std::uint64_t size = 0;
            for (int i = 7; i >= 0; --i)
                size = (size << 8) | static_cast<unsigned char>(header[i]);
            if (size > 128 * 1024 * 1024)
                fail(nodes::code::resource_limit, "exchange frame exceeds 128 MiB");
            std::string bytes(size, '\0');
            input.read(bytes.data(), bytes.size());
            if (static_cast<std::uint64_t>(input.gcount()) != size)
                fail(nodes::code::invalid_record, "truncated exchange frame");
            return archive::unpack(bytes);
        }
    } // namespace
    auto local_node::export_stream(std::ostream& output, std::string known_checkpoint) -> node_reply {
        auto& s = *state_;
        read_snapshot snapshot{*s.db};
        if (s.dirty)
            s.reload();
        else
            s.refresh_external();
        auto inventory = s.inventory(known_checkpoint);
        value::array lines;
        for (auto& [name, frontier] : s.worldlines())
            lines.emplace_back(value::map{{"frontier", strings(frontier)}, {"name", name}});
        auto b = base_body("inventory", s.project);
        b["base"] = known_checkpoint.empty() ? value{} : value{known_checkpoint};
        b["objects"] = strings(inventory.objects);
        b["missing"] = strings(inventory.missing);
        b["complete"] = inventory.complete;
        b["owner"] = s.node.id();
        b["worldlines"] = std::move(lines);
        write_frame(output, codec::encode(seal(std::move(b), s.node)));
        value::array parts;
        std::size_t buffered = 0;
        auto flush = [&] {
            if (!parts.empty()) {
                write_frame(output, codec::encode(value::map{{"parts", std::move(parts)}}));
                parts.clear();
                buffered = 0;
            }
        };
        auto emit = [&](const std::string& id) {
            auto data = s.object_bytes(id);
            for (std::size_t offset = 0; offset < data.size(); offset += 1024 * 1024) {
                auto chunk = data.substr(offset, 1024 * 1024);
                buffered += chunk.size();
                parts.emplace_back(
                    value::map{{"data", bytes{std::move(chunk)}}, {"id", id}, {"offset", std::uint64_t{offset}}, {"total", std::uint64_t{data.size()}}});
                if (buffered >= 1024 * 1024)
                    flush();
            }
        };
        if (known_checkpoint.empty()) {
            statement rows{*s.db, "SELECT id FROM objects ORDER BY rowid"};
            while (rows.next())
                emit(rows.data(0));
        } else {
            for (auto& id : inventory.objects)
                emit(id);
        }
        flush();
        return inventory;
    }
    auto local_node::import_stream(std::istream& input) -> node_reply {
        auto& s = *state_;
        s.refresh_external();
        s.ensure_kernel();
        s.ensure_status();
        auto header = read_frame(input);
        if (!header)
            fail(nodes::code::invalid_record, "exchange has no signed inventory");
        auto inventory = codec::decode(*header);
        const auto& body = open(inventory);
        s.check_schema(body);
        if (body.at("kind").text() != "inventory" || body.at("project").text() != s.project ||
            body.at("owner").text() != digest_of(inventory.at("public_key").raw()))
            fail("wrong_project", "exchange inventory origin or project mismatch");
        std::set<std::string> declared;
        for (auto& id : string_list(body.at("objects")))
            if (!content::is_hash(id) || !declared.insert(id).second)
                fail(nodes::code::invalid_record, "invalid inventory declaration");
        std::vector<object_part> pending;
        std::size_t pending_bytes = 0;
        auto flush = [&] {
            if (!pending.empty()) {
                s.receive({.action = "receive", .parts = std::move(pending)}, false, true, true, false);
                pending.clear();
                pending_bytes = 0;
            }
        };
        while (auto frame = read_frame(input)) {
            auto packet = codec::decode(*frame);
            fields(packet, {"parts"});
            for (auto& encoded : packet.at("parts").list()) {
                fields(encoded, {"data", "id", "offset", "total"});
                object_part part{encoded.at("id").text(), encoded.at("offset").number(), encoded.at("total").number(), encoded.at("data").raw()};
                if (!declared.contains(part.id))
                    fail(nodes::code::invalid_record, "undeclared transferred object");
                pending_bytes += part.data.size();
                pending.push_back(std::move(part));
            }
            if (pending_bytes >= 1024 * 1024)
                flush();
        }
        flush();
        {
            transaction tx{*s.db};
            s.dirty = true;
            s.validate_available();
            s.refresh_policies();
            tx.commit();
            s.dirty = false;
        }
        auto references = s.refs(body);
        auto absent = s.missing(references);
        auto declared_missing = string_list(body.at("missing"));
        absent.insert(absent.end(), declared_missing.begin(), declared_missing.end());
        auto out = s.identity();
        out.worldlines = s.worldlines();
        out.completeness_scope = "received_inventory";
        out.missing = vector_set(std::move(absent));
        out.complete = out.missing.empty() && body.at("complete").boolean() && std::ranges::all_of(references, [&](auto& id) { return s.is_validated(id); });
        if (!out.complete && out.missing.empty())
            out.reason = "inventory contains refused or unverified objects";
        out.storage_bytes = s.archive_bytes;
        for (auto& line : body.at("worldlines").list())
            out.worldlines[body.at("owner").text() + ":" + line.at("name").text()] = string_list(line.at("frontier"));
        {
            transaction tx{*s.db};
            s.dirty = true;
            if (out.complete) {
                auto id = s.retain(std::move(inventory));
                s.validate_object(id);
            }
            s.trim_objects();
            s.save_snapshot();
            tx.commit();
            s.dirty = false;
            s.trim_objects();
        }
        return out;
    }
    auto local_node::call(const node_request& in) -> node_reply {
        auto& s = *state_;
        struct cache_boundary {
            impl& state;
            ~cache_boundary() { state.trim_kernel(); }
        } cache{s};
        try {
            static const std::set<std::string> reads{"identity", "offer",   "usage",    "rejected", "send",  "inspect",    "tree",    "read",        "compare",
                                                     "history",  "receipt", "semantic", "anchor",   "locks", "worldlines", "content", "contribution"};
            if (s.read_only && !reads.contains(in.action))
                fail(nodes::code::policy_denied, "source reader does not accept mutations");
            std::optional<read_snapshot> snapshot;
            if (reads.contains(in.action))
                snapshot.emplace(*s.db);
            if (in.action == "audit") {
                transaction tx{*s.db};
                std::uint64_t generation_value = 0;
                {
                    statement generation{*s.db, "PRAGMA data_version"};
                    generation.next();
                    generation_value = generation.number(0);
                }
                if (!s.archive_audited || generation_value != s.data_version)
                    s.reload(true);
                s.dirty = true;
                s.source.detach_reader();
                s.reset_state_storage();
                s.save_snapshot();
                tx.commit();
                s.dirty = false;
                auto out = s.inventory();
                out.complete = out.complete && s.refused.empty();
                return out;
            }
            if (s.dirty)
                s.reload();
            else
                s.refresh_external();
            s.trim_objects();
            s.ensure_status();
            if (in.action == "identity")
                return s.identity();
            if (in.action == "initialize")
                return initialize(in.genesis_nonce);
            if (s.project.empty())
                fail("wrong_project", "initialize or join a trusted project first");
            if (in.action == "receive")
                return s.receive(in);
            if (in.action == "offer" || in.action == "usage")
                return s.inventory(in.target);
            if (in.action == "rejected") {
                auto out = s.identity();
                if (!in.target.empty()) {
                    auto found = s.refused.find(in.target);
                    if (found == s.refused.end())
                        fail(nodes::code::invalid_record, "object has no refusal record");
                    out.reason = found->second.second;
                }
                return out;
            }
            if (in.action == "send") {
                if (in.max_bytes == 0 || in.max_bytes > 16 * 1024 * 1024)
                    fail(nodes::code::resource_limit, "transfer packet budget must be 1 byte through 16 MiB");
                auto out = s.identity();
                auto budget = in.max_bytes;
                for (auto& id : in.want) {
                    auto encoded = s.object_bytes(id);
                    std::uint64_t offset = 0;
                    for (auto& requested : in.parts)
                        if (requested.id == id)
                            offset = requested.offset;
                    if (offset > encoded.size())
                        fail(nodes::code::invalid_record, "send offset exceeds object length");
                    while (offset < encoded.size() && budget) {
                        auto size = std::min<std::uint64_t>({budget, 1024 * 1024, encoded.size() - offset});
                        out.parts.push_back({id, offset, encoded.size(), hex(std::string_view(encoded).substr(offset, size))});
                        offset += size;
                        budget -= size;
                    }
                    if (!budget)
                        break;
                }
                return out;
            }
            if (in.action == "recover")
                return s.recover();
            if (!s.has_object(s.project)) {
                error e{nodes::code::missing_objects, "project genesis unavailable"};
                e.missing.push_back(s.project);
                throw e;
            }
            if (in.action == "policy") {
                s.ensure_kernel();
                return s.create_policy(in);
            }
            if (in.action == "semantic")
                return s.semantic_query(in);
            if (in.action == "anchor")
                return s.anchor(in);
            if (in.action == "lock")
                return s.lock(in);
            if (in.action == "unlock")
                return s.unlock(in);
            if (in.action == "locks")
                return s.locks(in);
            if (in.action == "worldlines")
                return s.worldline_list();
            if (in.action == "content")
                return s.content(in);
            if (in.action == "contribution")
                return s.contribution(in);
            if (in.action == "semantic.publish")
                return s.semantic_publish(in);
            if (in.action == "checkpoint") {
                if (in.target.empty())
                    s.ensure_kernel();
                return s.checkpoint(in);
            }
            if (in.action == "materialize") {
                s.ensure_kernel();
                auto it = s.roots.find(in.worldline);
                if (it == s.roots.end())
                    fail(nodes::code::invalid_record, "worldline not found");
                (void)s.root_line(in.worldline);
                s.materialize(in.worldline, it->second);
                auto out = s.view(it->second, false);
                out.materialized_version = out.version;
                return out;
            }
            if (in.action == "inspect" || in.action == "tree" || in.action == "read" || in.action == "compare" || in.action == "history") {
                auto found = s.roots.find(in.worldline);
                if (found != s.roots.end())
                    (void)s.root_line(in.worldline);
                auto heads = in.source.empty() ? (found == s.roots.end() ? std::vector<std::string>{} : found->second) : in.source;
                if (!in.snapshot.empty()) {
                    auto& checkpoint = s.get(in.snapshot, "checkpoint");
                    if (!s.is_validated(in.snapshot) || checkpoint.at("worldline").text() != in.worldline)
                        fail(nodes::code::invalid_record, "snapshot does not identify this worldline");
                    heads = string_list(checkpoint.at("frontier"));
                } else if (!in.revision.empty()) {
                    statement revision{*s.db, "SELECT data FROM revisions WHERE id=?1"};
                    revision.text(1, in.revision);
                    if (!revision.next())
                        fail(nodes::code::invalid_record, "revision not found");
                    heads = string_list(codec::decode(revision.data(0)).at("frontier"));
                }
                if (!in.revision.empty() && s.version(heads) != in.revision)
                    fail(nodes::code::invalid_record, "revision does not identify the selected frontier");
                if (!in.source.empty() || !in.revision.empty() || !in.snapshot.empty()) {
                    s.ensure_kernel();
                    s.within_line(s.root_line(in.worldline), heads);
                }
                for (auto& head : heads)
                    if (s.refused.contains(head))
                        fail(nodes::code::policy_denied, "selected source object was refused");
                if (in.action == "compare") {
                    s.ensure_kernel();
                    return s.compare(in, heads);
                }
                if (in.action == "history") {
                    s.ensure_kernel();
                    return s.history(in, heads);
                }
                return s.view(heads, in.action != "tree", in.action == "read" ? in.target : std::string{});
            }
            if (in.action == "receipt") {
                if (!in.target.empty())
                    return s.receipt(in.target, false);
                if (auto id = s.executed(in.operation))
                    return s.receipt(*id, false);
                // Flows that run several operations under one ID ask this first, so an abandoned one starts none of them.
                statement q{*s.db, "SELECT 1 FROM bindings WHERE author=?1 AND operation=?2 AND digest='abandoned'"};
                q.text(1, s.author.id());
                q.text(2, in.operation);
                if (q.next())
                    fail(nodes::code::identity_conflict, "operation was abandoned");
                fail(nodes::code::outcome_unknown, "no verified original execution receipt");
            }
            if (in.action == "abandon") {
                // An operation that has not run never will: its binding takes a digest no request has, so the request, arriving later,
                // fails as an operation ID reused with different input. One that ran answers with its receipt.
                if (!content::is_hash(in.operation))
                    fail(nodes::code::invalid_record, "abandon names an operation");
                transaction tx{*s.db};
                s.refresh_external();
                if (auto id = s.executed(in.operation))
                    return s.receipt(*id, false);
                statement q{*s.db,
                            "INSERT INTO bindings(author,operation,digest,receipt) VALUES(?1,?2,'abandoned','') ON CONFLICT(author,operation) DO UPDATE SET "
                            "digest='abandoned'"};
                q.text(1, s.author.id());
                q.text(2, in.operation);
                q.next();
                tx.commit();
                auto out = s.identity();
                out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
                return out;
            }
            static const std::set<std::string> mutations{"apply", "fork", "integrate", "admit", "reverse", "redo", "resolve"};
            if (!mutations.contains(in.action))
                fail(nodes::code::invalid_record, "unknown local node action");
            return s.publish(in);
        } catch (const error& e) {
            static const std::set<std::string> recorded{"apply", "fork", "integrate", "admit", "reverse", "redo", "resolve"};
            if (!s.read_only && recorded.contains(in.action) && content::is_hash(in.operation) && !s.project.empty() &&
                e.code != nodes::code::storage_failure) {
                try {
                    s.record_failure(in, e);
                } catch (...) {
                    s.dirty = true;
                }
            }
            if (s.dirty)
                try {
                    s.reload();
                } catch (const std::exception& failure) {
                    node_reply out;
                    out.code = nodes::code::outcome_unknown;
                    out.reason = std::string(e.what()) + "; durable state could not be confirmed: " + failure.what();
                    out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
                    return out;
                }
            auto out = s.identity();
            out.code = e.code;
            out.reason = e.what();
            out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
            out.missing = e.missing;
            out.conflicts = e.conflicts;
            out.complete =
                e.code != nodes::code::missing_objects && e.code != nodes::code::outcome_unknown && e.code != nodes::code::storage_failure && e.missing.empty();
            if (auto i = s.roots.find(in.worldline); i != s.roots.end())
                out.frontier = i->second;
            return out;
        } catch (const std::exception& e) {
            if (s.dirty)
                try {
                    s.reload();
                } catch (...) {
                    node_reply out;
                    out.code = nodes::code::outcome_unknown;
                    out.reason = "durable state could not be confirmed";
                    out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
                    return out;
                }
            auto out = s.identity();
            out.code = nodes::code::storage_failure;
            out.complete = false;
            out.reason = e.what();
            out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
            return out;
        }
    }
} // namespace nodes
