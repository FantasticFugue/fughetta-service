import std;
import configuration;
import json;
import host_operation;
import host_oci;
import containerd;

namespace {
    void require(bool condition, std::string_view message) {
        if (!condition)
            throw std::runtime_error(std::string{message});
    }

    auto field(const json::value& object, std::string_view key) -> json::value {
        return object.to_object().value().get(key).value();
    }

    auto contains(const json::value& values, std::string_view value) -> bool {
        const auto entries = values.to_array().value();
        return std::ranges::any_of(entries, [&](const auto& entry) { return entry.to_string().value() == value; });
    }

    auto denied(const config::host_settings& host, const host_operation::environment& contract) -> bool {
        try {
            (void)host_oci::spec(host, contract, "/run/netns/test", "env-test", {{"/bin/sh"}, {}});
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    }
}

int main() {
    try {
        config::host_settings host;
        host.rootfs = "/var/lib/fughetta/rootfs";
        host.shared_cache = "/srv/cache";
        host.pids = 123;
        host.isolation.runtimes = {std::string{config::runc}, std::string{config::runsc}, std::string{config::kata}};
        host_operation::environment contract{.os = "linux", .arch = "x86_64", .toolchain = "clang",
                                       .boundary = {.kind = "sandbox", .network = "none"},
                                       .limits = {1250, 64u << 20, 128u << 20}, .toolchain_root = "/srv/tools", .work_root = "/srv/work"};
        const auto spec = [&] { return host_oci::spec(host, contract, "/run/netns/test", "env-test", {{"/bin/sh"}, {}}); };
        const auto sandbox = spec();
        require(sandbox.runtime == config::runsc, "sandbox must select runsc");
        const auto doc = config::parse_as<json::value>(sandbox.spec, "OCI spec");
        const auto process = field(doc, "process");
        require(field(field(doc, "root"), "readonly").to_bool().value(), "rootfs must be read-only");
        require(field(process, "cwd").to_string().value() == "/work" && field(process, "noNewPrivileges").to_bool().value(),
                "process must start inside the restricted work directory");
        for (auto key : {"bounding", "effective", "permitted", "inheritable", "ambient"})
            require(field(field(process, "capabilities"), key).to_array().value().empty(), "capabilities must be empty");

        const auto platform = field(doc, "linux");
        const auto resources = field(platform, "resources");
        require(field(field(resources, "cpu"), "quota").to_int64().value() == 125000 &&
                field(field(resources, "cpu"), "period").to_int64().value() == 100000, "CPU quota mismatch");
        require(field(field(resources, "memory"), "limit").to_int64().value() == (64u << 20) &&
                field(field(resources, "memory"), "swap").to_int64().value() == (64u << 20), "memory ceiling mismatch");
        require(field(field(resources, "pids"), "limit").to_int64().value() == 123, "pids ceiling mismatch");
        require(!field(field(platform, "seccomp"), "syscalls").to_array().value().empty(), "seccomp rules missing");
        require(field(platform, "devices").to_array().value().empty(), "unexpected device");
        std::set<std::string> namespaces;
        for (const auto& item : field(platform, "namespaces").to_array().value()) {
            auto type = field(item, "type").to_string().value();
            if (type == "network")
                require(field(item, "path").to_string().value() == "/run/netns/test", "network namespace path mismatch");
            namespaces.emplace(type);
        }
        require(namespaces == std::set<std::string>{"cgroup", "ipc", "mount", "network", "pid", "user", "uts"}, "namespaces mismatch");
        std::map<std::string, bool> mounted;
        for (const auto& mount : field(doc, "mounts").to_array().value())
            mounted.emplace(field(mount, "destination").to_string().value(), contains(field(mount, "options"), "ro"));
        require(mounted.at("/opt/fug/toolchain") && mounted.at("/opt/fug/shared") && !mounted.at("/work"), "mount policy mismatch");

        contract.boundary.shared_kernel = true;
        require(spec().runtime == config::runsc, "sandbox must select runsc when sharing is permitted");
        contract.boundary.kind = "microvm";
        require(spec().runtime == config::kata, "VM must prefer Kata when sharing is permitted");
        contract.boundary.shared_kernel = false;
        require(spec().runtime == config::kata, "VM must prefer Kata");
        host.isolation.runtimes = {std::string{config::runc}};
        require(denied(host, contract), "VM must reject shared runc host");
        host.isolation.exclusive = true;
        require(denied(host, contract), "exclusive host must still provide a guest kernel for a microVM");
        contract.boundary.kind = "sandbox";
        require(denied(host, contract), "sandbox must reject runc");
        contract.boundary.kind = "container";
        require(denied(host, contract), "shared kernel requires consent");
        contract.boundary.shared_kernel = true;
        require(spec().runtime == config::runc, "exclusive shared-kernel contract must select runc");
        host.isolation.exclusive = false;
        require(denied(host, contract), "runc requires an exclusive host");
        host.isolation.exclusive = true;
        contract.boundary.devices = {"/dev/full"};
        require(denied(host, contract), "host must offer the device");
        host.isolation.devices = contract.boundary.devices;
        require(field(field(config::parse_as<json::value>(spec().spec, "OCI spec"), "linux"), "devices").to_array().value().size() == 1,
                "requested device missing");
        contract.boundary.network = "public";
        require(denied(host, contract), "unrecognized network policy accepted");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
