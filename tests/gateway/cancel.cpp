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
            auto run_core = [&](ctx& io, std::size_t core) -> coroutine::task<> {
                if (core == 1)
                    {
                    static constexpr auto run_operation_2 = [](ctx& io, front::in_flight& requests, bool& finished, bool& cancelled) -> coroutine::task<> {
                        coroutine::stop_source source;
                        requests.enter(1, "n:7", &source);
                        static constexpr auto run_operation = [](ctx& io) -> coroutine::task<> { co_await execution::schedule_after(io, 30s); };
                        auto r = co_await coroutine::under(source.get_token(),
                                                           run_operation(io));
                        requests.leave(1, "n:7", &source);
                        cancelled = !r;
                        finished = true;
                    };
                    return run_operation_2(io, requests, finished, cancelled);
                }
                static constexpr auto run_operation = [](ctx& io, front::in_flight& requests) -> coroutine::task<> {
                    co_await execution::schedule_after(io, 100ms);
                    requests.cancel(0, "s:other");
                    requests.cancel(0, "n:7");
                };
                return run_operation(io, requests);
            };
            rt.run(std::move(run_core));
            expect(finished && cancelled);
            expect(std::chrono::steady_clock::now() - started < 5s);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"cancellation">{cancellation_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
