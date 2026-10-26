export module client;
import std;
import coroutine;
import protocol;

export namespace source {
    struct stopped : std::runtime_error {
        stopped() : std::runtime_error("source request stopped; mutation outcome may be unknown") {}
    };
    auto call(net::ip::tcp::endpoint source, jobs::node_call request, coroutine::stop_token stop = {}) -> nodes::node_reply;
    auto call(net::ip::tcp::endpoint source, jobs::node_capture request, coroutine::stop_token stop = {}) -> nodes::node_reply;
    auto call(net::ip::tcp::endpoint source, jobs::node_usage request, coroutine::stop_token stop = {}) -> jobs::node_usage_reply;
    auto call(net::ip::tcp::endpoint source, language::request request, coroutine::stop_token stop = {}) -> language::result;
    auto call(net::ip::tcp::endpoint source, jobs::analysis_finish request, coroutine::stop_token stop = {}) -> language::result;
} // namespace source
