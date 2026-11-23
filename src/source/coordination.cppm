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
    auto local_node::impl::coordination(std::string_view name) -> std::optional<value> {
        statement q{*db, "SELECT data FROM metadata WHERE name=?1"};
        q.text(1, name);
        if (!q.next())
            return {};
        return codec::decode(q.data(0));
    }

    auto local_node::impl::execution_record(const std::string& token) -> value {
        auto record = coordination("run:" + token);
        if (!record || !record->at("active").boolean())
            fail("execution_expired", "execution is absent or revoked");
        auto task = coordination("task:" + record->at("task").text());
        if (!task || !task->at("active").boolean())
            fail("execution_expired", "task is closed");
        return *record;
    }

    void local_node::impl::check_execution(const node_request& in) {
        if (in.execution_token.empty())
            return;
        auto record = execution_record(in.execution_token);
        if (record.at("line").text() != in.worldline || record.at("run").text() != in.run_id)
            fail(nodes::code::permission_denied, "execution belongs to another worldline or run");
        static const std::set<std::string> reads{"tree",       "head",           "stat",         "read",           "inspect",
                                                 "compare",    "history",        "receipt",      "content",        "contribution",
                                                 "scope_list", "scope_register", "scope_locate", "scope_classify", "references",
                                                 "reference_history"};
        static const std::set<std::string> writes{"apply", "scope_acquire", "scope_renew", "scope_release", "reference"};
        if (!reads.contains(in.action) && !(record.at("writable").boolean() && writes.contains(in.action)))
            fail(nodes::code::permission_denied, "operation is outside the execution grant");
    }

    auto local_node::impl::execution_reply(const value& record, bool secret) -> node_reply {
        auto out = identity();
        out.worldline = record.at("line").text();
        out.execution = execution_view{.task = record.at("task").text(),
                                       .run = record.at("run").text(),
                                       .worldline = out.worldline,
                                       .token = secret ? record.at("token").text() : "",
                                       .identity = secret ? record.at("identity").text() : "",
                                       .active = record.at("active").boolean()};
        return out;
    }

    void local_node::impl::bind_task(const node_request& in) {
        checked_name(in.task_id);
        if (in.execution_owner.empty())
            fail(nodes::code::permission_denied, "task requires an owner");
        if (auto old = coordination("task:" + in.task_id)) {
            if (old->at("owner").text() != in.execution_owner || old->at("line").text() != in.worldline || old->at("operation").text() != in.operation)
                fail(nodes::code::identity_conflict, "task already has another binding");
            return;
        }
        metadata("task:" + in.task_id,
                 codec::encode(value::map{{"line", in.worldline}, {"owner", in.execution_owner}, {"active", true}, {"operation", in.operation}}));
    }

    auto local_node::impl::task_call(const node_request& in) -> node_reply {
        transaction tx{*db};
        refresh_external();
        if (in.action == "task_lookup")
            return execution_reply(execution_record(in.execution_token), true);
        if (!in.execution_token.empty())
            fail(nodes::code::permission_denied, "execution credentials cannot administer tasks");
        checked_name(in.task_id);
        if (in.action == "task_create") {
            if (!roots.contains(in.worldline) && in.worldline != "main")
                fail(nodes::code::invalid_record, "worldline not found");
            if (in.operation.empty())
                fail(nodes::code::invalid_record, "task creation requires operation_id");
            bind_task(in);
            tx.commit();
            auto out = identity();
            out.worldline = in.worldline;
            out.execution = execution_view{.task = in.task_id, .worldline = in.worldline, .active = coordination("task:" + in.task_id)->at("active").boolean()};
            return out;
        }
        auto task = coordination("task:" + in.task_id);
        if (!task || task->at("owner").text() != in.execution_owner)
            fail(nodes::code::permission_denied, "task belongs to another caller or does not exist");
        if (in.action == "task_start") {
            if (!task->at("active").boolean())
                fail("execution_expired", "task is closed");
            if (in.operation.empty() || in.execution_identity.empty())
                fail(nodes::code::invalid_record, "start requires identity and operation_id");
            auto retry = coordination("run-operation:" + in.operation);
            if (retry) {
                auto record = coordination("run:" + retry->at("token").text());
                if (!record || record->at("task").text() != in.task_id || record->at("identity").text() != in.execution_identity ||
                    record->at("writable").boolean() != in.execution_writable)
                    fail(nodes::code::identity_conflict, "execution operation has different input");
                return execution_reply(*record, true);
            }
            auto token = hex(random_bytes(32)), run = hex(random_bytes(16));
            value record = value::map{{"task", in.task_id},
                                      {"run", run},
                                      {"token", token},
                                      {"line", task->at("line")},
                                      {"identity", in.execution_identity},
                                      {"active", true},
                                      {"writable", in.execution_writable}};
            metadata("run:" + token, codec::encode(record));
            metadata("run-operation:" + in.operation, codec::encode(value::map{{"token", token}}));
            tx.commit();
            return execution_reply(record, true);
        }
        if (in.action != "task_stop" && in.action != "task_close")
            fail(nodes::code::invalid_record, "unknown task action");
        if (in.action == "task_stop" && in.run_id.empty())
            fail(nodes::code::invalid_record, "stop requires run");
        std::vector<std::pair<std::string, value>> updates;
        std::set<std::string> stopped;
        {
            statement q{*db, "SELECT name,data FROM metadata WHERE name LIKE 'run:%'"};
            while (q.next()) {
                auto record = codec::decode(q.data(1));
                if (record.at("task").text() != in.task_id || (in.action == "task_stop" && record.at("run").text() != in.run_id))
                    continue;
                stopped.insert(record.at("run").text());
                std::get<value::map>(record.data)["active"] = false;
                updates.emplace_back(q.data(0), std::move(record));
            }
        }
        if (in.action == "task_stop" && stopped.empty())
            fail(nodes::code::invalid_record, "run not found in task");
        for (const auto& [name, record] : updates)
            metadata(name, codec::encode(record));
        if (in.action == "task_close") {
            std::get<value::map>(task->data)["active"] = false;
            metadata("task:" + in.task_id, codec::encode(*task));
        }
        auto leases = scope_leases();
        std::erase_if(leases, [&](const auto& lease) { return stopped.contains(lease.at("owner").text()); });
        metadata("scope_leases", codec::encode(value::map{{"leases", leases}}));
        tx.commit();
        auto out = identity();
        out.worldline = task->at("line").text();
        out.execution = execution_view{.task = in.task_id, .run = in.run_id, .worldline = out.worldline, .active = false};
        return out;
    }

    auto local_node::impl::lease_now() -> std::uint64_t {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    auto local_node::impl::scope_leases() -> value::array {
        statement q{*db, "SELECT data FROM metadata WHERE name='scope_leases'"};
        if (!q.next())
            return {};
        auto records = codec::decode(q.data(0)).at("leases").list();
        std::erase_if(records, [](const auto& v) { return v.at("expires").number() <= lease_now(); });
        return records;
    }

    // The range between a record's two anchor characters in one file: the
    // left anchor precedes it and the right anchor is its end.
    auto anchored(const value& record, const source_file& file) -> std::optional<semantic::place> {
        auto left = element::decode(record.at("left")), right = element::decode(record.at("right"));
        // A body keeps its own first and last characters (its braces), so
        // text inserted just outside it never joins it.
        if (record.at("kind").text() == "body") {
            auto first = file.position(left), last = file.position(right);
            if (!first || !last || *first > *last)
                return {};
            return semantic::place{file.path, *first, *last + 1};
        }
        auto begin = left.run.empty() ? std::optional<std::uint64_t>{0} : file.position(left);
        auto end = right.run.empty() ? std::optional<std::uint64_t>{file.size()} : file.position(right);
        if (!begin || !end)
            return {};
        if (!left.run.empty())
            ++*begin;
        if (*begin > *end)
            return {};
        return semantic::place{file.path, *begin, *end};
    }
    auto range_anchors(const source_file& file, std::uint64_t begin, std::uint64_t end) -> std::pair<value, value> {
        return {begin ? file.at(begin - 1).encoded() : element{}.encoded(), end < file.size() ? file.at(end).encoded() : element{}.encoded()};
    }
    auto holder_name(std::string owner) -> std::string {
        std::ranges::replace(owner, '\n', ' ');
        return owner;
    }

    auto local_node::impl::lease_range(const value& lease, const source_state& state) -> std::optional<semantic::place> {
        if (lease.at("kind").text() == "entry")
            return semantic::place{lease.at("path").text(), 0, 0};
        auto found = state.files.find(lease.at("file").text());
        if (found == state.files.end() || !found->second->present)
            return {};
        auto held = materialize(state, *found->second);
        const auto& file = held ? *held : *found->second;
        if (lease.at("kind").text() == "file")
            return semantic::place{file.path, 0, file.kind == "text" ? file.size() : 0};
        return anchored(lease, file);
    }

    static auto scope_label(const value& lease) -> std::string { return lease.at("scope").text(); }

    auto local_node::impl::lease_reply(const value& lease, const source_state& state, bool reveal) -> scope_lease {
        auto range = lease_range(lease, state);
        return {.token = reveal ? lease.at("token").text() : "",
                .scope = scope_label(lease),
                .path = range ? range->path : lease.at("path").text(),
                .worldline = lease.at("line").text(),
                .version = version(roots.at(lease.at("line").text())),
                .owner = lease.at("owner").text(),
                .begin = range ? range->begin : 0,
                .end = range ? range->end : 0,
                .expires_at = lease.at("expires").number(),
                .valid = range.has_value(),
                .implicit = lease.at("implicit").boolean(),
                .kind = lease.at("kind").text()};
    }

    auto local_node::impl::scope_references() -> value::array {
        auto saved = coordination("scope_references");
        auto records = saved ? saved->at("references").list() : value::array{};
        std::erase_if(records, [](const auto& r) { return r.at("expires").number() <= lease_now(); });
        return records;
    }

    auto local_node::impl::register_scopes(const node_request& in) -> node_reply {
        transaction tx{*db};
        refresh_external();
        check_execution(in);
        ensure_kernel();
        auto selected = selected_heads(in);
        auto state = source.projection(selected);
        auto records = scope_references();
        if (in.analyzed_scopes.size() + in.body_ranges.size() + in.scope_hints.ffi_boundaries.size() > constants::scope_reference_limit)
            fail(nodes::code::resource_limit, "too many scope references in one batch");
        auto out = identity();
        out.worldline = in.worldline;
        out.version = version(selected);
        out.scopes = in.analyzed_scopes;
        if (in.declaration_ranges.size() > constants::scope_reference_limit)
            fail(nodes::code::resource_limit, "too many declaration anchors in one batch");
        for (const auto& [id, range] : in.declaration_ranges) {
            const auto& stub = state->path(range.path);
            auto held = materialize(*state, stub);
            const auto& file = held ? *held : stub;
            if (file.kind != "text" || range.begin >= range.end || range.end > file.size())
                fail(nodes::code::invalid_record, "analysis returned an invalid declaration range");
            auto element = file.at(range.begin);
            out.declaration_anchors.emplace(id, semantic::source_anchor{.file = file.id, .declaration = element.run + ":" + std::to_string(element.offset)});
        }
        // A reference keeps the characters around its range, so later
        // versions resolve it without analysis. Analyzing the same version
        // again refreshes its reference instead of adding another.
        auto refer = [&](std::string kind, const std::string& scope, std::string scope_kind, const semantic::place& range, bool sealed = false) {
            const auto& stub = state->path(range.path);
            auto held = materialize(*state, stub);
            const auto& file = held ? *held : stub;
            if (file.kind != "text" || range.begin >= range.end || range.end > file.size())
                fail(nodes::code::invalid_record, "analysis returned an invalid " + kind + " range");
            auto [left, right] =
                kind == "body" ? std::pair{file.at(range.begin).encoded(), file.at(range.end - 1).encoded()} : range_anchors(file, range.begin, range.end);
            auto same = std::ranges::find_if(records, [&](const auto& r) {
                return r.at("kind").text() == kind && r.at("line").text() == in.worldline && r.at("version").text() == out.version &&
                       r.at("file").text() == file.id && r.at("scope").text() == scope && r.at("left") == left && r.at("right") == right;
            });
            if (same != records.end()) {
                const auto& existing = std::get<value::map>(same->data);
                if (auto found = existing.find("sealed"); found != existing.end() && found->second.boolean())
                    sealed = true;
            }
            value record = value::map{{"kind", kind},
                                      {"line", in.worldline},
                                      {"version", out.version},
                                      {"configuration", in.configuration},
                                      {"environment", in.environment},
                                      {"scope", scope},
                                      {"scope_kind", std::move(scope_kind)},
                                      {"path", file.path},
                                      {"file", file.id},
                                      {"begin", range.begin},
                                      {"end", range.end},
                                      {"left", left},
                                      {"right", right},
                                      {"sealed", sealed},
                                      {"expires", lease_now() + constants::scope_max_seconds}};
            auto ref = same == records.end() ? hex(random_bytes(32)) : same->at("ref").text();
            std::get<value::map>(record.data)["ref"] = ref;
            if (same == records.end())
                records.push_back(std::move(record));
            else
                *same = std::move(record);
            return ref;
        };
        for (auto& scope : out.scopes)
            if (scope.location.source)
                scope.reference = refer("scope", scope.id, std::string{scope.kind.name()}, *scope.location.source);
        for (const auto& [symbol, range] : in.body_ranges)
            refer("body", symbol, "", range, in.scope_hints.sealed_bodies.contains(symbol));
        for (const auto& [symbol, range] : in.scope_hints.ffi_boundaries)
            refer("ffi_boundary", symbol, "", range, false);
        // Registration also happens silently on reads, so the table drops
        // the references that would expire first instead of refusing one.
        if (records.size() > constants::scope_reference_limit) {
            std::ranges::stable_sort(records, {}, [](const auto& r) { return r.at("expires").number(); });
            records.erase(records.begin(), records.end() - static_cast<std::ptrdiff_t>(constants::scope_reference_limit));
        }
        metadata("scope_references", codec::encode(value::map{{"references", records}}));
        tx.commit();
        return out;
    }

    auto local_node::impl::locate_scopes(const node_request& in) -> node_reply {
        ensure_kernel();
        auto selected = selected_heads(in);
        auto state = source.projection(selected);
        auto references = scope_references();
        auto out = identity();
        out.worldline = in.worldline;
        out.version = version(selected);
        for (const auto& ref : in.scope_refs) {
            auto found = std::ranges::find_if(references, [&](const auto& r) { return r.at("ref").text() == ref; });
            if (found == references.end())
                fail("scope_stale", "scope reference is absent or expired; query language(scopes)");
            if (found->at("line").text() != in.worldline)
                fail(nodes::code::permission_denied, "scope reference belongs to another worldline");
            auto file = state->files.find(found->at("file").text());
            if (file == state->files.end() || !file->second->present)
                fail("scope_stale", "the scope's file is not in this version");
            auto held = materialize(*state, *file->second);
            auto range = anchored(*found, held ? *held : *file->second);
            if (!range)
                fail("scope_stale", "the scope's boundaries are gone in this version; query language(scopes)");
            out.located.emplace(ref, *range);
        }
        return out;
    }

    // Compute visible character intervals [begin, end) in `left` whose CRDT elements do not appear in `right`.
    static auto missing_intervals(const source_file& left, const source_file& right) -> std::vector<std::pair<std::uint64_t, std::uint64_t>> {
        std::unordered_map<std::string, std::vector<std::pair<std::uint64_t, std::uint64_t>>> present;
        for (const auto& f : right.fragments)
            present[f.run].emplace_back(f.first, f.first + f.count);
        for (auto& [_, spans] : present)
            std::ranges::sort(spans);

        std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
        std::uint64_t pos = 0;
        for (const auto& f : left.fragments) {
            const auto f_begin = f.first, f_end = f.first + f.count;
            auto found = present.find(f.run);
            if (found == present.end()) {
                out.emplace_back(pos, pos + f.count);
                pos += f.count;
                continue;
            }
            std::uint64_t cursor = f_begin;
            for (const auto& [s_begin, s_end] : found->second) {
                if (s_end <= cursor)
                    continue;
                if (s_begin >= f_end)
                    break;
                if (s_begin > cursor)
                    out.emplace_back(pos + (cursor - f_begin), pos + (s_begin - f_begin));
                cursor = std::max(cursor, std::min(s_end, f_end));
            }
            if (cursor < f_end)
                out.emplace_back(pos + (cursor - f_begin), pos + f.count);
            pos += f.count;
        }
        return out;
    }

    auto local_node::impl::classify_scopes(const node_request& in) -> node_reply {
        if (in.target.empty())
            fail(nodes::code::invalid_record, "scope_classify requires a base revision in target");
        ensure_kernel();
        auto to_heads = selected_heads(in);
        auto base_id = resolve_revision(in.worldline, in.target);
        statement revision{*db, "SELECT data FROM revisions WHERE id=?1"};
        revision.text(1, base_id);
        if (!revision.next())
            fail(nodes::code::invalid_record, "base revision not found");
        auto from_heads = string_list(codec::decode(revision.data(0)).at("frontier"));
        if (version(from_heads) != base_id)
            fail(nodes::code::invalid_record, "base revision does not identify the selected frontier");
        within_line(root_line(in.worldline), from_heads);

        auto out = identity();
        out.worldline = in.worldline;
        out.version = version(to_heads);
        scope_classification result{.body_only = true, .sealed_body_only = true, .comment_or_whitespace_only = true, .ffi_boundary_touched = false};

        if (from_heads == to_heads) {
            out.classification = std::move(result);
            out.reprojected.reserve(in.scope_hints.reproject_places.size());
            for (const auto& place : in.scope_hints.reproject_places)
                out.reprojected.push_back(place);
            return out;
        }

        auto from_state = source.projection(from_heads);
        auto to_state = source.projection(to_heads);
        auto all_references = scope_references();
        std::unordered_map<std::string_view, std::vector<const value*>> references_by_file;
        for (const auto& r : all_references)
            if (r.at("line").text() == in.worldline)
                references_by_file[r.at("file").text()].push_back(&r);

        std::map<std::string, std::shared_ptr<const source_file>> before_materialized, after_materialized;
        auto get_before = [&](const source_file& stub) -> const source_file& {
            auto [it, inserted] = before_materialized.try_emplace(stub.id);
            if (inserted)
                it->second = materialize(*from_state, stub);
            return it->second ? *it->second : stub;
        };
        auto get_after = [&](const source_file& stub) -> const source_file& {
            auto [it, inserted] = after_materialized.try_emplace(stub.id);
            if (inserted)
                it->second = materialize(*to_state, stub);
            return it->second ? *it->second : stub;
        };

        std::set<std::string> touched_files, touched_symbols, touched_ffi_symbols;
        auto mark_declaration = [&] {
            result.body_only = false;
            result.sealed_body_only = false;
        };
        auto mark_non_comment = [&] {
            result.comment_or_whitespace_only = false;
        };

        for (const auto& [id, after_stub] : to_state->files) {
            if (!after_stub->present)
                continue;
            const auto before_it = from_state->files.find(id);
            if (before_it == from_state->files.end() || !before_it->second->present) {
                touched_files.insert(after_stub->path);
                mark_declaration();
                mark_non_comment();
                continue;
            }
            const auto& before_stub = *before_it->second;
            if (before_stub.path != after_stub->path || before_stub.kind != after_stub->kind || before_stub.executable != after_stub->executable) {
                touched_files.insert(before_stub.path);
                touched_files.insert(after_stub->path);
                mark_declaration();
                mark_non_comment();
                continue;
            }
            if (before_stub.digest == after_stub->digest)
                continue;
            touched_files.insert(after_stub->path);
            if (after_stub->kind != "text") {
                mark_declaration();
                mark_non_comment();
                continue;
            }
            const auto& before = get_before(before_stub);
            const auto& after = get_after(*after_stub);
            const auto deleted_spans = missing_intervals(before, after);
            const auto inserted_spans = missing_intervals(after, before);

            const auto& references = references_by_file[id];
            for (const auto* reference : references) {
                const auto& r = *reference;
                if (r.at("kind").text() != "ffi_boundary")
                    continue;
                auto f_before = anchored(r, before);
                auto f_after = anchored(r, after);
                if (!f_before || !f_after) {
                    result.ffi_boundary_touched = true;
                    touched_ffi_symbols.insert(r.at("scope").text());
                    continue;
                }
                auto overlaps_any = [](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& spans, const semantic::place& range) {
                    return std::ranges::any_of(spans, [&](const auto& seg) { return seg.first < range.end && range.begin < seg.second; });
                };
                if (overlaps_any(deleted_spans, *f_before) || overlaps_any(inserted_spans, *f_after)) {
                    result.ffi_boundary_touched = true;
                    touched_ffi_symbols.insert(r.at("scope").text());
                }
            }

            // Without a syntax tree no text change can be proven comment-only, and a file
            // without registered references has no known bodies.
            mark_non_comment();
            if (references.empty()) {
                mark_declaration();
                continue;
            }

            struct live_body {
                std::string symbol;
                semantic::place before, after;
                bool sealed = false;
            };
            std::vector<live_body> bodies;
            for (const auto* reference : references) {
                const auto& r = *reference;
                if (r.at("kind").text() != "body")
                    continue;
                auto b_before = anchored(r, before);
                auto b_after = anchored(r, after);
                if (!b_before || !b_after)
                    continue;
                if (b_before->end <= b_before->begin + 1 || b_after->end <= b_after->begin + 1)
                    continue;
                bool sealed = false;
                const auto& fields = std::get<value::map>(r.data);
                if (auto s = fields.find("sealed"); s != fields.end())
                    sealed = s->second.boolean();
                bodies.push_back({r.at("scope").text(), *b_before, *b_after, sealed});
            }

            auto check_intervals = [&](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& spans, bool is_after) {
                for (const auto& [seg_begin, seg_end] : spans) {
                    bool covered = false;
                    bool sealed_covered = false;
                    for (const auto& b : bodies) {
                        const auto& range = is_after ? b.after : b.before;
                        if (seg_begin > range.begin && seg_end <= range.end - 1) {
                            covered = true;
                            touched_symbols.insert(b.symbol);
                            if (b.sealed)
                                sealed_covered = true;
                        }
                    }
                    if (!covered)
                        mark_declaration();
                    else if (!sealed_covered)
                        result.sealed_body_only = false;
                }
            };

            check_intervals(deleted_spans, false);
            check_intervals(inserted_spans, true);
        }
        for (const auto& [id, before_stub] : from_state->files) {
            if (!before_stub->present)
                continue;
            const auto after_it = to_state->files.find(id);
            if (after_it == to_state->files.end() || !after_it->second->present) {
                touched_files.insert(before_stub->path);
                mark_declaration();
                mark_non_comment();
            }
        }

        result.touched_files.assign(touched_files.begin(), touched_files.end());
        result.touched_symbols.assign(touched_symbols.begin(), touched_symbols.end());
        result.touched_ffi_symbols.assign(touched_ffi_symbols.begin(), touched_ffi_symbols.end());
        out.classification = std::move(result);

        out.reprojected.reserve(in.scope_hints.reproject_places.size());
        for (const auto& place : in.scope_hints.reproject_places) {
            if (!touched_files.contains(place.path)) {
                auto found = to_state->paths.find(place.path);
                if (found != to_state->paths.end() && found->second->size() == 1) {
                    out.reprojected.push_back(place);
                    continue;
                }
            }
            auto before_path = from_state->paths.find(place.path);
            if (before_path == from_state->paths.end() || before_path->second->size() != 1) {
                out.reprojected.push_back(std::nullopt);
                continue;
            }
            const auto& file_id = *before_path->second->begin();
            auto before_it = from_state->files.find(file_id);
            auto after_it = to_state->files.find(file_id);
            if (before_it == from_state->files.end() || !before_it->second->present || after_it == to_state->files.end() || !after_it->second->present ||
                before_it->second->kind != "text" || after_it->second->kind != "text") {
                out.reprojected.push_back(std::nullopt);
                continue;
            }
            const auto& before = get_before(*before_it->second);
            const auto& after = get_after(*after_it->second);
            if (place.begin > place.end || place.end > before.size()) {
                out.reprojected.push_back(std::nullopt);
                continue;
            }
            if (place.begin < place.end) {
                auto first = after.position(before.at(place.begin));
                auto last = after.position(before.at(place.end - 1));
                if (first && last && *first <= *last) {
                    out.reprojected.push_back(semantic::place{after.path, *first, *last + 1});
                    continue;
                }
            }
            auto left = place.begin ? after.position(before.at(place.begin - 1)) : std::optional<std::uint64_t>{0};
            auto right = place.end < before.size() ? after.position(before.at(place.end)) : std::optional<std::uint64_t>{after.size()};
            if (!left || !right) {
                out.reprojected.push_back(std::nullopt);
                continue;
            }
            auto begin = place.begin ? *left + 1 : std::uint64_t{0};
            if (begin > *right) {
                out.reprojected.push_back(std::nullopt);
                continue;
            }
            out.reprojected.push_back(semantic::place{after.path, begin, *right});
        }
        return out;
    }

    auto local_node::impl::scope_call(const node_request& in) -> node_reply {
        std::optional<transaction> tx;
        if (in.action != "scope_list") {
            tx.emplace(*db);
            refresh_external();
        }
        ensure_kernel();
        auto root = roots.find(in.worldline);
        if (root == roots.end() && in.worldline != "main")
            fail(nodes::code::invalid_record, "worldline not found");
        auto heads = root == roots.end() ? std::vector<std::string>{} : root->second;
        auto state = source.projection(heads);
        check_execution(in);
        auto records = scope_leases();
        auto out = identity();
        out.worldline = in.worldline;
        out.version = version(heads);
        if (in.action == "scope_list") {
            for (const auto& record : records)
                if (record.at("line").text() == in.worldline)
                    out.leases.push_back(lease_reply(record, *state, false));
            return out;
        }
        if (in.lease_owner.empty())
            fail(nodes::code::permission_denied, "scope lease requires an authenticated caller");
        if (in.action == "scope_acquire") {
            if ((in.scope_refs.empty() && in.scope_files.empty() && in.scope_entries.empty()) || in.operation.empty())
                fail(nodes::code::invalid_record, "acquire requires references and operation_id");
            auto refs = in.scope_refs;
            std::ranges::sort(refs);
            if (std::ranges::adjacent_find(refs) != refs.end())
                fail(nodes::code::invalid_record, "duplicate scope reference");
            auto files = vector_set(in.scope_files), entries = vector_set(in.scope_entries);
            value input = value::map{{"owner", in.lease_owner},     {"line", in.worldline},   {"refs", strings(refs)},      {"files", strings(files)},
                                     {"entries", strings(entries)}, {"version", in.revision}, {"seconds", in.lease_seconds}};
            auto operation_key = "scope-operation:" + in.operation;
            if (auto previous = coordination(operation_key)) {
                if (previous->at("input") != input)
                    fail(nodes::code::identity_conflict, "lease operation_id has different input");
                for (const auto& token : previous->at("tokens").list()) {
                    auto found = std::ranges::find_if(records, [&](const auto& r) { return r.at("token") == token; });
                    if (found == records.end())
                        fail("scope_expired", "original acquisition is no longer live; use a new operation_id");
                    out.leases.push_back(lease_reply(*found, *state, true));
                }
                return out;
            }
            if (!in.lease_seconds || in.lease_seconds > constants::scope_max_seconds)
                fail(nodes::code::invalid_record, "lease seconds must be 1 through 600");
            auto references = scope_references();
            std::vector<std::string> tokens;
            if ((!files.empty() || !entries.empty()) && in.revision != out.version)
                fail("scope_stale", "file and entry leases require the current version");
            auto related = [](const std::string& a, const std::string& b) { return a == b || a.starts_with(b + "/") || b.starts_with(a + "/"); };
            for (auto kind : {std::string{"file"}, std::string{"entry"}}) {
                for (const auto& path : kind == "file" ? files : entries) {
                    checked_path(path);
                    std::string file_id;
                    if (kind == "file")
                        file_id = state->path(path).id;
                    for (const auto& record : records) {
                        if (record.at("line").text() != in.worldline)
                            continue;
                        auto range = lease_range(record, *state);
                        if (related(path, range ? range->path : record.at("path").text()))
                            fail("scope_conflict", "resource overlaps an existing lease");
                    }
                    if (records.size() >= constants::scope_lease_limit)
                        fail(nodes::code::resource_limit, "too many active scope leases");
                    auto token = hex(random_bytes(32));
                    tokens.push_back(token);
                    records.emplace_back(value::map{{"kind", kind},
                                                    {"token", token},
                                                    {"operation", in.operation},
                                                    {"owner", in.lease_owner},
                                                    {"scope", kind + ":" + path},
                                                    {"line", in.worldline},
                                                    {"version", out.version},
                                                    {"path", path},
                                                    {"file", file_id},
                                                    {"expires", lease_now() + in.lease_seconds},
                                                    {"implicit", false}});
                    out.leases.push_back(lease_reply(records.back(), *state, true));
                }
            }
            for (const auto& ref : refs) {
                auto selected = std::ranges::find_if(references, [&](const auto& r) { return r.at("ref").text() == ref; });
                if (selected == references.end())
                    fail("scope_stale", "scope reference is absent or expired; query language(scopes)");
                if (selected->at("line").text() != in.worldline)
                    fail(nodes::code::permission_denied, "scope reference belongs to another worldline");
                if (selected->at("version").text() != out.version)
                    fail("scope_stale", "analyze the current worldline before acquiring scopes");
                const auto& stub = state->path(selected->at("path").text());
                auto held = materialize(*state, stub);
                const auto& file = held ? *held : stub;
                auto begin = selected->at("begin").number(), end = selected->at("end").number();
                if (file.id != selected->at("file").text())
                    fail("scope_stale", "analyzed file identity changed");
                for (const auto& record : records) {
                    if (record.at("line").text() != in.worldline)
                        continue;
                    if (record.at("kind").text() == "entry") {
                        if (related(record.at("path").text(), file.path))
                            fail("scope_conflict", "scope overlaps a path entry lease");
                        continue;
                    }
                    if (record.at("file").text() != file.id)
                        continue;
                    auto range = lease_range(record, *state);
                    if (!range || (begin <= range->end && end >= range->begin))
                        fail("scope_conflict", "scope overlaps " + scope_label(record) + " until " + std::to_string(record.at("expires").number()));
                }
                if (records.size() >= constants::scope_lease_limit)
                    fail(nodes::code::resource_limit, "too many active scope leases");
                auto token = hex(random_bytes(32));
                tokens.push_back(token);
                records.emplace_back(value::map{{"kind", "scope"},
                                                {"token", token},
                                                {"operation", in.operation},
                                                {"owner", in.lease_owner},
                                                {"scope", selected->at("scope")},
                                                {"line", in.worldline},
                                                {"version", out.version},
                                                {"path", file.path},
                                                {"file", file.id},
                                                {"begin", begin},
                                                {"end", end},
                                                {"left", range_anchors(file, begin, end).first},
                                                {"right", range_anchors(file, begin, end).second},
                                                {"expires", lease_now() + in.lease_seconds},
                                                {"implicit", false}});
                out.leases.push_back(lease_reply(records.back(), *state, true));
            }
            metadata(operation_key, codec::encode(value::map{{"input", input}, {"tokens", strings(tokens)}}));
        } else {
            auto found = std::ranges::find_if(
                records, [&](const auto& record) { return record.at("token").text() == in.lease_token && record.at("line").text() == in.worldline; });
            if (found == records.end())
                fail("scope_expired", "lease is absent or expired; acquire a new lease");
            if (found->at("owner").text() != in.lease_owner)
                fail(nodes::code::permission_denied, "scope lease belongs to another caller");
            if (in.action == "scope_release")
                records.erase(found);
            else if (in.action == "scope_renew") {
                if (!in.lease_seconds || in.lease_seconds > constants::scope_max_seconds)
                    fail(nodes::code::invalid_record, "lease seconds must be 1 through 600");
                if (!lease_range(*found, *state))
                    fail("scope_stale", "lease anchors are no longer visible; release and analyze again");
                std::get<value::map>(found->data)["expires"] = lease_now() + in.lease_seconds;
                out.leases.push_back(lease_reply(*found, *state, true));
            } else
                fail(nodes::code::invalid_record, "unknown scope action");
        }
        metadata("scope_leases", codec::encode(value::map{{"leases", records}}));
        tx->commit();
        return out;
    }

    void local_node::impl::check_scope_leases(const node_request& in, const std::vector<std::string>& prior) {
        if (in.action == "fork")
            return;
        auto records = scope_leases();
        std::erase_if(records, [&](const auto& record) { return record.at("line").text() != in.worldline; });
        if (records.empty() && in.leases.empty())
            return;
        if (in.action == "promote") {
            auto checked = in;
            checked.action = "apply";
            for (const auto& change : promotion(in, false)) {
                const auto& file = change.after ? *change.after : *change.before;
                checked.edits.push_back({.kind = change.before ? (change.after ? nodes::edit_kind::write : nodes::edit_kind::remove) : nodes::edit_kind::create, .path = file.path});
            }
            check_scope_leases(checked, prior);
            return;
        }
        auto current = source.projection(prior);
        auto observed = source.projection(in.expected);
        std::unordered_set<std::string_view> requested, available;
        for (const auto& token : in.leases)
            requested.insert(token);
        for (const auto& record : records)
            if (record.at("owner").text() == in.lease_owner)
                available.insert(record.at("token").text());
        for (const auto& token : requested)
            if (!available.contains(token))
                fail("scope_expired", "write requires a live lease owned by this caller");
        // A caller's own leases never stand in its way, and implicit leases
        // guard only text edits, never history actions.
        std::erase_if(records, [&](const auto& record) {
            return (!in.lease_owner.empty() && record.at("owner").text() == in.lease_owner && !requested.contains(record.at("token").text())) ||
                   (record.at("implicit").boolean() && in.action != "apply");
        });
        if (!records.empty() && in.action != "apply")
            fail("scope_conflict", "release scope leases before changing worldline history");

        // These coordinates belong only to this publication's two immutable
        // projections. Never cache scalar offsets across source versions.
        struct protection {
            const value* record;
            std::string_view kind;
            bool held;
            std::optional<semantic::place> current, observed;
            std::string path;
        };
        std::vector<protection> protections;
        protections.reserve(records.size());
        std::map<std::string, std::vector<std::size_t>, std::less<>> owned, edits;
        for (std::size_t i = 0; i < in.edits.size(); ++i) {
            const auto& edit = in.edits[i];
            if (!edit.path.empty())
                edits[edit.path].push_back(i);
            if ((edit.kind == nodes::edit_kind::move || edit.kind == nodes::edit_kind::directory_move) && !edit.target.empty())
                edits[edit.target].push_back(i);
        }
        for (const auto& record : records) {
            auto kind = std::string_view{record.at("kind").text()};
            const bool held = requested.contains(record.at("token").text());
            // Resolve file identity/path before touching sequence anchors.
            // An invalid anchor falls back to the recorded path, while a
            // held lease can also cover an edit at its observed old path.
            bool relevant = !overlapping_paths(edits, record.at("path").text()).empty();
            if (kind != "entry" && !relevant) {
                auto file = current->files.find(record.at("file").text());
                relevant = file != current->files.end() && file->second->present && !overlapping_paths(edits, file->second->path).empty();
                if (held && !relevant) {
                    auto base_file = observed->files.find(record.at("file").text());
                    relevant = base_file != observed->files.end() && base_file->second->present && edits.contains(base_file->second->path);
                }
            }
            if (!relevant)
                continue;
            auto now = lease_range(record, *current);
            auto base = current == observed ? now : lease_range(record, *observed);
            if (held && base && kind != "entry")
                owned[base->path].push_back(protections.size());
            auto path = now ? now->path : record.at("path").text();
            protections.push_back({&record, kind, held, std::move(now), std::move(base), std::move(path)});
        }
        // A batch can present many leases on one file. Its coverage is a
        // property of each edit and the whole held set, computed just once.
        std::vector<std::optional<bool>> coverage(in.edits.size());
        auto covered = [&](std::size_t index) {
            auto& result = coverage[index];
            if (result)
                return *result;
            result = false;
            const auto& edit = in.edits[index];
            auto group = owned.find(edit.path);
            if (group == owned.end())
                return false;
            for (auto id : group->second) {
                const auto& p = protections[id];
                const auto& range = *p.observed;
                if (p.kind == "file" || (edit.kind == nodes::edit_kind::edit ? edit.offset >= range.begin && edit.offset <= range.end && edit.count <= range.end - edit.offset
                                                             : range.begin == 0 && range.end == observed->path(edit.path).size())) {
                    result = true;
                    break;
                }
            }
            return *result;
        };
        for (const auto& p : protections) {
            std::vector<std::size_t> affected;
            // Exact paths, ancestors and descendants include both ends of
            // moves, but never similarly spelled sibling prefixes.
            for (const auto& path : overlapping_paths(edits, p.path)) {
                const auto& group = edits.at(path);
                affected.insert(affected.end(), group.begin(), group.end());
            }
            std::ranges::sort(affected);
            affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
            for (auto index : affected) {
                const auto& edit = in.edits[index];
                if (p.kind == "entry") {
                    if (!p.held)
                        fail("scope_conflict", "edit touches a leased path entry");
                    continue;
                }
                if (!p.current || !p.observed)
                    fail("scope_stale", "edit base predates a leased scope; read the current version");
                if (p.held) {
                    if (p.kind != "file" && !covered(index))
                        fail("scope_conflict", "edit extends outside the held scopes; use a range edit");
                } else {
                    const auto& range = *p.observed;
                    const auto guard_begin = range.begin ? range.begin - 1 : 0;
                    // Every move touching this file changes its path or
                    // destination; only a disjoint range edit can pass.
                    if (p.kind == "file" || edit.kind != nodes::edit_kind::edit || (edit.offset <= range.end && edit.offset + edit.count >= guard_begin)) {
                        fail("scope_conflict", "edit overlaps " + scope_label(*p.record) + " held by " + holder_name(p.record->at("owner").text()) +
                                                   " until " + std::to_string(p.record->at("expires").number()));
                    }
                }
            }
        }
    }
    auto local_node::impl::claim_scopes(const node_request& in, const std::vector<std::string>& heads) -> std::string {
        if (in.edits.empty())
            return {};
        const bool implicit = in.leases.empty() && !in.lease_owner.empty();
        bool body_only = std::ranges::all_of(in.edits, [](const auto& edit) { return edit.kind == nodes::edit_kind::edit; });
        if (!body_only && !implicit)
            return "declaration_touched";
        // Snapping to these scopes would lock whole files or modules.
        static const std::set<std::string, std::less<>> broad{"translation_unit", "module", "namespace"};
        auto observed = source.projection(in.expected);
        auto after = source.projection(heads);
        auto references = scope_references();
        std::erase_if(references, [&](const auto& r) { return r.at("line").text() != in.worldline; });
        auto records = scope_leases();
        bool claimed = false;
        bool ffi_touched = false;
        for (const auto& edit : in.edits) {
            if (edit.kind != nodes::edit_kind::edit)
                continue;
            // An edit may select one of several files at its path by identity.
            const source_file* selected = nullptr;
            if (edit.document.empty())
                selected = &observed->path(edit.path);
            else if (auto found = observed->files.find(edit.document); found != observed->files.end())
                selected = found->second.get();
            else if (auto hidden = observed->hidden.find(edit.document); hidden != observed->hidden.end())
                selected = hidden->second.get();
            if (!selected)
                fail(nodes::code::invalid_record, "unknown selected file identity");
            auto held = materialize(*observed, *selected);
            const auto& file = held ? *held : *selected;
            const auto begin = edit.offset, end = edit.offset + edit.count;
            bool in_body = false;
            std::string_view snapped;
            std::optional<semantic::place> range;
            for (const auto& r : references) {
                if (r.at("file").text() != file.id)
                    continue;
                auto found = anchored(r, file);
                if (!found)
                    continue;
                if (r.at("kind").text() == "ffi_boundary") {
                    if (begin <= found->end && found->begin <= end)
                        ffi_touched = true;
                    continue;
                }
                if (found->begin > begin || found->end < end)
                    continue;
                // An edit at a body's braces may change the signature or the
                // next declaration.
                in_body = in_body || (r.at("kind").text() == "body" && found->begin < begin && end < found->end);
                if (broad.contains(r.at("scope_kind").text()))
                    continue;
                if (!range || found->end - found->begin < range->end - range->begin) {
                    snapped = r.at("scope").text();
                    range = found;
                }
            }
            body_only = body_only && in_body;
            if (!implicit)
                continue;
            auto now = after->files.find(file.id);
            if (now == after->files.end() || !now->second->present)
                continue;
            auto current = materialize(*after, *now->second);
            const auto& target = current ? *current : *now->second;
            auto lease = [&](std::string_view scope, std::uint64_t first, std::uint64_t last) {
                auto [left, right] = range_anchors(file, first, last);
                return value{value::map{{"kind", "scope"},
                                        {"token", hex(random_bytes(32))},
                                        {"operation", in.operation},
                                        {"owner", in.lease_owner},
                                        {"scope", scope.empty() ? "text:" + target.path : std::string{scope}},
                                        {"line", in.worldline},
                                        {"version", version(heads)},
                                        {"path", target.path},
                                        {"file", file.id},
                                        {"left", left},
                                        {"right", right},
                                        {"expires", lease_now() + constants::scope_lease_seconds},
                                        {"implicit", true}}};
            };
            // Without an enclosing scope the lease covers the edited
            // characters, widened to the nearest observed neighbours that
            // survive the write.
            auto edited = [&] {
                auto first = begin, last = end;
                while (first && !target.position(file.at(first - 1)))
                    --first;
                while (last < file.size() && !target.position(file.at(last)))
                    ++last;
                return lease({}, first, last);
            };
            auto candidate = range ? lease(snapped, range->begin, range->end) : edited();
            auto span = anchored(candidate, target);
            auto blocked = [&](const semantic::place& at, bool touching) {
                return std::ranges::any_of(records, [&](const auto& r) {
                    if (r.at("line").text() != in.worldline || r.at("owner").text() == in.lease_owner || r.at("file").text() != file.id)
                        return false;
                    auto other = r.at("kind").text() == "scope" ? anchored(r, target) : std::optional<semantic::place>{};
                    return !other || (touching ? at.begin <= other->end && at.end >= other->begin : at.begin < other->end && other->begin < at.end);
                });
            };
            // Another caller's lease inside the enclosing scope keeps this
            // lease to the edited characters, which it already avoids.
            if (range && (!span || blocked(*span, true))) {
                candidate = edited();
                span = anchored(candidate, target);
            }
            if (!span || blocked(*span, false))
                fail("scope_conflict", "edit in " + target.path + " has no anchors outside another caller's lease; read the current version and retry");
            auto own = [&](const auto& r) {
                return r.at("line").text() == in.worldline && r.at("owner").text() == in.lease_owner && r.at("file").text() == file.id;
            };
            // Explicit leases expire on their own schedule; only an implicit
            // lease is renewed in place of a new one.
            auto covering = std::ranges::find_if(records, [&](const auto& r) {
                auto mine = own(r) && r.at("implicit").boolean() ? anchored(r, target) : std::nullopt;
                return mine && mine->begin <= span->begin && span->end <= mine->end;
            });
            if (covering != records.end()) {
                std::get<value::map>(covering->data)["expires"] = std::max(covering->at("expires").number(), lease_now() + constants::scope_lease_seconds);
                claimed = true;
                continue;
            }
            std::erase_if(records, [&](const auto& r) {
                if (!own(r) || !r.at("implicit").boolean())
                    return false;
                auto mine = anchored(r, target);
                return !mine || (span->begin <= mine->begin && mine->end <= span->end);
            });
            if (records.size() >= constants::scope_lease_limit)
                fail(nodes::code::resource_limit, "too many active scope leases");
            claimed = true;
            records.push_back(std::move(candidate));
        }
        if (claimed)
            metadata("scope_leases", codec::encode(value::map{{"leases", records}}));
        if (ffi_touched)
            return "ffi_boundary_touched";
        if (body_only)
            return "body_only";
        return "declaration_touched";
    }
} // namespace nodes
