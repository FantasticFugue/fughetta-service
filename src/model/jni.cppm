export module jni;
import std;
import evaluation;
import coroutine;
import reflection;
import records;
import channels;
import objects;
export import provider;

export namespace jni {
    class resolver;
    struct group {
        std::string project, module, native;
        auto operator<=>(const group&) const = default;
    };
    struct options {
        // JVM artifact module -> native modules in its class loader. Never use
        // the worker host's library search path to infer target loading.
        std::map<std::string, std::vector<std::string>> libraries;
    };
    inline auto property(const semantic::records::symbol& symbol, const std::string& key) -> std::string {
        auto found = symbol.extensions.find(key);
        return found == symbol.extensions.end() ? "" : rfl::from_generic<std::string>(found->second).value();
    }
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
        struct target {
            const semantic::shard* shard;
            const semantic::records::symbol* symbol;
            semantic::artifact_location location;
            bool registered = false;
        };
        struct registration {
            std::string klass, name, descriptor, function, table;
        };
        std::unordered_map<const semantic::shard*, semantic::record_lookup> lookups;
        for (const auto* shard : inputs)
            lookups.try_emplace(shard, *shard);
        std::map<std::pair<std::string, std::string>, std::vector<target>> exports;
        std::map<std::tuple<std::string, std::string, std::string, std::string>, std::vector<target>> registrations;
        for (const auto* shard : inputs)
            for (const auto& symbol : shard->symbols) {
                if (symbol.domain != "native")
                    continue;
                for (const auto* occurrence : lookups.at(shard).positions(symbol.id))
                    if (occurrence->location.artifact && std::ranges::contains(occurrence->roles, std::string{"definition"})) {
                        if (std::ranges::contains(symbol.modifiers, "export"))
                            exports[{symbol.module, property(symbol, "native.linkage_name")}].push_back({shard, &symbol, *occurrence->location.artifact});
                        if (const auto found = symbol.extensions.find("native.jni_registrations"); found != symbol.extensions.end())
                            for (const auto& method : rfl::from_generic<std::vector<registration>>(found->second).value())
                                registrations[{symbol.module, method.klass, method.name, method.descriptor}].push_back(
                                    {shard, &symbol, *occurrence->location.artifact, true});
                    }
            }
        std::set<std::string> retained;
        auto retain = [&](const semantic::shard& shard, const std::string& id) {
            if (!retained.insert(id).second)
                return;
            const auto* found = lookups.at(&shard).identity(id);
            if (!found)
                throw std::invalid_argument("JNI endpoint has no declaration identity");
            out.identities.push_back(*found);
        };
        for (const auto* shard : inputs)
            for (const auto& symbol : shard->symbols) {
                if (symbol.domain != "jvm" || !std::ranges::contains(symbol.modifiers, "native"))
                    continue;
                if (selected_group && (shard->unit.recipe.unit.project != selected_group->project || symbol.module != selected_group->module))
                    continue;
                std::vector<target> targets;
                const auto libraries = config.libraries.find(symbol.module);
                const auto spellings = names(symbol);
                std::string entry;
                if (libraries != config.libraries.end())
                    for (const auto& library : libraries->second)
                        if (const auto found = registrations.find({library, property(symbol, "jvm.class"), symbol.name, property(symbol, "jvm.descriptor")});
                            found != registrations.end())
                            targets.insert(targets.end(), found->second.begin(), found->second.end());
                // JNI searches the short name first, then the descriptor-qualified
                // long name when no explicit registration applies. Preserve all
                // matches at the selected rank, including registration ambiguity.
                for (const auto& spelling : spellings) {
                    if (!targets.empty())
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
                const auto occurrences = lookups.at(shard).positions(symbol.id);
                const auto location = occurrences.empty() ? semantic::locator{} : occurrences.front()->location;
                const semantic::ownership owner{out.unit.id, out.unit.context, {.provider = "jni", .kind = std::string{"resolver"}, .location = location}};
                const auto id = out.unit.id + ":" + symbol.id;
                semantic::records::library_selection selection{.name = symbol.module, .strategy = std::string{"search"}};
                for (const auto& target : targets)
                    selection.candidates.push_back(target.location);
                if (targets.size() == 1)
                    selection.selected = targets.front().location;
                else if (!selected_group || selected_group->native.empty()) {
                    ++out.unit.coverage.unresolved_count;
                    out.diagnostics.push_back({.id = id,
                                               .producer = "jni",
                                               .code = spellings.empty()                     ? "name_unresolvable"
                                                       : libraries == config.libraries.end() ? "library_unmapped"
                                                       : targets.empty()                     ? "entry_missing"
                                                                                             : "entry_ambiguous",
                                               .severity = "warning",
                                               .message = "JNI method " + symbol.qualified_name + " requires one native export",
                                               .ownership = owner,
                                               .location = location});
                }
                for (const auto& target : targets) {
                    if (selected_group && (selected_group->native.empty() || target.shard->unit.id != selected_group->native))
                        continue;
                    retain(*shard, symbol.id);
                    retain(*target.shard, target.symbol->id);
                    if (target.registered)
                        selection.strategy = std::string{"dynamic"};
                    semantic::records::binding_detail detail{.mechanism = std::string{target.registered ? "jni_dynamic" : "jni_static"},
                                                             .convention = std::string{"platform"},
                                                             .library_selection = selection,
                                                             .entry_point = target.registered ? property(*target.symbol, "native.linkage_name") : entry};
                    detail.parameter_mapping.push_back({{std::string{"environment"}}, {std::string{"parameter"}, 0}, std::string{"pointer"}});
                    detail.parameter_mapping.push_back({{std::string{std::ranges::contains(symbol.modifiers, "static") ? "class" : "receiver"}},
                                                        {std::string{"parameter"}, 1},
                                                        std::string{"pointer"}});
                    detail.parameter_mapping[0].jni = semantic::records::jni_parameter{.native_type = "JNIEnv*"};
                    detail.parameter_mapping[1].jni =
                        parameter(std::ranges::contains(symbol.modifiers, "static") ? "Ljava/lang/Class;" : "L" + property(symbol, "jvm.class") + ";");
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
                    std::uint64_t at = 0;
                    while (offset < descriptor.size() && descriptor[offset] != ')') {
                        mapping(next(), {std::string{"parameter"}, at}, {std::string{"parameter"}, at + 2});
                        ++at;
                    }
                    ++offset;
                    mapping(next(), {std::string{"return_value"}}, {std::string{"return_value"}});
                    out.relations.push_back({.id = id + ":" + target.symbol->id,
                                             .kind = std::string{"binds"},
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
} // namespace jni

export namespace language {
    template <> struct server<"jni"> {
        using adapter = jni::resolver;
        using options = jni::options;
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 0> documents{};
        static constexpr std::array<std::string_view, 2> inputs{"managed", "native"}, facts{"relations", "diagnostics"};
        static constexpr std::array<std::string_view, 1> operations{"evaluate"}, roles{"relation-resolver"};
        static constexpr semantic::unit_kind unit_kind = semantic::unit_kind::group;
        static constexpr bool negotiated = false, multiple_inputs = true;
        static auto input_type(std::string_view) -> std::string_view { return "semantic_shard"; }
        static auto consumed_facts(std::string_view port) -> std::vector<semantic::fact_kind> {
            std::vector<semantic::fact_kind> out{std::string{"symbols"}, std::string{"definitions"}};
            if (port == "managed")
                out.push_back(std::string{"types"});
            return out;
        }
        static auto channels() -> std::vector<semantic::channel_definition> {
            return {{.name = "bindings", .projections = {std::string{"relations"}, std::string{"diagnostics"}}}};
        }
        static auto requirements(const options&) -> builds::requirements { return {}; }
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
            return {.provider = "jni", .initialization = rfl::to_generic(value)};
        }
    };
} // namespace language

export namespace jni {
    class resolver : public language::reader_adapter {
    public:
        using language::reader_adapter::reader_adapter;

    protected:
        auto do_open(rfl::Generic value) -> coroutine::task<> override {
            config_ = rfl::from_generic<options>(value).value();
            co_return;
        }
        auto do_units(const semantic::extent&) -> coroutine::task<std::vector<semantic::unit_key>> override {
            std::set<group> groups;
            if (const auto managed = installed_.source_facts.find("managed"); managed != installed_.source_facts.end())
                for (const auto& object : managed->second)
                    for (const auto& symbol : object->data->symbols)
                        if (symbol.domain == "jvm" && std::ranges::contains(symbol.modifiers, "native"))
                            groups.insert({object->data->unit.recipe.unit.project, symbol.module});
            const auto managed_groups = groups;
            for (const auto& group : managed_groups) {
                semantic::record_selection native;
                if (const auto found = config_.libraries.find(group.module); found != config_.libraries.end())
                    native.modules = found->second;
                if (native.modules.empty())
                    continue;
                if (const auto objects = installed_.source_facts.find("native"); objects != installed_.source_facts.end())
                    for (const auto& object : objects->second)
                        if (channels::selects(object->data->unit, native)) {
                            auto selected = group;
                            selected.native = object->data->unit.id;
                            groups.insert(std::move(selected));
                        }
            }
            std::vector<semantic::unit_key> out;
            for (const auto& group : groups)
                out.push_back({state_.project, state_.stage, state_.provider, "evaluate", semantic::unit_kind::group, rfl::json::write(group)});
            co_return out;
        }
        auto do_evaluate(const language::evaluation_request& request) -> coroutine::task<language::evaluated_unit> override {
            const auto selected = rfl::json::read<group>(request.unit.name).value();
            semantic::record_selection managed{.projects = {selected.project}, .modules = {selected.module}};
            semantic::record_selection native;
            if (const auto libraries = config_.libraries.find(selected.module); libraries != config_.libraries.end())
                native.modules = libraries->second;
            std::vector<const semantic::shard*> shards;
            std::vector<semantic::dependency> reads;
            std::vector<const semantic::unit*> native_units;
            for (const std::string port : {"managed", "native"}) {
                if (port == "native" && native.modules.empty())
                    continue;
                const auto& selection = port == "managed" ? managed : native;
                std::vector<const semantic::unit*> units;
                if (const auto objects = installed_.source_facts.find(port); objects != installed_.source_facts.end())
                    for (const auto& object : objects->second) {
                        units.push_back(&object->data->unit);
                        if (channels::selects(object->data->unit, selection))
                            shards.push_back(object->data.get());
                    }
                // Keep the complete selected membership, including an empty
                // lookup. A new export in a configured library must invalidate it.
                reads.push_back(channels::collection(state_.project, port, units, port == "native" ? "exports" : "linkage", selection));
                if (port == "native")
                    native_units = std::move(units);
            }
            auto shard = jni::resolve({request.unit, state_.options}, config_, shards, selected);
            if (!selected.native.empty()) {
                auto selected_library = native;
                // Ambiguous bindings expose every candidate's location, so they
                // depend on all those libraries. Resolved contributions only
                // read their selected library's addresses and record bodies.
                if (std::ranges::none_of(shard.relations, [](const auto& row) { return row.resolution == "candidate"; }))
                    selected_library.units = {selected.native};
                reads.push_back(channels::collection(state_.project, "native", native_units, "linkage", selected_library));
            }
            shard.unit.modules = {selected.module};
            shard.unit.observed_dependencies = std::move(reads);
            shard.unit.direct_inputs.push_back({semantic::input_kind::runtime, "toolchain", runtime_.execution});
            shard.unit.cacheable = !runtime_.execution.empty();
            shard.unit.coverage.requested = request.demand;
            language::evaluated_unit result;
            result.shards.push_back(std::move(shard));
            co_return result;
        }

    private:
        options config_;
    };
} // namespace jni
