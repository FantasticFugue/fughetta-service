module protocol;
import std;
import json;
import meta;

namespace jobs {
    namespace {
        template <class T> auto schema_code() -> std::uint32_t {
            auto schema = json::schema<T>();
            auto& root = std::get<json::object>(schema.get());
            root.erase(root.find("$schema"));
            auto& definitions = std::get<json::object>(root.find("$defs")->second.get());
            std::map<std::string, std::string, std::less<>> references;
            // Number definitions in traversal order so C++ type spellings stay
            // out of the wire fingerprint.
            for (std::size_t i = 0; i < definitions.size(); ++i) {
                auto id = std::to_string(i);
                references.emplace("#/$defs/" + definitions[i].first, "#/$defs/" + id);
                definitions[i].first = std::move(id);
            }
            auto rewrite = [&](this auto&& self, json::value& value) -> void {
                if (auto* object = std::get_if<json::object>(&value.get())) {
                    const auto description = object->find("description");
                    if (description != object->end() && std::holds_alternative<std::string>(description->second.get()))
                        object->erase(description);
                    for (auto& [key, field] : *object) {
                        if (auto* ref = std::get_if<std::string>(&field.get()); key == "$ref" && ref)
                            *ref = references.at(*ref);
                        else
                            self(field);
                    }
                } else if (auto* array = std::get_if<json::array>(&value.get())) {
                    for (auto& item : *array)
                        self(item);
                }
            };
            rewrite(schema);
            return meta::fnv1a(json::write(schema));
        }

        template <class... S, class... R> auto contract_version(meta::list<S...>, meta::list<R...>) -> std::uint32_t {
            std::uint32_t hash = 2166136261u;
            auto add = [&](std::uint32_t code) { hash = (hash ^ code) * 16777619u; };
            for (auto code : detail::codes(meta::list<S...>{}, meta::list<R...>{}))
                add(code);
            (add(schema_code<typename S::message>()), ...);
            ((add(schema_code<typename R::request>()), add(schema_code<typename R::reply>())), ...);
            add(schema_code<failure>());
            return hash;
        }
    }

    const std::uint32_t protocol_version = contract_version(signals{}, requests{});
}

namespace transport {
    auto decode_hello(std::string_view text) -> jobs::hello {
        auto hello = json::read<jobs::hello>(text);
        if (!hello) {
            // The receiver can refuse a peer using the protocol envelope alone.
            struct envelope {
                std::string auth;
                std::uint32_t protocol;
            };
            if (auto header = json::read<envelope>(text); header && header->protocol != jobs::protocol_version)
                return {.auth = std::move(header->auth), .protocol = header->protocol};
        }
        return std::move(hello).value();
    }
}
