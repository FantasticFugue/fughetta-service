export module process;
import std;
import mpmc;
import async;
import httpant.io;

export namespace native_process {
    auto self_usage() noexcept -> std::optional<io::process_usage>;
    auto sample(const io::process_record& process) noexcept -> std::optional<io::process_usage>;
    inline auto usage(io::process_group& processes) -> std::optional<io::process_usage> {
        std::lock_guard held{processes.mutex};
        io::process_usage total{};
        for (const auto& [pid, process] : processes.children) {
            std::lock_guard active{process->mutex};
            if (!process->live)
                return {};
            auto value = sample(*process);
            if (!value)
                return {};
            total.resident_bytes += value->resident_bytes;
            total.cpu_nanoseconds += value->cpu_nanoseconds;
            total.threads += value->threads;
            total.processes += value->processes;
        }
        return total;
    }
    class child {
    public:
        static auto spawn(native_io::engine<concurrent::queue>& engine, const io::command& command) -> child;
        child(child&&) noexcept;
        auto operator=(child&&) noexcept -> child&;
        child(const child&) = delete;
        ~child();
        auto id() const noexcept -> int;
        auto output() const noexcept -> native_io::handle;
        auto input() const noexcept -> native_io::handle;
        void close_input() noexcept;
        void terminate() noexcept;
        void kill() noexcept;
        auto reaped() const noexcept -> bool;
        void begin_wait(native_io::request& request) noexcept;
        auto ready(native_io::request& request, std::error_code& error) noexcept -> bool;
        auto reap() noexcept -> int;

    private:
        struct state;
        explicit child(std::unique_ptr<state> state);
        std::unique_ptr<state> state_;
    };
} // namespace native_process
