import std;
import boost.ut;
import coroutine;
import native;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = io::scheduler_of_t<native::context>;

    struct loop_context {
        execution::run_loop loop;
        auto get_scheduler() noexcept { return loop.get_scheduler(); }
        void run() { loop.run(); }
        void stop() noexcept { loop.finish(); }
    };
    static_assert(io::context<loop_context>);
    static_assert(!io::network_scheduler<io::scheduler_of_t<loop_context>>);
    static_assert(std::same_as<io::socket_t<ctx>, native::socket>);
    static_assert(std::same_as<io::listener_t<ctx>, native::listener>);
    static_assert(execution::sender_of<decltype(net::async_connect(std::declval<native::socket&>(), net::ip::tcp::endpoint{}))>);

    struct connect_failure {
        using sender_concept = execution::sender_t;
        using completion_signatures = execution::completion_signatures<execution::set_value_t()>;
        struct op {
            using operation_state_concept = execution::operation_state_t;
            void start() & noexcept {}
        };
        template <class R> auto connect(R) const -> op { throw std::runtime_error("connect failed"); }
    };

    struct throwing_value {
        throwing_value() noexcept = default;
        throwing_value(const throwing_value&) = delete;
        throwing_value(throwing_value&&) { throw std::runtime_error("value move failed"); }
    };
    struct move_failure {
        using sender_concept = execution::sender_t;
        using completion_signatures = execution::completion_signatures<execution::set_value_t(throwing_value)>;
        template <class R> struct op {
            using operation_state_concept = execution::operation_state_t;
            R receiver;
            void start() & noexcept { execution::set_value(std::move(receiver), throwing_value{}); }
        };
        template <class R> auto connect(R receiver) const -> op<R> { return {std::move(receiver)}; }
    };
    template <class Sender> auto await_boundary(Sender sender) -> coroutine::task<> { co_await std::move(sender); }

    struct marker {
        bool* hit;
        ~marker() { *hit = true; }
    };

    auto sleep(ctx& io) -> coroutine::task<> { co_await execution::schedule_after(io, 1ms); }
    auto hop(ctx& io) -> coroutine::task<> { co_await execution::schedule(io); }
    auto add(int a, int b) -> coroutine::task<int> { co_return a + b; }
    auto chain(ctx& io) -> coroutine::task<int> {
        co_await execution::schedule_after(io, 0ms);
        auto x = co_await add(1, 2);
        co_return x + co_await add(3, 4);
    }
    auto boom() -> coroutine::task<int> {
        throw std::runtime_error("boom");
        co_return 0;
    }
    auto rethrows() -> coroutine::task<int> { co_return co_await boom(); }

    auto forever(ctx& io, bool* hit) -> coroutine::task<> {
        marker m{hit};
        co_await execution::schedule_after(io, 1h);
        expect(false) << "a stopped timer resumed its coroutine";
    }

    struct reset_read {
        using sender_concept = execution::sender_t;
        using completion_signatures =
            execution::completion_signatures<execution::set_value_t(std::size_t), execution::set_error_t(std::error_code), execution::set_stopped_t()>;

        template <class R> struct op {
            using operation_state_concept = execution::operation_state_t;
            R rcvr;
            void start() & noexcept { execution::set_error(std::move(rcvr), std::make_error_code(std::errc::connection_reset)); }
        };
        template <class R> auto connect(R r) const -> op<R> { return {std::move(r)}; }
    };

    auto expects(ctx& io) -> coroutine::task<std::error_code> {
        co_await execution::schedule_after(io, 0ms);
        auto r = co_await coroutine::expect(reset_read());
        co_return r ? std::error_code{} : r.error();
    }

    auto plain(ctx& io) -> coroutine::task<std::size_t> {
        co_await execution::schedule_after(io, 0ms);
        co_return co_await reset_read();
    }

    auto catches(ctx& io, bool* hit) -> coroutine::task<int> {
        auto r = co_await coroutine::stop_point(execution::when_all(forever(io, hit), execution::just_stopped()));
        co_return r ? 1 : 2;
    }

    auto catches_task(ctx& io, bool* hit) -> coroutine::task<int> {
        auto r = co_await coroutine::stop_point(forever(io, hit));
        co_return r ? 1 : 2;
    }

    auto scoped(ctx& io, bool* hit) -> coroutine::task<int> {
        co_await coroutine::with_scope([&io, hit](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(forever(io, hit));
            co_await execution::schedule_after(io, 1ms);
        });
        co_return 3;
    }

    auto joining_scope(ctx& io, bool* hit) -> coroutine::task<> {
        co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(forever(io, hit));
            co_await children.join();
        });
    }

    auto failing_scope_body(ctx& io, bool* hit) -> coroutine::task<> {
        co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(forever(io, hit));
            throw std::runtime_error("scope body failed");
            co_return;
        });
    }

    auto failing_child() -> coroutine::task<> {
        throw std::logic_error("child");
        co_return;
    }

    auto scope_failure(ctx& io) -> coroutine::task<> {
        co_await coroutine::with_scope([&io](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(failing_child());
            co_await execution::schedule_after(io, 1ms);
        });
    }

    auto echo_once(native::listener& l) -> coroutine::task<> {
        auto s = co_await execution::then(net::async_accept(l), [](auto socket, auto) { return std::move(socket); });
        std::array<std::byte, 5> buf;
        co_await io::async_read(s, buf);
        co_await io::async_write(s, buf);
    }

    auto ask(ctx& io, std::uint16_t port) -> coroutine::task<std::string> {
        auto s = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, port});
        std::string_view hello = "hello";
        co_await io::async_write(s, std::as_bytes(std::span{hello}));
        std::string back(5, '\0');
        co_await io::async_read(s, std::as_writable_bytes(std::span{back}));
        co_return back;
    }

    auto loopback(ctx& io) -> coroutine::task<std::string> {
        auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        std::string out;
        co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(echo_once(l));
            out = co_await ask(io, l.local_endpoint().port());
        });
        co_return out;
    }

    auto pending_accept(ctx& io) -> coroutine::task<int> {
        auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto r = co_await coroutine::stop_point(execution::when_all(net::async_accept(l), execution::just_stopped()));
        co_return r ? 1 : 2;
    }

    auto offload(ctx& io, execution::thread_pool& pool) -> coroutine::task<bool> {
        auto loop = std::this_thread::get_id();
        auto worker = co_await io::blocking(io, pool, [] { return std::this_thread::get_id(); });
        co_return worker != loop&& std::this_thread::get_id() == loop;
    }

    auto ordered(coroutine::mutex& m, std::vector<int>& log, ctx& io, int id) -> coroutine::task<> {
        auto guard = co_await m.lock();
        log.push_back(id);
        co_await execution::schedule_after(io, 1ms);
        log.push_back(id);
    }

    auto contend(ctx& io) -> coroutine::task<std::vector<int>> {
        coroutine::mutex m;
        std::vector<int> log;
        co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
            for (int i = 0; i < 3; ++i)
                children.spawn(ordered(m, log, io, i));
            co_await children.join();
        });
        co_return log;
    }

    auto thread_id() -> coroutine::task<std::thread::id> { co_return std::this_thread::get_id(); }

    struct cross {
        std::thread::id caller, callee, resumed;
        int value = 0;
        std::string error;
        bool stopped = false;
        bool unwound = false;
    };

    auto remote_timer(ctx& io, std::atomic<bool>* unwound) -> coroutine::task<> {
        struct flag {
            std::atomic<bool>* f;
            ~flag() { *f = true; }
        } guard{unwound};
        co_await execution::schedule_after(io, 1h);
    }

    auto boom_remote() -> coroutine::task<int> {
        throw std::runtime_error("remote boom");
        co_return 0;
    }

    auto slow(ctx& io, std::chrono::milliseconds d, bool* hit) -> coroutine::task<int> {
        marker m{hit};
        co_await execution::schedule_after(io, d);
        co_return 7;
    }

    auto deadlines(ctx& io, bool* fast_hit, bool* slow_hit) -> coroutine::task<std::pair<std::optional<int>, std::optional<int>>> {
        auto fast = co_await coroutine::deadline(io, 1s, slow(io, 1ms, fast_hit));
        auto late = co_await coroutine::deadline(io, 20ms, slow(io, 1h, slow_hit));
        co_return std::pair{fast, late};
    }

    static constexpr auto foundation_suite = [] {
        "the coroutine boundary reports a throwing connect"_test = [] {
            loop_context context;
            std::string error;
            try {
                cores::run(context, await_boundary(connect_failure{}));
            } catch (const std::runtime_error& e) {
                error = e.what();
            }
            expect(error == "connect failed");
        };

        "the coroutine boundary reports a throwing completion move"_test = [] {
            loop_context context;
            std::string error;
            try {
                cores::run(context, await_boundary(move_failure{}));
            } catch (const std::runtime_error& e) {
                error = e.what();
            }
            expect(error == "value move failed");
        };

        "a pure execution context needs no I/O service interface"_test = [] {
            loop_context context;
            auto sch = context.get_scheduler();
            expect(cores::run(context, execution::then(execution::schedule(sch), [] { return 42; })) == 42);
        };

        "absolute time scheduling uses the resource scheduler"_test = [] {
            native::context context;
            auto sch = context.get_scheduler();
            expect(cores::run(context, execution::schedule_at(sch, execution::now(sch) + 1ms)));
        };

        "stopping a joining scope waits for child cancellation"_test = [] {
            native::context context;
            auto sch = context.get_scheduler();
            bool hit = false;
            expect(!cores::run(context, execution::when_all(joining_scope(sch, &hit), execution::just_stopped())));
            expect(hit);
        };

        "a failed scope body still joins its pending children"_test = [] {
            native::context context;
            auto sch = context.get_scheduler();
            bool hit = false;
            std::string error;
            try {
                cores::run(context, failing_scope_body(sch, &hit));
            } catch (const std::runtime_error& e) {
                error = e.what();
            }
            expect(error == "scope body failed");
            expect(hit);
        };

        "a sender runs on the loop"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(static_cast<bool>(cores::run(context, execution::schedule_after(io, 1ms))));
        };

        "a task awaits a timer"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(static_cast<bool>(cores::run(context, sleep(io))));
        };

        "a task hops onto the loop"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(static_cast<bool>(cores::run(context, hop(io))));
        };

        "values flow through a chain"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(cores::run(context, chain(io)) == 10);
        };

        "a failure is rethrown by the awaiter"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(throws<std::runtime_error>([&] { cores::run(context, rethrows()); }));
        };

        "expect returns the error instead of throwing"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            auto ec = cores::run(context, expects(io));
            expect(static_cast<bool>(ec) >> fatal);
            expect(*ec == std::errc::connection_reset);
        };

        "an error code is thrown only when not expected"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(throws<std::system_error>([&] { cores::run(context, plain(io)); }));
        };

        "a stopped task completes as stopped and unwinds"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            bool hit = false;
            auto stopped = cores::run(context, execution::when_all(forever(io, &hit), execution::just_stopped()));
            expect(!stopped);
            expect(hit);
        };

        "stop point turns a stop into a value"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            bool hit = false;
            expect(cores::run(context, catches(io, &hit)) == 2);
            expect(hit);
        };

        "a stop requested before start ends the await inline"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            bool hit = false;
            coroutine::stop_source source;
            source.request_stop();
            expect(cores::run(context, catches_task(io, &hit), source.get_token()) == 2);
            expect(hit);
        };

        "with scope stops and joins its children"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            bool hit = false;
            expect(cores::run(context, scoped(io, &hit)) == 3);
            expect(hit);
        };

        "a child failure surfaces at join"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(throws<std::logic_error>([&] { cores::run(context, scope_failure(io)); }));
        };

        "a socket echoes over loopback"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(cores::run(context, loopback(io)) == "hello");
        };

        "a pending accept is cancelled"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(cores::run(context, pending_accept(io)) == 2);
        };

        "offload runs on the pool and comes back"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            execution::thread_pool pool{2};
            expect(cores::run(context, offload(io, pool)) == true);
        };

        "the mutex serializes in fifo order"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(cores::run(context, contend(io)) == std::vector<int>{0, 0, 1, 1, 2, 2});
        };

        "a call runs on the other core and comes back"_test = [] {
            cores::io_context<native::context> rt{2};
            cross out;
            std::atomic<bool> unwound = false;
            rt.run([&](ctx& io, std::size_t core) -> coroutine::task<> {
                if (core != 0)
                    return [](ctx&) -> coroutine::task<> { co_return; }(io);
                return [](cores::context<ctx>& rt, ctx& io, cross& out, std::atomic<bool>& unwound) -> coroutine::task<> {
                    auto& there = rt.get_scheduler(1);
                    out.caller = std::this_thread::get_id();
                    out.callee = co_await cores::call(io, there, [] { return thread_id(); });
                    out.resumed = std::this_thread::get_id();
                    out.value = co_await cores::call(io, there, [] { return add(20, 22); });
                    try {
                        co_await cores::call(io, there, [] { return boom_remote(); });
                    } catch (const std::runtime_error& e) {
                        out.error = e.what();
                    }
                    auto r = co_await coroutine::stop_point(
                        execution::when_all(cores::call(io, there, [&there, &unwound] { return remote_timer(there, &unwound); }), execution::just_stopped()));
                    out.stopped = !r;
                }(rt, io, out, unwound);
            });
            expect(out.callee != out.caller);
            expect(out.resumed == out.caller);
            expect(out.value == 42);
            expect(out.error == "remote boom");
            expect(out.stopped);
            expect(unwound.load());
        };

        "deadlines join native senders on value error and stopped completion"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            auto value = cores::run(context, coroutine::deadline(io, 1s, execution::then(execution::schedule_after(io, 1ms), [] { return 31; })));
            expect(value && *value == std::optional<int>{31});
            auto empty = cores::run(context, coroutine::deadline(io, 1s, execution::just()));
            expect(empty && empty->has_value());
            auto timed_out = cores::run(context, coroutine::deadline(io, 1ms, execution::schedule_after(io, 1h)));
            expect(timed_out && !timed_out->has_value());
            expect(!cores::run(context, coroutine::deadline(io, 1s, execution::just_stopped())));
            bool failed = false;
            try {
                static_cast<void>(cores::run(context, coroutine::deadline(io, 1s, boom_remote())));
            } catch (const std::runtime_error& error) {
                failed = std::string_view{error.what()} == "remote boom";
            }
            expect(failed);
        };

        "a deadline ends work that runs too long"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            bool fast_hit = false, slow_hit = false;
            auto [fast, late] = *cores::run(context, deadlines(io, &fast_hit, &slow_hit));
            expect(fast == std::optional<int>{7});
            expect(!late && slow_hit) << "timed out, and unwound";
            bool hit = false;
            auto stopped = !cores::run(context, execution::when_all(coroutine::deadline(io, 1h, slow(io, 1h, &hit)), execution::just_stopped()));
            expect(stopped && hit);
        };

        "the io_context stops every core"_test = [] {
            cores::io_context<native::context> rt{2};
            std::atomic<int> unwound = 0;
            std::jthread stopper([&] {
                std::this_thread::sleep_for(50ms);
                rt.stop();
            });
            rt.run([&](ctx& io, std::size_t) -> coroutine::task<> {
                return [](ctx& io, std::atomic<int>& unwound) -> coroutine::task<> {
                    struct count {
                        std::atomic<int>& n;
                        ~count() { ++n; }
                    } c{unwound};
                    co_await execution::schedule_after(io, 1h);
                }(io, unwound);
            });
            expect(unwound.load() == 2);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"foundation">{foundation_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
