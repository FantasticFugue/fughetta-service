module;
#include <zlib.h>
export module jvm;
import std;
import evaluation;
import coroutine;
import constants;
import content;
import durability;
import reflection;
import records;
import channels;
import enumeration;
import projectmodel;
import kmetadata;
export import provider;

export namespace jvm {
    class reader;
    struct bytes {
        std::string_view data;
        std::size_t position = 0;
        auto take(std::size_t count) -> std::string_view {
            if (position > data.size() || count > data.size() - position)
                throw std::invalid_argument("truncated JVM artifact");
            auto out = data.substr(position, count);
            position += count;
            return out;
        }
        auto u1() -> std::uint8_t { return static_cast<unsigned char>(take(1)[0]); }
        auto u2() -> std::uint16_t {
            auto a = u1();
            return (a << 8) | u1();
        }
        auto u4() -> std::uint32_t {
            auto a = u2();
            return (std::uint32_t{a} << 16) | u2();
        }
    };
    struct pool_entry {
        std::uint8_t tag = 0;
        std::string text;
        std::uint16_t first = 0, second = 0;
    };
    inline auto pool_text(std::string_view encoded) -> std::string {
        // CONSTANT_Utf8 stores modified UTF-8 (NUL and UTF-16 surrogates),
        // while the public records and their JSON transport use UTF-8.
        bytes input{encoded};
        std::string out;
        auto append = [&](std::uint32_t value) {
            if (value < 0x80)
                out += static_cast<char>(value);
            else if (value < 0x800) {
                out += static_cast<char>(0xc0 | (value >> 6));
                out += static_cast<char>(0x80 | (value & 63));
            } else if (value < 0x10000) {
                out += static_cast<char>(0xe0 | (value >> 12));
                out += static_cast<char>(0x80 | ((value >> 6) & 63));
                out += static_cast<char>(0x80 | (value & 63));
            } else {
                out += static_cast<char>(0xf0 | (value >> 18));
                out += static_cast<char>(0x80 | ((value >> 12) & 63));
                out += static_cast<char>(0x80 | ((value >> 6) & 63));
                out += static_cast<char>(0x80 | (value & 63));
            }
        };
        std::optional<std::uint32_t> high;
        while (input.position < input.data.size()) {
            std::uint32_t value = input.u1();
            unsigned remaining = 0;
            if ((value & 0xe0) == 0xc0) {
                remaining = 1;
                value &= 31;
            } else if ((value & 0xf0) == 0xe0) {
                remaining = 2;
                value &= 15;
            } else if (value >= 0x80)
                throw std::invalid_argument("invalid JVM modified UTF-8");
            while (remaining--) {
                const auto next = input.u1();
                if ((next & 0xc0) != 0x80)
                    throw std::invalid_argument("invalid JVM modified UTF-8 continuation");
                value = (value << 6) | (next & 63);
            }
            if (high) {
                if (value >= 0xdc00 && value <= 0xdfff) {
                    append(0x10000 + ((*high - 0xd800) << 10) + value - 0xdc00);
                    high.reset();
                    continue;
                }
                append(0xfffd);
                high.reset();
            }
            if (value >= 0xd800 && value <= 0xdbff)
                high = value;
            else
                append(value >= 0xdc00 && value <= 0xdfff ? 0xfffd : value);
        }
        if (high)
            append(0xfffd);
        return out;
    }
    struct annotation {
        std::string descriptor;
        bool visible = false;
        std::map<std::string, rfl::Generic> values;
    };
    struct call {
        std::uint32_t offset = 0;
        std::string owner, name, descriptor;
        bool dynamic = false;
        std::uint8_t opcode = 0;
    };
    struct member {
        std::string name, descriptor, signature;
        std::uint16_t flags = 0;
        bool method = false;
        std::optional<std::string> constant;
        std::vector<call> calls;
        std::map<std::uint32_t, std::uint32_t> lines;
        std::vector<annotation> annotations;
    };
    struct object {
        std::string name, parent, source, debug;
        std::uint16_t flags = 0;
        std::vector<std::string> interfaces;
        std::vector<member> members;
        std::string entry, value;
        std::string signature;
        std::vector<annotation> annotations;
    };
    inline auto read(std::string_view data) -> object {
        bytes in{data};
        if (in.u4() != 0xcafebabe)
            throw std::invalid_argument("artifact is not a JVM class");
        in.u2();
        in.u2();
        std::vector<pool_entry> pool(in.u2());
        for (std::size_t i = 1; i < pool.size(); ++i) {
            auto& p = pool[i];
            p.tag = in.u1();
            switch (p.tag) {
            case 1: {
                auto n = in.u2();
                p.text = pool_text(in.take(n));
                break;
            }
            case 3:
            case 4:
                p.text = std::to_string(in.u4());
                break;
            case 5:
            case 6: {
                const auto high = in.u4();
                p.text = std::to_string((std::uint64_t{high} << 32) | in.u4());
                ++i;
                break;
            }
            case 7:
            case 8:
            case 16:
            case 19:
            case 20:
                p.first = in.u2();
                break;
            case 9:
            case 10:
            case 11:
            case 12:
            case 17:
            case 18:
                p.first = in.u2();
                p.second = in.u2();
                break;
            case 15:
                p.first = in.u1();
                p.second = in.u2();
                break;
            default:
                throw std::invalid_argument("unknown JVM constant pool tag");
            }
        }
        auto utf = [&](std::uint16_t index) -> std::string {
            const auto& p = pool.at(index);
            if (p.tag != 1)
                throw std::invalid_argument("JVM name does not refer to UTF8");
            return p.text;
        };
        auto klass = [&](std::uint16_t index) -> std::string { return index ? utf(pool.at(index).first) : ""; };
        // JVMS 4.7.16.1: preserve the discriminant, including nested annotation
        // and array values, instead of flattening annotation text.
        auto annotation_value = [&](auto&& self, bytes& source, char tag) -> rfl::Generic {
            std::map<std::string, rfl::Generic> value{{"tag", std::string(1, tag)}};
            if (tag == '@') {
                value["descriptor"] = utf(source.u2());
                std::map<std::string, rfl::Generic> members;
                const auto count = source.u2();
                for (unsigned i = 0; i < count; ++i) {
                    const auto name = utf(source.u2());
                    members[name] = self(self, source, source.u1());
                }
                value["values"] = rfl::to_generic(members);
            } else if (tag == '[') {
                std::vector<rfl::Generic> elements;
                const auto count = source.u2();
                for (unsigned i = 0; i < count; ++i)
                    elements.push_back(self(self, source, source.u1()));
                value["elements"] = rfl::to_generic(elements);
            } else if (tag == 'e') {
                value["descriptor"] = utf(source.u2());
                value["name"] = utf(source.u2());
            } else if (tag == 'c' || tag == 's')
                value["value"] = utf(source.u2());
            else {
                const auto& constant = pool.at(source.u2());
                const auto bits = std::stoull(constant.text);
                auto floating = [](auto number) {
                    std::array<char, 64> buffer;
                    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
                    return std::string{buffer.data(), result.ptr};
                };
                switch (tag) {
                case 'B':
                case 'C':
                case 'S':
                case 'I':
                    value["value"] = std::int64_t{std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits))};
                    break;
                case 'J':
                    value["value"] = std::bit_cast<std::int64_t>(bits);
                    break;
                case 'Z':
                    value["value"] = bits != 0;
                    break;
                // Text retains NaN/infinity without producing invalid JSON numbers.
                case 'F':
                    value["value"] = floating(std::bit_cast<float>(static_cast<std::uint32_t>(bits)));
                    break;
                case 'D':
                    value["value"] = floating(std::bit_cast<double>(bits));
                    break;
                default:
                    throw std::invalid_argument("invalid JVM annotation value tag");
                }
            }
            return rfl::to_generic(value);
        };
        auto annotations = [&](bytes& source, bool visible, std::vector<annotation>& output) {
            const auto count = source.u2();
            for (unsigned i = 0; i < count; ++i) {
                annotation row{.descriptor = utf(source.u2()), .visible = visible};
                const auto members = source.u2();
                for (unsigned j = 0; j < members; ++j) {
                    const auto name = utf(source.u2());
                    row.values[name] = annotation_value(annotation_value, source, source.u1());
                }
                output.push_back(std::move(row));
            }
        };
        auto attributes = [&](bytes& source, auto&& consume) {
            auto count = source.u2();
            for (unsigned i = 0; i < count; ++i) {
                auto name = utf(source.u2());
                auto size = source.u4();
                bytes value{source.take(size)};
                consume(name, value);
            }
        };
        object out;
        out.flags = in.u2();
        out.name = klass(in.u2());
        out.parent = klass(in.u2());
        auto interfaces = in.u2();
        for (unsigned i = 0; i < interfaces; ++i)
            out.interfaces.push_back(klass(in.u2()));
        for (bool method : {false, true}) {
            auto count = in.u2();
            for (unsigned i = 0; i < count; ++i) {
                member m;
                m.method = method;
                m.flags = in.u2();
                m.name = utf(in.u2());
                m.descriptor = utf(in.u2());
                attributes(in, [&](const std::string& name, bytes& value) {
                    if (name == "Signature")
                        m.signature = utf(value.u2());
                    if (name == "RuntimeVisibleAnnotations" || name == "RuntimeInvisibleAnnotations")
                        annotations(value, name == "RuntimeVisibleAnnotations", m.annotations);
                    if (name == "ConstantValue") {
                        const auto& constant = pool.at(value.u2());
                        m.constant = std::to_string(constant.tag) + ":" + (constant.tag == 8 ? utf(constant.first) : constant.text);
                    }
                    if (name != "Code")
                        return;
                    value.u2();
                    value.u2();
                    auto size = value.u4();
                    bytes code{value.take(size)};
                    while (code.position < code.data.size()) {
                        auto offset = code.position;
                        auto op = code.u1();
                        if (op >= 0xb6 && op <= 0xba) {
                            const auto& ref = pool.at(code.u2());
                            const auto& signature = pool.at(ref.second);
                            m.calls.push_back({static_cast<std::uint32_t>(offset), op == 0xba ? "" : klass(ref.first), utf(signature.first),
                                               utf(signature.second), op == 0xba, op});
                            if (op == 0xb9 || op == 0xba)
                                code.take(2);
                        } else if (op == 0xaa || op == 0xab) {
                            code.take((4 - code.position % 4) % 4);
                            code.u4();
                            if (op == 0xaa) {
                                auto low = static_cast<std::int32_t>(code.u4()), high = static_cast<std::int32_t>(code.u4());
                                if (high < low)
                                    throw std::invalid_argument("invalid JVM tableswitch");
                                code.take((static_cast<std::uint64_t>(std::int64_t{high} - low) + 1) * 4);
                            } else {
                                auto pairs = code.u4();
                                code.take(std::uint64_t{pairs} * 8);
                            }
                        } else if (op == 0xc4) {
                            auto widened = code.u1();
                            code.take(widened == 0x84 ? 4 : 2);
                        } else {
                            std::size_t count = 0;
                            if (op == 0x10 || op == 0x12 || (op >= 0x15 && op <= 0x19) || (op >= 0x36 && op <= 0x3a) || op == 0xa9 || op == 0xbc)
                                count = 1;
                            else if (op == 0x11 || op == 0x13 || op == 0x14 || op == 0x84 || (op >= 0x99 && op <= 0xa8) || (op >= 0xb2 && op <= 0xb5) ||
                                     op == 0xbb || op == 0xbd || op == 0xc0 || op == 0xc1 || op == 0xc6 || op == 0xc7)
                                count = 2;
                            else if (op == 0xc5)
                                count = 3;
                            else if (op == 0xc8 || op == 0xc9)
                                count = 4;
                            code.take(count);
                        }
                    }
                    auto exceptions = value.u2();
                    value.take(std::size_t{exceptions} * 8);
                    attributes(value, [&](const std::string& name, bytes& nested) {
                        if (name == "LineNumberTable") {
                            auto count = nested.u2();
                            for (unsigned i = 0; i < count; ++i) {
                                auto pc = nested.u2();
                                m.lines[pc] = nested.u2();
                            }
                        }
                    });
                });
                out.members.push_back(std::move(m));
            }
        }
        attributes(in, [&](const std::string& name, bytes& value) {
            if (name == "SourceFile")
                out.source = utf(value.u2());
            else if (name == "SourceDebugExtension")
                out.debug = value.data;
            else if (name == "Signature")
                out.signature = utf(value.u2());
            else if (name == "RuntimeVisibleAnnotations" || name == "RuntimeInvisibleAnnotations")
                annotations(value, name == "RuntimeVisibleAnnotations", out.annotations);
        });
        out.entry = out.name + ".class";
        out.value = content::hash_of(data).str();
        return out;
    }

    inline auto classes(std::string_view data, unsigned release = 0, const std::map<std::string, const object*>* previous = nullptr) -> std::vector<object> {
        auto parse = [&](std::string_view bytes) {
            const auto value = content::hash_of(bytes).str();
            if (previous)
                if (const auto found = previous->find(value); found != previous->end())
                    return *found->second;
            return read(bytes);
        };
        if (data.size() >= 4 && static_cast<unsigned char>(data[0]) == 0xca)
            return {parse(data)};
        auto le = [&](std::size_t at, unsigned count) -> std::uint64_t {
            if (at > data.size() || count > data.size() - at)
                throw std::invalid_argument("truncated JAR directory");
            std::uint64_t out = 0;
            for (unsigned i = 0; i < count; ++i)
                out |= std::uint64_t{static_cast<unsigned char>(data[at + i])} << (i * 8);
            return out;
        };
        if (data.size() < 22)
            throw std::invalid_argument("artifact is not a class or JAR");
        std::size_t end = data.size() - 22;
        while (le(end, 4) != 0x06054b50 || end + 22 + le(end + 20, 2) != data.size()) {
            if (!end || data.size() - end > 65557)
                throw std::invalid_argument("JAR directory is absent");
            --end;
        }
        auto count = le(end + 10, 2), directory = le(end + 16, 4);
        if (count == 0xffff || directory == 0xffffffff) {
            if (end < 20 || le(end - 20, 4) != 0x07064b50)
                throw std::invalid_argument("JAR ZIP64 locator is absent");
            auto zip64 = le(end - 12, 8);
            if (le(zip64, 4) != 0x06064b50)
                throw std::invalid_argument("JAR ZIP64 directory is absent");
            count = le(zip64 + 32, 8);
            directory = le(zip64 + 48, 8);
        }
        std::map<std::string, std::pair<unsigned, object>> selected;
        std::uint64_t unpacked = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            auto at = directory;
            if (le(at, 4) != 0x02014b50)
                throw std::invalid_argument("invalid JAR directory entry");
            auto flags = le(at + 8, 2), method = le(at + 10, 2), packed = le(at + 20, 4), plain = le(at + 24, 4);
            auto length = le(at + 28, 2), extra = le(at + 30, 2), comment = le(at + 32, 2), offset = le(at + 42, 4);
            bytes names{data};
            names.position = at + 46;
            auto name = std::string{names.take(length)};
            auto extras = names.take(extra);
            directory = at + 46 + length + extra + comment;
            if (!name.ends_with(".class"))
                continue;
            if (flags & 1)
                throw std::invalid_argument("encrypted JAR entries are not analysis inputs");
            for (std::size_t pos = 0; pos + 4 <= extras.size();) {
                auto tag = le(at + 46 + length + pos, 2), size = le(at + 48 + length + pos, 2);
                pos += 4;
                if (tag == 1) {
                    auto cursor = at + 46 + length + pos;
                    if (plain == 0xffffffff) {
                        plain = le(cursor, 8);
                        cursor += 8;
                    }
                    if (packed == 0xffffffff) {
                        packed = le(cursor, 8);
                        cursor += 8;
                    }
                    if (offset == 0xffffffff)
                        offset = le(cursor, 8);
                }
                pos += size;
            }
            const auto physical_entry = name;
            unsigned version = 0;
            constexpr std::string_view prefix = "META-INF/versions/";
            if (name.starts_with(prefix)) {
                auto slash = name.find('/', prefix.size());
                if (slash == std::string::npos)
                    continue;
                if (!release)
                    throw std::invalid_argument("multi-release JAR requires an explicit java_release");
                auto number = std::string_view{name}.substr(prefix.size(), slash - prefix.size());
                auto parsed = std::from_chars(number.data(), number.data() + number.size(), version);
                if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
                    throw std::invalid_argument("invalid JAR release directory");
                if (version > release)
                    continue;
                name.erase(0, slash + 1);
            }
            if (auto found = selected.find(name); found != selected.end() && found->second.first > version)
                continue;
            if (plain > constants::semantic_object_bytes - unpacked)
                throw std::length_error("JAR class data exceeds analysis budget");
            unpacked += plain;
            if (le(offset, 4) != 0x04034b50)
                throw std::invalid_argument("invalid JAR local entry");
            auto begin = offset + 30 + le(offset + 26, 2) + le(offset + 28, 2);
            bytes stored{data};
            stored.position = begin;
            auto input = stored.take(packed);
            std::string output;
            if (method == 0)
                output = input;
            else if (method == 8) {
                output.resize(plain);
                z_stream stream{};
                stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
                stream.avail_in = input.size();
                stream.next_out = reinterpret_cast<Bytef*>(output.data());
                stream.avail_out = output.size();
                if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                    throw std::runtime_error("cannot initialize JAR decompressor");
                auto result = inflate(&stream, Z_FINISH);
                inflateEnd(&stream);
                if (result != Z_STREAM_END || stream.total_out != plain)
                    throw std::invalid_argument("invalid compressed JAR class");
            } else
                throw std::invalid_argument("unknown JAR compression method");
            auto parsed = parse(output);
            parsed.entry = physical_entry;
            selected.insert_or_assign(name, std::pair{version, std::move(parsed)});
        }
        std::vector<object> out;
        for (auto& [_, entry] : selected)
            out.push_back(std::move(entry.second));
        return out;
    }
} // namespace jvm

export namespace language {
    template <> struct server<"jvm-artifact"> {
        using adapter = jvm::reader;
        struct options {
            std::string module;
            unsigned java_release = 0;
            std::string metadata_helper, metadata_executable = "java", source_provider = "kotlin";
        };
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 0> documents{};
        static constexpr std::array<std::string_view, 3> inputs{"artifact", "source_facts", "project_model"};
        static constexpr std::array<std::string_view, 5> facts{"symbols", "types", "definitions", "references", "relations"};
        static constexpr std::array<std::string_view, 1> roles{"artifact-reader"}, operations{"evaluate"};
        static constexpr semantic::unit_kind unit_kind = semantic::unit_kind::artifact;
        static constexpr std::string_view update_granularity = "class";
        static constexpr bool negotiated = false, multiple_inputs = true;
        static auto multiple_input(std::string_view port) -> bool { return port == "source_facts"; }
        static auto required_inputs(const options&) -> std::vector<std::string> { return {"artifact"}; }
        static auto input_type(std::string_view port) -> std::string_view {
            return port == "artifact" ? "jvm_artifact" : port == "project_model" ? "project_model" : "semantic_shard";
        }
        static auto consumed_facts(std::string_view port) -> std::vector<semantic::fact_kind> {
            return port == "source_facts" ? std::vector<semantic::fact_kind>{std::string{"symbols"}, std::string{"definitions"}}
                                          : std::vector<semantic::fact_kind>{};
        }
        static auto channels() -> std::vector<semantic::channel_definition> {
            return {{.name = "declarations", .projections = {std::string{"symbols"}, std::string{"types"}}},
                    {.name = "relations", .projections = {std::string{"references"}, std::string{"relations"}}},
                    linkage_channel()};
        }
        static auto input_validators() -> std::vector<input_validator> {
            return {{"jvm/class",
                     [](const semantic::input_object& required, std::span<const semantic::input_object> installed) {
                         return !required.object.empty() && std::ranges::contains(installed, required);
                     }},
                    {"jvm/inventory",
                     [](const semantic::input_object& required, std::span<const semantic::input_object> installed) {
                         return !required.object.empty() && std::ranges::contains(installed, required);
                     }},
                    {"jvm/lookup",
                     [](const semantic::input_object& required, std::span<const semantic::input_object> installed) {
                         return !required.object.empty() && std::ranges::contains(installed, required);
                     }},
                    {"jvm/dispatch", [](const semantic::input_object& required, std::span<const semantic::input_object> installed) {
                         return !required.object.empty() && std::ranges::contains(installed, required);
                     }}};
        }
        static auto requirements(const options& o) -> builds::requirements {
            return o.metadata_helper.empty() ? builds::requirements{} : builds::tools({o.metadata_executable, o.metadata_helper});
        }
        static void validate(const options& o) {
            if (o.module.empty())
                throw std::invalid_argument("JVM artifact module is required");
            if (!o.metadata_helper.empty()) {
                if (o.metadata_executable.empty() || o.source_provider.empty())
                    throw std::invalid_argument("Kotlin metadata requires its executable and source provider");
                (void)requirements(o);
            }
        }
        static auto launch(const options& o, const std::vector<builds::context>& inputs) -> program {
            const builds::context* artifact = nullptr;
            std::optional<std::string> model;
            for (const auto& input : inputs) {
                if (input.type == "jvm_artifact") {
                    if (artifact)
                        throw std::invalid_argument("JVM reader requires one jvm_artifact input");
                    artifact = &input;
                } else if (input.type == "project_model") {
                    if (model)
                        throw std::invalid_argument("JVM reader requires at most one project_model input");
                    model = input.path;
                } else
                    throw std::invalid_argument("JVM reader received an unsupported build input");
            }
            if (!artifact)
                throw std::invalid_argument("JVM reader requires one jvm_artifact input");
            program out{.provider = "jvm-artifact", .initialization = rfl::to_generic(std::tuple{artifact->path, model, o})};
            if (!o.metadata_helper.empty()) {
                out.commands["metadata"] = {o.metadata_executable, "-jar", builds::tool(o.metadata_helper), "--jvm-metadata"};
                out.executables = {o.metadata_helper};
            }
            return out;
        }
    };
} // namespace language

export namespace jvm {
    // Class signatures use scoped type variables, parameterized owners and
    // wildcard bounds. IDs include the declaring class so T in unrelated
    // classes never aliases the same record.
    struct class_signature {
        std::string_view input;
        std::string owner;
        std::map<std::string, semantic::records::type>& types;
        std::size_t at = 0;

        auto peek() const -> char { return at < input.size() ? input[at] : '\0'; }
        void take(char expected) {
            if (peek() != expected)
                throw std::invalid_argument("invalid JVM class Signature");
            ++at;
        }
        auto save(semantic::records::type row, std::size_t begin) -> std::string {
            row.id = owner + ":signature:" + std::string{input.substr(begin, at - begin)};
            row.domain = "jvm";
            auto id = row.id;
            types.try_emplace(id, std::move(row));
            return id;
        }
        auto type() -> std::string {
            const auto begin = at;
            const auto code = peek();
            if (!code)
                throw std::invalid_argument("truncated JVM class Signature");
            ++at;
            semantic::records::type row;
            if (code == 'T') {
                const auto end = input.find(';', at);
                if (end == std::string_view::npos)
                    throw std::invalid_argument("unterminated JVM type variable");
                row.kind = std::string{"parameter"};
                row.name = std::string{input.substr(at, end - at)};
                row.binder = owner;
                at = end + 1;
            } else if (code == '[') {
                row.kind = std::string{"array"};
                row.arguments = {type()};
            } else if (code == 'L') {
                std::string name;
                std::optional<std::string> enclosing;
                for (;;) {
                    const auto start = at;
                    while (peek() && peek() != '<' && peek() != '.' && peek() != ';')
                        ++at;
                    if (at == start)
                        throw std::invalid_argument("missing JVM class Signature name");
                    if (!name.empty())
                        name += '$';
                    name += input.substr(start, at - start);
                    row = {};
                    row.kind = std::string{"named"};
                    row.name = name;
                    row.declaration = name;
                    if (enclosing)
                        row.elements.push_back(*enclosing);
                    if (peek() == '<') {
                        ++at;
                        row.kind = std::string{"application"};
                        while (peek() != '>') {
                            const auto argument = at;
                            if (peek() == '*' || peek() == '+' || peek() == '-') {
                                const auto variance = input[at++];
                                semantic::records::type wildcard{.kind = std::string{"parameter"}, .qualifiers = {"wildcard"}};
                                if (variance == '+')
                                    wildcard.upper_bound = type();
                                else if (variance == '-')
                                    wildcard.lower_bound = type();
                                row.arguments.push_back(save(std::move(wildcard), argument));
                            } else
                                row.arguments.push_back(type());
                        }
                        take('>');
                    }
                    if (peek() != '.')
                        break;
                    enclosing = save(row, begin);
                    ++at;
                }
                take(';');
            } else {
                static const std::map<char, std::string> names{{'Z', "boolean"}, {'B', "byte"}, {'C', "char"},  {'S', "short"},
                                                               {'I', "int"},     {'J', "long"}, {'F', "float"}, {'D', "double"}};
                const auto found = names.find(code);
                if (found == names.end())
                    throw std::invalid_argument("invalid JVM class Signature type");
                row.kind = std::string{"primitive"};
                row.name = found->second;
            }
            return save(std::move(row), begin);
        }
        auto parse() -> std::string {
            semantic::records::type declared{.id = owner + ":signature", .domain = "jvm", .kind = std::string{"named"}, .name = owner, .declaration = owner};
            if (peek() == '<') {
                ++at;
                while (peek() != '>') {
                    const auto end = input.find(':', at);
                    if (end == std::string_view::npos)
                        throw std::invalid_argument("missing JVM type parameter bound");
                    auto name = std::string{input.substr(at, end - at)};
                    at = end;
                    const auto id = owner + ":signature:T" + name + ";";
                    semantic::records::type parameter{.id = id,
                                                      .domain = "jvm",
                                                      .kind = std::string{"parameter"},
                                                      .name = name,
                                                      .binder = owner,
                                                      .parameter_index = declared.type_parameters.size()};
                    take(':');
                    if (peek() != ':' && peek() != '>')
                        parameter.constraints.push_back(type());
                    while (peek() == ':') {
                        ++at;
                        parameter.constraints.push_back(type());
                    }
                    types.insert_or_assign(id, std::move(parameter));
                    declared.type_parameters.push_back(id);
                }
                take('>');
            }
            do {
                declared.constraints.push_back(type());
            } while (at < input.size());
            auto id = declared.id;
            types.emplace(id, std::move(declared));
            return id;
        }
    };
    struct dispatch_target {
        std::string owner, name, descriptor;
    };
    using dispatch_index = std::map<std::string, std::vector<dispatch_target>>;
    struct artifact_owner {
        std::string container, module;
    };
    using class_owners = std::map<std::string, artifact_owner>;
    inline auto local_prefix(const semantic::recipe& recipe, const std::string& container) -> std::string {
        return semantic::context_identifier(recipe) + ":" + content::hash_of(container).str() + ":";
    }
    inline auto records(const std::vector<object>& objects, semantic::recipe recipe, const std::string& module, const std::string& digest,
                        std::string container = {}, const std::vector<object>& inventory = {}, const class_owners& owners = {},
                        const dispatch_index& dispatches = {}) -> semantic::shard {
        if (container.empty())
            container = recipe.unit.name;
        auto context_recipe = recipe;
        context_recipe.unit.name = container;
        context_recipe.unit.kind = semantic::unit_kind::artifact;
        semantic::shard out;
        out.unit = semantic::make_unit(recipe, semantic::context_identifier(context_recipe));
        out.unit.direct_inputs = {{semantic::input_kind::artifact, recipe.unit.name, digest}};
        out.unit.observed_dependencies = {
            {.kind = semantic::dependency_kind::artifact, .project = recipe.unit.project, .key = recipe.unit.name, .value = digest}};
        out.unit.coverage.available = {
            {.modules = {module},
             .contexts = {out.unit.context},
             .facts = {std::string{"symbols"}, std::string{"types"}, std::string{"definitions"}, std::string{"references"}, std::string{"relations"}}}};
        auto ownership = semantic::ownership{out.unit.id, out.unit.context, {.provider = "jvm-artifact", .kind = std::string{"artifact"}}};
        std::map<std::string, std::size_t> symbols;
        auto symbol = [&](const std::string& key, const std::string& name, const std::string& kind, std::optional<std::string> owner = {}) -> std::string {
            if (symbols.contains(key))
                return key;
            symbols[key] = out.symbols.size();
            auto declared_container = container, declared_module = module;
            if (const auto found = owners.find(owner.value_or(key)); found != owners.end()) {
                declared_container = found->second.container;
                declared_module = found->second.module;
            }
            semantic::native_identity identity{recipe.unit.project, declared_module, "jvm-artifact", "jvm",
                                               rfl::json::write(std::tuple{recipe.options, declared_container, key})};
            out.identities.push_back({identity, key, {}});
            out.symbols.push_back({.id = key,
                                   .ownership = ownership,
                                   .language = "jvm",
                                   .domain = "jvm",
                                   .module = declared_module,
                                   .name = name,
                                   .qualified_name = key,
                                   .kind = semantic::symbol_kind{kind},
                                   .owner = owner,
                                   .modifiers = {"import"},
                                   .native_key = identity});
            return key;
        };
        std::map<std::string, semantic::records::type> types;
        std::function<std::string(std::string_view, std::size_t&)> type;
        type = [&](std::string_view descriptor, std::size_t& at) -> std::string {
            const auto begin = at;
            if (at >= descriptor.size())
                throw std::invalid_argument("incomplete JVM descriptor");
            auto code = descriptor[at++];
            semantic::records::type record;
            record.domain = "jvm";
            if (code == '[') {
                record.kind = std::string{"array"};
                record.arguments = {type(descriptor, at)};
            } else if (code == 'L') {
                auto end = descriptor.find(';', at);
                if (end == std::string_view::npos)
                    throw std::invalid_argument("unterminated JVM object descriptor");
                auto name = std::string{descriptor.substr(at, end - at)};
                at = end + 1;
                record.kind = std::string{"named"};
                record.name = name;
                record.declaration = symbol(name, name, "type");
            } else {
                static const std::map<char, std::string> names{{'V', "void"}, {'Z', "boolean"}, {'B', "byte"},  {'C', "char"},  {'S', "short"},
                                                               {'I', "int"},  {'J', "long"},    {'F', "float"}, {'D', "double"}};
                if (!names.contains(code))
                    throw std::invalid_argument("invalid JVM type descriptor");
                record.kind = std::string{"primitive"};
                record.name = names.at(code);
            }
            record.id = std::string{descriptor.substr(begin, at - begin)};
            auto id = record.id;
            types.try_emplace(id, std::move(record));
            return id;
        };
        auto signature = [&](const std::string& descriptor) -> std::string {
            std::size_t at = 0;
            if (!descriptor.starts_with('('))
                return type(descriptor, at);
            ++at;
            semantic::records::type record{.id = descriptor, .domain = "jvm", .kind = std::string{"function"}};
            record.parameter_groups.emplace_back();
            while (at < descriptor.size() && descriptor[at] != ')')
                record.parameter_groups.back().parameters.push_back({.type = type(descriptor, at)});
            if (at == descriptor.size())
                throw std::invalid_argument("unterminated JVM method descriptor");
            ++at;
            record.return_type = type(descriptor, at);
            types.try_emplace(descriptor, std::move(record));
            return descriptor;
        };
        auto method_key = [](const std::string& owner, const std::string& name, const std::string& descriptor) { return owner + "." + name + descriptor; };
        // Seed definitions before following calls, so intra-artifact targets share their declaration IDs.
        for (const auto& object : objects) {
            symbol(object.name, object.name, "type");
            for (const auto& member : object.members)
                symbol(method_key(object.name, member.name, member.descriptor), member.name,
                       member.method ? (member.name == "<init>" ? "constructor" : "method") : "variable", object.name);
        }
        for (const auto& object : objects) {
            std::optional<std::string> class_type;
            if (!object.signature.empty()) {
                class_signature parser{object.signature, object.name, types};
                class_type = parser.parse();
            }
            auto& klass = out.symbols.at(symbols.at(object.name));
            klass.modifiers.clear();
            klass.type = class_type;
            klass.extensions = {{"jvm.access_flags", std::int64_t{object.flags}},
                                {"jvm.signature", object.signature},
                                {"jvm.annotations", rfl::to_generic(object.annotations)}};
            out.occurrences.push_back({.id = object.name + ":definition",
                                       .ownership = ownership,
                                       .symbol = object.name,
                                       .location = {.artifact = semantic::artifact_location{digest, module, object.entry, 0}},
                                       .roles = {std::string{"definition"}}});
            auto inherit = [&](const std::string& parent, std::string kind) {
                if (parent.empty())
                    return;
                auto target = symbol(parent, parent, "type");
                out.relations.push_back({.id = object.name + ":" + kind + ":" + parent,
                                         .kind = semantic::relation_kind{kind},
                                         .ownership = ownership,
                                         .from = object.name,
                                         .to = target,
                                         .resolution = std::string{"resolved"},
                                         .from_context = out.unit.context,
                                         .to_context = out.unit.context});
            };
            inherit(object.parent, "inherits");
            for (const auto& parent : object.interfaces)
                inherit(parent, "implements");
            for (const auto& member : object.members) {
                const auto key = method_key(object.name, member.name, member.descriptor);
                const auto member_type = signature(member.descriptor);
                auto& row = out.symbols.at(symbols.at(key));
                row.type = member_type;
                row.modifiers.clear();
                row.visibility = member.flags & 1 ? "public" : member.flags & 2 ? "private" : member.flags & 4 ? "protected" : "package";
                for (auto [mask, name] : std::initializer_list<std::pair<unsigned, const char*>>{
                         {8, "static"}, {16, "final"}, {256, "native"}, {1024, "abstract"}, {4096, "synthetic"}})
                    if (member.flags & mask)
                        row.modifiers.push_back(name);
                row.extensions = {{"jvm.class", object.name}, {"jvm.descriptor", member.descriptor}, {"jvm.signature", member.signature}};
                row.extensions["jvm.annotations"] = rfl::to_generic(member.annotations);
                if (member.constant)
                    row.extensions["jvm.constant"] = *member.constant;
                out.occurrences.push_back(
                    {.id = key + ":definition",
                     .ownership = ownership,
                     .symbol = key,
                     .location = {.artifact = semantic::artifact_location{digest, module, object.entry + "#" + member.name + member.descriptor, 0}},
                     .roles = {std::string{"definition"}}});
                for (const auto& call : member.calls) {
                    const auto reference = key + ":call:" + std::to_string(call.offset);
                    semantic::locator location{
                        .artifact = semantic::artifact_location{digest, module, object.entry + "#" + member.name + member.descriptor, call.offset}};
                    if (call.dynamic) {
                        out.references.push_back({.id = reference,
                                                  .ownership = ownership,
                                                  .from = key,
                                                  .location = location,
                                                  .roles = {std::string{"reference"}, std::string{"call"}},
                                                  .resolution = std::string{"unresolved"},
                                                  .dispatch = std::string{"indirect"}});
                        ++out.unit.coverage.unresolved_count;
                        continue;
                    }
                    auto owner = symbol(call.owner, call.owner, "type");
                    const auto target =
                        symbol(method_key(call.owner, call.name, call.descriptor), call.name, call.name == "<init>" ? "constructor" : "method", owner);
                    const auto target_type = signature(call.descriptor);
                    out.symbols.at(symbols.at(target)).type = target_type;
                    std::vector<std::string> targets{target};
                    const bool virtual_call = call.opcode == 0xb6 || call.opcode == 0xb9;
                    if (virtual_call) {
                        const auto dispatch = rfl::json::write(std::tuple{call.owner, call.name, call.descriptor});
                        if (const auto found = dispatches.find(dispatch); found != dispatches.end()) {
                            for (const auto& candidate : found->second) {
                                const auto owner = symbol(candidate.owner, candidate.owner, "type");
                                targets.push_back(symbol(method_key(candidate.owner, candidate.name, candidate.descriptor), candidate.name, "method", owner));
                            }
                        } else {
                            const auto& universe = inventory.empty() ? objects : inventory;
                            std::map<std::string, const jvm::object*> hierarchy;
                            for (const auto& value : universe)
                                hierarchy[value.name] = &value;
                            auto derives = [&](const std::string& name) {
                                std::set<std::string> pending{name}, seen;
                                while (!pending.empty()) {
                                    auto current = *pending.begin();
                                    pending.erase(pending.begin());
                                    if (!seen.insert(current).second)
                                        continue;
                                    if (current == call.owner)
                                        return true;
                                    if (auto found = hierarchy.find(current); found != hierarchy.end()) {
                                        if (!found->second->parent.empty())
                                            pending.insert(found->second->parent);
                                        pending.insert(found->second->interfaces.begin(), found->second->interfaces.end());
                                    }
                                }
                                return false;
                            };
                            for (const auto& value : universe)
                                if (derives(value.name))
                                    for (const auto& candidate : value.members)
                                        if (candidate.method && candidate.name == call.name && candidate.descriptor == call.descriptor &&
                                            !(candidate.flags & (8 | 2 | 1024))) {
                                            const auto candidate_owner = symbol(value.name, value.name, "type");
                                            targets.push_back(symbol(method_key(value.name, call.name, call.descriptor), call.name, "method", candidate_owner));
                                        }
                        }
                        std::ranges::sort(targets);
                        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
                    }
                    out.references.push_back(
                        {.id = reference,
                         .ownership = ownership,
                         .from = key,
                         .location = location,
                         .target = target,
                         .candidates = virtual_call ? targets : std::vector<std::string>{},
                         .roles = {std::string{"reference"}, std::string{"call"}},
                         .resolution = virtual_call ? std::string{"ambiguous"} : std::string{"resolved"},
                         .dispatch = virtual_call ? (call.opcode == 0xb9 ? std::string{"interface"} : std::string{"virtual"}) : std::string{"direct"},
                         .target_context = out.unit.context});
                    if (virtual_call)
                        ++out.unit.coverage.unresolved_count;
                    for (const auto& candidate : targets)
                        out.relations.push_back({.id = reference + ":" + candidate,
                                                 .kind = std::string{"calls"},
                                                 .ownership = ownership,
                                                 .from = key,
                                                 .to = candidate,
                                                 .resolution = virtual_call ? std::string{"candidate"} : std::string{"resolved"},
                                                 .reference = reference,
                                                 .from_context = out.unit.context,
                                                 .to_context = out.unit.context});
                }
            }
        }
        for (auto& [_, value] : types) {
            if (value.declaration)
                symbol(*value.declaration, *value.declaration, "type");
            out.types.push_back(std::move(value));
        }
        // A class contribution keeps identities for cross-class edges, while the
        // defining class alone owns the actual declaration rows.
        if (!inventory.empty()) {
            std::set<std::string> own_classes;
            for (const auto& value : objects)
                own_classes.insert(value.name);
            std::erase_if(out.symbols, [&](const auto& row) {
                const auto declaring = row.owner.value_or(row.id);
                return owners.contains(declaring) && !own_classes.contains(declaring);
            });
        }
        const auto prefix = local_prefix(recipe, container);
        auto symbol_id = [&](std::string& value) { value = prefix + value; };
        auto optional_id = [&](auto& value) {
            if (value)
                symbol_id(*value);
        };
        for (auto& binding : out.identities)
            symbol_id(binding.symbol);
        for (auto& row : out.symbols) {
            symbol_id(row.id);
            optional_id(row.owner);
            optional_id(row.type);
        }
        for (auto& row : out.types) {
            symbol_id(row.id);
            optional_id(row.declaration);
            optional_id(row.binder);
            for (auto* values : {&row.arguments, &row.constraints, &row.elements, &row.type_parameters})
                for (auto& id : *values)
                    symbol_id(id);
            optional_id(row.receiver);
            optional_id(row.return_type);
            optional_id(row.lower_bound);
            optional_id(row.upper_bound);
            for (auto& group : row.parameter_groups)
                for (auto& parameter : group.parameters)
                    symbol_id(parameter.type);
            for (auto& argument : row.application_arguments)
                optional_id(argument.type);
        }
        for (auto& row : out.occurrences) {
            symbol_id(row.id);
            symbol_id(row.symbol);
        }
        for (auto& row : out.references) {
            symbol_id(row.id);
            optional_id(row.from);
            optional_id(row.target);
            for (auto& id : row.candidates)
                symbol_id(id);
        }
        for (auto& row : out.relations) {
            symbol_id(row.id);
            symbol_id(row.from);
            symbol_id(row.to);
            optional_id(row.reference);
        }
        return out;
    }
    struct source_line {
        std::string file, name;
        std::uint32_t line = 0;
    };
    struct line_mapping {
        std::uint32_t input = 0, file = 0, count = 1, output = 0, increment = 1;
    };
    struct source_map {
        std::map<std::uint32_t, std::pair<std::string, std::string>> files;
        std::vector<line_mapping> lines;
        bool present = false;
        auto locate(std::uint32_t line, const std::string& fallback) const -> std::vector<source_line> {
            if (!present)
                return {{fallback, fallback, line}};
            std::vector<source_line> out;
            for (const auto& entry : lines) {
                if (line < entry.output || std::uint64_t{line} - entry.output >= std::uint64_t{entry.count} * entry.increment)
                    continue;
                if (const auto file = files.find(entry.file); file != files.end())
                    out.push_back({file->second.second, file->second.first, entry.input + (line - entry.output) / entry.increment});
            }
            return out;
        }
    };
    // Kotlin's primary stratum describes generated and inlined source lines.
    // KotlinDebug describes call sites and must not replace that mapping.
    inline auto source_mapping(const std::string& debug) -> source_map {
        source_map out;
        if (debug.empty())
            return out;
        out.present = true;
        std::istringstream stream{debug};
        std::vector<std::string> text;
        for (std::string line; std::getline(stream, line);) {
            if (line.ends_with('\r'))
                line.pop_back();
            text.push_back(std::move(line));
        }
        if (text.size() < 3 || text[0] != "SMAP")
            return out;
        const auto& stratum = text[2];
        bool selected = false;
        char section = 0;
        std::uint32_t file_id = 0;
        auto number = [](std::string_view value, std::uint32_t& result) {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
        };
        for (std::size_t i = 3; i < text.size(); ++i) {
            const auto& line = text[i];
            if (line.starts_with("*S ")) {
                selected = line.substr(3) == stratum;
                section = 0;
                file_id = 0;
                continue;
            }
            if (line.starts_with('*')) {
                section = line == "*F" ? 'F' : line == "*L" ? 'L' : 0;
                continue;
            }
            if (!selected || line.empty())
                continue;
            if (section == 'F') {
                const bool extended = line.starts_with("+ ");
                const std::string_view value = std::string_view{line}.substr(extended ? 2 : 0);
                const auto space = value.find(' ');
                std::uint32_t id = 0;
                if (space == std::string_view::npos || !number(value.substr(0, space), id))
                    continue;
                auto name = std::string{value.substr(space + 1)}, path = name;
                if (extended && i + 1 < text.size() && !text[i + 1].starts_with('*'))
                    path = text[++i];
                out.files[id] = {std::move(name), std::move(path)};
            } else if (section == 'L') {
                const auto colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                const auto input = std::string_view{line}.substr(0, colon), output = std::string_view{line}.substr(colon + 1);
                const auto hash = input.find('#'), comma = input.find(','), step = output.find(',');
                line_mapping mapping;
                if (!number(input.substr(0, std::min(hash, comma)), mapping.input) || !number(output.substr(0, step), mapping.output))
                    continue;
                if (hash != std::string_view::npos && !number(input.substr(hash + 1, comma == std::string_view::npos ? comma : comma - hash - 1), file_id))
                    continue;
                mapping.file = file_id;
                if (comma != std::string_view::npos && !number(input.substr(comma + 1), mapping.count))
                    continue;
                if (step != std::string_view::npos && !number(output.substr(step + 1), mapping.increment))
                    continue;
                if (mapping.input && mapping.count && mapping.increment)
                    out.lines.push_back(mapping);
            }
        }
        return out;
    }
    inline void associate(semantic::shard& shard, const std::vector<object>& objects, const std::vector<channels::definition_body>& sources,
                          const std::map<std::string, std::string>& documents) {
        std::unordered_map<std::string_view, const semantic::records::symbol*> declarations;
        for (const auto& row : shard.symbols)
            declarations.try_emplace(row.qualified_name, &row);
        std::unordered_map<semantic::symbol_id, const semantic::records::occurrence*> positions;
        for (const auto& row : shard.occurrences)
            positions.try_emplace(row.symbol, &row);
        std::unordered_set<semantic::symbol_id> retained;
        for (const auto& row : shard.identities)
            retained.insert(row.symbol);
        std::map<std::string, std::vector<std::uint64_t>> starts;
        for (const auto& [path, text] : documents) {
            auto& lines = starts[path];
            lines.push_back(0);
            std::uint64_t scalar = 0;
            for (const auto byte : text)
                if ((static_cast<unsigned char>(byte) & 0xc0) != 0x80) {
                    ++scalar;
                    if (byte == '\n')
                        lines.push_back(scalar);
                }
            lines.push_back(scalar + 1);
        }
        auto paths = [&](const source_line& point) {
            std::set<std::string> exact, names;
            const auto debug = std::filesystem::path{point.file}.lexically_normal().generic_string();
            for (const auto& [path, _] : starts) {
                if (debug == path || debug.ends_with("/" + path) || path.ends_with("/" + debug))
                    exact.insert(path);
                if (std::filesystem::path{path}.filename() == point.name)
                    names.insert(path);
            }
            return exact.empty() ? names : exact;
        };
        std::set<std::pair<std::string, std::string>> emitted;
        for (const auto& object : objects) {
            const auto mapping = source_mapping(object.debug);
            for (const auto& member : object.members) {
                if (!member.method || member.lines.empty())
                    continue;
                const auto qualified = object.name + "." + member.name + member.descriptor;
                const auto declaration = declarations.find(qualified);
                if (declaration == declarations.end())
                    continue;
                const auto& target = declaration->second->id;
                for (const auto& [offset, line] : member.lines)
                    for (const auto& point : mapping.locate(line, object.source)) {
                        const auto matched = paths(point);
                        if (matched.size() != 1 || !point.line)
                            continue;
                        const auto& path = *matched.begin();
                        const auto& lines = starts.at(path);
                        if (point.line >= lines.size())
                            continue;
                        const channels::definition_body* selected = nullptr;
                        bool ambiguous = false;
                        for (const auto& source : sources) {
                            if (source.body.path != path || source.body.end <= lines[point.line - 1] || source.body.begin >= lines[point.line])
                                continue;
                            if (!selected)
                                selected = &source;
                            else if (source.identity.symbol == selected->identity.symbol && source.context == selected->context)
                                continue;
                            else if (source.body.begin >= selected->body.begin && source.body.end <= selected->body.end &&
                                     (source.body.begin != selected->body.begin || source.body.end != selected->body.end))
                                selected = &source;
                            else if (!(selected->body.begin >= source.body.begin && selected->body.end <= source.body.end &&
                                       (source.body.begin != selected->body.begin || source.body.end != selected->body.end)))
                                ambiguous = true;
                        }
                        if (!selected || ambiguous || !emitted.emplace(selected->identity.symbol, target).second)
                            continue;
                        const auto found = positions.find(target);
                        if (found == positions.end())
                            continue;
                        const auto* occurrence = found->second;
                        if (retained.insert(selected->identity.symbol).second)
                            shard.identities.push_back(selected->identity);
                        auto ownership = occurrence->ownership;
                        ownership.origin.kind = std::string{"debug"};
                        ownership.origin.location = selected->location;
                        ownership.origin.location->artifact = occurrence->location.artifact;
                        shard.relations.push_back({.id = "emits:" + selected->identity.symbol + ":" + target,
                                                   .kind = std::string{"emits"},
                                                   .ownership = std::move(ownership),
                                                   .from = selected->identity.symbol,
                                                   .to = target,
                                                   .resolution = std::string{"resolved"},
                                                   .detail = semantic::records::emission_detail{.file = path, .line = point.line},
                                                   .from_context = selected->context,
                                                   .to_context = shard.unit.context});
                    }
            }
        }
    }
    // Unit names are structural tuples; container paths remain explicit locator provenance.
    inline auto class_key(const std::string& container, const std::string& klass, const std::string& role) -> std::string {
        return rfl::json::write(std::tuple{role, container, klass});
    }
    inline void logical_locations(semantic::shard& shard, const semantic::artifact_input& input) {
        auto bind = [&](semantic::locator& location) {
            if (location.artifact && !location.artifact->input) {
                location.artifact->artifact.clear();
                location.artifact->input = input;
            }
        };
        for (auto& row : shard.occurrences)
            bind(row.location);
        for (auto& row : shard.references)
            bind(row.location);
        for (auto& row : shard.relations)
            if (row.ownership.origin.location)
                bind(*row.ownership.origin.location);
    }
    class reader : public language::reader_adapter {
    public:
        using language::reader_adapter::reader_adapter;

    protected:
        auto do_open(rfl::Generic initialization) -> coroutine::task<> override {
            auto value =
                rfl::from_generic<std::tuple<std::string, std::optional<std::string>, language::server<"jvm-artifact">::options>>(initialization).value();
            path_ = std::get<0>(value);
            model_ = std::get<1>(value);
            options_ = std::get<2>(value);
            co_return;
        }
        auto do_units(const semantic::extent& demand) -> coroutine::task<std::vector<semantic::unit_key>> override {
            if (!objects_)
                throw std::logic_error("JVM reader has no immutable object loader");
            std::map<std::string, container> selected;
            const auto artifact = installed_.model.find(path_);
            if (artifact != installed_.model.end() && !artifact->second.empty()) {
                selected[path_] = {.module = options_.module, .inputs = {{path_, artifact->second}}};
            } else {
                const auto prefix = path_.ends_with('/') ? path_ : path_ + "/";
                for (const auto& [path, digest] : installed_.model)
                    if (path.starts_with(prefix) && path.ends_with(".class") && !digest.empty())
                        selected[path_].inputs.emplace(path, digest);
                if (selected[path_].inputs.empty())
                    throw std::invalid_argument("JVM reader requires captured class or JAR inputs");
                selected[path_].module = options_.module;
            }
            if (model_) {
                std::string text;
                for (auto bytes : durability::read_plain(root_ / content::checked_path(*model_), constants::semantic_object_bytes))
                    text.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                const auto model = projectmodel::read(text);
                for (const auto& unit : model.units)
                    for (const auto& reference : unit.references)
                        reference.visit([&](const auto& value) {
                            using T = std::remove_cvref_t<decltype(value)>;
                            if constexpr (std::same_as<T, projectmodel::artifact_reference>) {
                                if (value.role != "classpath" && value.role != "friend")
                                    return;
                                if (!value.object.name.ends_with(".jar") && !value.object.name.ends_with(".class"))
                                    return;
                                const auto logical = "object:" + value.object.hash;
                                selected[logical] = {.module = logical, .inputs = {{logical, value.object.hash}}};
                            }
                        });
            }
            std::vector<const semantic::shard*> sources;
            for (const auto& [_, parts] : installed_.source_facts)
                for (const auto& part : parts)
                    sources.push_back(part->data.get());
            std::map<semantic::symbol_id, std::set<semantic::context_id>> declarations;
            for (const auto* source : sources)
                for (const auto& symbol : source->symbols)
                    if (symbol.domain == "kotlin")
                        declarations[symbol.id].insert(symbol.ownership.context);
            std::map<std::string, std::set<std::string>> source_bindings;
            std::map<std::string, std::set<semantic::unit_id>> source_units;
            for (const auto* source : sources)
                for (const auto& binding : source->identities)
                    if (const auto* native = std::get_if<semantic::native_identity>(&binding.declaration); native && native->domain == "kotlin") {
                        source_bindings[native->native_key].insert(
                            rfl::json::write(std::tuple{*native, binding.symbol, declarations[binding.symbol], source->unit.id}));
                        source_units[native->native_key].insert(source->unit.id);
                    }
            values_.clear();
            keys_.clear();
            owners_.clear();
            inventory_.clear();
            identity_producers_.clear();
            for (auto& [logical, container] : selected) {
                const auto cached = containers_.find(logical);
                if (cached != containers_.end() && cached->second.inputs == container.inputs) {
                    container.classes = std::move(cached->second.classes);
                    container.decoded = std::move(cached->second.decoded);
                    container.locations = std::move(cached->second.locations);
                } else {
                    std::map<std::string, const object*> previous;
                    if (cached != containers_.end())
                        for (const auto& object : cached->second.classes)
                            previous.emplace(object.value, &object);
                    for (const auto& [input, digest] : container.inputs) {
                        const auto path = objects_.path(digest);
                        std::string data;
                        for (auto bytes : durability::read_plain(path, constants::semantic_object_bytes))
                            data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                        auto parsed = classes(data, options_.java_release, &previous);
                        for (auto& value : parsed) {
                            container.locations[value.name] = input;
                            container.classes.push_back(std::move(value));
                        }
                    }
                }
                if (!options_.metadata_helper.empty() && !container.decoded &&
                    (demand.modules.empty() || std::ranges::contains(demand.modules, container.module))) {
                    std::vector<std::string> paths;
                    for (const auto& [_, digest] : container.inputs)
                        paths.push_back((objects_.path(digest)).string());
                    auto command = commands_.at("metadata");
                    if (paths.size() != 1)
                        command.back() = "--jvm-metadata-list";
                    command.push_back(std::to_string(options_.java_release));
                    if (paths.size() == 1)
                        command.push_back(paths.front());
                    else {
                        const auto list = root_ / (".fughetta-jvm-metadata-" + state_.id + ".json");
                        struct cleanup {
                            std::filesystem::path path;
                            ~cleanup() {
                                try {
                                    durability::remove_file(path);
                                } catch (...) {
                                }
                            }
                        } guard{list};
                        durability::durable_write(list, rfl::json::write(paths));
                        command.push_back(list.string());
                        container.decoded = rfl::json::read<kotlin_metadata::report>(co_await run(std::move(command))).value();
                    }
                    if (!container.decoded)
                        container.decoded = rfl::json::read<kotlin_metadata::report>(co_await run(std::move(command))).value();
                }
                // Class inputs are also the source of truth after a cold worker restart.
                if (container.locations.empty()) {
                    if (container.inputs.size() == 1)
                        for (const auto& object : container.classes)
                            container.locations[object.name] = container.inputs.begin()->first;
                    else
                        for (const auto& object : container.classes) {
                            const auto suffix = "/" + object.name + ".class";
                            const auto found = std::ranges::find_if(container.inputs, [&](const auto& input) { return input.first.ends_with(suffix); });
                            if (found == container.inputs.end())
                                throw std::invalid_argument("captured JVM class has no input binding");
                            container.locations[object.name] = found->first;
                        }
                }
                for (const auto& object : container.classes) {
                    if (!(object.flags & 0x8000) && !owners_.emplace(object.name, artifact_owner{logical, container.module}).second)
                        throw std::invalid_argument("JVM class is provided by multiple artifact inputs: " + object.name);
                    inventory_.push_back(object);
                }
                values_[{"jvm/inventory", logical}] = content::hash_of(rfl::json::write(container.inputs)).str();
            }
            containers_ = std::move(selected);
            dispatches_.clear();
            std::map<std::string, std::vector<const object*>> derived;
            std::map<std::string, const object*> classes_by_name;
            for (const auto& object : inventory_) {
                classes_by_name[object.name] = &object;
                if (!object.parent.empty())
                    derived[object.parent].push_back(&object);
                for (const auto& interface : object.interfaces)
                    derived[interface].push_back(&object);
            }
            auto dispatch = [&](const call& call) {
                const auto key = rfl::json::write(std::tuple{call.owner, call.name, call.descriptor});
                if (dispatches_.contains(key))
                    return;
                auto& targets = dispatches_[key];
                std::set<std::string> pending{call.owner}, seen;
                while (!pending.empty()) {
                    auto current = *pending.begin();
                    pending.erase(pending.begin());
                    if (!seen.insert(current).second)
                        continue;
                    if (const auto found = classes_by_name.find(current); found != classes_by_name.end())
                        for (const auto& member : found->second->members)
                            if (member.method && member.name == call.name && member.descriptor == call.descriptor && !(member.flags & (8 | 2 | 1024)))
                                targets.push_back({current, call.name, call.descriptor});
                    if (const auto children = derived.find(current); children != derived.end())
                        for (const auto* child : children->second)
                            pending.insert(child->name);
                }
                std::ranges::sort(targets, {}, &dispatch_target::owner);
                values_[{"jvm/dispatch", key}] = content::hash_of(rfl::json::write(targets)).str();
            };
            std::vector<semantic::unit_key> out;
            auto append = [&](const std::string& name, semantic::unit_kind kind) {
                semantic::unit_key key{state_.project, state_.stage, state_.provider, "evaluate", kind, name};
                out.push_back(key);
                return key;
            };
            for (auto& [logical, container] : containers_) {
                if (!demand.modules.empty() && !std::ranges::contains(demand.modules, container.module))
                    continue;
                append(class_key(logical, "", "container"), semantic::unit_kind::group);
                container.targets.clear();
                auto& targets = container.targets;
                semantic::recipe recipe{{state_.project, state_.stage, state_.provider, "evaluate", semantic::unit_kind::artifact, logical}, state_.options};
                const auto context = semantic::context_identifier(recipe), prefix = local_prefix(recipe, logical);
                for (std::size_t i = 0; i < container.classes.size(); ++i) {
                    const auto& object = container.classes[i];
                    semantic::native_identity class_identity{state_.project, container.module, state_.provider, "jvm",
                                                             rfl::json::write(std::tuple{state_.options, logical, object.name})};
                    targets.emplace(
                        kotlin_metadata::member_key{object.name, "class", "", ""},
                        kotlin_metadata::member_target{{class_identity, prefix + object.name, {}},
                                                       context,
                                                       {.artifact = semantic::artifact_location{
                                                            "", container.module, object.entry, 0,
                                                            semantic::artifact_input{state_.project, state_.stage, container.locations.at(object.name)}}}});
                    const auto key = class_key(logical, object.name, "class");
                    append(key, semantic::unit_kind::artifact);
                    keys_[key] = {logical, i};
                    values_[{"jvm/class", key}] = content::hash_of(rfl::json::write(std::tuple{object.entry, object.value})).str();
                    for (const auto& member : object.members) {
                        const auto raw = object.name + "." + member.name + member.descriptor;
                        semantic::native_identity identity{state_.project, container.module, state_.provider, "jvm",
                                                           rfl::json::write(std::tuple{state_.options, logical, raw})};
                        semantic::locator location{.artifact = semantic::artifact_location{
                                                       "", container.module, object.entry + "#" + member.name + member.descriptor, 0,
                                                       semantic::artifact_input{state_.project, state_.stage, container.locations.at(object.name)}}};
                        targets.emplace(kotlin_metadata::member_key{object.name, member.method ? "method" : "field", member.name, member.descriptor},
                                        kotlin_metadata::member_target{{identity, prefix + raw, {}}, context, location});
                        for (const auto& call : member.calls)
                            if (call.opcode == 0xb6 || call.opcode == 0xb9) {
                                dispatch(call);
                            }
                    }
                    if (!installed_.source_facts.empty() && logical == path_) {
                        const auto key = class_key(logical, object.name, "source_map");
                        append(key, semantic::unit_kind::group);
                        keys_[key] = {logical, i};
                    }
                }
                if (container.decoded) {
                    const auto context = semantic::context_identifier(recipe, logical + ":kotlin");
                    container.projector =
                        std::make_unique<kotlin_metadata::projector>(*container.decoded, container.module, options_.source_provider, context, targets, sources);
                    for (const auto& klass : container.decoded->classes) {
                        const auto found = std::ranges::find(container.classes, klass.owner, &object::name);
                        if (found == container.classes.end())
                            throw std::invalid_argument("Kotlin metadata class is absent from its captured artifact");
                        const auto key = class_key(logical, klass.owner, "metadata");
                        append(key, semantic::unit_kind::group);
                        keys_[key] = {logical, static_cast<std::size_t>(found - container.classes.begin())};
                        const auto observation = "jvm/identities:" + key;
                        std::set<std::string> reads;
                        for (const auto& identity : klass.identities) {
                            if (const auto bindings = source_bindings.find(identity.key); bindings != source_bindings.end())
                                reads.insert(bindings->second.begin(), bindings->second.end());
                            if (const auto units = source_units.find(identity.key); units != source_units.end())
                                identity_producers_[observation].insert(units->second.begin(), units->second.end());
                        }
                        values_[{"jvm/identities", observation}] = content::hash_of(rfl::json::write(reads)).str();
                    }
                }
            }
            co_return out;
        }
        auto input_value(const semantic::input_object& input) const -> std::optional<std::string> override {
            if (input.kind == semantic::input_kind::channel) {
                const auto found = values_.find({"jvm/identities", input.name});
                return found == values_.end() ? std::optional<std::string>{} : found->second;
            }
            if (input.kind != semantic::input_kind::provider)
                return {};
            if (input.type == "jvm/lookup")
                return lookup(input.name);
            const auto found = values_.find({input.type, input.name});
            return found == values_.end() ? std::optional<std::string>{} : found->second;
        }
        auto do_evaluate(const language::evaluation_request& request) -> coroutine::task<language::evaluated_unit> override {
            const semantic::recipe recipe{request.unit, state_.options};
            const auto name = rfl::json::read<std::tuple<std::string, std::string, std::string>>(request.unit.name).value();
            const auto& role = std::get<0>(name);
            const auto& logical = std::get<1>(name);
            auto& container = containers_.at(logical);
            semantic::shard shard;
            if (role == "container") {
                shard.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
                shard.unit.modules = {container.module};
                for (const auto& [path, digest] : container.inputs)
                    shard.unit.direct_inputs.push_back({semantic::input_kind::artifact, path, digest});
                shard.unit.direct_inputs.push_back({semantic::input_kind::provider, logical, values_.at({"jvm/inventory", logical}), "jvm/inventory"});
                shard.unit.coverage.available = {
                    {.modules = {container.module},
                     .contexts = {shard.unit.context},
                     .facts = {std::string{"symbols"}, std::string{"types"}, std::string{"definitions"}, std::string{"references"}, std::string{"relations"}}}};
            } else {
                const auto found = keys_.find(request.unit.name);
                if (found == keys_.end())
                    throw std::invalid_argument("JVM class unit no longer exists");
                const auto& object = container.classes.at(found->second.second);
                if (role == "metadata") {
                    shard = container.projector->records(recipe, object.name);
                    for (auto& available : shard.unit.coverage.available)
                        available.facts.push_back(std::string{"references"});
                } else
                    shard = records({object}, recipe, container.module, {}, logical, inventory_, owners_, dispatches_);
                shard.unit.modules = {container.module};
                const auto key = class_key(logical, object.name, "class");
                shard.unit.direct_inputs = {{semantic::input_kind::provider, key, values_.at({"jvm/class", key}), "jvm/class"}};
                shard.unit.observed_dependencies.clear();
                if (role == "metadata") {
                    const auto key = "jvm/identities:" + request.unit.name;
                    const auto value = values_.at({"jvm/identities", key});
                    shard.unit.direct_inputs.push_back({semantic::input_kind::channel, key, value});
                    const auto& producers = identity_producers_[key];
                    shard.unit.observed_dependencies.push_back({.kind = semantic::dependency_kind::declaration,
                                                                .project = state_.project,
                                                                .key = key,
                                                                .value = value,
                                                                .channel = "source_identities",
                                                                .scope = container.module,
                                                                .producers = {producers.begin(), producers.end()}});
                }
                if (role == "source_map") {
                    const auto definitions = channels::definition_bodies(installed_.source_facts, "definition_bodies");
                    std::set<std::string> files;
                    const auto mapping = source_mapping(object.debug);
                    for (const auto& definition : definitions) {
                        const auto file = std::filesystem::path{definition.body.path}.filename().generic_string();
                        if (file == object.source || std::ranges::any_of(mapping.files, [&](const auto& entry) { return entry.second.first == file; }))
                            if (installed_.source_files.contains(definition.body.path))
                                files.insert(definition.body.path);
                    }
                    std::map<std::string, std::string> documents;
                    if (!files.empty()) {
                        if (!loader_)
                            throw std::logic_error("JVM source mapping requires a document loader");
                        documents = co_await loader_.read(files);
                    }
                    associate(shard, {object}, definitions, documents);
                    std::erase_if(shard.relations, [](const auto& relation) { return relation.kind != "emits"; });
                    shard.symbols.clear();
                    shard.types.clear();
                    shard.occurrences.clear();
                    shard.references.clear();
                    std::set<semantic::symbol_id> retained;
                    for (const auto& relation : shard.relations) {
                        retained.insert(relation.from);
                        retained.insert(relation.to);
                    }
                    std::erase_if(shard.identities, [&](const auto& binding) { return !retained.contains(binding.symbol); });
                    shard.unit.coverage.unresolved_count = 0;
                    for (const auto& file : files)
                        shard.unit.observed_dependencies.push_back(
                            {.kind = semantic::dependency_kind::source, .project = state_.project, .key = file, .value = installed_.source_files.at(file)});
                    std::map<std::string, std::string> selected;
                    for (const auto& file : files)
                        selected[file] = installed_.source_files.at(file);
                    shard.unit.observed_dependencies.push_back(
                        enumeration::read(state_.project, "source_documents", selected, {.files = {files.begin(), files.end()}}));
                }
                if (role == "source_map") {
                    std::set<semantic::unit_id> producers;
                    for (const auto& [_, inputs] : installed_.source_facts)
                        for (const auto& source : inputs)
                            if (producers.insert(source->data->unit.id).second)
                                shard.unit.observed_dependencies.push_back(
                                    channels::consume(state_.project, state_.stage, *source->data, "definition_bodies", semantic::dependency_kind::body));
                }
                if (role == "class") {
                    std::set<std::string> lookups{object.parent};
                    lookups.insert(object.interfaces.begin(), object.interfaces.end());
                    for (const auto& member : object.members) {
                        for (std::size_t at = 0; (at = member.descriptor.find('L', at)) != std::string::npos;) {
                            const auto end = member.descriptor.find(';', ++at);
                            if (end == std::string::npos)
                                break;
                            lookups.insert(member.descriptor.substr(at, end - at));
                            at = end + 1;
                        }
                        for (const auto& call : member.calls) {
                            if (!call.owner.empty())
                                lookups.insert(call.owner);
                            if (call.opcode == 0xb6 || call.opcode == 0xb9) {
                                const auto name = rfl::json::write(std::tuple{call.owner, call.name, call.descriptor});
                                const semantic::input_object input{semantic::input_kind::provider, name, values_.at({"jvm/dispatch", name}), "jvm/dispatch"};
                                if (!std::ranges::contains(shard.unit.direct_inputs, input))
                                    shard.unit.direct_inputs.push_back(input);
                            }
                        }
                    }
                    for (const auto& name : lookups)
                        if (!name.empty())
                            shard.unit.direct_inputs.push_back({semantic::input_kind::provider, name, lookup(name), "jvm/lookup"});
                }
                logical_locations(shard, {state_.project, state_.stage, container.locations.at(object.name)});
            }
            shard.unit.direct_inputs.push_back({semantic::input_kind::runtime, "toolchain", runtime_.execution});
            shard.unit.cacheable = !runtime_.execution.empty();
            shard.unit.coverage.requested = request.demand;
            language::evaluated_unit result;
            result.shards.push_back(std::move(shard));
            co_return result;
        }

    private:
        struct container {
            std::string module;
            std::map<std::string, std::string> inputs, locations;
            std::vector<object> classes;
            std::optional<kotlin_metadata::report> decoded;
            kotlin_metadata::member_targets targets;
            std::unique_ptr<kotlin_metadata::projector> projector;
        };
        auto lookup(const std::string& name) const -> std::string {
            const auto found = owners_.find(name);
            return content::hash_of(found == owners_.end() ? std::string{"absent"}
                                                           : rfl::json::write(std::tuple{found->second.container, found->second.module}))
                .str();
        }
        std::string path_;
        std::optional<std::string> model_;
        std::map<std::string, container> containers_;
        std::vector<object> inventory_;
        class_owners owners_;
        dispatch_index dispatches_;
        std::map<std::string, std::set<semantic::unit_id>> identity_producers_;
        std::map<std::string, std::pair<std::string, std::size_t>> keys_;
        std::map<std::pair<std::string, std::string>, std::string> values_;
        language::server<"jvm-artifact">::options options_;
    };
} // namespace jvm
