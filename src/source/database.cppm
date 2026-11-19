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

    namespace {
        auto json_strings(const std::string& text) -> std::vector<std::string> {
            auto parsed = json::read<std::vector<std::string>>(text);
            if (!parsed)
                fail(nodes::code::storage_failure, "stored string array is unreadable");
            return *parsed;
        }
    } // namespace

    local_node::local_node(std::filesystem::path root, bool rebuild, bool read_only) : state_(std::make_unique<impl>(std::move(root), rebuild, read_only)) {}
    local_node::~local_node() = default;
    auto local_node::project_id() const -> std::string { return state_->project; }
    auto local_node::node_id() const -> std::string { return state_->writer; }
    auto local_node::author_id() const -> std::string { return state_->writer; }
    auto local_node::statistics() const -> node_work {
        auto& s = *state_;
        return {s.projected_before_eviction + s.source.projected_files(),
                s.manifest_rows_read,
                s.blob_reads,
                s.manifest_rows_written,
                s.blob_writes,
                s.state_bytes_written,
                s.kernel_loaded && s.source.fully_loaded(),
                s.causal_headers_read,
                s.causal_effects_read,
                s.file_histories_read,
                s.file_payloads_read,
                s.state_bytes_read,
                s.kernel_loaded ? (s.source.has_reader() ? "indexed" : "complete") : "none",
                s.cache_evictions,
                s.state_bytes_decoded};
    }
    void local_node::confirm_with(commit_check* check) { state_->db->confirm = check; }
    auto local_node::initialize(std::string genesis_nonce) -> node_reply {
        if (!genesis_nonce.empty() && !content::is_hash(genesis_nonce))
            fail(nodes::code::invalid_record, "genesis nonce must be 32-byte lowercase hex");
        auto& s = *state_;
        if (!s.project.empty()) {
            if (!s.has_object(s.project))
                fail(nodes::code::missing_objects, "trusted genesis has not been received");
            if (!genesis_nonce.empty() && s.get(s.project, "genesis").at("nonce").raw() != unhex(genesis_nonce))
                fail(nodes::code::identity_conflict, "repository has another genesis nonce");
            return s.identity();
        }
        // The genesis names the project and nothing else: a workspace has one writer and no admission policy.
        auto genesis =
            value::map{{"kind", "genesis"}, {"nonce", bytes{genesis_nonce.empty() ? random_bytes(32) : unhex(genesis_nonce)}}, {"schema", std::uint64_t{1}}};
        transaction tx{*s.db};
        s.dirty = true;
        s.project = s.put(std::move(genesis));
        s.metadata("project", s.project);
        s.root_at("main", {});
        s.save_snapshot();
        tx.commit();
        s.dirty = false;
        return s.identity();
    }
    auto local_node::call(const node_request& in) -> node_reply {
        auto& s = *state_;
        struct cache_boundary {
            impl& state;
            ~cache_boundary() { state.trim_kernel(); }
        } cache{s};
        try {
            static const std::set<std::string> reads{"identity",     "inspect",   "tree",       "head",         "stat",       "read",
                                                     "compare",      "diff",      "history",    "receipt",      "worldlines", "content",
                                                     "contribution", "mail_list", "scope_list", "scope_locate", "references", "reference_history"};
            if (s.read_only && !reads.contains(in.action))
                fail(nodes::code::policy_denied, "source reader does not accept mutations");
            std::optional<read_snapshot> snapshot;
            if (reads.contains(in.action))
                snapshot.emplace(*s.db);
            if (s.dirty)
                s.reload();
            else
                s.refresh_external();
            s.trim_objects();
            if (in.action == "identity")
                return s.identity();
            if (in.action == "initialize")
                return initialize(in.genesis_nonce);
            if (s.project.empty())
                fail("wrong_project", "initialize a project first");
            if (in.action.starts_with("task_"))
                return s.task_call(in);
            s.check_execution(in);
            if (in.action == "scope_register")
                return s.register_scopes(in);
            if (in.action == "scope_locate")
                return s.locate_scopes(in);
            if (in.action.starts_with("scope_"))
                return s.scope_call(in);
            if (!s.has_object(s.project)) {
                error e{nodes::code::missing_objects, "project genesis unavailable"};
                e.missing.push_back(s.project);
                throw e;
            }
            if (in.action == "mail_send" || in.action == "mail_list")
                return s.mail(in);
            if (in.action == "reference" || in.action == "references" || in.action == "reference_history")
                return s.reference_call(in);
            if (in.action == "worldlines")
                return s.worldline_list();
            if (in.action == "content")
                return s.content(in);
            if (in.action == "contribution")
                return s.contribution(in);
            if (in.action == "inspect" || in.action == "tree" || in.action == "head" || in.action == "stat" || in.action == "read" || in.action == "compare" ||
                in.action == "diff" || in.action == "history") {
                auto found = s.roots.find(in.worldline);
                if (found != s.roots.end())
                    (void)s.root_line(in.worldline);
                auto heads = in.source.empty() ? (found == s.roots.end() ? std::vector<std::string>{} : found->second) : in.source;
                if (!in.revision.empty()) {
                    statement revision{*s.db, "SELECT data FROM revisions WHERE id=?1"};
                    revision.text(1, s.resolve_revision(in.worldline, in.revision));
                    if (!revision.next())
                        fail(nodes::code::invalid_record, "revision not found");
                    heads = string_list(codec::decode(revision.data(0)).at("frontier"));
                }
                if (!in.revision.empty() && s.version(heads) != s.resolve_revision(in.worldline, in.revision))
                    fail(nodes::code::invalid_record, "revision does not identify the selected frontier");
                if (!in.source.empty() || !in.revision.empty()) {
                    s.ensure_kernel();
                    s.within_line(s.root_line(in.worldline), heads);
                }
                if (in.action == "compare" || in.action == "diff") {
                    s.ensure_kernel();
                    return s.compare(in, heads);
                }
                if (in.action == "history") {
                    s.ensure_kernel();
                    return s.history(in, heads);
                }
                if (in.action == "head") {
                    auto out = s.identity();
                    out.version = s.version(heads);
                    out.frontier = heads;
                    return out;
                }
                auto view = s.view(heads, in.action != "tree" && in.action != "stat", (in.action == "read" || in.action == "stat") ? in.target : std::string{});
                if (in.action == "tree" && found != s.roots.end()) {
                    const auto id = s.root_line(in.worldline);
                    if (id != s.project)
                        view.base_version = s.version(string_list(s.get(id, "line").at("base")));
                }
                return view;
            }
            if (in.action == "receipt") {
                if (!in.target.empty())
                    return s.receipt(in.target, false);
                if (auto id = s.executed(in.operation))
                    return s.receipt(*id, false);
                // Flows that run several operations under one ID ask this first, so an abandoned one starts none of them.
                statement q{*s.db, "SELECT 1 FROM bindings WHERE author=?1 AND operation=?2 AND digest='abandoned'"};
                q.text(1, s.writer);
                q.text(2, in.operation);
                if (q.next())
                    fail(nodes::code::identity_conflict, "operation was abandoned");
                fail(nodes::code::outcome_unknown, "no verified original execution receipt");
            }
            if (in.action == "abandon") {
                // An operation that has not run never will: its binding takes a digest no request has, so the request, arriving later,
                // fails as an operation ID reused with different input. One that ran answers with its receipt.
                if (!content::is_hash(in.operation))
                    fail(nodes::code::invalid_record, "abandon names an operation");
                transaction tx{*s.db};
                s.refresh_external();
                if (auto id = s.executed(in.operation))
                    return s.receipt(*id, false);
                statement q{*s.db,
                            "INSERT INTO bindings(author,operation,digest,receipt) VALUES(?1,?2,'abandoned','') ON CONFLICT(author,operation) DO UPDATE SET "
                            "digest='abandoned'"};
                q.text(1, s.writer);
                q.text(2, in.operation);
                q.next();
                tx.commit();
                auto out = s.identity();
                out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
                return out;
            }
            static const std::set<std::string> mutations{"apply", "promote", "fork", "reverse", "redo", "resolve"};
            if (!mutations.contains(in.action))
                fail(nodes::code::invalid_record, "unknown local node action");
            return s.publish(in);
        } catch (const error& e) {
            static const std::set<std::string> recorded{"apply", "promote", "fork", "reverse", "redo", "resolve"};
            if (!s.read_only && recorded.contains(in.action) && content::is_hash(in.operation) && !s.project.empty() &&
                e.code != nodes::code::storage_failure) {
                try {
                    s.record_refusal(in);
                } catch (...) {
                    s.dirty = true;
                }
            }
            if (s.dirty)
                try {
                    s.reload();
                } catch (const std::exception& failure) {
                    node_reply out;
                    out.code = nodes::code::outcome_unknown;
                    out.reason = std::string(e.what()) + "; durable state could not be confirmed: " + failure.what();
                    out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
                    return out;
                }
            auto out = s.identity();
            out.code = e.code;
            out.reason = e.what();
            out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
            out.missing = e.missing;
            out.conflicts = e.conflicts;
            out.complete =
                e.code != nodes::code::missing_objects && e.code != nodes::code::outcome_unknown && e.code != nodes::code::storage_failure && e.missing.empty();
            if (auto i = s.roots.find(in.worldline); i != s.roots.end())
                out.frontier = i->second;
            return out;
        } catch (const std::exception& e) {
            if (s.dirty)
                try {
                    s.reload();
                } catch (...) {
                    node_reply out;
                    out.code = nodes::code::outcome_unknown;
                    out.reason = "durable state could not be confirmed";
                    out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
                    return out;
                }
            auto out = s.identity();
            out.code = nodes::code::storage_failure;
            out.complete = false;
            out.reason = e.what();
            out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
            return out;
        }
    }

    auto local_node::impl::causal_storage::header(std::size_t index) -> value {
        statement q{*self.db,
                    "SELECT event_id,author,operation,lamport,ancestors_blob,dependencies_json,request_digest,effect_digest,effects FROM events WHERE seq=?1"};
        q.number(1, index);
        if (!q.next())
            fail(nodes::code::missing_objects, "required causal proof is absent");
        ++self.causal_headers_read;
        auto ancestors = decode_causal(q.data(4), index);
        value::array ranges;
        for (auto [first, last] : ancestors.ranges())
            ranges.emplace_back(value::map{{"first", static_cast<std::uint64_t>(first)}, {"last", static_cast<std::uint64_t>(last)}});
        auto dependencies = json_strings(q.data(5));
        return value::map{{"index", static_cast<std::uint64_t>(index)},
                          {"id", q.data(0)},
                          {"author", q.data(1)},
                          {"operation", q.data(2)},
                          {"clock", q.number(3)},
                          {"ancestors", std::move(ranges)},
                          {"effects", q.number(8)},
                          {"dependencies", strings(dependencies)},
                          {"request_digest", q.data(6)},
                          {"effect_digest", q.data(7)}};
    }

    auto local_node::impl::causal_storage::locate(const std::string& id) -> std::optional<std::size_t> {
        statement q{*self.db, "SELECT seq FROM events WHERE event_id=?1"};
        q.text(1, id);
        if (!q.next())
            return {};
        return static_cast<std::size_t>(q.number(0));
    }

    auto local_node::impl::causal_storage::records(const std::string& kind, const std::string& key) -> std::vector<value> {
        std::vector<value> out;
        auto file_record = [&](std::uint64_t event, std::uint64_t ordinal, std::string_view payload) -> value {
            auto stored = codec::decode(payload);
            fields(stored, {"binary", "effect"});
            const auto& binary = stored.at("binary");
            const auto* digest = std::get_if<std::string>(&binary.data);
            if (digest && (stored.at("effect").at("type").text() != "binary" || stored.at("effect").at("value").text() != *digest))
                fail(nodes::code::storage_failure, "indexed binary payload reference differs");
            return value::map{{"event", event}, {"ordinal", ordinal}, {"effect", stored.at("effect")},
                              {"binary", bytes{digest ? self.read_chunked_blob(*digest) : binary.raw()}}};
        };
        auto control_record = [](std::uint64_t event, std::uint64_t ordinal, const std::string& type, bool selected, value::array selections) {
            return value::map{{"event", event},
                              {"ordinal", ordinal},
                              {"effect", value::map{{"type", type}, {"selections", std::move(selections)}, {"value", selected}}},
                              {"binary", bytes{std::string{}}}};
        };
        if (kind == "file" || kind == "presence") {
            statement q{*self.db, kind == "file" ? "SELECT event_seq,ordinal,payload FROM file_effects WHERE file_id=?1 ORDER BY event_seq,ordinal"
                                                 : "SELECT event_seq,ordinal,payload FROM file_effects WHERE file_id=?1 AND type IN "
                                                   "('presence','path') ORDER BY event_seq,ordinal"};
            q.text(1, key);
            while (q.next())
                out.push_back(file_record(q.number(0), q.number(1), q.data(2)));
            if (kind == "file") {
                if (out.empty())
                    fail(nodes::code::missing_objects, "required file causal index is absent: " + key);
                ++self.file_histories_read;
            }
            self.causal_effects_read += out.size();
            return out;
        }
        if (kind == "effect") {
            statement q{*self.db, "SELECT event_seq,ordinal,payload FROM file_effects WHERE effect_id=?1"};
            q.text(1, key);
            if (q.next())
                out.push_back(file_record(q.number(0), q.number(1), q.data(2)));
            else {
                statement c{*self.db, "SELECT event_seq,ordinal,type,value FROM control_effects WHERE effect_id=?1 LIMIT 1"};
                c.text(1, key);
                if (c.next()) {
                    statement s{*self.db, "SELECT target_effect_id FROM control_effects WHERE effect_id=?1 ORDER BY target_effect_id"};
                    s.text(1, key);
                    value::array selections;
                    while (s.next())
                        selections.push_back(s.data(0));
                    out.push_back(control_record(c.number(0), c.number(1), c.data(2), c.number(3) != 0, std::move(selections)));
                }
            }
            self.causal_effects_read += out.size();
            return out;
        }
        if (kind == "event") {
            std::uint64_t index = 0;
            try {
                index = std::stoull(key, nullptr, 16);
            } catch (const std::exception&) {
                fail(nodes::code::invalid_record, "causal event key differs");
            }
            std::vector<std::pair<std::uint64_t, value>> ordered;
            {
                statement q{*self.db, "SELECT event_seq,ordinal,payload FROM file_effects WHERE event_seq=?1 ORDER BY ordinal"};
                q.number(1, index);
                while (q.next())
                    ordered.emplace_back(q.number(1), file_record(q.number(0), q.number(1), q.data(2)));
            }
            {
                statement c{*self.db, "SELECT event_seq,ordinal,effect_id,type,value,target_effect_id FROM control_effects WHERE event_seq=?1 "
                                      "ORDER BY ordinal,effect_id,target_effect_id"};
                c.number(1, index);
                std::optional<std::pair<std::uint64_t, std::string>> group;
                std::string type;
                bool value = false;
                value::array selections;
                auto flush = [&] {
                    if (!group)
                        return;
                    ordered.emplace_back(group->first, control_record(index, group->first, type, value, std::move(selections)));
                    selections = value::array{};
                };
                while (c.next()) {
                    auto ordinal = c.number(1);
                    auto effect_id = c.data(2);
                    if (!group || group->first != ordinal || group->second != effect_id) {
                        flush();
                        group = std::pair{ordinal, effect_id};
                        type = c.data(3);
                        value = c.number(4) != 0;
                    }
                    selections.push_back(c.data(5));
                }
                flush();
            }
            std::ranges::sort(ordered, [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [_, record] : ordered)
                out.push_back(std::move(record));
            self.causal_effects_read += out.size();
            return out;
        }
        if (kind == "control") {
            // One join returns every target of each effect that selects this one: no per-effect query.
            statement q{*self.db, "SELECT c.event_seq,c.ordinal,c.effect_id,c.type,c.value,s.target_effect_id FROM control_effects c "
                                  "JOIN control_effects s ON s.effect_id=c.effect_id WHERE c.target_effect_id=?1 "
                                  "ORDER BY c.event_seq,c.ordinal,s.target_effect_id"};
            q.text(1, key);
            std::optional<std::tuple<std::uint64_t, std::uint64_t, std::string>> group;
            std::string type;
            bool value = false;
            value::array selections;
            auto flush = [&] {
                if (!group)
                    return;
                auto& [event, ordinal, effect_id] = *group;
                out.push_back(control_record(event, ordinal, type, value, std::move(selections)));
                selections = value::array{};
            };
            while (q.next()) {
                auto key_now = std::tuple{q.number(0), q.number(1), q.data(2)};
                if (!group || *group != key_now) {
                    flush();
                    group = key_now;
                    type = q.data(3);
                    value = q.number(4) != 0;
                }
                selections.push_back(q.data(5));
            }
            flush();
            self.causal_effects_read += out.size();
            return out;
        }
        if (kind == "dependent") {
            statement q{*self.db, "SELECT event_seq FROM event_dependencies WHERE dependency=?1 ORDER BY event_seq"};
            q.text(1, key);
            while (q.next())
                out.push_back(value::map{{"event", q.number(0)}});
            return out;
        }
        fail(nodes::code::invalid_record, "unknown causal record kind: " + kind);
    }

    auto local_node::impl::materialize(const source_state& state, const source_file& file) -> std::shared_ptr<const source_file> {
        if (file.fragments_ready)
            return {};
        return source.replay(file, state.causal, state.activation);
    }

    auto local_node::impl::causal_storage::file_text(const source_file& file) -> std::string {
        ++self.file_payloads_read;
        self.cached_payload_files.insert(file.id);
        self.cached_payload_bytes += file.stored_bytes;
        auto text = self.read_blob(file.digest);
        if (text.size() != file.stored_bytes || digest_of(text) != file.digest)
            fail(nodes::code::storage_failure, "state file content differs");
        return text;
    }

    auto local_node::impl::causal_storage::projection(const std::vector<std::string>& heads) -> std::shared_ptr<const source_state> {
        auto data = self.revision(self.version(heads));
        if (!data)
            return {};
        auto root = self.revision_text(*data, "root");
        if (root.empty())
            return {};
        auto state = std::make_shared<source_state>();
        state->files = self.manifest_files(self.version(heads));
        if (content_root(state->files) != root)
            fail(nodes::code::missing_objects, "version manifest is incomplete; reconstruct from the retained archive");
        std::map<std::string, std::set<std::string>> owners;
        for (auto& [id, file] : state->files)
            owners[file->path].insert(id);
        for (auto& [path, ids] : owners)
            state->paths[path] = std::make_shared<const std::set<std::string>>(ids);
        if (auto* activation = self.revision_member(*data, "activation"))
            for (auto& item : activation->list())
                state->activation[item.at("id").text()] = item.at("value").boolean();
        for (auto& id : string_list(*self.revision_member(*data, "resolutions")))
            state->resolutions[id] = true;
        state->conflicts = string_list(*self.revision_member(*data, "conflicts"));
        state->file_conflicts = self.conflict_map(*data, "file_conflicts");
        state->path_conflicts = self.conflict_map(*data, "path_conflicts");
        state->control_conflicts = self.conflict_map(*data, "control_conflicts");
        state->dependency_conflicts = self.conflict_map(*data, "dependency_conflicts");
        state->digest = state_digest(*state);
        return state;
    }

    auto local_node::impl::causal_storage::base(const std::vector<std::string>& heads)
        -> std::pair<std::vector<std::string>, std::shared_ptr<const source_state>> {
        // A checkout baseline records the version its writer observed; walk that chain to the newest level with a
        // manifest, so a cold read of the baseline replays only what the baseline adds to it.
        auto current = self.version(heads);
        for (std::size_t depth = 0; depth <= constants::manifest_chain_limit; ++depth) {
            auto data = self.revision(current);
            if (!data)
                return {};
            auto frontier = string_list(*self.revision_member(*data, "frontier"));
            if (!self.revision_text(*data, "root").empty())
                return {frontier, projection(frontier)};
            // A reconstructed store keeps no manifest for the empty version, but its state is known.
            if (frontier.empty())
                return {frontier, std::make_shared<source_state>()};
            auto parent = self.revision_text(*data, "parent");
            if (parent.empty())
                return {};
            current = std::move(parent);
        }
        return {};
    }

    auto local_node::impl::causal_storage::file_changed(const std::string& file, const causal_set& missing) -> bool {
        for (auto [first, last] : missing.ranges()) {
            if (first >= last)
                continue;
            {
                statement q{*self.db, "SELECT 1 FROM file_effects WHERE file_id=?1 AND event_seq>=?2 AND event_seq<?3 LIMIT 1"};
                q.text(1, file);
                q.number(2, first);
                q.number(3, last);
                if (q.next())
                    return true;
            }
            // A resolve only records that a conflict was decided; render() reads activation, never resolution,
            // so a resolve cannot change a file's visible state and is not matched here.
            statement q{*self.db, "SELECT 1 FROM control_effects c JOIN file_effects f ON f.effect_id=c.target_effect_id WHERE f.file_id=?1 AND "
                                  "c.type='activate' AND c.event_seq>=?2 AND c.event_seq<?3 LIMIT 1"};
            q.text(1, file);
            q.number(2, first);
            q.number(3, last);
            if (q.next())
                return true;
        }
        return false;
    }

    void local_node::impl::ensure_kernel() {
        if (kernel_loaded)
            return;
        // A missing derived index is an unavailable state, never an empty history. Event sequence numbers are dense,
        // so the highest sequence names the count without scanning the table.
        statement q{*db, "SELECT COALESCE(MAX(seq)+1,0) FROM events"};
        q.next();
        if (q.number(0) != snapshot_events)
            fail(nodes::code::missing_objects, "authenticated causal index is absent; reconstruct from the retained archive");
        source = kernel{};
        source.demand(snapshot_events, &causal);
        kernel_loaded = true;
    }

    void local_node::impl::persist_event(std::size_t index) {
        auto header = source.event_header(index);
        auto event_id = header.at("id").text();
        auto& body = get(event_id, "batch");
        auto parents = string_list(body.at("parents"));
        causal_set ancestors;
        for (auto& range : header.at("ancestors").list())
            ancestors.add_range(range.at("first").number(), range.at("last").number());
        auto after = parents;
        after.push_back(event_id);
        auto version_id = version(source.frontier(std::move(after)));
        auto written_version = version({event_id});
        statement q{*db, "INSERT INTO events(seq,event_id,worldline,parents_json,ancestors_blob,lamport,author,operation,caller_operation,tool,intent,"
                         "result_json,dependencies_json,effects,request_digest,effect_digest,version_id,written_version) "
                         "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18)"};
        q.number(1, index);
        q.text(2, event_id);
        q.text(3, line_name(body.at("line").text()));
        q.text(4, json::write(parents));
        q.bind(5, encode_causal(ancestors));
        q.number(6, header.at("clock").number());
        q.text(7, header.at("author").text());
        q.text(8, header.at("operation").text());
        q.text(9, body.at("caller_operation").text());
        q.text(10, body.at("tool").text());
        q.text(11, body.at("intent").text());
        q.text(12, json::write(body.at("result").text()));
        q.text(13, json::write(string_list(header.at("dependencies"))));
        q.number(14, header.at("effects").number());
        q.text(15, header.at("request_digest").text());
        q.text(16, header.at("effect_digest").text());
        q.text(17, version_id);
        q.text(18, written_version);
        q.next();
        for (auto& record : source.event_records(index)) {
            auto& effect = record.at("effect");
            auto type = effect.at("type").text();
            auto ordinal = record.at("ordinal").number();
            auto effect_id = source.operation_effect(header.at("author").text(), header.at("operation").text(), ordinal);
            if (type == "activate" || type == "resolve") {
                auto selections = string_list(effect.at("selections"));
                if (selections.empty())
                    fail(nodes::code::invalid_record, "control effect has no selection");
                auto state = effect.at("value").boolean();
                for (auto& target : selections) {
                    statement c{*db, "INSERT OR IGNORE INTO control_effects(target_effect_id,event_seq,ordinal,effect_id,type,value) "
                                     "VALUES(?1,?2,?3,?4,?5,?6)"};
                    c.text(1, target);
                    c.number(2, index);
                    c.number(3, ordinal);
                    c.text(4, effect_id);
                    c.text(5, type);
                    c.number(6, state ? 1 : 0);
                    c.next();
                }
            } else {
                statement f{*db, "INSERT OR IGNORE INTO file_effects(file_id,event_seq,ordinal,effect_id,type,payload) VALUES(?1,?2,?3,?4,?5,?6)"};
                f.text(1, effect.at("file").text());
                f.number(2, index);
                f.number(3, ordinal);
                f.text(4, effect_id);
                f.text(5, type);
                const auto& binary = record.at("binary");
                const bool chunked = chunked_payload(binary.raw());
                if (chunked)
                    state_bytes_written += retain_chunked_blob(effect.at("value").text(), binary.raw());
                f.bind(6, codec::encode(value::map{{"effect", effect}, {"binary", chunked ? effect.at("value") : binary}}));
                f.next();
            }
        }
        for (auto& dependency : string_list(header.at("dependencies"))) {
            statement d{*db, "INSERT OR IGNORE INTO event_dependencies(dependency,event_seq) VALUES(?1,?2)"};
            d.text(1, dependency);
            d.number(2, index);
            d.next();
        }
    }

    void local_node::impl::save_snapshot() {
        if (project.empty() || !has_object(project))
            return;
        ensure_kernel();
        for (auto index = snapshot_events; index < source.size(); ++index)
            persist_event(index);
        auto body = value::map{{"kind", "state_snapshot"},
                               {"node", writer},
                               {"project", project},
                               {"schema", std::uint64_t{2}},
                               {"events", static_cast<std::uint64_t>(source.size())},
                               {"watermark", object_watermark}};
        auto encoded = codec::encode(std::move(body));
        metadata("state_snapshot", encoded);
        state_snapshot = digest_of(encoded);
        observed_snapshot = state_snapshot;
        snapshot_events = source.size();
        snapshot_watermark = object_watermark;
        snapshot_ready = true;
        // Keep the newest reconstruction state: the next publication reuses it instead of refetching the manifest.
        source.trim_projections();
        if (!source.has_reader())
            source.attach_reader(&causal);
    }

    auto local_node::impl::restore_snapshot() -> bool {
        statement q{*db, "SELECT data FROM metadata WHERE name='state_snapshot'"};
        if (!q.next())
            return false;
        auto encoded = q.data(0);
        auto body = codec::decode(encoded);
        auto schema = body.object().find("schema");
        // A snapshot of the page store names no event table: the archive alone is trusted, so it is replayed.
        if (schema == body.object().end() || schema->second.number() != 2)
            return false;
        fields(body, {"events", "kind", "node", "project", "schema", "watermark"});
        if (body.at("kind").text() != "state_snapshot" || body.at("project").text() != project || body.at("node").text() != writer)
            fail(nodes::code::invalid_record, "state snapshot identity differs");
        snapshot_events = body.at("events").number();
        snapshot_watermark = body.at("watermark").number();
        statement count{*db, "SELECT COALESCE(MAX(seq)+1,0) FROM events"};
        count.next();
        if (count.number(0) != snapshot_events)
            fail(nodes::code::missing_objects, "state snapshot event coverage is damaged; run an explicit audit");
        statement heads{*db, "SELECT name,frontier_json FROM worldlines"};
        while (heads.next())
            roots[heads.data(0)] = json_strings(heads.data(1));
        snapshot_ready = true;
        kernel_loaded = snapshot_events == 0;
        state_snapshot = digest_of(encoded);
        return true;
    }

    void local_node::impl::rebuild_worldlines() {
        roots.clear();
        std::map<std::string, std::vector<std::vector<std::string>>> commits;
        statement q{*db, "SELECT worldline,frontier FROM receipts"};
        while (q.next())
            commits[q.data(0)].push_back(string_list(codec::decode(q.data(1)).at("frontier")));
        commits.try_emplace("main", std::vector<std::vector<std::string>>{{}});
        // A receipt names the frontier each publication left; the head is the one no later commit contains.
        for (auto& [name, list] : commits) {
            const std::vector<std::string>* head = nullptr;
            for (auto& candidate : list) {
                bool dominated = false;
                for (auto& other : list) {
                    if (&other == &candidate)
                        continue;
                    auto covers = std::ranges::all_of(candidate, [&](const std::string& event) { return source.includes(other, event); });
                    if (covers && other != candidate) {
                        dominated = true;
                        break;
                    }
                }
                if (!dominated) {
                    head = &candidate;
                    break;
                }
            }
            roots[name] = head ? *head : list.front();
        }
        for (auto& [name, frontier] : roots)
            record_worldline(name, frontier, true);
    }

    void local_node::impl::reload(bool rebuild) {
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
        bodies.clear();
        objects.clear();
        object_order.clear();
        object_sizes.clear();
        cached_object_bytes = 0;
        roots.clear();
        line_names.reset();
        projected_before_eviction += source.projected_files();
        source = kernel{};
        cached_payload_files.clear();
        cached_payload_bytes = 0;
        snapshot_watermark = 0;
        snapshot_events = 0;
        state_snapshot.clear();
        loaded_records = 0;
        object_count = object_watermark = 0;
        snapshot_ready = false;
        kernel_loaded = true;
        if (rebuild) {
            db->run("DELETE FROM events; DELETE FROM event_dependencies; DELETE FROM file_effects; DELETE FROM control_effects; DELETE FROM manifest_files; "
                    "DELETE FROM manifest_paths; DELETE FROM blobs; "
                    "DELETE FROM blob_extents WHERE payload NOT IN (SELECT payload FROM object_payloads); "
                    "DELETE FROM blob_payloads WHERE digest NOT IN (SELECT payload FROM object_payloads); "
                    "DELETE FROM blob_chunks WHERE digest NOT IN (SELECT chunk FROM blob_extents);");
            // Revisions name frontiers; their manifests were derived and are gone until the archive replays them.
            std::vector<std::pair<std::string, std::string>> revisions;
            {
                statement q{*db, "SELECT id,data FROM revisions"};
                while (q.next())
                    revisions.emplace_back(q.data(0), codec::encode(value::map{{"frontier", strings(string_list(codec::decode(q.data(1)).at("frontier")))}}));
            }
            statement plain{*db, "UPDATE revisions SET data=?2 WHERE id=?1"};
            for (auto& [id, data] : revisions) {
                plain.reset();
                plain.text(1, id);
                plain.bind(2, data);
                plain.next();
            }
        }
        roots.clear();
        {
            statement heads{*db, "SELECT name,frontier_json FROM worldlines"};
            while (heads.next())
                roots[heads.data(0)] = json_strings(heads.data(1));
        }
        bool restored = !rebuild && restore_snapshot();
        std::uint64_t latest = 0;
        {
            statement q{*db, "SELECT COALESCE(MAX(rowid),0) FROM objects"};
            q.next();
            latest = q.number(0);
        }
        if (!restored || (!read_only && latest > snapshot_watermark)) {
            auto before = snapshot_events;
            ensure_kernel();
            load_objects(snapshot_watermark);
            if (!restored && !project.empty()) {
                if (roots.empty())
                    rebuild_worldlines();
                else
                    for (auto& [name, frontier] : roots)
                        record_worldline(name, frontier, true);
            }
            save_snapshot();
            replayed_events = source.size() - before;
        } else
            replayed_events = 0;
        {
            statement totals{*db, "SELECT COALESCE(MAX(rowid),0),COUNT(*) FROM objects"};
            totals.next();
            object_watermark = totals.number(0);
            object_count = totals.number(1);
        }
        snapshot.commit();
        if (rebuild && !read_only && !snapshot_ready)
            save_snapshot();
        data_version = observed_version;
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
