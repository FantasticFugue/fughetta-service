export module gateway;
import std;
import environment;
import coroutine;
import native;
import signals;
import configuration;
import registry;
import fleet;
import tools;
import front;
import keyring;
import usage;
import protocol;
import reflection;
import runtime;
import host;
import enrollment;

namespace gateway {
    class node {
    public:
        using socket = native::socket;
        using listener = native::listener;

        node(config::document doc, std::optional<std::filesystem::path> file, cores::context<native::scheduler>& rt, std::filesystem::path executable = {},
             std::atomic<bool>* interrupted = nullptr)
            : interrupted_(interrupted), executable_(std::move(executable)), doc_(doc), server_(doc.server), rt_(rt),
              control_(doc.runtime, doc.revisions.runtime), io_pool_(allocation::distribute(doc.runtime).gateway_disk),
              hub_(rt, config::store(std::move(doc), std::move(file)), io_pool_), identities_(server_.storage, server_.auth.workspace),
              router_(rt, routing::stale_after, &hub_), files_(rt, server_.storage, hub_, io_pool_, io::parse_endpoint(server_.source)) {
            files_.changed = [this] { hub_.events().publish(6); };
            identities_.changed = [this] { hub_.events().publish(1); };
            for (std::size_t c = 0; c < rt.size(); ++c)
                router_.workers(c).changed = [this] { hub_.events().publish(1); };
            if (server_.auth.mode == "token" && server_.auth.tokens.empty())
                logging::warn("gateway: no bearer token is configured; create one with `fughetta-service token NAME owner`");
            for (std::size_t c = 0; c < rt.size(); ++c)
                cores_.push_back(std::make_unique<per_core>(*this, c));
            queue_ = std::make_unique<tools::backlog>(rt, cores_[0]->services);
            for (auto& c : cores_)
                c->services.queue = queue_.get();
        }

        auto run(native::scheduler& io, std::size_t core) -> coroutine::task<> {
            auto& here = *cores_[core];
            runtime::group processes;
            auto http = native::listener{io, io::parse_endpoint(server_.http_bind, server_.http_port)};
            auto workers = native::listener{io, io::parse_endpoint(server_.worker_bind, server_.worker_port)};
            std::optional<listener> remote;
            if (server_.remote_worker_port)
                remote.emplace(native::listener{io, io::parse_endpoint(server_.remote_worker_bind, server_.remote_worker_port)});
            if (core == 0)
                logging::info(std::format("gateway: source and language API on http://{}:{}/, source {}, metadata {}, {} cores", server_.http_bind,
                                          server_.http_port, server_.source, server_.storage, rt_.size()));
            auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                if (core == 0 && !executable_.empty()) {
                    co_await processes.start(io, children, executable_, doc_);
                    daemon::publish(daemon::phase::initialized);
                    children.spawn(supervise(io, processes));
                }
                children.spawn(router_.workers(core).watch(io));
                children.spawn(accept_workers(workers, core, children));
                if (remote)
                    children.spawn(accept_workers(*remote, core, children, true));
                if (core == 0) {
                    children.spawn(queue_->serve(io));
                    if (!executable_.empty())
                        children.spawn(watch_runtime(io));
                }
                co_await accept_http(http, here, children);
            };
            co_await coroutine::with_scope(std::move(run_children));
        }

        auto reconfiguring() const -> bool { return reconfigure_; }

    private:
        // A child that exits unasked stops the whole service.
        auto supervise(native::scheduler& io, runtime::group& processes) -> coroutine::task<> {
            try {
                co_await processes.supervise(io);
            } catch (...) {
                daemon::publish(daemon::phase::stopping);
                rt_.stop();
                throw;
            }
        }

        auto watch_runtime(native::scheduler& io) -> coroutine::task<> {
            std::optional<std::chrono::steady_clock::time_point> deadline;
            for (;;) {
                if (interrupted_ && interrupted_->load()) {
                    control_.drain();
                    daemon::publish(daemon::phase::draining);
                    if (!deadline)
                        deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                    if ((control_.idle() && queue_->idle()) || std::chrono::steady_clock::now() >= *deadline) {
                        daemon::publish(daemon::phase::stopping);
                        rt_.stop();
                        co_return;
                    }
                    co_await execution::schedule_after(io, std::chrono::milliseconds{50});
                    continue;
                }
                auto current = hub_.current(0);
                if (current->runtime != doc_.runtime) {
                    control_.drain();
                    daemon::publish(daemon::phase::draining);
                    if (control_.idle() && queue_->idle()) {
                        reconfigure_ = true;
                        rt_.stop();
                        co_return;
                    }
                } else {
                    auto workers = router_.workers(0).describe(0);
                    if (std::ranges::count_if(workers, [](const auto& w) { return w.online; }) == doc_.runtime.workers) {
                        control_.ready(current->revisions.runtime);
                        daemon::publish(daemon::phase::ready);
                    }
                }
                co_await execution::schedule_after(io, std::chrono::milliseconds{50});
            }
        }

        struct per_core {
            per_core(node& n, std::size_t core)
                : services{n.hub_.slot(core), n.router_, n.files_, core},
                  http(front::context{n.hub_, n.files_, n.router_, n.rt_, core, &n.requests_, nullptr, &n.io_pool_, &services,
                                      n.executable_.empty() ? nullptr : &n.control_, &n.identities_, n.server_.remote_worker_port != 0},
                       services) {}
            tools::services services;
            front::server http;
        };

        auto accept_http(listener& l, per_core& here, coroutine::scope& children) -> coroutine::task<> {
            for (;;) {
                auto [connection, peer] = co_await net::async_accept(l);
                children.spawn(here.http.serve(std::move(connection)));
            }
        }

        auto accept_workers(listener& l, std::size_t core, coroutine::scope& children, bool remote = false) -> coroutine::task<> {
            for (;;) {
                auto [connection, peer] = co_await net::async_accept(l);
                children.spawn(router_.workers(core).admit(rt_.get_scheduler(core), std::move(connection), remote ? &identities_ : nullptr,
                                                           remote ? &io_pool_ : nullptr,
                                                           [this, core] { return hub_.current(core)->server.remote_worker_enrollment; }));
            }
        }

        std::atomic<bool>* interrupted_;
        std::filesystem::path executable_;
        config::document doc_;
        config::server server_;
        cores::context<native::scheduler>& rt_;
        allocation::control control_;
        bool reconfigure_ = false;
        execution::thread_pool io_pool_;
        front::config_hub hub_;
        front::in_flight requests_{rt_};
        enrollment::store identities_;
        routing::router router_;
        storage::registry files_;
        std::vector<std::unique_ptr<per_core>> cores_;
        std::unique_ptr<tools::backlog> queue_;
    };
} // namespace gateway

// `gateway token NAME ROLE [config.json]` adds a bearer token to the configuration and prints it once; only its hash is kept.
auto add_token(std::span<char*> args) -> int {
    if (args.size() < 2 || args.size() > 3)
        throw std::invalid_argument("usage: fughetta-service token NAME owner|member [config.json]");
    std::string role = args[1];
    std::filesystem::path file = args.size() > 2 ? args[2] : "config.json";
    config::store store{std::filesystem::exists(file) ? config::load(file) : config::document{}, file};
    auto doc = store.current();
    auto token = keyring::mint();
    auto server = doc->server;
    server.auth.tokens.push_back({.name = args[0], .hash = keyring::hash(token), .role = role});
    store.put_server(server, doc->revisions.server);
    std::println("{}", token);
    return 0;
}

export int run_gateway(int argc, char** argv, bool managed) {
    try {
        if (argc > 1 && std::string_view{argv[1]} == "token")
            return add_token(std::span{argv + 2, argv + argc});
        if (argc > 2)
            throw std::invalid_argument("usage: fughetta-service [config.json] | fughetta-service token NAME owner|member [config.json]");
        std::filesystem::path file = argc > 1 ? argv[1] : "config.json";
        environment::load(file.parent_path() / ".env");
        std::atomic<bool> interrupted{false};
        for (;;) {
            daemon::publish(daemon::phase::starting);
            config::document doc;
            if (argc > 1 || std::filesystem::exists(file))
                doc = config::load(file);
            if (managed) {
                doc.worker_token = keyring::mint();
                host::set_environment_if_absent("SOURCE_TOKEN", keyring::mint());
            }
            cores::io_context<runtime::context> context{1};
            struct shutdown {
                cores::context<native::scheduler>& context;
                std::atomic<bool>& interrupted;
                bool managed;
                void stop() noexcept {
                    interrupted.store(true);
                    if (!managed || daemon::current.load() == daemon::phase::starting || daemon::current.load() == daemon::phase::initialized)
                        context.stop();
                }
            } stop{context, interrupted, managed};
            cores::signal_watch signals{stop};
            gateway::node node{std::move(doc), file, context, managed ? host::executable() : std::filesystem::path{}, &interrupted};
            context.run([&](auto& io, std::size_t core) { return node.run(io, core); });
            if (interrupted.load() || !node.reconfiguring())
                break;
        }
    } catch (const std::exception& e) {
        daemon::publish(daemon::phase::stopping);
        logging::error(std::format("gateway: {}", e.what()));
        return 1;
    }
    daemon::publish(daemon::phase::stopping);
    return 0;
}
