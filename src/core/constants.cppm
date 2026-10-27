export module constants;
import std;

export namespace constants {
    inline constexpr std::size_t workspace_event_buffer_bytes = 65536;
    inline constexpr std::size_t workspace_event_connections_per_core = 64;
    inline constexpr auto workspace_event_heartbeat = std::chrono::seconds{15};
    inline constexpr auto workspace_event_lifetime = std::chrono::minutes{5};
    inline constexpr std::size_t worker_tree_hints = 256;
    inline constexpr std::size_t worker_registry_limit = 1024;
    inline constexpr std::size_t worker_hello_bytes = 128u << 10;
    inline constexpr auto remote_worker_stale = std::chrono::seconds{90};
    inline constexpr std::string_view source_ledger_directory = ".source-usage";
    inline constexpr std::string_view temporary_suffix = ".write-tmp";
    inline constexpr std::string_view lock_file = ".lock";
    inline constexpr std::string_view metrics_prefix = "gateway_";
    inline constexpr std::string_view mcp_server_name = "gateway";
    inline constexpr std::string_view bearer_challenge = "Bearer realm=\"gateway\"";
} // namespace constants
