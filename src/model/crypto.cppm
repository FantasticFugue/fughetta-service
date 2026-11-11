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
        auto writable(std::string& s) -> unsigned char* { return reinterpret_cast<unsigned char*>(s.data()); }
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
    auto key::generate() -> key { return from_secret(random_bytes(crypto_sign_SEEDBYTES)); }
    auto key::from_secret(std::string seed) -> key {
        initialize_crypto();
        if (seed.size() != crypto_sign_SEEDBYTES)
            fail(nodes::code::invalid_record, "invalid private key seed length");
        std::string pub(crypto_sign_PUBLICKEYBYTES, '\0');
        std::array<unsigned char, crypto_sign_SECRETKEYBYTES> expanded{};
        if (crypto_sign_seed_keypair(writable(pub), expanded.data(), raw(seed)) != 0)
            fail(nodes::code::storage_failure, "cannot derive signing key");
        sodium_memzero(expanded.data(), expanded.size());
        return {std::move(seed), std::move(pub)};
    }
    auto key::sign(std::string_view input) const -> std::string {
        initialize_crypto();
        if (secret.size() != crypto_sign_SEEDBYTES)
            fail(nodes::code::invalid_record, "invalid signing seed");
        std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> pub{};
        std::array<unsigned char, crypto_sign_SECRETKEYBYTES> expanded{};
        if (crypto_sign_seed_keypair(pub.data(), expanded.data(), raw(secret)) != 0)
            fail(nodes::code::storage_failure, "cannot derive signing key");
        std::string signature(crypto_sign_BYTES, '\0');
        auto status = crypto_sign_detached(writable(signature), nullptr, raw(input), input.size(), expanded.data());
        sodium_memzero(expanded.data(), expanded.size());
        if (status != 0)
            fail(nodes::code::storage_failure, "cannot sign node record");
        return signature;
    }
    auto verify_signature(std::string_view pub, std::string_view input, std::string_view signature) -> bool {
        initialize_crypto();
        return pub.size() == crypto_sign_PUBLICKEYBYTES && signature.size() == crypto_sign_BYTES &&
               crypto_sign_verify_detached(raw(signature), raw(input), input.size(), raw(pub)) == 0;
    }
} // namespace nodes
