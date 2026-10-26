import launch;
import boost.ut;
import protocol;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "all entries report invalid addresses"_test = [] {
            test::scratch tmp{"addresses"};
            test::spit(tmp.dir / "gateway.json", R"({"server":{"http_bind":"localhost"}})");
            test::spit(tmp.dir / "worker.json", R"({"gateway":"127.0.0.1:65536"})");
            for (auto command : {
                    test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {(tmp.dir / "gateway.json").string()}),
                    test::source_command(SERVICE_EXECUTABLE, tmp.dir, {"--bind", "localhost"}),
                    test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {(tmp.dir / "worker.json").string()})}) {
                test::child process{command};
                expect(process.wait(std::chrono::seconds{5}) == 1);
                expect(process.output().contains("invalid endpoint")) << process.output();
            }
        };

        "worker reports the unavailable isolation root"_test = [] {
#ifdef __linux__
            if (std::filesystem::exists("/sys/fs/cgroup/workers")) {
                std::println("SKIP: unavailable isolation root test requires /sys/fs/cgroup/workers to be absent");
                return;
            }
            test::scratch tmp{"isolation"};
            config::worker config;
            config.compute_threads = 1;
            config.work_root = (tmp.dir / "worker").string();
            test::spit(tmp.dir / "worker.json", rfl::json::write(config));
            test::child process{test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {(tmp.dir / "worker.json").string()})};
            expect(process.wait(std::chrono::seconds{5}) == 1);
            expect(process.output().contains("cannot inspect isolation root"));
            expect(process.output().contains("/sys/fs/cgroup/workers"));
#else
            std::println("SKIP: cgroup isolation root diagnostics require Linux");
#endif
        };

        "worker exits when its gateway connection closes"_test = [] {
#ifdef __linux__
            if (!std::filesystem::exists("/sys/fs/cgroup/workers")) {
                skip / "worker connection loss requires delegated cgroups"_test = [] {};
                return;
            }
#endif
            test::scratch tmp{"worker-exit"};
            std::atomic<unsigned> attempts = 0;
            test::listener server{[&](native::scheduler&, native::socket peer) -> coroutine::task<> {
                transport::channel<native::socket> channel{peer};
                auto frame = co_await channel.recv();
                test::require(frame.kind == jobs::code<jobs::hello>, "first worker message was not hello");
                ++attempts;
            }};
            config::worker config;
            config.gateway = std::format("127.0.0.1:{}", server.endpoint().port());
            config.work_root = (tmp.dir / "worker").string();
            test::spit(tmp.dir / "worker.json", rfl::json::write(config));
            test::child process{test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {"worker.json"})};
            expect(process.wait() == 1) << process.output();
            expect(attempts.load() == 1_u);
        };
    };
}

int main(int argc, const char** argv) {
    boost::ut::suite<"lifecycle">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
