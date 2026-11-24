module;
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

export module containerd_transport;
import std;
import descriptor;

export namespace containerd_transport {
    using clock = std::chrono::steady_clock;
    [[noreturn]] void socket_error(std::string_view operation) {
        throw std::system_error(errno, std::generic_category(), std::string{operation});
    }

    class connection {
    public:
        connection(std::string_view path, clock::time_point deadline) : deadline_(deadline) {
            sockaddr_un address{};
            if (!std::filesystem::path{path}.is_absolute() || path.size() >= sizeof address.sun_path || path.contains('\0'))
                throw std::invalid_argument("containerd requires an absolute Unix socket path");
            address.sun_family = AF_UNIX;
            std::ranges::copy(path, address.sun_path);
            fd_ = descriptor::fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
            if (fd_.get() < 0)
                socket_error("containerd socket");
            if (::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) < 0) {
                if (errno != EINPROGRESS)
                    socket_error("containerd connect");
                wait(POLLOUT);
                int error = 0;
                socklen_t len = sizeof error;
                if (::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &error, &len) < 0)
                    socket_error("containerd connect status");
                if (error)
                    throw std::system_error(error, std::generic_category(), "containerd connect");
            }
        }

        void send(std::span<const std::uint8_t> bytes) const {
            while (!bytes.empty()) {
                wait(POLLOUT);
                const auto size = ::send(fd_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
                if (size > 0)
                    bytes = bytes.subspan(size);
                else if (size == 0 || (errno != EINTR && errno != EAGAIN))
                    socket_error("containerd send");
            }
        }

        auto receive(std::span<std::uint8_t> bytes) const -> std::size_t {
            for (;;) {
                wait(POLLIN);
                const auto size = ::recv(fd_.get(), bytes.data(), bytes.size(), 0);
                if (size > 0)
                    return size;
                if (size == 0)
                    throw std::runtime_error("containerd closed the connection before completing the RPC");
                if (errno != EINTR && errno != EAGAIN)
                    socket_error("containerd receive");
            }
        }

    private:
        void wait(short events) const {
            for (;;) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline_ - clock::now());
                if (remaining.count() <= 0)
                    throw std::runtime_error("containerd RPC deadline exceeded");
                pollfd entry{fd_.get(), events, 0};
                const auto result = ::poll(&entry, 1, static_cast<int>(remaining.count()));
                if (result > 0)
                    return;
                if (result < 0 && errno != EINTR)
                    socket_error("containerd poll");
            }
        }

        descriptor::fd fd_{-1};
        clock::time_point deadline_;
    };
}
