#include <asio.hpp>

import std;
import coroutine;
import native;
import http;

namespace {
    std::atomic<std::size_t> allocations = 0;
}

void* operator new(std::size_t n) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;
    using http::operator co_await;

    struct sample {
        double ns_per_op;
        double allocs_per_op;
    };

    template <class Round> auto measure(std::size_t n, Round round) -> sample {
        round(std::max<std::size_t>(n / 10, 100));
        auto a0 = allocations.load();
        auto t0 = clock::now();
        round(n);
        auto t1 = clock::now();
        auto a1 = allocations.load();
        return {std::chrono::duration<double, std::nano>(t1 - t0).count() / double(n), double(a1 - a0) / double(n)};
    }

    void row(std::string_view scenario, std::string_view crossing, std::optional<sample> s) {
        if (s)
            std::println("| {:<5} | {:<46} | {:>9.1f} | {:>9.3f} |", scenario, crossing, s->ns_per_op, s->allocs_per_op);
        else
            std::println("| {:<5} | {:<46} | {:>9} | {:>9} |", scenario, crossing, "n/a", "n/a");
    }

    template <class Scheduler> auto hops(Scheduler sched, std::size_t n) -> coroutine::task<> {
        for (std::size_t i = 0; i < n; ++i)
            co_await execution::schedule(sched);
    }

    void raw_posts(asio::io_context& ioc, std::size_t n) {
        struct step {
            asio::io_context* ioc;
            std::size_t left;
            using allocator_type = memory::frame_allocator<void>;
            auto get_allocator() const noexcept -> allocator_type { return {}; }
            void operator()() {
                if (--left)
                    asio::post(*ioc, step{ioc, left});
            }
        };
        asio::post(ioc, step{&ioc, n});
        ioc.restart();
        ioc.run();
    }

    constexpr std::size_t message = 64;

    auto echo_server(native::socket& s, std::size_t n) -> coroutine::task<> {
        std::array<std::byte, message> buf;
        for (std::size_t i = 0; i < n; ++i) {
            co_await io::async_read(s, buf);
            co_await io::async_write(s, buf);
        }
    }
    auto echo_client(native::socket& s, std::size_t n) -> coroutine::task<> {
        std::array<std::byte, message> buf{};
        for (std::size_t i = 0; i < n; ++i) {
            co_await io::async_write(s, buf);
            co_await io::async_read(s, buf);
        }
    }

    struct raw_echo {
        asio::ip::tcp::socket& server;
        asio::ip::tcp::socket& client;
        std::size_t server_left, client_left;
        std::array<std::byte, message> sbuf{}, cbuf{};

        struct handler {
            raw_echo* self;
            int step;
            using allocator_type = memory::frame_allocator<void>;
            auto get_allocator() const noexcept -> allocator_type { return {}; }
            void operator()(std::error_code ec, std::size_t) {
                if (!ec)
                    self->next(step);
            }
        };

        void start() {
            asio::async_read(server, asio::buffer(sbuf), handler{this, 0});
            asio::async_write(client, asio::buffer(cbuf), handler{this, 1});
        }
        void next(int step) {
            switch (step) {
            case 0:
                asio::async_write(server, asio::buffer(sbuf), handler{this, 2});
                break;
            case 1:
                asio::async_read(client, asio::buffer(cbuf), handler{this, 3});
                break;
            case 2:
                if (--server_left)
                    asio::async_read(server, asio::buffer(sbuf), handler{this, 0});
                break;
            case 3:
                if (--client_left)
                    asio::async_write(client, asio::buffer(cbuf), handler{this, 1});
                break;
            }
        }
    };

    auto connected_pair(native::context& context) -> std::pair<native::socket, native::socket> {
        auto io = context.get_scheduler();
        auto l = io::listen(io, io::endpoint{});
        auto pair = [](native::scheduler& io, native::listener& l) -> coroutine::task<std::pair<native::socket, native::socket>> {
            auto c = co_await io::connect(io, {{127, 0, 0, 1}, l.port()});
            auto s = co_await io::async_accept(l);
            co_return std::pair{std::move(s), std::move(c)};
        };
        return *cores::run(context, pair(io, l));
    }

    auto both(native::context&, coroutine::task<> a, coroutine::task<> b) -> coroutine::task<> {
        co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(std::move(a));
            co_await std::move(b);
        });
    }

    struct stopping {
        using sender_concept = execution::sender_t;
        using completion_signatures = execution::completion_signatures<execution::set_value_t(), execution::set_stopped_t()>;
        template <class R> struct op {
            using operation_state_concept = execution::operation_state_t;
            R rcvr;
            void start() & noexcept { execution::set_stopped(std::move(rcvr)); }
        };
        template <class R> auto connect(R r) const -> op<R> { return {std::move(r)}; }
    };

    auto level3() -> coroutine::task<> { co_await stopping{}; }
    auto level2() -> coroutine::task<> { co_await level3(); }
    auto level1() -> coroutine::task<> { co_await level2(); }
    auto stops(std::size_t n) -> coroutine::task<std::size_t> {
        std::size_t caught = 0;
        for (std::size_t i = 0; i < n; ++i)
            caught += !co_await coroutine::stop_point(level1());
        co_return caught;
    }

    auto h_level3() -> http::task<> { co_await stopping{}; }
    auto h_level2() -> http::task<> { co_await h_level3(); }
    auto h_level1() -> http::task<> { co_await h_level2(); }
    auto h_stops(std::size_t n) -> http::task<std::size_t> {
        std::size_t caught = 0;
        for (std::size_t i = 0; i < n; ++i) {
            try {
                co_await h_level1();
            } catch (const http::stopped&) {
                ++caught;
            }
        }
        co_return caught;
    }
    auto application_http_stops(std::size_t n) -> coroutine::task<std::size_t> { co_return co_await h_stops(n); }
} // namespace

int main(int argc, char** argv) {
    std::size_t n = argc > 1 ? std::stoul(argv[1]) : 200'000;

    std::println("| case  | crossing                                       |     "
                 "ns/op | allocs/op |");
    std::println("|-------|------------------------------------------------|-----"
                 "-----:|----------:|");

    {
        native::context io;
        row("hop", "task -> native resource scheduler", measure(n, [&](std::size_t k) { cores::run(io, hops(io.get_scheduler(), k)); }));
        asio::io_context asio_context;
        row("hop", "raw asio::post chain (comparison)", measure(n, [&](std::size_t k) { raw_posts(asio_context, k); }));
    }
    {
        native::context io;
        auto [server, client] = connected_pair(io);
        auto rounds = n / 4;
        row("echo", "task -> native I/O sender",
            measure(rounds, [&](std::size_t k) { cores::run(io, both(io, echo_server(server, k), echo_client(client, k))); }));
        asio::io_context asio_context;
        asio::ip::tcp::acceptor listener{asio_context, {asio::ip::address_v4::loopback(), 0}};
        asio::ip::tcp::socket asio_client{asio_context};
        asio_client.connect(listener.local_endpoint());
        auto asio_server = listener.accept();
        asio_client.set_option(asio::ip::tcp::no_delay{true});
        asio_server.set_option(asio::ip::tcp::no_delay{true});
        row("echo", "raw asio completion handlers (comparison)", measure(rounds, [&](std::size_t k) {
                raw_echo e{asio_server, asio_client, k, k};
                e.start();
                asio_context.restart();
                asio_context.run();
            }));
    }

    {
        native::context io;
        row("stop", "task: unhandled_stopped through 3 frames", measure(n, [&](std::size_t k) { cores::run(io, stops(k)); }));
        row("stop", "http::task: throw http::stopped through 3 frames", measure(n, [&](std::size_t k) { cores::run(io, application_http_stops(k)); }));
    }
}
