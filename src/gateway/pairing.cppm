module;
#include <sodium.h>
module enrollment;
import std;
import content;

namespace enrollment {
    auto mint(std::size_t bytes) -> std::string {
        static const auto initialized = sodium_init();
        if (initialized < 0)
            throw std::runtime_error("cannot initialize Worker credentials");
        std::vector<unsigned char> random(bytes);
        randombytes_buf(random.data(), random.size());
        std::string encoded(bytes * 2 + 1, '\0');
        sodium_bin2hex(encoded.data(), encoded.size(), random.data(), random.size());
        sodium_memzero(random.data(), random.size());
        encoded.resize(bytes * 2);
        return encoded;
    }
    auto secret_matches(std::string_view digest, std::string_view secret) -> bool {
        if (secret.size() != 64 || digest.size() != 64)
            return false;
        auto presented = content::hash_of(secret).str();
        return sodium_memcmp(presented.data(), digest.data(), 64) == 0;
    }
} // namespace enrollment
