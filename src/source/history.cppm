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
    auto local_node::impl::version(const std::vector<std::string>& frontier) -> std::string {
        if (frontier.empty())
            return digest_of(codec::encode(value::map{{"frontier", strings({})}, {"project_id", project}}));
        ensure_kernel();
        return digest_of(codec::encode(value::map{{"frontier", strings(source.frontier(frontier))}, {"project_id", project}}));
    }

    auto local_node::impl::revision(const std::string& version_id) const -> std::optional<value> {
        statement q{*db, "SELECT data FROM revisions WHERE id=?1"};
        q.text(1, version_id);
        if (!q.next())
            return {};
        return codec::decode(q.data(0));
    }

    auto local_node::impl::revision_member(const value& data, std::string_view name) -> const value* {
        auto& fields = data.object();
        auto found = fields.find(name);
        return found == fields.end() ? nullptr : &found->second;
    }

    auto local_node::impl::revision_text(const value& data, std::string_view name) -> std::string {
        auto* found = revision_member(data, name);
        return found ? found->text() : std::string{};
    }

    auto local_node::impl::revision_number(const value& data, std::string_view name) -> std::uint64_t {
        auto* found = revision_member(data, name);
        return found ? found->number() : 0;
    }

    auto local_node::impl::revision_bool(const value& data, std::string_view name) -> bool {
        auto* found = revision_member(data, name);
        return found && found->boolean();
    }

    auto local_node::impl::conflict_array(const std::map<std::string, std::vector<std::string>>& index) -> value::array {
        value::array out;
        for (auto& [id, conflicts] : index)
            out.emplace_back(value::map{{"conflicts", strings(conflicts)}, {"id", id}});
        return out;
    }

    auto local_node::impl::conflict_map(const value& data, std::string_view name) -> std::map<std::string, std::vector<std::string>> {
        std::map<std::string, std::vector<std::string>> out;
        if (auto* found = revision_member(data, name))
            for (auto& entry : found->list())
                out[entry.at("id").text()] = string_list(entry.at("conflicts"));
        return out;
    }

    auto local_node::impl::manifest_chain(const std::string& version_id) const -> std::vector<std::pair<std::string, value>> {
        std::vector<std::pair<std::string, value>> chain;
        auto current = version_id;
        for (std::uint64_t depth = 0;; ++depth) {
            if (current.empty() || depth > constants::manifest_chain_limit)
                fail(nodes::code::missing_objects, "version manifest chain is incomplete; reconstruct from the retained archive");
            auto data = revision(current);
            if (!data || revision_text(*data, "root").empty())
                fail(nodes::code::missing_objects, "version manifest is absent; reconstruct from the retained archive");
            chain.emplace_back(current, std::move(*data));
            if (revision_bool(chain.back().second, "full"))
                return chain;
            current = revision_text(chain.back().second, "parent");
        }
    }

    auto local_node::impl::manifest_files(const std::string& version_id) -> std::map<std::string, std::shared_ptr<const source_file>> {
        auto chain = manifest_chain(version_id);
        std::map<std::string, std::shared_ptr<const source_file>> files;
        for (auto level = chain.rbegin(); level != chain.rend(); ++level) {
            statement q{*db, "SELECT file_id,present,path,digest,kind,size,executable FROM manifest_files WHERE version_id=?1"};
            q.text(1, level->first);
            while (q.next()) {
                auto id = q.data(0);
                ++manifest_rows_read;
                if (!q.number(1)) {
                    files.erase(id);
                    continue;
                }
                auto file = std::make_shared<source_file>();
                file->id = std::move(id);
                file->path = q.data(2);
                file->digest = q.data(3);
                file->kind = q.data(4);
                file->stored_bytes = q.number(5);
                file->executable = q.number(6) != 0;
                file->stored = &causal;
                file->fragments_ready = false;
                files[file->id] = std::move(file);
            }
        }
        return files;
    }

    auto local_node::impl::manifest_entry(const std::string& version_id, const std::string& path) -> std::vector<std::shared_ptr<const source_file>> {
        auto chain = manifest_chain(version_id);
        for (auto& [level, data] : chain) {
            statement q{*db, "SELECT file_id,present FROM manifest_paths WHERE version_id=?1 AND path=?2"};
            q.text(1, level);
            q.text(2, path);
            std::vector<std::string> owners;
            bool mentioned = false;
            while (q.next()) {
                mentioned = true;
                ++manifest_rows_read;
                if (q.number(1))
                    owners.push_back(q.data(0));
            }
            if (!mentioned)
                continue;
            std::vector<std::shared_ptr<const source_file>> out;
            for (auto& id : owners) {
                auto file = manifest_record(version_id, id);
                if (!file || !(*file)->present || (*file)->path != path)
                    fail(nodes::code::missing_objects, "manifest path entry has no file row");
                out.push_back(std::move(*file));
            }
            return out;
        }
        return {};
    }

    auto local_node::impl::manifest_state(const std::string& version_id, const std::string& file_id) const -> std::optional<std::pair<bool, std::string>> {
        for (auto& [level, data] : manifest_chain(version_id)) {
            statement q{*db, "SELECT present,path FROM manifest_files WHERE version_id=?1 AND file_id=?2"};
            q.text(1, level);
            q.text(2, file_id);
            if (q.next())
                return std::pair{q.number(0) != 0, q.data(1)};
        }
        return {};
    }

    auto local_node::impl::manifest_record(const std::string& version_id, const std::string& file_id) -> std::optional<std::shared_ptr<source_file>> {
        for (auto& [level, data] : manifest_chain(version_id)) {
            statement f{*db, "SELECT present,path,digest,kind,size,executable FROM manifest_files WHERE version_id=?1 AND file_id=?2"};
            f.text(1, level);
            f.text(2, file_id);
            if (!f.next())
                continue;
            if (!f.number(0))
                return std::shared_ptr<source_file>{};
            auto file = std::make_shared<source_file>();
            file->id = file_id;
            file->path = f.data(1);
            file->digest = f.data(2);
            file->kind = f.data(3);
            file->stored_bytes = f.number(4);
            file->executable = f.number(5) != 0;
            file->stored = &causal;
            file->fragments_ready = false;
            return file;
        }
        return std::nullopt;
    }

    void local_node::impl::record_revision(const std::string& version_id, const std::vector<std::string>& frontier, const source_state& state,
                                           const std::string& previous_version) {
        auto root = content_root(state.files);
        bool full = true, parent_usable = false;
        std::string parent;
        std::uint64_t depth = 0, parent_depth = 0;
        std::string parent_root;
        if (!previous_version.empty())
            if (auto data = revision(previous_version); data) {
                parent_root = revision_text(*data, "root");
                parent_depth = revision_number(*data, "depth");
                if (!parent_root.empty()) {
                    parent_usable = true;
                    full = false;
                    parent = previous_version;
                    depth = parent_depth + 1;
                    if (depth >= constants::manifest_chain_limit) {
                        parent.clear();
                        full = true;
                        depth = 0;
                    }
                }
            }
        // A state whose files did not change reuses its parent's manifest without deepening the chain.
        bool recorded = false;
        if (auto existing = revision(version_id); existing)
            recorded = !revision_text(*existing, "root").empty() && revision_text(*existing, "root") == root;
        const bool unchanged = !full && root == parent_root;
        if (unchanged)
            depth = parent_depth;
        // The files this revision writes: a snapshot still stores content only for the files it changed.
        std::set<std::string> affected;
        if (!parent_usable) {
            for (auto& [id, file] : state.files)
                affected.insert(id);
        } else if (auto parent_data = revision(previous_version); parent_data) {
            if (auto* previous_frontier = revision_member(*parent_data, "frontier"); previous_frontier)
                for (auto& id : source.changed_files(string_list(*previous_frontier), frontier))
                    affected.insert(id);
        }
        auto store_blob = [&](const source_file& file) {
            if (file.stored)
                return;
            const auto& content = file.content();
            write_blob(file.digest, file.kind, content.size(), file.kind == "text" ? scalar_count(content) : content.size(), content);
        };
        if (!recorded && (!unchanged || full)) {
            statement drop_files{*db, "DELETE FROM manifest_files WHERE version_id=?1"};
            drop_files.text(1, version_id);
            drop_files.next();
            statement drop_paths{*db, "DELETE FROM manifest_paths WHERE version_id=?1"};
            drop_paths.text(1, version_id);
            drop_paths.next();
            statement file_row{*db, "INSERT OR REPLACE INTO manifest_files(version_id,file_id,present,path,digest,kind,size,executable) "
                                    "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"};
            statement path_row{*db, "INSERT OR REPLACE INTO manifest_paths(version_id,path,file_id,present) VALUES(?1,?2,?3,?4)"};
            auto insert_path = [&](std::string_view path, const std::string& id, bool present) {
                path_row.reset();
                path_row.text(1, version_id);
                path_row.text(2, path);
                path_row.text(3, id);
                path_row.number(4, present ? 1 : 0);
                path_row.next();
                ++manifest_rows_written;
            };
            auto insert_file = [&](const source_file& file, bool present, std::string_view path) {
                file_row.reset();
                file_row.text(1, version_id);
                file_row.text(2, file.id);
                static const std::string none;
                file_row.number(3, present ? 1 : 0);
                file_row.text(4, path);
                file_row.text(5, present ? std::string_view{file.digest} : std::string_view{none});
                file_row.text(6, present ? std::string_view{file.kind} : std::string_view{none});
                file_row.number(7, present ? file.bytes() : 0);
                file_row.number(8, present && file.executable ? 1 : 0);
                file_row.next();
                ++manifest_rows_written;
            };
            if (full) {
                for (auto& [id, file] : state.files)
                    if (file->present) {
                        insert_file(*file, true, file->path);
                        insert_path(file->path, id, true);
                    }
            } else if (!unchanged) {
                for (auto& id : affected) {
                    auto visible = state.files.find(id);
                    auto hidden = state.hidden.find(id);
                    const source_file* file = visible != state.files.end()   ? visible->second.get()
                                              : hidden != state.hidden.end() ? hidden->second.get()
                                                                             : nullptr;
                    auto prior = manifest_state(parent, id);
                    bool present = file && file->present;
                    std::string path = file ? file->path : prior && prior->first ? prior->second : std::string{};
                    if (present) {
                        insert_file(*file, true, path);
                        if (!prior || !prior->first || prior->second != path)
                            insert_path(path, id, true);
                        if (prior && prior->first && prior->second != path)
                            insert_path(prior->second, id, false);
                    } else {
                        insert_file(file ? *file : source_file{.id = id}, false, path);
                        if (prior && prior->first)
                            insert_path(prior->second, id, false);
                        else if (!path.empty())
                            insert_path(path, id, false);
                    }
                }
            }
        }
        // Content is written whenever a file's bytes change, whichever manifest shape records it.
        if (!unchanged)
            for (auto& id : affected) {
                auto visible = state.files.find(id);
                if (visible != state.files.end() && visible->second->present)
                    store_blob(*visible->second);
            }
        value::array activation;
        for (auto& [id, active] : state.activation)
            activation.emplace_back(value::map{{"id", id}, {"value", active}});
        std::vector<std::string> resolutions;
        for (auto& [id, resolved] : state.resolutions)
            if (resolved)
                resolutions.push_back(id);
        auto data = codec::encode(value::map{{"activation", std::move(activation)},
                                             {"conflicts", strings(state.conflicts)},
                                             {"control_conflicts", conflict_array(state.control_conflicts)},
                                             {"dependency_conflicts", conflict_array(state.dependency_conflicts)},
                                             {"depth", depth},
                                             {"file_conflicts", conflict_array(state.file_conflicts)},
                                             {"frontier", strings(frontier)},
                                             {"full", full},
                                             {"parent", parent},
                                             {"path_conflicts", conflict_array(state.path_conflicts)},
                                             {"resolutions", strings(resolutions)},
                                             {"root", root}});
        statement q{*db, "INSERT INTO revisions(id,data) VALUES(?1,?2) ON CONFLICT(id) DO UPDATE SET data=excluded.data"};
        q.text(1, version_id);
        q.bind(2, data);
        q.next();
    }

    void local_node::impl::record_worldline(const std::string& name, const std::vector<std::string>& frontier, bool snapshot) {
        ensure_kernel();
        auto state = source.projection(frontier);
        auto version_id = version(frontier);
        std::string parent, base_version, creator = writer;
        if (auto previous = roots.find(name); !snapshot && previous != roots.end())
            parent = version(previous->second);
        if (name != "main") {
            auto found = registered_lines(name);
            if (!found.empty()) {
                auto& line = get(*found.begin(), "line");
                creator = line.at("creator").text();
                base_version = version(string_list(line.at("base")));
            }
            if (parent.empty())
                parent = base_version;
        }
        // A worldline forked at a version is that version until it writes: its manifest is the base's.
        if (parent != version_id)
            record_revision(version_id, source.frontier(frontier), *state, parent);
        statement q{*db, "INSERT INTO worldlines(name,head_version,base_version,creator,frontier_json) VALUES(?1,?2,?3,?4,?5) ON CONFLICT(name) DO "
                         "UPDATE SET head_version=excluded.head_version, base_version=excluded.base_version, creator=excluded.creator, "
                         "frontier_json=excluded.frontier_json"};
        q.text(1, name);
        q.text(2, version_id);
        q.text(3, base_version);
        q.text(4, creator);
        q.text(5, rfl::json::write(frontier));
        q.next();
    }

    void local_node::impl::root_at(const std::string& name, const std::vector<std::string>& frontier) {
        record_worldline(name, frontier);
        roots[name] = frontier;
    }

    auto local_node::impl::identity() -> node_reply {
        node_reply out;
        out.project = project;
        out.node = writer;
        out.author = writer;
        out.state_snapshot = state_snapshot;
        out.loaded_records = loaded_records;
        out.replayed_events = replayed_events;
        out.snapshot_events = snapshot_events;
        return out;
    }

    auto local_node::impl::view(const std::vector<std::string>& frontier, bool include_text, std::string_view path) -> node_reply {
        if (frontier.empty()) {
            auto out = identity();
            out.version = version(frontier);
            out.source_digest = state_digest(source_state{});
            out.worldlines = worldlines();
            return out;
        }
        ensure_kernel();
        auto out = identity();
        out.frontier = source.frontier(frontier);
        out.version = version(frontier);
        auto append = [&](const std::string& id, const std::string& file_path, const std::string& kind, const std::string& digest, std::uint64_t size,
                          bool executable) {
            std::string shown;
            if (include_text) {
                auto content = read_blob(digest);
                shown = kind == "binary" ? hex(content) : std::move(content);
            }
            out.files.push_back({id, file_path, kind, digest, std::move(shown), size, executable, kind == "binary" ? "hex" : "utf8"});
        };
        // A recorded version answers from its manifest chain and content cache; only an unrecorded selection projects events.
        if (auto data = revision(out.version); data) {
            auto root = revision_text(*data, "root");
            if (!root.empty()) {
                out.conflicts = string_list(*revision_member(*data, "conflicts"));
                out.source_digest = state_digest(root, out.conflicts);
                auto append_file = [&](const std::shared_ptr<const source_file>& file) {
                    append(file->id, file->path, file->kind, file->digest, file->bytes(), file->executable);
                };
                if (path.empty()) {
                    auto files = manifest_files(out.version);
                    if (content_root(files) != root)
                        fail(nodes::code::missing_objects, "version manifest is incomplete; reconstruct from the retained archive");
                    for (auto& [id, file] : files)
                        append_file(file);
                } else {
                    auto files = manifest_entry(out.version, std::string(path));
                    // A point read checks the manifest's size against the immutable content it names.
                    for (auto& file : files) {
                        statement size{*db, "SELECT raw_bytes FROM blobs WHERE digest=?1"};
                        size.text(1, file->digest);
                        if (!size.next() || size.number(0) != file->stored_bytes)
                            fail(nodes::code::missing_objects, "manifest file size differs from its content");
                        append_file(file);
                    }
                }
                out.worldlines = worldlines();
                return out;
            }
        }
        auto state_owner = source.projection(frontier);
        const auto& state = *state_owner;
        out.source_digest = state.digest;
        out.conflicts = state.conflicts;
        auto append_file = [&](const std::string& id, const std::shared_ptr<const source_file>& f) {
            out.files.push_back({id, f->path, f->kind, f->digest, include_text ? (f->kind == "binary" ? hex(f->content()) : f->content()) : "", f->bytes(),
                                 f->executable, f->kind == "binary" ? "hex" : "utf8"});
        };
        if (path.empty()) {
            for (auto& [id, file] : state.files)
                append_file(id, file);
        } else if (auto owners = state.paths.find(std::string(path)); owners != state.paths.end())
            for (auto& id : *owners->second)
                append_file(id, state.files.find(id)->second);
        out.worldlines = worldlines();
        return out;
    }

    auto local_node::impl::compare(const node_request& in, const std::vector<std::string>& heads) -> node_reply {
        within_line(root_line(in.target.empty() ? in.worldline : in.target), in.expected);
        auto current_owner = source.projection(heads);
        auto out = identity();
        out.base_version = version(in.expected);
        out.version = version(heads);
        out.frontier = source.frontier(heads);
        out.source_digest = current_owner->digest;
        out.conflicts = current_owner->conflicts;
        out.changes = in.action == "diff" ? manifest_differences(in.expected, heads) : differences(in.expected, heads, shown{in.max_bytes, in.max_bytes});
        return out;
    }

    auto local_node::impl::manifest_differences(const std::vector<std::string>& before_heads, const std::vector<std::string>& after_heads)
        -> std::vector<source_difference> {
        auto before = source.projection(before_heads), after = source.projection(after_heads);
        auto paths = [](const auto& state) {
            std::map<std::string, const source_file*> out;
            for (const auto& [_, f] : state.files)
                if (f->present)
                    out.emplace(f->path, f.get());
            return out;
        };
        auto old = paths(*before), current = paths(*after);
        std::set<std::string> names;
        for (const auto& [path, _] : old)
            names.insert(path);
        for (const auto& [path, _] : current)
            names.insert(path);
        auto side = [](const source_file* f) -> std::optional<file_view> {
            if (!f)
                return {};
            return file_view{f->id, f->path, f->kind, f->digest, {}, f->bytes(), f->executable, f->kind == "binary" ? "hex" : "utf8"};
        };
        std::vector<source_difference> out;
        for (const auto& path : names) {
            auto a = old.contains(path) ? old.at(path) : nullptr;
            auto b = current.contains(path) ? current.at(path) : nullptr;
            if (a && b && a->digest == b->digest && a->kind == b->kind && a->executable == b->executable)
                continue;
            out.push_back({!a ? "added" : !b ? "removed" : "modified", side(a), side(b)});
        }
        return out;
    }

    auto local_node::impl::differences(const std::vector<std::string>& before_heads, const std::vector<std::string>& after_heads, std::optional<shown> budget)
        -> std::vector<source_difference> {
        auto old_owner = source.projection(before_heads), current_owner = source.projection(after_heads);
        const auto& old = *old_owner;
        const auto& current = *current_owner;
        std::vector<source_difference> out;
        auto make = [&budget](const source_file& f) -> file_view {
            std::string text;
            if (!budget)
                text = f.kind == "binary" ? hex(f.content()) : f.content();
            else if (f.kind == "text" && budget->per_side && budget->total) {
                const auto& all = f.content();
                auto most = std::min<std::uint64_t>({budget->per_side, budget->total, all.size()});
                while (most > 0 && most < all.size() && (static_cast<unsigned char>(all[most]) & 0xC0) == 0x80)
                    --most;
                text = all.substr(0, most);
                budget->total -= most;
            }
            return {f.id, f.path, f.kind, f.digest, std::move(text), f.bytes(), f.executable, f.kind == "binary" ? "hex" : "utf8"};
        };
        for (auto a = old.files.begin(), b = current.files.begin(); a != old.files.end() || b != current.files.end();) {
            const source_file* before = nullptr;
            const source_file* after = nullptr;
            if (b == current.files.end() || (a != old.files.end() && a->first < b->first)) {
                before = a->second.get();
                ++a;
            } else if (a == old.files.end() || b->first < a->first) {
                after = b->second.get();
                ++b;
            } else {
                before = a->second.get();
                after = b->second.get();
                ++a;
                ++b;
            }
            if (before && after && before->path == after->path && before->digest == after->digest && before->executable == after->executable &&
                before->kind == after->kind)
                continue;
            source_difference change;
            change.type = !before ? "added" : !after ? "removed" : before->path != after->path ? "moved" : "modified";
            if (before)
                change.before = make(*before);
            if (after)
                change.after = make(*after);
            out.push_back(std::move(change));
        }
        return out;
    }

    auto local_node::impl::history(const node_request& in, const std::vector<std::string>& heads) -> node_reply {
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
        for (auto& id : selected)
            out.history.push_back(entry_of(id));
        return out;
    }

    auto local_node::impl::entry_of(const std::string& id) -> history_entry {
        auto& b = get(id, "batch");
        auto& caller = b.at("caller_operation").text();
        return {id,
                caller.empty() ? b.at("operation").text() : caller,
                b.at("author").text(),
                b.at("actor").text(),
                b.at("tool").text(),
                b.at("intent").text(),
                get(b.at("result").text(), "blob").at("data").raw(),
                b.at("line").text(),
                string_list(b.at("parents")),
                string_list(b.at("dependencies")),
                b.at("lamport").number()};
    }

    auto local_node::impl::contribution(const node_request& in) -> node_reply {
        if (in.target.empty())
            fail(nodes::code::invalid_record, "a contribution names a history entry");
        ensure_kernel();
        if (!has_object(in.target) || get(in.target).at("kind").text() != "batch")
            fail(nodes::code::missing_objects, "no batch has this history entry");
        auto& b = get(in.target, "batch");
        auto parents = string_list(b.at("parents"));
        auto after = parents;
        after.push_back(in.target);
        after = source.frontier(std::move(after));
        auto out = identity();
        out.history.push_back(entry_of(in.target));
        out.base_version = version(parents);
        out.version = version(after);
        // A shown text shorter than its file's size is cut.
        out.changes = differences(parents, after, shown{in.max_bytes, 16u << 20});
        for (auto& [name, frontier] : worldlines())
            if (out.lines.size() < 1000 && source.includes(frontier, in.target))
                out.lines.push_back({.name = name, .version = version(frontier)});
        return out;
    }

    auto local_node::impl::receipt(const std::string& id, bool replayed) -> node_reply {
        if (id.starts_with("reference:")) {
            statement row{*db, "SELECT data FROM reference_events WHERE id=?1"};
            row.text(1, id.substr(10));
            if (!row.next())
                fail(nodes::code::outcome_unknown, "reference receipt not found");
            auto record = rfl::json::read<reference_record>(row.data(0));
            if (!record)
                fail(nodes::code::storage_failure, "reference receipt is unreadable");
            auto out = identity();
            out.worldline = record->worldline;
            out.version = out.base_version = record->version;
            out.operation = record->operation_id;
            out.receipt = id;
            out.replayed = replayed;
            out.references.push_back(*record);
            return out;
        }
        statement row{*db, "SELECT operation,caller_operation,worldline,result,prior,frontier,version,base_version FROM receipts WHERE id=?1"};
        row.text(1, id);
        if (!row.next())
            fail(nodes::code::outcome_unknown, "original receipt has not been committed");
        auto out = identity();
        out.frontier = string_list(codec::decode(row.data(5)).at("frontier"));
        out.version = row.data(6);
        out.base_version = row.data(7);
        out.worldline = row.data(2);
        for (const auto& event : out.frontier) {
            const auto& batch = get(event);
            if (batch.at("kind").text() != "batch" || batch.at("operation").text() != row.data(0) || batch.at("author").text() != writer)
                continue;
            auto own = version({event});
            statement known{*db, "SELECT 1 FROM revisions WHERE id=?1"};
            known.text(1, own);
            auto found = known.next();
            if (found)
                out.written_version = std::move(own);
            break;
        }
        auto record_state = [&] {
            ensure_kernel();
            auto state_owner = source.projection(out.frontier);
            out.source_digest = state_owner->digest;
            out.conflicts = state_owner->conflicts;
        };
        if (auto data = revision(out.version); data) {
            auto root = revision_text(*data, "root");
            if (!root.empty()) {
                out.conflicts = string_list(*revision_member(*data, "conflicts"));
                out.source_digest = state_digest(root, out.conflicts);
            } else
                record_state();
        } else
            record_state();
        out.receipt = id;
        out.issuer = writer;
        out.author = writer;
        out.operation = row.data(1).empty() ? row.data(0) : row.data(1);
        out.result = get(row.data(3), "blob").at("data").raw();
        out.replayed = replayed;
        return out;
    }

    void local_node::impl::binding(const std::string& operation, const std::string& digest, const std::string& result) {
        statement q{*db, "INSERT INTO bindings(author,operation,digest,receipt) VALUES(?1,?2,?3,?4) ON CONFLICT(author,operation) DO UPDATE SET "
                         "receipt=excluded.receipt WHERE digest=excluded.digest"};
        q.text(1, writer);
        q.text(2, operation);
        q.text(3, digest);
        q.text(4, result);
        q.next();
    }

    auto local_node::impl::executed(const std::string& operation) -> std::optional<std::string> {
        statement q{*db, "SELECT receipt FROM bindings WHERE author=?1 AND operation=?2"};
        q.text(1, writer);
        q.text(2, operation);
        if (q.next() && !q.data(0).empty())
            return q.data(0);
        return {};
    }

    auto local_node::impl::retry(const node_request& in, const std::string& digest) -> std::optional<node_reply> {
        statement q{*db, "SELECT digest,receipt FROM bindings WHERE author=?1 AND operation=?2"};
        q.text(1, writer);
        q.text(2, in.operation);
        if (!q.next())
            return {};
        if (q.data(0) != digest)
            fail(nodes::code::identity_conflict, "operation ID has different input");
        if (q.data(1).empty())
            return {};
        return receipt(q.data(1), true);
    }

    auto local_node::impl::commit(const node_request& in, const std::string& line, const std::string& request_digest, const std::vector<std::string>& prior,
                                  const std::vector<std::string>& frontier, const std::string& event, const std::string& result_id) -> node_reply {
        // The receipt is named by the object the publication created: its batch, or the line of a fork.
        auto id = event.empty() ? line : event;
        statement record{*db, "INSERT INTO receipts(id,worldline,operation,caller_operation,author,result,prior,frontier,version,base_version) "
                              "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)"};
        record.text(1, id);
        record.text(2, in.worldline);
        record.text(3, in.operation);
        record.text(4, in.caller_operation);
        record.text(5, writer);
        record.text(6, result_id);
        record.bind(7, codec::encode(value::map{{"frontier", strings(prior)}}));
        record.bind(8, codec::encode(value::map{{"frontier", strings(frontier)}}));
        record.text(9, version(frontier));
        record.text(10, version(prior));
        record.next();
        binding(in.operation, request_digest, id);
        root_at(in.worldline, frontier);
        return receipt(id, false);
    }

    void local_node::impl::record_refusal(const node_request& in) {
        if (dirty)
            reload();
        auto digest = digest_of(codec::encode(request_body(in, project)));
        transaction tx{*db};
        statement q{*db, "INSERT OR IGNORE INTO bindings(author,operation,digest,receipt) VALUES(?1,?2,?3,'')"};
        q.text(1, writer);
        q.text(2, in.operation);
        q.text(3, digest);
        q.next();
        tx.commit();
    }

    void local_node::impl::interrupt_at(std::string_view boundary) {
        if (auto value = std::getenv("NODE_FAULT"); value && boundary == value)
            std::_Exit(86);
    }
} // namespace nodes
