export module registry;
import constants;
import std;
export import utility;
import coroutine;
import content;
import configuration;
import usage;
import durability;
import protocol;
import client;
import async;
import native;

export namespace storage {
    struct project_ctx {
        const config::document& config;
        config::effective settings;
        usage::ledger& ledger;
    };

    class registry {
    public:
        registry(cores::context<native::scheduler>& rt, std::filesystem::path storage, const config::slots& config, execution::thread_pool& pool,
                 net::ip::tcp::endpoint source)
            : rt_(rt), storage_(std::move(storage)), lock_(storage_, "storage"), config_(config), pool_(pool), source_(source) {}

        std::function<void()> changed;

        auto node(std::size_t here, std::string project, nodes::node_request request, worlds::actor actor, bool reading) -> coroutine::task<nodes::node_reply> {
            // The source opens a repository for any project it is asked about, so only configured projects reach it.
            auto doc = config_.current(here);
            if (!doc->find(project))
                throw config::not_found(std::format("unknown project '{}'", project));
            // Limits come from the configuration, whatever the caller sent.
            auto limits = config::resolve(*doc, project).limits;
            request.file_limit = limits.max_file_bytes;
            request.project_limit = limits.max_project_bytes;
            request.workspace_limit = limits.max_workspace_bytes;
            auto out = co_await remote(here, jobs::node_call{std::move(project), std::move(actor), std::move(request), reading});
            if (!reading && changed)
                changed();
            co_return out;
        }

        auto analysis(std::size_t here, std::string project, language::request request) -> coroutine::task<language::result> {
            completed(here, project, request);
            co_return co_await remote(here, std::move(request));
        }

        // A preparation a worker ran, for the source to publish and keep; its request is completed as `begin`'s was.
        auto analysis(std::size_t here, std::string project, jobs::analysis_finish done) -> coroutine::task<language::result> {
            completed(here, project, done.request);
            co_return co_await remote(here, std::move(done));
        }

        // What the source stores for each project.
        auto usage(std::size_t here, worlds::actor actor) -> coroutine::task<jobs::node_usage_reply> {
            co_return co_await remote(here, jobs::node_usage{std::move(actor)});
        }

        auto capture(std::size_t here, std::string project, std::string operation, std::vector<std::string> expected, std::string intent, worlds::actor actor)
            -> coroutine::task<nodes::node_reply> {
            auto settings = config::resolve(*config_.current(here), project);
            auto out = co_await remote(here, jobs::node_capture{std::move(project), settings.ignore, std::move(actor), std::move(operation),
                                                                std::move(expected), std::move(intent), settings.limits.max_file_bytes,
                                                                settings.limits.max_project_bytes, settings.limits.max_workspace_bytes});
            if (changed)
                changed();
            co_return out;
        }

        template <class F>
            requires std::copy_constructible<F>
        auto with(std::size_t here, std::string project, F f) -> coroutine::task<std::invoke_result_t<F&, project_ctx&>> {
            co_return co_await with_ledger(here, [project = std::move(project), f](usage::ledger& ledger, const config::document& d) mutable {
                project_ctx ctx{d, config::resolve(d, project), ledger};
                return f(ctx);
            });
        }

        // The usage ledger lives on core zero; every use of it runs there, one at a time, on the disk pool.
        template <class F>
            requires std::copy_constructible<F>
        auto with_ledger(std::size_t here, F f) -> coroutine::task<std::invoke_result_t<F&, usage::ledger&, const config::document&>> {
            if (here == 0)
                co_return co_await serial(std::move(f));
            co_return co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(0), [this, f] { return serial(f); });
        }

        // Runs `f` on the ledger later, without waiting for it.
        template <class F> void post(std::size_t here, F f) {
            auto start = [this, f = std::move(f)] mutable {
                rt_.spawn(0, serial([f = std::move(f)](usage::ledger& ledger, const config::document&) mutable { f(ledger); }));
            };
            if (here == 0)
                start();
            else
                rt_.post(0, std::move(start));
        }

    private:
        void completed(std::size_t here, const std::string& project, language::request& request) {
            auto settings = config::resolve(*config_.current(here), project);
            request.project = project;
            request.ignore = settings.ignore;
            // The environment the analysis is published under is what the language tool queries and scope locks name.
            request.environment = content::hash_of(config::write(request.config));
        }

        template <class Request> auto remote(std::size_t here, Request request) -> coroutine::task<jobs::reply_t<Request>> {
            auto stop = co_await coroutine::current_token;
            try {
                co_return co_await io::blocking(rt_.get_scheduler(here), pool_, [this, request = std::move(request), stop]() mutable {
                    return source::call(source_, std::move(request), stop);
                });
            } catch (const source::stopped&) {
            }
            co_await coroutine::stop;
            std::unreachable();
        }

        template <class F> auto serial(F f) -> coroutine::task<std::invoke_result_t<F&, usage::ledger&, const config::document&>> {
            auto held = co_await ledger_lock_.lock();
            auto doc = config_.current(0);
            co_return co_await io::blocking(rt_.get_scheduler(0), pool_, [this, f = std::move(f), doc]() mutable {
                if (!ledger_) {
                    ledger_ = std::make_unique<usage::ledger>(storage_ / constants::source_ledger_directory);
                    ledger_->changed = [this] {
                        if (changed)
                            changed();
                    };
                    if (auto& r = ledger_->recovery())
                        logging::warn("usage ledger: " + *r);
                }
                // A refusal leaves the ledger as it was; any other failure reopens it from its log, so running builds stay counted.
                try {
                    return f(*ledger_, *doc);
                } catch (const usage::quota_exceeded&) {
                    throw;
                } catch (const std::invalid_argument&) {
                    throw;
                } catch (...) {
                    ledger_.reset();
                    throw;
                }
            });
        }

        cores::context<native::scheduler>& rt_;
        std::filesystem::path storage_;
        durability::dir_lock lock_;
        const config::slots& config_;
        execution::thread_pool& pool_;
        net::ip::tcp::endpoint source_;
        coroutine::mutex ledger_lock_;
        std::unique_ptr<usage::ledger> ledger_;
    };
} // namespace storage
