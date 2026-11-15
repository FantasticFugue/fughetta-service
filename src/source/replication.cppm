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
    auto local_node::impl::inventory(const std::string& known_checkpoint) -> node_reply {
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

    auto local_node::impl::receive(const node_request& in, bool detailed, bool raw_parts, bool validate, bool snapshot) -> node_reply {
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

    auto local_node::impl::checkpoint(const node_request& in) -> node_reply {
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
        auto identities =
            protected_target(get(checkpoint_policy), in.worldline) ? unresolved_identities(heads, checkpoint_policy, in.worldline) : std::vector<std::string>{};
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

    auto local_node::impl::recover() -> node_reply {
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

    void local_node::impl::materialize(const std::string& name, const std::vector<std::string>& heads) {
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

    void local_node::impl::record_failure(const node_request& in, const error& failure) {
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
} // namespace nodes
