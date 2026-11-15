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
    auto local_node::impl::block_key(std::uint64_t index) const -> std::string { return std::format("k/{:016x}", index); }

    auto local_node::impl::ordinal_key(std::uint64_t index) const -> std::string { return std::format("{:016x}", index); }

    void local_node::impl::ensure_kernel() {
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

    void local_node::impl::persist_event(std::size_t index) {
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
                input["scalars"] = scalar_count(text);
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

    void local_node::impl::projection_stores() {
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

    auto local_node::impl::persist_projection(const source_state& state) -> std::string {
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

    auto local_node::impl::restore_projection(const std::string& bytes, const std::string& expected_digest) -> std::shared_ptr<const source_state> {
        projection_stores();
        auto state = std::make_shared<source_state>();
        auto v = codec::decode(bytes);
        fields(v, {"activation", "causal", "conflicts", "control_conflicts", "dependency_conflicts", "file_conflicts", "files", "hidden", "identity_conflicts",
                   "path_conflicts", "paths", "resolutions"});
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

    auto local_node::impl::file_header(std::string_view encoded, const std::string& id) -> std::shared_ptr<const source_file> {
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

    auto local_node::impl::file_bytes(const source_file& file) -> std::string {
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

    auto local_node::impl::matching_view(const std::vector<std::string>& heads) -> std::optional<std::string> {
        for (auto& [name, view] : views)
            if (view.frontier == heads)
                return name;
        return {};
    }

    auto local_node::impl::ready_reply(const std::string& name, bool include_text, std::string_view path) -> node_reply {
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

    void local_node::impl::persist_view(const std::string& name) {
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
        saved_view saved{
            heads,        state->conflicts,          root_admissions.contains(name) ? root_admissions.at(name) : std::string{}, version(heads), state->digest,
            files.root(), persist_projection(*state)};
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

    void local_node::impl::reset_state_storage() {
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

    void local_node::impl::save_snapshot() {
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

    auto local_node::impl::restore_snapshot() -> bool {
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

    void local_node::impl::reload(bool audit) {
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

    void local_node::impl::refresh_external() {
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
} // namespace nodes
