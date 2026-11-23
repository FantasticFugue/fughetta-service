export module host_operation;
import std;
import configuration;
import constants;
import provider;
import content;
import json;
import utility;

export namespace host_operation {
    struct conflict : std::runtime_error, util::retained_exception {
        using std::runtime_error::runtime_error;
        auto retain() const noexcept -> std::exception_ptr override { return util::copy_current_exception(*this); }
    };

    struct isolation {
        std::string kind;
        bool shared_kernel = false;
        std::string network = "private";
        std::vector<std::string> devices;
        friend auto operator==(const isolation&, const isolation&) -> bool = default;
    };
    struct environment {
        std::string os, arch, toolchain;
        isolation boundary;
        config::host_limits limits;
        std::string toolchain_root, work_root;
        allocation::budget runtime;
        friend auto operator==(const environment&, const environment&) -> bool = default;
    };
    inline auto runtime_for(const isolation& boundary, const config::host_policy& host) -> std::string_view {
        if (!std::ranges::all_of(boundary.devices, [&](const auto& device) { return std::ranges::contains(host.devices, device); }))
            return {};
        const auto offers = [&](std::string_view runtime) { return std::ranges::contains(host.runtimes, runtime); };
        if (boundary.kind == "sandbox" && offers(config::runsc))
            return config::runsc;
        if (boundary.kind == "vm" && offers(config::kata))
            return config::kata;
        if (host.exclusive && offers(config::runc) && (boundary.kind == "vm" || (boundary.kind == "container" && boundary.shared_kernel)))
            return config::runc;
        return {};
    }

    inline void validate(const environment& e, bool with_volume = true) {
        if (e.os.empty() || e.arch.empty() || e.toolchain.empty() || e.boundary.kind.empty() || e.boundary.network.empty() ||
            !e.limits.cpu_millis || !e.limits.memory_bytes || !e.limits.storage_bytes ||
            e.toolchain_root.contains(char{}) || e.work_root.contains(char{}) || !builds::absolute_path(e.toolchain_root) ||
            (with_volume ? !builds::absolute_path(e.work_root) || e.toolchain_root == e.work_root : !e.work_root.empty()))
            throw std::invalid_argument("environment requires workload, isolation, hard limits, and distinct absolute toolchain and volume paths");
        allocation::distribute(e.runtime);
        if (e.runtime.worker_memory_bytes.value_or(0) > e.limits.memory_bytes)
            throw std::invalid_argument("runtime worker memory exceeds the environment memory limit");
    }

    inline auto worker_contract(environment value) -> environment {
        value.work_root.clear();
        return value;
    }

    inline auto pool_boundary(const isolation& boundary) -> bool {
        return boundary.network == "private" && boundary.devices.empty() &&
            (boundary.kind == "sandbox" || (boundary.kind == "container" && boundary.shared_kernel));
    }

    enum class environment_kind : std::uint8_t { core, overflow, unspecified = 255 };
    enum class environment_state : std::uint8_t { running, stopped, unspecified = 255 };

    enum class verb : std::uint8_t { start_stop, warm, claim, destroy };

    struct volume {
        std::string device;
        std::uint64_t size_bytes = 0;
        friend auto operator==(const volume&, const volume&) -> bool = default;
    };

    struct operation {
        std::string workspace_id, environment_id, operation_id;
        environment_kind kind = environment_kind::unspecified;
        environment_state desired = environment_state::unspecified;
        std::string host_id;
        environment contract;
        std::optional<config::limits> granted;
        std::string core_address;
        std::uint32_t overflow_wait_seconds = 0;
        std::vector<std::string> egress;
        verb action = verb::start_stop;
        std::string profile;
        std::optional<volume> work_volume;
    };
    struct observed {
        std::string environment_id, operation_id;
        environment_state state = environment_state::unspecified;
        std::string instance_id;
        std::string address, worker_address, executor_address;
        bool volume_fenced = false, toolchain_read_only = false;
        std::optional<environment> applied;
        std::optional<volume> work_volume;
    };

    template <class T> auto read(const std::filesystem::path& file) -> T {
        return config::read_host_file<T>(file, "environment journal", 4u << 20);
    }

    inline auto name(std::string_view workspace, environment_kind kind) -> std::string {
        return "env-" + content::hash_of(std::string{workspace} + "/" + std::string{json::enum_spelling(kind)}).str();
    }

    inline auto pooled(const operation& op) -> bool {
        return op.action == verb::warm || op.action == verb::claim ||
            (op.action == verb::destroy && op.environment_id.starts_with("warm-"));
    }

    inline void validate_device(const volume& value, const std::filesystem::path& directory) {
        const std::filesystem::path path{value.device};
        const auto prefix = directory.has_filename() ? directory : directory.parent_path();
        const auto [end, child] = std::ranges::mismatch(prefix, path);
        if (!path.is_absolute() || !path.has_filename() || value.device.contains(char{}) ||
            path != path.lexically_normal() || end != prefix.end() || child == path.end())
            throw std::invalid_argument("work_volume.device must name a path under host.volume_devices");
    }

    struct warm_environment {
        std::string environment_id, profile;
        environment_state state;
    };

    inline auto sequence(const operation& op) -> std::uint64_t {
        if (!op.operation_id.starts_with(op.environment_id + "-"))
            throw std::invalid_argument("invalid environment operation identity");
        auto value = std::string_view{op.operation_id}.substr(op.environment_id.size() + 1);
        std::uint64_t number = 0;
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc{} || end != value.data() + value.size() || !number)
            throw std::invalid_argument("invalid operation sequence");
        return number;
    }

    inline void validate(const operation& op) {
        const auto serial = sequence(op);
        if (op.work_volume && (op.action != verb::start_stop || op.desired != environment_state::running ||
                               op.work_volume->size_bytes < constants::host_minimum_storage))
            throw std::invalid_argument("work_volume requires a start operation and at least 16 MiB");
        if (!pooled(op)) {
            validate(op.contract);
            config::check_name("workspace ID", op.workspace_id);
            if (op.kind == environment_kind::unspecified || op.environment_id != name(op.workspace_id, op.kind) ||
                op.desired == environment_state::unspecified || !op.profile.empty() ||
                (op.action == verb::destroy && op.desired != environment_state::stopped))
                throw std::invalid_argument("invalid workspace environment operation");
            return;
        }
        const bool bound = !op.workspace_id.empty();
        validate(op.contract, bound);
        config::check_name("Worker profile", op.profile);
        if (bound)
            config::check_name("workspace ID", op.workspace_id);
        if (!op.environment_id.starts_with("warm-") || op.environment_id.size() != 69 ||
            op.environment_id.find_first_not_of("0123456789abcdef", 5) != std::string::npos ||
            op.operation_id != op.environment_id + "-" + std::to_string(serial) ||
            op.kind != environment_kind::overflow || !pool_boundary(op.contract.boundary) ||
            op.granted || op.overflow_wait_seconds || !op.egress.empty() || bound != !op.core_address.empty() ||
            !((op.action == verb::warm && serial == 1 && !bound && op.desired == environment_state::running) ||
              (op.action == verb::claim && serial == 2 && bound && op.desired == environment_state::running) ||
              (op.action == verb::destroy && serial == (bound ? 3u : 2u) && op.desired == environment_state::stopped)))
            throw std::invalid_argument("invalid warm Worker operation");
    }

    inline void advance_pool(const operation& prior, const operation& op) {
        if (prior.profile != op.profile || prior.host_id != op.host_id ||
            worker_contract(prior.contract) != worker_contract(op.contract) ||
            !((prior.action == verb::warm && (op.action == verb::claim || op.action == verb::destroy)) ||
              (prior.action == verb::claim && op.action == verb::destroy)) ||
            (prior.action == verb::claim && (op.action != verb::destroy || prior.workspace_id != op.workspace_id ||
                                             prior.core_address != op.core_address || prior.contract != op.contract)))
            throw conflict("warm Worker operation conflicts with its recorded assignment");
    }

}
