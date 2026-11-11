export module resolver;
import std;
import native;
import coroutine;

export namespace resolver {
    // c-ares runs the lookup on its event thread while the caller waits on its
    // scheduler. Cancellation destroys the query's channel, which stops that
    // thread before returning.
    auto resolve(native::scheduler& io, std::string host, std::uint16_t port, std::string servers = {}) -> coroutine::task<net::ip::tcp::endpoint>;
} // namespace resolver
