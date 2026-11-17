module;
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

module posix;
import std;
import async;

namespace posix {
    namespace {
        auto sample_process(int pid, bool children) -> std::optional<io::process_usage> {
            std::ifstream file{std::format("/proc/{}/stat", pid)};
            std::string line;
            if (!std::getline(file, line))
                return {};
            auto end = line.rfind(')');
            if (end == line.npos)
                return {};
            std::istringstream fields{line.substr(end + 2)};
            std::array<std::string, 22> values;
            for (auto& value : values)
                if (!(fields >> value))
                    return {};
            auto number = [&](std::size_t index) -> std::uint64_t {
                std::uint64_t value = 0;
                auto& text = values[index];
                auto [last, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                if (error != std::errc{} || last != text.data() + text.size())
                    throw std::runtime_error("invalid process counter");
                return value;
            };
            auto ticks = number(11) + number(12);
            if (children)
                ticks += number(13) + number(14);
            auto frequency = ::sysconf(_SC_CLK_TCK);
            auto pages = ::sysconf(_SC_PAGESIZE);
            if (frequency <= 0 || pages <= 0)
                return {};
            return io::process_usage{.resident_bytes = number(21) * static_cast<std::uint64_t>(pages),
                                     .cpu_nanoseconds = ticks / frequency * 1'000'000'000 + ticks % frequency * 1'000'000'000 / frequency,
                                     .threads = static_cast<std::uint32_t>(number(17)),
                                     .processes = 1};
        }
    } // namespace
    auto process_usage(int pid, bool group) noexcept -> std::optional<io::process_usage> {
        try {
            if (!group)
                return sample_process(pid, false);
            io::process_usage total;
            for (auto& entry : std::filesystem::directory_iterator{"/proc"}) {
                auto name = entry.path().filename().string();
                int member = 0;
                auto [last, error] = std::from_chars(name.data(), name.data() + name.size(), member);
                if (error != std::errc{} || last != name.data() + name.size() || ::getpgid(member) != pid)
                    continue;
                auto sample = sample_process(member, true);
                if (!sample) {
                    if (::getpgid(member) != pid)
                        continue; // The process exited while the group was enumerated.
                    return {};
                }
                total.resident_bytes += sample->resident_bytes;
                total.cpu_nanoseconds += sample->cpu_nanoseconds;
                total.threads += sample->threads;
                total.processes += sample->processes;
            }
            return total.processes ? std::optional{total} : std::nullopt;
        } catch (...) {
            return {};
        }
    }

    namespace {
        // The xclang sysroot's headers predate these calls; numbers from 424 up
        // are the same on every Linux architecture.
        constexpr long open_pid_call = 434;
        constexpr long close_range_call = 436;
    } // namespace

    auto environment() noexcept -> char**& { return environ; }
    auto executable() -> std::filesystem::path { return std::filesystem::canonical("/proc/self/exe"); }

    auto follow_parent(int parent) noexcept -> bool { return ::prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && ::getppid() == parent; }
    auto descriptor_bound() noexcept -> int { return 0; }
    auto close_inherited(int) noexcept -> bool { return ::syscall(close_range_call, 3u, ~0u, 0) == 0; }
    // A user namespace that maps only this user lets an unprivileged process own a network namespace, which starts with
    // loopback down and no other interface. Only async-signal-safe calls run here.
    auto isolate_network() noexcept -> bool {
        auto put = [](const char* path, const char* text, std::size_t size) {
            int fd = ::open(path, O_WRONLY | O_CLOEXEC);
            if (fd < 0)
                return false;
            bool written = ::write(fd, text, size) == static_cast<ssize_t>(size);
            ::close(fd);
            return written;
        };
        auto map = [&](const char* path, unsigned id) {
            char digits[10];
            std::size_t count = 0;
            do
                digits[count++] = static_cast<char>('0' + id % 10);
            while ((id /= 10) != 0);
            char line[24];
            std::size_t size = 0;
            for (int copy = 0; copy < 2; ++copy) {
                for (auto i = count; i > 0; --i)
                    line[size++] = digits[i - 1];
                line[size++] = ' ';
            }
            line[size++] = '1';
            return put(path, line, size);
        };
        auto user = ::getuid();
        auto group = ::getgid();
        return ::unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0 && put("/proc/self/setgroups", "deny", 4) && map("/proc/self/uid_map", user) &&
               map("/proc/self/gid_map", group);
    }
    auto exit_handle(int pid) -> int {
        auto handle = static_cast<int>(::syscall(open_pid_call, pid, 0));
        if (handle < 0)
            throw std::system_error(errno, std::system_category(), "pidfd_open");
        return handle;
    }

    auto sync_filesystem(int descriptor) noexcept -> bool { return ::syncfs(descriptor) == 0; }

    // systemd's notification protocol: one datagram to NOTIFY_SOCKET, where a
    // leading '@' names an abstract socket.
    void notify(std::string_view message) {
        auto path = std::getenv("NOTIFY_SOCKET");
        if (!path || !*path)
            return;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        auto length = std::strlen(path);
        if (length >= sizeof(address.sun_path))
            throw std::runtime_error("NOTIFY_SOCKET path too long");
        std::memcpy(address.sun_path, path, length + 1);
        if (path[0] == '@')
            address.sun_path[0] = '\0';
        auto fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            throw std::runtime_error("cannot open systemd notification socket");
        auto sent = ::sendto(fd, message.data(), message.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&address),
                             sizeof address.sun_family + length + (path[0] != '@'));
        ::close(fd);
        if (sent < 0)
            throw std::runtime_error("cannot send systemd lifecycle notification");
    }
} // namespace posix
