export module process;
import std;
import mpmc;
import async;
import httpant.io;

export namespace native_process {
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
