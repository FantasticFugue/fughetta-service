export module runner;
import std;
import native;
import async;
import coroutine;
import isolation;
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

    auto append_log(std::optional<native::file>& log, std::span<const std::byte> bytes, std::uint64_t& size,
                    std::uint64_t limit, bool& truncated, std::string& error) -> coroutine::task<> {
        if (!log) co_return;
        try {
            const auto kept = std::min<std::uint64_t>(bytes.size(), limit - size);
            truncated = truncated || kept != bytes.size();
            std::size_t offset = 0;
            while (offset < kept) {
                const auto written = co_await io::async_write_at(*log, bytes.subspan(offset, kept - offset), size);
                if (!written) throw std::runtime_error("cannot write execution log");
                offset += written;
                size += written;
            }
        } catch (const std::exception& failure) { error = failure.what(); log.reset(); }
    }

    auto run_step(native::scheduler& io, io::command cmd, std::chrono::milliseconds timeout, std::uint64_t output_limit, std::string name,
                  isolation::scope* resources = nullptr, std::optional<std::filesystem::path> log_path = {},
                  std::uint64_t log_limit = constants::log_bytes) -> coroutine::task<jobs::step_report> {
        auto started = std::chrono::steady_clock::now();
        std::optional<native::file> log;
        std::string log_error;
        if (log_path) try { log.emplace(io::open_file(io, *log_path, true)); }
        catch (const std::exception& error) { log_error = error.what(); }
        std::uint64_t log_size = 0;
        bool log_truncated = false;
        auto proc = io::spawn_process(io, cmd);
        tail out{output_limit};
        bool timed_out = false;
        bool cpu_exhausted = false;
        auto memory_before = resources ? resources->memory_failures() : 0;
        int code = -1;
        co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
            children.spawn([](native::scheduler& io, native::process& proc, std::chrono::milliseconds timeout, bool& timed_out) -> coroutine::task<> {
                co_await execution::schedule_after(io, timeout);
                timed_out = true;
                proc.kill();
            }(io, proc, timeout, timed_out));
            if (resources && cmd.max_cpu_seconds)
                children.spawn([](native::scheduler& io, native::process& proc, isolation::scope& resources, bool& exhausted) -> coroutine::task<> {
                    for (;;) {
                        if (resources.cpu_exhausted()) {
                            exhausted = true;
                            proc.kill();
                            co_return;
                        }
                        co_await execution::schedule_after(io, std::chrono::milliseconds{100});
                    }
                }(io, proc, *resources, cpu_exhausted));
            co_await execution::when_all(
                [](native::process& proc, tail& out, std::optional<native::file>& log, std::uint64_t& size,
                   std::uint64_t limit, bool& truncated, std::string& error) -> coroutine::task<> {
                    std::array<std::byte, 16384> buf;
                    while (auto n = co_await io::async_read_some(proc, net::buffer(buf))) {
                        out.append(std::span{buf}.first(n));
                        co_await append_log(log, std::span<const std::byte>{buf}.first(n), size, limit, truncated, error);
                    }
                }(proc, out, log, log_size, log_limit, log_truncated, log_error),
                [](native::process& proc, int& code) -> coroutine::task<> { code = co_await io::async_wait(proc); }(proc, code));
        });
        std::string diagnostics;
        auto diagnostic = [&](std::string_view message) {
            diagnostics += message;
            out.append(std::as_bytes(std::span{message}));
            code = 137;
        };
        if (resources && (cpu_exhausted || resources->cpu_exhausted()))
            diagnostic("\nworker: build process-tree CPU budget exhausted\n");
        if (resources && resources->memory_failures() > memory_before)
            diagnostic("\nworker: build process-tree memory limit exceeded (OOM)\n");
        if (!diagnostics.empty())
            co_await append_log(log, std::as_bytes(std::span{diagnostics}), log_size, log_limit, log_truncated, log_error);
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

    auto prepared_step(native::scheduler& io, io::command command, std::chrono::milliseconds timeout, std::uint64_t output_limit, std::string name,
                       isolation::pool& pool) -> coroutine::task<jobs::step_report> {
        auto resource = pool.start({}, {});
        resource->concurrency(1);
        command.isolation = resource->target();
        auto token = co_await coroutine::current_token;
        std::optional<jobs::step_report> result;
        std::exception_ptr error;
        try {
            result = co_await coroutine::under(token, run_step(io, std::move(command), timeout, output_limit, std::move(name), resource.get()));
        } catch (...) {
            error = std::current_exception();
        }
        resource->close();
        if (error)
            std::rethrow_exception(error);
        if (!result)
            co_await coroutine::stop;
        co_return std::move(*result);
    }
} // namespace runner
