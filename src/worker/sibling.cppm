export module sibling;
import std;
import native;
import coroutine;
import protocol;
import configuration;
import executor;
import backend;
import host;
import durability;
import codec;
import constants;

export namespace sibling {
    // The execute role of a Worker whose backend is a container: started when a call needs it, dialing back to a
    // listener of this process, and stopped once idle. Analyses that run no project code stay in this process.
    class manager final : public replica::execute_side {
    public:
        // `front` is the directory this process keeps; the execute role writes the one beside it.
        manager(native::scheduler& io, coroutine::scope& tasks, config::worker cfg, std::filesystem::path front)
            : io_(io), tasks_(tasks), cfg_(std::move(cfg)), front_(std::move(front)), root_(front_.parent_path() / (front_.filename().string() + "-execute")),
              listener_(io, net::ip::tcp::endpoint{net::ip::make_address(cfg_.backend.host), 0}) {
            tasks_.spawn(idle());
        }

        auto root() const -> const std::filesystem::path& override { return root_; }

        auto open() -> coroutine::task<replica::execute_link> override {
            auto held = co_await starting_.lock();
            if (!current_ || current_->ended)
                current_ = co_await launch();
            current_->used = std::chrono::steady_clock::now();
            ++current_->holds;
            auto hold = std::shared_ptr<void>(nullptr, [running = current_](void*) {
                --running->holds;
                running->used = std::chrono::steady_clock::now();
            });
            co_return replica::execute_link{*current_->chan, std::move(hold)};
        }

    private:
        struct instance {
            explicit instance(native::process p) : process(std::move(p)) {}
            native::process process;
            std::optional<native::socket> sock;
            std::optional<transport::channel<native::socket>> chan;
            std::chrono::steady_clock::time_point used;
            std::size_t holds = 0;
            bool ended = false, exited = false;
        };

        auto launch() -> coroutine::task<std::shared_ptr<instance>> {
            for (auto& argv : backend::remove(cfg_.backend, cfg_.name))
                co_await quiet(std::move(argv));
            std::filesystem::create_directories(root_);
            const auto directory = front_ / "execute";
            std::filesystem::create_directories(directory);
            const auto token = nodes::hex(nodes::random_bytes(32));
            auto execute = cfg_;
            execute.gateway = std::format("{}:{}", cfg_.backend.host, listener_.local_endpoint().port());
            execute.token = token;
            execute.name = cfg_.name + "-execute";
            execute.data_root = root_.string();
            execute.work_root = root_.string();
            execute.roles = {std::string{constants::execute_role}};
            execute.backend = {};
            execute.worker_id.reset();
            execute.workspace_id.reset();
            const auto file = directory / "worker.json";
            durability::durable_private_write(file, config::write(execute));
            const auto executable = host::executable();
            auto argv = backend::launch(cfg_.backend, cfg_.name, executable, file, root_, backend::mounts(cfg_, executable, file, root_), host::account());
            auto started = std::make_shared<instance>(io::spawn_process(io_, {.argv = std::move(argv), .cwd = root_}));
            tasks_.spawn(watch(started));
            auto accepted = co_await coroutine::deadline(io_, constants::execute_start, accept(*started, token));
            if (!accepted) {
                started->process.terminate();
                throw std::runtime_error(std::format("the execute role did not start in {}s", constants::execute_start.count()));
            }
            tasks_.spawn(pump(started));
            co_return started;
        }

        auto accept(instance& started, std::string token) -> coroutine::task<> {
            for (;;) {
                auto [connection, peer] = co_await net::async_accept(listener_);
                started.sock.emplace(std::move(connection));
                started.chan.emplace(*started.sock);
                try {
                    auto first = co_await coroutine::deadline(io_, std::chrono::seconds{10}, started.chan->recv(constants::worker_hello_bytes));
                    if (first && first->kind == jobs::code<jobs::hello>) {
                        auto hello = transport::decode<jobs::hello>(*first);
                        if (hello.auth == token && hello.roles == std::vector<std::string>{std::string{constants::execute_role}})
                            co_return;
                    }
                } catch (const std::exception&) {
                }
                started.chan.reset();
                started.sock.reset();
            }
        }

        // Heartbeats and slot reports of the execute role mean nothing to this process.
        static auto pump(std::shared_ptr<instance> running) -> coroutine::task<> {
            try {
                co_await running->chan->pump([](const transport::frame&) {});
            } catch (...) {
            }
            running->ended = true;
            running->process.terminate();
        }

        static auto output(native::process& process) -> coroutine::task<> {
            std::array<std::byte, 4096> buffer;
            while (auto count = co_await io::async_read_some(process, net::buffer(buffer)))
                std::cerr << std::string_view{reinterpret_cast<const char*>(buffer.data()), count};
        }

        // The container ends with this process: a stop terminates it and waits for it. A client killed after the grace
        // period may leave the container behind, so a stop also removes it; nothing starts another by then.
        auto watch(std::shared_ptr<instance> running) -> coroutine::task<> {
            auto token = co_await coroutine::current_token;
            auto wait = [](std::shared_ptr<instance> running) -> coroutine::task<> {
                co_await io::async_wait(running->process);
                running->exited = true;
                running->ended = true;
            };
            auto stop = [this](std::shared_ptr<instance> running, coroutine::stop_token token) -> coroutine::task<> {
                std::optional<std::chrono::steady_clock::time_point> until;
                while (!running->exited) {
                    if (token.stop_requested() && !until) {
                        running->process.terminate();
                        until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                    }
                    if (until && std::chrono::steady_clock::now() >= *until)
                        running->process.kill();
                    co_await execution::schedule_after(io_, std::chrono::milliseconds{25});
                }
            };
            auto collect = [&](coroutine::stop_token token) -> coroutine::task<> {
                co_await execution::when_all(output(running->process), wait(running), stop(running, token));
            };
            co_await coroutine::under(coroutine::stop_token{}, collect(token));
            if (token.stop_requested())
                for (auto& argv : backend::remove(cfg_.backend, cfg_.name))
                    co_await coroutine::under(coroutine::stop_token{}, quiet(std::move(argv)));
        }

        auto quiet(std::vector<std::string> argv) -> coroutine::task<> {
            try {
                auto process = io::spawn_process(io_, {.argv = std::move(argv), .cwd = front_});
                auto drain = [](native::process& process) -> coroutine::task<> {
                    std::array<std::byte, 4096> buffer;
                    while (co_await io::async_read_some(process, net::buffer(buffer))) {
                    }
                };
                auto wait = [](native::process& process) -> coroutine::task<> { co_await io::async_wait(process); };
                co_await execution::when_all(drain(process), wait(process));
            } catch (const std::exception&) {
            }
        }

        // An execute role nobody has called for a while stops; the next call starts it again.
        auto idle() -> coroutine::task<> {
            for (;;) {
                co_await execution::schedule_after(io_, std::chrono::seconds{30});
                auto held = co_await starting_.lock();
                if (current_ && !current_->ended && !current_->holds && std::chrono::steady_clock::now() - current_->used >= constants::execute_idle) {
                    // The next call starts another instead of reaching this one while it exits.
                    current_->ended = true;
                    current_->process.terminate();
                }
            }
        }

        native::scheduler& io_;
        coroutine::scope& tasks_;
        config::worker cfg_;
        std::filesystem::path front_, root_;
        native::listener listener_;
        coroutine::mutex starting_;
        std::shared_ptr<instance> current_;
    };
} // namespace sibling
