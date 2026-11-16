module;
#include <sodium.h>
module codec;
import std;

namespace nodes {
    namespace {
        void initialize_crypto() {
            static const int ready = sodium_init();
            if (ready < 0)
                fail(nodes::code::storage_failure, "libsodium initialization failed");
        }
        auto raw(std::string_view s) -> const unsigned char* { return reinterpret_cast<const unsigned char*>(s.data()); }
    } // namespace
    auto random_bytes(std::size_t n) -> std::string {
        initialize_crypto();
        std::string out(n, '\0');
        randombytes_buf(out.data(), out.size());
        return out;
    }
    auto digest_of(std::string_view bytes) -> std::string {
        initialize_crypto();
        std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
        if (crypto_hash_sha256(digest.data(), raw(bytes), bytes.size()) != 0)
            fail(nodes::code::storage_failure, "SHA-256 failed");
        return hex(std::string_view{reinterpret_cast<const char*>(digest.data()), digest.size()});
    }
} // namespace nodes
