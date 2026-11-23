#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

import boost.ut;
import descriptor;
import host;
import launch;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;

    auto children(int pid) -> std::set<int> {
        std::set<int> result;
        for (const auto& task : fs::directory_iterator{std::format("/proc/{}/task", pid)}) {
            std::ifstream ids{task.path() / "children"};
            for (int child; ids >> child;)
                result.insert(child);
        }
        return result;
    }

    auto notifications(const std::string& name) -> descriptor::fd {
        descriptor::fd socket{::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
        if (socket.get() < 0)
            descriptor::fail("cannot open notification socket", name);
        sockaddr_un address{.sun_family = AF_UNIX, .sun_path = {}};
        std::ranges::copy(name, address.sun_path + 1);
        auto size = sizeof(address.sun_family) + 1 + name.size();
        if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), static_cast<socklen_t>(size)) < 0)
            descriptor::fail("cannot bind notification socket", name);
        return socket;
    }

    auto received(const descriptor::fd& socket) -> std::vector<std::string> {
        std::vector<std::string> messages;
        std::array<char, 4096> buffer;
        for (;;) {
            auto size = ::recv(socket.get(), buffer.data(), buffer.size(), 0);
            if (size >= 0)
                messages.emplace_back(buffer.data(), static_cast<std::size_t>(size));
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
                return messages;
            else if (errno != EINTR)
                descriptor::fail("cannot read notification socket", {});
        }
    }

    struct health {
        std::string lifecycle;
        std::string instance;
        std::optional<bool> source;
        std::size_t online_workers = 0;
    };

    struct assignment {
        bool managed = false;
        bool online = false;
        unsigned parallel = 0;
        unsigned compute_threads = 0;
        unsigned max_inflight = 0;
    };

    constexpr auto cases = [] {
        "managed workers receive their budget and are reaped after failure and restart"_test = [] {
            test::scratch tmp{"managed-budget"};
            auto port = test::port();
            auto doc = test::document(tmp.dir, port, test::port());
            doc.runtime = {.threads = 10, .jobs = 6, .workers = 2, .worker_memory_bytes = {}};
            const auto worker_count = doc.runtime.workers;
            test::spit(tmp.dir / "config.json", config::write(doc));

            for (bool interrupt : {true, false}) {
                auto name = std::format("fughetta-{}-{}", ::getpid(), interrupt ? "failure" : "restart");
                auto receiver = notifications(name);
                auto command = test::command(SERVICE_EXECUTABLE, tmp.dir, {"--instance", "lifecycle-test", "config.json"});
                command.env = host::environment();
                (*command.env)["NOTIFY_SOCKET"] = "@" + name;
                test::child process{command};
                test::ready(process, port);
                test::eventually([&] {
                    test::require(!process.finished(), process.output());
                    auto response = test::request(port, http::method::GET, "/healthz");
                    test::require(response.status == 200, response.body);
                    auto state = test::decode<health>(response.body);
                    if (!state.source.value_or(false) || state.online_workers != worker_count || state.lifecycle != "ready")
                        return false;
                    expect(state.instance == "fughetta-service-lifecycle-test");
                    return true;
                }, 20s);

                const auto pids = children(process.pid());
                expect(pids.size() == worker_count + 1);
                std::vector<int> workers;
                for (auto pid : pids) {
                    const auto command_line = test::slurp(std::format("/proc/{}/cmdline", pid));
                    if (std::ranges::any_of(command_line | std::views::split('\0'), [](auto arg) { return std::string_view{arg} == "worker"; }))
                        workers.push_back(pid);
                }
                expect((workers.size() == worker_count) >> fatal);
                auto response = test::request(port, http::method::GET, "/api/v1/workers");
                test::require(response.status == 200, response.body);
                auto assignments = test::decode<std::vector<assignment>>(response.body);
                expect(assignments.size() == worker_count);
                for (const auto& assigned : assignments) {
                    expect(assigned.online && assigned.managed);
                    expect(assigned.parallel == 3_u);
                    expect(assigned.compute_threads == 1_u);
                    expect(assigned.max_inflight == 1_u);
                }
                if (interrupt) {
                    if (::kill(workers.front(), SIGKILL) < 0)
                        descriptor::fail("cannot stop the managed worker", std::to_string(workers.front()));
                    expect(process.wait(15s) == 1) << process.output();
                } else {
                    expect(process.stop(15s) == 0) << process.output();
                }

                const auto messages = received(receiver);
                auto initialized = std::ranges::find(messages, "STATUS=initialized");
                auto ready = std::ranges::find_if(messages, [](const auto& message) { return message.contains("READY=1"); });
                expect(ready != messages.end() && initialized < ready);
                expect(std::ranges::any_of(messages, [](const auto& message) { return message.contains("STOPPING=1"); }));
                for (auto pid : pids)
                    expect(!fs::exists(std::format("/proc/{}", pid))) << pid;
            }
        };
    };
} // namespace

int main(int argc, const char** argv) {
    boost::ut::suite<"managed-budget">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
