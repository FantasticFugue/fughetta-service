module client;
import native;
import codec;
import protocol;

namespace source {
    namespace detail {
        template <jobs::request Req>
        auto exchange(native::scheduler& io, net::ip::tcp::endpoint endpoint, Req request, bool& issued) -> coroutine::task<jobs::reply_t<Req>> {
            auto connected = co_await coroutine::deadline(io, std::chrono::seconds{10}, io::connect(io, endpoint));
            if (!connected)
                throw std::runtime_error("source connect timed out");
            auto sock = std::move(*connected);
            transport::channel<native::socket> chan{sock};
            co_return co_await coroutine::with_scope([&](coroutine::scope& children) -> coroutine::task<jobs::reply_t<Req>> {
                children.spawn(chan.pump([](const transport::frame&) {}));
                co_await chan.post(jobs::hello{.name = "gateway", .auth = jobs::source_token(), .protocol = jobs::protocol_version});
                issued = true;
                auto result = co_await coroutine::deadline(io, std::chrono::seconds{120}, chan.call(request));
                if (!result)
                    throw std::runtime_error("source request timed out; mutation outcome may be unknown");
                if (!*result)
                    transport::raise_call_error(std::move(result->error()));
                co_return std::move(**result);
            });
        }

        template <jobs::request Req> auto invoke(net::ip::tcp::endpoint endpoint, Req request, coroutine::stop_token stop) -> jobs::reply_t<Req> {
            native::context resource;
            auto io = resource.get_scheduler();
            bool issued = false;
            // A node write whose answer is lost may have committed: the caller learns that as outcome_unknown and asks for its receipt.
            bool node_write = false;
            if constexpr (std::same_as<Req, jobs::node_call>)
                node_write = !request.reading;
            if constexpr (std::same_as<Req, jobs::node_capture>)
                node_write = true;
            auto unknown = [](std::string_view why) {
                return transport::remote_error(std::format("the outcome of this change is unknown; ask for its receipt: {}", why),
                                               nodes::code::outcome_unknown);
            };
            try {
                auto result = cores::run(resource, exchange(io, endpoint, std::move(request), issued), stop);
                if (!result)
                    throw stopped{};
                return std::move(*result);
            } catch (const stopped&) {
                throw;
            } catch (const transport::remote_error& e) {
                if (node_write && issued && e.code.empty())
                    throw unknown(e.what());
                if (e.code == "invalid_argument")
                    throw std::invalid_argument(e.what());
                throw;
            } catch (const std::exception& e) {
                if (node_write && issued)
                    throw unknown(e.what());
                throw;
            }
        }
    } // namespace detail

    auto call(net::ip::tcp::endpoint source, jobs::node_call request, coroutine::stop_token stop) -> nodes::node_reply {
        return detail::invoke(source, std::move(request), stop);
    }
    auto call(net::ip::tcp::endpoint source, jobs::node_capture request, coroutine::stop_token stop) -> nodes::node_reply {
        return detail::invoke(source, std::move(request), stop);
    }
    auto call(net::ip::tcp::endpoint source, jobs::node_usage request, coroutine::stop_token stop) -> jobs::node_usage_reply {
        return detail::invoke(source, std::move(request), stop);
    }
    auto call(net::ip::tcp::endpoint source, language::request request, coroutine::stop_token stop) -> language::result {
        return detail::invoke(source, std::move(request), stop);
    }
    auto call(net::ip::tcp::endpoint source, jobs::analysis_finish request, coroutine::stop_token stop) -> language::result {
        return detail::invoke(source, std::move(request), stop);
    }
} // namespace source
