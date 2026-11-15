export module websocket;
import std;
import coroutine;
import http;

export namespace web::ws {
    using http::operator co_await;
    auto random_mask() -> http::websocket::mask;
    auto random_key() -> std::string;

    // One reader and one writer share a scheduler. A write waits for the peer's
    // transport; there is no detached output queue. Message boundaries disappear
    // here so the jobs channel continues to consume a plain byte stream.
    template <http::byte_stream S> class stream {
    public:
        stream(S& socket, http::websocket::side side, std::span<const std::byte> pending = {}, http::websocket::limits limits = {})
            : state_(std::make_unique<state>(socket, side, limits)) {
            state_->decoder_.feed(pending);
        }

        auto read_some(std::span<std::byte> into) -> coroutine::task<std::size_t> {
            if (into.empty())
                co_return 0;
            for (;;) {
                if (state_->offset_ < state_->payload_.size()) {
                    auto count = std::min(into.size(), state_->payload_.size() - state_->offset_);
                    std::ranges::copy(std::span{state_->payload_}.subspan(state_->offset_, count), into.begin());
                    state_->offset_ += count;
                    co_return count;
                }
                if (state_->received_close_)
                    co_return 0;
                std::optional<http::websocket::frame> frame;
                std::exception_ptr failed;
                std::uint16_t status = 1002;
                try {
                    frame = state_->decoder_.next();
                } catch (const http::websocket::failure& e) {
                    status = e.status;
                    failed = std::current_exception();
                }
                if (failed) {
                    std::array reason{std::byte(status >> 8), std::byte(status & 255)};
                    co_await send(http::websocket::opcode::close, reason);
                    std::rethrow_exception(failed);
                }
                if (!frame) {
                    std::array<std::byte, 16384> bytes;
                    auto count = co_await read(std::span{bytes}.first(std::min(bytes.size(), state_->decoder_.available())));
                    if (!count)
                        throw std::runtime_error("WebSocket transport ended without a close frame");
                    state_->decoder_.feed(std::span{bytes}.first(count));
                    continue;
                }
                using enum http::websocket::opcode;
                if (frame->kind == ping) {
                    co_await send(pong, frame->payload);
                    continue;
                }
                if (frame->kind == pong) {
                    if (state_->ping_nonce_ && std::ranges::equal(frame->payload, *state_->ping_nonce_))
                        state_->ping_nonce_.reset();
                    continue;
                }
                if (frame->kind == close) {
                    state_->received_close_ = true;
                    co_await send(close, frame->payload);
                    co_return 0;
                }
                state_->payload_ = std::move(frame->payload);
                state_->offset_ = 0;
            }
        }

        auto write_some(std::span<const std::byte> from) -> coroutine::task<std::size_t> {
            if (from.empty())
                co_return 0;
            auto count = std::min<std::size_t>(from.size(), 16384);
            co_await send(http::websocket::opcode::binary, from.first(count));
            co_return count;
        }

        auto close(std::uint16_t status = 1000) -> coroutine::task<> {
            std::array reason{std::byte(status >> 8), std::byte(status & 255)};
            co_await send(http::websocket::opcode::close, reason);
        }
        auto awaiting_pong() const noexcept -> bool { return state_->ping_nonce_.has_value(); }
        auto ping() -> coroutine::task<> {
            std::array<std::byte, 8> nonce;
            auto first = random_mask(), second = random_mask();
            std::ranges::copy(first, nonce.begin());
            std::ranges::copy(second, nonce.begin() + 4);
            state_->ping_nonce_ = nonce;
            co_await send(http::websocket::opcode::ping, nonce);
        }

    private:
        auto read(std::span<std::byte> bytes) -> http::task<std::size_t> {
            auto& stop = co_await http::current_stop_token();
            co_return co_await state_->socket_.async_read(bytes, stop);
        }
        auto write(std::span<const std::byte> bytes) -> http::task<std::size_t> {
            auto& stop = co_await http::current_stop_token();
            co_return co_await state_->socket_.async_write(bytes, stop);
        }
        auto send(http::websocket::opcode kind, std::span<const std::byte> payload) -> coroutine::task<> {
            auto held = co_await state_->output_.lock();
            if (state_->sent_close_) {
                if (kind == http::websocket::opcode::close)
                    co_return;
                throw std::runtime_error("WebSocket is closing");
            }
            if (state_->received_close_ && kind != http::websocket::opcode::close)
                throw std::runtime_error("WebSocket peer has closed");
            if (kind == http::websocket::opcode::close)
                state_->sent_close_ = true;
            auto masking = state_->side_ == http::websocket::side::client ? std::optional{random_mask()} : std::nullopt;
            auto bytes = http::websocket::encode(kind, payload, true, masking);
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                auto count = co_await write(std::span<const std::byte>{bytes}.subspan(offset));
                if (!count)
                    throw std::runtime_error("WebSocket write made no progress");
                offset += count;
            }
        }
        struct state {
            S& socket_;
            http::websocket::side side_;
            http::websocket::decoder decoder_;
            coroutine::mutex output_;
            std::vector<std::byte> payload_;
            std::size_t offset_ = 0;
            bool sent_close_ = false, received_close_ = false;
            std::optional<std::array<std::byte, 8>> ping_nonce_;
            state(S& socket, http::websocket::side side, http::websocket::limits limits) : socket_(socket), side_(side), decoder_(side, limits) {}
        };
        std::unique_ptr<state> state_;
    };
} // namespace web::ws
