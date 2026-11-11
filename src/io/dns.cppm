module;
#include <ares.h>
module resolver;
import std;
import native;
import coroutine;

namespace resolver {
    namespace {
        // c-ares runs the lookup on its own event thread; completed() is the only
        // code that thread runs here, and done publishes what it wrote.
        struct query {
            ares_channel_t* channel = nullptr;
            std::uint16_t port = 0;
            std::atomic<bool> done = false;
            int error = ARES_SUCCESS;
            std::optional<net::ip::tcp::endpoint> answer;
            ~query() {
                if (channel)
                    ares_destroy(channel);
            }
            static void completed(void* argument, int status, int, ares_addrinfo* result) noexcept {
                auto& q = *static_cast<query*>(argument);
                q.error = status;
                if (status == ARES_SUCCESS && result) {
                    for (auto* node = result->nodes; node; node = node->ai_next) {
                        if (node->ai_family != AF_INET)
                            continue;
                        std::array<std::uint8_t, 4> ip;
                        std::memcpy(ip.data(), &reinterpret_cast<const sockaddr_in*>(node->ai_addr)->sin_addr, ip.size());
                        q.answer = net::ip::tcp::endpoint{net::ip::address_v4{ip}, q.port};
                        break;
                    }
                }
                if (result)
                    ares_freeaddrinfo(result);
                q.done.store(true, std::memory_order_release);
            }
        };
    } // namespace
    auto resolve(native::scheduler& io, std::string host, std::uint16_t port, std::string servers) -> coroutine::task<net::ip::tcp::endpoint> {
        try {
            co_return io::parse_endpoint(host, port);
        } catch (const std::invalid_argument&) {
        }
        static const int initialized = ares_library_init(ARES_LIB_INIT_ALL);
        if (initialized != ARES_SUCCESS || !ares_threadsafety())
            throw std::runtime_error("cannot initialize DNS resolver");
        query q;
        q.port = port;
        ares_options options{};
        auto status = ares_init_options(&q.channel, &options, ARES_OPT_EVENT_THREAD);
        if (status != ARES_SUCCESS)
            throw std::runtime_error(std::format("cannot create DNS query ({})", ares_strerror(status)));
        if (!servers.empty() && ares_set_servers_ports_csv(q.channel, servers.c_str()) != ARES_SUCCESS)
            throw std::invalid_argument("DNS server address is invalid");
        ares_addrinfo_hints hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = ARES_AI_NOSORT;
        auto service = std::to_string(port);
        ares_getaddrinfo(q.channel, host.c_str(), service.c_str(), &hints, query::completed, &q);
        while (!q.done.load(std::memory_order_acquire))
            co_await execution::schedule_after(io, std::chrono::milliseconds{10});
        if (!q.answer)
            throw std::runtime_error(std::format("DNS lookup failed ({})", ares_strerror(q.error)));
        co_return *q.answer;
    }
} // namespace resolver
