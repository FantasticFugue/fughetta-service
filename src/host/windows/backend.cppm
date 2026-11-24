module;
#include <windows.h>
#include <virtdisk.h>
#include <sddl.h>

export module host_backend;
import std;
import async;
import configuration;
import constants;
import containerd;
import coroutine;
import durability;
import host_operation;
import host_windows_network;
import host_windows_job;
import json;
import keyring;
import native;
import winapi;

namespace host_backend {
    namespace fs = std::filesystem;
    using namespace host_operation;

    struct record {
        std::string workspace, root, socket, name_space;
        host_windows_network::endpoint network;
        bool process = false;
        std::string disk;
        bool data_ready = false;
    };

    auto settings(const config::host_settings& host, const operation& op, const fs::path& storage,
                  const std::string& address, const std::string& home,
                  const std::string& loopback = "127.0.0.1") -> std::string {
        const auto token_file = storage / "credentials" / (op.workspace_id + ".json");
        durability::durable_directories(token_file.parent_path());
        if (!fs::exists(token_file))
            durability::durable_private_write(token_file, config::write(keyring::mint()));
        const auto token = read<std::string>(token_file);
        if (op.kind == environment_kind::overflow) {
            config::worker worker;
            worker.name = op.environment_id;
            worker.gateway = op.core_address;
            worker.token = token;
            worker.data_root = home + "\\hot";
            worker.work_root = home + "\\trees";
            worker.dependency_cache = home + "\\dependencies";
            worker.tools = host.tools;
            worker.compute_threads = op.contract.runtime.threads;
            worker.parallel = op.contract.runtime.jobs;
            worker.memory_bytes = op.contract.runtime.worker_memory_bytes;
            return config::write(worker);
        }
        config::document core;
        core.runtime = op.contract.runtime;
        core.granted = op.granted;
        core.tools = host.tools;
        core.worker_token = token;
        core.server.auth = {.mode = config::auth_mode::trusted, .workspace = op.workspace_id, .front_token = host.front_token};
        core.server.storage = home + "\\core";
        core.server.worker_bind = loopback;
        core.server.source = std::format("{}:{}", loopback, config::tcp_egress(core.server.source).second);
        core.server.http_bind = address.empty() ? loopback : address;
        core.server.http_port = constants::host_core_port;
        if (op.overflow_wait_seconds && address != core.server.worker_bind) core.server.overflow_worker_bind = address;
        core.server.worker_port = constants::host_worker_port;
        core.server.overflow_wait_seconds = op.overflow_wait_seconds;
        if (!address.empty()) {
            core.server.remote_worker_bind = address;
            core.server.remote_worker_port = constants::host_remote_port;
            core.server.remote_worker_enrollment = host.remote_worker_enrollment;
        }
        config::validate(core);
        return config::write(core);
    }

    void check_disk(const containerd::mount& mount, std::uint64_t limit) {
        if (mount.type.get() != "windows-layer")
            throw std::runtime_error("workspace requires a writable Windows layer");
        const auto path = fs::path{mount.source.get()} / "sandbox.vhdx";
        VIRTUAL_STORAGE_TYPE type{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
        OPEN_VIRTUAL_DISK_PARAMETERS parameters{OPEN_VIRTUAL_DISK_VERSION_1};
        parameters.Version1.RWDepth = OPEN_VIRTUAL_DISK_RW_DEPTH_DEFAULT;
        winapi::handle disk;
        auto code = ::OpenVirtualDisk(&type, path.c_str(), VIRTUAL_DISK_ACCESS_GET_INFO, OPEN_VIRTUAL_DISK_FLAG_NONE,
                                      &parameters, &disk.value);
        if (code != ERROR_SUCCESS)
            winapi::fail("open workspace VHDX", code);
        GET_VIRTUAL_DISK_INFO info{GET_VIRTUAL_DISK_INFO_SIZE};
        ULONG size = sizeof(info);
        code = ::GetVirtualDiskInformation(disk.value, &size, &info, nullptr);
        if (code != ERROR_SUCCESS)
            winapi::fail("read workspace VHDX capacity", code);
        if (!info.Size.VirtualSize || info.Size.VirtualSize > limit)
            throw conflict("Windows scratch VHDX exceeds the workspace storage limit");
    }

    auto beneath(const fs::path& directory, const fs::path& value) -> bool {
        auto part = value.begin();
        for (const auto& parent : directory) {
            if (part == value.end() || ::CompareStringOrdinal(parent.c_str(), -1, part->c_str(), -1, TRUE) != CSTR_EQUAL)
                return false;
            ++part;
        }
        return true;
    }

    auto bind(const fs::path& source, std::string destination) -> json::object {
        return {{"source", source.string()}, {"destination", std::move(destination)}, {"options", json::array{"ro"}}};
    }

    auto run(const config::host_settings& host, const operation& op) -> observed {
        if (pooled(op) || op.work_volume)
            throw conflict("Windows environments use persistent local workspace snapshots");
        const fs::path storage = fs::canonical(host.storage) / "windows";
        const auto dir = storage / op.environment_id;
        durability::durable_directories(dir);
        const auto journal = dir / "environment.json";
        const auto root = fs::weakly_canonical(op.contract.work_root);
        const auto allowed = fs::canonical(host.work_root);
        if (beneath(root, allowed) || !beneath(allowed, root) || beneath(root, storage) || beneath(storage, root))
            throw conflict("workspace path must be below the configured work root");
        const auto previous = fs::exists(journal) ? std::optional{read<record>(journal)} : std::nullopt;
        if (previous && (previous->root != root.string() || previous->workspace != op.workspace_id ||
                         previous->socket != host.socket || previous->name_space != host.name_space))
            throw conflict("Windows workspace identity differs from its retained environment");
        observed result{.environment_id = op.environment_id, .operation_id = op.operation_id, .state = op.desired};
        if (op.desired == environment_state::stopped) {
            if (previous) {
                if (previous->process)
                    host_windows_job::stop(previous->network.id, previous->disk, root, host.timeout_seconds);
                else {
                    containerd::stop(host.socket, host.name_space, op.environment_id);
                    host_windows_network::remove(previous->network);
                }
                result.local_work_root = op.contract.work_root;
            }
            result.volume_fenced = true;
            return result;
        }
        const auto tools = fs::canonical(op.contract.toolchain_root);
        if (!beneath(fs::canonical(host.toolchain_root), tools) || !fs::is_directory(tools))
            throw conflict("toolchain is outside the host toolchain directory");
        for (const auto& source : {host.rootfs, host.toolchain_root, host.shared_cache, host.storage}) {
            if (source.empty()) continue;
            const auto protected_root = fs::canonical(source);
            if (beneath(root, protected_root) || beneath(protected_root, root))
                throw conflict("workspace storage overlaps a host directory");
        }
        if (host.isolation.exclusive) {
            const auto owner = storage / "exclusive.json";
            if (!fs::exists(owner))
                durability::durable_private_write(owner, config::write(op.workspace_id));
            if (read<std::string>(owner) != op.workspace_id)
                throw conflict("exclusive Windows host belongs to another workspace");
        }
        durability::durable_directories(root);
        const auto marker = root / "environment.json";
        if (!fs::exists(marker)) {
            if (!fs::is_empty(root))
                throw conflict("workspace root is already in use");
            durability::durable_private_write(marker, config::write(op.environment_id));
        }
        if (read<std::string>(marker) != op.environment_id)
            throw conflict("workspace root belongs to another environment");

        auto network = previous ? previous->network : host_windows_network::endpoint{};
        if (!previous) {
            const auto slots = storage / "slots";
            durability::durable_directories(slots);
            unsigned slot = 1;
            for (; slot < 32767; ++slot) {
                const auto file = slots / (std::to_string(slot) + ".json");
                if (!fs::exists(file))
                    durability::durable_private_write(file, config::write(op.workspace_id));
                if (read<std::string>(file) == op.workspace_id) break;
            }
            if (slot == 32767) throw conflict("Windows workspace network has no free address");
            const auto ip = [&](unsigned index) { return std::format("{}.{}.{}", host.network_prefix, index / 256, index % 256); };
            const bool core = op.kind == environment_kind::core;
            network = {host_windows_network::new_id(), ip(2 * slot + !core), ip(2 * slot + core)};
            durability::durable_private_write(journal, config::write(record{op.workspace_id, root.string(), host.socket, host.name_space, network}));
        }
        const bool process = op.contract.boundary.kind == "process";
        const bool online = op.contract.boundary.network == "private";
        const auto files = dir / "config";
        durability::durable_directories(files);

        const auto& limits = op.contract.limits;
        auto mounts = containerd::writable_snapshot(host.socket, host.name_space, op.environment_id, host.windows_base_snapshot, limits.storage_bytes);
        if (mounts.size() != 1) throw conflict("unexpected Windows workspace layer layout");
        check_disk(mounts.front(), limits.storage_bytes);
        const auto disk = fs::path{mounts.front().source.get()} / "sandbox.vhdx";
        record current{op.workspace_id, root.string(), host.socket, host.name_space, network, process, disk.string(),
                       previous && previous->data_ready && previous->disk == disk.string()};
        std::string address = online ? (process ? host.bind : network.address) : "";
        std::string home = R"(C:\FughettaWorkspace)";
        PSID user{};
        if (!::ConvertStringSidToSidW(L"S-1-5-93-2-2", &user)) winapi::fail("get ContainerUser SID");
        std::unique_ptr<void, decltype(&::LocalFree)> user_sid{user, ::LocalFree};
        fs::path workdir;
        if (process || !current.data_ready) {
            current.process = true;
            durability::durable_private_write(journal, config::write(current));
            workdir = host_windows_job::mount(disk, root, network.id) / "FughettaWorkspace";
            fs::create_directory(workdir);
            host_windows_job::grant(workdir, user, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | FILE_DELETE_CHILD, true);
            if (!process) host_windows_job::stop(network.id, disk, root, host.timeout_seconds);
            current.data_ready = true;
        }
        current.process = process;
        durability::durable_private_write(journal, config::write(current));
        DWORD pid{};
        if (process) {
            const auto loopback = std::format("127.1.{}", network.address.substr(host.network_prefix.size() + 1));
            home = workdir.string();
            durability::durable_private_write(files / "config.json", settings(host, op, storage, address, home, loopback));
            pid = host_windows_job::start(host, op, network.id, workdir, files / "config.json", tools, network.peer, loopback);
        } else {
            durability::durable_private_write(files / "config.json", settings(host, op, storage, address, home));
            host_windows_job::grant(files, user, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false);
            host_windows_job::grant(files / "config.json", user, FILE_GENERIC_READ, false);
            BYTE system_sid[SECURITY_MAX_SID_SIZE]{};
            DWORD sid_bytes = sizeof(system_sid);
            if (!::CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid, &sid_bytes)) winapi::fail("get LocalSystem SID");
            host_windows_job::grant(files / "config.json", system_sid, FILE_GENERIC_READ, false);
            if (online)
                host_windows_network::attach(host, op, network);
            json::array mapped{bind(fs::canonical(host.rootfs), R"(C:\Fughetta\bin)"),
                               bind(files, R"(C:\Fughetta\config)"), bind(tools, R"(C:\Fughetta\tools)")};
            if (!host.shared_cache.empty())
                mapped.emplace_back(bind(fs::canonical(host.shared_cache), R"(C:\Fughetta\shared)"));
            json::array args{R"(C:\Fughetta\bin\fughetta-service.exe)"};
            if (op.kind == environment_kind::overflow) {
                args.emplace_back("--role");
                args.emplace_back("worker");
            } else args.emplace_back("--workspace-config");
            args.emplace_back(R"(C:\Fughetta\config\config.json)");
            const auto processors = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            const auto rate = std::min<std::uint64_t>(10000, std::uint64_t{limits.cpu_millis} * 10 / std::max<DWORD>(processors, 1));
            if (!rate) throw conflict("CPU budget is below the Windows CPU rate resolution");
            json::object windows;
            if (online) windows.emplace_back("network", json::object{{"endpointList", json::array{network.id}}});
            json::object annotations;
            if (op.contract.boundary.kind == "microvm") {
                const auto vcpus = (std::uint64_t{limits.cpu_millis} + 999) / 1000;
                windows.emplace_back("hyperv", json::object{});
                annotations.emplace_back("io.microsoft.virtualmachine.computetopology.processor.count", std::to_string(vcpus));
                annotations.emplace_back("io.microsoft.virtualmachine.computetopology.processor.limit",
                    std::to_string(std::uint64_t{limits.cpu_millis} * 100 / vcpus));
                annotations.emplace_back("io.microsoft.virtualmachine.computetopology.memory.sizeinmb", std::to_string(limits.memory_bytes >> 20));
            } else {
                windows.emplace_back("resources", json::object{{"cpu", json::object{{"maximum", rate}}},
                    {"memory", json::object{{"limit", limits.memory_bytes}}}});
            }
            const auto spec = json::write(json::object{{"ociVersion", "1.1.0"}, {"root", json::object{{"path", ""}}},
                {"process", json::object{{"args", std::move(args)}, {"cwd", home},
                                          {"user", json::object{{"username", "ContainerUser"}}}}},
                {"mounts", std::move(mapped)}, {"windows", std::move(windows)}, {"annotations", std::move(annotations)}});
            auto task = containerd::start(host.socket, host.name_space, op.environment_id, {std::string{config::runhcs}, spec, std::move(mounts)});
            pid = task.pid.get();
        }
        result.instance_id = op.operation_id + "-" + std::to_string(pid);
        result.volume_fenced = true;
        result.toolchain_read_only = true;
        result.applied = op.contract;
        if (online && op.kind == environment_kind::core) {
            const auto endpoint = [&](auto port) { return std::format("{}:{}", address, port); };
            result.address = endpoint(constants::host_core_port);
            result.worker_address = endpoint(constants::host_remote_port);
            if (op.overflow_wait_seconds) result.executor_address = endpoint(constants::host_worker_port);
        }
        return result;
    }
}

export namespace host_backend {
    auto reconcile(native::scheduler& io, execution::thread_pool& pool, const config::host_settings& host, const host_operation::operation& op)
        -> coroutine::task<host_operation::observed> {
        co_return co_await io::blocking(io, pool, [&] { return run(host, op); });
    }
}
