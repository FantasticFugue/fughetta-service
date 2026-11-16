export module executor;
import std;
import native;
import coroutine;
import replica;
import runner;
import protocol;
import host;
import lsp;
import evaluation;
import constants;
import reflection;
import delivery;
import durability;
import dependencies;
import toolchain;
import channels;
import enumeration;
import codec;
import process;
import allocation;

export namespace replica {
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
    };

    class service {
    public:
        using socket = native::socket;
        using dispatcher = transport::dispatcher<socket>;

        service(cores::context<native::scheduler>& rt, options o, execution::thread_pool& pool, std::size_t home = 0)
            : rt_(rt), store_(o.root, o.limits), options_(std::move(o)), pool_(pool), home_(home) {}

        void set_runtime(toolchain::identity identity) { options_.runtime = std::move(identity); }

        // The owner keeps the I/O runtime alive until all context-owned children are gone.
        auto run(std::size_t core, coroutine::task<> work) -> coroutine::task<> {
            auto token = co_await coroutine::current_token;
            bool stopped = false;
            std::exception_ptr failed;
            try {
                stopped = !(co_await coroutine::under(token, std::move(work)));
            } catch (...) {
                failed = std::current_exception();
            }
            auto finish = [this]() -> coroutine::task<> {
                std::exception_ptr failed;
                while (!contexts_.empty()) {
                    auto key = contexts_.begin()->first;
                    try {
                        co_await retire(key);
                    } catch (...) {
                        if (!failed)
                            failed = std::current_exception();
                    }
                }
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

        template <io::stream S> void install(transport::dispatcher<S>& d, std::size_t core) {
            auto handle_export_read = [this, core](jobs::export_read request, auto&) -> coroutine::task<jobs::source_bytes> {
                require_consumer();
                co_return co_await io::blocking(rt_.get_scheduler(core), pool_, [this, root = options_.root, request] {
                    std::shared_lock guard{exports_lock_};
                    return delivery::read(root / "exports", request);
                });
            };
            d.template on<jobs::export_read>(std::move(handle_export_read));
            d.template on<jobs::sync_offer>([this, core](jobs::sync_offer o, auto&) {
                require_consumer();
                return at_home(core, [o = std::move(o)](store& s) { return s.offer(o); });
            });
            auto handle_source_bytes = [this, core](jobs::source_bytes p, auto&) -> coroutine::task<jobs::authority_ack> {
                require_consumer();
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
                    if (core == home_)
                        co_return co_await build(std::move(r), inputs);
                    co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_),
                                                   [this, r = std::move(r), inputs] { return build(r, inputs); });
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
            const auto text = rfl::json::write(request.manifest);
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
            const auto key = content::hash_of(rfl::json::write(std::tuple{request, recipe}));
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
                    item->fail(std::current_exception());
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

        struct preparation {
            std::string computation;
            content::hash snapshot;
            std::set<std::string> tasks;
        };

        std::uint64_t status_sequence_ = 0;

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
            if (const auto context = contexts_.find({slot, request.stage}); context != contexts_.end()) {
                auto child = context->second->processes.usage();
                if (!child)
                    return {};
                result->resident_bytes += child->resident_bytes;
                result->cpu_nanoseconds += child->cpu_nanoseconds;
                result->threads += child->threads;
                result->processes += child->processes;
            }
            return result;
        }
        auto memory_state() const -> jobs::worker_memory {
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
                const auto capacity = *options_.memory_bytes - std::min(*options_.memory_bytes, self->resident_bytes);
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
                    return retained <= capacity - reserved - request->reserved;
                };
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
                failed = std::current_exception();
            }
            old->server.reset();
            if (failed)
                std::rethrow_exception(failed);
        }

        auto retire_slot(const std::string& slot) -> coroutine::task<> {
            preparations_.erase(slot);
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
                        failed = std::current_exception();
                }
            }
            if (failed)
                std::rethrow_exception(failed);
        }

        auto status_home() -> coroutine::task<jobs::slot_report> {
            jobs::slot_report out;
            out.slots = co_await locked([](store& s) { return s.slots(); });
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
            if (state.limit && state.resident && *state.resident > *state.limit) {
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
                return !active_.contains(item.first) && std::ranges::none_of(contexts_, [&](const auto& context) { return context.first.first == item.first; });
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

        auto push(jobs::source_bytes p) -> coroutine::task<jobs::authority_ack> {
            co_await collect_sessions();
            std::set<std::string> busy;
            for (const auto& [key, value] : contexts_)
                busy.insert(key.first);
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

        void allocate_runtime(std::map<std::string, std::string>& env) const {
            const auto jobs = std::to_string(options_.parallel);
            auto& java = env["JAVA_TOOL_OPTIONS"];
            if (!java.empty())
                java += ' ';
            java += "-XX:ActiveProcessorCount=" + jobs;
            env["DOTNET_PROCESSOR_COUNT"] = jobs;
        }

        template <jobs::execution_request Request> auto build(Request work, jobs::analysis_link messages = {}) -> coroutine::task<jobs::build_report> {
            auto& r = jobs::traits<Request>::preparation(work);
            auto* analysis = jobs::traits<Request>::analysis(work);
            co_await collect_sessions();
            auto started = std::chrono::steady_clock::now();
            auto key = store::tree_key(r.project, r.tree);
            if (analysis && analysis->slot_id != key)
                throw std::invalid_argument("analysis request belongs to another slot");
            auto lock = tree_locks_[key].lock();
            if (!lock) {
                lock = std::make_shared<coroutine::mutex>();
                tree_locks_[key] = lock;
            }
            struct tree_lifetime {
                std::unordered_map<std::string, std::weak_ptr<coroutine::mutex>>& locks;
                std::string key;
                std::shared_ptr<coroutine::mutex> lock;
                ~tree_lifetime() {
                    if (lock.use_count() == 1)
                        locks.erase(key);
                }
            } lifetime{tree_locks_, key, std::move(lock)};
            auto turn = co_await lifetime.lock->lock();
            struct release_resources {
                service* owner;
                std::string key;
                ~release_resources() { owner->resources_.erase(key); }
            } resource_lease{this, key};
            auto resource = co_await reserve_memory(key, analysis ? analysis->execution.stage_id : std::string{}, r.max_memory_bytes);
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
            // A receipt belongs to this live directory and one computation. Moving
            // it out before checkout makes cancellation and intervening builds
            // invalidate it even when they stop before producing a report.
            preparation prepared;
            if (auto found = preparations_.find(key); found != preparations_.end()) {
                if (!r.fresh && analysis && found->second.computation == analysis->execution.computation_id && found->second.snapshot == r.snapshot)
                    prepared = std::move(found->second);
                preparations_.erase(found);
            }
            if (analysis) {
                prepared.computation = analysis->execution.computation_id;
                prepared.snapshot = r.snapshot;
            }
            if (!r.preparation_tasks.empty()) {
                if (!analysis || prepared.computation.empty())
                    throw std::invalid_argument("preparation tasks require an analysis computation");
                std::vector<jobs::step> pending;
                std::size_t covered = 0;
                for (const auto& task : r.preparation_tasks) {
                    if (task.first_step != covered || task.step_count == 0 || task.step_count > r.steps.size() - covered)
                        throw std::invalid_argument("invalid preparation task steps");
                    if (!prepared.tasks.contains(task.name))
                        pending.insert(pending.end(), r.steps.begin() + covered, r.steps.begin() + covered + task.step_count);
                    covered += task.step_count;
                }
                if (covered != r.steps.size())
                    throw std::invalid_argument("preparation tasks do not cover the requested steps");
                r.steps = std::move(pending);
            }
            if (r.fresh)
                co_await retire_slot(key);
            bool preparation = !r.measure.empty() || analysis;
            // A preparation keeps the snapshot's files, which the offer cache may let go while the steps run.
            auto [dir, reports, files, retired] = co_await locked(
                [project = r.project, snapshot = r.snapshot, tree = r.tree, key, preparation, fresh = r.fresh, producers = r.generation_producers](store& s) {
                    auto retired = fresh ? s.detach(project, tree) : std::filesystem::path{};
                    auto dir = s.checkout(project, snapshot, tree);
                    s.reconcile_generations(project, tree, producers);
                    return std::tuple{dir, s.reports(key), preparation ? s.files(project, snapshot) : std::vector<content::entry>{}, retired};
                });
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
            bool analysis_expired = false;
            delivery::pins model_pins;
            try {
                report = co_await coroutine::under(token, run_steps(r, dir, reports, deadline, analysis != nullptr, messages, model_pins));
                if (report && analysis)
                    report->analysis_execution = analysis->execution;
                if (report && report->ok() && preparation) {
                    auto io = rt_.get_scheduler(home_);
                    auto remaining = std::max(std::chrono::steady_clock::duration::zero(), deadline - std::chrono::steady_clock::now());
                    bool budget_limited = false;
                    if (analysis) {
                        auto milliseconds = std::min<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count(),
                                                                    std::uint64_t{analysis->seconds} * 1000);
                        if (analysis->budget.milliseconds) {
                            budget_limited = *analysis->budget.milliseconds <= milliseconds;
                            milliseconds = std::min(milliseconds, *analysis->budget.milliseconds);
                        }
                        remaining = std::chrono::milliseconds{milliseconds};
                        report->missing.push_back({.scope = analysis->demand, .reason = "semantic time budget exhausted", .deferred = true});
                    }
                    co_await install_models(r.measure, dir, messages, model_pins);
                    const auto analysis_started = std::chrono::steady_clock::now();
                    auto finished = co_await coroutine::under(
                        token, coroutine::deadline(io, remaining, afterwards(r, analysis, dir, files, *report, messages, model_pins)));
                    *report->execution_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - analysis_started).count();
                    if (!finished)
                        report.reset();
                    else if (!*finished) {
                        analysis_expired = true;
                        report->analysis_session.reset();
                        if (budget_limited) {
                            report->analyzed = true;
                            if (report->missing.empty())
                                report->missing.push_back({.scope = analysis->demand, .reason = "semantic time budget exhausted", .deferred = true});
                        } else {
                            report->analyzed = false;
                            report->steps.push_back(
                                {.name = "analysis", .timed_out = true, .output = "time budget exhausted during artifact or language analysis"});
                        }
                    }
                }
            } catch (...) {
                failed = std::current_exception();
            }
            if (analysis && (resource->exceeded || failed || !report || !report->ok() || analysis_expired))
                co_await coroutine::under(coroutine::stop_token{}, retire_slot(key));
            if (resource->exceeded) {
                jobs::build_report limited{.worker = options_.worker, .tree = dir.string()};
                limited.analysis_error = "worker resident memory budget exhausted";
                limited.analysis_error_code = "resource_limit";
                if (analysis)
                    limited.analysis_execution = analysis->execution;
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
            if (analysis && report->ok() && report->analyzed && !report->analysis_error) {
                for (const auto& task : r.preparation_tasks)
                    prepared.tasks.insert(task.name);
                preparations_[key] = std::move(prepared);
            }
            report->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            co_return std::move(*report);
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

        auto install_models(const std::vector<builds::context>& inputs, const std::filesystem::path& dir, const jobs::analysis_link& fetch,
                            delivery::pins& held) -> coroutine::task<> {
            const auto storage = options_.root / "exports";
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
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] {
                    std::unique_lock guard{exports_lock_};
                    delivery::retain_model(storage, file.digest);
                    projectmodel::installed(delivery::model(storage, file.digest), storage, options_.runtime.inputs);
                });
            }
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
                            auto value = rfl::json::read<semantic::shard>(bytes);
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

        auto afterwards(const jobs::build_request& r, const jobs::analysis_plan* analysis, const std::filesystem::path& dir,
                        const std::vector<content::entry>& files, jobs::build_report& report, const jobs::analysis_link& messages, delivery::pins& model_pins)
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
            if (!analysis)
                co_return;
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
                auto local = language::locate({.provider = analysis->provider,
                                               .argv = analysis->argv,
                                               .env = analysis->env,
                                               .initialization = analysis->initialization,
                                               .commands = analysis->commands,
                                               .executables = analysis->executables,
                                               .registration = analysis->registration,
                                               .documents = analysis->documents},
                                              dir, options_.tools, options_.parallel, cache);
                auto env = environment(r.env, local.env);
                const auto semantic_environment = env;
                const auto transport = analysis->registration
                                           ? analysis->registration->adapter
                                           : language::providers::dispatch(analysis->provider, []<class Provider>() { return Provider::adapter::transport; });
                if (transport == language::adapter_kind::helper)
                    env["FUGHETTA_OBJECTS"] = std::filesystem::absolute(options_.root / "exports").string();
                auto identity =
                    content::hash_of(rfl::json::write(std::tuple{analysis->provider, analysis->options, local.argv, env, local.initialization, local.commands,
                                                                 local.executables, options_.runtime, r.max_memory_bytes, r.max_cpu_seconds}))
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
                    auto runtime = options_.runtime;
                    std::map<std::string, std::string> executables;
                    for (std::size_t i = 0; i < local.executables.size(); ++i)
                        executables.emplace("executable:" + std::to_string(i), local.executables[i]);
                    if (!local.argv.empty())
                        executables.emplace("main", local.argv.front());
                    for (const auto& [name, argv] : local.commands)
                        executables.emplace("command:" + name, argv.front());
                    // In-process readers use the worker's observed runtime;
                    // there is no external executable subset to select.
                    if (!executables.empty())
                        runtime.execution = toolchain::select(runtime, executables, semantic_environment);
                    allocate_runtime(env);
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
                                                           std::move(local.commands), std::move(runtime),
                                                           language::document_loader{.root = dir, .scheduler = rt_.get_scheduler(home_), .pool = &pool_},
                                                           std::move(local.registration), {}, cache);
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
                auto generated = co_await locked([&](store& s) { return s.generated_inputs(r.project, r.tree); });
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
                if (!analysis->candidates.empty()) {
                    if (!messages)
                        throw std::logic_error("analysis reuse requires its gateway validator");
                    const auto current_inputs = co_await hot.server->current_inputs(analysis->candidates);
                    const auto decision = co_await messages->ask(jobs::reuse_request{analysis->execution, installed, current_inputs});
                    for (const auto& candidate : analysis->candidates)
                        if (std::ranges::contains(decision.accepted, reuse::key{candidate.id, semantic::recipe_identifier(candidate.recipe)}))
                            accepted.push_back(candidate);
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
                        const auto candidate = std::ranges::find_if(analysis->candidates, [&](const auto& candidate) {
                            return candidate.id == *unit.reused && candidate.recipe == semantic::recipe{unit.request.unit, installed.options};
                        });
                        if (candidate == analysis->candidates.end())
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

        // Close the process group after its processes have been released.
        auto run_steps(const jobs::build_request& r, const std::filesystem::path& dir, const std::filesystem::path& reports,
                       std::chrono::steady_clock::time_point deadline, bool analyzing, const jobs::analysis_link& models, delivery::pins& model_pins)
            -> coroutine::task<jobs::build_report> {
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
            for (auto& invocation : invocations) {
                auto& step = invocation.command;
                co_await install_models(step.inputs, dir, models, model_pins);
                elapsed measured{step.preparation ? *report.preparation_seconds : *report.execution_seconds};
                if (step.argv.empty())
                    throw std::invalid_argument(std::format("step '{}' has no command", step.name));
                const auto cache =
                    options_.dependency_cache.value_or(options_.root / (options_.dependency_publish ? "publisher/dependencies" : "dependencies"));
                auto local = co_await io::blocking(rt_.get_scheduler(home_), pool_, [this, step, dir, reports, cache] {
                    return builds::locate(step, dir, options_.tools, options_.parallel, reports, cache,
                                          options_.dependency_publish.value_or(options_.dependency_upstream.value_or(std::filesystem::path{})));
                });
                std::unique_ptr<durability::dir_lock> dependency_lock;
                bool cache_waited = false;
                double cache_wait_seconds = 0;
                if (auto found = step.env.find("VCPKG_BINARY_SOURCES"); found != step.env.end() && found->second.contains("${cache:vcpkg}")) {
                    const auto waiting_since = std::chrono::steady_clock::now();
                    while (std::chrono::steady_clock::now() < deadline) {
                        dependency_lock = co_await io::blocking(rt_.get_scheduler(home_), pool_, [cache = options_.dependency_publish.value_or(cache)] {
                            try {
                                return std::make_unique<durability::dir_lock>(cache / "vcpkg", "dependency cache");
                            } catch (const durability::lock_busy&) {
                                return std::unique_ptr<durability::dir_lock>{};
                            }
                        });
                        if (dependency_lock)
                            break;
                        cache_waited = true;
                        co_await execution::schedule_after(rt_.get_scheduler(home_), constants::dependency_lock_poll);
                    }
                    cache_wait_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - waiting_since).count();
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
                allocate_runtime(*cmd.env);
                cmd.processes = resources_.at(store::tree_key(r.project, r.tree))->processes.group();
                // Exporter inputs belong to preparation commands. Keeping them
                // out of language process identity preserves scoped SDK reuse.
                cmd.env->erase("FUGHETTA_STAGING");
                cmd.env->erase("FUGHETTA_SDKS");
                if (step.preparation) {
                    (*cmd.env)["FUGHETTA_STAGING"] = std::filesystem::absolute(reports / "objects").string();
                    (*cmd.env)["FUGHETTA_SDKS"] = rfl::json::write(options_.runtime.inputs);
                }
                if (step.generation)
                    co_await locked([&](store& s) { s.begin_generation(r.project, r.tree, *step.generation); });
                if (local.standard_output)
                    cmd.standard_output = reports / *local.standard_output;
                const auto log_name = std::to_string(report.steps.size()) + ".log";
                report.steps.push_back(co_await runner::run_step(rt_.get_scheduler(home_), std::move(cmd), left, r.output_limit, step.name,
                                                                 log_setup_error.empty() ? std::optional{logs / log_name} : std::nullopt,
                                                                 constants::log_bytes - log_bytes));
                auto& last = report.steps.back();
                last.tasks = invocation.tasks;
                if (dependency_lock && options_.dependency_publish && last.exit_code == 0 && !last.timed_out) {
                    try {
                        co_await io::blocking(rt_.get_scheduler(home_), pool_, [cache, destination = *options_.dependency_publish, deadline] {
                            dependencies::publish(cache, destination, deadline);
                        });
                    } catch (const std::exception& error) {
                        last.cache_error = std::string{error.what()}.substr(0, 512);
                    }
                }
                dependency_lock.reset();
                if (!log_setup_error.empty())
                    last.log_error = log_setup_error;
                try {
                    if (last.log_error.empty()) {
                        auto saved = co_await io::blocking(rt_.get_scheduler(home_), pool_,
                                                           [this, logs, log_name, root = options_.root,
                                                            publish = !analyzing && r.measure.empty() && (last.truncated || last.output.size() > 8192)] {
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
                        auto generated = rfl::json::read<manifest>(bytes);
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
        std::map<std::string, int> active_;
        std::map<session_key, std::unique_ptr<context>> contexts_;
        std::map<std::string, preparation> preparations_;
        std::chrono::steady_clock::time_point export_collect_{};
    };
} // namespace replica
