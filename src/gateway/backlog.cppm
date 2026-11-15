module tools;
import std;
import native;
import coroutine;
import configuration;
import world;
import codec;

namespace tools {
    namespace {
        // Finished builds kept for equal requests.
        constexpr std::size_t cached_results = 256;
        // Finished jobs the list still shows.
        constexpr std::size_t finished_shown = 20;
        constexpr auto busy_pause = std::chrono::seconds{1};

        auto contains(const semantic::extent& available, const semantic::extent& requested) -> bool {
            auto subset = [](const auto& all, const auto& selected) {
                return std::ranges::all_of(selected, [&](const auto& value) { return std::ranges::contains(all, value); });
            };
            // Providers interpret an empty selector, including facts, as all.
            auto scope = [&](const auto& all, const auto& selected) { return all.empty() || (!selected.empty() && subset(all, selected)); };
            return scope(available.modules, requested.modules) && scope(available.files, requested.files) && scope(available.contexts, requested.contexts) &&
                   scope(available.facts, requested.facts);
        }

        auto includes(const backlog::entry& available, const backlog::entry& requested) -> bool {
            if (available.key == requested.key)
                return true;
            if (requested.operation != "prepare" || available.operation != "prepare" || requested.family.empty() || available.family != requested.family ||
                available.project != requested.project || requested.demands.empty())
                return false;
            return std::ranges::all_of(requested.demands, [&](const auto& item) {
                auto found = available.demands.find(item.first);
                return found != available.demands.end() && contains(found->second, item.second);
            });
        }

        auto overlaps(const backlog::entry& left, const backlog::entry& right) -> bool {
            if (left.operation != "prepare" || right.operation != "prepare" || left.family.empty() || left.family != right.family ||
                left.project != right.project)
                return false;
            auto intersects = [](const auto& a, const auto& b) {
                return a.empty() || b.empty() || std::ranges::any_of(a, [&](const auto& value) { return std::ranges::contains(b, value); });
            };
            return std::ranges::any_of(right.demands, [&](const auto& item) {
                auto found = left.demands.find(item.first);
                return found != left.demands.end() && intersects(found->second.files, item.second.files) &&
                       intersects(found->second.modules, item.second.modules) && intersects(found->second.contexts, item.second.contexts);
            });
        }
    } // namespace

    auto backlog::submit(std::size_t here, entry e, runner r) -> coroutine::task<std::shared_ptr<ticket>> {
        auto shared = std::make_shared<runner>(std::move(r));
        if (here == 0)
            co_return enqueue(std::move(e), std::move(shared));
        struct submit_work {
            backlog* owner;
            entry value;
            std::shared_ptr<runner> run;
            auto operator()() -> coroutine::task<std::shared_ptr<ticket>> { co_return owner->enqueue(value, run); }
        };
        co_return co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(0), submit_work{this, std::move(e), std::move(shared)});
    }

    auto backlog::list(std::size_t here) -> coroutine::task<std::vector<view>> {
        if (here == 0)
            co_return views();
        struct list_work {
            backlog* owner;
            auto operator()() -> coroutine::task<std::vector<view>> { co_return owner->views(); }
        };
        co_return co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(0), list_work{this});
    }

    auto backlog::forget(std::size_t here, std::string key) -> coroutine::task<> {
        if (here != 0) {
            struct forget_work {
                backlog* owner;
                std::string key;
                auto operator()() { return owner->forget(0, key); }
            };
            co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(0), forget_work{this, std::move(key)});
            co_return;
        }
        std::erase_if(cache_, [&](const auto& item) { return item.first == key; });
    }

    auto backlog::cancel(std::size_t here, std::uint64_t id, worlds::actor who) -> coroutine::task<std::string> {
        if (here == 0)
            co_return withdraw(id, who);
        struct cancel_work {
            backlog* owner;
            std::uint64_t id;
            worlds::actor who;
            auto operator()() -> coroutine::task<std::string> { co_return owner->withdraw(id, who); }
        };
        co_return co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(0), cancel_work{this, id, std::move(who)});
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
            if (e.operation == "build" && j->what.operation == "build" && e.cacheable && j->what.cacheable && !j->what.fresh && !j->running &&
                j->what.key != e.key && j->what.project == e.project && j->what.configuration == e.configuration && j->what.operation == e.operation &&
                j->what.worldline == e.worldline && std::ranges::all_of(j->tickets, [&](auto& t) { return t->actor_ == e.actor; }))
                finish(j, refused(j->what, "replaced by a newer request for the same configuration"), "replaced");
        }
        auto held = std::make_shared<ticket>(e.actor);
        if (e.cacheable && !e.fresh)
            if (auto hit = std::ranges::find(cache_, e.key, &std::pair<std::string, outcome>::first); hit != cache_.end()) {
                cache_.splice(cache_.begin(), cache_, hit);
                held->reuse_ = "cache";
                held->result_ = hit->second;
                std::visit(
                    [](auto& result) {
                        result.timings = {.sync_seconds = 0,
                                          .queue_seconds = 0,
                                          .dependency_seconds = 0,
                                          .worker_seconds = 0,
                                          .preparation_seconds = 0,
                                          .execution_seconds = 0};
                    },
                    held->result_);
                held->done_.store(true, std::memory_order_release);
                return held;
            }
        if (auto same = std::ranges::find_if(jobs_, [&](auto& j) { return !e.fresh && !j->stop.get_token().stop_requested() && includes(j->what, e); });
            same != jobs_.end()) {
            held->reuse_ = "shared";
            held->job_ = (*same)->id;
            held->progress_ = (*same)->progress;
            (*same)->tickets.push_back(held);
            return held;
        }
        auto j = std::make_shared<job>();
        j->id = next_id_++;
        j->computation = "computation-" + nodes::hex(nodes::random_bytes(16));
        j->what = std::move(e);
        j->run = std::move(*r);
        held->job_ = j->id;
        held->progress_ = j->progress;
        j->tickets.push_back(held);
        if (!j->what.fresh)
            for (const auto& previous : jobs_)
                if (!previous->stop.get_token().stop_requested() && overlaps(previous->what, j->what)) {
                    auto dependency = std::make_shared<ticket>(j->what.actor);
                    previous->tickets.push_back(dependency);
                    j->predecessors.push_back(std::move(dependency));
                }
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
            auto found = std::ranges::find_if(jobs_, [&](auto& j) {
                return !j->running && j->what.actor == actor && j->retry_at <= now &&
                       std::ranges::any_of(j->tickets, [](const auto& held) { return !held->left_.load(std::memory_order_acquire); }) &&
                       std::ranges::all_of(j->predecessors, [](const auto& held) { return held->done(); });
            });
            if (found != jobs_.end())
                return *found;
        }
        return nullptr;
    }

    // The free tree of the job's set whose last build shared the most files with it, the least recently used of equals; a new one when all
    // are busy.
    auto backlog::choose(const entry& e, const std::string& group) -> tree& {
        auto& set = trees_[group];
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
            best = &set.emplace_back(tree{.name = std::format("{}-{}", group, set.size())});
        return *best;
    }

    auto backlog::launch(std::shared_ptr<job> j) -> coroutine::task<> {
        j->predecessors.clear();
        // A busy retry retains these directories while releasing the dispatch
        // turn. Other computations can run without changing this graph's inputs.
        for (const auto& group : j->what.trees) {
            if (j->placed.contains(group))
                continue;
            auto& selected = choose(j->what, group);
            selected.busy = true;
            j->directories.push_back(&selected);
            j->placed.emplace(group, selected.name);
        }
        const auto started = std::chrono::steady_clock::now();
        for (auto& ticket : j->tickets)
            ticket->queued_ += std::max(0.0, std::chrono::duration<double>(started - std::max(j->queued_at, ticket->submitted_)).count());
        std::optional<std::optional<outcome>> out;
        std::string failure, failure_code;
        try {
            out = co_await coroutine::under(j->stop.get_token(), j->run(zero_, *j->progress, j->placed, j->computation));
        } catch (const tool_error& e) {
            failure = e.what();
            failure_code = e.code;
        } catch (const std::exception& e) {
            failure = e.what();
        }
        --running_;
        if (!failure.empty()) {
            auto result = refused(j->what, std::move(failure));
            if (!failure_code.empty())
                std::get<config_result>(result).error_code = std::move(failure_code);
            finish(j, std::move(result), "done");
        } else if (!out)
            finish(j, refused(j->what, "cancelled"), "cancelled");
        else if (!*out) {
            j->running = false;
            j->queued_at = std::chrono::steady_clock::now();
            j->retry_at = j->queued_at + busy_pause;
        } else
            finish(j, std::move(**out), "done");
        step();
    }

    void backlog::finish(const std::shared_ptr<job>& j, outcome r, std::string state) {
        for (const auto& held : j->predecessors)
            held->leave();
        j->predecessors.clear();
        auto* built = std::get_if<config_result>(&r);
        auto* analyzed = std::get_if<language::result>(&r);
        const bool synced =
            built ? !built->worker.empty() && built->ok : !analyzed->worker.empty() && analyzed->status == "prepared" && analyzed->analysis == "analyzed";
        for (auto* directory : j->directories) {
            directory->busy = false;
            if (synced) {
                directory->files = {j->what.files.begin(), j->what.files.end()};
                directory->used = ++clock_;
            } else
                directory->files.clear();
        }
        j->directories.clear();
        auto reusable = built ? built->error.empty() && !built->worker.empty()
                              : analyzed->status == "prepared" && analyzed->analysis == "analyzed" && !analyzed->worker.empty() && analyzed->missing.empty() &&
                                    analyzed->cacheable;
        if (built)
            reusable = reusable && built->export_error.empty() &&
                       std::ranges::all_of(built->models, [](const auto& file) { return file.delivery_error.empty(); }) &&
                       std::ranges::all_of(built->exports, [](const auto& file) { return file.delivery_error.empty(); }) &&
                       std::ranges::all_of(built->steps, [](const auto& step) {
                           return step.log_error.empty() && step.cache_error.empty() && (!step.log || step.log->delivery_error.empty());
                       });
        if (state == "done" && j->what.cacheable && reusable) {
            std::erase_if(cache_, [&](auto& c) { return c.first == j->what.key; });
            cache_.emplace_front(j->what.key, r);
            if (cache_.size() > cached_results)
                cache_.pop_back();
        }
        auto ok = built ? built->ok : std::get<language::result>(r).status == "prepared" && std::get<language::result>(r).analysis == "analyzed";
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
            auto queued = t->queued_;
            if (!j->running)
                queued += std::max(0.0, std::chrono::duration<double>(std::chrono::steady_clock::now() - std::max(j->queued_at, t->submitted_)).count());
            std::visit([&](auto& result) { result.timings.queue_seconds = queued; }, t->result_);
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
        auto asker = worlds::execution_key(who);
        if ((!who.run.empty() || worlds::rank(who.role) < worlds::rank("owner")) &&
            !std::ranges::all_of(j->tickets, [&](auto& t) { return t->actor_ == asker; }))
            throw tool_error(nodes::code::permission_denied, "another caller waits for this job too; only an owner cancels it");
        if (j->running) {
            j->stop.request_stop();
            return "stopping";
        }
        finish(j, refused(j->what, "cancelled"), "cancelled");
        return "cancelled";
    }
} // namespace tools
