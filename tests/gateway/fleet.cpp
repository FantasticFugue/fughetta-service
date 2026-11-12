import std;
import boost.ut;
import coroutine;
import native;
import protocol;
import fleet;
import configuration;
import enrollment;
import support;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = io::scheduler_of_t<native::context>;
    using socket = native::socket;

    static constexpr auto reconnect_suite = [] {
        "a fleet spreads retries and caps delays, and reset restores fast recovery"_test = [] {
            std::set<long long> first_delays;
            bool bounded = true;
            for (std::uint32_t seed = 0; seed < 1000; ++seed) {
                native::reconnect_backoff retry{seed};
                int ceiling = 1000;
                for (int attempt = 0; attempt < 20; ++attempt) {
                    auto delay = retry.next().count();
                    bounded &= delay >= ceiling / 2 && delay <= ceiling;
                    if (attempt == 0)
                        first_delays.insert(delay);
                    ceiling = std::min(ceiling * 2, 30000);
                }
                retry.reset();
                auto delay = retry.next();
                bounded &= delay >= 500ms && delay <= 1s;
            }
            expect(bounded);
            expect(first_delays.size() > 350u) << "1000 workers must spread their retries";
        };
    };

    struct event {
        std::coroutine_handle<> waiter{};
        bool set = false;

        struct awaiter {
            event& e;
            auto await_ready() const noexcept { return e.set; }
            void await_suspend(std::coroutine_handle<> h) noexcept { e.waiter = h; }
            void await_resume() const noexcept {}
        };
        auto wait() noexcept -> awaiter { return {*this}; }
        void fire() {
            set = true;
            if (auto w = std::exchange(waiter, {}))
                w.resume();
        }
    };

    auto linux_platform() -> jobs::platform { return {.os = "linux", .arch = "x86_64"}; }

    auto fake_worker(socket& s, std::string name, std::vector<std::string> tools = {"cmake"}, std::uint32_t max_inflight = 4, std::string auth = "",
                     bool publisher = false) -> coroutine::task<> {
        transport::channel<socket> chan{s};
        co_await chan.post(jobs::hello{.name = name,
                                       .auth = auth,
                                       .platform = linux_platform(),
                                       .max_inflight = max_inflight,
                                       .protocol = jobs::protocol_version,
                                       .tools = tools,
                                       .toolchain_id = std::string(64, 'a'),
                                       .dependency_publisher = publisher});
        try {
            for (;;) {
                auto f = co_await chan.recv();
                auto job = transport::decode<jobs::hash_file>(f);
                for (std::uint64_t i = 1; i <= 3; ++i)
                    co_await chan.post(jobs::progress{.done = i, .total = 3}, f.corr);
                co_await chan.reply(f.corr, jobs::digest{.hex = name + ":" + job.path.value(), .bytes = 3});
            }
        } catch (const std::exception&) {
        }
    }

    struct outcome {
        std::optional<std::size_t> first_pick;
        std::string hex;
        std::vector<std::uint64_t> progress;
        std::string missing;
    };

    struct harness {
        cores::io_context<native::context> rt{3};
        routing::router router{rt};
        event ready;
        event done;
        outcome out;

        auto host(ctx& io) -> coroutine::task<> {
            auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
            auto client = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
            auto [server, endpoint] = co_await net::async_accept(l);
            auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                children.spawn(router.workers(2).admit(io, std::move(server)));
                children.spawn(fake_worker(client, "fake"));
                while (!router.workers(2).holds({}))
                    co_await execution::schedule_after(io, 1ms);
                rt.post(0, [this] { ready.fire(); });
                co_await done.wait();
            };
            co_await coroutine::with_scope(std::move(run_children));
        }

        auto submit(ctx&) -> coroutine::task<> {
            co_await ready.wait();
            out.first_pick = router.routes(0).pick(std::vector<bool>(3, false));

            jobs::progress_log log;
            auto d = co_await router.run(0, {}, jobs::hash_file{.path = "acme/a.txt"}, &log);
            std::size_t seen = 0;
            for (auto& p : log.since(seen))
                out.progress.push_back(p.done);
            out.hex = d.hex;

            try {
                co_await router.run(0, {.needs = {.tools = {"mold"}}}, jobs::hash_file{.path = "nowhere/x"});
            } catch (const routing::no_worker& e) {
                out.missing = e.what();
            }
            auto pinned = co_await router.run(0, {.needs = {.toolchain_id = std::string(64, 'a')}}, jobs::hash_file{.path = "pinned"});
            expect(pinned.hex == "fake:pinned");
            bool mismatch = false;
            try {
                co_await router.run(0, {.needs = {.toolchain_id = std::string(64, 'b')}}, jobs::hash_file{.path = "wrong"});
            } catch (const routing::no_worker& error) {
                mismatch = !error.busy;
            }
            expect(mismatch) << "matching tool names cannot substitute another runtime fingerprint";
            rt.post(2, [this] { done.fire(); });
        }
    };

    static constexpr auto routing_suite = [] {
        "revoked identities stop dispatch immediately without waiting for the fleet watcher"_test = [] {
            test::scratch scratch{"fleet-identity-revoke"};
            enrollment::store registry{scratch.dir, "workspace-a"};
            auto credential = registry.add("machine", "owner");
            execution::thread_pool pool{1};
            cores::io_context<native::context> rt{1};
            routing::router router{rt};
            bool enabled = true;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto client = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
                auto [server, endpoint] = co_await net::async_accept(listener);
                transport::channel<socket> channel{client};
                co_await channel.post(jobs::hello{.name = "untrusted name",
                                                  .auth = credential.secret,
                                                  .protocol = jobs::protocol_version,
                                                  .worker_id = credential.worker_id,
                                                  .workspace_id = credential.workspace_id,
                                                  .heartbeat_seconds = 15});
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn(router.workers(0).admit(io, std::move(server), &registry, &pool, [&] { return enabled; }));
                    auto frame = co_await channel.recv();
                    expect(transport::decode<jobs::admission>(frame).accepted);
                    expect(router.workers(0).holds({}));
                    auto run_operation = [&](auto& selected) -> coroutine::task<> {
                        auto identity = router.workers(0).identity(selected);
                        expect(identity.first == "machine");
                        expect(identity.second == std::optional{credential.worker_id});
                        co_return;
                    };
                    co_await router.workers(0).with({}, std::move(run_operation));
                    enabled = false;
                    expect(router.workers(0).holds({}));
                    auto next = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
                    auto [next_peer, endpoint] = co_await net::async_accept(listener);
                    transport::channel<socket> next_channel{next};
                    co_await next_channel.post(jobs::hello{.name = "machine",
                                                           .auth = credential.secret,
                                                           .protocol = jobs::protocol_version,
                                                           .worker_id = credential.worker_id,
                                                           .workspace_id = credential.workspace_id,
                                                           .heartbeat_seconds = 15});
                    children.spawn(router.workers(0).admit(io, std::move(next_peer), &registry, &pool, [&] { return enabled; }));
                    auto refusal = transport::decode<jobs::admission>(co_await next_channel.recv());
                    expect(!refusal.accepted && refusal.code == "worker_enrollment_disabled");
                    expect(router.workers(0).holds({}));
                    registry.revoke(credential.worker_id);
                    expect(!router.workers(0).holds({}));
                    expect(!router.workers(0).describe(0).front().online);
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            rt.run(std::move(run_core));
        };
        "long-lived workers keep bounded recent tree affinity"_test = [] {
            cores::io_context<native::context> rt{1};
            routing::router router{rt};
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto socket = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
                auto [peer, endpoint] = co_await net::async_accept(listener);
                auto& fleet = router.workers(0);
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn(fleet.admit(io, std::move(peer)));
                    children.spawn(fake_worker(socket, "persistent"));
                    while (!fleet.holds({}))
                        co_await execution::schedule_after(io, 1ms);
                    for (int i = 0; i < 600; ++i) {
                        static constexpr auto run_operation = [](auto&) -> coroutine::task<> { co_return; };
                        co_await fleet.with({.tree_key = std::format("app/tree-{}", i)}, run_operation);
                    }
                    auto views = fleet.describe(0);
                    expect((views.size() == 1u) >> fatal);
                    expect(views.front().trees.size() <= 256u);
                    expect(std::ranges::contains(views.front().trees, "app/tree-599"));
                    expect(!std::ranges::contains(views.front().trees, "app/tree-0"));
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            rt.run(std::move(run_core));
        };
        "a worker lease stays valid if it disconnects while the gateway reads source"_test = [] {
            cores::io_context<native::context> rt{1};
            routing::router router{rt};
            bool removed = false, rejected = false;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto worker = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
                auto [peer, endpoint] = co_await net::async_accept(listener);
                transport::channel<socket> chan{worker};
                co_await chan.post(jobs::hello{.name = "disconnect", .platform = linux_platform(), .protocol = jobs::protocol_version});
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn(router.workers(0).admit(io, std::move(peer)));
                    while (!router.workers(0).holds({}))
                        co_await execution::schedule_after(io, 1ms);
                    auto run_operation = [&](auto& selected) -> coroutine::task<> {
                        worker.close();
                        co_await execution::schedule_after(io, 20ms);
                        removed = !router.workers(0).holds({});
                        static constexpr auto run_until_deadline = [](auto& selected, bool& rejected) -> coroutine::task<> {
                            auto result = co_await selected.call(jobs::hash_file{.path = "acme/a"});
                            rejected = !result;
                        };
                        auto result = co_await coroutine::deadline(io, 100ms, run_until_deadline(selected, rejected));
                        expect(result.has_value());
                    };
                    co_await router.workers(0).with({}, std::move(run_operation));
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            rt.run(std::move(run_core));
            expect(removed && rejected);
        };

        "a job runs where its data is and reports back"_test = [] {
            harness h;
            auto run_core = [&](ctx& io, std::size_t core) -> coroutine::task<> {
                if (core == 0)
                    return h.submit(io);
                if (core == 2)
                    return h.host(io);
                static constexpr auto run_operation = [](ctx&) -> coroutine::task<> { co_return; };
                return run_operation(io);
            };
            h.rt.run(std::move(run_core));
            expect(h.out.first_pick == std::optional<std::size_t>{2});
            expect(h.out.hex == "fake:acme/a.txt");
            expect(h.out.progress == std::vector<std::uint64_t>{1, 2, 3});
            expect(h.out.missing.starts_with("no build machine is online")) << h.out.missing;
        };
    };

    struct pick_outcome {
        std::string first, second, third, held, overflow;
        std::string capacity_err;
    };

    struct picker {
        cores::io_context<native::context> rt{1};
        routing::router router{rt};
        event held;
        pick_outcome out;

        auto who(routing::selection sel) -> coroutine::task<std::string> {
            static constexpr auto run_operation = [](auto& c) -> coroutine::task<std::string> {
                auto d = co_await c.call(jobs::hash_file{.path = "who"});
                expect(d.has_value() >> fatal);
                co_return d->hex;
            };
            co_return co_await router.workers(0).with(std::move(sel), run_operation);
        }
    };

    static constexpr auto selection_suite = [] {
        "dependency publishers cannot receive general work"_test = [] {
            picker h;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto client = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
                auto [peer, endpoint] = co_await net::async_accept(l);
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn(h.router.workers(0).admit(io, std::move(peer)));
                    children.spawn(fake_worker(client, "publisher", {"cmake"}, 1, "", true));
                    while (h.router.workers(0).describe(0).empty())
                        co_await execution::schedule_after(io, 1ms);
                    auto& fleet = h.router.workers(0);
                    expect(fleet.describe(0).front().dependency_publisher);
                    expect(!fleet.holds({}));
                    expect(!fleet.busy({}));
                    expect(fleet.diagnose({}) == "no live general build worker is available");
                    bool refused = false;
                    try {
                        co_await h.who({});
                    } catch (const routing::no_worker& error) {
                        refused = !error.busy;
                    }
                    expect(refused);
                    expect(co_await h.who({.needs = {.dependency_publisher = true}}) == "publisher:who");
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            h.rt.run(std::move(run_core));
        };
        "affinity beats load, then it is least inflight, bounded by capacity"_test = [] {
            picker h;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto a = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
                auto [pa, endpoint] = co_await net::async_accept(l);
                auto b = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
                auto [pb, endpoint_2] = co_await net::async_accept(l);
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn(h.router.workers(0).admit(io, std::move(pa)));
                    children.spawn(h.router.workers(0).admit(io, std::move(pb)));
                    children.spawn(fake_worker(a, "alpha", {"cmake"}, 1));
                    children.spawn(fake_worker(b, "beta", {"cmake"}, 1));
                    while (h.router.workers(0).describe(0).size() < 2)
                        co_await execution::schedule_after(io, 1ms);

                    routing::selection t1{.tree_key = "t1"};
                    h.out.first = co_await h.who(t1);
                    auto resident = h.out.first.substr(0, h.out.first.find(':'));
                    auto other = resident == "alpha" ? "beta" : "alpha";

                    h.out.second = co_await h.who({.tree_key = "t2"});
                    h.out.third = co_await h.who(t1);

                    static constexpr auto run_child = [](auto& h, routing::selection t1) -> coroutine::task<> {
                        auto run_operation = [&](auto&) -> coroutine::task<std::string> {
                            co_await h.held.wait();
                            co_return "held";
                        };
                        co_await h.router.workers(0).with(t1, std::move(run_operation));
                    };
                    children.spawn(run_child(h, t1));
                    auto filled = [&] {
                        for (auto& view : h.router.workers(0).describe(0))
                            if (view.name == resident)
                                return view.inflight > 0;
                        return false;
                    };
                    while (!filled())
                        co_await execution::schedule_after(io, 1ms);
                    h.out.overflow = co_await h.who(t1);
                    h.held.fire();

                    h.out.capacity_err = other;
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            h.rt.run(std::move(run_core));
            expect(h.out.first == "alpha:who" || h.out.first == "beta:who") << h.out.first;
            expect(h.out.second != h.out.first) << h.out.second << " least-inflight after the resident";
            expect(h.out.third == h.out.first) << h.out.third << "affinity beats the idle worker";
            expect(h.out.overflow == h.out.capacity_err + ":who") << h.out.overflow << "capacity overrides affinity";
        };

        "requirements exclude workers and name what is missing"_test = [] {
            picker h;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto a = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
                auto [pa, endpoint] = co_await net::async_accept(l);
                auto b = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
                auto [pb, endpoint_2] = co_await net::async_accept(l);
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn(h.router.workers(0).admit(io, std::move(pa)));
                    children.spawn(h.router.workers(0).admit(io, std::move(pb)));
                    children.spawn(fake_worker(a, "gcc-builder", {"gcc", "cmake"}));
                    children.spawn(fake_worker(b, "clang-builder", {"clang", "cmake"}));
                    while (h.router.workers(0).describe(0).size() < 2)
                        co_await execution::schedule_after(io, 1ms);

                    h.out.first = co_await h.who({.needs = {.tools = {"clang"}}});
                    try {
                        co_await h.who({.needs = {.tools = {"mold"}}});
                    } catch (const routing::no_worker& e) {
                        h.out.capacity_err = e.what();
                    }
                    try {
                        co_await h.who({.needs = {.os = "windows"}});
                    } catch (const routing::no_worker& e) {
                        h.out.overflow = e.what();
                    }
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            h.rt.run(std::move(run_core));
            expect(h.out.first == "clang-builder:who") << h.out.first;
            expect(h.out.capacity_err == "no attached worker provides all required tools (missing: mold)") << h.out.capacity_err;
            expect(h.out.overflow == "no attached worker matches the required platform os=windows") << h.out.overflow;
        };

        "a full worker is told apart from a missing one"_test = [] {
            picker h;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto a = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
                auto [pa, endpoint] = co_await net::async_accept(l);
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn(h.router.workers(0).admit(io, std::move(pa)));
                    children.spawn(fake_worker(a, "solo", {"cmake"}, 1));
                    while (h.router.workers(0).describe(0).empty())
                        co_await execution::schedule_after(io, 1ms);

                    static constexpr auto run_child = [](auto& h) -> coroutine::task<> {
                        auto run_operation = [&](auto&) -> coroutine::task<std::string> {
                            co_await h.held.wait();
                            co_return "held";
                        };
                        co_await h.router.workers(0).with({}, std::move(run_operation));
                    };
                    children.spawn(run_child(h));
                    while (h.router.workers(0).describe(0).at(0).inflight == 0)
                        co_await execution::schedule_after(io, 1ms);
                    try {
                        co_await h.who({});
                    } catch (const routing::no_worker& e) {
                        h.out.capacity_err = e.what();
                    }
                    h.held.fire();
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            h.rt.run(std::move(run_core));
            expect(h.out.capacity_err == "every attached worker is at its dispatch limit") << h.out.capacity_err;
        };
    };

    auto mute(socket& s, std::uint32_t protocol) -> coroutine::task<> {
        transport::channel<socket> chan{s};
        co_await chan.post(jobs::hello{.name = "mute", .platform = linux_platform(), .protocol = protocol});
        std::array<std::byte, 64> buf;
        while (co_await io::async_read_some(s, net::buffer(buf))) {
        }
    }

    struct watchdog_result {
        bool joined = false;
        bool dropped = false;
        bool refused = false;
        std::string in_flight;
    };

    auto watched(ctx& io, routing::router& router, watchdog_result& out) -> coroutine::task<> {
        auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto client = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
        auto [server, endpoint] = co_await net::async_accept(l);
        auto stranger = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
        auto [stranger_peer, endpoint_2] = co_await net::async_accept(l);
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(router.workers(0).watch(io));
            children.spawn(router.workers(0).admit(io, std::move(server)));
            children.spawn(mute(client, jobs::protocol_version));
            children.spawn(router.workers(0).admit(io, std::move(stranger_peer)));
            children.spawn(mute(stranger, jobs::protocol_version + 1));
            for (int i = 0; i < 100 && !router.workers(0).holds({}); ++i)
                co_await execution::schedule_after(io, std::chrono::milliseconds{5});
            out.joined = router.workers(0).holds({}) && router.workers(0).describe(0).size() == 1u;
            out.refused = router.workers(0).describe(0).size() == 1u;
            static constexpr auto run_child = [](routing::router& router, std::string& outcome) -> coroutine::task<> {
                outcome = "pending";
                try {
                    co_await router.run(0, {}, jobs::hash_file{.path = "acme/x"});
                    outcome = "answered";
                } catch (const std::exception& e) {
                    outcome = std::string{"failed: "} + e.what();
                }
            };
            children.spawn(run_child(router, out.in_flight));
            co_await execution::schedule_after(io, std::chrono::milliseconds{600});
            out.dropped = router.workers(0).describe(0).empty();
        };
        co_await coroutine::with_scope(std::move(run_children));
    }

    static constexpr auto watchdog_suite = [] {
        "a silent worker is dropped, a stranger never admitted"_test = [] {
            cores::io_context<native::context> rt{1};
            routing::router router{rt, std::chrono::milliseconds{200}};
            watchdog_result out;
            rt.run([&](ctx& io, std::size_t) { return watched(io, router, out); });
            expect(out.joined);
            expect(out.refused) << "the other protocol's worker is not listed";
            expect(out.dropped);
            expect(out.in_flight.starts_with("failed: ")) << out.in_flight << " (a disconnect is an error, not a stop)";
        };
    };

    struct gate_outcome {
        std::size_t admitted = 0;
    };

    static constexpr auto admission_suite = [] {
        "a worker enrolls with the configured token"_test = [] {
            cores::io_context<native::context> rt{1};
            config::document doc;
            doc.worker_token = "s3cret";
            config::slots config{1, std::make_shared<const config::document>(std::move(doc))};
            routing::router router{rt, routing::stale_after, &config};
            gate_outcome out;
            auto run_core = [&](ctx& io, std::size_t) -> coroutine::task<> {
                auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
                auto good = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
                auto [good_peer, endpoint] = co_await net::async_accept(l);
                auto bad = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
                auto [bad_peer, endpoint_2] = co_await net::async_accept(l);
                auto none = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
                auto [none_peer, endpoint_3] = co_await net::async_accept(l);
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn(router.workers(0).admit(io, std::move(good_peer)));
                    children.spawn(router.workers(0).admit(io, std::move(bad_peer)));
                    children.spawn(router.workers(0).admit(io, std::move(none_peer)));
                    children.spawn(fake_worker(good, "good", {"cmake"}, 4, "s3cret"));
                    children.spawn(fake_worker(bad, "bad", {"cmake"}, 4, "wrong"));
                    children.spawn(fake_worker(none, "none", {"cmake"}, 4, ""));
                    for (int i = 0; i < 100 && router.workers(0).describe(0).empty(); ++i)
                        co_await execution::schedule_after(io, std::chrono::milliseconds{5});
                    co_await execution::schedule_after(io, std::chrono::milliseconds{50});
                    out.admitted = router.workers(0).describe(0).size();
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            rt.run(std::move(run_core));
            expect(out.admitted == 1u) << "only the right token passes; a wrong or missing one does not";
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"reconnect">{reconnect_suite};
    boost::ut::suite<"routing">{routing_suite};
    boost::ut::suite<"selection">{selection_suite};
    boost::ut::suite<"watchdog">{watchdog_suite};
    boost::ut::suite<"admission">{admission_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
