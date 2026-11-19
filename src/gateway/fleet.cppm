export module fleet;
import std;
import constants;
import enrollment;
import native;
import coroutine;
import protocol;
import configuration;
import content;

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
        std::vector<jobs::worker_slot> slots;
        std::optional<std::string> worker_id;
        std::optional<std::string> created_by;
        std::optional<std::int64_t> created_at, last_connected_at, revoked_at;
        bool managed = true;
        std::string toolchain_id;
        bool dependency_publisher = false;
        jobs::worker_memory memory;
        std::uint64_t analysis_inflight = 0;
    };

    struct requirements {
        std::vector<std::string> tools;
        std::optional<std::string> os;
        std::optional<std::string> arch;
        std::optional<std::string> toolchain_id;
        bool dependency_publisher = false;
        friend auto operator==(const requirements&, const requirements&) -> bool = default;
    };

    struct capacity_demand {
        std::optional<std::string> os, arch, toolchain_id;
        std::vector<std::string> tools;
        bool analysis = false;
    };

    struct location {
        std::size_t core = 0;
        std::uint64_t worker = 0;
    };

    struct selection {
        requirements needs;
        std::string tree_key;
        std::string options, snapshot;
        std::string stage;
        std::optional<std::uint64_t> memory_bytes;
        std::optional<std::uint64_t> worker;
        std::optional<std::size_t> core;
        // An analysis that reads project data: it takes the analyze role and its limit. Everything else executes.
        bool analysis = false;
        friend auto operator==(const selection&, const selection&) -> bool = default;
    };

    struct placement {
        std::size_t core = 0;
        std::uint64_t worker = 0;
        int heat = 0;
        std::size_t inflight = 0;
        clock::time_point used;
        std::optional<double> seconds;
        bool personal = false;
    };
    struct proposals {
        std::vector<placement> candidates;
        bool busy = false;
    };
    inline auto measured(const std::vector<placement>& candidates) -> bool {
        return !candidates.empty() && std::ranges::all_of(candidates, [](const auto& value) { return value.seconds.has_value(); });
    }
    inline auto preferred(const placement& a, const placement& b, bool costs) -> bool {
        if (a.personal != b.personal)
            return a.personal;
        if (a.personal && a.heat != b.heat)
            return a.heat > b.heat;
        if (costs && a.seconds != b.seconds)
            return *a.seconds < *b.seconds;
        return std::tuple{-a.heat, a.inflight, a.used, a.core, a.worker} < std::tuple{-b.heat, b.inflight, b.used, b.core, b.worker};
    }

    // No worker takes a dispatch now. `busy` when one could but is at its dispatch limit, so the dispatch may wait for it.
    struct no_worker : std::runtime_error {
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
            auto offered = propose(sel);
            return offered.busy && offered.candidates.empty();
        }

        auto propose(const selection& sel) const -> proposals {
            proposals out;
            const auto now = clock::now();
            for (const auto& m : members_) {
                if (stale(*m, now) || !tools_met(*m, sel.needs) || !platform_met(*m, sel.needs) || (sel.worker && *sel.worker != m->id))
                    continue;
                if (sel.memory_bytes && m->hello.memory_bytes && *sel.memory_bytes > *m->hello.memory_bytes)
                    continue;
                if (!std::ranges::contains(m->hello.roles, sel.analysis ? constants::analyze_role : constants::execute_role))
                    continue;
                const auto running = sel.analysis ? m->analyses : m->inflight;
                const auto limit = sel.analysis ? m->hello.max_analyses : m->hello.max_inflight;
                if (running >= limit || !memory_available(*m, sel)) {
                    out.busy = true;
                    continue;
                }
                placement value{.core = core_,
                                .worker = m->id,
                                .heat = warmth(*m, sel),
                                .inflight = running,
                                .used = m->last_used,
                                .personal = m->registry != nullptr};
                if (const auto observed = m->costs.find(cost_key(sel, value.heat)); observed != m->costs.end())
                    value.seconds = observed->second.seconds * (1.0 + double(running) / limit);
                out.candidates.push_back(std::move(value));
            }
            return out;
        }

        // `observed` is false for work that is only a part of a dispatch held elsewhere: its time and tree are that dispatch's.
        template <class Work> auto with(const selection& sel, Work work, bool observed = true) -> std::invoke_result_t<Work&, transport::channel<socket>&> {
            auto m = pick(sel);
            if (!m)
                throw no_worker{diagnose(sel), busy(sel)};
            const auto started = clock::now();
            const auto heat = warmth(*m, sel);
            const auto reserved = memory_reservation(*m, sel);
            auto& running = sel.analysis ? m->analyses : m->inflight;
            ++running;
            m->memory_reserved += reserved;
            if (changed)
                changed();
            struct release {
                std::shared_ptr<member> m;
                fleet* owner;
                std::uint64_t reserved;
                std::size_t& running;
                ~release() {
                    --running;
                    m->memory_reserved -= reserved;
                    if (owner->changed)
                        owner->changed();
                }
            } guard{m, this, reserved, running};
            using yielded = typename std::invoke_result_t<Work&, transport::channel<socket>&>::value_type;
            if constexpr (std::is_void_v<yielded>) {
                co_await work(m->chan);
                m->last_used = clock::now();
                if (observed)
                    observe(*m, sel, heat, std::chrono::duration<double>(m->last_used - started).count());
                co_return;
            } else {
                auto out = co_await work(m->chan);
                m->last_used = clock::now();
                bool successful = true;
                if constexpr (requires {
                                  out->analysis_error;
                                  out->ok();
                              })
                    successful = out && out->ok() && !out->analysis_error;
                else if constexpr (requires { out->ok(); })
                    successful = out && out->ok();
                else if constexpr (requires {
                                       out.first.analysis_error;
                                       out.first.ok();
                                   })
                    successful = out.first.ok() && !out.first.analysis_error;
                else if constexpr (requires { out.first.ok(); })
                    successful = out.first.ok();
                else if constexpr (requires { bool(out); })
                    successful = bool(out);
                if (successful && observed)
                    observe(*m, sel, heat, std::chrono::duration<double>(m->last_used - started).count());
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
                else if (!pool || !hello.worker_id || !hello.workspace_id || !hello.max_inflight || hello.max_inflight > 64 || !hello.max_analyses ||
                         hello.max_analyses > 64 || hello.roles.empty() || !hello.heartbeat_seconds || hello.heartbeat_seconds > 30 ||
                         hello.tools.size() > 4096)
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
            held->id = ++next_member_;
            held->registry = registry;
            held->hello.slots.resize(std::min(held->hello.slots.size(), constants::worker_slot_reports));
            held->slots = std::move(held->hello.slots);
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
                co_await held->chan.pump([held, this](const transport::frame& f) {
                    if (f.kind == jobs::code<jobs::heartbeat>)
                        held->seen = clock::now();
                    if (f.kind == jobs::code<jobs::slot_report>) {
                        auto report = transport::decode<jobs::slot_report>(f);
                        if (report.sequence < held->slot_sequence)
                            return;
                        held->slot_sequence = report.sequence;
                        report.slots.resize(std::min(report.slots.size(), constants::worker_slot_reports));
                        held->slots = std::move(report.slots);
                        held->memory = report.memory;
                        if (changed)
                            changed();
                    }
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
                out.push_back({m->hello.name, m->hello.platform, m->hello.max_inflight, core, m->inflight,
                               static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m->seen).count()), !stale(*m, now),
                               m->hello.tools, m->slots, m->hello.worker_id});
                out.back().analysis_inflight = m->analyses;
                out.back().toolchain_id = m->hello.toolchain_id;
                out.back().dependency_publisher = m->hello.dependency_publisher;
                out.back().memory = m->memory;
                out.back().memory.limit = m->hello.memory_bytes;
                out.back().memory.reserved = std::max(m->memory.reserved, m->memory_reserved);
            }
            return out;
        }

        auto locate(const transport::channel<socket>& channel) const -> location {
            for (const auto& member : members_)
                if (&member->chan == &channel)
                    return {core_, member->id};
            throw no_worker{"Worker connection is no longer attached"};
        }

        // The inputs a worker's tree last reported for one preparation, so an analysis of unchanged inputs is
        // sent without asking the preparation again. The worker refuses it when its tree has moved on.
        auto inputs(const transport::channel<socket>& channel, const std::string& preparation) const -> std::optional<content::hash> {
            const auto& known = attached(channel).inputs;
            const auto found = known.find(preparation);
            return found == known.end() ? std::nullopt : std::optional{found->second};
        }
        void remember(const transport::channel<socket>& channel, const std::string& preparation, std::optional<content::hash> reported) {
            auto& known = attached(channel).inputs;
            if (!reported) {
                known.erase(preparation);
                return;
            }
            if (known.size() >= remembered_inputs && !known.contains(preparation))
                known.clear();
            known.insert_or_assign(preparation, std::move(*reported));
        }
        auto offered(const transport::channel<socket>& channel, const std::string& project, const content::hash& snapshot) const -> bool {
            auto* worker = find_channel(channel);
            return worker && worker->offers.contains({project, snapshot});
        }
        void remember_offer(const transport::channel<socket>& channel, const std::string& project, const content::hash& snapshot) {
            auto* worker = find_channel(channel);
            if (!worker)
                return;
            auto& offers = worker->offers;
            if (offers.size() >= remembered_inputs)
                offers.clear();
            offers.emplace(project, snapshot);
        }
        // Runs work of the execute role on the worker an analysis holds, in a place of its own.
        template <class Work> auto executing(const transport::channel<socket>& channel, selection sel, Work work) {
            const auto& held = attached(channel);
            if (!std::ranges::contains(held.hello.roles, constants::execute_role))
                throw no_worker{std::format("worker '{}' only analyzes; the preparation needs one that also executes", held.hello.name)};
            sel.worker = held.id;
            sel.analysis = false;
            // The analysis this runs beside already reserved the dispatch's memory.
            sel.memory_bytes = 0;
            return with(sel, std::move(work), false);
        }

        auto receipt_identity(const transport::channel<socket>& channel) const {
            const auto& worker = attached(channel);
            return std::pair{worker.registry, worker.hello.worker_id.value_or("")};
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
            std::uint64_t id = 0;
            struct observation {
                double seconds = 0;
                clock::time_point used;
            };
            std::map<std::tuple<std::string, std::string, std::string, int>, observation> costs;
            jobs::hello hello;
            socket sock;
            transport::channel<socket> chan;
            std::size_t inflight = 0, analyses = 0;
            std::map<std::string, content::hash> inputs;
            std::set<std::pair<std::string, content::hash>> offers;
            clock::time_point seen = clock::now();
            clock::time_point last_used = clock::now();
            std::vector<jobs::worker_slot> slots;
            std::uint64_t slot_sequence = 0;
            jobs::worker_memory memory;
            std::uint64_t memory_reserved = 0;
            enrollment::store* registry = nullptr;
            bool retiring = false;
        };

        static constexpr std::size_t remembered_inputs = 256;
        auto find_channel(const transport::channel<socket>& channel) const -> member* {
            for (const auto& held : members_)
                if (&held->chan == &channel)
                    return held.get();
            return nullptr;
        }
        auto attached(const transport::channel<socket>& channel) const -> member& {
            if (auto* worker = find_channel(channel))
                return *worker;
            throw no_worker{"Worker connection is no longer attached"};
        }

        static auto memory_reservation(const member& m, const selection& sel) -> std::uint64_t {
            if (!m.hello.memory_bytes)
                return 0;
            const auto baseline = std::min(m.memory.baseline.value_or(0), *m.hello.memory_bytes);
            return sel.memory_bytes.value_or(*m.hello.memory_bytes - baseline);
        }
        static auto memory_available(const member& m, const selection& sel) -> bool {
            if (!m.hello.memory_bytes)
                return true;
            const auto baseline = std::min(m.memory.baseline.value_or(0), *m.hello.memory_bytes);
            const auto available = *m.hello.memory_bytes - baseline;
            const auto reserved = std::max(m.memory.reserved, m.memory_reserved);
            return reserved <= available && memory_reservation(m, sel) <= available - reserved;
        }
        static auto cost_key(const selection& sel, int heat) -> std::tuple<std::string, std::string, std::string, int> {
            return {sel.tree_key.substr(0, sel.tree_key.find('/')), sel.options, sel.stage, heat};
        }
        static auto warmth(const member& m, const selection& sel) -> int {
            int heat = 0;
            for (const auto& slot : m.slots)
                if (!slot.busy && slot.project + "/" + slot.tree == sel.tree_key) {
                    heat = std::max(heat, !sel.snapshot.empty() && slot.snapshot == sel.snapshot ? 2 : 1);
                    for (const auto& cache : slot.cached)
                        if (!sel.options.empty() && cache.options == sel.options && cache.stage == sel.stage &&
                            !sel.snapshot.empty() && cache.snapshot == sel.snapshot && cache.toolchain == m.hello.toolchain_id)
                            heat = std::max(heat, 3);
                    for (const auto& session : slot.sessions)
                        if (!sel.options.empty() && session.options == sel.options && session.stage == sel.stage)
                            heat = std::max(heat, !sel.snapshot.empty() && session.snapshot == sel.snapshot ? 4 : 2);
                }
            return heat;
        }
        static void observe(member& m, const selection& sel, int heat, double seconds) {
            if (sel.options.empty())
                return;
            const auto key = cost_key(sel, heat);
            if (auto found = m.costs.find(key); found != m.costs.end()) {
                found->second.seconds += constants::worker_cost_weight * (seconds - found->second.seconds);
                found->second.used = clock::now();
            } else {
                if (m.costs.size() >= constants::worker_cost_records) {
                    auto oldest = std::ranges::min_element(m.costs, {}, [](const auto& value) { return value.second.used; });
                    m.costs.erase(oldest);
                }
                m.costs.emplace(key, member::observation{seconds, clock::now()});
            }
        }

        auto stale(const member& m, clock::time_point now) const -> bool {
            return m.retiring || (m.registry && !m.registry->active(m.hello.worker_id.value_or(""))) ||
                   now - m.seen > (m.registry ? constants::remote_worker_stale : stale_);
        }

        static auto tools_met(const member& m, const requirements& needs) -> bool {
            return m.hello.dependency_publisher == needs.dependency_publisher && (!needs.toolchain_id || *needs.toolchain_id == m.hello.toolchain_id) &&
                   std::ranges::all_of(needs.tools, [&](const std::string& t) { return std::ranges::contains(m.hello.tools, t); });
        }

        static auto platform_met(const member& m, const requirements& needs) -> bool {
            return (!needs.os || m.hello.platform.os == *needs.os) && (!needs.arch || m.hello.platform.arch == *needs.arch);
        }

        auto pick(const selection& sel) const -> std::shared_ptr<member> {
            auto candidates = propose(sel).candidates;
            if (candidates.empty())
                return {};
            const bool costs = measured(candidates);
            auto best = std::ranges::min_element(candidates, [costs](const auto& a, const auto& b) { return preferred(a, b, costs); });
            const auto found = std::ranges::find_if(members_, [&](const auto& m) { return m->id == best->worker; });
            return *found;
        }

        std::uint64_t next_member_ = 0;
        std::atomic<int>& attached_;
        const config::slots* config_;
        std::size_t core_;
        std::chrono::milliseconds stale_;
        std::list<std::shared_ptr<member>> members_;
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

        auto overflow_limit(std::size_t here) const -> std::chrono::seconds {
            return std::chrono::seconds{config_ ? config_->current(here)->server.overflow_wait_seconds : 0};
        }
        auto wait_for_capacity(const requirements& needs, bool analysis = false) -> std::shared_ptr<void> {
            auto demand = std::make_shared<capacity_demand>(capacity_demand{needs.os, needs.arch, needs.toolchain_id, needs.tools, analysis});
            std::lock_guard held{demand_mutex_};
            std::erase_if(demands_, [](const auto& pending) { return pending.expired(); });
            demands_.push_back(demand);
            return demand;
        }
        auto demand() const -> std::vector<capacity_demand> {
            std::lock_guard held{demand_mutex_};
            std::vector<capacity_demand> out;
            for (const auto& pending : demands_)
                if (auto live = pending.lock())
                    out.push_back(*live);
            return out;
        }

        template <class Work>
            requires std::copy_constructible<Work>
        auto with_worker(std::size_t here, selection sel, Work work, jobs::progress_log* progress = nullptr)
            -> std::invoke_result_t<Work&, channel&, jobs::progress_log*, std::size_t> {
            const auto limit = sel.worker || sel.needs.dependency_publisher ? std::chrono::seconds{0} : overflow_limit(here);
            const auto deadline = std::chrono::steady_clock::now() + limit;
            std::shared_ptr<void> waiting;
            for (;;) {
                try {
                    co_return co_await attempt(here, sel, work, progress, waiting);
                } catch (const no_worker&) {
                    if (!limit.count() || std::chrono::steady_clock::now() >= deadline)
                        throw;
                    if (!waiting)
                        waiting = wait_for_capacity(sel.needs, sel.analysis);
                }
                co_await execution::schedule_after(rt_.get_scheduler(here), std::chrono::milliseconds{250});
            }
        }

        // `progress`, when given, outlives the call: a forwarded attempt reports into it from the other core while this one waits.
        template <class Work>
            requires std::copy_constructible<Work>
        auto attempt(std::size_t here, selection sel, Work work, jobs::progress_log* progress, std::shared_ptr<void>& waiting)
            -> std::invoke_result_t<Work&, channel&, jobs::progress_log*, std::size_t> {
            using result = typename std::invoke_result_t<Work&, channel&, jobs::progress_log*, std::size_t>::value_type;
            auto& s = *cores_[here];
            auto offered = co_await propose(here, sel);
            const bool costs = measured(offered.candidates);
            std::ranges::sort(offered.candidates, [costs](const auto& a, const auto& b) { return preferred(a, b, costs); });
            bool busy = offered.busy;
            for (const auto& candidate : offered.candidates) {
                auto selected = sel;
                selected.worker = candidate.worker;
                waiting.reset();
                if (candidate.core == here) {
                    try {
                        co_return co_await s.workers.with(selected, [&](channel& c) { return work(c, progress, here); });
                    } catch (const no_worker& e) {
                        busy = busy || e.busy;
                        continue;
                    }
                }
                std::optional<result> out;
                try {
                    out.emplace(
                        co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(candidate.core), remote(candidate.core, selected, work, progress)));
                } catch (const no_worker& e) {
                    busy = busy || e.busy;
                    continue;
                }
                co_return std::move(*out);
            }
            if (sel.worker && !busy)
                throw no_worker{"the worker reserved for this directory group is no longer available"};
            throw no_worker{busy ? "every eligible worker is at its dispatch limit" : s.workers.diagnose(sel), busy};
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
        auto propose(std::size_t here, const selection& sel) -> coroutine::task<proposals> {
            proposals out;
            for (std::size_t core = 0; core < rt_.size(); ++core) {
                if ((sel.core && *sel.core != core) || !attached_[core].load(std::memory_order_relaxed))
                    continue;
                auto value = core == here ? workers(core).propose(sel)
                                          : co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(core),
                                                                 [this, core, sel] { return local_proposal(core, sel); });
                out.busy = out.busy || value.busy;
                out.candidates.insert(out.candidates.end(), value.candidates.begin(), value.candidates.end());
            }
            co_return out;
        }
        auto local_proposal(std::size_t core, const selection& sel) -> coroutine::task<proposals> { co_return workers(core).propose(sel); }

        auto local_description(std::size_t core) -> coroutine::task<std::vector<worker_view>> { co_return workers(core).describe(core); }

        struct core_state {
            core_state(router& owner, std::size_t core) : workers(owner.attached_[core], owner.config_, core, owner.stale_) {}
            fleet workers;
        };

        template <class Work> auto remote(std::size_t there, selection sel, const Work& work, jobs::progress_log* progress) {
            return [this, there, sel = std::move(sel), work, progress]() {
                return cores_[there]->workers.with(sel, [work, progress, there](channel& c) mutable { return work(c, progress, there); });
            };
        }

        cores::context<native::scheduler>& rt_;
        std::chrono::milliseconds stale_;
        const config::slots* config_;
        mutable std::mutex demand_mutex_;
        std::vector<std::weak_ptr<capacity_demand>> demands_;
        // Workers attached to each core: each core counts its own, and every core reads them all to choose where to forward.
        std::vector<std::atomic<int>> attached_;
        std::vector<std::unique_ptr<core_state>> cores_;
    };
} // namespace routing
