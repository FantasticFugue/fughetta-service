export module json:value;
import std;
import meta;

export namespace json {
    struct error {
        std::string message;
        auto what() const -> const std::string& { return message; }
    };
    template <class T> using result = std::expected<T, error>;
    inline auto unexpected(std::string message) { return std::unexpected(error{std::move(message)}); }

    struct value;
    using array = std::vector<value>;

    struct object : std::vector<std::pair<std::string, value>> {
        using base = std::vector<std::pair<std::string, value>>;
        using base::base;
        using base::operator[];

        auto get(std::string_view key) const -> result<value>;
        auto find(this auto& fields, std::string_view key);
        auto operator[](std::string_view key) -> value&;
        void insert(std::string key, value item);
    };

    struct value {
        std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, array, object> data;

        value() = default;
        value(std::nullptr_t) {}
        value(std::nullopt_t) {}
        value(bool flag) : data(flag) {}
        template <std::signed_integral T> value(T number) : data(static_cast<std::int64_t>(number)) {}
        template <std::unsigned_integral T> value(T number) : data(static_cast<std::uint64_t>(number)) {}
        template <std::floating_point T> value(T number) : data(static_cast<double>(number)) {}
        value(const char* text) : data(std::string{text}) {}
        value(std::string_view text) : data(std::string{text}) {}
        value(std::string text) : data(std::move(text)) {}
        value(array items) : data(std::move(items)) {}
        value(object fields) : data(std::move(fields)) {}

        auto get() const& -> const auto& { return data; }
        auto get() & -> auto& { return data; }

        template <class T> auto as() const -> result<T> {
            if (auto* item = std::get_if<T>(&data))
                return *item;
            return std::unexpected(error{"unexpected JSON value type"});
        }
        auto to_object() const -> result<object> { return as<object>(); }
        auto to_array() const -> result<array> { return as<array>(); }
        auto to_string() const -> result<std::string> { return as<std::string>(); }
        auto to_bool() const -> result<bool> { return as<bool>(); }
        auto to_int64() const -> result<std::int64_t> {
            if (const auto* number = std::get_if<std::uint64_t>(&data); number && std::in_range<std::int64_t>(*number))
                return static_cast<std::int64_t>(*number);
            return as<std::int64_t>();
        }
        auto to_int() const -> result<int> {
            if (const auto number = to_int64(); number && std::in_range<int>(*number))
                return static_cast<int>(*number);
            return unexpected("expected JSON int");
        }
        auto operator==(const value&) const -> bool = default;
    };

    inline auto object::find(this auto& fields, std::string_view key) {
        return std::ranges::find(fields, key, &base::value_type::first);
    }
    inline auto object::get(std::string_view key) const -> result<value> {
        if (auto item = find(key); item != end())
            return item->second;
        return unexpected("missing JSON field " + std::string{key});
    }
    inline auto object::operator[](std::string_view key) -> value& {
        if (auto item = find(key); item != end())
            return item->second;
        return emplace_back(std::string{key}, value{}).second;
    }
    inline void object::insert(std::string key, value item) { emplace_back(std::move(key), std::move(item)); }

    template <class T, meta::fixed_string Name> struct member {
        using underlying_type = T;
        static constexpr auto name = Name;
        T value;
        auto get() const -> const T& { return value; }
    };

    template <class T, meta::fixed_string Name> using required = member<T, Name>;
    template <class T, meta::fixed_string Name> using optional = member<std::optional<T>, Name>;

    struct defaults {};
    struct strict {};
    struct camel_case {};

    template <class T> struct annotated {
        using underlying_type = T;
        T item{};

        annotated() = default;
        template <class U> requires(!std::same_as<std::remove_cvref_t<U>, annotated> && std::constructible_from<T, U>)
        annotated(U&& input) : item(std::forward<U>(input)) {}
        annotated(T input) : item(std::move(input)) {}
        auto get() & -> T& { return item; }
        auto get() const& -> const T& { return item; }
        auto get() && -> T&& { return std::move(item); }
        auto value() & -> T& { return item; }
        auto value() const& -> const T& { return item; }
        auto value() && -> T&& { return std::move(item); }
        template <class U>
            requires (!std::derived_from<std::remove_cvref_t<U>, annotated> &&
                      requires(T& storage, U&& input) { storage = std::forward<U>(input); })
        auto operator=(U&& input) -> annotated& {
            item = std::forward<U>(input);
            return *this;
        }
        auto operator==(const annotated&) const -> bool = default;
    };

    template <meta::fixed_string Text, class T> struct described : annotated<T> {
        using annotated<T>::annotated;
        using annotated<T>::operator=;
        static constexpr auto description = Text;
    };
    template <meta::fixed_string Name, class T> struct named : annotated<T> {
        using annotated<T>::annotated;
        using annotated<T>::operator=;
        static constexpr auto name = Name;
    };
    template <class T> struct skip : annotated<T> {
        using annotated<T>::annotated;
        using annotated<T>::operator=;
        static constexpr bool skipped = true;
    };
    template <class T> struct flatten : annotated<T> {
        using annotated<T>::annotated;
        using annotated<T>::operator=;
        static constexpr bool flattened = true;
    };

    template <meta::fixed_string... Names> class literal {
        std::size_t index_{};

    public:
        static constexpr std::array names{Names.view()...};
        constexpr literal() = default;
        template <meta::fixed_string Name> static consteval auto make() -> literal {
            static_assert(((Name == Names) || ...), "unknown literal");
            return literal{Name.view()};
        }
        constexpr literal(std::string_view text) {
            const auto found = std::ranges::find(names, text);
            if (found == names.end())
                throw std::invalid_argument("invalid JSON literal " + std::string{text});
            index_ = static_cast<std::size_t>(found - names.begin());
        }
        literal(const std::string& text) : literal(std::string_view{text}) {}
        literal(const char* text) : literal(std::string_view{text}) {}
        auto view() const -> std::string_view { return names[index_]; }
        auto str() const -> std::string { return std::string{view()}; }
        auto name() const -> std::string { return str(); }
        auto value() const -> std::size_t { return index_; }
        auto operator<=>(const literal&) const = default;
        auto operator==(const literal&) const -> bool = default;
        auto operator==(std::string_view text) const -> bool { return view() == text; }
        auto operator==(const char* text) const -> bool { return view() == text; }
        auto operator==(const std::string& text) const -> bool { return view() == text; }
    };

    template <meta::fixed_string Tag, class... Choices> class tagged {
        std::variant<Choices...> data_;

    public:
        static constexpr auto discriminator = Tag;
        using alternatives = std::tuple<Choices...>;
        tagged() = default;
        template <class T> requires (!std::same_as<std::remove_cvref_t<T>, tagged> && std::constructible_from<decltype(data_), T>)
        tagged(T&& item) : data_(std::forward<T>(item)) {}
        auto variant() & -> auto& { return data_; }
        auto variant() const& -> const auto& { return data_; }
        template <class F> decltype(auto) visit(F&& f) & { return std::visit(std::forward<F>(f), data_); }
        template <class F> decltype(auto) visit(F&& f) const& { return std::visit(std::forward<F>(f), data_); }
        auto operator==(const tagged&) const -> bool = default;
    };
}
