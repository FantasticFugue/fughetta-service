export module client;
import utility;
import std;
import coroutine;
import protocol;

export namespace source {
    struct stopped : std::runtime_error, util::retained_exception {
        stopped() : std::runtime_error("source request stopped; mutation outcome may be unknown") {}
        auto retain() const noexcept -> std::exception_ptr override;
    };
    auto stopped::retain() const noexcept -> std::exception_ptr { return util::copy_current_exception(*this); }
    auto call(net::ip::tcp::endpoint source, jobs::node_call request, coroutine::stop_token stop = {}) -> nodes::node_reply;
    auto call(net::ip::tcp::endpoint source, jobs::node_capture request, coroutine::stop_token stop = {}) -> nodes::node_reply;
    auto call(net::ip::tcp::endpoint source, jobs::node_usage request, coroutine::stop_token stop = {}) -> jobs::node_usage_reply;
} // namespace source
