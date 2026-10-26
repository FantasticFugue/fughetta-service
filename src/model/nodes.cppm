export module nodes;
import std;
export import semantic;

export namespace nodes {
    struct edit {
        std::string kind;
        std::string path;
        std::string text;
        std::string target;
        std::uint64_t offset = 0;
        std::uint64_t count = 0;
        bool executable = false;
        std::string document;
    };
    struct object_part {
        std::string id;
        std::uint64_t offset = 0;
        std::uint64_t total = 0;
        std::string data;
    };
    // A writer's hold on one syntax scope of a worldline, as the named analysis environment reported it.
    // The global namespace "::" holds everything outside classes, functions and namespaces, and every file.
    struct lock {
        std::uint64_t id = 0;
        std::string holder, scope, symbol, environment, document;
        std::optional<semantic::anchor> first, last;
        std::optional<semantic::place> place;
        std::int64_t acquired = 0;
    };
    inline constexpr std::string_view global_scope = "::";
    struct line_view {
        std::string name, version, base_version, creator;
    };
    struct node_request {
        std::string action;
        std::string worldline = "main";
        std::string revision, snapshot;
        std::string operation;
        std::string owner;
        std::string actor;
        std::string holder;
        std::string caller_operation;
        std::string genesis_nonce;
        std::vector<std::string> expected;
        std::vector<std::string> source;
        std::vector<edit> edits;
        std::string target;
        std::optional<bool> activation;
        std::string intent;
        std::string result;
        std::string tool;
        std::vector<std::string> dependencies;
        std::vector<std::string> decisions;
        std::vector<std::string> authors;
        std::vector<std::string> nodes;
        std::vector<std::string> policies;
        std::vector<std::string> settled;
        std::vector<std::string> protected_worldlines{"main"};
        std::vector<std::string> want;
        std::vector<object_part> parts;
        std::string configuration;
        std::string environment;
        std::optional<semantic::generation> generation;
        std::vector<semantic::span> spans;
        std::vector<std::string> scopes;
        bool force = false;
        bool capture = false;
        // Size limits for the published state, in bytes; zero is none. The workspace limit bounds the stored history of all its projects.
        std::uint64_t file_limit = 0, project_limit = 0, workspace_limit = 0;
        std::uint64_t max_bytes = 1024 * 1024;
        std::uint64_t limit = 100;
    };
    struct file_view {
        std::string id, path, kind, digest, text;
        std::uint64_t size = 0;
        bool executable = false;
        std::string encoding = "utf8";
    };
    struct source_difference {
        std::string type;
        std::optional<file_view> before, after;
    };
    struct history_entry {
        std::string event, operation, author, actor, executor, tool, intent, result, outcome, policy, line;
        std::vector<std::string> parents, dependencies, receipts;
        std::uint64_t lamport = 0;
    };
    struct node_reply {
        std::string code = "ok";
        std::string reason;
        std::string project, node, author;
        std::string operation;
        std::string version, source_digest, receipt, issuer, result, checkpoint;
        std::string materialized_version;
        std::string base_version;
        std::string receipt_status;
        std::string worldline;
        std::vector<std::string> frontier, missing, conflicts, objects, policies;
        std::vector<std::string> identity_conflicts, refused;
        std::vector<object_part> parts;
        std::vector<file_view> files;
        std::vector<source_difference> changes;
        std::vector<history_entry> history;
        std::map<std::string, std::vector<std::string>> worldlines;
        std::optional<semantic::generation> semantic;
        std::vector<semantic::span> spans;
        std::vector<lock> locks;
        std::vector<line_view> lines;
        // restore: the project configuration the package held, as JSON.
        std::string configuration;
        bool stale = false;
        bool replayed = false;
        bool complete = true;
        bool truncated = false;
        std::string completeness_scope = "none";
        std::uint64_t storage_bytes = 0;
        std::string state_snapshot;
        std::uint64_t loaded_records = 0, replayed_events = 0, snapshot_events = 0;
    };
} // namespace nodes
