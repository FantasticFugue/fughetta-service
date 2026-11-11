module;
#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

module posix;
import std;

namespace posix {
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
