module;
#include <sqlite3.h>
module engine;
import :state;
import std;
import constants;
import content;
import codec;
import kernel;
import durability;
import archive;
import reflection;
import nodestore;

namespace nodes {
    using namespace persistence;

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

    // The causal closure of a version, encoded as ordered half-open event ranges. Every node reads the same
    // bytes for the same history, so a shard exchanged between them names exactly the events it observed.
    auto encode_causal(const causal_set& ancestors) -> std::string {
        std::string out{"DCA1"};
        codec::integer(out, ancestors.ranges.size());
        for (auto [first, last] : ancestors.ranges) {
            codec::integer(out, first);
            codec::integer(out, last);
        }
        return out;
    }

    auto decode_causal(std::string_view bytes) -> causal_set {
        if (!bytes.starts_with("DCA1") || bytes.size() < 12)
            fail(nodes::code::invalid_record, "causal closure framing");
        std::size_t offset = 4;
        auto integer = [&]() {
            if (bytes.size() - offset < 8)
                fail(nodes::code::invalid_record, "causal closure integer");
            std::uint64_t n = 0;
            for (int i = 7; i >= 0; --i)
                n = (n << 8) | static_cast<unsigned char>(bytes[offset + i]);
            offset += 8;
            return n;
        };
        auto count = integer();
        if (count > (bytes.size() - offset) / 16)
            fail(nodes::code::invalid_record, "causal closure count");
        causal_set out;
        for (std::uint64_t i = 0; i < count; ++i) {
            auto first = integer(), last = integer();
            if (first >= last || (!out.ranges.empty() && first <= out.ranges.back().second))
                fail(nodes::code::invalid_record, "causal closure intervals differ");
            out.ranges.emplace_back(first, last);
        }
        if (offset != bytes.size())
            fail(nodes::code::invalid_record, "trailing causal closure bytes");
        return out;
    }

    // The relational state store. Every derived table is a projection of the archive in `objects`:
    // `blobs` spells file bytes, `events` spells each committed batch, `file_effects` and `control_effects`
    // index its effects by file or by control target, and `manifest_files`/`manifest_paths` anchor version manifests.
    auto create_schema(database& db) -> void {
        db.run("CREATE TABLE IF NOT EXISTS objects(id TEXT PRIMARY KEY, data BLOB NOT NULL, kind TEXT NOT NULL);"
               "CREATE INDEX IF NOT EXISTS objects_kind ON objects(kind);"
               "CREATE TABLE IF NOT EXISTS metadata(name TEXT PRIMARY KEY, data BLOB NOT NULL);"
               "CREATE TABLE IF NOT EXISTS revisions(id TEXT PRIMARY KEY, data BLOB NOT NULL);"
               "CREATE TABLE IF NOT EXISTS reference_events(id INTEGER PRIMARY KEY AUTOINCREMENT,line TEXT NOT NULL,name TEXT NOT NULL,data BLOB NOT "
               "NULL);"
               "CREATE INDEX IF NOT EXISTS reference_events_line ON reference_events(line,id);"
               "CREATE TABLE IF NOT EXISTS named_refs(line TEXT NOT NULL,name TEXT NOT NULL,event INTEGER NOT NULL,PRIMARY KEY(line,name));"
               "CREATE TABLE IF NOT EXISTS dmail(id INTEGER PRIMARY KEY AUTOINCREMENT, at INTEGER NOT NULL, sender TEXT NOT NULL, "
               "line TEXT NOT NULL, recipient TEXT NOT NULL, body TEXT NOT NULL, operation TEXT NOT NULL, UNIQUE(sender,operation));"
               "CREATE INDEX IF NOT EXISTS dmail_recipient ON dmail(recipient,id);"
               "CREATE TABLE IF NOT EXISTS bindings(author TEXT NOT NULL, operation TEXT NOT NULL, digest TEXT NOT NULL, receipt TEXT NOT NULL, PRIMARY "
               "KEY(author,operation));"
               "CREATE TABLE IF NOT EXISTS receipts(id TEXT PRIMARY KEY, worldline TEXT NOT NULL, operation TEXT NOT NULL, "
               "caller_operation TEXT NOT NULL, author TEXT NOT NULL, result TEXT NOT NULL, prior BLOB NOT NULL, frontier BLOB NOT NULL, "
               "version TEXT NOT NULL, base_version TEXT NOT NULL, reach TEXT NOT NULL DEFAULT '');"
               "DROP TABLE IF EXISTS semantics;"
               "DROP TABLE IF EXISTS locks;"
               "DROP TABLE IF EXISTS roots;"
               "DROP TABLE IF EXISTS state_nodes;"
               "DROP TABLE IF EXISTS state_values;"
               "DROP TABLE IF EXISTS state_links;"
               "DROP TABLE IF EXISTS state_heads;"
               "DROP TABLE IF EXISTS state_payloads;"
               "CREATE TABLE IF NOT EXISTS blobs(digest TEXT PRIMARY KEY, kind TEXT NOT NULL, raw_bytes INTEGER NOT NULL, "
               "scalar_count INTEGER NOT NULL, data BLOB NOT NULL);"
               "CREATE TABLE IF NOT EXISTS worldlines(name TEXT PRIMARY KEY, head_version TEXT NOT NULL, base_version TEXT, creator TEXT NOT NULL, "
               "frontier_json TEXT NOT NULL);"
               "CREATE TABLE IF NOT EXISTS events(seq INTEGER PRIMARY KEY, event_id TEXT NOT NULL UNIQUE, worldline TEXT NOT NULL, "
               "parents_json TEXT NOT NULL, ancestors_blob BLOB NOT NULL, lamport INTEGER NOT NULL, author TEXT NOT NULL, operation TEXT NOT NULL, "
               "caller_operation TEXT NOT NULL, tool TEXT NOT NULL, intent TEXT NOT NULL, result_json TEXT NOT NULL, dependencies_json TEXT NOT NULL, "
               "effects INTEGER NOT NULL, request_digest TEXT NOT NULL, effect_digest TEXT NOT NULL, version_id TEXT NOT NULL, "
               "written_version TEXT NOT NULL);"
               "CREATE INDEX IF NOT EXISTS events_worldline_seq ON events(worldline, seq DESC);"
               "CREATE TABLE IF NOT EXISTS event_dependencies(dependency TEXT NOT NULL, event_seq INTEGER NOT NULL, PRIMARY KEY(dependency,event_seq)) "
               "WITHOUT ROWID;"
               "CREATE TABLE IF NOT EXISTS file_effects(file_id TEXT NOT NULL, event_seq INTEGER NOT NULL, ordinal INTEGER NOT NULL, "
               "effect_id TEXT NOT NULL UNIQUE, type TEXT NOT NULL, payload BLOB NOT NULL, PRIMARY KEY(file_id, event_seq, ordinal)) WITHOUT ROWID;"
               "CREATE INDEX IF NOT EXISTS file_effects_event ON file_effects(event_seq, ordinal);"
               "CREATE TABLE IF NOT EXISTS control_effects(target_effect_id TEXT NOT NULL, event_seq INTEGER NOT NULL, ordinal INTEGER NOT NULL, "
               "effect_id TEXT NOT NULL, type TEXT NOT NULL, value INTEGER NOT NULL, PRIMARY KEY(target_effect_id, event_seq, ordinal)) WITHOUT ROWID;"
               "CREATE INDEX IF NOT EXISTS control_effects_effect ON control_effects(effect_id, event_seq, ordinal);"
               "DROP TABLE IF EXISTS version_files;"
               "CREATE TABLE IF NOT EXISTS manifest_files(version_id TEXT NOT NULL, file_id TEXT NOT NULL, present INTEGER NOT NULL, "
               "path TEXT NOT NULL, digest TEXT NOT NULL, kind TEXT NOT NULL, size INTEGER NOT NULL, executable INTEGER NOT NULL, "
               "PRIMARY KEY(version_id, file_id)) WITHOUT ROWID;"
               "CREATE INDEX IF NOT EXISTS manifest_files_by_path ON manifest_files(version_id, path);"
               "CREATE TABLE IF NOT EXISTS manifest_paths(version_id TEXT NOT NULL, path TEXT NOT NULL, file_id TEXT NOT NULL, present INTEGER NOT NULL, "
               "PRIMARY KEY(version_id, path, file_id)) WITHOUT ROWID;");
    }

    auto local_node::impl::checked_root(std::filesystem::path path) -> std::filesystem::path {
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

    local_node::impl::impl(std::filesystem::path path, bool rebuild, bool reading) : root(checked_root(std::move(path))) {
        read_only = reading;
        for (auto name : {"repository.sqlite", "repository.sqlite-wal", "repository.sqlite-shm"})
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(root / name)))
                fail(nodes::code::storage_failure, "node database is a symbolic link");
        db = std::make_unique<database>(root / "repository.sqlite");
        create_schema(*db);
        {
            statement q{*db, "SELECT data FROM metadata WHERE name='identity'"};
            if (q.next())
                writer = q.data(0);
            else {
                writer = hex(random_bytes(32));
                metadata("identity", writer);
            }
        }
        {
            // The project is whatever genesis this repository holds; nothing joins another repository's project.
            statement q{*db, "SELECT data FROM metadata WHERE name='project'"};
            if (q.next())
                project = q.data(0);
        }
        reload(rebuild);
    }

    void local_node::impl::metadata(std::string_view name, std::string_view data) {
        statement q{*db, "INSERT INTO metadata(name,data) VALUES(?1,?2) ON CONFLICT(name) DO UPDATE SET data=excluded.data"};
        q.text(1, name);
        q.bind(2, data);
        q.next();
    }

    void local_node::impl::trim_kernel() {
        if (!kernel_loaded) {
            cached_payload_files.clear();
            cached_payload_bytes = 0;
            return;
        }
        if (!dirty && snapshot_ready && kernel_loaded && source.size() == snapshot_events &&
            (source.loaded_files() > 8 || source.loaded_events() > 512 || cached_payload_files.size() > 8 || cached_payload_bytes > 8 * 1024 * 1024)) {
            ++cache_evictions;
            cached_payload_files.clear();
            cached_payload_bytes = 0;
            // Release older reconstruction states; the newest one stays so the next write reuses it.
            source.trim_projections(1);
            if (source.loaded_events() > 4096 || source.loaded_files() > 1024) {
                projected_before_eviction += source.projected_files();
                source = kernel{};
                kernel_loaded = snapshot_events == 0;
            }
        }
    }

    void local_node::impl::trim_objects() {
        if (objects.size() <= 512 && cached_object_bytes <= 512 * 1024)
            return;
        auto remaining = object_order.size();
        while ((objects.size() > 256 || cached_object_bytes > 256 * 1024) && remaining-- && !object_order.empty()) {
            auto id = std::move(object_order.front());
            object_order.pop_front();
            if (!objects.contains(id))
                continue;
            cached_object_bytes -= object_sizes.at(id);
            object_sizes.erase(id);
            bodies.erase(id);
            objects.erase(id);
        }
    }

    auto local_node::impl::put(value body) -> std::string {
        auto kind = body.at("kind").text();
        std::vector<std::string> required;
        if (kind == "batch")
            required = string_list(body.at("required"));
        auto id = retain(std::move(body));
        // A batch is one committed contribution: the causal engine sees it as it is written.
        if (kind == "batch") {
            // A binary effect names content the batch requires; the renderer reads those bytes back by digest.
            for (auto& blob_id : required)
                if (get(blob_id).at("kind").text() == "blob") {
                    auto& content = get(blob_id, "blob").at("data").raw();
                    source.add_blob(digest_of(content), content);
                }
            source.add(id, get(id));
        } else if (kind == "line")
            line_names.reset();
        return id;
    }

    auto local_node::impl::retain(value object) -> std::string {
        auto data = codec::encode(object);
        auto id = digest_of(data);
        persist(std::move(object), id, data);
        return id;
    }

    void local_node::impl::persist(value object, const std::string& id, std::string_view data) {
        check_schema(object);
        auto packed = archive::pack(data);
        statement q{*db, "INSERT OR IGNORE INTO objects(id,data,kind) VALUES(?1,?2,?3)"};
        q.text(1, id);
        q.bind(2, packed);
        q.text(3, object.at("kind").text());
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
        bodies.emplace(id, body_view{&stored->second});
    }

    auto local_node::impl::blob(std::string bytes_value) -> std::string {
        auto body = base_body("blob", project);
        body["data"] = bytes{std::move(bytes_value)};
        return put(std::move(body));
    }

    auto local_node::impl::get(const std::string& id, std::string_view kind) const -> const value& {
        auto it = bodies.find(id);
        if (it == bodies.end()) {
            auto data = object_bytes(id);
            auto object = codec::decode(data);
            check_schema(object);
            auto [stored, inserted] = objects.emplace(id, std::move(object));
            if (inserted) {
                object_order.push_back(id);
                object_sizes.emplace(id, data.size());
                cached_object_bytes += data.size();
            }
            it = bodies.emplace(id, body_view{&stored->second}).first;
            ++loaded_records;
        }
        if (!kind.empty() && it->second.at("kind").text() != kind)
            fail(nodes::code::invalid_record, "reference has incorrect kind");
        return it->second;
    }

    auto local_node::impl::has_object(const std::string& id) const -> bool {
        statement q{*db, "SELECT 1 FROM objects WHERE id=?1"};
        q.text(1, id);
        return q.next();
    }

    auto local_node::impl::object_bytes(const std::string& id) const -> std::string {
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

    auto local_node::impl::object_ids(std::string_view kind) const -> std::vector<std::string> {
        statement q{*db, kind.empty() ? "SELECT id FROM objects ORDER BY id" : "SELECT id FROM objects WHERE kind=?1 ORDER BY id"};
        if (!kind.empty())
            q.text(1, kind);
        std::vector<std::string> ids;
        while (q.next())
            ids.push_back(q.data(0));
        return ids;
    }

    void local_node::impl::write_blob(const std::string& digest, std::string_view kind, std::uint64_t raw_bytes, std::uint64_t scalars, std::string_view data) {
        if (digest_of(data) != digest)
            fail(nodes::code::storage_failure, "content payload digest differs");
        auto packed = archive::pack(data);
        statement q{*db, "INSERT OR IGNORE INTO blobs(digest,kind,raw_bytes,scalar_count,data) VALUES(?1,?2,?3,?4,?5)"};
        q.text(1, digest);
        q.text(2, kind);
        q.number(3, raw_bytes);
        q.number(4, scalars);
        q.bind(5, packed);
        q.next();
        if (sqlite3_changes(db->handle)) {
            ++blob_writes;
            state_bytes_written += packed.size();
        }
    }

    auto local_node::impl::read_blob(const std::string& digest) -> std::string {
        statement q{*db, "SELECT data,raw_bytes FROM blobs WHERE digest=?1"};
        q.text(1, digest);
        if (!q.next())
            fail(nodes::code::missing_objects, "required content payload is absent: " + digest);
        auto packed = q.data(0);
        auto size = q.number(1);
        ++blob_reads;
        state_bytes_read += packed.size();
        std::string data;
        try {
            data = archive::unpack(packed, size);
        } catch (const error& e) {
            fail(nodes::code::storage_failure, "stored content payload is corrupt: " + std::string(e.what()));
        }
        state_bytes_decoded += data.size();
        if (digest_of(data) != digest || data.size() != size)
            fail(nodes::code::storage_failure, "content payload hash differs");
        return data;
    }

    void local_node::impl::load_objects(std::uint64_t after) {
        // Rows are written in causal order, so one pass in rowid order re-adds every batch committed since the snapshot.
        statement query{*db, "SELECT id,data FROM objects WHERE rowid>?1 ORDER BY rowid"};
        query.number(1, after);
        struct record {
            std::string id;
            value object;
            std::size_t bytes = 0;
        };
        std::vector<record> batch;
        std::size_t buffered = 0;
        auto merge = [&] {
            for (auto& row : batch) {
                if (row.object.at("kind").text() == "batch")
                    source.add(row.id, row.object);
                auto [stored, inserted] = objects.emplace(row.id, std::move(row.object));
                if (inserted) {
                    object_order.push_back(stored->first);
                    object_sizes.emplace(stored->first, row.bytes);
                    cached_object_bytes += row.bytes;
                }
                bodies.emplace(stored->first, body_view{&stored->second});
                ++loaded_records;
            }
            batch.clear();
            buffered = 0;
            trim_objects();
        };
        while (query.next()) {
            auto packed = query.data(1);
            auto object = codec::decode(archive::unpack(packed));
            check_schema(object);
            buffered += packed.size();
            batch.push_back({query.data(0), std::move(object), packed.size()});
            if (batch.size() >= 256 || buffered >= 8 * 1024 * 1024)
                merge();
        }
        merge();
    }

    void local_node::impl::check_schema(const value& b) const {
        auto kind = b.at("kind").text();
        if (kind == "genesis")
            fields(b, {"kind", "nonce", "schema"});
        else if (kind == "blob")
            fields(b, {"data", "kind", "project", "schema"});
        else if (kind == "line")
            fields(b, {"base", "creator", "kind", "name", "operation", "project", "schema"});
        else if (kind == "batch")
            fields(b, {"actor", "author", "caller_operation", "dependencies", "effect_digest", "effects", "intent", "kind", "lamport", "line", "operation",
                       "parents", "project", "request_digest", "required", "result", "schema", "tool"});
        else
            fail(nodes::code::invalid_record, "unrecognized object kind");
        if (b.at("schema").number() != 1)
            fail(nodes::code::invalid_record, "unsupported record schema");
        if (kind == "genesis") {
            if (b.at("nonce").raw().size() != 32)
                fail(nodes::code::invalid_record, "invalid genesis nonce");
            return;
        }
        if (kind == "blob") {
            (void)b.at("data").raw();
            return;
        }
        if (!content::is_hash(b.at("project").text()))
            fail(nodes::code::invalid_record, "invalid project identity");
        if (kind == "line") {
            checked_name(b.at("name").text());
            if (!content::is_hash(b.at("creator").text()) || !content::is_hash(b.at("operation").text()))
                fail(nodes::code::invalid_record, "invalid worldline creation identity");
            auto& base = b.at("base").list();
            for (std::size_t i = 0; i < base.size(); ++i)
                if (!content::is_hash(base[i].text()) || (i && !canonical_less(base[i - 1].text(), base[i].text())))
                    fail(nodes::code::invalid_record, "noncanonical worldline base");
            return;
        }
        if (!content::is_hash(b.at("author").text()) || !content::is_hash(b.at("operation").text()))
            fail(nodes::code::invalid_record, "invalid operation identity");
        if (!b.at("lamport").number())
            fail(nodes::code::invalid_record, "invalid Lamport value");
        for (auto name : {"dependencies", "parents", "required"}) {
            auto& set = b.at(name).list();
            for (std::size_t i = 0; i < set.size(); ++i) {
                auto& id = set[i].text();
                if (std::string_view{name} != "dependencies" && !content::is_hash(id))
                    fail(nodes::code::invalid_record, "invalid object address");
                if (i && !canonical_less(set[i - 1].text(), id))
                    fail(nodes::code::invalid_record, "noncanonical set field: " + std::string(name));
            }
        }
        for (auto name : {"effect_digest", "line", "request_digest", "result"}) {
            if (!content::is_hash(b.at(name).text()))
                fail(nodes::code::invalid_record, "invalid object reference");
        }
        // The caller's own operation ID is display text the gateway may choose; the batch keys on its scoped operation.
        (void)b.at("caller_operation").text();
        (void)b.at("effects").list();
        (void)b.at("intent").text();
        (void)b.at("actor").text();
        (void)b.at("tool").text();
    }
} // namespace nodes
