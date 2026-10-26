export module support;
import std;
import native;
import async;
import protocol;

export namespace test {
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
        explicit source_service(std::filesystem::path root, builds::toolset tools = {});
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
