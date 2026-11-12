import std;
import boost.ut;
import coroutine;
import native;
import http;
import protocol;

namespace {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    using ctx = io::scheduler_of_t<native::context>;
    using socket = native::socket;

    auto answer(socket& s, bool reply, bool fail) -> coroutine::task<> {
        transport::channel<socket> chan{s};
        for (;;) {
            auto f = co_await chan.recv();
            if (!reply)
                continue;
            auto job = transport::decode<jobs::hash_file>(f);
            if (fail) {
                co_await chan.fail(f.corr, "missing file", "not_found");
                continue;
            }
            co_await chan.reply(f.corr, jobs::digest{.hex = job.path.value()});
        }
    }

    auto call(ctx& io, bool reply, bool fail = false) -> coroutine::task<std::optional<std::string>> {
        auto l = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        std::optional<std::string> out;
        auto s = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, l.local_endpoint().port()});
        auto peer = co_await execution::then(net::async_accept(l), [](auto socket, auto) { return std::move(socket); });
        transport::channel<socket> chan{s};
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            children.spawn(answer(peer, reply, fail));
            children.spawn(chan.pump([](const transport::frame&) {}));
            if (reply) {
                auto d = co_await chan.call(jobs::hash_file{.path = "local/a.txt"});
                if (fail) {
                    expect(!d.has_value() >> fatal);
                    auto* error = std::get_if<jobs::failure>(&d.error());
                    expect((error != nullptr) >> fatal);
                    expect(error->code == "not_found");
                    out = error->message;
                } else {
                    expect(d.has_value() >> fatal);
                    out = d->hex;
                }
            } else {
                static constexpr auto run_child = [](transport::channel<socket>& chan) -> coroutine::task<> {
                    co_await chan.call(jobs::hash_file{.path = "local/b.txt"});
                    expect(false) << "an unanswered call returned";
                };
                children.spawn(run_child(chan));
                co_await execution::schedule_after(io, 5ms);
            }
        };
        co_await coroutine::with_scope(std::move(run_children));
        co_return out;
    }

    auto encode_frame(std::uint32_t kind, std::uint64_t corr, std::span<const std::byte> payload) -> std::vector<std::byte> {
        std::vector<std::byte> bytes(transport::header_size + payload.size());
        auto length = static_cast<std::uint32_t>(payload.size());
        std::memcpy(bytes.data(), &length, 4);
        std::memcpy(bytes.data() + 4, &kind, 4);
        std::memcpy(bytes.data() + 8, &corr, 8);
        std::ranges::copy(payload, bytes.begin() + transport::header_size);
        return bytes;
    }

    auto fragmented_frames(ctx& io, std::size_t chunk) -> coroutine::task<> {
        auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto writer = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
        auto reader = co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
        auto progress = transport::encode(jobs::progress{.done = 7, .total = 11});
        auto cancel = transport::encode(jobs::cancel{});
        auto bytes = encode_frame(jobs::code<jobs::progress>, 0xfedcba9876543210ULL, progress);
        auto next = encode_frame(jobs::code<jobs::cancel>, 42, cancel);
        bytes.insert(bytes.end(), next.begin(), next.end());
        transport::channel<socket> channel{reader};
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            static constexpr auto run_child = [](socket& writer, std::vector<std::byte>& bytes, std::size_t chunk) -> coroutine::task<> {
                for (std::size_t offset = 0; offset < bytes.size(); offset += chunk)
                    co_await io::async_write(writer, std::span{bytes}.subspan(offset, std::min(chunk, bytes.size() - offset)));
            };
            children.spawn(run_child(writer, bytes, chunk));
            auto first = co_await channel.recv();
            expect(first.kind == jobs::code<jobs::progress> && first.corr == 0xfedcba9876543210ULL);
            auto value = transport::decode<jobs::progress>(first);
            expect(value.done == 7_u && value.total == 11_u);
            auto second = co_await channel.recv();
            expect(second.kind == jobs::code<jobs::cancel> && second.corr == 42_u);
            expect(std::ranges::equal(second.payload, cancel));
        };
        co_await coroutine::with_scope(std::move(run_children));
    }

    auto frame_limit(ctx& io, bool oversized) -> coroutine::task<> {
        auto listener = native::listener{io, {net::ip::address_v4::loopback(), 0}};
        auto writer = co_await io::connect(io, {net::ip::address_v4{{127, 0, 0, 1}}, listener.local_endpoint().port()});
        auto reader = co_await execution::then(net::async_accept(listener), [](auto socket, auto) { return std::move(socket); });
        auto bytes = encode_frame(jobs::code<jobs::heartbeat>, 13, {});
        auto length = static_cast<std::uint32_t>(transport::max_payload + (oversized ? 1 : 0));
        std::memcpy(bytes.data(), &length, 4);
        if (!oversized)
            bytes.resize(transport::header_size + length, std::byte{0x5a});
        transport::channel<socket> channel{reader};
        auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
            static constexpr auto run_child = [](socket& writer, std::vector<std::byte>& bytes) -> coroutine::task<> {
                co_await io::async_write(writer, bytes);
            };
            children.spawn(run_child(writer, bytes));
            bool rejected = false;
            try {
                auto frame = co_await channel.recv();
                expect(!oversized);
                expect(frame.payload.size() == transport::max_payload);
                expect(frame.corr == 13_u && frame.kind == jobs::code<jobs::heartbeat>);
                expect(std::ranges::all_of(frame.payload, [](std::byte b) { return b == std::byte{0x5a}; }));
            } catch (const std::length_error& error) {
                rejected = true;
                expect(std::string_view{error.what()} == "frame too large");
            }
            expect(rejected == oversized);
        };
        co_await coroutine::with_scope(std::move(run_children));
    }

    static constexpr auto edge_suite = [] {
        "jobs frames preserve signal kinds and correlation across arbitrary byte chunks"_test = [] {
            for (auto chunk : {1u, 7u, 16u, 4096u}) {
                native::context context;
                auto io = context.get_scheduler();
                expect(cores::run(context, fragmented_frames(io, chunk)));
            }
        };

        "jobs accepts exactly 64 MiB and rejects an oversized header without reading its body"_test = [] {
            for (bool oversized : {false, true}) {
                native::context context;
                auto io = context.get_scheduler();
                expect(cores::run(context, frame_limit(io, oversized)));
            }
        };

        "a call is answered through the pump"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            auto out = cores::run(context, call(io, true));
            expect(static_cast<bool>(out) >> fatal);
            expect(*out == std::optional<std::string>{"local/a.txt"});
        };

        "a remote failure is returned as a value"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            auto out = cores::run(context, call(io, true, true));
            expect(out.has_value() >> fatal);
            expect(*out == std::optional<std::string>{"missing file"});
        };

        "stopping the pump stops waiting calls"_test = [] {
            native::context context;
            auto io = context.get_scheduler();
            auto out = cores::run(context, call(io, false));
            expect(static_cast<bool>(out) >> fatal);
            expect(*out == std::nullopt);
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"edge">{edge_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
