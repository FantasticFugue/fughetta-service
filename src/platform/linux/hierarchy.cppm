module;
#include <cerrno>
#include <fcntl.h>
#include <sys/statfs.h>
#include <unistd.h>

module isolation;
import std;
import async;
import cgroup;
import descriptor;

namespace isolation {
    using cgroup::counter;
    using cgroup::membership;
    using cgroup::read;
    using cgroup::reap;
    using cgroup::verify;
    using cgroup::write;
    struct scope::state {
    public:
        state(std::filesystem::path path, std::optional<std::uint64_t> memory, std::optional<std::uint32_t> cpu) : path_(std::move(path)), cpu_(cpu) {
            if (!std::filesystem::create_directory(path_))
                throw std::runtime_error(std::format("cgroup '{}' already exists", path_.string()));
            try {
                membership probe{isolation::target{path_.string()}};
                write(path_ / "memory.oom.group", "1");
                if (memory) {
                    write(path_ / "memory.max", std::to_string(*memory));
                    write(path_ / "memory.swap.max", "0");
                }
                counter(read(path_ / "cpu.stat"), "usage_usec");
                counter(read(path_ / "memory.events"), "oom_kill");
            } catch (...) {
                ::rmdir(path_.c_str());
                throw;
            }
        }
        ~state() {
            if (closed_)
                return;
            try {
                write(path_ / "cgroup.kill", "1");
                if (::rmdir(path_.c_str()) != 0)
                    descriptor::fail("cannot remove stopped cgroup", path_);
            } catch (...) {
                constexpr char error[] = "worker: cgroup cleanup incomplete; retained for recovery\n";
                ::write(2, error, sizeof(error) - 1);
            }
        }
        auto target() const -> isolation::target { return {path_.string()}; }
        auto cpu_exhausted() const -> bool { return cpu_ && counter(read(path_ / "cpu.stat"), "usage_usec") >= std::uint64_t{*cpu_} * 1000000; }
        auto memory_failures() const -> std::uint64_t { return counter(read(path_ / "memory.events"), "oom_kill"); }
        void concurrency(std::uint32_t parallel) {
            if (!parallel) throw std::invalid_argument("CPU concurrency must be positive");
            write(path_ / "cpu.max", std::format("{} 100000", std::uint64_t{parallel} * 100000));
        }
        void close() {
            reap(path_);
            closed_ = true;
        }

    private:
        std::filesystem::path path_;
        std::optional<std::uint32_t> cpu_;
        bool closed_ = false;
    };

    struct pool::state {
    public:
        explicit state(std::string_view worker_key) : state(cgroup::root, worker_key) {}

        state(const std::filesystem::path& root, std::string_view worker_key) {
            verify(root);
            if (worker_key.empty() || !std::ranges::all_of(worker_key, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                throw std::invalid_argument("invalid worker cgroup key");
            path_ = root / ("worker-" + std::string{worker_key});
            std::filesystem::create_directory(path_);
            write(path_ / "cgroup.subtree_control", "+cpu +memory");
            for (auto& entry : std::filesystem::directory_iterator(path_))
                if (entry.is_directory() && entry.path().filename().string().starts_with("build-"))
                    reap(entry.path());
        }
        auto start(std::optional<std::uint64_t> memory, std::optional<std::uint32_t> cpu) -> std::shared_ptr<scope> {
            static std::atomic<std::uint64_t> serial{0};
            auto name = std::format("build-{}-{}", ::getpid(), serial.fetch_add(1));
            return std::make_shared<scope>(path_ / name, memory, cpu);
        }

    private:
        std::filesystem::path path_;
    };
    scope::scope(std::filesystem::path path, std::optional<std::uint64_t> memory, std::optional<std::uint32_t> cpu)
        : state_(std::make_unique<state>(std::move(path), memory, cpu)) {}
    scope::~scope() = default;
    auto scope::target() const -> isolation::target { return state_->target(); }
    auto scope::cpu_exhausted() const -> bool { return state_->cpu_exhausted(); }
    auto scope::memory_failures() const -> std::uint64_t { return state_->memory_failures(); }
    void scope::concurrency(std::uint32_t parallel) { state_->concurrency(parallel); }
    void scope::close() { state_->close(); }
    pool::pool(std::string_view worker_key) : state_(std::make_unique<state>(worker_key)) {}
    pool::pool(const std::filesystem::path& root, std::string_view worker_key) : state_(std::make_unique<state>(root, worker_key)) {}
    pool::~pool() = default;
    auto pool::start(std::optional<std::uint64_t> memory, std::optional<std::uint32_t> cpu) -> std::shared_ptr<scope> { return state_->start(memory, cpu); }
} // namespace isolation
