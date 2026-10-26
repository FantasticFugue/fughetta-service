export module worker;
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
import tls;
import httpant.tls;
import websocket;
import http;
import resolver;
import constants;

namespace worker {
struct refused : std::runtime_error {
    explicit refused(std::string_view code) : std::runtime_error(
        code=="worker_revoked" ? "Worker identity was revoked" :
        code=="worker_protocol_mismatch" ? "Worker and Core protocols differ; update the Worker and Core together" :
        code=="worker_enrollment_disabled" ? "Remote Worker admission is disabled" :
        "Worker credentials or endpoint were refused") {}
};
class node {
  public:
    using socket = native::socket;

    node(config::worker cfg, cores::context<native::scheduler> &rt)
        : cfg_(std::move(cfg)), name_(cfg_.name.empty() ? host::name() : cfg_.name), compute_(cfg_.compute_threads),
          builds_(rt,
                  replica::options{.root = cfg_.worker_id ? std::filesystem::path{cfg_.work_root}/ *cfg_.workspace_id/ *cfg_.worker_id : std::filesystem::path{cfg_.work_root},
                                   .worker = name_,
                                   .limits = {.max_cache_bytes = cfg_.max_cache_bytes, .tree_ttl = std::chrono::hours{24 * cfg_.tree_ttl_days}},
                                   .env = cfg_.env,
                                   .inherit_env = cfg_.inherit_env,
                                   .tools = cfg_.tools,
                                   .isolate = true,
                                   .parallel = cfg_.parallel},
                  compute_) {}

    auto run(native::scheduler &io, std::size_t core) -> coroutine::task<> {
        co_await builds_.prepare(core);
        if (cfg_.gateway.starts_with("wss://")) {
            auto target=tls::address::parse(cfg_.gateway);
            native::reconnect_backoff retry;
            unsigned attempts=0;
            for (;;) {
                auto started=std::chrono::steady_clock::now();
                try {co_await remote(io,core,target);}
                catch (const refused&) {throw;}
                catch (const http::tls::verification_failure&) {throw;}
                catch (const std::exception&) {}
                if (std::chrono::steady_clock::now()-started>=native::reconnect_backoff::stable_after) {retry.reset();attempts=0;}
                if (++attempts>=cfg_.reconnect_attempts) throw std::runtime_error("Worker reconnect attempt limit reached");
                auto delay=retry.next();
                logging::info(std::format("worker {} workspace {}: disconnected; reconnect attempt {} in {}ms",*cfg_.worker_id,*cfg_.workspace_id,attempts,delay.count()));
                co_await execution::schedule_after(io,delay);
            }
        }
        co_await serve(io, core);
    }

  private:
    auto serve(native::scheduler &io, std::size_t core) -> coroutine::task<> {
        auto connected = co_await coroutine::deadline(io, std::chrono::seconds{10}, connect(io));
        if (!connected) {
            throw std::runtime_error(std::format("worker '{}': no answer from {} in 10s", name_, cfg_.gateway));
        }
        auto sock = std::move(*connected);
        logging::info(std::format("worker '{}': connected to {}", name_, cfg_.gateway));
        co_await session(io, core, sock);
    }

    template <io::stream S> auto session(native::scheduler &io, std::size_t core, S &stream,socket* connection=nullptr) -> coroutine::task<> {
        transport::channel<S> chan{stream};
        transport::dispatcher<S> serve;
        builds_.install(serve, core);
        serve.template on<jobs::hash_file>([this, &io](jobs::hash_file job, auto &) -> coroutine::task<jobs::digest> {
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
                                       .tools = tools_,.worker_id=cfg_.worker_id,.workspace_id=cfg_.workspace_id,
                                       .heartbeat_seconds=cfg_.worker_id ? cfg_.heartbeat_seconds : 2});

        if (cfg_.worker_id) {
            auto first=co_await coroutine::deadline(io,std::chrono::seconds{30},chan.recv(constants::worker_hello_bytes));
            if (!first) throw std::runtime_error("Worker admission timed out");
            if (first->kind!=jobs::code<jobs::admission>) throw refused("worker_protocol_mismatch");
            auto admitted=transport::decode<jobs::admission>(*first);
            if (!admitted.accepted) {
                if (admitted.code=="worker_registry_unavailable") throw std::runtime_error("Core Worker registry is unavailable");
                throw refused(admitted.code);
            }
            if (admitted.worker_id!=*cfg_.worker_id) throw refused("worker_identity_mismatch");
            logging::info(std::format("worker {} workspace {}: online",*cfg_.worker_id,*cfg_.workspace_id));
        }

        std::cerr << "service-ready:worker\n";
        co_await coroutine::with_scope([&](coroutine::scope &requests) -> coroutine::task<> {
            requests.spawn(beat(io, chan));
            if constexpr (requires {stream.ping();}) {
                if (connection) requests.spawn(pings(io,stream,*connection));
            }
            for (;;) {
                auto frame=co_await chan.recv();
                if (cfg_.worker_id && frame.kind==jobs::code<jobs::admission>) {
                    auto admitted=transport::decode<jobs::admission>(frame);
                    if (!admitted.accepted) throw refused(admitted.code);
                    continue;
                }
                requests.spawn(serve.handle(chan,std::move(frame)));
            }
        });
    }

    auto connect(native::scheduler &io) -> coroutine::task<socket> { co_return co_await io::connect(io, io::parse_endpoint(cfg_.gateway)); }

    auto remote(native::scheduler &io, std::size_t core,const tls::address& target) -> coroutine::task<> {
        auto connect_remote = [&]() -> coroutine::task<socket> {
            auto endpoint = co_await resolver::resolve(io, target.host, static_cast<std::uint16_t>(std::stoul(target.port)));
            co_return co_await io::connect(io, endpoint);
        };
        auto connected = co_await coroutine::deadline(io, std::chrono::seconds{30}, connect_remote());
        if (!connected)
            throw std::runtime_error("Worker WSS connection timed out");
        auto socket = std::move(*connected);
        http::tls::context context{false};
        context.trust(cfg_.ca_file);
        http::tls::stream secure{web::stream{socket}, context};
        secure.hostname(target.host);
        auto opened = co_await coroutine::deadline(io, std::chrono::seconds{30}, open(secure, target));
        if (!opened)
            throw std::runtime_error("Worker WSS handshake timed out");
        web::ws::stream stream{secure, http::websocket::side::client, *opened};
        try { co_await session(io, core, stream,&socket); }
        catch (const io::closed&) {}
        co_await secure.close();
    }

    auto open(http::tls::stream<web::stream<native::socket>> &secure, const tls::address &target) -> coroutine::task<std::vector<std::byte>> {
        co_await secure.handshake();
        http::v1::client client{secure};
        auto key = web::ws::random_key();
        http::request head{.method = http::method::GET,
                           .target = target.target,
                           .fields = {{"host", target.authority},
                                      {"connection", "Upgrade"},
                                      {"upgrade", "websocket"},
                                      {"sec-websocket-version", "13"},
                                      {"sec-websocket-key", key},
                                      {"sec-websocket-protocol", "fughetta-worker.v1"}}};
        auto response = co_await http::coroutine::upgrade(client, std::move(head));
        if (response.head.status!=101) {
            if (response.head.status.code()>=500 || response.head.status==429) throw std::runtime_error("Worker tunnel is temporarily unavailable");
            throw refused("worker_endpoint_refused");
        }
        http::websocket::check_response(response.head, key, "fughetta-worker.v1");
        co_return std::move(response.pending);
    }

    template <io::stream S> auto beat(native::scheduler &io, transport::channel<S> &chan) -> coroutine::task<> {
        for (;;) {
            co_await execution::schedule_after(io, std::chrono::seconds{cfg_.worker_id ? cfg_.heartbeat_seconds : 2});
            co_await chan.post(jobs::heartbeat{});
        }
    }

    template <class S> auto pings(native::scheduler& io,S& stream,socket& connection) -> coroutine::task<> {
        struct close {socket& connection;~close(){connection.shutdown(net::socket_base::shutdown_send);}} close_on_exit{connection};
        for (;;) {
            co_await execution::schedule_after(io,std::chrono::seconds{cfg_.ping_seconds});
            if (stream.awaiting_pong()) throw std::runtime_error("Worker WebSocket pong timed out");
            co_await stream.ping();
        }
    }

    template <std::invocable F> auto offload(native::scheduler &io, F fn) -> coroutine::task<std::expected<std::invoke_result_t<F>, std::string>> {
        using result = std::expected<std::invoke_result_t<F>, std::string>;
        co_return co_await (execution::schedule(compute_.get_scheduler()) | execution::then([fn = std::move(fn)]() mutable -> result {
                                try {
                                    return fn();
                                } catch (const std::exception &e) {
                                    return std::unexpected(std::string{e.what()});
                                }
                            }) |
                            execution::continues_on(io));
    }

    auto hash(const jobs::hash_file &job) const -> jobs::digest {
        files::mapping file{resolve(job.path.value())};
        return {content::hash_of(file.bytes()).str(), file.bytes().size()};
    }

    auto resolve(std::string_view rel) const -> std::filesystem::path {
        std::filesystem::path p{rel};
        if (p.is_absolute() || std::ranges::contains(p, ".."))
            throw std::invalid_argument("path escapes data root");
        auto root=std::filesystem::path{cfg_.data_root};
        if (cfg_.worker_id) root=root/ *cfg_.workspace_id/ *cfg_.worker_id;
        return root / p;
    }

    config::worker cfg_;
    std::string name_;
    std::vector<std::string> tools_ = host::detect_tools(cfg_.tools);
    execution::thread_pool compute_;
    replica::service builds_;
};
} // namespace worker

export int run_worker(int argc, char **argv,bool private_file=false) {
    try {
        auto cfg = argc > 1 ? config::load_worker(argv[1],private_file) : config::worker{};
        cores::io_context<runtime::context> context{1};
        cores::signal_watch signals{context};
        worker::node node{std::move(cfg), context};
        context.run([&](auto &io, std::size_t core) { return node.run(io, core); });
    } catch (const std::exception &e) {
        logging::error(std::format("worker: {}", e.what()));
        return 1;
    }
    return 0;
}

export int worker_cli(int argc,char** argv);
