import std;
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
            return [this, name, takes](tools::services&, jobs::progress_log& progress, std::string tree) -> coroutine::task<std::optional<tools::backlog::outcome>> {
                ran.push_back(name);
                trees.push_back(std::move(tree));
                most = std::max(most, ++running);
                progress.push({.stage = name});
                co_await execution::schedule_after(rt.get_scheduler(0), takes);
                --running;
                co_return tools::config_result{.configuration = name, .worker = "w", .ok = true};
            };
        }

        static auto entry(std::string key, std::string actor, std::string configuration = "Debug") -> tools::backlog::entry {
            return {.key = std::move(key), .project = "app", .configuration = std::move(configuration), .operation = "build", .actor = std::move(actor),
                    .cacheable = true};
        }

        static auto built(const std::shared_ptr<tools::backlog::ticket>& t) -> const tools::config_result& { return std::get<tools::config_result>(t->result()); }

        static auto done(ctx& io, const std::shared_ptr<tools::backlog::ticket>& t) -> coroutine::task<bool> {
            for (int i = 0; i < 400 && !t->done(); ++i)
                co_await execution::schedule_after(io, 5ms);
            co_return t->done();
        }

        // The queue serves on core zero; `body` runs on the last core, and the run ends when it does.
        template <class Body> void run(Body body) {
            std::atomic<bool> over{false};
            rt.run([&](ctx& io, std::size_t core) -> coroutine::task<> {
                co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                    if (core == 0)
                        children.spawn(queue.serve(io));
                    if (core + 1 == rt.size()) {
                        co_await body(io);
                        over = true;
                    }
                    while (!over)
                        co_await execution::schedule_after(io, 5ms);
                });
            });
        }
    };

    static constexpr auto cases = [] {
        "a caller on another core queues, follows, lists and cancels jobs"_test = [] {
            harness h{1, 2};
            const worlds::actor x{.issuer = "token", .user = "x", .caller = "x", .role = "member"};
            auto ex = worlds::identity_key(x);
            std::vector<std::string> stages;
            h.run([&](ctx& io) -> coroutine::task<> {
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
            });
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
            h.run([&](ctx& io) -> coroutine::task<> {
                auto a = co_await h.queue.submit(0, job("a", {"f1", "f2"}), h.job(io, "a"));
                auto b = co_await h.queue.submit(0, job("b", {"g1", "g2"}), h.job(io, "b"));
                expect(co_await harness::done(io, a) && co_await harness::done(io, b));
                auto c = co_await h.queue.submit(0, job("c", {"f1", "f2", "f3"}), h.job(io, "c"));
                expect(co_await harness::done(io, c));
                auto d = co_await h.queue.submit(0, job("d", {"g1"}), h.job(io, "d"));
                expect(co_await harness::done(io, d));
            });
            expect((h.trees.size() == 4u) >> fatal);
            expect(h.trees[0] != h.trees[1]) << "jobs running at once take different trees";
            expect(h.trees[2] == h.trees[0]) << "c builds on where a left off";
            expect(h.trees[3] == h.trees[1]) << "d builds on where b left off";
        };

        "jobs past the limit wait their turn instead of failing"_test = [] {
            harness h{1};
            h.run([&](ctx& io) -> coroutine::task<> {
                auto a = co_await h.queue.submit(0, harness::entry("a", "x", "A"), h.job(io, "a"));
                auto b = co_await h.queue.submit(0, harness::entry("b", "y", "B"), h.job(io, "b"));
                expect(b->ahead() == 0u && !b->done()) << "b waits while a runs";
                expect(co_await harness::done(io, a) && co_await harness::done(io, b));
                expect(harness::built(b).ok && harness::built(b).configuration == "b");
            });
            expect(h.most == 1u) << "one at a time";
            expect(h.ran == std::vector<std::string>{"a", "b"});
        };

        "equal jobs run once, and a finished build answers again unless asked fresh"_test = [] {
            harness h{4};
            h.run([&](ctx& io) -> coroutine::task<> {
                auto first = co_await h.queue.submit(0, harness::entry("same", "x"), h.job(io, "first"));
                auto second = co_await h.queue.submit(0, harness::entry("same", "y"), h.job(io, "second"));
                expect(first->job() == second->job()) << "the second asker joins the first job";
                expect(co_await harness::done(io, first) && co_await harness::done(io, second));
                expect(harness::built(first).configuration == "first" && harness::built(second).configuration == "first");
                auto cached = co_await h.queue.submit(0, harness::entry("same", "z"), h.job(io, "third"));
                expect(cached->done() && harness::built(cached).configuration == "first") << "from the cache, at once";
                auto fresh = harness::entry("same", "z");
                fresh.fresh = true;
                auto again = co_await h.queue.submit(0, fresh, h.job(io, "fresh"));
                expect(co_await harness::done(io, again) && harness::built(again).configuration == "fresh");
            });
            expect(h.ran == std::vector<std::string>{"first", "fresh"});
        };

        "a job no worker can take now waits for one"_test = [] {
            harness h{4};
            int tries = 0;
            h.run([&](ctx& io) -> coroutine::task<> {
                auto held = co_await h.queue.submit(0, harness::entry("busy", "x"),
                    [&tries](tools::services&, jobs::progress_log&, std::string) -> coroutine::task<std::optional<tools::backlog::outcome>> {
                        if (++tries == 1)
                            co_return std::nullopt;
                        co_return tools::config_result{.configuration = "busy", .worker = "w", .ok = true};
                    });
                expect(co_await harness::done(io, held) && harness::built(held).ok);
            });
            expect(tries == 2);
        };

        "askers take turns, and a newer request replaces the same asker's queued one"_test = [] {
            harness h{1};
            h.run([&](ctx& io) -> coroutine::task<> {
                auto x1 = co_await h.queue.submit(0, harness::entry("x1", "x", "one"), h.job(io, "x1"));
                auto x2 = co_await h.queue.submit(0, harness::entry("x2", "x", "two"), h.job(io, "x2"));
                auto x3 = co_await h.queue.submit(0, harness::entry("x3", "x", "three"), h.job(io, "x3"));
                auto y1 = co_await h.queue.submit(0, harness::entry("y1", "y", "one"), h.job(io, "y1"));
                auto x3b = co_await h.queue.submit(0, harness::entry("x3b", "x", "three"), h.job(io, "x3b"));
                expect(x3->done() && harness::built(x3).error.contains("replaced")) << harness::built(x3).error;
                for (auto& t : {x1, x2, y1, x3b})
                    expect(co_await harness::done(io, t));
            });
            expect(h.ran == std::vector<std::string>{"x1", "y1", "x2", "x3b"}) << "x asked first, then y gets a turn";
        };

        "a job nobody waits for is dropped, and only its asker or an owner cancels it"_test = [] {
            harness h{1};
            const worlds::actor x{.issuer = "token", .user = "x", .caller = "x", .role = "member"};
            const worlds::actor y{.issuer = "token", .user = "y", .caller = "y", .role = "member"};
            const worlds::actor owner{.issuer = "token", .user = "o", .caller = "o", .role = "owner"};
            auto ex = worlds::identity_key(x), why = worlds::identity_key(y);
            h.run([&](ctx& io) -> coroutine::task<> {
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
            });
            expect(h.ran == std::vector<std::string>{"long"}) << "no queued job that was left or cancelled ran";
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"backlog">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
