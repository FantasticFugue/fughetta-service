module;
#include <cerrno>
#include <unistd.h>

export module host_backend;
import std;
import containerd;
import host_operation;
import configuration;
import constants;
import coroutine;
import native;
import durability;
import keyring;
import host_oci;
import host_resources;
import runner;
import async;

export namespace host_backend {
    namespace fs = std::filesystem;

    struct record {
        config::host_settings host;
        host_operation::operation request;
        std::string work_root;
        unsigned slot;
        std::uint32_t pid = 0;
        std::optional<host_resources::block_id> device;
    };

    auto credentials(const fs::path& storage, const std::string& workspace) -> std::string {
        const auto dir = storage / "credentials";
        durability::durable_directories(dir);
        const auto file = dir / (workspace + ".json");
        if (!fs::exists(file))
            durability::durable_private_write(file, config::write(keyring::mint()));
        return host_operation::read<std::string>(file);
    }

    void own(const fs::path& path, std::uint32_t mapped_id) {
        if (::lchown(path.c_str(), mapped_id, mapped_id) != 0)
            throw std::system_error(errno, std::generic_category(), "workspace file ownership");
    }

    auto worker_settings(const config::host_settings& host, const host_operation::operation& op,
                         const std::string& token) -> config::worker {
        config::worker conf;
        conf.name = op.environment_id;
        conf.gateway = op.core_address;
        conf.token = token;
        conf.data_root = "/work/hot";
        conf.work_root = "/work/trees";
        conf.dependency_cache = "/work/dependencies";
        conf.tools = host.tools;
        conf.compute_threads = op.contract.runtime.threads;
        conf.parallel = op.contract.runtime.jobs;
        conf.memory_bytes = op.contract.runtime.worker_memory_bytes;
        return conf;
    }

    void assign_host(const config::host_settings& host, const fs::path& storage, const std::string& workspace) {
        const auto file = storage / "exclusive.json";
        const auto owner = host.isolation.exclusive ? std::optional{workspace} : std::nullopt;
        if (fs::exists(file)) {
            if (host_operation::read<std::optional<std::string>>(file) != owner)
                throw host_operation::conflict("Linux host isolation assignment differs from its journal");
        } else {
            durability::durable_private_write(file, config::write(owner));
        }
    }

    auto configure(const config::host_settings& host, const host_operation::operation& op, const host_resources::network& network,
                   const fs::path& work, const fs::path& storage) -> void {
        using namespace host_oci;
        const auto core = op.kind == host_operation::environment_kind::core;
        const auto online = op.contract.boundary.network == "private";
        const auto token = credentials(storage, op.workspace_id);
        const auto file = work / "config.json";
        const auto tmp = work / "tmp";
        fs::create_directory(tmp);
        if (!fs::is_directory(fs::symlink_status(tmp)))
            throw std::runtime_error("workspace temporary directory is invalid");
        own(tmp, host.mapped_id);

        if (core) {
            config::document conf;
            if (fs::exists(fs::symlink_status(file))) {
                std::string data;
                for (auto part : durability::read_plain(file, 1u << 20))
                    data.append(reinterpret_cast<const char*>(part.data()), part.size());
                conf = config::parse_as<config::document>(data, "workspace Core settings");
            }
            conf.runtime = op.contract.runtime;
            conf.granted = op.granted;
            conf.tools = host.tools;
            conf.server = {};
            conf.server.auth = {.mode = config::auth_mode::trusted, .workspace = op.workspace_id, .front_token = host.front_token};
            conf.server.storage = "/work/core";
            conf.server.http_bind = online ? network.address : "127.0.0.1";
            conf.server.http_port = constants::host_core_port;
            conf.server.worker_port = constants::host_worker_port;
            conf.server.overflow_wait_seconds = op.overflow_wait_seconds;
            conf.worker_token = token;
            if (online) {
                if (op.overflow_wait_seconds) conf.server.overflow_worker_bind = network.address;
                conf.server.remote_worker_bind = network.address;
                conf.server.remote_worker_port = constants::host_remote_port;
                conf.server.remote_worker_enrollment = host.remote_worker_enrollment;
            }
            config::validate(conf);
            durability::durable_private_write(file, config::write(conf));
        } else {
            if (!online || op.core_address.empty())
                throw std::invalid_argument("overflow Worker needs a private Core endpoint");
            durability::durable_private_write(file, config::write(worker_settings(host, op, token)));
        }
        own(file, host.mapped_id);
    }

    auto pool_worker(native::scheduler& io, execution::thread_pool& pool, const config::host_settings& host,
                     const host_operation::operation& op) -> coroutine::task<host_operation::observed> {
        using namespace host_operation;
        auto saved = host;
        saved.token.clear();
        saved.front_token.clear();
        const auto storage = fs::path{host.storage} / "linux";
        const auto root = storage / op.environment_id;
        const auto journal = root / "request.json";
        const auto handoff = root / "handoff";
        const auto binding = handoff / "config.json";
        const auto image = root / "workspace.ext4";
        const auto work = root / "work";
        const auto slot_key = "." + op.environment_id;
        const auto slot_file = storage / "slots" / (slot_key + ".json");
        host_resources::local_fence writer{storage / "writers"};
        auto previous = co_await io::blocking(io, pool, [&]() -> std::optional<record> {
            if (!fs::exists(journal))
                return {};
            auto entry = read<record>(journal);
            const auto& bound = entry.host;
            if (bound.socket != host.socket || bound.name_space != host.name_space ||
                bound.network_prefix != host.network_prefix || bound.mapped_id != host.mapped_id ||
                (op.action != verb::destroy && bound != saved))
                throw conflict("Linux warm Worker host settings differ from its journal");
            return entry;
        });

        observed result{.environment_id = op.environment_id, .operation_id = op.operation_id, .state = op.desired,
                        .volume_fenced = true};
        if (op.action == verb::destroy) {
            if (previous) {
                const auto net = host_resources::network_for(host, previous->slot, op.kind, true);
                co_await io::blocking(io, pool, [&] {
                    containerd::stop(host.socket, host.name_space, op.environment_id);
                    return true;
                });
                co_await host_resources::remove_network(io, pool, host, net, true);
                if (!previous->work_root.empty())
                    co_await host_resources::unmount_work(io, pool, host, previous->work_root, image);
                co_await host_resources::unmount_work(io, pool, host, work, image);
            }
            co_await io::blocking(io, pool, [&] {
                writer.release(op.environment_id);
                if (fs::exists(root)) {
                    for (const auto& entry : fs::directory_iterator(root))
                        if (entry.path() != journal)
                            fs::remove_all(entry.path());
                    if (fs::exists(journal))
                        durability::durable_remove(journal);
                    fs::remove(root);
                }
                if (fs::exists(slot_file))
                    durability::durable_remove(slot_file);
                return true;
            });
            co_return result;
        }

        if (op.action == verb::warm) {
            auto contract = op.contract;
            co_await io::blocking(io, pool, [&] {
                host_resources::prepare_toolchain(host, contract);
                host_resources::check_cgroups();
                if (!fs::is_directory(host.rootfs) || (!host.shared_cache.empty() && !fs::is_directory(host.shared_cache)))
                    throw std::invalid_argument("Linux rootfs and shared_cache must name host directories");
                if (!previous) {
                    const auto slot = host_resources::reserve_network(storage / "slots", slot_key);
                    durability::durable_directories(root);
                    previous.emplace(record{saved, op, {}, slot});
                    durability::durable_private_write(journal, config::write(*previous));
                }
                return true;
            });
            contract.work_root = work.string();
            const auto net = host_resources::network_for(host, previous->slot, op.kind, true);
            const auto task = co_await io::blocking(io, pool, [&] {
                return containerd::get_task(host.socket, host.name_space, op.environment_id);
            });
            if (!task || task->status.get() == containerd::task_stopped) {
                if (task)
                    co_await io::blocking(io, pool, [&] {
                        containerd::stop(host.socket, host.name_space, op.environment_id);
                        return true;
                    });
                co_await host_resources::mount_work(io, pool, host, contract, image);
                co_await host_resources::setup_network(io, pool, host, net, op, root);
            }
            co_await io::blocking(io, pool, [&] {
                fs::create_directory(work / "claim");
                fs::create_directory(work / "tmp");
                own(work / "tmp", host.mapped_id);
                fs::create_directory(handoff);
                fs::permissions(handoff, fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec);
                const host_oci::process program{{std::string{host_oci::service}, "--role", "worker",
                                                 "--await-binding", "/work/claim/config.json"}, {}};
                const auto spec = host_oci::spec(host, contract, net.path, op.environment_id, program,
                                                (root / "hosts").string(), handoff.string());
                const auto running = containerd::start(host.socket, host.name_space, op.environment_id, spec);
                previous->pid = running.pid.get();
                durability::durable_private_write(journal, config::write(*previous));
                return true;
            });
        } else {
            if (!previous)
                throw conflict("Linux warm Worker journal is missing");
            const auto net = host_resources::network_for(host, previous->slot, op.kind, true);
            auto contract = op.contract;
            const bool attach = co_await io::blocking(io, pool, [&] {
                const auto task = containerd::get_task(host.socket, host.name_space, op.environment_id);
                if (!task || task->pid.get() != previous->pid || task->status.get() != containerd::task_running)
                    throw conflict("warm Worker task is no longer running");
                host_resources::prepare_paths(host, contract);
                if (!previous->work_root.empty() && previous->work_root != contract.work_root)
                    throw conflict("Linux Worker volume path changed");
                const bool mounted = host_resources::mounted(contract.work_root, image);
                if (!mounted) {
                    durability::durable_directories(contract.work_root);
                    if (!fs::is_empty(contract.work_root))
                        throw conflict("Worker volume mount point must be empty");
                }
                assign_host(host, storage, op.workspace_id);
                writer.acquire(op.environment_id, contract.work_root);
                previous->request = op;
                previous->work_root = contract.work_root;
                durability::durable_private_write(journal, config::write(*previous));
                return !mounted;
            });
            if (attach)
                co_await host_resources::command(io, host, {"mount", "--bind", "--", work.string(), contract.work_root});
            co_await host_resources::claim_network(io, pool, host, net, op, root);
            co_await io::blocking(io, pool, [&] {
                if (!fs::exists(binding)) {
                    const auto token = credentials(storage, op.workspace_id);
                    const auto draft = root / "binding.json";
                    durability::durable_private_write(draft, config::write(worker_settings(host, op, token)));
                    own(draft, host.mapped_id);
                    durability::durable_rename(draft, binding);
                }
                return true;
            });
        }
        result.instance_id = op.environment_id + "-1-" + std::to_string(previous->pid);
        result.toolchain_read_only = true;
        result.applied = op.contract;
        co_return result;
    }

    auto process_unit(const config::host_settings& host, std::string_view id) -> std::string {
        return std::format("fughetta-{}-{}.service", host.name_space, id);
    }

    auto main_pid(native::scheduler& io, const config::host_settings& host, const std::string& unit)
        -> coroutine::task<std::uint32_t> {
        auto result = co_await runner::run_step(io,
            {.argv = {"systemctl", "show", "--property=MainPID", "--value", unit}, .cwd = host.storage},
            std::chrono::seconds{host.timeout_seconds}, 4096, "read process unit");
        if (result.timed_out || result.truncated || result.exit_code)
            throw std::runtime_error("cannot inspect " + unit + ": " + result.output);
        auto text = std::string_view{result.output};
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        std::uint32_t pid{};
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), pid);
        if (ec != std::errc{} || end != text.data() + text.size())
            throw std::runtime_error("invalid MainPID for " + unit);
        co_return pid;
    }

    auto start_process(native::scheduler& io, const config::host_settings& host, const host_operation::operation& op,
                       const host_operation::environment& contract, const host_resources::network& net, const std::string& hosts_file)
        -> coroutine::task<std::uint32_t> {
        const auto property_path = [](const std::string& path) -> const std::string& {
            if (path.find_first_of(":%\\\"' \t\r\n") != std::string::npos)
                throw std::invalid_argument("systemd mount path contains a property delimiter");
            return path;
        };
        const auto unit = process_unit(host, op.environment_id);
        std::vector<std::string> args{"systemd-run", "--collect", "--unit=" + unit,
            "--property=TasksMax=" + std::to_string(host.pids), "--property=MemorySwapMax=0",
            "--property=MemoryMax=" + std::to_string(contract.limits.memory_bytes),
            std::format("--property=CPUQuota={}.{}%", contract.limits.cpu_millis / 10, contract.limits.cpu_millis % 10)};
        if (host_operation::runtime_for(contract.boundary, host.isolation) == config::nspawn) {
            args.insert(args.end(), {"--service-type=notify", "--property=NotifyAccess=all", "--property=Delegate=yes", "--property=NetworkNamespacePath=" + net.path,
                "--", "systemd-nspawn", "--settings=no", "--keep-unit", "--register=no", "--as-pid2",
                "--machine=" + op.environment_id.substr(0, 63), "--read-only", "--directory=" + host.rootfs,
                "--private-users=" + std::to_string(host.mapped_id) + ":1", "--private-users-ownership=off",
                "--drop-capability=all", "--no-new-privileges=yes", "--timezone=off", "--resolv-conf=off",
                "--bind=" + property_path(contract.work_root) + ":/work",
                "--bind-ro=" + property_path(contract.toolchain_root) + ":/opt/fug/toolchain", "--chdir=/work"});
            if (!host.shared_cache.empty()) args.push_back("--bind-ro=" + property_path(host.shared_cache) + ":/opt/fug/shared");
            if (!hosts_file.empty()) args.push_back("--bind-ro=" + property_path(hosts_file) + ":/etc/hosts");
        } else {
            args.insert(args.end(), {"--service-type=exec",
            "--property=User=" + std::to_string(host.mapped_id), "--property=Group=" + std::to_string(host.mapped_id),
            "--property=RootDirectory=" + property_path(host.rootfs), "--property=WorkingDirectory=/work",
            "--property=NetworkNamespacePath=" + net.path, "--property=ProtectSystem=strict", "--property=ProtectHome=yes",
            "--property=MountAPIVFS=yes", "--property=PrivateDevices=yes", "--property=PrivateIPC=yes",
            "--property=ProtectProc=invisible", "--property=RestrictNamespaces=yes", "--property=NoNewPrivileges=yes",
            "--property=CapabilityBoundingSet=", "--property=KillMode=control-group", "--property=ProtectControlGroups=yes",
            "--property=ProtectKernelTunables=yes", "--property=ProtectKernelModules=yes",
            "--property=BindPaths=" + property_path(contract.work_root) + ":/work", "--property=ReadWritePaths=/work",
            "--property=BindReadOnlyPaths=" + property_path(contract.toolchain_root) + ":/opt/fug/toolchain"});
            if (!host.shared_cache.empty())
                args.push_back("--property=BindReadOnlyPaths=" + property_path(host.shared_cache) + ":/opt/fug/shared");
            if (!hosts_file.empty())
                args.push_back("--property=BindReadOnlyPaths=" + property_path(hosts_file) + ":/etc/hosts");
        }
        args.insert(args.end(), {"--setenv=HOME=/work", "--setenv=TMPDIR=/work/tmp",
            "--setenv=PATH=/opt/fug/toolchain/bin:/usr/local/bin:/usr/bin:/bin", "--"});
        args.emplace_back(host_oci::service);
        if (op.kind == host_operation::environment_kind::overflow)
            args.insert(args.end(), {"--role", "worker"});
        else
            args.emplace_back("--workspace-config");
        args.emplace_back("/work/config.json");
        co_await host_resources::command(io, host, std::move(args));
        const auto pid = co_await main_pid(io, host, unit);
        if (!pid)
            throw std::runtime_error("process unit exited during startup");
        co_return pid;
    }

    auto reconcile(native::scheduler& io, execution::thread_pool& pool, const config::host_settings& host, const host_operation::operation& op)
        -> coroutine::task<host_operation::observed> {
        if (host_operation::pooled(op))
            co_return co_await pool_worker(io, pool, host, op);
        auto saved = host;
        saved.token.clear();
        saved.front_token.clear();
        const auto storage = fs::path{host.storage} / "linux";
        const auto serial = host_operation::sequence(op);
        const bool start = op.desired == host_operation::environment_state::running;
        const auto root = storage / op.environment_id;
        const auto journal = root / "request.json";
        const auto image = root / "workspace.ext4";
        host_resources::local_fence writer{storage / "writers"};
        auto [previous, contract, slot] = co_await io::blocking(io, pool, [&] {
            const auto previous = fs::exists(journal) ? std::optional{host_operation::read<record>(journal)} : std::nullopt;
            if (previous) {
                const auto prior = host_operation::sequence(previous->request);
                const auto& bound = previous->host;
                const bool skipped_start = !start && previous->request.desired == op.desired && serial > 2 && serial - 2 == prior;
                if (bound.socket != host.socket || bound.name_space != host.name_space ||
                    bound.network_prefix != host.network_prefix || bound.mapped_id != host.mapped_id ||
                    bound.isolation.exclusive != host.isolation.exclusive || previous->request.contract.work_root != op.contract.work_root ||
                    (prior == serial && (config::write(previous->request) != config::write(op) || (start && bound != saved))) ||
                    (prior != serial && !skipped_start && (serial - 1 != prior || previous->request.desired == op.desired)))
                    throw host_operation::conflict("Linux environment operation conflicts with its journal");
            } else if (start && serial != 1) {
                throw host_operation::conflict("Linux environment operation journal is missing");
            }
            auto contract = op.contract;
            if (start)
                host_resources::prepare_paths(host, contract);
            else if (previous)
                contract.work_root = previous->work_root;
            else
                contract.work_root = fs::weakly_canonical(op.contract.work_root).string();
            if (previous && previous->work_root != contract.work_root)
                throw host_operation::conflict("Linux workspace volume path changed");
            const auto slot = previous ? previous->slot : host_resources::reserve_network(storage / "slots", op.workspace_id);
            return std::tuple{std::move(previous), std::move(contract), slot};
        });
        const auto net = host_resources::network_for(host, slot, op.kind);
        const bool private_network = (start || !previous ? op : previous->request).contract.boundary.network == "private";
        const auto endpoint = [&](auto port) { return std::format("{}:{}", net.address, port); };
        const auto hosts_file = private_network ? (root / "hosts").string() : std::string{};
        host_operation::observed result{.environment_id = op.environment_id, .operation_id = op.operation_id, .state = op.desired};
        if (!start) {
            const bool pending = !previous || previous->request.operation_id != op.operation_id;
            if (previous && previous->request.desired == host_operation::environment_state::running) {
                const auto previous_runtime = host_operation::runtime_for(previous->request.contract.boundary, previous->host.isolation);
                if (previous_runtime == config::systemd || previous_runtime == config::nspawn) {
                    const auto unit = process_unit(host, op.environment_id);
                    auto result = co_await runner::run_step(io,
                        {.argv = {"systemctl", "stop", unit}, .cwd = host.storage},
                        std::chrono::seconds{host.timeout_seconds}, 4096, "stop process unit");
                    if (result.timed_out || result.truncated ||
                        (result.exit_code && (result.exit_code != 5 || co_await main_pid(io, host, unit))))
                        throw std::runtime_error("cannot stop " + unit + ": " + result.output);
                } else co_await io::blocking(io, pool, [&] {
                    containerd::stop(host.socket, host.name_space, op.environment_id);
                    return true;
                });
                co_await host_resources::remove_network(io, pool, host, net, private_network);
                co_await host_resources::unmount_work(io, pool, host, contract.work_root, image, previous->device);
            }
            co_await io::blocking(io, pool, [&] {
                if (pending) {
                    durability::durable_directories(root);
                    durability::durable_private_write(journal, config::write(record{saved, op, contract.work_root, slot}));
                }
                writer.release(op.environment_id);
                return true;
            });
            result.volume_fenced = true;
            if (fs::is_regular_file(image))
                result.local_work_root = contract.work_root;
            co_return result;
        }

        std::optional<host_resources::block_volume> volume;
        std::optional<host_resources::block_id> device;
        if (op.work_volume) {
            volume.emplace(co_await host_resources::wait_volume(io, pool, host, *op.work_volume));
            device = volume->id;
            if (volume->size < std::max(op.work_volume->size_bytes, contract.limits.storage_bytes))
                throw std::invalid_argument("workspace block device is smaller than the requested storage");
        }
        const auto runtime = host_operation::runtime_for(contract.boundary, host.isolation);
        const bool managed_unit = runtime == config::systemd || runtime == config::nspawn;
        const auto workload = co_await io::blocking(io, pool, [&] {
            if (device) {
                const bool retry = previous && previous->request.operation_id == op.operation_id;
                if (retry && previous->device != device)
                    throw host_operation::conflict("workspace block device changed during start");
                const bool attached = host_resources::mounted(contract.work_root, image, device);
                const auto own_mount = retry && attached ? host_resources::mount_id(contract.work_root) : std::nullopt;
                host_resources::require_available(*device, own_mount);
            }
            host_oci::process program{{std::string{host_oci::service}}, {}};
            if (op.kind == host_operation::environment_kind::overflow)
                program.args.insert(program.args.end(), {"--role", "worker"});
            else
                program.args.emplace_back("--workspace-config");
            program.args.emplace_back("/work/config.json");
            auto workload = managed_unit ? containerd::workload{} : host_oci::spec(host, contract, net.path, op.environment_id, program, hosts_file);
            host_resources::check_cgroups();
            if (!fs::is_directory(host.rootfs) || (!host.shared_cache.empty() && !fs::is_directory(host.shared_cache)))
                throw std::invalid_argument("Linux rootfs and shared_cache must name host directories");
            assign_host(host, storage, op.workspace_id);
            writer.acquire(op.environment_id, contract.work_root);
            durability::durable_directories(root);
            durability::durable_private_write(journal, config::write(record{saved, op, contract.work_root, slot, 0, device}));
            return workload;
        });
        std::uint32_t pid = 0;
        if (managed_unit) {
            pid = co_await main_pid(io, host, process_unit(host, op.environment_id));
        } else {
            pid = co_await io::blocking(io, pool, [&] {
                const auto task = containerd::get_task(host.socket, host.name_space, op.environment_id);
                if (task && task->status.get() == containerd::task_stopped)
                    containerd::stop(host.socket, host.name_space, op.environment_id);
                else if (task)
                    return task->pid.get();
                return std::uint32_t{};
            });
        }
        if (!pid) {
            if (volume)
                co_await host_resources::mount_volume(io, pool, host, contract.work_root, *volume);
            else
                co_await host_resources::mount_work(io, pool, host, contract, image);
            co_await host_resources::setup_network(io, pool, host, net, op, root);
            co_await io::blocking(io, pool, [&] {
                configure(host, op, net, contract.work_root, storage);
                return true;
            });
        }
        if (!host_resources::mounted(contract.work_root, image, device) || !fs::exists(net.path))
            throw std::runtime_error("Linux task is missing its provisioned resources");
        if (managed_unit) {
            if (!pid)
                pid = co_await start_process(io, host, op, contract, net, hosts_file);
        } else {
            pid = co_await io::blocking(io, pool, [&] {
                return containerd::start(host.socket, host.name_space, op.environment_id, workload).pid.get();
            });
        }
        result.instance_id = op.operation_id + "-" + std::to_string(pid);
        if (op.kind == host_operation::environment_kind::core && private_network) {
            result.address = endpoint(constants::host_core_port);
            result.worker_address = endpoint(constants::host_remote_port);
            if (op.overflow_wait_seconds)
                result.executor_address = endpoint(constants::host_worker_port);
        }
        result.volume_fenced = true;
        result.toolchain_read_only = true;
        result.applied = op.contract;
        if (volume)
            result.work_volume = host_operation::volume{op.work_volume->device, volume->size};
        co_return result;
    }
}

