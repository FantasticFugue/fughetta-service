export module engine:api;
import std;
export import nodes;
import codec;
import kernel;
import archive;
import utility;

export namespace nodes {
    struct node_work {
        std::uint64_t projected_files = 0, manifest_rows_read = 0, blob_reads = 0, manifest_rows_written = 0, blob_writes = 0, state_bytes_written = 0;
        bool causal_loaded = false;
        std::uint64_t causal_headers_read = 0, causal_effects_read = 0, file_histories_read = 0, file_payloads_read = 0, state_bytes_read = 0;
        std::string causal_scope = "none";
        std::uint64_t cache_evictions = 0, state_bytes_decoded = 0;
    };
    // Asked as each write transaction commits; a throw refuses the write.
    class commit_check {
    public:
        virtual void check() = 0;

    protected:
        ~commit_check() = default;
    };

    class local_node {
        struct impl;
        std::unique_ptr<impl> state_;

    public:
        explicit local_node(std::filesystem::path root, bool rebuild = false, bool read_only = false);
        local_node(const local_node&) = delete;
        ~local_node();
        auto call(const node_request& in) -> node_reply;
        auto initialize(std::string genesis_nonce = {}) -> node_reply;
        auto project_id() const -> std::string;
        auto node_id() const -> std::string;
        auto author_id() const -> std::string;
        auto statistics() const -> node_work;
        // `check`, while set, is asked as each write commits.
        void confirm_with(commit_check* check);
    };
} // namespace nodes
