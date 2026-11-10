module;
#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#else
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

export module launch;
export import std;
export import support;
export import native;
export import coroutine;
export import http;
export import reflection;
export import configuration;
import keyring;
import usage;

export namespace test {
    inline void require(bool condition, std::string_view message) {
        if (!condition)
            throw std::runtime_error(std::string{message});
    }

    template <class Predicate> void eventually(Predicate predicate, std::chrono::seconds timeout = std::chrono::seconds{15}) {
        auto end = std::chrono::steady_clock::now() + timeout;
        do {
            if (predicate())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        } while (std::chrono::steady_clock::now() < end);
        throw std::runtime_error("readiness deadline exceeded");
    }

    class child {
        native::context context_;
        native::process process_;
        std::mutex mutex_;
        std::string output_;
        std::promise<int> promise_;
        std::future<int> result_ = promise_.get_future();
        std::jthread thread_;
        bool reaped_ = false;

        auto drain() -> coroutine::task<> {
            std::array<std::byte, 4096> buffer;
            while (auto count = co_await io::async_read_some(process_, net::buffer(buffer))) {
                std::lock_guard guard{mutex_};
                output_.append(reinterpret_cast<const char*>(buffer.data()), count);
            }
        }

    public:
        explicit child(io::command command) : process_(io::spawn_process(context_.get_scheduler(), command)) {
            thread_ = std::jthread([this] {
                try {
                    auto result = cores::run(context_, execution::when_all(drain(), io::async_wait(process_)));
                    if (!result)
                        throw std::runtime_error("child wait was stopped");
                    promise_.set_value(*result);
                } catch (...) {
                    promise_.set_exception(std::current_exception());
                }
            });
        }
        ~child() {
            if (!reaped_) {
                process_.kill();
                if (thread_.joinable())
                    thread_.join();
                std::cerr << output();
            }
        }
        auto output() -> std::string {
            std::lock_guard guard{mutex_};
            return output_;
        }
        auto finished() -> bool { return result_.wait_for(std::chrono::seconds{0}) == std::future_status::ready; }
        auto wait(std::chrono::seconds timeout = std::chrono::seconds{10}) -> int {
            if (result_.wait_for(timeout) != std::future_status::ready) {
                process_.kill();
                thread_.join();
                throw std::runtime_error("child exit deadline exceeded: " + output());
            }
            thread_.join();
            reaped_ = true;
            return result_.get();
        }
        auto stop() -> int {
#ifdef _WIN32
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process_.pid());
#else
            ::kill(process_.pid(), SIGTERM);
#endif
            return wait();
        }
    };

    auto port() -> std::uint16_t {
        struct lease {
            decltype(::socket(AF_INET, SOCK_DGRAM, 0)) socket;
            static auto error() -> std::error_code {
#ifdef _WIN32
                return {WSAGetLastError(), std::system_category()};
#else
                return {errno, std::generic_category()};
#endif
            }
            static auto busy(std::error_code error) -> bool {
#ifdef _WIN32
                return error.category() == std::system_category() && error.value() == WSAEADDRINUSE;
#else
                return error == std::errc::address_in_use;
#endif
            }
            explicit lease(int type = SOCK_DGRAM) : socket(::socket(AF_INET, type, 0)) {
                if (socket == decltype(socket)(-1))
                    throw std::system_error(error(), "test port lease");
            }
            ~lease() {
#ifdef _WIN32
                ::closesocket(socket);
#else
                ::close(socket);
#endif
            }
        };
        struct allocator {
            native::context context;
            std::mutex mutex;
            std::vector<std::unique_ptr<lease>> leases;
            unsigned next = 16384 + std::random_device{}() % 16384;
        };
        static allocator ports;
        std::lock_guard guard{ports.mutex};
        for (unsigned attempt = 0; attempt < 16384; ++attempt) {
            auto candidate = static_cast<std::uint16_t>(ports.next);
            ports.next = ports.next == 32767 ? 16384 : ports.next + 1;
            auto reservation = std::make_unique<lease>();
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(candidate);
            if (::bind(reservation->socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
                auto error = lease::error();
                if (lease::busy(error))
                    continue;
                throw std::system_error(error, "test port lease bind");
            }
            // Probe without reuse options so existing listeners are rejected even
            // when they enable SO_REUSEPORT. The child binds this loopback address.
            lease probe{SOCK_STREAM};
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(probe.socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || ::listen(probe.socket, 1) != 0) {
                auto error = lease::error();
                if (lease::busy(error))
                    continue;
                throw std::system_error(error, "test TCP port probe");
            }
            // Keep the UDP lease until process exit so parallel allocators cannot
            // reuse this number while the child takes ownership of its TCP port.
            ports.leases.push_back(std::move(reservation));
            return candidate;
        }
        throw std::runtime_error("test port range exhausted");
    }

    inline void ready(child& process, std::uint16_t port) {
        eventually([&] {
            require(!process.finished(), "entry exited: " + process.output());
            try {
                native::context context;
                auto peer = cores::run(context, io::connect(context.get_scheduler(), {net::ip::address_v4{{127, 0, 0, 1}}, port}));
                return peer.has_value();
            } catch (const std::system_error&) {
                return false;
            }
        });
    }

    class listener {
        cores::io_context<native::context> context_{1};
        std::jthread thread_;
        net::ip::tcp::endpoint endpoint_;

    public:
        template <class Serve>
            requires std::invocable<const Serve&, native::scheduler&, native::socket>
        explicit listener(Serve serve) {
            std::promise<net::ip::tcp::endpoint> promise;
            auto result = promise.get_future();
            thread_ = std::jthread([this, &promise, serve = std::move(serve)] {
                context_.run([&](native::scheduler& scheduler, std::size_t) -> coroutine::task<> {
                    auto acceptor = native::listener{scheduler, {net::ip::address_v4::loopback(), 0}};
                    promise.set_value({net::ip::address_v4{{127, 0, 0, 1}}, acceptor.local_endpoint().port()});
                    co_await coroutine::with_scope([&](coroutine::scope& tasks) -> coroutine::task<> {
                        for (;;)
                            tasks.spawn(
                                serve(scheduler, co_await execution::then(net::async_accept(acceptor), [](auto socket, auto) { return std::move(socket); })));
                    });
                });
            });
            endpoint_ = result.get();
        }
        ~listener() {
            context_.stop();
            thread_.join();
        }
        auto endpoint() const -> net::ip::tcp::endpoint { return endpoint_; }
    };

    struct response {
        int status;
        std::string body;
    };
    auto request(std::uint16_t port, http::method method, std::string path, std::string body = {}, std::string token = "sk_test") -> response {
        native::context context;
        auto run = [&]() -> coroutine::task<response> {
            auto peer = co_await io::connect(context.get_scheduler(), {net::ip::address_v4{{127, 0, 0, 1}}, port});
            http_stream stream{peer};
            http::v1::client client{stream};
            http::request head{
                .method = method, .target = std::move(path), .scheme = "http", .authority = "localhost", .fields = {{"content-type", "application/json"}}};
            if (!token.empty())
                head.fields.push_back({"authorization", "Bearer " + token});
            auto result = co_await http::coroutine::fetch(client, std::move(head), std::string_view{body});
            co_return response{result.head.status.code(), std::string{result.text()}};
        };
        auto scheduler = context.get_scheduler();
        auto result = cores::run(context, coroutine::deadline(scheduler, std::chrono::seconds{30}, run()));
        require(result && result->has_value(), "HTTP deadline exceeded");
        return std::move(**result);
    }

    template <class T> auto decode(std::string_view text) -> T {
        auto parsed = rfl::json::read<T, rfl::DefaultIfMissing>(std::string{text});
        require(bool(parsed), "invalid response: " + std::string{text});
        return std::move(*parsed);
    }
    inline auto save(std::uint16_t port, std::string path, std::string value) -> response {
        auto current = request(port, http::method::GET, path);
        require(current.status == 200, current.body);
        struct revision {
            std::uint64_t revision = 0;
        };
        return request(port, http::method::PUT, path, std::format(R"({{"revision":{},"value":{}}})", decode<revision>(current.body).revision, value));
    }
    inline auto tool(std::uint16_t port, std::string name, std::string args, bool error = false, std::string credential = "sk_test") -> std::string {
        if (credential == "sk_test" && (name == "worldline" || name == "language" || name == "scope" || name == "build" || name == "test" || name == "run")) {
            auto object = decode<rfl::Generic>(args).to_object().value();
            if (!object.get("worldline")) object["worldline"] = "main";
            args = rfl::json::write(object);
        }
        auto response = request(port, http::method::POST, "/mcp",
                                std::format(R"({{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{{"name":"{}","arguments":{}}}}})", name, args), credential);
        require(response.status == 200, response.body);
        struct payload {
            bool isError = false;
            rfl::Generic structuredContent;
        };
        struct reply {
            std::optional<payload> result;
        };
        auto parsed = decode<reply>(response.body);
        require(parsed.result && parsed.result->isError == error, response.body);
        return error ? response.body : rfl::json::write(parsed.result->structuredContent);
    }
    inline constexpr std::string_view project = "/api/v1/projects/app";
    inline auto worldline(std::uint16_t port, std::string project, std::string action, std::string fields = {}, bool error = false) -> std::string {
        if (!fields.contains("\"worldline\"")) { if (!fields.empty()) fields += ","; fields += "\"worldline\":\"main\""; }
        auto response = request(port, http::method::POST, "/api/v1/projects/" + project + "/worldline",
                                std::format(R"({{"project":{},"action":{}{}}})", rfl::json::write(project), rfl::json::write(action),
                                            fields.empty() ? "" : "," + fields));
        require(error ? response.status >= 400 : response.status == 200, response.body);
        return response.body;
    }
    inline auto source_version(std::uint16_t port, const std::string& project) -> std::string {
        struct revision { std::string version; };
        return decode<revision>(worldline(port, project, "inspect")).version;
    }
    inline auto document(const std::filesystem::path& root, std::uint16_t http, std::uint16_t source) -> config::document {
        config::document doc;
        doc.server.http_port = http;
        doc.server.worker_port = port();
        doc.server.source = std::format("127.0.0.1:{}", source);
        doc.server.storage = (root / "state").string();
        doc.server.auth.tokens = {{.name = "tester", .hash = keyring::hash("sk_test")}};
        doc.projects["app"] = {};
        return doc;
    }
    inline auto command(std::filesystem::path binary, std::filesystem::path root, std::vector<std::string> args = {}) -> io::command {
        args.insert(args.begin(), binary.string());
        return {.argv = std::move(args), .cwd = std::move(root)};
    }
    inline auto role_command(std::filesystem::path binary, std::filesystem::path root, std::string role, std::vector<std::string> args = {}) -> io::command {
        args.insert(args.begin(), {"--role", std::move(role)});
        return command(std::move(binary), std::move(root), std::move(args));
    }
    inline auto gateway_command(std::filesystem::path binary, std::filesystem::path root, std::vector<std::string> args = {}) -> io::command {
        return role_command(std::move(binary), std::move(root), "gateway", std::move(args));
    }
    inline auto source_command(std::filesystem::path binary, std::filesystem::path root, std::vector<std::string> args = {}) -> io::command {
        return role_command(std::move(binary), std::move(root), "source", std::move(args));
    }
    inline auto worker_command(std::filesystem::path binary, std::filesystem::path root, std::vector<std::string> args = {}) -> io::command {
        return role_command(std::move(binary), std::move(root), "worker", std::move(args));
    }
} // namespace test
