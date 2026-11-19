module;
#include <pugixml.hpp>

export module xml;
import std;
import meta;
import fields;

export namespace xml {
    template <class T, meta::fixed_string Name> struct attribute {
        static constexpr auto name = Name;
        T value{};
    };

    template <class T, meta::fixed_string Name> struct element {
        static constexpr auto name = Name;
        T value{};
    };

    template <class T, meta::fixed_string Name> struct elements {
        static constexpr auto name = Name;
        std::vector<T> value;
    };
}

namespace xml::detail {
    template <class T> inline constexpr bool is_optional = false;
    template <class T> inline constexpr bool is_optional<std::optional<T>> = true;

    template <class T> concept scalar = std::same_as<T, std::string> || std::integral<T> || std::same_as<T, float> || std::same_as<T, double>;

    template <scalar T> auto convert(std::string_view text) -> T {
        if constexpr (std::same_as<T, std::string>) {
            return std::string{text};
        } else if constexpr (std::same_as<T, bool>) {
            if (text == "true" || text == "1")
                return true;
            if (text == "false" || text == "0")
                return false;
            throw std::runtime_error("invalid XML boolean");
        } else {
            T value{};
            const auto end = text.data() + text.size();
            const auto [last, error] = std::from_chars(text.data(), end, value);
            if (error != std::errc{} || last != end)
                throw std::runtime_error("invalid XML number");
            return value;
        }
    }

    template <class T> auto read_value(pugi::xml_node node) -> T;

    template <class T, meta::fixed_string Name> void read_field(pugi::xml_node node, attribute<T, Name>& field) {
        const auto input = node.attribute(Name.chars);
        if constexpr (is_optional<T>) {
            if (input)
                field.value = convert<typename T::value_type>(input.value());
        } else {
            if (!input)
                throw std::runtime_error("missing XML attribute " + std::string{Name.view()});
            field.value = convert<T>(input.value());
        }
    }

    template <class T, meta::fixed_string Name> void read_field(pugi::xml_node node, element<T, Name>& field) {
        const auto input = node.child(Name.chars);
        if constexpr (is_optional<T>) {
            if (input)
                field.value = read_value<typename T::value_type>(input);
        } else {
            if (!input)
                throw std::runtime_error("missing XML element " + std::string{Name.view()});
            field.value = read_value<T>(input);
        }
    }

    template <class T, meta::fixed_string Name> void read_field(pugi::xml_node node, elements<T, Name>& field) {
        for (auto child : node.children(Name.chars))
            field.value.push_back(read_value<T>(child));
    }

    template <class T> auto read_value(pugi::xml_node node) -> T {
        if constexpr (scalar<T>) {
            return convert<T>(node.child_value());
        } else {
            T out{};
            meta::for_each_member(out, [&]<std::size_t>(auto& member) { read_field(node, member); });
            return out;
        }
    }

    template <class Target, scalar T> void put(Target target, const T& value) {
        if constexpr (std::same_as<T, std::string>) {
            if (value.contains('\0'))
                throw std::runtime_error("NUL in XML value");
            if (target.set(value.data(), value.size()))
                return;
        } else if (target.set(value)) {
            return;
        }
        throw std::runtime_error("could not write XML value");
    }

    // XML attributes and element text use distinct pugixml handles.
    template <scalar T> void put(pugi::xml_attribute target, const T& value) {
        if constexpr (std::same_as<T, std::string>) {
            if (value.contains('\0'))
                throw std::runtime_error("NUL in XML value");
            if (target.set_value(value.data(), value.size()))
                return;
        } else if (target.set_value(value)) {
            return;
        }
        throw std::runtime_error("could not write XML attribute");
    }

    template <class T> void write_value(pugi::xml_node node, const T& value);

    template <class T, meta::fixed_string Name> void write_field(pugi::xml_node node, const attribute<T, Name>& field) {
        if constexpr (is_optional<T>) {
            if (field.value)
                put(node.append_attribute(Name.chars), *field.value);
        } else {
            put(node.append_attribute(Name.chars), field.value);
        }
    }

    template <class T, meta::fixed_string Name> void write_field(pugi::xml_node node, const element<T, Name>& field) {
        if constexpr (is_optional<T>) {
            if (field.value)
                write_value(node.append_child(Name.chars), *field.value);
        } else {
            write_value(node.append_child(Name.chars), field.value);
        }
    }

    template <class T, meta::fixed_string Name> void write_field(pugi::xml_node node, const elements<T, Name>& field) {
        for (const auto& value : field.value)
            write_value(node.append_child(Name.chars), value);
    }

    template <class T> void write_value(pugi::xml_node node, const T& value) {
        if (!node)
            throw std::runtime_error("could not create XML element");
        if constexpr (scalar<T>)
            put(node.text(), value);
        else
            meta::for_each_member(value, [&]<std::size_t>(const auto& member) { write_field(node, member); });
    }

    struct string_writer : pugi::xml_writer {
        std::string output;
        void write(const void* data, std::size_t size) override { output.append(static_cast<const char*>(data), size); }
    };
}

export namespace xml {
    template <class T, meta::fixed_string... Roots> auto read(std::string_view text) -> T {
        pugi::xml_document document;
        const auto parsed = document.load_buffer(text.data(), text.size());
        if (!parsed)
            throw std::runtime_error(std::format("invalid XML at byte {}: {}", parsed.offset, parsed.description()));

        const auto root = document.document_element();
        if (root.next_sibling())
            throw std::runtime_error("XML must have one root element");
        if constexpr (sizeof...(Roots) != 0)
            if (!((std::string_view{root.name()} == Roots.view()) || ...))
                throw std::runtime_error("unexpected XML root element " + std::string{root.name()});
        return detail::read_value<T>(root);
    }

    template <meta::fixed_string Root, class T> auto write(const T& value) -> std::string {
        pugi::xml_document document;
        detail::write_value(document.append_child(Root.chars), value);
        detail::string_writer output;
        document.save(output, "", pugi::format_raw, pugi::encoding_utf8);
        return std::move(output.output);
    }
}
