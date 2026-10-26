export module allocation;
import std;

export namespace allocation {
    struct budget {
        std::uint32_t threads = 10;
        std::uint32_t jobs = 3;
        std::uint32_t workers = 2;
        friend auto operator==(const budget&, const budget&) -> bool = default;
    };
    struct worker {
        std::uint32_t parallel;
    };
    struct plan {
        std::uint32_t gateway_io = 1, gateway_disk = 2;
        std::uint32_t source_io = 1, source_disk = 2;
        std::vector<worker> workers;
    };
    inline auto distribute(budget b) -> plan {
        if (b.threads < 6 || !b.jobs || b.workers >= b.jobs || b.workers > (b.threads - 6) / 2)
            throw std::invalid_argument("runtime requires threads >= 6 + 2 * workers, jobs >= 1, and workers < jobs (one job is reserved for source preparation)");
        plan result;
        auto remaining = b.threads - 6 - 2 * b.workers;
        result.gateway_disk += remaining / 2;
        result.source_disk += remaining - remaining / 2;
        for (std::uint32_t i = 0; i < b.workers; ++i)
            result.workers.push_back({(b.jobs - 1) / b.workers + (i < (b.jobs - 1) % b.workers ? 1u : 0u)});
        return result;
    }
    struct observation {
        std::uint64_t revision = 0;
        budget desired;
        std::uint64_t applied_revision = 0;
        budget applied;
        std::string phase;
        std::uint64_t active_requests = 0;
    };

    // Tracks admitted requests until their responses finish. Control requests stay available during draining.
    class control {
        mutable std::mutex mutex_;
        budget applied_;
        std::uint64_t revision_;
        std::string phase_ = "starting";
        std::uint64_t active_ = 0, controls_ = 0;

        void leave(bool administrative) {
            std::lock_guard held{mutex_};
            --(administrative ? controls_ : active_);
        }
    public:
        control(budget applied, std::uint64_t revision) : applied_(applied), revision_(revision) {}
        class lease {
            control* owner_;
            bool administrative_;
        public:
            lease(control& owner, bool administrative) : owner_(&owner), administrative_(administrative) {}
            lease(const lease&) = delete;
            lease(lease&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), administrative_(other.administrative_) {}
            ~lease() { if (owner_) owner_->leave(administrative_); }
        };
        auto enter(bool administrative) -> std::optional<lease> {
            std::lock_guard held{mutex_};
            if (!administrative && phase_ != "ready") return {};
            ++(administrative ? controls_ : active_);
            return lease{*this, administrative};
        }
        void ready(std::uint64_t revision) { std::lock_guard held{mutex_}; revision_ = revision; phase_ = "ready"; }
        void drain() { std::lock_guard held{mutex_}; phase_ = "draining"; }
        auto idle() const -> bool { std::lock_guard held{mutex_}; return active_ == 0 && controls_ == 0; }
        auto observe(std::uint64_t revision, budget desired) const -> observation {
            std::lock_guard held{mutex_};
            auto phase = phase_ == "ready" && desired != applied_ ? "pending" : phase_;
            return {revision, desired, revision_, applied_, phase, active_};
        }
    };

}
