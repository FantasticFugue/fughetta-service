export module pinvoke;
import std;
import evaluation;
import coroutine;
import json;
import records;
import channels;
import objects;
import clr;
import llvm;
export import bridge;

export namespace pinvoke {
    using binding::property;
    using from = language::domain<"clr">;
    using to = language::domain<"native">;
    struct group {
        std::string project, module, library, native;
        // Managed source declarations bound to native source definitions before any artifact exists.
        bool source = false;
        auto operator<=>(const group&) const = default;
    };
    struct options {
        // DLL import name -> native artifact module. Selection is explicit;
        // target OS loader search paths do not come from the worker host.
        std::map<std::string, std::string> libraries;
    };
    // A declaration a frontend marks as imported, before its assembly exists.
    inline auto source_declaration(const semantic::records::symbol& symbol) {
        return symbol.domain != from::name && std::ranges::contains(symbol.modifiers, "pinvoke");
    }
    inline auto source_definition(const semantic::records::symbol& symbol) {
        if (symbol.domain != "source" || symbol.kind != semantic::symbol_kind::make<"function">())
            return false;
        if (symbol.language == "cpp")
            return symbol.qualified_name == symbol.name || symbol.qualified_name == "::" + symbol.name || !property(symbol, "native.linkage_name").empty();
        return symbol.language == "rust" && (!property(symbol, "native.linkage_name").empty() || std::ranges::contains(symbol.modifiers, "no_mangle") ||
                                             std::ranges::contains(symbol.modifiers, "export_name"));
    }
    inline auto resolve(const semantic::recipe& recipe, const options& config, const std::vector<const semantic::shard*>& inputs,
                        const std::optional<group>& selected_group = {}) -> semantic::shard {
        semantic::shard out;
        out.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
        // The adapter supplies collection reads and a runtime identity before
        // allowing reuse. Direct projection callers do not have installed ports.
        out.unit.cacheable = false;
        out.unit.coverage.available = {{.contexts = {out.unit.context}, .facts = {std::string{"relations"}, std::string{"diagnostics"}}}};
        using binding::target;
        binding::endpoints<to> ends{inputs, source_definition};
        const auto& lookups = ends.lookups;
        const auto& exports = ends.exports;
        const auto& definitions = ends.definitions;
        for (const auto* shard : inputs)
            for (const auto& symbol : shard->symbols) {
                const bool source = source_declaration(symbol);
                if (!source && (symbol.domain != from::name || !std::ranges::contains(symbol.modifiers, "pinvoke")))
                    continue;
                if (selected_group && (shard->unit.recipe.unit.project != selected_group->project || symbol.module != selected_group->module ||
                                       property(symbol, "clr.pinvoke.library") != selected_group->library || selected_group->source != source))
                    continue;
                const auto library = property(symbol, "clr.pinvoke.library"), entry = property(symbol, "clr.pinvoke.entry_point");
                const auto selected = config.libraries.find(library);
                semantic::records::library_selection selection{.name = library, .strategy = std::string{source ? "source" : "exact"}};
                std::vector<target> targets;
                if (source) {
                    if (auto found = definitions.find(entry); found != definitions.end())
                        targets = found->second;
                } else if (selected != config.libraries.end()) {
                    if (auto found = exports.find({selected->second, entry}); found != exports.end())
                        targets = found->second;
                }
                // A source definition has no artifact location to select.
                if (!source)
                    for (const auto& target : targets)
                        selection.candidates.push_back(target.location);
                // Source binding is opportunistic: the native function may come
                // from a prebuilt library, so only ambiguity is reported.
                if (source && targets.empty())
                    continue;
                const auto occurrences = lookups.at(shard).positions(symbol.id);
                const auto location = occurrences.empty() ? semantic::locator{} : occurrences.front()->location;
                const semantic::ownership owner{out.unit.id, out.unit.context, {.provider = "pinvoke", .kind = std::string{"resolver"}, .location = location}};
                const auto id = out.unit.id + ":" + symbol.id;
                if (targets.size() == 1 && !source)
                    selection.selected = targets.front().location;
                if (targets.size() != 1 && (!selected_group || selected_group->native.empty())) {
                    ++out.unit.coverage.unresolved_count;
                    out.diagnostics.push_back({.id = id,
                                               .producer = "pinvoke",
                                               .code = !source && selected == config.libraries.end() ? "library_unmapped"
                                                       : targets.empty()                             ? "entry_missing"
                                                                                                     : "entry_ambiguous",
                                               .severity = "warning",
                                               .message = "P/Invoke " + library + "!" + entry + " requires one native export",
                                               .ownership = owner,
                                               .location = location});
                }
                for (const auto& target : targets) {
                    if (selected_group && (selected_group->native.empty() || target.shard->unit.id != selected_group->native))
                        continue;
                    ends.retain(out, *shard, symbol.id);
                    ends.retain(out, *target.shard, target.symbol->id);
                    semantic::records::binding_detail detail{
                        .mechanism = std::string{"pinvoke"},
                        .convention = decltype(semantic::records::binding_detail::convention){property(symbol, "clr.pinvoke.convention")},
                        .library_selection = selection,
                        .entry_point = entry};
                    if (const auto policy = symbol.extensions.find("clr.pinvoke.marshalling"); policy != symbol.extensions.end())
                        detail.marshalling = json::from_value<semantic::records::import_marshalling>(policy->second).value();
                    struct metadata {
                        std::uint32_t sequence = 0;
                        semantic::records::parameter_marshalling value;
                    };
                    std::map<std::uint32_t, semantic::records::parameter_marshalling> parameters;
                    if (const auto found = symbol.extensions.find("clr.pinvoke.parameters"); found != symbol.extensions.end()) {
                        auto decoded = json::from_value<std::vector<metadata>>(found->second).value();
                        for (auto& parameter : decoded)
                            parameters.emplace(parameter.sequence, std::move(parameter.value));
                    }
                    if (symbol.type) {
                        const auto* signature = lookups.at(shard).type(*symbol.type);
                        if (signature) {
                            auto mapping = [&](const std::string& type, semantic::records::parameter_slot slot, std::uint32_t sequence) {
                                static const std::set<std::string> scalars{"System.SByte",  "System.Byte",   "System.Int16",  "System.UInt16",
                                                                           "System.Int32",  "System.UInt32", "System.Int64",  "System.UInt64",
                                                                           "System.Single", "System.Double", "System.IntPtr", "System.UIntPtr"};
                                const auto* value = lookups.at(shard).type(type);
                                const auto name = value ? value->name.value_or("") : "";
                                if (slot.kind == "return_value" && name == "System.Void")
                                    return;
                                semantic::records::parameter_mapping result{slot, slot, std::string{scalars.contains(name) ? "identity" : "custom"}};
                                if (const auto found = parameters.find(sequence); found != parameters.end()) {
                                    result.marshalling = found->second;
                                    const auto& metadata = found->second;
                                    if (metadata.native) {
                                        const auto& native = metadata.native->name;
                                        static const std::set<std::string> strings{"BStr",      "LPStr",    "LPWStr", "LPTStr",
                                                                                   "ByValTStr", "AnsiBStr", "TBStr",  "LPUTF8Str"};
                                        static const std::set<std::string> arrays{"LPArray", "SafeArray", "ByValArray"};
                                        // Preserve MarshalAs exactly. Only matching
                                        // scalar representations can be identity;
                                        // narrowing, bools and custom marshalers
                                        // remain explicit marshaling operations.
                                        static const std::map<std::string, std::string> scalar_native{
                                            {"System.SByte", "I1"},  {"System.Byte", "U1"},   {"System.Int16", "I2"},      {"System.UInt16", "U2"},
                                            {"System.Int32", "I4"},  {"System.UInt32", "U4"}, {"System.Int64", "I8"},      {"System.UInt64", "U8"},
                                            {"System.Single", "R4"}, {"System.Double", "R8"}, {"System.IntPtr", "SysInt"}, {"System.UIntPtr", "SysUInt"}};
                                        const auto scalar = scalar_native.find(metadata.managed_type);
                                        result.conversion = std::string{strings.contains(native)                                    ? "string"
                                                                        : arrays.contains(native)                                   ? "array"
                                                                        : scalar != scalar_native.end() && scalar->second == native ? "identity"
                                                                                                                                    : "custom"};
                                    } else if (metadata.layout)
                                        result.conversion = std::string{"custom"};
                                    else if (metadata.by_reference || (value && value->kind == "pointer"))
                                        result.conversion = std::string{"pointer"};
                                    else if (metadata.managed_type == "System.String")
                                        result.conversion = std::string{"string"};
                                    else if (value && value->kind == "array")
                                        result.conversion = std::string{"array"};
                                }
                                detail.parameter_mapping.push_back(std::move(result));
                            };
                            std::uint64_t index = 0;
                            for (const auto& group : signature->parameter_groups)
                                for (const auto& parameter : group.parameters) {
                                    mapping(parameter.type, {.kind = std::string{"parameter"}, .index = index}, static_cast<std::uint32_t>(index + 1));
                                    ++index;
                                }
                            if (signature->return_type)
                                mapping(*signature->return_type, {.kind = std::string{"return_value"}}, 0);
                        }
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
        using options = pinvoke::options;
        using group = pinvoke::group;
        static auto grouped(const std::string& project, const semantic::records::symbol& symbol) -> std::optional<group> {
            if (!std::ranges::contains(symbol.modifiers, "pinvoke"))
                return {};
            return group{
                .project = project, .module = symbol.module, .library = property(symbol, "clr.pinvoke.library"), .source = symbol.domain != from::name};
        }
        static auto libraries(const options& config, const group& value) -> std::vector<std::string> {
            const auto found = config.libraries.find(value.library);
            return found == config.libraries.end() ? std::vector<std::string>{} : std::vector<std::string>{found->second};
        }
        static auto resolve(const semantic::recipe& recipe, const options& config, const std::vector<const semantic::shard*>& inputs, const group& selected) {
            return pinvoke::resolve(recipe, config, inputs, selected);
        }
    };
    using resolver = binding::resolver<mechanism>;
} // namespace pinvoke

export namespace language {
    template <> struct server<"pinvoke"> : binds<pinvoke::from, pinvoke::to> {
        using adapter = pinvoke::resolver;
        using options = pinvoke::options;
        static void validate(const options& value) {
            for (const auto& [library, module] : value.libraries)
                if (library.empty() || module.empty())
                    throw std::invalid_argument("P/Invoke library mappings require names and modules");
        }
        static auto launch(const options& value, const std::vector<builds::context>&) -> program {
            return {.provider = "pinvoke", .initialization = json::to_value(value)};
        }
    };
    static_assert(bridge<server<"pinvoke">>);
} // namespace language
