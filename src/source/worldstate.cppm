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
import json;
import nodestore;

namespace nodes {
    using namespace persistence;

    auto local_node::impl::line_of(const std::string& event) const -> std::string {
        auto& body = get(event);
        if (body.at("kind").text() != "batch")
            fail("worldline_boundary", "selected source is not a source batch");
        return body.at("line").text();
    }

    auto local_node::impl::lines() const -> const line_index& {
        if (!line_names) {
            line_names.emplace();
            for (auto& id : object_ids("line"))
                line_names->names[get(id, "line").at("name").text()].insert(id);
        }
        return *line_names;
    }

    auto local_node::impl::registered_lines(const std::string& name) const -> const std::set<std::string>& {
        static const std::set<std::string> none;
        auto found = lines().names.find(name);
        return found == lines().names.end() ? none : found->second;
    }

    auto local_node::impl::worldlines() const -> const std::map<std::string, std::vector<std::string>>& { return roots; }

    auto local_node::impl::named_line(const std::string& name) const -> std::string {
        if (name == "main")
            return project;
        auto& found = registered_lines(name);
        if (found.empty())
            fail(nodes::code::invalid_record, "worldline not found");
        return *found.begin();
    }

    auto local_node::impl::line_name(const std::string& line) const -> std::string {
        if (line == project)
            return "main";
        for (auto& [name, ids] : lines().names)
            if (ids.contains(line))
                return name;
        fail(nodes::code::invalid_record, "source batch names an unregistered worldline");
    }

    auto local_node::impl::line_base(const std::string& line) const -> std::vector<std::string> {
        return line == project ? std::vector<std::string>{} : string_list(get(line, "line").at("base"));
    }

    // An event belongs to a worldline when the line produced it or the line was forked after it.
    auto local_node::impl::in_line(const std::string& line, const std::string& event) const -> bool {
        return line_of(event) == line || source.includes(line_base(line), event);
    }

    void local_node::impl::within_line(const std::string& line, const std::vector<std::string>& heads) const {
        for (auto& head : heads)
            if (!in_line(line, head))
                fail("worldline_boundary", "selected source belongs to another worldline");
    }

    auto local_node::impl::root_line(const std::string& name) const -> std::string { return named_line(name); }

    // An observation is a state of the line only when it contains the state the line was forked from.
    void local_node::impl::observes_line(const std::string& line, const std::vector<std::string>& observed) const {
        within_line(line, observed);
        for (auto& event : line_base(line))
            if (!source.includes(observed, event))
                fail("stale_frontier", "observed source does not contain the worldline base");
    }

    auto local_node::impl::promotion(const node_request& in, bool include_text) -> std::vector<source_difference> {
        if (in.target.empty() || !in.edits.empty())
            fail(nodes::code::invalid_record, "promote requires a source worldline and no edits");
        auto shared = source.reachable(in.source);
        std::erase_if(shared, [&](const auto& id) { return !source.includes(in.expected, id); });
        auto base = source.projection(source.frontier(std::move(shared)));
        auto incoming = source.projection(in.source);
        auto current = source.projection(in.expected);
        if (!base->conflicts.empty() || !incoming->conflicts.empty() || !current->conflicts.empty())
            fail(nodes::code::source_conflict, "promote requires conflict-free versions");
        auto paths = [](const auto& state) {
            std::map<std::string, const source_file*> out;
            for (const auto& [_, f] : state.files)
                if (f->present)
                    out.emplace(f->path, f.get());
            return out;
        };
        auto before = paths(*base), after = paths(*incoming), target = paths(*current);
        auto equal = [](const source_file* a, const source_file* b) {
            return (!a && !b) || (a && b && a->digest == b->digest && a->kind == b->kind && a->executable == b->executable);
        };
        auto at = [](const auto& files, const auto& path) -> const source_file* {
            auto it = files.find(path);
            return it == files.end() ? nullptr : it->second;
        };
        auto view = [include_text](const source_file* f) -> std::optional<file_view> {
            if (!f)
                return {};
            return file_view{f->id,
                             f->path,
                             f->kind,
                             f->digest,
                             include_text ? (f->kind == "binary" ? hex(f->content()) : f->content()) : std::string{},
                             f->bytes(),
                             f->executable,
                             f->kind == "binary" ? "hex" : "utf8"};
        };
        std::set<std::string> names;
        for (const auto& [name, _] : before)
            names.insert(name);
        for (const auto& [name, _] : after)
            names.insert(name);
        std::vector<source_difference> out;
        for (const auto& path : names) {
            auto old = at(before, path), next = at(after, path), now = at(target, path);
            if (equal(old, next) || equal(now, next))
                continue;
            if (!equal(old, now))
                fail(nodes::code::source_conflict, path + " changed in both worldlines");
            if (next)
                for (const auto& occupied : overlapping_paths(current->paths, path))
                    if (occupied != path && !names.contains(occupied))
                        fail(nodes::code::source_conflict, path + " overlaps " + occupied);
            out.push_back({!now ? "added" : !next ? "removed" : "modified", view(now), view(next)});
        }
        return out;
    }

    auto local_node::impl::effects(const node_request& in, const std::vector<std::string>& heads, const std::string& creating_author) -> value::array {
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
        if (in.action == "promote") {
            std::size_t created = 0;
            for (const auto& change : promotion(in)) {
                if (change.before)
                    property(change.before->id, "presence", false);
                if (!change.after)
                    continue;
                const auto& f = *change.after;
                auto file = (creating_author.empty() ? writer : creating_author) + ":" + in.operation + ":file:" + std::to_string(created++);
                property(file, "path", f.path);
                property(file, "presence", true);
                property(file, "mode", f.executable);
                if (f.kind == "binary")
                    property(file, "binary", digest_of(unhex(f.text)));
                else
                    insert(file, {}, {}, f.text);
            }
            return effects;
        }
        if (in.action == "reverse" || in.action == "redo") {
            if (!source.includes(heads, in.target))
                fail(nodes::code::invalid_record, "compensation target is outside selected history");
            auto selected = source.effect_ids(in.target);
            if (selected.empty())
                fail(nodes::code::invalid_record, "batch has no reversible effects");
            bool desired = in.action == "redo";
            // The contribution must be in the state the compensation leaves: a reversal needs it active, a redo inactive.
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
        std::map<std::string, std::vector<std::pair<std::uint64_t, std::uint64_t>>> edited_spans;
        // A file is placed only on a path that is free once the batch is done: one this batch empties, or one nobody holds.
        std::set<std::string> freed, placed;
        for (auto& e : in.edits) {
            if (!e.document.empty() && e.kind != nodes::edit_kind::edit && e.kind != nodes::edit_kind::write && e.kind != nodes::edit_kind::binary)
                fail(nodes::code::invalid_record, "only a content change names a file identity");
            if (e.kind == nodes::edit_kind::remove || e.kind == nodes::edit_kind::move || e.kind == nodes::edit_kind::recreate || e.kind == nodes::edit_kind::binary_recreate)
                freed.insert(e.path);
            else if (e.kind == nodes::edit_kind::directory_move)
                for (auto it = state.paths.lower_bound(e.path + "/"); it != state.paths.end() && it->first.starts_with(e.path + "/"); ++it)
                    freed.insert(it->first);
        }
        auto claim = [&](const std::string& path) {
            for (const auto& held : overlapping_paths(state.paths, path))
                if (!state.paths.find(held)->second->empty() && !freed.contains(held))
                    fail(nodes::code::source_conflict, path + " overlaps " + held);
            for (const auto& held : overlapping_paths(placed, path))
                fail(nodes::code::source_conflict, path + " overlaps " + held);
            placed.insert(path);
        };
        for (auto& e : in.edits) {
            checked_path(e.path);
            if (e.kind == nodes::edit_kind::edit) {
                if (touched.contains(e.path))
                    fail(nodes::code::invalid_record, "multiple edits of one path require separate observed batches");
                auto& spans = edited_spans[e.path];
                const auto begin = e.offset, end = e.offset + e.count;
                for (const auto& [prev_begin, prev_end] : spans)
                    if (begin <= prev_end && prev_begin <= end)
                        fail(nodes::code::invalid_record, "overlapping or adjacent edits of one path require separate observed batches");
                spans.emplace_back(begin, end);
            } else {
                if (edited_spans.contains(e.path) || !touched.insert(e.path).second)
                    fail(nodes::code::invalid_record, "multiple edits of one path require separate observed batches");
            }
            const bool recreate = e.kind == nodes::edit_kind::recreate || e.kind == nodes::edit_kind::binary_recreate;
            if (e.kind == nodes::edit_kind::create || e.kind == nodes::edit_kind::binary_create || recreate) {
                if (recreate) {
                    if (in.tool != "execute.capture/1")
                        fail(nodes::code::invalid_record, "recreate belongs to worktree capture");
                    property(state.path(e.path).id, "presence", false);
                }
                claim(e.path);
                auto file = (creating_author.empty() ? writer : creating_author) + ":" + in.operation + ":file:" + std::to_string(created++);
                property(file, "path", e.path);
                property(file, "presence", true);
                property(file, "mode", e.executable);
                if (e.kind == nodes::edit_kind::binary_create || e.kind == nodes::edit_kind::binary_recreate)
                    property(file, "binary", digest_of(unhex(e.text)));
                else
                    insert(file, {}, {}, e.text);
                continue;
            }
            if (e.kind == nodes::edit_kind::directory_move) {
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
            std::shared_ptr<const source_file> held;
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
            const bool needs_runs = e.kind == nodes::edit_kind::edit || e.kind == nodes::edit_kind::write;
            if (!selected) {
                if (needs_runs) {
                    // A version manifest names content, not character runs: materialize the file this edit anchors into.
                    held = source.materialize(state.path(e.path).id, heads);
                    selected = held.get();
                } else
                    selected = &state.path(e.path);
            } else if (needs_runs && !selected->fragments_ready) {
                held = source.materialize(selected->id, heads);
                selected = held.get();
            }
            auto& file = *selected;
            if (in.tool == "execute.capture/1" && (e.kind == nodes::edit_kind::write || e.kind == nodes::edit_kind::binary) && e.executable != file.executable)
                property(file.id, "mode", e.executable);
            if (e.kind == nodes::edit_kind::move) {
                checked_path(e.target);
                // A move never replaces a file; a concurrent move onto one path stays a recorded path conflict.
                claim(e.target);
                property(file.id, "path", e.target);
            } else if (e.kind == nodes::edit_kind::remove)
                property(file.id, "presence", false);
            else if (e.kind == nodes::edit_kind::mode)
                property(file.id, "mode", e.executable);
            else if (e.kind == nodes::edit_kind::binary) {
                if (file.kind != "binary")
                    fail(nodes::code::invalid_record, "binary replacement requires a binary document");
                property(file.id, "binary", digest_of(unhex(e.text)));
            } else if (e.kind == nodes::edit_kind::edit || e.kind == nodes::edit_kind::write) {
                if (file.kind != "text")
                    fail(nodes::code::invalid_record, "text edit requires a text document");
                auto offset = e.kind == nodes::edit_kind::write ? 0 : e.offset, count = e.kind == nodes::edit_kind::write ? file.size() : e.count;
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

    auto local_node::impl::publish(const node_request& in) -> node_reply {
        auto declared_tool = tool_name(in.tool, in.action);
        checked_name(in.worldline);
        if (!content::is_hash(in.operation))
            fail(nodes::code::invalid_record, "operation ID must be 32-byte lowercase hex");
        if (in.reference || in.mail || in.after || !in.genesis_nonce.empty() || !in.parts.empty() || !in.configuration.empty() || !in.environment.empty() ||
            in.force || !in.revision.empty() || in.max_bytes != 1024 * 1024 || in.limit != 100)
            fail(nodes::code::invalid_record, "source publication contains fields for another operation");
        if (in.activation && in.action != "resolve")
            fail(nodes::code::invalid_record, "activation choices belong to conflict resolution");
        if (!in.source.empty() && in.action != "promote" && in.action != "fork")
            fail(nodes::code::invalid_record, "source selection belongs to fork or promotion");
        if (in.action == "fork" && (!in.edits.empty() || !in.decisions.empty() || !in.dependencies.empty() || !in.target.empty()))
            fail(nodes::code::invalid_record, "a fork cannot contain source effects");
        if (!in.target.empty() && in.action != "promote" && in.action != "reverse" && in.action != "redo" && in.action != "resolve")
            fail(nodes::code::invalid_record, "unexpected compensation target");
        if (in.action == "resolve" && !in.target.empty() && in.decisions.empty())
            fail(nodes::code::source_conflict, "activation resolution requires observed conflict decisions");
        auto request_digest = digest_of(codec::encode(request_body(in, project)));
        if (auto result = retry(in, request_digest))
            return *result;
        if (!content::is_hash(in.owner))
            fail(nodes::code::invalid_record, "source publication requires an explicit writer identity");
        if (in.owner != writer)
            fail(nodes::code::outcome_unknown, "publication names another writer and its result is unavailable");
        interrupt_at("before_transaction");
        transaction tx{*db};
        refresh_external();
        if (auto result = retry(in, request_digest))
            return *result;
        ensure_kernel();
        dirty = true;
        auto found = roots.find(in.worldline);
        if (in.action == "fork" && (found != roots.end() || in.worldline == "main" || !registered_lines(in.worldline).empty()))
            fail(nodes::code::source_conflict, "worldline already exists");
        std::vector<std::string> prior = found == roots.end() ? std::vector<std::string>{} : found->second;
        // A fork or a promotion selects the current root of its source; a writer may observe an older state of its own worldline.
        if (in.action == "fork" || in.action == "promote"
                ? in.expected != prior
                : in.expected != source.frontier(in.expected) || std::ranges::any_of(in.expected, [&](auto& event) { return !source.includes(prior, event); }))
            fail("stale_frontier", "expected source frontier is not part of this worldline");
        check_execution(in);
        if (in.action == "promote")
            within_line(root_line(in.target), in.source);
        check_scope_leases(in, prior);
        auto heads = prior;
        if (in.action == "apply" && declared_tool == "execute.capture/1" && prior != in.expected) {
            auto now = source.projection(prior);
            auto observed = source.projection(in.expected);
            for (const auto& edit : in.edits) {
                const auto old = observed->paths.find(edit.path), current = now->paths.find(edit.path);
                if (old == observed->paths.end())
                    continue; // The normal destination check handles files created during execution.
                if (old->second->size() != 1 || current == now->paths.end() || *old->second != *current->second)
                    fail(nodes::code::source_conflict, edit.path + " changed while the command ran");
                const auto& id = *old->second->begin();
                const auto& before = *observed->files.at(id);
                const auto& after = *now->files.at(id);
                if (std::tie(before.kind, before.digest, before.executable) != std::tie(after.kind, after.digest, after.executable) ||
                    now->file_conflicts.contains(id) || now->path_conflicts.contains(edit.path))
                    fail(nodes::code::source_conflict, edit.path + " changed while the command ran");
            }
        }
        std::string line;
        if (in.action == "fork") {
            heads = source.frontier(in.source);
            std::set<std::string> observed_lines;
            for (auto& event : heads)
                observed_lines.insert(line_of(event));
            if (observed_lines.size() > 1)
                fail("worldline_boundary", "fork source spans several worldlines");
            auto created = base_body("line", project);
            created["base"] = strings(vector_set(heads));
            created["creator"] = writer;
            created["name"] = in.worldline;
            created["operation"] = in.operation;
            line = put(std::move(created));
        } else {
            line = root_line(in.worldline);
            observes_line(line, in.expected);
            if (in.expected.empty() && !prior.empty())
                fail("stale_frontier", "an edit must observe a state of the worldline");
            heads = in.expected;
        }
        auto result_id = blob(in.result);
        std::string event;
        if (in.action != "fork") {
            std::vector<std::string> required;
            for (auto& edit : in.edits)
                if (edit.kind == nodes::edit_kind::binary || edit.kind == nodes::edit_kind::binary_create || edit.kind == nodes::edit_kind::binary_recreate)
                    required.push_back(blob(unhex(edit.text)));
            if (in.action == "promote") {
                required.insert(required.end(), in.source.begin(), in.source.end());
                for (const auto& change : promotion(in))
                    if (change.after && change.after->kind == "binary")
                        required.push_back(blob(unhex(change.after->text)));
            }
            auto changes = effects(in, heads);
            auto b = base_body("batch", project);
            b["actor"] = in.actor;
            b["author"] = writer;
            b["caller_operation"] = in.caller_operation;
            b["dependencies"] = strings(source.dependencies(heads, changes, in.dependencies));
            b["effect_digest"] = effects_digest(changes);
            b["effects"] = std::move(changes);
            b["intent"] = in.intent;
            b["lamport"] = source.clock(heads);
            b["line"] = line;
            b["operation"] = in.operation;
            b["parents"] = strings(heads);
            b["request_digest"] = request_digest;
            b["required"] = strings(vector_set(std::move(required)));
            b["result"] = result_id;
            b["tool"] = declared_tool;
            event = put(std::move(b));
            // A checkout that changes again while sending can advance its local baseline
            // to its own submitted text without pretending it wrote back concurrent edits. The recorded parent
            // names the version the writer observed, so a cold read of this baseline starts from that manifest.
            statement own_revision{*db, "INSERT OR IGNORE INTO revisions(id,data) VALUES(?1,?2)"};
            own_revision.text(1, version({event}));
            own_revision.bind(2, codec::encode(value::map{{"frontier", strings({event})}, {"parent", version(in.expected)}}));
            own_revision.next();
            heads = prior;
            heads.push_back(event);
            heads = source.frontier(std::move(heads));
            auto projected = source.projection(heads);
            check_limits(in, *source.projection(prior), *projected);
            if (in.action == "promote" && !projected->conflicts.empty())
                fail(nodes::code::source_conflict, "promoted changes conflict with the target source");
            if ((in.action == "reverse" || in.action == "redo") &&
                std::ranges::any_of(projected->conflicts, [](auto& c) { return c.starts_with("dependency:"); }))
                fail("dependency_conflict", "reversal requires a recorded dependency decision");
        }
        // Concurrent creates and moves must not silently replace a path created after the observed snapshot.
        if (in.action == "apply") {
            auto now = source.projection(prior);
            auto observed = source.projection(in.expected);
            auto check_destination = [&](const std::string& path) {
                for (const auto& occupied : overlapping_paths(now->paths, path)) {
                    auto before = observed->paths.find(occupied);
                    for (const auto& id : *now->paths.find(occupied)->second)
                        if (before == observed->paths.end() || !before->second->contains(id))
                            fail(nodes::code::source_conflict, path + " was created since this batch read the source");
                }
            };
            for (auto& e : in.edits) {
                if (e.kind == nodes::edit_kind::create || e.kind == nodes::edit_kind::binary_create)
                    check_destination(e.path);
                else if (e.kind == nodes::edit_kind::move)
                    check_destination(e.target);
                else if (e.kind == nodes::edit_kind::directory_move) {
                    auto prefix = e.path + "/";
                    for (auto it = observed->paths.lower_bound(prefix); it != observed->paths.end() && it->first.starts_with(prefix); ++it)
                        check_destination(e.target + it->first.substr(e.path.size()));
                }
            }
        }
        interrupt_at("after_objects");
        auto out = commit(in, line, request_digest, prior, heads, event, result_id);
        if (in.action == "fork" && !in.task_id.empty())
            bind_task(in);
        save_snapshot();
        out.state_snapshot = state_snapshot;
        out.snapshot_events = snapshot_events;
        out.replayed_events = replayed_events;
        interrupt_at("before_commit");
        check_execution(in);
        check_scope_leases(in, prior);
        if (in.action == "apply") {
            out.reach = claim_scopes(in, heads);
            statement reach{*db, "UPDATE receipts SET reach=?1 WHERE id=?2"};
            reach.text(1, out.reach);
            reach.text(2, out.receipt);
            reach.next();
        }
        tx.commit();
        dirty = false;
        interrupt_at("after_commit");
        return out;
    }

    auto local_node::impl::resolve_revision(const std::string& line, const std::string& selection) -> std::string {
        if (selection.empty())
            return selection;
        statement row{*db, "SELECT e.data FROM named_refs r JOIN reference_events e ON e.id=r.event WHERE r.line=?1 AND r.name=?2"};
        row.text(1, line);
        row.text(2, selection.starts_with('@') ? selection.substr(1) : selection);
        if (row.next()) {
            auto record = json::read<reference_record>(row.data(0));
            if (!record)
                fail(nodes::code::storage_failure, "reference is unreadable");
            return record->version;
        }
        if (selection.starts_with('@'))
            fail(nodes::code::invalid_record, "named reference not found in this worldline");
        return selection;
    }

    auto local_node::impl::reference_call(const node_request& in) -> node_reply {
        auto out = identity();
        out.worldline = in.worldline;
        (void)root_line(in.worldline);
        if (in.action != "reference") {
            if (!in.limit || in.limit > 1000)
                fail(nodes::code::resource_limit, "reference page must contain 1 through 1000 records");
            statement rows{*db,
                           in.action == "references"
                               ? "SELECT e.data FROM named_refs r JOIN reference_events e ON e.id=r.event WHERE r.line=?1 AND e.id>?2 ORDER BY e.id LIMIT ?3"
                               : "SELECT data FROM reference_events WHERE line=?1 AND id>?2 ORDER BY id LIMIT ?3"};
            rows.text(1, in.worldline);
            rows.number(2, in.after);
            rows.number(3, in.limit + 1);
            while (rows.next()) {
                if (out.references.size() == in.limit) {
                    out.truncated = true;
                    break;
                }
                auto record = json::read<reference_record>(rows.data(0));
                if (!record)
                    fail(nodes::code::storage_failure, "reference is unreadable");
                out.references.push_back(*record);
            }
            return out;
        }
        if (!in.reference || in.actor.empty() || !content::is_hash(in.operation) || in.owner != writer || in.revision.empty())
            fail(nodes::code::invalid_record, "reference requires actor, operation, owner and selected version");
        const auto& requested = *in.reference;
        checked_name(requested.name);
        if (content::is_hash(requested.name) || (requested.kind != "tag" && requested.kind != "channel"))
            fail(nodes::code::invalid_record, "reference needs a readable name and tag or channel kind");
        if (requested.attestations.size() > 100 || in.intent.size() > 65536)
            fail(nodes::code::resource_limit, "reference metadata is too large");
        for (const auto& item : requested.attestations)
            if ((item.kind != "build" && item.kind != "test" && item.kind != "review") || item.conclusion.empty() || item.conclusion.size() > 16384 ||
                item.receipt.size() > 1024)
                fail(nodes::code::invalid_record, "attestation needs build, test or review kind and a conclusion");
        // Bind the caller's selection before resolving a moving channel so replay returns the original target.
        auto digest = digest_of(json::write(std::tuple{in.action, in.worldline, in.revision, in.actor, in.intent, requested}));
        transaction tx{*db};
        refresh_external();
        if (auto previous = retry(in, digest))
            return *previous;
        ensure_kernel();
        auto heads = selected_heads(in);
        auto state = source.projection(heads);
        if (!state->conflicts.empty())
            fail(nodes::code::source_conflict, "resolve source conflicts before naming a version");
        std::string previous;
        statement current{*db, "SELECT e.data FROM named_refs r JOIN reference_events e ON e.id=r.event WHERE r.line=?1 AND r.name=?2"};
        current.text(1, in.worldline);
        current.text(2, requested.name);
        if (current.next()) {
            auto record = json::read<reference_record>(current.data(0));
            if (!record)
                fail(nodes::code::storage_failure, "reference is unreadable");
            if (record->kind != requested.kind)
                fail(nodes::code::source_conflict, "reference kind cannot change from " + record->kind + " to " + requested.kind);
            if (record->kind == "tag")
                fail(nodes::code::source_conflict, "immutable tag already exists");
            previous = record->receipt;
        }
        if (previous != requested.expected)
            fail("stale_reference", "reference moved; read its current receipt before updating");
        reference_record record{.name = requested.name,
                                .kind = requested.kind,
                                .worldline = in.worldline,
                                .version = version(heads),
                                .actor = in.actor,
                                .intent = in.intent,
                                .operation_id = in.caller_operation.empty() ? in.operation : in.caller_operation,
                                .previous = previous};
        record.created_at =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        record.attestations = requested.attestations;
        statement append{*db, "INSERT INTO reference_events(line,name,data) VALUES(?1,?2,'')"};
        append.text(1, in.worldline);
        append.text(2, requested.name);
        append.next();
        record.sequence = static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db->handle));
        record.receipt = "reference:" + std::to_string(record.sequence);
        statement save{*db, "UPDATE reference_events SET data=?1 WHERE id=?2"};
        save.text(1, json::write(record));
        save.number(2, record.sequence);
        save.next();
        statement point{*db, "INSERT INTO named_refs(line,name,event) VALUES(?1,?2,?3) ON CONFLICT(line,name) DO UPDATE SET event=excluded.event"};
        point.text(1, in.worldline);
        point.text(2, record.name);
        point.number(3, record.sequence);
        point.next();
        binding(in.operation, digest, record.receipt);
        check_execution(in);
        tx.commit();
        return receipt(record.receipt, false);
    }

    auto local_node::impl::selected_heads(const node_request& in) -> std::vector<std::string> {
        auto found = roots.find(in.worldline);
        if (found == roots.end() && in.worldline != "main")
            fail(nodes::code::invalid_record, "worldline not found");
        auto line = root_line(in.worldline);
        std::vector<std::string> heads = found == roots.end() ? std::vector<std::string>{} : found->second;
        if (!in.revision.empty()) {
            statement revision{*db, "SELECT data FROM revisions WHERE id=?1"};
            revision.text(1, resolve_revision(in.worldline, in.revision));
            if (!revision.next())
                fail(nodes::code::invalid_record, "revision not found");
            heads = string_list(codec::decode(revision.data(0)).at("frontier"));
            within_line(line, heads);
        }
        return heads;
    }

    auto local_node::impl::content(const node_request& in) -> node_reply {
        if (in.parts.empty() || in.parts.size() > content_parts)
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

    auto local_node::impl::mail(const node_request& in) -> node_reply {
        auto out = identity();
        auto read_message = [](statement& row) -> nodes::mail_message {
            return {.id = row.number(0),
                    .at = row.number(1),
                    .sender = row.data(2),
                    .worldline = row.data(3),
                    .recipient = row.data(4),
                    .body = row.data(5),
                    .operation = row.data(6)};
        };
        if (in.action == "mail_list") {
            if (in.target.empty() || in.target.size() > 256 || !in.limit || in.limit > 100)
                fail(nodes::code::invalid_record, "d-mail reads require a recipient and a limit of 1 through 100");
            statement rows{*db, "SELECT id,at,sender,line,recipient,body,operation FROM dmail WHERE recipient=?1 AND id>?2 ORDER BY id LIMIT ?3"};
            rows.text(1, in.target);
            rows.number(2, in.after);
            rows.number(3, in.limit + 1);
            std::uint64_t bytes = 0;
            while (rows.next()) {
                auto message = read_message(rows);
                if (out.messages.size() == in.limit || bytes + message.body.size() > 4 * 1024 * 1024) {
                    out.truncated = true;
                    break;
                }
                bytes += message.body.size();
                out.messages.push_back(std::move(message));
            }
            return out;
        }
        if (!in.mail || in.actor.empty() || !content::is_hash(in.operation))
            fail(nodes::code::invalid_record, "d-mail requires an authenticated sender, operation ID, recipient and body");
        const auto& value = *in.mail;
        if (value.recipient.empty() || value.recipient.size() > 256 || value.body.empty() || value.body.size() > 1024 * 1024 ||
            value.recipient.find('\0') != std::string::npos || value.body.find('\0') != std::string::npos)
            fail(nodes::code::invalid_record, "d-mail recipient must be 1 through 256 bytes and body 1 byte through 1 MiB of text");
        (void)scalars(value.recipient);
        (void)scalars(value.body);
        (void)root_line(in.worldline);
        transaction tx{*db};
        statement prior{*db, "SELECT id,at,sender,line,recipient,body,operation FROM dmail WHERE sender=?1 AND operation=?2"};
        prior.text(1, in.actor);
        prior.text(2, in.operation);
        if (prior.next()) {
            auto previous = read_message(prior);
            if (previous.worldline != in.worldline || previous.recipient != value.recipient || previous.body != value.body)
                fail(nodes::code::source_conflict, "d-mail operation ID was already used with another message");
            out.messages.push_back(std::move(previous));
            out.replayed = true;
            return out;
        }
        auto at = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        statement insert{*db, "INSERT INTO dmail(at,sender,line,recipient,body,operation) VALUES(?1,?2,?3,?4,?5,?6)"};
        insert.number(1, at);
        insert.text(2, in.actor);
        insert.text(3, in.worldline);
        insert.text(4, value.recipient);
        insert.text(5, value.body);
        insert.text(6, in.operation);
        insert.next();
        auto id = static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db->handle));
        tx.commit();
        out.messages.push_back(
            {.id = id, .at = at, .sender = in.actor, .worldline = in.worldline, .recipient = value.recipient, .body = value.body, .operation = in.operation});
        return out;
    }

    auto local_node::impl::worldline_list() -> node_reply {
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

} // namespace nodes
