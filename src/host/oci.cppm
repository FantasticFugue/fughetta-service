module;
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

export module host_oci;
import std;
import json;
import configuration;
import containerd;
import host_operation;

export namespace host_oci {
    inline constexpr std::string_view tools_path = "/opt/fug/toolchain";
    inline constexpr std::string_view work_path = "/work";
    inline constexpr std::string_view cache_path = "/opt/fug/shared";
    inline constexpr std::string_view service = "/usr/local/bin/fughetta-service";

    struct process {
        std::vector<std::string> args;
        std::vector<std::string> env;
    };
}

namespace host_oci {
    auto seccomp() -> json::object {
        const auto rule = [](json::array names, json::array args = {}) -> json::object {
            return {{"names", std::move(names)}, {"action", "SCMP_ACT_ERRNO"}, {"args", std::move(args)}};
        };
        json::array syscalls{
            rule({"add_key", "request_key", "keyctl", "bpf", "perf_event_open", "userfaultfd", "io_uring_setup",
                  "io_uring_enter", "io_uring_register", "open_by_handle_at", "ptrace", "process_vm_readv", "process_vm_writev",
                  "mount", "umount2", "pivot_root", "move_mount", "open_tree", "fsopen", "fsconfig", "fsmount", "fspick",
                  "mount_setattr", "setns", "unshare", "reboot", "kexec_load", "kexec_file_load", "init_module",
                  "finit_module", "delete_module", "swapon", "swapoff", "iopl", "ioperm"}),
            json::object{{"names", json::array{"clone3"}}, {"action", "SCMP_ACT_ERRNO"}, {"errnoRet", 38}}};
        for (const auto flag : {CLONE_NEWUSER, CLONE_NEWNS, CLONE_NEWPID, CLONE_NEWIPC, CLONE_NEWUTS, CLONE_NEWNET, CLONE_NEWCGROUP})
            syscalls.emplace_back(rule({"clone"}, {json::object{{"index", 0}, {"value", flag}, {"valueTwo", flag}, {"op", "SCMP_CMP_MASKED_EQ"}}}));
        for (int family = 0; family < 17; ++family)
            if (family != AF_UNIX && family != AF_INET && family != AF_INET6 && family != AF_NETLINK)
                syscalls.emplace_back(rule({"socket"}, {json::object{{"index", 0}, {"value", family}, {"op", "SCMP_CMP_EQ"}}}));
        syscalls.emplace_back(rule({"socket"}, {json::object{{"index", 0}, {"value", 17}, {"op", "SCMP_CMP_GE"}}}));
        return {{"defaultAction", "SCMP_ACT_ALLOW"}, {"syscalls", std::move(syscalls)}};
    }

    auto bind_mount(std::string_view target, std::string_view source, bool read_only) -> json::object {
        return {{"destination", target}, {"source", source}, {"type", "bind"},
                {"options", json::array{"bind", "rprivate", "nosuid", "nodev", read_only ? "ro" : "rw"}}};
    }

    auto device_rule(std::string_view type, unsigned major_id, unsigned minor_id) -> json::object {
        return {{"allow", true}, {"type", type}, {"major", major_id}, {"minor", minor_id}, {"access", "rw"}};
    }
}

export namespace host_oci {
    auto spec(const config::host_settings& host, const host_operation::environment& contract, std::string_view netns,
              std::string_view id, const process& program, std::string_view hosts_file = {},
              std::string_view handoff = {}) -> containerd::workload {
        host_operation::validate(contract);
        const auto runtime = host_operation::runtime_for(contract.boundary, host.isolation);
        if (runtime.empty() || contract.os != "linux" || (contract.arch != "x86_64" && contract.arch != "aarch64") ||
            (contract.boundary.network != "none" && contract.boundary.network != "private") ||
            !host.mapped_id || !host.pids || netns.empty() || program.args.empty() ||
            contract.limits.memory_bytes > std::numeric_limits<std::int64_t>::max())
            throw std::invalid_argument("Linux host cannot enforce the requested environment");

        json::array mounts{
            json::object{{"destination", "/proc"}, {"source", "proc"}, {"type", "proc"}, {"options", json::array{"nosuid", "nodev", "noexec"}}},
            json::object{{"destination", "/dev"}, {"source", "tmpfs"}, {"type", "tmpfs"}, {"options", json::array{"nosuid", "noexec", "mode=755", "size=65536"}}},
            json::object{{"destination", "/dev/pts"}, {"source", "devpts"}, {"type", "devpts"}, {"options", json::array{"newinstance", "nosuid", "noexec", "ptmxmode=0666", "mode=0620"}}},
            json::object{{"destination", "/dev/shm"}, {"source", "tmpfs"}, {"type", "tmpfs"}, {"options", json::array{"nosuid", "nodev", "noexec", "mode=1777", "size=67108864"}}},
            bind_mount(tools_path, contract.toolchain_root, true)};
        mounts.emplace_back(bind_mount(work_path, contract.work_root, false));
        if (!handoff.empty())
            mounts.emplace_back(bind_mount("/work/claim", handoff, true));
        if (!hosts_file.empty())
            mounts.emplace_back(bind_mount("/etc/hosts", hosts_file, true));
        if (!host.shared_cache.empty())
            mounts.emplace_back(bind_mount(cache_path, host.shared_cache, true));

        const bool guest = contract.boundary.kind == "microvm";
        const auto uid = guest ? host.mapped_id : 0u;
        json::array namespaces;
        if (!guest) namespaces.emplace_back(json::object{{"type", "user"}});
        for (const auto type : {"mount", "pid", "ipc", "uts", "cgroup"})
            namespaces.emplace_back(json::object{{"type", type}});
        namespaces.emplace_back(json::object{{"type", "network"}, {"path", netns}});
        const json::array mapping{json::object{{"containerID", 0}, {"hostID", host.mapped_id}, {"size", 1}}};
        json::array devices, access{json::object{{"allow", false}, {"access", "rwm"}}};
        for (const auto minor_id : {3u, 5u, 7u, 8u, 9u})
            access.emplace_back(device_rule("c", 1, minor_id));
        access.emplace_back(device_rule("c", 5, 0));
        access.emplace_back(device_rule("c", 5, 2));
        access.emplace_back(json::object{{"allow", true}, {"type", "c"}, {"major", 136}, {"access", "rw"}});
        for (const auto& path : contract.boundary.devices) {
            struct stat node{};
            if (!path.starts_with("/dev/") || std::filesystem::path{path}.lexically_normal() != path ||
                ::stat(path.c_str(), &node) != 0 || (!S_ISCHR(node.st_mode) && !S_ISBLK(node.st_mode)))
                throw std::invalid_argument("requested Linux device is unavailable");
            const auto type = S_ISCHR(node.st_mode) ? "c" : "b";
            const auto major_id = major(node.st_rdev), minor_id = minor(node.st_rdev);
            devices.emplace_back(json::object{{"path", path}, {"type", type}, {"major", major_id},
                                              {"minor", minor_id}, {"fileMode", 0600}, {"uid", uid}, {"gid", uid}});
            access.emplace_back(device_rule(type, major_id, minor_id));
        }

        const auto cpu = std::uint64_t{contract.limits.cpu_millis};
        const auto period = std::max<std::uint64_t>(100000, (1000000 + cpu - 1) / cpu);
        const json::object resources{
            {"cpu", json::object{{"period", period}, {"quota", cpu * period / 1000}}},
            {"memory", json::object{{"limit", contract.limits.memory_bytes}, {"swap", contract.limits.memory_bytes}}},
            {"pids", json::object{{"limit", host.pids}}}, {"devices", std::move(access)}};
        json::array args, env{"PATH=/opt/fug/toolchain/bin:/usr/local/bin:/usr/bin:/bin",
            "HOME=/work", "TMPDIR=/work/tmp"};
        for (const auto& arg : program.args)
            args.emplace_back(arg);
        for (const auto& entry : program.env)
            env.emplace_back(entry);
        const auto empty_caps = json::object{{"bounding", json::array{}}, {"effective", json::array{}}, {"permitted", json::array{}},
                                            {"inheritable", json::array{}}, {"ambient", json::array{}}};
        const json::object document{
            {"ociVersion", "1.1.0"}, {"hostname", "workspace"},
            {"root", json::object{{"path", "rootfs"}, {"readonly", true}}},
            {"process", json::object{{"user", json::object{{"uid", uid}, {"gid", uid}}}, {"args", std::move(args)},
                                     {"env", std::move(env)}, {"cwd", work_path}, {"noNewPrivileges", true}, {"capabilities", empty_caps}}},
            {"mounts", std::move(mounts)},
            {"linux", json::object{{"namespaces", std::move(namespaces)}, {"uidMappings", guest ? json::array{} : mapping}, {"gidMappings", guest ? json::array{} : mapping},
                                   {"cgroupsPath", "/fughetta/" + std::string{id}}, {"devices", std::move(devices)},
                                   {"resources", resources}, {"seccomp", seccomp()},
                                   {"maskedPaths", json::array{"/proc/acpi", "/proc/kcore", "/proc/keys", "/proc/timer_list", "/proc/scsi"}},
                                   {"readonlyPaths", json::array{"/proc/asound", "/proc/bus", "/proc/fs", "/proc/irq", "/proc/sys", "/proc/sysrq-trigger"}}}}};
        return {std::string{runtime}, json::write(json::value{document}),
                {containerd::mount{"bind", host.rootfs, "", std::vector<std::string>{"bind", "ro", "rprivate", "nosuid", "nodev"}}}};
    }
}
