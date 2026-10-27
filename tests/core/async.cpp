import std;
import boost.ut;
import async;
import native;
import support;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;

    struct loop_resource {
        execution::run_loop loop;
        loop_resource() = default;
        explicit loop_resource(cores::core_index) noexcept {}
        auto get_scheduler() noexcept { return loop.get_scheduler(); }
        void run() { loop.run(); }
        void stop() noexcept { loop.finish(); }
    };
    static_assert(io::context<loop_resource>);
    static_assert(!io::file_scheduler<decltype(std::declval<loop_resource&>().get_scheduler())>);

    struct failed_resource {
        static inline std::atomic<int> attempts = 0;
        static inline std::atomic<int> alive = 0;
        execution::run_loop loop;
        failed_resource() {
            if (attempts.fetch_add(1) == 0)
                throw std::runtime_error("resource startup failed");
            ++alive;
        }
        explicit failed_resource(cores::core_index) : failed_resource() {}
        ~failed_resource() { --alive; }
        auto get_scheduler() noexcept { return loop.get_scheduler(); }
        void run() { loop.run(); }
        void stop() noexcept { loop.finish(); }
    };

    struct marker_query {
        template <class Env> auto operator()(const Env& env) const noexcept -> decltype(env.query(*this)) { return env.query(*this); }
        static consteval auto query(execution::forwarding_query_t) noexcept -> bool { return true; }
    };

    struct alternative {
        using sender_concept = execution::sender_t;
        using completion_signatures = execution::completion_signatures<execution::set_value_t(int), execution::set_value_t(std::string_view)>;
        bool number;
        template <class R> struct op {
            using operation_state_concept = execution::operation_state_t;
            bool number;
            R receiver;
            void start() & noexcept {
                if (number)
                    execution::set_value(std::move(receiver), 42);
                else
                    execution::set_value(std::move(receiver), std::string_view{"text"});
            }
        };
        template <class R> auto connect(R receiver) const -> op<R> { return {number, std::move(receiver)}; }
    };

    struct hop_receiver {
        using receiver_concept = execution::receiver_t;
        native::context* context;
        execution::inplace_stop_token stop;
        std::thread::id* completed_on;
        int* result;
        auto get_env() const noexcept { return execution::prop{execution::get_stop_token, stop}; }
        void finish(int value) noexcept {
            *completed_on = std::this_thread::get_id();
            *result = value;
            context->stop();
        }
        void set_value() noexcept { finish(1); }
        template <class Error> void set_error(Error&&) noexcept { finish(2); }
        void set_stopped() noexcept { finish(3); }
    };

    struct accept_receiver {
        using receiver_concept = execution::receiver_t;
        native::context* context;
        native::scheduler scheduler;
        execution::inplace_stop_token stop;
        int* result;
        auto get_env() const noexcept {
            return execution::env{execution::prop{execution::get_start_scheduler, scheduler}, execution::prop{execution::get_stop_token, stop},
                                  execution::prop{execution::get_allocator, memory::frame_allocator<std::byte>{}}};
        }
        void set_value(native::socket, net::ip::tcp::endpoint) noexcept {
            *result = 1;
            context->stop();
        }
        template <class Error> void set_error(Error&&) noexcept {
            *result = 2;
            context->stop();
        }
        void set_stopped() noexcept {
            *result = 3;
            context->stop();
        }
    };

    struct aligned_work {
        using sender_concept = execution::sender_t;
        using completion_signatures = execution::completion_signatures<execution::set_value_t()>;
        bool* aligned;
        template <class R> struct alignas(64) op {
            using operation_state_concept = execution::operation_state_t;
            R receiver;
            bool* aligned;
            void start() & noexcept {
                *aligned = reinterpret_cast<std::uintptr_t>(this) % 64 == 0;
                execution::set_value(std::move(receiver));
            }
        };
        template <class R> auto connect(R receiver) const -> op<R> { return {std::move(receiver), aligned}; }
    };

    template <class Scheduler> struct cleanup_on_destroy {
        using sender_concept = execution::sender_t;
        using completion_signatures = execution::completion_signatures<execution::set_stopped_t()>;
        cores::context<Scheduler>* context;
        std::size_t core;
        bool* cleaned;
        template <class R> struct op {
            using operation_state_concept = execution::operation_state_t;
            R receiver;
            cores::context<Scheduler>* context;
            std::size_t core;
            bool* cleaned;
            ~op() {
                context->spawn(core, execution::then(execution::schedule(context->get_scheduler(core)), [flag = cleaned]() noexcept { *flag = true; }));
            }
            void start() & noexcept { execution::set_stopped(std::move(receiver)); }
        };
        template <class R> auto connect(R receiver) const -> op<R> { return {std::move(receiver), context, core, cleaned}; }
    };

    struct allocation_counts {
        std::atomic<std::size_t> allocated{0}, freed{0};
    };
    template <class T> struct recording_allocator {
        using value_type = T;
        allocation_counts* counts;
        template <class U> recording_allocator(const recording_allocator<U>& other) noexcept : counts(other.counts) {}
        explicit recording_allocator(allocation_counts* counts) noexcept : counts(counts) {}
        auto allocate(std::size_t n) -> T* {
            counts->allocated.fetch_add(1);
            return std::allocator<T>{}.allocate(n);
        }
        void deallocate(T* pointer, std::size_t n) noexcept {
            counts->freed.fetch_add(1);
            std::allocator<T>{}.deallocate(pointer, n);
        }
        template <class U> friend auto operator==(recording_allocator a, recording_allocator<U> b) noexcept -> bool { return a.counts == b.counts; }
    };

    static constexpr auto execution_suite = [] {
        "one native scheduler serves both positioned files and network streams"_test = [] {
            static_assert(io::network_scheduler<native::scheduler> && io::file_scheduler<native::scheduler>);
            test::scratch scratch{"native-file-sender"};
            native::context context;
            auto scheduler = context.get_scheduler();
            auto file = io::open_file(scheduler, scratch.dir / "data", true);
            std::array<std::byte, 3> out{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}}, in{};
            expect(cores::run(context, io::async_write_at(file, out, 7)) == 3u);
            expect(cores::run(context, io::async_read_at(file, in, 7)) == 3u);
            expect(std::ranges::equal(in, out));
            expect(cores::run(context, io::async_read_at(file, in, 1000)) == 0u);
            auto after_close = io::async_write_at(file, out, 1000);
            file.close();
            expect(throws<std::system_error>([&] { cores::run(context, std::move(after_close)); }));
            expect(test::slurp(scratch.dir / "data").size() == 10u);
        };
        "per-core execution accepts a native scheduler with no post or context trait"_test = [] {
            cores::io_context<loop_resource> context{2};
            std::array<int, 2> result{};
            context.run([&](auto& scheduler, std::size_t core) {
                return execution::then(cores::call(scheduler, context.get_scheduler(1 - core), [core] { return execution::just(int(core + 10)); }),
                                       [&, core](int value) noexcept { result[core] = value; });
            });
            expect(result == std::array<int, 2>{10, 11});
        };

        "resource startup failure releases peers and reaches the caller"_test = [] {
            failed_resource::attempts = 0;
            failed_resource::alive = 0;
            cores::io_context<failed_resource> context{2};
            std::atomic<int> roots = 0;
            std::string error;
            try {
                context.run([&](auto&, std::size_t) {
                    ++roots;
                    return execution::just();
                });
            } catch (const std::runtime_error& e) {
                error = e.what();
            }
            expect(error == "resource startup failed");
            expect(roots.load() == 0 && failed_resource::alive.load() == 0);
        };

        "root task failures reach the caller after resource threads join"_test = [] {
            cores::io_context<loop_resource> context{2};
            std::string error;
            try {
                context.run([](auto&, std::size_t core) {
                    return execution::then(execution::just(), [core] {
                        if (core == 0)
                            throw std::runtime_error("root task failed");
                    });
                });
            } catch (const std::runtime_error& e) {
                error = e.what();
            }
            expect(error == "root task failed");
        };

        "cross-core work forwards the receiver environment"_test = [] {
            loop_resource context;
            auto scheduler = context.get_scheduler();
            auto work = cores::call(scheduler, scheduler, [] { return execution::read_env(marker_query{}); });
            expect(cores::run(context, execution::write_env(std::move(work), execution::prop{marker_query{}, 73})) == 73);
        };

        "resource runners preserve an uncancellable caller token"_test = [] {
            loop_resource context;
            auto possible =
                cores::run(context, execution::then(execution::read_env(execution::get_stop_token), [](auto token) noexcept { return token.stop_possible(); }));
            expect(possible.has_value() >> fatal);
            expect(!*possible);
        };

        "cross-core work preserves alternative value completions"_test = [] {
            for (bool number : {false, true}) {
                loop_resource context;
                auto scheduler = context.get_scheduler();
                auto result = cores::run(context, execution::into_variant(cores::call(scheduler, scheduler, [number] { return alternative{number}; })));
                expect(result.has_value() >> fatal);
                if (number)
                    expect(std::get<0>(std::get<std::tuple<int>>(*result)) == 42);
                else
                    expect(std::get<0>(std::get<std::tuple<std::string_view>>(*result)) == "text");
            }
        };

        "posted work runs before shutdown completes"_test = [] {
            cores::io_context<loop_resource> context{1};
            int received = 0;
            context.run([&](auto&, std::size_t) {
                for (int i = 0; i < 2000; ++i)
                    context.post(0, [&] { ++received; });
                return execution::just();
            });
            expect(received == 2000);
        };

        "execution shutdown joins owned background work"_test = [] {
            cores::io_context<loop_resource> context{1};
            execution::thread_pool pool{1};
            std::atomic<bool> finished = false;
            context.run([&](auto& scheduler, std::size_t core) {
                auto work = execution::continues_on(execution::starts_on(pool.get_scheduler(), execution::then(execution::just(),
                                                                                                               [&] {
                                                                                                                   std::this_thread::sleep_for(10ms);
                                                                                                                   finished.store(true);
                                                                                                               })),
                                                    scheduler);
                context.spawn(core, execution::write_env(std::move(work), execution::prop{execution::get_stop_token, execution::never_stop_token{}}));
                return execution::just();
            });
            expect(finished.load());
        };

        "root destruction can enqueue cleanup before the scope finishes"_test = [] {
            cores::io_context<loop_resource> context{1};
            bool cleaned = false;
            context.run([&](auto& scheduler, std::size_t core) {
                using scheduler_type = std::decay_t<decltype(scheduler)>;
                return cleanup_on_destroy<scheduler_type>{&context, core, &cleaned};
            });
            expect(cleaned);
        };

        "native scope allocations honor over-aligned operation states"_test = [] {
            cores::io_context<loop_resource> context{1};
            bool aligned = false;
            context.run([&](auto&, std::size_t core) {
                context.spawn(core, aligned_work{&aligned});
                return execution::just();
            });
            expect(aligned);
            memory::frame_allocator<std::uint64_t> allocator;
            expect(throws<std::bad_array_new_length>([&] { allocator.allocate(std::numeric_limits<std::size_t>::max()); }));
        };

        "a stop before resource publication reaches the roots"_test = [] {
            cores::io_context<loop_resource> context{2};
            std::atomic<int> ran = 0;
            context.stop();
            context.run([&](auto& scheduler, std::size_t) { return execution::then(execution::schedule(scheduler), [&]() noexcept { ++ran; }); });
            expect(ran.load() == 0);
        };

        "socket connect and read support sender adaptor composition"_test = [] {
            native::context context;
            auto scheduler = context.get_scheduler();
            auto listener = native::listener{scheduler, {net::ip::address_v4::loopback(), 0}};
            auto client = native::socket{scheduler, net::ip::tcp::v4()};
            allocation_counts counts;
            auto connected = cores::run(
                context,
                execution::when_all(
                    execution::just(net::ip::tcp::endpoint{net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()}) | net::async_connect(client),
                    execution::write_env(net::async_accept(listener), execution::prop{execution::get_allocator, recording_allocator<std::byte>{&counts}})));
            expect(connected.has_value() >> fatal);
            auto peer = std::move(std::get<0>(*connected));
            expect(counts.allocated.load() == counts.freed.load());
            std::array<std::byte, 3> out{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}}, in{};
            auto transferred =
                cores::run(context, execution::when_all(execution::just(net::buffer(std::span<const std::byte>{out})) | io::async_write_some(client),
                                                        execution::just(net::buffer(std::span<std::byte>{in})) | io::async_read_some(peer)));
            expect(transferred.has_value() >> fatal);
            expect(std::get<0>(*transferred) == 3u && std::get<1>(*transferred) == 3u);
            expect(std::ranges::equal(in, out));
            client.close();
            expect(cores::run(context, io::async_read_some(peer, net::buffer(in))) == 0u);
        };

        "an unsolicited close is an error, not stopped"_test = [] {
            native::context context;
            auto scheduler = context.get_scheduler();
            auto listener = native::listener{scheduler, {net::ip::address_v4::loopback(), 0}};
            expect(throws<std::system_error>([&] {
                cores::run(context, execution::when_all(net::async_accept(listener),
                                                        execution::then(execution::schedule(scheduler), [&]() noexcept { listener.close(); })));
            }));
        };

        "a foreign thread can cancel a native I/O sender"_test = [] {
            native::context context;
            auto scheduler = context.get_scheduler();
            auto listener = native::listener{scheduler, {net::ip::address_v4::loopback(), 0}};
            execution::inplace_stop_source source;
            std::latch started{1};
            std::jthread stopper([&] {
                started.wait();
                source.request_stop();
            });
            auto work = execution::let_value(execution::just(), [&] {
                started.count_down();
                return net::async_accept(listener);
            });
            expect(!cores::run(context, std::move(work), source.get_token()));
        };

        "a pre-stopped schedule completes on its execution resource"_test = [] {
            native::context context;
            auto scheduler = context.get_scheduler();
            execution::inplace_stop_source source;
            source.request_stop();
            std::thread::id completed_on;
            int result = 0;
            auto operation = execution::connect(execution::schedule(scheduler), hop_receiver{&context, source.get_token(), &completed_on, &result});
            std::jthread initiator([&] { execution::start(operation); });
            context.run();
            initiator.join();
            expect(result == 3);
            expect(completed_on == std::this_thread::get_id());
        };

        "the native Asio adapter accepts direct foreign cancellation"_test = [] {
            native::context context;
            auto scheduler = context.get_scheduler();
            auto listener = native::listener{scheduler, {net::ip::address_v4::loopback(), 0}};
            execution::inplace_stop_source source;
            int result = 0;
            auto operation = execution::connect(net::async_accept(listener), accept_receiver{&context, scheduler, source.get_token(), &result});
            execution::start(operation);
            std::jthread stopper([&] { source.request_stop(); });
            context.run();
            expect(result == 3);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"execution">{execution_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
