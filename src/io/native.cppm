export module native;
import std;
export import httpant.io;
import async;
import affinity;
import process;
export import mpmc;

export namespace native {
    using socket = http::io::net::basic_stream_socket<http::io::net::ip::tcp, concurrent::queue>;
    using datagram = http::io::net::basic_datagram_socket<http::io::net::ip::udp, concurrent::queue>;
    using listener = http::io::net::basic_socket_acceptor<http::io::net::ip::tcp, concurrent::queue>;
    using file = http::io::net::basic_file<concurrent::queue>;
    using scheduler = http::io::net::basic_scheduler<concurrent::queue>;

    class process {
    public:
        process(process&&) noexcept = default;
        auto operator=(process&&) noexcept -> process& = default;
        ~process() = default;
        auto pid() const noexcept -> int { return s_->child.id(); }
        auto read_some(std::span<std::byte> into) {
            return http::io::net::detail::sender<concurrent::queue, std::size_t, http::io::net::detail::count_result>{
                {.kind = native_io::operation::read, .resource = {s_->engine}, .file = s_->child.output(), .buffer = into.data(), .size = into.size()}, {}};
        }
        // Requires a process started with io::command::interactive.
        auto write_some(std::span<const std::byte> from) {
            return http::io::net::detail::sender<concurrent::queue, std::size_t, http::io::net::detail::count_result>{
                {.kind = native_io::operation::write,
                 .resource = {s_->engine},
                 .file = s_->child.input(),
                 .buffer = const_cast<std::byte*>(from.data()),
                 .size = from.size()},
                {}};
        }
        void close_input() noexcept { s_->child.close_input(); }
        auto wait() { return waiting{s_.get()}; }
        void terminate() noexcept { s_->child.terminate(); }
        void kill() noexcept { s_->child.kill(); }
        static auto spawn(const http::io::net::basic_scheduler<concurrent::queue>& on, const io::command& command) -> process {
            return process{std::make_unique<state>(on.engine(), native_process::child::spawn(*on.engine(), command))};
        }

    private:
        struct state {
            native_io::engine<concurrent::queue>* engine;
            native_process::child child;
        };
        explicit process(std::unique_ptr<state> state) : s_(std::move(state)) {}
        struct waiting {
            using sender_concept = execution::sender_t;
            using completion_signatures = execution::completion_signatures<execution::set_value_t(int), execution::set_error_t(std::error_code),
                                                                           execution::set_error_t(std::exception_ptr), execution::set_stopped_t()>;
            state* s;
            auto get_env() const noexcept;
            template <class R> struct op : native_io::request {
                using operation_state_concept = execution::operation_state_t;
                R receiver;
                state* s;
                std::atomic<bool> killed{false};
                struct on_stop {
                    op* self;
                    void operator()() const noexcept {
                        self->killed.store(true, std::memory_order_release);
                        self->s->child.kill();
                    }
                };
                using token = execution::stop_token_of_t<execution::env_of_t<R>>;
                std::optional<execution::stop_callback_for_t<token, on_stop>> watch;
                op(R r, state* state) : receiver(std::move(r)), s(state) {
                    this->complete = [](native_io::request& request, std::int64_t, std::error_code error) noexcept {
                        auto& self = static_cast<op&>(request);
                        if (!self.s->child.ready(request, error))
                            return;
                        self.watch.reset();
                        if (error) {
                            execution::set_error(std::move(self.receiver), error);
                            return;
                        }
                        auto code = self.s->child.reap();
                        if (self.killed.load(std::memory_order_acquire))
                            execution::set_stopped(std::move(self.receiver));
                        else
                            execution::set_value(std::move(self.receiver), code);
                    };
                }
                op(op&&) = delete;
                void start() & noexcept {
                    if (s->child.reaped()) {
                        execution::set_error(std::move(receiver), std::make_error_code(std::errc::no_child_process));
                        return;
                    }
                    try {
                        watch.emplace(execution::get_stop_token(execution::get_env(receiver)), on_stop{this});
                    } catch (...) {
                        execution::set_error(std::move(receiver), std::current_exception());
                        return;
                    }
                    s->child.begin_wait(*this);
                }
            };
            template <execution::receiver R> auto connect(R receiver) const -> op<R> { return {std::move(receiver), s}; }
        };
        std::unique_ptr<state> s_;
    };

    inline auto process::waiting::get_env() const noexcept {
        return execution::prop{execution::get_completion_scheduler<execution::set_value_t>, scheduler{{s->engine}}};
    }

    class context : public http::io::net::basic_io_context<concurrent::queue> {
    public:
        context() = default;
        explicit context(cores::core_index core) { affinity::bind_current(core); }
    };
    class reconnect_backoff {
    public:
        explicit reconnect_backoff(std::uint32_t seed = std::random_device{}()) : random_(seed) {}
        auto next() -> std::chrono::milliseconds {
            auto delay = std::uniform_int_distribution<int>{ceiling_ms_ / 2, ceiling_ms_}(random_);
            ceiling_ms_ = std::min(ceiling_ms_ * 2, 30000);
            return std::chrono::milliseconds{delay};
        }
        void reset() noexcept { ceiling_ms_ = 1000; }
        static constexpr auto stable_after = std::chrono::seconds{30};

    private:
        std::mt19937 random_;
        int ceiling_ms_ = 1000;
    };

    static_assert(io::stream<socket>);
    static_assert(io::context<context>);
    static_assert(io::network_scheduler<scheduler>);
    static_assert(io::file_scheduler<scheduler>);
    static_assert(execution::timed_scheduler<scheduler>);
} // namespace native

export template <> struct io::spawner<native::scheduler> {
    static auto spawn(const native::scheduler& on, const io::command& command) -> native::process { return native::process::spawn(on, command); }
};
static_assert(io::process_scheduler<native::scheduler>);
