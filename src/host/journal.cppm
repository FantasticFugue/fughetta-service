export module host_journal;
import std;
import configuration;
import durability;
import host_operation;

export namespace host_journal {
    using namespace host_operation;

    struct refusal {
        std::uint16_t status;
        std::string message;
    };

    struct record {
        operation request;
        std::optional<observed> result;
        std::optional<refusal> failure;
    };

    class journal {
        std::filesystem::path root_;
        durability::dir_lock lock_;
        std::map<std::string, record, std::less<>> records_;
        std::map<std::string, record*, std::less<>> latest_;
        std::map<std::string, const observed*, std::less<>> environments_;

        static auto prepare(std::filesystem::path root) -> std::filesystem::path {
            durability::durable_directories(root);
            return root;
        }

        void persist(const record& entry) const {
            durability::durable_private_write(root_ / (entry.request.operation_id + ".json"), config::write(entry));
        }

        void index(record& entry) {
            const auto& id = entry.request.environment_id;
            auto& newest = latest_[id];
            if (!newest || sequence(newest->request) < sequence(entry.request))
                newest = &entry;
            if (entry.result) {
                auto& last = environments_[id];
                if (!last || sequence(records_.at(last->operation_id).request) < sequence(entry.request))
                    last = &*entry.result;
            }
        }

    public:
        explicit journal(const std::filesystem::path& root) : root_(prepare(root)), lock_(root_, "host operation journal") {
            for (const auto& file : std::filesystem::directory_iterator(root_)) {
                if (file.path().extension() != ".json")
                    continue;
                auto entry = read<record>(file.path());
                validate(entry.request);
                auto id = entry.request.operation_id;
                if (file.path().stem() != id)
                    throw std::runtime_error("host operation journal identity differs from its filename");
                index(records_.emplace(std::move(id), std::move(entry)).first->second);
            }
        }

        auto find(std::string_view id) const -> const record* {
            auto it = records_.find(id);
            return it == records_.end() ? nullptr : &it->second;
        }

        auto latest(const record& entry) const -> bool {
            return latest_.at(entry.request.environment_id) == &entry;
        }

        auto accept(operation request) -> const record& {
            validate(request);
            if (auto* entry = find(request.operation_id)) {
                if (config::write(entry->request) != config::write(request))
                    throw conflict("operation identity was reused with different inputs");
                return *entry;
            }
            const auto serial = sequence(request);
            const auto prior = latest_.find(request.environment_id);
            if (prior == latest_.end()) {
                if (serial != 1 || request.desired != environment_state::running)
                    throw conflict("environment ownership journal is missing");
            } else {
                const auto& previous = *prior->second;
                if (pooled(request)) {
                    advance_pool(previous.request, request);
                    if (serial - 1 != sequence(previous.request) ||
                        (!previous.result && !(request.action == verb::destroy && previous.failure)))
                        throw conflict("warm Worker operation is out of sequence");
                } else if (serial - 1 != sequence(previous.request) ||
                    (!previous.result && !(previous.failure && previous.request.work_volume && request.desired == environment_state::stopped)) ||
                    previous.request.host_id != request.host_id ||
                    previous.request.contract.work_root != request.contract.work_root ||
                    previous.request.desired == request.desired)
                    throw conflict("environment operation conflicts with its journal");
            }
            const auto id = request.operation_id;
            auto it = records_.emplace(id, record{std::move(request), {}, {}}).first;
            try {
                persist(it->second);
            } catch (...) {
                records_.erase(it);
                throw;
            }
            index(it->second);
            return it->second;
        }

        void retry(const std::string& id) {
            auto& entry = records_.at(id);
            if (entry.failure) {
                persist(record{entry.request, {}, {}});
                entry.failure.reset();
            }
        }

        void fail(const std::string& id, refusal failure) {
            auto& entry = records_.at(id);
            persist(record{entry.request, {}, failure});
            entry.failure = std::move(failure);
        }

        void complete(const std::string& id, observed result) {
            auto& entry = records_.at(id);
            persist(record{entry.request, result, {}});
            entry.result = std::move(result);
            index(entry);
        }

        auto pending() const -> std::vector<const record*> {
            std::vector<const record*> result;
            for (const auto& [id, entry] : latest_)
                if (!entry->result && !entry->failure)
                    result.push_back(entry);
            return result;
        }

        auto inventory() const -> std::vector<observed> {
            std::vector<observed> result;
            for (const auto& [id, environment] : environments_)
                result.push_back(*environment);
            return result;
        }

        auto warm() const -> std::vector<warm_environment> {
            std::vector<warm_environment> result;
            for (const auto& [id, entry] : latest_)
                if (entry->request.action == verb::warm && entry->result)
                    result.push_back({id, entry->request.profile, entry->result->state});
            return result;
        }

    };
}
