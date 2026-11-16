export module language;
import std;
import native;
import async;
import reflection;
import content;
export import profiles;
export import semantic;

export namespace language {
    struct node {
        std::string id, kind, configuration;
        std::vector<std::string> dependencies;
        std::map<std::string, std::vector<language::input>> inputs;
        std::map<std::string, std::string> outputs;
        std::vector<builds::step> steps;
        std::optional<program> server;
        std::optional<language::execution> execution;
        std::map<std::string, std::string> origins;
    };
    struct graph {
        std::map<std::string, builds::requirements> directory_groups;
        language::defaults defaults;
        std::map<std::string, std::string> origins;
        std::vector<node> nodes;
        std::vector<std::string> stages;
        std::map<std::string, std::vector<reference>> outputs;
    };
    struct plan {
        builds::plan build;
        program server;
        std::vector<builds::context> inputs;
        content::hash fingerprint;
        language::execution execution;
    };
    struct report {
        std::string name;
        std::int32_t exit_code = -1;
        bool timed_out = false;
        double seconds = 0;
        std::string output;
        bool truncated = false;
        std::vector<std::string> tasks;
    };
    struct result {
        std::string status = "not_prepared";
        std::string worldline;
        std::string revision;
        content::hash snapshot;
        std::string tree;
        std::vector<builds::artifact_result> inputs;
        std::vector<builds::artifact_result> artifacts;
        std::vector<std::string> build_dirs;
        std::vector<report> steps;
        std::string error;
        std::optional<session_state> session;
        std::vector<semantic::view_id> stage_views;
        std::vector<semantic::view_id> checkpoint_views;
        rfl::Skip<semantic::retention_roots> roots;
        std::vector<semantic::missing> missing;
        bool cacheable = true;
        std::map<std::string, std::string> toolchains;
        std::uint64_t reused_units = 0;
        // What the worker language session found for the pinned snapshot:
        // "analyzed", "failed" (see analysis_error) or "not_analyzed" when no server is configured.
        std::string analysis = "not_analyzed";
        std::optional<std::string> analysis_error;
        std::string analysis_error_code = "analysis_failed";
        // The worker that prepared and analyzed the snapshot.
        std::string worker;
        std::optional<std::string> worker_id;
        builds::timings timings;
    };
    struct request {
        std::string configuration;
        language::stage config;
        std::string stage = "main";
        std::map<std::string, builds::configuration> builds;
        std::map<std::string, std::vector<semantic::shard_id>> source_facts;
        semantic::request query;
        semantic::budget budget;
        semantic::extent demand;
        std::map<semantic::unit_id, std::vector<semantic::shard_id>> retained;
        semantic::snapshot_vector snapshots;
        semantic::source_vector sources;
        semantic::input_vector project_inputs;
        std::vector<semantic::view_id> project_views;
    };

    auto expand(const configuration& c, const std::map<std::string, builds::configuration>& builds, const std::vector<std::string>& outputs = {},
                const defaults& project = {}) -> graph;
    auto resolve(const stage& c, const defaults& defaults) -> stage;
    auto provider_of(const provider& p) -> std::string_view;
    void validate(const stage& c);
    auto prepare(const stage& c, const std::map<std::string, builds::configuration>& builds) -> plan;
    auto select_builds(const stage& c, const std::map<std::string, builds::configuration>& builds) -> std::map<std::string, builds::configuration>;
    auto fingerprint(const stage& c, const std::map<std::string, builds::configuration>& builds) -> content::hash;
    // Binds a program to one worker: its tree, its tools, its parallel jobs and `cache`, the directory the session may write.
    auto locate(program value, const std::filesystem::path& tree, const builds::toolset& tools, std::uint32_t jobs, const std::filesystem::path& cache)
        -> program;
    // Refuses a plan whose build directories or outputs overlap the tracked files of the tree to prepare.
    void check(const plan& p, const std::vector<content::entry>& files);
    auto timeout(const builds::limits& c) -> std::uint32_t;
    auto output_limit(const builds::limits& c) -> std::uint64_t;
} // namespace language
