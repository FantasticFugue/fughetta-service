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
} // namespace source
