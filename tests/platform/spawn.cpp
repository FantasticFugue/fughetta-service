#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import boost.ut;
import coroutine;
import native;
import process;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;

    auto output(native::process& child) -> coroutine::task<std::string> {
        std::array<std::byte, 4096> buffer;
        std::string text;
        while (auto count = co_await io::async_read_some(child, net::buffer(buffer)))
            text.append(reinterpret_cast<const char*>(buffer.data()), count);
        co_return text;
    }

    auto run(native::context& context, std::vector<std::string> argv, io::command command = {}) -> std::pair<std::string, int> {
        command.argv = std::move(argv);
        if (command.cwd.empty())
            command.cwd = std::filesystem::current_path();
        auto child = io::spawn_process(context.get_scheduler(), command);
        auto result = cores::run(context, execution::when_all(output(child), io::async_wait(child)));
        return {std::move(std::get<0>(*result)), std::get<1>(*result)};
    }

    static constexpr auto spawn_suite = [] {
        "process observations cover descendants and end with the child"_test = [] {
            native::context context;
            auto group = std::make_shared<io::process_group>();
            io::command command{.argv = {"sh", "-c", "sleep 60 & echo ready; read line; kill $!; wait"},
                                .cwd = std::filesystem::current_path(),
                                .stdio = io::process_stdio::blocking,
                                .processes = group};
            auto child = io::spawn_process(context.get_scheduler(), command);
            auto inspect = [&]() -> coroutine::task<> {
                std::array<std::byte, 64> bytes;
                auto count = co_await io::async_read_some(child, net::buffer(bytes));
                expect(count > 0);
                auto usage = native_process::usage(*group);
                expect(usage.has_value());
                if (usage) {
                    expect(usage->processes >= 2_u && usage->threads >= 2_u);
                    expect(usage->resident_bytes > 0_u);
                }
                auto own = native_process::self_usage();
                expect(own.has_value());
                if (own)
                    expect(own->processes == 1_u && own->threads > 0_u && own->resident_bytes > 0_u);
                std::string line = "finish\n";
                co_await child.write_some(std::as_bytes(std::span{line}));
                child.close_input();
            };
            auto result = cores::run(context, execution::when_all(inspect(), io::async_wait(child)));
            expect(result.has_value());
            expect(group->children.empty());
            expect(native_process::usage(*group)->processes == 0_u);
        };

        "exit code and merged output"_test = [] {
            native::context context;
            auto [text, code] = run(context, {"sh", "-c", "pwd; echo out; echo err >&2; exit 3"});
            expect(code == 3);
            expect(text == std::filesystem::current_path().string() + "\nout\nerr\n");
        };

        "an interactive child reads its input and keeps stdout for replies"_test = [] {
            native::context context;
            io::command command{
                .argv = {"sh", "-c", "read line; echo \"got $line\"; echo noise >&2; cat"}, .cwd = std::filesystem::current_path(),
                .stdio = io::process_stdio::blocking};
            auto child = io::spawn_process(context.get_scheduler(), command);
            auto talk = [](native::process& child) -> coroutine::task<> {
                std::string message = "hello\nrest\n";
                auto bytes = std::as_bytes(std::span{message});
                while (!bytes.empty())
                    bytes = bytes.subspan(co_await child.write_some(bytes));
                child.close_input();
            };
            auto result = cores::run(context, execution::when_all(talk(child), output(child), io::async_wait(child)));
            expect(std::get<0>(*result) == "got hello\nrest\n") << std::get<0>(*result);
            expect(std::get<1>(*result) == 0);
        };

        "a report on standard output goes to its file and the step output keeps standard error"_test = [] {
            native::context context;
            auto file = std::filesystem::temp_directory_path() / std::format("spawn-report-{}.json", ::getpid());
            {
                std::ofstream old{file};
                old << "a longer report from an earlier step";
            }
            auto [text, code] = run(context, {"sh", "-c", "echo '[]'; echo progress >&2"}, {.standard_output = file});
            std::ifstream in{file};
            std::string report{std::istreambuf_iterator<char>{in}, {}};
            std::filesystem::remove(file);
            expect(code == 0 && text == "progress\n") << text;
            expect(report == "[]\n") << report;
        };

        "missing executable exits with 127"_test = [] {
            native::context context;
            auto [text, code] = run(context, {"no-such-test-command"});
            expect(code == 127 && text.empty());
        };

        "invalid working directory exits with 126"_test = [] {
            native::context context;
            auto [text, code] = run(context, {"sh", "-c", "echo executed"}, {.cwd = "/no-such-spawn-test-directory"});
            expect(code == 126 && text.empty());
        };

        "child environment and descriptor inheritance"_test = [] {
            native::context context;
            auto listener = native::listener{context.get_scheduler(), {net::ip::address_v4::loopback(), 0}};
            auto [text, code] = run(context, {"sh", "-c", "ls /proc/self/fd | wc -l; echo \"$GREETING|$HOME\""},
                                    {.env = std::map<std::string, std::string>{{"GREETING", "hi"}, {"PATH", "/usr/bin:/bin"}}});
            expect(code == 0);
            expect(text == "4\nhi|\n") << text;
        };

        "memory and CPU limits"_test = [] {
            native::context context;
            auto [text, code] = run(context, {"sh", "-c", "ulimit -v; ulimit -t"}, {.max_memory_bytes = 512ull << 20, .max_cpu_seconds = 7});
            expect(code == 0);
            expect(text == "524288\n7\n") << text;
        };

        "failed resource limit prevents command execution"_test = [] {
            auto pid = ::fork();
            if (pid == 0) {
                rlimit hard{1, 1};
                if (::setrlimit(RLIMIT_CPU, &hard) != 0)
                    ::_exit(2);
                try {
                    native::context context;
                    auto [text, code] = run(context, {"sh", "-c", "echo executed"}, {.max_cpu_seconds = 2});
                    ::_exit(code == 126 && text.contains("cannot set RLIMIT_CPU") && !text.contains("executed") ? 0 : 3);
                } catch (...) {
                    ::_exit(4);
                }
            }
            expect(pid > 0);
            if (pid > 0) {
                int status = 0;
                expect(::waitpid(pid, &status, 0) == pid);
                expect(WIFEXITED(status) && WEXITSTATUS(status) == 0) << status;
            }
        };

        "leader exit closes descendant output"_test = [] {
            native::context context;
            auto start = std::chrono::steady_clock::now();
            auto [text, code] = run(context, {"sh", "-c", "sleep 30 & echo leader"});
            expect(code == 0 && text == "leader\n");
            expect(std::chrono::steady_clock::now() - start < 5s);
        };

        "large output is read through EOF"_test = [] {
            native::context context;
            auto [text, code] = run(context, {"sh", "-c", "i=0; while [ $i -lt 2000 ]; do echo line$i; i=$((i+1)); done"});
            std::string wanted;
            for (int i = 0; i < 2000; ++i)
                wanted += std::format("line{}\n", i);
            expect(code == 0 && text == wanted);
        };

        "kill returns signal exit code"_test = [] {
            native::context context;
            auto child = io::spawn_process(context.get_scheduler(), {.argv = {"sleep", "30"}, .cwd = std::filesystem::current_path()});
            expect(child.pid() > 0);
            child.kill();
            expect(cores::run(context, io::async_wait(child)) == 137);
        };

        "timeout kills command and retains output"_test = [] {
            native::context context;
            auto scheduler = context.get_scheduler();
            auto child = io::spawn_process(scheduler, {.argv = {"sh", "-c", "echo started; sleep 30"}, .cwd = std::filesystem::current_path()});
            auto start = std::chrono::steady_clock::now();
            auto result = cores::run(context, execution::when_all(output(child), io::async_wait(child),
                                                                  execution::then(execution::schedule_after(scheduler, 100ms), [&] { child.kill(); })));
            expect(result.has_value() >> fatal);
            expect(std::get<0>(*result) == "started\n" && std::get<1>(*result) == 137);
            expect(std::chrono::steady_clock::now() - start < 5s);
        };

        "stop kills and reaps command"_test = [] {
            native::context context;
            auto scheduler = context.get_scheduler();
            auto child = io::spawn_process(scheduler, {.argv = {"sleep", "30"}, .cwd = std::filesystem::current_path()});
            execution::inplace_stop_source stop;
            auto result = cores::run(
                context, execution::when_all(io::async_wait(child), execution::then(execution::schedule_after(scheduler, 20ms), [&] { stop.request_stop(); })),
                stop.get_token());
            expect(!result);
            expect(throws<std::system_error>([&] { cores::run(context, io::async_wait(child)); }));
        };

        "process destruction kills and reaps command"_test = [] {
            native::context context;
            int pid;
            {
                auto child = io::spawn_process(context.get_scheduler(), {.argv = {"sleep", "30"}, .cwd = std::filesystem::current_path()});
                pid = child.pid();
                auto moved = std::move(child);
                expect(moved.pid() == pid);
            }
            int status = 0;
            expect(::waitpid(pid, &status, WNOHANG) == -1);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"spawn">{spawn_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
