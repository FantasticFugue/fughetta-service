export module usage;
import utility;
import std;
import reflection;
import configuration;
import durability;

export namespace usage {
    inline auto now() -> std::int64_t { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    inline auto day_of(std::int64_t t) -> std::int64_t {
        auto q = t / 86400;
        return q - (t % 86400 < 0); // floor division without t - 86399, which overflows near INT64_MIN
    }
    inline auto month_start(std::int64_t day) -> std::int64_t {
        std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{day}}};
        return std::chrono::sys_days{ymd.year() / ymd.month() / 1}.time_since_epoch().count();
    }
    inline auto date_of(std::int64_t day) -> std::string { return std::format("{:%F}", std::chrono::sys_days{std::chrono::days{day}}); }

    struct build_event {
        std::int64_t at = 0;
        std::string project;
        std::string target;
        std::string worker;
        std::optional<std::string> worker_id;
        bool ok = false;
        std::int32_t exit_code = -1;
        bool timed_out = false;
        double seconds = 0;
        std::uint64_t files = 0;
        std::uint64_t blobs_sent = 0;
        std::uint64_t bytes_sent = 0;
        std::string error;
        std::string workspace_id, worldline, version, snapshot, operation, actor;
        std::string error_code;
    };

    struct tool_event {
        std::int64_t at = 0;
        std::string project;
        std::string tool;
        bool error = false;
        double seconds = 0;
    };

    // A caller's observation is separate from the one execution shared by equal inputs.
    struct call_event {
        std::int64_t at = 0;
        std::string project, configuration, operation, actor, worldline, version, snapshot, calculation;
        std::string reuse = "executed", state = "cancelled", parameters;
        double seconds = 0;
        builds::timings timings;
    };

    // One line of the ledger. `seq` grows by one per event, so a reader resumes after the last one it has.
    struct event {
        std::uint64_t seq = 0;
        std::optional<build_event> build;
        std::optional<tool_event> tool;
        std::optional<call_event> call;
    };

    // Events after a cursor. `oldest` is the first event still kept; a reader whose cursor is older has missed what lies between.
    struct page {
        std::vector<event> events;
        std::uint64_t next = 0;
        std::uint64_t oldest = 0;
    };

    struct quota_exceeded : std::runtime_error, util::retained_error<quota_exceeded> {
        using std::runtime_error::runtime_error;
    };

    struct day {
        std::string date;
        std::string project;
        std::uint32_t builds = 0;
        std::uint32_t failed_builds = 0;
        double build_seconds = 0;
        std::uint64_t bytes_synced = 0;
        std::uint32_t tool_calls = 0;
        std::uint32_t tool_errors = 0;
    };

    struct project_totals {
        std::uint64_t builds_ok = 0;
        std::uint64_t builds_failed = 0;
        std::uint64_t builds_not_run = 0;
        std::uint64_t builds_unknown = 0, builds_cancelled = 0;
        double build_seconds = 0;
        std::uint64_t bytes_synced = 0;
    };
    struct tool_totals {
        std::uint64_t calls = 0;
        std::uint64_t errors = 0;
    };
    struct totals {
        std::map<std::string, project_totals> projects;
        std::map<std::string, tool_totals> tools;
    };

    class ledger {
    public:
        explicit ledger(std::filesystem::path dir, std::uint32_t keep_days = 32) : dir_(std::move(dir)), keep_days_(keep_days) {
            std::filesystem::create_directories(dir_);
            replay();
        }

        std::function<void()> changed;

        // Admits a build within the rate and time quotas; how many run at once is the backlog's to keep.
        auto admit_build(const std::string& project, const config::effective_limits& limits) -> std::uint64_t {
            if (auto n = builds_last_hour(project); n >= limits.builds_per_hour)
                throw quota_exceeded(std::format("{} builds of '{}' in the last hour (limit {})", n, project, limits.builds_per_hour));
            if (auto s = build_seconds_today(); s >= limits.build_seconds_per_day)
                throw quota_exceeded(std::format("{:.0f} build seconds used today (limit {})", s, limits.build_seconds_per_day));
            if (auto s = build_seconds_this_month(); s >= limits.build_seconds_per_month)
                throw quota_exceeded(std::format("{:.0f} build seconds used this month (limit {})", s, limits.build_seconds_per_month));
            auto ticket = next_ticket_++;
            running_.emplace(ticket, project);
            if (changed)
                changed();
            return ticket;
        }

        void release(std::uint64_t ticket) {
            if (running_.erase(ticket) && changed)
                changed();
        }

        void record(std::uint64_t ticket, build_event e) {
            running_.erase(ticket);
            if (!e.at)
                e.at = now();
            append(event{.seq = next_seq_++, .build = e}, e.at);
            apply(e);
            if (changed)
                changed();
        }

        void record(tool_event e) {
            if (!e.at)
                e.at = now();
            append(event{.seq = next_seq_++, .tool = e}, e.at);
            apply(e);
            if (changed)
                changed();
        }

        void record(call_event e) {
            if (!e.at)
                e.at = now();
            append(event{.seq = next_seq_++, .call = e}, e.at);
            apply(e);
            if (changed)
                changed();
        }

        auto running() const -> std::uint32_t { return static_cast<std::uint32_t>(running_.size()); }

        auto builds_last_hour(std::string_view project) const -> std::uint32_t {
            auto since = now() - 3600;
            std::uint32_t n = 0;
            for (auto it = builds_.rbegin(); it != builds_.rend() && it->at >= since; ++it)
                if (it->project == project && (it->error.empty() || it->error_code == "outcome_unknown" || it->error_code == "cancelled"))
                    ++n;
            return n;
        }

        auto build_seconds_today() const -> double {
            auto today = day_of(now());
            double s = 0;
            for (auto& [key, d] : days_)
                if (key.first == today)
                    s += d.build_seconds;
            return s;
        }

        auto build_seconds_this_month() const -> double {
            auto first = month_start(day_of(now()));
            double s = 0;
            for (auto& [key, d] : days_)
                if (key.first >= first)
                    s += d.build_seconds;
            return s;
        }

        auto counters() const -> const totals& { return totals_; }

        auto recovery() const -> const std::optional<std::string>& { return recovery_; }

        auto days(std::uint32_t n, std::optional<std::string_view> project = {}) const -> std::vector<day> {
            auto first = day_of(now()) - static_cast<std::int64_t>(n) + 1;
            std::vector<day> out;
            for (auto& [key, d] : days_)
                if (key.first >= first && (!project || key.second == *project))
                    out.push_back(d);
            return out;
        }

        auto builds(std::optional<std::string_view> project, std::size_t n) const -> std::vector<build_event> {
            std::vector<build_event> out;
            for (auto it = builds_.rbegin(); it != builds_.rend() && out.size() < n; ++it)
                if (!project || it->project == *project)
                    out.push_back(*it);
            return out;
        }

        auto calls(std::optional<std::string_view> project, std::size_t n) const -> std::vector<call_event> {
            std::vector<call_event> out;
            for (auto it = calls_.rbegin(); it != calls_.rend() && out.size() < n; ++it)
                if ((!project || it->project == *project) && it->at >= now() - static_cast<std::int64_t>(keep_days_) * 86400)
                    out.push_back(*it);
            return out;
        }

        // Up to `limit` events after `after`, read from the log where the first of them starts.
        auto events(std::uint64_t after, std::size_t limit) -> page {
            prune();
            page out{.next = after, .oldest = index_.empty() ? next_seq_ : index_.front().seq};
            auto first = std::ranges::upper_bound(index_, after, {}, &mark::seq);
            if (first == index_.end() || !limit)
                return out;
            std::ifstream in{log_path(), std::ios::binary};
            in.seekg(static_cast<std::streamoff>(first->offset));
            std::string line;
            while (out.events.size() < limit && std::getline(in, line)) {
                auto e = rfl::json::read<event, rfl::DefaultIfMissing>(line);
                if (!e || (!e->build && !e->tool && !e->call))
                    continue;
                out.next = e->seq;
                out.events.push_back(std::move(*e));
            }
            return out;
        }

    private:
        // Where each kept event starts in the log, in the order they were written; seq grows along it.
        struct mark {
            std::uint64_t seq = 0, offset = 0;
            std::int64_t at = 0;
        };

        auto log_path() const -> std::filesystem::path { return dir_ / "events.jsonl"; }
        auto cutoff() const -> std::int64_t { return now() - static_cast<std::int64_t>(keep_days_) * 86400; }

        void append(const event& e, std::int64_t at) {
            auto line = rfl::json::write(e) + '\n';
            durability::durable_append(log_path(), line);
            index_.push_back({e.seq, size_, at});
            size_ += line.size();
            if (now() - compacted_at_ >= 86400)
                compact();
        }

        void prune() {
            auto oldest = cutoff();
            while (!index_.empty() && index_.front().at < oldest)
                index_.pop_front();
        }

        // Drops what is past keeping from the log once a day: the kept events are its tail, written again from where the first starts.
        // A log with nothing kept holds a line with only the last number, so numbering goes on.
        void compact() {
            prune();
            std::string tail;
            if (index_.empty()) {
                if (next_seq_ > 1)
                    tail = rfl::json::write(event{.seq = next_seq_ - 1}) + '\n';
            } else {
                auto base = index_.front().offset;
                std::ifstream in{log_path(), std::ios::binary};
                in.seekg(static_cast<std::streamoff>(base));
                tail.assign(std::istreambuf_iterator<char>{in}, {});
                for (auto& m : index_)
                    m.offset -= base;
            }
            durability::durable_write(log_path(), tail, false, ".tmp");
            size_ = tail.size();
            compacted_at_ = now();
        }

        void replay() {
            auto cutoff = this->cutoff();
            std::string text;
            {
                std::ifstream in{log_path(), std::ios::binary};
                if (!in && std::filesystem::exists(log_path()))
                    throw std::runtime_error(std::format("cannot read {}", log_path().string()));
                text.assign(std::istreambuf_iterator<char>{in}, {});
            }
            std::vector<event> kept;
            std::size_t bad = 0, torn = 0;
            for (std::size_t pos = 0; pos < text.size();) {
                auto end = text.find('\n', pos);
                bool complete = end != std::string::npos;
                std::string_view line{text.data() + pos, (complete ? end : text.size()) - pos};
                pos = complete ? end + 1 : text.size();
                if (line.empty())
                    continue;
                auto e = rfl::json::read<event, rfl::DefaultIfMissing>(line);
                if (!e) {
                    (complete ? bad : torn) += 1;
                    continue;
                }
                next_seq_ = std::max(next_seq_, e->seq + 1);
                auto at = e->build ? e->build->at : e->tool ? e->tool->at : e->call ? e->call->at : 0;
                if (at < cutoff)
                    continue;
                if (e->build)
                    apply(*e->build);
                if (e->tool)
                    apply(*e->tool);
                if (e->call)
                    apply(*e->call);
                kept.push_back(std::move(*e));
            }
            if (bad) {
                auto evidence = log_path();
                evidence += std::format(".corrupt-{}", now());
                durability::durable_write(evidence, text);
                recovery_ =
                    std::format("{} unreadable line{} in {}; the log as found is kept in {}", bad, bad == 1 ? "" : "s", log_path().string(), evidence.string());
            } else if (torn) {
                recovery_ = std::format("dropped a torn last line in {} (a crash while it was written)", log_path().string());
            }
            // When the last number is not among the kept events, a line with only that number keeps the numbering going.
            if (next_seq_ > 1 && (kept.empty() || kept.back().seq != next_seq_ - 1))
                kept.push_back(event{.seq = next_seq_ - 1});
            std::string out;
            for (auto& e : kept) {
                auto at = e.build ? e.build->at : e.tool ? e.tool->at : e.call ? e.call->at : 0;
                if (e.build || e.tool || e.call)
                    index_.push_back({e.seq, out.size(), at});
                out += rfl::json::write(e) + '\n';
            }
            durability::durable_write(log_path(), out, false, ".tmp");
            size_ = out.size();
            compacted_at_ = now();
        }

        auto day_for(std::int64_t at, const std::string& project) -> day& {
            auto key = std::pair{day_of(at), project};
            auto [it, fresh] = days_.try_emplace(key);
            if (fresh) {
                it->second.date = date_of(key.first);
                it->second.project = project;
            }
            return it->second;
        }

        void apply(const build_event& e) {
            builds_.push_back(e);
            if (builds_.size() > max_builds_kept)
                builds_.pop_front();
            auto& d = day_for(e.at, e.project);
            ++d.builds;
            if (!e.ok)
                ++d.failed_builds;
            d.build_seconds += e.seconds;
            d.bytes_synced += e.bytes_sent;
            auto& t = totals_.projects[e.project];
            if (e.error_code == "outcome_unknown")
                ++t.builds_unknown;
            else if (e.error_code == "cancelled")
                ++t.builds_cancelled;
            else
                (e.ok ? t.builds_ok : !e.error.empty() ? t.builds_not_run : t.builds_failed) += 1;
            t.build_seconds += e.seconds;
            t.bytes_synced += e.bytes_sent;
        }

        void apply(const tool_event& e) {
            auto& d = day_for(e.at, e.project);
            ++d.tool_calls;
            if (e.error)
                ++d.tool_errors;
            auto& t = totals_.tools[e.tool];
            ++t.calls;
            if (e.error)
                ++t.errors;
        }

        void apply(const call_event& e) {
            calls_.push_back(e);
            if (calls_.size() > max_builds_kept) calls_.pop_front();
        }

        static constexpr std::size_t max_builds_kept = 10000;

        std::filesystem::path dir_;
        std::uint32_t keep_days_;
        std::optional<std::string> recovery_;
        std::deque<build_event> builds_;
        std::deque<call_event> calls_;
        std::map<std::pair<std::int64_t, std::string>, day> days_;
        std::map<std::uint64_t, std::string> running_;
        totals totals_;
        std::uint64_t next_ticket_ = 1;
        std::uint64_t next_seq_ = 1;
        std::deque<mark> index_;
        std::uint64_t size_ = 0;
        std::int64_t compacted_at_ = 0;
    };
} // namespace usage
