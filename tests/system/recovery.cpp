import launch;
import boost.ut;
import protocol;
import content;

namespace {
    using namespace boost::ut;

    auto send(native::socket& peer, const transport::frame& frame) -> coroutine::task<> {
        auto length = static_cast<std::uint32_t>(frame.payload.size());
        std::vector<std::byte> bytes(transport::header_size + length);
        std::memcpy(bytes.data(), &length, 4);
        std::memcpy(bytes.data() + 4, &frame.kind, 4);
        std::memcpy(bytes.data() + 8, &frame.corr, 8);
        std::ranges::copy(frame.payload, bytes.begin() + transport::header_size);
        co_await io::async_write(peer, bytes);
    }

    static constexpr auto cases = [] {
        "a main worldline receipt recovers a lost reply across independent restarts"_test = [] {
            test::scratch tmp{"recovery"};
            auto source_port = test::port();
            auto http_port = test::port();
            std::atomic<int> mutations{0};
            std::atomic<int> dropped{0};
            test::listener proxy{[&](native::scheduler& io, native::socket peer) -> coroutine::task<> {
                try {
                    auto upstream = co_await io::connect(io, {net::ip::address_v4::loopback(), source_port});
                    transport::channel<native::socket> incoming{peer}, outgoing{upstream};
                    co_await send(upstream, co_await incoming.recv());
                    auto request = co_await incoming.recv();
                    bool drop = false;
                    if (request.kind == jobs::code<jobs::node_call>) {
                        auto call = transport::codec<jobs::node_call>::decode(request.payload);
                        drop = call.request.action == "apply" && std::ranges::any_of(call.request.edits, [](auto& e) { return e.path == "ambiguous.txt"; });
                        if (drop && ++mutations > 1)
                            drop = false;
                    }
                    co_await send(upstream, request);
                    auto response = co_await outgoing.recv();
                    if (drop) {
                        test::require(response.kind == jobs::result, "mutation did not finish before disconnect");
                        ++dropped;
                    } else {
                        co_await send(peer, response);
                    }
                } catch (const std::exception&) {
                }
            }};
            auto doc = test::document(tmp.dir, http_port, proxy.endpoint().port());
            auto file = tmp.dir / "config.json";
            test::spit(file, rfl::json::write(doc));
            auto start_source = [&] {
                auto process = std::make_unique<test::child>(
                    test::source_command(SERVICE_EXECUTABLE, tmp.dir, {"--port", std::to_string(source_port), "--storage", (tmp.dir / "source").string()}));
                test::ready(*process, source_port);
                return process;
            };
            auto source = start_source();
            auto start_gateway = [&] {
                auto process = std::make_unique<test::child>(test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {file.string()}));
                test::ready(*process, http_port);
                return process;
            };
            auto gateway = start_gateway();
            auto saved = test::save(http_port, "/api/v1/config/limits", R"({"max_projects":7})");
            test::require(saved.status == 200, saved.body);
            // Pin the observation before publishing; retry and receipt recovery use that same input.
            const auto observed = test::source_version(http_port, "app");
            const auto changes = std::format(R"("observed":"{}","operation_id":"lost-ack","changes":[{{"kind":"create","path":"ambiguous.txt","text":"committed without ack"}}])", observed);
            auto lost = test::worldline(http_port, "app", "apply", changes, true);
            expect(lost.contains("outcome_unknown")) << "a lost answer requires receipt recovery: " << lost;
            expect(mutations == 1 && dropped == 1);
            auto receipt = [&] { return test::worldline(http_port, "app", "receipt", R"("operation_id":"lost-ack")"); };
            auto original = receipt();
            struct entry {
                std::string path, digest;
                std::uint64_t size = 0;
            };
            struct outcome {
                std::string version, receipt;
                std::vector<entry> files;
            };
            auto published = test::decode<outcome>(original);
            expect(published.files.size() == 1u && published.files[0].path == "ambiguous.txt" && published.files[0].size == 21u);
            expect(published.files[0].digest == content::hash_of("committed without ack") && content::is_hash(published.version));
            auto read = [&] { return test::worldline(http_port, "app", "read", R"("path":"ambiguous.txt")"); };
            expect(read().contains(content::hash_of("committed without ack")));
            auto replayed = test::worldline(http_port, "app", "apply", changes);
            expect(replayed.contains(R"("replayed":true)")) << replayed;
            expect(source->stop() == 0);
            auto unavailable = test::worldline(http_port, "app", "apply",
                std::format(R"("observed":"{}","operation_id":"unavailable","changes":[{{"kind":"create","path":"unavailable.txt","text":"x"}}])", published.version), true);
            expect(!unavailable.empty());
            expect(!std::filesystem::exists(tmp.dir / "state/app"));
            source = start_source();
            test::eventually([&] {
                try {
                    return test::decode<outcome>(receipt()).receipt == published.receipt;
                } catch (const std::runtime_error&) {
                    return false;
                }
            });
            expect(read().contains(content::hash_of("committed without ack")));
            test::worldline(http_port, "app", "apply", std::format(
                R"("observed":"{}","operation_id":"edit","changes":[{{"kind":"edit","path":"ambiguous.txt","offset":0,"count":9,"text":"recovered"}}])", published.version));
            expect(gateway->stop() == 0);
            gateway = start_gateway();
            expect(read().contains(content::hash_of("recovered without ack")));
            expect(test::decode<outcome>(receipt()).receipt == published.receipt);
            auto limits = test::request(http_port, http::method::GET, "/api/v1/config");
            struct configuration {
                config::limits limits;
            };
            expect(limits.status == 200 && test::decode<configuration>(limits.body).limits.max_projects == 7u);
            auto builds = test::request(http_port, http::method::GET, "/api/v1/builds");
            expect(builds.status == 200 && builds.body == "[]");
            expect(dropped == 1);
            expect(gateway->stop() == 0);
            expect(source->stop() == 0);
        };
    };
} // namespace

int main(int argc, const char** argv) {
    boost::ut::suite<"recovery">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
