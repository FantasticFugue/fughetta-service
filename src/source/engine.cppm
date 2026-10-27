export module engine;
import std;
export import nodes;
import codec;
import kernel;
import archive;
import utility;

export namespace nodes {
    inline auto encode_packet(const std::vector<object_part>& parts) -> std::string {
        value::array encoded;
        for (auto& part : parts)
            encoded.emplace_back(value::map{{"data", bytes{unhex(part.data)}}, {"id", part.id}, {"offset", part.offset}, {"total", part.total}});
        return codec::encode(value::map{{"parts", std::move(encoded)}});
    }
    inline auto decode_packet(std::string_view packet) -> std::vector<object_part> {
        auto decoded = codec::decode(packet);
        fields(decoded, {"parts"});
        std::vector<object_part> parts;
        for (auto& v : decoded.at("parts").list()) {
            fields(v, {"data", "id", "offset", "total"});
            parts.push_back({v.at("id").text(), v.at("offset").number(), v.at("total").number(), hex(v.at("data").raw())});
        }
        return parts;
    }
    struct node_work {
        std::uint64_t projected_files = 0, state_nodes_read = 0, state_values_read = 0, state_nodes_written = 0, state_values_written = 0,
                      state_bytes_written = 0;
        bool causal_loaded = false;
        std::uint64_t causal_headers_read = 0, causal_effects_read = 0, file_histories_read = 0, file_payloads_read = 0, state_bytes_read = 0;
        std::string causal_scope = "none";
        std::uint64_t sequence_pages_read = 0, sequence_pages_written = 0, insertion_payloads_read = 0, cache_evictions = 0, state_bytes_decoded = 0,
                      causal_blocks_read = 0;
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
        explicit local_node(std::filesystem::path root, std::string trusted_project = {}, bool audit = false, bool read_only = false);
        local_node(const local_node&) = delete;
        ~local_node();
        auto call(const node_request& in) -> node_reply;
        auto initialize(std::string genesis_nonce = {}) -> node_reply;
        auto project_id() const -> std::string;
        auto node_id() const -> std::string;
        auto author_id() const -> std::string;
        auto statistics() const -> node_work;
        void use_author(key signer);
        // `check`, while set, is asked as each write commits.
        void confirm_with(commit_check* check);
        auto objects(const std::vector<std::string>& ids) -> std::vector<object_part>;
        auto receive_packet(std::string_view packet) -> node_reply;
        auto export_stream(std::ostream& output, std::string known_checkpoint = {}) -> node_reply;
        auto import_stream(std::istream& input) -> node_reply;
    };
} // namespace nodes
