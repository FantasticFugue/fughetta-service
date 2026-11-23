export module backend;
import std;
import configuration;
import async;

export namespace backend {
    inline auto launch(const config::worker_backend& limits, const std::filesystem::path& executable,
                       const std::filesystem::path& file, const std::filesystem::path& root,
                       std::map<std::string, std::string> env) -> io::command {
        return {.argv = {executable.string(), "--role", "worker", file.string()},
                .cwd = root, .env = std::move(env),
                .max_memory_bytes = limits.max_memory_bytes, .max_cpu_seconds = limits.max_cpu_seconds,
                .stdio = io::process_stdio::asynchronous};
    }
} // namespace backend
