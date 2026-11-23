export module executor;
import std;
import publicrecord;
import native;
import coroutine;
import replica;
import runner;
import protocol;
import host;
import lsp;
import evaluation;
import constants;
import json;
import delivery;
import durability;
import dependencies;
import toolchain;
import channels;
import enumeration;
import codec;
import process;
import allocation;
import utility;

export namespace replica {
    // A call path to the execute role of this Worker when it runs in a process of its own. `hold` keeps the process from
    // being stopped while the caller uses `chan`.
    struct execute_link {
        transport::channel<native::stream_view>& chan;
        std::shared_ptr<void> hold;
    };
    class execute_side {
    public:
        virtual ~execute_side() = default;
        // The execute role's directory, which this process reads.
        virtual auto root() const -> const std::filesystem::path& = 0;
        // Starts the execute role when it is not running.
        virtual auto open() -> coroutine::task<execute_link> = 0;
        virtual auto memory() const -> jobs::worker_memory = 0;
    };

    struct options {
        std::filesystem::path root = "work";
        std::string worker;
        replica::limits limits;
        std::map<std::string, std::string> env;
        bool inherit_env = true;
        builds::toolset tools;
        std::uint32_t parallel = 1;
        std::optional<std::uint64_t> memory_bytes;
        std::optional<std::filesystem::path> dependency_cache;
        std::optional<std::filesystem::path> dependency_upstream;
        std::optional<std::filesystem::path> dependency_publish;
        std::optional<builds::dependency_recipe> dependency_recipe;
        toolchain::identity runtime;
        // Set when the execute role runs elsewhere: this process then answers analyses that run no project code, and
        // forwards everything else.
        execute_side* execute = nullptr;
    };

    class service {
    public:
        using socket = native::socket;
        using dispatcher = transport::dispatcher<socket>;

        service(cores::context<native::scheduler>& rt, options o, execution::thread_pool& pool, std::size_t home = 0)
            : rt_(rt), store_(o.root, o.limits), options_(std::move(o)), pool_(pool), home_(home) {}

        void set_runtime(toolchain::identity identity) { options_.runtime = std::move(identity); }
        auto root() const -> const std::filesystem::path& { return options_.root; }
        // From now on the execute role runs behind `side`; this process reads the trees it prepares.
        void set_execute(execute_side& side) {
            options_.execute = &side;
            execute_view_.emplace(side.root());
        }

        // The owner keeps the I/O runtime alive until all context-owned children are gone.
        auto run(std::size_t core, coroutine::task<> work) -> coroutine::task<> {
            auto token = co_await coroutine::current_token;
            bool stopped = false;
            std::exception_ptr failed;
            try {
                stopped = !(co_await coroutine::under(token, std::move(work)));
            } catch (...) {
                failed = util::capture_current_exception();
            }
            auto finish = [this]() -> coroutine::task<> {
                std::exception_ptr failed;
                while (!contexts_.empty()) {
                    auto key = contexts_.begin()->first;
                    try {
                        co_await retire(key);
                    } catch (...) {
                        if (!failed)
                            failed = util::capture_current_exception();
                    }
                }
                while (!gradle_.empty())
                    co_await retire_gradle(gradle_.begin()->first);
                if (failed)
                    std::rethrow_exception(failed);
            };
            auto cleanup = [&]() -> coroutine::task<> {
                if (core == home_)
                    co_await finish();
                else
                    co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), finish);
            };
            co_await coroutine::under(coroutine::stop_token{}, cleanup());
            if (failed)
                std::rethrow_exception(failed);
            if (stopped)
                co_await coroutine::stop;
        }

        auto reclaim(std::size_t core, bool pressure = false) -> coroutine::task<> {
            if (core == home_)
                co_await collect_sessions(pressure);
            else
                co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), [this, pressure] { return collect_sessions(pressure); });
        }

        auto status(std::size_t core) -> coroutine::task<jobs::slot_report> {
            if (core == home_)
                co_return co_await status_home();
            co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), [this] { return status_home(); });
        }

        template <io::stream S> void install(transport::dispatcher<S>& d, std::size_t core, bool private_execute = false) {
            if (private_execute) {
                auto holds = [this](jobs::tree_holds request, auto&) -> coroutine::task<jobs::authority_ack> {
                    if (request.sequence > remote_holds_.sequence) {
                        options_.memory_bytes = request.memory_bytes;
                        remote_holds_ = std::move(request);
                    }
                    co_return jobs::authority_ack{};
                };
                d.template on<jobs::tree_holds>(std::move(holds));
            }
            auto handle_export_read = [this, core](jobs::export_read request, auto&) -> coroutine::task<jobs::source_bytes> {
                require_consumer();
                auto execute = execute_view_ ? std::optional{execute_view_->root() / "exports"} : std::nullopt;
                co_return co_await io::blocking(rt_.get_scheduler(core), pool_, [this, root = options_.root, execute, request] {
                    std::shared_lock guard{exports_lock_};
                    // What the execute role captured stays in its own directory.
                    if (execute && !std::filesystem::exists(root / "exports" / request.digest.view()))
                        return delivery::read(*execute, request);
                    return delivery::read(root / "exports", request);
                });
            };
            d.template on<jobs::export_read>(std::move(handle_export_read));
            auto handle_sync_offer = [this, core](jobs::sync_offer o, auto&) -> coroutine::task<jobs::sync_plan> {
                require_consumer();
                if (options_.execute)
                    co_return co_await forward(o);
                co_return co_await at_home(core, [o = std::move(o)](store& s) { return s.offer(o); });
            };
            d.template on<jobs::sync_offer>(std::move(handle_sync_offer));
            auto handle_source_bytes = [this, core](jobs::source_bytes p, auto&) -> coroutine::task<jobs::authority_ack> {
                require_consumer();
                if (options_.execute)
                    co_return co_await forward(p);
                if (core == home_)
                    co_return co_await push(std::move(p));
                co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), [this, p = std::move(p)] { return push(p); });
            };
            d.template on<jobs::source_bytes>(std::move(handle_source_bytes));
            auto handle_execution = [this, core]<jobs::execution_request Request>(Request r, auto& ex) -> coroutine::task<jobs::build_report> {
                require_consumer();
                co_await ex.progress({.stage = "checkout"});
                auto inputs = std::make_shared<jobs::analysis_exchange>(rt_.get_scheduler(home_), rt_.get_scheduler(core));
                auto execute = [&](coroutine::scope& children) -> coroutine::task<jobs::build_report> {
                    auto close = jobs::analysis_exchange::lifetime{inputs};
                    children.spawn(relay(ex, inputs));
                    // A build runs on the home core while its request arrived on this one, so every message it reports goes
                    // back to the core that owns the connection.
                    auto report_progress = [this, core, &ex](jobs::progress p) -> coroutine::task<> {
                        if (core == home_) {
                            co_await ex.progress(std::move(p));
                            co_return;
                        }
                        co_await cores::call(rt_.get_scheduler(home_), rt_.get_scheduler(core),
                                             [&ex, p = std::move(p)]() -> coroutine::task<> { co_await ex.progress(p); });
                    };
                    auto sink = progress_sink{report_progress};
                    if (options_.execute) {
                        if (core != home_)
                            throw std::logic_error("a Worker with an execute role of its own runs on one core");
                        co_return co_await front(std::move(r), inputs, sink);
                    }
                    // Only a build runs steps of its own; an analysis reports through its execution channel.
                    if constexpr (std::same_as<Request, jobs::build_request>) {
                        if (core == home_)
                            co_return co_await build(std::move(r), inputs, &sink);
                        co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_),
                                                       [this, r = std::move(r), inputs, &sink] { return build(r, inputs, &sink); });
                    } else {
                        if (core == home_)
                            co_return co_await build(std::move(r), inputs);
                        co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_),
                                                       [this, r = std::move(r), inputs] { return build(r, inputs); });
                    }
                };
                auto report = co_await coroutine::with_scope(std::move(execute));
                // Publish the completed slot before the next stage can be dispatched.
                co_await ex.chan.post(co_await status(core));
                co_return report;
            };
            d.template on<jobs::build_request>(handle_execution);
            d.template on<jobs::analysis_request>(std::move(handle_execution));
            auto handle_dependency_fill = [this, core](jobs::dependency_fill request, auto& ex) -> coroutine::task<jobs::build_report> {
                co_await ex.progress({.stage = "dependencies"});
                if (core == home_)
                    co_return co_await fill(std::move(request));
                co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_),
                                               [this, request = std::move(request)] { return fill(request); });
            };
            d.template on<jobs::dependency_fill>(std::move(handle_dependency_fill));
        }

    private:
        void require_consumer() const {
            if (options_.dependency_publish)
                throw std::invalid_argument("dependency publishers only accept dependency.fill");
        }

        auto fill(jobs::dependency_fill request) -> coroutine::task<jobs::build_report> {
            if (!options_.dependency_publish || !options_.dependency_recipe)
                throw std::invalid_argument("this worker is not a dependency publisher");
            const auto recipe = *options_.dependency_recipe;
            builds::validate(recipe);
            builds::validate(request.manifest);
            if (recipe.triplets.size() > 1 && (request.triplet.empty() || request.host_triplet.empty()))
                throw std::invalid_argument("publisher with multiple allowed triplets requires explicit target and host triplets");
            if (request.triplet.empty())
                request.triplet = recipe.triplets.front();
            if (request.host_triplet.empty())
                request.host_triplet = recipe.triplets.front();
            if (!std::ranges::contains(recipe.triplets, request.triplet) || !std::ranges::contains(recipe.triplets, request.host_triplet))
                throw std::invalid_argument("dependency triplet is not admitted by the publisher");
            if (request.features.size() > constants::dependency_manifest_entries)
                throw std::invalid_argument("too many dependency features");
            for (const auto& feature : request.features)
                builds::dependency_name(feature);
            if (!request.timeout_s)
                throw std::invalid_argument("dependency fill deadline expired");
            const auto text = json::write(request.manifest);
            const auto digest = content::hash_of(text);
            const content::manifest snapshot{{content::entry{.path = "vcpkg.json", .digest = digest, .size = text.size()}}};
            co_await locked([snapshot, digest, text](store& cache) {
                cache.offer({"dependencies", snapshot});
                auto bytes = std::as_bytes(std::span{text});
                cache.put(jobs::source_bytes{{jobs::source_part{digest, 0, text.size(), {bytes.begin(), bytes.end()}}}});
            });
            builds::adapter<"vcpkg">::options options{.executable = recipe.executable,
                                                      .root = recipe.root,
                                                      .install_dir = "installed",
                                                      .triplet = request.triplet,
                                                      .host_triplet = request.host_triplet,
                                                      .features = request.features,
                                                      .buildtrees_dir = "buildtrees",
                                                      .packages_dir = "packages",
                                                      .env = recipe.env,
                                                      .offline = recipe.offline,
                                                      .default_features = request.default_features};
            builds::adapter<"vcpkg">::validate(options);
            const auto key = content::hash_of(json::write(std::tuple{request, recipe}));
            jobs::build_request build_request{.project = "dependencies",
                                              .tree = key.str(),
                                              .snapshot = snapshot.id(),
                                              .steps = builds::adapter<"vcpkg">::expand(options, "install", {}),
                                              .timeout_s = std::min(request.timeout_s, recipe.timeout_s),
                                              .fresh = true};
            build_request.steps.front().dependencies.reset();
            for (const auto& path : recipe.overlay_ports)
                build_request.steps.front().argv.push_back("--overlay-ports=" + path);
            for (const auto& path : recipe.overlay_triplets)
                build_request.steps.front().argv.push_back("--overlay-triplets=" + path);
            co_return co_await build(std::move(build_request));
        }

        // Asks the execute role and answers its questions through `inputs`, relaying its progress to `sink`.
        template <class Req, class Sink> auto forward(const Req& request, jobs::analysis_link inputs, Sink& sink) -> coroutine::task<jobs::reply_t<Req>> {
            auto link = co_await options_.execute->open();
            std::optional<std::uint64_t> budget;
            if (options_.memory_bytes) {
                co_await execute_memory();
                const auto local = local_memory_state();
                if (!local.resident)
                    throw transport::coded_error("worker_overloaded", "front memory accounting is unavailable");
                const auto occupied = std::max(*local.resident, local.baseline.value_or(0) + local.reserved);
                budget = *options_.memory_bytes - std::min(*options_.memory_bytes, occupied);
                if (!*budget)
                    throw transport::coded_error("worker_overloaded", "worker memory budget is in use");
            }
            jobs::tree_holds holds{.sequence = ++holds_sequence_, .memory_bytes = budget};
            for (const auto& [key, value] : preparations_)
                holds.trees.insert(key);
            for (const auto& [key, value] : contexts_)
                holds.trees.insert(key.first);
            for (const auto& [key, value] : tree_locks_)
                if (!value.expired())
                    holds.trees.insert(key);
            auto pinned = co_await link.chan.call(holds);
            if (!pinned)
                throw std::runtime_error("execute role rejected tree retention");
            jobs::progress_log log;
            std::optional<transport::call_result<jobs::reply_t<Req>>> reply;
            bool done = false;
            auto ask = [&]() -> coroutine::task<> {
                struct finish {
                    bool& done;
                    ~finish() { done = true; }
                } finish{done};
                reply.emplace(co_await link.chan.call(request, &log, inputs));
            };
            auto relay_progress = [&]() -> coroutine::task<> {
                std::size_t seen = 0;
                for (;;) {
                    const bool last = done;
                    for (auto& p : log.since(seen))
                        co_await sink(std::move(p));
                    if (last)
                        co_return;
                    co_await execution::schedule_after(rt_.get_scheduler(home_), std::chrono::milliseconds{150});
                }
            };
            co_await execution::when_all(ask(), relay_progress());
            if (!*reply) {
                if (auto* failure = std::get_if<jobs::failure>(&reply->error()))
                    throw transport::coded_error(failure->code, std::move(failure->message));
                transport::raise_call_error(std::move(reply->error()));
            }
            co_return std::move(**reply);
        }

        template <class Req> auto forward(const Req& request) -> coroutine::task<jobs::reply_t<Req>> {
            auto ignore = [](jobs::progress) -> coroutine::task<> { co_return; };
            co_return co_await forward(request, jobs::analysis_link{}, ignore);
        }

        // Forward builds to the execute child. A retained, unchanged preparation can answer while it is stopped.
        template <class Sink> auto front(jobs::build_request r, jobs::analysis_link inputs, Sink& sink) -> coroutine::task<jobs::build_report> {
            const auto started = std::chrono::steady_clock::now();
            const auto key = store::tree_key(r.project, r.tree);
            auto turn = co_await lock_tree(key);
            if (!r.fresh)
                r.fresh = co_await observed([&](const view& trees) { return trees.unfinished_generation(r.project, r.tree); });
            // The stage a preparation serves only names its memory reservation.
            auto same = r;
            same.timeout_s = 0;
            same.prepares = same.prepares ? std::optional<std::string>{std::string{}} : std::nullopt;
            const auto request = content::hash_of(json::write(same)).str();
            const auto dir = trees().tree(key);
            if (auto found = preparations_.find(key); found != preparations_.end()) {
                if (!r.fresh && r.prepares && r.exports.empty() && found->second.snapshot == r.snapshot && found->second.request == request &&
                    std::filesystem::is_directory(dir)) {
                    found->second.used = std::chrono::steady_clock::now();
                    co_return jobs::build_report{.worker = options_.worker,
                                                 .tree = dir.string(),
                                                 .measured = found->second.inputs.outputs,
                                                 .execution_seconds = 0.0,
                                                 .inputs = found->second.inputs};
                }
                preparations_.erase(found);
            }
            if (r.fresh)
                co_await retire_slot(key);
            auto report = co_await forward(r, inputs, sink);
            if (r.prepares && !report.ok())
                co_await coroutine::under(coroutine::stop_token{}, retire_slot(key));
            if (r.prepares && report.ok() && report.inputs) {
                preparation prepared{.snapshot = r.snapshot, .inputs = *report.inputs, .request = request};
                prepared.files = co_await observed([&](const view& trees) { return trees.placed_files(key); });
                // An analysis here reads the tree only when it holds exactly the snapshot's files.
                prepared.forward = content::manifest{prepared.files}.id() != r.snapshot;
                if (!prepared.forward)
                    prepared.loads_code = co_await install_models(r.measure, dir, inputs, prepared.models);
                prepared.used = std::chrono::steady_clock::now();
                preparations_[key] = std::move(prepared);
            }
            report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            co_return report;
        }

        // An analysis that runs project code runs beside the build; other analyses run here.
        template <class Sink> auto front(jobs::analysis_request work, jobs::analysis_link inputs, Sink& sink) -> coroutine::task<jobs::build_report> {
            const auto key = store::tree_key(work.project, work.tree);
            const auto prepared = preparations_.find(key);
            const bool remote = prepared != preparations_.end() && prepared->second.snapshot == work.snapshot && prepared->second.inputs.id() == work.inputs &&
                                (prepared->second.forward || work.analysis.project_code || (work.analysis.model_code && prepared->second.loads_code));
            if (!remote)
                co_return co_await build(std::move(work), inputs);
            auto turn = co_await lock_tree(key);
            // The receipt stays while the execute role answers analyses of its tree.
            struct holding {
                std::map<std::string, int>& active;
                std::string key;
                ~holding() {
                    if (!--active.at(key))
                        active.erase(key);
                }
            } hold{active_, key};
            ++active_[key];
            if (auto found = preparations_.find(key); found != preparations_.end())
                found->second.used = std::chrono::steady_clock::now();
            try {
                co_return co_await forward(work, inputs, sink);
            } catch (const transport::coded_error& error) {
                if (error.code == jobs::why::stale_inputs)
                    preparations_.erase(key);
                throw;
            }
        }

        template <class Exchange> static auto relay(Exchange& execution, jobs::analysis_link inputs) -> coroutine::task<> {
            while (auto item = co_await inputs->next()) {
                auto release = jobs::analysis_exchange::completion{item};
                try {
                    if (auto* unit = std::get_if<language::evaluated_unit>(&item->data)) {
                        co_await execution.commit(std::move(*unit));
                        item->answer(std::monostate{});
                    } else if (auto* request = std::get_if<jobs::reuse_request>(&item->data)) {
                        auto reply = co_await execution.chan.call(*request);
                        if (!reply)
                            transport::raise_call_error(reply.error());
                        item->answer(std::move(*reply));
                    } else if (auto* request = std::get_if<jobs::shard_read>(&item->data)) {
                        auto reply = co_await execution.chan.call(*request);
                        if (!reply)
                            transport::raise_call_error(reply.error());
                        item->answer(std::move(*reply));
                    } else {
                        auto model = std::move(std::get<jobs::model_read>(item->data));
                        model.parent = execution.corr;
                        auto reply = co_await execution.chan.call(model);
                        if (!reply)
                            transport::raise_call_error(reply.error());
                        item->answer(std::move(*reply));
                    }
                } catch (...) {
                    item->fail(util::capture_current_exception());
                }
            }
        }

        // Samples belong to the existing native child lifecycle. A failed sample
        // remains unknown; it is never treated as zero resident memory.
        class process_book {
            std::shared_ptr<io::process_group> group_ = std::make_shared<io::process_group>();
            mutable std::uint64_t high_water_ = 0;

        public:
            auto group() const -> std::shared_ptr<io::process_group> { return group_; }
            auto usage() const -> std::optional<io::process_usage> {
                auto value = native_process::usage(*group_);
                if (value)
                    high_water_ = std::max(high_water_, value->resident_bytes);
                return value;
            }
            auto high_water() const -> std::uint64_t { return high_water_; }
        };
        struct resource_request {
            std::string stage;
            std::uint64_t reserved = 0;
            process_book processes;
            coroutine::stop_source stop;
            bool exceeded = false;
        };
        struct gradle_context {
            process_book processes;
            std::unique_ptr<runner::gradle_session> session;
            std::string identity;
            std::chrono::steady_clock::time_point used;
        };

        struct context {
            std::unique_ptr<language::adapter> server;
            std::string identity;
            delivery::pins models;
            process_book processes;
            std::chrono::steady_clock::time_point used;
            double open_seconds = 0, install_seconds = 0;
            std::map<semantic::unit_key, double> unit_seconds;
            auto rebuild_seconds() const -> double {
                auto total = open_seconds + install_seconds;
                for (const auto& [_, seconds] : unit_seconds)
                    total += seconds;
                return total;
            }
        };

        // What a preparation left in a tree: for each task, the digest of the steps that last ran on the
        // snapshot; the source files and inputs an analysis of it reads; and the model objects those inputs name.
        struct preparation {
            content::hash snapshot;
            std::map<std::string, std::string> tasks;
            std::vector<content::entry> files;
            jobs::analysis_inputs inputs;
            delivery::pins models;
            std::chrono::steady_clock::time_point used;
            // A model the inputs name loads compiler plugins or type providers.
            bool loads_code = false;
            // This process cannot read the tree, and the execute role answers every analysis of it.
            bool forward = false;
            // The build request that made it, without its deadline.
            std::string request;
        };

        static auto model_sources_only(std::span<const jobs::step> steps, std::span<const content::entry> before,
                                       std::span<const content::entry> after) -> bool {
            if (before.size() != after.size() || steps.empty())
                return false;
            static constexpr std::array<std::string_view, 8> suffixes{".kt", ".java", ".scala", ".sc", ".cs", ".fs", ".fsi", ".fsx"};
            for (const auto& [old, file] : std::views::zip(before, after)) {
                if (old.path != file.path || old.executable != file.executable)
                    return false;
                if (old.digest == file.digest)
                    continue;
                if (!std::ranges::any_of(suffixes, [&](auto suffix) { return file.path.ends_with(suffix); }))
                    return false;
                for (const auto& step : steps)
                    if (!step.model_source_dirs || !std::ranges::any_of(*step.model_source_dirs, [&](const auto& dir) {
                            return dir == "." || jobs::worktree_capture::within(file.path, dir);
                        }))
                        return false;
            }
            return true;
        }

        std::uint64_t status_sequence_ = 0;
        std::uint64_t generations_ = 0;

        using session_key = std::pair<std::string, std::string>;
        auto retention(const session_key& key, std::chrono::steady_clock::time_point now) const -> double {
            const auto& value = *contexts_.at(key);
            const auto usage = value.processes.usage();
            if (!usage)
                return -1;
            return allocation::retention_value(value.rebuild_seconds(), usage->resident_bytes, std::chrono::duration<double>(now - value.used).count());
        }
        void order_idle(std::vector<std::pair<std::chrono::steady_clock::time_point, session_key>>& idle) const {
            const auto now = std::chrono::steady_clock::now();
            std::map<session_key, double> values;
            for (const auto& [_, key] : idle)
                values.emplace(key, retention(key, now));
            std::ranges::sort(idle, [&](const auto& a, const auto& b) {
                return std::tuple{values.at(a.second), a.first, a.second} < std::tuple{values.at(b.second), b.first, b.second};
            });
        }

        auto in_use(const session_key& key) const -> bool {
            const auto request = resources_.find(key.first);
            return request != resources_.end() && request->second->stage == key.second;
        }
        auto task_usage(const std::string& slot, const resource_request& request) const -> std::optional<io::process_usage> {
            auto result = request.processes.usage();
            if (!result)
                return {};
            auto include = [&](const process_book& processes) {
                auto child = processes.usage();
                if (!child)
                    return false;
                result->resident_bytes += child->resident_bytes;
                result->cpu_nanoseconds += child->cpu_nanoseconds;
                result->threads += child->threads;
                result->processes += child->processes;
                return true;
            };
            if (const auto context = contexts_.find({slot, request.stage}); context != contexts_.end() && !include(context->second->processes))
                return {};
            if (const auto session = gradle_.find(slot); session != gradle_.end() && !include(session->second->processes))
                return {};
            return result;
        }
        auto execute_memory() -> coroutine::task<jobs::worker_memory> {
            const auto until = std::chrono::steady_clock::now() + constants::execute_memory_wait;
            auto& io = rt_.get_scheduler(home_);
            for (;;) {
                auto memory = options_.execute->memory();
                if (memory.resident)
                    co_return memory;
                if (std::chrono::steady_clock::now() >= until)
                    throw transport::coded_error("worker_overloaded", "execute memory accounting did not become available");
                co_await execution::schedule_after(io, std::chrono::milliseconds{100});
            }
        }

        auto local_memory_state() const -> jobs::worker_memory {
            jobs::worker_memory out{.limit = options_.memory_bytes};
            if (auto self = native_process::self_usage()) {
                out.baseline = self->resident_bytes;
                out.resident = self->resident_bytes;
            }
            for (const auto& [key, request] : resources_) {
                out.reserved += request->reserved;
                const auto child = request->processes.usage();
                if (out.resident && child)
                    *out.resident += child->resident_bytes;
                else
                    out.resident.reset();
            }
            for (const auto& [key, context] : contexts_) {
                const auto child = context->processes.usage();
                if (out.resident && child)
                    *out.resident += child->resident_bytes;
                else
                    out.resident.reset();
            }
            for (const auto& [slot, context] : gradle_) {
                const auto child = context->processes.usage();
                if (out.resident && child)
                    *out.resident += child->resident_bytes;
                else
                    out.resident.reset();
            }
            return out;
        }
        auto memory_state() const -> jobs::worker_memory {
            auto out = local_memory_state();
            if (options_.execute) {
                const auto child = options_.execute->memory();
                out.reserved += child.reserved;
                if (out.resident && child.resident)
                    *out.resident += *child.resident;
                else
                    out.resident.reset();
            }
            return out;
        }
        auto reserve_memory(const std::string& slot, std::string stage, std::optional<std::uint64_t> requested)
            -> coroutine::task<std::shared_ptr<resource_request>> {
            auto lock = co_await memory_lock_.lock();
            auto request = std::make_shared<resource_request>();
            request->stage = std::move(stage);
            if (options_.memory_bytes) {
                const auto self = native_process::self_usage();
                if (!self)
                    throw transport::coded_error("resource_limit", "resource accounting is unavailable");
                auto occupied = self->resident_bytes;
                if (options_.execute) {
                    const auto child = co_await execute_memory();
                    occupied += std::max(*child.resident, child.baseline.value_or(0) + child.reserved);
                }
                const auto capacity = *options_.memory_bytes - std::min(*options_.memory_bytes, occupied);
                request->reserved = requested.value_or(capacity);
                std::uint64_t reserved = 0;
                for (const auto& [key, active] : resources_)
                    reserved += active->reserved;
                if (!request->reserved || request->reserved > capacity)
                    throw transport::coded_error("resource_limit", "request exceeds the worker memory budget");
                if (reserved > capacity || request->reserved > capacity - reserved)
                    throw transport::coded_error("worker_overloaded", "worker memory budget is in use");
                std::vector<std::pair<std::chrono::steady_clock::time_point, session_key>> idle;
                for (const auto& [key, value] : contexts_)
                    if (!in_use(key) && key != session_key{slot, request->stage})
                        idle.emplace_back(value->used, key);
                order_idle(idle);
                std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> idle_gradle;
                for (const auto& [key, context] : gradle_)
                    if (key != slot && !resources_.contains(key))
                        idle_gradle.emplace_back(context->used, key);
                std::ranges::sort(idle_gradle);
                auto room = [&] {
                    std::uint64_t retained = 0;
                    for (const auto& [key, value] : contexts_) {
                        if (in_use(key) || key == session_key{slot, request->stage})
                            continue;
                        const auto usage = value->processes.usage();
                        if (!usage)
                            return false;
                        retained += usage->resident_bytes;
                    }
                    for (const auto& [key, context] : gradle_) {
                        if (key == slot || resources_.contains(key))
                            continue;
                        const auto usage = context->processes.usage();
                        if (!usage)
                            return false;
                        retained += usage->resident_bytes;
                    }
                    return retained <= capacity - reserved - request->reserved;
                };
                for (const auto& [used, key] : idle_gradle) {
                    if (room())
                        break;
                    co_await retire_gradle(key);
                }
                for (const auto& [used, key] : idle) {
                    if (room())
                        break;
                    co_await retire(key);
                }
                if (!room())
                    throw transport::coded_error("worker_overloaded", "worker memory budget is in use");
            } else
                request->reserved = requested.value_or(0);
            resources_.emplace(slot, request);
            co_return request;
        }

        auto retire(const session_key& key) -> coroutine::task<> {
            auto found = contexts_.find(key);
            if (found == contexts_.end())
                co_return;
            auto old = std::move(found->second);
            contexts_.erase(found);
            std::exception_ptr failed;
            try {
                co_await old->server->close();
            } catch (...) {
                failed = util::capture_current_exception();
            }
            old->server.reset();
            if (failed)
                std::rethrow_exception(failed);
        }

        auto retire_gradle(const std::string& slot) -> coroutine::task<> {
            auto found = gradle_.find(slot);
            if (found == gradle_.end())
                co_return;
            auto old = std::move(found->second);
            gradle_.erase(found);
            co_await old->session->close();
        }

        // When the execute child owns the tree, its builds replace the preparation receipt.
        auto retire_slot(const std::string& slot) -> coroutine::task<> {
            if (!options_.execute)
                preparations_.erase(slot);
            co_await retire_gradle(slot);
            std::vector<session_key> sessions;
            for (const auto& [key, _] : contexts_)
                if (key.first == slot)
                    sessions.push_back(key);
            std::exception_ptr failed;
            for (const auto& key : sessions) {
                try {
                    co_await retire(key);
                } catch (...) {
                    if (!failed)
                        failed = util::capture_current_exception();
                }
            }
            if (failed)
                std::rethrow_exception(failed);
        }

        auto status_home() -> coroutine::task<jobs::slot_report> {
            jobs::slot_report out;
            out.slots = co_await observed([analysis_store = view{store_.root()}, toolchain = options_.runtime.id](const view& trees) {
                auto slots = trees.slots();
                for (auto& slot : slots) {
                    if (slot.snapshot.empty())
                        continue;
                    const auto key = store::tree_key(slot.project, slot.tree);
                    auto scan = [&](const std::filesystem::path& cache) {
                        std::error_code error;
                        for (const auto& stage : std::filesystem::directory_iterator(cache, error)) {
                            if (slot.cached.size() >= constants::language_sessions)
                                break;
                            std::ifstream input{stage.path() / ".fughetta-warm"};
                            std::string line;
                            if (!std::getline(input, line))
                                continue;
                            auto marker = json::read<jobs::cached_session>(line);
                            if (marker && marker->snapshot == slot.snapshot && marker->toolchain == toolchain)
                                slot.cached.push_back(std::move(*marker));
                        }
                    };
                    scan(analysis_store.analysis(key));
                    if (analysis_store.root() != trees.root())
                        scan(trees.analysis(key));
                }
                return slots;
            });
            for (auto& slot : out.slots) {
                const auto key = store::tree_key(slot.project, slot.tree);
                slot.busy = active_.contains(key);
                for (const auto& [identity, context] : contexts_)
                    if (identity.first == key) {
                        const auto& state = context->server->state();
                        if (state.phase == "ready")
                            slot.sessions.push_back({identity.second, state.provider, state.options, state.snapshot, state.epoch, context->processes.usage(),
                                                     context->rebuild_seconds(), context->processes.high_water()});
                    }
            }
            out.sequence = ++status_sequence_;
            out.memory = memory_state();
            co_return out;
        }

        auto collect_sessions(bool pressure = false) -> coroutine::task<> {
            const auto collection_time = std::chrono::steady_clock::now();
            if (collection_time >= export_collect_) {
                export_collect_ = collection_time + constants::export_collect_interval;
                std::erase_if(live_shards_, [](const auto& value) { return value.second.expired(); });
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [this, root = options_.root] {
                    std::unique_lock guard{exports_lock_};
                    for (const auto& error :
                         delivery::collect(root / "exports", constants::export_retention, constants::export_cache_bytes, {}, model_retention_.roots()))
                        std::println(std::cerr, "export collection: {}", error);
                });
            }
            auto memory_lock = co_await memory_lock_.lock();
            std::vector<std::pair<std::chrono::steady_clock::time_point, session_key>> idle;
            for (const auto& [key, value] : contexts_)
                if (!in_use(key))
                    idle.emplace_back(value->used, key);
            order_idle(idle);
            auto now = std::chrono::steady_clock::now();
            for (const auto& [used, key] : idle) {
                auto found = contexts_.find(key);
                if (found != contexts_.end() && found->second->used == used && !in_use(key) &&
                    (pressure || (options_.memory_bytes && memory_state().resident.value_or(*options_.memory_bytes) > *options_.memory_bytes) ||
                     now - used >= constants::language_idle || contexts_.size() >= constants::language_sessions))
                    co_await retire(key);
            }
            // The same heartbeat reclaim pass enforces observed group RSS. Stops
            // flow through the regular cancellation/retirement path.
            for (const auto& [slot, request] : resources_) {
                const auto actual = task_usage(slot, *request);
                if (request->reserved && actual && actual->resident_bytes > request->reserved) {
                    request->exceeded = true;
                    request->stop.request_stop();
                }
            }
            const auto state = memory_state();
            std::vector<std::string> idle_gradle;
            for (const auto& [slot, session] : gradle_)
                if (!tree_locks_.contains(slot) && (pressure || !session->session->alive() || now - session->used >= constants::language_idle ||
                    (state.limit && state.resident.value_or(*state.limit) > *state.limit)))
                    idle_gradle.push_back(slot);
            for (const auto& slot : idle_gradle)
                if (!tree_locks_.contains(slot))
                    co_await retire_gradle(slot);
            const auto after_reclaim = idle_gradle.empty() ? state : memory_state();
            if (after_reclaim.limit && after_reclaim.resident && *after_reclaim.resident > *after_reclaim.limit) {
                std::shared_ptr<resource_request> largest;
                std::uint64_t bytes = 0;
                for (const auto& [slot, request] : resources_) {
                    const auto actual = task_usage(slot, *request);
                    if (!request->exceeded && actual && (!largest || actual->resident_bytes > bytes)) {
                        largest = request;
                        bytes = actual->resident_bytes;
                    }
                }
                if (largest) {
                    largest->exceeded = true;
                    largest->stop.request_stop();
                }
            }
            std::erase_if(preparations_, [&](const auto& item) {
                return (pressure || now - item.second.used >= constants::language_idle) && !active_.contains(item.first) &&
                       std::ranges::none_of(contexts_, [&](const auto& context) { return context.first.first == item.first; });
            });
        }

        template <class F> struct home_work {
            service* owner;
            F work;
            auto operator()() -> coroutine::task<std::invoke_result_t<F&, store&>> { return owner->locked(std::move(work)); }
        };
        template <class F> auto at_home(std::size_t core, F f) -> coroutine::task<std::invoke_result_t<F&, store&>> {
            if (core == home_)
                co_return co_await locked(std::move(f));
            co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), home_work<F>{this, std::move(f)});
        }
        template <class F> auto locked(F f) -> coroutine::task<std::invoke_result_t<F&, store&>> {
            auto held = co_await store_lock_.lock();
            co_return co_await io::blocking(rt_.get_scheduler(home_), pool_, [this, f = std::move(f)]() mutable { return f(store_); });
        }
        // The trees builds write: this process's own, or the execute role's, which this one only reads.
        auto trees() const -> const view& { return execute_view_ ? *execute_view_ : store_; }
        template <class F> auto observed(F f) -> coroutine::task<std::invoke_result_t<F&, const view&>> {
            if (!execute_view_)
                co_return co_await locked(std::move(f));
            co_return co_await io::blocking(rt_.get_scheduler(home_), pool_, [this, f = std::move(f)]() mutable { return f(*execute_view_); });
        }

        struct tree_turn {
            std::unordered_map<std::string, std::weak_ptr<coroutine::mutex>>& locks;
            std::string key;
            std::shared_ptr<coroutine::mutex> lock;
            std::optional<coroutine::mutex::guard> turn;
            ~tree_turn() {
                turn.reset();
                if (lock.use_count() == 1)
                    locks.erase(key);
            }
        };
        auto tree_lease(const std::string& key) -> std::unique_ptr<tree_turn> {
            auto lock = tree_locks_[key].lock();
            if (!lock) {
                lock = std::make_shared<coroutine::mutex>();
                tree_locks_[key] = lock;
            }
            return std::make_unique<tree_turn>(tree_locks_, key, std::move(lock));
        }

        // Builds and analyses of one tree take turns.
        auto lock_tree(const std::string& key) -> coroutine::task<std::unique_ptr<tree_turn>> {
            auto held = tree_lease(key);
            held->turn.emplace(co_await held->lock->lock());
            co_return held;
        }

        auto seed_parent(const jobs::build_request& request, const std::string& key, const content::hash& recipe)
            -> coroutine::task<std::unique_ptr<tree_turn>> {
            if (!request.parent_snapshot || request.fresh || recipe.empty())
                co_return nullptr;
            const auto parents = co_await locked([project = request.project, pool = key.substr(0, key.rfind('-') + 1),
                                                  snapshot = *request.parent_snapshot, recipe](store& s) {
                return s.warm_parents(project, pool, snapshot, recipe);
            });
            for (const auto& parent : parents) {
                if (parent == key || tree_locks_.contains(parent) || remote_holds_.trees.contains(parent))
                    continue;
                auto held = tree_lease(parent);
                if (auto turn = held->lock->try_lock()) {
                    held->turn.emplace(std::move(*turn));
                    co_return held;
                }
            }
            co_return nullptr;
        }

        auto push(jobs::source_bytes p) -> coroutine::task<jobs::authority_ack> {
            co_await collect_sessions();
            std::set<std::string> busy = remote_holds_.trees;
            for (const auto& [key, _] : tree_locks_)
                busy.insert(key);
            for (const auto& [key, value] : contexts_)
                busy.insert(key.first);
            for (const auto& [key, value] : gradle_)
                busy.insert(key);
            for (auto& [key, n] : active_)
                if (n > 0)
                    busy.insert(key);
            co_return co_await locked([p = std::move(p), busy = std::move(busy)](store& s) {
                auto ack = s.put(p);
                s.collect(busy);
                return ack;
            });
        }

        auto environment(const std::map<std::string, std::string>& build, const std::map<std::string, std::string>& step) const
            -> std::map<std::string, std::string> {
            auto env = options_.inherit_env ? host::environment() : std::map<std::string, std::string>{};
            for (auto& [k, v] : options_.env)
                env[k] = v;
            for (auto& [k, v] : build)
                env[k] = v;
            for (auto& [k, v] : step)
                env[k] = v;
            env["OMP_NUM_THREADS"] = std::to_string(options_.parallel);
            return env;
        }

        auto analysis_environment(const std::map<std::string, std::string>& step) const -> std::map<std::string, std::string> {
            std::map<std::string, std::string> env;
            if (options_.inherit_env) {
                const auto inherited = host::environment();
                for (const auto name : constants::analysis_environment)
                    if (const auto found = inherited.find(std::string{name}); found != inherited.end())
                        env.insert(*found);
            }
            for (const auto& [name, value] : options_.env)
                env[name] = value;
            for (const auto& [name, value] : step)
                env[name] = value;
            env["OMP_NUM_THREADS"] = std::to_string(options_.parallel);
            return env;
        }

        void allocate_runtime(std::map<std::string, std::string>& env, bool java_environment = true) const {
            const auto jobs = std::to_string(options_.parallel);
            if (java_environment) {
                auto& java = env["JAVA_TOOL_OPTIONS"];
                if (!java.empty())
                    java += ' ';
                java += "-XX:ActiveProcessorCount=" + jobs;
            }
            env["DOTNET_PROCESSOR_COUNT"] = jobs;
        }

        auto gradle_installation(const builds::step& step, const std::string& launcher, const std::optional<std::uint32_t>& cpu,
                                 std::uint64_t output_limit) const -> std::filesystem::path {
            if (!step.gradle || cpu || step.standard_output || output_limit > constants::semantic_object_bytes / 8 ||
                !options_.tools.contains("java") || !options_.tools.contains("fughetta-jvm"))
                return {};
            std::error_code error;
            const auto executable = std::filesystem::canonical(launcher, error);
            const auto bin = executable.parent_path();
            if (error || bin.filename() != "bin" || (executable.filename() != "gradle" && executable.filename() != "gradle.bat"))
                return {};
            return bin.parent_path();
        }

        auto step_recipes(const jobs::build_request& request, const toolchain::bindings& bindings, const std::string& key) const
            -> std::vector<content::hash> {
            const auto dir = trees().tree(key);
            const auto reports = trees().reports(key);
            const auto cache = options_.dependency_cache.value_or(options_.root / (options_.dependency_publish ? "publisher/dependencies" : "dependencies"));
            auto base = environment(request.env, {});
            const auto parent = host::environment();
            std::map<std::string, std::string> digests;
            auto observe = [&](std::string path, const builds::environment& env) -> std::pair<std::string, std::string> {
                if (!builds::absolute_path(path)) {
                    const auto search = env.find("PATH");
                    const auto inherited = parent.find("PATH");
                    if (search == env.end() || inherited == parent.end() || search->second != inherited->second)
                        return {};
                    const auto found = host::resolve_tools({}, {path});
                    if (found.empty())
                        return {};
                    path = found.begin()->second;
                }
                const auto name = toolchain::tool_path(path);
                if (const auto relative = std::filesystem::path{name}.lexically_relative(std::filesystem::absolute(dir));
                    !relative.empty() && *relative.begin() != "..")
                    return {};
                const auto [found, inserted] = digests.try_emplace(name);
                if (inserted) {
                    found->second = toolchain::tool_digest(options_.runtime, name);
                    if (found->second.empty())
                        try {
                            found->second = toolchain::digest(name);
                        } catch (const std::runtime_error&) {
                        }
                }
                return {name, found->second};
            };
            std::map<std::string, std::string> sdk_objects;
            for (const auto& sdk : bindings.sdk_inputs)
                sdk_objects.emplace(sdk.name, sdk.object);
            std::vector<content::hash> recipes;
            for (const auto& step : request.steps) {
                builds::toolset used;
                const auto local = builds::locate(step, dir, bindings.tools, options_.parallel, reports, cache,
                                                 options_.dependency_publish.value_or(options_.dependency_upstream.value_or(std::filesystem::path{})),
                                                 bindings.sdks, &used);
                auto env = base;
                auto launch_env = base;
                for (const auto& [name, value] : step.env)
                    env[name] = value;
                for (const auto& [name, value] : local.env)
                    launch_env[name] = value;
                for (const auto& [name, value] : bindings.env) {
                    env[name] = value;
                    launch_env[name] = value;
                }
                allocate_runtime(env, !step.gradle);
                auto& recipe = recipes.emplace_back();
                if (local.argv.empty())
                    continue;
                std::vector<std::pair<std::string, std::string>> tools{observe(local.argv.front(), launch_env)};
                for (const auto& [_, path] : used)
                    tools.push_back(observe(path, launch_env));
                if (!gradle_installation(step, local.argv.front(), request.max_cpu_seconds, request.output_limit).empty())
                    for (const auto name : {"java", "fughetta-jvm"})
                        tools.push_back(observe(options_.tools.at(name), launch_env));
                if (std::ranges::any_of(tools, [](const auto& tool) { return tool.second.empty(); }))
                    continue;
                recipe = content::hash_of(json::write(std::tie(step, env, tools, sdk_objects, options_.runtime.platform)));
            }
            return recipes;
        }

        // Where a running step says which step it is on and how that step ended; the caller routes each message to the
        // core that owns its connection.
        using progress_sink = util::function_ref<coroutine::task<>(jobs::progress)>;

        auto build(jobs::build_request r, jobs::analysis_link messages = {}, progress_sink* sink = nullptr) -> coroutine::task<jobs::build_report> {
            if (r.capture_worktree_changes) {
                if (r.prepares || !r.measure.empty() || r.capture_worktree_changes->prefixes.empty())
                    throw std::invalid_argument("worktree capture requires explicit paths and a standalone build");
                for (const auto& prefix : r.capture_worktree_changes->prefixes)
                    content::checked_path(prefix);
                // Changes must come from these steps, including newly created files.
                r.fresh = true;
                r.relocatable_build_dirs.clear();
                r.parent_snapshot.reset();
            }
            co_await collect_sessions();
            auto started = std::chrono::steady_clock::now();
            auto key = store::tree_key(r.project, r.tree);
            auto turn = co_await lock_tree(key);
            struct release_resources {
                service* owner;
                std::string key;
                ~release_resources() { owner->resources_.erase(key); }
            } resource_lease{this, key};
            auto resource = co_await reserve_memory(key, r.prepares.value_or(std::string{}), r.max_memory_bytes);
            struct holding {
                std::map<std::string, int>& active;
                std::string key;
                ~holding() {
                    if (!--active.at(key))
                        active.erase(key);
                }
            } hold{active_, key};
            ++active_[key];
            if (!r.fresh)
                r.fresh = co_await locked([&](store& s) { return s.unfinished_generation(r.project, r.tree); });
            const auto bindings = toolchain::bind(options_.runtime, r.resolved_toolchain, options_.tools);
            std::vector<content::hash> steps;
            if (!r.relocatable_build_dirs.empty() || !r.preparation_tasks.empty())
                steps = co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] { return step_recipes(r, bindings, key); });
            content::hash recipe;
            if (!r.relocatable_build_dirs.empty() && std::ranges::none_of(steps, &content::hash::empty))
                recipe = content::hash_of(json::write(std::tie(steps, r.relocatable_build_dirs, r.generation_producers)));
            // Move the receipt out of the live directory before checkout so
            // cancellation and intervening builds invalidate it before execution.
            preparation prepared;
            std::optional<preparation> previous;
            if (auto found = preparations_.find(key); found != preparations_.end()) {
                if (!r.fresh && r.prepares)
                    previous = std::move(found->second);
                preparations_.erase(found);
            }
            prepared.snapshot = r.snapshot;
            const bool same_snapshot = previous && previous->snapshot == r.snapshot;
            if (same_snapshot) {
                prepared.tasks = std::move(previous->tasks);
                prepared.inputs.generation = previous->inputs.generation;
            }
            // A task is the same work when its steps and their environment are.
            std::vector<std::pair<std::string, std::string>> completing;
            if (!r.preparation_tasks.empty()) {
                if (!r.prepares)
                    throw std::invalid_argument("preparation tasks belong to a build that prepares an analysis");
                bool source_edit = false;
                if (!same_snapshot && previous && r.exports.empty()) {
                    if (r.body_only) {
                        source_edit = co_await locked([&](store& s) {
                            if (!std::filesystem::is_directory(s.tree(key)))
                                return false;
                            const auto next = s.files(r.project, r.snapshot);
                            if (previous->files.size() != next.size())
                                return false;
                            for (const auto& [old, file] : std::views::zip(previous->files, next))
                                if (old.path != file.path || old.executable != file.executable)
                                    return false;
                            return true;
                        });
                    }
                    if (!source_edit && std::ranges::all_of(r.steps, [](const auto& step) { return step.model_source_dirs.has_value(); })) {
                        source_edit = co_await locked([&](store& s) {
                            return std::filesystem::is_directory(s.tree(key)) &&
                                   model_sources_only(r.steps, previous->files, s.files(r.project, r.snapshot));
                        });
                    }
                }
                const auto& retained = source_edit ? previous->tasks : prepared.tasks;
                std::vector<jobs::step> pending;
                std::size_t covered = 0;
                for (const auto& task : r.preparation_tasks) {
                    if (task.first_step != covered || task.step_count == 0 || task.step_count > r.steps.size() - covered)
                        throw std::invalid_argument("invalid preparation task steps");
                    const std::span commands{r.steps.data() + covered, task.step_count};
                    const std::span recipes{steps.data() + covered, task.step_count};
                    covered += task.step_count;
                    if (std::ranges::any_of(recipes, &content::hash::empty)) {
                        prepared.tasks.erase(task.name);
                        pending.insert(pending.end(), commands.begin(), commands.end());
                        continue;
                    }
                    completing.emplace_back(task.name, content::hash_of(json::write(recipes)).str());
                    if (const auto ran = retained.find(task.name); ran == retained.end() || ran->second != completing.back().second)
                        pending.insert(pending.end(), commands.begin(), commands.end());
                }
                if (covered != r.steps.size())
                    throw std::invalid_argument("preparation tasks do not cover the requested steps");
                // Across snapshots, use all of the requested model preparation or run it again.
                if (!source_edit || pending.empty())
                    r.steps = std::move(pending);
            }
            if (r.fresh)
                co_await retire_slot(key);
            bool preparation = !r.measure.empty() || r.prepares;
            const bool retain_files = preparation || r.capture_worktree_changes.has_value();
            const bool ran_steps = !r.steps.empty();
            auto parent = co_await seed_parent(r, key, recipe);
            // A preparation keeps the snapshot's files, which the offer cache may let go while the steps run.
            auto [dir, reports, files, retired] = co_await locked(
                [project = r.project, snapshot = r.snapshot, tree = r.tree, key, retain_files, fresh = r.fresh, producers = r.generation_producers,
                 seed = parent ? parent->key : std::string{}, parent_snapshot = r.parent_snapshot, recipe, dirs = r.relocatable_build_dirs](store& s) {
                    auto retired = fresh ? s.detach(project, tree) : std::filesystem::path{};
                    // A build may have changed the candidate before we acquired its tree lease.
                    const auto ready = !seed.empty() && parent_snapshot && s.warm_build_time(seed, *parent_snapshot, recipe);
                    auto dir = s.checkout(project, snapshot, tree, ready ? std::string_view{seed} : std::string_view{}, dirs);
                    s.reconcile_generations(project, tree, producers);
                    return std::tuple{dir, s.reports(key), retain_files ? s.files(project, snapshot) : std::vector<content::entry>{}, retired};
                });
            parent.reset();
            if (!retired.empty())
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [retired] { store::discard(retired); });
            auto deadline = started + std::chrono::seconds{r.timeout_s};
            auto parent_token = co_await coroutine::current_token;
            struct forward_stop {
                coroutine::stop_source* source;
                void operator()() const noexcept { source->request_stop(); }
            };
            std::optional<execution::stop_callback_for_t<coroutine::stop_token, forward_stop>> parent_stop;
            if (parent_token.stop_possible())
                parent_stop.emplace(parent_token, forward_stop{&resource->stop});
            auto token = resource->stop.get_token();
            std::optional<jobs::build_report> report;
            std::exception_ptr failed;
            delivery::pins model_pins;
            try {
                report = co_await coroutine::under(token, run_steps(r, dir, reports, deadline, r.prepares.has_value(), messages, model_pins, bindings, sink));
                if (report && report->ok() && preparation) {
                    auto remaining = std::max(std::chrono::steady_clock::duration::zero(), deadline - std::chrono::steady_clock::now());
                    prepared.loads_code = co_await install_models(r.measure, dir, messages, model_pins);
                    const auto measuring = std::chrono::steady_clock::now();
                    auto finished = co_await coroutine::under(token, coroutine::deadline(rt_.get_scheduler(home_), remaining, measure(r, dir, files, *report)));
                    *report->execution_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - measuring).count();
                    if (!finished)
                        report.reset();
                    else if (!*finished)
                        report->steps.push_back({.name = "analysis", .timed_out = true, .output = "time budget exhausted while measuring build outputs"});
                }
            } catch (...) {
                failed = util::capture_current_exception();
            }
            auto discard_captured_tree = [&]() -> coroutine::task<> {
                co_await retire_gradle(key);
                auto discarded = co_await locked([&](store& s) { return s.detach(r.project, r.tree); });
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [discarded] { store::discard(discarded); });
            };
            if (r.capture_worktree_changes && (resource->exceeded || failed || !report))
                co_await coroutine::under(coroutine::stop_token{}, discard_captured_tree());
            if (r.prepares && (resource->exceeded || failed || !report || !report->ok()))
                co_await coroutine::under(coroutine::stop_token{}, retire_slot(key));
            if (resource->exceeded) {
                jobs::build_report limited{.worker = options_.worker, .tree = dir.string()};
                limited.analysis_error = "worker resident memory budget exhausted";
                limited.analysis_error_code = "resource_limit";
                co_return limited;
            }
            if (failed)
                std::rethrow_exception(failed);
            if (!report)
                co_await coroutine::stop;
            if (!r.exports.empty()) {
                try {
                    auto captured = co_await io::blocking(rt_.get_scheduler(home_), pool_,
                                                          [this, root = options_.root, dir, exports = r.exports, allow_missing = !report->ok()] {
                                                              std::shared_lock guard{exports_lock_};
                                                              std::vector<std::string> skipped;
                                                              auto files = delivery::capture(dir, exports, root / "exports", allow_missing, &skipped);
                                                              return std::pair{std::move(files), std::move(skipped)};
                                                          });
                    std::ranges::move(captured.first, std::back_inserter(report->exports));
                    report->export_skipped = std::move(captured.second);
                } catch (const std::exception& error) {
                    report->export_error = std::string{error.what()}.substr(0, 512);
                }
            }
            if (r.capture_worktree_changes) {
                if (report->ok()) {
                    try {
                        report->worktree_changes = co_await locked([&](store& s) { return s.capture_worktree(dir, files, *r.capture_worktree_changes); });
                    } catch (const std::exception& error) {
                        report->worktree_capture_error = std::string{error.what()}.substr(0, 512);
                    }
                }
                co_await coroutine::under(coroutine::stop_token{}, discard_captured_tree());
            }
            if (r.prepares && report->ok()) {
                if (ran_steps || !prepared.inputs.generation)
                    prepared.inputs.generation = ++generations_;
                report->inputs = jobs::analysis_inputs{r.snapshot, report->measured, prepared.inputs.generation};
                for (auto& [task, digest] : completing)
                    prepared.tasks.insert_or_assign(task, std::move(digest));
                prepared.inputs = *report->inputs;
                prepared.files = std::move(files);
                prepared.used = std::chrono::steady_clock::now();
                prepared.models = std::move(model_pins);
                preparations_[key] = std::move(prepared);
            }
            if (report->ok() && !recipe.empty()) {
                try {
                    co_await locked([key, snapshot = r.snapshot, recipe](store& s) { s.remember_warm_build(key, snapshot, recipe); });
                } catch (const std::system_error& error) {
                    logging::warn(std::format("worker: could not retain build tree {}: {}", key, error.what()));
                }
            }
            report->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            co_return std::move(*report);
        }

        // Analyzes the tree a preparation left. The request names the inputs it reads; the tree is not written.
        auto build(jobs::analysis_request work, jobs::analysis_link messages = {}) -> coroutine::task<jobs::build_report> {
            auto* analysis = &work.analysis;
            // What the analysis session inherits from its preparation.
            const jobs::build_request r{.project = work.project,
                                        .tree = work.tree,
                                        .snapshot = work.snapshot,
                                        .timeout_s = work.timeout_s,
                                        .env = work.env,
                                        .max_memory_bytes = work.max_memory_bytes,
                                        .max_cpu_seconds = work.max_cpu_seconds};
            co_await collect_sessions();
            auto started = std::chrono::steady_clock::now();
            auto key = store::tree_key(r.project, r.tree);
            if (analysis->slot_id != key)
                throw std::invalid_argument("analysis request belongs to another slot");
            auto turn = co_await lock_tree(key);
            struct release_resources {
                service* owner;
                std::string key;
                ~release_resources() { owner->resources_.erase(key); }
            } resource_lease{this, key};
            auto resource = co_await reserve_memory(key, analysis->execution.stage_id, r.max_memory_bytes);
            struct holding {
                std::map<std::string, int>& active;
                std::string key;
                ~holding() {
                    if (!--active.at(key))
                        active.erase(key);
                }
            } hold{active_, key};
            ++active_[key];
            // Any build of the tree since the preparation discarded its receipt.
            const auto prepared = preparations_.find(key);
            if (prepared == preparations_.end() || prepared->second.snapshot != r.snapshot || prepared->second.inputs.id() != work.inputs)
                throw transport::coded_error(jobs::why::stale_inputs, "the tree no longer holds the inputs this analysis names");
            prepared->second.used = std::chrono::steady_clock::now();
            jobs::build_report report{.worker = options_.worker, .measured = prepared->second.inputs.outputs, .preparation_seconds = 0, .execution_seconds = 0};
            auto model_pins = prepared->second.models;
            const auto files = prepared->second.files;
            const auto loads_code = prepared->second.loads_code;
            // A preparation that finished while this analysis waited for the tree may hand it to the execute role.
            if (options_.execute && (prepared->second.forward || analysis->project_code || (analysis->model_code && loads_code)))
                throw transport::coded_error(jobs::why::stale_inputs, "the execute role answers this analysis");
            const auto dir = trees().tree(key);
            report.tree = dir.string();
            report.analysis_execution = analysis->execution;
            auto parent_token = co_await coroutine::current_token;
            struct forward_stop {
                coroutine::stop_source* source;
                void operator()() const noexcept { source->request_stop(); }
            };
            std::optional<execution::stop_callback_for_t<coroutine::stop_token, forward_stop>> parent_stop;
            if (parent_token.stop_possible())
                parent_stop.emplace(parent_token, forward_stop{&resource->stop});
            auto token = resource->stop.get_token();
            auto milliseconds = std::min(std::uint64_t{r.timeout_s}, std::uint64_t{analysis->seconds}) * 1000;
            bool budget_limited = false;
            if (analysis->budget.milliseconds) {
                budget_limited = *analysis->budget.milliseconds <= milliseconds;
                milliseconds = std::min(milliseconds, *analysis->budget.milliseconds);
            }
            report.missing.push_back({.scope = analysis->demand, .reason = "semantic time budget exhausted", .deferred = true});
            std::exception_ptr failed;
            bool stopped = false, expired = false;
            try {
                auto finished =
                    co_await coroutine::under(token, coroutine::deadline(rt_.get_scheduler(home_), std::chrono::milliseconds{milliseconds},
                                                                         analyze(r, *analysis, dir, files, report, messages, model_pins, loads_code)));
                stopped = !finished;
                if (finished && !*finished) {
                    expired = true;
                    report.analysis_session.reset();
                    if (budget_limited) {
                        report.analyzed = true;
                        if (report.missing.empty())
                            report.missing.push_back({.scope = analysis->demand, .reason = "semantic time budget exhausted", .deferred = true});
                    } else {
                        report.analyzed = false;
                        report.steps.push_back({.name = "analysis", .timed_out = true, .output = "time budget exhausted during artifact or language analysis"});
                    }
                }
            } catch (...) {
                failed = util::capture_current_exception();
            }
            if (resource->exceeded || failed || stopped || expired)
                co_await coroutine::under(coroutine::stop_token{}, retire_slot(key));
            if (resource->exceeded) {
                jobs::build_report limited{.worker = options_.worker, .tree = dir.string()};
                limited.analysis_error = "worker resident memory budget exhausted";
                limited.analysis_error_code = "resource_limit";
                limited.analysis_execution = analysis->execution;
                co_return limited;
            }
            if (failed)
                std::rethrow_exception(failed);
            if (stopped)
                co_await coroutine::stop;
            *report.execution_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            report.seconds = *report.execution_seconds;
            co_return report;
        }

        // Once every step succeeded: the artifacts the request names are measured, and the language server it names surveys the tree's
        // documents. A survey that fails is reported, not thrown: the build itself succeeded.
        auto capture_artifacts(const jobs::analysis_plan& analysis, const std::filesystem::path& dir, jobs::build_report& report, delivery::pins& held)
            -> coroutine::task<std::set<std::string>> {
            co_return co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] {
                std::unique_lock guard{exports_lock_};
                const auto declared = jobs::analysis_exports(analysis);
                std::map<std::string, std::string> paths;
                for (const auto& [name, input] : declared)
                    paths.emplace(name, input.path);
                const auto storage = options_.root / "exports";
                auto captured = delivery::capture(dir, paths, storage);
                std::set<std::string> allowed;
                for (auto& file : captured) {
                    const auto& input = declared.at(file.name);
                    const auto measured = std::ranges::find(report.measured, input.name, &builds::artifact_result::name);
                    if (measured == report.measured.end())
                        throw std::invalid_argument("analysis artifact has no measured build output");
                    if (input.kind == "directory") {
                        file.path = (std::filesystem::path{input.path} / file.path).generic_string();
                        report.measured.push_back({.name = input.name,
                                                   .type = input.type,
                                                   .path = std::filesystem::absolute(dir / file.path).string(),
                                                   .digest = file.digest,
                                                   .kind = "file"});
                    } else {
                        file.path = input.path;
                        measured->digest = file.digest;
                    }
                    model_retention_.acquire(file.digest.str(), held);
                    allowed.insert(file.digest.str());
                    if (input.type == "project_model")
                        for (const auto& id : projectmodel::objects(delivery::model(storage, file.digest))) {
                            model_retention_.acquire(id, held);
                            allowed.insert(id);
                        }
                }
                std::ranges::move(captured, std::back_inserter(report.exports));
                return allowed;
            });
        }

        // Installs the project models among `inputs` with the objects they name, and tells whether one loads code.
        auto install_models(const std::vector<builds::context>& inputs, const std::filesystem::path& dir, const jobs::analysis_link& fetch,
                            delivery::pins& held) -> coroutine::task<bool> {
            const auto storage = options_.root / "exports";
            bool loads_code = false;
            for (const auto& input : inputs) {
                if (input.type != "project_model")
                    continue;
                auto [file, missing] = co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] {
                    std::unique_lock guard{exports_lock_};
                    builds::inspect(input, dir);
                    auto file = delivery::capture(dir, {{"model", input.path}}, storage).at(0);
                    model_retention_.acquire(file.digest.str(), held);
                    auto objects = projectmodel::objects(delivery::model(storage, file.digest));
                    for (const auto& id : objects)
                        model_retention_.acquire(id, held);
                    std::erase_if(objects, [&](const auto& id) { return std::filesystem::is_regular_file(storage / id); });
                    return std::pair{std::move(file), std::move(objects)};
                });
                if (execute_view_)
                    missing = co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] { return copy_objects(storage, std::move(missing)); });
                for (const auto& id : missing) {
                    if (!fetch)
                        throw std::runtime_error("model object receiver is unavailable");
                    // A private receiver directory keeps concurrent builds of the same object independent.
                    const auto receiving = storage / ("pending-model-" + nodes::hex(nodes::random_bytes(16)));
                    struct cleanup {
                        std::filesystem::path path;
                        ~cleanup() {
                            std::error_code ec;
                            durability::remove_tree(path, ec);
                        }
                    } cleanup{receiving};
                    std::uint64_t offset = 0, total = 0;
                    do {
                        auto bytes = co_await fetch->ask(jobs::model_read{.model = file.digest, .object = {id, offset, jobs::part_size}});
                        if (bytes.parts.size() != 1)
                            throw std::invalid_argument("invalid model object part count");
                        auto& part = bytes.parts.front();
                        if (part.digest != id || part.offset != offset || part.packed || (offset && part.total != total) ||
                            (part.bytes.empty() && part.offset != part.total))
                            throw std::invalid_argument("invalid model object part");
                        total = part.total;
                        offset += part.bytes.size();
                        co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] {
                            if (delivery::stage(receiving, part)) {
                                delivery::verify(receiving, id, total);
                                std::unique_lock guard{exports_lock_};
                                const auto target = storage / id;
                                if (!std::filesystem::exists(target)) {
                                    delivery::readonly(receiving / "partial" / id);
                                    durability::durable_rename(receiving / "partial" / id, target);
                                }
                            }
                        });
                    } while (offset < total);
                }
                loads_code = co_await io::blocking(rt_.get_scheduler(home_), pool_,
                                                   [&] {
                                                       std::unique_lock guard{exports_lock_};
                                                       delivery::retain_model(storage, file.digest);
                                                       const auto model = delivery::model(storage, file.digest);
                                                       projectmodel::installed(model, storage, options_.runtime.inputs);
                                                       return projectmodel::loads_code(model);
                                                   }) ||
                             loads_code;
            }
            co_return loads_code;
        }

        // Copies the objects the execute role already holds, checking each against its name; returns the rest.
        auto copy_objects(const std::filesystem::path& storage, std::set<std::string> wanted) -> std::set<std::string> {
            const auto from = execute_view_->root() / "exports";
            const auto receiving = storage / ("pending-model-" + nodes::hex(nodes::random_bytes(16)));
            struct cleanup {
                std::filesystem::path path;
                ~cleanup() {
                    std::error_code ec;
                    durability::remove_tree(path, ec);
                }
            } cleanup{receiving};
            std::filesystem::create_directories(receiving / "partial");
            std::set<std::string> rest;
            for (const auto& id : wanted) {
                std::error_code ec;
                const auto size = std::filesystem::file_size(from / id, ec);
                if (ec || size > constants::export_bytes || !std::filesystem::copy_file(from / id, receiving / "partial" / id, ec) || ec) {
                    rest.insert(id);
                    continue;
                }
                delivery::verify(receiving, content::hash{id}, size);
                std::unique_lock guard{exports_lock_};
                if (!std::filesystem::exists(storage / id)) {
                    delivery::readonly(receiving / "partial" / id);
                    durability::durable_rename(receiving / "partial" / id, storage / id);
                }
            }
            return rest;
        }

        auto source_objects(const jobs::analysis_plan& analysis, const jobs::analysis_link& fetch) -> coroutine::task<language::record_inputs> {
            language::record_inputs out;
            for (const auto& [port, ids] : analysis.source_facts)
                for (const auto& id : ids) {
                    auto object = shard_cache_.find(id);
                    if (!object)
                        object = live_shards_[id].lock();
                    if (!object) {
                        if (!fetch)
                            throw std::logic_error("semantic input requires its object reader");
                        std::string bytes;
                        std::uint64_t total = 0;
                        do {
                            auto part = co_await fetch->ask(jobs::shard_read{analysis.execution, id, bytes.size(), constants::semantic_batch_bytes});
                            if (part.offset != bytes.size() || !part.total || part.total > constants::semantic_object_bytes || (total && total != part.total) ||
                                part.data.empty() || part.data.size() > part.total - part.offset)
                                throw std::invalid_argument("invalid semantic object transfer");
                            total = part.total;
                            bytes.append(reinterpret_cast<const char*>(part.data.data()), part.data.size());
                        } while (bytes.size() < total);
                        if (!shard_directory_) {
                            shard_directory_ = std::make_shared<semantic::object_directory>();
                            shard_directory_->path = options_.root / ("semantic-objects-" + nodes::hex(nodes::random_bytes(16)));
                        }
                        object = co_await io::blocking(rt_.get_scheduler(home_), pool_, [directory = shard_directory_, id, bytes = std::move(bytes)] {
                            auto value = json::read<semantic::shard>(bytes);
                            if (!value || value->id != id || semantic::shard_key(*value) != id)
                                throw std::invalid_argument("semantic object has another identity");
                            content::checked_name(id);
                            auto object = std::make_shared<semantic::shard_object>();
                            object->directory = directory;
                            object->path = directory->path / id;
                            std::filesystem::create_directories(directory->path);
                            durability::durable_write(object->path, bytes);
                            object->data = std::make_shared<const semantic::shard>(std::move(*value));
                            return std::shared_ptr<const semantic::shard_object>{std::move(object)};
                        });
                        live_shards_[id] = object;
                        shard_cache_.put(id, object, total, constants::semantic_hot_bytes);
                    }
                    out[port].push_back(std::move(object));
                }
            co_return out;
        }

        auto measure(const jobs::build_request& r, const std::filesystem::path& dir, const std::vector<content::entry>& files, jobs::build_report& report)
            -> coroutine::task<> {
            co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] {
                for (auto& file : files)
                    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(dir / file.path)) || store::hash_of(dir / file.path) != file.digest)
                        throw std::runtime_error("the preparation changed tracked file " + file.path);
            });
            if (!r.measure.empty())
                report.measured = co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] {
                    std::vector<builds::artifact_result> out;
                    for (auto& artifact : r.measure) {
                        auto value = builds::inspect(artifact, dir);
                        value.name = artifact.name;
                        out.push_back(std::move(value));
                    }
                    return out;
                });
        }

        auto analyze(const jobs::build_request& r, const jobs::analysis_plan& plan, const std::filesystem::path& dir, const std::vector<content::entry>& files,
                     jobs::build_report& report, const jobs::analysis_link& messages, delivery::pins& model_pins, bool loads_code) -> coroutine::task<> {
            const auto* analysis = &plan;
            if (!messages)
                throw std::logic_error("analysis requires an input exchange");
            try {
                auto allowed = co_await capture_artifacts(*analysis, dir, report, model_pins);
                language::object_loader objects{options_.root / "exports", std::move(allowed)};
                lsp::limits bounds{.total = std::chrono::seconds{analysis->seconds}};
                bounds.records = std::min<std::uint64_t>(bounds.records, analysis->budget.nodes.value_or(bounds.records));
                auto documents = co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] {
                    std::vector<language::document> out;
                    std::uint64_t bytes = 0;
                    for (auto& file : files) {
                        std::optional<std::string_view> language;
                        if (analysis->registration) {
                            auto suffix = std::filesystem::path{file.path}.extension().string();
                            if (!suffix.empty())
                                if (const auto found = analysis->documents.find(suffix.substr(1)); found != analysis->documents.end())
                                    language = found->second;
                        } else
                            language = language::providers::document(analysis->provider, file.path);
                        if (!language)
                            continue;
                        if (file.size > bounds.document_bytes)
                            throw std::length_error(std::format("{} is larger than the {} bytes a survey reads", file.path, bounds.document_bytes));
                        bytes += file.size;
                        std::ifstream in{dir / file.path, std::ios::binary};
                        std::string text{std::istreambuf_iterator<char>{in}, {}};
                        // A file that holds a NUL is not text, whatever its name.
                        if (text.find('\0') != std::string::npos) {
                            bytes -= file.size;
                            continue;
                        }
                        if (out.size() >= bounds.documents || bytes > bounds.bytes)
                            throw std::length_error(std::format("analysis input covers at most {} documents and {} bytes", bounds.documents, bounds.bytes));
                        out.push_back({file.path, std::string{*language}, std::move(text)});
                    }
                    return out;
                });
                const auto slot = store::tree_key(r.project, r.tree);
                const auto cache = store_.analysis(slot) / analysis->execution.stage_id;
                const auto bindings = toolchain::bind(options_.runtime, analysis->resolved_toolchain, options_.tools);
                auto local = language::locate({.provider = analysis->provider,
                                               .argv = analysis->argv,
                                               .env = analysis->env,
                                               .initialization = analysis->initialization,
                                               .commands = analysis->commands,
                                               .executables = analysis->executables,
                                               .registration = analysis->registration,
                                               .documents = analysis->documents},
                                              dir, bindings.tools, options_.parallel, cache, bindings.sdks);
                const bool trusted = plan.project_code || (plan.model_code && loads_code);
                if (options_.execute && trusted)
                    throw std::logic_error("the execute role answers analyses that run project code");
                auto overrides = trusted ? r.env : builds::environment{};
                for (const auto& [name, value] : local.env)
                    overrides[name] = value;
                auto env = analysis_environment(overrides);
                for (const auto& [name, value] : bindings.env)
                    env[name] = value;
                allocate_runtime(env);
                auto known_tool = [&](const std::string& executable) {
                    return !toolchain::tool_digest(options_.runtime, toolchain::tool_path(executable)).empty();
                };
                if (options_.execute &&
                    ((!local.argv.empty() && !known_tool(local.argv.front())) || !std::ranges::all_of(local.executables, known_tool) ||
                     !std::ranges::all_of(local.commands, [&](const auto& command) { return known_tool(command.second.front()); })))
                    throw std::invalid_argument("the analysis names an executable outside this Worker's toolchain");
                auto identity =
                    content::hash_of(json::write(std::tuple{analysis->provider, analysis->options, local.argv, env, local.initialization, local.commands,
                                                           local.executables, bindings.sdks, options_.runtime, r.max_memory_bytes, r.max_cpu_seconds}))
                        .str();
                const session_key key{slot, analysis->execution.stage_id};
                if (auto found = contexts_.find(key); found != contexts_.end() && found->second->identity != identity)
                    co_await retire(key);
                if (!contexts_.contains(key)) {
                    while (contexts_.size() >= constants::language_sessions) {
                        std::vector<std::pair<std::chrono::steady_clock::time_point, session_key>> idle;
                        for (const auto& [candidate, value] : contexts_)
                            if (candidate.first == slot || !active_.contains(candidate.first))
                                idle.emplace_back(value->used, candidate);
                        if (idle.empty())
                            throw std::length_error("language session capacity exhausted");
                        order_idle(idle);
                        co_await retire(idle.front().second);
                    }
                    auto value = std::make_unique<context>();
                    value->identity = identity;
                    value->used = std::chrono::steady_clock::now();
                    io::command command{.argv = std::move(local.argv),
                                        .cwd = dir,
                                        .env = std::move(env),
                                        .max_memory_bytes = r.max_memory_bytes,
                                        .max_cpu_seconds = r.max_cpu_seconds,
                                        .processes = value->processes.group()};
                    value->server = language::make_adapter(rt_.get_scheduler(home_), std::move(command),
                                                           language::session_state{.slot = slot,
                                                                                   .project = r.project,
                                                                                   .provider = analysis->provider,
                                                                                   .options = analysis->options,
                                                                                   .stage = analysis->execution.stage_id},
                                                           std::move(local.commands), options_.runtime,
                                                           language::document_loader{.root = dir, .scheduler = rt_.get_scheduler(home_), .pool = &pool_},
                                                           std::move(local.registration), {.root = options_.root / "exports"}, cache);
                    value->server->executables(std::move(local.executables));
                    value->server->sdks(bindings.sdks);
                    std::filesystem::create_directories(cache);
                    auto& created = *contexts_.emplace(key, std::move(value)).first->second;
                    const auto opening = std::chrono::steady_clock::now();
                    co_await created.server->open(std::move(local.initialization));
                    created.open_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - opening).count();
                }
                auto& hot = *contexts_.at(key);
                hot.used = std::chrono::steady_clock::now();
                hot.server->objects(std::move(objects));
                std::map<std::string, std::string> model;
                for (const auto& output : report.measured)
                    if (!output.digest.empty() && std::ranges::contains(analysis->artifacts, output.name, &builds::context::name))
                        model.emplace(std::filesystem::path(output.path).lexically_relative(std::filesystem::absolute(dir)).generic_string(),
                                      output.digest.str());
                std::map<std::string, std::string> source_files;
                for (const auto& file : files)
                    source_files.emplace(file.path, file.digest.str());
                auto generated = co_await observed([&](const view& trees) { return trees.generated_inputs(r.project, r.tree); });
                auto source_facts = co_await source_objects(*analysis, messages);
                hot.models.insert(hot.models.end(), model_pins.begin(), model_pins.end());
                const auto installing = std::chrono::steady_clock::now();
                auto installed = co_await hot.server->update(hot.server->state().epoch, {.snapshot = r.snapshot.str(),
                                                                                         .documents = std::move(documents),
                                                                                         .source_files = std::move(source_files),
                                                                                         .model = std::move(model),
                                                                                         .generated = std::move(generated),
                                                                                         .source_facts = std::move(source_facts),
                                                                                         .projects = analysis->inputs});
                hot.models = model_pins;
                auto units = co_await hot.server->units(installed.epoch, analysis->demand, analysis->query);
                hot.install_seconds = std::max(hot.install_seconds, std::chrono::duration<double>(std::chrono::steady_clock::now() - installing).count());
                std::vector<semantic::candidate> accepted;
                const auto public_inputs = analysis->public_lookup ? hot.server->public_inputs(units, analysis->demand) : public_shards::units{};
                if (!analysis->candidates.empty() || !public_inputs.empty()) {
                    if (!messages)
                        throw std::logic_error("analysis reuse requires its gateway validator");
                    auto current_inputs = co_await hot.server->current_inputs(analysis->candidates);
                    auto processes = hot.server->current_processes(units, analysis->demand);
                    jobs::reuse_request request{analysis->execution, installed, std::move(current_inputs), std::move(processes)};
                    for (const auto& [unit, inputs] : public_inputs) {
                        auto& installed_inputs = request.inputs[unit.project];
                        for (const auto& input : inputs)
                            if (!std::ranges::contains(installed_inputs, input))
                                installed_inputs.push_back(input);
                        request.public_units.emplace_back(unit, inputs);
                    }
                    auto decision = co_await messages->ask(std::move(request));
                    for (const auto& candidate : analysis->candidates)
                        if (std::ranges::contains(decision.accepted, reuse::key{candidate.id, semantic::recipe_identifier(candidate.recipe)}))
                            accepted.push_back(candidate);
                    accepted.append_range(std::move(decision.public_candidates));
                }

                report.analyzed = true;
                report.missing.clear();
                for (const auto& path : units)
                    report.missing.push_back(hot.server->deferred(path, analysis->demand, "semantic time budget exhausted"));
                std::uint64_t evaluated = 0;
                std::size_t records = 0;
                for (const auto& path : units) {
                    auto pending = std::ranges::find_if(
                        report.missing, [&](const auto& missing) { return std::ranges::contains(missing.units, semantic::unit_identifier(path)); });
                    if (pending == report.missing.end())
                        throw std::invalid_argument("provider returned duplicate or unrequested unit IDs");
                    const bool units_exhausted = evaluated >= analysis->budget.units.value_or(bounds.documents);
                    language::evaluation_request evaluation{.execution = analysis->execution,
                                                            .slot_id = analysis->slot_id,
                                                            .unit = path,
                                                            .epoch = installed.epoch,
                                                            .inputs = installed.inputs,
                                                            .demand = analysis->demand,
                                                            .budget = analysis->budget,
                                                            .retained_only = units_exhausted || records == bounds.records};
                    const auto computing = std::chrono::steady_clock::now();
                    auto unit = path.operation == "resolve" ? co_await hot.server->resolve(std::move(evaluation), analysis->query, accepted)
                                                            : co_await hot.server->evaluate(std::move(evaluation), accepted);
                    if (!unit.deferred && !unit.reused)
                        hot.unit_seconds[path] =
                            std::max(hot.unit_seconds[path], std::chrono::duration<double>(std::chrono::steady_clock::now() - computing).count());
                    if (unit.deferred) {
                        pending->reason = units_exhausted ? "semantic unit budget exhausted" : "semantic node budget exhausted";
                        continue;
                    }
                    bool retained = false;
                    std::size_t count = 0;
                    if (unit.reused) {
                        const auto candidate = std::ranges::find_if(accepted, [&](const auto& candidate) {
                            return candidate.id == *unit.reused && candidate.recipe == semantic::recipe{unit.request.unit, installed.options};
                        });
                        if (candidate == accepted.end())
                            throw std::logic_error("adapter returned an unoffered recipe");
                        retained = candidate->retained;
                        report.analysis_cacheable &= candidate->cacheable;
                        if (!retained)
                            count = candidate->nodes;
                    }
                    if (!retained)
                        ++evaluated;
                    for (auto& shard : unit.shards) {
                        report.analysis_cacheable &= shard.unit.cacheable && shard.unit.coverage.missing.empty();
                        count += shard.identities.size() + shard.symbols.size() + shard.types.size() + shard.occurrences.size() + shard.references.size() +
                                 shard.scopes.size() + shard.relations.size() + shard.diagnostics.size() + shard.typed_ranges.size();
                        semantic::contributions(shard);
                    }
                    if (count > bounds.records - records) {
                        pending->reason = std::format("semantic node budget exhausted; unit requires {} nodes", count);
                        continue;
                    }
                    records += count;
                    co_await coroutine::under(coroutine::stop_token{}, messages->ask(std::move(unit)));
                    ++report.committed_units;
                    report.missing.erase(pending);
                }
                hot.used = std::chrono::steady_clock::now();
                if (report.committed_units && !options_.runtime.id.empty()) {
                    const jobs::cached_session marker{analysis->execution.stage_id, analysis->options, r.snapshot.str(), options_.runtime.id};
                    co_await io::blocking(rt_.get_scheduler(home_), pool_, [cache, marker] {
                        try {
                            const auto path = cache / ".fughetta-warm";
                            if (std::ranges::any_of(std::filesystem::directory_iterator(cache),
                                                    [&](const auto& entry) { return entry.path() != path; }))
                                durability::durable_write(path, json::write(marker));
                        } catch (const std::exception&) {
                            // This inventory only informs placement. The provider's output is already available.
                        }
                    });
                }
                report.analysis_session = std::move(installed);
            } catch (const std::length_error& e) {
                report.analysis_error = e.what();
                report.analysis_error_code = "resource_limit";
            } catch (const language::resolution_error& e) {
                report.analysis_error = e.what();
                report.analysis_error_code = "invalid_record";
            } catch (const std::exception& e) {
                report.analysis_error = e.what();
            }
            if (report.analysis_error) {
                const session_key key{store::tree_key(r.project, r.tree), analysis->execution.stage_id};
                auto found = contexts_.find(key);
                const bool failed = found != contexts_.end() && found->second->server->state().phase == "failed";
                if (failed || (report.analysis_error_code != "invalid_record" && report.analysis_error_code != "resource_limit"))
                    co_await retire(key);
            }
        }

        auto run_command(const std::string& slot, const builds::step& step, io::command command, std::chrono::milliseconds timeout,
                         std::uint64_t output_limit, std::optional<std::filesystem::path> log, std::uint64_t log_limit)
            -> coroutine::task<runner::result> {
            auto& scheduler = rt_.get_scheduler(home_);
            if (!step.gradle)
                co_return co_await runner::run_step(scheduler, std::move(command), timeout, output_limit, step.name, log, log_limit);

            const auto java_limit = "-XX:ActiveProcessorCount=" + std::to_string(options_.parallel);
            command.argv.insert(command.argv.end(), {"-Pkotlin.compiler.execution.strategy=in-process", "-Dorg.gradle.native=false"});

            const auto java = options_.tools.find("java"), host = options_.tools.find("fughetta-jvm");
            const auto installation = gradle_installation(step, command.argv.front(), command.max_cpu_seconds, output_limit);
            if (installation.empty()) {
                // A standalone Gradle launcher may fork a single-use daemon; that JVM must also stay in the group.
                auto& launcher_options = (*command.env)["JAVA_TOOL_OPTIONS"];
                if (!launcher_options.empty())
                    launcher_options += ' ';
                launcher_options += "-Dorg.gradle.native=false " + java_limit;
                command.argv.insert(command.argv.end(), {"--no-daemon", "--console=plain"});
                co_return co_await runner::run_step(scheduler, std::move(command), timeout, output_limit, step.name, log, log_limit);
            }

            const auto state = std::filesystem::absolute(store_.analysis(slot) / "gradle");
            const auto configured = command.env->find("GRADLE_USER_HOME");
            const auto home = std::filesystem::absolute(configured != command.env->end() && !configured->second.empty()
                                                            ? command.cwd / configured->second
                                                            : options_.root / "gradle");
            (*command.env)["GRADLE_USER_HOME"] = home.string();
            auto bootstrap = *command.env;
            const auto identity = json::write(std::tuple{java->second, host->second, installation.string(), command.cwd.string(), bootstrap,
                                                         command.max_memory_bytes});
            auto found = gradle_.find(slot);
            if (found != gradle_.end() && (found->second->identity != identity || !found->second->session->alive())) {
                co_await retire_gradle(slot);
                found = gradle_.end();
            }
            if (found == gradle_.end()) {
                co_await io::blocking(scheduler, pool_, [home, state] {
                    std::filesystem::create_directories(home);
                    std::filesystem::create_directories(state);
                });
                auto resident = std::make_unique<gradle_context>();
                resident->identity = identity;
                resident->used = std::chrono::steady_clock::now();
                const auto classpath_separator = std::filesystem::path::preferred_separator == L'\\' ? ";" : ":";
                const auto classpath = host->second + classpath_separator + (installation / "lib" / "*").string();
                resident->session = std::make_unique<runner::gradle_session>(
                    scheduler, io::command{.argv = {java->second, "-Xms32m", "-Xmx128m", java_limit, "-cp", classpath, "fughetta.jvm.MainKt", "--gradle",
                                                     installation.string(), home.string(), state.string(),
                                                     std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(constants::language_idle).count()),
                                                     java_limit},
                                           .cwd = command.cwd,
                                           .env = std::move(bootstrap),
                                           .max_memory_bytes = command.max_memory_bytes,
                                           .stdio = io::process_stdio::blocking,
                                           .processes = resident->processes.group(),
                                           .kill_group_on_owner_exit = true});
                found = gradle_.emplace(slot, std::move(resident)).first;
            }
            auto& resident = *found->second;
            std::vector<std::string> arguments(std::next(command.argv.begin()), command.argv.end());
            auto result = co_await resident.session->run(std::move(arguments), std::move(*command.env), timeout, output_limit, step.name, log, log_limit);
            resident.used = std::chrono::steady_clock::now();
            co_return result;
        }

        auto run_steps(const jobs::build_request& r, const std::filesystem::path& dir, const std::filesystem::path& reports,
                       std::chrono::steady_clock::time_point deadline, bool analyzing, const jobs::analysis_link& models, delivery::pins& model_pins,
                       const toolchain::bindings& bindings, progress_sink* sink = nullptr) -> coroutine::task<jobs::build_report> {
            jobs::build_report report{.worker = options_.worker, .tree = dir.string(), .preparation_seconds = 0, .execution_seconds = 0};
            const auto logs = reports.parent_path() / (reports.filename().string() + ".logs");
            std::string log_setup_error;
            try {
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [logs] {
                    store::discard(logs);
                    std::filesystem::create_directories(logs);
                });
            } catch (const std::exception& error) {
                log_setup_error = error.what();
            }
            std::uint64_t log_bytes = 0;
            struct elapsed {
                double& seconds;
                std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
                ~elapsed() { seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); }
            };
            auto invocations = builds::invocations(r.steps);
            const auto steps_total = static_cast<std::uint64_t>(invocations.size());
            std::uint64_t steps_done = 0;
            for (auto& invocation : invocations) {
                auto& step = invocation.command;
                if (sink)
                    co_await (*sink)({.done = steps_done, .total = steps_total, .stage = step.name});
                co_await install_models(step.inputs, dir, models, model_pins);
                elapsed measured{step.preparation ? *report.preparation_seconds : *report.execution_seconds};
                if (step.argv.empty())
                    throw std::invalid_argument(std::format("step '{}' has no command", step.name));
                const auto cache =
                    options_.dependency_cache.value_or(options_.root / (options_.dependency_publish ? "publisher/dependencies" : "dependencies"));
                auto local = co_await io::blocking(rt_.get_scheduler(home_), pool_, [this, &bindings, step, dir, reports, cache] {
                    return builds::locate(step, dir, bindings.tools, options_.parallel, reports, cache,
                                          options_.dependency_publish.value_or(options_.dependency_upstream.value_or(std::filesystem::path{})), bindings.sdks);
                });
                std::vector<durability::dir_lock> dependency_locks;
                bool cache_waited = false;
                double cache_wait_seconds = 0;
                if (auto found = step.env.find("VCPKG_BINARY_SOURCES"); found != step.env.end() && found->second.contains("${cache:vcpkg}")) {
                    const auto waiting_since = std::chrono::steady_clock::now();
                    while (std::chrono::steady_clock::now() < deadline) {
                        dependency_locks = co_await io::blocking(rt_.get_scheduler(home_), pool_, [cache, publish = options_.dependency_publish] {
                            std::vector<durability::dir_lock> locks;
                            try {
                                locks.emplace_back(cache / "vcpkg", "dependency cache");
                                if (publish)
                                    locks.emplace_back(*publish / "vcpkg", "published dependency cache");
                            } catch (const durability::lock_busy&) {
                                locks.clear();
                            }
                            return locks;
                        });
                        if (!dependency_locks.empty())
                            break;
                        cache_waited = true;
                        co_await execution::schedule_after(rt_.get_scheduler(home_), constants::dependency_lock_poll);
                    }
                    cache_wait_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - waiting_since).count();
                }
                if (!dependency_locks.empty() && std::chrono::steady_clock::now() >= next_dependency_prune_) {
                    next_dependency_prune_ = std::chrono::steady_clock::now() + constants::dependency_prune_interval;
                    try {
                        co_await io::blocking(rt_.get_scheduler(home_), pool_, [cache, deadline] { dependencies::prune(cache, deadline); });
                    } catch (const std::exception& e) {
                        std::println(std::cerr, "dependency cache maintenance: {}", e.what());
                    }
                }
                auto cwd = std::filesystem::path(local.cwd);
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [cwd, reports] {
                    std::error_code ignored;
                    std::filesystem::create_directories(cwd, ignored);
                    // A report left by an earlier step or build is never taken for this step's.
                    std::filesystem::remove_all(reports);
                    std::filesystem::create_directories(reports);
                });
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left <= std::chrono::milliseconds::zero()) {
                    report.steps.push_back({.name = step.name,
                                            .timed_out = true,
                                            .output = cache_waited ? std::format("not started: waited {:.2f} seconds for the shared vcpkg cache lock held by "
                                                                                 "another install; execution deadline exceeded",
                                                                                 cache_wait_seconds)
                                                                   : "not started: the build's time ran out",
                                            .tasks = invocation.tasks});
                    break;
                }
                io::command cmd{.argv = std::move(local.argv),
                                .cwd = cwd,
                                .env = environment(r.env, local.env),
                                .max_memory_bytes = r.max_memory_bytes,
                                .max_cpu_seconds = r.max_cpu_seconds};
                for (const auto& [name, value] : bindings.env)
                    (*cmd.env)[name] = value;
                allocate_runtime(*cmd.env, !step.gradle);
                // Keep Cargo build-script fingerprints equal across check, build and test.
                if (step.cargo)
                    cmd.env->try_emplace("RUSTC_BOOTSTRAP", "");
                cmd.processes = resources_.at(store::tree_key(r.project, r.tree))->processes.group();
                // Keep exporter inputs stable across Gradle invocations in the same slot.
                // Gradle inherits these at launch because native environment rewriting is disabled.
                cmd.env->erase("FUGHETTA_STAGING");
                cmd.env->erase("FUGHETTA_SDKS");
                if (step.preparation || step.gradle) {
                    (*cmd.env)["FUGHETTA_STAGING"] = std::filesystem::absolute(reports / "objects").string();
                    (*cmd.env)["FUGHETTA_SDKS"] = json::write(options_.runtime.inputs);
                }
                if (step.generation)
                    co_await locked([&](store& s) { s.begin_generation(r.project, r.tree, *step.generation); });
                if (local.standard_output)
                    cmd.standard_output = reports / *local.standard_output;
                const auto log_name = std::to_string(report.steps.size()) + ".log";
                report.steps.push_back(co_await run_command(store::tree_key(r.project, r.tree), step, std::move(cmd), left, r.output_limit,
                                                            log_setup_error.empty() ? std::optional{logs / log_name} : std::nullopt,
                                                            constants::log_bytes - log_bytes));
                auto& last = report.steps.back();
                last.tasks = invocation.tasks;
                ++steps_done;
                if (sink)
                    co_await (*sink)({.done = steps_done,
                                     .total = steps_total,
                                     .stage = last.timed_out ? std::format("{}: timed out", step.name)
                                                             : std::format("{}: exit {}", step.name, last.exit_code)});
                if (!dependency_locks.empty() && options_.dependency_publish && last.exit_code == 0 && !last.timed_out) {
                    try {
                        co_await io::blocking(rt_.get_scheduler(home_), pool_, [cache, destination = *options_.dependency_publish, deadline] {
                            dependencies::publish(cache, destination, deadline);
                        });
                    } catch (const std::exception& error) {
                        last.cache_error = std::string{error.what()}.substr(0, 512);
                    }
                }
                dependency_locks.clear();
                if (!log_setup_error.empty())
                    last.log_error = log_setup_error;
                try {
                    if (last.log_error.empty()) {
                        auto saved = co_await io::blocking(rt_.get_scheduler(home_), pool_,
                                                           [this, logs, log_name, root = options_.root,
                                                            publish = !analyzing && r.measure.empty() &&
                                                                      ((step.gradle && last.timed_out) || last.truncated || last.output.size() > 8192)] {
                                                               std::shared_lock guard{exports_lock_};
                                                               const auto size = std::filesystem::file_size(logs / log_name);
                                                               std::optional<builds::exported_file> file;
                                                               if (publish && size)
                                                                   file = delivery::capture(logs, {{"step-" + log_name, log_name}}, root / "exports").at(0);
                                                               std::filesystem::remove(logs / log_name);
                                                               return std::pair{size, file};
                                                           });
                        log_bytes += saved.first;
                        last.log = std::move(saved.second);
                    }
                } catch (const std::exception& error) {
                    last.log_error = error.what();
                    log_bytes = constants::log_bytes;
                }
                auto found = co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] { return builds::collect(local, reports, dir); });
                last.diagnostics = std::move(found.diagnostics);
                last.tests = std::move(found.tests);
                if (last.exit_code != 0 || last.timed_out)
                    break;
                if (step.generation && !step.generation->manifest.empty())
                    co_await locked([&](store& s) {
                        struct manifest {
                            std::vector<std::string> files;
                        };
                        const auto path = reports / content::checked_name(step.generation->manifest);
                        std::string bytes;
                        for (auto part : durability::read_plain(path, constants::generated_manifest_bytes))
                            bytes.append(reinterpret_cast<const char*>(part.data()), part.size());
                        auto generated = json::read<manifest>(bytes);
                        if (!generated)
                            throw std::runtime_error("invalid generated output manifest: " + generated.error().what());
                        s.commit_generation(r.project, r.tree, *step.generation, generated->files);
                    });
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] {
                    std::unique_lock guard{exports_lock_};
                    for (auto& output : step.outputs) {
                        builds::inspect(output, dir);
                        if (output.type == "project_model") {
                            auto file = delivery::capture_model(dir, output, reports / "objects", options_.root / "exports");
                            projectmodel::installed(delivery::model(options_.root / "exports", file.digest), options_.root / "exports",
                                                    options_.runtime.inputs);
                            model_retention_.acquire(file.digest.str(), model_pins);
                            for (const auto& object : file.objects)
                                model_retention_.acquire(object.digest.str(), model_pins);
                            report.models.push_back(std::move(file));
                        }
                    }
                    std::filesystem::remove_all(reports / "objects");
                });
            }
            co_return report;
        }

        cores::context<native::scheduler>& rt_;
        store store_;
        options options_;
        semantic::hot_cache<semantic::shard_object> shard_cache_;
        std::unordered_map<semantic::shard_id, std::weak_ptr<const semantic::shard_object>> live_shards_;
        std::shared_ptr<semantic::object_directory> shard_directory_;
        execution::thread_pool& pool_;
        std::size_t home_;
        coroutine::mutex store_lock_;
        coroutine::mutex memory_lock_;
        std::map<std::string, std::shared_ptr<resource_request>> resources_;
        std::shared_mutex exports_lock_;
        delivery::retention model_retention_;
        std::unordered_map<std::string, std::weak_ptr<coroutine::mutex>> tree_locks_;
        jobs::tree_holds remote_holds_;
        std::uint64_t holds_sequence_ = 0;
        std::chrono::steady_clock::time_point next_dependency_prune_{};
        std::map<std::string, int> active_;
        std::map<session_key, std::unique_ptr<context>> contexts_;
        std::map<std::string, std::unique_ptr<gradle_context>> gradle_;
        std::map<std::string, preparation> preparations_;
        std::chrono::steady_clock::time_point export_collect_{};
        std::optional<view> execute_view_;
    };
} // namespace replica
