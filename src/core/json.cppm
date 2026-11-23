module;
#include <yyjson.h>

export module json;
import std;
import fields;
import meta;
export import :value;

namespace json::detail {
    template <class T> using bare = std::remove_cvref_t<T>;
    template <class T> concept annotation = requires(const T& value) { typename T::underlying_type; value.get(); };
    template <class T> concept is_literal = requires(const T& value) { T::names; value.view(); };
    template <class T> concept is_tagged = requires(const T& value) { T::discriminator; typename T::alternatives; value.variant(); };
    template <class T> concept mapping = requires { typename T::key_type; typename T::mapped_type; };
    template <class T> concept tuple = requires { std::tuple_size<T>::value; };
    template <class T> concept sequence = std::ranges::range<T> && !std::convertible_to<const T&, std::string_view> && !mapping<T> && !tuple<T>;
    template <class T> inline constexpr bool optional = false;
    template <class T> inline constexpr bool optional<std::optional<T>> = true;
    template <class T> inline constexpr bool variant = false;
    template <class... T> inline constexpr bool variant<std::variant<T...>> = true;
    template <class T> inline constexpr bool skipped = requires { T::skipped; };
    template <class T> inline constexpr bool flattened = requires { T::flattened; };
    template <class T> inline constexpr bool unique = false;
    template <class T, class D> inline constexpr bool unique<std::unique_ptr<T, D>> = true;
    template <class T> inline constexpr bool shared = false;
    template <class T> inline constexpr bool shared<std::shared_ptr<T>> = true;
    template <class T> inline constexpr bool nullable = optional<T> || unique<T> || shared<T> || std::is_pointer_v<T>;
    template <class T> consteval bool required() {
        if constexpr (annotation<T>)
            return required<typename T::underlying_type>();
        else
            return !nullable<T>;
    }
    template <class T> bool present(const T& item) {
        if constexpr (annotation<T>)
            return present(item.get());
        else if constexpr (nullable<T>)
            return static_cast<bool>(item);
        else
            return true;
    }

    template <class... Options> struct policy {
        static constexpr bool defaults = (std::same_as<Options, json::defaults> || ...);
        static constexpr bool strict = (std::same_as<Options, json::strict> || ...);
        static constexpr bool camel = (std::same_as<Options, camel_case> || ...);
    };

    template <std::size_t N> struct field_key {
        std::array<char, N> bytes{};
        std::size_t length{};
        constexpr auto view() const -> std::string_view { return {bytes.data(), length}; }
        constexpr operator std::string_view() const { return view(); }
    };

    template <class Record, std::size_t I, class Field, class Policy> consteval auto key() {
        constexpr auto name = [] {
            if constexpr (requires { Field::name.view(); })
                return Field::name.view();
            else
                return meta::member_name<Record, I>();
        }();
        field_key<name.size()> out;
        bool upper = false;
        for (const auto ch : name) {
            if (Policy::camel && ch == '_') {
                upper = true;
                continue;
            }
            out.bytes[out.length++] = upper && ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
            upper = false;
        }
        return out;
    }

    template <class Policy, class Record, class F> constexpr void fields(Record& item, F&& f) {
        meta::for_each_member(item, [&]<std::size_t I>(auto& field) {
            using field_type = bare<decltype(field)>;
            if constexpr (flattened<field_type>)
                fields<Policy>(field.get(), f);
            else if constexpr (!skipped<field_type>) {
                static constexpr auto name = key<bare<Record>, I, field_type, Policy>();
                f(name, field);
            }
        });
    }

    struct document_deleter {
        void operator()(yyjson_doc* doc) const { yyjson_doc_free(doc); }
    };
    using document_ptr = std::unique_ptr<yyjson_doc, document_deleter>;
    inline thread_local document_ptr last_read;

    inline auto parse(std::string_view text, yyjson_read_err* error = nullptr) -> document_ptr {
        return document_ptr{yyjson_read_opts(const_cast<char*>(text.data()), text.size(), YYJSON_READ_BIGNUM_AS_RAW, nullptr, error)};
    }

    inline auto string_of(const yyjson_val* item) -> std::string_view {
        return {yyjson_get_str(item), yyjson_get_len(item)};
    }

    inline auto signed_integer(const yyjson_val* item) -> std::optional<std::int64_t> {
        if (yyjson_is_sint(item))
            return yyjson_get_sint(item);
        if (yyjson_is_uint(item) && std::in_range<std::int64_t>(yyjson_get_uint(item)))
            return static_cast<std::int64_t>(yyjson_get_uint(item));
        return {};
    }

    struct dom_input {
        yyjson_val* item;
        auto null() const -> bool { return yyjson_is_null(item); }
        template <class T> auto scalar() const -> result<T> {
            if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view>) {
                if (yyjson_is_str(item))
                    return T{string_of(item)};
            } else if constexpr (std::same_as<T, bool>) {
                if (yyjson_is_bool(item))
                    return yyjson_get_bool(item);
            } else if constexpr (std::floating_point<T>) {
                if (yyjson_is_num(item))
                    return static_cast<T>(yyjson_get_num(item));
            } else if constexpr (std::unsigned_integral<T>) {
                if (yyjson_is_uint(item) && std::in_range<T>(yyjson_get_uint(item)))
                    return static_cast<T>(yyjson_get_uint(item));
                if (yyjson_is_sint(item) && std::in_range<T>(yyjson_get_sint(item)))
                    return static_cast<T>(yyjson_get_sint(item));
            } else {
                if (auto number = signed_integer(item); number && std::in_range<T>(*number))
                    return static_cast<T>(*number);
            }
            return json::unexpected("unexpected JSON scalar");
        }
        auto at(std::string_view key) const -> std::optional<dom_input> {
            if (auto* child = yyjson_obj_getn(item, key.data(), key.size()))
                return dom_input{child};
            return {};
        }
        template <class F> auto object(F&& f) const -> result<void> {
            yyjson_obj_iter iter;
            if (!yyjson_obj_iter_init(item, &iter))
                return json::unexpected("expected JSON object");
            while (auto* key = yyjson_obj_iter_next(&iter)) {
                if (auto status = f(string_of(key), dom_input{yyjson_obj_iter_get_val(key)}); !status)
                    return status;
            }
            return {};
        }
        template <class F> auto array(F&& f) const -> result<void> {
            yyjson_arr_iter iter;
            if (!yyjson_arr_iter_init(item, &iter))
                return json::unexpected("expected JSON array");
            return f(yyjson_arr_size(item), [&](auto&& visit) -> result<void> {
                while (auto* child = yyjson_arr_iter_next(&iter)) {
                    if (auto status = visit(dom_input{child}); !status)
                        return status;
                }
                return {};
            });
        }
        auto dynamic() const -> result<value> {
            switch (yyjson_get_type(item)) {
            case YYJSON_TYPE_NULL: return value{};
            case YYJSON_TYPE_BOOL: return value{yyjson_get_bool(item)};
            case YYJSON_TYPE_NUM:
                if (yyjson_is_real(item))
                    return value{yyjson_get_real(item)};
                if (auto number = signed_integer(item))
                    return *number;
                return value{yyjson_get_uint(item)};
            case YYJSON_TYPE_STR: return value{std::string{string_of(item)}};
            case YYJSON_TYPE_RAW: return json::unexpected("JSON number is outside the supported range");
            case YYJSON_TYPE_ARR: {
                json::array out;
                auto status = array([&](auto size, auto visit) {
                    out.reserve(size);
                    return visit([&](auto child) -> result<void> {
                        auto decoded = child.dynamic();
                        if (!decoded)
                            return std::unexpected(std::move(decoded.error()));
                        out.push_back(std::move(*decoded));
                        return {};
                    });
                });
                if (!status)
                    return std::unexpected(std::move(status.error()));
                return value{std::move(out)};
            }
            case YYJSON_TYPE_OBJ: {
                json::object out;
                auto status = object([&](auto name, auto child) -> result<void> {
                    auto decoded = child.dynamic();
                    if (!decoded)
                        return std::unexpected(std::move(decoded.error()));
                    out.insert(std::string{name}, std::move(*decoded));
                    return {};
                });
                if (!status)
                    return std::unexpected(std::move(status.error()));
                return value{std::move(out)};
            }
            default: std::unreachable();
            }
        }
    };

    struct value_input {
        const value& item;
        auto null() const -> bool { return std::holds_alternative<std::monostate>(item.data); }
        template <class T> auto scalar() const -> result<T> {
            if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view>) {
                if (const auto* text = std::get_if<std::string>(&item.data))
                    return T{*text};
            } else if constexpr (std::same_as<T, bool>) {
                if (const auto* flag = std::get_if<bool>(&item.data))
                    return *flag;
            } else {
                if (const auto* number = std::get_if<std::int64_t>(&item.data)) {
                    if constexpr (std::floating_point<T>)
                        return static_cast<T>(*number);
                    else if (std::in_range<T>(*number))
                        return static_cast<T>(*number);
                }
                if (const auto* number = std::get_if<std::uint64_t>(&item.data)) {
                    if constexpr (std::floating_point<T>)
                        return static_cast<T>(*number);
                    else if (std::in_range<T>(*number))
                        return static_cast<T>(*number);
                }
                if constexpr (std::floating_point<T>)
                    if (const auto* number = std::get_if<double>(&item.data))
                        return static_cast<T>(*number);
            }
            return json::unexpected("unexpected JSON scalar");
        }
        auto at(std::string_view key) const -> std::optional<value_input> {
            if (const auto* object = std::get_if<json::object>(&item.data))
                if (auto field = object->find(key); field != object->end())
                    return value_input{field->second};
            return {};
        }
        template <class F> auto object(F&& f) const -> result<void> {
            const auto* object = std::get_if<json::object>(&item.data);
            if (!object)
                return json::unexpected("expected JSON object");
            for (const auto& [name, child] : *object) {
                if (auto status = f(name, value_input{child}); !status)
                    return status;
            }
            return {};
        }
        template <class F> auto array(F&& f) const -> result<void> {
            const auto* array = std::get_if<json::array>(&item.data);
            if (!array)
                return json::unexpected("expected JSON array");
            return f(array->size(), [&](auto&& visit) -> result<void> {
                for (const auto& child : *array) {
                    if (auto status = visit(value_input{child}); !status)
                        return status;
                }
                return {};
            });
        }
        auto dynamic() const -> result<value> { return item; }
    };

    template <class T, class Policy, class Input> auto decode(Input input) -> result<T>;

    template <class Policy, class T, class Input> auto decode_into(T& target, Input input) -> result<void> {
        auto decoded = decode<T, Policy>(input);
        if (!decoded)
            return std::unexpected(std::move(decoded.error()));
        target = std::move(*decoded);
        return {};
    }

    template <class T> using member_tuple = decltype(meta::members(std::declval<T&>()));
    template <class T, std::size_t I> using member_type = bare<std::tuple_element_t<I, member_tuple<T>>>;
    template <class T, std::size_t... I> consteval bool flat(std::index_sequence<I...>) { return (!flattened<member_type<T, I>> && ...); }

    template <class T, class Policy, class Input> struct member_setter {
        std::string_view name;
        std::uint32_t hash;
        result<void> (*assign)(T&, Input);
        bool mandatory;

        template <std::size_t I> static consteval auto make() -> member_setter {
            using field = member_type<T, I>;
            if constexpr (skipped<field>) {
                return {};
            } else {
                const std::string_view name = spelling<I>.view();
                return {name, meta::fnv1a(name), [](T& record, Input child) {
                    return decode_into<Policy>(std::get<I>(meta::members(record)), child);
                }, required<field>()};
            }
        }

        template <std::size_t I> static constexpr auto spelling = key<T, I, member_type<T, I>, Policy>();
    };

    template <class T, class Policy, class Input, std::size_t... I> auto read_record(Input input, std::index_sequence<I...>) -> result<T> {
        using setter = member_setter<T, Policy, Input>;
        static constexpr std::array<setter, sizeof...(I)> setters{setter::template make<I>()...};
        static constexpr auto mandatory = ((setter::template make<I>().mandatory ? (std::uint64_t{1} << I) : 0) | ... | 0);
        T out{};
        std::uint64_t seen = 0;
        std::size_t next = 0;
        auto status = input.object([&](std::string_view name, Input child) -> result<void> {
            auto match = setters.begin() + next;
            if (match == setters.end() || match->name != name || !match->assign) {
                if constexpr (sizeof...(I) > 8) {
                    const auto hash = meta::fnv1a(name);
                    match = std::ranges::find_if(setters, [&](const setter& field) {
                        return field.hash == hash && field.name == name && field.assign;
                    });
                } else {
                    match = std::ranges::find_if(setters, [&](const setter& field) { return field.name == name && field.assign; });
                }
            }
            if (match != setters.end()) {
                const auto index = static_cast<std::size_t>(match - setters.begin());
                const auto bit = std::uint64_t{1} << index;
                if ((seen & bit) == 0) {
                    if (auto assigned = match->assign(out, child); !assigned)
                        return assigned;
                    seen |= bit;
                    next = index + 1;
                }
            } else if constexpr (Policy::strict) {
                return json::unexpected("unexpected JSON field " + std::string{name});
            }
            return {};
        });
        if (!status)
            return std::unexpected(std::move(status.error()));
        if constexpr (!Policy::defaults && mandatory != 0) {
            if (const auto missing = mandatory & ~seen)
                return json::unexpected("missing JSON field " + std::string{setters[std::countr_zero(missing)].name});
        }
        return out;
    }

    template <class T, class Policy, class Input> auto record(Input input) -> result<T> {
        using indices = std::make_index_sequence<std::tuple_size_v<member_tuple<T>>>;
        if constexpr (flat<T>(indices{})) {
            return read_record<T, Policy>(input, indices{});
        } else {
            T out{};
            std::bitset<128> seen;
            auto status = input.object([&](std::string_view name, Input child) -> result<void> {
                std::size_t index = 0;
                bool found = false;
                result<void> assigned;
                fields<Policy>(out, [&](std::string_view key, auto& field) {
                    if (!found && name == key) {
                        found = true;
                        if (!seen[index]) {
                            assigned = decode_into<Policy>(field, child);
                            seen.set(index);
                        }
                    }
                    ++index;
                });
                if (Policy::strict && !found)
                    return json::unexpected("unexpected JSON field " + std::string{name});
                return assigned;
            });
            if (!status)
                return std::unexpected(std::move(status.error()));
            if constexpr (!Policy::defaults) {
                std::size_t index = 0;
                std::optional<std::string_view> missing;
                fields<Policy>(out, [&](std::string_view name, auto& field) {
                    if constexpr (required<bare<decltype(field)>>())
                        if (!seen[index] && !missing)
                            missing = name;
                    ++index;
                });
                if (missing)
                    return json::unexpected("missing JSON field " + std::string{*missing});
            }
            return out;
        }
    }

    template <auto Value> consteval auto enum_name() -> std::string_view {
        constexpr std::string_view signature = __PRETTY_FUNCTION__;
        constexpr auto begin = signature.find("Value = ") + 8;
        constexpr auto value = signature.substr(begin, signature.rfind(']') - begin);
        if constexpr (value.contains('('))
            return {};
        else
            return value.substr(value.rfind("::") + 2);
    }
    template <class E, std::size_t... I> consteval auto enum_names(std::index_sequence<I...>) {
        return std::array{enum_name<static_cast<E>(I)>()...};
    }
    template <class E> inline constexpr auto names = enum_names<E>(std::make_index_sequence<32>{});

    template <class E> auto enum_text(E item) -> std::string_view {
        const auto index = static_cast<std::size_t>(item);
        if (index >= names<E>.size() || names<E>[index].empty())
            throw std::invalid_argument("unrecognized wire enum");
        return names<E>[index];
    }

    template <class T, class Policy, std::size_t I = 0, class Input> auto union_value(Input input) -> result<T> {
        if constexpr (I == std::variant_size_v<T>) {
            return json::unexpected("JSON value does not match any alternative");
        } else {
            if (auto decoded = decode<std::variant_alternative_t<I, T>, Policy>(input))
                return T{std::in_place_index<I>, std::move(*decoded)};
            return union_value<T, Policy, I + 1>(input);
        }
    }

    template <class Record, class Policy, class F, std::size_t... I>
    constexpr void describe_members(F&& visit, std::index_sequence<I...>) {
        ([&] {
            using field = member_type<Record, I>;
            if constexpr (flattened<field>) {
                using nested = typename field::underlying_type;
                describe_members<nested, Policy>(visit, std::make_index_sequence<std::tuple_size_v<member_tuple<nested>>>{});
            } else if constexpr (!skipped<field>) {
                static constexpr auto name = key<Record, I, field, Policy>();
                visit.template operator()<field>(name.view());
            }
        }(), ...);
    }

    template <class Choice, class Policy, auto Tag> consteval auto discriminator_names() -> std::span<const std::string_view> {
        std::span<const std::string_view> result;
        describe_members<Choice, Policy>([&]<class Field>(std::string_view key) {
            if constexpr (is_literal<Field>)
                if (key == Tag.view())
                    result = Field::names;
        }, std::make_index_sequence<std::tuple_size_v<member_tuple<Choice>>>{});
        return result;
    }

    template <class T, class Policy, std::size_t I = 0, class Input> auto tagged_value(std::string_view tag, Input input) -> result<T> {
        if constexpr (I == std::tuple_size_v<typename T::alternatives>) {
            return json::unexpected("unknown JSON discriminator " + std::string{tag});
        } else {
            using choice = std::tuple_element_t<I, typename T::alternatives>;
            constexpr auto names = discriminator_names<choice, Policy, T::discriminator>();
            if (std::ranges::find(names, tag) != names.end())
                return decode<choice, Policy>(input).transform([](choice&& decoded) { return T{std::move(decoded)}; });
            return tagged_value<T, Policy, I + 1>(tag, input);
        }
    }

    template <class T, class Policy, class Input> auto decode(Input input) -> result<T> {
        if constexpr (std::same_as<T, value>) {
            return input.dynamic();
        } else if constexpr (std::same_as<T, json::object> || std::same_as<T, json::array>) {
            auto data = input.dynamic();
            if (!data)
                return std::unexpected(std::move(data.error()));
            if (auto* decoded = std::get_if<T>(&data->data))
                return std::move(*decoded);
            return json::unexpected("unexpected JSON container");
        } else if constexpr (annotation<T>) {
            return decode<typename T::underlying_type, Policy>(input).transform([](auto&& decoded) { return T{std::move(decoded)}; });
        } else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view> || std::integral<T> || std::floating_point<T>) {
            return input.template scalar<T>();
        } else if constexpr (requires { typename T::wire_type; }) {
            return decode<typename T::wire_type, Policy>(input).transform([](auto&& decoded) { return T{std::move(decoded)}; });
        } else if constexpr (std::same_as<T, std::monostate> || std::same_as<T, std::nullptr_t>) {
            if (!input.null())
                return json::unexpected("expected JSON null");
            return {};
        } else if constexpr (optional<T>) {
            if (input.null())
                return T{};
            return decode<typename T::value_type, Policy>(input).transform([](auto&& decoded) { return T{std::move(decoded)}; });
        } else if constexpr (unique<T> || shared<T>) {
            if (input.null())
                return T{};
            using item = typename T::element_type;
            return decode<item, Policy>(input).transform([](item&& decoded) { return T{new item(std::move(decoded))}; });
        } else if constexpr (is_literal<T>) {
            const auto name = input.template scalar<std::string_view>();
            if (!name)
                return std::unexpected(name.error());
            if (const auto decoded = T::from_name(*name))
                return *decoded;
            return json::unexpected("unknown JSON literal " + std::string{*name});
        } else if constexpr (is_tagged<T>) {
            const auto tag = input.at(T::discriminator.view());
            if (!tag)
                return json::unexpected("missing JSON discriminator " + std::string{T::discriminator.view()});
            return tag->template scalar<std::string_view>().and_then([&](std::string_view name) { return tagged_value<T, Policy>(name, input); });
        } else if constexpr (variant<T>) {
            return union_value<T, Policy>(input);
        } else if constexpr (std::is_enum_v<T>) {
            const auto name = input.template scalar<std::string_view>();
            if (!name)
                return std::unexpected(name.error());
            const auto match = std::ranges::find(names<T>, *name);
            if (name->empty() || match == names<T>.end())
                return json::unexpected("unknown JSON enum " + std::string{*name});
            return static_cast<T>(match - names<T>.begin());
        } else if constexpr (mapping<T>) {
            T out;
            result<void> status;
            if constexpr (std::constructible_from<typename T::key_type, std::string_view>) {
                status = input.object([&](auto name, auto child) -> result<void> {
                    auto decoded = decode<typename T::mapped_type, Policy>(child);
                    if (!decoded)
                        return std::unexpected(std::move(decoded.error()));
                    out.emplace(typename T::key_type{name}, std::move(*decoded));
                    return {};
                });
            } else {
                status = input.array([&](auto, auto visit) {
                    return visit([&](auto child) -> result<void> {
                        auto pair = decode<std::pair<typename T::key_type, typename T::mapped_type>, Policy>(child);
                        if (!pair)
                            return std::unexpected(std::move(pair.error()));
                        out.emplace(std::move(pair->first), std::move(pair->second));
                        return {};
                    });
                });
            }
            if (!status)
                return std::unexpected(std::move(status.error()));
            return out;
        } else if constexpr (tuple<T>) {
            T out{};
            auto status = input.array([&](std::size_t size, auto visit) -> result<void> {
                if (size != std::tuple_size_v<T>)
                    return json::unexpected("unexpected JSON tuple length");
                std::size_t index = 0;
                return visit([&](auto child) {
                    std::size_t position = 0;
                    result<void> assigned;
                    std::apply([&](auto&... item) {
                        ((index == position++ ? static_cast<void>(assigned = decode_into<Policy>(item, child)) : void()), ...);
                    }, out);
                    ++index;
                    return assigned;
                });
            });
            if (!status)
                return std::unexpected(std::move(status.error()));
            return out;
        } else if constexpr (sequence<T>) {
            T out;
            auto status = input.array([&](std::size_t size, auto visit) {
                if constexpr (requires { out.reserve(size); })
                    out.reserve(size);
                return visit([&](auto child) -> result<void> {
                    auto item = decode<typename T::value_type, Policy>(child);
                    if (!item)
                        return std::unexpected(std::move(item.error()));
                    if constexpr (requires { out.push_back(std::move(*item)); })
                        out.push_back(std::move(*item));
                    else
                        out.insert(std::move(*item));
                    return {};
                });
            });
            if (!status)
                return std::unexpected(std::move(status.error()));
            return out;
        } else {
            return record<T, Policy>(input);
        }
    }

    struct value_writer {
        template <class T> auto scalar(const T& item) -> value { return value{item}; }
        template <class F> auto object(F&& members) -> value {
            json::object out;
            members([&](std::string_view name, auto&& write) { out.insert(std::string{name}, write()); });
            return out;
        }
        template <class F> auto array(F&& members) -> value {
            json::array out;
            members([&](auto&& write) { out.push_back(write()); });
            return out;
        }
    };

    class json_writer {
        struct document_deleter {
            void operator()(yyjson_mut_doc* doc) const { yyjson_mut_doc_free(doc); }
        };
        struct buffer_deleter {
            void operator()(char* text) const { std::free(text); }
        };
        std::unique_ptr<yyjson_mut_doc, document_deleter> doc_{yyjson_mut_doc_new(nullptr)};

        static auto checked(yyjson_mut_val* item) -> yyjson_mut_val* {
            if (!item)
                throw std::bad_alloc{};
            return item;
        }

    public:
        json_writer() {
            if (!doc_)
                throw std::bad_alloc{};
        }

        auto scalar(std::nullptr_t) -> yyjson_mut_val* { return checked(yyjson_mut_null(doc_.get())); }
        auto scalar(std::string_view text) -> yyjson_mut_val* {
            return checked(yyjson_mut_strncpy(doc_.get(), text.empty() ? "" : text.data(), text.size()));
        }
        template <class T> auto scalar(const T& item) -> yyjson_mut_val* {
            if constexpr (std::convertible_to<const T&, std::string_view>) {
                return scalar(std::string_view{item});
            } else if constexpr (std::floating_point<T>) {
                const auto number = static_cast<double>(item);
                if (!std::isfinite(number))
                    throw std::runtime_error("invalid JSON number");
                return checked(yyjson_mut_real(doc_.get(), number));
            } else if constexpr (std::same_as<T, bool>) {
                return checked(yyjson_mut_bool(doc_.get(), item));
            } else if constexpr (std::unsigned_integral<T>) {
                return checked(yyjson_mut_uint(doc_.get(), item));
            } else {
                return checked(yyjson_mut_sint(doc_.get(), item));
            }
        }
        template <class F> auto object(F&& members) -> yyjson_mut_val* {
            auto* out = checked(yyjson_mut_obj(doc_.get()));
            members([&](std::string_view name, auto&& write) {
                auto* key = scalar(name);
                if (!yyjson_mut_obj_add(out, key, write()))
                    throw std::runtime_error("could not add JSON object member");
            });
            return out;
        }
        template <class F> auto array(F&& members) -> yyjson_mut_val* {
            auto* out = checked(yyjson_mut_arr(doc_.get()));
            members([&](auto&& write) {
                if (!yyjson_mut_arr_append(out, write()))
                    throw std::runtime_error("could not add JSON array element");
            });
            return out;
        }
        auto finish(yyjson_mut_val* root) -> std::string {
            std::size_t size = 0;
            yyjson_write_err error{};
            const std::unique_ptr<char, buffer_deleter> text{yyjson_mut_val_write_opts(root, 0, nullptr, &size, &error)};
            if (!text)
                throw std::runtime_error(std::string{"could not write JSON: "} + error.msg);
            return {text.get(), size};
        }
    };

    template <class Policy, class Sink, class T> auto encode(Sink& sink, const T& item) -> decltype(sink.scalar(nullptr)) {
        if constexpr (std::same_as<T, value>) {
            return std::visit([&](const auto& child) { return encode<Policy>(sink, child); }, item.data);
        } else if constexpr (annotation<T>) {
            return encode<Policy>(sink, item.get());
        } else if constexpr (std::same_as<T, std::monostate> || std::same_as<T, std::nullptr_t>) {
            return sink.scalar(nullptr);
        } else if constexpr (std::convertible_to<const T&, std::string_view> || std::integral<T> || std::floating_point<T>) {
            return sink.scalar(item);
        } else if constexpr (std::is_enum_v<T>) {
            return sink.scalar(enum_text(item));
        } else if constexpr (is_literal<T>) {
            return sink.scalar(item.view());
        } else if constexpr (nullable<T>) {
            if (item)
                return encode<Policy>(sink, *item);
            return sink.scalar(nullptr);
        } else if constexpr (is_tagged<T>) {
            return item.visit([&](const auto& child) { return encode<Policy>(sink, child); });
        } else if constexpr (variant<T>) {
            return std::visit([&](const auto& child) { return encode<Policy>(sink, child); }, item);
        } else if constexpr (std::same_as<T, json::object> || (mapping<T> && requires { requires std::convertible_to<typename T::key_type, std::string_view>; })) {
            return sink.object([&](auto append) {
                for (const auto& [name, child] : item)
                    append(name, [&] { return encode<Policy>(sink, child); });
            });
        } else if constexpr (tuple<T>) {
            return sink.array([&](auto append) {
                std::apply([&](const auto&... child) { (append([&] { return encode<Policy>(sink, child); }), ...); }, item);
            });
        } else if constexpr (sequence<T> || mapping<T>) {
            return sink.array([&](auto append) {
                for (const auto& child : item)
                    append([&] { return encode<Policy>(sink, child); });
            });
        } else {
            return sink.object([&](auto append) {
                fields<Policy>(item, [&](const auto& name, const auto& field) {
                    if (present(field))
                        append(name, [&] { return encode<Policy>(sink, field); });
                });
            });
        }
    }

}

export namespace json {
    template <class E> requires std::is_enum_v<E>
    auto enum_spelling(E item) -> std::string_view {
        return detail::enum_text(item);
    }

    template <class T, class... Options> auto read(std::string_view text) -> result<T> {
        yyjson_read_err error{};
        auto doc = detail::parse(text, &error);
        if (!doc)
            return json::unexpected(std::string{"could not parse JSON: "} + error.msg);
        // Borrowed strings from typed reads live until the next successful read on this thread.
        auto* root = yyjson_doc_get_root(doc.get());
        detail::last_read = std::move(doc);
        return detail::decode<T, detail::policy<Options...>>(detail::dom_input{root});
    }

    template <class T, class... Options> auto from_value(const value& input) -> result<T> {
        return detail::decode<T, detail::policy<Options...>>(detail::value_input{input});
    }

    template <class... Options, class T> auto write(const T& item) -> std::string {
        detail::json_writer output;
        return output.finish(detail::encode<detail::policy<Options...>>(output, item));
    }

    template <class... Options, class T> auto to_value(const T& item) -> value {
        detail::value_writer output;
        return detail::encode<detail::policy<Options...>>(output, item);
    }
}

namespace json::detail {
    template <class T> consteval auto type_name() -> std::string_view {
        constexpr std::string_view signature = __PRETTY_FUNCTION__;
        const auto start = signature.find("T = ") + 4;
        return signature.substr(start, signature.rfind(']') - start);
    }

    struct definitions {
        json::object entries;
        std::vector<std::string_view> visiting;
    };

    inline auto typed(std::string_view type) -> json::object { return {{"type", type}}; }
    template <class T, class Policy> auto describe(definitions& out, bool root = false) -> json::object;

    template <class T, class Policy> auto describe_object(definitions& out) -> json::object {
        json::object properties;
        json::array mandatory;
        describe_members<T, Policy>([&]<class Field>(std::string_view name) {
            properties.insert(std::string{name}, describe<Field, Policy>(out));
            if constexpr (!Policy::defaults && required<Field>())
                mandatory.emplace_back(name);
        }, std::make_index_sequence<std::tuple_size_v<member_tuple<T>>>{});
        return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(mandatory)}, {"additionalProperties", !Policy::strict}};
    }

    template <class T, class Policy, std::size_t... I> auto describe_tuple(definitions& out, std::index_sequence<I...>) -> json::array {
        return {describe<std::tuple_element_t<I, T>, Policy>(out)...};
    }
    template <class T, class Policy, std::size_t... I> auto describe_variant(definitions& out, std::index_sequence<I...>) -> json::array {
        return {describe<std::variant_alternative_t<I, T>, Policy>(out)...};
    }

    template <class T, class Policy> auto describe(definitions& out, bool root) -> json::object {
        if constexpr (annotation<T>) {
            auto content = describe<typename T::underlying_type, Policy>(out, root);
            if constexpr (requires { T::description; })
                content["description"] = T::description.view();
            return content;
        } else if constexpr (std::same_as<T, value>) {
            return {};
        } else if constexpr (std::same_as<T, bool>) {
            return typed("boolean");
        } else if constexpr (std::same_as<T, std::monostate> || std::same_as<T, std::nullptr_t>) {
            return typed("null");
        } else if constexpr (std::is_enum_v<T>) {
            json::array choices;
            for (const auto name : names<T>)
                if (!name.empty())
                    choices.emplace_back(name);
            return {{"type", "string"}, {"enum", std::move(choices)}};
        } else if constexpr (std::integral<T>) {
            return typed("integer");
        } else if constexpr (std::floating_point<T>) {
            return typed("number");
        } else if constexpr (std::convertible_to<const T&, std::string_view>) {
            return typed("string");
        } else if constexpr (is_literal<T>) {
            json::array choices;
            for (const auto name : T::names)
                choices.emplace_back(name);
            return {{"type", "string"}, {"enum", std::move(choices)}};
        } else if constexpr (nullable<T>) {
            using type = bare<decltype(*std::declval<const T&>())>;
            return {{"anyOf", json::array{describe<type, Policy>(out), typed("null")}}};
        } else if constexpr (is_tagged<T>) {
            using choices = typename T::alternatives;
            return {{"anyOf", describe_tuple<choices, Policy>(out, std::make_index_sequence<std::tuple_size_v<choices>>{})}};
        } else if constexpr (variant<T>) {
            return {{"anyOf", describe_variant<T, Policy>(out, std::make_index_sequence<std::variant_size_v<T>>{})}};
        } else if constexpr (std::same_as<T, json::object>) {
            return typed("object");
        } else if constexpr (mapping<T>) {
            if constexpr (std::constructible_from<typename T::key_type, std::string_view>)
                return {{"type", "object"}, {"additionalProperties", describe<typename T::mapped_type, Policy>(out)}};
            else
                return {{"type", "array"}, {"items", describe<std::pair<typename T::key_type, typename T::mapped_type>, Policy>(out)}};
        } else if constexpr (tuple<T>) {
            return {{"type", "array"}, {"prefixItems", describe_tuple<T, Policy>(out, std::make_index_sequence<std::tuple_size_v<T>>{})},
                    {"minItems", std::tuple_size_v<T>}, {"maxItems", std::tuple_size_v<T>}};
        } else if constexpr (sequence<T>) {
            return {{"type", "array"}, {"items", describe<typename T::value_type, Policy>(out)}};
        } else {
            constexpr auto name = type_name<T>();
            if (root)
                return describe_object<T, Policy>(out);
            if (out.entries.find(name) == out.entries.end() && std::ranges::find(out.visiting, name) == out.visiting.end()) {
                out.visiting.push_back(name);
                auto entry = describe_object<T, Policy>(out);
                out.visiting.pop_back();
                out.entries.insert(std::string{name}, std::move(entry));
            }
            return {{"$ref", "#/$defs/" + std::string{name}}};
        }
    }
}

export namespace json {
    template <class T, class... Options> auto schema() -> value {
        detail::definitions defs;
        json::object document{{"$schema", "https://json-schema.org/draft/2020-12/schema"}};
        auto root = detail::describe<T, detail::policy<Options...>>(defs, true);
        for (auto& [name, entry] : root)
            document.insert(std::move(name), std::move(entry));
        document.insert("$defs", std::move(defs.entries));
        return document;
    }
}

export namespace json {
    // Views and their strings borrow from the owning document.
    class view {
    public:
        view() = default;
        auto member(std::string_view name) const -> view;
        auto is_object() const -> bool;
        auto is_null() const -> bool;
        auto boolean() const -> std::optional<bool>;
        auto integer() const -> std::optional<std::int64_t>;
        auto string() const -> std::optional<std::string_view>;

    private:
        friend class document;
        explicit view(yyjson_val* raw) : raw_(raw) {}
        yyjson_val* raw_ = nullptr;
    };

    class document {
    public:
        explicit document(std::string_view text);
        document(const document&) = delete;
        auto root() const -> view;

    private:
        detail::document_ptr doc_;
    };

    namespace detail {
        template <class T> struct reader;
        template <> struct reader<view> {
            static auto read(view input) -> std::optional<view> { return input; }
        };
        template <> struct reader<bool> {
            static auto read(view input) -> std::optional<bool> { return input.boolean(); }
        };
        template <> struct reader<std::int64_t> {
            static auto read(view input) -> std::optional<std::int64_t> { return input.integer(); }
        };
        template <> struct reader<std::string_view> {
            static auto read(view input) -> std::optional<std::string_view> { return input.string(); }
        };
        template <class... T> struct reader<std::variant<T...>> {
            static auto read(view input) -> std::optional<std::variant<T...>> {
                std::optional<std::variant<T...>> out;
                auto visit = [&]<class U> {
                    if (auto item = reader<U>::read(input)) {
                        out.emplace(std::in_place_type<U>, *item);
                        return true;
                    }
                    return false;
                };
                (void)(visit.template operator()<T>() || ...);
                return out;
            }
        };

        template <class Field> auto read_field(view object, Field& field) -> bool {
            const auto input = object.member(Field::name.view());
            using type = typename Field::underlying_type;
            if constexpr (optional<type>) {
                if (input.is_null())
                    return true;
                const auto item = reader<typename type::value_type>::read(input);
                if (!item)
                    return false;
                field.value = *item;
                return true;
            } else {
                if (input.is_null())
                    return false;
                const auto item = reader<type>::read(input);
                if (!item)
                    return false;
                field.value = *item;
                return true;
            }
        }

    }

    template <class T> auto read(view object) -> std::optional<T> {
        if (!object.is_object())
            return {};
        T out{};
        if (!std::apply([&](auto&... field) { return (detail::read_field(object, field) && ...); }, meta::members(out)))
            return {};
        return out;
    }

}
namespace json {
    document::document(std::string_view text) : doc_(detail::parse(text)) {}
    auto document::root() const -> view { return view{yyjson_doc_get_root(doc_.get())}; }

    auto view::member(std::string_view name) const -> view {
        return view{yyjson_obj_getn(raw_, name.data(), name.size())};
    }
    auto view::is_object() const -> bool { return yyjson_is_obj(raw_); }
    auto view::is_null() const -> bool { return !raw_ || yyjson_is_null(raw_); }
    auto view::boolean() const -> std::optional<bool> {
        if (yyjson_is_bool(raw_))
            return yyjson_get_bool(raw_);
        return {};
    }
    auto view::integer() const -> std::optional<std::int64_t> { return detail::signed_integer(raw_); }
    auto view::string() const -> std::optional<std::string_view> {
        if (yyjson_is_str(raw_))
            return detail::string_of(raw_);
        return {};
    }
}
