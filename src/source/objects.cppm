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
import index;
import reflection;
import nodestore;

namespace nodes {
    using namespace persistence;
    auto local_node::impl::fragment_value(const fragment& f) -> value { return value::map{{"count", f.count}, {"first", f.first}, {"run", f.run}}; }

    auto local_node::impl::span_value(const anchor_span& f) -> value { return value::map{{"count", f.count}, {"first", f.first}, {"key", f.key}}; }

    void local_node::impl::projection_reference(const std::string& type, const std::string& id, int delta) {
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

    void local_node::impl::payload_reference(const std::string& id, int delta) {
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

    void local_node::impl::projection_roots(const std::string& previous, const std::string& current) {
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

    void local_node::impl::collect_projection_garbage() {
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

    auto local_node::impl::sequence_from(const value& roots) -> std::shared_ptr<sequence_view> {
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

    auto local_node::impl::sequence_for(const source_file& file) -> std::shared_ptr<const sequence_state> {
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

    auto local_node::impl::retain_payload(std::string data) -> std::string {
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

    auto local_node::impl::read_payload(const std::string& id) -> std::string {
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

    auto local_node::impl::load_key(const std::filesystem::path& file, archive::files& files) -> key {
        auto status = std::filesystem::symlink_status(file);
        if (std::filesystem::is_symlink(status))
            fail(nodes::code::storage_failure, "key file is a symbolic link");
        if (std::filesystem::exists(status)) {
            return key::from_secret(files.read(file));
        }
        auto out = key::generate();
        files.write(file, out.secret);
        std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);
        return out;
    }

    local_node::impl::impl(std::filesystem::path path, std::string trust, bool audit, bool reading)
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

    void local_node::impl::metadata(std::string_view name, std::string_view data) {
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

    void local_node::impl::trim_kernel() {
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

    auto local_node::impl::is_validated(const std::string& id) const -> bool {
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

    void local_node::impl::queue_object(const std::string& id) const {
        if (!is_validated(id) && !refused.contains(id) && pending_objects.insert(id).second) {
            new_candidates.push_back(id);
            dirty_status.insert(id);
        }
    }

    void local_node::impl::mark_validated(const std::string& id) {
        validated.insert(id);
        pending_objects.erase(id);
        remaining_dependencies.erase(id);
        dirty_status.insert(id);
        completed_objects.push_back(id);
    }

    void local_node::impl::ensure_status() {
        if (status_loaded)
            return;
        status_loaded = true;
        pages->each("m/x/", [&](const std::string& key, std::string bytes) {
            auto body = codec::decode(bytes);
            refused[key.substr(4)] = {body.at("code").text(), body.at("reason").text()};
        });
        pages->each("m/i/", [&](const std::string& key, std::string bytes) { identity_conflicts[key.substr(4)] = codec::decode(bytes).at("id").text(); });
    }

    auto local_node::impl::claims(const std::string& logical) -> std::set<std::string>& {
        if (!operation_claims.contains(logical))
            if (auto bytes = pages->get("m/c/" + logical)) {
                auto ids = string_list(codec::decode(*bytes).at("ids"));
                operation_claims[logical] = {ids.begin(), ids.end()};
            }
        return operation_claims[logical];
    }

    auto local_node::impl::receipts(const std::string& logical) -> std::vector<std::string>& {
        if (!receipt_index.contains(logical))
            if (auto bytes = pages->get("m/r/" + logical))
                receipt_index[logical] = string_list(codec::decode(*bytes).at("ids"));
        return receipt_index[logical];
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

    auto local_node::impl::put(value body, const key& signer) -> std::string {
        auto kind = body.at("kind").text();
        auto id = retain(seal(std::move(body), signer));
        if (kind == "blob" || kind == "intent" || kind == "attempt" || kind == "failure" || kind == "review")
            validate_object(id);
        return id;
    }

    auto local_node::impl::retain(value object) -> std::string {
        auto data = codec::encode(object);
        auto id = digest_of(data);
        (void)open(object);
        persist(std::move(object), id, data);
        return id;
    }

    void local_node::impl::persist(value object, const std::string& id, std::string_view data) {
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

    auto local_node::impl::blob(std::string bytes_value) -> std::string {
        auto body = base_body("blob", project);
        body["data"] = bytes{std::move(bytes_value)};
        return put(std::move(body), author);
    }

    auto local_node::impl::get(const std::string& id, std::string_view kind) const -> const value& {
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

    auto local_node::impl::request_input(const std::string& id) -> const value& {
        if (auto it = decoded_inputs.find(id); it != decoded_inputs.end())
            return it->second;
        return decoded_inputs.emplace(id, codec::decode(get(id, "blob").at("data").raw())).first->second;
    }

    auto local_node::impl::ref_ids(const value& b) const -> std::vector<const std::string*> {
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

    auto local_node::impl::refs(const value& b) const -> std::vector<std::string> {
        auto ids = ref_ids(b);
        std::vector<std::string> out;
        out.reserve(ids.size());
        for (auto* id : ids)
            out.push_back(*id);
        return out;
    }

    void local_node::impl::check_schema(const value& b) const {
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
            fields(b, {"attempt", "author", "dependencies", "effect_digest", "effects", "expected", "intent", "kind", "lamport", "line", "operation", "parents",
                       "policy", "project", "request_digest", "required", "schema"});
        else if (kind == "line")
            fields(b, {"base", "creator", "kind", "name", "operation", "project", "schema"});
        else if (kind == "policy")
            fields(b, {"authors", "epoch", "kind", "maintainers", "nodes", "previous", "project", "protected", "schema", "settled"});
        else if (kind == "admission")
            fields(b, {"accepted_dependencies", "attempt", "author_override", "frontier", "issuer", "kind", "line", "operation", "policy", "previous", "prior",
                       "project", "result", "review", "schema", "target"});
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
        for (auto name :
             {"project", "policy", "intent", "attempt", "input", "result", "admission", "configuration", "manifest", "inventory", "base", "review", "line"}) {
            auto it = b.object().find(name);
            if (kind == "line" && std::string_view{name} == "base")
                continue;
            if (it != b.object().end() && !std::holds_alternative<std::monostate>(it->second.data) && !content::is_hash(it->second.text()))
                fail(nodes::code::invalid_record, "invalid object reference");
        }
        if (kind == "blob")
            (void)b.at("data").raw();
    }
} // namespace nodes
