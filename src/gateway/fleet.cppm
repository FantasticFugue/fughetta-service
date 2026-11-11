export module fleet;
import std;
import constants;
import enrollment;
import native;
import coroutine;
import protocol;
import configuration;
import content;
import utility;

export namespace routing {
    using clock = std::chrono::steady_clock;

    inline constexpr auto stale_after = std::chrono::seconds{6};

    namespace say {
        inline auto refused(const jobs::hello& h) -> std::string {
            return std::format("worker '{}' speaks protocol {:#x}, not {:#x}: refused", h.name, h.protocol, jobs::protocol_version);
        }
        inline auto unauthorized(const jobs::hello& h) -> std::string { return std::format("worker '{}' not admitted (wrong token)", h.name); }
        inline auto joined(const jobs::hello& h) -> std::string {
            return std::format("worker.connection connected workspace_id={} worker_id={} protocol={:#x}", h.workspace_id.value_or("local"),
                               h.worker_id.value_or("managed"), h.protocol);
        }
        inline auto left(const jobs::hello& h) -> std::string {
            return std::format("worker.connection disconnected workspace_id={} worker_id={} protocol={:#x} reason=channel_closed",
                               h.workspace_id.value_or("local"), h.worker_id.value_or("managed"), h.protocol);
        }
        inline auto silent(const jobs::hello& h, std::chrono::seconds for_) -> std::string {
            return std::format("worker '{}' silent for {}s: disconnecting", h.name, for_.count());
        }
    } // namespace say

    struct worker_view {
        std::string name;
        jobs::platform platform;
        std::uint64_t max_inflight = 0;
        std::size_t core = 0;
        std::uint64_t inflight = 0;
        std::uint64_t silent_ms = 0;
        bool online = true;
        std::vector<std::string> tools;
        std::vector<std::string> trees;
        std::optional<std::string> worker_id;
        std::optional<std::string> created_by;
        std::optional<std::int64_t> created_at, last_connected_at, revoked_at;
        bool managed = true;
        std::string toolchain_id;
        bool dependency_publisher = false;
    };

    struct requirements {
        std::vector<std::string> tools;
        std::optional<std::string> os;
        std::optional<std::string> arch;
        std::optional<std::string> toolchain_id;
        bool dependency_publisher = false;
        friend auto operator==(const requirements&, const requirements&) -> bool = default;
    };

    struct selection {
        requirements needs;
        std::string tree_key;
        friend auto operator==(const selection&, const selection&) -> bool = default;
    };

    // No worker takes a dispatch now. `busy` when one could but is at its dispatch limit, so the dispatch may wait for it.
    struct no_worker : std::runtime_error, util::retained_error<no_worker> {
        explicit no_worker(std::string why, bool busy = false) : std::runtime_error(std::move(why)), busy(busy) {}
        bool busy;
    };

    // A worker's first frame, or nothing when it does not arrive in time or is not
    // a hello. Kept out of the `admit` template so the deadline race is compiled
    // once in this module, keeping that work out of its importers.
    auto receive_hello(native::scheduler& io, native::socket& peer) -> coroutine::task<std::optional<jobs::hello>> {
        try {
            transport::channel<native::socket> probe{peer};
            auto first = co_await coroutine::deadline(io, std::chrono::seconds{10}, probe.recv(constants::worker_hello_bytes));
            if (!first || first->kind != jobs::code<jobs::hello>)
                co_return std::nullopt;
            co_return transport::decode<jobs::hello>(*first);
        } catch (const std::exception&) {
            co_return std::nullopt;
        }
    }

    class fleet {
    public:
        using socket = native::socket;
        struct closed {
            auto operator()() const -> bool { return false; }
        };

        // `attached` counts the members for other cores to read; `config`, when there is one, names the token a worker must present.
        fleet(std::atomic<int>& attached, const config::slots* config, std::size_t core, std::chrono::milliseconds stale = stale_after)
            : attached_(attached), config_(config), core_(core), stale_(stale) {}

        std::function<void()> changed;

        auto holds(const selection& sel) const -> bool { return pick(sel) != nullptr; }

        auto diagnose(const selection& sel) const -> std::string {
            auto now = clock::now();
            bool any_live = false, full = false, missing_tools = false, bad_platform = false;
            std::vector<std::string> missing;
            for (auto& m : members_) {
                if (stale(*m, now))
                    continue;
                if (m->hello.dependency_publisher != sel.needs.dependency_publisher)
                    continue;
                any_live = true;
                if (!tools_met(*m, sel.needs)) {
                    missing_tools = true;
                    for (auto& t : sel.needs.tools)
                        if (!std::ranges::contains(m->hello.tools, t) && !std::ranges::contains(missing, t))
                            missing.push_back(t);
                    continue;
                }
                if (!platform_met(*m, sel.needs)) {
                    bad_platform = true;
                    continue;
                }
                full = true;
            }
            if (members_.empty())
                return "no build machine is online; start `fug worker` on a machine of this workspace, or wake the one that serves it";
            if (missing_tools) {
                std::string joined;
                for (auto& t : missing)
                    joined += (joined.empty() ? "" : ", ") + t;
                return joined.empty() && sel.needs.toolchain_id ? "no attached worker has the selected toolchain fingerprint"
                                                                : std::format("no attached worker provides all required tools (missing: {})", joined);
            }
            if (bad_platform) {
                std::string want;
                if (sel.needs.os)
                    want += " os=" + *sel.needs.os;
                if (sel.needs.arch)
                    want += " arch=" + *sel.needs.arch;
                return std::format("no attached worker matches the required platform{}", want);
            }
            if (full)
                return "every attached worker is at its dispatch limit";
            if (any_live)
                return "no attached worker can take the dispatch now";
            return sel.needs.dependency_publisher ? "no live dependency publisher is available" : "no live general build worker is available";
        }

        // Whether a live worker meets the selection but has no room for another dispatch.
        auto busy(const selection& sel) const -> bool {
            auto now = clock::now();
            return std::ranges::any_of(members_, [&](auto& m) { return !stale(*m, now) && tools_met(*m, sel.needs) && platform_met(*m, sel.needs); }) &&
                   !holds(sel);
        }

        template <class Work> auto with(const selection& sel, Work work) -> std::invoke_result_t<Work&, transport::channel<socket>&> {
            auto m = pick(sel);
            if (!m)
                throw no_worker{diagnose(sel), busy(sel)};
            ++m->inflight;
            if (changed)
                changed();
            struct release {
                std::shared_ptr<member> m;
                fleet* owner;
                ~release() {
                    --m->inflight;
                    if (owner->changed)
                        owner->changed();
                }
            } guard{m, this};
            using yielded = typename std::invoke_result_t<Work&, transport::channel<socket>&>::value_type;
            if constexpr (std::is_void_v<yielded>) {
                co_await work(m->chan);
                if (!sel.tree_key.empty())
                    m->remember(sel.tree_key);
                m->last_used = clock::now();
                co_return;
            } else {
                auto out = co_await work(m->chan);
                if (!sel.tree_key.empty())
                    m->remember(sel.tree_key);
                m->last_used = clock::now();
                co_return std::move(out);
            }
        }

        // `enabled` says whether a remote worker may enroll now; by default none may.
        template <class Enabled = closed>
        auto admit(native::scheduler& io, socket peer, enrollment::store* registry = nullptr, execution::thread_pool* pool = nullptr, Enabled enabled = {})
            -> coroutine::task<> {
            auto received = co_await receive_hello(io, peer);
            if (!received)
                co_return;
            jobs::hello hello = std::move(*received);
            // The enrollment secret only lives as long as this admission; every exit
            // path below (refusals included) must leave no credential bytes behind.
            struct scrub {
                jobs::hello& held;
                ~scrub() {
                    std::ranges::fill(held.auth, '\0');
                    held.auth.clear();
                }
            } wipe{hello};
            if (hello.protocol != jobs::protocol_version) {
                logging::warn("worker.connection refused reason=worker_protocol_mismatch");
                if (registry)
                    co_await refuse(io, peer, "worker_protocol_mismatch");
                co_return;
            }
            if (registry) {
                std::string rejected;
                std::optional<enrollment::record> identity;
                if (!enabled())
                    rejected = "worker_enrollment_disabled";
                else if (!pool || !hello.worker_id || !hello.workspace_id || !hello.max_inflight || hello.max_inflight > 64 || !hello.heartbeat_seconds ||
                         hello.heartbeat_seconds > 30 || hello.tools.size() > 4096)
                    rejected = "worker_authentication_failed";
                else {
                    try {
                        identity = co_await io::blocking(io, *pool, [registry, workspace = *hello.workspace_id, id = *hello.worker_id, secret = hello.auth] {
                            return registry->authenticate(workspace, id, secret);
                        });
                        if (!identity || !registry->active(identity->worker_id))
                            rejected = "worker_revoked";
                    } catch (const enrollment::refused& error) {
                        rejected = error.code;
                    } catch (const std::exception&) {
                        rejected = "worker_registry_unavailable";
                    }
                }
                if (!rejected.empty()) {
                    logging::warn(std::format("worker.connection refused reason={}", rejected));
                    co_await refuse(io, peer, std::move(rejected));
                    co_return;
                }
                hello.name = identity->name;
                for (auto& member : members_)
                    if (member->hello.worker_id == hello.worker_id) {
                        member->retiring = true;
                        member->sock.shutdown(net::socket_base::shutdown_send);
                    }
            } else {
                auto token = config_ ? config_->current(core_)->worker_token : std::nullopt;
                if (hello.worker_id || hello.workspace_id || (token && !content::secure_equal(hello.auth, *token))) {
                    logging::warn("worker.connection refused reason=worker_authentication_failed");
                    co_return;
                }
            }
            // Clear before the hello is stored on the member; the scrub still covers
            // every refusal and exception path above.
            std::ranges::fill(hello.auth, '\0');
            hello.auth.clear();
            auto held = std::make_shared<member>(std::move(hello), std::move(peer));
            held->registry = registry;
            auto it = members_.insert(members_.end(), held);
            logging::info(say::joined(held->hello));
            attached_.fetch_add(1, std::memory_order_relaxed);
            if (changed)
                changed();
            struct leave {
                fleet* self;
                std::list<std::shared_ptr<member>>::iterator it;
                ~leave() {
                    logging::info(say::left((*it)->hello));
                    self->attached_.fetch_sub(1, std::memory_order_relaxed);
                    self->members_.erase(it);
                    if (self->changed)
                        self->changed();
                }
            } guard{this, it};
            try {
                if (registry)
                    co_await held->chan.post(jobs::admission{true, "", held->hello.worker_id.value_or("")});
                co_await held->chan.pump([held](const transport::frame& f) {
                    if (f.kind == jobs::code<jobs::heartbeat>)
                        held->seen = clock::now();
                });
            } catch (const std::exception&) {
            }
        }

        auto watch(native::scheduler& io) -> coroutine::task<> {
            for (;;) {
                co_await execution::schedule_after(io, std::min<std::chrono::milliseconds>(std::chrono::seconds{2}, stale_ / 2));
                auto now = clock::now();
                for (auto& m : members_)
                    if ((stale(*m, now) || (m->registry && !m->registry->active(m->hello.worker_id.value_or("")))) && m->sock.is_open()) {
                        logging::warn(say::silent(m->hello, std::chrono::duration_cast<std::chrono::seconds>(now - m->seen)));
                        m->sock.close();
                    }
            }
        }

        auto describe(std::size_t core) const -> std::vector<worker_view> {
            std::vector<worker_view> out;
            auto now = clock::now();
            for (auto& m : members_) {
                out.push_back({m->hello.name,
                               m->hello.platform,
                               m->hello.max_inflight,
                               core,
                               m->inflight,
                               static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m->seen).count()),
                               !stale(*m, now),
                               m->hello.tools,
                               {m->trees.begin(), m->trees.end()},
                               m->hello.worker_id});
                out.back().toolchain_id = m->hello.toolchain_id;
                out.back().dependency_publisher = m->hello.dependency_publisher;
            }
            return out;
        }

        auto identity(const transport::channel<socket>& channel) const -> std::pair<std::string, std::optional<std::string>> {
            for (const auto& member : members_)
                if (&member->chan == &channel)
                    return {member->hello.name, member->hello.worker_id};
            throw no_worker{"Worker connection is no longer attached"};
        }

        auto update_identity(native::scheduler& io, const std::string& id, const std::optional<std::string>& name) -> coroutine::task<> {
            std::vector<std::shared_ptr<member>> matched;
            for (auto& held : members_)
                if (held->hello.worker_id == std::optional{id})
                    matched.push_back(held);
            for (auto& held : matched) {
                if (name) {
                    held->hello.name = *name;
                    continue;
                }
                held->retiring = true;
                try {
                    co_await coroutine::deadline(io, std::chrono::seconds{2}, held->chan.post(jobs::admission{false, "worker_revoked", id}));
                } catch (const std::exception&) {
                }
                held->sock.shutdown(net::socket_base::shutdown_send);
            }
            if (!matched.empty() && changed)
                changed();
        }

    private:
        static auto refuse(native::scheduler& io, socket& peer, std::string code) -> coroutine::task<> {
            transport::channel<socket> channel{peer};
            try {
                co_await coroutine::deadline(io, std::chrono::seconds{2}, channel.post(jobs::admission{false, std::move(code), ""}));
            } catch (const std::exception&) {
            }
        }
        struct member {
            member(jobs::hello h, socket s) : hello(std::move(h)), sock(std::move(s)), chan(sock) {}
            jobs::hello hello;
            socket sock;
            transport::channel<socket> chan;
            std::size_t inflight = 0;
            clock::time_point seen = clock::now();
            clock::time_point last_used = clock::now();
            std::set<std::string> trees;
            std::deque<std::string> recent_trees;
            enrollment::store* registry = nullptr;
            bool retiring = false;
            void remember(const std::string& tree) {
                if (trees.contains(tree))
                    std::erase(recent_trees, tree);
                else if (trees.size() >= constants::worker_tree_hints) {
                    trees.erase(recent_trees.front());
                    recent_trees.pop_front();
                }
                trees.insert(tree);
                recent_trees.push_back(tree);
            }
        };

        static void prefer(std::shared_ptr<member>& cur, const std::shared_ptr<member>& cand) {
            if (!cur || cand->inflight < cur->inflight ||
                (cand->inflight == cur->inflight &&
                 (cand->last_used < cur->last_used || (cand->last_used == cur->last_used && cand->hello.name < cur->hello.name))))
                cur = cand;
        }

        auto stale(const member& m, clock::time_point now) const -> bool {
            return m.retiring || (m.registry && !m.registry->active(m.hello.worker_id.value_or(""))) ||
                   now - m.seen > (m.registry ? constants::remote_worker_stale : stale_);
        }

        static auto tools_met(const member& m, const requirements& needs) -> bool {
            return m.hello.dependency_publisher == needs.dependency_publisher &&
                   (!needs.toolchain_id || *needs.toolchain_id == m.hello.toolchain_id) &&
                   std::ranges::all_of(needs.tools, [&](const std::string& t) { return std::ranges::contains(m.hello.tools, t); });
        }

        static auto platform_met(const member& m, const requirements& needs) -> bool {
            return (!needs.os || m.hello.platform.os == *needs.os) && (!needs.arch || m.hello.platform.arch == *needs.arch);
        }

        auto eligible(const member& m, const selection& sel, clock::time_point now) const -> bool {
            return !stale(m, now) && m.inflight < m.hello.max_inflight && tools_met(m, sel.needs) && platform_met(m, sel.needs);
        }

        auto pick(const selection& sel) const -> std::shared_ptr<member> {
            auto now = clock::now();
            std::shared_ptr<member> best, resident;
            for (auto& m : members_) {
                if (!eligible(*m, sel, now))
                    continue;
                prefer(best, m);
                if (!sel.tree_key.empty() && m->trees.contains(sel.tree_key))
                    prefer(resident, m);
            }
            return resident ? resident : best;
        }

        std::atomic<int>& attached_;
        const config::slots* config_;
        std::size_t core_;
        std::chrono::milliseconds stale_;
        std::list<std::shared_ptr<member>> members_;
    };

    class directory {
    public:
        explicit directory(const std::vector<std::atomic<int>>& attached) : attached_(attached), forwarded_(attached.size(), 0) {}

        auto pick(const std::vector<bool>& tried) const -> std::optional<std::size_t> {
            std::optional<std::size_t> best;
            int best_n = 0;
            for (std::size_t c = 0; c < attached_.size(); ++c) {
                auto n = attached_[c].load(std::memory_order_relaxed);
                if (n <= 0 || tried[c])
                    continue;
                if (!best || forwarded_[c] * best_n < forwarded_[*best] * n) {
                    best = c;
                    best_n = n;
                }
            }
            return best;
        }

        void forwarded(std::size_t core, int delta) { forwarded_[core] += delta; }

    private:
        // Workers attached to each core, as each core counts its own, and dispatches this core forwarded to it.
        const std::vector<std::atomic<int>>& attached_;
        std::vector<int> forwarded_;
    };

    class router {
    public:
        using socket = native::socket;
        using channel = transport::channel<socket>;

        explicit router(cores::context<native::scheduler>& rt, std::chrono::milliseconds stale = stale_after, const config::slots* config = nullptr)
            : rt_(rt), stale_(stale), config_(config), attached_(rt.size()) {
            for (std::size_t c = 0; c < rt.size(); ++c)
                cores_.push_back(std::make_unique<core_state>(*this, c));
        }

        auto io_context() -> cores::context<native::scheduler>& { return rt_; }

        auto workers(std::size_t core) -> fleet& { return cores_[core]->workers; }
        auto routes(std::size_t core) -> directory& { return cores_[core]->routes; }

        // `progress`, when given, outlives the call: a forwarded attempt reports into it from the other core while this one waits.
        template <class Work>
            requires std::copy_constructible<Work>
        auto with_worker(std::size_t here, selection sel, Work work, jobs::progress_log* progress = nullptr)
            -> std::invoke_result_t<Work&, channel&, jobs::progress_log*, std::size_t> {
            using result = typename std::invoke_result_t<Work&, channel&, jobs::progress_log*, std::size_t>::value_type;
            auto& s = *cores_[here];
            std::vector<bool> tried(rt_.size(), false);
            tried[here] = true;
            bool busy = s.workers.busy(sel);
            for (;;) {
                if (s.workers.holds(sel))
                    co_return co_await s.workers.with(sel, [&](channel& c) { return work(c, progress, here); });

                auto there = s.routes.pick(tried);
                if (!there)
                    throw no_worker{busy ? "every eligible worker is at its dispatch limit" : s.workers.diagnose(sel), busy};
                tried[*there] = true;

                s.routes.forwarded(*there, +1);
                struct release {
                    directory& d;
                    std::size_t core;
                    ~release() { d.forwarded(core, -1); }
                } guard{s.routes, *there};
                std::optional<result> out;
                try {
                    out.emplace(co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(*there), remote(*there, sel, work, progress)));
                } catch (const no_worker& e) {
                    busy = busy || e.busy;
                    continue;
                }
                co_return std::move(*out);
            }
        }

        template <jobs::request Req>
        auto run(std::size_t here, selection sel, Req req, jobs::progress_log* progress = nullptr) -> coroutine::task<jobs::reply_t<Req>> {
            auto result = co_await with_worker(
                here, std::move(sel), [req = std::move(req)](channel& c, jobs::progress_log* log, std::size_t) { return c.call(req, log); }, progress);
            if (!result)
                transport::raise_call_error(std::move(result.error()));
            co_return std::move(*result);
        }

        auto describe(std::size_t here) -> coroutine::task<std::vector<worker_view>> {
            std::vector<worker_view> out;
            for (std::size_t c = 0; c < rt_.size(); ++c) {
                auto some = c == here ? workers(c).describe(c)
                                      : co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(c), [this, c] { return local_description(c); });
                out.insert(out.end(), some.begin(), some.end());
            }
            co_return out;
        }
        auto update_identity(std::size_t here, std::string id, std::optional<std::string> name = {}) -> coroutine::task<> {
            for (std::size_t c = 0; c < rt_.size(); ++c) {
                if (c == here)
                    co_await workers(c).update_identity(rt_.get_scheduler(c), id, name);
                else
                    co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(c),
                                         [this, c, id, name] { return workers(c).update_identity(rt_.get_scheduler(c), id, name); });
            }
        }

    private:
        auto local_description(std::size_t core) -> coroutine::task<std::vector<worker_view>> { co_return workers(core).describe(core); }

        struct core_state {
            core_state(router& owner, std::size_t core) : workers(owner.attached_[core], owner.config_, core, owner.stale_), routes(owner.attached_) {}
            fleet workers;
            directory routes;
        };

        template <class Work> auto remote(std::size_t there, selection sel, const Work& work, jobs::progress_log* progress) {
            return [this, there, sel = std::move(sel), work, progress]() {
                return cores_[there]->workers.with(sel, [work, progress, there](channel& c) mutable { return work(c, progress, there); });
            };
        }

        cores::context<native::scheduler>& rt_;
        std::chrono::milliseconds stale_;
        const config::slots* config_;
        // Workers attached to each core: each core counts its own, and every core reads them all to choose where to forward.
        std::vector<std::atomic<int>> attached_;
        std::vector<std::unique_ptr<core_state>> cores_;
    };
} // namespace routing
