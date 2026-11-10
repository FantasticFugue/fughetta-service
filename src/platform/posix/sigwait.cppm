module;
#include <pthread.h>
#include <signal.h>
#ifdef __linux__
#include <sys/socket.h>
#include <sys/un.h>
// Keep the POSIX daemon() declaration out of the application host namespace.
#define daemon posix_daemon
#include <unistd.h>
#undef daemon
#include <cstdlib>
#include <cstring>
#endif

module signals;
import std;

namespace cores {
    struct signal_watch::state {
        sigset_t set{};
        std::atomic<bool> done{false};
        std::jthread watcher;
        state(void* owner, void (*stop)(void*) noexcept) {
            sigemptyset(&set);
            sigaddset(&set, SIGINT);
            sigaddset(&set, SIGTERM);
            ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
            watcher = std::jthread([this, owner, stop] {
                int sig = 0;
                ::sigwait(&set, &sig);
                if (!done.load())
                    stop(owner);
            });
        }
        ~state() {
            done.store(true);
            ::pthread_kill(watcher.native_handle(), SIGTERM);
        }
    };
    signal_watch::signal_watch(void* owner, void (*stop)(void*) noexcept) : state_(std::make_unique<state>(owner, stop)) {}
    signal_watch::~signal_watch() = default;
} // namespace cores

namespace daemon {
    void publish(phase value) {
        if (current.exchange(value) == value) return;
#ifdef __linux__
        auto path = ::getenv("NOTIFY_SOCKET");
        if (!path || !*path) return;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        auto length = std::strlen(path);
        if (length >= sizeof(address.sun_path))
            throw std::runtime_error("NOTIFY_SOCKET path too long");
        std::memcpy(address.sun_path, path, length + 1);
        if (path[0] == '@') address.sun_path[0] = '\0';
        std::string message = "STATUS=" + std::string{name(value)};
        if (value == phase::ready) message += "\nREADY=1";
        if (value == phase::stopping) message += "\nSTOPPING=1";
        auto fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) throw std::runtime_error("cannot open systemd notification socket");
        auto sent = ::sendto(fd, message.data(), message.size(), MSG_NOSIGNAL,
            reinterpret_cast<sockaddr*>(&address), offsetof(sockaddr_un, sun_path) + length + (path[0] != '@'));
        ::close(fd);
        if (sent < 0) throw std::runtime_error("cannot send systemd lifecycle notification");
#endif
    }
    auto run(int argc, char** argv, int (*application)(int, char**), bool, std::string_view instance) -> int {
        identity = instance;
        auto result = application(argc, argv);
        if (current.load() != phase::starting) publish(phase::stopped);
        return result;
    }
}
