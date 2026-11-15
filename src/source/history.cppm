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
    auto local_node::impl::version(const std::vector<std::string>& frontier) -> std::string {
        if (frontier.empty())
            return digest_of(codec::encode(value::map{{"frontier", strings({})}, {"project_id", project}}));
        if (auto name = matching_view(frontier))
            return views.at(*name).version;
        ensure_kernel();
        return digest_of(codec::encode(value::map{{"frontier", strings(source.frontier(frontier))}, {"project_id", project}}));
    }

    void local_node::impl::root_at(const std::string& name, const std::vector<std::string>& frontier, std::string admission) {
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

    auto local_node::impl::identity() -> node_reply {
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

    auto local_node::impl::view(const std::vector<std::string>& frontier, bool include_text, std::string_view path) -> node_reply {
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

    auto local_node::impl::entry_of(const std::string& id, std::vector<std::string> proof) -> history_entry {
        auto& b = get(id, "batch");
        auto& attempt = get(b.at("attempt").text(), "attempt");
        auto& intent = get(b.at("intent").text(), "intent");
        std::string outcome = identity_conflicts.contains(b.at("author").text() + ":" + b.at("operation").text()) ? std::string{nodes::code::identity_conflict}
                              : proof.empty()                                                                     ? "unconfirmed"
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

    auto local_node::impl::contribution(const node_request& in) -> node_reply {
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

    auto local_node::impl::policy(std::string_view worldline) -> std::string {
        if (policies.empty())
            fail(nodes::code::missing_objects, "project policy is unavailable");
        if (policies.size() != 1 && (worldline.empty() || std::ranges::any_of(policies, [&](auto& id) { return protected_target(get(id), worldline); })))
            fail("policy_conflict", "protected target requires a joined-parent resolution");
        return policies.front();
    }

    auto local_node::impl::target(const std::string& name) -> value { return value::map{{"name", name}, {"owner", node.id()}, {"reviewer", author.id()}}; }

    auto local_node::impl::unresolved_identities(const std::vector<std::string>& heads, const std::string& p, const std::string& worldline)
        -> std::vector<std::string> {
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
                            if (b.at("policy").text() != p || b.at("target").at("name").text() != worldline || b.at("target").at("owner").text() != node.id())
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
        if (!is_validated(id))
            fail(nodes::code::outcome_unknown, "original receipt has not been verified");
        auto& r = get(id, "receipt");
        auto& a = get(r.at("admission").text(), "admission");
        auto out = identity();
        out.frontier = string_list(a.at("frontier"));
        out.version = version(out.frontier);
        out.base_version = version(string_list(a.at("prior")));
        for (const auto& event : out.frontier) {
            const auto& batch = get(event);
            if (batch.at("kind").text() != "batch" || batch.at("operation").text() != a.at("operation").text() ||
                batch.at("author").text() != r.at("author").text())
                continue;
            auto own = version({event});
            statement known{*db, "SELECT 1 FROM revisions WHERE id=?1"};
            known.text(1, own);
            if (known.next())
                out.written_version = std::move(own);
            break;
        }
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

    void local_node::impl::binding(const std::string& operation, const std::string& digest, const std::string& result) {
        statement q{*db, "INSERT INTO bindings(author,operation,digest,receipt) VALUES(?1,?2,?3,?4) ON CONFLICT(author,operation) DO UPDATE SET "
                         "receipt=excluded.receipt WHERE digest=excluded.digest"};
        q.text(1, author.id());
        q.text(2, operation);
        q.text(3, digest);
        q.text(4, result);
        q.next();
    }

    auto local_node::impl::executed(const std::string& operation) -> std::optional<std::string> {
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

    auto local_node::impl::retry(const node_request& in, const std::string& digest) -> std::optional<node_reply> {
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

    auto local_node::impl::accept(const node_request& in, const std::string& line, const std::string& request_digest, const std::string& effect_digest,
                                  const std::vector<std::string>& prior, const std::vector<std::string>& frontier, const std::string& result_id,
                                  const std::string& attempt_id, bool override_author) -> node_reply {
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

    void local_node::impl::interrupt_at(std::string_view boundary) {
        if (auto value = std::getenv("NODE_FAULT"); value && boundary == value)
            std::_Exit(86);
    }
} // namespace nodes
