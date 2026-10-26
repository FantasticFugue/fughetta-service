export module mcp;
import std;
export import utility;
import reflection;
import coroutine;
import http;
import world;

export namespace mcp {
    inline constexpr std::array<std::string_view, 3> protocol_versions = {"2025-06-18", "2025-03-26", "2024-11-05"};
    inline constexpr std::string_view protocol_version = protocol_versions[0];

    inline auto supported(std::string_view version) -> bool { return std::ranges::contains(protocol_versions, version); }

    namespace code {
        inline constexpr int parse_error = -32700;
        inline constexpr int invalid_request = -32600;
        inline constexpr int method_not_found = -32601;
        inline constexpr int invalid_params = -32602;
        inline constexpr int internal_error = -32603;
        inline constexpr int request_cancelled = -32800;
    } // namespace code

    using key = std::variant<std::int64_t, std::string>;

    struct request {
        std::string jsonrpc;
        std::string method;
        std::optional<key> id;
        std::optional<rfl::Generic> params;
    };

    struct error {
        int code = 0;
        std::string message;
    };

    struct response {
        std::string jsonrpc = "2.0";
        std::optional<key> id;
        std::optional<rfl::Generic> result;
        std::optional<mcp::error> error;
    };

    struct notification {
        std::string jsonrpc = "2.0";
        std::string method;
        rfl::Generic params;
    };

    inline auto key_text(const key& k) -> std::string {
        return std::visit(
            [](const auto& v) {
                if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>)
                    return "s:" + v;
                else
                    return "n:" + std::to_string(v);
            },
            k);
    }

    inline auto cancelled_request(const request& r) -> std::optional<key> {
        if (r.method != "notifications/cancelled" || !r.params)
            return std::nullopt;
        auto params = r.params->to_object();
        if (!params)
            return std::nullopt;
        auto id = params->get("requestId");
        if (!id)
            return std::nullopt;
        if (auto n = id->to_int64())
            return key{*n};
        if (auto t = id->to_string())
            return key{*t};
        return std::nullopt;
    }

    // Where a call's notifications go: the event stream its client reads, tagged with the progress token the client chose. One without a
    // stream drops them.
    class notifier {
    public:
        notifier() = default;
        explicit notifier(web::event_stream& stream) : stream_(&stream) {}

        // The same stream for a call that asked for progress under `token`; nowhere for one that did not.
        auto tagged(std::optional<rfl::Generic> token) const -> notifier {
            notifier out;
            if (token) {
                out.stream_ = stream_;
                out.token_ = std::move(token);
            }
            return out;
        }

        void operator()(const notification& n) const {
            if (!stream_)
                return;
            auto sent = n;
            if (token_)
                if (auto o = n.params.to_object()) {
                    auto p = *o;
                    p["progressToken"] = *token_;
                    sent.params = p;
                }
            stream_->push(web::event_stream::message(rfl::json::write(sent)));
        }

    private:
        web::event_stream* stream_ = nullptr;
        std::optional<rfl::Generic> token_;
    };

    inline auto object(std::initializer_list<std::pair<std::string, rfl::Generic>> fields) -> rfl::Generic::Object {
        rfl::Generic::Object out;
        for (auto& [k, v] : fields)
            out.insert(k, v);
        return out;
    }

    struct tool {
        std::string name;
        std::string description;
        rfl::Generic inputSchema;
        std::optional<rfl::Generic> outputSchema;
    };

    struct block {
        std::string type = "text";
        std::string text;
    };

    struct tool_result {
        std::vector<block> content;
        std::optional<rfl::Generic> structuredContent;
        bool isError = false;
    };

    struct call_params {
        std::string name;
        std::optional<rfl::Generic> arguments;
        std::optional<rfl::Generic> _meta;
    };

    struct tool_outcome {
        std::string text;
        std::optional<rfl::Generic> structured;
        bool error = false;
    };

    inline auto progress_token(const request& req) -> std::optional<rfl::Generic> {
        if (req.method != "tools/call" || !req.params)
            return std::nullopt;
        auto params = req.params->to_object();
        if (!params)
            return std::nullopt;
        auto meta = params->get("_meta");
        if (!meta)
            return std::nullopt;
        auto m = meta->to_object();
        if (!m)
            return std::nullopt;
        auto token = m->get("progressToken");
        if (!token)
            return std::nullopt;
        return *token;
    }

    template <class T>
    concept toolset = requires(T& t, std::string_view name, rfl::Generic args, notifier& out, const worlds::actor& who) {
        { t.list() } -> std::convertible_to<const std::vector<tool>&>;
        { t.call(name, std::move(args), out, who) } -> std::same_as<coroutine::task<tool_outcome>>;
    };

    template <toolset Tools> class server {
    public:
        Tools tools;

        server(std::string name, std::string version, Tools t) : tools(std::move(t)), name_(std::move(name)), version_(std::move(version)) {}

        auto handle(const request& req, notifier& out, const worlds::actor& who) -> coroutine::task<std::optional<response>> {
            if (!req.id)
                co_return std::nullopt;
            std::optional<response> answer;
            try {
                if (req.method == "initialize")
                    answer = reply(*req.id, initialize(req));
                else if (req.method == "ping")
                    answer = reply(*req.id, rfl::Generic::Object{});
                else if (req.method == "tools/list")
                    answer = reply(*req.id, object({{"tools", rfl::to_generic(tools.list())}}));
                else if (req.method != "tools/call")
                    answer = fail(*req.id, code::method_not_found, "method not found: " + req.method);
            } catch (const std::exception& e) {
                answer = fail(*req.id, code::internal_error, e.what());
            }
            if (answer)
                co_return answer;

            auto params = rfl::from_generic<call_params>(req.params.value_or(rfl::Generic::Object{}));
            if (!params)
                co_return fail(*req.id, code::invalid_params, params.error().what());
            auto progress = out.tagged(progress_token(req));
            auto outcome = co_await tools.call(params->name, params->arguments.value_or(rfl::Generic::Object{}), progress, who);
            co_return reply(*req.id, rfl::to_generic(tool_result{.content = {block{.text = std::move(outcome.text)}},
                                                                 .structuredContent = std::move(outcome.structured),
                                                                 .isError = outcome.error}));
        }

    private:
        auto initialize(const request& req) const -> rfl::Generic {
            std::string version{protocol_version};
            if (req.params)
                if (auto o = req.params->to_object())
                    if (auto v = o->get("protocolVersion"))
                        if (auto s = v->to_string(); s && supported(*s))
                            version = *s;
            return object({
                {"protocolVersion", version},
                {"capabilities", object({{"tools", object({{"listChanged", false}})}})},
                {"serverInfo", object({{"name", name_}, {"version", version_}})},
            });
        }

        static auto reply(const key& id, rfl::Generic result) -> response { return {.id = id, .result = std::move(result)}; }
        static auto fail(const key& id, int code, std::string message) -> response { return {.id = id, .error = mcp::error{code, std::move(message)}}; }

        std::string name_;
        std::string version_;
    };
} // namespace mcp
