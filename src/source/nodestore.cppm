module;
#include <sqlite3.h>
export module nodestore;
import nodes;
import engine;
import std;
import constants;
import content;
import codec;
import kernel;
import durability;
import archive;
import index;
import reflection;

export namespace nodes::persistence {
    auto overlapping_paths(const auto& paths, const std::string& path) -> std::vector<std::string> {
        std::vector<std::string> out;
        for (auto prefix = path; !prefix.empty();) {
            if (paths.contains(prefix))
                out.push_back(prefix);
            auto slash = prefix.rfind('/');
            if (slash == std::string::npos)
                break;
            prefix.resize(slash);
        }
        const auto prefix = path + "/";
        for (auto it = paths.lower_bound(prefix); it != paths.end(); ++it) {
            const auto& key = [&]() -> const std::string& {
                if constexpr (requires { it->first; })
                    return it->first;
                else
                    return *it;
            }();
            if (!key.starts_with(prefix))
                break;
            out.push_back(key);
        }
        return out;
    }

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
                "CREATE TABLE IF NOT EXISTS reference_events(id INTEGER PRIMARY KEY AUTOINCREMENT,line TEXT NOT NULL,name TEXT NOT NULL,data BLOB NOT "
                "NULL);"
                "CREATE INDEX IF NOT EXISTS reference_events_line ON reference_events(line,id);"
                "CREATE TABLE IF NOT EXISTS named_refs(line TEXT NOT NULL,name TEXT NOT NULL,event INTEGER NOT NULL,PRIMARY KEY(line,name));"
                "DROP TABLE IF EXISTS semantics;"
                "DROP TABLE IF EXISTS locks;"
                "CREATE TABLE IF NOT EXISTS dmail(id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL, sender TEXT NOT NULL, "
                "line TEXT NOT NULL, recipient TEXT NOT NULL, body TEXT NOT NULL, operation TEXT NOT NULL, UNIQUE(sender,operation));"
                "CREATE INDEX IF NOT EXISTS dmail_recipient ON dmail(recipient,id);"
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
                    if (*entries[index].value &&
                        (middle == last || std::tie(entries[index].priority, *entries[index].key) < std::tie(entries[middle].priority, *entries[middle].key)))
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
                statement q{db_,
                            table == "state_nodes" ? "UPDATE state_nodes SET refs=refs+?2 WHERE id=?1" : "UPDATE state_values SET refs=refs+?2 WHERE id=?1"};
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
} // namespace nodes::persistence
