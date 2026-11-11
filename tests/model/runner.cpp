
import std;
import boost.ut;
import coroutine;
import native;
import protocol;
import runner;
import support;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;

    auto run(native::context& context, std::filesystem::path dir, std::vector<std::string> argv, std::chrono::seconds timeout = 10s,
             std::uint64_t limit = 1 << 20, io::command extra = {}) {
        auto io = context.get_scheduler();
        extra.argv = std::move(argv);
        extra.cwd = std::move(dir);
        return *cores::run(context, ::runner::run_step(io, std::move(extra), timeout, limit, "test"));
    }

    static constexpr auto cases = [] {
        "a command's exit code and merged output come back"_test = [] {
            test::scratch tmp{"replica"};
            native::context context;
            auto io = context.get_scheduler();
            auto r = run(context, tmp.dir, {"sh", "-c", "pwd; echo out; echo err >&2; exit 3"});
            expect(r.exit_code == 3);
            expect(r.output.contains("out\n") && r.output.contains("err\n"));
            expect(r.output.contains(tmp.dir.filename().string()));
            expect(!r.timed_out && !r.truncated);
            expect(run(context, tmp.dir, {"no-such-test-command"}).exit_code == 127);
        };

        "a command past its timeout is killed"_test = [] {
            test::scratch tmp{"replica"};
            native::context context;
            auto io = context.get_scheduler();
            auto r = run(context, tmp.dir, {"sh", "-c", "echo started; sleep 30"}, 1s);
            expect(r.timed_out);
            expect(r.exit_code == 128 + 9);
            expect(r.seconds < 10.0);
            expect(r.output == "started\n");
        };

        "only the tail of a long output is kept"_test = [] {
            test::scratch tmp{"replica"};
            native::context context;
            auto io = context.get_scheduler();
            auto r = run(context, tmp.dir, {"sh", "-c", "i=0; while [ $i -lt 2000 ]; do echo line$i; i=$((i+1)); done"}, 10s, 64);
            expect(r.truncated);
            expect(r.output.size() == 64u);
            expect(r.output.ends_with("line1999\n"));
        };

        "a stop kills the command"_test = [] {
            test::scratch tmp{"replica"};
            native::context context;
            auto io = context.get_scheduler();
            auto started = std::chrono::steady_clock::now();
            auto stopped =
                !cores::run(context, execution::when_all(::runner::run_step(io, io::command{.argv = {"sleep", "30"}, .cwd = tmp.dir}, 60s, 1024, "test"),
                                                         execution::just_stopped()));
            expect(stopped);
            expect(std::chrono::steady_clock::now() - started < 10s);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"runner">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
