export module protocol;
import std;
export import utility;
import reflection;
import coroutine;
import meta;
export import content;
export import world;
export import nodes;
export import build;
export import language;

// A content digest is sent as its lowercase hex text, so the frames and the JSON schema are unchanged by the
// digest being its own type.
export namespace rfl {
    template <> struct Reflector<content::hash> {
        using ReflType = std::string;
        static auto from(const content::hash& h) -> std::string { return std::string{h.view()}; }
        static auto to(const std::string& value) -> content::hash { return content::hash{value}; }
    };
} // namespace rfl

export namespace jobs {
    struct platform {
        std::string os;
        std::string arch;
    };

    struct hello {
        std::string name;
        std::string auth;
        jobs::platform platform;
        std::uint32_t compute_threads = 0;
        std::uint32_t max_inflight = 1;
        std::uint32_t protocol = 0;
        std::vector<std::string> tools;
        std::optional<std::string> worker_id;
        std::optional<std::string> workspace_id;
        std::uint32_t heartbeat_seconds=2;
    };

    struct admission {
        bool accepted=false;
        std::string code;
        std::string worker_id;
    };

    struct heartbeat {
        std::uint32_t running = 0;
        std::uint32_t queued = 0;
    };

    struct progress {
        std::uint64_t done = 0;
        std::uint64_t total = 0;
        std::string stage;
    };

    // The progress of one call, kept where the call is awaited: the core that hears a report appends it, and the caller reads what is new
    // when it looks.
    class progress_log {
    public:
        void push(progress p) {
            std::lock_guard held{lock_};
            entries_.push_back(std::move(p));
        }
        // The reports since `seen`, which moves past them.
        auto since(std::size_t& seen) -> std::vector<progress> {
            std::lock_guard held{lock_};
            std::vector<progress> out(entries_.begin() + static_cast<std::ptrdiff_t>(std::min(seen, entries_.size())), entries_.end());
            seen = entries_.size();
            return out;
        }

    private:
        std::mutex lock_;
        std::vector<progress> entries_;
    };

    struct cancel {
        std::string why;
    };

    struct failure {
        std::string message;
        std::string code;
    };

    namespace why {
        inline constexpr std::string_view stale_blob = "stale_blob";
        inline constexpr std::string_view stale_head = "stale_head";
        inline constexpr std::string_view receipt_conflict = "receipt_conflict";
        inline constexpr std::string_view unknown_operation = "unknown_operation";
    } // namespace why

    struct hash_file {
        rfl::Description<"Path relative to the worker's data root; first segment names the dataset", std::string> path;
        std::optional<std::string> algo;
    };

    struct digest {
        std::string hex;
        std::uint64_t bytes = 0;
    };

    inline auto locate(const hash_file& j) -> std::string_view {
        std::string_view p = j.path.value();
        return p.substr(0, p.find('/'));
    }

    struct sync_offer {
        std::string project;
        content::manifest tree;
    };

    struct sync_plan {
        content::hash snapshot;
        std::vector<content::hash> missing;
    };

    // The most plain bytes one piece of a blob holds.
    inline constexpr std::uint64_t part_size = 4u << 20;

    // A piece of a blob. `offset` and `total` count plain bytes; `packed` bytes are one archive frame of the piece.
    struct source_part {
        content::hash digest;
        std::uint64_t offset = 0;
        std::uint64_t total = 0;
        std::vector<std::byte> bytes;
        bool packed = false;
    };

    struct source_bytes {
        std::vector<source_part> parts;
    };

    struct authority_ack {
        std::uint64_t stored = 0;
    };

    inline auto source_token() -> std::string {
        auto value = std::getenv("SOURCE_TOKEN");
        if (!value || !*value)
            throw std::invalid_argument("set SOURCE_TOKEN for authenticated source RPC");
        return value;
    }

    struct node_call {
        std::string project;
        worlds::actor actor;
        nodes::node_request request;
        bool reading = false;
    };
    // What the node repositories hold: each project's stored history on disk and the size of its main source.
    struct node_usage {
        worlds::actor actor;
    };
    struct project_usage {
        std::string project;
        std::uint64_t storage_bytes = 0, source_bytes = 0;
    };
    struct node_usage_reply {
        std::vector<project_usage> projects;
        // Exported packages, which count toward the storage limit too.
        std::uint64_t packages_bytes = 0;
    };
    // Everything the workspace stores, as its storage limit counts it.
    inline auto stored(const node_usage_reply& r) -> std::uint64_t {
        auto n = r.packages_bytes;
        for (auto& p : r.projects)
            n += p.storage_bytes;
        return n;
    }
    // A preparation a worker ran, handed to the source: it anchors and publishes the outline and keeps the receipt.
    struct analysis_finish {
        language::request request;
        language::result result;
        std::optional<semantic::outline> outline;
        std::vector<std::string> surveyed;
        std::string service;
        std::optional<std::string> survey_error;
    };

    struct node_capture {
        std::string project;
        std::vector<std::string> ignore;
        worlds::actor actor;
        std::string operation;
        std::vector<std::string> expected;
        std::string intent;
        std::uint64_t file_limit = 0, project_limit = 0, workspace_limit = 0;
    };

    using step = builds::step;

    // A language server to ask about a tree's documents once its steps succeeded.
    struct survey {
        std::string provider;
        std::vector<std::string> argv;
        rfl::Generic initialization;
        std::map<std::string, std::string> env;
        std::uint32_t seconds = 600;
    };

    struct build_request {
        std::string project;
        std::string tree;
        content::hash snapshot;
        std::vector<step> steps;
        std::uint32_t timeout_s = 1800;
        std::uint64_t output_limit = 1u << 20;
        std::map<std::string, std::string> env;
        std::optional<std::uint64_t> max_memory_bytes;
        std::optional<std::uint32_t> max_cpu_seconds;
        // Files the steps must write anew, removed before they run; artifacts to measure once every step succeeded; and a survey of the
        // tree after them. A request with any of these is a preparation: its steps must leave the tracked files as they were.
        std::vector<std::string> replaced;
        std::vector<builds::context> measure;
        std::optional<jobs::survey> survey;
    };

    struct step_report {
        std::string name;
        std::int32_t exit_code = -1;
        bool timed_out = false;
        double seconds = 0;
        std::string output;
        bool truncated = false;
    };

    struct build_report {
        std::string worker;
        std::optional<std::string> worker_id;
        std::string tree;
        std::vector<step_report> steps;
        double seconds = 0;
        std::vector<builds::artifact_result> measured;
        // The survey's outline and the documents it covered, or why it failed.
        std::optional<semantic::outline> outline;
        std::vector<std::string> surveyed;
        std::optional<std::string> survey_error;

        auto ok() const -> bool {
            return !steps.empty() && std::ranges::all_of(steps, [](auto& s) { return s.exit_code == 0 && !s.timed_out; });
        }
    };

    template <meta::fixed_string Name> inline constexpr std::uint32_t kind = meta::fnv1a(Name);

    template <meta::fixed_string Name, class Message> struct signal {
        using message = Message;
        static constexpr std::uint32_t code = kind<Name>;
    };

    template <meta::fixed_string Name, class Request, class Reply> struct rpc {
        using request = Request;
        using reply = Reply;
        static constexpr std::uint32_t code = kind<Name>;
    };

    using signals = meta::list<signal<"hello", hello>,signal<"worker.admission",admission>, signal<"heartbeat", heartbeat>, signal<"progress", progress>, signal<"cancel", cancel>>;

    using requests = meta::list<rpc<"node.call", node_call, nodes::node_reply>, rpc<"node.capture", node_capture, nodes::node_reply>,
                                rpc<"node.usage", node_usage, node_usage_reply>,
                                rpc<"analysis.prepare", language::request, language::result>,
                                rpc<"analysis.finish", analysis_finish, language::result>, rpc<"hash_file", hash_file, digest>,
                                rpc<"sync.offer", sync_offer, sync_plan>, rpc<"sync.parts", source_bytes, authority_ack>, rpc<"build", build_request, build_report>>;

    inline constexpr std::uint32_t result = kind<"result">;
    inline constexpr std::uint32_t failure_code = kind<"failure">;

    namespace detail {
        template <class... S, class... R> consteval auto codes(meta::list<S...>, meta::list<R...>) {
            return std::array{S::code..., R::code..., result, failure_code};
        }
        template <class... S, class... R> consteval auto listed_once(meta::list<S...>, meta::list<R...>) -> bool {
            return meta::unique<typename S::message..., typename R::request...>;
        }

        template <class T> struct for_request {
            template <class E> struct match : std::bool_constant<std::same_as<typename E::request, T>> {};
        };
        template <class T> struct for_signal {
            template <class E> struct match : std::bool_constant<std::same_as<typename E::message, T>> {};
        };
    } // namespace detail

    static_assert(meta::distinct(detail::codes(signals{}, requests{})), "two messages hash to one kind: rename one");

    inline constexpr std::uint32_t protocol_version = [] {
        std::uint32_t h = 2166136261u;
        for (auto c : detail::codes(signals{}, requests{}))
            h = (h ^ c) * 16777619u;
        return h;
    }();
    static_assert(detail::listed_once(signals{}, requests{}), "a message type is listed twice");

    template <class T> using rpc_of = meta::find_t<detail::for_request<T>::template match, requests>;
    template <class T> using signal_of = meta::find_t<detail::for_signal<T>::template match, signals>;

    template <class R>
    concept request = !std::is_void_v<rpc_of<R>>;
    template <class M>
    concept one_way = !std::is_void_v<signal_of<M>>;

    template <request R> using reply_t = typename rpc_of<R>::reply;

    template <class T>
        requires request<T> || one_way<T>
    inline constexpr std::uint32_t code = [] {
        if constexpr (request<T>)
            return rpc_of<T>::code;
        else
            return signal_of<T>::code;
    }();
} // namespace jobs

export namespace transport {
    struct frame {
        std::uint32_t kind = 0;
        std::uint64_t corr = 0;
        std::vector<std::byte> payload;
    };

    inline constexpr std::size_t header_size = 4 + 4 + 8;
    inline constexpr std::size_t max_payload = 64u << 20;

    template <class T> struct codec {
        static auto encode(const T& value) -> std::vector<std::byte> {
            auto text = rfl::json::write(value);
            auto bytes = std::as_bytes(std::span{text});
            return {bytes.begin(), bytes.end()};
        }
        static auto decode(std::span<const std::byte> payload) -> T {
            if (payload.empty())
                throw std::invalid_argument("protocol: empty frame payload");
            return rfl::json::read<T>(std::string_view{reinterpret_cast<const char*>(payload.data()), payload.size()}).value();
        }
    };

    template <> struct codec<jobs::source_bytes> {
        static auto encode(const jobs::source_bytes& push) -> std::vector<std::byte> {
            std::vector<std::byte> out;
            auto put = [&](const void* p, std::size_t n) {
                if (!n)
                    return;   // avoid forming a range from a possibly-null pointer
                auto* b = static_cast<const std::byte*>(p);
                out.insert(out.end(), b, b + n);
            };
            auto count = static_cast<std::uint32_t>(push.parts.size());
            put(&count, 4);
            for (auto& b : push.parts) {
                if (!content::is_hash(b.digest))
                    throw std::invalid_argument("source_bytes: bad digest");
                std::uint64_t len = b.bytes.size();
                std::uint8_t packed = b.packed;
                put(b.digest.data(), 64);
                put(&b.offset, 8);
                put(&b.total, 8);
                put(&packed, 1);
                put(&len, 8);
                put(b.bytes.data(), b.bytes.size());
            }
            return out;
        }
        static auto decode(std::span<const std::byte> in) -> jobs::source_bytes {
            auto take = [&](void* p, std::size_t n) {
                if (in.size() < n)
                    throw std::length_error("source_bytes: truncated");
                std::memcpy(p, in.data(), n);
                in = in.subspan(n);
            };
            std::uint32_t count;
            take(&count, 4);
            jobs::source_bytes push;
            for (std::uint32_t i = 0; i < count; ++i) {
                jobs::source_part b;
                b.digest.resize(64);
                take(b.digest.data(), 64);
                take(&b.offset, 8);
                take(&b.total, 8);
                std::uint8_t packed;
                take(&packed, 1);
                b.packed = packed != 0;
                std::uint64_t len;
                take(&len, 8);
                if (len > in.size())
                    throw std::length_error("source_bytes: truncated");
                b.bytes.assign(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(len));
                in = in.subspan(len);
                push.parts.push_back(std::move(b));
            }
            return push;
        }
    };

    template <class T> auto encode(const T& value) -> std::vector<std::byte> { return codec<T>::encode(value); }

    template <class T> auto decode(const frame& f) -> T { return codec<T>::decode(f.payload); }

    struct remote_error : std::runtime_error {
        remote_error(std::string message, std::string_view code) : std::runtime_error(std::move(message)), code(code) {}
        std::string code;
    };

    using call_error = std::variant<jobs::failure, std::exception_ptr>;
    template <class T> using call_result = std::expected<T, call_error>;

    [[noreturn]] inline void raise_call_error(call_error error) {
        std::visit(
            []<class E>(E&& e) -> void {
                if constexpr (std::same_as<std::remove_cvref_t<E>, jobs::failure>)
                    throw remote_error(std::move(e.message), std::move(e.code));
                else
                    std::rethrow_exception(e);
            },
            std::move(error));
        std::unreachable();
    }

    struct coded_error : std::runtime_error {
        coded_error(std::string_view code, std::string message) : std::runtime_error(std::move(message)), code(code) {}
        std::string code;
    };

    template <io::stream S> class channel {
    public:
        explicit channel(S& peer) noexcept : peer_(&peer) {}
        channel(channel&&) = delete;

        template <class T> auto send(std::uint32_t kind, std::uint64_t corr, const T& value) -> coroutine::task<> {
            auto body = codec<T>::encode(value);
            if (body.size() > max_payload)
                throw std::length_error("frame too large");
            std::vector<std::byte> out(header_size + body.size());
            auto len = static_cast<std::uint32_t>(body.size());
            std::memcpy(out.data(), &len, 4);
            std::memcpy(out.data() + 4, &kind, 4);
            std::memcpy(out.data() + 8, &corr, 8);
            std::memcpy(out.data() + header_size, body.data(), body.size());
            auto turn = co_await writing_.lock();
            co_await io::async_write(*peer_, out);
        }

        auto recv(std::size_t limit=max_payload) -> coroutine::task<frame> {
            std::array<std::byte, header_size> head;
            co_await io::async_read(*peer_, head);
            std::uint32_t len;
            frame f;
            std::memcpy(&len, head.data(), 4);
            std::memcpy(&f.kind, head.data() + 4, 4);
            std::memcpy(&f.corr, head.data() + 8, 8);
            if (len > max_payload || len>limit)
                throw std::length_error("frame too large");
            f.payload.resize(len);
            co_await io::async_read(*peer_, f.payload);
            co_return f;
        }

        template <jobs::one_way M> auto post(const M& message, std::uint64_t corr = 0) -> coroutine::task<> { return send(jobs::code<M>, corr, message); }

        template <class Reply> auto reply(std::uint64_t corr, const Reply& value) -> coroutine::task<> { return send(jobs::result, corr, value); }
        auto fail(std::uint64_t corr, std::string message, std::string code = {}) -> coroutine::task<> {
            co_await send(jobs::failure_code, corr, jobs::failure{std::move(message), std::move(code)});
        }

        template <jobs::request Req> auto call(const Req& req, jobs::progress_log* progress = nullptr) -> coroutine::task<call_result<jobs::reply_t<Req>>> {
            using Res = jobs::reply_t<Req>;
            if (ended_)
                co_return std::unexpected(call_error{ended_});
            pending slot{.progress = progress};
            auto corr = next_corr_++;
            pending_.emplace(corr, &slot);
            auto drop = scope_exit{[&] { pending_.erase(corr); }};
            auto sent = co_await coroutine::expect(send(jobs::code<Req>, corr, req));
            if (!sent)
                co_return std::unexpected(call_error{std::move(sent.error())});
            auto received = co_await slot;
            if (slot.cancelled) {
                try {
                    co_await coroutine::under(coroutine::stop_token{}, post(jobs::cancel{"the caller stopped"}, corr));
                } catch (const std::exception&) {
                }
                co_await coroutine::stop;
            }
            if (!received)
                co_return std::unexpected(call_error{std::move(received.error())});
            auto& f = *received;
            if (f.kind == jobs::failure_code) {
                co_return std::unexpected(call_error{decode<jobs::failure>(f)});
            }
            co_return decode<Res>(f);
        }

        template <std::invocable<const frame&> Other> auto pump(Other other) -> coroutine::task<> {
            std::exception_ptr why;
            bool stopped = false;
            try {
                for (;;) {
                    auto next = co_await coroutine::stop_point(recv());
                    if (!next) {
                        stopped = true;
                        break;
                    }
                    auto& f = *next;
                    auto it = pending_.find(f.corr);
                    if (it == pending_.end()) {
                        other(f);
                        continue;
                    }
                    if (f.kind == jobs::code<jobs::progress>) {
                        if (auto* log = it->second->progress)
                            log->push(decode<jobs::progress>(f));
                    } else {
                        it->second->fulfil(std::move(f));
                    }
                }
            } catch (...) {
                why = std::current_exception();
            }
            if (!why && !stopped)
                why = std::make_exception_ptr(io::closed{});
            ended_ = why ? why : std::make_exception_ptr(io::closed{});
            std::vector<pending*> waiting;
            for (auto& [corr, slot] : pending_)
                waiting.push_back(slot);
            for (auto* slot : waiting)
                stopped ? slot->stop() : slot->abort(why);
            if (stopped)
                co_await coroutine::stop;
        }

    private:
        struct pending {
            jobs::progress_log* progress = nullptr;
            std::optional<frame> value;
            std::exception_ptr error;
            bool stopped = false;
            bool cancelled = false;
            std::coroutine_handle<> waiter;
            std::coroutine_handle<> (*stop_chain)(std::coroutine_handle<>) noexcept = nullptr;

            struct on_stop {
                pending* self;
                void operator()() const noexcept {
                    self->cancelled = true;
                    self->wake();
                }
            };
            std::optional<execution::stop_callback_for_t<coroutine::stop_token, on_stop>> watch;

            bool await_ready() const noexcept { return value || error; }
            template <class P> auto await_suspend(std::coroutine_handle<P> h) noexcept -> std::coroutine_handle<> {
                stop_chain = [](std::coroutine_handle<> frame) noexcept {
                    return std::coroutine_handle<P>::from_address(frame.address()).promise().unhandled_stopped();
                };
                auto token = execution::get_stop_token(execution::get_env(h.promise()));
                if (stopped)
                    return h.promise().unhandled_stopped();
                if (token.stop_requested()) {
                    cancelled = true;
                    return h;
                }
                waiter = h;
                if (token.stop_possible())
                    watch.emplace(token, on_stop{this});
                return std::noop_coroutine();
            }
            auto await_resume() -> std::expected<frame, std::exception_ptr> {
                watch.reset();
                if (cancelled)
                    return frame{};
                if (error)
                    return std::unexpected(error);
                return std::move(*value);
            }
            void fulfil(frame f) {
                value = std::move(f);
                wake();
            }
            void abort(std::exception_ptr e) {
                error = e;
                wake();
            }
            void stop() {
                stopped = true;
                if (auto frame = std::exchange(waiter, {})) {
                    auto next = stop_chain(frame);
                    next.resume();
                }
            }
            void wake() {
                if (auto w = std::exchange(waiter, {}))
                    w.resume();
            }
        };

        template <class F> struct scope_exit {
            F fn;
            ~scope_exit() { fn(); }
        };

        S* peer_;
        coroutine::mutex writing_;
        std::uint64_t next_corr_ = 1;
        std::exception_ptr ended_;
        std::unordered_map<std::uint64_t, pending*> pending_;
    };

    template <io::stream S> class dispatcher {
    public:
        struct exchange {
            channel<S>& chan;
            std::uint64_t corr;

            auto progress(jobs::progress p) -> coroutine::task<> { co_await chan.post(p, corr); }
        };

        template <jobs::request Req, class F>
            requires std::same_as<std::invoke_result_t<F&, Req, exchange&>, coroutine::task<jobs::reply_t<Req>>>
        void on(F handler) {
            handlers_[jobs::code<Req>] = [handler = std::move(handler)](channel<S>& chan, frame f) mutable -> coroutine::task<> {
                return serve<Req>(handler, chan, std::move(f));
            };
        }

        auto handles(std::uint32_t kind) const -> bool { return handlers_.contains(kind); }

        auto handle(channel<S>& chan, frame f) -> coroutine::task<> {
            if (f.kind == jobs::code<jobs::cancel>) {
                if (auto r = running_.find({&chan, f.corr}); r != running_.end())
                    r->second->request_stop();
                co_return;
            }
            auto it = handlers_.find(f.kind);
            if (it == handlers_.end()) {
                co_await chan.fail(f.corr, unknown_kind(f.kind));
                co_return;
            }
            struct forward {
                coroutine::stop_source* source;
                void operator()() const noexcept { source->request_stop(); }
            };
            auto parent = co_await coroutine::current_token;
            coroutine::stop_source source;
            std::optional<execution::stop_callback_for_t<coroutine::stop_token, forward>> link;
            if (parent.stop_possible())
                link.emplace(parent, forward{&source});
            std::pair key{&chan, f.corr};
            running_[key] = &source;
            struct forget {
                dispatcher* self;
                std::pair<channel<S>*, std::uint64_t> key;
                ~forget() { self->running_.erase(key); }
            } done{this, key};
            co_await coroutine::under(source.get_token(), it->second(chan, std::move(f)));
        }

    private:
        template <class Req, class F> static auto serve(F& handler, channel<S>& chan, frame f) -> coroutine::task<> {
            exchange ex{chan, f.corr};
            std::optional<jobs::reply_t<Req>> out;
            std::string error, code;
            try {
                out.emplace(co_await handler(decode<Req>(f), ex));
            } catch (const coded_error& e) {
                error = e.what();
                code = e.code;
            } catch (const std::exception& e) {
                error = e.what();
            }
            if (out)
                co_await chan.reply(f.corr, *out);
            else
                co_await chan.fail(f.corr, std::move(error), std::move(code));
        }

        static auto unknown_kind(std::uint32_t kind) -> std::string { return std::format("unknown request kind {:#010x}", kind); }

        std::unordered_map<std::uint32_t, util::move_only_function<coroutine::task<>(channel<S>&, frame)>> handlers_;
        std::map<std::pair<channel<S>*, std::uint64_t>, coroutine::stop_source*> running_;
    };
} // namespace transport
