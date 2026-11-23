export module protobuf;
import std;
import fields;
import json;

export namespace protobuf {
    template <std::uint32_t Number, class T> struct field : json::annotated<T> {
        static_assert(Number > 0 && Number < (1u << 29));
        static constexpr auto number = Number;
        using json::annotated<T>::annotated;
        using json::annotated<T>::operator=;
    };
}

namespace protobuf::detail {
    inline void write_varint(std::string& out, std::uint64_t value) {
        while (value >= 128) {
            out.push_back(static_cast<char>((value & 127) | 128));
            value >>= 7;
        }
        out.push_back(static_cast<char>(value));
    }

    class reader {
    public:
        explicit reader(std::string_view input) : input_(input) {}
        bool empty() const { return input_.empty(); }

        auto bytes(std::uint64_t size) -> std::string_view {
            if (size > input_.size())
                throw std::runtime_error("truncated protobuf field");
            auto result = input_.substr(0, static_cast<std::size_t>(size));
            input_.remove_prefix(result.size());
            return result;
        }

        auto varint() -> std::uint64_t {
            std::uint64_t value = 0;
            for (unsigned shift = 0; shift < 64; shift += 7) {
                const auto byte = static_cast<unsigned char>(bytes(1).front());
                if (shift == 63 && byte > 1)
                    break;
                value |= std::uint64_t{byte & 127u} << shift;
                if (byte < 128)
                    return value;
            }
            throw std::runtime_error("invalid protobuf varint");
        }

        void skip(unsigned wire) {
            switch (wire) {
            case 0: varint(); return;
            case 1: bytes(8); return;
            case 2: bytes(varint()); return;
            case 5: bytes(4); return;
            default: throw std::runtime_error("unsupported protobuf wire type");
            }
        }

    private:
        std::string_view input_;
    };

    template <class T> inline constexpr bool optional = false;
    template <class T> inline constexpr bool optional<std::optional<T>> = true;
    template <class T> inline constexpr bool repeated = false;
    template <class T> inline constexpr bool repeated<std::vector<T>> = true;

    template <class T> void read_into(T& result, std::string_view data);

    template <class T> void read_length_delimited(T& value, std::string_view data) {
        if constexpr (std::same_as<T, std::string>)
            value = data;
        else
            read_into(value, data);
    }

    template <class T> void read_into(T& result, std::string_view data) {
        reader in{data};
        while (!in.empty()) {
            const auto tag = in.varint();
            const auto number = tag >> 3;
            const auto wire = static_cast<unsigned>(tag & 7);
            if (number == 0 || number >= (1u << 29))
                throw std::runtime_error("invalid protobuf field number");
            bool found = false;
            meta::for_each_member(result, [&]<std::size_t>(auto& member) {
                using field_type = std::remove_reference_t<decltype(member)>;
                using value_type = typename field_type::underlying_type;
                if (number != field_type::number)
                    return;
                found = true;
                constexpr auto expected = std::unsigned_integral<value_type> ? 0u : 2u;
                if (wire != expected)
                    throw std::runtime_error("unexpected protobuf field wire type");
                if constexpr (std::unsigned_integral<value_type>) {
                    const auto value = in.varint();
                    if (value > std::numeric_limits<value_type>::max())
                        throw std::runtime_error("protobuf integer out of range");
                    member = static_cast<value_type>(value);
                } else if constexpr (std::same_as<value_type, std::string>) {
                    member = in.bytes(in.varint());
                } else if constexpr (optional<value_type>) {
                    auto& value = member.get();
                    if (!value)
                        value.emplace();
                    read_length_delimited(*value, in.bytes(in.varint()));
                } else if constexpr (repeated<value_type>) {
                    read_length_delimited(member.get().emplace_back(), in.bytes(in.varint()));
                } else
                    static_assert(false, "unsupported protobuf field type");
            });
            if (!found)
                in.skip(wire);
        }
    }
}

export namespace protobuf {
    template <class T> auto read(std::string_view data) -> T {
        T result{};
        detail::read_into(result, data);
        return result;
    }

    template <class T> auto write(const T& message) -> std::string {
        std::string out;
        meta::for_each_member(message, [&]<std::size_t>(const auto& member) {
            using field_type = std::remove_cvref_t<decltype(member)>;
            using value_type = typename field_type::underlying_type;
            const auto& value = member.get();
            constexpr auto tag = std::uint64_t{field_type::number} << 3;
            if constexpr (std::unsigned_integral<value_type>) {
                if (value != 0) {
                    detail::write_varint(out, tag);
                    detail::write_varint(out, value);
                }
            } else {
                const auto emit = [&](std::string_view body) {
                    detail::write_varint(out, tag | 2);
                    detail::write_varint(out, body.size());
                    out.append(body);
                };
                const auto emit_item = [&](const auto& item) {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(item)>, std::string>)
                        emit(item);
                    else
                        emit(write(item));
                };
                if constexpr (detail::optional<value_type>) {
                    if (value)
                        emit_item(*value);
                } else if constexpr (detail::repeated<value_type>) {
                    for (const auto& item : value)
                        emit_item(item);
                } else if (!value.empty())
                    emit_item(value);
            }
        });
        return out;
    }
}
