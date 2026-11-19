module;
#include <simdjson.h>
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
        std::array<char, N + 3> json{};
        std::size_t json_length{};
        constexpr auto view() const -> std::string_view { return {bytes.data(), length}; }
        constexpr operator std::string_view() const { return view(); }
        constexpr auto encoded() const -> std::string_view { return {json.data(), json_length}; }

        consteval void encode() {
            for (unsigned char ch : view())
                if (ch < 0x20 || ch == '"' || ch == '\\')
                    return;
            json[0] = '"';
            std::ranges::copy(view(), json.data() + 1);
            json[length + 1] = '"';
            json[length + 2] = ':';
            json_length = length + 3;
        }
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
        out.encode();
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

    class parse_error : public std::runtime_error { using std::runtime_error::runtime_error; };
    [[noreturn]] inline void invalid(std::string_view message) { throw parse_error{std::string{message}}; }

    struct dom_input {
        simdjson::dom::element item;
        auto null() const -> bool { return item.is_null(); }
        template <class T> auto scalar() const -> T {
            if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view>) {
                std::string_view text;
                if (!item.get_string().get(text))
                    return T{text};
            } else if constexpr (std::same_as<T, bool>) {
                bool out;
                if (!item.get_bool().get(out))
                    return out;
            } else if constexpr (std::floating_point<T>) {
                double out;
                if (!item.get_double().get(out))
                    return static_cast<T>(out);
            } else if constexpr (std::unsigned_integral<T>) {
                std::uint64_t out;
                if (!item.get_uint64().get(out) && std::in_range<T>(out))
                    return static_cast<T>(out);
            } else {
                std::int64_t out;
                if (!item.get_int64().get(out) && std::in_range<T>(out))
                    return static_cast<T>(out);
            }
            invalid("unexpected JSON scalar");
        }
        auto at(std::string_view key) const -> std::optional<dom_input> {
            simdjson::dom::element out;
            if (!item.at_key(key).get(out))
                return dom_input{out};
            return {};
        }
        template <class F> void object(F&& f) const {
            simdjson::dom::object object;
            if (item.get_object().get(object))
                invalid("expected JSON object");
            for (auto [name, child] : object)
                f(name, dom_input{child});
        }
        template <class F> void array(F&& f) const {
            simdjson::dom::array array;
            if (item.get_array().get(array))
                invalid("expected JSON array");
            f(array.size(), [&](auto&& visit) {
                for (auto child : array)
                    visit(dom_input{child});
            });
        }
        auto dynamic() const -> value {
            using kind = simdjson::dom::element_type;
            switch (item.type()) {
            case kind::NULL_VALUE: return {};
            case kind::BOOL: return scalar<bool>();
            case kind::INT64: return scalar<std::int64_t>();
            case kind::UINT64: return scalar<std::uint64_t>();
            case kind::DOUBLE: return scalar<double>();
            case kind::STRING: return scalar<std::string>();
            case kind::BIGINT: invalid("JSON integer is outside the 64-bit range");
            case kind::ARRAY: {
                json::array out;
                array([&](auto size, auto visit) {
                    out.reserve(size);
                    visit([&](auto child) { out.push_back(child.dynamic()); });
                });
                return out;
            }
            case kind::OBJECT: {
                json::object out;
                object([&](auto name, auto child) { out.insert(std::string{name}, child.dynamic()); });
                return out;
            }
            }
            std::unreachable();
        }
    };

    struct value_input {
        const value& item;
        auto null() const -> bool { return std::holds_alternative<std::monostate>(item.data); }
        template <class T> auto scalar() const -> T {
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
            invalid("unexpected JSON scalar");
        }
        auto at(std::string_view key) const -> std::optional<value_input> {
            if (const auto* object = std::get_if<json::object>(&item.data))
                if (auto field = object->find(key); field != object->end())
                    return value_input{field->second};
            return {};
        }
        template <class F> void object(F&& f) const {
            const auto* object = std::get_if<json::object>(&item.data);
            if (!object)
                invalid("expected JSON object");
            for (const auto& [name, child] : *object)
                f(name, value_input{child});
        }
        template <class F> void array(F&& f) const {
            const auto* array = std::get_if<json::array>(&item.data);
            if (!array)
                invalid("expected JSON array");
            f(array->size(), [&](auto&& visit) {
                for (const auto& child : *array)
                    visit(value_input{child});
            });
        }
        auto dynamic() const -> value { return item; }
    };

    template <class T, class Policy, class Input> auto decode(Input input) -> T;

    template <class T> using member_tuple = decltype(meta::members(std::declval<T&>()));
    template <class T, std::size_t I> using member_type = bare<std::tuple_element_t<I, member_tuple<T>>>;
    template <class T, std::size_t... I> consteval bool flat(std::index_sequence<I...>) { return (!flattened<member_type<T, I>> && ...); }

    template <class T, class Policy, class Input> struct member_setter {
        std::string_view name;
        std::uint32_t hash;
        void (*assign)(T&, Input);
        bool mandatory;

        template <std::size_t I> static consteval auto make() -> member_setter {
            using field = member_type<T, I>;
            if constexpr (skipped<field>) {
                return {};
            } else {
                const std::string_view name = spelling<I>.view();
                return {name, meta::fnv1a(name), [](T& record, Input child) {
                    std::get<I>(meta::members(record)) = decode<field, Policy>(child);
                }, required<field>()};
            }
        }

        template <std::size_t I> static constexpr auto spelling = key<T, I, member_type<T, I>, Policy>();
    };

    template <class T, class Policy, class Input, std::size_t... I> auto read_record(Input input, std::index_sequence<I...>) -> T {
        using setter = member_setter<T, Policy, Input>;
        static constexpr std::array<setter, sizeof...(I)> setters{setter::template make<I>()...};
        static constexpr auto mandatory = ((setter::template make<I>().mandatory ? (std::uint64_t{1} << I) : 0) | ... | 0);
        T out{};
        std::uint64_t seen = 0;
        std::size_t next = 0;
        input.object([&](std::string_view name, Input child) {
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
                    match->assign(out, child);
                    seen |= bit;
                    next = index + 1;
                }
            } else if constexpr (Policy::strict) {
                invalid("unexpected JSON field " + std::string{name});
            }
        });
        if constexpr (!Policy::defaults && mandatory != 0) {
            if (const auto missing = mandatory & ~seen)
                invalid("missing JSON field " + std::string{setters[std::countr_zero(missing)].name});
        }
        return out;
    }

    template <class T, class Policy, class Input> auto record(Input input) -> T {
        using indices = std::make_index_sequence<std::tuple_size_v<member_tuple<T>>>;
        if constexpr (flat<T>(indices{})) {
            return read_record<T, Policy>(input, indices{});
        } else {
            T out{};
            std::bitset<128> seen;
            input.object([&](std::string_view name, Input child) {
                std::size_t index = 0;
                bool found = false;
                fields<Policy>(out, [&](std::string_view key, auto& field) {
                    if (!found && name == key) {
                        found = true;
                        if (!seen[index]) {
                            field = decode<bare<decltype(field)>, Policy>(child);
                            seen.set(index);
                        }
                    }
                    ++index;
                });
                if (Policy::strict && !found)
                    invalid("unexpected JSON field " + std::string{name});
            });
            if constexpr (!Policy::defaults) {
                std::size_t index = 0;
                fields<Policy>(out, [&](std::string_view name, auto& field) {
                    if constexpr (required<bare<decltype(field)>>())
                        if (!seen[index])
                            invalid("missing JSON field " + std::string{name});
                    ++index;
                });
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

    template <class T, class Policy, std::size_t I = 0, class Input> auto union_value(Input input) -> T {
        if constexpr (I == std::variant_size_v<T>) {
            invalid("JSON value does not match any alternative");
        } else {
            try {
                return T{std::in_place_index<I>, decode<std::variant_alternative_t<I, T>, Policy>(input)};
            } catch (const parse_error&) {
                return union_value<T, Policy, I + 1>(input);
            }
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

    template <class T, class Policy, std::size_t I = 0, class Input> auto tagged_value(std::string_view tag, Input input) -> T {
        if constexpr (I == std::tuple_size_v<typename T::alternatives>) {
            invalid("unknown JSON discriminator " + std::string{tag});
        } else {
            using choice = std::tuple_element_t<I, typename T::alternatives>;
            constexpr auto names = discriminator_names<choice, Policy, T::discriminator>();
            if (std::ranges::find(names, tag) != names.end())
                return T{decode<choice, Policy>(input)};
            return tagged_value<T, Policy, I + 1>(tag, input);
        }
    }

    template <class T, class Policy, class Input> auto decode(Input input) -> T {
        if constexpr (std::same_as<T, value>) {
            return input.dynamic();
        } else if constexpr (std::same_as<T, json::object> || std::same_as<T, json::array>) {
            auto data = input.dynamic();
            if (auto* result = std::get_if<T>(&data.data))
                return std::move(*result);
            invalid("unexpected JSON container");
        } else if constexpr (annotation<T>) {
            return T{decode<typename T::underlying_type, Policy>(input)};
        } else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view> || std::integral<T> || std::floating_point<T>) {
            return input.template scalar<T>();
        } else if constexpr (requires { typename T::wire_type; }) {
            return T{decode<typename T::wire_type, Policy>(input)};
        } else if constexpr (std::same_as<T, std::monostate> || std::same_as<T, std::nullptr_t>) {
            if (!input.null())
                invalid("expected JSON null");
            return {};
        } else if constexpr (optional<T>) {
            if (input.null())
                return {};
            return T{decode<typename T::value_type, Policy>(input)};
        } else if constexpr (unique<T> || shared<T>) {
            if (input.null())
                return {};
            using item = typename T::element_type;
            return T{new item(decode<item, Policy>(input))};
        } else if constexpr (is_literal<T>) {
            const auto name = input.template scalar<std::string_view>();
            if (std::ranges::find(T::names, name) == T::names.end())
                invalid("unknown JSON literal " + std::string{name});
            return T{name};
        } else if constexpr (is_tagged<T>) {
            const auto tag = input.at(T::discriminator.view());
            if (!tag)
                invalid("missing JSON discriminator " + std::string{T::discriminator.view()});
            return tagged_value<T, Policy>(tag->template scalar<std::string_view>(), input);
        } else if constexpr (variant<T>) {
            return union_value<T, Policy>(input);
        } else if constexpr (std::is_enum_v<T>) {
            const auto name = input.template scalar<std::string_view>();
            const auto match = std::ranges::find(names<T>, name);
            if (name.empty() || match == names<T>.end())
                invalid("unknown JSON enum " + std::string{name});
            return static_cast<T>(match - names<T>.begin());
        } else if constexpr (mapping<T>) {
            T out;
            if constexpr (std::constructible_from<typename T::key_type, std::string_view>) {
                input.object([&](auto name, auto child) {
                    out.emplace(typename T::key_type{name}, decode<typename T::mapped_type, Policy>(child));
                });
            } else {
                input.array([&](auto, auto visit) {
                    visit([&](auto child) {
                        auto pair = decode<std::pair<typename T::key_type, typename T::mapped_type>, Policy>(child);
                        out.emplace(std::move(pair.first), std::move(pair.second));
                    });
                });
            }
            return out;
        } else if constexpr (tuple<T>) {
            T out{};
            input.array([&](std::size_t size, auto visit) {
                if (size != std::tuple_size_v<T>)
                    invalid("unexpected JSON tuple length");
                std::size_t index = 0;
                visit([&](auto child) {
                    std::size_t position = 0;
                    std::apply([&](auto&... item) {
                        ((index == position++ ? static_cast<void>(item = decode<bare<decltype(item)>, Policy>(child)) : void()), ...);
                    }, out);
                    ++index;
                });
            });
            return out;
        } else if constexpr (sequence<T>) {
            T out;
            input.array([&](std::size_t size, auto visit) {
                if constexpr (requires { out.reserve(size); })
                    out.reserve(size);
                visit([&](auto child) {
                    auto item = decode<typename T::value_type, Policy>(child);
                    if constexpr (requires { out.push_back(std::move(item)); })
                        out.push_back(std::move(item));
                    else
                        out.insert(std::move(item));
                });
            });
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
        simdjson::builder::string_builder buffer_;
        void comma(bool& written) {
            if (std::exchange(written, true))
                buffer_.append_comma();
        }

    public:
        void scalar(std::nullptr_t) { buffer_.append_null(); }
        void scalar(std::string_view text) { buffer_.escape_and_append_with_quotes(text); }
        template <class T> void scalar(const T& item) {
            if constexpr (std::convertible_to<const T&, std::string_view>) {
                scalar(std::string_view{item});
            } else if constexpr (std::floating_point<T>) {
                yyjson_val number{};
                yyjson_set_real(&number, static_cast<double>(item));
                char text[40];
                const auto* end = std::isfinite(item) ? yyjson_write_number(&number, text) : nullptr;
                if (!end)
                    throw std::runtime_error("invalid JSON number");
                buffer_.append_raw(text, static_cast<std::size_t>(end - text));
            } else {
                buffer_.append(item);
            }
        }
        template <class F> void object(F&& members) {
            buffer_.start_object();
            bool written = false;
            members([&](const auto& name, auto&& write) {
                comma(written);
                if constexpr (requires { name.encoded(); }) {
                    if (const auto token = name.encoded(); !token.empty()) {
                        buffer_.append_raw(token.data(), token.size());
                        write();
                        return;
                    }
                }
                scalar(std::string_view{name});
                buffer_.append_colon();
                write();
            });
            buffer_.end_object();
        }
        template <class F> void array(F&& members) {
            buffer_.start_array();
            bool written = false;
            members([&](auto&& write) {
                comma(written);
                write();
            });
            buffer_.end_array();
        }
        auto finish() -> std::string {
            std::string_view text;
            if (auto error = buffer_.view().get(text))
                throw std::runtime_error(simdjson::error_message(error));
            if (!simdjson::validate_utf8(text))
                throw std::runtime_error("invalid UTF-8 in JSON string");
            std::string out{text};
            // Stored identities include the spelling of control escapes.
            for (auto slash = out.find('\\'); slash != std::string::npos; slash = out.find('\\', slash + 2)) {
                if (out[slash + 1] != 'u')
                    continue;
                for (auto digit = slash + 2; digit < slash + 6; ++digit)
                    if (out[digit] >= 'a' && out[digit] <= 'f')
                        out[digit] -= 'a' - 'A';
                slash += 4;
            }
            return out;
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

    inline auto parser() -> simdjson::dom::parser& {
        static thread_local simdjson::dom::parser instance;
        return instance;
    }

    template <class T, class Policy, class Input> auto try_decode(Input input) -> result<T> {
        try {
            return decode<T, Policy>(input);
        } catch (const parse_error& error) {
            return json::unexpected(error.what());
        }
    }
}

export namespace json {
    template <class T, class... Options> auto read(std::string_view text) -> result<T> {
        simdjson::dom::element root;
        if (const auto error = detail::parser().parse(text.data(), text.size()).get(root))
            return json::unexpected(std::string{"could not parse JSON: "} + simdjson::error_message(error));
        return detail::try_decode<T, detail::policy<Options...>>(detail::dom_input{root});
    }

    template <class T, class... Options> auto from_value(const value& input) -> result<T> {
        return detail::try_decode<T, detail::policy<Options...>>(detail::value_input{input});
    }

    template <class... Options, class T> auto write(const T& item) -> std::string {
        detail::json_writer output;
        detail::encode<detail::policy<Options...>>(output, item);
        return output.finish();
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
    inline constexpr auto padding = simdjson::SIMDJSON_PADDING;

    // A document view is valid until the next document on the same thread.
    class view {
    public:
        view() = default;
        explicit view(simdjson::dom::element raw) : raw_(raw), present_(true) {}
        auto member(std::string_view name) const -> view;
        auto is_object() const -> bool;
        auto is_null() const -> bool;
        auto boolean() const -> std::optional<bool>;
        auto integer() const -> std::optional<std::int64_t>;
        auto string() const -> std::optional<std::string_view>;

    private:
        simdjson::dom::element raw_;
        bool present_ = false;
    };

    class document {
    public:
        explicit document(std::string_view text, std::size_t readable = 0);
        document(const document&) = delete;
        auto root() const -> view;

    private:
        view root_;
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
    document::document(std::string_view text, std::size_t readable) {
        static thread_local simdjson::dom::parser parser;
        simdjson::dom::element root;
        const bool copy = readable < text.size() || readable - text.size() < simdjson::SIMDJSON_PADDING;
        if (!parser.parse(text.data(), text.size(), copy).get(root))
            root_ = view{root};
    }
    auto document::root() const -> view { return root_; }

    auto view::member(std::string_view name) const -> view {
        simdjson::dom::element child;
        if (present_ && !raw_[name].get(child))
            return view{child};
        return {};
    }
    auto view::is_object() const -> bool { return present_ && raw_.is_object(); }
    auto view::is_null() const -> bool { return !present_ || raw_.is_null(); }
    auto view::boolean() const -> std::optional<bool> {
        bool flag;
        if (present_ && !raw_.get_bool().get(flag))
            return flag;
        return {};
    }
    auto view::integer() const -> std::optional<std::int64_t> {
        std::int64_t number;
        if (present_ && !raw_.get_int64().get(number))
            return number;
        return {};
    }
    auto view::string() const -> std::optional<std::string_view> {
        std::string_view text;
        if (present_ && !raw_.get_string().get(text))
            return text;
        return {};
    }
}
