module;
#include <cstdio>

export module async;
import std;
export import httpant.io;
export import utility;

export namespace execution {
    using stdexec::completion_signatures;
    using stdexec::completion_signatures_of_t;
    using stdexec::connect_result_t;
    using stdexec::env_of_t;
    using stdexec::error_types_of_t;
    using stdexec::operation_state_t;
    using stdexec::receiver;
    using stdexec::receiver_t;
    using stdexec::scheduler;
    using stdexec::scheduler_t;
    using stdexec::sender;
    using stdexec::sender_t;
    using stdexec::set_error_t;
    using stdexec::set_stopped_t;
    using stdexec::set_value_t;
    using stdexec::value_types_of_t;

    using stdexec::as_awaitable;
    using stdexec::connect;
    using stdexec::env;
    using stdexec::forward_progress_guarantee;
    using stdexec::forwarding_query;
    using stdexec::forwarding_query_t;
    using stdexec::get_allocator;
    using stdexec::get_allocator_t;
    using stdexec::get_completion_scheduler;
    using stdexec::get_completion_signatures;
    using stdexec::get_env;
    using stdexec::get_forward_progress_guarantee;
    using stdexec::get_forward_progress_guarantee_t;
    using stdexec::get_start_scheduler;
    using stdexec::get_stop_token;
    using stdexec::get_stop_token_t;
    using stdexec::into_variant;
    using stdexec::prop;
    using stdexec::read_env;
    using stdexec::sender_adaptor_closure;
    using stdexec::set_error;
    using stdexec::set_stopped;
    using stdexec::set_value;
    using stdexec::start;
    using stdexec::write_env;

    using stdexec::continues_on;
    using stdexec::just;
    using stdexec::just_error;
    using stdexec::just_stopped;
    using stdexec::let_value;
    using stdexec::on;
    using stdexec::schedule;
    using stdexec::starts_on;
    using stdexec::then;
    using stdexec::upon_error;
    using stdexec::upon_stopped;
    using stdexec::when_all;

    using experimental::execution::decay_arguments;
    using experimental::execution::ignore_completion;
    using experimental::execution::materialize;
    using experimental::execution::now;
    using experimental::execution::schedule_after;
    using experimental::execution::schedule_at;
    using experimental::execution::timed_scheduler;
    using experimental::execution::transform_completion_signatures;
    using stdexec::associate;
    using stdexec::counting_scope;
    using stdexec::inline_scheduler;
    using stdexec::run_loop;
    using stdexec::simple_counting_scope;
    using stdexec::spawn;

    using stdexec::sync_wait;

    using stdexec::inplace_stop_source;
    using stdexec::inplace_stop_token;
    using stdexec::never_stop_token;
    using stdexec::stop_callback_for_t;
    using stdexec::stop_token_of_t;
    using stdexec::unstoppable_token;

    template <class S, class... T>
    concept sender_of = stdexec::sender_of<S, stdexec::set_value_t(T...)>;

    class thread_pool {
        struct task {
            task* next;
            void (*run)(task*) noexcept;
        };

        void enqueue(task* work) noexcept {
            {
                std::lock_guard held{mutex_};
                if (tail_)
                    tail_->next = work;
                else
                    head_ = work;
                tail_ = work;
            }
            ready_.notify_one();
        }

        void run() noexcept {
            for (;;) {
                std::unique_lock held{mutex_};
                ready_.wait(held, [this] { return head_ || stopping_; });
                if (!head_)
                    return;
                auto* work = head_;
                head_ = work->next;
                if (!head_)
                    tail_ = nullptr;
                held.unlock();
                work->run(work);
            }
        }

    public:
        explicit thread_pool(std::size_t count) {
            try {
                for (std::size_t i = 0; i < count; ++i)
                    workers_.emplace_back([this] { run(); });
            } catch (...) {
                stop();
                throw;
            }
        }
        ~thread_pool() { stop(); }

        struct scheduler {
            using scheduler_concept = scheduler_t;
            thread_pool* pool;
            auto operator==(const scheduler&) const -> bool = default;

            struct sender {
                using sender_concept = sender_t;
                using completion_signatures = execution::completion_signatures<set_value_t(), set_stopped_t()>;
                thread_pool* pool;

                auto get_env() const noexcept;

                template <class R> struct operation : task {
                    using operation_state_concept = operation_state_t;
                    thread_pool* pool;
                    R receiver;

                    operation(thread_pool* owner, R out)
                        : task{nullptr,
                               [](task* work) noexcept {
                                   auto& self = *static_cast<operation*>(work);
                                   if (get_stop_token(execution::get_env(self.receiver)).stop_requested())
                                       set_stopped(std::move(self.receiver));
                                   else
                                       set_value(std::move(self.receiver));
                               }},
                          pool(owner), receiver(std::move(out)) {}

                    void start() noexcept { pool->enqueue(this); }
                };

                template <class R> auto connect(R receiver) const -> operation<R> { return {pool, std::move(receiver)}; }
            };

            auto schedule() const noexcept -> sender { return {pool}; }
            auto query(get_forward_progress_guarantee_t) const noexcept -> forward_progress_guarantee { return forward_progress_guarantee::parallel; }
        };

        auto get_scheduler() noexcept -> scheduler { return {this}; }
        auto available_parallelism() const noexcept -> std::uint32_t { return static_cast<std::uint32_t>(workers_.size()); }

    private:
        void stop() noexcept {
            {
                std::lock_guard held{mutex_};
                stopping_ = true;
            }
            ready_.notify_all();
            for (auto& worker : workers_)
                worker.join();
        }

        // Blocking tasks remain available to every free worker until dequeued.
        std::mutex mutex_;
        std::condition_variable ready_;
        task* head_ = nullptr;
        task* tail_ = nullptr;
        bool stopping_ = false;
        std::vector<std::thread> workers_;
    };

    inline auto thread_pool::scheduler::sender::get_env() const noexcept {
        return execution::env{prop{get_completion_scheduler<set_value_t>, scheduler{pool}}, prop{get_completion_scheduler<set_stopped_t>, scheduler{pool}}};
    }
} // namespace execution

export namespace logging {
    enum class level { debug, info, warn, error };

    inline std::atomic<level> threshold = level::info;

    inline void write(level l, std::string_view message) {
        if (l < threshold.load(std::memory_order_relaxed))
            return;
        static constexpr std::array<std::string_view, 4> names = {"DEBUG", "INFO", "WARN", "ERROR"};
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto line = std::format("{:%FT%TZ} {} {}\n", now, names[static_cast<int>(l)], message);
        std::fwrite(line.data(), 1, line.size(), stderr);
    }

    inline void info(std::string_view message) { write(level::info, message); }
    inline void warn(std::string_view message) { write(level::warn, message); }
    inline void error(std::string_view message) { write(level::error, message); }
} // namespace logging

export namespace memory {
    using frames = http::memory::frames;

    template <class T> struct frame_allocator {
        using value_type = T;
        frame_allocator() = default;
        template <class U> constexpr frame_allocator(const frame_allocator<U>&) noexcept {}

        auto allocate(std::size_t n) -> T* {
            if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
                throw std::bad_array_new_length{};
            if constexpr (alignof(T) > alignof(std::max_align_t))
                return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
            else
                return static_cast<T*>(frames::allocate(n * sizeof(T)));
        }
        void deallocate(T* p, std::size_t n) noexcept {
            if constexpr (alignof(T) > alignof(std::max_align_t))
                ::operator delete(p, std::align_val_t{alignof(T)});
            else
                frames::deallocate(p, n * sizeof(T));
        }

        template <class U> friend constexpr bool operator==(frame_allocator, frame_allocator<U>) noexcept { return true; }
    };

    class slab {
    public:
        slab(std::size_t block, std::size_t count) : block_(block), region_(std::make_unique_for_overwrite<std::byte[]>(block * count)) {
            free_.reserve(count);
            for (std::size_t i = count; i-- > 0;)
                free_.push_back(region_.get() + i * block_);
        }

        auto take() -> std::span<std::byte> {
            if (free_.empty())
                throw std::bad_alloc{};
            auto* p = free_.back();
            free_.pop_back();
            return {p, block_};
        }
        void give(std::span<std::byte> b) noexcept { free_.push_back(b.data()); }

    private:
        std::size_t block_;
        std::unique_ptr<std::byte[]> region_;
        std::vector<std::byte*> free_;
    };

    class lease {
    public:
        explicit lease(slab& s) : home_(&s), bytes_(s.take()) {}
        lease(lease&& o) noexcept : home_(std::exchange(o.home_, nullptr)), bytes_(o.bytes_) {}
        lease(const lease&) = delete;
        ~lease() {
            if (home_)
                home_->give(bytes_);
        }

        auto bytes() const noexcept -> std::span<std::byte> { return bytes_; }

    private:
        slab* home_;
        std::span<std::byte> bytes_;
    };

    class arena {
    public:
        explicit arena(std::size_t capacity) : buf_(std::make_unique_for_overwrite<std::byte[]>(capacity)), cap_(capacity) {}

        auto allocate(std::size_t n, std::size_t align = alignof(std::max_align_t)) -> void* {
            auto at = (used_ + align - 1) & ~(align - 1);
            if (at + n > cap_)
                throw std::bad_alloc{};
            used_ = at + n;
            return buf_.get() + at;
        }
        void reset() noexcept { used_ = 0; }

    private:
        std::unique_ptr<std::byte[]> buf_;
        std::size_t cap_;
        std::size_t used_ = 0;
    };
} // namespace memory

export namespace net = http::io::net;
export namespace io {
    inline auto parse_endpoint(std::string_view text, std::optional<std::uint16_t> port = {}) -> net::ip::tcp::endpoint {
        auto invalid = [&] { return std::invalid_argument(std::format("invalid endpoint '{}'", text)); };
        auto host = text;
        if (!port) {
            auto colon = text.rfind(':');
            if (colon == text.npos)
                throw invalid();
            unsigned value = 0;
            auto part = text.substr(colon + 1);
            auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
            if (error != std::errc{} || end != part.data() + part.size() || value > 65535)
                throw invalid();
            port = static_cast<std::uint16_t>(value);
            host = text.substr(0, colon);
        }
        if (host.starts_with('[') && host.ends_with(']'))
            host = host.substr(1, host.size() - 2);
        std::error_code error;
        auto address = net::ip::make_address(host, error);
        if (error)
            throw invalid();
        return {address, *port};
    }

    struct command {
        std::vector<std::string> argv;
        std::filesystem::path cwd;
        std::optional<std::map<std::string, std::string>> env;
        std::optional<std::uint64_t> max_memory_bytes;
        std::optional<std::uint32_t> max_cpu_seconds;
        // A conversation: this process writes the child's standard input, the child's standard output holds only its
        // replies, and its standard error is discarded.
        bool interactive = false;
        // A file, created or truncated, that receives the child's standard output; its standard error alone is then what this
        // process reads. Not for a conversation.
        std::optional<std::filesystem::path> standard_output;
    };

    // The io layer specializes this for each scheduler that can start processes.
    template <class Scheduler> struct spawner;

    struct spawn_process_t {
        template <execution::scheduler Scheduler>
        auto operator()(const Scheduler& sch, const command& cmd) const -> decltype(spawner<Scheduler>::spawn(sch, cmd)) {
            return spawner<Scheduler>::spawn(sch, cmd);
        }
    };
    inline constexpr spawn_process_t spawn_process{};

    struct async_wait_t {
        template <class Process> auto operator()(Process& process) const noexcept(noexcept(process.wait())) -> decltype(process.wait()) {
            return process.wait();
        }
    };
    inline constexpr async_wait_t async_wait{};

    using http::io::net::async_accept;
    using http::io::net::async_connect;
    using http::io::net::async_read;
    using http::io::net::async_read_at;
    using http::io::net::async_read_some;
    using http::io::net::async_receive;
    using http::io::net::async_receive_from;
    using http::io::net::async_send;
    using http::io::net::async_send_to;
    using http::io::net::async_write;
    using http::io::net::async_write_at;
    using http::io::net::async_write_some;
    using http::io::net::closed;
    using http::io::net::open_file;

    template <class Scheduler> using process_t = decltype(spawn_process(std::declval<Scheduler>(), std::declval<const command&>()));

    template <class S>
    concept process_scheduler = execution::scheduler<S> && requires(S sch, const command& cmd, process_t<S>& p, std::span<std::byte> b) {
        requires std::move_constructible<process_t<S>>;
        { async_read_some(p, b) } -> execution::sender_of<std::size_t>;
        { async_wait(p) } -> execution::sender_of<int>;
        { p.kill() } noexcept -> std::same_as<void>;
    };

    template <class S>
    concept stream = std::movable<S> && requires(S& s, std::span<std::byte> b, std::span<const std::byte> cb) {
        { async_read_some(s, net::buffer(b)) } -> execution::sender_of<std::size_t>;
        { async_write_some(s, net::buffer(cb)) } -> execution::sender_of<std::size_t>;
    };

    template <class C>
    concept context = std::default_initializable<C> && requires(C& c) {
        { c.get_scheduler() } -> execution::scheduler;
        { c.run() } -> std::same_as<void>;
        { c.stop() } noexcept -> std::same_as<void>;
    };
    template <context Context> using scheduler_of_t = decltype(std::declval<Context&>().get_scheduler());
    template <class Scheduler> using socket_t = typename Scheduler::socket_type;
    template <class Scheduler> using listener_t = typename Scheduler::acceptor_type;
    template <class S>
    concept network_scheduler = execution::scheduler<S> && requires(S sch, socket_t<S>& sock, listener_t<S>& l) {
        requires stream<socket_t<S>>;
        requires std::movable<listener_t<S>>;
        { async_connect(sock, net::ip::tcp::endpoint{}) } -> execution::sender_of<>;
        { async_accept(l) } -> execution::sender_of<socket_t<S>, net::ip::tcp::endpoint>;
        { sock.get_scheduler() } -> std::same_as<S>;
        { l.get_scheduler() } -> std::same_as<S>;
        { sock.close() } noexcept -> std::same_as<void>;
        { l.close() } noexcept -> std::same_as<void>;
        { l.local_endpoint().port() } -> std::same_as<std::uint16_t>;
    };

    template <network_scheduler Scheduler> auto connect(Scheduler scheduler, net::ip::tcp::endpoint to) {
        return execution::let_value(execution::just(), [scheduler, to] {
            return execution::let_value(execution::just(socket_t<Scheduler>{scheduler, to.protocol()}), [to](auto& socket) {
                socket.set_option(net::ip::tcp::no_delay{true});
                socket.set_option(net::socket_base::keep_alive{true});
                return execution::then(async_connect(socket, to), [&socket] { return std::move(socket); });
            });
        });
    }

    template <class S> using file_t = decltype(open_file(std::declval<S>(), std::declval<const std::filesystem::path&>()));
    template <class S>
    concept file_scheduler = execution::scheduler<S> && requires(S sch, file_t<S>& file, std::span<std::byte> b, std::span<const std::byte> cb) {
        requires std::movable<file_t<S>>;
        { async_read_at(file, b, std::uint64_t{}) } -> execution::sender_of<std::size_t>;
        { async_write_at(file, cb, std::uint64_t{}) } -> execution::sender_of<std::size_t>;
        { file.close() } noexcept -> std::same_as<void>;
    };

    template <class Value> struct blocking_value {
        using stored = std::decay_t<Value>;
        using signature = execution::set_value_t(stored);
    };
    template <> struct blocking_value<void> {
        using stored = std::monostate;
        using signature = execution::set_value_t();
    };

    template <execution::scheduler Scheduler, class F> struct blocking_sender {
        using Value = std::invoke_result_t<F&>;
        using sender_concept = execution::sender_t;
        using completion_signatures = execution::completion_signatures<typename blocking_value<Value>::signature, execution::set_error_t(std::error_code),
                                                                       execution::set_error_t(std::exception_ptr), execution::set_stopped_t()>;
        Scheduler scheduler;
        execution::thread_pool* pool;
        F work;
        auto get_env() const noexcept { return execution::prop{execution::get_completion_scheduler<execution::set_value_t>, scheduler}; }

        template <class R> struct operation {
            using operation_state_concept = execution::operation_state_t;
            using environment = execution::prop<execution::get_stop_token_t, execution::never_stop_token>;
            R receiver;
            F work;
            std::optional<typename blocking_value<Value>::stored> value;
            std::variant<std::monostate, std::error_code, std::exception_ptr> error;
            bool stopped = false;

            struct returning {
                using receiver_concept = execution::receiver_t;
                operation* self;
                auto get_env() const noexcept -> environment { return {execution::get_stop_token, execution::never_stop_token{}}; }
                void set_value() noexcept { self->complete(); }
                template <class E> void set_error(E e) noexcept {
                    self->fail(std::move(e));
                    self->complete();
                }
                void set_stopped() noexcept {
                    self->stopped = true;
                    self->complete();
                }
            };
            struct running {
                using receiver_concept = execution::receiver_t;
                operation* self;
                auto get_env() const noexcept -> environment { return {execution::get_stop_token, execution::never_stop_token{}}; }
                void set_value() noexcept { self->run(); }
                template <class E> void set_error(E e) noexcept {
                    self->fail(std::move(e));
                    execution::start(self->back);
                }
                void set_stopped() noexcept {
                    self->stopped = true;
                    execution::start(self->back);
                }
            };
            using back_sender = decltype(execution::schedule(std::declval<Scheduler>()));
            using pool_sender = decltype(execution::schedule(std::declval<execution::thread_pool::scheduler>()));
            execution::connect_result_t<back_sender, returning> back;
            execution::connect_result_t<pool_sender, running> on_pool;

            operation(Scheduler scheduler, execution::thread_pool& pool, F function, R out)
                : receiver(std::move(out)), work(std::move(function)), back(execution::connect(execution::schedule(scheduler), returning{this})),
                  on_pool(execution::connect(execution::schedule(pool.get_scheduler()), running{this})) {}
            operation(operation&&) = delete;
            void start() noexcept { execution::start(on_pool); }
            template <class E> void fail(E e) noexcept {
                if constexpr (std::same_as<E, std::error_code>)
                    error.template emplace<1>(e);
                else if constexpr (std::same_as<E, std::exception_ptr>)
                    error.template emplace<2>(std::move(e));
                else
                    error.template emplace<2>(std::make_exception_ptr(std::move(e)));
            }
            void run() noexcept {
                try {
                    if constexpr (std::is_void_v<Value>) {
                        work();
                        value.emplace();
                    } else
                        value.emplace(work());
                } catch (...) {
                    fail(std::current_exception());
                }
                execution::start(back);
            }
            void complete() noexcept {
                auto out = std::move(receiver);
                if (error.index() == 1)
                    execution::set_error(std::move(out), std::get<1>(error));
                else if (error.index() == 2)
                    execution::set_error(std::move(out), std::move(std::get<2>(error)));
                else if (stopped)
                    execution::set_stopped(std::move(out));
                else if constexpr (std::is_void_v<Value>)
                    execution::set_value(std::move(out));
                else
                    execution::set_value(std::move(out), std::move(*value));
            }
        };
        template <class R> auto connect(R receiver) && -> operation<R> { return {scheduler, *pool, std::move(work), std::move(receiver)}; }
    };

    template <execution::scheduler Scheduler, class F>
        requires std::invocable<F&>
    auto blocking(Scheduler sch, execution::thread_pool& pool, F fn) {
        return blocking_sender<Scheduler, F>{std::move(sch), &pool, std::move(fn)};
    }
} // namespace io

export namespace cores {
    struct core_index {
        std::size_t value;
    };

    namespace detail {
        template <class... Tuples> struct single_values {
            static_assert(sizeof...(Tuples) <= 1, "run requires one value signature; use into_variant for alternatives");
            using type = std::tuple<>;
        };
        template <class Tuple> struct single_values<Tuple> {
            using type = Tuple;
        };
        template <class Tuple> struct unwrap {
            using type = Tuple;
        };
        template <class T> struct unwrap<std::tuple<T>> {
            using type = T;
        };
        template <> struct unwrap<std::tuple<>> {
            using type = void;
        };
        template <class... Values> using decayed_tuple = std::tuple<std::decay_t<Values>...>;
        template <class... Tuples> using value_variant = std::variant<std::monostate, Tuples...>;

        template <class E> auto exception(E&& e) noexcept -> std::exception_ptr {
            try {
                if constexpr (std::same_as<std::decay_t<E>, std::exception_ptr>)
                    return std::forward<E>(e);
                else if constexpr (std::same_as<std::decay_t<E>, std::error_code>)
                    return std::make_exception_ptr(std::system_error(e));
                else
                    return std::make_exception_ptr(std::forward<E>(e));
            } catch (...) {
                return std::current_exception();
            }
        }

        inline void report(std::exception_ptr error, std::string_view where) noexcept {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                logging::error(std::format("{}: {}", where, e.what()));
            } catch (...) {
                logging::error(std::string{where});
            }
        }

        struct control_error {
            template <class E> void operator()(E&& e) const noexcept {
                report(exception(std::forward<E>(e)), "execution resource cannot schedule control work");
                std::terminate();
            }
        };

        template <execution::scheduler Scheduler, class F> struct control_sender {
            using sender_concept = execution::sender_t;
            using completion_signatures = execution::completion_signatures<execution::set_value_t()>;
            Scheduler scheduler;
            F function;
            auto get_env() const noexcept { return execution::prop{execution::get_completion_scheduler<execution::set_value_t>, scheduler}; }
            template <class R> struct operation {
                using operation_state_concept = execution::operation_state_t;
                R receiver;
                F function;
                struct receiver_type {
                    using receiver_concept = execution::receiver_t;
                    operation* self;
                    auto get_env() const noexcept { return execution::prop{execution::get_stop_token, execution::never_stop_token{}}; }
                    void set_value() noexcept { self->complete(); }
                    template <class E> void set_error(E e) noexcept { control_error{}(std::move(e)); }
                    void set_stopped() noexcept { std::terminate(); }
                };
                using sender = decltype(execution::schedule(std::declval<Scheduler>()));
                execution::connect_result_t<sender, receiver_type> scheduled;
                operation(Scheduler scheduler, F action, R out)
                    : receiver(std::move(out)), function(std::move(action)),
                      scheduled(execution::connect(execution::schedule(scheduler), receiver_type{this})) {}
                operation(operation&&) = delete;
                void start() noexcept { execution::start(scheduled); }
                void complete() noexcept {
                    auto action = std::move(function);
                    auto out = std::move(receiver);
                    try {
                        action();
                    } catch (...) {
                        control_error{}(std::current_exception());
                    }
                    execution::set_value(std::move(out));
                }
            };
            template <class R> auto connect(R receiver) && -> operation<R> { return {scheduler, std::move(function), std::move(receiver)}; }
        };

        template <execution::scheduler Scheduler, class Function> class notification {
            struct receiver {
                using receiver_concept = execution::receiver_t;
                Scheduler scheduler;
                Function function;
                auto get_env() const noexcept {
                    return execution::env{execution::prop{execution::get_start_scheduler, scheduler},
                                          execution::prop{execution::get_stop_token, execution::never_stop_token{}},
                                          execution::prop{execution::get_allocator, memory::frame_allocator<std::byte>{}}};
                }
                void set_value() noexcept { function(); }
                template <class E> void set_error(E&& e) noexcept { control_error{}(std::forward<E>(e)); }
                void set_stopped() noexcept { std::terminate(); }
            };
            using sender = decltype(execution::schedule(std::declval<Scheduler>()));
            execution::connect_result_t<sender, receiver> operation_;

        public:
            notification(Scheduler scheduler, Function function)
                : operation_(execution::connect(execution::schedule(scheduler), receiver{scheduler, std::move(function)})) {}
            notification(notification&&) = delete;
            void start() noexcept { execution::start(operation_); }
        };

        template <class Owner, void (Owner::*Function)() noexcept> struct invoke {
            Owner* owner;
            void operator()() const noexcept { (owner->*Function)(); }
        };

        template <execution::scheduler Scheduler, class Make, bool CheckStop> struct call_sender;
        template <class Sender> struct make_sender {
            Sender sender;
            auto operator()() && { return std::move(sender); }
        };

        template <class Context, class Stored, class Token> struct run_state {
            using scheduler_type = io::scheduler_of_t<Context>;
            std::optional<Stored> value;
            std::exception_ptr error;
            Context* context;
            scheduler_type scheduler;
            Token stop;
        };

        template <class Context, class Stored, class Token> struct run_receiver {
            using receiver_concept = execution::receiver_t;
            run_state<Context, Stored, Token>* state;
            auto get_env() const noexcept {
                return execution::env{execution::prop{execution::get_start_scheduler, state->scheduler},
                                      execution::prop{execution::get_stop_token, state->stop},
                                      execution::prop{execution::get_allocator, memory::frame_allocator<std::byte>{}}};
            }
            template <class... Values> void set_value(Values&&... values) noexcept {
                try {
                    state->value.emplace(std::forward<Values>(values)...);
                } catch (...) {
                    state->error = std::current_exception();
                }
                state->context->stop();
            }
            template <class E> void set_error(E&& e) noexcept {
                state->error = exception(std::forward<E>(e));
                state->context->stop();
            }
            void set_stopped() noexcept { state->context->stop(); }
        };
    } // namespace detail

    template <io::context Context, execution::sender Sender, class Token = execution::never_stop_token>
    auto run(Context& context, Sender sender, Token stop = {}) {
        using work_sender = detail::call_sender<io::scheduler_of_t<Context>, detail::make_sender<Sender>, false>;
        using env = decltype(std::declval<detail::run_receiver<Context, std::monostate, Token>>().get_env());
        using tuple = typename execution::value_types_of_t<work_sender, env, std::tuple, detail::single_values>::type;
        using value = typename detail::unwrap<tuple>::type;
        using stored = std::conditional_t<std::is_void_v<value>, std::monostate, value>;
        detail::run_state<Context, stored, Token> state{.context = &context, .scheduler = context.get_scheduler(), .stop = stop};
        auto operation = execution::connect(work_sender{state.scheduler, state.scheduler, detail::make_sender<Sender>{std::move(sender)}},
                                            detail::run_receiver<Context, stored, Token>{&state});
        execution::start(operation);
        context.run();
        if (state.error)
            std::rethrow_exception(state.error);
        if constexpr (std::is_void_v<value>)
            return state.value.has_value();
        else
            return std::move(state.value);
    }

    namespace detail {
        template <class Scheduler>
        using call_environment = decltype(execution::env{execution::prop{execution::get_start_scheduler, std::declval<Scheduler>()},
                                                         execution::prop{execution::get_stop_token, execution::inplace_stop_token{}}});
        template <class Scheduler, class Make>
        using call_work = decltype(execution::write_env(std::declval<std::invoke_result_t<Make>>(), std::declval<call_environment<Scheduler>>()));

        template <execution::scheduler Scheduler, class Make, class R, bool CheckStop> class call_op {
            using work_sender = call_work<Scheduler, Make>;
            using values = execution::value_types_of_t<work_sender, execution::env_of_t<R>, decayed_tuple, value_variant>;
            struct inner_receiver {
                using receiver_concept = execution::receiver_t;
                call_op* self;
                auto get_env() const noexcept -> execution::env_of_t<R> { return execution::get_env(self->receiver_); }
                template <class... Values> void set_value(Values&&... value) noexcept {
                    try {
                        self->result_.template emplace<1>(std::in_place_type<std::tuple<std::decay_t<Values>...>>, std::forward<Values>(value)...);
                    } catch (...) {
                        self->result_.template emplace<2>(std::current_exception());
                    }
                    self->inner_done();
                }
                template <class E> void set_error(E&& e) noexcept {
                    self->result_.template emplace<2>(exception(std::forward<E>(e)));
                    self->inner_done();
                }
                void set_stopped() noexcept {
                    self->result_.template emplace<3>();
                    self->inner_done();
                }
            };
            using inner_op = execution::connect_result_t<work_sender, inner_receiver>;
            struct on_stop {
                call_op* self;
                void operator()() const noexcept { self->forward_stop(); }
            };
            using token = execution::stop_token_of_t<execution::env_of_t<R>>;
            template <void (call_op::*Function)() noexcept> using message = notification<Scheduler, invoke<call_op, Function>>;

        public:
            using operation_state_concept = execution::operation_state_t;
            call_op(Scheduler here, Scheduler there, Make make, R receiver)
                : here_(std::move(here)), there_(std::move(there)), make_(std::move(make)), receiver_(std::move(receiver)), begin_(there_, {this}),
                  stop_(there_, {this}), ack_(here_, {this}), done_(here_, {this}) {}
            call_op(call_op&&) = delete;
            void start() & noexcept {
                auto stop = execution::get_stop_token(execution::get_env(receiver_));
                if constexpr (CheckStop) {
                    if (stop.stop_requested()) {
                        execution::set_stopped(std::move(receiver_));
                        return;
                    }
                }
                stoppable_ = stop.stop_possible();
                if (stoppable_)
                    on_stop_.emplace(std::move(stop), on_stop{this});
                begin_.start();
            }

        private:
            void begin() noexcept {
                try {
                    struct connecting {
                        call_op* self;
                        operator inner_op() {
                            return execution::connect(
                                execution::write_env(std::invoke(std::move(self->make_)),
                                                     call_environment<Scheduler>{
                                                         execution::prop{execution::get_start_scheduler, self->there_},
                                                         execution::prop{execution::get_stop_token,
                                                                         self->stoppable_ ? self->inner_stop_.get_token() : execution::inplace_stop_token{}}}),
                                inner_receiver{self});
                        }
                    };
                    inner_.emplace(connecting{this});
                } catch (...) {
                    result_.template emplace<2>(std::current_exception());
                    inner_done();
                    return;
                }
                execution::start(*inner_);
            }
            void stop_on_b() noexcept {
                if (!finished_on_b_.load(std::memory_order_acquire))
                    inner_stop_.request_stop();
                ack_.start();
            }
            void inner_done() noexcept {
                finished_on_b_.store(true, std::memory_order_release);
                inner_.reset();
                done_.start();
            }
            void forward_stop() noexcept {
                stop_in_flight_.store(true, std::memory_order_release);
                stop_.start();
            }
            void done_on_a() noexcept {
                on_stop_.reset();
                done_arrived_.store(true, std::memory_order_release);
                try_finish();
            }
            void ack_on_a() noexcept {
                stop_in_flight_.store(false, std::memory_order_release);
                try_finish();
            }
            void try_finish() noexcept {
                if (done_arrived_.load(std::memory_order_acquire) && !stop_in_flight_.load(std::memory_order_acquire) &&
                    !completed_.exchange(true, std::memory_order_acq_rel))
                    finish();
            }
            void finish() noexcept {
                switch (result_.index()) {
                case 1:
                    std::visit(
                        [this](auto& tuple) noexcept {
                            if constexpr (!std::same_as<std::decay_t<decltype(tuple)>, std::monostate>)
                                std::apply([this](auto&... value) noexcept { execution::set_value(std::move(receiver_), std::move(value)...); }, tuple);
                        },
                        std::get<1>(result_));
                    break;
                case 2:
                    execution::set_error(std::move(receiver_), std::move(std::get<2>(result_)));
                    break;
                default:
                    execution::set_stopped(std::move(receiver_));
                    break;
                }
            }
            Scheduler here_, there_;
            Make make_;
            R receiver_;
            std::optional<execution::stop_callback_for_t<token, on_stop>> on_stop_;
            bool stoppable_ = false;
            std::atomic<bool> stop_in_flight_{false}, done_arrived_{false}, finished_on_b_{false}, completed_{false};
            execution::inplace_stop_source inner_stop_;
            std::optional<inner_op> inner_;
            std::variant<std::monostate, values, std::exception_ptr, std::monostate> result_;
            message<&call_op::begin> begin_;
            message<&call_op::stop_on_b> stop_;
            message<&call_op::ack_on_a> ack_;
            message<&call_op::done_on_a> done_;
        };

        template <execution::scheduler Scheduler, class Make, bool CheckStop> struct call_sender {
            using sender_concept = execution::sender_t;
            Scheduler here, there;
            Make make;
            template <class Self, class Env> static consteval auto get_completion_signatures() {
                return execution::transform_completion_signatures(
                    execution::completion_signatures_of_t<call_work<Scheduler, Make>, Env>{}, execution::decay_arguments<execution::set_value_t>{},
                    execution::ignore_completion{}, execution::ignore_completion{},
                    execution::completion_signatures<execution::set_error_t(std::exception_ptr), execution::set_stopped_t()>{});
            }
            auto get_env() const noexcept { return execution::prop{execution::get_completion_scheduler<execution::set_value_t>, here}; }
            template <execution::receiver R> auto connect(R receiver) && -> call_op<Scheduler, Make, R, CheckStop> {
                return {here, there, std::move(make), std::move(receiver)};
            }
        };
    } // namespace detail

    template <execution::scheduler Scheduler, class Make>
        requires execution::sender<std::invoke_result_t<Make>>
    auto call(Scheduler here, Scheduler there, Make make) {
        return detail::call_sender<Scheduler, Make, true>{std::move(here), std::move(there), std::move(make)};
    }

    template <execution::scheduler Scheduler> class context {
    public:
        using scheduler_type = Scheduler;
        auto size() const noexcept -> std::size_t { return n_; }
        auto get_scheduler(std::size_t core) noexcept -> Scheduler& {
            auto& slot = schedulers_[core];
            if (!slot)
                std::terminate(); // core out of range or used before publish(): never deref an empty optional
            return *slot;
        }

        void stop() {
            std::lock_guard lock(lifecycle_);
            if (finishing_ || stopping_)
                return;
            stopping_ = true;
            for (std::size_t core = 0; core < n_; ++core) {
                if (active_)
                    post(core, [this, core]() noexcept { stops_[core].root.request_stop(); });
                else
                    stops_[core].root.request_stop();
            }
        }

        // Runs `f` on `core` later, from any core; a throw from it ends the process. Shutdown waits for it.
        template <std::invocable F> void post(std::size_t core, F f) {
            execution::spawn(detail::control_sender<Scheduler, F>{get_scheduler(core), std::move(f)}, operations_->get_token(),
                             execution::env{execution::prop{execution::get_start_scheduler, get_scheduler(core)},
                                            execution::prop{execution::get_stop_token, execution::never_stop_token{}},
                                            execution::prop{execution::get_allocator, memory::frame_allocator<std::byte>{}}});
        }

        template <execution::sender Sender> void spawn(std::size_t core, Sender sender) {
            auto handled = execution::upon_error(
                std::move(sender), [](auto&& e) noexcept { detail::report(detail::exception(std::forward<decltype(e)>(e)), "a background task failed"); });
            execution::spawn(std::move(handled), operations_->get_token(),
                             execution::env{execution::prop{execution::get_start_scheduler, get_scheduler(core)},
                                            execution::prop{execution::get_stop_token, execution::never_stop_token{}},
                                            execution::prop{execution::get_allocator, memory::frame_allocator<std::byte>{}}});
        }

    protected:
        context(std::size_t n, void* owner, void (*stop_all)(void*) noexcept)
            : n_(n), owner_(owner), stop_all_(stop_all), schedulers_(n), stops_(std::make_unique<stop_state[]>(n)) {}
        context(const context&) = delete;
        void prepare() {
            std::lock_guard lock(lifecycle_);
            join_.reset();
            operations_.emplace();
            active_ = false;
            finishing_ = false;
            failure_ = {};
            roots_.store(n_);
        }
        void publish(std::size_t core, Scheduler scheduler) {
            std::lock_guard lock(lifecycle_);
            schedulers_[core].emplace(std::move(scheduler));
        }
        void activate() {
            std::lock_guard lock(lifecycle_);
            join_ = std::make_unique<join_state>(*this);
            active_ = true;
        }
        void release_join() noexcept { join_.reset(); }
        void rethrow_failure() {
            if (failure_)
                std::rethrow_exception(failure_);
        }
        void unpublish(std::size_t core) {
            std::lock_guard lock(lifecycle_);
            schedulers_[core].reset();
        }
        auto root_environment(std::size_t core) const noexcept {
            return execution::env{execution::prop{execution::get_start_scheduler, *schedulers_[core]},
                                  execution::prop{execution::get_stop_token, stops_[core].root.get_token()},
                                  execution::prop{execution::get_allocator, memory::frame_allocator<std::byte>{}}};
        }
        struct root_result {
            context* owner;
            std::size_t core;
            template <class... Values> void operator()(execution::set_value_t, Values&&...) const noexcept { owner->root_done(); }
            template <class E> void operator()(execution::set_error_t, E&& e) const noexcept {
                auto error = detail::exception(std::forward<E>(e));
                {
                    std::lock_guard lock(owner->lifecycle_);
                    if (!owner->failure_)
                        owner->failure_ = error;
                }
                detail::report(error, std::format("core {} failed", core));
                owner->stop();
                owner->root_done();
            }
            void operator()(execution::set_stopped_t) const noexcept { owner->root_done(); }
        };
        auto token() noexcept { return operations_->get_token(); }

    private:
        void root_done() noexcept {
            if (roots_.fetch_sub(1) != 1)
                return;
            std::lock_guard lock(lifecycle_);
            finishing_ = true;
            active_ = false;
            join_->start();
        }
        struct join_receiver {
            using receiver_concept = execution::receiver_t;
            using environment = decltype(execution::env{execution::prop{execution::get_start_scheduler, std::declval<Scheduler>()},
                                                        execution::prop{execution::get_stop_token, execution::never_stop_token{}},
                                                        execution::prop{execution::get_allocator, memory::frame_allocator<std::byte>{}}});
            context* owner;
            Scheduler scheduler;
            auto get_env() const noexcept -> environment {
                return execution::env{execution::prop{execution::get_start_scheduler, scheduler},
                                      execution::prop{execution::get_stop_token, execution::never_stop_token{}},
                                      execution::prop{execution::get_allocator, memory::frame_allocator<std::byte>{}}};
            }
            void set_value() noexcept { owner->stop_all_(owner->owner_); }
            template <class E> void set_error(E&& e) noexcept { detail::control_error{}(std::forward<E>(e)); }
            void set_stopped() noexcept { std::terminate(); }
        };
        struct join_handle {
            virtual ~join_handle() = default;
            virtual void start() noexcept = 0;
        };
        struct join_state final : join_handle {
            using sender = decltype(std::declval<execution::simple_counting_scope&>().join());
            execution::connect_result_t<sender, join_receiver> operation;
            explicit join_state(context& owner) : operation(execution::connect(owner.operations_->join(), join_receiver{&owner, owner.get_scheduler(0)})) {}
            void start() noexcept override { execution::start(operation); }
        };
        struct stop_state {
            execution::inplace_stop_source root;
        };
        std::size_t n_;
        void* owner_;
        void (*stop_all_)(void*) noexcept;
        std::vector<std::optional<Scheduler>> schedulers_;
        std::unique_ptr<stop_state[]> stops_;
        std::optional<execution::simple_counting_scope> operations_;
        std::unique_ptr<join_handle> join_;
        std::atomic<std::size_t> roots_{0};
        bool stopping_ = false, finishing_ = false, active_ = false;
        std::exception_ptr failure_;
        std::mutex lifecycle_;
    };

    template <io::context Resource>
        requires std::constructible_from<Resource, core_index>
    class io_context : public context<io::scheduler_of_t<Resource>> {
        using base = context<io::scheduler_of_t<Resource>>;
        template <class Body> struct make_root {
            Body* body;
            typename base::scheduler_type* scheduler;
            std::size_t core;
            auto operator()() const { return (*body)(*scheduler, core); }
        };

    public:
        using typename base::scheduler_type;
        // A context always owns at least one core: callers take `size()` as a
        // divisor (owner selection) and index their per-core arrays by it, and
        // hardware_concurrency() may legitimately report 0.
        explicit io_context(std::size_t n = std::thread::hardware_concurrency())
            : base(std::max<std::size_t>(n, 1), this, &stop_all), resources_(std::max<std::size_t>(n, 1), nullptr) {}

        template <class Body>
            requires execution::sender<std::invoke_result_t<Body&, scheduler_type&, std::size_t>>
        void run(Body body) {
            this->prepare();
            std::latch ready(static_cast<std::ptrdiff_t>(this->size()));
            std::latch activated(static_cast<std::ptrdiff_t>(this->size()));
            std::latch finished(static_cast<std::ptrdiff_t>(this->size()));
            std::exception_ptr startup_error;
            std::mutex errors;
            auto remember = [&] {
                std::lock_guard lock(errors);
                if (!startup_error)
                    startup_error = std::current_exception();
            };
            {
                std::vector<std::jthread> threads;
                try {
                    for (std::size_t core = 0; core < this->size(); ++core) {
                        threads.emplace_back([&, core] {
                            std::optional<Resource> resource;
                            try {
                                resource.emplace(core_index{core});
                                resources_[core] = &*resource;
                                this->publish(core, resource->get_scheduler());
                            } catch (...) {
                                remember();
                            }
                            ready.arrive_and_wait();
                            if (core == 0 && !startup_error) {
                                try {
                                    this->activate();
                                } catch (...) {
                                    remember();
                                }
                            }
                            activated.arrive_and_wait();
                            if (startup_error) {
                                this->unpublish(core);
                                resources_[core] = nullptr;
                                return;
                            }
                            {
                                auto& scheduler = this->get_scheduler(core);
                                auto work = execution::let_value(execution::just(), make_root<Body>{&body, &scheduler, core});
                                execution::spawn(execution::then(execution::materialize(std::move(work)), typename base::root_result{this, core}),
                                                 this->token(), this->root_environment(core));
                                resource->run();
                            }
                            if (core == 0)
                                this->release_join();
                            finished.arrive_and_wait();
                            this->unpublish(core);
                            resources_[core] = nullptr;
                        });
                    }
                } catch (...) {
                    remember();
                    auto missing = static_cast<std::ptrdiff_t>(this->size() - threads.size());
                    ready.count_down(missing);
                    activated.count_down(missing);
                }
            }
            if (startup_error)
                std::rethrow_exception(startup_error);
            this->rethrow_failure();
        }

    private:
        static void stop_all(void* owner) noexcept {
            for (auto* resource : static_cast<io_context*>(owner)->resources_)
                resource->stop();
        }
        std::vector<Resource*> resources_;
    };

} // namespace cores
