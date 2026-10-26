export module executor;
import std;
import native;
import coroutine;
import replica;
import runner;
import protocol;
import host;
import isolation;
import lsp;

export namespace replica {
    struct options {
        std::filesystem::path root = "work";
        std::string worker;
        replica::limits limits;
        std::map<std::string, std::string> env;
        bool inherit_env = true;
        builds::toolset tools;
        bool isolate = false;
        std::uint32_t parallel = 1;
    };

    class service {
    public:
        using socket = native::socket;
        using dispatcher = transport::dispatcher<socket>;

        service(cores::context<native::scheduler>& rt, options o, execution::thread_pool& pool, std::size_t home = 0)
            : rt_(rt), store_(o.root, o.limits), options_(std::move(o)), pool_(pool), home_(home) {}

        auto prepare(std::size_t core) -> coroutine::task<> {
            if (core == home_)
                co_await prepare_resources();
            else
                co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), [this] { return prepare_resources(); });
        }

        template <io::stream S> void install(transport::dispatcher<S>& d, std::size_t core) {
            d.template on<jobs::sync_offer>(
                [this, core](jobs::sync_offer o, auto&) { return at_home(core, [o = std::move(o)](store& s) { return s.offer(o); }); });
            d.template on<jobs::source_bytes>([this, core](jobs::source_bytes p, auto&) -> coroutine::task<jobs::authority_ack> {
                if (core == home_)
                    co_return co_await push(std::move(p));
                co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), [this, p = std::move(p)] { return push(p); });
            });
            d.template on<jobs::build_request>([this, core](jobs::build_request r, auto& ex) -> coroutine::task<jobs::build_report> {
                co_await ex.progress({.stage = "checkout"});
                if (core == home_)
                    co_return co_await build(std::move(r));
                co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), [this, r = std::move(r)] { return build(r); });
            });
        }

    private:
        auto prepare_resources() -> coroutine::task<> {
            if (!options_.isolate)
                co_return;
            auto held = co_await resources_lock_.lock();
            if (!resources_)
                resources_ = co_await io::blocking(rt_.get_scheduler(home_), pool_, [this] {
                    auto key = content::hash_of(std::filesystem::canonical(options_.root).string());
                    return std::make_shared<isolation::pool>(key);
                });
        }

        template <class F> auto at_home(std::size_t core, F f) -> coroutine::task<std::invoke_result_t<F&, store&>> {
            if (core == home_)
                co_return co_await locked(std::move(f));
            co_return co_await cores::call(rt_.get_scheduler(core), rt_.get_scheduler(home_), [this, f] { return locked(f); });
        }
        template <class F> auto locked(F f) -> coroutine::task<std::invoke_result_t<F&, store&>> {
            auto held = co_await store_lock_.lock();
            co_return co_await io::blocking(rt_.get_scheduler(home_), pool_, [this, f = std::move(f)]() mutable { return f(store_); });
        }

        auto push(jobs::source_bytes p) -> coroutine::task<jobs::authority_ack> {
            std::set<std::string> busy;
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
            auto started = std::chrono::steady_clock::now();
            auto key = store::tree_key(r.project, r.tree);
            auto lock = tree_locks_[key].lock();
            if (!lock) {
                lock = std::make_shared<coroutine::mutex>();
                tree_locks_[key]=lock;
            }
            struct tree_lifetime {
                std::unordered_map<std::string,std::weak_ptr<coroutine::mutex>>& locks;
                std::string key;
                std::shared_ptr<coroutine::mutex> lock;
                ~tree_lifetime() { if (lock.use_count()==1) locks.erase(key); }
            } lifetime{tree_locks_,key,std::move(lock)};
            auto turn = co_await lifetime.lock->lock();
            struct holding {
                std::map<std::string, int>& active;
                std::string key;
                ~holding() { if (!--active.at(key)) active.erase(key); }
            } hold{active_, key};
            ++active_[key];
            bool preparation = !r.replaced.empty() || !r.measure.empty() || r.survey;
            // A preparation keeps the snapshot's files, which the offer cache may let go while the steps run.
            auto [dir, files] = co_await locked([project = r.project, snapshot = r.snapshot, tree = r.tree, preparation](store& s) {
                auto dir = s.checkout(project, snapshot, tree);
                return std::pair{dir, preparation ? s.files(project, snapshot) : std::vector<content::entry>{}};
            });
            if (!r.replaced.empty())
                co_await io::blocking(rt_.get_scheduler(home_), pool_, [&] { replace(dir, r.replaced); });
            std::shared_ptr<isolation::scope> resources;
            if (options_.isolate) {
                co_await prepare_resources();
                resources =
                    co_await io::blocking(rt_.get_scheduler(home_), pool_, [this, &r] { auto resource = resources_->start(r.max_memory_bytes, r.max_cpu_seconds); resource->concurrency(options_.parallel); return resource; });
            } else if (r.max_memory_bytes || r.max_cpu_seconds) {
                throw std::runtime_error("build resource limits require isolation; command not started");
            }
            auto deadline = started + std::chrono::seconds{r.timeout_s};
            auto token = co_await coroutine::current_token;
            std::optional<jobs::build_report> report;
            std::exception_ptr failed;
            try {
                report = co_await coroutine::under(token, run_steps(r, dir, resources.get(), deadline));
                if (report && report->ok() && preparation &&
                    !co_await coroutine::under(token, afterwards(r, dir, files, *report, resources.get())))
                    report.reset();
            } catch (...) {
                failed = std::current_exception();
            }
            if (resources) {
                try {
                    co_await coroutine::under(coroutine::stop_token{}, close_resources(resources));
                } catch (...) {
                    failed = std::current_exception();
                }
            }
            if (failed)
                std::rethrow_exception(failed);
            if (!report)
                co_await coroutine::stop;
            report->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            co_return std::move(*report);
        }

        // Once every step succeeded: the artifacts the request names are measured, and the language server it names surveys the tree's
        // documents. A survey that fails is reported, not thrown: the build itself succeeded.
        auto afterwards(const jobs::build_request& r, const std::filesystem::path& dir, const std::vector<content::entry>& files, jobs::build_report& report, isolation::scope* resources)
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
                auto local = language::locate({.provider = r.survey->provider, .argv = r.survey->argv, .env = r.survey->env},
                                              dir, options_.tools, options_.parallel);
                io::command server{.argv = std::move(local.argv), .cwd = dir, .env = environment(r.env, local.env),
                                   .isolation = resources ? std::optional{resources->target()} : std::nullopt};
                report.outline = co_await lsp::survey(rt_.get_scheduler(home_), std::move(server), r.survey->initialization, dir, std::move(documents), bounds);
            } catch (const std::exception& e) {
                report.survey_error = e.what();
                report.surveyed.clear();
            }
        }

        // Removes what the steps must write anew, so an old copy is never taken for theirs; only a plain file or a directory is removed.
        static void replace(const std::filesystem::path& dir, const std::vector<std::string>& paths) {
            for (auto& relative : paths) {
                content::checked_path(relative);
                auto path = dir;
                for (const auto& component : std::filesystem::path(relative)) {
                    path /= component;
                    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
                        throw std::invalid_argument(relative + " goes through a symbolic link");
                }
                auto status = std::filesystem::symlink_status(path);
                if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status))
                    throw std::invalid_argument(relative + " must be a regular file or a directory");
                std::filesystem::remove_all(path);
            }
        }

        auto close_resources(std::shared_ptr<isolation::scope> resources) -> coroutine::task<> {
            co_await io::blocking(rt_.get_scheduler(home_), pool_, [resources] { resources->close(); });
        }

        auto run_steps(const jobs::build_request& r, const std::filesystem::path& dir, isolation::scope* resources,
                       std::chrono::steady_clock::time_point deadline) -> coroutine::task<jobs::build_report> {
            jobs::build_report report{.worker = options_.worker, .tree = dir.string()};
            for (auto& step : r.steps) {
                if (step.argv.empty())
                    throw std::invalid_argument(std::format("step '{}' has no command", step.name));
                auto local = builds::locate(step, dir, options_.tools, options_.parallel);
                auto cwd = std::filesystem::path(local.cwd);
                std::error_code ignored;
                std::filesystem::create_directories(cwd, ignored);
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (left <= std::chrono::milliseconds::zero()) {
                    report.steps.push_back({.name = step.name, .timed_out = true, .output = "not started: the build's time ran out"});
                    break;
                }
                io::command cmd{.argv = std::move(local.argv),
                                .cwd = cwd,
                                .env = environment(r.env, local.env),
                                .max_memory_bytes = r.max_memory_bytes,
                                .max_cpu_seconds = r.max_cpu_seconds,
                                .isolation = resources ? std::optional{resources->target()} : std::nullopt};
                report.steps.push_back(co_await runner::run_step(rt_.get_scheduler(home_), std::move(cmd), left, r.output_limit, step.name, resources));
                auto& last = report.steps.back();
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
        std::shared_ptr<isolation::pool> resources_;
        coroutine::mutex resources_lock_;
        execution::thread_pool& pool_;
        std::size_t home_;
        coroutine::mutex store_lock_;
        std::unordered_map<std::string, std::weak_ptr<coroutine::mutex>> tree_locks_;
        std::map<std::string, int> active_;
    };
} // namespace replica
