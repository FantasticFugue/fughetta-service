module;
#include <sodium.h>
module websocket;
import std;
import http;

namespace web::ws {
    namespace {
        void entropy(std::span<std::byte> bytes) {
            static const auto initialized = sodium_init();
            if (initialized < 0)
                throw std::runtime_error("cannot initialize Worker transport entropy");
            randombytes_buf(bytes.data(), bytes.size());
        }
    } // namespace
    auto random_mask() -> http::websocket::mask {
        http::websocket::mask bytes;
        entropy(bytes);
        return bytes;
    }
    auto random_key() -> std::string {
        std::array<std::byte, 16> bytes;
        entropy(bytes);
        return http::websocket::key(bytes);
    }
} // namespace web::ws
