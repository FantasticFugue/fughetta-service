export module coroutine;
import std;
export import async;

export namespace coroutine {
    using stop_token = execution::inplace_stop_token;
    using stop_source = execution::inplace_stop_source;

    template <class T = void> class task;
    class scope;

    struct current_token_t {};
    inline constexpr current_token_t current_token{};

    struct stop_t {};
    inline constexpr stop_t stop{};

    template <class S> struct expect_t {
        S sender;
    };
    template <class S> auto expect(S&& s) -> expect_t<std::remove_cvref_t<S>> { return {std::forward<S>(s)}; }

    template <class A> struct stop_point_t {
        A inner;
    };
    template <class A> auto stop_point(A&& a) -> stop_point_t<std::remove_cvref_t<A>> { return {std::forward<A>(a)}; }

    template <class T> struct under_t {
        stop_token token;
        task<T> inner;
    };
    template <class T> auto under(stop_token token, task<T> t) -> under_t<T> { return {token, std::move(t)}; }

    namespace detail {
        template <class> inline constexpr bool is_task = false;
        template <class T> inline constexpr bool is_task<task<T>> = true;

        template <class T> using stored = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

        struct env;

        struct promise_base {
            static auto operator new(std::size_t n) -> void* { return memory::frames::allocate(n); }
            static void operator delete(void* p, std::size_t n) noexcept { memory::frames::deallocate(p, n); }

            auto get_env() const noexcept -> env;
            auto initial_suspend() noexcept -> std::suspend_always { return {}; }
            void unhandled_exception() noexcept { error_ = util::current_exception(); }

            auto unhandled_stopped() noexcept -> std::coroutine_handle<> { return on_stopped_(stopped_ctx_); }

            std::coroutine_handle<> continuation_{};
            std::coroutine_handle<> (*complete_)(void*, promise_base&) noexcept = nullptr;
            void* complete_ctx_ = nullptr;
            std::coroutine_handle<> (*on_stopped_)(void*) noexcept = nullptr;
            void* stopped_ctx_ = nullptr;
            stop_token stop_{};
            std::exception_ptr error_{};
        };

        struct final_awaiter {
            auto await_ready() const noexcept -> bool { return false; }
            template <class P> auto await_suspend(std::coroutine_handle<P> h) const noexcept -> std::coroutine_handle<> {
                auto& p = h.promise();
                if (p.complete_)
                    return p.complete_(p.complete_ctx_, p);
                return p.continuation_;
            }
            void await_resume() const noexcept {}
        };

        struct stopped_awaiter {
            promise_base& promise;
            auto await_ready() const noexcept -> bool { return false; }
            auto await_suspend(std::coroutine_handle<>) const noexcept -> std::coroutine_handle<> { return promise.unhandled_stopped(); }
            [[noreturn]] void await_resume() const noexcept { std::unreachable(); }
        };

        template <class T> struct result {
            std::optional<T> value_{};
            template <class V = T>
                requires std::constructible_from<T, V>
            void return_value(V&& v) {
                value_.emplace(std::forward<V>(v));
            }
        };
        template <> struct result<void> {
            void return_void() noexcept {}
        };

        inline auto forward_stopped(void* parent) noexcept -> std::coroutine_handle<> { return static_cast<promise_base*>(parent)->unhandled_stopped(); }

        struct env {
            stop_token stop;
            auto query(execution::get_stop_token_t) const noexcept -> stop_token { return stop; }
            auto query(execution::get_allocator_t) const noexcept { return memory::frame_allocator<std::byte>{}; }
        };

        inline auto promise_base::get_env() const noexcept -> env { return {stop_}; }

        template <class... Ts> using decayed_tuple = std::tuple<std::decay_t<Ts>...>;

        template <class Tuple> struct unwrap {
            using type = Tuple;
        };
        template <class V> struct unwrap<std::tuple<V>> {
            using type = V;
        };
        template <> struct unwrap<std::tuple<>> {
            using type = void;
        };

        template <class... Es> struct error_list {
            using variant = std::variant<Es...>;
            static constexpr std::size_t size = sizeof...(Es);
            using expected_error = variant;
        };
        template <class E> struct error_list<E> {
            using variant = std::variant<E>;
            static constexpr std::size_t size = 1;
            using expected_error = E;
        };
        template <> struct error_list<> {
            using variant = std::monostate;
            static constexpr std::size_t size = 0;
            using expected_error = std::error_code;
        };

        template <class... Tuples> struct single_value {
            static_assert(sizeof...(Tuples) <= 1, "an awaited sender completes with at most one value signature");
            using type = std::tuple<>;
        };
        template <class Tuple> struct single_value<Tuple> {
            using type = Tuple;
        };

        enum class mode { plain, expect, stop_point };

        template <class S>
        concept tagged_sender = requires { typename S::sender_concept; };

        template <class A>
        concept awaitable = requires(A a) { a.await_ready(); } || requires(A a) { std::move(a).operator co_await(); };

        template <class Tuple, class ErrorStorage, mode M> struct sender_state {
            promise_base* promise_;
            std::coroutine_handle<> self_{};
            std::variant<std::monostate, Tuple, ErrorStorage, std::monostate> result_{};
            std::exception_ptr boundary_error_{};
            enum class phase { starting, suspended, completed };
            std::atomic<phase> phase_{phase::starting};

            struct receiver {
                using receiver_concept = execution::receiver_t;
                sender_state* self;
                env context;

                auto get_env() const noexcept -> env { return context; }

                template <class... Vs> void set_value(Vs&&... vs) noexcept {
                    try {
                        self->result_.template emplace<1>(std::forward<Vs>(vs)...);
                    } catch (...) {
                        self->boundary_error_ = util::current_exception();
                    }
                    self->resume();
                }
                template <class E> void set_error(E&& e) noexcept {
                    try {
                        self->result_.template emplace<2>(std::in_place_type<std::decay_t<E>>, std::forward<E>(e));
                    } catch (...) {
                        self->boundary_error_ = util::current_exception();
                    }
                    self->resume();
                }
                void set_stopped() noexcept {
                    self->result_.template emplace<3>();
                    self->resume();
                }
            };

            void resume() noexcept {
                auto continuation = self_;
                auto* promise = promise_;
                auto stopped = result_.index() == 3;
                if (phase_.exchange(phase::completed, std::memory_order_acq_rel) == phase::suspended) {
                    if constexpr (M != mode::stop_point) {
                        if (stopped) {
                            promise->unhandled_stopped().resume();
                            return;
                        }
                    }
                    continuation.resume();
                }
            }
        };

        template <class S, mode M> class sender_awaiter {
            using values = execution::value_types_of_t<S, env, decayed_tuple, single_value>;
            using tuple = typename values::type;
            using errors = execution::error_types_of_t<S, env, error_list>;

        public:
            using value_type = typename unwrap<tuple>::type;

        private:
            static constexpr bool has_errors = errors::size != 0;
            using error_storage = std::conditional_t<has_errors, typename errors::variant, std::monostate>;

            using expected_error = typename errors::expected_error;

            using state = sender_state<tuple, error_storage, M>;
            using receiver = typename state::receiver;

            using op_t = execution::connect_result_t<S, receiver>;

        public:
            template <class X> sender_awaiter(X&& s, promise_base& p) : sender_(std::forward<X>(s)), state_{&p} {}
            sender_awaiter(const sender_awaiter&) = delete;
            ~sender_awaiter() {
                if (live_)
                    op().~op_t();
            }

            auto await_ready() const noexcept -> bool { return false; }

            auto await_suspend(std::coroutine_handle<> self) noexcept -> std::coroutine_handle<> {
                state_.self_ = self;
                if (state_.promise_->stop_.stop_requested()) {
                    state_.result_.template emplace<3>();
                } else {
                    try {
                        ::new (static_cast<void*>(storage_)) op_t(execution::connect(std::move(sender_), receiver{&state_, {state_.promise_->stop_}}));
                    } catch (...) {
                        state_.boundary_error_ = util::current_exception();
                        return self;
                    }
                    live_ = true;
                    execution::start(op());
                    if (state_.phase_.exchange(state::phase::suspended, std::memory_order_acq_rel) != state::phase::completed)
                        return std::noop_coroutine();
                }
                if (M != mode::stop_point && state_.result_.index() == 3)
                    return state_.promise_->unhandled_stopped();
                return self;
            }

            auto await_resume() {
                if (state_.boundary_error_)
                    util::rethrow_exception(state_.boundary_error_);
                if constexpr (M == mode::stop_point) {
                    using out = std::conditional_t<std::is_void_v<value_type>, bool, std::optional<value_type>>;
                    if (state_.result_.index() == 3)
                        return out{};
                    if (state_.result_.index() == 2)
                        raise();
                    if constexpr (std::is_void_v<value_type>)
                        return true;
                    else
                        return out{take()};
                } else if constexpr (M == mode::expect) {
                    using out = std::expected<value_type, expected_error>;
                    if (state_.result_.index() == 2) {
                        if constexpr (errors::size == 1)
                            return out{std::unexpect, std::move(std::get<0>(std::get<2>(state_.result_)))};
                        else if constexpr (has_errors)
                            return out{std::unexpect, std::move(std::get<2>(state_.result_))};
                    }
                    if constexpr (std::is_void_v<value_type>)
                        return out{};
                    else
                        return out{take()};
                } else {
                    if (state_.result_.index() == 2)
                        raise();
                    if constexpr (!std::is_void_v<value_type>)
                        return take();
                }
            }

        private:
            auto op() noexcept -> op_t& { return *std::launder(reinterpret_cast<op_t*>(storage_)); }

            auto take() -> value_type {
                auto& t = std::get<1>(state_.result_);
                if constexpr (std::tuple_size_v<tuple> == 1)
                    return std::move(std::get<0>(t));
                else
                    return std::move(t);
            }

            [[noreturn]] void raise() {
                if constexpr (has_errors) {
                    std::visit(
                        []<class E>(E& e) {
                            if constexpr (std::same_as<E, std::exception_ptr>)
                                util::rethrow_exception(e);
                            else if constexpr (std::same_as<E, std::error_code>)
                                throw std::system_error(e);
                            else
                                throw std::move(e);
                        },
                        std::get<2>(state_.result_));
                }
                std::unreachable();
            }

            S sender_;
            state state_;
            bool live_ = false;
            alignas(op_t) unsigned char storage_[sizeof(op_t)];
        };

        static_assert(std::same_as<typename sender_awaiter<decltype(execution::just_stopped()), mode::plain>::value_type, void>);

        template <class T, mode M> class task_awaiter {
        public:
            using handle = std::coroutine_handle<typename task<T>::promise_type>;

            task_awaiter(handle child, promise_base& parent, std::optional<stop_token> token = {}) noexcept : child_(child), parent_(&parent), token_(token) {}
            task_awaiter(const task_awaiter&) = delete;
            ~task_awaiter() {
                if (child_)
                    child_.destroy();
            }

            auto await_ready() const noexcept -> bool { return false; }

            auto await_suspend(std::coroutine_handle<> self) noexcept -> std::coroutine_handle<> {
                if (!child_)
                    return self; // awaiting a moved-from/empty task: resume and let await_resume report
                auto& p = child_.promise();
                p.continuation_ = self;
                p.stop_ = token_ ? *token_ : parent_->stop_;
                if constexpr (M == mode::stop_point) {
                    self_ = self;
                    p.on_stopped_ = &caught;
                    p.stopped_ctx_ = this;
                } else {
                    p.on_stopped_ = &forward_stopped;
                    p.stopped_ctx_ = parent_;
                }
                return child_;
            }

            auto await_resume() {
                if (!child_)
                    throw std::runtime_error("awaiting an empty coroutine task");
                auto& p = child_.promise();
                if constexpr (M == mode::stop_point) {
                    using out = std::conditional_t<std::is_void_v<T>, bool, std::optional<T>>;
                    if (stopped_)
                        return out{};
                    if (p.error_)
                        util::rethrow_exception(p.error_);
                    if constexpr (std::is_void_v<T>)
                        return true;
                    else
                        return out{std::move(*p.value_)};
                } else {
                    if (p.error_)
                        util::rethrow_exception(p.error_);
                    if constexpr (!std::is_void_v<T>)
                        return std::move(*p.value_);
                }
            }

        private:
            static auto caught(void* ctx) noexcept -> std::coroutine_handle<> {
                auto& self = *static_cast<task_awaiter*>(ctx);
                self.stopped_ = true;
                return self.self_;
            }

            handle child_;
            promise_base* parent_;
            std::optional<stop_token> token_;
            std::coroutine_handle<> self_{};
            bool stopped_ = false;
        };

        template <class Token> struct stop_bridge {
            struct forward {
                stop_source* source;
                void operator()() const noexcept { source->request_stop(); }
            };
            stop_source source;
            std::optional<execution::stop_callback_for_t<Token, forward>> callback;

            auto attach(Token outer) -> stop_token {
                callback.emplace(std::move(outer), forward{&source});
                return source.get_token();
            }
            void detach() noexcept { callback.reset(); }
        };
        template <> struct stop_bridge<stop_token> {
            auto attach(stop_token outer) noexcept -> stop_token { return outer; }
            void detach() noexcept {}
        };
        template <execution::unstoppable_token Token> struct stop_bridge<Token> {
            auto attach(Token) noexcept -> stop_token { return {}; }
            void detach() noexcept {}
        };

    } // namespace detail

    template <class T> class [[clang::coro_await_elidable]] task {
    public:
        using value_type = T;

        struct promise_type : detail::promise_base, detail::result<T> {
            auto get_return_object() noexcept -> task { return task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
            auto final_suspend() noexcept -> detail::final_awaiter { return {}; }

            template <class A> decltype(auto) await_transform(A&& a) {
                using D = std::remove_cvref_t<A>;
                if constexpr (detail::is_task<D>) {
                    return detail::task_awaiter<typename D::value_type, detail::mode::plain>{std::exchange(a.h_, {}), *this};
                } else if constexpr (requires { std::forward<A>(a).as_awaitable(*this); }) {
                    return std::forward<A>(a).as_awaitable(*this);
                } else if constexpr (requires { typename D::promise_type; }) {
                    static_assert(sizeof(D) == 0, "a coroutine task must provide promise-aware as_awaitable");
                } else if constexpr (detail::tagged_sender<D>) {
                    return detail::sender_awaiter<D, detail::mode::plain>{std::forward<A>(a), *this};
                } else if constexpr (detail::awaitable<D>) {
                    return std::forward<A>(a);
                } else if constexpr (execution::sender<D>) {
                    return detail::sender_awaiter<D, detail::mode::plain>{std::forward<A>(a), *this};
                } else {
                    return std::forward<A>(a);
                }
            }
            template <class S> auto await_transform(expect_t<S>&& e) { return detail::sender_awaiter<S, detail::mode::expect>{std::move(e.sender), *this}; }
            auto await_transform(stop_t) noexcept -> detail::stopped_awaiter { return {*this}; }
            template <class U> auto await_transform(under_t<U>&& u) {
                return detail::task_awaiter<U, detail::mode::stop_point>{std::exchange(u.inner.h_, {}), *this, u.token};
            }
            template <class A> auto await_transform(stop_point_t<A>&& s) {
                if constexpr (detail::is_task<A>)
                    return detail::task_awaiter<typename A::value_type, detail::mode::stop_point>{std::exchange(s.inner.h_, {}), *this};
                else
                    return detail::sender_awaiter<A, detail::mode::stop_point>{std::move(s.inner), *this};
            }
            struct token_reader {
                stop_token stop;
                auto await_ready() const noexcept -> bool { return true; }
                void await_suspend(std::coroutine_handle<>) const noexcept {}
                auto await_resume() const noexcept -> stop_token { return stop; }
            };
            auto await_transform(const current_token_t&) noexcept -> token_reader { return {stop_}; }
        };

        task(task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
        auto operator=(task&& o) noexcept -> task& {
            if (this != &o) {
                if (h_)
                    h_.destroy();
                h_ = std::exchange(o.h_, {});
            }
            return *this;
        }
        ~task() {
            if (h_)
                h_.destroy();
        }

        using sender_concept = execution::sender_t;
        using completion_signatures =
            execution::completion_signatures<std::conditional_t<std::is_void_v<T>, execution::set_value_t(), execution::set_value_t(detail::stored<T>)>,
                                             execution::set_error_t(std::exception_ptr), execution::set_stopped_t()>;

        template <class R> class operation {
        public:
            using operation_state_concept = execution::operation_state_t;

            operation(std::coroutine_handle<promise_type> h, R r) : h_(h), rcvr_(std::move(r)) {}
            operation(operation&&) = delete;
            ~operation() {
                if (h_)
                    h_.destroy();
            }

            void start() & noexcept {
                try {
                    if (!h_)
                        throw std::logic_error("cannot start an empty coroutine task");
                    h_.promise().stop_ = bridge_.attach(execution::get_stop_token(execution::get_env(rcvr_)));
                } catch (...) {
                    auto failure = util::current_exception();
                    bridge_.detach();
                    auto receiver = std::move(rcvr_);
                    execution::set_error(std::move(receiver), std::move(failure));
                    return;
                }
                auto& p = h_.promise();
                p.complete_ = &complete;
                p.complete_ctx_ = this;
                p.on_stopped_ = &stopped;
                p.stopped_ctx_ = this;
                h_.resume();
            }

        private:
            using token = execution::stop_token_of_t<execution::env_of_t<R>>;

            static auto complete(void* ctx, detail::promise_base&) noexcept -> std::coroutine_handle<> {
                auto& self = *static_cast<operation*>(ctx);
                auto& p = self.h_.promise();
                self.bridge_.detach();
                auto receiver = std::move(self.rcvr_);
                if (p.error_) {
                    auto failure = p.error_;
                    execution::set_error(std::move(receiver), std::move(failure));
                } else if constexpr (std::is_void_v<T>) {
                    execution::set_value(std::move(receiver));
                } else {
                    try {
                        auto value = std::move(*p.value_);
                        execution::set_value(std::move(receiver), std::move(value));
                    } catch (...) {
                        execution::set_error(std::move(receiver), util::current_exception());
                    }
                }
                return std::noop_coroutine();
            }

            static auto stopped(void* ctx) noexcept -> std::coroutine_handle<> {
                auto& self = *static_cast<operation*>(ctx);
                self.bridge_.detach();
                auto receiver = std::move(self.rcvr_);
                execution::set_stopped(std::move(receiver));
                return std::noop_coroutine();
            }

            std::coroutine_handle<promise_type> h_;
            R rcvr_;
            detail::stop_bridge<token> bridge_{};
        };

        template <execution::receiver R> auto connect(R r) && -> operation<R> { return operation<R>{std::exchange(h_, {}), std::move(r)}; }

    private:
        explicit task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}

        friend struct promise_type;
        template <class> friend class task;

        std::coroutine_handle<promise_type> h_;
    };

    class scope {
    public:
        void spawn(task<> child) {
            execution::spawn(execution::upon_error(std::move(child),
                                                   [this](std::exception_ptr e) noexcept {
                                                       if (!failure_)
                                                           failure_ = std::move(e);
                                                   }),
                             scope_.get_token(), execution::prop{execution::get_allocator, memory::frame_allocator<std::byte>{}});
        }

        void request_stop() noexcept { scope_.request_stop(); }

        auto join() -> task<> {
            co_await under(stop_token{}, join_unstoppable());
            if (auto e = std::exchange(failure_, {}))
                util::rethrow_exception(e);
        }

    private:
        auto join_unstoppable() -> task<> {
            co_await execution::write_env(scope_.join(), execution::prop{execution::get_start_scheduler, execution::inline_scheduler{}});
        }

        execution::counting_scope scope_;
        std::exception_ptr failure_{};
    };

    template <class Body>
        requires detail::is_task<std::invoke_result_t<Body&, scope&>>
    auto with_scope(Body body) -> std::invoke_result_t<Body&, scope&> {
        using T = typename std::invoke_result_t<Body&, scope&>::value_type;
        scope children;
        struct forward {
            scope* children;
            void operator()() const noexcept { children->request_stop(); }
        };
        auto parent = co_await current_token;
        std::optional<execution::stop_callback_for_t<stop_token, forward>> link;
        if (parent.stop_possible())
            link.emplace(parent, forward{&children});
        std::conditional_t<std::is_void_v<T>, bool, std::optional<T>> outcome{};
        std::exception_ptr failure;
        try {
            outcome = co_await stop_point(body(children));
        } catch (...) {
            failure = util::current_exception();
        }
        children.request_stop();
        try {
            co_await children.join();
        } catch (...) {
            if (!failure)
                failure = util::current_exception();
        }
        if (failure)
            util::rethrow_exception(failure);
        if (!outcome || parent.stop_requested())
            co_await coroutine::stop;
        if constexpr (!std::is_void_v<T>)
            co_return std::move(*outcome);
    }

    class mutex {
    public:
        class guard {
        public:
            explicit guard(mutex& m) noexcept : m_(&m) {}
            guard(guard&& o) noexcept : m_(std::exchange(o.m_, nullptr)) {}
            ~guard() {
                if (m_)
                    m_->unlock();
            }

        private:
            mutex* m_;
        };

        struct lock_awaiter {
            mutex& m;
            lock_awaiter* next = nullptr;
            std::coroutine_handle<> waiter{};
            bool linked_ = false;

            auto await_ready() noexcept -> bool { return !std::exchange(m.locked_, true); }
            void await_suspend(std::coroutine_handle<> h) noexcept {
                waiter = h;
                (m.tail_ ? m.tail_->next : m.head_) = this;
                m.tail_ = this;
                linked_ = true;
            }
            // If the waiting coroutine is destroyed while parked (cancellation), this
            // temporary is destroyed in place and the mutex list would keep a pointer
            // into the freed frame. Unlink it so the next unlock() never touches it.
            ~lock_awaiter() {
                if (!linked_)
                    return;
                lock_awaiter** link = &m.head_;
                while (*link && *link != this)
                    link = &(*link)->next;
                if (*link != this)
                    return;
                *link = next;
                if (m.tail_ == this)
                    m.tail_ = next ? next : nullptr;
            }
            auto await_resume() noexcept -> guard { return guard{m}; }
        };

        auto lock() noexcept -> lock_awaiter { return {*this}; }
        auto try_lock() noexcept -> std::optional<guard> {
            if (std::exchange(locked_, true))
                return std::nullopt;
            return guard{*this};
        }

    private:
        void unlock() noexcept {
            if (auto* next = head_) {
                head_ = next->next;
                if (!head_)
                    tail_ = nullptr;
                next->waiter.resume();
            } else {
                locked_ = false;
            }
        }

        bool locked_ = false;
        lock_awaiter* head_ = nullptr;
        lock_awaiter* tail_ = nullptr;
    };

    namespace detail {
        template <class Value> struct deadline_state {
            enum class winner { pending, work, timer };
            std::atomic<winner> first{winner::pending};
            std::optional<Value> out;
            stop_source source;
            std::mutex mutex;
            std::exception_ptr failure;
            void fail(std::exception_ptr error) {
                std::lock_guard lock(mutex);
                if (!failure)
                    failure = std::move(error);
            }
        };

        inline auto deadline_branch(stop_token token, task<> work) -> task<> { co_await under(token, std::move(work)); }

        template <class Sender, class Value> auto deadline_work(Sender work, deadline_state<Value>& race) -> task<> {
            using winner = typename deadline_state<Value>::winner;
            try {
                auto result = co_await stop_point(std::move(work));
                auto pending = winner::pending;
                if (result && race.first.compare_exchange_strong(pending, winner::work)) {
                    if constexpr (std::is_void_v<typename sender_awaiter<Sender, mode::plain>::value_type>)
                        race.out.emplace();
                    else
                        race.out.emplace(std::move(*result));
                }
            } catch (...) {
                race.fail(util::current_exception());
            }
            race.source.request_stop();
        }

        template <execution::timed_scheduler Io, class Value>
        auto deadline_timer(Io& io, std::chrono::nanoseconds limit, deadline_state<Value>& race) -> task<> {
            using winner = typename deadline_state<Value>::winner;
            try {
                auto result = co_await stop_point(execution::schedule_after(io, limit));
                auto pending = winner::pending;
                if (result)
                    race.first.compare_exchange_strong(pending, winner::timer);
            } catch (...) {
                race.fail(util::current_exception());
            }
            race.source.request_stop();
        }
    } // namespace detail

    template <execution::timed_scheduler Io, execution::sender Sender>
    auto deadline(Io& io, std::chrono::nanoseconds limit, Sender work)
        -> task<std::optional<detail::stored<typename detail::sender_awaiter<Sender, detail::mode::plain>::value_type>>> {
        using value = typename detail::sender_awaiter<Sender, detail::mode::plain>::value_type;
        using race_type = detail::deadline_state<detail::stored<value>>;
        race_type race;
        using winner = typename race_type::winner;
        struct forward {
            stop_source* source;
            void operator()() const noexcept { source->request_stop(); }
        };
        auto parent = co_await current_token;
        std::optional<execution::stop_callback_for_t<stop_token, forward>> link;
        if (parent.stop_possible())
            link.emplace(parent, forward{&race.source});

        scope children;
        try {
            children.spawn(detail::deadline_branch(race.source.get_token(), detail::deadline_work(std::move(work), race)));
            children.spawn(detail::deadline_branch(race.source.get_token(), detail::deadline_timer(io, limit, race)));
        } catch (...) {
            race.fail(util::current_exception());
            race.source.request_stop();
        }
        co_await under(stop_token{}, children.join());
        if (race.failure)
            util::rethrow_exception(race.failure);
        if (race.first.load(std::memory_order_acquire) == winner::pending)
            co_await coroutine::stop;
        co_return std::move(race.out);
    }
} // namespace coroutine
