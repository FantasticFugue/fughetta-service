export module runner;
import std;
import native;
import async;
import coroutine;
import provider;
import constants;
import json;
import utility;

export namespace runner {
    struct result {
        std::string name;
        std::int32_t exit_code = -1;
        bool timed_out = false;
        double seconds = 0;
        std::string output;
        bool truncated = false;
        // What the step's reports say, read on the worker.
        std::vector<builds::finding> diagnostics;
        std::optional<builds::tally> tests;
        std::optional<builds::exported_file> log;
        bool log_truncated = false;
        std::string log_error;
        std::string cache_error;
        // A batch shares one process outcome and elapsed time across these tasks.
        std::vector<std::string> tasks;
    };

    class tail {
    public:
        explicit tail(std::uint64_t limit) : limit_(limit) {}

        void append(std::span<const std::byte> chunk) {
            if (!chunk.empty())
                text_.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
            if (text_.size() > 2 * limit_ + 4096)
                trim();
        }
        auto take() -> std::pair<std::string, bool> {
            trim();
            return {std::move(text_), truncated_};
        }

    private:
        void trim() {
            if (text_.size() <= limit_)
                return;
            text_.erase(0, text_.size() - limit_);
            truncated_ = true;
        }

        std::uint64_t limit_;
        std::string text_;
        bool truncated_ = false;
    };

    auto append_log(std::optional<native::file>& log, std::span<const std::byte> bytes, std::uint64_t& size, std::uint64_t limit, bool& truncated,
                    std::string& error) -> coroutine::task<> {
        if (!log)
            co_return;
        try {
            const auto kept = std::min<std::uint64_t>(bytes.size(), limit - size);
            truncated = truncated || kept != bytes.size();
            std::size_t offset = 0;
            while (offset < kept) {
                const auto written = co_await io::async_write_at(*log, bytes.subspan(offset, kept - offset), size);
                if (!written)
                    throw std::runtime_error("cannot write execution log");
                offset += written;
                size += written;
            }
        } catch (const std::exception& failure) {
            error = failure.what();
            log.reset();
        }
    }

    auto run_step(native::scheduler& io, io::command cmd, std::optional<std::chrono::milliseconds> timeout, std::uint64_t output_limit, std::string name,
                  std::optional<std::filesystem::path> log_path = {}, std::uint64_t log_limit = constants::log_bytes) -> coroutine::task<result> {
        auto started = std::chrono::steady_clock::now();
        std::optional<native::file> log;
        std::string log_error;
        if (log_path)
            try {
                log.emplace(io::open_file(io, *log_path, true));
            } catch (const std::exception& error) {
                log_error = error.what();
            }
        std::uint64_t log_size = 0;
        bool log_truncated = false;
        auto proc = io::spawn_process(io, cmd);
        if (cmd.stdio != io::process_stdio::captured)
            proc.close_input();
        tail out{output_limit};
        bool timed_out = false;
        int code = -1;
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            static constexpr auto kill_after_timeout = [](native::scheduler& io, native::process& proc, std::chrono::milliseconds timeout,
                                                          bool& timed_out) -> coroutine::task<> {
                co_await execution::schedule_after(io, timeout);
                timed_out = true;
                proc.kill();
            };
            if (timeout)
                children.spawn(kill_after_timeout(io, proc, *timeout, timed_out));
            static constexpr auto wait_for_exit = [](native::process& proc, int& code) -> coroutine::task<> { code = co_await io::async_wait(proc); };
            static constexpr auto read_output = [](native::process& proc, tail& out, std::optional<native::file>& log, std::uint64_t& size, std::uint64_t limit,
                                                   bool& truncated, std::string& error) -> coroutine::task<> {
                std::array<std::byte, 16384> buf;
                while (auto n = co_await io::async_read_some(proc, net::buffer(buf))) {
                    out.append(std::span{buf}.first(n));
                    co_await append_log(log, std::span<const std::byte>{buf}.first(n), size, limit, truncated, error);
                }
            };
            co_await execution::when_all(read_output(proc, out, log, log_size, log_limit, log_truncated, log_error), wait_for_exit(proc, code));
        };
        co_await coroutine::with_scope(std::move(run_children));
        auto [text, truncated] = out.take();
        co_return result{.name = std::move(name),
                         .exit_code = code,
                         .timed_out = timed_out,
                         .seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                         .output = std::move(text),
                         .truncated = truncated,
                         .log_truncated = log_truncated,
                         .log_error = std::move(log_error)};
    }

    class gradle_session {
        struct request {
            std::vector<std::string> arguments;
            builds::environment environment;
            std::uint64_t output_limit, log_limit;
            std::optional<std::string> log;
        };
        template <class T> struct message {
            std::uint64_t request_id;
            std::string_view operation;
            T params;
        };
        struct response {
            std::int32_t exit_code;
            bool reusable;
            std::string output;
            bool truncated, log_truncated;
            std::string log_error;
        };
        struct reply {
            std::uint64_t request_id;
            std::optional<response> result;
            std::optional<std::string> error;
        };

    public:
        gradle_session(native::scheduler& io, const io::command& command) : io_(io), process_(io::spawn_process(io, command)) {}
        auto alive() const -> bool { return process_.has_value(); }

        auto close() -> coroutine::task<> {
            if (!process_)
                co_return;
            process_->close_input();
            const auto exited = co_await coroutine::deadline(io_, std::chrono::milliseconds{500}, wait_for_close());
            if (!exited) {
                process_->kill();
                (void)co_await io::async_wait(*process_);
            }
            process_.reset();
        }

        auto run(std::vector<std::string> arguments, builds::environment environment, std::chrono::milliseconds timeout, std::uint64_t output_limit,
                 std::string name, const std::optional<std::filesystem::path>& log, std::uint64_t log_limit) -> coroutine::task<result> {
            const auto started = std::chrono::steady_clock::now();
            request input{std::move(arguments), std::move(environment), std::min(output_limit, constants::semantic_object_bytes / 8), log_limit,
                          log ? std::optional{log->string()} : std::nullopt};
            const auto id = ++sequence_;
            std::optional<std::optional<response>> completed;
            std::exception_ptr failed;
            try {
                completed = co_await coroutine::stop_point(coroutine::deadline(io_, timeout, execute(id, input)));
            } catch (...) {
                failed = util::capture_current_exception();
            }
            if (failed) {
                co_await coroutine::under(coroutine::stop_token{}, close());
                std::rethrow_exception(failed);
            }
            const bool timed_out = !completed || !*completed;
            if (timed_out) {
                auto canceled = co_await coroutine::under(coroutine::stop_token{}, cancel(id));
                if (!completed)
                    co_await coroutine::stop;
                *completed = std::move(*canceled);
            } else if (!(**completed).reusable) {
                co_await coroutine::under(coroutine::stop_token{}, close());
            }
            auto response = std::move(*completed).value_or(gradle_session::response{});
            co_return result{.name = std::move(name), .exit_code = timed_out ? -1 : response.exit_code, .timed_out = timed_out,
                             .seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                             .output = std::move(response.output), .truncated = response.truncated,
                             .log_truncated = response.log_truncated, .log_error = std::move(response.log_error)};
        }

    private:
        auto wait_for_close() -> coroutine::task<int> { co_return co_await io::async_wait(*process_); }

        template <class T> auto send(const message<T>& value) -> coroutine::task<> {
            auto line = json::write(value) + '\n';
            if (line.size() > constants::semantic_object_bytes)
                throw std::length_error("Gradle request exceeds the object limit");
            co_await io::async_write(*process_, std::as_bytes(std::span{line}));
        }

        auto receive(std::uint64_t id) -> coroutine::task<response> {
            for (;;) {
                if (const auto end = buffer_.find('\n'); end != buffer_.npos) {
                    auto value = json::read<reply>(std::string_view{buffer_}.substr(0, end)).value();
                    buffer_.erase(0, end + 1);
                    if (value.request_id != id)
                        throw std::runtime_error("Gradle returned another request's result");
                    if (value.error)
                        throw std::runtime_error(*value.error);
                    if (!value.result)
                        throw std::runtime_error("Gradle response has no result");
                    co_return std::move(*value.result);
                }
                std::array<std::byte, 16384> bytes;
                const auto count = co_await io::async_read_some(*process_, net::buffer(bytes));
                if (!count)
                    throw std::runtime_error("Gradle host closed its output");
                if (buffer_.size() + count > constants::semantic_object_bytes)
                    throw std::length_error("Gradle response exceeds the object limit");
                buffer_.append(reinterpret_cast<const char*>(bytes.data()), count);
            }
        }

        auto execute(std::uint64_t id, const request& input) -> coroutine::task<response> {
            co_await send(message{id, "build", input});
            co_return co_await receive(id);
        }

        auto cancel(std::uint64_t id) -> coroutine::task<std::optional<response>> {
            auto request_stop = [&]() -> coroutine::task<response> {
                co_await send(message{id, "cancel", json::object{}});
                co_return co_await receive(id);
            };
            std::optional<response> completed;
            try {
                completed = co_await coroutine::deadline(io_, std::chrono::milliseconds{250}, request_stop());
            } catch (const std::exception&) {
            }
            co_await close();
            co_return completed;
        }

        native::scheduler& io_;
        std::optional<native::process> process_;
        std::string buffer_;
        std::uint64_t sequence_ = 0;
    };

} // namespace runner
