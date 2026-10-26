
import std;
import boost.ut;
import coroutine;
import native;
import protocol;
import runner;
import support;
import isolation;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;

    auto run(native::context& context, std::filesystem::path dir, std::vector<std::string> argv, std::chrono::seconds timeout = 10s, std::uint64_t limit = 1 << 20,
             io::command extra = {}) {
        auto io = context.get_scheduler();
        extra.argv = std::move(argv);
        extra.cwd = std::move(dir);
        return *cores::run(context, ::runner::run_step(io, std::move(extra), timeout, limit, "test"));
    }

auto require(bool ok, std::string_view what) -> void {
    if (!ok)
        throw std::runtime_error(std::string{what});
}

auto command(native::context& rt, const std::filesystem::path& dir, isolation::scope& group, std::string script, std::chrono::milliseconds timeout = 5s)
    -> jobs::step_report {
    auto io = rt.get_scheduler();
    io::command cmd{.argv = {"python3", "-c", std::move(script)}, .cwd = dir, .max_cpu_seconds = 1, .isolation = group.target()};
    auto report = cores::run(rt, ::runner::run_step(io, std::move(cmd), timeout, 16384, "kernel", &group));
    require(report.has_value(), "unexpected cancellation");
    return *report;
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

        "an unusable cgroup prevents command side effects"_test = [] {
            test::scratch tmp{"cgroup-failure"};
            expect(throws<std::invalid_argument>([&] { isolation::pool p{tmp.dir, "deadbeef"}; }));
            native::context context;
            expect(throws<std::system_error>([&] {
                run(context, tmp.dir, {"sh", "-c", "echo executed > marker"}, 10s, 1024, {.isolation = isolation::target{(tmp.dir / "missing").string()}});
            }));
            expect(!std::filesystem::exists(tmp.dir / "marker"));
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
    static constexpr auto kernel_cases = [] {
        const char* configured = std::getenv("TEST_CGROUP_ROOT");
        if (!configured || !*configured) {
            skip / "delegated cgroup process limits"_test = [] {};
            return;
        }
        "delegated cgroup process limits"_test = [configured] {
            test::scratch tmp{"runner-kernel"};
            auto key = content::hash_of(tmp.dir.string());
            isolation::pool pool{configured, key};
            native::context rt;
        {
            auto group = pool.start(96ull << 20, {});
            auto r = command(rt, tmp.dir, *group, "import subprocess; subprocess.Popen(['sleep','60'], start_new_session=True); print('leader', flush=True)");
            require(r.exit_code == 0 && !r.timed_out && r.seconds < 3, "setsid descendant kept the command alive");
            group->close();
            require(!std::filesystem::exists(std::filesystem::path{group->target().identity}), "normal command cgroup not removed");
        }
        {
            auto group = pool.start(96ull << 20, {});
            auto r = command(rt, tmp.dir, *group,
                             "import subprocess,sys,time; code='import time; x=bytearray(64*1024*1024); time.sleep(30)'; "
                             "a=subprocess.Popen([sys.executable,'-c',code]); b=subprocess.Popen([sys.executable,'-c',code]); a.wait(); b.wait()",
                             10s);
            require(r.exit_code == 137 && r.output.contains("memory limit exceeded (OOM)"), "process-tree OOM was not reported");
            group->close();
        }
        {
            auto group = pool.start({}, 1);
            std::string burn = "import time; end=time.process_time()+0.65\nwhile time.process_time()<end: pass";
            auto first = command(rt, tmp.dir, *group, burn);
            auto second = command(rt, tmp.dir, *group, burn);
            require(first.exit_code == 0, "CPU budget exhausted on first short step");
            require(second.exit_code == 137 && second.output.contains("CPU budget exhausted"), "CPU budget did not span steps");
            group->close();
        }
        {
            auto group = pool.start({}, {});
            auto r = command(rt, tmp.dir, *group, "import subprocess,time; subprocess.Popen(['sleep','60'], start_new_session=True); time.sleep(60)", 200ms);
            require(r.timed_out && r.seconds < 3, "process-tree timeout did not finish promptly");
            group->close();
        }
        {
            auto group = pool.start({}, {});
            auto io = rt.get_scheduler();
            auto stop_later = [](auto& io) -> coroutine::task<> {
                co_await execution::schedule_after(io, 200ms);
                co_await execution::just_stopped();
            };
            auto stopped = !cores::run(
                rt,
                execution::when_all(
                    ::runner::run_step(io,
                                      io::command{.argv = {"python3", "-c",
                                                           "import subprocess,time; subprocess.Popen(['sleep','60'], start_new_session=True); time.sleep(60)"},
                                                  .cwd = tmp.dir,
                                                  .isolation = group->target()},
                                      10s, 1024, "cancel", group.get()),
                    stop_later(io)));
            require(stopped, "cancellation was not propagated");
            group->close();
        }
        };
    };
}

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"runner">{cases};
    boost::ut::suite<"runner_kernel">{kernel_cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
