module;
#include <sqlite3.h>
export module engine:state;
import :api;
import std;
import content;
import codec;
import kernel;
import nodestore;

export namespace nodes {
    using namespace persistence;

    // The paths a rename into `path` reaches: the path itself, its ancestors, and its descendants.
    auto overlapping_paths(const auto& paths, const std::string& path) -> std::vector<std::string> {
        std::vector<std::string> out;
        for (auto prefix = path; !prefix.empty();) {
            if (paths.contains(prefix))
                out.push_back(prefix);
            auto slash = prefix.rfind('/');
            if (slash == std::string::npos)
                break;
            prefix.resize(slash);
        }
        const auto prefix = path + "/";
        for (auto it = paths.lower_bound(prefix); it != paths.end(); ++it) {
            const auto& key = [&]() -> const std::string& {
                if constexpr (requires { it->first; })
                    return it->first;
                else
                    return *it;
            }();
            if (!key.starts_with(prefix))
                break;
            out.push_back(key);
        }
        return out;
    }

    auto vector_set(std::vector<std::string> ids) -> std::vector<std::string>;
    auto base_body(std::string kind, std::string project) -> value::map;
    auto chunked_payload(std::string_view data) -> bool;
    auto effects_digest(const value::array& effects) -> std::string;
    auto request_body(const node_request& in, const std::string& project) -> value;
    auto tool_name(std::string_view declared, std::string_view action) -> std::string;
    // `events.ancestors_blob`: the half-open event ranges of a causal closure, compact and ordered.
    auto encode_causal(const causal_set& ancestors) -> std::string;
    auto decode_causal(std::string_view bytes, std::size_t event) -> causal_set;

    struct local_node::impl {
        struct body_view {
            const value* body;
            operator const value&() const { return *body; }
            auto at(std::string_view name) const -> const value& { return body->at(name); }
            auto object() const -> const value::map& { return body->object(); }
        };
        std::filesystem::path root;
        // The workspace's own writer identity: a batch, a receipt and a snapshot all name one writer.
        std::string writer;
        std::unique_ptr<database> db;
        std::string project;
        bool read_only = false;
        std::string observed_snapshot;
        mutable std::map<std::string, value> objects;
        mutable std::map<std::string, body_view> bodies;
        mutable std::deque<std::string> object_order;
        mutable std::unordered_map<std::string, std::size_t> object_sizes;
        mutable std::size_t cached_object_bytes = 0;
        kernel source;
        std::map<std::string, std::vector<std::string>> roots;
        // Worldline names bound by their creation line, built on first use after each reload.
        struct line_index {
            std::map<std::string, std::set<std::string>> names;
        };
        mutable std::optional<line_index> line_names;
        bool dirty = false;
        std::uint64_t data_version = 0;
        std::uint64_t snapshot_watermark = 0, snapshot_events = 0, replayed_events = 0;
        mutable std::uint64_t loaded_records = 0;
        std::string state_snapshot;
        bool snapshot_ready = false, kernel_loaded = true;
        std::uint64_t causal_headers_read = 0, causal_effects_read = 0, file_histories_read = 0, file_payloads_read = 0, projected_before_eviction = 0,
                      manifest_rows_read = 0, manifest_rows_written = 0, blob_reads = 0, blob_writes = 0;
        std::uint64_t state_bytes_read = 0, state_bytes_written = 0, state_bytes_decoded = 0;
        std::set<std::string> cached_payload_files;
        std::uint64_t cached_payload_bytes = 0, cache_evictions = 0;
        std::uint64_t object_count = 0, object_watermark = 0;

        static auto checked_root(std::filesystem::path path) -> std::filesystem::path;
        impl(std::filesystem::path path, bool rebuild, bool reading);
        void metadata(std::string_view name, std::string_view data);
        void trim_kernel();
        void trim_objects();
        auto put(value body) -> std::string;
        auto retain(value object) -> std::string;
        void load_batch(const std::string& id, const value& body);
        void persist(value object, const std::string& id, std::string_view data);
        auto blob(std::string bytes_value) -> std::string;
        auto get(const std::string& id, std::string_view kind = {}) const -> const value&;
        auto has_object(const std::string& id) const -> bool;
        auto object_bytes(const std::string& id) const -> std::string;
        auto object_ids(std::string_view kind = {}) const -> std::vector<std::string>;
        void check_schema(const value& b) const;
        void write_blob(const std::string& digest, std::string_view kind, std::uint64_t raw_bytes, std::uint64_t scalars, std::string_view data);
        auto read_blob(const std::string& digest) -> std::string;
        auto read_chunked_blob(const std::string& digest) -> std::string;
        auto retain_chunked_blob(const std::string& digest, std::string_view data) -> std::uint64_t;
        // A version's manifest names content only: a caller that needs character runs replays this file's effects.
        auto materialize(const source_state& state, const source_file& file) -> std::shared_ptr<const source_file>;
        auto line_of(const std::string& event) const -> std::string;
        // The line a root name stands for: main is the project, every other name is bound by the line object that created it.
        auto lines() const -> const line_index&;
        auto registered_lines(const std::string& name) const -> const std::set<std::string>&;
        auto line_name(const std::string& line) const -> std::string;
        auto worldlines() const -> const std::map<std::string, std::vector<std::string>>&;
        auto named_line(const std::string& name) const -> std::string;
        auto line_base(const std::string& line) const -> std::vector<std::string>;
        // An event belongs to a worldline when the line produced it or the line was forked after it.
        auto in_line(const std::string& line, const std::string& event) const -> bool;
        void within_line(const std::string& line, const std::vector<std::string>& heads) const;
        auto root_line(const std::string& name) const -> std::string;
        // An observation is a state of the line only when it contains the state the line was forked from.
        void observes_line(const std::string& line, const std::vector<std::string>& observed) const;
        void load_objects(std::uint64_t after = 0);
        // The kernel's view of this node's causal storage.
        struct causal_storage final : causal_reader {
            impl& self;
            explicit causal_storage(impl& self) : self(self) {}
            auto header(std::size_t index) -> value override;
            auto locate(const std::string& id) -> std::optional<std::size_t> override;
            auto records(const std::string& kind, const std::string& key) -> std::vector<value> override;
            auto file_text(const source_file& file) -> std::string override;
            auto projection(const std::vector<std::string>& heads) -> std::shared_ptr<const source_state> override;
            auto base(const std::vector<std::string>& heads) -> std::pair<std::vector<std::string>, std::shared_ptr<const source_state>> override;
            auto file_changed(const std::string& file, const causal_set& missing) -> bool override;
        } causal{*this};
        void ensure_kernel();
        void persist_event(std::size_t index);
        void save_snapshot();
        auto restore_snapshot() -> bool;
        void reload(bool rebuild = false);
        void refresh_external();
        void rebuild_worldlines();
        // The revision data of a recorded version: frontier, state root, conflicts, control decisions and manifest chain link.
        auto revision(const std::string& version) const -> std::optional<value>;
        static auto revision_member(const value& data, std::string_view name) -> const value*;
        static auto revision_text(const value& data, std::string_view name) -> std::string;
        static auto revision_number(const value& data, std::string_view name) -> std::uint64_t;
        static auto revision_bool(const value& data, std::string_view name) -> bool;
        static auto conflict_array(const std::map<std::string, std::vector<std::string>>& index) -> value::array;
        static auto conflict_map(const value& data, std::string_view name) -> std::map<std::string, std::vector<std::string>>;
        // The manifest chain of a version: newest first, ending in a complete snapshot.
        auto manifest_chain(const std::string& version) const -> std::vector<std::pair<std::string, value>>;
        // Present files of a version, resolved from its snapshot and at most `manifest_chain_limit` deltas.
        auto manifest_files(const std::string& version) -> std::map<std::string, std::shared_ptr<const source_file>>;
        // The present files a version places at one path, resolved without loading the whole manifest.
        auto manifest_entry(const std::string& version, const std::string& path) -> std::vector<std::shared_ptr<const source_file>>;
        // Whether a version's manifest still holds `file_id`, and at which path.
        auto manifest_state(const std::string& version, const std::string& file_id) const -> std::optional<std::pair<bool, std::string>>;
        // A version's complete row for one file, taken from the newest level that changes it.
        auto manifest_record(const std::string& version, const std::string& file_id) -> std::optional<std::shared_ptr<source_file>>;
        // Persist a version's manifest delta (or snapshot) against `parent`, then the revision that names it.
        void record_revision(const std::string& version_id, const std::vector<std::string>& frontier, const source_state& state,
                             const std::string& parent_version);
        void record_worldline(const std::string& name, const std::vector<std::string>& frontier, bool snapshot = false);
        auto version(const std::vector<std::string>& frontier) -> std::string;
        void root_at(const std::string& name, const std::vector<std::string>& frontier);
        auto identity() -> node_reply;
        auto view(const std::vector<std::string>& frontier, bool include_text = true, std::string_view path = {}) -> node_reply;
        auto compare(const node_request& in, const std::vector<std::string>& heads) -> node_reply;
        auto manifest_differences(const std::vector<std::string>& before_heads, const std::vector<std::string>& after_heads) -> std::vector<source_difference>;
        // The files that differ between two states, with their full contents on both sides.
        // A budget for shown contents: text up to `per_side` bytes a side within `total`, cut between characters; binary content not at all.
        struct shown {
            std::uint64_t per_side, total;
        };
        auto differences(const std::vector<std::string>& before_heads, const std::vector<std::string>& after_heads, std::optional<shown> budget = std::nullopt)
            -> std::vector<source_difference>;
        auto history(const node_request& in, const std::vector<std::string>& heads) -> node_reply;
        // The history entry of batch `id`: one committed contribution.
        auto entry_of(const std::string& id) -> history_entry;
        // One batch as a contribution: its history entry, what it changed against its parents, and the worldlines that contain it.
        auto contribution(const node_request& in) -> node_reply;
        auto receipt(const std::string& id, bool replayed) -> node_reply;
        void binding(const std::string& operation, const std::string& digest, const std::string& result);
        // The receipt of `operation` as this node executed it, if it ran.
        auto executed(const std::string& operation) -> std::optional<std::string>;
        auto retry(const node_request& in, const std::string& digest) -> std::optional<node_reply>;
        // A refused publication keeps its input digest so the same operation ID cannot later run different input.
        void record_refusal(const node_request& in);
        // One committed contribution: its batch, the worldline root it was published on, and the receipt that answers for it.
        auto commit(const node_request& in, const std::string& line, const std::string& request_digest, const std::vector<std::string>& prior,
                    const std::vector<std::string>& frontier, const std::string& event, const std::string& result_id) -> node_reply;
        static void interrupt_at(std::string_view boundary);
        auto coordination(std::string_view name) -> std::optional<value>;
        auto execution_record(const std::string& token) -> value;
        void check_execution(const node_request& in);
        auto execution_reply(const value& record, bool secret) -> node_reply;
        void bind_task(const node_request& in);
        auto task_call(const node_request& in) -> node_reply;
        // Leases are local coordination metadata, never source history or cache identity.
        static auto lease_now() -> std::uint64_t;
        auto scope_leases() -> value::array;
        auto lease_range(const value& lease, const source_state& state) -> std::optional<semantic::place>;
        auto lease_reply(const value& lease, const source_state& state, bool reveal) -> scope_lease;
        auto scope_references() -> value::array;
        auto register_scopes(const node_request& in) -> node_reply;
        // Where registered scope references are in one version of a worldline.
        auto locate_scopes(const node_request& in) -> node_reply;
        // Net CRDT change classification between two versions of a worldline, with optional range reprojection.
        auto classify_scopes(const node_request& in) -> node_reply;
        auto scope_call(const node_request& in) -> node_reply;
        void check_scope_leases(const node_request& in, const std::vector<std::string>& prior);
        // Takes or renews the caller's implicit leases for a committed apply
        // and names how far its text edits reach.
        auto claim_scopes(const node_request& in, const std::vector<std::string>& heads) -> std::string;

        auto promotion(const node_request& in, bool include_text = true) -> std::vector<source_difference>;

        auto effects(const node_request& in, const std::vector<std::string>& heads, const std::string& creating_author = {}) -> value::array;
        auto publish(const node_request& in) -> node_reply;
        auto resolve_revision(const std::string& line, const std::string& selection) -> std::string;
        auto reference_call(const node_request& in) -> node_reply;
        // Reads select a stored revision within the requested worldline.
        auto selected_heads(const node_request& in) -> std::vector<std::string>;
        static constexpr std::size_t content_parts = 10000;
        // Byte ranges of files of one version, named by the SHA-256 of their bytes; builds sync workers from these.
        auto content(const node_request& in) -> node_reply;
        auto mail(const node_request& in) -> node_reply;
        auto worldline_list() -> node_reply;
        // Limits hold for the state a batch publishes, concurrent changes joined: no file it grows or brings back exceeds the file limit,
        // and it may not grow a project beyond the project limit. Zero is no limit.
        template <class State> void check_limits(const node_request& in, const State& before, const State& after) {
            auto total = [](const State& state) {
                std::uint64_t n = 0;
                for (auto& [_, file] : state.files)
                    n += file->bytes();
                return n;
            };
            if (in.file_limit)
                for (auto& [id, file] : after.files)
                    if (auto size = file->bytes(); size > in.file_limit) {
                        auto was = before.files.find(id);
                        if (was == before.files.end() || was->second->bytes() < size)
                            fail(nodes::code::resource_limit, std::format("{} would exceed the {} byte file limit", file->path, in.file_limit));
                    }
            if (in.project_limit)
                if (auto bytes = total(after); bytes > in.project_limit && bytes > total(before))
                    fail(nodes::code::resource_limit, std::format("the project would exceed its {} byte limit", in.project_limit));
        }
    };
} // namespace nodes
