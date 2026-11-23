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
import process;

export namespace sibling {
    // An on-demand execute process connected through inherited standard streams.
    class manager final : public replica::execute_side {
    public:
        // `front` is the directory this process keeps; the execute role writes the one beside it.
        manager(native::scheduler& io, coroutine::scope& tasks, config::worker cfg, std::filesystem::path front)
            : io_(io), tasks_(tasks), cfg_(std::move(cfg)), front_(std::filesystem::absolute(front)), root_(front_.parent_path() / (front_.filename().string() + "-execute")) {
            tasks_.spawn(idle());
        }

        auto root() const -> const std::filesystem::path& override { return root_; }

        auto memory() const -> jobs::worker_memory override {
            if (launching_)
                return {};
            if (!current_ || current_->exited)
                return {.resident = 0, .baseline = 0};
            return current_->memory;
        }

        auto open() -> coroutine::task<replica::execute_link> override {
            auto held = co_await starting_.lock();
            if (current_ && current_->ended)
                while (!current_->exited)
                    co_await execution::schedule_after(io_, std::chrono::milliseconds{25});
            if (!current_ || current_->ended) {
                struct starting {
                    bool& flag;
                    ~starting() { flag = false; }
                } reset{launching_};
                launching_ = true;
                current_ = co_await launch();
            }
            current_->used = std::chrono::steady_clock::now();
            ++current_->holds;
            auto hold = std::shared_ptr<void>(nullptr, [running = current_](void*) {
                --running->holds;
                running->used = std::chrono::steady_clock::now();
            });
            co_return replica::execute_link{current_->chan, std::move(hold)};
        }

    private:
        struct instance {
            explicit instance(native::process p) : process(std::move(p)), stream(process), chan(stream) {}
            native::process process;
            native::stream_view stream;
            transport::channel<native::stream_view> chan;
            jobs::worker_memory memory;
            std::chrono::steady_clock::time_point used;
            std::size_t holds = 0;
            bool ended = false, exited = false;
        };

        auto environment() const -> std::map<std::string, std::string> {
            auto env = cfg_.env;
            if (cfg_.inherit_env) {
                const auto inherited = host::environment();
                for (const auto name : constants::analysis_environment)
                    if (auto found = inherited.find(std::string{name}); found != inherited.end())
                        env.try_emplace(found->first, found->second);
            }
            return env;
        }

        auto launch() -> coroutine::task<std::shared_ptr<instance>> {
            std::filesystem::create_directories(root_);
            const auto directory = front_ / "execute";
            std::filesystem::create_directories(directory);
            auto execute = cfg_;
            execute.execute_stdio = true;
            execute.token.reset();
            execute.front_token.reset();
            execute.inherit_env = false;
            execute.env = environment();
            execute.name = cfg_.name + "-execute";
            execute.data_root = root_.string();
            execute.work_root = root_.string();
            execute.roles = {config::execute_role};
            execute.backend = {};
            execute.worker_id.reset();
            execute.workspace_id.reset();
            const auto file = directory / "worker.json";
            durability::durable_private_write(file, config::write(execute));
            const auto executable = host::executable();
            auto command = backend::launch(cfg_.backend, executable, file, root_, std::move(execute.env));
            auto started = std::make_shared<instance>(io::spawn_process(io_, command));
            tasks_.spawn(watch(started));
            try {
                if (!co_await coroutine::deadline(io_, constants::execute_start, accept(*started)))
                    throw std::runtime_error(std::format("the execute role did not start in {}s", constants::execute_start.count()));
            } catch (...) {
                started->process.kill();
                throw;
            }
            tasks_.spawn(pump(started));
            co_return started;
        }

        static auto accept(instance& started) -> coroutine::task<> {
            auto first = co_await started.chan.recv(constants::worker_hello_bytes);
            if (first.kind != jobs::code<jobs::hello>)
                throw std::runtime_error("invalid execute role greeting");
            const auto hello = transport::decode<jobs::hello>(first);
            if (hello.protocol != jobs::protocol_version || hello.roles != std::vector<config::worker_role>{config::execute_role})
                throw std::runtime_error("invalid execute role greeting");
            for (;;) {
                auto report = co_await started.chan.recv(constants::worker_hello_bytes);
                if (report.kind == jobs::code<jobs::slot_report>) {
                    started.memory = transport::decode<jobs::slot_report>(report).memory;
                    co_return;
                }
            }
        }

        // Slot reports include the execute process and its separately grouped children.
        static auto pump(std::shared_ptr<instance> running) -> coroutine::task<> {
            try {
                co_await running->chan.pump([&](const transport::frame& frame) {
                    if (frame.kind == jobs::code<jobs::slot_report>)
                        running->memory = transport::decode<jobs::slot_report>(frame).memory;
                });
            } catch (...) {
            }
            running->ended = true;
            running->process.terminate();
        }

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
                    if ((token.stop_requested() || running->ended) && !until) {
                        running->process.terminate();
                        until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                    }
                    if (until && std::chrono::steady_clock::now() >= *until)
                        running->process.kill();
                    co_await execution::schedule_after(io_, std::chrono::milliseconds{25});
                }
            };
            auto collect = [&](coroutine::stop_token token) -> coroutine::task<> {
                co_await execution::when_all(wait(running), stop(running, token));
            };
            co_await coroutine::under(coroutine::stop_token{}, collect(token));
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
        coroutine::mutex starting_;
        bool launching_ = false;
        std::shared_ptr<instance> current_;
    };
} // namespace sibling
