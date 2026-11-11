export module support;
import std;
import native;
import async;
import coroutine;
import http;
import protocol;

export namespace test {
    inline auto shell() -> std::string {
        if (const auto* value = std::getenv("TEST_SHELL")) return value;
        return "/bin/sh";
    }

    inline auto absolute(std::string_view value) -> std::string {
        return std::filesystem::absolute(std::filesystem::path{value}).generic_string();
    }

    // Keep sender constraint instantiation inside a module for clients compiled outside module units.
    struct http_stream {
        web::stream<native::socket> transport;
        explicit http_stream(native::socket& socket) : transport(socket) {}
        auto async_read(std::span<std::byte> buffer, std::stop_token stop) -> coroutine::task<std::size_t> {
            co_return co_await transport.async_read(buffer, stop);
        }
        auto async_write(std::span<const std::byte> buffer, std::stop_token stop) -> coroutine::task<std::size_t> {
            co_return co_await transport.async_write(buffer, stop);
        }
    };

    struct scratch {
        std::filesystem::path dir;
        explicit scratch(std::string_view name) : dir(std::filesystem::temp_directory_path() / std::format("test-{}-{}", name, std::random_device{}())) {
            std::filesystem::create_directories(dir);
        }
        scratch(const scratch&) = delete;
        ~scratch() {
            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        }
    };

    class source_service {
        struct state;
        std::unique_ptr<state> state_;

    public:
        explicit source_service(std::filesystem::path root);
        ~source_service();
        auto endpoint() const -> net::ip::tcp::endpoint;
    };

    inline auto toolset() -> std::map<std::string, std::string> {
        std::map<std::string, std::string> out;
        for (auto [name, variable] : {std::pair{"cc", "TEST_CC"}, std::pair{"python", "TEST_PYTHON"}, std::pair{"bazel", "TEST_BAZEL"}})
            if (auto value = std::getenv(variable); value && *value)
                out[name] = value;
        return out;
    }

    struct event {
        std::coroutine_handle<> waiter{};
        bool set = false;

        struct awaiter {
            event& e;
            auto await_ready() const noexcept -> bool { return e.set; }
            void await_suspend(std::coroutine_handle<> h) noexcept { e.waiter = h; }
            void await_resume() const noexcept {}
        };
        auto wait() noexcept -> awaiter { return {*this}; }
        void fire() {
            set = true;
            if (auto w = std::exchange(waiter, {}))
                w.resume();
        }
    };

    inline auto slurp(const std::filesystem::path& p) -> std::string {
        std::ifstream in{p, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, {}};
    }

    inline void spit(const std::filesystem::path& p, std::string_view text) {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream{p, std::ios::binary} << text;
    }
} // namespace test
