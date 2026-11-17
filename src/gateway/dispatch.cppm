export module tools:execution;
import utility;
import constants;
import std;
import knowledge;
import reflection;
import coroutine;
import meta;
import protocol;
import mcp;
import configuration;
import usage;
import world;
import codec;
import archive;
import registry;
import fleet;
import commands;
export import templates;
export import publication;
import native;

export import :worldlines;

export namespace tools {
    struct mutation {
        std::string version, receipt, operation_id;
        bool replayed = false;
    };

    struct step_result {
        std::string name;
        std::int32_t exit_code = -1;
        bool timed_out = false;
        double seconds = 0;
        std::string output;
        bool truncated = false;
        std::optional<builds::exported_file> log;
        bool log_truncated = false;
        std::string log_error;
        std::string cache_error;
        std::vector<std::string> tasks;
    };

    using diagnostic = builds::finding;
    using test_summary = builds::tally;

    struct config_result {
        std::string configuration;
        std::string worker;
        std::optional<std::string> worker_id;
        bool ok = false;
        double seconds = 0;
        std::vector<step_result> steps;
        std::vector<diagnostic> diagnostics;
        std::optional<test_summary> tests;
        std::uint64_t files = 0;
        std::uint64_t blobs_sent = 0;
        std::uint64_t bytes_sent = 0;
        std::string snapshot;
        std::string error;
        std::string error_code;
        std::vector<builds::exported_file> exports, models;
        std::string export_error;
        std::vector<std::string> export_skipped;
        std::string dependency_cache;
        builds::timings timings;
    };

    // Build, test and run jobs of the workspace, kept on core zero. As many run at once as the limits and the workers allow; waiting
    // jobs take turns by who asked. A job equal to one queued or running is not run again: its asker waits for the same result, and a
    // build equal to a finished one answers from the cache. A new request replaces the same asker's older one that has not started.
    // Each project and configuration has a few build trees on the workers; a job takes the free one whose last build shared the most
    // files with it, so a forked worldline builds on from where its origin left off.
    class backlog {
    public:
        struct entry {
            // Equal keys compute equal results: the files built and everything the worker is told.
            std::string key;
            std::string project, configuration, operation;
            // Who asked, as worlds::identity_key names them.
            std::string actor;
            // Directory pools reserved together by the job. `files` names the files built, as path and digest.
            std::vector<std::string> trees;
            std::vector<std::string> files;
            bool cacheable = false;
            // Run again even when a finished result is cached.
            bool fresh = false;
            std::string workspace_id, worldline, version;
            std::shared_ptr<execution_identity> executor;
            // Equal analysis families have the same effective inputs, execution
            // budget and result projection. Demands may differ within that family.
            std::string family;
            std::map<std::string, semantic::extent> demands;
        };

        struct view {
            std::uint64_t id = 0;
            std::string project, configuration, operation;
            // queued, running, done, cancelled or replaced.
            std::string state;
            std::optional<bool> ok;
            // Callers waiting for it, and the queued jobs before it.
            std::size_t waiting = 0, ahead = 0;
        };

        // A job's result: a build, test or run configuration's, or a preparation's.
        using outcome = std::variant<config_result, language::result>;

        // A caller's hold on a job: core zero writes it, the caller's core reads it.
        class ticket {
        public:
            explicit ticket(std::string actor) : actor_(std::move(actor)) {}
            auto done() const -> bool { return done_.load(std::memory_order_acquire); }
            // Valid once done.
            auto result() const -> const outcome& { return result_; }
            auto ahead() const -> std::size_t { return ahead_.load(std::memory_order_relaxed); }
            auto job() const -> std::uint64_t { return job_.load(std::memory_order_relaxed); }
            auto reuse() const -> const std::string& { return reuse_; }
            // The progress its job reported since `seen`, which moves past it.
            auto progress(std::size_t& seen) -> std::vector<jobs::progress> { return progress_ ? progress_->since(seen) : std::vector<jobs::progress>{}; }
            void leave() { left_.store(true, std::memory_order_release); }

        private:
            friend class backlog;
            std::string actor_;
            std::string reuse_ = "executed";
            std::chrono::steady_clock::time_point submitted_ = std::chrono::steady_clock::now();
            double queued_ = 0;
            std::atomic<bool> done_{false}, left_{false};
            std::atomic<std::size_t> ahead_{0};
            std::atomic<std::uint64_t> job_{0};
            outcome result_;
            // The job's, set before the ticket is handed out; none for a cached result.
            std::shared_ptr<jobs::progress_log> progress_;
        };

        // Runs a job on core zero in its reserved directories, reporting into the job's log; nothing when every eligible worker is busy, so the job
        // waits for one.
        using placements = std::map<std::string, std::string>;
        using runner =
            util::move_only_function<coroutine::task<std::optional<outcome>>(services&, jobs::progress_log&, placements trees, std::string computation)>;

        backlog(cores::context<native::scheduler>& rt, services& zero) : rt_(rt), zero_(zero) {}

        auto submit(std::size_t here, entry e, runner r) -> coroutine::task<std::shared_ptr<ticket>>;
        auto list(std::size_t here) -> coroutine::task<std::vector<view>>;
        auto forget(std::size_t here, std::string key) -> coroutine::task<>;
        // Cancels a job the caller asked for alone, or any job for an owner. Says what happened.
        auto cancel(std::size_t here, std::uint64_t id, worlds::actor who) -> coroutine::task<std::string>;
        // Core zero's loop: starts what may start and lets go of jobs nobody waits for any more.
        auto serve(native::scheduler& io) -> coroutine::task<>;
        auto idle() const -> bool { return jobs_.empty() && running_ == 0 && analyzing_ == 0; }

    private:
        // A reserved directory in a project/layout/runtime pool. Its installed
        // files guide placement across worldlines; the worker owns checkout state.
        struct tree {
            std::string name;
            bool busy = false;
            std::unordered_set<std::string> files;
            std::uint64_t used = 0;
        };

        struct job {
            std::uint64_t id = 0;
            std::string computation;
            entry what;
            runner run;
            bool running = false;
            placements placed;
            std::vector<tree*> directories;
            // After every eligible worker was busy, the job is not tried again before this.
            std::chrono::steady_clock::time_point retry_at{}, queued_at = std::chrono::steady_clock::now();
            std::vector<std::shared_ptr<ticket>> tickets;
            // Holds on earlier computations whose committed units this job can reuse.
            std::vector<std::shared_ptr<ticket>> predecessors;
            std::shared_ptr<jobs::progress_log> progress = std::make_shared<jobs::progress_log>();
            coroutine::stop_source stop;
        };

        auto enqueue(entry e, std::shared_ptr<runner> r) -> std::shared_ptr<ticket>;
        auto choose(const entry& e, const std::string& group) -> tree&;
        auto views() const -> std::vector<view>;
        auto withdraw(std::uint64_t id, const worlds::actor& who) -> std::string;
        void step();
        auto next(bool builds, bool analyses) -> std::shared_ptr<job>;
        auto launch(std::shared_ptr<job> j) -> coroutine::task<>;
        void finish(const std::shared_ptr<job>& j, outcome r, std::string state);
        static auto refused(const entry& e, std::string why) -> outcome;

        cores::context<native::scheduler>& rt_;
        services& zero_;
        std::uint64_t next_id_ = 1;
        std::list<std::shared_ptr<job>> jobs_;
        // Builds and analyses each run up to the limit, so neither waits behind the other.
        std::size_t running_ = 0, analyzing_ = 0;
        // Who asked, in the order their turns come.
        std::deque<std::string> turns_;
        std::list<std::pair<std::string, outcome>> cache_;
        std::map<std::string, std::deque<tree>> trees_;
        std::uint64_t clock_ = 0;
        std::deque<view> finished_;
    };

    struct results {
        bool ok = false;
        std::string snapshot;
        std::vector<config_result> results;
        std::string worldline;
        std::string revision;

        auto failed() const -> bool { return !ok; }
    };

    namespace detail {
        // What the steps' reports say, read on the worker: their diagnostics, at most `max` and each once, and the tests of every
        // step that ran some.
        auto read(jobs::build_report& r, std::size_t max = 100) -> std::pair<std::vector<diagnostic>, std::optional<test_summary>> {
            std::vector<diagnostic> out;
            std::optional<test_summary> tests;
            for (auto& step : r.steps) {
                if (step.tests) {
                    if (!tests)
                        tests.emplace();
                    tests->total += step.tests->total;
                    tests->failed += step.tests->failed;
                    std::ranges::move(step.tests->failed_tests, std::back_inserter(tests->failed_tests));
                }
                for (auto& d : step.diagnostics) {
                    if (out.size() >= max)
                        break;
                    if (!std::ranges::any_of(out,
                                             [&](auto& o) { return o.file == d.file && o.line == d.line && o.column == d.column && o.message == d.message; }))
                        out.push_back(std::move(d));
                }
            }
            return {std::move(out), std::move(tests)};
        }

        auto operation(config::effective_config c, builds::request request) -> std::pair<config::effective_config, std::vector<jobs::step>> {
            if (!c.build.enabled)
                throw std::invalid_argument(std::format("build configuration '{}' is disabled", c.name));
            if (!c.build.operations.contains(request.operation)) {
                std::string names;
                for (const auto& [op, _] : c.build.operations)
                    names += (names.empty() ? "" : ", ") + op;
                if (names.empty())
                    names = "none";
                throw std::invalid_argument("build configuration '" + c.name + "' offers no '" + request.operation + "' operation (it offers: " + names + ")");
            }
            auto plan = builds::prepare(c.build, request);
            c.needs = plan.needs;
            c.env = std::move(plan.env);
            return {std::move(c), std::move(plan.steps)};
        }
    } // namespace detail

    using jobs::part_size;
    inline constexpr std::uint64_t push_batch = 8u << 20;
    static_assert(push_batch + part_size < transport::max_payload / 2);

    struct sync_stats {
        std::uint64_t blobs = 0;
        std::uint64_t bytes = 0;
        double seconds = 0;
    };

    inline constexpr std::chrono::seconds offer_deadline{60};
    inline constexpr std::chrono::seconds push_deadline{120};
    inline constexpr std::chrono::seconds build_overhead{300};

    auto no_answer(std::string_view what, std::chrono::seconds limit) -> std::runtime_error {
        return std::runtime_error("the worker did not answer " + std::string{what} + " within " + std::to_string(limit.count()) + " s");
    }

    template <class T>
    auto within(native::scheduler& io, std::chrono::seconds limit, std::string_view what, coroutine::task<transport::call_result<T>> call)
        -> coroutine::task<T> {
        auto r = co_await coroutine::deadline(io, limit, std::move(call));
        if (!r)
            throw no_answer(what, limit);
        if (!*r)
            transport::raise_call_error(std::move(r->error()));
        co_return std::move(**r);
    }

    struct execution_identity {
        std::mutex mutex;
        std::pair<std::string, std::optional<std::string>> value;
        bool began = false;
        builds::timings timings;
        std::optional<std::chrono::steady_clock::time_point> syncing;
        void sync_begin() {
            std::lock_guard held{mutex};
            syncing = std::chrono::steady_clock::now();
        }
        void sync_end() {
            std::lock_guard held{mutex};
            timings.sync_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - *syncing).count();
            syncing.reset();
        }
        void begin() {
            std::lock_guard held{mutex};
            began = true;
        }
        auto started() -> bool {
            std::lock_guard held{mutex};
            return began;
        }
        void select(std::pair<std::string, std::optional<std::string>> identity) {
            std::lock_guard held{mutex};
            value = std::move(identity);
        }
        template <class Result> void stamp(Result& result) {
            std::lock_guard held{mutex};
            result.worker = value.first;
            result.worker_id = value.second;
            if constexpr (requires { result.timings; }) {
                result.timings = timings;
                if (syncing)
                    result.timings.sync_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - *syncing).count();
            }
        }
    };

    auto backlog::refused(const entry& e, std::string why) -> outcome {
        auto code = why == "cancelled" ? "cancelled" : why.starts_with("replaced") ? "replaced" : "execution_failed";
        config_result result{.configuration = e.configuration, .snapshot = e.version, .error = std::move(why), .error_code = code};
        if (e.executor)
            e.executor->stamp(result);
        return result;
    }

    template <class Request>
    auto sync_and_run(native::scheduler& io, storage::registry& files, std::size_t core, transport::channel<native::socket>& worker, std::string project,
                      content::manifest tree, Request work, jobs::progress_log* progress, std::string worldline, std::string version, worlds::actor actor,
                      std::shared_ptr<execution_identity> executor, semantic::retention_roots roots = {}, semantic::snapshot_vector snapshots = {},
                      semantic::source_vector sources = {}, routing::fleet* beside = nullptr, routing::selection placed = {})
        -> coroutine::task<std::pair<jobs::build_report, sync_stats>> {
        auto& request = jobs::traits<Request>::preparation(work);
        auto* analysis = jobs::traits<Request>::analysis(work);
        auto report = [&](std::uint64_t done, std::uint64_t total, std::string stage) {
            if (progress)
                progress->push(jobs::progress{done, total, std::move(stage)});
        };
        sync_stats stats;
        auto expected_snapshot = tree.id();
        std::unordered_map<content::hash, std::uint64_t> sizes;
        std::map<std::string, std::string> source_objects;
        for (auto& e : tree.files) {
            sizes.emplace(e.digest, e.size);
            if (analysis)
                source_objects.emplace(e.path, e.digest.str());
        }
        // The worker's cache receives the snapshot's files before anything executes on them.
        auto synchronize = [&]() -> coroutine::task<> {
            const auto sync_started = std::chrono::steady_clock::now();
            executor->sync_begin();
            auto plan = co_await within(io, offer_deadline, "the offer", worker.call(jobs::sync_offer{project, tree}));
            if (plan.snapshot != expected_snapshot)
                throw tool_error("invalid_worker_reply", "Worker returned a snapshot different from the offered source");

            struct piece {
                content::hash digest;
                std::uint64_t offset, total, length;
            };
            std::vector<std::vector<piece>> pushes(1);
            std::uint64_t batch = 0, total = 0;
            for (auto& h : plan.missing) {
                auto size = sizes.at(h);
                total += size;
                std::uint64_t at = 0;
                do {
                    auto len = std::min(part_size, size - at);
                    if (batch + len > push_batch && !pushes.back().empty()) {
                        pushes.emplace_back();
                        batch = 0;
                    }
                    pushes.back().push_back({h, at, size, len});
                    batch += len;
                    at += len;
                } while (at < size);
            }
            for (auto& push : pushes) {
                if (push.empty())
                    continue;
                nodes::node_request wanted{.action = "content", .worldline = worldline, .revision = version};
                for (auto& p : push)
                    wanted.parts.push_back({p.digest.str(), p.offset, p.length, {}});
                auto reply = co_await files.node(core, project, std::move(wanted), actor, true);
                if (reply.code != "ok")
                    throw tool_error(reply.code, reply.reason);
                // A piece is sent compressed when that makes it smaller.
                std::vector<jobs::source_part> parts;
                for (auto& p : reply.parts) {
                    auto bytes = lines::unhex(p.data);
                    stats.bytes += bytes.size();
                    auto packed = nodes::archive::pack(bytes);
                    auto& sent = packed.size() < bytes.size() ? packed : bytes;
                    parts.push_back(
                        {p.id, p.offset, p.total,
                         std::vector<std::byte>{reinterpret_cast<const std::byte*>(sent.data()), reinterpret_cast<const std::byte*>(sent.data()) + sent.size()},
                         &sent == &packed});
                }
                co_await within(io, push_deadline, "a push", worker.call(jobs::source_bytes{std::move(parts)}));
                report(stats.bytes, total, "sync");
            }
            executor->sync_end();
            stats.blobs = plan.missing.size();
            stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - sync_started).count();
            report(0, 0, request.steps.empty() ? "run" : request.steps.front().name);
            executor->begin();
        };

        request.snapshot = expected_snapshot;
        auto limit = std::chrono::seconds{request.timeout_s} + build_overhead;
        publishing::bound publication;
        if (analysis) {
            auto own = std::ranges::find(snapshots, project, &semantic::snapshot::project);
            if (own == snapshots.end())
                snapshots.push_back({project, worldline, version, expected_snapshot.str()});
            else
                *own = {project, worldline, version, expected_snapshot.str()};
            sources[project] = std::move(source_objects);
            publication = co_await publishing::bind(files, core, project, std::move(snapshots), actor, *analysis, std::move(sources), roots);
        }
        auto units = publication.units;
        publication.files = &files;
        publication.core = core;
        publication.project = project;
        auto inputs = std::make_shared<jobs::analysis_exchange>(io, io);
        auto execute = [&](coroutine::scope& children) -> coroutine::task<jobs::build_report> {
            auto close = jobs::analysis_exchange::lifetime{inputs};
            children.spawn(publication.serve(inputs));
            if constexpr (std::same_as<Request, jobs::staged_analysis>) {
                const auto started = std::chrono::steady_clock::now();
                request.prepares = analysis->execution.stage_id;
                // The same preparation of the same snapshot, whatever time it is given.
                const auto recipe =
                    content::hash_of(rfl::json::write(std::tuple{request.project, request.tree, request.snapshot, request.steps, request.preparation_tasks,
                                                                 request.env, request.measure, request.generation_producers}))
                        .str();
                // Inputs a worker already reported for this preparation need no second asking; a tree that has
                // moved on since refuses the analysis, and the preparation runs after all.
                auto known = beside ? beside->inputs(worker, recipe) : std::optional<content::hash>{};
                std::optional<jobs::build_report> prepared;
                for (unsigned attempt = 0;; ++attempt) {
                    if (!known) {
                        auto prepare = [&](transport::channel<native::socket>& executing) -> coroutine::task<jobs::build_report> {
                            co_await synchronize();
                            co_return co_await within(io, limit, "the preparation", executing.call(request, progress, inputs));
                        };
                        // Beside an analysis, the preparation is execute work with its own place on the worker.
                        prepared = beside && placed.analysis ? co_await beside->executing(worker, placed, prepare) : co_await prepare(worker);
                        if (!prepared->inputs) {
                            prepared->analysis_execution = analysis->execution;
                            co_return std::move(*prepared);
                        }
                        known = prepared->inputs->id();
                        if (beside)
                            beside->remember(worker, recipe, known);
                    } else
                        executor->begin();
                    try {
                        auto analyzed = co_await within(io, limit, "the analysis", worker.call(jobs::analysis_of(work, *known, started), progress, inputs));
                        if (prepared)
                            jobs::merge(analyzed, std::move(*prepared));
                        co_return analyzed;
                    } catch (const transport::remote_error& refused) {
                        if (refused.code != jobs::why::stale_inputs || attempt == 2)
                            throw;
                        known.reset();
                        if (beside)
                            beside->remember(worker, recipe, std::nullopt);
                    }
                }
            } else {
                co_await synchronize();
                co_return co_await within(io, limit, "the build", worker.call(work, progress, inputs));
            }
        };
        auto r = co_await coroutine::with_scope(std::move(execute));
        if (r.ok() && !r.analysis_error &&
            (r.committed_units != units->keys.size() ||
             (r.analysis_session && units->session && (r.analysis_session->id != units->session->id || r.analysis_session->epoch != units->session->epoch))))
            throw tool_error("invalid_worker_reply", "provider completion differs from its committed units");
        r.reused_units = units->reused;
        units->missing = r.missing;
        std::uint64_t exported = 0, logged = 0;
        if (r.exports.size() > constants::export_files)
            throw tool_error("invalid_worker_reply", "too many exported files");
        std::vector<builds::exported_file*> transfer;
        for (auto& file : r.exports)
            transfer.push_back(&file);
        const auto consumed = analysis ? jobs::analysis_exports(*analysis) : std::map<std::string, builds::context>{};
        for (const auto& file : r.exports) {
            content::checked_name(file.name);
            content::checked_path(file.path);
            const auto captured = consumed.find(file.name);
            const bool input = captured != consumed.end() &&
                               (captured->second.kind == "directory" ? file.path.starts_with(captured->second.path + "/") : captured->second.path == file.path);
            if ((!request.exports.contains(file.name) && !input) || !content::is_hash(file.digest) || file.size > constants::export_bytes - exported)
                throw tool_error("invalid_worker_reply", "invalid exported file");
            exported += file.size;
        }
        std::map<std::string, builds::context> expected_models;
        for (const auto& step : request.steps)
            for (const auto& output : step.outputs)
                if (output.type == "project_model")
                    expected_models.emplace(output.name, output);
        if (r.models.size() > expected_models.size())
            throw tool_error("invalid_worker_reply", "too many produced models");
        std::set<std::string> produced_models;
        for (auto& file : r.models) {
            const auto found = expected_models.find(file.name);
            if (found == expected_models.end() || !produced_models.insert(file.name).second || !content::is_hash(file.digest) ||
                file.size > constants::export_bytes - exported)
                throw tool_error("invalid_worker_reply", "unexpected project model");
            exported += file.size;
            transfer.push_back(&file);
        }
        std::map<content::hash, builds::exported_file> objects;
        auto add_objects = [&](const auto& file) {
            for (const auto& object : file.objects) {
                if (!content::is_hash(object.digest))
                    throw tool_error("invalid_worker_reply", "invalid model object digest");
                if (auto found = objects.find(object.digest); found != objects.end()) {
                    if (found->second.size != object.size)
                        throw tool_error("invalid_worker_reply", "inconsistent model object size");
                    continue;
                }
                if (objects.size() >= constants::export_files || object.size > constants::export_bytes - exported)
                    throw tool_error("invalid_worker_reply", "model objects exceed delivery limits");
                exported += object.size;
                objects.emplace(object.digest, builds::exported_file{.digest = object.digest, .size = object.size});
            }
        };
        for (const auto& file : r.exports)
            add_objects(file);
        for (const auto& file : r.models)
            add_objects(file);
        for (auto& [_, file] : objects)
            transfer.push_back(&file);
        for (std::size_t index = 0; index < r.steps.size(); ++index)
            if (r.steps[index].log) {
                const auto& log = *r.steps[index].log;
                if (index >= request.steps.size() || log.name != "step-" + std::to_string(index) + ".log" || log.path != std::to_string(index) + ".log" ||
                    !content::is_hash(log.digest) || log.size > constants::log_bytes - logged)
                    throw tool_error("invalid_worker_reply", "invalid execution log");
                logged += log.size;
                transfer.push_back(&*r.steps[index].log);
            }
        std::set<std::string> transfer_ids;
        for (const auto* file : transfer)
            transfer_ids.insert(file->digest.str());
        auto transfer_pins = co_await files.pin_exports(core, std::move(transfer_ids));
        for (auto* target : transfer) {
            auto& file = *target;
            try {
                if (co_await files.has_export(core, file.digest, file.size))
                    continue;
                std::uint64_t offset = 0;
                do {
                    const auto length = std::min(part_size, file.size - offset);
                    auto bytes = co_await within(io, push_deadline, "an export", worker.call(jobs::export_read{file.digest, offset, length}));
                    if (bytes.parts.size() != 1)
                        throw tool_error("invalid_worker_reply", "invalid export part count");
                    auto& part = bytes.parts.front();
                    if (part.digest != file.digest || part.offset != offset || part.total != file.size || part.packed || part.bytes.size() != length)
                        throw tool_error("invalid_worker_reply", "invalid export part");
                    co_await files.receive_export(core, std::move(part));
                    offset += length;
                } while (offset < file.size);
            } catch (const std::exception& error) {
                file.delivery_error = std::string{error.what()}.substr(0, 512);
            }
        }
        auto models = r.models;
        for (const auto& file : r.exports)
            if (!file.objects.empty())
                models.push_back(file);
        for (auto& file : models)
            for (const auto& ref : file.objects)
                if (!objects.at(ref.digest).delivery_error.empty())
                    file.delivery_error = objects.at(ref.digest).delivery_error;
        models = co_await files.retain_models(core, std::move(models));
        std::size_t model_index = 0;
        for (auto& file : r.models)
            file = std::move(models.at(model_index++));
        for (auto& file : r.exports)
            if (!file.objects.empty())
                file = std::move(models.at(model_index++));
        bool inputs_delivered = true;
        r.models = co_await files.grant_exports(core, project, std::move(r.models));
        for (const auto& file : r.models)
            if (!file.delivery_error.empty()) {
                inputs_delivered = false;
                r.export_error = "model delivery failed: " + file.delivery_error;
                if (analysis)
                    r.analysis_error = r.export_error;
            }
        std::vector<builds::exported_file> captured;
        for (const auto& file : r.exports)
            if (consumed.contains(file.name))
                captured.push_back(file);
        captured = co_await files.grant_exports(core, project, std::move(captured));
        std::size_t captured_index = 0;
        for (auto& file : r.exports)
            if (consumed.contains(file.name)) {
                file = std::move(captured.at(captured_index++));
                if (!file.delivery_error.empty()) {
                    inputs_delivered = false;
                    r.export_error = "analysis artifact delivery failed: " + file.delivery_error;
                    r.analysis_error = r.export_error;
                }
            }
        // Transfer pins cover capture through publication. Semantic shard roots
        // take over only after the immutable inputs are installed and authorized.
        if (inputs_delivered && publication.files)
            if (auto view = co_await publication.finish())
                r.stage_views.push_back(*view);
        co_return std::pair{std::move(r), stats};
    }

    struct invocation {
        storage::registry& files;
        std::size_t core;
        usage::call_event event;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        ~invocation() {
            event.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            files.post(core, [value = event](usage::ledger& ledger) { ledger.record(value); });
        }
    };

    // A build admitted by the ledger; ended without a record, it is recorded as cancelled.
    struct admission {
        storage::registry* files;
        std::size_t core;
        std::uint64_t id;
        usage::build_event cancelled;
        std::chrono::steady_clock::time_point started;
        std::shared_ptr<execution_identity> executor = std::make_shared<execution_identity>();
        bool ended = false;
        ~admission() {
            if (ended)
                return;
            auto e = cancelled;
            executor->stamp(e);
            e.error = "cancelled";
            e.error_code = "cancelled";
            e.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            files->post(core, [id = id, e](usage::ledger& ledger) { ledger.record(id, e); });
        }
    };

    // One configuration's job, run by the backlog on core zero: admitted by the ledger, sent to a worker, recorded. Nothing when every
    // eligible worker is busy; the admission is then given back and the job waits.
    auto dispatch(services& s, std::string project, config::effective_config config, std::vector<jobs::step> steps, content::manifest snapshot,
                  jobs::build_request request, std::string worldline, std::string version, worlds::actor actor, jobs::progress_log& progress,
                  std::string operation, std::shared_ptr<execution_identity> executor, std::string toolchain)
        -> coroutine::task<std::optional<backlog::outcome>> {
        if (!actor.run.empty())
            co_await selected(s, project, worldline, version, actor);
        config_result r{.configuration = config.name, .snapshot = version};
        auto workspace = s.snapshot()->server.auth.workspace;
        auto who = worlds::execution_key(actor);
        auto started = std::chrono::steady_clock::now();
        std::optional<std::uint64_t> id;
        try {
            id = co_await s.files.with(s.core, project, [](storage::project_ctx& c) { return c.ledger.admit_build(c.settings.project, c.settings.limits); });
        } catch (const std::exception& e) {
            r.error = e.what();
        }
        if (!id)
            co_return r;
        admission held{&s.files,
                       s.core,
                       *id,
                       usage::build_event{.project = project,
                                          .target = config.name,
                                          .workspace_id = workspace,
                                          .worldline = worldline,
                                          .version = version,
                                          .snapshot = snapshot.id().str(),
                                          .operation = operation,
                                          .actor = who},
                       started,
                       std::move(executor)};

        auto& files = s.files;
        auto& rt = s.workers.io_context();
        auto& router = s.workers;
        std::optional<std::pair<jobs::build_report, sync_stats>> done;
        try {
            auto run_on_worker = [&files, &rt, &router, executor = held.executor, project, tree = snapshot, request, worldline, version,
                                  actor](auto& worker, jobs::progress_log* progress,
                                         std::size_t core) -> coroutine::task<std::pair<jobs::build_report, sync_stats>> {
                auto identity = router.workers(core).identity(worker);
                executor->select(identity);
                auto result =
                    co_await sync_and_run(rt.get_scheduler(core), files, core, worker, project, tree, request, progress, worldline, version, actor, executor);
                result.first.worker = std::move(identity.first);
                result.first.worker_id = std::move(identity.second);
                co_return result;
            };
            done =
                co_await s.workers.with_worker(s.core,
                                               routing::selection{.needs = config.needs.get() ? routing::requirements{.tools = config.needs.get()->tools,
                                                                                                                      .os = config.needs.get()->platform.os,
                                                                                                                      .arch = config.needs.get()->platform.arch,
                                                                                                                      .toolchain_id = toolchain}
                                                                                              : routing::requirements{.toolchain_id = toolchain},
                                                                  .tree_key = request.project + "/" + request.tree,
                                                                  .memory_bytes = request.max_memory_bytes},
                                               std::move(run_on_worker), &progress);
        } catch (const routing::no_worker& e) {
            if (e.busy) {
                held.ended = true;
                s.files.post(s.core, [id = *id](usage::ledger& ledger) { ledger.release(id); });
                co_return std::nullopt;
            }
            r.error = e.what();
            r.error_code = "no_worker";
        } catch (const tool_error& e) {
            r.error = e.what();
            r.error_code = e.code;
        } catch (const transport::remote_error& e) {
            r.error = e.what();
            r.error_code = e.code.empty() ? "worker_failed" : e.code;
        } catch (const std::exception& e) {
            r.error = e.what();
            r.error_code = held.executor->started() ? "outcome_unknown" : "sync_failed";
        }

        held.executor->stamp(r);
        if (done) {
            auto& [rep, sync] = *done;
            r.worker = rep.worker;
            r.worker_id = rep.worker_id;
            r.ok = rep.ok();
            r.seconds = rep.seconds;
            r.timings.sync_seconds = sync.seconds;
            r.timings.worker_seconds = rep.seconds;
            r.timings.preparation_seconds = rep.preparation_seconds;
            r.timings.execution_seconds = rep.execution_seconds;
            std::tie(r.diagnostics, r.tests) = detail::read(rep);
            for (auto& st : rep.steps)
                r.steps.push_back({st.name, st.exit_code, st.timed_out, st.seconds, std::move(st.output), st.truncated, std::move(st.log), st.log_truncated,
                                   std::move(st.log_error), std::move(st.cache_error), std::move(st.tasks)});
            r.blobs_sent = sync.blobs;
            r.bytes_sent = sync.bytes;
            r.exports = std::move(rep.exports);
            r.models = std::move(rep.models);
            r.export_error = std::move(rep.export_error);
            r.export_skipped = std::move(rep.export_skipped);
        }
        auto last = r.steps.empty() ? nullptr : &r.steps.back();
        usage::build_event event{.project = project,
                                 .target = config.name,
                                 .worker = r.worker,
                                 .worker_id = r.worker_id,
                                 .ok = r.ok,
                                 .exit_code = last ? last->exit_code : -1,
                                 .timed_out = last && last->timed_out,
                                 .seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                                 .blobs_sent = r.blobs_sent,
                                 .bytes_sent = r.bytes_sent,
                                 .error = r.error,
                                 .workspace_id = workspace,
                                 .worldline = worldline,
                                 .version = version,
                                 .snapshot = snapshot.id().str(),
                                 .operation = operation,
                                 .actor = who,
                                 .error_code = r.error_code};
        // Posted, not awaited: a cancellation arriving now must not leave the admission held.
        held.ended = true;
        s.files.post(s.core, [id = *id, event](usage::ledger& ledger) { ledger.record(id, event); });
        co_return r;
    }

    // Waits here for a queued job and passes its progress on as it comes, with its place in the queue while it waits. A caller that goes
    // away leaves the job.
    template <class Pass> auto follow(services& s, std::shared_ptr<backlog::ticket> held, Pass pass) -> coroutine::task<backlog::outcome> {
        struct leaving {
            std::shared_ptr<backlog::ticket> held;
            ~leaving() { held->leave(); }
        } guard{held};
        auto io = s.workers.io_context().get_scheduler(s.core);
        std::size_t seen = 0;
        std::optional<std::size_t> told;
        while (!held->done()) {
            for (auto& p : held->progress(seen))
                pass(p);
            if (seen == 0 && told != held->ahead()) {
                told = held->ahead();
                pass({.stage = std::format("queued, {} ahead", *told)});
            }
            co_await execution::schedule_after(io, std::chrono::milliseconds{50});
        }
        for (auto& p : held->progress(seen))
            pass(p);
        co_return held->result();
    }

    // A compact result of worker preparation and analysis.
    auto prepared(const language::plan& plan, const content::manifest& files, std::string version, jobs::build_report& report) -> language::result {
        language::result out{.status = report.ok() ? "prepared" : "failed",
                             .revision = std::move(version),
                             .snapshot = files.id(),
                             .tree = report.tree,
                             .build_dirs = plan.build.build_dirs,
                             .worker = report.worker,
                             .worker_id = report.worker_id};
        for (auto& step : report.steps)
            out.steps.push_back({step.name, step.exit_code, step.timed_out, step.seconds, std::move(step.output), step.truncated, std::move(step.tasks)});
        if (!report.ok()) {
            out.error = "analysis preparation command failed";
            for (const auto& step : out.steps) {
                if (step.exit_code == 0 && !step.timed_out)
                    continue;
                constexpr std::size_t limit = 4096;
                auto start = step.output.size() > limit ? step.output.size() - limit : 0;
                while (start < step.output.size() && (static_cast<unsigned char>(step.output[start]) & 0xC0) == 0x80)
                    ++start;
                const auto status = step.timed_out ? std::string{"timed out"} : "exit code " + std::to_string(step.exit_code);
                out.error = "analysis preparation step '" + step.name + "': " + status + "; truncated=" + (step.truncated || start != 0 ? "true" : "false") +
                            "\n" + step.output.substr(start);
                break;
            }
        }
        out.artifacts = report.measured;
        for (const auto& input : plan.inputs)
            if (auto measured = std::ranges::find(report.measured, input.name, &builds::artifact_result::name); measured != report.measured.end())
                out.inputs.push_back(*measured);
        return out;
    }

    auto hot_key(const std::string& project, const std::string& layout) -> std::string {
        return std::string{content::hash_of(rfl::json::write(std::vector<std::string>{project, layout})).view().substr(0, 24)};
    }

    auto runtime_pool(const std::string& group, const std::string& runtime) -> std::string {
        return group + "-" + std::string{content::hash_of(runtime).view().substr(0, 24)};
    }

    // Select an actual runtime before computing a content key. Busy workers still name
    // valid runtimes: a cache hit needs no slot, and a miss can wait in the backlog.
    auto select_toolchain(services& s, routing::requirements needs, const std::string& trees) -> coroutine::task<std::string> {
        auto workers = co_await s.workers.describe(s.core);
        std::optional<std::tuple<bool, bool, std::string>> selected;
        std::map<std::string, std::size_t> unavailable;
        for (const auto& worker : workers) {
            std::string reason;
            if (!worker.online)
                reason = "offline";
            else if (worker.dependency_publisher != needs.dependency_publisher)
                reason = "different dependency publisher role";
            else if (needs.os && worker.platform.os != *needs.os)
                reason = "requires OS " + *needs.os;
            else if (needs.arch && worker.platform.arch != *needs.arch)
                reason = "requires architecture " + *needs.arch;
            else if (auto missing = std::ranges::find_if(needs.tools, [&](const auto& name) { return !std::ranges::contains(worker.tools, name); });
                     missing != needs.tools.end())
                reason = "missing tool " + *missing;
            else if (worker.toolchain_id.empty())
                reason = "toolchain inspection not reported";
            if (!reason.empty()) {
                ++unavailable[reason];
                continue;
            }
            const auto pool = runtime_pool(trees, worker.toolchain_id);
            bool cold = !std::ranges::any_of(worker.slots, [&](const auto& slot) { return !slot.busy && slot.tree.starts_with(pool + "-"); });
            const bool publisher = std::ranges::any_of(workers, [&](const auto& candidate) {
                return candidate.online && candidate.dependency_publisher && candidate.toolchain_id == worker.toolchain_id;
            });
            auto candidate = std::tuple{cold, !publisher, worker.toolchain_id};
            if (!selected || candidate < *selected)
                selected = std::move(candidate);
        }
        if (!selected)
            throw tool_error("no_worker", "no worker with the required tools and a reported toolchain fingerprint is online; " +
                                              (workers.empty() ? std::string{"no workers attached"} : rfl::json::write(unavailable)));
        co_return std::get<2>(*selected);
    }

    // Run fills before admitting the consumer job: no nested backlog slot is held while waiting.
    auto fill_dependencies(services& s, const std::string& project, const std::vector<jobs::step>& steps, const content::manifest& snapshot,
                           const std::string& toolchain, const worlds::actor& actor, const std::string& worldline, const std::string& version,
                           std::chrono::steady_clock::time_point deadline, mcp::notifier* note) -> coroutine::task<std::string> {
        if (!std::ranges::any_of(steps, [](const auto& step) { return step.dependencies.has_value(); }))
            co_return "";
        const auto workers = co_await s.workers.describe(s.core);
        if (!std::ranges::any_of(workers, [&](const auto& worker) { return worker.online && worker.dependency_publisher && worker.toolchain_id == toolchain; }))
            co_return "unavailable: no matching dependency publisher";
        if (std::ranges::any_of(snapshot.files, [](const auto& file) { return std::filesystem::path{file.path}.filename() == "vcpkg-configuration.json"; }))
            co_return "skipped: custom registries";
        std::string status;
        std::set<std::string> filled;
        bool omitted = false;
        for (const auto& step : steps) {
            if (!step.dependencies)
                continue;
            auto record_status = [&](std::string value) {
                if (omitted)
                    return;
                constexpr std::string_view marker = "additional dependency results omitted";
                const auto entry = step.name + ": " + value;
                if (status.size() + entry.size() + 2 <= constants::dependency_status_bytes - marker.size() - 2) {
                    if (!status.empty())
                        status += "; ";
                    status += entry;
                } else {
                    if (!status.empty())
                        status += "; ";
                    status += marker;
                    omitted = true;
                }
            };
            const auto& input = *step.dependencies;
            const auto path = (std::filesystem::path{input.manifest_dir} / "vcpkg.json").lexically_normal().generic_string();
            const auto found = std::ranges::find(snapshot.files, path, &content::entry::path);
            if (found == snapshot.files.end() || found->size > constants::dependency_manifest_bytes) {
                record_status("skipped: manifest absent or too large");
                continue;
            }
            try {
                nodes::node_request wanted{.action = "content", .worldline = worldline, .revision = version};
                wanted.parts.push_back({found->digest.str(), 0, found->size, {}});
                auto reply = co_await s.files.node(s.core, project, std::move(wanted), actor, true);
                if (reply.code != "ok" || reply.parts.size() != 1)
                    throw std::runtime_error("dependency manifest is unavailable");
                const auto text = lines::unhex(reply.parts.front().data);
                if (text.size() != found->size || content::hash_of(text) != found->digest)
                    throw std::runtime_error("dependency manifest digest mismatch");
                jobs::dependency_fill request{.manifest = builds::dependency_manifest(text),
                                              .triplet = input.triplet,
                                              .host_triplet = input.host_triplet,
                                              .features = input.features,
                                              .default_features = input.default_features};
                const auto key = content::hash_of(rfl::json::write(std::tuple{std::string{"dependency.fill"}, toolchain, request})).str();
                if (!filled.insert(key).second) {
                    record_status("same dependencies as an earlier step");
                    continue;
                }
                backlog::entry entry{.key = key,
                                     .project = project,
                                     .configuration = "dependencies",
                                     .operation = "dependencies",
                                     .actor = worlds::execution_key(actor),
                                     .cacheable = false};
                auto run_operation = [request, toolchain, project, actor, worldline, version,
                                      snapshot_id = snapshot.id().str()](services& zero, jobs::progress_log& progress, backlog::placements,
                                                                         std::string) -> coroutine::task<std::optional<backlog::outcome>> {
                    config_result out{.configuration = "dependencies"};
                    std::uint64_t admission_id;
                    try {
                        admission_id = co_await zero.files.with(zero.core, project, [](storage::project_ctx& context) {
                            return context.ledger.admit_build(context.settings.project, context.settings.limits);
                        });
                    } catch (const std::exception& error) {
                        out.error = std::string{error.what()}.substr(0, 512);
                        co_return out;
                    }
                    admission held{&zero.files, zero.core, admission_id,
                                   usage::build_event{.project = project,
                                                      .target = "dependencies",
                                                      .workspace_id = zero.snapshot()->server.auth.workspace,
                                                      .worldline = worldline,
                                                      .version = version,
                                                      .snapshot = snapshot_id,
                                                      .operation = "dependencies",
                                                      .actor = worlds::execution_key(actor),
                                                      .error_code = "cancelled"},
                                   std::chrono::steady_clock::now()};
                    std::int32_t exit_code = -1;
                    bool timed_out = false;
                    try {
                        auto run_on_worker = [request, &zero, identity = held.executor](auto& worker, jobs::progress_log* progress,
                                                                                        std::size_t core) -> coroutine::task<jobs::build_report> {
                            identity->select(zero.workers.workers(core).identity(worker));
                            identity->begin();
                            co_return co_await within(zero.workers.io_context().get_scheduler(core), std::chrono::seconds{request.timeout_s} + build_overhead,
                                                      "dependency fill", worker.call(request, progress));
                        };
                        auto report =
                            co_await zero.workers.with_worker(zero.core, routing::selection{.needs = {.toolchain_id = toolchain, .dependency_publisher = true}},
                                                              std::move(run_on_worker), &progress);
                        out.worker = report.worker;
                        out.ok = !report.steps.empty() && report.ok() &&
                                 std::ranges::all_of(report.steps, [](const auto& step) { return step.cache_error.empty(); });
                        if (!report.steps.empty()) {
                            exit_code = report.steps.back().exit_code;
                            timed_out = report.steps.back().timed_out;
                        }
                        if (!out.ok)
                            out.error = "trusted dependency fill failed; private build remains available";
                    } catch (const routing::no_worker& error) {
                        if (error.busy) {
                            held.ended = true;
                            zero.files.post(zero.core, [admission_id](usage::ledger& ledger) { ledger.release(admission_id); });
                            co_return std::nullopt;
                        }
                        out.error = error.what();
                    } catch (const std::exception& error) {
                        out.error = std::string{error.what()}.substr(0, 512);
                    }
                    held.executor->stamp(out);
                    auto event = held.cancelled;
                    held.executor->stamp(event);
                    event.ok = out.ok;
                    event.exit_code = exit_code;
                    event.timed_out = timed_out;
                    event.error_code = out.ok ? "" : "dependency_fill_failed";
                    event.error = out.error;
                    event.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - held.started).count();
                    held.ended = true;
                    zero.files.post(zero.core, [admission_id, event](usage::ledger& ledger) { ledger.record(admission_id, event); });
                    co_return out;
                };
                auto ticket = co_await s.queue->submit(s.core, std::move(entry), std::move(run_operation));
                const auto remaining = std::max(std::chrono::steady_clock::duration::zero(), deadline - std::chrono::steady_clock::now());
                auto done = co_await coroutine::deadline(s.workers.io_context().get_scheduler(s.core), remaining,
                                                         follow(s, ticket, [note](const jobs::progress& progress) {
                                                             if (note)
                                                                 (*note)(progress_note(progress));
                                                         }));
                if (!done) {
                    record_status("timeout: waiting for shared dependencies");
                    co_return status;
                }
                const auto& result = std::get<config_result>(*done);
                record_status(result.ok ? "filled" : "failed: " + result.error);
            } catch (const std::exception& error) {
                record_status("skipped: " + std::string{error.what()}.substr(0, 256));
            }
        }
        co_return status;
    }

    // Preparation uses the directory group's selected worker and reserved tree. Source supplies the pinned files.
    auto run_preparation(services& zero, std::string project, std::string worldline, std::string revision, language::request request, std::string tree,
                         worlds::actor actor, std::string toolchain, std::string computation, std::string stage, builds::requirements placement,
                         std::set<std::string> producers, std::optional<routing::location>& residence, semantic::retention_roots roots,
                         jobs::progress_log* progress = nullptr) -> coroutine::task<std::optional<backlog::outcome>> {
        language::plan plan;
        try {
            plan = language::prepare(request.config, request.builds);
        } catch (const std::invalid_argument& error) {
            throw tool_error(nodes::code::invalid_record, error.what());
        }
        auto line = execution_line(actor, std::optional{worldline});
        auto selected_tree = co_await selected(zero, project, line, revision, actor);
        content::manifest files;
        for (auto& f : lines::manifest(selected_tree))
            files.files.push_back({f.path, f.digest, f.size, f.executable});
        try {
            language::check(plan, files.files);
        } catch (const std::invalid_argument& error) {
            throw tool_error(nodes::code::invalid_record, error.what());
        }
        auto needs = routing::requirements{.tools = placement.tools, .os = placement.platform.os, .arch = placement.platform.arch};
        if (!plan.server.argv.empty() && plan.server.argv.front().find('/') == std::string::npos &&
            !std::ranges::contains(needs.tools, plan.server.argv.front()))
            needs.tools.push_back(plan.server.argv.front());
        needs.toolchain_id = toolchain;
        jobs::staged_analysis build{.preparation = {.project = project,
                                                    .tree = std::move(tree),
                                                    .snapshot = files.id(),
                                                    .steps = plan.build.steps,
                                                    .preparation_tasks = plan.build.tasks,
                                                    .timeout_s = language::timeout(plan.execution.limits),
                                                    .output_limit = language::output_limit(plan.execution.limits),
                                                    .env = plan.build.env,
                                                    .max_memory_bytes = plan.execution.limits.max_memory_bytes,
                                                    .max_cpu_seconds = plan.execution.limits.max_cpu_seconds}};
        build.preparation.generation_producers = std::move(producers);
        for (auto& step : plan.build.steps)
            std::ranges::copy(step.outputs, std::back_inserter(build.preparation.measure));
        build.analysis = jobs::analysis_plan{.provider = plan.server.provider,
                                             .options = language::fingerprint(request.config, request.builds).str(),
                                             .configuration = request.configuration,
                                             .inputs = request.project_inputs,
                                             .artifacts = plan.inputs,
                                             .argv = plan.server.argv,
                                             .initialization = plan.server.initialization,
                                             .env = plan.server.env,
                                             .seconds = language::timeout(plan.execution.limits),
                                             .query = request.query,
                                             .budget = request.budget,
                                             .execution = {"request-" + nodes::hex(nodes::random_bytes(16)), std::move(computation), std::move(stage)},
                                             .slot_id = project + "/" + build.preparation.tree,
                                             .demand = request.demand,
                                             .source_facts = request.source_facts,
                                             .commands = plan.server.commands,
                                             .executables = plan.server.executables,
                                             .retained = request.retained,
                                             .registration = plan.server.registration,
                                             .documents = plan.server.documents,
                                             .project_code = language::describe_provider(request.config.server).executes_project_code};
        auto& registry = zero.files;
        auto& rt = zero.workers.io_context();
        auto& router = zero.workers;
        auto executor = std::make_shared<execution_identity>();
        auto started = std::chrono::steady_clock::now();
        std::uint64_t admission_id;
        try {
            admission_id = co_await zero.files.with(zero.core, project,
                                                    [](storage::project_ctx& c) { return c.ledger.admit_build(c.settings.project, c.settings.limits); });
        } catch (const usage::quota_exceeded& error) {
            throw tool_error(nodes::code::resource_limit, error.what());
        }
        admission held{&zero.files,
                       zero.core,
                       admission_id,
                       usage::build_event{.project = project,
                                          .target = request.config.build_configuration,
                                          .workspace_id = zero.snapshot()->server.auth.workspace,
                                          .worldline = line,
                                          .version = selected_tree.version,
                                          .snapshot = files.id().str(),
                                          .operation = "prepare",
                                          .actor = worlds::execution_key(actor)},
                       started,
                       executor};
        auto record = [&](const language::result& result, const sync_stats& sync) {
            auto event = held.cancelled;
            executor->stamp(event);
            event.ok = result.status == "prepared" && result.analysis == "analyzed";
            event.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            event.blobs_sent = sync.blobs;
            event.bytes_sent = sync.bytes;
            event.error = result.analysis_error.value_or(result.error);
            event.error_code = event.ok || result.status == "failed" ? "" : result.status == "prepared" ? result.analysis_error_code : result.status;
            if (!result.steps.empty()) {
                event.exit_code = result.steps.back().exit_code;
                event.timed_out = result.steps.back().timed_out;
            }
            held.ended = true;
            zero.files.post(zero.core, [id = admission_id, event](usage::ledger& ledger) { ledger.record(id, event); });
        };
        std::optional<std::pair<jobs::build_report, sync_stats>> done;
        std::string failure, failure_code;
        bool stopped = false;
        auto token = co_await coroutine::current_token;
        try {
            // An analysis that loads project code executes; one that only reads project data takes the analyze role.
            const routing::selection placed{.needs = needs,
                                            .tree_key = build.preparation.project + "/" + build.preparation.tree,
                                            .options = build.analysis.options,
                                            .snapshot = build.preparation.snapshot.str(),
                                            .stage = build.analysis.execution.stage_id,
                                            .memory_bytes = build.preparation.max_memory_bytes,
                                            .worker = residence ? std::optional{residence->worker} : std::nullopt,
                                            .core = residence ? std::optional{residence->core} : std::nullopt,
                                            .analysis = !build.analysis.project_code};
            auto run_on_worker = [&registry, &rt, &router, &residence, executor, project, files, build, line, version = selected_tree.version, actor, roots,
                                  placed, snapshots = request.snapshots,
                                  sources = request.sources](auto& worker, jobs::progress_log* progress,
                                                             std::size_t core) -> coroutine::task<std::pair<jobs::build_report, sync_stats>> {
                residence = router.workers(core).locate(worker);
                auto identity = router.workers(core).identity(worker);
                executor->select(identity);
                auto result = co_await sync_and_run(rt.get_scheduler(core), registry, core, worker, project, files, build, progress, line, version, actor,
                                                    executor, roots, snapshots, sources, &router.workers(core), placed);
                result.first.worker = std::move(identity.first);
                result.first.worker_id = std::move(identity.second);
                co_return result;
            };
            done = co_await coroutine::under(token, zero.workers.with_worker(zero.core, placed, std::move(run_on_worker), progress));
            stopped = !done;
        } catch (const routing::no_worker& e) {
            if (e.busy) {
                held.ended = true;
                zero.files.post(zero.core, [id = admission_id](usage::ledger& ledger) { ledger.release(id); });
                co_return std::nullopt;
            }
            failure = e.what();
            failure_code = "no_worker";
        } catch (const tool_error& e) {
            failure = e.what();
            failure_code = e.code;
        } catch (const transport::remote_error& e) {
            failure = e.what();
            failure_code = e.code.empty() ? "worker_failed" : e.code;
        } catch (const std::exception& e) {
            failure = e.what();
            failure_code = executor->started() ? "outcome_unknown" : "sync_failed";
        }
        if (stopped)
            co_await coroutine::stop;
        if (!done) {
            language::result failed{.status = failure_code, .revision = selected_tree.version, .snapshot = files.id(), .error = failure};
            executor->stamp(failed);
            record(failed, {});
            co_return failed;
        }
        auto& report = done->first;
        auto result = prepared(plan, files, selected_tree.version, report);
        result.roots.value() = roots;
        result.worldline = line;
        result.timings.sync_seconds = done->second.seconds;
        result.timings.worker_seconds = report.seconds;
        result.timings.preparation_seconds = report.preparation_seconds;
        result.timings.execution_seconds = report.execution_seconds;
        if (report.analysis_error) {
            result.analysis = "failed";
            result.analysis_error = report.analysis_error;
            result.analysis_error_code = report.analysis_error_code;
        } else if (report.ok() && report.analyzed) {
            const bool deferred = std::ranges::any_of(report.missing, [](const auto& missing) { return missing.deferred; });
            const auto& session = report.analysis_session;
            if (!report.analysis_execution || *report.analysis_execution != build.analysis.execution || (!session && !deferred) ||
                (session && (session->phase != "ready" || session->epoch == 0 || session->project != project || session->provider != plan.server.provider ||
                             session->options != build.analysis.options || session->snapshot != result.snapshot.str()))) {
                result.analysis = "failed";
                result.analysis_error_code = "stale_analysis";
                result.analysis_error = "provider result does not belong to the requested inputs";
            } else {
                result.analysis = "analyzed";
                result.session = std::move(report.analysis_session);
                result.reused_units = report.reused_units;
                result.stage_views = std::move(report.stage_views);
                result.missing = std::move(report.missing);
                result.cacheable = report.analysis_cacheable;
            }
        } else {
            result.analysis = "failed";
            result.analysis_error = result.error.empty() ? "language worker produced no result" : result.error;
        }
        record(result, done->second);
        co_return result;
    }

    // Each caller owns one ticket for the selected stage graph and its directory groups.
    auto prepare(services& s, mcp::call_context context, std::string project, std::string configuration, std::string worldline, std::string revision,
                 std::vector<language::request> requests, std::set<std::string> exposed, const worlds::actor& actor, mcp::notifier* note = nullptr,
                 semantic::request requested = {}, std::map<std::string, std::string> pinned_toolchains = {}) -> coroutine::task<language::result> {
        for (auto& request : requests)
            request.configuration = configuration;
        if (requests.empty() || worldline.empty())
            throw tool_error("context_required", "worldline and analysis stages are required");
        auto source = co_await selected(s, project, worldline, revision, actor);
        content::manifest manifest;
        for (const auto& file : source.files)
            manifest.files.push_back({file.path, file.digest, file.size, file.executable});
        invocation observed{s.files,
                            s.core,
                            {.project = project,
                             .configuration = configuration,
                             .operation = requested.action.str(),
                             .actor = worlds::execution_key(actor),
                             .worldline = worldline,
                             .version = source.version,
                             .snapshot = manifest.id().str(),
                             .reuse = "none",
                             .parameters = rfl::json::write(requested)}};
        observed.event.timings.client_sync_seconds = context.client_sync_seconds;
        std::map<std::string, std::string> toolchains, groups;
        std::map<std::string, builds::requirements> placements;
        std::vector<language::plan> plans;
        try {
            for (auto& request : requests) {
                auto plan = language::prepare(request.config, request.builds);
                language::check(plan, manifest.files);
                auto group = hot_key(project, *plan.execution.directory_group);
                groups.emplace(request.stage, group);
                builds::combine(placements[group], plan.execution.needs.get());
                plans.push_back(std::move(plan));
            }
            std::map<std::string, std::string> runtimes;
            if (pinned_toolchains.empty()) {
                for (const auto& [group, needs] : placements)
                    runtimes.emplace(group, co_await select_toolchain(s, {.tools = needs.tools, .os = needs.platform.os, .arch = needs.platform.arch}, group));
            } else
                for (const auto& [stage, group] : groups)
                    runtimes.emplace(group, pinned_toolchains.at(stage));
            std::map<std::string, builds::requirements> selected;
            for (auto& [stage, group] : groups) {
                const auto& runtime = runtimes.at(group);
                toolchains.emplace(stage, runtime);
                auto pool = runtime_pool(group, runtime);
                selected.try_emplace(pool, placements.at(group));
                group = std::move(pool);
            }
            placements = std::move(selected);
        } catch (const std::invalid_argument& error) {
            observed.event.state = "failed";
            throw tool_error(nodes::code::invalid_record, error.what());
        } catch (...) {
            observed.event.state = "failed";
            throw;
        }
        std::map<std::string, std::set<std::string>> generation_producers;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto& producers = generation_producers[groups.at(requests[i].stage)];
            for (const auto& step : plans[i].build.steps)
                if (step.generation)
                    producers.insert(step.generation->producer);
        }
        auto key = content::hash_of(rfl::json::write(std::tuple{std::string{"language"}, project, manifest.id().str(), toolchains, requests, exposed})).str();
        std::map<std::string, semantic::extent> demands;
        auto family_requests = requests;
        for (auto& request : family_requests) {
            demands.emplace(request.stage, std::move(request.demand));
            request.demand = {};
        }
        auto family =
            content::hash_of(rfl::json::write(std::tuple{std::string{"language"}, project, manifest.id().str(), toolchains, family_requests, exposed})).str();
        double dependency_seconds = 0;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto& request = requests[i];
            const auto& plan = plans[i];
            const auto started = std::chrono::steady_clock::now();
            const auto deadline = started + std::chrono::seconds{language::timeout(plan.execution.limits)};
            co_await fill_dependencies(s, project, plan.build.steps, manifest, toolchains.at(request.stage), actor, worldline, source.version, deadline, note);
            dependency_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const auto left = std::chrono::ceil<std::chrono::seconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0)
                throw tool_error("timeout", "execution deadline exceeded while filling dependencies");
            request.builds.at(request.config.build_configuration).limits.timeout_s = static_cast<std::uint32_t>(left);
        }
        observed.event.timings.dependency_seconds = dependency_seconds;
        backlog::entry what{.key = key,
                            .project = project,
                            .configuration = configuration,
                            .operation = "prepare",
                            .actor = worlds::execution_key(actor),
                            .cacheable = true,
                            .worldline = worldline,
                            .family = std::move(family),
                            .demands = std::move(demands)};
        for (const auto& [_, group] : groups)
            if (!std::ranges::contains(what.trees, group))
                what.trees.push_back(group);
        for (auto& f : source.files)
            what.files.push_back(f.path + '\n' + f.digest);
        // Completed stages publish immutable units. Keep them while a later stage
        // waits for a worker, so a retry resumes the graph instead of evaluating
        // its completed prefix and charging its execution quota again.
        auto evaluated = std::make_shared<std::pair<std::size_t, language::result>>();
        auto stage_outputs = std::make_shared<std::map<std::string, std::vector<semantic::view_id>>>();
        auto stage_closures = std::make_shared<std::map<std::string, std::vector<semantic::view_id>>>();
        auto residences = std::make_shared<std::map<std::string, std::optional<routing::location>>>();
        evaluated->second.status = "prepared";
        evaluated->second.analysis = "analyzed";
        evaluated->second.toolchains = toolchains;
        evaluated->second.roots.value() = co_await s.files.with_semantic(s.core, [](semantic::store& store) { return store.protect(); });
        auto held = co_await s.queue->submit(
            s.core, std::move(what),
            [project, worldline, version = source.version, requests, actor, toolchains, groups, placements, generation_producers, exposed, evaluated,
             residences, stage_outputs, stage_closures](services& zero, jobs::progress_log& progress, backlog::placements trees,
                                                        std::string computation) -> coroutine::task<std::optional<backlog::outcome>> {
                auto& combined = evaluated->second;
                auto add = [](std::optional<double>& total, const std::optional<double>& value) {
                    if (value)
                        total = total.value_or(0) + *value;
                };
                for (; evaluated->first < requests.size(); ++evaluated->first) {
                    auto request = requests[evaluated->first];
                    std::vector<semantic::view_id> inputs = request.project_views;
                    for (const auto& [port, producers] : request.config.inputs)
                        for (const auto& producer : producers)
                            if (const auto* ref = std::get_if<language::reference>(&producer)) {
                                const auto& views = stage_outputs->at(ref->stage);
                                auto shards = co_await zero.files.with_semantic(zero.core, [views](semantic::store& store) {
                                    std::set<semantic::shard_id> ids;
                                    for (const auto& view : views)
                                        for (const auto& [unit, objects] : store.describe(view).units)
                                            ids.insert(objects.begin(), objects.end());
                                    return std::vector<semantic::shard_id>{ids.begin(), ids.end()};
                                });
                                auto& target = request.source_facts[port];
                                std::ranges::move(shards, std::back_inserter(target));
                                for (const auto& view : stage_closures->at(ref->stage))
                                    if (!std::ranges::contains(inputs, view))
                                        inputs.push_back(view);
                            }
                    auto done = co_await run_preparation(zero, project, worldline, version, request, trees.at(groups.at(request.stage)), actor,
                                                         toolchains.at(request.stage), computation, request.stage, placements.at(groups.at(request.stage)),
                                                         generation_producers.at(groups.at(request.stage)), (*residences)[groups.at(request.stage)],
                                                         combined.roots.value(), &progress);
                    if (!done)
                        co_return std::nullopt;
                    if (std::holds_alternative<config_result>(*done))
                        co_return std::move(*done);
                    auto result = std::get<language::result>(std::move(*done));
                    add(combined.timings.sync_seconds, result.timings.sync_seconds);
                    add(combined.timings.worker_seconds, result.timings.worker_seconds);
                    add(combined.timings.preparation_seconds, result.timings.preparation_seconds);
                    add(combined.timings.execution_seconds, result.timings.execution_seconds);
                    if (result.status != "prepared" || result.analysis != "analyzed") {
                        result.timings = combined.timings;
                        co_return result;
                    }
                    // Ports carry only the producing stage's own facts. Keep
                    // its transitive inputs separately for the queryable view.
                    (*stage_outputs)[request.stage] = result.stage_views;
                    for (const auto& view : inputs)
                        if (!std::ranges::contains(result.stage_views, view))
                            result.stage_views.push_back(view);
                    (*stage_closures)[request.stage] = result.stage_views;
                    combined.reused_units += result.reused_units;
                    combined.cacheable &= result.cacheable;
                    combined.worldline = result.worldline;
                    combined.revision = result.revision;
                    combined.snapshot = result.snapshot;
                    combined.worker = result.worker;
                    combined.worker_id = result.worker_id;
                    combined.steps.insert(combined.steps.end(), result.steps.begin(), result.steps.end());
                    combined.checkpoint_views.insert(combined.checkpoint_views.end(), result.stage_views.begin(), result.stage_views.end());
                    combined.missing.insert(combined.missing.end(), result.missing.begin(), result.missing.end());
                    if (!exposed.contains(request.stage))
                        continue;
                    combined.stage_views.insert(combined.stage_views.end(), result.stage_views.begin(), result.stage_views.end());
                }
                co_await zero.files.with_semantic(zero.core, [roots = combined.roots.value(), views = combined.checkpoint_views](semantic::store&) {
                    *roots = semantic::retained_objects{.views = std::set<semantic::view_id>{views.begin(), views.end()}};
                });
                co_return combined;
            });
        observed.event.calculation = key;
        observed.event.reuse = held->reuse();
        auto done = co_await follow(s, held, [note](const jobs::progress& progress) {
            if (note)
                (*note)(progress_note(progress));
        });
        if (auto* refused = std::get_if<config_result>(&done)) {
            observed.event.timings = refused->timings;
            observed.event.timings.client_sync_seconds = context.client_sync_seconds;
            observed.event.timings.dependency_seconds = dependency_seconds;
            observed.event.state = "failed";
            throw tool_error(refused->error_code, refused->error);
        }
        auto result = std::get<language::result>(std::move(done));
        result.timings.dependency_seconds = dependency_seconds;
        result.timings.client_sync_seconds = context.client_sync_seconds;
        observed.event.timings = result.timings;
        observed.event.state = result.status == "prepared" && result.analysis == "analyzed" ? "succeeded" : "failed";
        result.worldline = worldline;
        result.revision = source.version;
        result.snapshot = manifest.id();
        co_return result;
    }

    // One configuration of a build, test or run: queued in the backlog, waited for here, its progress passed on as it comes.
    auto execute(services& s, mcp::call_context context, std::string project, std::string operation, bool cacheable, bool fresh,
                 config::effective_config config, std::vector<jobs::step> steps, content::manifest snapshot, mcp::notifier& note, config_result& r,
                 std::string worldline, std::string version, worlds::actor actor) -> coroutine::task<> {
        r.timings.client_sync_seconds = context.client_sync_seconds;
        for (auto& step : steps)
            if (step.generation)
                step.generation->producer = config.name + "/" + step.generation->producer;
        jobs::build_request request{.project = project,
                                    .steps = steps,
                                    .timeout_s = config.limits.timeout_s,
                                    .output_limit = config.limits.output_limit,
                                    .env = config.env,
                                    .max_memory_bytes = config.limits.max_memory_bytes,
                                    .max_cpu_seconds = config.limits.max_cpu_seconds,
                                    .fresh = fresh};
        for (const auto& step : steps)
            if (step.generation)
                request.generation_producers.insert(step.generation->producer);
        request.exports = config.build.operations.at(operation).exports;
        auto trees = hot_key(project, config.name);
        auto required = config.needs.get().value_or(config::requirements{});
        auto needs = routing::requirements{.tools = required.tools, .os = required.platform.os, .arch = required.platform.arch};
        invocation observed{s.files,
                            s.core,
                            {.project = project,
                             .configuration = config.name,
                             .operation = operation,
                             .actor = worlds::execution_key(actor),
                             .worldline = worldline,
                             .version = version,
                             .snapshot = snapshot.id().str(),
                             .reuse = "none"}};
        observed.event.timings.client_sync_seconds = context.client_sync_seconds;
        std::string toolchain;
        try {
            toolchain = co_await select_toolchain(s, needs, trees);
        } catch (const tool_error& error) {
            observed.event.state = "failed";
            r.configuration = config.name;
            r.snapshot = version;
            r.error = error.what();
            r.error_code = error.code;
            co_return;
        }
        trees = runtime_pool(trees, toolchain);
        auto normalized = request;
        normalized.project.clear();
        normalized.tree.clear();
        normalized.fresh = false;
        auto key = content::hash_of(rfl::json::write(std::tuple{operation, snapshot.id().str(), toolchain, normalized, config.needs})).str();
        // Test and run operations are effects: every call gets its own execution.
        static std::atomic<std::uint64_t> calls{0};
        if (!cacheable)
            key += "/call/" + std::to_string(calls.fetch_add(1));
        const auto dependency_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{request.timeout_s};
        const auto dependency_started = std::chrono::steady_clock::now();
        const auto dependency_status =
            co_await fill_dependencies(s, project, steps, snapshot, toolchain, actor, worldline, version, dependency_deadline, &note);
        const auto dependency_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - dependency_started).count();
        observed.event.timings.dependency_seconds = dependency_seconds;
        const auto left = std::chrono::ceil<std::chrono::seconds>(dependency_deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            r.timings.dependency_seconds = dependency_seconds;
            r.configuration = config.name;
            r.snapshot = version;
            r.dependency_cache = dependency_status;
            r.error = "execution deadline exceeded while filling dependencies";
            r.error_code = "timeout";
            observed.event.state = "failed";
            co_return;
        }
        request.timeout_s = static_cast<std::uint32_t>(left);
        auto executor = std::make_shared<execution_identity>();
        backlog::entry what{.key = key,
                            .project = project,
                            .configuration = config.name,
                            .operation = operation,
                            .actor = worlds::execution_key(actor),
                            .trees = {trees},
                            .cacheable = cacheable,
                            .fresh = fresh,
                            .workspace_id = s.snapshot()->server.auth.workspace,
                            .worldline = worldline,
                            .version = version,
                            .executor = executor};
        for (auto& f : snapshot.files)
            what.files.push_back(f.path + '\n' + f.digest.str());
        for (bool retried = false;;) {
            auto held = co_await s.queue->submit(s.core, what,
                                                 [project, config, steps, snapshot, request, worldline, version, actor, operation, executor,
                                                  toolchain](services& zero, jobs::progress_log& progress, backlog::placements trees, std::string) {
                                                     auto placed = request;
                                                     placed.tree = std::move(trees.begin()->second);
                                                     return dispatch(zero, project, config, steps, snapshot, std::move(placed), worldline, version, actor,
                                                                     progress, operation, executor, toolchain);
                                                 });
            observed.event.calculation = key;
            observed.event.reuse = held->reuse();
            // A joined job or a cached result may come from an equal configuration of another name.
            r = std::get<config_result>(co_await follow(s, held, [&note, &config](const jobs::progress& p) {
                auto named = p;
                named.stage = config.name + ": " + p.stage;
                note(progress_note(named));
            }));
            r.dependency_cache = dependency_status;
            r.timings.dependency_seconds = dependency_seconds;
            r.timings.client_sync_seconds = context.client_sync_seconds;
            observed.event.timings = r.timings;
            observed.event.state = r.ok ? "succeeded" : "failed";
            r.configuration = config.name;
            r.snapshot = version;
            auto downloads = r.exports;
            downloads.insert(downloads.end(), r.models.begin(), r.models.end());
            for (const auto& step : r.steps)
                if (step.log)
                    downloads.push_back(*step.log);
            auto granted = co_await s.files.grant_exports(s.core, project, std::move(downloads));
            std::size_t index = 0;
            for (auto& file : r.exports)
                file = std::move(granted.at(index++));
            for (auto& file : r.models)
                file = std::move(granted.at(index++));
            for (auto& step : r.steps)
                if (step.log)
                    *step.log = std::move(granted.at(index++));
            if (std::ranges::any_of(r.models, [](const auto& file) { return !file.delivery_error.empty(); }) ||
                std::ranges::any_of(r.exports, [](const auto& file) { return !file.delivery_error.empty(); }) ||
                std::ranges::any_of(r.steps, [](const auto& step) { return step.log && !step.log->delivery_error.empty(); }))
                co_await s.queue->forget(s.core, key);
            if (!retried && held->reuse() == "cache" &&
                (std::ranges::any_of(r.models, [](const auto& file) { return !file.delivery_error.empty(); }) ||
                 std::ranges::any_of(r.exports, [](const auto& file) { return !file.objects.empty() && !file.delivery_error.empty(); }))) {
                retried = true;
                continue;
            }
            break;
        }
    }

    // Builds run one fixed version of the selected task worldline.
    // Only the build tool's results are cached; `fresh` rebuilds from a clean tree.
    auto execute_all(services& s, mcp::call_context context, std::string project, std::string operation, bool cacheable, bool fresh,
                     std::vector<std::pair<config::effective_config, std::vector<jobs::step>>> jobs, mcp::notifier& note, std::string worldline,
                     std::string version, const worlds::actor& actor) -> coroutine::task<results> {
        results out;
        out.results.resize(jobs.size());
        if (worldline.empty())
            throw tool_error("context_required", "worldline is required");
        auto tree = co_await selected(s, project, worldline, version, actor);
        content::manifest snapshot;
        for (auto& f : lines::manifest(tree))
            snapshot.files.push_back({f.path, f.digest, f.size, f.executable});
        out.worldline = worldline;
        out.revision = tree.version;
        out.snapshot = tree.version;
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            for (std::size_t i = 0; i < jobs.size(); ++i)
                children.spawn(execute(s, context, project, operation, cacheable, fresh, jobs[i].first, jobs[i].second, snapshot, note, out.results[i],
                                       worldline, tree.version, actor));
            co_await children.join();
        };
        co_await coroutine::with_scope(std::move(run_children));
        out.ok = std::ranges::all_of(out.results, &config_result::ok);
        co_return out;
    }

} // namespace tools
