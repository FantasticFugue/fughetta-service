#include <sqlite3.h>
import launch;
import boost.ut;
import protocol;
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
            test::child source{test::source_command(SERVICE_EXECUTABLE, tmp.dir, {"--port", std::to_string(source_port), "--storage", (tmp.dir / "source").string()})};
            test::ready(source, source_port);
            test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {file.string()})};
            test::ready(gateway, http_port);

            auto call = [&](std::string name, std::string project, std::string args, bool error = false) {
                return test::tool(http_port, std::move(name), std::format(R"({{"project":"{}",{}}})", project, args), error);
            };
            auto lock = [&](std::string project) { call("worldline", project, R"("action":"lock","scopes":["::"])"); };
            lock("app");
            lock("lib");
            call("write", "app", R"("path":"hello.txt","content":"durable source\n","operation_id":"hello")");
            call("write", "lib", R"("path":"independent.txt","content":"independent","operation_id":"independent")");
            expect(call("read", "app", R"("path":"hello.txt")").contains("durable source"));
            expect(!std::filesystem::exists(tmp.dir / "source/app/hello.txt")) << "main lives in the node repository, not in a checkout";
            expect(!std::filesystem::exists(tmp.dir / "state/app")) << "the gateway keeps no source";
            auto repository = tmp.dir / "source/.nodes/app/repository.sqlite";

            {
                writer_lock held{repository};
                auto blocked = std::async(std::launch::async, [&] { return call("write", "app", R"("path":"blocked.txt","content":"committed later","operation_id":"blocked-write")"); });
                issued("blocked.txt");
                auto began = std::chrono::steady_clock::now();
                auto other = call("read", "lib", R"("path":"independent.txt")");
                expect(other.contains("independent"));
                // A read that waited for the blocked publication would take the source's whole five-second busy wait.
                expect(std::chrono::steady_clock::now() - began < std::chrono::seconds{4}) << "another project's read waited for the blocked publication";
                expect(blocked.wait_for(std::chrono::seconds{0}) == std::future_status::timeout) << "the publication did not wait for the write lock";
                held.release();
                expect(blocked.get().contains(R"("replayed":false)"));
                expect(call("read", "app", R"("path":"blocked.txt")").contains("committed later"));
            }

            struct stored_project { std::string name; std::uint64_t bytes = 0; };
            struct stored { std::uint64_t bytes = 0; std::vector<stored_project> per_project; };
            auto usage = test::decode<stored>(test::request(http_port, http::method::GET, "/api/v1/usage").body);
            expect(usage.bytes > 0u && usage.per_project.size() == 2u) << "usage counts the node repositories";
            for (auto& p : usage.per_project)
                expect(p.bytes > 0u) << p.name;

            auto source_view = test::request(http_port, http::method::GET, "/api/v1/projects/app/source");
            struct main_version { std::string version; };
            auto main = test::decode<main_version>(call("worldline", "app", R"("action":"inspect")"));
            expect(source_view.status == 200 && source_view.body.contains("hello.txt") && source_view.body.contains(main.version))
                << "the console's source view is main: " << source_view.body;

            auto listed = test::request(http_port, http::method::POST, "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
            for (auto name : {"read", "write", "edit", "worldline", "language", "build", "test", "run", "configurations", "generate"})
                expect(listed.body.contains(std::format(R"("name":"{}")", name))) << name;
            expect(!listed.body.contains(R"("name":"receipt")"));

            auto catalog = call("generate", "app", R"("action":"catalog")");
            expect(catalog.contains("cpp-class") && catalog.contains("cpp-console"));
            auto parameters = std::string{R"("parameters":{"template_id":"cpp-class","name":"Generated","directory":"src"})"};
            auto preview = call("generate", "app", R"("action":"preview",)" + parameters);
            expect(preview.contains("src/Generated.hpp") && !preview.contains("receipt"));
            auto generated = call("generate", "app", R"("action":"apply","operation_id":"generate-class",)" + parameters);
            expect(generated.contains(R"("replayed":false)")) << generated;
            expect(call("read", "app", R"("path":"src/Generated.hpp")").contains("Generated"));
            auto replayed = call("generate", "app", R"("action":"apply","operation_id":"generate-class",)" + parameters);
            expect(replayed.contains(R"("replayed":true)")) << replayed;
            call("worldline", "app", R"("action":"unlock","scopes":["::"])");
            auto refused = call("generate", "app", R"("action":"apply","operation_id":"generate-other",)" +
                                std::string{R"("parameters":{"template_id":"cpp-class","name":"Other","directory":"src"})"}, true);
            expect(refused.contains("scope_locked")) << "generation without the global scope: " << refused;

            // The console creates a project from a template on its user's behalf.
            auto path = std::string{"/api/v1/projects/demo/generate"};
            auto project = std::string{R"({"template_id":"cpp-console","name":"demo"})"};
            auto shown = test::request(http_port, http::method::POST, path, std::format(R"({{"parameters":{},"preview":true}})", project));
            expect(shown.status == 200 && shown.body.contains("src/main.cpp")) << shown.body;
            expect(test::request(http_port, http::method::GET, "/api/v1/projects/demo").status == 404);
            auto body = std::format(R"({{"parameters":{},"preview":false,"operation_id":"create-demo","revision":0}})", project);
            auto created = test::request(http_port, http::method::POST, path, body);
            expect(created.status == 200) << created.body;
            expect(call("read", "demo", R"("path":"src/main.cpp")").contains("main"));
            auto again = test::request(http_port, http::method::POST, path, body);
            expect(again.status == 200 && again.body.contains(R"("replayed":true)")) << again.body;
            auto duplicate = test::request(http_port, http::method::POST, path,
                std::format(R"({{"parameters":{},"preview":false,"operation_id":"duplicate-demo","revision":0}})", project));
            expect(duplicate.status == 409) << duplicate.body;
            expect(call("worldline", "demo", R"("action":"locks")").contains(R"("locks":[])")) << "a console generation leaves no lock behind";
            test::require(test::save(http_port, "/api/v1/config/limits", R"({"max_file_bytes":8})").status == 200, "limits saved");
            auto oversized = test::request(http_port, http::method::POST, "/api/v1/projects/small/generate",
                R"({"parameters":{"template_id":"cpp-console","name":"small"},"preview":false,"operation_id":"small-demo","revision":0})");
            expect(oversized.status == 400 && oversized.body.contains("resource_limit")) << oversized.body;
            expect(test::request(http_port, http::method::GET, "/api/v1/projects/small").status == 404) << "a refused creation leaves no project";
            test::require(test::save(http_port, "/api/v1/config/limits", "{}").status == 200, "limits restored");


            lock("app");
            auto kept = test::request(http_port, http::method::POST, "/api/v1/projects/app/generate",
                                      R"({"parameters":{"template_id":"cpp-class","name":"Kept","directory":"src"},"preview":false,"operation_id":"kept"})");
            expect(kept.status == 200) << kept.body;
            expect(call("worldline", "app", R"("action":"locks")").contains(R"("scope":"::")")) << "the console keeps a global scope its caller already held";
            auto taken = test::request(http_port, http::method::POST, "/api/v1/projects/app/generate",
                                       R"({"parameters":{"template_id":"cpp-class","name":"Kept","directory":"src"},"preview":false,"operation_id":"kept-again"})");
            expect(taken.status == 409 && taken.body.contains("source_conflict")) << taken.body;
            expect(call("build", "app", R"("worldline":"main")", true).size() > 0u) << "a build with no worker fails";
            expect(call("write", "app", R"("path":"../escape","content":"x","operation_id":"escape")", true).contains("invalid_record"));
            expect(call("write", "app", R"("path":"anonymous.txt","content":"x")", true).contains("invalid_record")) << "a change names its operation ID";
            expect(call("read", "missing", R"("path":"x")", true).contains("missing_objects")) << "an unknown project is not opened";
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
}

int main(int argc, const char** argv) {
    boost::ut::suite<"isolation">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
