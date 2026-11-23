module hostrole;
import std;
import async;
import configuration;
import coroutine;
import host_api;
import host_backend;
import host_operation;
import native;
import runtime;
import signals;

auto run_host(int argc, char** argv) -> int {
    if (argc > 2)
        throw std::invalid_argument("usage: fughetta-service --role host [config.json]");
    const auto settings = config::load_host(argc == 2 ? argv[1] : "config.json");
    cores::io_context<runtime::context> context{1};
    struct shutdown {
        cores::context<native::scheduler>& context;
        void stop() noexcept { context.stop(); }
    } stop{context};
    cores::signal_watch signals{stop};
    execution::thread_pool pool{1};
    auto serve = [&](native::scheduler& io, std::size_t) -> coroutine::task<> {
        auto provision = [&](const host_operation::operation& operation) {
            return host_backend::reconcile(io, pool, settings, operation);
        };
        host_api::server server{io, settings, provision};
        native::listener listener{io, io::parse_endpoint(settings.bind, settings.port)};
        lifecycle::publish(lifecycle::phase::ready);
        std::cerr << "service-ready:host\n";
        co_await server.run(listener);
    };
    lifecycle::publish(lifecycle::phase::starting);
    context.run(serve);
    lifecycle::publish(lifecycle::phase::stopping);
    return 0;
}
