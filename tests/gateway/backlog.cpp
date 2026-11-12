import std;
import semantic;
import boost.ut;
import coroutine;
import native;
import configuration;
import registry;
import fleet;
import tools;
import protocol;
import world;
import support;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = native::scheduler;

    // A backlog on one core with fake jobs: each records when it ran and answers after a short while.
    struct harness {
        test::scratch tmp{"backlog"};
        cores::io_context<native::context> rt;
        execution::thread_pool pool{1};
        config::slots config{rt.size(), nullptr};
        routing::router router{rt};
        storage::registry files{rt, tmp.dir / "state", config, pool, net::ip::tcp::endpoint{net::ip::address_v4{{127, 0, 0, 1}}, 1}};
        tools::services services{config.slot(0), router, files, 0};
        tools::backlog queue{rt, services};
        std::vector<std::string> ran, trees;
        std::size_t running = 0, most = 0;

        explicit harness(std::uint32_t concurrent, std::size_t cores = 1) : rt(cores) {
            config::document d;
            d.limits.concurrent_builds = concurrent;
            auto snapshot = std::make_shared<const config::document>(std::move(d));
            for (std::size_t c = 0; c < rt.size(); ++c)
                config.set(c, snapshot);
            services.queue = &queue;
        }

        // A job runs on core zero, as the backlog's own jobs do, whichever core asked for it.
        auto job(ctx&, std::string name, std::chrono::milliseconds takes = 30ms) -> tools::backlog::runner {
            auto run_job = [this, name, takes](tools::services&, jobs::progress_log& progress,
                                               std::string tree) -> coroutine::task<std::optional<tools::backlog::outcome>> {
                ran.push_back(name);
                trees.push_back(std::move(tree));
                most = std::max(most, ++running);
                progress.push({.stage = name});
                co_await execution::schedule_after(rt.get_scheduler(0), takes);
                --running;
                co_return tools::config_result{.configuration = name, .worker = "w", .ok = true};
            };
            return run_job;
        }

        static auto entry(std::string key, std::string actor, std::string configuration = "Debug") -> tools::backlog::entry {
            return {.key = std::move(key),
                    .project = "app",
                    .configuration = std::move(configuration),
                    .operation = "build",
                    .actor = std::move(actor),
                    .cacheable = true};
        }

        // A dependency.fill entry as the gateway writes it: equal keys for equal selector and toolchain input, never cached.
        static auto fill(std::string key, std::string actor) -> tools::backlog::entry {
            return {.key = key, .project = "app", .configuration = "dependencies", .operation = "dependencies", .actor = std::move(actor), .cacheable = false};
        }

        static auto built(const std::shared_ptr<tools::backlog::ticket>& t) -> const tools::config_result& {
            return std::get<tools::config_result>(t->result());
        }

        static auto done(ctx& io, const std::shared_ptr<tools::backlog::ticket>& t) -> coroutine::task<bool> {
            for (int i = 0; i < 400 && !t->done(); ++i)
                co_await execution::schedule_after(io, 5ms);
            co_return t->done();
        }

        // The queue serves on core zero; `body` runs on the last core, and the run ends when it does.
        template <class Body> void run(Body body) {
            std::atomic<bool> over{false};
            auto run_core = [&](ctx& io, std::size_t core) -> coroutine::task<> {
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    if (core == 0)
                        children.spawn(queue.serve(io));
                    if (core + 1 == rt.size()) {
                        co_await body(io);
                        over = true;
                    }
                    while (!over)
                        co_await execution::schedule_after(io, 5ms);
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            rt.run(std::move(run_core));
        }
    };

    static constexpr auto cases = [] {
        "a caller on another core queues, follows, lists and cancels jobs"_test = [] {
            harness h{1, 2};
            const worlds::actor x{.issuer = "token", .user = "x", .caller = "x", .role = "member"};
            auto ex = worlds::identity_key(x);
            std::vector<std::string> stages;
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto first = co_await h.queue.submit(1, harness::entry("first", ex, "A"), h.job(io, "first", 100ms));
                auto second = co_await h.queue.submit(1, harness::entry("second", ex, "B"), h.job(io, "second"));
                auto listed = co_await h.queue.list(1);
                expect(listed.size() == 2u && listed[0].state == "running" && listed[1].state == "queued" && listed[1].ahead == 0u);
                expect(co_await h.queue.cancel(1, second->job(), x) == "cancelled");
                expect(second->done() && harness::built(second).error == "cancelled");
                std::size_t seen = 0;
                while (!first->done()) {
                    for (auto& p : first->progress(seen))
                        stages.push_back(p.stage);
                    co_await execution::schedule_after(io, 5ms);
                }
                for (auto& p : first->progress(seen))
                    stages.push_back(p.stage);
                expect(harness::built(first).ok);
            };
            h.run(std::move(exercise));
            expect(stages == std::vector<std::string>{"first"}) << "progress reported on core zero reaches the caller once";
            expect(h.ran == std::vector<std::string>{"first"});
        };

        "a job takes the free tree that last built the most of its files"_test = [] {
            harness h{2};
            auto job = [](std::string key, std::vector<std::string> files) {
                auto e = harness::entry(key, key, key);
                e.trees = "set";
                e.files = std::move(files);
                return e;
            };
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto a = co_await h.queue.submit(0, job("a", {"f1", "f2"}), h.job(io, "a"));
                auto b = co_await h.queue.submit(0, job("b", {"g1", "g2"}), h.job(io, "b"));
                expect(co_await harness::done(io, a) && co_await harness::done(io, b));
                auto c = co_await h.queue.submit(0, job("c", {"f1", "f2", "f3"}), h.job(io, "c"));
                expect(co_await harness::done(io, c));
                auto d = co_await h.queue.submit(0, job("d", {"g1"}), h.job(io, "d"));
                expect(co_await harness::done(io, d));
            };
            h.run(std::move(exercise));
            expect((h.trees.size() == 4u) >> fatal);
            expect(h.trees[0] != h.trees[1]) << "jobs running at once take different trees";
            expect(h.trees[2] == h.trees[0]) << "c builds on where a left off";
            expect(h.trees[3] == h.trees[1]) << "d builds on where b left off";
        };

        "preparation updates the files used to select a hot directory"_test = [] {
            harness h{2};
            auto entry = [](std::string key, std::vector<std::string> files) {
                auto e = harness::entry(key, key, key);
                e.trees = "set";
                e.files = std::move(files);
                return e;
            };
            std::string prepared;
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto a = co_await h.queue.submit(0, entry("a", {"old-a"}), h.job(io, "a"));
                auto b = co_await h.queue.submit(0, entry("b", {"old-b"}), h.job(io, "b"));
                expect(co_await harness::done(io, a) && co_await harness::done(io, b));
                auto run_operation = [&prepared](tools::services&, jobs::progress_log&,
                                                 std::string tree) -> coroutine::task<std::optional<tools::backlog::outcome>> {
                    prepared = tree;
                    co_return language::result{.status = "prepared", .worker = "w"};
                };
                auto p = co_await h.queue.submit(0, entry("prepare", {"old-b", "new"}), std::move(run_operation));
                expect(co_await harness::done(io, p));
                auto c = co_await h.queue.submit(0, entry("c", {"new"}), h.job(io, "c"));
                expect(co_await harness::done(io, c));
            };
            h.run(std::move(exercise));
            expect(prepared == h.trees[1] && h.trees.back() == prepared);
        };

        "jobs past the limit wait their turn instead of failing"_test = [] {
            harness h{1};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto a = co_await h.queue.submit(0, harness::entry("a", "x", "A"), h.job(io, "a"));
                auto b = co_await h.queue.submit(0, harness::entry("b", "y", "B"), h.job(io, "b"));
                expect(b->ahead() == 0u && !b->done()) << "b waits while a runs";
                expect(co_await harness::done(io, a) && co_await harness::done(io, b));
                expect(harness::built(b).ok && harness::built(b).configuration == "b");
            };
            h.run(std::move(exercise));
            expect(h.most == 1u) << "one at a time";
            expect(h.ran == std::vector<std::string>{"a", "b"});
        };

        "equal jobs run once, and a finished build answers again unless asked fresh"_test = [] {
            harness h{4};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto first = co_await h.queue.submit(0, harness::entry("same", "x"), h.job(io, "first"));
                auto second = co_await h.queue.submit(0, harness::entry("same", "y"), h.job(io, "second"));
                expect(first->job() == second->job()) << "the second asker joins the first job";
                expect(co_await harness::done(io, first) && co_await harness::done(io, second));
                expect(harness::built(first).configuration == "first" && harness::built(second).configuration == "first");
                auto cached = co_await h.queue.submit(0, harness::entry("same", "z"), h.job(io, "third"));
                expect(cached->done() && harness::built(cached).configuration == "first") << "from the cache, at once";
                expect(harness::built(first).timings.queue_seconds.value_or(-1) >= 0);
                expect(harness::built(second).timings.queue_seconds.value_or(-1) >= 0);
                const auto& timing = harness::built(cached).timings;
                expect(timing.sync_seconds == 0 && timing.queue_seconds == 0 && timing.worker_seconds == 0);
                expect(timing.preparation_seconds == 0 && timing.execution_seconds == 0);
                auto fresh = harness::entry("same", "z");
                fresh.fresh = true;
                auto again = co_await h.queue.submit(0, fresh, h.job(io, "fresh"));
                expect(co_await harness::done(io, again) && harness::built(again).configuration == "fresh");
            };
            h.run(std::move(exercise));
            expect(h.ran == std::vector<std::string>{"first", "fresh"});
        };

        "equal fills from different callers run once, and a finished fill is not cached"_test = [] {
            harness h{4};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto x = co_await h.queue.submit(0, harness::fill("same", "x"), h.job(io, "fill"));
                auto y = co_await h.queue.submit(0, harness::fill("same", "y"), h.job(io, "fill"));
                expect(x->job() == y->job()) << "the second caller joins the in-flight fill of the same key";
                expect(co_await harness::done(io, x) && co_await harness::done(io, y));
                expect(harness::built(x).ok && harness::built(y).configuration == "fill");
                auto later = co_await h.queue.submit(0, harness::fill("same", "z"), h.job(io, "again"));
                expect(later->job() != x->job()) << "fills are not memoized";
                expect(co_await harness::done(io, later));
                expect(later->reuse() == "executed" && harness::built(later).configuration == "again")
                    << "once the archive may have pruned it, the same selector and toolchain fill again";
            };
            h.run(std::move(exercise));
            expect(h.ran == std::vector<std::string>{"fill", "again"}) << "shared in flight, never from a result cache";
        };

        "one slot serves both consumer flows once their shared fill is done"_test = [] {
            harness h{1};
            // Two gateway flows, each like fill_dependencies: submit and await the fill first, the consumer job only after.
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                std::size_t completed = 0;
                auto flow = [&](std::string name) -> coroutine::task<> {
                    auto fill = co_await h.queue.submit(0, harness::fill("shared", name), h.job(io, "fill"));
                    expect(co_await harness::done(io, fill) && harness::built(fill).ok);
                    auto consumer = co_await h.queue.submit(0, harness::entry(name, name), h.job(io, name));
                    expect(co_await harness::done(io, consumer) && harness::built(consumer).ok);
                    ++completed;
                };
                auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                    for (auto name : {"one", "two"})
                        children.spawn(flow(name));
                    for (int n = 0; n < 400 && completed != 2; ++n)
                        co_await execution::schedule_after(io, 5ms);
                    expect(completed == 2u);
                };
                co_await coroutine::with_scope(std::move(run_children));
            };
            h.run(std::move(exercise));
            expect((h.ran.size() == 3u) >> fatal);
            expect(h.ran.front() == "fill") << "no consumer held the only slot while its fill was still waiting";
            expect(std::ranges::count(h.ran, "one") == 1 && std::ranges::count(h.ran, "two") == 1) << "both flows completed";
            expect(h.most == 1u) << "never two at once";
        };

        "a shared fill outlives one caller leaving, and is dropped when its last waiter leaves"_test = [] {
            harness h{1};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto first = co_await h.queue.submit(0, harness::fill("keep", "x"), h.job(io, "fill", 100ms));
                auto second = co_await h.queue.submit(0, harness::fill("keep", "y"), h.job(io, "fill"));
                expect(first->job() == second->job());
                first->leave(); // one caller goes away
                expect(co_await harness::done(io, second) && harness::built(second).ok) << "the fill finishes for the caller still waiting";
                auto blocker = co_await h.queue.submit(0, harness::entry("blocker", "z"), h.job(io, "blocker", 100ms));
                auto one = co_await h.queue.submit(0, harness::fill("drop", "x"), h.job(io, "dropped"));
                auto two = co_await h.queue.submit(0, harness::fill("drop", "y"), h.job(io, "dropped"));
                expect(one->job() == two->job());
                one->leave();
                two->leave(); // nobody needs the queued fill any more
                expect(co_await harness::done(io, blocker));
                auto listed = co_await h.queue.list(0);
                expect(std::ranges::any_of(listed, [](auto& v) { return v.state == "cancelled" && v.operation == "dependencies"; }))
                    << "the fill its last waiter left is cancelled";
            };
            h.run(std::move(exercise));
            expect(h.ran == std::vector<std::string>{"fill", "blocker"}) << "the fill nobody waited for never ran";
        };

        "the last fill waiter cancels an executing dependency job"_test = [] {
            harness h{1};
            bool started = false, exited = false, completed = false;
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto work = [&](tools::services&, jobs::progress_log&, std::string) -> coroutine::task<std::optional<tools::backlog::outcome>> {
                    started = true;
                    struct finish {
                        bool& value;
                        ~finish() { value = true; }
                    } finished{exited};
                    co_await execution::schedule_after(io, 5s);
                    completed = true;
                    co_return tools::config_result{.ok = true};
                };
                auto first = co_await h.queue.submit(0, harness::fill("cancel", "one"), work);
                auto second = co_await h.queue.submit(0, harness::fill("cancel", "two"), work);
                for (int n = 0; n < 400 && !started; ++n)
                    co_await execution::schedule_after(io, 5ms);
                expect(started);
                first->leave();
                second->leave();
                for (int n = 0; n < 400 && !exited; ++n)
                    co_await execution::schedule_after(io, 5ms);
                expect(exited && !completed);
                auto next = co_await h.queue.submit(0, harness::entry("barrier", "three"), h.job(io, "barrier"));
                expect(co_await harness::done(io, next));
            };
            h.run(std::move(exercise));
            expect(exited && !completed);
        };

        "dependency publication failures are not cached as completed builds"_test = [] {
            harness h{1};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                static constexpr auto run_operation = [](tools::services&, jobs::progress_log&,
                                                         std::string) -> coroutine::task<std::optional<tools::backlog::outcome>> {
                    tools::config_result result{.configuration = "first", .worker = "w", .ok = true};
                    result.steps.push_back({.name = "install", .exit_code = 0, .cache_error = "publication failed"});
                    co_return result;
                };
                auto first = co_await h.queue.submit(0, harness::entry("publication", "x"), run_operation);
                expect(co_await harness::done(io, first));
                expect(harness::built(first).ok && harness::built(first).steps.front().cache_error == "publication failed");
                auto retry = co_await h.queue.submit(0, harness::entry("publication", "x"), h.job(io, "retry"));
                expect(co_await harness::done(io, retry));
                expect(harness::built(retry).configuration == "retry" && retry->reuse() == "executed");
            };
            h.run(std::move(exercise));
        };
        "export capture failures are not cached as completed builds"_test = [] {
            harness h{1};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                static constexpr auto run_operation = [](tools::services&, jobs::progress_log&,
                                                         std::string) -> coroutine::task<std::optional<tools::backlog::outcome>> {
                    co_return tools::config_result{.configuration = "first", .worker = "w", .ok = true, .export_error = "disk full"};
                };
                auto first = co_await h.queue.submit(0, harness::entry("export", "x"), run_operation);
                expect(co_await harness::done(io, first));
                expect(harness::built(first).ok && harness::built(first).export_error == "disk full");
                auto retry = co_await h.queue.submit(0, harness::entry("export", "x"), h.job(io, "retry"));
                expect(co_await harness::done(io, retry));
                expect(harness::built(retry).configuration == "retry" && retry->reuse() == "executed");
            };
            h.run(std::move(exercise));
        };
        "a fresh build does not join an in-flight equal build"_test = [] {
            harness h{1};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto regular = co_await h.queue.submit(0, harness::entry("same", "x"), h.job(io, "regular"));
                auto entry = harness::entry("same", "y");
                entry.fresh = true;
                auto fresh = co_await h.queue.submit(0, entry, h.job(io, "fresh"));
                expect(regular->job() != fresh->job());
                expect(co_await harness::done(io, regular) && co_await harness::done(io, fresh));
                expect(harness::built(fresh).configuration == "fresh");
            };
            h.run(std::move(exercise));
            expect(h.ran == std::vector<std::string>{"regular", "fresh"});
        };

        "a queued fresh build survives a newer build from the same caller"_test = [] {
            harness h{1};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto busy = co_await h.queue.submit(0, harness::entry("busy", "other", "Other"), h.job(io, "busy", 80ms));
                auto entry = harness::entry("fresh", "x");
                entry.fresh = true;
                auto fresh = co_await h.queue.submit(0, entry, h.job(io, "fresh"));
                auto newer = co_await h.queue.submit(0, harness::entry("newer", "x"), h.job(io, "newer"));
                expect(!fresh->done());
                expect(co_await harness::done(io, busy) && co_await harness::done(io, fresh) && co_await harness::done(io, newer));
                expect(harness::built(fresh).ok && harness::built(newer).ok);
            };
            h.run(std::move(exercise));
            expect(h.ran == std::vector<std::string>{"busy", "fresh", "newer"});
        };

        "a job no worker can take now waits for one"_test = [] {
            harness h{4};
            int tries = 0;
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto run_operation = [&tries](tools::services&, jobs::progress_log&, std::string) -> coroutine::task<std::optional<tools::backlog::outcome>> {
                    if (++tries == 1)
                        co_return std::nullopt;
                    co_return tools::config_result{.configuration = "busy", .worker = "w", .ok = true};
                };
                auto held = co_await h.queue.submit(0, harness::entry("busy", "x"), std::move(run_operation));
                expect(co_await harness::done(io, held) && harness::built(held).ok);
            };
            h.run(std::move(exercise));
            expect(tries == 2);
        };

        "askers take turns, and a newer request replaces the same asker's queued one"_test = [] {
            harness h{1};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto x1 = co_await h.queue.submit(0, harness::entry("x1", "x", "one"), h.job(io, "x1"));
                auto x2 = co_await h.queue.submit(0, harness::entry("x2", "x", "two"), h.job(io, "x2"));
                auto x3 = co_await h.queue.submit(0, harness::entry("x3", "x", "three"), h.job(io, "x3"));
                auto y1 = co_await h.queue.submit(0, harness::entry("y1", "y", "one"), h.job(io, "y1"));
                auto x3b = co_await h.queue.submit(0, harness::entry("x3b", "x", "three"), h.job(io, "x3b"));
                expect(x3->done() && harness::built(x3).error.contains("replaced")) << harness::built(x3).error;
                for (auto& t : {x1, x2, y1, x3b})
                    expect(co_await harness::done(io, t));
            };
            h.run(std::move(exercise));
            expect(h.ran == std::vector<std::string>{"x1", "y1", "x2", "x3b"}) << "x asked first, then y gets a turn";
        };

        "queued requests from different worldlines do not replace each other"_test = [] {
            harness h{1};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto blocker = co_await h.queue.submit(0, harness::entry("block", "other"), h.job(io, "block", 100ms));
                auto first = harness::entry("first", "same");
                first.worldline = "main";
                auto second = harness::entry("second", "same");
                second.worldline = "branch";
                auto a = co_await h.queue.submit(0, first, h.job(io, "first"));
                auto b = co_await h.queue.submit(0, second, h.job(io, "second"));
                for (auto& ticket : {blocker, a, b})
                    expect(co_await harness::done(io, ticket) && harness::built(ticket).ok);
            };
            h.run(std::move(exercise));
            expect(h.ran == std::vector<std::string>{"block", "first", "second"});
        };

        "concurrent task runs sharing a caller and worldline keep both queued builds"_test = [] {
            harness h{1};
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto blocker = co_await h.queue.submit(0, harness::entry("block", "other"), h.job(io, "block", 100ms));
                worlds::actor caller{.issuer = "token", .user = "same", .caller = "agent", .role = "member", .run = "first"};
                auto first = harness::entry("first", worlds::execution_key(caller));
                caller.run = "second";
                auto second = harness::entry("second", worlds::execution_key(caller));
                first.worldline = second.worldline = "main";
                auto a = co_await h.queue.submit(0, first, h.job(io, "first"));
                auto b = co_await h.queue.submit(0, second, h.job(io, "second"));
                for (auto& ticket : {blocker, a, b})
                    expect(co_await harness::done(io, ticket) && harness::built(ticket).ok);
            };
            h.run(std::move(exercise));
            expect((h.ran.size() == 3u) >> fatal);
            expect(h.ran.front() == "block");
            std::ranges::sort(h.ran);
            expect(h.ran == std::vector<std::string>{"block", "first", "second"});
        };

        "queued effect and language calls from the same actor all execute"_test = [] {
            for (const auto& operation : {"run", "test", "prepare", "dependencies"}) {
                harness h{1};
                auto exercise = [&](ctx& io) -> coroutine::task<> {
                    auto blocker = co_await h.queue.submit(0, harness::entry("block", "other"), h.job(io, "block", 100ms));
                    auto first = harness::entry("run/1", "same");
                    first.cacheable = std::string_view{operation} == "prepare";
                    first.operation = operation;
                    auto second = first;
                    second.key = "run/2";
                    auto a = co_await h.queue.submit(0, first, h.job(io, "first"));
                    auto b = co_await h.queue.submit(0, second, h.job(io, "second"));
                    for (auto& ticket : {blocker, a, b})
                        expect(co_await harness::done(io, ticket) && harness::built(ticket).ok);
                };
                h.run(std::move(exercise));
                expect(h.ran == std::vector<std::string>{"block", "first", "second"});
            }
        };

        "pushed diagnostics are not cached even after a matching version arrives"_test = [] {
            harness h{1};
            int runs = 0;
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto what = harness::entry("push", "same");
                what.operation = "prepare";
                auto work = [&runs](tools::services&, jobs::progress_log&, std::string) -> coroutine::task<std::optional<tools::backlog::outcome>> {
                    ++runs;
                    language::result out{.status = "prepared"};
                    out.analysis = "analyzed";
                    out.worker = "worker";
                    out.outline = semantic::outline{.complete = true, .cacheable = false};
                    co_return out;
                };
                auto first = co_await h.queue.submit(0, what, work);
                expect(co_await harness::done(io, first));
                auto second = co_await h.queue.submit(0, what, work);
                expect(co_await harness::done(io, second));
                expect(second->reuse() == "executed");
            };
            h.run(std::move(exercise));
            expect(runs == 2);
        };

        "a job nobody waits for is dropped, and only its asker or an owner cancels it"_test = [] {
            harness h{1};
            const worlds::actor x{.issuer = "token", .user = "x", .caller = "x", .role = "member"};
            const worlds::actor y{.issuer = "token", .user = "y", .caller = "y", .role = "member"};
            const worlds::actor owner{.issuer = "token", .user = "o", .caller = "o", .role = "owner"};
            auto ex = worlds::identity_key(x), why = worlds::identity_key(y);
            auto exercise = [&](ctx& io) -> coroutine::task<> {
                auto running = co_await h.queue.submit(0, harness::entry("long", ex), h.job(io, "long", 200ms));
                auto left = co_await h.queue.submit(0, harness::entry("left", ex, "Release"), h.job(io, "left"));
                left->leave();
                auto own = co_await h.queue.submit(0, harness::entry("own", ex, "Own"), h.job(io, "own"));
                auto shared = co_await h.queue.submit(0, harness::entry("shared", ex, "Other"), h.job(io, "shared"));
                auto joined = co_await h.queue.submit(0, harness::entry("shared", why, "Other"), h.job(io, "shared"));
                expect(co_await h.queue.cancel(0, own->job(), x) == "cancelled") << "its only asker cancels it";
                bool refused = false;
                try {
                    co_await h.queue.cancel(0, shared->job(), x);
                } catch (const tools::tool_error& e) {
                    refused = e.code == "permission_denied";
                }
                expect(refused) << "another caller waits for it too";
                expect(co_await h.queue.cancel(0, shared->job(), owner) == "cancelled");
                expect(shared->done() && joined->done() && harness::built(joined).error == "cancelled");
                expect(co_await harness::done(io, running));
                auto listed = co_await h.queue.list(0);
                expect(std::ranges::any_of(listed, [](auto& v) { return v.state == "cancelled" && v.configuration == "Release"; })) << "the left job";
            };
            h.run(std::move(exercise));
            expect(h.ran == std::vector<std::string>{"long"}) << "no queued job that was left or cancelled ran";
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"backlog">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
