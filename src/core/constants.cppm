export module constants;
import std;

export namespace constants {
    inline constexpr std::uint64_t scope_lease_seconds = 120;
    inline constexpr std::uint64_t scope_max_seconds = 600;
    inline constexpr std::uint64_t scope_reference_limit = 16384;
    inline constexpr std::size_t scope_lease_limit = 1024;
    inline constexpr std::uint64_t mcp_artifact_bytes = 64u << 10;
    inline constexpr std::size_t dependency_manifest_bytes = 1u << 20;
    inline constexpr std::size_t dependency_manifest_entries = 4096;
    inline constexpr std::size_t dependency_request_bytes = 2u << 20;
    inline constexpr std::size_t dependency_status_bytes = 4096;
    inline constexpr std::size_t workspace_event_buffer_bytes = 65536;
    inline constexpr std::size_t workspace_event_connections_per_core = 64;
    // A stream whose reader falls behind is closed at this size instead of buffering without bound.
    inline constexpr std::size_t event_stream_buffer_bytes = 65536;
    inline constexpr std::size_t source_handler_limit = 64;
    inline constexpr auto client_sync_limit = std::chrono::hours{24};
    inline constexpr auto mcp_heartbeat = std::chrono::seconds{15};
    inline constexpr auto workspace_event_heartbeat = std::chrono::seconds{15};
    inline constexpr auto workspace_event_lifetime = std::chrono::minutes{5};
    inline constexpr auto language_idle = std::chrono::minutes{10};
    inline constexpr std::size_t language_sessions = 16;
    inline constexpr std::uint64_t language_memory_bytes = 1ull << 30;
    inline constexpr std::size_t worker_tree_hints = 256;
    inline constexpr std::uint64_t export_bytes = 1ull << 30;
    inline constexpr std::size_t export_files = 4096;
    inline constexpr std::uint64_t log_bytes = 64ull << 20;
    inline constexpr auto export_retention = std::chrono::hours{24 * 7};
    inline constexpr auto export_transfer_grace = std::chrono::minutes{10};
    inline constexpr auto export_collect_interval = std::chrono::minutes{1};
    inline constexpr std::uint64_t export_cache_bytes = 20ull << 30;
    inline constexpr auto dependency_lock_poll = std::chrono::milliseconds{100};
    inline constexpr std::uint64_t dependency_archive_bytes = 8ull << 30;
    inline constexpr std::size_t dependency_archive_count = 65536;
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
