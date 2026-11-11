import std;
import boost.ut;
import coroutine;
import native;
import protocol;
import tools;
import front;
import world;
import mcp;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = io::scheduler_of_t<native::context>;

    static constexpr auto cancellation_suite = [] {
        "a cancel reaches its request on another core"_test = [] {
            cores::io_context<native::context> rt{2};
            front::in_flight requests{rt};
            bool finished = false, cancelled = false;
            auto started = std::chrono::steady_clock::now();
            rt.run([&](ctx& io, std::size_t core) -> coroutine::task<> {
                if (core == 1)
                    return [](ctx& io, front::in_flight& requests, bool& finished, bool& cancelled) -> coroutine::task<> {
                        coroutine::stop_source source;
                        requests.enter(1, "n:7", &source);
                        auto r = co_await coroutine::under(source.get_token(),
                                                           [](ctx& io) -> coroutine::task<> { co_await execution::schedule_after(io, 30s); }(io));
                        requests.leave(1, "n:7", &source);
                        cancelled = !r;
                        finished = true;
                    }(io, requests, finished, cancelled);
                return [](ctx& io, front::in_flight& requests) -> coroutine::task<> {
                    co_await execution::schedule_after(io, 100ms);
                    requests.cancel(0, "s:other");
                    requests.cancel(0, "n:7");
                }(io, requests);
            });
            expect(finished && cancelled);
            expect(std::chrono::steady_clock::now() - started < 5s);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"cancellation">{cancellation_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
