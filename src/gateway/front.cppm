export module front;
import std;
import enrollment;
import constants;
import native;
import json;
import coroutine;
import http;
import mcp;
import settings;
import configuration;
import registry;
import fleet;
import tools;
import protocol;
import world;
import keyring;
import nodes;
import codec;
import utility;
import websocket;

export namespace front {

    // Notifications carry invalidations, never credentials or mutable resource bodies.
    // Publishers may run on the disk pool; each HTTP body belongs to one scheduler.
    class workspace_events {
    public:
        struct subscription {
            web::event_stream body;
            bool closed = false;
            void send(std::string text) {
                if (closed)
                    return;
                if (body.pending_bytes() + text.size() > constants::workspace_event_buffer_bytes) {
                    closed = true;
                    body.take();
                    body.close();
                } else
                    body.push(text);
            }
        };
        workspace_events(cores::context<native::scheduler>& rt, std::string workspace)
            : rt_(rt), workspace_(std::move(workspace)), local_(rt.size()), pending_(rt.size()) {}
        auto listen(std::size_t core) -> std::shared_ptr<subscription> {
            auto& listeners = local_[core];
            std::erase_if(listeners, [](auto& item) { return item.expired(); });
            if (listeners.size() >= constants::workspace_event_connections_per_core)
                return {};
            auto sub = std::make_shared<subscription>();
            listeners.push_back(sub);
            sub->send(message(15, "reset"));
            return sub;
        }
        void publish(unsigned changes) {
            for (std::size_t core = 0; core < local_.size(); ++core) {
                if (pending_[core].fetch_or(changes) != 0)
                    continue;
                rt_.post(core, [this, core] {
                    auto changes = pending_[core].exchange(0);
                    auto text = message(changes, "change");
                    // Copy strong references before resuming readers, which can close streams.
                    std::vector<std::shared_ptr<subscription>> live;
                    for (auto& weak : local_[core])
                        if (auto sub = weak.lock())
                            live.push_back(std::move(sub));
                    for (auto& sub : live)
                        sub->send(text);
                    std::erase_if(local_[core], [](auto& item) { return item.expired(); });
                });
            }
        }

    private:
        auto message(unsigned changes, std::string_view event) const -> std::string {
            return std::format("event: {}\ndata: {{\"workspace_id\":{},\"changes\":{}}}\n\n", event, json::write(workspace_), changes);
        }
        cores::context<native::scheduler>& rt_;
        std::string workspace_;
        std::vector<std::vector<std::weak_ptr<subscription>>> local_;
        std::vector<std::atomic<unsigned>> pending_;
    };

    class config_hub : public config::slots {
    public:
        config_hub(cores::context<native::scheduler>& rt, config::store store, execution::thread_pool& pool)
            : config::slots(rt.size(), store.current()), rt_(rt), store_(std::move(store)), pool_(pool), events_(rt, store_.current()->server.auth.workspace) {}

        auto events() -> workspace_events& { return events_; }

        auto disk_pool() -> execution::thread_pool& { return pool_; }

        template <class F> auto edit(std::size_t here, F f) -> coroutine::task<> {
            if (here == 0)
                co_return co_await apply(f);
            co_await cores::call(rt_.get_scheduler(here), rt_.get_scheduler(0), [this, &f] { return apply(f); });
        }

    private:
        // `f` belongs to the edit that awaits this.
        auto apply(util::function_ref<void(config::store&)> f) -> coroutine::task<>;
        auto publish(config::store::snapshot snap) -> coroutine::task<>;

        cores::context<native::scheduler>& rt_;
        config::store store_;
        execution::thread_pool& pool_;
        coroutine::mutex lock_;
        workspace_events events_;
    };

    // Clients choose JSON-RPC IDs and repeat them, so a cancellation names a request of the same caller and endpoint.
    inline auto request_key(const worlds::actor& who, bool configuration, const mcp::key& id) -> std::string {
        return std::format("{}\n{}\n{}\n{}\n{}", worlds::identity_key(who), who.client.value_or(""), who.run, configuration ? "config" : "business",
                           mcp::key_text(id));
    }

    class in_flight {
    public:
        explicit in_flight(cores::context<native::scheduler>& rt) : rt_(rt), local_(rt.size()) {}

        void enter(std::size_t core, const std::string& id, coroutine::stop_source* source) {
            local_[core][id] = source;
            std::lock_guard lock(owners_lock_);
            owners_[id] = core;
        }
        void leave(std::size_t core, const std::string& id, coroutine::stop_source* source) {
            auto it = local_[core].find(id);
            if (it == local_[core].end() || it->second != source)
                return;
            local_[core].erase(it);
            std::lock_guard lock(owners_lock_);
            if (auto o = owners_.find(id); o != owners_.end() && o->second == core)
                owners_.erase(o);
        }

        void cancel(std::size_t here, const std::string& id) {
            std::optional<std::size_t> core;
            {
                std::lock_guard lock(owners_lock_);
                if (auto o = owners_.find(id); o != owners_.end())
                    core = o->second;
            }
            if (!core)
                return;
            if (*core == here)
                stop(here, id);
            else
                rt_.post(*core, [this, c = *core, id] { stop(c, id); });
        }

    private:
        void stop(std::size_t core, const std::string& id) {
            if (auto it = local_[core].find(id); it != local_[core].end())
                it->second->request_stop();
        }

        cores::context<native::scheduler>& rt_;
        std::mutex owners_lock_;
        std::map<std::string, std::size_t> owners_;
        std::vector<std::map<std::string, coroutine::stop_source*>> local_;
    };

    struct context {
        config_hub& config;
        storage::registry& files;
        routing::router& workers;
        cores::context<native::scheduler>& rt;
        std::size_t core;
        in_flight* requests = nullptr;
        const worlds::actor* who = nullptr;
        // A pool for blocking calls, such as the source probe.
        execution::thread_pool* pool = nullptr;
        // This core's tool services, which REST routes share with MCP.
        tools::services* services = nullptr;
        allocation::control* runtime = nullptr;
        enrollment::store* identities = nullptr;
        bool remote_listener = false;
    };

    struct coded_error : std::runtime_error, util::retained_exception {
        http::status status;
        std::string code;
        std::vector<std::string> missing, conflicts, frontier;
        coded_error(http::status status, std::string_view code, std::string message) : std::runtime_error(std::move(message)), status(status), code(code) {}
        auto retain() const noexcept -> std::exception_ptr override;
    };
    auto coded_error::retain() const noexcept -> std::exception_ptr { return util::copy_current_exception(*this); }

    namespace api {
        template <class T> struct section_view {
            std::uint64_t revision;
            T value;
        };

        // Bodies and answers of the node routes.
        struct node_capture {
            std::string operation;
            std::vector<std::string> expected;
            std::string intent;
        };
        struct node_source_head {
            std::string head, revision, snapshot, worldline;
            std::vector<content::entry> files;
            bool truncated = false;
            std::vector<std::string> conflicts;
            std::vector<std::string> frontier;
            std::map<std::string, std::vector<std::string>> worldlines;
            std::string node;
        };
        struct node_source_file {
            std::string content, revision, snapshot, worldline, path;
            std::uint64_t first_line = 1, lines = 0, total_lines = 0;
            bool truncated = false, binary = false;
        };
    } // namespace api

    inline auto conflict_reply(const config::revision_conflict& e) -> web::reply {
        return web::json_reply(409, std::format(R"({{"code":"revision_conflict","message":{},"revision":{},"value":{}}})",
                                                json::write(std::string{e.what()}), e.revision, e.value));
    }

    inline auto coded_reply(const coded_error& e) -> web::reply {
        return web::json_reply(e.status, std::format(R"({{"code":{},"message":{},"missing":{},"conflicts":{},"frontier":{}}})", json::write(e.code),
                                                     json::write(std::string{e.what()}), json::write(e.missing), json::write(e.conflicts),
                                                     json::write(e.frontier)));
    }

    // Answers one REST request; the configuration MCP answers through it too.
    auto route(context& c, std::string_view method, const web::target& target, std::string_view body) -> coroutine::task<web::reply>;

    // Answers a configuration tool as the REST route it mirrors.
    struct dispatch {
        context ctx;
        auto operator()(const worlds::actor& who, std::string method, std::string path, std::string body) const -> coroutine::task<web::reply> {
            auto c = ctx;
            c.who = &who;
            auto target = web::target::parse(path);
            co_return co_await route(c, method, target, body);
        }
    };

    // One agent connection covers project setup, source editing, execution and delivery.
    class workspace_tools {
        tools::standard::bound execution_;
        settings::bound<dispatch> settings_;
        std::vector<mcp::tool> tools_;

    public:
        workspace_tools(context ctx, tools::services& services) : execution_(services), settings_(dispatch{ctx}) {
            tools_ = execution_.list();
            const auto& configuration = settings_.list();
            tools_.insert(tools_.end(), configuration.begin(), configuration.end());
        }
        auto list() const -> const std::vector<mcp::tool>& { return tools_; }
        auto call(std::string_view name, json::value args, mcp::notifier& note, const worlds::actor& who, mcp::call_context call = {})
            -> coroutine::task<mcp::tool_outcome> {
            if (std::ranges::any_of(settings_.list(), [name](const auto& tool) { return tool.name == name; })) {
                if (!who.run.empty())
                    co_return tools::refuse_tool({nodes::code::permission_denied, "execution credentials cannot access project settings"});
                co_return co_await settings_.call(name, std::move(args), note, who, call);
            }
            co_return co_await execution_.call(name, std::move(args), note, who, call);
        }
    };

    class server {
    public:
        using socket = native::socket;
        using toolset = workspace_tools;

        server(context ctx, tools::services& services, std::chrono::milliseconds heartbeat = constants::mcp_heartbeat)
            : ctx_(ctx), heartbeat_(heartbeat), mcp_{std::string{constants::mcp_server_name}, "0.1.0", toolset{ctx, services}},
              settings_{"Fughetta configuration", "0.1.0", settings::bound{dispatch{ctx}}} {}

        auto serve(socket peer) -> coroutine::task<> {
            web::stream<socket> transport{peer};
            http::v1::server<web::stream<socket>> conn{transport};
            std::string serving;
            try {
                while (!conn.should_close()) {
                    auto in = co_await http::coroutine::receive(conn);
                    serving = std::format("{} {}", in.head.method.token(), in.head.target);
                    auto doc = ctx_.config.current(ctx_.core);
                    auto t = web::target::parse(in.head.target);
                    auto origin = web::header(in.head, "origin");
                    bool allowed = !origin || std::ranges::contains(doc->server.allowed_origins, *origin);

                    auto method = in.head.method.token();
                    const bool health = method == "GET" && t.path == "/healthz";
                    const bool preflight = method == "OPTIONS" && origin;
                    const bool trusted = doc->server.auth.mode == config::auth_mode::trusted;
                    auto authorization = web::header(in.head, "authorization").value_or("");
                    auto credential = trusted ? web::header(in.head, "x-fughetta-execution") : std::nullopt;
                    if (!trusted && !health && !preflight) {
                        if (auto token = keyring::bearer(authorization); token && token->starts_with("fugrun."))
                            credential = token;
                    }
                    std::optional<worlds::actor> who;
                    if (trusted && (!health || credential)) {
                        std::optional<web::reply> refused;
                        if (!preflight || credential)
                            refused = authenticate(doc->server.auth, in.head, who);
                        else if (!keyring::accepts_front(doc->server.auth, authorization))
                            refused = unauthorized("send the front bearer token");
                        if (refused) {
                            refused->head.fields.push_back({"connection", "close"});
                            if (origin && allowed) {
                                refused->head.fields.push_back({"access-control-allow-origin", std::string{*origin}});
                                refused->head.fields.push_back({"vary", "origin"});
                            }
                            co_await web::respond(conn, std::move(in.token), std::move(refused->head), std::move(refused->body));
                            peer.shutdown(net::socket_base::shutdown_send);
                            auto drain = [&peer]() -> coroutine::task<> {
                                std::array<std::byte, 4096> buffer;
                                while (co_await io::async_read_some(peer, net::buffer(buffer))) {}
                            };
                            co_await coroutine::deadline(ctx_.rt.get_scheduler(ctx_.core), std::chrono::seconds{1}, drain());
                            break;
                        }
                    }

                    std::string body;
                    bool too_large = false;
                    try {
                        body = co_await web::read_all(in.body, doc->server.max_request_bytes);
                    } catch (const std::length_error&) {
                        too_large = true;
                    }
                    if (too_large) {
                        auto r = error(413, std::format("request larger than {} bytes", doc->server.max_request_bytes));
                        r.head.fields.push_back({"connection", "close"});
                        co_await web::respond(conn, std::move(in.token), std::move(r.head), std::move(r.body));
                        break;
                    }
                    if (!allowed) {
                        auto r = error(403, std::format("origin '{}' is not allowed", *origin));
                        co_await web::respond(conn, std::move(in.token), std::move(r.head), std::move(r.body));
                        continue;
                    }

                    web::reply r;
                    auto c = ctx_;
                    std::optional<web::reply> refused;
                    if (credential)
                        refused = co_await authenticate_execution(t, *credential, who);
                    else if (!trusted && !health && !preflight)
                        refused = authenticate(doc->server.auth, in.head, who);
                    if (who)
                        c.who = &*who;
                    std::optional<allocation::control::lease> admitted;
                    bool control_read = method == "OPTIONS" || (method == "GET" && (t.path == "/healthz" || t.path == "/api/v1/config/runtime" ||
                                                                                    t.path == "/api/v1/builds" || t.path == "/api/v1/events" ||
                                                                                    t.path == "/api/v1/hosting/activity"));
                    // Status polling must not prevent a drained instance from rebuilding its processes.
                    if (!refused && c.runtime && !control_read) {
                        if (auto held = c.runtime->enter(t.path == "/api/v1/config/runtime"))
                            admitted.emplace(std::move(*held));
                        if (!admitted && method == "POST" && (t.path == "/mcp" || t.path == "/mcp/config")) {
                            if (draining_cancel(body))
                                if (auto held = c.runtime->enter(true))
                                    admitted.emplace(std::move(*held));
                        }
                        if (!admitted && !control_read)
                            refused = error(503, "runtime budget is being applied; retry after the service is ready");
                    }
                    if (refused) {
                        r = std::move(*refused);
                    } else if (preflight && !credential) {
                        r = {.head = {.status = 204,
                                      .fields = {{"access-control-allow-methods", "GET, PUT, POST, DELETE, OPTIONS"},
                                                 {"access-control-allow-headers", "authorization, content-type, accept, mcp-protocol-version, last-event-id"},
                                                 {"access-control-max-age", "600"}}}};
                    } else if (t.path == "/api/v1/events" && method == "GET") {
                        auto sub = ctx_.config.events().listen(ctx_.core);
                        if (!sub)
                            r = error(503, "Workspace event connection limit reached");
                        else {
                            http::response head{.status = 200,
                                                .fields = {{"content-type", "text/event-stream"},
                                                           {"cache-control", "no-store, no-transform"},
                                                           {"x-accel-buffering", "no"},
                                                           {"transfer-encoding", "chunked"},
                                                           {"connection", "close"}}};
                            if (origin) {
                                head.fields.push_back({"access-control-allow-origin", std::string{*origin}});
                                head.fields.push_back({"vary", "origin"});
                            }
                            auto send = [&](coroutine::scope& children) -> coroutine::task<> {
                                struct stop {
                                    coroutine::scope& children;
                                    ~stop() { children.request_stop(); }
                                } done{children};
                                co_await coroutine::deadline(ctx_.rt.get_scheduler(ctx_.core), constants::workspace_event_lifetime,
                                                             http::coroutine::respond(conn, std::move(in.token), std::move(head), sub->body));
                            };
                            auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                                children.spawn(send(children));
                                children.spawn(heartbeat(*sub, in.head, *who, children));
                                co_await children.join();
                            };
                            co_await coroutine::with_scope(std::move(run_children));
                            break;
                        }
                    } else if (t.path == "/mcp" || t.path == "/mcp/config") {
                        if (method != "POST") {
                            r = error(405, "MCP takes POST; this server offers no GET stream");
                            r.head.fields.push_back({"allow", "POST"});
                        } else if (auto answer = co_await mcp(conn, in, body, origin, c.who, t.path == "/mcp/config")) {
                            r = std::move(*answer);
                        } else {
                            continue;
                        }
                    } else {
                        r = co_await route(c, method, t, body);
                    }
                    if (origin) {
                        r.head.fields.push_back({"access-control-allow-origin", std::string{*origin}});
                        r.head.fields.push_back({"vary", "origin"});
                    }
                    co_await web::respond(conn, std::move(in.token), std::move(r.head), std::move(r.body));
                }
            } catch (const std::exception& e) {
                // A peer that aborts mid-request reaches here too; the log line is how a
                // silent drop gets distinguished from a gateway fault.
                logging::error(
                    std::format("gateway: connection closed after unhandled exception{}: {}", serving.empty() ? "" : " while serving " + serving, e.what()));
            }
            peer.close();
        }

        auto serve_worker(socket peer) -> coroutine::task<> {
            auto tunnel = std::make_shared<web::ws::worker_tunnel>(std::move(peer));
            auto io = ctx_.rt.get_scheduler(ctx_.core);
            try {
                auto upgraded = co_await coroutine::deadline(io, std::chrono::seconds{10}, upgrade_worker(*tunnel));
                if (!upgraded.value_or(false))
                    co_return;
                auto run = [&](coroutine::scope& children) -> coroutine::task<> {
                    children.spawn(tunnel->watch(io));
                    co_await ctx_.workers.workers(ctx_.core).admit(io, tunnel, ctx_.identities, ctx_.pool,
                        [this] { return ctx_.config.current(ctx_.core)->server.remote_worker_enrollment; });
                };
                co_await coroutine::with_scope(std::move(run));
            } catch (const std::exception& e) {
                logging::warn(std::format("worker tunnel: {}", e.what()));
            }
        }

    private:
        auto upgrade_worker(web::ws::worker_tunnel& tunnel) -> coroutine::task<bool> {
            http::v1::server conn{tunnel.transport()};
            auto in = co_await http::coroutine::receive(conn);
            auto doc = ctx_.config.current(ctx_.core);
            std::optional<web::reply> refused;
            http::response head;
            if (doc->server.auth.mode == config::auth_mode::trusted &&
                !keyring::accepts_front(doc->server.auth, web::header(in.head, "authorization").value_or("")))
                refused = unauthorized("send the front bearer token");
            else if (web::header(in.head, "origin"))
                refused = error(403, "Worker tunnels refuse Origin");
            else if (in.head.target != constants::worker_tunnel_path)
                refused = error(404, "Worker tunnel endpoint not found");
            else {
                try {
                    if (!conn.upgraded() || conn.should_close() || web::header(in.head, "transfer-encoding") ||
                        web::header(in.head, "content-length").value_or("0") != "0")
                        throw http::websocket::failure(1002, "Worker tunnel requires a WebSocket upgrade without request content");
                    head = http::websocket::opening_response(in.head, constants::worker_tunnel_protocol);
                } catch (const http::websocket::failure& e) {
                    refused = error(400, e.what());
                }
            }
            if (refused) {
                refused->head.fields.push_back({"connection", "close"});
                co_await web::respond(conn, std::move(in.token), std::move(refused->head), std::move(refused->body));
                co_return false;
            }
            http::span_body empty{std::span<const std::byte>{}};
            co_await http::coroutine::respond(conn, std::move(in.token), std::move(head), empty);
            tunnel.start(http::websocket::side::server, conn.take_pending());
            co_return true;
        }

        auto heartbeat(workspace_events::subscription& sub, const http::request& head, const worlds::actor& initial, coroutine::scope& children)
            -> coroutine::task<> {
            for (;;) {
                co_await execution::schedule_after(ctx_.rt.get_scheduler(ctx_.core), constants::workspace_event_heartbeat);
                std::optional<worlds::actor> who;
                if (sub.closed || authenticate(ctx_.config.current(ctx_.core)->server.auth, head, who) || !who ||
                    worlds::identity_key(*who) != worlds::identity_key(initial) || who->role != initial.role) {
                    sub.closed = true;
                    sub.body.take();
                    sub.body.close();
                    children.request_stop();
                    co_return;
                }
                sub.send(": heartbeat\n\n");
            }
        }

        static auto draining_cancel(std::string_view body) -> bool {
            auto rpc = json::read<mcp::request>(std::string{body});
            return rpc && mcp::cancelled_request(*rpc).has_value();
        }

        static auto error(http::status status, std::string why) -> web::reply {
            std::string_view code = status == http::status{401}   ? std::string_view{"unauthenticated"}
                                    : status == http::status{403} ? nodes::code::permission_denied
                                    : status == http::status{404} ? std::string_view{"not_found"}
                                    : status == http::status{405} ? std::string_view{"method_not_allowed"}
                                    : status == http::status{413} ? std::string_view{"too_large"}
                                    : status == http::status{503} ? std::string_view{"unavailable"}
                                                                  : std::string_view{"invalid_request"};
            return coded_reply(coded_error{status, code, std::move(why)});
        }

        static auto unauthorized(std::string why) -> web::reply {
            auto r = error(401, std::move(why));
            r.head.fields.push_back({"www-authenticate", std::string{constants::bearer_challenge}});
            return r;
        }

        auto authenticate_execution(const web::target& target, std::string_view credential, std::optional<worlds::actor>& who)
            -> coroutine::task<std::optional<web::reply>> {
            if (target.path != "/mcp")
                co_return error(403, "execution credentials are restricted to the task MCP endpoint");
            try {
                auto execution = co_await tools::execution_actor(*ctx_.services, std::string{credential});
                if (who) {
                    if (execution.issuer != who->issuer || execution.user != who->user)
                        co_return unauthorized("execution belongs to another caller");
                    execution.role = std::move(who->role);
                    execution.email = std::move(who->email);
                }
                who = std::move(execution);
            } catch (const std::exception& e) {
                co_return unauthorized(e.what());
            }
            co_return std::nullopt;
        }

        template <class Head>
        static auto authenticate(const config::auth& auth, const Head& head, std::optional<worlds::actor>& who) -> std::optional<web::reply> {
            try {
                who = keyring::authenticate(auth, [&head](std::string_view name) { return web::header(head, name); });
            } catch (const keyring::unauthorized& e) {
                return unauthorized(e.what());
            }
            return std::nullopt;
        }

        template <class Conn, class In>
        auto mcp(Conn& conn, In& in, const std::string& body, std::optional<std::string_view> origin, const worlds::actor* who, bool configuration)
            -> coroutine::task<std::optional<web::reply>> {
            if (auto v = web::header(in.head, "mcp-protocol-version"); v && !mcp::supported(*v)) {
                co_return error(400, std::format("unsupported MCP-Protocol-Version '{}'", *v));
            }
            auto rpc = json::read<mcp::request>(body);
            if (!rpc) {
                co_return web::json_reply(400, json::write(mcp::response{.error = {mcp::error{{mcp::code::parse_error}, {rpc.error().what()}}}}));
            }
            if (auto cancelled = mcp::cancelled_request(*rpc)) {
                if (ctx_.requests && who)
                    ctx_.requests->cancel(ctx_.core, request_key(*who, configuration, *cancelled));
                co_return web::reply{.head = {.status = 202}};
            }
            if (!rpc->id) {
                mcp::notifier drop;
                co_await handle(*rpc, drop, *who, configuration);
                co_return web::reply{.head = {.status = 202}};
            }
            auto accept = web::header(in.head, "accept").value_or("");
            if (accept.find("text/event-stream") == std::string_view::npos || rpc->method != "tools/call") {
                mcp::notifier drop;
                auto out = co_await cancellable(*rpc, drop, *who, configuration);
                co_return web::json_reply(200, json::write(out ? **out : cancelled(*rpc)));
            }

            web::event_stream events;
            coroutine::stop_source heartbeat_stop, disconnected;
            http::response head{.status = 200,
                                .fields = {{"content-type", "text/event-stream"}, {"cache-control", "no-store, no-transform"}, {"x-accel-buffering", "no"}}};
            if (origin) {
                head.fields.push_back({"access-control-allow-origin", std::string{*origin}});
                head.fields.push_back({"vary", "origin"});
            }
            auto run_children = [&](coroutine::scope& children) -> coroutine::task<> {
                static constexpr auto run_child_2 = [](Conn& conn, auto token, http::response head, web::event_stream& events,
                                                       coroutine::stop_source& disconnected) -> coroutine::task<> {
                    try {
                        co_await http::coroutine::respond(conn, std::move(token), std::move(head), events);
                    } catch (...) {
                        disconnected.request_stop();
                        throw;
                    }
                };
                children.spawn(run_child_2(conn, std::move(in.token), std::move(head), events, disconnected));
                static constexpr auto run_child = [](native::scheduler io, web::event_stream& events, coroutine::stop_token token,
                                                     std::chrono::milliseconds interval) -> coroutine::task<> {
                    static constexpr auto run_operation = [](native::scheduler io, web::event_stream& events,
                                                             std::chrono::milliseconds interval) -> coroutine::task<> {
                        for (;;) {
                            co_await execution::schedule_after(io, interval);
                            events.push(": keepalive\n\n");
                        }
                    };
                    co_await coroutine::under(token, run_operation(io, events, interval));
                };
                children.spawn(run_child(ctx_.rt.get_scheduler(ctx_.core), events, heartbeat_stop.get_token(), heartbeat_));
                struct stop_heartbeat {
                    coroutine::stop_source& source;
                    ~stop_heartbeat() { source.request_stop(); }
                } stop{heartbeat_stop};
                mcp::notifier forward{events};
                auto out = co_await cancellable(*rpc, forward, *who, configuration, disconnected.get_token());
                if (disconnected.stop_requested()) {
                    // The request ticket has been released; no response can reach this peer.
                } else if (!out)
                    events.finish(web::event_stream::message(json::write(cancelled(*rpc))));
                else if (*out)
                    // The answer is committed to the stream after the progress it still holds, and read in pieces as the reader
                    // takes them.
                    events.finish(web::event_stream::message(json::write(**out)));
                heartbeat_stop.request_stop();
                events.close();
                co_await children.join();
            };
            co_await coroutine::with_scope(std::move(run_children));
            co_return std::nullopt;
        }

        auto handle(const mcp::request& rpc, mcp::notifier& out, const worlds::actor& who, bool configuration)
            -> coroutine::task<std::optional<mcp::response>> {
            if (configuration)
                co_return co_await settings_.handle(rpc, out, who);
            co_return co_await mcp_.handle(rpc, out, who);
        }

        auto cancellable(const mcp::request& rpc, mcp::notifier& out, const worlds::actor& who, bool configuration, coroutine::stop_token peer = {})
            -> coroutine::task<std::optional<std::optional<mcp::response>>> {
            struct forward {
                coroutine::stop_source* source;
                void operator()() const noexcept { source->request_stop(); }
            };
            auto parent = co_await coroutine::current_token;
            coroutine::stop_source source;
            std::optional<execution::stop_callback_for_t<coroutine::stop_token, forward>> link, peer_link;
            if (parent.stop_possible())
                link.emplace(parent, forward{&source});
            if (peer.stop_possible())
                peer_link.emplace(peer, forward{&source});
            struct registered {
                in_flight* requests;
                std::size_t core;
                std::string id;
                coroutine::stop_source* source;
                ~registered() {
                    if (requests)
                        requests->leave(core, id, source);
                }
            } entry{ctx_.requests, ctx_.core, request_key(who, configuration, *rpc.id), &source};
            if (entry.requests)
                entry.requests->enter(entry.core, entry.id, &source);
            std::optional<std::optional<mcp::response>> r;
            std::exception_ptr failure;
            try {
                r = co_await coroutine::under(source.get_token(), handle(rpc, out, who, configuration));
            } catch (...) {
                failure = util::capture_current_exception();
            }
            if (failure)
                std::rethrow_exception(failure);
            if (!r) {
                if (parent.stop_requested())
                    co_await coroutine::stop;
                co_return std::nullopt;
            }
            co_return std::optional<std::optional<mcp::response>>{std::move(*r)};
        }

        static auto cancelled(const mcp::request& rpc) -> mcp::response {
            return {.id = {rpc.id}, .error = {mcp::error{{mcp::code::request_cancelled}, {"the request was cancelled"}}}};
        }

        context ctx_;
        std::chrono::milliseconds heartbeat_;
        mcp::server<toolset> mcp_;
        mcp::server<settings::bound<dispatch>> settings_;
    };
} // namespace front
