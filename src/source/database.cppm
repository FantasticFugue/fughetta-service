module;
#include <sqlite3.h>
module engine;
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
import :state;

namespace nodes {
    using namespace persistence;

    local_node::local_node(std::filesystem::path root, std::string trusted_project, bool audit, bool read_only)
        : state_(std::make_unique<impl>(std::move(root), std::move(trusted_project), audit, read_only)) {}
    local_node::~local_node() = default;
    auto local_node::project_id() const -> std::string { return state_->project; }
    auto local_node::node_id() const -> std::string { return state_->node.id(); }
    auto local_node::author_id() const -> std::string { return state_->author.id(); }
    auto local_node::statistics() const -> node_work {
        auto& s = *state_;
        return {s.projected_before_eviction + s.source.projected_files(),
                s.db->state_nodes_read,
                s.db->state_values_read,
                s.db->state_nodes_written,
                s.db->state_values_written,
                s.db->state_bytes_written,
                s.kernel_loaded && s.source.fully_loaded(),
                s.causal_headers_read,
                s.causal_effects_read,
                s.file_histories_read,
                s.file_payloads_read,
                s.db->state_bytes_read,
                s.kernel_loaded ? (s.source.has_reader() ? "indexed" : "complete") : "none",
                s.sequence_pages_read,
                s.sequence_pages_written,
                s.insertion_payloads_read,
                s.cache_evictions,
                s.db->state_bytes_decoded,
                s.db->causal_blocks_read};
    }
    void local_node::use_author(key signer) { state_->author = std::move(signer); }
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
        auto genesis = value::map{{"authors", strings({s.author.id()})},
                                  {"founding_key", bytes{s.author.public_key}},
                                  {"kind", "genesis"},
                                  {"maintainers", strings({s.author.id()})},
                                  {"nodes", strings({s.node.id()})},
                                  {"nonce", bytes{genesis_nonce.empty() ? random_bytes(32) : unhex(genesis_nonce)}},
                                  {"owner", s.author.id()},
                                  {"protected", strings({"main"})},
                                  {"schema", std::uint64_t{1}}};
        transaction tx{*s.db};
        s.dirty = true;
        s.project = s.put(std::move(genesis), s.author);
        s.validate_object(s.project);
        s.metadata("project", s.project);
        s.root_at("main", {});
        s.refresh_policies();
        s.save_snapshot();
        tx.commit();
        s.dirty = false;
        return s.identity();
    }
    auto local_node::objects(const std::vector<std::string>& ids) -> std::vector<object_part> {
        std::vector<object_part> out;
        for (auto& id : ids) {
            auto data = state_->object_bytes(id);
            for (std::size_t offset = 0; offset < data.size(); offset += 1024 * 1024)
                out.push_back({id, offset, data.size(), hex(std::string_view(data).substr(offset, 1024 * 1024))});
        }
        return out;
    }
    auto local_node::receive_packet(std::string_view packet) -> node_reply {
        try {
            return call({.action = "receive", .parts = decode_packet(packet)});
        } catch (const error& e) {
            auto out = state_->identity();
            out.code = e.code;
            out.reason = e.what();
            return out;
        }
    }
    namespace {
        void write_frame(std::ostream& output, std::string_view bytes) {
            auto packed = archive::pack(bytes);
            std::string header;
            codec::integer(header, packed.size());
            output.write(header.data(), header.size());
            output.write(packed.data(), packed.size());
            if (!output)
                fail(nodes::code::storage_failure, "exchange stream write failed");
        }
        auto read_frame(std::istream& input) -> std::optional<std::string> {
            std::array<char, 8> header{};
            input.read(header.data(), header.size());
            if (input.gcount() == 0 && input.eof())
                return {};
            if (input.gcount() != 8)
                fail(nodes::code::invalid_record, "truncated exchange frame header");
            std::uint64_t size = 0;
            for (int i = 7; i >= 0; --i)
                size = (size << 8) | static_cast<unsigned char>(header[i]);
            if (size > 128 * 1024 * 1024)
                fail(nodes::code::resource_limit, "exchange frame exceeds 128 MiB");
            std::string bytes(size, '\0');
            input.read(bytes.data(), bytes.size());
            if (static_cast<std::uint64_t>(input.gcount()) != size)
                fail(nodes::code::invalid_record, "truncated exchange frame");
            return archive::unpack(bytes);
        }
    } // namespace
    auto local_node::export_stream(std::ostream& output, std::string known_checkpoint) -> node_reply {
        auto& s = *state_;
        read_snapshot snapshot{*s.db};
        if (s.dirty)
            s.reload();
        else
            s.refresh_external();
        auto inventory = s.inventory(known_checkpoint);
        value::array lines;
        for (auto& [name, frontier] : s.worldlines())
            lines.emplace_back(value::map{{"frontier", strings(frontier)}, {"name", name}});
        auto b = base_body("inventory", s.project);
        b["base"] = known_checkpoint.empty() ? value{} : value{known_checkpoint};
        b["objects"] = strings(inventory.objects);
        b["missing"] = strings(inventory.missing);
        b["complete"] = inventory.complete;
        b["owner"] = s.node.id();
        b["worldlines"] = std::move(lines);
        write_frame(output, codec::encode(seal(std::move(b), s.node)));
        value::array parts;
        std::size_t buffered = 0;
        auto flush = [&] {
            if (!parts.empty()) {
                write_frame(output, codec::encode(value::map{{"parts", std::move(parts)}}));
                parts.clear();
                buffered = 0;
            }
        };
        auto emit = [&](const std::string& id) {
            auto data = s.object_bytes(id);
            for (std::size_t offset = 0; offset < data.size(); offset += 1024 * 1024) {
                auto chunk = data.substr(offset, 1024 * 1024);
                buffered += chunk.size();
                parts.emplace_back(
                    value::map{{"data", bytes{std::move(chunk)}}, {"id", id}, {"offset", std::uint64_t{offset}}, {"total", std::uint64_t{data.size()}}});
                if (buffered >= 1024 * 1024)
                    flush();
            }
        };
        if (known_checkpoint.empty()) {
            statement rows{*s.db, "SELECT id FROM objects ORDER BY rowid"};
            while (rows.next())
                emit(rows.data(0));
        } else {
            for (auto& id : inventory.objects)
                emit(id);
        }
        flush();
        return inventory;
    }
    auto local_node::import_stream(std::istream& input) -> node_reply {
        auto& s = *state_;
        s.refresh_external();
        s.ensure_kernel();
        s.ensure_status();
        auto header = read_frame(input);
        if (!header)
            fail(nodes::code::invalid_record, "exchange has no signed inventory");
        auto inventory = codec::decode(*header);
        const auto& body = open(inventory);
        s.check_schema(body);
        if (body.at("kind").text() != "inventory" || body.at("project").text() != s.project ||
            body.at("owner").text() != digest_of(inventory.at("public_key").raw()))
            fail("wrong_project", "exchange inventory origin or project mismatch");
        std::set<std::string> declared;
        for (auto& id : string_list(body.at("objects")))
            if (!content::is_hash(id) || !declared.insert(id).second)
                fail(nodes::code::invalid_record, "invalid inventory declaration");
        std::vector<object_part> pending;
        std::size_t pending_bytes = 0;
        auto flush = [&] {
            if (!pending.empty()) {
                s.receive({.action = "receive", .parts = std::move(pending)}, false, true, true, false);
                pending.clear();
                pending_bytes = 0;
            }
        };
        while (auto frame = read_frame(input)) {
            auto packet = codec::decode(*frame);
            fields(packet, {"parts"});
            for (auto& encoded : packet.at("parts").list()) {
                fields(encoded, {"data", "id", "offset", "total"});
                object_part part{encoded.at("id").text(), encoded.at("offset").number(), encoded.at("total").number(), encoded.at("data").raw()};
                if (!declared.contains(part.id))
                    fail(nodes::code::invalid_record, "undeclared transferred object");
                pending_bytes += part.data.size();
                pending.push_back(std::move(part));
            }
            if (pending_bytes >= 1024 * 1024)
                flush();
        }
        flush();
        {
            transaction tx{*s.db};
            s.dirty = true;
            s.validate_available();
            s.refresh_policies();
            tx.commit();
            s.dirty = false;
        }
        auto references = s.refs(body);
        auto absent = s.missing(references);
        auto declared_missing = string_list(body.at("missing"));
        absent.insert(absent.end(), declared_missing.begin(), declared_missing.end());
        auto out = s.identity();
        out.worldlines = s.worldlines();
        out.completeness_scope = "received_inventory";
        out.missing = vector_set(std::move(absent));
        out.complete = out.missing.empty() && body.at("complete").boolean() && std::ranges::all_of(references, [&](auto& id) { return s.is_validated(id); });
        if (!out.complete && out.missing.empty())
            out.reason = "inventory contains refused or unverified objects";
        out.storage_bytes = s.archive_bytes;
        for (auto& line : body.at("worldlines").list())
            out.worldlines[body.at("owner").text() + ":" + line.at("name").text()] = string_list(line.at("frontier"));
        {
            transaction tx{*s.db};
            s.dirty = true;
            if (out.complete) {
                auto id = s.retain(std::move(inventory));
                s.validate_object(id);
            }
            s.trim_objects();
            s.save_snapshot();
            tx.commit();
            s.dirty = false;
            s.trim_objects();
        }
        return out;
    }
    auto local_node::call(const node_request& in) -> node_reply {
        auto& s = *state_;
        struct cache_boundary {
            impl& state;
            ~cache_boundary() { state.trim_kernel(); }
        } cache{s};
        try {
            static const std::set<std::string> reads{"identity",   "offer",   "usage",        "rejected",  "send",       "inspect",    "tree",
                                                     "head",       "stat",    "read",         "compare",   "diff",       "history",    "receipt",
                                                     "worldlines", "content", "contribution", "mail_list", "scope_list", "references", "reference_history"};
            if (s.read_only && !reads.contains(in.action))
                fail(nodes::code::policy_denied, "source reader does not accept mutations");
            std::optional<read_snapshot> snapshot;
            if (reads.contains(in.action))
                snapshot.emplace(*s.db);
            if (in.action == "audit") {
                transaction tx{*s.db};
                std::uint64_t generation_value = 0;
                {
                    statement generation{*s.db, "PRAGMA data_version"};
                    generation.next();
                    generation_value = generation.number(0);
                }
                if (!s.archive_audited || generation_value != s.data_version)
                    s.reload(true);
                s.dirty = true;
                s.source.detach_reader();
                s.reset_state_storage();
                s.save_snapshot();
                tx.commit();
                s.dirty = false;
                auto out = s.inventory();
                out.complete = out.complete && s.refused.empty();
                return out;
            }
            if (s.dirty)
                s.reload();
            else
                s.refresh_external();
            s.trim_objects();
            s.ensure_status();
            if (in.action == "identity")
                return s.identity();
            if (in.action == "initialize")
                return initialize(in.genesis_nonce);
            if (s.project.empty())
                fail("wrong_project", "initialize or join a trusted project first");
            if (in.action.starts_with("task_"))
                return s.task_call(in);
            s.check_execution(in);
            if (in.action == "scope_register")
                return s.register_scopes(in);
            if (in.action.starts_with("scope_"))
                return s.scope_call(in);
            if (in.action == "receive")
                return s.receive(in);
            if (in.action == "offer" || in.action == "usage")
                return s.inventory(in.target);
            if (in.action == "rejected") {
                auto out = s.identity();
                if (!in.target.empty()) {
                    auto found = s.refused.find(in.target);
                    if (found == s.refused.end())
                        fail(nodes::code::invalid_record, "object has no refusal record");
                    out.reason = found->second.second;
                }
                return out;
            }
            if (in.action == "send") {
                if (in.max_bytes == 0 || in.max_bytes > 16 * 1024 * 1024)
                    fail(nodes::code::resource_limit, "transfer packet budget must be 1 byte through 16 MiB");
                auto out = s.identity();
                auto budget = in.max_bytes;
                for (auto& id : in.want) {
                    auto encoded = s.object_bytes(id);
                    std::uint64_t offset = 0;
                    for (auto& requested : in.parts)
                        if (requested.id == id)
                            offset = requested.offset;
                    if (offset > encoded.size())
                        fail(nodes::code::invalid_record, "send offset exceeds object length");
                    while (offset < encoded.size() && budget) {
                        auto size = std::min<std::uint64_t>({budget, 1024 * 1024, encoded.size() - offset});
                        out.parts.push_back({id, offset, encoded.size(), hex(std::string_view(encoded).substr(offset, size))});
                        offset += size;
                        budget -= size;
                    }
                    if (!budget)
                        break;
                }
                return out;
            }
            if (in.action == "recover")
                return s.recover();
            if (!s.has_object(s.project)) {
                error e{nodes::code::missing_objects, "project genesis unavailable"};
                e.missing.push_back(s.project);
                throw e;
            }
            if (in.action == "policy") {
                s.ensure_kernel();
                return s.create_policy(in);
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
            if (in.action == "checkpoint") {
                if (in.target.empty())
                    s.ensure_kernel();
                return s.checkpoint(in);
            }
            if (in.action == "materialize") {
                s.ensure_kernel();
                auto it = s.roots.find(in.worldline);
                if (it == s.roots.end())
                    fail(nodes::code::invalid_record, "worldline not found");
                (void)s.root_line(in.worldline);
                s.materialize(in.worldline, it->second);
                auto out = s.view(it->second, false);
                out.materialized_version = out.version;
                return out;
            }
            if (in.action == "inspect" || in.action == "tree" || in.action == "head" || in.action == "stat" || in.action == "read" || in.action == "compare" ||
                in.action == "diff" || in.action == "history") {
                auto found = s.roots.find(in.worldline);
                if (found != s.roots.end())
                    (void)s.root_line(in.worldline);
                auto heads = in.source.empty() ? (found == s.roots.end() ? std::vector<std::string>{} : found->second) : in.source;
                if (!in.snapshot.empty()) {
                    auto& checkpoint = s.get(in.snapshot, "checkpoint");
                    if (!s.is_validated(in.snapshot) || checkpoint.at("worldline").text() != in.worldline)
                        fail(nodes::code::invalid_record, "snapshot does not identify this worldline");
                    heads = string_list(checkpoint.at("frontier"));
                } else if (!in.revision.empty()) {
                    statement revision{*s.db, "SELECT data FROM revisions WHERE id=?1"};
                    revision.text(1, s.resolve_revision(in.worldline, in.revision));
                    if (!revision.next())
                        fail(nodes::code::invalid_record, "revision not found");
                    heads = string_list(codec::decode(revision.data(0)).at("frontier"));
                }
                if (!in.revision.empty() && s.version(heads) != s.resolve_revision(in.worldline, in.revision))
                    fail(nodes::code::invalid_record, "revision does not identify the selected frontier");
                if (!in.source.empty() || !in.revision.empty() || !in.snapshot.empty()) {
                    s.ensure_kernel();
                    s.within_line(s.root_line(in.worldline), heads);
                }
                for (auto& head : heads)
                    if (s.refused.contains(head))
                        fail(nodes::code::policy_denied, "selected source object was refused");
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
                return s.view(heads, in.action != "tree" && in.action != "stat", (in.action == "read" || in.action == "stat") ? in.target : std::string{});
            }
            if (in.action == "receipt") {
                if (!in.target.empty())
                    return s.receipt(in.target, false);
                if (auto id = s.executed(in.operation))
                    return s.receipt(*id, false);
                // Flows that run several operations under one ID ask this first, so an abandoned one starts none of them.
                statement q{*s.db, "SELECT 1 FROM bindings WHERE author=?1 AND operation=?2 AND digest='abandoned'"};
                q.text(1, s.author.id());
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
                q.text(1, s.author.id());
                q.text(2, in.operation);
                q.next();
                tx.commit();
                auto out = s.identity();
                out.operation = in.caller_operation.empty() ? in.operation : in.caller_operation;
                return out;
            }
            static const std::set<std::string> mutations{"apply", "promote", "fork", "integrate", "admit", "reverse", "redo", "resolve"};
            if (!mutations.contains(in.action))
                fail(nodes::code::invalid_record, "unknown local node action");
            return s.publish(in);
        } catch (const error& e) {
            static const std::set<std::string> recorded{"apply", "promote", "fork", "integrate", "admit", "reverse", "redo", "resolve"};
            if (!s.read_only && recorded.contains(in.action) && content::is_hash(in.operation) && !s.project.empty() &&
                e.code != nodes::code::storage_failure) {
                try {
                    s.record_failure(in, e);
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
} // namespace nodes
