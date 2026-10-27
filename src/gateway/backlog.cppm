module tools;
import std;
import native;
import coroutine;
import configuration;
import world;

namespace tools {
    namespace {
        // Finished builds kept for equal requests.
        constexpr std::size_t cached_results = 256;
        // Finished jobs the list still shows.
        constexpr std::size_t finished_shown = 20;
        constexpr auto busy_pause = std::chrono::seconds{1};
    } // namespace

    auto backlog::submit(std::size_t here, entry e, runner r) -> coroutine::task<std::shared_ptr<ticket>> {
        auto shared = std::make_shared<runner>(std::move(r));
        if (here == 0)
            co_return enqueue(std::move(e), std::move(shared));
        co_return co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(0),
                                       [this, e, shared]() -> coroutine::task<std::shared_ptr<ticket>> { co_return enqueue(e, shared); });
    }

    auto backlog::list(std::size_t here) -> coroutine::task<std::vector<view>> {
        if (here == 0)
            co_return views();
        co_return co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(0), [this]() -> coroutine::task<std::vector<view>> { co_return views(); });
    }

    auto backlog::cancel(std::size_t here, std::uint64_t id, worlds::actor who) -> coroutine::task<std::string> {
        if (here == 0)
            co_return withdraw(id, who);
        co_return co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(0),
                                       [this, id, who]() -> coroutine::task<std::string> { co_return withdraw(id, who); });
    }

    auto backlog::serve(native::scheduler& io) -> coroutine::task<> {
        for (;;) {
            step();
            co_await execution::schedule_after(io, jobs_.empty() ? std::chrono::milliseconds{500} : std::chrono::milliseconds{50});
        }
    }

    auto backlog::enqueue(entry e, std::shared_ptr<runner> r) -> std::shared_ptr<ticket> {
        // The same asker's older request for the same project, configuration and operation has not started: this one replaces it.
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            auto j = *it++;
            if (!j->running && j->what.key != e.key && j->what.project == e.project && j->what.configuration == e.configuration &&
                j->what.operation == e.operation && std::ranges::all_of(j->tickets, [&](auto& t) { return t->actor_ == e.actor; }))
                finish(j, refused(j->what, "replaced by a newer request for the same configuration"), "replaced");
        }
        auto held = std::make_shared<ticket>(e.actor);
        if (e.cacheable && !e.fresh)
            if (auto hit = std::ranges::find(cache_, e.key, &std::pair<std::string, outcome>::first); hit != cache_.end()) {
                cache_.splice(cache_.begin(), cache_, hit);
                held->result_ = hit->second;
                held->done_.store(true, std::memory_order_release);
                return held;
            }
        if (auto same = std::ranges::find_if(jobs_, [&](auto& j) { return j->what.key == e.key; }); same != jobs_.end()) {
            held->job_ = (*same)->id;
            held->progress_ = (*same)->progress;
            (*same)->tickets.push_back(held);
            return held;
        }
        auto j = std::make_shared<job>();
        j->id = next_id_++;
        j->what = std::move(e);
        j->run = std::move(*r);
        held->job_ = j->id;
        held->progress_ = j->progress;
        j->tickets.push_back(held);
        // A new asker comes before those who have had turns.
        if (!std::ranges::contains(turns_, j->what.actor))
            turns_.push_front(j->what.actor);
        jobs_.push_back(std::move(j));
        step();
        return held;
    }

    void backlog::step() {
        // A job nobody waits for any more is dropped while queued and stopped while running.
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            auto j = *it++;
            std::erase_if(j->tickets, [](auto& t) { return t->left_.load(std::memory_order_acquire); });
            if (!j->tickets.empty())
                continue;
            if (j->running)
                j->stop.request_stop();
            else
                finish(j, refused(j->what, "cancelled"), "cancelled");
        }
        auto cap = config::resolve_limits(*zero_.snapshot()).concurrent_builds;
        while (running_ < cap) {
            auto j = next();
            if (!j)
                break;
            j->running = true;
            ++running_;
            rt_.spawn(0, launch(j));
        }
        std::size_t ahead = 0;
        for (auto& j : jobs_) {
            if (j->running)
                continue;
            for (auto& t : j->tickets)
                t->ahead_.store(ahead, std::memory_order_relaxed);
            ++ahead;
        }
    }

    // The oldest job of whoever's turn it is that may be tried now; that asker then waits for everyone else's turn.
    auto backlog::next() -> std::shared_ptr<job> {
        auto now = std::chrono::steady_clock::now();
        for (std::size_t n = 0; n < turns_.size(); ++n) {
            auto actor = turns_.front();
            turns_.pop_front();
            turns_.push_back(actor);
            auto found = std::ranges::find_if(jobs_, [&](auto& j) { return !j->running && j->what.actor == actor && j->retry_at <= now; });
            if (found != jobs_.end())
                return *found;
        }
        return nullptr;
    }

    // The free tree of the job's set whose last build shared the most files with it, the least recently used of equals; a new one when all
    // are busy.
    auto backlog::choose(const entry& e) -> tree& {
        auto& set = trees_[e.trees];
        tree* best = nullptr;
        std::size_t best_shared = 0;
        for (auto& t : set) {
            if (t.busy)
                continue;
            auto shared = static_cast<std::size_t>(std::ranges::count_if(e.files, [&](auto& f) { return t.files.contains(f); }));
            if (!best || shared > best_shared || (shared == best_shared && t.used < best->used)) {
                best = &t;
                best_shared = shared;
            }
        }
        if (!best)
            best = &set.emplace_back(tree{.name = std::format("{}-{}", e.trees, set.size())});
        return *best;
    }

    auto backlog::launch(std::shared_ptr<job> j) -> coroutine::task<> {
        // Only jobs that build take a tree.
        auto* tree = j->what.trees.empty() ? nullptr : &choose(j->what);
        if (tree)
            tree->busy = true;
        std::optional<std::optional<outcome>> out;
        std::string failure;
        try {
            out = co_await coroutine::under(j->stop.get_token(), j->run(zero_, *j->progress, tree ? tree->name : std::string{}));
        } catch (const std::exception& e) {
            failure = e.what();
        }
        if (tree) {
            tree->busy = false;
            // A tree keeps what a worker built in it. A job that was stopped or failed may have changed it on the way, so its last build
            // no longer says what it holds; a job that found every worker busy never reached it.
            auto* built = out && *out ? std::get_if<config_result>(&**out) : nullptr;
            if (built && !built->worker.empty()) {
                tree->files = {j->what.files.begin(), j->what.files.end()};
                tree->used = ++clock_;
            } else if (!out || !failure.empty() || built)
                tree->files.clear();
        }
        --running_;
        if (!failure.empty())
            finish(j, refused(j->what, std::move(failure)), "done");
        else if (!out)
            finish(j, refused(j->what, "cancelled"), "cancelled");
        else if (!*out) {
            j->running = false;
            j->retry_at = std::chrono::steady_clock::now() + busy_pause;
        } else
            finish(j, std::move(**out), "done");
        step();
    }

    void backlog::finish(const std::shared_ptr<job>& j, outcome r, std::string state) {
        auto* built = std::get_if<config_result>(&r);
        if (state == "done" && j->what.cacheable && built && built->error.empty() && !built->worker.empty()) {
            std::erase_if(cache_, [&](auto& c) { return c.first == j->what.key; });
            cache_.emplace_front(j->what.key, r);
            if (cache_.size() > cached_results)
                cache_.pop_back();
        }
        auto ok = built ? built->ok : std::get<language::result>(r).status == "prepared";
        finished_.push_front({.id = j->id,
                              .project = j->what.project,
                              .configuration = j->what.configuration,
                              .operation = j->what.operation,
                              .state = std::move(state),
                              .ok = ok,
                              .waiting = j->tickets.size()});
        if (finished_.size() > finished_shown)
            finished_.pop_back();
        for (auto& t : j->tickets) {
            t->result_ = r;
            t->done_.store(true, std::memory_order_release);
        }
        jobs_.remove(j);
        std::erase_if(turns_, [&](auto& actor) { return std::ranges::none_of(jobs_, [&](auto& o) { return o->what.actor == actor; }); });
    }

    auto backlog::views() const -> std::vector<view> {
        std::vector<view> out;
        std::size_t ahead = 0;
        for (auto& j : jobs_) {
            out.push_back({.id = j->id,
                           .project = j->what.project,
                           .configuration = j->what.configuration,
                           .operation = j->what.operation,
                           .state = j->running ? "running" : "queued",
                           .waiting = j->tickets.size(),
                           .ahead = j->running ? 0 : ahead});
            if (!j->running)
                ++ahead;
        }
        out.insert(out.end(), finished_.begin(), finished_.end());
        return out;
    }

    auto backlog::withdraw(std::uint64_t id, const worlds::actor& who) -> std::string {
        auto found = std::ranges::find_if(jobs_, [id](auto& j) { return j->id == id; });
        if (found == jobs_.end())
            throw config::not_found(std::format("no queued or running job {}", id));
        auto j = *found;
        auto asker = worlds::identity_key(who);
        if (worlds::rank(who.role) < worlds::rank("owner") && !std::ranges::all_of(j->tickets, [&](auto& t) { return t->actor_ == asker; }))
            throw tool_error(nodes::code::permission_denied, "another caller waits for this job too; only an owner cancels it");
        if (j->running) {
            j->stop.request_stop();
            return "stopping";
        }
        finish(j, refused(j->what, "cancelled"), "cancelled");
        return "cancelled";
    }
} // namespace tools
