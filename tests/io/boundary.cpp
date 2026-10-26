import std;
import boost.ut;
import coroutine;
import http;
import httpant.io;
import native;
import utility;

namespace {
    using namespace boost::ut;

    enum class channel { value, error, code_error, stopped };
    enum class timing { early, late, racing };

    struct threads {
        std::thread worker;
        std::atomic<bool> release{false};
        std::atomic<int> destroyed{0};
        std::thread::id completing_on;
        ~threads() {
            if (worker.joinable())
                worker.join();
        }
    };

    struct threaded {
        using sender_concept = execution::sender_t;
        using completion_signatures = execution::completion_signatures<execution::set_value_t(int), execution::set_error_t(std::exception_ptr),
                                                                       execution::set_error_t(std::error_code), execution::set_stopped_t()>;
        threads* control;
        timing when;
        channel result;
        template <class R> struct operation {
            using operation_state_concept = execution::operation_state_t;
            R receiver;
            threads* control;
            timing when;
            channel result;
            ~operation() { ++control->destroyed; }
            void start() & noexcept {
                auto* owner = control;
                auto early = when == timing::early;
                owner->worker = std::thread([r = std::move(receiver), owner, when = when, result = result]() mutable {
                    while (when == timing::late && !owner->release.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    owner->completing_on = std::this_thread::get_id();
                    if (result == channel::value)
                        execution::set_value(std::move(r), 42);
                    else if (result == channel::error)
                        execution::set_error(std::move(r), std::make_exception_ptr(std::runtime_error("thread error")));
                    else if (result == channel::code_error)
                        execution::set_error(std::move(r), std::make_error_code(std::errc::connection_reset));
                    else
                        execution::set_stopped(std::move(r));
                });
                if (early)
                    owner->worker.join();
            }
        };
        template <class R> auto connect(R receiver) && -> operation<R> { return {std::move(receiver), control, when, result}; }
    };

    struct observation {
        int completions = 0, resumed = 0, cleaned = 0;
        channel result{};
        std::string text;
        std::thread::id resumed_on;
    };

    struct cleanup {
        observation* observed;
        ~cleanup() { ++observed->cleaned; }
    };

    template <class Token = execution::never_stop_token> struct destroying_receiver {
        using receiver_concept = execution::receiver_t;
        util::move_only_function<void()> destroy;
        observation* observed;
        Token stop{};
        auto get_env() const noexcept { return execution::prop{execution::get_stop_token, stop}; }
        template <class Value> void set_value(const Value& value) noexcept {
            destroy();
            ++observed->completions;
            observed->result = channel::value;
            if constexpr (std::same_as<Value, std::string>)
                observed->text = value;
            else if constexpr (std::same_as<Value, int>)
                observed->text = std::to_string(value);
        }
        void set_value() noexcept {
            destroy();
            ++observed->completions;
            observed->result = channel::value;
        }
        void set_error(const std::exception_ptr& failure) noexcept {
            destroy();
            ++observed->completions;
            observed->result = channel::error;
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& e) {
                observed->text = e.what();
            }
        }
        void set_stopped() noexcept {
            destroy();
            ++observed->completions;
            observed->result = channel::stopped;
        }
    };

    template <class Sender> void start_destroying(Sender sender, observation& observed, threads* control = nullptr) {
        using operation = execution::connect_result_t<Sender, destroying_receiver<>>;
        std::unique_ptr<operation> state;
        state.reset(new operation(execution::connect(std::move(sender), destroying_receiver<>{[&state] { state.reset(); }, &observed})));
        execution::start(*state);
        if (control) {
            control->release.store(true, std::memory_order_release);
            if (control->worker.joinable())
                control->worker.join();
        }
        expect(!state);
        expect(observed.completions == 1_i);
    }

    enum class policy { plain, expected, stop_point };

    auto await_thread(threaded sender, policy how, observation& observed) -> coroutine::task<std::string> {
        cleanup guard{&observed};
        std::string text;
        if (how == policy::expected) {
            auto value = co_await coroutine::expect(std::move(sender));
            static_assert(std::variant_size_v<typename decltype(value)::error_type> == 2);
            if (value)
                text = std::to_string(*value);
            else if (auto* code = std::get_if<std::error_code>(&value.error())) {
                expect(*code == std::errc::connection_reset);
                text = "expected code";
            } else {
                expect(bool(std::get<std::exception_ptr>(value.error())));
                text = "expected error";
            }
        } else if (how == policy::stop_point) {
            auto value = co_await coroutine::stop_point(std::move(sender));
            text = value ? std::to_string(*value) : "caught stop";
        } else {
            text = std::to_string(co_await std::move(sender));
        }
        ++observed.resumed;
        observed.resumed_on = std::this_thread::get_id();
        co_return text;
    }

    auto http_thread(threaded sender) -> http::task<int> {
        using http::operator co_await;
        co_return co_await std::move(sender);
    }
    auto nested_http(threaded sender, observation& observed) -> coroutine::task<std::string> {
        cleanup guard{&observed};
        auto value = co_await http_thread(std::move(sender));
        ++observed.resumed;
        co_return std::to_string(value);
    }

    template <execution::sender Sender>
    auto await_operation(Sender sender) -> coroutine::task<> { co_await std::move(sender); }

    auto http_wait(native::scheduler scheduler, bool& entered) -> http::task<> {
        entered = true;
        using http::operator co_await;
        co_await scheduler.schedule_after(std::chrono::hours{1});
    }
    auto application_wait(native::scheduler scheduler, bool& entered, observation& observed,
                          coroutine::stop_source* stop_before_await = nullptr) -> coroutine::task<> {
        cleanup guard{&observed};
        if (stop_before_await)
            stop_before_await->request_stop();
        co_await http_wait(scheduler, entered);
        ++observed.resumed;
    }

    auto immediate(channel result, observation& observed) -> coroutine::task<std::string> {
        cleanup guard{&observed};
        if (result == channel::error)
            throw std::runtime_error("root error");
        if (result == channel::stopped)
            co_await coroutine::stop;
        co_return std::string(4096, 'x');
    }

    struct delivery_value {
        int* moves;
        explicit delivery_value(int& count) noexcept : moves(&count) {}
        delivery_value(delivery_value&& other) : moves(other.moves) {
            if (++*moves == 2)
                throw std::runtime_error("delivery move failed");
        }
    };
    auto throwing_delivery(int& moves) -> coroutine::task<delivery_value> { co_return delivery_value{moves}; }

    template <class F> struct failed_callback {
        template <class Token> failed_callback(Token, F) { throw std::runtime_error("registration failed"); }
    };
    struct failing_token {
        template <class F> using callback_type = failed_callback<F>;
        bool stop_possible() const noexcept { return true; }
        bool stop_requested() const noexcept { return false; }
        bool operator==(const failing_token&) const noexcept = default;
    };

    static constexpr auto boundary_suite = [] {
        "expect preserves error codes from a sender with multiple error types"_test = [] {
            for (auto when : {timing::early, timing::late, timing::racing}) {
                for (int repeat = 0; repeat < (when == timing::racing ? 64 : 1); ++repeat) {
                    threads control;
                    observation observed;
                    start_destroying(await_thread(threaded{&control, when, channel::code_error}, policy::expected, observed), observed, &control);
                    expect(observed.result == channel::value);
                    expect(observed.text == "expected code");
                    expect(observed.completions == 1_i && observed.resumed == 1_i && observed.cleaned == 1_i);
                    expect(control.destroyed.load() == 1_i);
                }
            }
        };

        "expect preserves void values and single errors"_test = [] {
            observation observed;
            start_destroying(
                []() -> coroutine::task<> {
                    auto value = co_await coroutine::expect(execution::just());
                    static_assert(std::same_as<decltype(value), std::expected<void, std::error_code>>);
                    expect(value.has_value());
                    auto failure = co_await coroutine::expect(execution::just_error(std::make_error_code(std::errc::permission_denied)));
                    static_assert(std::same_as<decltype(failure), std::expected<void, std::error_code>>);
                    expect(!failure);
                    expect(failure.error() == std::errc::permission_denied);
                }(),
                observed);
            expect(observed.result == channel::value);
        };

        "threaded completions preserve plain expect and stop_point policies"_test = [] {
            for (auto when : {timing::early, timing::late, timing::racing}) {
                for (auto result : {channel::value, channel::error, channel::stopped}) {
                    for (auto how : {policy::plain, policy::expected, policy::stop_point}) {
                        for (int repeat = 0; repeat < (when == timing::racing ? 64 : 1); ++repeat) {
                            threads control;
                            observation observed;
                            start_destroying(await_thread(threaded{&control, when, result}, how, observed), observed, &control);
                            auto caught_error = result == channel::error && how == policy::expected;
                            auto caught_stop = result == channel::stopped && how == policy::stop_point;
                            auto resumes = result == channel::value || caught_error || caught_stop;
                            expect(observed.result == (resumes ? channel::value : result));
                            expect(observed.resumed == int(resumes));
                            expect(observed.cleaned == 1_i);
                            expect(control.destroyed.load() == 1_i);
                            if (resumes) {
                                expect(observed.text == (caught_error ? "expected error" : caught_stop ? "caught stop" : "42"));
                                if (when != timing::racing)
                                    expect(observed.resumed_on == (when == timing::early ? std::this_thread::get_id() : control.completing_on));
                            } else if (result == channel::error)
                                expect(observed.text == "thread error");
                        }
                    }
                }
            }
        };

        "root completion survives immediate operation destruction"_test = [] {
            for (auto result : {channel::value, channel::error, channel::stopped}) {
                observation observed;
                start_destroying(immediate(result, observed), observed);
                expect(observed.result == result);
                expect(observed.text == (result == channel::value ? std::string(4096, 'x') : result == channel::error ? "root error" : ""));
                expect(observed.cleaned == 1_i);
            }
            observation observed;
            start_destroying([]() -> coroutine::task<> { co_return; }(), observed);
            expect(observed.result == channel::value);
        };

        "a throwing root result move uses the error channel"_test = [] {
            int moves = 0;
            observation observed;
            start_destroying(throwing_delivery(moves), observed);
            expect(observed.result == channel::error);
            expect(observed.text == "delivery move failed");
            expect(moves == 2_i);
        };

        "token registration failure completes without starting the coroutine"_test = [] {
            observation observed;
            using receiver = destroying_receiver<failing_token>;
            using operation = execution::connect_result_t<coroutine::task<std::string>, receiver>;
            std::unique_ptr<operation> state;
            state.reset(new operation(execution::connect(immediate(channel::value, observed), receiver{[&state] { state.reset(); }, &observed})));
            execution::start(*state);
            expect(!state);
            expect(observed.completions == 1_i && observed.cleaned == 0_i);
            expect(observed.result == channel::error);
            expect(observed.text == "registration failed");
        };

        "direct HTTP task awaiting forwards in-flight cancellation"_test = [] {
            native::context context;
            auto scheduler = context.get_scheduler();
            coroutine::stop_source source;
            bool entered = false;
            observation observed;
            auto work = execution::when_all(application_wait(scheduler, entered, observed),
                execution::then(scheduler.schedule_after(std::chrono::milliseconds{2}), [&] { source.request_stop(); }));
            auto root = await_operation(std::move(work));
            using receiver = destroying_receiver<coroutine::stop_token>;
            using operation = execution::connect_result_t<decltype(root), receiver>;
            std::unique_ptr<operation> state;
            state.reset(new operation(execution::connect(std::move(root),
                receiver{[&] { state.reset(); context.stop(); }, &observed, source.get_token()})));
            execution::start(*state);
            context.run();
            expect(!state && entered);
            expect(observed.result == channel::stopped);
            expect(observed.completions == 1_i && observed.resumed == 0_i && observed.cleaned == 1_i);
        };

        "direct HTTP task awaiting does not enter a pre-stopped child"_test = [] {
            native::context context;
            coroutine::stop_source source;
            bool entered = false;
            observation observed;
            auto work = application_wait(context.get_scheduler(), entered, observed, &source);
            using receiver = destroying_receiver<coroutine::stop_token>;
            using operation = execution::connect_result_t<decltype(work), receiver>;
            std::unique_ptr<operation> state;
            state.reset(new operation(execution::connect(std::move(work),
                receiver{[&] { state.reset(); }, &observed, source.get_token()})));
            execution::start(*state);
            expect(!state && !entered);
            expect(observed.result == channel::stopped);
            expect(observed.completions == 1_i && observed.resumed == 0_i && observed.cleaned == 1_i);
        };

        "httpant and application bridges compose across threaded completion"_test = [] {
            for (auto when : {timing::early, timing::late, timing::racing}) {
                for (auto result : {channel::value, channel::error, channel::stopped}) {
                    threads control;
                    observation observed;
                    start_destroying(nested_http(threaded{&control, when, result}, observed), observed, &control);
                    expect(observed.result == result);
                    expect(observed.resumed == int(result == channel::value));
                    expect(observed.cleaned == 1_i);
                    expect(control.destroyed.load() == 1_i);
                    expect(observed.text == (result == channel::value ? "42" : result == channel::error ? "thread error" : ""));
                }
            }
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"async boundary">{boundary_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
