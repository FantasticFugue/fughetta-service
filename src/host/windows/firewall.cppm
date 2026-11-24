module;
#include <windows.h>
#include <fwpmu.h>

export module host_windows_firewall;
import std;
import async;
import configuration;
import constants;
import host_operation;
import host_windows_network;
import native;
import winapi;

export namespace host_windows_firewall {
    class transaction {
        HANDLE engine_{};
        GUID owner_;
        bool committed_ = false;

        static void check(DWORD code, const char* action) {
            if (code != ERROR_SUCCESS)
                winapi::fail(action, code);
        }

        static constexpr const GUID* layers[] = {&FWPM_LAYER_ALE_AUTH_CONNECT_V4, &FWPM_LAYER_ALE_AUTH_CONNECT_V6,
            &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6};

        void clear(const GUID& layer) {
            FWPM_FILTER_ENUM_TEMPLATE0 selection{};
            selection.layerKey = layer;
            selection.providerKey = &owner_;
            selection.actionMask = ~0u;
            HANDLE cursor{};
            check(::FwpmFilterCreateEnumHandle0(engine_, &selection, &cursor), "enumerate workspace firewall");
            struct enumeration {
                HANDLE engine, cursor;
                ~enumeration() { ::FwpmFilterDestroyEnumHandle0(engine, cursor); }
            } enumeration{engine_, cursor};
            for (;;) {
                FWPM_FILTER0** entries{};
                UINT32 count{};
                check(::FwpmFilterEnum0(engine_, cursor, 64, &entries, &count), "read workspace filters");
                struct page {
                    FWPM_FILTER0** entries;
                    ~page() { if (entries) ::FwpmFreeMemory0(reinterpret_cast<void**>(&entries)); }
                } page{entries};
                for (UINT32 i = 0; i < count; ++i)
                    check(::FwpmFilterDeleteById0(engine_, entries[i]->filterId), "remove workspace filter");
                if (count < 64) break;
            }
        }

        void filter(const GUID& layer, PSID sid, std::optional<std::string_view> remote, unsigned port, bool inbound) {
            FWPM_FILTER_CONDITION0 conditions[4]{};
            UINT32 n = 0;
            const auto condition = [&](const GUID& field, FWP_DATA_TYPE type) -> FWP_CONDITION_VALUE0& {
                auto& c = conditions[n++];
                c.fieldKey = field;
                c.matchType = FWP_MATCH_EQUAL;
                c.conditionValue.type = type;
                return c.conditionValue;
            };
            condition(FWPM_CONDITION_ALE_PACKAGE_ID, FWP_SID).sid = static_cast<SID*>(sid);
            if (remote) {
                condition(FWPM_CONDITION_IP_PROTOCOL, FWP_UINT8).uint8 = 6;
                condition(FWPM_CONDITION_IP_REMOTE_ADDRESS, FWP_UINT32).uint32 = net::ip::make_address_v4(*remote).to_uint();
                condition(inbound ? FWPM_CONDITION_IP_LOCAL_PORT : FWPM_CONDITION_IP_REMOTE_PORT, FWP_UINT16).uint16 = static_cast<UINT16>(port);
            }
            FWPM_FILTER0 filter{};
            filter.displayData.name = const_cast<wchar_t*>(L"Fughetta workspace");
            filter.flags = FWPM_FILTER_FLAG_PERSISTENT;
            filter.providerKey = &owner_;
            filter.layerKey = layer;
            filter.subLayerKey = owner_;
            filter.action.type = remote ? FWP_ACTION_PERMIT : FWP_ACTION_BLOCK;
            UINT64 weight = remote ? 2 : 1;
            filter.weight.type = FWP_UINT64;
            filter.weight.uint64 = &weight;
            filter.numFilterConditions = n;
            filter.filterCondition = conditions;
            check(::FwpmFilterAdd0(engine_, &filter, nullptr, nullptr), "install workspace firewall rule");
        }

    public:
        explicit transaction(const std::string& id) : owner_(host_windows_network::guid(id)) {
            check(::FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, nullptr, &engine_), "open Windows Filtering Platform");
            const auto status = ::FwpmTransactionBegin0(engine_, 0);
            if (status != ERROR_SUCCESS) {
                ::FwpmEngineClose0(engine_);
                check(status, "begin workspace firewall update");
            }
        }
        transaction(const transaction&) = delete;
        ~transaction() {
            if (!committed_) ::FwpmTransactionAbort0(engine_);
            ::FwpmEngineClose0(engine_);
        }
        void commit() {
            check(::FwpmTransactionCommit0(engine_), "commit workspace firewall update");
            committed_ = true;
        }
        void install(PSID sid, const config::host_settings& host, const host_operation::operation& op, const std::string& peer,
                     const std::string& loopback) {
            FWPM_PROVIDER0 provider{};
            provider.providerKey = owner_;
            provider.flags = FWPM_PROVIDER_FLAG_PERSISTENT;
            provider.displayData.name = const_cast<wchar_t*>(L"Fughetta workspace");
            const auto added = ::FwpmProviderAdd0(engine_, &provider, nullptr);
            if (added != FWP_E_ALREADY_EXISTS) check(added, "create workspace firewall provider");
            FWPM_SUBLAYER0 sublayer{};
            sublayer.subLayerKey = owner_;
            sublayer.providerKey = &owner_;
            sublayer.flags = FWPM_SUBLAYER_FLAG_PERSISTENT;
            sublayer.displayData = provider.displayData;
            sublayer.weight = 0xffff;
            const auto sub = ::FwpmSubLayerAdd0(engine_, &sublayer, nullptr);
            if (sub != FWP_E_ALREADY_EXISTS) check(sub, "create workspace firewall sublayer");
            for (const auto* layer : layers) {
                clear(*layer);
                filter(*layer, sid, {}, 0, false);
            }
            if (op.kind == host_operation::environment_kind::core) {
                const auto [address, source_port] = config::tcp_egress(config::server{}.source);
                for (const auto port : {source_port, unsigned{constants::host_worker_port}}) {
                    filter(FWPM_LAYER_ALE_AUTH_CONNECT_V4, sid, loopback, port, false);
                    filter(FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, sid, loopback, port, true);
                    filter(FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, sid, "127.0.0.1", port, true);
                }
            }
            if (op.contract.boundary.network == "private") {
                const auto allow = [&](const std::string& endpoint) {
                    const auto [addr, port] = config::tcp_egress(endpoint);
                    filter(FWPM_LAYER_ALE_AUTH_CONNECT_V4, sid, addr, port, false);
                };
                for (const auto& value : host.egress) allow(value);
                for (const auto& value : op.egress) allow(value);
                if (op.kind == host_operation::environment_kind::overflow)
                    allow(op.core_address);
                else {
                    if (op.overflow_wait_seconds) {
                        filter(FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, sid, peer, constants::host_worker_port, true);
                        filter(FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, sid, host.bind, constants::host_worker_port, true);
                    }
                    for (const auto& source : host.gateway_sources) {
                        filter(FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, sid, source, constants::host_core_port, true);
                        filter(FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, sid, source, constants::host_remote_port, true);
                    }
                }
            }
            commit();
        }
        void remove() {
            FWPM_PROVIDER0* existing{};
            const auto status = ::FwpmProviderGetByKey0(engine_, &owner_, &existing);
            if (status != FWP_E_PROVIDER_NOT_FOUND) {
                check(status, "read workspace firewall provider");
                ::FwpmFreeMemory0(reinterpret_cast<void**>(&existing));
                for (const auto* layer : layers) clear(*layer);
                check(::FwpmSubLayerDeleteByKey0(engine_, &owner_), "remove workspace firewall sublayer");
                check(::FwpmProviderDeleteByKey0(engine_, &owner_), "remove workspace firewall provider");
            }
            commit();
        }
    };
}
