export module http;
import std;
import coroutine;
import meta;
export import httpant;

export namespace web {
    template <io::stream S> using stream = http::stream<S>;

    namespace detail {
        template <http::body_stream B> auto read_body(B& body, std::size_t limit) -> http::task<std::string> {
            std::string out;
            std::array<std::byte, 4096> buf;
            auto& stop = co_await http::current_stop_token();
            while (auto n = co_await body.async_read(buf, stop)) {
                if (out.size() + n > limit)
                    throw std::length_error("request body too large");
                out.append(reinterpret_cast<const char*>(buf.data()), n);
            }
            co_return out;
        }
    } // namespace detail

    template <http::body_stream B> auto read_all(B& body, std::size_t limit = 1 << 20) -> http::task<std::string> {
        return detail::read_body(body, limit);
    }

    template <class Server, class Token>
    auto respond(Server& server, Token token, http::response head, std::string text) -> http::task<> {
        if (!http::find_header(head.fields, "content-length"))
            head.fields.push_back({"content-length", std::to_string(text.size())});
        auto bytes = std::as_bytes(std::span{text});
        http::span_body body{bytes};
        co_await http::coroutine::respond(server, std::move(token), std::move(head), body);
    }

    inline auto json(http::status status = 200) -> http::response { return {.status = status, .fields = {{"content-type", "application/json"}}}; }

    struct reply {
        http::response head;
        std::string body;
    };

    inline auto json_reply(http::status status, std::string body) -> reply { return {.head = json(status), .body = std::move(body)}; }

    inline auto header(const http::request& req, std::string_view name) -> std::optional<std::string_view> { return http::find_header(req.fields, name); }

    class event_stream {
    public:
        void push(std::string_view text) {
            pending_.append(text);
            wake();
        }
        void close() {
            closed_ = true;
            wake();
        }

        static auto message(std::string_view data) -> std::string { return std::format("event: message\ndata: {}\n\n", data); }
        // What was pushed and not read yet, taken without waiting.
        auto take() -> std::string { return std::exchange(pending_, {}); }

        class read_awaiter {
        public:
            read_awaiter(event_stream& s, std::span<std::byte> into, std::stop_token stop) : s_(s), into_(into), stop_(std::move(stop)) {}
            // If the reading coroutine is destroyed while parked, this temporary dies
            // with it; clear the owner's waiter so a later push()/close() never resumes
            // a handle into the freed frame.
            ~read_awaiter() {
                if (handle_ && s_.waiter_ == handle_)
                    s_.waiter_ = {};
            }

            auto await_ready() const noexcept -> bool { return !s_.pending_.empty() || s_.closed_; }
            auto await_suspend(std::coroutine_handle<> h) -> bool {
                if (stop_.stop_requested()) {
                    cancelled_ = true;
                    return false;
                }
                handle_ = h;
                s_.waiter_ = h;
                if (stop_.stop_possible())
                    on_stop_.emplace(stop_, wake_on_stop{this});
                return true;
            }
            auto await_resume() -> std::size_t {
                on_stop_.reset();
                if (cancelled_)
                    throw http::stopped{};
                auto n = std::min(into_.size(), s_.pending_.size());
                std::memcpy(into_.data(), s_.pending_.data(), n);
                s_.pending_.erase(0, n);
                return n;
            }

        private:
            struct wake_on_stop {
                read_awaiter* self;
                void operator()() const noexcept {
                    self->cancelled_ = true;
                    if (auto h = std::exchange(self->s_.waiter_, {}))
                        h.resume();
                }
            };

            event_stream& s_;
            std::span<std::byte> into_;
            std::stop_token stop_;
            std::coroutine_handle<> handle_{};
            bool cancelled_ = false;
            std::optional<std::stop_callback<wake_on_stop>> on_stop_;
        };

        auto async_read(std::span<std::byte> into, std::stop_token stop) -> read_awaiter { return {*this, into, std::move(stop)}; }

    private:
        void wake() {
            if (auto h = std::exchange(waiter_, {}))
                h.resume();
        }

        std::string pending_;
        bool closed_ = false;
        std::coroutine_handle<> waiter_{};
    };
    static_assert(http::body_stream<event_stream>);

    template <meta::fixed_string Pattern> struct path {
        static constexpr std::string_view pattern = Pattern;

        static consteval auto count_params() -> std::size_t {
            std::size_t n = 0;
            for (char c : pattern)
                n += c == '{';
            return n;
        }
        static consteval auto well_formed() -> bool {
            if (pattern.empty() || pattern.front() != '/')
                return false;
            bool open = false;
            for (std::size_t i = 0; i < pattern.size(); ++i) {
                char c = pattern[i];
                if (c == '{') {
                    if (open || pattern[i - 1] != '/')
                        return false;
                    open = true;
                } else if (c == '}') {
                    if (!open || (i + 1 < pattern.size() && pattern[i + 1] != '/'))
                        return false;
                    open = false;
                }
            }
            return !open;
        }
        static_assert(well_formed(), "a route is '/'-separated segments; {name} fills a whole segment");

        static constexpr std::size_t params = count_params();
        using values = std::array<std::string, params>;

        static auto match(std::string_view target) -> std::optional<values> {
            values out;
            std::size_t n = 0;
            auto p = pattern, t = target;
            while (!p.empty() && !t.empty()) {
                auto ps = p.substr(1, p.find('/', 1) - 1);
                auto ts = t.substr(1, t.find('/', 1) - 1);
                if (ps.starts_with('{')) {
                    if (ts.empty())
                        return std::nullopt;
                    out[n++] = std::string{ts};
                } else if (ps != ts) {
                    return std::nullopt;
                }
                p.remove_prefix(std::min(p.size(), ps.size() + 1));
                t.remove_prefix(std::min(t.size(), ts.size() + 1));
            }
            if (!p.empty() || !t.empty())
                return std::nullopt;
            return out;
        }
    };

    struct target {
        std::string path;
        std::map<std::string, std::string, std::less<>> query;

        static auto parse(std::string_view raw) -> target {
            target t;
            auto q = raw.find('?');
            t.path = raw.substr(0, q);
            if (q == std::string_view::npos)
                return t;
            for (auto part : std::views::split(raw.substr(q + 1), '&')) {
                std::string_view kv{part.begin(), part.end()};
                if (kv.empty())
                    continue;
                auto eq = kv.find('=');
                t.query[decode(kv.substr(0, eq))] = eq == std::string_view::npos ? "" : decode(kv.substr(eq + 1));
            }
            return t;
        }

        auto number(std::string_view key, std::uint64_t fallback, std::uint64_t max) const -> std::uint64_t {
            auto it = query.find(key);
            if (it == query.end())
                return fallback;
            std::uint64_t v = 0;
            auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
            if (ec != std::errc{} || end != it->second.data() + it->second.size())
                throw std::invalid_argument(std::format("bad {}", key));
            return std::min(v, max);
        }
        // A query value, borrowed from this target: valid until the target is destroyed.
        auto text(std::string_view key) const -> std::optional<std::string_view> {
            auto it = query.find(key);
            return it == query.end() ? std::nullopt : std::optional<std::string_view>{it->second};
        }
        // A percent-encoded path segment, as written.
        static auto unescape(std::string_view segment) -> std::string { return decode(segment); }

    private:
        static auto decode(std::string_view s) -> std::string {
            std::string out;
            for (std::size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '+')
                    out += ' ';
                else if (s[i] == '%' && i + 2 < s.size()) {
                    unsigned v = 0;
                    std::from_chars(s.data() + i + 1, s.data() + i + 3, v, 16);
                    out += static_cast<char>(v);
                    i += 2;
                } else
                    out += s[i];
            }
            return out;
        }
    };
} // namespace web
