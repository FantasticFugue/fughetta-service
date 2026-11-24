module;
#include <windows.h>
#include <objbase.h>
#include <computenetwork.h>

export module host_windows_network;
import std;
import configuration;
import constants;
import host_operation;
import json;
import winapi;

export namespace host_windows_network {
    struct endpoint {
        std::string id, address, peer;
    };

    inline auto guid(std::string_view id) -> GUID {
        GUID value{};
        if (FAILED(::CLSIDFromString(winapi::wide("{" + std::string{id} + "}").c_str(), &value)))
            throw std::invalid_argument("invalid Windows network identifier");
        return value;
    }

    inline auto new_id() -> std::string {
        GUID id{};
        if (FAILED(::CoCreateGuid(&id)))
            throw std::runtime_error("cannot allocate an HCN endpoint ID");
        wchar_t text[39]{};
        ::StringFromGUID2(id, text, 39);
        return winapi::utf8(std::wstring_view{text}.substr(1, 36));
    }

    struct handles {
        HCN_NETWORK network{};
        HCN_ENDPOINT port{};
        ~handles() {
            if (port) ::HcnCloseEndpoint(port);
            if (network) ::HcnCloseNetwork(network);
        }
    };

    inline void check(HRESULT status, std::string_view action) {
        if (FAILED(status))
            throw std::system_error(static_cast<int>(status), std::system_category(), std::string{action});
    }

    inline void remove(const endpoint& e) {
        const auto status = ::HcnDeleteEndpoint(guid(e.id), nullptr);
        if (status != HCN_E_ENDPOINT_NOT_FOUND)
            check(status, "delete workspace HCN endpoint");
    }

    inline void attach(const config::host_settings& host, const host_operation::operation& op, const endpoint& e) {
        json::array policies;
        const auto rule = [&](std::string direction, std::string remote, std::string ports, bool allow) {
            json::object acl{{"Direction", direction}, {"Action", allow ? "Allow" : "Block"},
                             {"Priority", allow ? 100 : 1000}};
            if (allow) {
                acl.emplace_back("Protocols", "6");
                acl.emplace_back("RemoteAddresses", remote);
                acl.emplace_back(direction == "In" ? "LocalPorts" : "RemotePorts", ports);
                auto host_acl = acl;
                host_acl.emplace_back("RuleType", "Host");
                policies.emplace_back(json::object{{"Type", "ACL"}, {"Settings", std::move(host_acl)}});
            }
            acl.emplace_back("RuleType", "Switch");
            policies.emplace_back(json::object{{"Type", "ACL"}, {"Settings", std::move(acl)}});
        };
        const auto outbound = [&](const std::string& address) {
            const auto split = address.rfind(':');
            rule("Out", address.substr(0, split), address.substr(split + 1), true);
        };
        for (const auto& address : host.egress) outbound(address);
        for (const auto& address : op.egress) outbound(address);
        if (op.kind == host_operation::environment_kind::core) {
            for (const auto& source : host.gateway_sources)
                rule("In", source, std::format("{},{}", constants::host_core_port, constants::host_remote_port), true);
            if (op.overflow_wait_seconds) {
                rule("In", e.peer, std::to_string(constants::host_worker_port), true);
                if (host.isolation.exclusive)
                    rule("In", host.bind, std::to_string(constants::host_worker_port), true);
            }
        } else {
            outbound(op.core_address);
        }
        rule("In", {}, {}, false);
        rule("Out", {}, {}, false);
        const auto configuration = json::write(json::object{
            {"SchemaVersion", json::object{{"Major", 2}, {"Minor", 0}}}, {"Name", op.environment_id}, {"HostComputeNetwork", host.windows_network},
            {"IpConfigurations", json::array{json::object{{"IpAddress", e.address}, {"PrefixLength", 16}}}},
            {"Policies", std::move(policies)}});
        handles h;
        const auto id = guid(e.id);
        const auto status = ::HcnOpenEndpoint(id, &h.port, nullptr);
        if (SUCCEEDED(status))
            return;
        if (status != HCN_E_ENDPOINT_NOT_FOUND)
            check(status, "open workspace HCN endpoint");
        check(::HcnOpenNetwork(guid(host.windows_network), &h.network, nullptr), "open configured workspace HCN network");
        PWSTR detail{};
        const auto created = ::HcnCreateEndpoint(h.network, id, winapi::wide(configuration).c_str(), &h.port, &detail);
        std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> message{detail, ::CoTaskMemFree};
        if (FAILED(created))
            check(created, detail ? "create workspace HCN endpoint: " + winapi::utf8(detail) : "create workspace HCN endpoint");
    }
}
