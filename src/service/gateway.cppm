module;
#include <argparse/argparse.hpp>
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
import json;
import runtime;
import host;
import enrollment;
import publiccache;

namespace gateway {
    class node {
    public:
        using socket = native::socket;
        using listener = native::listener;

        node(config::document doc, std::optional<std::filesystem::path> file, cores::context<native::scheduler>& rt, std::filesystem::path executable = {},
             std::atomic<bool>* interrupted = nullptr, bool provisioned = false)
            : interrupted_(interrupted), executable_(std::move(executable)), doc_(doc), server_(doc.server), rt_(rt),
              control_(doc.runtime, doc.revisions.runtime), io_pool_(allocation::distribute(doc.runtime).gateway_disk),
              hub_(rt, config::store(std::move(doc), std::move(file), provisioned), io_pool_), identities_(server_.storage, server_.auth.workspace),
              router_(rt, routing::stale_after, &hub_),
              files_(rt, server_.storage, hub_, io_pool_, io::parse_endpoint(server_.source), doc_.public_shards) {
            files_.changed = [this] { hub_.events().publish(6); };
            identities_.changed = [this] { hub_.events().publish(1); };
            for (std::size_t c = 0; c < rt.size(); ++c)
                router_.workers(c).changed = [this] { hub_.events().publish(1); };
            if (server_.auth.mode == config::auth_mode::token && server_.auth.tokens.empty())
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
            std::optional<listener> overflow;
            if (!server_.overflow_worker_bind.empty())
                overflow.emplace(native::listener{io, io::parse_endpoint(server_.overflow_worker_bind, server_.worker_port)});
            std::optional<listener> remote;
            if (server_.remote_worker_port)
                remote.emplace(native::listener{io, io::parse_endpoint(server_.remote_worker_bind, server_.remote_worker_port)});
            if (core == 0)
                logging::info(std::format("gateway: source and language API on http://{}:{}/, source {}, metadata {}, {} cores", server_.http_bind,
                                          server_.http_port, server_.source, server_.storage, rt_.size()));
            auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                if (core == 0 && !executable_.empty()) {
                    co_await processes.start(io, children, executable_, doc_);
                    lifecycle::publish(lifecycle::phase::initialized);
                    children.spawn(supervise(io, processes));
                }
                children.spawn(router_.workers(core).watch(io));
                children.spawn(accept_workers(workers, core, children));
                if (overflow)
                    children.spawn(accept_workers(*overflow, core, children));
                if (remote)
                    children.spawn(accept_tunnels(*remote, here, children));
                if (core == 0) {
                    children.spawn(queue_->serve(io));
                    if (!executable_.empty())
                        children.spawn(watch_runtime(io));
                }
                co_await accept_http(http, here, children);
            };
            co_await coroutine::with_scope(std::move(run_children));
        }

        auto settings() { return hub_.current(0); }
        auto reconfiguring() const -> bool { return reconfigure_; }

    private:
        // A child that exits unasked stops the whole service.
        auto supervise(native::scheduler& io, runtime::group& processes) -> coroutine::task<> {
            try {
                co_await processes.supervise(io);
            } catch (...) {
                lifecycle::publish(lifecycle::phase::stopping);
                rt_.stop();
                throw;
            }
        }

        auto watch_runtime(native::scheduler& io) -> coroutine::task<> {
            std::optional<std::chrono::steady_clock::time_point> deadline;
            for (;;) {
                if (interrupted_ && interrupted_->load()) {
                    control_.drain();
                    lifecycle::publish(lifecycle::phase::draining);
                    if (!deadline)
                        deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                    if ((control_.idle() && queue_->idle()) || std::chrono::steady_clock::now() >= *deadline) {
                        lifecycle::publish(lifecycle::phase::stopping);
                        rt_.stop();
                        co_return;
                    }
                    co_await execution::schedule_after(io, std::chrono::milliseconds{50});
                    continue;
                }
                auto current = hub_.current(0);
                if (current->runtime != doc_.runtime) {
                    control_.drain();
                    lifecycle::publish(lifecycle::phase::draining);
                    if (control_.idle() && queue_->idle()) {
                        reconfigure_ = true;
                        rt_.stop();
                        co_return;
                    }
                } else {
                    auto workers = router_.workers(0).describe(0);
                    if (std::ranges::count_if(workers, [](const auto& w) { return w.online && !w.worker_id && w.name.starts_with("local-"); }) == doc_.runtime.workers) {
                        control_.ready(current->revisions.runtime);
                        lifecycle::publish(lifecycle::phase::ready);
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

        auto accept_workers(listener& l, std::size_t core, coroutine::scope& children) -> coroutine::task<> {
            for (;;) {
                auto [connection, peer] = co_await net::async_accept(l);
                children.spawn(router_.workers(core).admit(rt_.get_scheduler(core), std::move(connection)));
            }
        }

        auto accept_tunnels(listener& l, per_core& here, coroutine::scope& children) -> coroutine::task<> {
            for (;;) {
                auto [connection, peer] = co_await net::async_accept(l);
                children.spawn(here.http.serve_worker(std::move(connection)));
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
auto add_token(int argc, char** argv) -> int {
    argparse::ArgumentParser arguments{"fughetta-service token", "", argparse::default_arguments::help};
    arguments.add_argument("name");
    arguments.add_argument("role").choices("owner", "member");
    arguments.add_argument("config").nargs(argparse::nargs_pattern::optional).default_value(std::string{"config.json"});
    arguments.parse_args(argc, argv);
    const auto role = arguments.get<std::string>("role") == "owner" ? config::token_role::owner : config::token_role::member;
    const auto file = std::filesystem::path{arguments.get<std::string>("config")};
    const auto budget = allocation::self_hosted(host::available_cpus());
    config::store store{std::filesystem::exists(file) ? config::load(file, budget) : config::document{.runtime = budget}, file};
    auto doc = store.current();
    auto token = keyring::mint();
    auto server = doc->server;
    server.auth.tokens.push_back({.name = arguments.get<std::string>("name"), .hash = keyring::hash(token), .role = role});
    store.put_server(server, doc->revisions.server);
    std::println("{}", token);
    return 0;
}

export int run_gateway(int argc, char** argv, bool managed) {
    try {
        if (argc > 1 && std::string_view{argv[1]} == "token")
            return add_token(argc - 1, argv + 1);
        const bool provisioned = argc == 3 && std::string_view{argv[1]} == "--workspace-config";
        if (argc > 2 && !provisioned)
            throw std::invalid_argument("usage: fughetta-service [config.json] | fughetta-service token NAME owner|member [config.json]");
        std::filesystem::path file = provisioned ? argv[2] : argc > 1 ? argv[1] : "config.json";
        if (!provisioned) environment::load(file.parent_path() / ".env");
        config::store::snapshot next;
        std::atomic<bool> interrupted{false};
        for (;;) {
            lifecycle::publish(lifecycle::phase::starting);
            config::document doc;
            const auto budget = managed && !provisioned ? std::optional{allocation::self_hosted(host::available_cpus())} : std::nullopt;
            if (next) {
                doc = *next;
            } else if (provisioned) {
                doc = file == "-" ? config::parse(config::read_workspace_config(std::cin)) : config::load(file);
                file = std::filesystem::path{doc.server.storage} / "workspace.json";
                if (std::filesystem::exists(file)) {
                    std::ifstream input{file, std::ios::binary};
                    if (!input) throw std::runtime_error("cannot open workspace settings: " + file.string());
                    std::string text{std::istreambuf_iterator<char>{input}, {}};
                    config::parse_as<config::workspace_settings>(text, "workspace settings").apply(doc);
                }
            } else if (argc > 1 || std::filesystem::exists(file))
                doc = config::load(file, budget);
            else if (budget)
                doc.runtime = *budget;
            if (managed) {
                // A hosted backend provisions a shared credential for overflow Workers.
                // The embedded supervisor still rotates its private credential on restart.
                if (!doc.server.overflow_wait_seconds)
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
                    if (!managed || lifecycle::current.load() == lifecycle::phase::starting || lifecycle::current.load() == lifecycle::phase::initialized)
                        context.stop();
                }
            } stop{context, interrupted, managed};
            cores::signal_watch signals{stop};
            gateway::node node{std::move(doc), file, context, managed ? host::executable() : std::filesystem::path{}, &interrupted, provisioned};
            context.run([&](auto& io, std::size_t core) { return node.run(io, core); });
            if (interrupted.load() || !node.reconfiguring())
                break;
            if (provisioned) next = node.settings();
        }
    } catch (const std::exception& e) {
        lifecycle::publish(lifecycle::phase::stopping);
        logging::error(std::format("gateway: {}", e.what()));
        return 1;
    }
    lifecycle::publish(lifecycle::phase::stopping);
    return 0;
}


export auto fill_public_cache(int argc, char** argv) -> int {
    if (argc < 4)
        throw std::invalid_argument("usage: fughetta-service public-cache-fill CONFIG DIRECTORY KEY [KEY ...]");
    const auto doc = config::load(argv[1]);
    if (!doc.public_shards)
        throw std::invalid_argument("public_shards is not configured");
    const public_shards::cache cache{*doc.public_shards, doc.server.storage, false};
    native::context context;
    auto run = [&]() -> coroutine::task<> {
        for (int i = 3; i < argc; ++i)
            co_await cache.fill(context.get_scheduler(), argv[2], argv[i]);
    };
    cores::run(context, run());
    return 0;
}
