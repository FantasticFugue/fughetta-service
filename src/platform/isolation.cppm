export module isolation;
import std;
import async;

export namespace isolation {
    using target = io::isolation_target;
    class scope {
    public:
        scope(std::filesystem::path path, std::optional<std::uint64_t> memory, std::optional<std::uint32_t> cpu);
        scope(const scope&) = delete;
        ~scope();
        auto target() const -> isolation::target;
        auto cpu_exhausted() const -> bool;
        auto memory_failures() const -> std::uint64_t;
        void concurrency(std::uint32_t parallel);
        void close();

    private:
        struct state;
        std::unique_ptr<state> state_;
    };
    // The cgroup this process started in, prepared to hold build groups: the process moves into a leaf and the root delegates cpu and memory.
    // On Windows it names the job root.
    auto delegated() -> std::filesystem::path;
    class pool {
    public:
        explicit pool(std::string_view worker_key);
        pool(const std::filesystem::path& root, std::string_view worker_key);
        pool(const pool&) = delete;
        ~pool();
        auto start(std::optional<std::uint64_t> memory, std::optional<std::uint32_t> cpu) -> std::shared_ptr<scope>;

    private:
        struct state;
        std::unique_ptr<state> state_;
    };
} // namespace isolation
