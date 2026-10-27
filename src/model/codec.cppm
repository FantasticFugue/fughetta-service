export module codec;
import std;
import content;

export namespace nodes {
    // The node engine's result and error codes, as one place to spell them. A `node_reply::code` or an
    // exception's `code` holds one of these; callers compare against them without repeating a literal.
    namespace code {
        inline constexpr std::string_view ok = "ok";
        inline constexpr std::string_view invalid_record = "invalid_record";
        inline constexpr std::string_view storage_failure = "storage_failure";
        inline constexpr std::string_view resource_limit = "resource_limit";
        inline constexpr std::string_view missing_objects = "missing_objects";
        inline constexpr std::string_view source_conflict = "source_conflict";
        inline constexpr std::string_view permission_denied = "permission_denied";
        inline constexpr std::string_view outcome_unknown = "outcome_unknown";
        inline constexpr std::string_view policy_denied = "policy_denied";
        inline constexpr std::string_view identity_conflict = "identity_conflict";
    } // namespace code

    struct error : std::runtime_error {
        std::string code;
        std::vector<std::string> missing;
        std::vector<std::string> conflicts;
        error(std::string_view c, std::string message) : std::runtime_error(std::move(message)), code(c) {}
    };

    [[noreturn]] inline void fail(std::string_view code, std::string reason) { throw error{code, std::move(reason)}; }
    inline void checked_path(std::string_view path) {
        try {
            content::checked_path(path);
        } catch (const std::invalid_argument& e) {
            fail(nodes::code::invalid_record, e.what());
        }
    }
    inline void checked_name(std::string_view name) {
        try {
            content::checked_name(name);
        } catch (const std::invalid_argument& e) {
            fail(nodes::code::invalid_record, e.what());
        }
    }
    auto digest_of(std::string_view bytes) -> std::string;

    struct bytes {
        std::string data;
        auto operator==(const bytes&) const -> bool = default;
    };
    struct value {
        using array = std::vector<value>;
        struct map : std::vector<std::pair<std::string, value>> {
            using base = std::vector<std::pair<std::string, value>>;
            map() = default;
            map(std::initializer_list<base::value_type> fields);
            auto lower(std::string_view key) const -> std::size_t;
            auto find(std::string_view key) -> base::iterator;
            auto find(std::string_view key) const -> base::const_iterator;
            auto operator[](std::string_view key) -> value&;
            auto emplace(std::string key, value item) -> std::pair<base::iterator, bool>;
            auto operator==(const map& other) const -> bool;
        };
        std::variant<std::monostate, bool, std::uint64_t, bytes, std::string, array, map> data;
        value() = default;
        value(bool v) : data(v) {}
        value(std::uint64_t v) : data(v) {}
        value(bytes v) : data(std::move(v)) {}
        value(std::string v) : data(std::move(v)) {}
        value(const char* v) : data(std::string(v)) {}
        value(array v) : data(std::move(v)) {}
        value(map v) : data(std::move(v)) {}
        value(const value& other) {
            switch (other.data.index()) {
            case 0:
                break;
            case 1:
                data.emplace<bool>(other.boolean());
                break;
            case 2:
                data.emplace<std::uint64_t>(other.number());
                break;
            case 3:
                data.emplace<bytes>(bytes{other.raw()});
                break;
            case 4:
                data.emplace<std::string>(other.text());
                break;
            case 5:
                data.emplace<array>(other.list());
                break;
            case 6:
                data.emplace<map>(other.object());
                break;
            default:
                fail(nodes::code::invalid_record, "invalid in-memory value");
            }
        }
        value(value&&) = default;
        auto operator=(value&&) -> value& = default;
        auto operator=(const value& other) -> value& {
            if (this != &other) {
                value copy{other};
                data.swap(copy.data);
            }
            return *this;
        }
        auto operator==(const value&) const -> bool = default;
        auto object() const -> const map& {
            if (auto p = std::get_if<map>(&data))
                return *p;
            fail(nodes::code::invalid_record, "expected map");
        }
        auto list() const -> const array& {
            if (auto p = std::get_if<array>(&data))
                return *p;
            fail(nodes::code::invalid_record, "expected array");
        }
        auto text() const -> const std::string& {
            if (auto p = std::get_if<std::string>(&data))
                return *p;
            fail(nodes::code::invalid_record, "expected string");
        }
        auto raw() const -> const std::string& {
            if (auto p = std::get_if<bytes>(&data))
                return p->data;
            fail(nodes::code::invalid_record, "expected bytes");
        }
        auto number() const -> std::uint64_t {
            if (auto p = std::get_if<std::uint64_t>(&data))
                return *p;
            fail(nodes::code::invalid_record, "expected unsigned integer");
        }
        auto boolean() const -> bool {
            if (auto p = std::get_if<bool>(&data))
                return *p;
            fail(nodes::code::invalid_record, "expected boolean");
        }
        auto at(std::string_view key) const -> const value& {
            auto& m = object();
            auto index = m.lower(key);
            auto* fields = m.data();
            if (index == m.size() || std::string_view{fields[index].first} != key)
                fail(nodes::code::invalid_record, "missing field: " + std::string(key));
            return fields[index].second;
        }
    };
    inline value::map::map(std::initializer_list<base::value_type> fields) : base(fields) {
        bool ordered = true;
        for (std::size_t i = 1; i < size(); ++i) {
            auto compared = data()[i - 1].first.compare(data()[i].first);
            if (!compared)
                fail(nodes::code::invalid_record, "duplicate map key");
            if (compared > 0) {
                ordered = false;
                break;
            }
        }
        if (!ordered) {
            std::ranges::sort(*this, {}, &base::value_type::first);
            for (std::size_t i = 1; i < size(); ++i)
                if (data()[i - 1].first == data()[i].first)
                    fail(nodes::code::invalid_record, "duplicate map key");
        }
    }
    inline auto value::map::lower(std::string_view key) const -> std::size_t {
        std::size_t first = 0, last = size();
        auto* fields = data();
        while (first < last) {
            auto middle = first + (last - first) / 2;
            if (std::string_view{fields[middle].first} < key)
                first = middle + 1;
            else
                last = middle;
        }
        return first;
    }
    inline auto value::map::find(std::string_view key) -> base::iterator {
        auto index = lower(key);
        return index < size() && std::string_view{data()[index].first} == key ? begin() + index : end();
    }
    inline auto value::map::find(std::string_view key) const -> base::const_iterator {
        auto index = lower(key);
        return index < size() && std::string_view{data()[index].first} == key ? begin() + index : end();
    }
    inline auto value::map::operator[](std::string_view key) -> value& {
        auto index = lower(key);
        auto it = index < size() ? begin() + index : end();
        if (it == end() || it->first != key)
            it = insert(it, {std::string(key), value{}});
        return it->second;
    }
    inline auto value::map::emplace(std::string key, value item) -> std::pair<base::iterator, bool> {
        auto index = lower(key);
        auto it = index < size() ? begin() + index : end();
        if (it != end() && it->first == key)
            return {it, false};
        return {insert(it, {std::move(key), std::move(item)}), true};
    }
    inline auto value::map::operator==(const map& other) const -> bool { return static_cast<const base&>(*this) == static_cast<const base&>(other); }

    inline auto strings(const std::vector<std::string>& input) -> value {
        value::array out;
        out.reserve(input.size());
        for (auto& s : input)
            out.emplace_back(s);
        return out;
    }
    inline auto string_list(const value& input) -> std::vector<std::string> {
        std::vector<std::string> out;
        out.reserve(input.list().size());
        for (auto& v : input.list())
            out.push_back(v.text());
        return out;
    }
    inline auto canonical_less(std::string_view a, std::string_view b) -> bool {
        auto left = static_cast<std::uint64_t>(a.size()), right = static_cast<std::uint64_t>(b.size());
        for (unsigned i = 0; i < 8; ++i) {
            auto x = (left >> (i * 8)) & 255, y = (right >> (i * 8)) & 255;
            if (x != y)
                return x < y;
        }
        return a < b;
    }
    inline auto canonical_set(std::vector<std::string> values) -> std::vector<std::string> {
        std::ranges::sort(values, canonical_less);
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }
    inline void fields(const value& v, std::initializer_list<std::string_view> names) {
        auto& map = v.object();
        if (map.size() != names.size())
            fail(nodes::code::invalid_record, "incorrect record fields");
        auto* actual = map.data();
        std::size_t index = 0;
        for (auto name : names)
            if (std::string_view{actual[index++].first} != name)
                fail(nodes::code::invalid_record, "incorrect record fields");
    }
    inline auto scalars(std::string_view text) -> std::u32string {
        std::u32string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size();) {
            auto b = static_cast<unsigned char>(text[i++]);
            char32_t cp = b;
            int tail = 0;
            char32_t minimum = 0;
            if (b >= 0xc2 && b <= 0xdf) {
                cp = b & 31;
                tail = 1;
                minimum = 0x80;
            } else if (b >= 0xe0 && b <= 0xef) {
                cp = b & 15;
                tail = 2;
                minimum = 0x800;
            } else if (b >= 0xf0 && b <= 0xf4) {
                cp = b & 7;
                tail = 3;
                minimum = 0x10000;
            } else if (b >= 0x80)
                fail(nodes::code::invalid_record, "invalid UTF-8 lead byte");
            for (int j = 0; j < tail; ++j) {
                if (i == text.size())
                    fail(nodes::code::invalid_record, "truncated UTF-8");
                auto next = static_cast<unsigned char>(text[i++]);
                if ((next & 0xc0) != 0x80)
                    fail(nodes::code::invalid_record, "invalid UTF-8 continuation");
                cp = (cp << 6) | (next & 63);
            }
            if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                fail(nodes::code::invalid_record, "invalid Unicode scalar");
            out.push_back(cp);
        }
        return out;
    }
    inline void append_scalar(std::string& out, char32_t c) {
        if (c < 0x80)
            out += static_cast<char>(c);
        else if (c < 0x800) {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xe0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        } else {
            out += static_cast<char>(0xf0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 63));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        }
    }

    namespace codec {
        inline constexpr std::size_t record_limit = 128 * 1024 * 1024;
        inline constexpr std::size_t collection_limit = 1'000'000;
        inline void integer(std::string& out, std::uint64_t n) {
            for (int i = 0; i < 8; ++i) {
                out += static_cast<char>(n & 255);
                n >>= 8;
            }
        }
        inline void text(std::string& out, std::string_view data) {
            if (data.size() > record_limit)
                fail(nodes::code::resource_limit, "record byte limit");
            for (unsigned char c : data)
                if (c >= 128) {
                    (void)scalars(data);
                    break;
                }
            out += '\5';
            integer(out, data.size());
            out.append(data);
        }
        inline void encode_value(std::string& out, const value& v, unsigned depth) {
            if (depth > 64)
                fail(nodes::code::resource_limit, "record nesting limit");
            if (std::holds_alternative<std::monostate>(v.data)) {
                out += '\0';
                return;
            }
            if (auto p = std::get_if<bool>(&v.data)) {
                out += static_cast<char>(*p ? 2 : 1);
                return;
            }
            if (auto p = std::get_if<std::uint64_t>(&v.data)) {
                out += '\3';
                integer(out, *p);
                return;
            }
            if (auto p = std::get_if<bytes>(&v.data)) {
                if (p->data.size() > record_limit)
                    fail(nodes::code::resource_limit, "record byte limit");
                out += '\4';
                integer(out, p->data.size());
                out += p->data;
                return;
            }
            if (auto p = std::get_if<std::string>(&v.data)) {
                text(out, *p);
                return;
            }
            if (auto p = std::get_if<value::array>(&v.data)) {
                if (p->size() > collection_limit)
                    fail(nodes::code::resource_limit, "record collection limit");
                out += '\6';
                integer(out, p->size());
                for (auto& x : *p)
                    encode_value(out, x, depth + 1);
                return;
            }
            auto& m = v.object();
            out += '\7';
            integer(out, m.size());
            if (m.size() > collection_limit)
                fail(nodes::code::resource_limit, "record collection limit");
            std::string_view prior;
            for (auto& [k, x] : m) {
                if (k.empty() || std::ranges::any_of(k, [](unsigned char c) { return c < 0x21 || c > 0x7e; }))
                    fail(nodes::code::invalid_record, "invalid map key");
                if (!prior.empty() && k <= prior)
                    fail(nodes::code::invalid_record, "noncanonical map ordering");
                prior = k;
                text(out, k);
                encode_value(out, x, depth + 1);
            }
        }
        inline auto encode(const value& v) -> std::string {
            (void)v.object();
            std::string out{"DCN1"};
            encode_value(out, v, 0);
            if (out.size() > record_limit)
                fail(nodes::code::resource_limit, "record byte limit");
            return out;
        }
        struct decoder {
            std::string_view input;
            std::size_t offset = 4;
            auto take(std::size_t n) -> std::string_view {
                if (n > input.size() - offset)
                    fail(nodes::code::invalid_record, "truncated record");
                auto out = input.substr(offset, n);
                offset += n;
                return out;
            }
            auto integer() -> std::uint64_t {
                auto data = take(8);
                std::uint64_t out = 0;
                for (int i = 7; i >= 0; --i)
                    out = (out << 8) | static_cast<unsigned char>(data[i]);
                return out;
            }
            auto read(unsigned depth = 0) -> value {
                if (depth > 64)
                    fail(nodes::code::resource_limit, "record nesting limit");
                auto tag = static_cast<unsigned char>(take(1)[0]);
                if (tag == 0)
                    return {};
                if (tag == 1 || tag == 2)
                    return tag == 2;
                if (tag == 3)
                    return integer();
                if (tag == 4 || tag == 5) {
                    auto n = integer();
                    auto data = std::string(take(n));
                    if (tag == 4)
                        return bytes{std::move(data)};
                    for (unsigned char c : data)
                        if (c >= 128) {
                            (void)scalars(data);
                            break;
                        }
                    return data;
                }
                if (tag != 6 && tag != 7)
                    fail(nodes::code::invalid_record, "unsupported value tag");
                auto n = integer();
                if (n > input.size() - offset)
                    fail(nodes::code::invalid_record, "impossible collection length");
                if (n > collection_limit)
                    fail(nodes::code::resource_limit, "record collection limit");
                if (tag == 6) {
                    value::array out;
                    out.reserve(n);
                    for (std::uint64_t i = 0; i < n; ++i)
                        out.push_back(read(depth + 1));
                    return out;
                }
                value::map out;
                out.reserve(n);
                std::string_view prior;
                for (std::uint64_t i = 0; i < n; ++i) {
                    auto key = read(depth + 1).text();
                    if (key.empty() || (i && key <= prior) || std::ranges::any_of(key, [](unsigned char c) { return c < 0x21 || c > 0x7e; }))
                        fail(nodes::code::invalid_record, "noncanonical map key");
                    out.emplace_back(std::move(key), read(depth + 1));
                    prior = out.back().first;
                }
                return out;
            }
        };
        inline auto decode(std::string_view input) -> value {
            if (input.size() > record_limit)
                fail(nodes::code::resource_limit, "record byte limit");
            if (!input.starts_with("DCN1"))
                fail(nodes::code::invalid_record, "invalid record framing");
            decoder d{input};
            auto out = d.read();
            (void)out.object();
            if (d.offset != input.size())
                fail(nodes::code::invalid_record, "trailing record bytes");
            return out;
        }
    } // namespace codec

    struct key {
        std::string secret;
        std::string public_key;
        static auto generate() -> key;
        static auto from_secret(std::string bytes) -> key;
        auto id() const -> std::string { return digest_of(public_key); }
        auto sign(std::string_view bytes) const -> std::string;
    };
    auto verify_signature(std::string_view public_key, std::string_view bytes, std::string_view signature) -> bool;
    auto random_bytes(std::size_t n) -> std::string;
    inline auto signed_message(const value& body) -> std::string {
        return "fughetta/node/1/" + body.at("kind").text() + std::string(1, '\0') + codec::encode(body);
    }
    inline auto seal(value body, const key& signer) -> value {
        auto signature = signer.sign(signed_message(body));
        value::map out;
        out.reserve(3);
        out.emplace("body", std::move(body));
        out.emplace("public_key", bytes{signer.public_key});
        out.emplace("signature", bytes{std::move(signature)});
        return out;
    }
    inline auto open(const value& object) -> const value& {
        fields(object, {"body", "public_key", "signature"});
        auto& body = object.at("body");
        if (body.at("schema").number() != 1)
            fail(nodes::code::invalid_record, "unsupported record schema");
        if (!verify_signature(object.at("public_key").raw(), signed_message(body), object.at("signature").raw()))
            fail("invalid_signature", "origin signature failed");
        return body;
    }
    inline auto object_id(const value& object) -> std::string { return digest_of(codec::encode(object)); }
    inline auto hex(std::string_view bytes) -> std::string {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (unsigned char b : bytes) {
            out += digits[b >> 4];
            out += digits[b & 15];
        }
        return out;
    }
    inline auto unhex(std::string_view text) -> std::string {
        if (text.size() % 2)
            fail(nodes::code::invalid_record, "invalid hex length");
        std::string out;
        auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            fail(nodes::code::invalid_record, "invalid hex digit");
        };
        for (std::size_t i = 0; i < text.size(); i += 2)
            out += static_cast<char>((digit(text[i]) << 4) | digit(text[i + 1]));
        return out;
    }
} // namespace nodes
