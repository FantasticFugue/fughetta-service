import std;
import boost.ut;
import coroutine;
import native;
import http;
import websocket;
import protocol;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    namespace ws = http::websocket;

    auto rpc(native::scheduler& io) -> coroutine::task<> {
        auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto socket = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
        auto peer = co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
        web::stream client_transport{socket};
        web::ws::stream client{client_transport, ws::side::client};
        web::stream server_transport{peer};
        web::ws::stream server{server_transport, ws::side::server};
        static_assert(io::stream<decltype(client)>);
        transport::channel outgoing{client};
        transport::channel incoming{server};
        co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
            auto answer = [&]() -> coroutine::task<> {
                auto frame = co_await incoming.recv();
                auto request = transport::decode<jobs::hash_file>(frame);
                expect(request.path.value().size() == 200000u);
                co_await incoming.post(jobs::progress{.stage = "received"}, frame.corr);
                co_await incoming.reply(frame.corr, jobs::digest{.hex = "done"});
            };
            children.spawn(answer());
            jobs::progress_log progress;
            children.spawn(outgoing.pump([](const transport::frame&) {}));
            auto result = co_await outgoing.call(jobs::hash_file{.path = std::string(200000, 'a')}, &progress);
            expect(result.has_value() >> fatal);
            expect(result->hex == "done");
            std::size_t seen = 0;
            expect(progress.since(seen).size() == 1u);
        });
    }

    auto control(native::scheduler& io) -> coroutine::task<> {
        auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto socket = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
        auto peer = co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
        web::stream server_transport{peer};
        web::ws::stream server{server_transport, ws::side::server};
        auto packet = ws::encode(ws::opcode::binary, {}, true, web::ws::random_mask());
        auto append = [&](ws::opcode kind, std::string_view text, bool final) {
            auto encoded = ws::encode(kind, std::as_bytes(std::span{text}), final, web::ws::random_mask());
            packet.insert(packet.end(), encoded.begin(), encoded.end());
        };
        append(ws::opcode::binary, "abc", false);
        append(ws::opcode::ping, "probe", true);
        append(ws::opcode::continuation, "def", true);
        auto close = ws::encode(ws::opcode::close, {}, true, web::ws::random_mask());
        packet.insert(packet.end(), close.begin(), close.end());
        co_await io::async_write(socket, packet);
        std::string collected;
        std::array<std::byte, 2> into;
        while (auto count = co_await net::async_read_some(server, net::buffer(into)))
            collected.append(reinterpret_cast<const char*>(into.data()), count);
        expect(collected == "abcdef");
        ws::decoder decoder{ws::side::client};
        std::array<std::byte, 256> response;
        bool pong = false, closed = false;
        while (!closed) {
            auto count = co_await net::async_read_some(socket, net::buffer(response));
            expect(count > 0u >> fatal);
            decoder.feed(std::span{response}.first(count));
            while (auto frame = decoder.next()) {
                if (frame->kind == ws::opcode::pong) {
                    pong = true;
                    expect(std::ranges::equal(frame->payload, std::as_bytes(std::span{"probe", 5})));
                }
                if (frame->kind == ws::opcode::close)
                    closed = true;
            }
        }
        expect(pong);
    }

    auto coalesced(native::scheduler& io) -> coroutine::task<> {
        auto listener=native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto socket=co_await io::connect(io,{net::ip::address_v4{{127,0,0,1}},listener.local_endpoint().port()});
        auto peer=co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
        web::stream server_transport{peer};
        web::ws::stream server{server_transport,ws::side::server};
        transport::channel incoming{server};
        std::vector<std::byte> grouped;
        for (std::uint64_t correlation=1;correlation<=2;++correlation) {
            auto payload=transport::codec<jobs::hash_file>::encode({.path=std::format("file-{}",correlation)});
            std::vector<std::byte> frame(transport::header_size+payload.size());
            auto length=static_cast<std::uint32_t>(payload.size());
            auto kind=jobs::code<jobs::hash_file>;
            std::memcpy(frame.data(),&length,4);
            std::memcpy(frame.data()+4,&kind,4);
            std::memcpy(frame.data()+8,&correlation,8);
            std::ranges::copy(payload,frame.begin()+transport::header_size);
            grouped.insert(grouped.end(),frame.begin(),frame.end());
        }
        auto packet=ws::encode(ws::opcode::binary,grouped,true,web::ws::random_mask());
        co_await io::async_write(socket,packet);
        for (std::uint64_t correlation=1;correlation<=2;++correlation) {
            auto frame=co_await incoming.recv();
            expect(frame.corr==correlation);
            expect(transport::decode<jobs::hash_file>(frame).path.value()==std::format("file-{}",correlation));
        }
    }

    auto slow(native::scheduler& io) -> coroutine::task<> {
        auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto socket = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
        auto peer = co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
        web::stream client_transport{socket};
        web::ws::stream client{client_transport, ws::side::client};
        std::array<std::byte, 16384> block;
        auto flood = [&]() -> coroutine::task<> {
            for (std::size_t i = 0; i < 65536; ++i)
                co_await net::async_write_some(client, net::buffer(block));
        };
        auto completed = co_await coroutine::deadline(io, 100ms, flood());
        expect(!completed) << "a peer that never consumes must apply backpressure";
    }
} // namespace

int main(int argc, const char** argv) {
    suite<"websocket"> cases = [] {
        "coalesced jobs frames share one WebSocket message"_test=[] {
            native::context io;
            auto scheduler=io.get_scheduler();
            expect(cores::run(io,coalesced(scheduler)));
        };
        "jobs frame crosses many WebSocket messages"_test = [] {
            native::context io;
            auto scheduler = io.get_scheduler();
            expect(cores::run(io, rpc(scheduler)));
        };
        "empty frames fragments ping pong and close preserve the byte stream"_test = [] {
            native::context io;
            auto scheduler = io.get_scheduler();
            expect(cores::run(io, control(scheduler)));
        };
        "slow consumer blocks bounded output and cancellation joins the write"_test = [] {
            native::context io;
            auto scheduler = io.get_scheduler();
            expect(cores::run(io, slow(scheduler)));
        };
    };
    return cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
