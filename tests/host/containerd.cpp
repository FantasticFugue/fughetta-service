#include <cerrno>
#include <elf.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import configuration;
import containerd;
import durability;
import host_api;
import host_operation;
import launch;
import protocol;

namespace {
    using namespace host_operation;
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;
    constexpr auto token = "containerd-test-host";
    constexpr auto get = http::method::GET;
    constexpr auto post = http::method::POST;
    constexpr auto operations = "/v1/operations";

    auto wait_for(pid_t pid) -> int {
        int state = 0;
        while (::waitpid(pid, &state, 0) < 0)
            if (errno != EINTR)
                throw std::runtime_error("wait for fixture process");
        return state;
    }

    void rootfs(const fs::path& root, const fs::path& executable, const fs::path& cwd) {
        const auto copy = [&](const fs::path& input, const fs::path& target) {
            fs::create_directories(target.parent_path());
            fs::copy_file(input, target, fs::copy_options::overwrite_existing);
        };
        std::ifstream file{executable, std::ios::binary};
        Elf64_Ehdr header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        fs::path loader;
        for (unsigned i = 0; i < header.e_phnum; ++i) {
            file.seekg(header.e_phoff + i * header.e_phentsize);
            Elf64_Phdr segment{};
            file.read(reinterpret_cast<char*>(&segment), sizeof(segment));
            if (segment.p_type == PT_INTERP) {
                file.seekg(segment.p_offset);
                std::string name;
                std::getline(file, name, '\0');
                loader = name;
                break;
            }
        }
        test::require(loader.is_absolute(), "find fixture dynamic loader");
        test::child listing{{.argv = {loader.string(), "--list", executable.string()}, .cwd = cwd}};
        test::require(listing.wait() == 0, "list fixture runtime libraries: " + listing.output());
        copy(loader, root / loader.relative_path());
        std::istringstream libraries{listing.output()};
        for (std::string word; libraries >> word;)
            if (fs::path{word}.is_absolute())
                copy(word, root / fs::path{word}.relative_path());
        copy(executable, root / "usr/local/bin/fughetta-service");
        for (const auto* name : {"proc", "dev", "work", "opt/fug/toolchain", "opt/fug/shared", "etc"})
            fs::create_directories(root / name);
        std::ofstream{root / "etc/hosts"} << "127.0.0.1 localhost\n";
        std::ofstream{root / "marker"};
    }

    auto submit(unsigned port, const operation& op) -> test::response {
        return test::request(port, post, operations, config::write(op), token, 95s);
    }

    auto operate(unsigned port, const operation& op) -> observed {
        auto response = submit(port, op);
        if (response.status == 202)
            test::eventually([&] {
                response = test::request(port, get, std::string{operations} + "/" + op.operation_id, {}, token);
                return response.status != 202;
            }, 30s);
        test::require(response.status == 200, std::format("operation {}: {} {}", op.operation_id, response.status, response.body));
        return test::decode<observed>(response.body);
    }

    auto report(const fs::path& file) -> std::map<std::string, bool> {
        const auto failure = file.parent_path() / "fixture-error";
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (!fs::exists(file) && std::chrono::steady_clock::now() < deadline) {
            test::require(!fs::exists(failure), test::slurp(failure));
            std::this_thread::sleep_for(25ms);
        }
        test::require(fs::exists(file), "missing workload report " + file.string());
        return test::decode<std::map<std::string, bool>>(test::slurp(file));
    }

    auto stop(operation op, unsigned serial) -> operation {
        op.operation_id = op.environment_id + "-" + std::to_string(serial);
        op.desired = environment_state::stopped;
        return op;
    }

    auto clean(const fs::path& path) -> std::string {
        auto value = test::slurp(path);
        if (value.ends_with('\n'))
            value.pop_back();
        return value;
    }

    auto utility(const fs::path& dir, std::vector<std::string> args) -> std::string {
        test::child child{{.argv = std::move(args), .cwd = dir}};
        const auto status = child.wait();
        auto output = child.output();
        test::require(status == 0, output);
        if (output.ends_with('\n'))
            output.pop_back();
        return output;
    }

    struct loop_volume {
        fs::path image;
        std::string device;

        loop_volume(fs::path path, std::uint64_t size) : image(std::move(path)) {
            std::ofstream{image};
            fs::resize_file(image, size);
            device = utility(image.parent_path(), {"losetup", "--find", "--show", "--", image.string()});
        }

        ~loop_volume() {
            try {
                (void)utility(image.parent_path(), {"losetup", "--detach", device});
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
            }
        }
    };

    void volume_scenario(const fs::path& dir) {
        rootfs(dir / "rootfs", HOST_WORKLOAD, dir);
        fs::create_directories(dir / "tools");
        fs::create_directories(dir / "shared");
        fs::create_directories(dir / "devices");
        std::ofstream{dir / "tools/endpoints"} << "127.0.0.1 1 1 1";
        constexpr std::uint64_t bytes = 40u << 20;
        const loop_volume disk{dir / "workspace.disk", bytes};
        const auto alias = dir / "devices/scsi-0HC_Volume_123";
        const auto other_alias = dir / "devices/alternate";
        fs::create_symlink(disk.device, alias);
        fs::create_symlink(disk.device, other_alias);

        config::host_settings host;
        host.port = test::port();
        host.host_id = "test-host";
        host.token = token;
        host.front_token = "containerd-test-front";
        host.name_space = dir.filename().string();
        host.rootfs = (dir / "rootfs").string();
        host.shared_cache = (dir / "shared").string();
        host.toolchain_root = (dir / "tools").string();
        host.work_root = (dir / "volumes").string();
        host.volume_devices = (dir / "devices").string();
        host.isolation = {{std::string{config::runc}}, true, {}};
        host.pids = 16;
        host.capacity = {2000, 512u << 20, 128u << 20};

        const auto work = dir / "volumes/work";
        operation start{.workspace_id = "test-workspace", .kind = environment_kind::core,
            .desired = environment_state::running, .host_id = host.host_id,
            .contract = {.os = "linux", .arch = std::string{host_api::linux_arch()}, .toolchain = "test",
                .boundary = {.kind = "container", .shared_kernel = true, .network = "none"},
                .limits = {450, 64u << 20, 32u << 20}, .toolchain_root = host.toolchain_root, .work_root = work.string(),
                .runtime = {.threads = 6, .jobs = 1, .workers = 0}},
            .work_volume = volume{alias.string(), 32u << 20}};
        start.environment_id = name(start.workspace_id, start.kind);
        start.operation_id = start.environment_id + "-1";
        auto halt = stop(start, 2);
        halt.work_volume.reset();
        const auto settings = dir / "host.json";
        const auto launch = [&] {
            durability::durable_private_write(settings, config::write(config::host_document{host}));
            return io::command{.argv = {SERVICE_EXECUTABLE, "--role", "host", settings.string()}, .cwd = dir};
        };
        const auto marker = work / "persistent";
        for (const auto fresh : {false, true}) {
            host.storage = (dir / (fresh ? "fresh" : "state")).string();
            if (fresh)
                halt.action = verb::destroy;
            test::child executor{launch()};
            test::eventually([&] {
                test::require(!executor.finished(), executor.output());
                return executor.output().contains("service-ready:host");
            });
            try {
                const auto actual = operate(host.port, start);
                test::require(actual.work_volume == volume{alias.string(), bytes} &&
                              !fs::exists(fs::path{host.storage} / "linux" / start.environment_id / "workspace.ext4"),
                              "start did not use the full block device");
                if (fresh) {
                    test::require(test::slurp(marker) == "workspace data", "attached volume lost its data across host replacement");
                } else {
                    std::ofstream{marker} << "workspace data";
                    auto competing = start;
                    competing.kind = environment_kind::overflow;
                    competing.environment_id = name(competing.workspace_id, competing.kind);
                    competing.operation_id = competing.environment_id + "-1";
                    competing.contract.work_root = (dir / "volumes/other").string();
                    competing.contract.boundary.network = "private";
                    competing.core_address = "10.203.0.6:9090";
                    competing.work_volume->device = other_alias.string();
                    const auto refused = submit(host.port, competing);
                    test::require(refused.status == 409 && refused.body.contains("block device already has a writer"), refused.body);
                }
            } catch (...) {
                (void)submit(host.port, halt);
                throw;
            }
            test::require(!operate(host.port, halt).work_volume && fs::is_empty(work),
                          "stop or destroy left the block device mounted");

            if (fresh) {
                const loop_volume foreign{dir / "foreign.disk", bytes};
                (void)utility(dir, {"mkfs.ext4", "-t", "ext2", "-q", foreign.device});
                fs::remove(alias);
                fs::create_symlink(foreign.device, alias);
                auto rejected = start;
                rejected.operation_id = rejected.environment_id + "-3";
                const auto response = submit(host.port, rejected);
                test::require(response.status == 409 && response.body.contains("must be blank or contain ext4") &&
                              utility(dir, {"blkid", "-p", "-o", "value", "-s", "TYPE", foreign.device}) == "ext2",
                              "start reformatted an existing filesystem: " + response.body);
            }
            test::require(executor.stop() == 0, executor.output());
        }
    }

    void check_isolation(unsigned port, const fs::path& state, operation start, const operation& stop_op) {
        const fs::path work{start.contract.work_root};
        const auto lease = state / "writers" / (start.environment_id + ".json");
        const auto actual = operate(port, start);
        const auto pid = actual.instance_id.substr(actual.instance_id.rfind('-') + 1);
        const auto group = clean(fs::path{"/proc"} / pid / "cgroup");
        const auto cgroup = fs::path{"/sys/fs/cgroup"} / fs::path{group.substr(group.find("::") + 2)}.relative_path();
        test::require(actual.volume_fenced && fs::exists(lease), "workspace volume lacks its writer reservation");
        const auto marker = work / "persistent";
        const bool online = start.contract.boundary.network == "private";
        if (online)
            test::require(test::slurp(marker) == "workspace data", "workspace data did not survive restart");
        else
            std::ofstream{marker} << "workspace data";
        test::require(clean(cgroup / "cpu.max") == "45000 100000" && clean(cgroup / "pids.max") == "16" &&
                      clean(cgroup / "memory.max") == std::to_string(64u << 20) && clean(cgroup / "memory.swap.max") == "0",
                      "cgroup limits differ from the contract");
        const auto result = report(work / "result.json");
        test::require(result == std::map<std::string, bool>{{"toolchain", true}, {"shared", true}, {"root", true}, {"loopback", true},
                      {"allowed", online}, {"host_allowed", online}, {"denied", false}, {"pids", true}, {"memory", true}, {"storage", true}},
                      "runc isolation result: " + config::write(result));
        std::istringstream events{test::slurp(cgroup / "memory.events")};
        std::map<std::string, unsigned> counts;
        for (std::string name; events >> name >> counts[name];) {}
        test::require(counts["oom_kill"] >= 1, "memory limit did not kill the allocating child");
        test::require(config::write(operate(port, start)) == config::write(actual), "start replay changed the observation");
        auto duplicate = start;
        duplicate.operation_id = stop_op.operation_id;
        const auto duplicate_reply = submit(port, duplicate);
        test::require(duplicate_reply.status == 409 && duplicate_reply.body.contains("conflicts with its journal"), duplicate_reply.body);
        auto competing = start;
        competing.kind = environment_kind::overflow;
        competing.environment_id = name(competing.workspace_id, competing.kind);
        competing.operation_id = competing.environment_id + "-1";
        competing.contract.boundary.network = "private";
        competing.egress.clear();
        competing.core_address = "10.236.0.10:9090";
        const auto refused = submit(port, competing);
        test::require(refused.status == 409 && refused.body.contains("workspace volume already has a writer"), refused.body);
        fs::remove(work / "result.json");
    }

    void scenario(const fs::path& dir) {
        rootfs(dir / "rootfs", HOST_WORKLOAD, dir);
        for (auto name : {"tools", "shared"}) {
            fs::create_directory(dir / name);
            const auto marker = dir / name / "marker";
            std::ofstream{marker};
            fs::permissions(marker, fs::perms::all);
        }
        fs::permissions(dir / "rootfs/marker", fs::perms::all);
        native::context context;
        auto io = context.get_scheduler();
        native::listener allowed{io, {net::ip::address_v4{}, 0}};
        native::listener host_allowed{io, {net::ip::address_v4{}, 0}};
        native::listener denied{io, {net::ip::address_v4{}, 0}};
        const auto allowed_port = allowed.local_endpoint().port();
        const auto host_port = host_allowed.local_endpoint().port();
        const auto denied_port = denied.local_endpoint().port();
        std::ofstream{dir / "tools/endpoints"} << "10.236.0.9 " << allowed_port << ' ' << host_port << ' ' << denied_port;
        config::host_settings host;
        host.port = test::port();
        host.host_id = "test-host";
        host.token = token;
        host.front_token = "containerd-test-front";
        host.name_space = dir.filename().string();
        host.storage = (dir / "state").string();
        host.rootfs = (dir / "rootfs").string();
        host.shared_cache = (dir / "shared").string();
        host.work_root = (dir / "volumes").string();
        host.toolchain_root = (dir / "tools").string();
        host.isolation = {{std::string{config::runc}}, true, {}};
        host.pids = 16;
        host.network_prefix = "10.236";
        host.egress = {std::format("10.236.0.9:{}", host_port)};
        host.capacity = {2000, 512u << 20, 128u << 20};

        const auto settings = dir / "host.json";
        durability::durable_private_write(settings, config::write(config::host_document{host}));
        const auto state = fs::path{host.storage} / "linux";
        const auto work = dir / "volumes/work";
        operation start{.workspace_id = "test-workspace", .kind = environment_kind::core,
            .desired = environment_state::running, .host_id = host.host_id,
            .contract = {.os = "linux", .arch = std::string{host_api::linux_arch()}, .toolchain = "test",
                .boundary = {.kind = "container", .shared_kernel = true, .network = "none"},
                .limits = {450, 64u << 20, 32u << 20}, .toolchain_root = host.toolchain_root, .work_root = work.string(),
                .runtime = {.threads = 6, .jobs = 1, .workers = 0}}};
        start.environment_id = name(start.workspace_id, start.kind);
        const auto host_command = [&] {
            return io::command{.argv = {SERVICE_EXECUTABLE, "--role", "host", settings.string()}, .cwd = dir};
        };
        const auto ready = [](test::child& child) {
            test::eventually([&] {
                test::require(!child.finished(), "host startup: " + child.output());
                return child.output().contains("service-ready:host");
            });
        };
        observed stopped;
        {
            test::child executor{host_command()};
            ready(executor);
            for (const auto serial : {1u, 3u}) {
                start.operation_id = start.environment_id + "-" + std::to_string(serial);
                if (serial == 3) {
                    start.contract.boundary.network = "private";
                    start.egress = {std::format("10.236.0.9:{}", allowed_port)};
                }
                const auto stop_op = stop(start, serial + 1);
                try {
                    check_isolation(host.port, state, start, stop_op);
                } catch (...) {
                    if (fs::exists(state / start.environment_id / "request.json"))
                        (void)submit(host.port, stop_op);
                    throw;
                }
                stopped = operate(host.port, stop_op);
                test::require(stopped.volume_fenced && !fs::exists(state / "writers" / (start.environment_id + ".json")) &&
                              fs::is_empty(work), "stop left the workspace writer or mount");
            }
            test::require(executor.stop() == 0, executor.output());
        }
        host.egress.push_back(std::format("10.236.0.21:{}", denied_port));
        durability::durable_private_write(settings, config::write(config::host_document{host}));
        test::child executor{host_command()};
        ready(executor);
        const auto replay = test::request(host.port, get, std::string{operations} + "/" + stopped.operation_id, {}, token);
        test::require(replay.status == 200 && config::write(test::decode<observed>(replay.body)) == config::write(stopped),
                      "restart lost the stop observation");

        config::document core;
        core.runtime = start.contract.runtime;
        core.server.storage = (dir / "core").string();
        core.server.http_port = test::port();
        core.server.worker_bind = "0.0.0.0";
        core.server.worker_port = test::port();
        core.worker_token = read<std::string>(state / "credentials/test-workspace.json");
        const auto core_file = dir / "core.json";
        durability::durable_private_write(core_file, config::write(core));
        test::child gateway{{.argv = {SERVICE_EXECUTABLE, "--role", "gateway", core_file.string()}, .cwd = dir}};
        test::eventually([&] {
            test::require(!gateway.finished(), "Core startup: " + gateway.output());
            return gateway.output().contains("source and language API");
        });
        // The pool uses slot two, with the Core listener reached through the host side of its veth.
        std::ofstream{dir / "tools/pool-endpoints"} << "10.236.0.21 " << core.server.worker_port << ' ' << denied_port
                                                  << ' ' << jobs::code<jobs::hello>;
        std::ofstream{dir / "tools/hello.json"} << json::write(jobs::hello{
            .platform = {.os = "linux", .arch = std::string{host_api::linux_arch()}},
            .compute_threads = 1, .protocol = jobs::protocol_version});
        auto warm = start;
        warm.workspace_id.clear();
        warm.kind = environment_kind::overflow;
        warm.action = verb::warm;
        warm.profile = "linux";
        warm.environment_id = "warm-" + std::string(64, 'a');
        warm.operation_id = warm.environment_id + "-1";
        warm.contract.work_root.clear();
        warm.egress.clear();
        auto claim = warm;
        claim.action = verb::claim;
        claim.operation_id = claim.environment_id + "-2";
        claim.workspace_id = start.workspace_id;
        claim.contract.work_root = (dir / "volumes/hot").string();
        claim.core_address = std::format("10.236.0.21:{}", core.server.worker_port);
        auto destroy = stop(warm, 2);
        destroy.action = verb::destroy;
        const auto pool = state / warm.environment_id;
        try {
            const auto idle = operate(host.port, warm);
            test::require(report(pool / "work/warm.json") == std::map<std::string, bool>{{"core", false}, {"denied", false}, {"binding", false}},
                          "warm Worker reached an endpoint or received a credential");
            destroy = stop(claim, 3);
            destroy.action = verb::destroy;
            const auto active = operate(host.port, claim);
            test::require(active.instance_id == idle.instance_id, "claim replaced the runc task");
            test::require(report(fs::path{claim.contract.work_root} / "claimed.json") ==
                          std::map<std::string, bool>{{"core", true}, {"denied", false}, {"binding", true}},
                          "claimed Worker egress differs from its binding");
            test::eventually([&] { return gateway.output().contains("worker.connection connected workspace_id=local worker_id=managed"); });
            auto other = claim;
            other.workspace_id = "other-workspace";
            test::require(submit(host.port, other).status == 409, "second workspace claimed the running Worker");
        } catch (...) {
            (void)submit(host.port, destroy);
            throw;
        }
        (void)operate(host.port, destroy);
        auto other = claim;
        other.workspace_id = "other-workspace";
        test::require(submit(host.port, other).status == 409 && !fs::exists(pool) &&
                      fs::is_empty(claim.contract.work_root) && !containerd::get_task(host.socket, host.name_space, warm.environment_id),
                      "destroy left a task or volume, or allowed reassignment");
        test::require(gateway.stop() == 0, gateway.output());
        test::require(executor.stop() == 0, executor.output());
    }
}

int main(int argc, char** argv) {
    try {
        const auto executable = fs::canonical("/proc/self/exe");
        if (::geteuid() != 0) {
            const auto pid = ::fork();
            test::require(pid >= 0, "start sudo for the runc suite");
            if (!pid) {
                ::execlp("sudo", "sudo", "-n", "--", executable.c_str(), argc > 1 ? argv[1] : nullptr, static_cast<char*>(nullptr));
                ::_exit(77);
            }
            const auto state = wait_for(pid);
            if (!WIFEXITED(state))
                return 2;
            const auto code = WEXITSTATUS(state);
            return code == 1 ? 77 : code;
        }
        if (!fs::is_socket("/run/containerd/containerd.sock")) {
            std::cerr << "runc suite requires /run/containerd/containerd.sock\n";
            return 77;
        }
        const auto path = std::getenv("PATH") ? std::getenv("PATH") : "";
        const bool volume_only = argc == 2 && std::string_view{argv[1]} == "volume";
        auto tools = std::vector<std::string_view>{"runc", "ip", "nft", "fallocate", "mkfs.ext4", "mount", "umount"};
        if (volume_only)
            tools.insert(tools.end(), {"blkid", "losetup"});
        for (const auto tool : tools) {
            std::istringstream search{path};
            bool found = false;
            for (std::string dir; std::getline(search, dir, ':');)
                if (::access((fs::path{dir} / tool).c_str(), X_OK) == 0) {
                    found = true;
                    break;
                }
            if (!found) {
                std::cerr << "runc suite requires " << tool << '\n';
                return 77;
            }
        }
        ::setenv("TMPDIR", executable.parent_path().c_str(), 1);
        const test::scratch scratch{"host-runtime"};
        if (volume_only) {
            if (!fs::exists("/dev/loop-control")) {
                std::cerr << "volume suite requires /dev/loop-control\n";
                return 77;
            }
            volume_scenario(scratch.dir);
        } else {
            scenario(scratch.dir);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
