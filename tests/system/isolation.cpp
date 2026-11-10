#include <sqlite3.h>
import launch;
import boost.ut;
import protocol;
import content;
import usage;

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

    // Holds the repository's write lock the way another process would, so a publication waits for it.
    class writer_lock {
        sqlite3* db_ = nullptr;

    public:
        explicit writer_lock(const std::filesystem::path& file) {
            test::require(sqlite3_open_v2(file.string().c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "cannot open the repository");
            sqlite3_busy_timeout(db_, 5000);
            test::require(sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK, "cannot take the repository write lock");
        }
        void release() {
            if (db_) {
                sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                sqlite3_close(db_);
                db_ = nullptr;
            }
        }
        ~writer_lock() { release(); }
    };

    // The first frame a source accepts, from a caller that knows the token and speaks `protocol`.
    auto hello(std::string auth, std::uint32_t protocol) -> transport::frame {
        return {jobs::code<jobs::hello>, 0, transport::encode(jobs::hello{.name = "isolation", .auth = std::move(auth), .protocol = protocol})};
    }
    // Opens a source connection, sends `first`, then whatever `then` returns, and gives back the source's first reply,
    // or nothing when it closed the connection.
    auto exchange(std::uint16_t port, transport::frame first, std::optional<transport::frame> then = {}) -> std::optional<transport::frame> {
        native::context context;
        auto run = [&]() -> coroutine::task<std::optional<transport::frame>> {
            auto peer = co_await io::connect(context.get_scheduler(), {net::ip::address_v4::loopback(), port});
            transport::channel<native::socket> channel{peer};
            co_await send(peer, first);
            if (then)
                co_await send(peer, *then);
            try {
                co_return co_await channel.recv();
            } catch (const std::exception&) {
                co_return std::nullopt;
            }
        };
        auto scheduler = context.get_scheduler();
        auto result = cores::run(context, coroutine::deadline(scheduler, std::chrono::seconds{10}, run()));
        test::require(result && result->has_value(), "source exchange deadline exceeded");
        return std::move(**result);
    }
    auto token() -> std::string { return jobs::source_token(); }
    static constexpr auto cases = [] {
        "projects and entries stay isolated through real gateway and source processes"_test = [] {
            test::scratch tmp{"isolation"};
            auto source_port = test::port();
            auto http_port = test::port();
            std::mutex mutex;
            std::condition_variable forwarded;
            std::set<std::string> applied;
            // Notes each apply the gateway sends, once the source has it.
            test::listener proxy{[&](native::scheduler& io, native::socket peer) -> coroutine::task<> {
                try {
                    auto upstream = co_await io::connect(io, {net::ip::address_v4::loopback(), source_port});
                    transport::channel<native::socket> incoming{peer}, outgoing{upstream};
                    co_await send(upstream, co_await incoming.recv());
                    auto request = co_await incoming.recv();
                    co_await send(upstream, request);
                    if (request.kind == jobs::code<jobs::node_call>) {
                        auto call = transport::codec<jobs::node_call>::decode(request.payload);
                        std::lock_guard guard{mutex};
                        applied.insert(call.project + " " + call.request.action);
                        for (const auto& e : call.request.edits)
                            applied.insert(e.path);
                        forwarded.notify_all();
                    }
                    co_await send(peer, co_await outgoing.recv());
                } catch (const std::exception&) {
                }
            }};
            auto issued = [&](const std::string& path) {
                std::unique_lock guard{mutex};
                test::require(forwarded.wait_for(guard, std::chrono::seconds{5}, [&] { return applied.contains(path); }), path + " was not sent to the source");
            };
            auto doc = test::document(tmp.dir, http_port, proxy.endpoint().port());
            doc.projects["lib"] = {};
            auto file = tmp.dir / "config.json";
            test::spit(file, rfl::json::write(doc));
            test::child source{
                test::source_command(SERVICE_EXECUTABLE, tmp.dir, {"--port", std::to_string(source_port), "--storage", (tmp.dir / "source").string()})};
            test::ready(source, source_port);
            test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {file.string()})};
            test::ready(gateway, http_port);

            auto source_call = [&](std::string project, std::string action, std::string fields = {}, bool error = false) {
                return test::worldline(http_port, std::move(project), std::move(action), std::move(fields), error);
            };
            auto create = [&](std::string project, std::string path, std::string text, std::string operation, std::string observed) {
                return source_call(project, "apply", std::format(
                    R"("observed":{},"operation_id":{},"changes":[{{"kind":"create","path":{},"text":{}}}])",
                    rfl::json::write(observed), rfl::json::write(operation), rfl::json::write(path), rfl::json::write(text)));
            };
            create("app", "hello.txt", "durable source\n", "hello", test::source_version(http_port, "app"));
            create("lib", "independent.txt", "independent", "independent", test::source_version(http_port, "lib"));
            expect(source_call("app", "read", R"("path":"hello.txt")").contains(content::hash_of("durable source\n")));
            expect(!std::filesystem::exists(tmp.dir / "source/app/hello.txt")) << "main lives in the node repository, not in a checkout";
            expect(!std::filesystem::exists(tmp.dir / "state/app")) << "the gateway keeps no source";
            auto repository = tmp.dir / "source/.nodes/app/repository.sqlite";

            {
                const auto observed = test::source_version(http_port, "app");
                writer_lock held{repository};
                auto blocked = std::async(std::launch::async, [&] {
                    return create("app", "blocked.txt", "committed later", "blocked-write", observed);
                });
                issued("blocked.txt");
                auto began = std::chrono::steady_clock::now();
                auto other = source_call("lib", "read", R"("path":"independent.txt")");
                expect(other.contains(content::hash_of("independent")));
                // A read that waited for the blocked publication would take the source's whole five-second busy wait.
                expect(std::chrono::steady_clock::now() - began < std::chrono::seconds{4}) << "another project's read waited for the blocked publication";
                expect(blocked.wait_for(std::chrono::seconds{0}) == std::future_status::timeout) << "the publication did not wait for the write lock";
                held.release();
                expect(blocked.get().contains(R"("replayed":false)"));
                expect(source_call("app", "read", R"("path":"blocked.txt")").contains(content::hash_of("committed later")));
            }

            struct stored_project {
                std::string name;
                std::uint64_t bytes = 0;
            };
            struct stored {
                std::uint64_t bytes = 0;
                std::vector<stored_project> per_project;
            };
            auto usage = test::decode<stored>(test::request(http_port, http::method::GET, "/api/v1/usage").body);
            expect(usage.bytes > 0u && usage.per_project.size() == 2u) << "usage counts the node repositories";
            for (auto& p : usage.per_project)
                expect(p.bytes > 0u) << p.name;

            auto source_view = test::request(http_port, http::method::GET, "/api/v1/projects/app/source");
            struct main_version {
                std::string version;
            };
            auto main = test::decode<main_version>(source_call("app", "inspect"));
            expect(source_view.status == 200 && source_view.body.contains("hello.txt") && source_view.body.contains(main.version))
                << "the console's source view is main: " << source_view.body;

            auto listed = test::request(http_port, http::method::POST, "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
            for (auto name : {"language", "build", "test", "run", "configurations", "worldline"})
                expect(listed.body.contains(std::format(R"("name":"{}")", name))) << name;
            for (auto name : {"read", "write", "edit", "generate", "receipt", "jobs"})
                expect(!listed.body.contains(std::format(R"("name":"{}")", name))) << name;
            auto offered = test::request(http_port, http::method::GET, "/api/v1/templates");
            expect(offered.status == 200 && offered.body.contains("cpp-class") && !offered.body.contains("console")) << offered.body;

            // Projects come from their settings, and generation only writes files into a project that already exists.
            auto created = test::request(http_port, http::method::PUT, "/api/v1/projects/demo", R"({"revision":0,"value":{}})");
            test::require(created.status == 200, "project settings saved: " + created.body);
            auto path = std::string{"/api/v1/projects/demo/generate"};
            auto refused_template = test::request(http_port, http::method::POST, path,
                                                  R"({"parameters":{"template_id":"cpp-console","name":"demo"},"preview":false,"operation_id":"create-demo"})");
            expect(refused_template.status == 400 && refused_template.body.contains("invalid_request")) << refused_template.body;
            auto shown = test::request(http_port, http::method::POST, path,
                                       R"({"parameters":{"template_id":"cpp-class","name":"Shown","directory":"src"},"preview":true})");
            expect(shown.status == 200 && shown.body.contains("src/Shown.hpp")) << shown.body;
            auto body = R"({"parameters":{"template_id":"cpp-class","name":"Made","directory":"src"},"preview":false,"operation_id":"make-class"})";
            auto made = test::request(http_port, http::method::POST, path, body);
            expect(made.status == 200) << made.body;
            expect(source_call("demo", "read", R"("path":"src/Made.hpp")").contains("Made"));
            auto again = test::request(http_port, http::method::POST, path, body);
            expect(again.status == 200 && again.body.contains(R"("replayed":true)")) << again.body;
            test::require(test::request(http_port, http::method::PUT, "/api/v1/projects/small", R"({"revision":0,"value":{}})").status == 200,
                          "small project saved");
            test::require(test::save(http_port, "/api/v1/config/limits", R"({"max_file_bytes":8})").status == 200, "limits saved");
            auto oversized =
                test::request(http_port, http::method::POST, "/api/v1/projects/small/generate",
                              R"({"parameters":{"template_id":"cpp-class","name":"Small","directory":"src"},"preview":false,"operation_id":"small-class"})");
            expect(oversized.status == 400 && oversized.body.contains("resource_limit")) << oversized.body;
            test::require(test::save(http_port, "/api/v1/config/limits", "{}").status == 200, "limits restored");

            auto kept = test::request(http_port, http::method::POST, "/api/v1/projects/app/generate",
                                      R"({"parameters":{"template_id":"cpp-class","name":"Kept","directory":"src"},"preview":false,"operation_id":"kept"})");
            expect(kept.status == 200) << kept.body;
            auto taken =
                test::request(http_port, http::method::POST, "/api/v1/projects/app/generate",
                              R"({"parameters":{"template_id":"cpp-class","name":"Kept","directory":"src"},"preview":false,"operation_id":"kept-again"})");
            expect(taken.status == 409 && taken.body.contains("source_conflict")) << taken.body;
            expect(!test::tool(http_port, "build", R"({"project":"app","worldline":"main"})", true).empty()) << "a build with no worker fails";
            auto observed = test::source_version(http_port, "app");
            expect(source_call("app", "apply", std::format(
                R"("observed":"{}","operation_id":"escape","changes":[{{"kind":"create","path":"../escape","text":"x"}}])", observed), true).contains("invalid_record"));
            expect(source_call("app", "apply", std::format(
                R"("observed":"{}","changes":[{{"kind":"create","path":"anonymous.txt","text":"x"}}])", observed), true).contains("invalid_record")) << "a change names its operation ID";
            expect(source_call("missing", "read", R"("path":"x")", true).contains("not_found")) << "an unknown project is not opened";
            expect(!std::filesystem::exists(tmp.dir / "source/.nodes/missing"));
            expect(!std::filesystem::exists(tmp.dir / "source/escape"));

            expect(!exchange(source_port, hello("wrong-secret", jobs::protocol_version))) << "an unauthenticated caller is closed without an answer";
            auto mismatch = exchange(source_port, hello(token(), jobs::protocol_version + 1));
            expect(mismatch && mismatch->kind == jobs::failure_code && mismatch->corr == 0u);
            if (mismatch)
                expect(transport::codec<jobs::failure>::decode(mismatch->payload).message.contains("protocol mismatch"));

            expect(gateway.stop() == 0);
            expect(source.stop() == 0);
        };
    };
} // namespace

int main(int argc, const char** argv) {
    boost::ut::suite<"isolation">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
