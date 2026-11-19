export module protocol;
import std;
import constants;
export import reuse;
export import utility;
import json;
import coroutine;
import native;
import meta;
export import content;
export import world;
export import nodes;
export import build;
export import language;
export import runner;

export namespace jobs {
    struct platform {
        std::string os;
        std::string arch;
    };

    struct warm_session {
        std::string stage, provider, options, snapshot;
        std::uint64_t epoch = 0;
        std::optional<io::process_usage> usage;
        double rebuild_seconds = 0;
        std::uint64_t high_water_bytes = 0;
    };
    struct cached_session {
        std::string stage, options, snapshot, toolchain;
    };
    struct worker_slot {
        std::string project, tree;
        bool busy = false;
        std::vector<warm_session> sessions;
        std::string snapshot;
        std::vector<cached_session> cached;
    };

    struct hello {
        std::string name;
        std::string auth;
        jobs::platform platform;
        std::uint32_t compute_threads = 0;
        // Dispatches of the execute role the worker runs at once, and of the analyze role beside them.
        std::uint32_t max_inflight = 1;
        std::uint32_t max_analyses = 1;
        std::vector<std::string> roles{std::string{constants::execute_role}, std::string{constants::analyze_role}};
        std::uint32_t protocol = 0;
        std::vector<std::string> tools;
        std::optional<std::string> worker_id;
        std::optional<std::string> workspace_id;
        std::uint32_t heartbeat_seconds = 2;
        std::string toolchain_id;
        bool dependency_publisher = false;
        std::optional<std::uint64_t> memory_bytes;
        std::vector<worker_slot> slots;
    };

    struct admission {
        bool accepted = false;
        std::string code;
        std::string worker_id;
    };

    struct heartbeat {
        std::uint32_t running = 0;
        std::uint32_t queued = 0;
    };
    struct worker_memory {
        std::optional<std::uint64_t> limit;
        std::uint64_t reserved = 0;
        std::optional<std::uint64_t> resident, baseline;
    };
    struct tree_holds {
        std::uint64_t sequence = 0;
        std::set<std::string> trees;
        std::optional<std::uint64_t> memory_bytes;
    };
    struct slot_report {
        std::vector<worker_slot> slots;
        std::uint64_t sequence = 0;
        worker_memory memory;
    };

    struct progress {
        std::uint64_t done = 0;
        std::uint64_t total = 0;
        std::string stage;
        std::optional<language::evaluation_identity> execution;
    };

    // The progress of one call, kept where the call is awaited: the core that hears a report appends it, and the caller reads what is new
    // when it looks.
    class progress_log {
    public:
        void push(progress p) {
            std::lock_guard held{lock_};
            entries_.push_back(std::move(p));
        }
        // The reports since `seen`, which moves past them.
        auto since(std::size_t& seen) -> std::vector<progress> {
            std::lock_guard held{lock_};
            std::vector<progress> out(entries_.begin() + static_cast<std::ptrdiff_t>(std::min(seen, entries_.size())), entries_.end());
            seen = entries_.size();
            return out;
        }

    private:
        std::mutex lock_;
        std::vector<progress> entries_;
    };

    struct cancel {
        std::string why;
        std::optional<language::evaluation_identity> execution;
    };

    struct failure {
        std::string message;
        std::string code;
    };

    namespace why {
        inline constexpr std::string_view stale_blob = "stale_blob";
        inline constexpr std::string_view stale_head = "stale_head";
        inline constexpr std::string_view receipt_conflict = "receipt_conflict";
        inline constexpr std::string_view unknown_operation = "unknown_operation";
        inline constexpr std::string_view stale_inputs = "stale_inputs";
    } // namespace why

    struct hash_file {
        json::described<"Path relative to the worker's data root; first segment names the dataset", std::string> path;
        std::optional<std::string> algo;
    };

    struct digest {
        std::string hex;
        std::uint64_t bytes = 0;
    };

    inline auto locate(const hash_file& j) -> std::string_view {
        std::string_view p = j.path.get();
        return p.substr(0, p.find('/'));
    }

    struct sync_offer {
        std::string project;
        content::manifest tree;
        std::optional<content::hash> retained;
    };

    struct sync_plan {
        content::hash snapshot;
        std::vector<content::hash> missing;
        bool needs_tree = false;
    };

    // The most plain bytes one piece of a blob holds.
    inline constexpr std::uint64_t part_size = 4u << 20;

    // A piece of a blob. `offset` and `total` count plain bytes; `packed` bytes are one archive frame of the piece.
    struct source_part {
        content::hash digest;
        std::uint64_t offset = 0;
        std::uint64_t total = 0;
        std::vector<std::byte> bytes;
        bool packed = false;
    };

    struct source_bytes {
        std::vector<source_part> parts;
    };
    struct export_read {
        content::hash digest;
        std::uint64_t offset = 0;
        std::uint64_t length = part_size;
    };

    struct authority_ack {
        std::uint64_t stored = 0;
    };

    inline auto source_token() -> std::string {
        auto value = std::getenv("SOURCE_TOKEN");
        if (!value || !*value)
            throw std::invalid_argument("set SOURCE_TOKEN for authenticated source RPC");
        return value;
    }

    struct node_call {
        std::string project;
        worlds::actor actor;
        nodes::node_request request;
        bool reading = false;
    };
    // What the node repositories hold: each project's stored history on disk and the size of its main source.
    struct node_usage {
        worlds::actor actor;
    };
    struct project_usage {
        std::string project;
        std::uint64_t storage_bytes = 0, source_bytes = 0;
    };
    struct node_usage_reply {
        std::vector<project_usage> projects;
        // Exported packages, which count toward the storage limit too.
        std::uint64_t packages_bytes = 0;
    };
    // Everything the workspace stores, as its storage limit counts it.
    inline auto stored(const node_usage_reply& r) -> std::uint64_t {
        auto n = r.packages_bytes;
        for (auto& p : r.projects)
            n += p.storage_bytes;
        return n;
    }
    struct node_capture {
        std::string project;
        std::vector<std::string> ignore;
        worlds::actor actor;
        std::string operation;
        std::vector<std::string> expected;
        std::string intent;
        std::uint64_t file_limit = 0, project_limit = 0, workspace_limit = 0;
    };

    using step = builds::step;

    // A language server to ask about a tree's documents once its steps succeeded.
    struct analysis_plan {
        std::string provider;
        std::string options, configuration;
        semantic::input_vector inputs;
        std::vector<builds::context> artifacts;
        std::vector<std::string> argv;
        json::value initialization;
        std::map<std::string, std::string> env;
        std::uint32_t seconds = 600;
        semantic::request query;
        semantic::budget budget;
        language::evaluation_identity execution;
        std::string slot_id;
        semantic::extent demand;
        std::map<std::string, std::vector<semantic::shard_id>> source_facts;
        std::map<std::string, std::vector<std::string>> commands;
        std::vector<std::string> executables;
        std::vector<semantic::candidate> candidates;
        std::map<semantic::unit_id, std::vector<semantic::shard_id>> retained;
        std::optional<language::description> registration;
        std::map<std::string, std::string> documents;
        // The provider runs code the project supplies, so its session takes the project's build environment.
        // A provider that only reads project data runs in the worker's own environment.
        bool project_code = false;
        // The provider loads the plugins its project model names; the worker runs it as project code when the model names any.
        bool model_code = false;
    };

    // These labels identify files in one transfer, not semantic units.
    inline auto analysis_exports(const analysis_plan& analysis) -> std::map<std::string, builds::context> {
        std::map<std::string, builds::context> out;
        // Only a domain reader takes its artifacts as files.
        if (analysis.registration ? !analysis.registration->reads
                                  : !language::providers::dispatch(analysis.provider, []<class Provider>() { return language::domain_reader<Provider>; }))
            return out;
        for (std::size_t index = 0; index < analysis.artifacts.size(); ++index)
            out.emplace("analysis-artifact-" + std::to_string(index), analysis.artifacts[index]);
        return out;
    }

    struct worktree_capture {
        std::vector<std::string> prefixes, excluded;
        std::uint64_t file_limit = constants::worktree_capture_bytes;

        static auto within(std::string_view path, std::string_view prefix) -> bool {
            return path == prefix || (path.starts_with(prefix) && path.size() > prefix.size() && path[prefix.size()] == '/');
        }
        auto excludes(std::string_view path) const -> bool {
            for (auto part : std::views::split(path, '/')) {
                const std::string_view name{part.begin(), part.end()};
                if (name == ".git" || name == ".hg" || name == ".svn" || name == "__pycache__" ||
                    name == ".pytest_cache" || name == ".ruff_cache" || name == "node_modules" || name == ".fughetta")
                    return true;
            }
            return std::ranges::any_of(excluded, [&](const auto& prefix) { return prefix == "." || within(path, prefix); });
        }
        auto includes(std::string_view path) const -> bool {
            return !excludes(path) && std::ranges::any_of(prefixes, [&](const auto& prefix) { return within(path, prefix); });
        }
    };

    struct worktree_change {
        std::string path;
        // Absent for removal and mode-only changes; otherwise the entire new file, encoded as hex.
        std::optional<std::string> bytes;
        std::optional<bool> executable;
        auto operator==(const worktree_change&) const -> bool = default;
    };

    struct build_request {
        std::string project;
        std::string tree;
        content::hash snapshot;
        std::vector<step> steps;
        std::vector<builds::task_plan> preparation_tasks;
        std::uint32_t timeout_s = 1800;
        std::uint64_t output_limit = 1u << 20;
        std::map<std::string, std::string> env;
        std::optional<std::uint64_t> max_memory_bytes;
        std::optional<std::uint32_t> max_cpu_seconds;
        // Measure outputs and analyze the tree after successful steps. These operations must preserve tracked source files.
        std::vector<builds::context> measure;
        bool fresh = false;
        std::map<std::string, std::string> exports;
        // Complete producer inventory for this directory group, including other analysis stages.
        std::set<std::string> generation_producers;
        // The analysis stage this build prepares the tree for: its report names the inputs it leaves, and a
        // preparation task that already ran on this snapshot is not run again.
        std::optional<std::string> prepares;
        std::vector<std::string> relocatable_build_dirs;
        std::optional<content::hash> parent_snapshot;
        std::optional<worktree_capture> capture_worktree_changes;
    };

    // What a preparation leaves for the analysis of one snapshot, by content: the measured outputs of its steps.
    // The files stay in the build tree; an analysis names the inventory it reads by `id`.
    struct analysis_inputs {
        content::hash snapshot;
        std::vector<builds::artifact_result> outputs;
        // Advances whenever a preparation step runs in the tree: a measured directory has no digest.
        std::uint64_t generation = 0;
        auto id() const -> content::hash { return content::hash_of(json::write(*this)); }
    };

    // An analysis of a prepared tree. `inputs` is the inventory its preparation reported; a worker whose tree
    // holds another refuses with stale_inputs.
    struct analysis_request {
        std::string project;
        std::string tree;
        content::hash snapshot;
        content::hash inputs;
        std::uint32_t timeout_s = 1800;
        std::map<std::string, std::string> env;
        std::optional<std::uint64_t> max_memory_bytes;
        std::optional<std::uint32_t> max_cpu_seconds;
        analysis_plan analysis;
    };

    // What a caller wants analyzed: sent as the preparation's build, then the analysis of the inputs it reports.
    struct staged_analysis {
        build_request preparation;
        analysis_plan analysis;
    };

    // The analysis of a staged request on the inputs its preparation reported, with the time left since `started`.
    inline auto analysis_of(const staged_analysis& work, content::hash inputs, std::chrono::steady_clock::time_point started) -> analysis_request {
        const auto& preparation = work.preparation;
        const auto spent = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        return {.project = preparation.project,
                .tree = preparation.tree,
                .snapshot = preparation.snapshot,
                .inputs = std::move(inputs),
                .timeout_s = preparation.timeout_s - std::min<std::uint32_t>(preparation.timeout_s, spent),
                .env = preparation.env,
                .max_memory_bytes = preparation.max_memory_bytes,
                .max_cpu_seconds = preparation.max_cpu_seconds,
                .analysis = work.analysis};
    }

    struct dependency_fill {
        builds::vcpkg_manifest manifest;
        std::string triplet;
        std::string host_triplet;
        std::vector<std::string> features;
        bool default_features = true;
        std::uint32_t timeout_s = 1800;
    };

    // Facts are staged until the separate commit event names their complete inventory.
    struct facts_batch {
        std::uint64_t offset = 0;
        std::vector<std::byte> data;
    };
    struct unit_contribution {
        std::string unit;
        semantic::contribution records;
        std::uint64_t typed_ranges = 0;
        friend auto operator==(const unit_contribution&, const unit_contribution&) -> bool = default;
    };
    inline auto inventory(const language::evaluated_unit& unit) -> std::vector<unit_contribution> {
        std::vector<unit_contribution> out;
        for (const auto& shard : unit.shards)
            out.push_back({shard.unit.id, semantic::contributions(shard), shard.typed_ranges.size()});
        return out;
    }
    struct unit_committed {
        language::evaluation_identity execution;
        std::string slot_id;
        semantic::unit_key unit;
        std::uint64_t epoch = 0, bytes = 0;
        std::vector<unit_contribution> contributions;
    };
    struct analysis_completed {
        language::evaluation_identity execution;
        std::uint64_t committed_units = 0;
    };
    struct analysis_failed {
        language::evaluation_identity execution;
        std::uint64_t committed_units = 0;
        std::string code, message;
    };
    struct reuse_request {
        language::evaluation_identity execution;
        language::session_state session;
        semantic::input_vector inputs;
    };
    struct reuse_reply {
        std::vector<reuse::key> accepted;
    };
    struct shard_read {
        language::evaluation_identity execution;
        semantic::shard_id id;
        std::uint64_t offset = 0, limit = 0;
    };
    struct shard_piece {
        std::uint64_t offset = 0, total = 0;
        std::vector<std::byte> data;
    };

    struct model_read {
        std::uint64_t parent = 0;
        content::hash model;
        export_read object;
    };

    template <class T> struct analysis_response;
    template <> struct analysis_response<language::evaluated_unit> {
        using type = std::monostate;
    };
    template <> struct analysis_response<reuse_request> {
        using type = reuse_reply;
    };
    template <> struct analysis_response<shard_read> {
        using type = shard_piece;
    };
    template <> struct analysis_response<model_read> {
        using type = source_bytes;
    };

    // Request-scoped rendezvous. A producer cannot send another message until
    // the owning task has consumed and answered the current one.
    class analysis_exchange {
        class signal {
            std::mutex mutex_;
            bool ready_ = false;
            std::coroutine_handle<> waiter_;

        public:
            struct awaiter {
                signal& owner;
                auto await_ready() const noexcept -> bool { return false; }
                auto await_suspend(std::coroutine_handle<> continuation) -> bool {
                    std::lock_guard held{owner.mutex_};
                    if (owner.ready_)
                        return false;
                    owner.waiter_ = continuation;
                    return true;
                }
                void await_resume() const noexcept {}
            };
            auto wait() -> awaiter { return {*this}; }
            void reset() {
                std::lock_guard held{mutex_};
                ready_ = false;
            }
            void set() {
                std::coroutine_handle<> next;
                {
                    std::lock_guard held{mutex_};
                    ready_ = true;
                    next = std::exchange(waiter_, {});
                }
                if (next)
                    next.resume();
            }
        };

    public:
        using request = std::variant<language::evaluated_unit, reuse_request, shard_read, model_read>;
        using response = std::variant<std::monostate, reuse_reply, shard_piece, source_bytes>;
        struct message {
            request data;
            explicit message(request data) : data(std::move(data)), unanswered_(util::make_exception_ptr(std::runtime_error("analysis input task stopped"))) {}
            template <class T> void answer(T value) {
                {
                    std::lock_guard held{mutex};
                    if (done)
                        return;
                    value_ = std::move(value);
                    done = true;
                }
                ready.set();
            }
            void fail(std::exception_ptr error) {
                {
                    std::lock_guard held{mutex};
                    if (done)
                        return;
                    error_ = std::move(error);
                    done = true;
                }
                ready.set();
            }
            void abandon() { fail(unanswered_); }

        private:
            friend class analysis_exchange;
            std::mutex mutex;
            signal ready;
            bool done = false;
            response value_;
            std::exception_ptr error_, unanswered_;
            auto take() -> response {
                std::lock_guard held{mutex};
                if (error_)
                    std::rethrow_exception(error_);
                return std::move(value_);
            }
        };
        struct lifetime {
            std::shared_ptr<analysis_exchange> inputs;
            ~lifetime() { inputs->close(); }
        };
        struct completion {
            std::shared_ptr<message> item;
            ~completion() { item->abandon(); }
        };
        analysis_exchange(native::scheduler producer, native::scheduler consumer) : producer_(producer), consumer_(consumer) {}
        template <class T> auto ask(T value) -> coroutine::task<typename analysis_response<T>::type> {
            co_await execution::schedule(producer_);
            auto serial = co_await writing_.lock();
            auto item = std::make_shared<message>(request{std::move(value)});
            {
                std::lock_guard held{mutex_};
                if (closed_)
                    throw std::runtime_error("analysis input exchange is closed");
                pending_ = item;
                taken_ = false;
            }
            available_.set();
            co_await item->ready.wait();
            co_await coroutine::under(coroutine::stop_token{}, arrive(producer_));
            {
                std::lock_guard held{mutex_};
                pending_.reset();
            }
            co_return std::get<typename analysis_response<T>::type>(item->take());
        }
        auto next() -> coroutine::task<std::shared_ptr<message>> {
            co_await available_.wait();
            co_await execution::schedule(consumer_);
            std::lock_guard held{mutex_};
            if (closed_)
                co_return nullptr;
            if (!pending_ || taken_)
                throw std::logic_error("analysis exchange has no pending message");
            taken_ = true;
            available_.reset();
            co_return pending_;
        }
        void close() {
            std::shared_ptr<message> item;
            {
                std::lock_guard held{mutex_};
                closed_ = true;
                item = pending_;
            }
            if (item)
                item->abandon();
            available_.set();
        }

    private:
        static auto arrive(native::scheduler scheduler) -> coroutine::task<> { co_await execution::schedule(scheduler); }
        native::scheduler producer_, consumer_;
        coroutine::mutex writing_;
        std::mutex mutex_;
        signal available_;
        std::shared_ptr<message> pending_;
        bool closed_ = false, taken_ = false;
    };
    using analysis_link = std::shared_ptr<analysis_exchange>;

    struct build_report {
        std::string worker;
        std::optional<std::string> worker_id;
        std::string tree;
        std::vector<runner::result> steps;
        double seconds = 0;
        std::vector<builds::artifact_result> measured;
        // Survey coverage summary and covered documents; facts arrive in unit batches.
        bool analyzed = false, analysis_cacheable = true;
        std::optional<language::session_state> analysis_session;
        std::optional<language::evaluation_identity> analysis_execution;
        std::uint64_t committed_units = 0;
        // Filled by the gateway after durable unit publication.
        std::vector<semantic::view_id> stage_views;
        std::vector<semantic::missing> missing;
        std::uint64_t reused_units = 0;
        std::optional<std::string> analysis_error;
        std::string analysis_error_code = "analysis_failed";
        std::vector<builds::exported_file> exports;
        std::vector<builds::exported_file> models;
        std::string export_error;
        std::vector<std::string> export_skipped;
        std::optional<double> preparation_seconds, execution_seconds;
        // Set by a build that prepares an analysis, once every step succeeded.
        std::optional<analysis_inputs> inputs;

        std::vector<semantic::execution_receipt> receipts;
        // Filled only after the control plane verifies the corresponding receipt.
        std::vector<semantic::artifact_receipt> artifact_audit;
        std::optional<std::vector<worktree_change>> worktree_changes;
        std::string worktree_capture_error;

        auto ok() const -> bool {
            return std::ranges::all_of(steps, [](auto& s) { return s.exit_code == 0 && !s.timed_out; });
        }
    };

    inline auto shard_claim(language::evaluated_unit& unit, semantic::shard& shard, std::string workspace, std::string worker)
        -> semantic::execution_receipt {
        return {std::move(workspace), std::move(worker), unit.session.snapshot, semantic::recipe_identifier(shard.unit.recipe),
                "semantic_shard", semantic::shard_key(shard), content::hash_of(json::write(unit.request.execution)).str(), {}};
    }

    template <class Request> auto artifact_recipe(const Request& request) -> std::string {
        return content::hash_of(json::write(request)).str();
    }

    struct artifact_submission {
        semantic::execution_receipt receipt;
        std::string manifest;
    };

    inline auto artifact_claim(const build_report& report, std::string snapshot, std::string recipe, std::string workspace, std::string worker)
        -> std::optional<artifact_submission> {
        std::vector<std::pair<std::size_t, builds::exported_file>> logs;
        for (std::size_t index = 0; index < report.steps.size(); ++index)
            if (report.steps[index].log)
                logs.emplace_back(index, *report.steps[index].log);
        if (report.measured.empty() && report.exports.empty() && report.models.empty() && logs.empty() && !report.inputs)
            return {};
        auto manifest = json::write(std::tie(report.measured, report.exports, report.models, logs, report.inputs));
        return artifact_submission{
            {std::move(workspace), std::move(worker), std::move(snapshot), std::move(recipe), "artifact_manifest", content::hash_of(manifest).str(), {}, {}},
            std::move(manifest)};
    }

    struct receipt_signer {
        const content::receipt_key* key = nullptr;
        std::string workspace, worker;
        auto sign(semantic::execution_receipt claim) const -> semantic::execution_receipt {
            claim.signature = key->sign(claim.payload());
            return claim;
        }
    };

    // One report over a preparation and the analysis that followed it.
    inline void merge(build_report& analyzed, build_report prepared) {
        analyzed.steps.insert(analyzed.steps.begin(), std::make_move_iterator(prepared.steps.begin()), std::make_move_iterator(prepared.steps.end()));
        analyzed.exports.insert(analyzed.exports.begin(), std::make_move_iterator(prepared.exports.begin()), std::make_move_iterator(prepared.exports.end()));
        analyzed.models = std::move(prepared.models);
        std::ranges::move(prepared.artifact_audit, std::back_inserter(analyzed.artifact_audit));
        analyzed.export_error = std::move(prepared.export_error);
        analyzed.export_skipped = std::move(prepared.export_skipped);
        analyzed.inputs = std::move(prepared.inputs);
        analyzed.preparation_seconds = prepared.preparation_seconds;
        analyzed.execution_seconds = prepared.execution_seconds.value_or(0) + analyzed.execution_seconds.value_or(0);
        analyzed.seconds += prepared.seconds;
    }

    template <meta::fixed_string Name> inline constexpr std::uint32_t kind = meta::fnv1a(Name);

    template <meta::fixed_string Name, class Message> struct signal {
        using message = Message;
        static constexpr std::uint32_t code = kind<Name>;
    };

    template <class Request> struct traits {};
    template <> struct traits<build_request> {
        static auto preparation(auto& request) -> decltype(auto) { return (request); }
        static auto analysis(auto&) -> analysis_plan* { return nullptr; }
    };
    template <> struct traits<staged_analysis> {
        static auto preparation(auto& request) -> decltype(auto) { return (request.preparation); }
        static auto analysis(auto& request) { return &request.analysis; }
    };
    template <> struct traits<analysis_request> {
        static auto identity(const analysis_request& request) { return request.analysis.execution; }
        static auto slot(const analysis_request& request) { return request.analysis.slot_id; }
        static void validate(const build_report& report, const language::evaluation_identity& identity, std::uint64_t count, bool succeeded) {
            if (report.analysis_execution != identity || report.committed_units != count || (report.ok() && !report.analysis_error) != succeeded)
                throw std::invalid_argument("provider reply differs from its terminal event");
        }
        static auto terminal(const std::optional<build_report>& report, const language::evaluation_identity& identity, std::uint64_t count,
                             const std::string& error, const std::string& code) -> std::variant<analysis_completed, analysis_failed> {
            if (report && report->ok() && !report->analysis_error)
                return analysis_completed{identity, count};
            auto failed_code = report ? (report->analysis_error ? report->analysis_error_code : "resource_limit") : code;
            auto message = report ? report->analysis_error.value_or("execution time budget exhausted") : error;
            return analysis_failed{identity, count, std::move(failed_code), std::move(message)};
        }
    };

    template <class Request>
    concept execution_request = std::same_as<Request, build_request> || std::same_as<Request, analysis_request>;

    template <meta::fixed_string Name, class Request, class Reply> struct rpc {
        using request = Request;
        using reply = Reply;
        static constexpr std::uint32_t code = kind<Name>;
    };

    using signals = meta::list<signal<"hello", hello>, signal<"worker.admission", admission>, signal<"heartbeat", heartbeat>,
                               signal<"worker.slots", slot_report>, signal<"progress", progress>, signal<"cancel", cancel>,
                               signal<"analysis.facts_batch", facts_batch>, signal<"analysis.unit_committed", unit_committed>,
                               signal<"analysis.completed", analysis_completed>, signal<"analysis.failed", analysis_failed>>;

    using requests = meta::list<rpc<"node.call", node_call, nodes::node_reply>, rpc<"node.capture", node_capture, nodes::node_reply>,
                                rpc<"node.usage", node_usage, node_usage_reply>, rpc<"hash_file", hash_file, digest>, rpc<"sync.offer", sync_offer, sync_plan>,
                                rpc<"execute.holds", tree_holds, authority_ack>, rpc<"sync.parts", source_bytes, authority_ack>, rpc<"build", build_request, build_report>,
                                rpc<"analysis", analysis_request, build_report>, rpc<"analysis.reuse", reuse_request, reuse_reply>,
                                rpc<"analysis.shard", shard_read, shard_piece>, rpc<"model.read", model_read, source_bytes>,
                                rpc<"export.read", export_read, source_bytes>, rpc<"dependency.fill", dependency_fill, build_report>>;

    inline constexpr std::uint32_t result = kind<"result">;
    inline constexpr std::uint32_t failure_code = kind<"failure">;

    namespace detail {
        template <class... S, class... R> consteval auto codes(meta::list<S...>, meta::list<R...>) {
            return std::array{S::code..., R::code..., result, failure_code};
        }
        template <class... S, class... R> consteval auto listed_once(meta::list<S...>, meta::list<R...>) -> bool {
            return meta::unique<typename S::message..., typename R::request...>;
        }

        template <class T> struct for_request {
            template <class E> struct match : std::bool_constant<std::same_as<typename E::request, T>> {};
        };
        template <class T> struct for_signal {
            template <class E> struct match : std::bool_constant<std::same_as<typename E::message, T>> {};
        };
    } // namespace detail

    static_assert(meta::distinct(detail::codes(signals{}, requests{})), "two messages hash to one kind: rename one");

    inline constexpr std::uint32_t protocol_version = [] {
        std::uint32_t h = 2166136261u;
        for (auto c : detail::codes(signals{}, requests{}))
            h = (h ^ c) * 16777619u;
        return h;
    }();
    static_assert(detail::listed_once(signals{}, requests{}), "a message type is listed twice");

    template <class T> using rpc_of = meta::find_t<detail::for_request<T>::template match, requests>;
    template <class T> using signal_of = meta::find_t<detail::for_signal<T>::template match, signals>;

    template <class R>
    concept request = !std::is_void_v<rpc_of<R>>;
    template <class M>
    concept one_way = !std::is_void_v<signal_of<M>>;

    template <request R> using reply_t = typename rpc_of<R>::reply;

    template <class T>
        requires request<T> || one_way<T>
    inline constexpr std::uint32_t code = [] {
        if constexpr (request<T>)
            return rpc_of<T>::code;
        else
            return signal_of<T>::code;
    }();
} // namespace jobs

export namespace transport {
    struct frame {
        std::uint32_t kind = 0;
        std::uint64_t corr = 0;
        std::vector<std::byte> payload;
    };

    inline constexpr std::size_t header_size = 4 + 4 + 8;
    inline constexpr std::size_t max_payload = 64u << 20;

    template <class T> struct codec {
        static auto encode(const T& value) -> std::vector<std::byte> {
            auto text = json::write(value);
            auto bytes = std::as_bytes(std::span{text});
            return {bytes.begin(), bytes.end()};
        }
        static auto decode(std::span<const std::byte> payload) -> T {
            if (payload.empty())
                throw std::invalid_argument("protocol: empty frame payload");
            if constexpr (std::same_as<T, jobs::dependency_fill>) {
                if (payload.size() > constants::dependency_request_bytes)
                    throw std::invalid_argument("dependency fill request exceeds size limit");
                return json::read<T, json::strict>(std::string_view{reinterpret_cast<const char*>(payload.data()), payload.size()}).value();
            }
            return json::read<T>(std::string_view{reinterpret_cast<const char*>(payload.data()), payload.size()}).value();
        }
    };

    template <> struct codec<jobs::source_bytes> {
        static auto encode(const jobs::source_bytes& push) -> std::vector<std::byte> {
            std::vector<std::byte> out;
            auto put = [&](const void* p, std::size_t n) {
                if (!n)
                    return; // avoid forming a range from a possibly-null pointer
                auto* b = static_cast<const std::byte*>(p);
                out.insert(out.end(), b, b + n);
            };
            auto count = static_cast<std::uint32_t>(push.parts.size());
            put(&count, 4);
            for (auto& b : push.parts) {
                if (!content::is_hash(b.digest))
                    throw std::invalid_argument("source_bytes: bad digest");
                std::uint64_t len = b.bytes.size();
                std::uint8_t packed = b.packed;
                put(b.digest.data(), 64);
                put(&b.offset, 8);
                put(&b.total, 8);
                put(&packed, 1);
                put(&len, 8);
                put(b.bytes.data(), b.bytes.size());
            }
            return out;
        }
        static auto decode(std::span<const std::byte> in) -> jobs::source_bytes {
            auto take = [&](void* p, std::size_t n) {
                if (in.size() < n)
                    throw std::length_error("source_bytes: truncated");
                std::memcpy(p, in.data(), n);
                in = in.subspan(n);
            };
            std::uint32_t count;
            take(&count, 4);
            jobs::source_bytes push;
            for (std::uint32_t i = 0; i < count; ++i) {
                jobs::source_part b;
                b.digest.resize(64);
                take(b.digest.data(), 64);
                take(&b.offset, 8);
                take(&b.total, 8);
                std::uint8_t packed;
                take(&packed, 1);
                b.packed = packed != 0;
                std::uint64_t len;
                take(&len, 8);
                if (len > in.size())
                    throw std::length_error("source_bytes: truncated");
                b.bytes.assign(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(len));
                in = in.subspan(len);
                push.parts.push_back(std::move(b));
            }
            return push;
        }
    };

    template <> struct codec<jobs::facts_batch> {
        static auto encode(const jobs::facts_batch& batch) -> std::vector<std::byte> {
            if (batch.data.size() > constants::semantic_batch_bytes)
                throw std::length_error("semantic batch exceeds its frame budget");
            std::vector<std::byte> out(8 + batch.data.size());
            std::memcpy(out.data(), &batch.offset, 8);
            std::ranges::copy(batch.data, out.begin() + 8);
            return out;
        }
        static auto decode(std::span<const std::byte> in) -> jobs::facts_batch {
            if (in.size() < 8 || in.size() - 8 > constants::semantic_batch_bytes)
                throw std::length_error("invalid semantic batch frame");
            jobs::facts_batch batch;
            std::memcpy(&batch.offset, in.data(), 8);
            batch.data.assign(in.begin() + 8, in.end());
            return batch;
        }
    };

    template <> struct codec<jobs::shard_piece> {
        static auto encode(const jobs::shard_piece& piece) -> std::vector<std::byte> {
            if (piece.data.size() > constants::semantic_batch_bytes)
                throw std::length_error("semantic object slice exceeds its frame budget");
            std::vector<std::byte> bytes(16 + piece.data.size());
            std::memcpy(bytes.data(), &piece.offset, 8);
            std::memcpy(bytes.data() + 8, &piece.total, 8);
            std::ranges::copy(piece.data, bytes.begin() + 16);
            return bytes;
        }
        static auto decode(std::span<const std::byte> bytes) -> jobs::shard_piece {
            if (bytes.size() < 16 || bytes.size() - 16 > constants::semantic_batch_bytes)
                throw std::length_error("invalid semantic object frame");
            jobs::shard_piece piece;
            std::memcpy(&piece.offset, bytes.data(), 8);
            std::memcpy(&piece.total, bytes.data() + 8, 8);
            piece.data.assign(bytes.begin() + 16, bytes.end());
            return piece;
        }
    };

    template <class T> auto encode(const T& value) -> std::vector<std::byte> { return codec<T>::encode(value); }

    template <class T> auto decode(const frame& f) -> T { return codec<T>::decode(f.payload); }

    struct remote_error : std::runtime_error, util::retained_exception {
        remote_error(std::string message, std::string_view code) : std::runtime_error(std::move(message)), code(code) {}
        std::string code;
        auto retain() const noexcept -> std::exception_ptr override;
    };
    auto remote_error::retain() const noexcept -> std::exception_ptr { return util::copy_current_exception(*this); }

    using call_error = std::variant<jobs::failure, std::exception_ptr>;
    template <class T> using call_result = std::expected<T, call_error>;

    [[noreturn]] inline void raise_call_error(call_error error) {
        std::visit(
            []<class E>(E&& e) -> void {
                if constexpr (std::same_as<std::remove_cvref_t<E>, jobs::failure>)
                    throw remote_error(std::move(e.message), std::move(e.code));
                else
                    std::rethrow_exception(e);
            },
            std::move(error));
        std::unreachable();
    }

    struct coded_error : std::runtime_error, util::retained_exception {
        coded_error(std::string_view code, std::string message) : std::runtime_error(std::move(message)), code(code) {}
        std::string code;
        auto retain() const noexcept -> std::exception_ptr override;
    };
    auto coded_error::retain() const noexcept -> std::exception_ptr { return util::copy_current_exception(*this); }

    template <io::stream S> class channel {
    public:
        explicit channel(S& peer) noexcept : peer_(&peer) {}
        channel(channel&&) = delete;

        template <class T> auto send(std::uint32_t kind, std::uint64_t corr, const T& value) -> coroutine::task<> {
            co_await send_frame(kind, corr, codec<T>::encode(value));
        }

        auto recv(std::size_t limit = max_payload) -> coroutine::task<frame> {
            std::array<std::byte, header_size> head;
            co_await io::async_read(*peer_, head);
            std::uint32_t len;
            frame f;
            std::memcpy(&len, head.data(), 4);
            std::memcpy(&f.kind, head.data() + 4, 4);
            std::memcpy(&f.corr, head.data() + 8, 8);
            if (len > max_payload || len > limit)
                throw std::length_error("frame too large");
            f.payload.resize(len);
            co_await io::async_read(*peer_, f.payload);
            co_return f;
        }

        template <jobs::one_way M> auto post(const M& message, std::uint64_t corr = 0) -> coroutine::task<> { return send(jobs::code<M>, corr, message); }

        template <class Reply> auto reply(std::uint64_t corr, const Reply& value) -> coroutine::task<> { return send(jobs::result, corr, value); }
        auto fail(std::uint64_t corr, std::string message, std::string code = {}) -> coroutine::task<> {
            co_await send(jobs::failure_code, corr, jobs::failure{std::move(message), std::move(code)});
        }

        template <jobs::request Req>
        auto call(const Req& req, jobs::progress_log* progress = nullptr, jobs::analysis_link inputs = {}) -> coroutine::task<call_result<jobs::reply_t<Req>>> {
            using Res = jobs::reply_t<Req>;
            if (ended_)
                co_return std::unexpected(call_error{ended_});
            auto owned = std::make_shared<pending>();
            auto& slot = *owned;
            slot.progress = progress;
            slot.inputs = std::move(inputs);
            if constexpr (requires { jobs::traits<Req>::identity(req); }) {
                slot.identity = jobs::traits<Req>::identity(req);
                slot.slot_id = jobs::traits<Req>::slot(req);
            }
            auto corr = next_corr_++;
            pending_.emplace(corr, owned);
            auto drop = scope_exit{[&] { pending_.erase(corr); }};
            auto sent = co_await coroutine::expect(send(jobs::code<Req>, corr, req));
            if (!sent)
                co_return std::unexpected(call_error{std::move(sent.error())});
            auto received = co_await slot;
            if (slot.cancelled) {
                slot.receiving.request_stop();
                try {
                    jobs::cancel cancelled{.why = "the caller stopped"};
                    cancelled.execution = slot.identity;
                    co_await coroutine::under(coroutine::stop_token{}, post(cancelled, corr));
                } catch (const std::exception&) {
                }
                co_await coroutine::stop;
            }
            if (!received)
                co_return std::unexpected(call_error{std::move(received.error())});
            auto& f = *received;
            // Admission can reject an RPC before a provider evaluation exists.
            if (f.kind == jobs::failure_code)
                co_return std::unexpected(call_error{decode<jobs::failure>(f)});
            if (slot.identity && !slot.finished)
                throw std::invalid_argument("provider request ended without a terminal event");
            auto report = decode<Res>(f);
            if constexpr (requires { jobs::traits<Req>::validate(report, *slot.identity, slot.committed, slot.succeeded); })
                jobs::traits<Req>::validate(report, *slot.identity, slot.committed, slot.succeeded);
            co_return report;
        }

        template <std::invocable<const frame&> Other> auto pump(Other other) -> coroutine::task<> {
            std::exception_ptr why;
            bool stopped = false;
            try {
                for (;;) {
                    auto next = co_await coroutine::stop_point(recv());
                    if (!next) {
                        stopped = true;
                        break;
                    }
                    auto& f = *next;
                    if (f.kind == jobs::code<jobs::reuse_request>) {
                        co_await answer<jobs::reuse_request>(f);
                        continue;
                    }
                    if (f.kind == jobs::code<jobs::model_read>) {
                        co_await answer<jobs::model_read>(f);
                        continue;
                    }
                    if (f.kind == jobs::code<jobs::shard_read>) {
                        co_await answer<jobs::shard_read>(f);
                        continue;
                    }
                    const bool response = f.kind == jobs::result || f.kind == jobs::failure_code || f.kind == jobs::code<jobs::progress> ||
                                          f.kind == jobs::code<jobs::facts_batch> || f.kind == jobs::code<jobs::unit_committed> ||
                                          f.kind == jobs::code<jobs::analysis_completed> || f.kind == jobs::code<jobs::analysis_failed>;
                    if (!response) {
                        if constexpr (std::same_as<std::invoke_result_t<Other&, const frame&>, coroutine::task<>>)
                            co_await other(f);
                        else
                            other(f);
                        continue;
                    }
                    auto it = pending_.find(f.corr);
                    if (it == pending_.end())
                        continue;
                    auto slot = it->second;
                    if (f.kind == jobs::code<jobs::facts_batch>) {
                        auto batch = decode<jobs::facts_batch>(f);
                        if (!slot->identity || slot->finished || batch.offset != slot->unit_bytes ||
                            batch.data.size() > constants::semantic_object_bytes - slot->unit_bytes)
                            throw std::invalid_argument("invalid semantic unit batch");
                        slot->unit_bytes += batch.data.size();
                        if (slot->inputs && !slot->cancelled)
                            slot->unit.append(reinterpret_cast<const char*>(batch.data.data()), batch.data.size());
                    } else if (f.kind == jobs::code<jobs::unit_committed>) {
                        auto committed = decode<jobs::unit_committed>(f);
                        if (slot->finished || committed.execution != slot->identity || committed.slot_id != slot->slot_id || !committed.bytes ||
                            committed.bytes != slot->unit_bytes)
                            throw std::invalid_argument("invalid semantic unit commit");
                        if (slot->inputs && !slot->cancelled) {
                            auto unit = json::read<language::evaluated_unit>(slot->unit);
                            if (!unit || unit->request.execution != committed.execution || unit->request.slot_id != committed.slot_id ||
                                unit->request.unit != committed.unit || unit->request.epoch != committed.epoch ||
                                jobs::inventory(*unit) != committed.contributions)
                                throw std::invalid_argument("semantic contribution differs from its commit");
                            co_await slot->deliver(std::move(*unit));
                        }
                        slot->unit.clear();
                        slot->unit_bytes = 0;
                        ++slot->committed;
                    } else if (f.kind == jobs::code<jobs::analysis_completed>) {
                        auto completed = decode<jobs::analysis_completed>(f);
                        if (slot->unit_bytes)
                            throw std::invalid_argument("provider completed with an uncommitted unit");
                        slot->finish(completed.execution, completed.committed_units, true);
                    } else if (f.kind == jobs::code<jobs::analysis_failed>) {
                        auto failed = decode<jobs::analysis_failed>(f);
                        slot->finish(failed.execution, failed.committed_units, false);
                        slot->unit.clear();
                        slot->unit_bytes = 0;
                    } else if (f.kind == jobs::code<jobs::progress>) {
                        if (auto* log = it->second->progress)
                            log->push(decode<jobs::progress>(f));
                    } else {
                        it->second->fulfil(std::move(f));
                    }
                }
            } catch (...) {
                why = util::capture_current_exception();
            }
            if (!why && !stopped)
                why = util::make_exception_ptr(io::closed{});
            ended_ = why ? why : util::make_exception_ptr(io::closed{});
            std::vector<std::shared_ptr<pending>> waiting;
            for (auto& [corr, slot] : pending_)
                waiting.push_back(slot);
            for (auto& slot : waiting)
                stopped ? slot->stop() : slot->abort(why);
            if (stopped)
                co_await coroutine::stop;
        }

    private:
        template <jobs::request Req> auto answer(const frame& frame) -> coroutine::task<> {
            auto request = decode<Req>(frame);
            auto parent = [&] {
                if constexpr (std::same_as<Req, jobs::model_read>)
                    return pending_.find(request.parent);
                else
                    return std::ranges::find_if(pending_, [&](const auto& entry) { return entry.second->identity == request.execution; });
            }();
            if (parent == pending_.end() || parent->second->finished || parent->second->cancelled || !parent->second->inputs) {
                co_await fail(frame.corr, "execution has no active input receiver");
                co_return;
            }
            auto slot = parent->second;
            std::optional<jobs::reply_t<Req>> result;
            std::string failed;
            try {
                result = co_await slot->inputs->ask(std::move(request));
            } catch (const std::exception& error) {
                failed = error.what();
            }
            if (result)
                co_await reply(frame.corr, *result);
            else
                co_await fail(frame.corr, failed);
        }

        auto send_frame(std::uint32_t kind, std::uint64_t corr, std::vector<std::byte> body) -> coroutine::task<> {
            if (body.size() > max_payload)
                throw std::length_error("frame too large");
            std::vector<std::byte> out(header_size + body.size());
            auto len = static_cast<std::uint32_t>(body.size());
            std::memcpy(out.data(), &len, 4);
            std::memcpy(out.data() + 4, &kind, 4);
            std::memcpy(out.data() + 8, &corr, 8);
            std::memcpy(out.data() + header_size, body.data(), body.size());
            auto turn = co_await writing_.lock();
            co_await io::async_write(*peer_, out);
        }

        struct pending {
            jobs::progress_log* progress = nullptr;
            jobs::analysis_link inputs;
            coroutine::stop_source receiving;
            std::string unit, slot_id;
            std::uint64_t unit_bytes = 0, committed = 0;
            std::optional<language::evaluation_identity> identity;
            bool finished = false, succeeded = false;
            void finish(const language::evaluation_identity& expected, std::uint64_t count, bool success) {
                if (finished || identity != expected || count != committed)
                    throw std::invalid_argument("invalid provider terminal event");
                finished = true;
                succeeded = success;
            }
            std::optional<frame> value;
            std::exception_ptr error;
            bool stopped = false;
            bool cancelled = false;
            std::coroutine_handle<> waiter;
            std::coroutine_handle<> (*stop_chain)(std::coroutine_handle<>) noexcept = nullptr;

            auto deliver(language::evaluated_unit unit) -> coroutine::task<> {
                struct forward {
                    coroutine::stop_source* source;
                    void operator()() const noexcept { source->request_stop(); }
                };
                auto parent = co_await coroutine::current_token;
                std::optional<execution::stop_callback_for_t<coroutine::stop_token, forward>> link;
                if (parent.stop_possible())
                    link.emplace(parent, forward{&receiving});
                co_await coroutine::under(receiving.get_token(), inputs->ask(std::move(unit)));
            }

            struct on_stop {
                pending* self;
                void operator()() const noexcept {
                    self->cancelled = true;
                    self->wake();
                }
            };
            std::optional<execution::stop_callback_for_t<coroutine::stop_token, on_stop>> watch;

            bool await_ready() const noexcept { return value || error; }
            template <class P> auto await_suspend(std::coroutine_handle<P> h) noexcept -> std::coroutine_handle<> {
                stop_chain = [](std::coroutine_handle<> frame) noexcept {
                    return std::coroutine_handle<P>::from_address(frame.address()).promise().unhandled_stopped();
                };
                auto token = execution::get_stop_token(execution::get_env(h.promise()));
                if (stopped)
                    return h.promise().unhandled_stopped();
                if (token.stop_requested()) {
                    cancelled = true;
                    return h;
                }
                waiter = h;
                if (token.stop_possible())
                    watch.emplace(token, on_stop{this});
                return std::noop_coroutine();
            }
            auto await_resume() -> std::expected<frame, std::exception_ptr> {
                watch.reset();
                if (cancelled)
                    return frame{};
                if (error)
                    return std::unexpected(error);
                return std::move(*value);
            }
            void fulfil(frame f) {
                value = std::move(f);
                wake();
            }
            void abort(std::exception_ptr e) {
                error = e;
                wake();
            }
            void stop() {
                stopped = true;
                if (auto frame = std::exchange(waiter, {})) {
                    auto next = stop_chain(frame);
                    next.resume();
                }
            }
            void wake() {
                if (auto w = std::exchange(waiter, {}))
                    w.resume();
            }
        };

        template <class F> struct scope_exit {
            F fn;
            ~scope_exit() { fn(); }
        };

        S* peer_;
        coroutine::mutex writing_;
        std::uint64_t next_corr_ = 1;
        std::exception_ptr ended_;
        std::unordered_map<std::uint64_t, std::shared_ptr<pending>> pending_;
    };

    // Runs a staged analysis on one worker and replies with one report over both stages. The analysis starts
    // only when the preparation succeeded, and takes the time the preparation left. Another stage may prepare the
    // same tree between the two; the worker then refuses the analysis and the preparation is asked again.
    template <class Channel>
    auto staged(Channel& channel, jobs::staged_analysis work, jobs::progress_log* progress = nullptr, jobs::analysis_link inputs = {})
        -> coroutine::task<call_result<jobs::build_report>> {
        const auto started = std::chrono::steady_clock::now();
        work.preparation.prepares = work.analysis.execution.stage_id;
        for (unsigned attempt = 0;; ++attempt) {
            auto prepared = co_await channel.call(work.preparation, progress, inputs);
            if (!prepared || !prepared->inputs) {
                if (prepared)
                    prepared->analysis_execution = work.analysis.execution;
                co_return prepared;
            }
            auto analyzed = co_await channel.call(jobs::analysis_of(work, prepared->inputs->id(), started), progress, inputs);
            if (!analyzed) {
                const auto* refused = std::get_if<jobs::failure>(&analyzed.error());
                if (refused && refused->code == jobs::why::stale_inputs && attempt < 2)
                    continue;
                co_return analyzed;
            }
            jobs::merge(*analyzed, std::move(*prepared));
            co_return analyzed;
        }
    }

    template <io::stream S> class dispatcher {
    public:
        jobs::receipt_signer signer;

        struct exchange {
            channel<S>& chan;
            std::uint64_t corr;
            std::uint64_t committed = 0;
            std::optional<language::evaluation_identity> execution;
            const jobs::receipt_signer& signer;

            auto progress(jobs::progress p) -> coroutine::task<> {
                p.execution = execution;
                co_await chan.post(p, corr);
            }
            auto commit(language::evaluated_unit unit) -> coroutine::task<> {
                if (execution && unit.request.execution != *execution)
                    throw std::logic_error("provider unit belongs to another execution");
                unit.receipts.clear();
                if (signer.key)
                    for (auto& shard : unit.shards)
                        unit.receipts.push_back(signer.sign(jobs::shard_claim(unit, shard, signer.workspace, signer.worker)));
                jobs::unit_committed done{unit.request.execution, unit.request.slot_id, unit.request.unit, unit.request.epoch, 0, jobs::inventory(unit)};
                auto data = json::write(unit);
                done.bytes = data.size();
                if (data.size() > constants::semantic_object_bytes)
                    throw std::length_error("semantic unit exceeds its storage budget");
                for (std::size_t offset = 0; offset < data.size();) {
                    auto length = std::min(constants::semantic_batch_bytes, data.size() - offset);
                    auto bytes = std::as_bytes(std::span{data.data() + offset, length});
                    co_await chan.post(jobs::facts_batch{offset, {bytes.begin(), bytes.end()}}, corr);
                    offset += length;
                }
                co_await chan.post(done, corr);
                co_await progress({.done = ++committed, .stage = "analysis"});
            }
        };

        template <jobs::request Req, class F>
            requires std::same_as<std::invoke_result_t<F&, Req, exchange&>, coroutine::task<jobs::reply_t<Req>>>
        void on(F handler) {
            auto dispatch = [handler = std::move(handler)](exchange& ex, frame f) mutable -> coroutine::task<> {
                return serve<Req>(handler, ex, std::move(f));
            };
            handlers_[jobs::code<Req>] = std::move(dispatch);
        }

        auto handles(std::uint32_t kind) const -> bool { return handlers_.contains(kind); }

        auto handle(channel<S>& chan, frame f) -> coroutine::task<> {
            if (f.kind == jobs::code<jobs::cancel>) {
                auto cancelled = decode<jobs::cancel>(f);
                if (auto r = running_.find({&chan, f.corr}); r != running_.end() && cancelled.execution == r->second.active->execution)
                    r->second.source->request_stop();
                co_return;
            }
            auto it = handlers_.find(f.kind);
            if (it == handlers_.end()) {
                co_await chan.fail(f.corr, unknown_kind(f.kind));
                co_return;
            }
            struct forward {
                coroutine::stop_source* source;
                void operator()() const noexcept { source->request_stop(); }
            };
            auto parent = co_await coroutine::current_token;
            coroutine::stop_source source;
            std::optional<execution::stop_callback_for_t<coroutine::stop_token, forward>> link;
            if (parent.stop_possible())
                link.emplace(parent, forward{&source});
            std::pair key{&chan, f.corr};
            exchange ex{chan, f.corr, 0, {}, signer};
            running_[key] = {&source, &ex};
            struct forget {
                dispatcher* self;
                std::pair<channel<S>*, std::uint64_t> key;
                ~forget() { self->running_.erase(key); }
            } done{this, key};
            co_await coroutine::under(source.get_token(), it->second(ex, std::move(f)));
        }

    private:
        template <class Req, class F> static auto serve(F& handler, exchange& ex, frame f) -> coroutine::task<> {
            std::optional<jobs::reply_t<Req>> out;
            std::string error, code;
            try {
                auto request = decode<Req>(f);
                if constexpr (requires { jobs::traits<Req>::identity(request); })
                    ex.execution = jobs::traits<Req>::identity(request);
                if constexpr (jobs::execution_request<Req>) {
                    const auto recipe = ex.signer.key ? jobs::artifact_recipe(request) : std::string{};
                    const auto snapshot = request.snapshot.str();
                    auto report = co_await handler(std::move(request), ex);
                    report.receipts.clear();
                    report.artifact_audit.clear();
                    if (ex.signer.key)
                        if (auto claim = jobs::artifact_claim(report, snapshot, recipe, ex.signer.workspace, ex.signer.worker))
                            report.receipts.push_back(ex.signer.sign(std::move(claim->receipt)));
                    out.emplace(std::move(report));
                } else {
                    out.emplace(co_await handler(std::move(request), ex));
                }
            } catch (const coded_error& e) {
                error = e.what();
                code = e.code;
            } catch (const std::exception& e) {
                error = e.what();
            }
            if constexpr (requires { jobs::traits<Req>::terminal(out, *ex.execution, ex.committed, error, code); })
                if (ex.execution) {
                    auto event = jobs::traits<Req>::terminal(out, *ex.execution, ex.committed, error, code);
                    co_await std::visit([&](const auto& value) { return ex.chan.post(value, ex.corr); }, event);
                }
            if (out)
                co_await ex.chan.reply(ex.corr, *out);
            else
                co_await ex.chan.fail(ex.corr, std::move(error), std::move(code));
        }

        static auto unknown_kind(std::uint32_t kind) -> std::string { return std::format("unknown request kind {:#010x}", kind); }

        std::unordered_map<std::uint32_t, util::move_only_function<coroutine::task<>(exchange&, frame)>> handlers_;
        struct running_request {
            coroutine::stop_source* source;
            exchange* active;
        };
        std::map<std::pair<channel<S>*, std::uint64_t>, running_request> running_;
    };
} // namespace transport
