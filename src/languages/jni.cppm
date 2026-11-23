export module jni;
import std;
import evaluation;
import coroutine;
import json;
import records;
import channels;
import objects;
import jvm;
import llvm;
export import bridge;

export namespace jni {
    using binding::property;
    using from = language::domain<"jvm">;
    using to = language::domain<"native">;
    struct group {
        std::string project, module, native;
        // JVM source declarations bound to C++ source definitions, before any artifact exists.
        bool source = false;
        auto operator<=>(const group&) const = default;
    };
    struct options {
        // JVM artifact module -> native modules in its class loader. Never use
        // the worker host's library search path to infer target loading.
        std::map<std::string, std::vector<std::string>> libraries;
    };
    // JNI names encode UTF-16 code units, including separate surrogate escapes.
    // Accept the class file's modified UTF-8 and normalized UTF-8 alike.
    inline auto escape(std::string_view text) -> std::optional<std::string> {
        std::string out;
        auto append = [&](std::uint32_t value) {
            if (value >= '0' && value <= '3' && (out.empty() || out.back() == '_'))
                return false;
            if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9'))
                out += static_cast<char>(value);
            else if (value == '/')
                out += '_';
            else if (value == '_')
                out += "_1";
            else if (value == ';')
                out += "_2";
            else if (value == '[')
                out += "_3";
            else
                out += std::format("_0{:04x}", value);
            return true;
        };
        for (std::size_t i = 0; i < text.size();) {
            const auto first = static_cast<unsigned char>(text[i++]);
            unsigned count = first < 0x80 ? 0 : first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
            std::uint32_t value = count ? first & ((1u << (6 - count)) - 1) : first;
            if (i + count > text.size())
                throw std::invalid_argument("truncated JNI name");
            while (count--)
                value = (value << 6) | (static_cast<unsigned char>(text[i++]) & 63);
            if (value > 0xffff) {
                value -= 0x10000;
                if (!append(0xd800 + (value >> 10)) || !append(0xdc00 + (value & 1023)))
                    return {};
            } else if (!append(value))
                return {};
        }
        return out;
    }
    inline auto names(const semantic::records::symbol& symbol) -> std::vector<std::string> {
        const auto klass = escape(property(symbol, "jvm.class")), method = escape(symbol.name);
        const auto descriptor = property(symbol, "jvm.descriptor");
        const auto end = descriptor.find(')');
        if (!klass || klass->empty() || !method || !descriptor.starts_with('(') || end == std::string::npos)
            return {};
        const auto arguments = escape(std::string_view{descriptor}.substr(1, end - 1));
        if (!arguments)
            return {};
        const auto short_name = "Java_" + *klass + "_" + *method;
        return {short_name, short_name + "__" + *arguments};
    }
    // A named native source declaration. Property accessors end their CallableId
    // with "get" or "set" instead of their own name.
    inline auto source_declaration(const semantic::records::symbol& symbol) {
        if (symbol.kind != semantic::symbol_kind::make<"function">() && symbol.kind != semantic::symbol_kind::make<"method">())
            return false;
        const std::string_view id = symbol.qualified_name;
        if (symbol.name.empty() || !id.ends_with(symbol.name))
            return false;
        const auto prefix = id.substr(0, id.size() - symbol.name.size());
        if (!prefix.empty() && !prefix.ends_with('.') && !prefix.ends_with('/'))
            return false;
        const bool external = std::ranges::contains(symbol.modifiers, "external");
        const bool native = std::ranges::contains(symbol.modifiers, "native");
        return (symbol.domain == "kotlin" && external) ||
               ((external || native) && (symbol.domain == "scala" || !property(symbol, "jvm.class").empty()));
    }
    inline auto source_definition(const semantic::records::symbol& symbol) {
        if (symbol.domain != "source" || (symbol.language != "cpp" && symbol.language != "rust") ||
            symbol.kind != semantic::symbol_kind::make<"function">())
            return false;
        const auto linked = property(symbol, "native.linkage_name");
        return (linked.empty() ? std::string_view{symbol.name} : std::string_view{linked}).starts_with("Java_");
    }
    // The JVM class comes from the source record or its qualified name
    // "pkg/Outer.Inner.name". Top-level functions use a file facade.
    inline auto source_class(const semantic::records::symbol& symbol, std::string_view file) -> std::string {
        if (auto klass = property(symbol, "jvm.class"); !klass.empty())
            return klass;
        const std::string_view id = symbol.qualified_name;
        const auto slash = id.rfind('/');
        const auto package = slash == std::string_view::npos ? std::string_view{} : id.substr(0, slash);
        const auto rest = slash == std::string_view::npos ? id : id.substr(slash + 1);
        std::string klass;
        if (const auto dot = rest.rfind('.'); dot != std::string_view::npos) {
            klass = std::string{rest.substr(0, dot)};
            std::ranges::replace(klass, '.', '$');
        } else {
            auto stem = std::filesystem::path{std::string{file}}.stem().string();
            if (stem.empty())
                return {};
            if (symbol.domain == "scala")
                klass = stem + "$package";
            else {
                for (auto& c : stem)
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '$')
                        c = '_';
                // Kotlin's package part naming prefixes a stem that cannot start a Java identifier.
                if (std::isdigit(static_cast<unsigned char>(stem.front())))
                    stem.insert(stem.begin(), '_');
                stem.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(stem.front())));
                klass = stem + "Kt";
            }
        }
        return package.empty() ? klass : std::string{package} + "/" + klass;
    }
    inline auto parameter(std::string_view descriptor) -> semantic::records::jni_parameter {
        static const std::map<char, std::string> primitives{{'Z', "jboolean"}, {'B', "jbyte"},  {'C', "jchar"},   {'S', "jshort"}, {'I', "jint"},
                                                            {'J', "jlong"},    {'F', "jfloat"}, {'D', "jdouble"}, {'V', "void"}};
        semantic::records::jni_parameter out{.descriptor = std::string{descriptor}};
        while (descriptor.starts_with('[')) {
            ++out.array_dimensions;
            descriptor.remove_prefix(1);
        }
        if (descriptor.empty())
            throw std::invalid_argument("incomplete JNI parameter descriptor");
        if (out.array_dimensions) {
            out.reference = true;
            out.element_descriptor = std::string{descriptor};
            const auto scalar = primitives.find(descriptor.front());
            out.native_type = out.array_dimensions == 1 && scalar != primitives.end() ? scalar->second + "Array" : "jobjectArray";
        } else if (const auto scalar = primitives.find(descriptor.front()); scalar != primitives.end())
            out.native_type = scalar->second;
        else {
            out.reference = true;
            out.native_type = descriptor == "Ljava/lang/String;"      ? "jstring"
                              : descriptor == "Ljava/lang/Class;"     ? "jclass"
                              : descriptor == "Ljava/lang/Throwable;" ? "jthrowable"
                                                                      : "jobject";
        }
        return out;
    }
    inline auto resolve(const semantic::recipe& recipe, const options& config, const std::vector<const semantic::shard*>& inputs,
                        const std::optional<group>& selected_group = {}) -> semantic::shard {
        semantic::shard out;
        out.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
        out.unit.cacheable = false;
        out.unit.coverage.available = {{.contexts = {out.unit.context}, .facts = {std::string{"relations"}, std::string{"diagnostics"}}}};
        struct registration {
            std::string klass, name, descriptor, function, table;
        };
        using binding::target;
        std::map<std::tuple<std::string, std::string, std::string, std::string>, std::vector<target>> registrations;
        binding::endpoints<to> ends{inputs, source_definition, [&](const target& defined) {
                                        if (const auto found = defined.symbol->extensions.find("native.jni_registrations");
                                            found != defined.symbol->extensions.end())
                                            for (const auto& method : json::from_value<std::vector<registration>>(found->second).value())
                                                registrations[{defined.symbol->module, method.klass, method.name, method.descriptor}].push_back(
                                                    {defined.shard, defined.symbol, defined.location, true});
                                    }};
        const auto& lookups = ends.lookups;
        const auto& exports = ends.exports;
        const auto& definitions = ends.definitions;
        for (const auto* shard : inputs)
            for (const auto& symbol : shard->symbols) {
                const bool source = source_declaration(symbol);
                if (!source && (symbol.domain != from::name || !std::ranges::contains(symbol.modifiers, "native")))
                    continue;
                if (selected_group &&
                    (shard->unit.recipe.unit.project != selected_group->project || symbol.module != selected_group->module || selected_group->source != source))
                    continue;
                const auto occurrences = lookups.at(shard).positions(symbol.id);
                const auto location = occurrences.empty() ? semantic::locator{} : occurrences.front()->location;
                // A source declaration has no descriptor: its short name binds
                // exactly, and descriptor-qualified long names are candidates.
                const auto klass = source ? source_class(symbol, location.source ? std::string_view{location.source->path} : std::string_view{})
                                          : property(symbol, "jvm.class");
                const bool is_static = std::ranges::contains(symbol.modifiers, "static") ||
                                       (source && (symbol.kind == semantic::symbol_kind::make<"function">() || symbol.extensions.contains("jvm.static")));
                std::vector<target> targets;
                const auto libraries = config.libraries.find(symbol.module);
                std::vector<std::string> spellings;
                if (!source)
                    spellings = names(symbol);
                else if (const auto escaped_class = escape(klass), method = escape(symbol.name); !klass.empty() && escaped_class && method)
                    spellings = {"Java_" + *escaped_class + "_" + *method};
                std::string entry;
                if (source && !spellings.empty()) {
                    if (auto found = definitions.find(spellings.front()); found != definitions.end())
                        targets = found->second;
                    else
                        for (auto it = definitions.lower_bound(spellings.front() + "__");
                             it != definitions.end() && it->first.starts_with(spellings.front() + "__"); ++it)
                            targets.insert(targets.end(), it->second.begin(), it->second.end());
                } else if (!source && libraries != config.libraries.end())
                    for (const auto& library : libraries->second)
                        if (const auto found = registrations.find({library, property(symbol, "jvm.class"), symbol.name, property(symbol, "jvm.descriptor")});
                            found != registrations.end())
                            targets.insert(targets.end(), found->second.begin(), found->second.end());
                // JNI searches the short name first, then the descriptor-qualified
                // long name when no explicit registration applies. Preserve all
                // matches at the selected rank, including registration ambiguity.
                for (const auto& spelling : spellings) {
                    if (source || !targets.empty())
                        break;
                    if (libraries != config.libraries.end())
                        for (const auto& library : libraries->second)
                            if (auto found = exports.find({library, spelling}); found != exports.end())
                                targets.insert(targets.end(), found->second.begin(), found->second.end());
                    if (!targets.empty()) {
                        entry = spelling;
                        break;
                    }
                }
                // Source binding is opportunistic: the native function may come
                // from a prebuilt library, so only ambiguity is reported.
                if (source && targets.empty())
                    continue;
                const semantic::ownership owner{out.unit.id, out.unit.context, {.provider = "jni", .kind = std::string{"resolver"}, .location = location}};
                const auto id = out.unit.id + ":" + symbol.id;
                semantic::records::library_selection selection{.name = symbol.module, .strategy = std::string{source ? "source" : "search"}};
                // A source definition has no artifact location to select.
                if (!source)
                    for (const auto& target : targets)
                        selection.candidates.push_back(target.location);
                if (targets.size() == 1) {
                    if (!source)
                        selection.selected = targets.front().location;
                } else if (!selected_group || selected_group->native.empty()) {
                    ++out.unit.coverage.unresolved_count;
                    out.diagnostics.push_back({.id = id,
                                               .producer = "jni",
                                               .code = spellings.empty()                                ? "name_unresolvable"
                                                       : !source && libraries == config.libraries.end() ? "library_unmapped"
                                                       : targets.empty()                                ? "entry_missing"
                                                                                                        : "entry_ambiguous",
                                               .severity = "warning",
                                               .message = "JNI method " + symbol.qualified_name + " requires one native export",
                                               .ownership = owner,
                                               .location = location});
                }
                for (const auto& target : targets) {
                    if (selected_group && (selected_group->native.empty() || target.shard->unit.id != selected_group->native))
                        continue;
                    ends.retain(out, *shard, symbol.id);
                    ends.retain(out, *target.shard, target.symbol->id);
                    if (target.registered)
                        selection.strategy = std::string{"dynamic"};
                    auto target_entry = target.registered || target.source ? property(*target.symbol, std::string{to::linkage}) : entry;
                    if (target.source && target_entry.empty())
                        target_entry = target.symbol->name;
                    semantic::records::binding_detail detail{.mechanism = std::string{target.registered ? "jni_dynamic" : "jni_static"},
                                                             .convention = std::string{"platform"},
                                                             .library_selection = selection,
                                                             .entry_point = std::move(target_entry)};
                    detail.parameter_mapping.push_back({{std::string{"environment"}}, {std::string{"parameter"}, 0}, std::string{"pointer"}});
                    detail.parameter_mapping.push_back(
                        {{std::string{is_static ? "class" : "receiver"}}, {std::string{"parameter"}, 1}, std::string{"pointer"}});
                    detail.parameter_mapping[0].jni = semantic::records::jni_parameter{.native_type = "JNIEnv*"};
                    detail.parameter_mapping[1].jni = parameter(is_static ? "Ljava/lang/Class;" : "L" + klass + ";");
                    const auto descriptor = property(symbol, "jvm.descriptor");
                    std::size_t offset = 1;
                    auto next = [&] {
                        const auto begin = offset;
                        while (offset < descriptor.size() && descriptor[offset] == '[')
                            ++offset;
                        if (offset >= descriptor.size())
                            throw std::invalid_argument("incomplete JNI descriptor");
                        if (descriptor[offset++] == 'L') {
                            const auto end = descriptor.find(';', offset);
                            if (end == std::string::npos)
                                throw std::invalid_argument("unterminated JNI descriptor");
                            offset = end + 1;
                        }
                        return parameter(std::string_view{descriptor}.substr(begin, offset - begin));
                    };
                    auto mapping = [&](semantic::records::jni_parameter value, semantic::records::parameter_slot from, semantic::records::parameter_slot to) {
                        if (value.native_type == "void")
                            return;
                        // Reference arguments are opaque JNI handles. In particular,
                        // arrays and strings are not automatically copied to C buffers.
                        detail.parameter_mapping.push_back({from, to, std::string{value.reference ? "pointer" : "identity"}, {}, std::move(value)});
                    };
                    if (!source) {
                        std::uint64_t at = 0;
                        while (offset < descriptor.size() && descriptor[offset] != ')') {
                            mapping(next(), {std::string{"parameter"}, at}, {std::string{"parameter"}, at + 2});
                            ++at;
                        }
                        ++offset;
                        mapping(next(), {std::string{"return_value"}}, {std::string{"return_value"}});
                    }
                    out.relations.push_back({.id = id + ":" + target.symbol->id,
                                             .kind = semantic::relation_kind::make<"binds">(),
                                             .ownership = owner,
                                             .from = symbol.id,
                                             .to = target.symbol->id,
                                             .resolution = std::string{targets.size() == 1 ? "resolved" : "candidate"},
                                             .detail = std::move(detail),
                                             .from_context = symbol.ownership.context,
                                             .to_context = target.symbol->ownership.context});
                }
            }
        return out;
    }
    struct mechanism {
        using options = jni::options;
        using group = jni::group;
        static auto grouped(const std::string& project, const semantic::records::symbol& symbol) -> std::optional<group> {
            if (source_declaration(symbol))
                return group{.project = project, .module = symbol.module, .source = true};
            if (symbol.domain == from::name && std::ranges::contains(symbol.modifiers, "native"))
                return group{project, symbol.module};
            return {};
        }
        static auto libraries(const options& config, const group& value) -> std::vector<std::string> {
            const auto found = config.libraries.find(value.module);
            return found == config.libraries.end() ? std::vector<std::string>{} : found->second;
        }
        static auto resolve(const semantic::recipe& recipe, const options& config, const std::vector<const semantic::shard*>& inputs, const group& selected) {
            return jni::resolve(recipe, config, inputs, selected);
        }
    };
    using resolver = binding::resolver<mechanism>;
} // namespace jni

export namespace language {
    template <> struct server<"jni"> : binds<jni::from, jni::to> {
        using adapter = jni::resolver;
        using options = jni::options;
        static void validate(const options& value) {
            for (const auto& [module, libraries] : value.libraries) {
                std::set<std::string> unique;
                if (module.empty())
                    throw std::invalid_argument("JNI requires a JVM module name");
                for (const auto& library : libraries)
                    if (library.empty() || !unique.insert(library).second)
                        throw std::invalid_argument("JNI library modules must be nonempty and unique");
            }
        }
        static auto launch(const options& value, const std::vector<builds::context>&) -> program {
            return {.provider = "jni", .initialization = json::to_value(value)};
        }
    };
    static_assert(bridge<server<"jni">>);
} // namespace language
