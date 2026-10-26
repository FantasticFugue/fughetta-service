module;
#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <poll.h>
#endif
#include <ares.h>
module resolver;
import std;
import native;
import coroutine;

namespace resolver {
namespace {
struct query {
    ares_channel_t *channel = nullptr;
    std::map<ares_socket_t, std::pair<bool, bool>> sockets;
    bool done = false;
    int error = ARES_SUCCESS;
    std::optional<net::ip::tcp::endpoint> answer;
    std::exception_ptr failed;
    ~query() {
        if (channel)
            ares_destroy(channel);
    }
    static void socket_state(void *argument, ares_socket_t socket, int readable, int writable) noexcept {
        auto &q = *static_cast<query *>(argument);
        try {
            if (!readable && !writable)
                q.sockets.erase(socket);
            else
                q.sockets[socket] = {readable != 0, writable != 0};
        } catch (...) {
            q.failed = std::current_exception();
        }
    }
    static void completed(void *argument, int status, int, ares_addrinfo *result) noexcept {
        auto &q = *static_cast<query *>(argument);
        q.done = true;
        q.error = status;
        if (status == ARES_SUCCESS && result) {
            for (auto *node = result->nodes; node; node = node->ai_next) {
                if (node->ai_family != AF_INET)
                    continue;
                auto *address = reinterpret_cast<const sockaddr_in *>(node->ai_addr);
                std::array<std::uint8_t, 4> ip;
                std::memcpy(ip.data(), &address->sin_addr, ip.size());
                q.answer = net::ip::tcp::endpoint{net::ip::address_v4{ip}, ntohs(address->sin_port)};
                break;
            }
        }
        if (result)
            ares_freeaddrinfo(result);
    }
    void tick() {
#ifdef _WIN32
        using descriptor = WSAPOLLFD;
#else
        using descriptor = pollfd;
#endif
        std::vector<descriptor> descriptors;
        for (const auto &[socket, wanted] : sockets)
            descriptors.push_back({socket, static_cast<short>((wanted.first ? POLLIN : 0) | (wanted.second ? POLLOUT : 0)), 0});
#ifdef _WIN32
        auto ready = descriptors.empty() ? 0 : WSAPoll(descriptors.data(), static_cast<ULONG>(descriptors.size()), 0);
#else
        auto ready = ::poll(descriptors.data(), descriptors.size(), 0);
#endif
        if (ready < 0)
            throw std::runtime_error("DNS socket polling failed");
        std::vector<ares_fd_events_t> events;
        for (const auto &descriptor : descriptors) {
            unsigned flags = 0;
            if (descriptor.revents & (POLLIN | POLLHUP | POLLERR))
                flags |= ARES_FD_EVENT_READ;
            if (descriptor.revents & (POLLOUT | POLLHUP | POLLERR))
                flags |= ARES_FD_EVENT_WRITE;
            if (descriptor.revents & POLLNVAL)
                throw std::runtime_error("DNS socket was closed unexpectedly");
            if (flags)
                events.push_back({descriptor.fd, flags});
        }
        if (auto status = ares_process_fds(channel, events.data(), events.size(), ARES_PROCESS_FLAG_NONE); status != ARES_SUCCESS)
            throw std::runtime_error(std::format("DNS processing failed ({})", ares_strerror(status)));
    }
};
} // namespace
auto resolve(native::scheduler &io, std::string host, std::uint16_t port, std::string servers) -> coroutine::task<net::ip::tcp::endpoint> {
    try {
        co_return io::parse_endpoint(host, port);
    } catch (const std::invalid_argument &) {
    }
    static const int initialized = ares_library_init(ARES_LIB_INIT_ALL);
    if (initialized != ARES_SUCCESS)
        throw std::runtime_error("cannot initialize DNS resolver");
    query q;
    ares_options options{};
    options.sock_state_cb = query::socket_state;
    options.sock_state_cb_data = &q;
    auto status = ares_init_options(&q.channel, &options, ARES_OPT_SOCK_STATE_CB);
    if (status != ARES_SUCCESS)
        throw std::runtime_error("cannot create DNS query");
    if (!servers.empty() && ares_set_servers_ports_csv(q.channel, servers.c_str()) != ARES_SUCCESS)
        throw std::invalid_argument("DNS server address is invalid");
    ares_addrinfo_hints hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = ARES_AI_NOSORT;
    auto service = std::to_string(port);
    ares_getaddrinfo(q.channel, host.c_str(), service.c_str(), &hints, query::completed, &q);
    while (!q.done) {
        if (q.failed)
            std::rethrow_exception(q.failed);
        q.tick();
        if (!q.done)
            co_await execution::schedule_after(io, std::chrono::milliseconds{10});
    }
    if (q.failed)
        std::rethrow_exception(q.failed);
    if (!q.answer)
        throw std::runtime_error(std::format("DNS lookup failed ({})", ares_strerror(q.error)));
    co_return *q.answer;
}
} // namespace resolver
