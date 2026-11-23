export module nodes;
import std;
import constants;
export import semantic;

export namespace nodes {
    enum class edit_kind : std::uint8_t { edit, write, binary, create, remove, move, mode, directory_move, recreate, binary_create, binary_recreate };
    struct edit {
        edit_kind kind;
        std::string path;
        std::string text;
        std::string target;
        std::uint64_t offset = 0;
        std::uint64_t count = 0;
        bool executable = false;
        std::string document;
    };
    // A byte range of a file, named by the digest of the whole file: reads ask for one, replies carry it.
    struct object_part {
        std::string id;
        std::uint64_t offset = 0;
        std::uint64_t total = 0;
        std::string data;
    };
    struct line_view {
        std::string name, version, base_version, creator;
    };
    struct mail_input {
        std::string recipient, body;
    };
    struct mail_message {
        std::uint64_t id = 0, at = 0;
        std::string sender, worldline, recipient, body, operation;
    };
    struct scope_lease {
        std::string token, scope, path, worldline, version, owner;
        std::uint64_t begin = 0, end = 0, expires_at = 0;
        bool valid = true;
        bool implicit = false;
        std::string kind = "scope";
    };
    struct execution_view {
        std::string task, run, worldline, token, identity;
        bool active = true;
    };
    struct attestation {
        std::string kind, receipt, conclusion;
    };
    struct reference_input {
        std::string name, kind = "tag";
        // Empty creates a name; updates name the last reference receipt.
        std::string expected;
        std::vector<attestation> attestations;
    };
    struct reference_record {
        std::string name, kind, worldline, version, actor, intent, operation_id, receipt, previous;
        std::uint64_t sequence = 0, created_at = 0;
        std::vector<attestation> attestations;
    };
    struct scope_reprojection {
        // Subset of body_ranges whose interior edits cannot alter any external declaration signature.
        std::set<std::string> sealed_bodies;
        // Cross-language FFI contract declarations (JNI / PInvoke / pybind) anchored for zero-compile break detection.
        std::map<std::string, semantic::place> ffi_boundaries;
        // scope_classify: source ranges in `target` (base revision) to reproject into `revision` via CRDT anchors.
        std::vector<semantic::place> reproject_places;
    };
    struct node_request {
        std::string action;
        std::optional<mail_input> mail;
        std::optional<reference_input> reference;
        std::uint64_t after = 0;
        std::string worldline = "main";
        std::string revision;
        std::string lease_owner, lease_token;
        std::vector<std::string> leases;
        std::uint64_t lease_seconds = constants::scope_lease_seconds;
        std::string operation;
        std::string owner;
        std::string actor;
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
        std::vector<object_part> parts;
        std::string configuration;
        std::string environment;
        bool force = false;
        bool capture = false;
        // Size limits for the published state, in bytes; zero is none. The workspace limit bounds the stored history of all its projects.
        std::uint64_t file_limit = 0, project_limit = 0, workspace_limit = 0;
        std::uint64_t max_bytes = 1024 * 1024;
        std::uint64_t limit = 100;
        std::string task_id, run_id, execution_token, execution_identity, execution_owner;
        bool execution_writable = true;
        std::vector<std::string> scope_refs, scope_files, scope_entries;
        std::vector<semantic::records::scope> analyzed_scopes;
        std::map<std::string, semantic::place> declaration_ranges;
        // Function bodies by symbol, registered as references without replying with them.
        std::map<std::string, semantic::place> body_ranges;
        scope_reprojection scope_hints;
    };
    struct scope_classification {
        bool body_only = false;
        bool sealed_body_only = false;
        bool comment_or_whitespace_only = false;
        bool ffi_boundary_touched = false;
        std::vector<std::string> touched_files;
        std::vector<std::string> touched_symbols;
        std::vector<std::string> touched_ffi_symbols;
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
        std::string event, operation, author, actor, tool, intent, result, line;
        std::vector<std::string> parents, dependencies;
        std::uint64_t lamport = 0;
    };
    struct node_reply {
        std::string code = "ok";
        std::string reason;
        std::string project, node, author;
        std::string operation;
        std::string version, source_digest, receipt, issuer, result;
        // The submitted edit on its observed base, before joining concurrent publications.
        std::string written_version;
        std::string base_version;
        std::string worldline;
        std::vector<std::string> frontier, missing, conflicts, objects;
        std::vector<object_part> parts;
        std::vector<file_view> files;
        std::vector<source_difference> changes;
        std::vector<history_entry> history;
        std::vector<reference_record> references;
        std::map<std::string, std::vector<std::string>> worldlines;
        std::vector<line_view> lines;
        std::vector<scope_lease> leases;
        std::vector<mail_message> messages;
        // restore: the project configuration the package held, as JSON.
        std::string configuration;
        bool stale = false;
        bool replayed = false;
        bool complete = true;
        bool truncated = false;
        std::string completeness_scope = "none";
        std::uint64_t storage_bytes = 0;
        std::vector<semantic::records::scope> scopes;
        std::map<std::string, semantic::source_anchor> declaration_anchors;
        // scope_locate: each requested reference's range in the selected version.
        std::map<std::string, semantic::place> located;
        // apply: body_only when every text edit stays inside a registered body, else declaration_touched.
        std::string reach;
        // scope_classify: net CRDT change classification from `target` to `revision`, plus reprojected places.
        std::optional<scope_classification> classification;
        std::vector<std::optional<semantic::place>> reprojected;
        std::optional<execution_view> execution;
        std::string state_snapshot;
        std::uint64_t loaded_records = 0, replayed_events = 0, snapshot_events = 0;
    };
} // namespace nodes
