export module mcp;
import std;
import constants;
export import utility;
import json;
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
        std::optional<json::value> params;
    };

    struct error {
        json::required<std::int64_t, "code"> code;
        json::required<std::string, "message"> message;
    };

    struct response {
        json::required<std::string_view, "jsonrpc"> version{"2.0"};
        json::optional<key, "id"> id;
        json::optional<json::value, "result"> result;
        json::optional<mcp::error, "error"> error;
    };

    // An audit projection of a reply. The tool result itself is relayed as received.
    inline auto response_failed(std::string_view text, std::string_view expected_id) -> std::optional<bool> {
        using id_view = std::variant<std::int64_t, std::string_view>;
        struct response_fields {
            json::required<id_view, decltype(response::id)::name> id;
            json::optional<json::view, decltype(response::result)::name> result;
            json::optional<json::view, decltype(response::error)::name> error;
        };
        struct result_fields {
            json::optional<bool, "isError"> failed;
        };
        const json::document document{text};
        auto response = json::read<response_fields>(document.root());
        if (!response)
            return {};
        const auto matches = std::visit(
            [&](const auto& id) {
                if constexpr (std::is_same_v<std::decay_t<decltype(id)>, std::string_view>)
                    return expected_id.starts_with("s:") && expected_id.substr(2) == id;
                else {
                    char digits[24];
                    const auto end = std::to_chars(digits, digits + sizeof(digits), id).ptr;
                    return expected_id.starts_with("n:") && expected_id.substr(2) == std::string_view{digits, end};
                }
            },
            response->id.value);
        if (!matches)
            return {};
        if (response->error.value)
            return true;
        if (response->result.value)
            if (auto result = json::read<result_fields>(*response->result.value))
                return result->failed.value.value_or(false);
        return false;
    }

    struct notification {
        std::string jsonrpc = "2.0";
        std::string method;
        json::value params;
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

    struct call_context {
        std::optional<double> client_sync_seconds;
    };

    // Where a call's notifications go: the event stream its client reads, tagged with the progress token the client chose. One without a
    // stream drops them.
    class notifier {
    public:
        notifier() = default;
        explicit notifier(web::event_stream& stream) : stream_(&stream) {}

        // The same stream for a call that asked for progress under `token`; nowhere for one that did not.
        auto tagged(std::optional<json::value> token) const -> notifier {
            notifier out;
            if (token) {
                out.stream_ = stream_;
                out.token_ = std::move(token);
                out.progress_ = std::make_shared<std::int64_t>(0);
            }
            return out;
        }

        void operator()(const notification& n) const {
            if (!stream_)
                return;
            auto sent = n;
            if (token_)
                if (auto o = n.params.to_object()) {
                    json::object p;
                    for (auto& [key, value] : *o)
                        if (n.method != "notifications/progress" || key != "total")
                            p[key] = value;
                    if (n.method == "notifications/progress")
                        p["progress"] = ++*progress_;
                    p["progressToken"] = *token_;
                    sent.params = p;
                }
            stream_->push(web::event_stream::message(json::write(sent)));
        }

    private:
        web::event_stream* stream_ = nullptr;
        std::optional<json::value> token_;
        std::shared_ptr<std::int64_t> progress_;
    };

    struct tool {
        std::string name;
        std::string description;
        json::value inputSchema;
        std::optional<json::value> outputSchema;
    };

    struct block {
        std::string type = "text";
        std::string text;
    };

    struct tool_result {
        std::vector<block> content;
        std::optional<json::value> structuredContent;
        bool isError = false;
    };

    struct call_params {
        std::string name;
        std::optional<json::value> arguments;
        std::optional<json::value> _meta;
    };

    struct tool_outcome {
        std::string text;
        std::optional<json::value> structured;
        bool error = false;
    };

    inline auto progress_token(const request& req) -> std::optional<json::value> {
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
    concept toolset = requires(T& t, std::string_view name, json::value args, notifier& out, const worlds::actor& who) {
        { t.list() } -> std::convertible_to<const std::vector<tool>&>;
        { t.call(name, std::move(args), out, who, call_context{}) } -> std::same_as<coroutine::task<tool_outcome>>;
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
                    answer = reply(*req.id, json::object{});
                else if (req.method == "tools/list")
                    answer = reply(*req.id, json::object({{"tools", json::to_value(tools.list())}}));
                else if (req.method != "tools/call")
                    answer = fail(*req.id, code::method_not_found, "method not found: " + req.method);
            } catch (const std::exception& e) {
                answer = fail(*req.id, code::internal_error, e.what());
            }
            if (answer)
                co_return answer;

            auto params = json::from_value<call_params>(req.params.value_or(json::object{}));
            if (!params)
                co_return fail(*req.id, code::invalid_params, params.error().what());
            auto progress = out.tagged(progress_token(req));
            call_context context;
            if (params->_meta) {
                if (auto meta = params->_meta->to_object()) {
                    if (auto value = meta->get("fughetta/client_sync_seconds")) {
                        auto seconds = json::from_value<double>(*value);
                        if (seconds && std::isfinite(*seconds) && *seconds >= 0 &&
                            *seconds <= std::chrono::duration<double>(constants::client_sync_limit).count())
                            context.client_sync_seconds = *seconds;
                    }
                }
            }
            try {
                auto outcome = co_await tools.call(params->name, params->arguments.value_or(json::object{}), progress, who, context);
                co_return reply(*req.id, json::to_value(tool_result{.content = {block{.text = std::move(outcome.text)}},
                                                                     .structuredContent = std::move(outcome.structured),
                                                                     .isError = outcome.error}));
            } catch (const std::exception& e) {
                co_return fail(*req.id, code::internal_error, std::format("tool '{}' failed: {}", params->name, e.what()));
            }
        }

    private:
        auto initialize(const request& req) const -> json::value {
            std::string version{protocol_version};
            if (req.params)
                if (auto o = req.params->to_object())
                    if (auto v = o->get("protocolVersion"))
                        if (auto s = v->to_string(); s && supported(*s))
                            version = *s;
            return json::object({
                {"protocolVersion", version},
                {"capabilities", json::object({{"tools", json::object({{"listChanged", false}})}})},
                {"serverInfo", json::object({{"name", name_}, {"version", version_}})},
            });
        }

        static auto reply(const key& id, json::value result) -> response { return {.id = {id}, .result = {std::move(result)}}; }
        static auto fail(const key& id, int code, std::string message) -> response { return {.id = {id}, .error = {mcp::error{{code}, {std::move(message)}}}}; }

        std::string name_;
        std::string version_;
    };
} // namespace mcp
