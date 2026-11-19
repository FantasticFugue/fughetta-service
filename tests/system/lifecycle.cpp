import launch;
import boost.ut;
import protocol;
import content;
import toolchain;
import host;
import durability;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "all entries report invalid addresses"_test = [] {
            test::scratch tmp{"addresses"};
            test::spit(tmp.dir / "gateway.json", R"({"server":{"http_bind":"localhost"}})");
            test::spit(tmp.dir / "worker.json", R"({"gateway":"127.0.0.1:65536"})");
            for (auto command : {test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {(tmp.dir / "gateway.json").string()}),
                                 test::source_command(SERVICE_EXECUTABLE, tmp.dir, {"--bind", "localhost"}),
                                 test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {(tmp.dir / "worker.json").string()})}) {
                test::child process{command};
                expect(process.wait(std::chrono::seconds{5}) == 1);
                expect(process.output().contains("invalid endpoint")) << process.output();
            }
        };

        "a real execute child builds over inherited stdio and exits with its front"_test = [] {
            test::scratch tmp{"worker-stdio"};
            std::atomic<bool> completed = false;
            auto run_operation = [&](native::scheduler&, native::socket peer) -> coroutine::task<> {
                transport::channel<native::socket> channel{peer};
                auto hello = co_await channel.recv();
                test::require(hello.kind == jobs::code<jobs::hello>, "missing worker greeting");
                auto run = [&](coroutine::scope& tasks) -> coroutine::task<> {
                    tasks.spawn(channel.pump([](const transport::frame&) {}));
                    content::manifest tree;
                    test::require((co_await channel.call(jobs::sync_offer{"app", tree})).has_value(), "offer failed");
                    test::require((co_await channel.call(jobs::source_bytes{})).has_value(), "sync failed");
                    const auto python = std::getenv("TEST_PYTHON") ? std::getenv("TEST_PYTHON") : "/usr/bin/python3";
                    jobs::build_request request{.project = "app", .tree = "stdio", .snapshot = tree.id(),
                        .steps = {{.name = "execute", .argv = {python, "-c",
                            "import os, sys; sys.stdout.buffer.write(('execute ' + os.environ.get('FUGHETTA_TEST_SECRET', 'no-secret') + '\\n').encode())"}}},
                        .timeout_s = 10};
                    auto result = co_await channel.call(request);
                    test::require(result.has_value(), "execute call failed");
                    test::require(result->ok(), result->steps.empty() ? "no execute steps" : result->steps.front().output);
                    expect(result->steps.front().output == "execute no-secret\n");
                    expect(std::filesystem::path{result->tree} == tmp.dir / "worker-execute/trees/app/stdio");
                    completed = true;
                };
                co_await coroutine::with_scope(run);
            };
            test::listener server{std::move(run_operation)};
            config::worker cfg;
            cfg.gateway = std::format("127.0.0.1:{}", server.endpoint().port());
            cfg.work_root = (tmp.dir / "worker").string();
            cfg.inherit_env = false;
            for (const auto& tool : toolchain::required_tools())
                cfg.tools[tool] = (tmp.dir / "no-tools" / tool).string();
            test::spit(tmp.dir / "worker.json", config::write(cfg));
            auto command = test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {"worker.json"});
            command.env = host::environment();
            (*command.env)["FUGHETTA_TEST_SECRET"] = "parent-secret";
            test::child process{command};
            expect(process.wait() == 1) << process.output();
            expect(completed.load()) << process.output();
            durability::dir_lock released{tmp.dir / "worker-execute", "execute tree"};
        };

        "worker exits when its gateway connection closes"_test = [] {
            test::scratch tmp{"worker-exit"};
            const auto snapshot = content::manifest{}.id().str();
            const auto tree = tmp.dir / "worker-execute/trees/app/native";
            std::filesystem::create_directories(tree);
            test::spit(tree.parent_path() / "native.manifest", "# snapshot " + snapshot + "\n");
            std::atomic<unsigned> attempts = 0;
            auto run_operation = [&](native::scheduler&, native::socket peer) -> coroutine::task<> {
                transport::channel<native::socket> channel{peer};
                auto frame = co_await channel.recv();
                test::require(frame.kind == jobs::code<jobs::hello>, "first worker message was not hello");
                const auto hello = transport::decode<jobs::hello>(frame);
                test::require(std::ranges::any_of(hello.slots, [&](const auto& slot) {
                    return slot.project == "app" && slot.tree == "native" && slot.snapshot == snapshot;
                }), "the worker greeting omitted its persisted checkout");
                ++attempts;
            };
            test::listener server{std::move(run_operation)};
            config::worker config;
            config.gateway = std::format("127.0.0.1:{}", server.endpoint().port());
            config.work_root = (tmp.dir / "worker").string();
            // This session executes no jobs; host SDK discovery is outside its lifecycle.
            config.inherit_env = false;
            for (const auto& tool : toolchain::required_tools())
                config.tools[tool] = (tmp.dir / "no-tools" / tool).string();
            test::spit(tmp.dir / "worker.json", json::write(config));
            test::child process{test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {"worker.json"})};
            expect(process.wait() == 1) << process.output();
            expect(attempts.load() == 1_u);
        };
    };
} // namespace

int main(int argc, const char** argv) {
    boost::ut::suite<"lifecycle">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
