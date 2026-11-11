export module enrollment;
import utility;
import std;
import reflection;
import content;
import durability;
import constants;

export namespace enrollment {
    auto mint(std::size_t bytes) -> std::string;
    auto secret_matches(std::string_view digest, std::string_view secret) -> bool;
    inline auto now() -> std::int64_t { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    struct refused : std::runtime_error, util::retained_error<refused> {
        int status;
        std::string code;
        refused(int status, std::string code, std::string message) : std::runtime_error(std::move(message)), status(status), code(std::move(code)) {}
    };
    struct worker {
        std::string worker_id, name, secret_hash, created_by;
        std::int64_t created_at = 0, last_connected_at = 0;
        std::optional<std::int64_t> revoked_at;
    };
    struct record {
        std::string worker_id, name, created_by;
        std::int64_t created_at = 0, last_connected_at = 0;
        std::optional<std::int64_t> revoked_at;
    };
    struct document {
        std::string workspace_id;
        std::map<std::string, worker> workers;
    };
    struct credential {
        std::string workspace_id, worker_id, secret;
    };

    class store {
    public:
        std::function<void()> changed;
        store(const std::filesystem::path& root, std::string workspace)
            : directory_(root / "worker-registry"), lock_(directory_, "Worker registry"), file_(directory_ / "registry.json") {
            data_.workspace_id = std::move(workspace);
            if (std::filesystem::exists(file_)) {
                std::string json;
                try {
                    for (auto piece : durability::read_plain(file_, 16u << 20))
                        json.append(reinterpret_cast<const char*>(piece.data()), piece.size());
                } catch (const durability::unreadable&) {
                    throw std::runtime_error("cannot read Worker registry");
                }
                auto parsed = rfl::json::read<document, rfl::DefaultIfMissing, rfl::NoExtraFields>(json);
                if (!parsed || parsed->workspace_id != data_.workspace_id)
                    throw std::runtime_error("Worker registry belongs to another workspace or is invalid");
                data_ = std::move(*parsed);
            }
        }

        auto add(std::string name, std::string actor, std::int64_t at = now()) -> credential {
            check_name(name);
            credential result;
            transact([&](document& next) {
                if (next.workers.size() >= constants::worker_registry_limit)
                    throw refused(429, "worker_limit", "Worker registry is full");
                result = {next.workspace_id, "worker_" + mint(16), mint(32)};
                next.workers.emplace(result.worker_id,
                                     worker{result.worker_id, std::move(name), content::hash_of(result.secret).str(), std::move(actor), at, 0, std::nullopt});
            });
            return result;
        }

        auto authenticate(std::string_view workspace, std::string_view id, std::string_view secret, std::int64_t at = now()) -> std::optional<record> {
            std::optional<record> result;
            transact([&](document& next) {
                auto found = next.workers.find(std::string{id});
                if (workspace != next.workspace_id || found == next.workers.end() || !secret_matches(found->second.secret_hash, secret))
                    throw refused(401, "worker_authentication_failed", "Worker credentials were refused");
                if (found->second.revoked_at)
                    throw refused(401, "worker_revoked", "Worker identity was revoked");
                found->second.last_connected_at = at;
                result = public_record(found->second);
            });
            return result;
        }

        auto active(std::string_view id) const -> bool {
            std::lock_guard held{mutex_};
            auto found = data_.workers.find(std::string{id});
            return !poisoned_ && found != data_.workers.end() && !found->second.revoked_at;
        }
        void revoke(std::string_view id, std::int64_t at = now()) {
            transact([&](document& next) {
                auto& worker = lookup(next, id);
                if (!worker.revoked_at)
                    worker.revoked_at = at;
            });
        }
        void rename(std::string_view id, std::string name) {
            check_name(name);
            transact([&](document& next) { lookup(next, id).name = std::move(name); });
        }
        auto list() const -> std::vector<record> {
            std::lock_guard held{mutex_};
            healthy();
            std::vector<record> out;
            for (const auto& [_, worker] : data_.workers)
                out.push_back(public_record(worker));
            return out;
        }
        auto workspace_id() const -> std::string {
            std::lock_guard held{mutex_};
            return data_.workspace_id;
        }

    private:
        static void check_name(std::string_view name) {
            if (name.empty() || name.size() > 128 || std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; }))
                throw refused(400, "invalid_worker_name", "Worker name must contain 1 to 128 bytes without control characters");
        }
        static auto public_record(const worker& w) -> record { return {w.worker_id, w.name, w.created_by, w.created_at, w.last_connected_at, w.revoked_at}; }
        static auto lookup(document& data, std::string_view id) -> worker& {
            auto found = data.workers.find(std::string{id});
            if (found == data.workers.end())
                throw refused(404, "worker_not_found", "Worker identity was not found");
            return found->second;
        }
        void healthy() const {
            if (poisoned_)
                throw refused(503, "worker_registry_unavailable", "Worker registry requires restart after a persistence failure");
        }
        template <class F> void transact(F change) {
            std::lock_guard held{mutex_};
            healthy();
            if (data_.workspace_id.empty())
                throw refused(503, "worker_registry_unavailable", "Core workspace identity is not configured");
            auto next = data_;
            change(next);
            try {
                durability::durable_private_write(file_, rfl::json::write(next));
            } catch (...) {
                poisoned_ = true;
                throw refused(503, "worker_registry_unavailable", "Worker registry persistence failed");
            }
            data_ = std::move(next);
            if (changed)
                changed();
        }
        std::filesystem::path directory_;
        durability::dir_lock lock_;
        std::filesystem::path file_;
        mutable std::mutex mutex_;
        document data_;
        bool poisoned_ = false;
    };
} // namespace enrollment
