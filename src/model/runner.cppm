export module runner;
import std;
import native;
import async;
import coroutine;
import protocol;
import constants;

export namespace runner {
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

    auto run_step(native::scheduler& io, io::command cmd, std::chrono::milliseconds timeout, std::uint64_t output_limit, std::string name,
                  std::optional<std::filesystem::path> log_path = {}, std::uint64_t log_limit = constants::log_bytes) -> coroutine::task<jobs::step_report> {
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
            children.spawn(kill_after_timeout(io, proc, timeout, timed_out));
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
        co_return jobs::step_report{.name = std::move(name),
                                    .exit_code = code,
                                    .timed_out = timed_out,
                                    .seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                                    .output = std::move(text),
                                    .truncated = truncated,
                                    .log_truncated = log_truncated,
                                    .log_error = std::move(log_error)};
    }

} // namespace runner
