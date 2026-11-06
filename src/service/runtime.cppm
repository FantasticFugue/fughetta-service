export module runtime;
import std;
import native;
import coroutine;
import configuration;
import reflection;
import durability;

export namespace runtime {
    // Service processes share the host scheduler; spawning a child must not inherit a single pinned CPU.
    class context : public http::io::net::basic_io_context<concurrent::queue> {
    public:
        context() = default;
        explicit context(cores::core_index) {}
    };

    class group {
        struct child {
            native::process process;
            std::string role;
            bool ready = false;
            bool exited = false;
            int status = -1;
        };
        std::optional<durability::dir_lock> lock_;
        std::vector<std::unique_ptr<child>> children_;

        static auto output(child& c) -> coroutine::task<> {
            std::array<std::byte, 4096> buffer;
            std::string pending;
            while (auto count = co_await io::async_read_some(c.process, net::buffer(buffer))) {
                std::string_view text{reinterpret_cast<const char*>(buffer.data()), count};
                std::cerr << text;
                pending.append(text);
                if (pending.contains("service-ready:" + c.role + "\n"))
                    c.ready = true;
                if (pending.size() > 8192)
                    pending.erase(0, pending.size() - 4096);
            }
        }
        static auto wait(child& c) -> coroutine::task<> {
            c.status = co_await io::async_wait(c.process);
            c.exited = true;
        }
        static auto shutdown(native::scheduler& io, child& c, coroutine::stop_token token) -> coroutine::task<> {
            std::optional<std::chrono::steady_clock::time_point> until;
            while (!c.exited) {
                if (token.stop_requested() && !until) {
                    c.process.terminate();
                    until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                }
                if (until && std::chrono::steady_clock::now() >= *until)
                    c.process.kill();
                co_await execution::schedule_after(io, std::chrono::milliseconds{25});
            }
        }
        static auto collect(native::scheduler& io, child& c, coroutine::stop_token token) -> coroutine::task<> {
            co_await execution::when_all(output(c), wait(c), shutdown(io, c, token));
        }
        // A stop asks the child to end, and the watch lasts until it has.
        static auto watch(native::scheduler& io, child& c) -> coroutine::task<> {
            auto token = co_await coroutine::current_token;
            co_await coroutine::under(coroutine::stop_token{}, collect(io, c, token));
        }
        auto launch(native::scheduler& io, coroutine::scope& tasks, io::command command, std::string role) -> child& {
            auto c = std::make_unique<child>(io::spawn_process(io, command), std::move(role));
            auto& result = *c;
            children_.push_back(std::move(c));
            tasks.spawn(watch(io, result));
            return result;
        }
        static auto ready(native::scheduler& io, child& c) -> coroutine::task<> {
            auto until = std::chrono::steady_clock::now() + std::chrono::seconds{15};
            while (!c.ready) {
                if (c.exited)
                    throw std::runtime_error(std::format("{} exited during startup: {}", c.role, c.status));
                if (std::chrono::steady_clock::now() >= until)
                    throw std::runtime_error(c.role + " startup deadline exceeded");
                co_await execution::schedule_after(io, std::chrono::milliseconds{10});
            }
        }

    public:
        // Starts the source and the workers in `tasks`, returning once each is ready; a stop of `tasks` ends them.
        auto start(native::scheduler& io, coroutine::scope& tasks, const std::filesystem::path& executable, const config::document& doc) -> coroutine::task<> {
            auto plan = allocation::distribute(doc.runtime);
            auto root = std::filesystem::absolute(doc.server.storage);
            auto directory = root / "runtime";
            lock_.emplace(directory, "service supervisor");
            std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
            auto endpoint = io::parse_endpoint(doc.server.source);
            // Managed links stay on loopback; deployment changes only the public HTTP listener.
            if (doc.server.source != std::format("127.0.0.1:{}", endpoint.port()) || doc.server.worker_bind != "127.0.0.1")
                throw std::invalid_argument("managed source and worker listeners must use 127.0.0.1");
            io::command source_command{.argv = {executable.string(), "--role", "source", "--port", std::to_string(endpoint.port()), "--storage",
                                                (root / "source").string(), "--threads", std::to_string(plan.source_disk)},
                                       .cwd = std::filesystem::current_path()};
            auto& source = launch(io, tasks, std::move(source_command), "source");
            co_await ready(io, source);
            for (std::size_t i = 0; i < plan.workers.size(); ++i) {
                config::worker worker;
                worker.gateway = std::format("127.0.0.1:{}", doc.server.worker_port);
                worker.token = doc.worker_token;
                worker.name = std::format("local-{}", i);
                worker.data_root = (root / "source").string();
                worker.work_root = (root / "workers" / worker.name).string();
                worker.dependency_cache = (root / "dependencies").string();
                worker.compute_threads = 1;
                worker.max_inflight = 1;
                worker.tools = doc.tools;
                worker.parallel = plan.workers[i].parallel;
                auto file = directory / (worker.name + ".json");
                std::ofstream out{file, std::ios::trunc};
                out << config::write(worker);
                out.close();
                if (!out)
                    throw std::runtime_error("cannot write worker configuration: " + file.string());
                auto& process =
                    launch(io, tasks, {.argv = {executable.string(), "--role", "worker", file.string()}, .cwd = std::filesystem::current_path()}, "worker");
                co_await ready(io, process);
            }
            std::cerr << std::format("service budget: {} execution threads, {} build jobs, {} workers\n", doc.runtime.threads, doc.runtime.jobs,
                                     plan.workers.size());
        }

        // Lasts until stopped, and fails as soon as a child exits unasked.
        auto supervise(native::scheduler& io) -> coroutine::task<> {
            auto token = co_await coroutine::current_token;
            while (!token.stop_requested()) {
                for (auto& c : children_)
                    if (c->exited)
                        throw std::runtime_error(std::format("{} child exited with status {}", c->role, c->status));
                co_await execution::schedule_after(io, std::chrono::milliseconds{25});
            }
        }
    };
} // namespace runtime
