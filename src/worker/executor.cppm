export module executor;
import std;
import native;
import coroutine;
import replica;
import runner;
import protocol;
import host;
import lsp;
import constants;
import reflection;
import delivery;
import durability;
import dependencies;

export namespace replica {
    struct options {
        std::filesystem::path root = "work";
        std::string worker;
        replica::limits limits;
        std::map<std::string, std::string> env;
        bool inherit_env = true;
        builds::toolset tools;
        std::uint32_t parallel = 1;
        std::optional<std::filesystem::path> dependency_cache;
        std::optional<std::filesystem::path> dependency_upstream;
        std::optional<std::filesystem::path> dependency_publish;
        std::optional<builds::dependency_recipe> dependency_recipe;
    };

    class service {
    public:
        using socket = native::socket;
        using dispatcher = transport::dispatcher<socket>;

        service(cores::context<native::scheduler>& rt, options o, execution::thread_pool& pool, std::size_t home = 0)
            : rt_(rt), store_(o.root, o.limits), options_(std::move(o)), pool_(pool), home_(home) {}

        // The owner keeps the I/O runtime alive until all context-owned children are gone.
        auto run(std::size_t core, coroutine::task<> work) -> coroutine::task<> {
            auto token = co_await coroutine::current_token;
            bool stopped = false;
            std::exception_ptr failed;
            try {
                stopped = !(co_await coroutine::under(token, std::move(work)));
            } catch (...) {
                failed = util::current_exception();
            }
            auto finish = [this]() -> coroutine::task<> {
                std::exception_ptr failed;
                while (!contexts_.empty()) {
                    auto key = contexts_.begin()->first;
                    try {
                        co_await retire(key);
                    } catch (...) {
                        if (!failed)
                            failed = util::current_exception();
                    }
                }
                if (failed)
                    util::rethrow_exception(failed);
            };
            auto cleanup = [&]() -> coroutine::task<> {
                if (core == home_)
                    co_await finish();
                else
                    co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), finish);
            };
            co_await coroutine::under(coroutine::stop_token{}, cleanup());
            if (failed)
                util::rethrow_exception(failed);
            if (stopped)
                co_await coroutine::stop;
        }

        auto reclaim(std::size_t core, bool pressure = false) -> coroutine::task<> {
            if (core == home_)
                co_await collect_sessions(pressure);
            else
                co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), [this, pressure] { return collect_sessions(pressure); });
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
            d.template on<jobs::sync_offer>(
                [this, core](jobs::sync_offer o, auto&) {
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
            auto handle_build_request = [this, core](jobs::build_request r, auto& ex) -> coroutine::task<jobs::build_report> {
                require_consumer();
                co_await ex.progress({.stage = "checkout"});
                if (core == home_)
                    co_return co_await build(std::move(r));
                co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), [this, r = std::move(r)] { return build(r); });
            };
            d.template on<jobs::build_request>(std::move(handle_build_request));
            auto handle_dependency_fill = [this, core](jobs::dependency_fill request, auto& ex) -> coroutine::task<jobs::build_report> {
                co_await ex.progress({.stage = "dependencies"});
                if (core == home_) co_return co_await fill(std::move(request));
                co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_),
                    [this, request = std::move(request)] { return fill(request); });
            };
            d.template on<jobs::dependency_fill>(std::move(handle_dependency_fill));
        }

    private:
        void require_consumer() const {
            if (options_.dependency_publish) throw std::invalid_argument("dependency publishers only accept dependency.fill");
        }

        auto fill(jobs::dependency_fill request) -> coroutine::task<jobs::build_report> {
            if (!options_.dependency_publish || !options_.dependency_recipe)
                throw std::invalid_argument("this worker is not a dependency publisher");
            const auto recipe = *options_.dependency_recipe;
            builds::validate(recipe);
            builds::validate(request.manifest);
            if (request.triplet.empty()) request.triplet = recipe.triplets.front();
            if (request.host_triplet.empty()) request.host_triplet = recipe.triplets.front();
            if (!std::ranges::contains(recipe.triplets, request.triplet) || !std::ranges::contains(recipe.triplets, request.host_triplet))
                throw std::invalid_argument("dependency triplet is not admitted by the publisher");
            if (request.features.size() > constants::dependency_manifest_entries)
                throw std::invalid_argument("too many dependency features");
            for (const auto& feature : request.features) builds::dependency_name(feature);
            if (!request.timeout_s) throw std::invalid_argument("dependency fill deadline expired");
            const auto text = rfl::json::write(request.manifest);
            const auto digest = content::hash_of(text);
            const content::manifest snapshot{{content::entry{.path = "vcpkg.json", .digest = digest, .size = text.size()}}};
            co_await locked([snapshot, digest, text](store& cache) {
                cache.offer({"dependencies", snapshot});
                auto bytes = std::as_bytes(std::span{text});
                cache.put(jobs::source_bytes{{jobs::source_part{digest, 0, text.size(), {bytes.begin(), bytes.end()}}}});
            });
            builds::adapter<"vcpkg">::options options{.executable = recipe.executable, .root = recipe.root,
                .install_dir = "installed", .triplet = request.triplet, .host_triplet = request.host_triplet,
                .features = request.features, .buildtrees_dir = "buildtrees", .packages_dir = "packages", .env = recipe.env, .offline = recipe.offline,
                .default_features = request.default_features};
            builds::adapter<"vcpkg">::validate(options);
            const auto key = content::hash_of(rfl::json::write(std::tuple{request, recipe}));
            jobs::build_request build_request{.project = "dependencies", .tree = key.str(), .snapshot = snapshot.id(),
                .steps = builds::adapter<"vcpkg">::expand(options, "install", {}),
                .timeout_s = std::min(request.timeout_s, recipe.timeout_s), .fresh = true};
            build_request.steps.front().dependencies.reset();
            for (const auto& path : recipe.overlay_ports) build_request.steps.front().argv.push_back("--overlay-ports=" + path);
            for (const auto& path : recipe.overlay_triplets) build_request.steps.front().argv.push_back("--overlay-triplets=" + path);
            co_return co_await build(std::move(build_request));
        }

        struct context {
            std::unique_ptr<lsp::session> server;
            std::string identity;
            std::chrono::steady_clock::time_point used;
        };

        auto retire(const std::string& key) -> coroutine::task<> {
            auto found = contexts_.find(key);
            if (found == contexts_.end())
                co_return;
            auto old = std::move(found->second);
            contexts_.erase(found);
            std::exception_ptr failed;
            try {
                co_await old->server->abort();
            } catch (...) {
                failed = util::current_exception();
            }
            old->server.reset();
            if (failed)
                util::rethrow_exception(failed);
        }

        auto collect_sessions(bool pressure = false) -> coroutine::task<> {
            const auto collection_time = std::chrono::steady_clock::now();
            if (collection_time >= export_collect_) {
                export_collect_ = collection_time + constants::export_collect_interval;
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [this, root = options_.root] {
                    std::unique_lock guard{exports_lock_};
                    delivery::collect(root / "exports");
                });
            }
            std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> idle;
            for (const auto& [key, value] : contexts_)
                if (!active_.contains(key))
                    idle.emplace_back(value->used, key);
            std::ranges::sort(idle);
            auto now = std::chrono::steady_clock::now();
            for (const auto& [used, key] : idle) {
                auto found = contexts_.find(key);
                if (found != contexts_.end() && found->second->used == used && !active_.contains(key) &&
                    (pressure || now - used >= constants::language_idle ||
                     contexts_.size() >= constants::language_sessions))
                    co_await retire(key);
            }
        }

        template <class F> struct home_work {
            service* owner;
            F work;
            auto operator()() -> coroutine::task<std::invoke_result_t<F&, store&>> {
                return owner->locked(std::move(work));
            }
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

        auto build(jobs::build_request r) -> coroutine::task<jobs::build_report> {
            co_await collect_sessions();
            auto started = std::chrono::steady_clock::now();
            auto key = store::tree_key(r.project, r.tree);
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
            struct holding {
                std::map<std::string, int>& active;
                std::string key;
                ~holding() {
                    if (!--active.at(key))
                        active.erase(key);
                }
            } hold{active_, key};
            ++active_[key];
            if (r.fresh)
                co_await retire(key);
            bool preparation = !r.measure.empty() || r.survey;
            // A preparation keeps the snapshot's files, which the offer cache may let go while the steps run.
            auto [dir, reports, files, retired] = co_await locked([project = r.project, snapshot = r.snapshot, tree = r.tree, key, preparation, fresh = r.fresh](store& s) {
                auto retired = fresh ? s.detach(project, tree) : std::filesystem::path{};
                auto dir = s.checkout(project, snapshot, tree);
                return std::tuple{dir, s.reports(key), preparation ? s.files(project, snapshot) : std::vector<content::entry>{}, retired};
            });
            if (!retired.empty())
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [retired] { store::discard(retired); });
            auto deadline = started + std::chrono::seconds{r.timeout_s};
            auto token = co_await coroutine::current_token;
            std::optional<jobs::build_report> report;
            std::exception_ptr failed;
            try {
                report = co_await coroutine::under(token, run_steps(r, dir, reports, deadline));
                if (report && report->ok() && preparation) {
                    auto io = rt_.get_scheduler(home_);
                    auto remaining = std::max(std::chrono::steady_clock::duration::zero(), deadline - std::chrono::steady_clock::now());
                    auto finished = co_await coroutine::under(token, coroutine::deadline(io, remaining, afterwards(r, dir, files, *report)));
                    if (!finished)
                        report.reset();
                    else if (!*finished)
                        report->steps.push_back(
                            {.name = "analysis", .timed_out = true, .output = "execution deadline exceeded during artifact or language analysis"});
                }
            } catch (...) {
                failed = util::current_exception();
            }
            if (r.survey && (failed || !report || !report->ok()))
                co_await coroutine::under(coroutine::stop_token{}, retire(key));
            if (failed)
                util::rethrow_exception(failed);
            if (!report)
                co_await coroutine::stop;
            if (!r.exports.empty()) {
                try {
                    auto captured = co_await io::blocking(rt_.get_scheduler(home_), pool_, [this, root = options_.root, dir, exports = r.exports, allow_missing = !report->ok()] {
                        std::shared_lock guard{exports_lock_};
                        std::vector<std::string> skipped;
                        auto files = delivery::capture(dir, exports, root / "exports", allow_missing, &skipped);
                        return std::pair{std::move(files), std::move(skipped)};
                    });
                    report->exports = std::move(captured.first);
                    report->export_skipped = std::move(captured.second);
                } catch (const std::exception& error) {
                    report->export_error = std::string{error.what()}.substr(0, 512);
                }
            }
            report->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            co_return std::move(*report);
        }

        // Once every step succeeded: the artifacts the request names are measured, and the language server it names surveys the tree's
        // documents. A survey that fails is reported, not thrown: the build itself succeeded.
        auto afterwards(const jobs::build_request& r, const std::filesystem::path& dir, const std::vector<content::entry>& files, jobs::build_report& report)
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
            if (!r.survey)
                co_return;
            try {
                lsp::limits bounds{.total = std::chrono::seconds{r.survey->seconds}};
                auto documents = co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] {
                    std::vector<lsp::document> out;
                    std::uint64_t bytes = 0;
                    for (auto& file : files) {
                        auto language = language::providers::document(r.survey->provider, file.path);
                        if (!language)
                            continue;
                        if (file.size > bounds.document_bytes)
                            throw std::runtime_error(std::format("{} is larger than the {} bytes a survey reads", file.path, bounds.document_bytes));
                        bytes += file.size;
                        std::ifstream in{dir / file.path, std::ios::binary};
                        std::string text{std::istreambuf_iterator<char>{in}, {}};
                        // A file that holds a NUL is not text, whatever its name.
                        if (text.find('\0') != std::string::npos) {
                            bytes -= file.size;
                            continue;
                        }
                        out.push_back({file.path, std::string{*language}, std::move(text)});
                    }
                    if (out.size() > bounds.documents || bytes > bounds.bytes)
                        throw std::runtime_error(std::format("the survey covers at most {} documents and {} bytes", bounds.documents, bounds.bytes));
                    return out;
                });
                for (auto& d : documents)
                    report.surveyed.push_back(d.path);
                auto local =
                    language::locate({.provider = r.survey->provider, .argv = r.survey->argv, .env = r.survey->env}, dir, options_.tools, options_.parallel);
                auto env = environment(r.env, local.env);
                auto identity = content::hash_of(rfl::json::write(std::tuple{local.argv, env, r.survey->initialization, r.max_memory_bytes})).str();
                auto key = store::tree_key(r.project, r.tree);
                if (auto found = contexts_.find(key); found != contexts_.end() && found->second->identity != identity)
                    co_await retire(key);
                if (!contexts_.contains(key)) {
                    auto value = std::make_unique<context>();
                    value->identity = identity;
                    value->used = std::chrono::steady_clock::now();
                    io::command command{.argv = std::move(local.argv),
                                        .cwd = dir,
                                        .env = std::move(env)};
                    value->server = std::make_unique<lsp::session>(rt_.get_scheduler(home_), std::move(command));
                    auto& created = *contexts_.emplace(key, std::move(value)).first->second;
                    co_await created.server->start(dir, r.survey->initialization);
                }
                auto& hot = *contexts_.at(key);
                hot.used = std::chrono::steady_clock::now();
                std::vector<std::string> changed;
                for (const auto& output : report.measured)
                    changed.push_back(output.path);
                co_await hot.server->synchronize(documents, changed);
                semantic::outline outline;
                std::size_t records = 0;
                for (const auto& document : documents) {
                    auto result = co_await hot.server->describe(document);
                    outline.complete = outline.complete && result.complete;
                    outline.cacheable = outline.cacheable && result.cacheable;
                    records += result.scopes.size() + result.symbols.size() + result.diagnostics.size();
                    if (records > bounds.records)
                        throw std::runtime_error("language result exceeds the record limit");
                    std::ranges::move(result.scopes, std::back_inserter(outline.scopes));
                    std::ranges::move(result.symbols, std::back_inserter(outline.symbols));
                    std::ranges::move(result.diagnostics, std::back_inserter(outline.diagnostics));
                }
                hot.used = std::chrono::steady_clock::now();
                if (r.survey->query.action == "definitions" || r.survey->query.action == "references")
                    report.locations = co_await hot.server->lookup(r.survey->query);
                report.outline = std::move(outline);
            } catch (const lsp::query_error& e) {
                report.survey_error = e.what();
                report.survey_error_code = "invalid_record";
            } catch (const std::exception& e) {
                report.survey_error = e.what();
                report.surveyed.clear();
            }
            if (report.survey_error && report.survey_error_code != "invalid_record")
                co_await retire(store::tree_key(r.project, r.tree));
        }

        // Close the process group after its processes have been released.
        auto run_steps(const jobs::build_request& r, const std::filesystem::path& dir, const std::filesystem::path& reports,
                       std::chrono::steady_clock::time_point deadline) -> coroutine::task<jobs::build_report> {
            jobs::build_report report{.worker = options_.worker, .tree = dir.string()};
            const auto logs = reports.parent_path() / (reports.filename().string() + ".logs");
            std::string log_setup_error;
            try {
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [logs] {
                    store::discard(logs);
                    std::filesystem::create_directories(logs);
                });
            } catch (const std::exception& error) { log_setup_error = error.what(); }
            std::uint64_t log_bytes = 0;
            for (auto& step : r.steps) {
                if (step.argv.empty())
                    throw std::invalid_argument(std::format("step '{}' has no command", step.name));
                const auto cache = options_.dependency_cache.value_or(options_.root / (options_.dependency_publish ? "publisher/dependencies" : "dependencies"));
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
                            try { return std::make_unique<durability::dir_lock>(cache / "vcpkg", "dependency cache"); }
                            catch (const durability::lock_busy&) { return std::unique_ptr<durability::dir_lock>{}; }
                        });
                        if (dependency_lock) break;
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
                    report.steps.push_back({.name = step.name, .timed_out = true,
                        .output = cache_waited ? std::format("not started: waited {:.2f} seconds for the shared vcpkg cache lock held by another install; execution deadline exceeded", cache_wait_seconds)
                                               : "not started: the build's time ran out"});
                    break;
                }
                io::command cmd{.argv = std::move(local.argv),
                                .cwd = cwd,
                                .env = environment(r.env, local.env),
                                .max_memory_bytes = r.max_memory_bytes,
                                .max_cpu_seconds = r.max_cpu_seconds};
                if (local.standard_output)
                    cmd.standard_output = reports / *local.standard_output;
                const auto log_name = std::to_string(report.steps.size()) + ".log";
                report.steps.push_back(co_await runner::run_step(rt_.get_scheduler(home_), std::move(cmd), left, r.output_limit, step.name,
                    log_setup_error.empty() ? std::optional{logs / log_name} : std::nullopt, constants::log_bytes - log_bytes));
                auto& last = report.steps.back();
                if (dependency_lock && options_.dependency_publish && last.exit_code == 0 && !last.timed_out) {
                    try {
                        co_await io::blocking(rt_.get_scheduler(home_), pool_, [cache, destination = *options_.dependency_publish, deadline] {
                            dependencies::publish(cache, destination, deadline);
                        });
                    } catch (const std::exception& error) { last.cache_error = std::string{error.what()}.substr(0, 512); }
                }
                dependency_lock.reset();
                if (!log_setup_error.empty()) last.log_error = log_setup_error;
                try {
                    if (last.log_error.empty()) {
                        auto saved = co_await io::blocking(rt_.get_scheduler(home_), pool_, [this, logs, log_name, root = options_.root,
                            publish = !r.survey && r.measure.empty() && (last.truncated || last.output.size() > 8192)] {
                            std::shared_lock guard{exports_lock_};
                            const auto size = std::filesystem::file_size(logs / log_name);
                            std::optional<builds::exported_file> file;
                            if (publish && size) file = delivery::capture(logs, {{"step-" + log_name, log_name}}, root / "exports").at(0);
                            std::filesystem::remove(logs / log_name);
                            return std::pair{size, file};
                        });
                        log_bytes += saved.first;
                        last.log = std::move(saved.second);
                    }
                } catch (const std::exception& error) { last.log_error = error.what(); log_bytes = constants::log_bytes; }
                auto found = co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] { return builds::collect(local, reports, dir); });
                last.diagnostics = std::move(found.diagnostics);
                last.tests = std::move(found.tests);
                if (last.exit_code != 0 || last.timed_out)
                    break;
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] {
                    for (auto& output : step.outputs)
                        builds::inspect(output, dir);
                });
            }
            co_return report;
        }

        cores::context<native::scheduler>& rt_;
        store store_;
        options options_;
        execution::thread_pool& pool_;
        std::size_t home_;
        coroutine::mutex store_lock_;
        std::shared_mutex exports_lock_;
        std::unordered_map<std::string, std::weak_ptr<coroutine::mutex>> tree_locks_;
        std::map<std::string, int> active_;
        std::map<std::string, std::unique_ptr<context>> contexts_;
        std::chrono::steady_clock::time_point export_collect_{};
    };
} // namespace replica
