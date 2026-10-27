module support;
import coroutine;
import server;

namespace test {
    struct source_service::state {
        cores::io_context<native::context> rt{1};
        execution::thread_pool disk{2};
        source_server::service service;
        net::ip::tcp::endpoint endpoint;
        std::jthread thread;

        state(std::filesystem::path root, builds::toolset tools) : service(rt, std::move(root), disk, std::move(tools)) {
            std::promise<net::ip::tcp::endpoint> ready;
            auto started = ready.get_future();
            thread = std::jthread([this, &ready] {
                rt.run([&](native::scheduler& io, std::size_t) -> coroutine::task<> {
                    auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                    ready.set_value(net::ip::tcp::endpoint{net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
                    co_await coroutine::with_scope([&](coroutine::scope& tasks) -> coroutine::task<> {
                        for (;;)
                            tasks.spawn(service.serve(
                                io, co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); }), 0));
                    });
                });
            });
            endpoint = started.get();
        }
        ~state() {
            rt.stop();
            thread.join();
        }
    };

    source_service::source_service(std::filesystem::path root, builds::toolset tools) : state_(std::make_unique<state>(std::move(root), std::move(tools))) {}
    source_service::~source_service() = default;
    auto source_service::endpoint() const -> net::ip::tcp::endpoint { return state_->endpoint; }
} // namespace test
