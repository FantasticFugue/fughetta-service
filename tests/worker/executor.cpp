import std;
import boost.ut;
import coroutine;
import native;
import content;
import protocol;
import replica;
import executor;
import support;
import isolation;
import archive;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = native::scheduler;

    auto part(std::string_view text, std::uint64_t offset, std::uint64_t length, std::string_view whole) -> jobs::source_part {
        auto bytes = std::as_bytes(std::span{text.substr(offset, length)});
        return {content::hash_of(whole).str(), offset, whole.size(), {bytes.begin(), bytes.end()}};
    }
    auto blob(std::string_view text) -> jobs::source_part { return part(text, 0, text.size(), text); }

    auto entry(std::string path, std::string_view text, bool exec = false) -> content::entry {
        return {.path = std::move(path), .digest = content::hash_of(text), .size = text.size(), .executable = exec};
    }

    template <class F> void exercise(bool isolate, F work) {
        test::scratch tmp{"executor"};
        cores::io_context<native::context> rt{2};
        execution::thread_pool disk{2};
        replica::service service{
            rt, {.root = tmp.dir / "cache", .worker = "worker-test", .env = {{"VALUE", "worker"}}, .inherit_env = false, .isolate = isolate}, disk, 1};
        rt.run([&](ctx& io, std::size_t core) -> coroutine::task<> {
            if (core != 0)
                co_return;
            co_await service.prepare(0);
            auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
            co_await coroutine::with_scope([&](coroutine::scope& tasks) -> coroutine::task<> {
                tasks.spawn([](ctx& io, auto& listener, auto& service, coroutine::scope& tasks) -> coroutine::task<> {
                    auto socket = co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
                    transport::channel<native::socket> channel{socket};
                    transport::dispatcher<native::socket> dispatch;
                    service.install(dispatch, 0);
                    for (;;)
                        tasks.spawn(dispatch.handle(channel, co_await channel.recv()));
                }(io, listener, service, tasks));
                auto socket = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
                transport::channel<native::socket> channel{socket};
                co_await coroutine::with_scope([&](coroutine::scope& pump) -> coroutine::task<> {
                    pump.spawn(channel.pump([](const transport::frame&) {}));
                    co_await work(channel, tmp.dir);
                });
            });
        });
    }

    static constexpr auto cases = [] {
        "a checkout writes what changed and keeps build outputs"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir};
            content::manifest v1{{entry("bin/run.sh", "#!/bin/sh\n", true), entry("src/a.txt", "a1"), entry("src/old.txt", "old")}};

            auto plan = s.offer({"app", v1});
            expect(plan.snapshot == v1.id());
            expect(plan.missing.size() == 3u);
            expect(throws<std::runtime_error>([&] { s.checkout("app", plan.snapshot); }));
            expect(s.put({{blob("#!/bin/sh\n"), blob("a1"), blob("old")}}).stored == 3u);
            expect(s.offer({"app", v1}).missing.empty());

            auto dir = s.checkout("app", plan.snapshot);
            expect(dir == tmp.dir / "trees/app/default");
            expect(test::slurp(dir / "src/a.txt") == "a1");
            auto perms = std::filesystem::status(dir / "bin/run.sh").permissions();
            expect((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
            test::spit(dir / "build.out", "made by the build");

            content::manifest v2{{entry("bin/run.sh", "#!/bin/sh\n", true), entry("src/a.txt", "a2")}};
            auto plan2 = s.offer({"app", v2});
            expect(plan2.missing == std::vector<content::hash>{content::hash_of("a2")});
            s.put({{blob("a2")}});
            s.checkout("app", plan2.snapshot);
            expect(test::slurp(dir / "src/a.txt") == "a2");
            expect(!std::filesystem::exists(dir / "src/old.txt"));
            expect(std::filesystem::exists(dir / "build.out"));
        };

        "a file changed in the tree is restored, even at the same size"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir};
            content::manifest m{{entry("a.txt", "good")}};
            auto plan = s.offer({"app", m});
            s.put({{blob("good")}});
            auto dir = s.checkout("app", plan.snapshot);
            std::this_thread::sleep_for(20ms);
            test::spit(dir / "a.txt", "evil");
            s.checkout("app", plan.snapshot);
            expect(test::slurp(dir / "a.txt") == "good");
        };

        "a large blob arrives in parts and is checked whole"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir};
            std::string big(10000, 'x');
            big[1234] = 'y';
            content::manifest m{{entry("big.bin", big)}};
            auto plan = s.offer({"app", m});
            expect(s.put({{part(big, 0, 4000, big)}}).stored == 0u) << "not complete yet";
            expect(s.put({{part(big, 4000, 4000, big), part(big, 8000, 2000, big)}}).stored == 1u);
            expect(test::slurp(s.checkout("app", plan.snapshot) / "big.bin") == big);

            std::string other(10, 'a'), changed(10, 'b');
            auto first = part(other, 0, 5, other);
            auto second = part(changed, 5, 5, changed);
            second.digest = first.digest;
            s.put({{first}});
            try {
                s.put({{second}});
                expect(false) << "a mismatching blob was kept";
            } catch (const transport::coded_error& e) {
                expect(e.code == jobs::why::stale_blob);
            }
        };

        "the cache evicts unused blobs past its limit"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir, {.max_cache_bytes = 25}};
            std::string a(10, 'a'), b(10, 'b'), c(10, 'c');
            content::manifest m{{entry("c.txt", c)}};
            auto plan = s.offer({"app", m});
            s.put({{blob(a)}});
            std::this_thread::sleep_for(5ms);
            s.put({{blob(b)}});
            s.put({{blob(c)}});
            s.checkout("app", plan.snapshot);
            s.collect();
            expect(s.cache_bytes() <= 25u);
            auto missing = s.offer({"app", {{entry("a", a), entry("b", b), entry("c", c)}}}).missing;
            expect(std::ranges::contains(missing, content::hash_of(a))) << "the least recently used went first";
            expect(!std::ranges::contains(missing, content::hash_of(c))) << "a tree's blob stays";
        };

        "a compressed piece is stored as its plain bytes"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir};
            std::string text(4096, 'p');
            auto plan = s.offer({"app", {{entry("a", text)}}});
            auto frame = nodes::archive::pack(text);
            jobs::source_part packed{
                .digest = content::hash_of(text),
                .total = text.size(),
                .bytes = {reinterpret_cast<const std::byte*>(frame.data()), reinterpret_cast<const std::byte*>(frame.data()) + frame.size()},
                .packed = true};
            expect(s.put({{packed}}).stored == 1u);
            expect(test::slurp(s.checkout("app", plan.snapshot) / "a") == text);
            auto damaged = packed;
            damaged.digest = content::hash_of("other");
            damaged.bytes.resize(damaged.bytes.size() / 2);
            expect(throws([&] { s.put({{damaged}}); })) << "a damaged frame is refused";
            auto inflated = packed;
            inflated.digest = content::hash_of("short");
            inflated.total = 100;
            expect(throws([&] { s.put({{inflated}}); })) << "a frame is not unpacked past what the piece may hold";
        };

        "the store refuses what does not check out"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir};
            expect(throws<std::runtime_error>([&] { s.checkout("app", content::hash_of("nothing")); }));
            expect(throws<std::invalid_argument>([&] { s.offer({"../app", {}}); }));
            expect(throws<std::invalid_argument>([&] { s.offer({"app", {{entry("../x", "x")}}}); }));
        };

        "the cache keeps what an offer needs until its build checks out"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir, {.max_cache_bytes = 15}};
            std::string a(10, 'a'), b(10, 'b');
            auto first = s.offer({"app", {{entry("a", a)}}});
            s.put({{blob(a)}});
            auto second = s.offer({"app", {{entry("b", b)}}});
            s.put({{blob(b)}});
            s.collect();
            expect(s.offer({"app", {{entry("a", a)}}}).missing.empty()) << "pushed, not checked out yet: kept";
            expect(std::filesystem::exists(s.checkout("app", first.snapshot, "one") / "a"));
            expect(std::filesystem::exists(s.checkout("app", second.snapshot, "two") / "b"));
            for (int i = 0; i < 20; ++i)
                s.offer({"app", {{entry("c", std::string(i + 1, 'c'))}}});
            expect(std::filesystem::exists(s.checkout("app", first.snapshot, "one") / "a")) << "more than 8 offers since: still known";
        };

        "stale trees go, unless a build holds them; so do abandoned parts"_test = [] {
            test::scratch tmp{"replica"};
            replica::store s{tmp.dir, {.tree_ttl = 0h, .partial_ttl = 0h}};
            auto plan = s.offer({"app", {{entry("a", "x")}}});
            s.put({{blob("x")}});
            s.checkout("app", plan.snapshot, "busy");
            s.checkout("app", plan.snapshot, "idle");
            std::string big(100, 'z');
            auto part = blob(big);
            part.bytes.resize(10);
            s.put({{part}});
            std::this_thread::sleep_for(10ms);
            s.collect({"app/busy"});
            expect(std::filesystem::exists(s.tree("app/busy") / "a")) << "a build holds it";
            expect(!std::filesystem::exists(s.tree("app/idle")));
            expect(std::filesystem::is_empty(tmp.dir / "blobs" / "partial"));
        };

        "the cache lock and blob index survive reopening"_test = [] {
            test::scratch tmp{"cache-index"};
            {
                replica::store cache{tmp.dir};
                cache.put({{blob("retained")}});
                expect(throws<std::runtime_error>([&] { replica::store second{tmp.dir}; }));
            }
            replica::store cache{tmp.dir};
            expect(cache.cache_bytes() == 8u);
            auto offered = cache.offer({"app", {{entry("a", "retained")}}});
            expect(offered.missing.empty());
            expect(test::slurp(cache.checkout("app", offered.snapshot) / "a") == "retained");
        };
        "service dispatch syncs bytes, executes steps and stops at failure"_test = [] {
            exercise(false, [](auto& channel, const auto&) -> coroutine::task<> {
                content::manifest tree{{entry("input.txt", "hello")}};
                auto offer = co_await channel.call(jobs::sync_offer{"app", tree});
                expect(offer.has_value() >> fatal);
                expect(offer->missing == std::vector<content::hash>{content::hash_of("hello")});
                auto first = co_await channel.call(jobs::source_bytes{{part("hello", 0, 2, "hello")}});
                expect(first.has_value() && first->stored == 0u);
                auto last = co_await channel.call(jobs::source_bytes{{part("hello", 2, 3, "hello")}});
                expect(last.has_value() && last->stored == 1u);
                jobs::build_request request{
                    .project = "app",
                    .tree = "debug",
                    .snapshot = tree.id(),
                    .steps = {{.name = "first", .argv = {"/bin/sh", "-c", "cat input.txt; echo $VALUE; echo output > artifact"}, .env = {{"VALUE", "step"}}},
                              {.name = "fail", .argv = {"/bin/sh", "-c", "exit 3"}},
                              {.name = "unreached", .argv = {"/bin/sh", "-c", "echo bad > unreached"}}},
                    .env = {{"VALUE", "build"}}};
                auto report = co_await channel.call(request);
                expect(report.has_value() >> fatal);
                expect(report->worker == "worker-test" && report->steps.size() == 2u);
                expect(!report->ok() && report->steps[0].output == "hellostep\n" && report->steps[1].exit_code == 3);
                expect(test::slurp(std::filesystem::path{report->tree} / "artifact") == "output\n");
                expect(!std::filesystem::exists(std::filesystem::path{report->tree} / "unreached"));
                request.steps.resize(1);
                request.steps[0].argv = {"/bin/sh", "-c", "cat artifact; cat input.txt"};
                auto again = co_await channel.call(request);
                expect(again.has_value() && again->ok() && again->steps[0].output == "output\nhello");
                request.max_cpu_seconds = 1;
                request.steps[0].argv = {"/bin/sh", "-c", "touch denied"};
                auto denied = co_await channel.call(request);
                expect(!denied.has_value());
                expect(!std::filesystem::exists(std::filesystem::path{report->tree} / "denied"));
                request.max_cpu_seconds.reset();
                request.timeout_s = 1;
                request.steps[0].argv = {"/bin/sh", "-c", "echo start; sleep 30"};
                auto timeout = co_await channel.call(request);
                expect(timeout.has_value() && timeout->steps[0].timed_out && !timeout->ok());
            });
        };

        "service rejects a damaged push without caching its contents"_test = [] {
            exercise(false, [](auto& channel, const auto&) -> coroutine::task<> {
                auto corrupt = blob("bad");
                corrupt.digest = content::hash_of("yes");
                auto pushed = co_await channel.call(jobs::source_bytes{{corrupt}});
                expect(!pushed.has_value());
                auto offered = co_await channel.call(jobs::sync_offer{"app", {{entry("a", "yes")}}});
                expect(offered.has_value() && offered->missing.size() == 1u);
            });
        };
    };
} // namespace

int main(int argc, const char** argv) {
    boost::ut::suite<"cache">{cases};
    boost::ut::suite<"isolation">{[] {
        auto root = std::filesystem::path{"/sys/fs/cgroup/workers"};
        std::error_code ec;
        auto writable = std::filesystem::status(root, ec).permissions();
        if (ec || (writable & std::filesystem::perms::owner_write) == std::filesystem::perms::none) {
            skip / "delegated executor resource limits"_test = [] {};
            return;
        }
        "worker enforces its CPU share and build environment"_test = [] {
            exercise(true, [](auto& channel, const auto&) -> coroutine::task<> {
                content::manifest tree;
                auto offered = co_await channel.call(jobs::sync_offer{"app", tree});
                expect(offered.has_value() >> fatal);
                jobs::build_request request{
                    .project = "app",
                    .snapshot = tree.id(),
                    .steps =
                        {{.name = "budget",
                          .argv = {"/bin/sh", "-c",
                                   "group=$(sed -n 's/^0:://p' /proc/self/cgroup); cat /sys/fs/cgroup$group/cpu.max; printf '%s\\n' \"$OMP_NUM_THREADS\""}}},
                    .env = {{"OMP_NUM_THREADS", "99"}}};
                auto result = co_await channel.call(request);
                expect(result.has_value() >> fatal);
                expect(result->ok() && result->steps[0].output == "100000 100000\n1\n") << result->steps[0].output;
            });
        };
        "delegated executor resource limits"_test = [] {
            exercise(true, [](auto& channel, const auto& root) -> coroutine::task<> {
                content::manifest tree;
                auto offered = co_await channel.call(jobs::sync_offer{"app", tree});
                expect(offered.has_value() >> fatal);
                jobs::build_request request{.project = "app",
                                            .snapshot = tree.id(),
                                            .steps = {{.name = "limit",
                                                       .argv = {"/bin/sh", "-c",
                                                                "group=$(sed -n 's/^0:://p' /proc/self/cgroup); cat /sys/fs/cgroup$group/memory.max "
                                                                "/sys/fs/cgroup$group/memory.swap.max /sys/fs/cgroup$group/cpu.max"}}},
                                            .max_memory_bytes = 512ull << 20,
                                            .max_cpu_seconds = 7};
                auto result = co_await channel.call(request);
                expect(result.has_value() >> fatal);
                expect(result->ok() && result->steps[0].output == "536870912\n0\n100000 100000\n");
                auto group = std::filesystem::path{"/sys/fs/cgroup/workers"} /
                             std::format("worker-{}", content::hash_of(std::filesystem::canonical(root / "cache").string()).view());
                for (const auto& child : std::filesystem::directory_iterator(group))
                    expect(!child.is_directory());
                request.max_cpu_seconds = 1;
                request.steps = {
                    {.name = "first", .argv = {"python3", "-c", "import time; end=time.process_time()+0.65\nwhile time.process_time()<end: pass"}},
                    {.name = "second", .argv = {"python3", "-c", "import time; end=time.process_time()+0.65\nwhile time.process_time()<end: pass"}}};
                auto exhausted = co_await channel.call(request);
                expect(exhausted.has_value() >> fatal);
                expect(exhausted->steps.size() == 2u && exhausted->steps[0].exit_code == 0);
                expect(exhausted->steps[1].exit_code == 137 && exhausted->steps[1].output.contains("CPU budget exhausted"));
                for (const auto& child : std::filesystem::directory_iterator(group))
                    expect(!child.is_directory());
            });
        };
    }};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
