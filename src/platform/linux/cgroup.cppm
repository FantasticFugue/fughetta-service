module;
#include <cerrno>
#include <fcntl.h>
#include <sys/statfs.h>
#include <unistd.h>

export module cgroup;
import std;
import async;
import descriptor;

export namespace cgroup {
    using target = io::isolation_target;
    inline constexpr std::string_view root = "/sys/fs/cgroup/workers";
    inline void write(const std::filesystem::path& path, std::string_view value) {
        auto fd = descriptor::open(path, O_WRONLY);
        ssize_t n;
        do {
            n = ::write(fd.get(), value.data(), value.size());
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            descriptor::fail("cannot set cgroup control", path);
        if (static_cast<std::size_t>(n) != value.size())
            descriptor::fail("short cgroup control write", path, EIO);
    }

    inline auto read(const std::filesystem::path& path) -> std::string {
        auto fd = descriptor::open(path, O_RDONLY);
        std::array<char, 4096> buffer;
        ssize_t n;
        do {
            n = ::read(fd.get(), buffer.data(), buffer.size());
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            descriptor::fail("cannot read cgroup control", path);
        if (n == static_cast<ssize_t>(buffer.size()))
            descriptor::fail("oversized cgroup control", path, EOVERFLOW);
        return {buffer.data(), static_cast<std::size_t>(n)};
    }

    inline auto counter(std::string_view text, std::string_view wanted) -> std::uint64_t {
        for (auto line : std::views::split(text, '\n')) {
            std::string_view row{line.begin(), line.end()};
            auto split = row.find(' ');
            if (split == std::string_view::npos || row.substr(0, split) != wanted)
                continue;
            auto value = row.substr(split + 1);
            std::uint64_t out = 0;
            auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
            if (ec == std::errc{} && end == value.data() + value.size())
                return out;
            break;
        }
        throw std::runtime_error(std::format("missing or invalid cgroup counter '{}'", wanted));
    }

    inline void verify(const std::filesystem::path& root) {
        struct statfs info{};
        if (::statfs(root.c_str(), &info) != 0)
            descriptor::fail("cannot inspect isolation root", root);
        if (info.f_type != 0x63677270)
            throw std::invalid_argument(std::format("isolation root '{}' is not cgroup v2", root.string()));
    }

    inline void reap(const std::filesystem::path& path) {
        write(path / "cgroup.kill", "1");
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (counter(read(path / "cgroup.events"), "populated") != 0) {
            if (std::chrono::steady_clock::now() >= until)
                throw std::runtime_error(std::format("cgroup '{}' still populated after kill (2 s)", path.string()));
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        for (auto& entry : std::filesystem::directory_iterator(path))
            if (entry.is_directory())
                reap(entry.path());
        if (::rmdir(path.c_str()) != 0)
            descriptor::fail("cannot remove cgroup", path);
    }

    class membership {
    public:
        explicit membership(const target& group) : membership(std::filesystem::path{group.identity}) {}
        auto join_fd() const noexcept -> int { return join_.get(); }
        void kill() noexcept {
            ssize_t n;
            do {
                n = ::write(kill_.get(), "1", 1);
            } while (n < 0 && errno == EINTR);
            if (n != 1) {
                constexpr char error[] = "worker: cgroup.kill failed; cleanup must be retried\n";
                ::write(2, error, sizeof(error) - 1);
            }
        }

    private:
        explicit membership(const std::filesystem::path& path)
            : join_(descriptor::open(path / "cgroup.procs", O_WRONLY)), kill_(descriptor::open(path / "cgroup.kill", O_WRONLY)) {
            verify(path);
        }
        descriptor::fd join_, kill_;
    };

} // namespace cgroup
