export module backend;
import std;
import configuration;

export namespace backend {
    // A host directory the execute container sees at the same path.
    struct mount {
        std::filesystem::path path;
        bool writable = false;
    };

    inline auto container_name(std::string_view worker) -> std::string { return std::format("fughetta-{}", worker); }

    // The command that runs `executable --role worker config` as the execute role in the backend's container, with
    // `root` as its working directory and every mount at the path it has on the host.
    inline auto launch(const config::worker_backend& b, std::string_view worker, const std::filesystem::path& executable, const std::filesystem::path& config,
                       const std::filesystem::path& root, const std::vector<mount>& mounts, std::string_view account) -> std::vector<std::string> {
        const auto name = container_name(worker);
        std::vector<std::string> argv;
        auto tail = [&] {
            argv.push_back(executable.string());
            argv.push_back("--role");
            argv.push_back("worker");
            argv.push_back(config.string());
        };
        if (b.kind == "docker") {
            argv = {b.program.empty() ? "docker" : b.program, "run", "--rm", "--init", "--name", name, "--network", "host"};
            if (!b.runtime.empty())
                argv.push_back("--runtime=" + b.runtime);
            if (!account.empty())
                argv.push_back("--user=" + std::string{account});
            for (const auto& m : mounts)
                argv.push_back(std::format("--volume={0}:{0}:{1}", m.path.string(), m.writable ? "rw" : "ro"));
            argv.push_back("--workdir=" + root.string());
            argv.push_back(b.image);
            tail();
        } else if (b.kind == "containerd") {
            argv = {b.program.empty() ? "ctr" : b.program, "run", "--rm", "--net-host"};
            argv.push_back(b.runtime == "kata" ? "--runtime=io.containerd.kata.v2" : "--runtime=io.containerd.runsc.v1");
            if (!account.empty())
                argv.push_back("--user=" + std::string{account});
            for (const auto& m : mounts)
                argv.push_back(std::format("--mount=type=bind,src={0},dst={0},options=rbind:{1}", m.path.string(), m.writable ? "rw" : "ro"));
            argv.push_back("--cwd=" + root.string());
            argv.push_back(b.image);
            argv.push_back(name);
            tail();
        } else if (b.kind == "apple") {
            argv = {b.program.empty() ? "container" : b.program, "run", "--rm", "--name", name};
            if (!account.empty())
                argv.push_back("--user=" + std::string{account});
            for (const auto& m : mounts)
                argv.push_back(std::format("--mount=type=bind,source={0},target={0}{1}", m.path.string(), m.writable ? "" : ",readonly"));
            argv.push_back("--workdir=" + root.string());
            argv.push_back(b.image);
            tail();
        } else
            throw std::invalid_argument("the local worker backend starts no container");
        return argv;
    }

    // Commands that remove a container of this name a previous Worker left behind; each may fail when there is none.
    inline auto remove(const config::worker_backend& b, std::string_view worker) -> std::vector<std::vector<std::string>> {
        const auto name = container_name(worker);
        if (b.kind == "docker")
            return {{b.program.empty() ? "docker" : b.program, "rm", "--force", name}};
        if (b.kind == "containerd") {
            const auto program = b.program.empty() ? "ctr" : b.program;
            return {
                {program, "task", "kill", "--signal", "SIGKILL", name}, {program, "task", "delete", "--force", name}, {program, "container", "delete", name}};
        }
        if (b.kind == "apple") {
            const auto program = b.program.empty() ? "container" : b.program;
            return {{program, "delete", "--force", name}};
        }
        return {};
    }

    // What the execute role reads and writes: its own root and the dependency cache it fills, beside the read-only
    // executable, its configuration directory, toolchains and SDKs. Configured tools are found in the image or under a
    // listed toolchain; a tool's own directory, such as /usr/bin, would hide the image's.
    inline auto mounts(const config::worker& cfg, const std::filesystem::path& executable, const std::filesystem::path& config,
                       const std::filesystem::path& root) -> std::vector<mount> {
        std::vector<mount> out;
        std::set<std::filesystem::path> seen;
        auto add = [&](std::filesystem::path path, bool writable) {
            path = path.lexically_normal();
            if (!path.has_filename() && path.has_relative_path())
                path = path.parent_path();
            if (seen.insert(path).second)
                out.push_back({std::move(path), writable});
        };
        add(root, true);
        if (cfg.dependency_cache)
            add(*cfg.dependency_cache, true);
        if (cfg.dependency_upstream)
            add(*cfg.dependency_upstream, false);
        add(executable, false);
        add(config.parent_path(), false);
        for (const auto& path : cfg.backend.toolchains)
            add(path, false);
        for (const auto& [_, manifest] : cfg.sdk_manifests)
            for (const auto& path : manifest.roots)
                add(path, false);
        return out;
    }
} // namespace backend
