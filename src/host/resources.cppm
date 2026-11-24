module;
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <netdb.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

export module host_resources;
import std;
import configuration;
import constants;
import content;
import host_operation;
import async;
import durability;
import descriptor;
import native;
import coroutine;
import runner;

namespace host_resources {
    namespace fs = std::filesystem;

    auto below(const fs::path& parent, const fs::path& child) -> bool {
        return std::ranges::mismatch(parent, child).in1 == parent.end();
    }

    auto read_line(const fs::path& path) -> std::string {
        std::ifstream in{path};
        std::string line;
        if (!std::getline(in, line))
            throw std::runtime_error("cannot read " + path.string());
        return line;
    }
}

export namespace host_resources {
    class local_fence {
        fs::path directory_;

    public:
        explicit local_fence(fs::path directory) : directory_(std::move(directory)) {}

        void acquire(std::string_view environment, const std::string& path) {
            durability::durable_directories(directory_);
            for (const auto& entry : fs::directory_iterator(directory_)) {
                if (entry.path().extension() != ".json")
                    continue;
                const auto existing = host_operation::read<std::string>(entry.path());
                if (entry.path().stem() == environment) {
                    if (existing != path)
                        throw host_operation::conflict("environment already holds another workspace volume");
                } else if (below(existing, path) || below(path, existing)) {
                    throw host_operation::conflict("workspace volume already has a writer");
                }
            }
            durability::durable_private_write(directory_ / (std::string{environment} + ".json"), config::write(path));
        }

        void release(std::string_view environment) {
            const auto file = directory_ / (std::string{environment} + ".json");
            if (fs::exists(file))
                durability::durable_remove(file);
        }
    };

    auto command(native::scheduler& io, const config::host_settings& host, std::vector<std::string> argv) -> coroutine::task<> {
        const auto utility = argv.front();
        auto result = co_await runner::run_step(io, {.argv = std::move(argv), .cwd = host.storage},
                                                std::chrono::seconds{host.timeout_seconds}, 16u << 10, utility);
        if (result.exit_code || result.timed_out || result.truncated)
            throw std::runtime_error(std::format("{} failed (status {}): {}", utility, result.exit_code, result.output));
    }

    auto check_cgroups() -> void {
        const auto controllers = read_line("/sys/fs/cgroup/cgroup.controllers");
        const auto entries = " " + controllers + " ";
        for (auto controller : {" cpu ", " memory ", " pids "})
            if (!entries.contains(controller))
                throw std::runtime_error("Linux host needs cgroup v2 CPU, memory and pids controllers");
    }

    auto prepare_toolchain(const config::host_settings& host, host_operation::environment& e) -> void {
        if (!fs::path{e.toolchain_root}.is_absolute())
            throw std::invalid_argument("Linux toolchain mount requires an absolute Linux path");
        const auto tools = fs::canonical(e.toolchain_root);
        if (!below(host.toolchain_root, tools) || !fs::is_directory(tools))
            throw std::invalid_argument("Linux toolchain mount is outside the configured host root");
        e.toolchain_root = tools.string();
    }

    auto prepare_paths(const config::host_settings& host, host_operation::environment& e) -> void {
        if (!fs::path{e.work_root}.is_absolute())
            throw std::invalid_argument("Linux workspace mounts require absolute Linux paths");
        prepare_toolchain(host, e);
        const auto work = fs::weakly_canonical(e.work_root);
        if (work == host.work_root || !below(host.work_root, work))
            throw std::invalid_argument("environment mounts are outside the configured host roots");
        for (const auto& other : {host.rootfs, host.storage, e.toolchain_root, host.shared_cache}) {
            if (other.empty())
                continue;
            const auto dir = fs::weakly_canonical(other);
            if (!dir.is_absolute() || below(dir, work) || below(work, dir))
                throw std::invalid_argument("workspace work_root must be separate from host directories");
        }
        e.work_root = work.string();
    }

    auto mount_id(const fs::path& path) -> std::optional<struct statx> {
        struct statx info{};
        if (::statx(AT_FDCWD, path.c_str(), AT_NO_AUTOMOUNT, STATX_MNT_ID, &info) != 0) {
            if (errno == ENOENT)
                return {};
            throw std::system_error(errno, std::generic_category(), path.string());
        }
        if (!(info.stx_mask & STATX_MNT_ID))
            throw std::runtime_error("Linux host cannot identify workspace mounts");
        return info;
    }

    struct block_id {
        unsigned major, minor;
        friend auto operator==(const block_id&, const block_id&) -> bool = default;
    };

    struct block_volume {
        descriptor::fd handle;
        block_id id;
        std::uint64_t size;

        auto source() const -> std::string {
            return std::format("/proc/{}/fd/{}", ::getpid(), handle.get());
        }
    };

    auto open_volume(const host_operation::volume& volume, const fs::path& directory, bool& busy) -> std::optional<block_volume> {
        busy = false;
        const fs::path path{volume.device};
        const auto prefix = directory.has_filename() ? directory : directory.parent_path();
        const auto parent = fs::weakly_canonical(path.parent_path());
        if (!below(fs::weakly_canonical(prefix), parent))
            throw std::invalid_argument("work_volume.device directory is outside host.volume_devices");
        std::error_code error;
        const auto resolved = fs::canonical(path, error);
        if (error == std::errc::no_such_file_or_directory)
            return {};
        if (error)
            throw fs::filesystem_error("resolve work_volume.device", path, error);
        if (!fs::is_block_file(resolved))
            throw std::invalid_argument("work_volume.device must name a block device");
        auto handle = descriptor::open(resolved, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
        struct stat info{};
        if (::fstat(handle.get(), &info) != 0)
            descriptor::fail("cannot identify", resolved);
        if (!S_ISBLK(info.st_mode))
            throw std::invalid_argument("work_volume.device must name a block device");
        if (::flock(handle.get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK) {
                busy = true;
                return {};
            }
            descriptor::fail("cannot lock", resolved);
        }
        std::uint64_t size = 0;
        if (::ioctl(handle.get(), BLKGETSIZE64, &size) != 0)
            descriptor::fail("cannot read block device size", resolved);
        return block_volume{std::move(handle), {major(info.st_rdev), minor(info.st_rdev)}, size};
    }

    auto wait_volume(native::scheduler& io, execution::thread_pool& pool, const config::host_settings& host,
                     const host_operation::volume& volume) -> coroutine::task<block_volume> {
        host_operation::validate_device(volume, host.volume_devices);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{host.timeout_seconds};
        bool busy = false;
        for (;;) {
            auto device = co_await io::blocking(io, pool, [&] { return open_volume(volume, host.volume_devices, busy); });
            if (device)
                co_return std::move(*device);
            if (std::chrono::steady_clock::now() >= until) {
                if (busy)
                    throw host_operation::conflict("workspace block device is reserved");
                throw std::runtime_error("workspace block device did not appear before the host timeout");
            }
            co_await execution::schedule_after(io, std::chrono::milliseconds{100});
        }
    }

    auto mounted(const fs::path& path, const fs::path& image, std::optional<block_id> device = {}) -> bool {
        const auto info = mount_id(path);
        if (!info || info->stx_mnt_id == mount_id(path.parent_path()).value().stx_mnt_id)
            return false;
        if (device) {
            if (*device != block_id{info->stx_dev_major, info->stx_dev_minor})
                throw host_operation::conflict("workspace work_root has a different mount");
            return true;
        }
        const auto backing = fs::path{"/sys/dev/block"} /
            std::format("{}:{}", info->stx_dev_major, info->stx_dev_minor) / "loop/backing_file";
        if (!fs::exists(backing) || read_line(backing) != image.string())
            throw std::runtime_error("workspace work_root has a different mount");
        return true;
    }

    void require_available(block_id id, const std::optional<struct statx>& own_mount) {
        const auto number = std::format("{}:{}", id.major, id.minor);
        std::ifstream mounts{"/proc/self/mountinfo"};
        for (std::string line; std::getline(mounts, line);) {
            std::istringstream fields{line};
            std::uint64_t mount = 0, parent = 0;
            std::string device;
            if (fields >> mount >> parent >> device && device == number && (!own_mount || mount != own_mount->stx_mnt_id))
                throw host_operation::conflict("workspace block device already has a writer");
        }
        if (!mounts.eof())
            throw std::runtime_error("cannot read Linux workspace mounts");
    }

    void own_work(const config::host_settings& host, const std::string& path) {
        if (::chown(path.c_str(), host.mapped_id, host.mapped_id) != 0)
            throw std::system_error(errno, std::generic_category(), "workspace ownership");
    }

    auto mount_volume(native::scheduler& io, execution::thread_pool& pool, const config::host_settings& host,
                      const std::string& work, const block_volume& device) -> coroutine::task<> {
        const bool attach = co_await io::blocking(io, pool, [&] {
            if (mounted(work, {}, device.id))
                return false;
            durability::durable_directories(work);
            if (!fs::is_empty(work))
                throw std::runtime_error("workspace mount point must be empty");
            return true;
        });
        if (attach) {
            const auto source = device.source();
            const auto signature = co_await runner::run_step(
                io, {.argv = {"blkid", "-p", "-o", "value", "-s", "TYPE", "-s", "PTTYPE", "--", source}, .cwd = host.storage},
                std::chrono::seconds{host.timeout_seconds}, 16u << 10, "blkid");
            if (signature.timed_out || signature.truncated)
                throw std::runtime_error("could not read workspace block device signatures");
            if (signature.exit_code == 2 && signature.output.empty())
                co_await command(io, host, {"mkfs.ext4", "-q", "-m", "0", "-b", "4096", "-E", "nodiscard", source});
            else if (signature.exit_code != 0 || signature.output != "ext4\n")
                throw host_operation::conflict("workspace block device must be blank or contain ext4");
            co_await io::blocking(io, pool, [&] {
                if (::mount(source.c_str(), work.c_str(), "ext4", MS_NOSUID | MS_NODEV, nullptr) != 0)
                    descriptor::fail("cannot mount workspace block device at", work);
                return true;
            });
        }
        co_await io::blocking(io, pool, [&] {
            own_work(host, work);
            return true;
        });
    }

    auto mount_work(native::scheduler& io, execution::thread_pool& pool, const config::host_settings& host,
                    const host_operation::environment& e, const fs::path& image) -> coroutine::task<> {
        if (e.limits.storage_bytes < constants::host_minimum_storage || e.limits.storage_bytes > std::numeric_limits<std::int64_t>::max())
            throw std::invalid_argument("ext4 workspace images require 16 MiB to INT64_MAX bytes");
        const auto draft = image.string() + ".creating";
        const bool create = co_await io::blocking(io, pool, [&] {
            if (fs::exists(image))
                return false;
            fs::remove(draft);
            durability::durable_private_write(draft, "");
            return true;
        });
        if (create) {
            co_await command(io, host, {"fallocate", "-l", std::to_string(e.limits.storage_bytes), "--", draft});
            co_await command(io, host, {"mkfs.ext4", "-F", "-q", "-m", "0", "-b", "4096", "-E", "nodiscard", draft});
        }
        const bool attach = co_await io::blocking(io, pool, [&] {
            if (create) {
                descriptor::sync(descriptor::open(draft, O_RDONLY | O_NOFOLLOW), draft);
                durability::durable_rename(draft, image);
            }
            if (fs::file_size(image) != e.limits.storage_bytes)
                throw std::runtime_error("workspace image size differs from its storage limit");
            if (mounted(e.work_root, image))
                return false;
            durability::durable_directories(e.work_root);
            if (!fs::is_empty(e.work_root))
                throw std::runtime_error("workspace mount point must be empty");
            return true;
        });
        if (attach)
            co_await command(io, host, {"mount", "-t", "ext4", "-o", "loop,nosuid,nodev", "--", image.string(), e.work_root});
        co_await io::blocking(io, pool, [&] {
            if (!mounted(e.work_root, image))
                throw std::runtime_error("workspace image was not mounted");
            own_work(host, e.work_root);
            return true;
        });
    }

    auto unmount_work(native::scheduler& io, execution::thread_pool& pool, const config::host_settings& host,
                      const fs::path& work, const fs::path& image, std::optional<block_id> device = {}) -> coroutine::task<> {
        const bool attached = co_await io::blocking(io, pool, [&] { return mounted(work, image, device); });
        if (attached)
            co_await command(io, host, {"umount", "--", work.string()});
    }

    struct network {
        std::string name, path, peer, address, overflow, pool_interfaces;
    };

    auto network_for(const config::host_settings& host, unsigned slot, host_operation::environment_kind kind,
                     bool pooled = false) -> network {
        const auto index = 2 * slot + (kind == host_operation::environment_kind::overflow ? 1 : 0);
        const auto ip = [&](unsigned index, unsigned offset) {
            return std::format("{}.{}.{}", host.network_prefix, index / 64, (index % 64) * 4 + offset);
        };
        const auto prefix = content::hash_of(host.name_space).str().substr(0, 6);
        const auto name = (pooled ? "fw" : "fg") + prefix + std::to_string(index);
        return {name, "/run/netns/" + name, ip(index, 1), ip(index, 2), ip(2 * slot + 1, 2), "fw" + prefix + "*"};
    }

    auto reserve_network(const fs::path& directory, const std::string& workspace) -> unsigned {
        durability::durable_directories(directory);
        const auto file = directory / (workspace + ".json");
        if (fs::exists(file))
            return host_operation::read<unsigned>(file);
        std::set<unsigned> taken;
        for (const auto& entry : fs::directory_iterator(directory))
            if (entry.path().extension() == ".json")
                taken.insert(host_operation::read<unsigned>(entry.path()));
        for (unsigned slot = 1; slot < 8192; ++slot)
            if (!taken.contains(slot)) {
                durability::durable_private_write(file, config::write(slot));
                return slot;
            }
        throw std::runtime_error("Linux workspace network is full");
    }

    struct endpoint {
        std::string name, address;
        unsigned port;
    };

    auto numeric_address(std::string_view name) -> bool {
        in_addr value{};
        return ::inet_pton(AF_INET, std::string{name}.c_str(), &value) == 1;
    }

    auto resolve(std::string_view authority) -> std::vector<endpoint> {
        const auto [name, port] = config::tcp_egress(authority);
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (::getaddrinfo(std::string{name}.c_str(), nullptr, &hints, &found))
            throw std::runtime_error("cannot resolve egress host " + std::string{name});
        const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses{found, ::freeaddrinfo};
        std::vector<endpoint> result;
        for (auto* node = found; node; node = node->ai_next) {
            char ip[INET_ADDRSTRLEN];
            const auto* address = reinterpret_cast<const sockaddr_in*>(node->ai_addr);
            if (!::inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip)))
                throw std::system_error(errno, std::generic_category(), "egress IPv4 address");
            result.push_back({std::string{name}, ip, port});
        }
        return result;
    }

    auto remove_network(native::scheduler& io, execution::thread_pool& pool, const config::host_settings& host,
                        const network& net, bool private_network) -> coroutine::task<> {
        const bool exists = co_await io::blocking(io, pool, [&] { return fs::exists(net.path); });
        if (exists)
            co_await command(io, host, {"ip", "netns", "delete", net.name});
        if (private_network)
            co_await command(io, host, {"nft", "destroy", "table", "inet", net.name});
    }

    auto firewall(const config::host_settings& host, const network& net, const host_operation::operation& op,
                  std::span<const endpoint> endpoints) -> std::string {
        std::string rules = std::format("table inet {} {{\n", net.name);
        const auto add = [&](std::string_view chain, std::string_view hook) {
            rules += std::format("chain {} {{ type filter hook {} priority -10; policy accept;\n", chain, hook);
        };
        const auto from = std::format("iifname \"{}\" ", net.name);
        const auto to = std::format("oifname \"{}\" ", net.name);
        const bool pooled = host_operation::pooled(op);
        const auto egress = [&] {
            if (!pooled)
                rules += std::format("{}ip saddr {} ct state established accept\n", from, net.address);
            for (const auto& endpoint : endpoints)
                rules += std::format("{}ip saddr {} ip daddr {} tcp dport {} ct state {} accept\n",
                                     from, net.address, endpoint.address, endpoint.port, pooled ? "{ new, established }" : "new");
            rules += from + "drop\n";
        };
        const auto ingress = [&](bool local) {
            if (pooled) {
                for (const auto& endpoint : endpoints)
                    rules += std::format("{}ip daddr {} ip saddr {} tcp sport {} ct state established accept\n",
                                         to, net.address, endpoint.address, endpoint.port);
            } else {
                rules += std::format("{}ip daddr {} ct state established accept\n", to, net.address);
            }
            if (op.kind == host_operation::environment_kind::core) {
                const auto accept = [&](std::string_view source, std::string_view ports) {
                    rules += std::format("{}ip saddr {} ip daddr {} tcp dport {} ct state new accept\n", to, source, net.address, ports);
                };
                const auto ports = std::format("{{ {}, {} }}", constants::host_core_port, constants::host_remote_port);
                if (local)
                    accept(net.peer, ports);
                else {
                    for (const auto& source : host.gateway_sources)
                        accept(source, ports);
                    if (op.overflow_wait_seconds) {
                        accept(net.overflow, std::to_string(constants::host_worker_port));
                        rules += std::format("iifname \"{}\" {}ip daddr {} tcp dport {} ct state new accept\n",
                                             net.pool_interfaces, to, net.address, constants::host_worker_port);
                    }
                }
            }
            rules += to + "drop\n";
        };
        add("incoming", "input");
        egress();
        rules += "}\n";
        add("outgoing", "output");
        ingress(true);
        rules += "}\n";
        add("transit", "forward");
        egress();
        ingress(false);
        rules += std::format("}}\nchain translate {{ type nat hook postrouting priority srcnat; policy accept;\n"
                             "ip saddr {} ip daddr != {}.0.0/16 masquerade\n}}\n}}\n", net.address, host.network_prefix);
        return rules;
    }

    auto setup_network(native::scheduler& io, execution::thread_pool& pool, const config::host_settings& host,
                       const network& net, const host_operation::operation& op, const fs::path& directory) -> coroutine::task<> {
        const bool private_network = op.contract.boundary.network == "private";
        co_await remove_network(io, pool, host, net, private_network);
        co_await command(io, host, {"ip", "netns", "add", net.name});
        co_await command(io, host, {"ip", "-n", net.name, "link", "set", "lo", "up"});
        if (!private_network)
            co_return;

        const auto endpoints = co_await io::blocking(io, pool, [&] {
            std::vector<endpoint> all;
            auto append = [&](std::string_view address) {
                auto entries = resolve(address);
                all.insert(all.end(), std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
            };
            if (!host_operation::pooled(op))
                for (const auto& entry : host.egress)
                    append(entry);
            for (const auto& entry : op.egress)
                append(entry);
            if (!op.core_address.empty())
                append(op.core_address);
            if ((!host.gateway_sources.empty() || op.overflow_wait_seconds ||
                 std::ranges::any_of(all, [&](const auto& entry) { return entry.address != net.peer; })) &&
                read_line("/proc/sys/net/ipv4/ip_forward") != "1")
                throw std::runtime_error("private workspace routing requires IPv4 forwarding");
            return all;
        });

        const auto rules = directory / "network.nft";
        co_await io::blocking(io, pool, [&] {
            durability::durable_private_write(rules, firewall(host, net, op, endpoints));
            std::string hosts = "127.0.0.1 localhost\n::1 localhost\n";
            for (const auto& entry : endpoints)
                if (!numeric_address(entry.name))
                    hosts += entry.address + " " + entry.name + "\n";
            const auto hosts_file = directory / "hosts";
            durability::durable_private_write(hosts_file, hosts);
            fs::permissions(hosts_file, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
            return true;
        });
        co_await command(io, host, {"nft", "-f", rules.string()});
        co_await command(io, host, {"ip", "link", "add", "name", net.name, "type", "veth", "peer", "name", "eth0", "netns", net.name});
        co_await command(io, host, {"ip", "addr", "add", net.peer + "/30", "dev", net.name});
        co_await command(io, host, {"ip", "-n", net.name, "addr", "add", net.address + "/30", "dev", "eth0"});
        co_await command(io, host, {"ip", "link", "set", net.name, "up"});
        co_await command(io, host, {"ip", "-n", net.name, "link", "set", "eth0", "up"});
        if (!host_operation::pooled(op))
            co_await command(io, host, {"ip", "-n", net.name, "route", "add", "default", "via", net.peer});
    }

    auto claim_network(native::scheduler& io, execution::thread_pool& pool, const config::host_settings& host,
                       const network& net, const host_operation::operation& op, const fs::path& root) -> coroutine::task<> {
        const auto rules = root / "claim.nft";
        co_await io::blocking(io, pool, [&] {
            const auto [address, port] = config::tcp_egress(op.core_address);
            if (!numeric_address(address))
                throw std::invalid_argument("Worker claim requires a numeric IPv4 Core endpoint");
            if (address != net.peer && read_line("/proc/sys/net/ipv4/ip_forward") != "1")
                throw std::runtime_error("private workspace routing requires IPv4 forwarding");
            if (address != net.peer && address.starts_with(host.network_prefix + ".")) {
                const auto slot_file = root.parent_path() / "slots" / (op.workspace_id + ".json");
                const auto slot = fs::exists(slot_file) ? host_operation::read<unsigned>(slot_file) : 0;
                if (!slot || network_for(host, slot, host_operation::environment_kind::core).address != address ||
                    port != constants::host_worker_port)
                    throw host_operation::conflict("claim needs the workspace Core execution endpoint");
            }
            const endpoint target{std::string{address}, std::string{address}, port};
            durability::durable_private_write(rules, std::format("delete table inet {}\n", net.name) +
                firewall(host, net, op, std::span{&target, 1}));
            return true;
        });
        co_await command(io, host, {"ip", "-n", net.name, "route", "replace", "default", "via", net.peer});
        co_await command(io, host, {"nft", "-f", rules.string()});
    }
}
