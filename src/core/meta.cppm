export module meta;
import std;

export namespace meta {
    template <std::size_t N> struct fixed_string {
        char chars[N]{};

        consteval fixed_string(const char (&s)[N]) { std::ranges::copy(s, chars); }

        constexpr auto view() const noexcept -> std::string_view { return {chars, N - 1}; }
        constexpr operator std::string_view() const noexcept { return view(); }

        template <std::size_t M> constexpr bool operator==(const fixed_string<M>& o) const noexcept { return view() == o.view(); }
    };

    constexpr auto fnv1a(std::string_view s) noexcept -> std::uint32_t {
        std::uint32_t h = 2166136261u;
        for (char c : s) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 16777619u;
        }
        return h;
    }

    template <class... T> struct list {
        static constexpr std::size_t size = sizeof...(T);
    };

    template <class T, std::size_t N> constexpr auto distinct(const std::array<T, N>& values) -> bool {
        for (std::size_t i = 0; i < N; ++i)
            for (std::size_t j = i + 1; j < N; ++j)
                if (values[i] == values[j])
                    return false;
        return true;
    }

    template <class X, class... T> inline constexpr std::size_t count = (std::size_t{std::same_as<X, T>} + ... + 0);

    template <class... T> inline constexpr bool unique = ((count<T, T...> == 1) && ...);

    template <template <class> class Match, class List> struct find;
    template <template <class> class Match, class First, class... Rest> struct find<Match, list<First, Rest...>> {
        using type = std::conditional_t<Match<First>::value, First, typename find<Match, list<Rest...>>::type>;
    };
    template <template <class> class Match> struct find<Match, list<>> {
        using type = void;
    };
    template <template <class> class Match, class List> using find_t = typename find<Match, List>::type;
} // namespace meta
