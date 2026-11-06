module;
#include <sodium.h>
export module content;
import std;

export namespace content {

    class sha256 {
    public:
        using digest = std::array<std::uint8_t, 32>;
        sha256() {
            static const int initialized = sodium_init();
            if (initialized < 0)
                throw std::runtime_error("cryptographic initialization failed");
            crypto_hash_sha256_init(&state_);
        }
        void update(std::span<const std::byte> data) {
            if (!data.empty())
                crypto_hash_sha256_update(&state_, reinterpret_cast<const unsigned char*>(data.data()), data.size());
        }
        void update(std::string_view s) { update(std::as_bytes(std::span{s})); }
        auto finish() -> digest {
            digest out;
            crypto_hash_sha256_final(&state_, out.data());
            return out;
        }

    private:
        crypto_hash_sha256_state state_{};
    };

    // A content digest as its own type, not a bare string, so a hash cannot be passed where a path,
    // a user or another string is meant and the digest maps and digests read as one kind. It still reads as a
    // `std::string_view` (hex text) for comparison, formatting and the frames, and builds from one.
    class hash {
    public:
        hash() = default;
        hash(std::string_view text) : text_(text) {}
        hash(const char* text) : text_(text) {}
        hash(std::string text) : text_(std::move(text)) {}

        auto view() const noexcept -> std::string_view { return text_; }
        operator std::string_view() const noexcept { return text_; }
        auto str() const -> const std::string& { return text_; }
        auto empty() const noexcept -> bool { return text_.empty(); }
        auto size() const noexcept -> std::size_t { return text_.size(); }
        auto data() noexcept -> char* { return text_.data(); }
        auto data() const noexcept -> const char* { return text_.data(); }
        void resize(std::size_t n) { text_.resize(n); }

        friend auto operator==(const hash&, const hash&) -> bool = default;
        auto operator<=>(const hash&) const -> std::strong_ordering = default;

    private:
        std::string text_;
    };

    inline auto to_hex(const sha256::digest& d) -> hash {
        static constexpr char digits[] = "0123456789abcdef";
        hash out(std::string(64, '0'));
        auto* encoded = out.data();
        auto* bytes = d.data();
        for (std::size_t i = 0; i < d.size(); ++i) {
            encoded[2 * i] = digits[bytes[i] >> 4];
            encoded[2 * i + 1] = digits[bytes[i] & 15];
        }
        return out;
    }

    inline auto hash_of(std::span<const std::byte> data) -> hash {
        sha256 h;
        h.update(data);
        return to_hex(h.finish());
    }
    inline auto hash_of(std::string_view data) -> hash { return hash_of(std::as_bytes(std::span{data})); }

    // Constant-time equality for secrets: unequal lengths differ at once; equal lengths compare in constant time.
    inline auto secure_equal(std::string_view a, std::string_view b) -> bool {
        if (a.size() != b.size())
            return false;
        if (a.empty())
            return true;
        return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
    }

    inline auto is_hash(std::string_view h) -> bool {
        return h.size() == 64 && std::ranges::all_of(h, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    inline auto checked_path(std::string_view p) -> std::string_view {
        auto bad = [&](std::string_view why) -> std::string_view { throw std::invalid_argument(std::format("bad path '{}': {}", p, why)); };
        if (p.empty())
            return bad("empty");
        if (p.front() == '/' || p.find('\\') != std::string_view::npos)
            return bad("must be relative, '/' separated");
        for (auto part : std::views::split(p, '/')) {
            std::string_view s{part.begin(), part.end()};
            if (s.empty() || s == "." || s == "..")
                return bad("empty, '.' or '..' segment");
        }
        if (std::ranges::any_of(p, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; }))
            return bad("control character");
        return p;
    }

    inline auto checked_name(std::string_view n) -> std::string_view {
        checked_path(n);
        if (n.find('/') != std::string_view::npos)
            throw std::invalid_argument(std::format("bad name '{}': one segment only", n));
        return n;
    }

    namespace detail {
        constexpr auto match_segment(std::string_view p, std::string_view s) -> bool {
            std::size_t pi = 0, si = 0, star = std::string_view::npos, mark = 0;
            while (si < s.size()) {
                if (pi < p.size() && (p[pi] == '?' || p[pi] == s[si])) {
                    ++pi;
                    ++si;
                } else if (pi < p.size() && p[pi] == '*') {
                    star = pi++;
                    mark = si;
                } else if (star != std::string_view::npos) {
                    pi = star + 1;
                    si = ++mark;
                } else
                    return false;
            }
            while (pi < p.size() && p[pi] == '*')
                ++pi;
            return pi == p.size();
        }

        constexpr auto split(std::string_view path) -> std::vector<std::string_view> {
            std::vector<std::string_view> out;
            for (auto part : std::views::split(path, '/'))
                out.emplace_back(part.begin(), part.end());
            return out;
        }

        constexpr auto match_segments(std::span<const std::string_view> p, std::span<const std::string_view> s) -> bool {
            if (p.empty())
                return s.empty();
            if (p.front() == "**") {
                for (std::size_t skip = 0; skip <= s.size(); ++skip)
                    if (match_segments(p.subspan(1), s.subspan(skip)))
                        return true;
                return false;
            }
            return !s.empty() && match_segment(p.front(), s.front()) && match_segments(p.subspan(1), s.subspan(1));
        }
    } // namespace detail

    constexpr auto glob(std::string_view pattern, std::string_view path) -> bool {
        auto p = detail::split(pattern);
        auto s = detail::split(path);
        return detail::match_segments(p, s);
    }

    constexpr auto ignored(std::string_view pattern, std::string_view path) -> bool {
        bool dir_only = pattern.ends_with('/');
        if (dir_only)
            pattern.remove_suffix(1);
        bool anchored = pattern.starts_with('/');
        if (anchored)
            pattern.remove_prefix(1);
        if (pattern.empty())
            return false;
        auto segs = detail::split(path);
        if (segs.empty())
            return false;
        if (!anchored && pattern.find('/') == std::string_view::npos) {
            auto upto = dir_only ? segs.size() - 1 : segs.size();
            for (std::size_t i = 0; i < upto; ++i)
                if (detail::match_segment(pattern, segs[i]))
                    return true;
            return false;
        }
        auto pats = detail::split(pattern);
        auto upto = dir_only ? segs.size() - 1 : segs.size();
        for (std::size_t n = 1; n <= upto; ++n)
            if (detail::match_segments(pats, std::span{segs}.first(n)))
                return true;
        return false;
    }

    inline auto ignored(std::span<const std::string> patterns, std::string_view path) -> bool {
        return std::ranges::any_of(patterns, [&](const std::string& p) { return ignored(p, path); });
    }

    struct entry {
        std::string path;
        hash digest;
        std::uint64_t size = 0;
        bool executable = false;
    };

    struct manifest {
        std::vector<entry> files;

        auto id() const -> hash {
            sha256 h;
            for (auto& e : files)
                h.update(std::format("{}\x1f{}\x1f{}\x1f{}\n", e.path, e.digest.view(), e.size, e.executable ? 1 : 0));
            return to_hex(h.finish());
        }
        auto total_size() const -> std::uint64_t {
            std::uint64_t n = 0;
            for (auto& e : files)
                n += e.size;
            return n;
        }
    };
} // namespace content

template <> struct std::formatter<content::hash, char> : std::formatter<std::string_view, char> {
    template <class FormatContext> auto format(const content::hash& h, FormatContext& ctx) const {
        return std::formatter<std::string_view, char>::format(h.view(), ctx);
    }
};

template <> struct std::hash<content::hash> {
    auto operator()(const content::hash& h) const noexcept -> std::size_t { return std::hash<std::string_view>{}(h.view()); }
};
