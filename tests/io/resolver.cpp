import std;
import boost.ut;
import coroutine;
import native;
import resolver;
import tls;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;

    auto stalled(native::scheduler& io) -> coroutine::task<> {
        auto blackhole = native::datagram{io, net::ip::udp::endpoint{net::ip::address_v4::loopback(), 0}};
        unsigned received = 0;
        auto drain = [&]() -> coroutine::task<> {
            std::array<std::byte, 4096> bytes;
            for (;;) {
                co_await net::async_receive(blackhole, net::buffer(bytes));
                ++received;
            }
        };
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(drain());
            auto started = std::chrono::steady_clock::now();
            auto answer = co_await coroutine::deadline(
                io, 75ms, resolver::resolve(io, "stalled.example.com", 443, std::format("127.0.0.1:{}", blackhole.local_endpoint().port())));
            expect(!answer);
            expect(received > 0u);
            expect(std::chrono::steady_clock::now() - started < 500ms);
        };
        co_await coroutine::with_scope(std::move(run_children));
        auto localhost = co_await resolver::resolve(io, "localhost", 8443);
        expect(localhost.port() == 8443u);
        expect(localhost.address().to_v4().to_bytes() == std::array<std::uint8_t, 4>{127, 0, 0, 1});
    }
} // namespace

int main(int argc, const char** argv) {
    suite<"resolver"> cases = [] {
        "WSS URL validation requires a DNS hostname before TLS setup"_test = [] {
            expect(tls::address::parse("wss://localhost:443/w/acme/workers/tunnel").host == "localhost");
            expect(throws<std::invalid_argument>([] { tls::address::parse("wss://127.0.0.1/w/acme/workers/tunnel"); }));
            expect(throws<std::invalid_argument>([] { tls::address::parse("wss://user:secret@example.com/w/acme/workers/tunnel"); }));
        };
        "a nonresponsive DNS server cannot hold a connection deadline or its sockets"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            expect(cores::run(context, stalled(io)));
        };
    };
    return cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
