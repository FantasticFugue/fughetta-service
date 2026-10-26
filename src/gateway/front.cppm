export module front;
import std;
import enrollment;
import constants;
import native;
import reflection;
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

export namespace front {

    class config_hub : public config::slots {
    public:
        config_hub(cores::context<native::scheduler>& rt, config::store store, execution::thread_pool& pool)
            : config::slots(rt.size(), store.current()), rt_(rt), store_(std::move(store)), pool_(pool) {}

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
    };

    // Clients choose JSON-RPC IDs and repeat them, so a cancellation names a request of the same caller and endpoint.
    inline auto request_key(const worlds::actor& who, bool configuration, const mcp::key& id) -> std::string {
        return std::format("{}\n{}\n{}\n{}", worlds::identity_key(who), who.client.value_or(""), configuration ? "config" : "business", mcp::key_text(id));
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
        enrollment::store* identities=nullptr;
        bool remote_listener=false;
    };

    struct coded_error : std::runtime_error {
        http::status status;
        std::string code;
        std::vector<std::string> missing, conflicts, frontier;
        coded_error(http::status status, std::string_view code, std::string message)
            : std::runtime_error(std::move(message)), status(status), code(code) {}
    };

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
                                                rfl::json::write(std::string{e.what()}), e.revision, e.value));
    }

    inline auto coded_reply(const coded_error& e) -> web::reply {
        return web::json_reply(e.status, std::format(R"({{"code":{},"message":{},"missing":{},"conflicts":{},"frontier":{}}})", rfl::json::write(e.code),
                                                     rfl::json::write(std::string{e.what()}), rfl::json::write(e.missing), rfl::json::write(e.conflicts), rfl::json::write(e.frontier)));
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

    class server {
    public:
        using socket = native::socket;
        using toolset = tools::standard::bound;

        server(context ctx, tools::services& services) : ctx_(ctx), mcp_{std::string{constants::mcp_server_name}, "0.1.0", toolset{services}},
            settings_{"Fughetta configuration", "0.1.0", settings::bound{dispatch{ctx}}} {}

        auto serve(socket peer) -> coroutine::task<> {
            web::stream<socket> transport{peer};
            http::v1::server<web::stream<socket>> conn{transport};
            try {
                while (!conn.should_close()) {
                    auto in = co_await http::coroutine::receive(conn);
                    auto doc = ctx_.config.current(ctx_.core);
                    auto t = web::target::parse(in.head.target);
                    auto origin = web::header(in.head, "origin");
                    bool allowed = !origin || std::ranges::contains(doc->server.allowed_origins, *origin);

                    std::string body;
                    bool too_large = false;
                    try {
                        body = co_await web::read_all(in.body,t.path=="/api/v1/worker-enrollments/redeem" ? 2048 : doc->server.max_request_bytes);
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

                    auto method = in.head.method.token();
                    web::reply r;
                    auto c = ctx_;
                    std::optional<worlds::actor> who;
                    std::optional<web::reply> refused;
                    if (t.path=="/api/v1/worker-enrollments/redeem" &&
                        (origin || doc->server.auth.workspace.empty() || web::header(in.head,"x-fughetta-workspace")!=std::optional<std::string_view>{doc->server.auth.workspace})) {
                        refused=error(403,"Worker enrollment workspace was refused");
                    } else if (!(method == "OPTIONS" && origin) && t.path != "/healthz" && t.path!="/api/v1/worker-enrollments/redeem") {
                        refused = authenticate(doc->server.auth, in.head, who);
                        if (who)
                            c.who = &*who;
                    }
                    std::optional<allocation::control::lease> admitted;
                    bool control_read = method == "OPTIONS" || (method == "GET" && (t.path == "/healthz" || t.path == "/api/v1/config/runtime" || t.path == "/api/v1/builds"));
                    // Status polling must not prevent a drained instance from rebuilding its processes.
                    if (!refused && c.runtime && !control_read) {
                        if (auto held = c.runtime->enter(t.path == "/api/v1/config/runtime"))
                            admitted.emplace(std::move(*held));
                        if (!admitted && method == "POST" && (t.path == "/mcp" || t.path == "/mcp/config")) {
                            auto kind = draining_rpc(t.path, body);
                            control_read = kind == drain_access::inspect;
                            if (kind == drain_access::cancel)
                                if (auto held = c.runtime->enter(true)) admitted.emplace(std::move(*held));
                        }
                        if (!admitted && !control_read)
                            refused = error(503, "runtime budget is being applied; retry after the service is ready");
                    }
                    if (refused) {
                        r = std::move(*refused);
                    } else if (method == "OPTIONS" && origin) {
                        r = {.head = {.status = 204,
                                      .fields = {{"access-control-allow-methods", "GET, PUT, POST, DELETE, OPTIONS"},
                                                 {"access-control-allow-headers", "authorization, content-type, accept, mcp-protocol-version"},
                                                 {"access-control-max-age", "600"}}}};
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
            } catch (const std::exception&) {
            }
            peer.close();
        }

    private:
        enum class drain_access { work, inspect, cancel };
        static auto draining_rpc(std::string_view path, std::string_view body) -> drain_access {
            auto rpc = rfl::json::read<mcp::request>(std::string{body});
            if (!rpc) return drain_access::work;
            if (mcp::cancelled_request(*rpc)) return drain_access::cancel;
            if (path != "/mcp" || rpc->method != "tools/call" || !rpc->params) return drain_access::work;
            auto params = rpc->params->to_object();
            if (!params) return drain_access::work;
            auto name = params->get("name");
            if (!name) return drain_access::work;
            auto text = name->to_string();
            if (!text || *text != "jobs") return drain_access::work;
            if (auto arguments = params->get("arguments"))
                if (auto object = arguments->to_object())
                    if (auto action = object->get("action")) {
                        auto value = action->to_string();
                        if (value && *value == "cancel") return drain_access::cancel;
                    }
            return drain_access::inspect;
        }

        static auto error(http::status status, std::string why) -> web::reply {
            std::string_view code = status == http::status{401} ? std::string_view{"unauthenticated"}
                                 : status == http::status{403} ? nodes::code::permission_denied
                                 : status == http::status{404} ? std::string_view{"not_found"}
                                 : status == http::status{405} ? std::string_view{"method_not_allowed"}
                                 : status == http::status{413} ? std::string_view{"too_large"}
                                 : status == http::status{503} ? std::string_view{"unavailable"}
                                                              : std::string_view{"invalid_request"};
            return coded_reply(coded_error{status, code, std::move(why)});
        }

        template <class Head> static auto authenticate(const config::auth& auth, const Head& head, std::optional<worlds::actor>& who) -> std::optional<web::reply> {
            try {
                who = keyring::authenticate(auth, [&head](std::string_view name) { return web::header(head, name); });
            } catch (const keyring::unauthorized& e) {
                auto r = error(401, e.what());
                r.head.fields.push_back({"www-authenticate", std::string{constants::bearer_challenge}});
                return r;
            }
            return std::nullopt;
        }

        template <class Conn, class In>
        auto mcp(Conn& conn, In& in, const std::string& body, std::optional<std::string_view> origin, const worlds::actor* who, bool configuration)
            -> coroutine::task<std::optional<web::reply>> {
            if (auto v = web::header(in.head, "mcp-protocol-version"); v && !mcp::supported(*v)) {
                co_return error(400, std::format("unsupported MCP-Protocol-Version '{}'", *v));
            }
            auto rpc = rfl::json::read<mcp::request>(body);
            if (!rpc) {
                co_return web::json_reply(400, rfl::json::write(mcp::response{.error = mcp::error{mcp::code::parse_error, rpc.error().what()}}));
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
            if (accept.find("text/event-stream") == std::string_view::npos || !mcp::progress_token(*rpc)) {
                mcp::notifier drop;
                auto out = co_await cancellable(*rpc, drop, *who, configuration);
                co_return web::json_reply(200, rfl::json::write(out ? **out : cancelled(*rpc)));
            }

            web::event_stream events;
            http::response head{.status = 200, .fields = {{"content-type", "text/event-stream"}, {"cache-control", "no-cache"}}};
            if (origin) {
                head.fields.push_back({"access-control-allow-origin", std::string{*origin}});
                head.fields.push_back({"vary", "origin"});
            }
            co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<> {
                children.spawn([](Conn& conn, auto token, http::response head, web::event_stream& events) -> coroutine::task<> {
                    co_await http::coroutine::respond(conn, std::move(token), std::move(head), events);
                }(conn, std::move(in.token), std::move(head), events));
                mcp::notifier forward{events};
                auto out = co_await cancellable(*rpc, forward, *who, configuration);
                if (!out)
                    events.push(web::event_stream::message(rfl::json::write(cancelled(*rpc))));
                else if (*out)
                    events.push(web::event_stream::message(rfl::json::write(**out)));
                events.close();
                co_await children.join();
            });
            co_return std::nullopt;
        }

        auto handle(const mcp::request& rpc, mcp::notifier& out, const worlds::actor& who, bool configuration)
            -> coroutine::task<std::optional<mcp::response>> {
            if (configuration)
                co_return co_await settings_.handle(rpc, out, who);
            co_return co_await mcp_.handle(rpc, out, who);
        }

        auto cancellable(const mcp::request& rpc, mcp::notifier& out, const worlds::actor& who, bool configuration)
            -> coroutine::task<std::optional<std::optional<mcp::response>>> {
            struct forward {
                coroutine::stop_source* source;
                void operator()() const noexcept { source->request_stop(); }
            };
            auto parent = co_await coroutine::current_token;
            coroutine::stop_source source;
            std::optional<execution::stop_callback_for_t<coroutine::stop_token, forward>> link;
            if (parent.stop_possible())
                link.emplace(parent, forward{&source});
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
            try { r = co_await coroutine::under(source.get_token(), handle(rpc, out, who, configuration)); }
            catch (...) { failure = std::current_exception(); }
            if (failure) std::rethrow_exception(failure);
            if (!r) {
                if (parent.stop_requested())
                    co_await coroutine::stop;
                co_return std::nullopt;
            }
            co_return std::optional<std::optional<mcp::response>>{std::move(*r)};
        }

        static auto cancelled(const mcp::request& rpc) -> mcp::response {
            return {.id = rpc.id, .error = mcp::error{mcp::code::request_cancelled, "the request was cancelled"}};
        }

        context ctx_;
        mcp::server<toolset> mcp_;
        mcp::server<settings::bound<dispatch>> settings_;
    };
} // namespace front
