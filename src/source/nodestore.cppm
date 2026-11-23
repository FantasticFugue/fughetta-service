module;
#include <sqlite3.h>
export module nodestore;
import std;
import engine;
import codec;

export namespace nodes::persistence {
    // The node's only durable primitive: one connection whose prepared statements are reused, and the
    // three lifetimes the rest of the engine writes through.
    struct database {
        sqlite3* handle = nullptr;
        std::map<std::string, std::vector<sqlite3_stmt*>, std::less<>> statements;
        nodes::commit_check* confirm = nullptr;
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
        }
        ~database() {
            for (auto& [_, idle] : statements)
                for (auto* query : idle)
                    sqlite3_finalize(query);
            if (handle)
                sqlite3_close_v2(handle);
        }
        database(const database&) = delete;
        auto operator=(const database&) -> database& = delete;
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
        statement(const statement&) = delete;
        auto operator=(const statement&) -> statement& = delete;
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
            if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
                sqlite3_bind_text(handle, n, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT) != SQLITE_OK)
                fail(nodes::code::storage_failure, "cannot bind object key");
        }
        void number(int n, std::uint64_t v) {
            if (v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                sqlite3_bind_int64(handle, n, static_cast<sqlite3_int64>(v)) != SQLITE_OK)
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
            return p ? std::string(p, static_cast<std::size_t>(sqlite3_column_bytes(handle, n))) : std::string{};
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
            if (!committed)
                sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr);
        }
        transaction(const transaction&) = delete;
        auto operator=(const transaction&) -> transaction& = delete;
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
        read_snapshot(const read_snapshot&) = delete;
        auto operator=(const read_snapshot&) -> read_snapshot& = delete;
        void commit() {
            if (owned)
                db.run("COMMIT");
            committed = true;
        }
    };
} // namespace nodes::persistence
