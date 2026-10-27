export module resolver;
import std;
import native;
import coroutine;

export namespace resolver {
    // DNS is driven on the caller's scheduler. Cancellation destroys its query
    // and sockets before returning, without a blocking resolver thread to join.
    auto resolve(native::scheduler& io, std::string host, std::uint16_t port, std::string servers = {}) -> coroutine::task<net::ip::tcp::endpoint>;
} // namespace resolver
