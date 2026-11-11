export module worker;
import utility;
import std;
import runtime;
import coroutine;
import native;
import signals;
import protocol;
import executor;
import configuration;
import durability;
import host;
import constants;
import toolchain;

namespace worker {
    // The Core refused this identity; retrying cannot succeed.
    struct refused : std::runtime_error, util::retained_error<refused> {
        explicit refused(std::string_view code)
            : std::runtime_error(code == "worker_revoked" ? "Worker identity was revoked" : "Worker credentials or endpoint were refused") {}
    };
    // Refusals that leave the identity valid are retried like a lost connection.
    [[noreturn]] void reject(std::string_view code) {
        if (code == "worker_protocol_mismatch")
            throw std::runtime_error("Worker and Core protocols differ; update the Worker and Core together");
        if (code == "worker_enrollment_disabled")
            throw std::runtime_error("Remote Worker admission is disabled");
        if (code == "worker_registry_unavailable")
            throw std::runtime_error("Core Worker registry is unavailable");
        throw refused(code);
    }
    class node {
    public:
        using socket = native::socket;

        node(config::worker cfg, cores::context<native::scheduler>& rt)
            : cfg_(std::move(cfg)), name_(cfg_.name.empty() ? host::name() : cfg_.name), compute_(cfg_.compute_threads),
              builds_(rt,
                      replica::options{.root = cfg_.worker_id ? std::filesystem::path{cfg_.work_root} / *cfg_.workspace_id / *cfg_.worker_id
                                                              : std::filesystem::path{cfg_.work_root},
                                       .worker = name_,
                                       .limits = {.max_cache_bytes = cfg_.max_cache_bytes, .tree_ttl = std::chrono::hours{24 * cfg_.tree_ttl_days}},
                                       .env = cfg_.env,
                                       .inherit_env = cfg_.inherit_env,
                                       .tools = cfg_.tools,
                                       .parallel = cfg_.parallel,
                                       .dependency_cache = cfg_.dependency_cache ? std::optional{std::filesystem::path{*cfg_.dependency_cache}} : std::nullopt,
                                       .dependency_upstream = cfg_.dependency_upstream ? std::optional{std::filesystem::path{*cfg_.dependency_upstream}} : std::nullopt,
                                       .dependency_publish = cfg_.dependency_publish ? std::optional{std::filesystem::path{*cfg_.dependency_publish}} : std::nullopt,
                                       .dependency_recipe = cfg_.dependency_recipe},
                      compute_) {}

        auto run(native::scheduler& io, std::size_t core) -> coroutine::task<> { co_await builds_.run(core, connected(io, core)); }

        auto connected(native::scheduler& io, std::size_t core) -> coroutine::task<> {
            auto identity = co_await offload(io, [this] {
                auto env = cfg_.inherit_env ? host::environment() : std::map<std::string, std::string>{};
                for (const auto& [name, value] : cfg_.env)
                    env[name] = value;
                env["OMP_NUM_THREADS"] = std::to_string(cfg_.parallel);
                return toolchain::inspect(host::resolve_tools(cfg_.tools, toolchain::required_tools()), env, cfg_.sdk_manifests);
            });
            if (!identity)
                throw std::runtime_error(identity.error());
            tools_ = std::move(identity->tools);
            toolchain_ = std::move(identity->id);
            if (!cfg_.worker_id) {
                co_await serve(io, core);
                co_return;
            }
            // A registered Worker reaches its Core through the client's local relay, which may restart its tunnel at any time.
            native::reconnect_backoff retry;
            unsigned attempts = 0;
            for (;;) {
                auto started = std::chrono::steady_clock::now();
                try {
                    co_await serve(io, core);
                } catch (const refused&) {
                    throw;
                } catch (const std::exception& e) {
                    logging::info(std::format("worker {} workspace {}: {}", *cfg_.worker_id, *cfg_.workspace_id, e.what()));
                }
                if (std::chrono::steady_clock::now() - started >= native::reconnect_backoff::stable_after) {
                    retry.reset();
                    attempts = 0;
                }
                if (++attempts >= cfg_.reconnect_attempts)
                    throw std::runtime_error("Worker reconnect attempt limit reached");
                auto delay = retry.next();
                logging::info(std::format("worker {} workspace {}: disconnected; reconnect attempt {} in {}ms", *cfg_.worker_id, *cfg_.workspace_id, attempts,
                                          delay.count()));
                co_await execution::schedule_after(io, delay);
            }
        }

    private:
        auto serve(native::scheduler& io, std::size_t core) -> coroutine::task<> {
            auto connected = co_await coroutine::deadline(io, std::chrono::seconds{10}, connect(io));
            if (!connected) {
                throw std::runtime_error(std::format("worker '{}': no answer from {} in 10s", name_, cfg_.gateway));
            }
            auto sock = std::move(*connected);
            logging::info(std::format("worker '{}': connected to {}", name_, cfg_.gateway));
            co_await session(io, core, sock);
        }

        template <io::stream S> auto session(native::scheduler& io, std::size_t core, S& stream) -> coroutine::task<> {
            transport::channel<S> chan{stream};
            transport::dispatcher<S> serve;
            builds_.install(serve, core);
            serve.template on<jobs::hash_file>([this, &io](jobs::hash_file job, auto&) -> coroutine::task<jobs::digest> {
                if (cfg_.dependency_publish) throw std::invalid_argument("dependency publishers only accept dependency.fill");
                auto out = co_await offload(io, [this, job = std::move(job)] { return hash(job); });
                if (!out)
                    throw std::runtime_error(out.error());
                co_return *out;
            });
            co_await chan.post(jobs::hello{.name = name_,
                                           .auth = cfg_.token.value_or(""),
                                           .platform = {.os = std::string{host::os}, .arch = std::string{host::arch}},
                                           .compute_threads = compute_.available_parallelism(),
                                           .max_inflight = cfg_.max_inflight,
                                           .protocol = jobs::protocol_version,
                                           .tools = tools_,
                                           .worker_id = cfg_.worker_id,
                                           .workspace_id = cfg_.workspace_id,
                                           .heartbeat_seconds = cfg_.worker_id ? cfg_.heartbeat_seconds : 2,
                                           .toolchain_id = toolchain_,
                                           .dependency_publisher = cfg_.dependency_publish.has_value()});

            if (cfg_.worker_id) {
                auto first = co_await coroutine::deadline(io, std::chrono::seconds{30}, chan.recv(constants::worker_hello_bytes));
                if (!first)
                    throw std::runtime_error("Worker admission timed out");
                if (first->kind != jobs::code<jobs::admission>)
                    reject("worker_protocol_mismatch");
                auto admitted = transport::decode<jobs::admission>(*first);
                if (!admitted.accepted)
                    reject(admitted.code);
                if (admitted.worker_id != *cfg_.worker_id)
                    throw refused("worker_identity_mismatch");
                logging::info(std::format("worker {} workspace {}: online", *cfg_.worker_id, *cfg_.workspace_id));
            }

            std::cerr << "service-ready:worker\n";
            // Child requests can finish after the scope body; keep their counter alive through the join.
            std::atomic<std::size_t> inflight{0};
            co_await coroutine::with_scope([&](coroutine::scope& requests) -> coroutine::task<> {
                requests.spawn(beat(io, chan, core));
                // The Core picks this Worker by the same limit; enforcing it locally keeps a misbehaving Core from driving unbounded concurrency.
                auto max_inflight = static_cast<std::size_t>(cfg_.max_inflight);
                for (;;) {
                    auto frame = co_await chan.recv();
                    if (cfg_.worker_id && frame.kind == jobs::code<jobs::admission>) {
                        auto admitted = transport::decode<jobs::admission>(frame);
                        if (!admitted.accepted)
                            reject(admitted.code);
                        continue;
                    }
                    // Cancellation is cheap and must always reach the running call, so it never waits for a slot.
                    if (frame.kind == jobs::code<jobs::cancel>) {
                        requests.spawn(serve.handle(chan, std::move(frame)));
                        continue;
                    }
                    if (inflight.load(std::memory_order_relaxed) >= max_inflight) {
                        co_await chan.fail(frame.corr, "worker is at max_inflight", "worker_overloaded");
                        continue;
                    }
                    inflight.fetch_add(1, std::memory_order_relaxed);
                    requests.spawn(tracked(serve, chan, std::move(frame), inflight));
                }
            });
        }

        template <io::stream S>
        static auto tracked(transport::dispatcher<S>& serve, transport::channel<S>& chan, transport::frame frame, std::atomic<std::size_t>& inflight)
            -> coroutine::task<> {
            struct release {
                std::atomic<std::size_t>& inflight;
                ~release() { inflight.fetch_sub(1, std::memory_order_relaxed); }
            } counted{inflight};
            co_await serve.handle(chan, std::move(frame));
        }

        auto connect(native::scheduler& io) -> coroutine::task<socket> { co_return co_await io::connect(io, io::parse_endpoint(cfg_.gateway)); }

        template <io::stream S> auto beat(native::scheduler& io, transport::channel<S>& chan, std::size_t core) -> coroutine::task<> {
            for (;;) {
                co_await execution::schedule_after(io, std::chrono::seconds{cfg_.worker_id ? cfg_.heartbeat_seconds : 2});
                co_await chan.post(jobs::heartbeat{});
                co_await builds_.reclaim(core);
            }
        }

        template <std::invocable F> auto offload(native::scheduler& io, F fn) -> coroutine::task<std::expected<std::invoke_result_t<F>, std::string>> {
            using result = std::expected<std::invoke_result_t<F>, std::string>;
            co_return co_await (execution::schedule(compute_.get_scheduler()) | execution::then([fn = std::move(fn)]() mutable -> result {
                                    try {
                                        return fn();
                                    } catch (const std::exception& e) {
                                        return std::unexpected(std::string{e.what()});
                                    }
                                }) |
                                execution::continues_on(io));
        }

        auto hash(const jobs::hash_file& job) const -> jobs::digest {
            files::mapping file{resolve(job.path.value())};
            return {content::hash_of(file.bytes()).str(), file.bytes().size()};
        }

        auto resolve(std::string_view rel) const -> std::filesystem::path {
            std::filesystem::path p{rel};
            if (p.is_absolute() || std::ranges::contains(p, ".."))
                throw std::invalid_argument("path escapes data root");
            auto root = std::filesystem::path{cfg_.data_root};
            if (cfg_.worker_id)
                root = root / *cfg_.workspace_id / *cfg_.worker_id;
            return root / p;
        }

        config::worker cfg_;
        std::string name_;
        std::vector<std::string> tools_;
        std::string toolchain_;
        execution::thread_pool compute_;
        replica::service builds_;
    };
} // namespace worker

export int run_worker(int argc, char** argv, bool private_file = false) {
    try {
        auto cfg = argc > 1 ? config::load_worker(argv[1], private_file) : config::worker{};
        cores::io_context<runtime::context> context{1};
        cores::signal_watch signals{context};
        worker::node node{std::move(cfg), context};
        context.run([&](auto& io, std::size_t core) { return node.run(io, core); });
    } catch (const worker::refused& e) {
        logging::error(std::format("worker: {}", e.what()));
        return 2;
    } catch (const std::exception& e) {
        logging::error(std::format("worker: {}", e.what()));
        return 1;
    }
    return 0;
}

// `worker run --config PATH` serves a registered machine; its credential file must be owner-only.
export int worker_cli(int argc, char** argv) {
    if (argc != 4 || std::string_view{argv[1]} != "run" || std::string_view{argv[2]} != "--config") {
        logging::error("worker: use worker run --config PATH");
        return 1;
    }
    std::array<char*, 2> arguments{argv[0], argv[3]};
    return run_worker(static_cast<int>(arguments.size()), arguments.data(), true);
}
