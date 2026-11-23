export module pybind;
import std;
import constants;
import channels;
import durability;
import coroutine;
import evaluation;
import json;
import records;
import objects;
import llvm;
export import bridge;

export namespace language {
    template <> struct domain<"python"> {
        static constexpr std::string_view name = "python", artifact = "python-runtime", linkage = "python.native_entry";
    };
}

namespace pybind {
    using from = language::domain<"python">;
    using to = language::domain<"native">;

    using entry = semantic::records::native_entry;
    struct member {
        std::string name, kind;
        json::value surface;
        std::optional<std::string> owner;
        std::optional<std::vector<std::string>> signatures;
        std::optional<std::string> wrapper, wrapper_library;
        std::optional<entry> native_entry;
        std::optional<std::vector<entry>> native_targets;
    };
    struct observed_target {
        entry native;
        semantic::records::call_dispatch dispatch;
    };
    struct observed_call {
        std::string member, path, caller;
        std::uint64_t first_line, begin, end;
        std::optional<std::vector<observed_target>> native_targets;
        std::optional<bool> unresolved_native_dispatch;
        std::optional<std::vector<entry>> native_candidates;
        std::optional<bool> native_candidates_complete;
    };
    struct observation {
        std::string path, function, role, slot, type_module, type_name;
        std::uint64_t declaration, begin, end;
    };
    struct report {
        std::string domain, module;
        std::vector<member> symbols;
        std::optional<std::string> library;
        std::optional<std::vector<observed_call>> calls;
        std::optional<std::vector<observation>> observations;
        std::optional<std::map<std::string, json::value>> observed_surfaces;
        std::optional<std::string> test_execution;
    };
    struct reader_options {
        std::optional<std::string> declarations;
    };
    struct reader_settings {
        std::string artifact;
        reader_options options;
    };

    struct source_declaration {
        semantic::identity_binding identity;
        semantic::context_id context;
        semantic::place location;
        std::string name, qualified_name;
        semantic::symbol_kind kind;
    };

    struct observation_sources {
        std::vector<source_declaration> declarations;
        std::map<semantic::symbol_id, const semantic::identity_binding*> bindings;

        void add(const semantic::shard& shard) {
            for (const auto& binding : shard.identities)
                bindings.try_emplace(binding.symbol, &binding);
            semantic::record_lookup lookup{shard};
            for (const auto& symbol : shard.symbols)
                if (symbol.language == "python")
                    if (const auto* identity = lookup.identity(symbol.id))
                        for (const auto* at : lookup.positions(symbol.id))
                            if (at->location.source && std::ranges::contains(at->roles, std::string{"definition"}))
                                declarations.push_back({*identity, symbol.ownership.context, at->selection.value_or(*at->location.source),
                                                        symbol.name, symbol.qualified_name, symbol.kind});
        }
    };

    void publish_observations(semantic::shard& out, const semantic::ownership& owner, const report& report,
                              const std::vector<source_declaration>& declarations,
                              const std::map<semantic::symbol_id, const semantic::identity_binding*>& bindings,
                              const std::map<std::string, std::string>& source_files) {
        auto& facts = out.unit.coverage.available.front().facts;
        facts.emplace_back(std::string{"typed_ranges"});
        facts.emplace_back(std::string{"types"});
        std::set<std::string> files;
        std::map<std::string, semantic::type_id> types;
        std::set<semantic::symbol_id> retained;
        auto retain = [&](const semantic::identity_binding& binding) {
            std::vector<const semantic::identity_binding*> chain;
            std::set<semantic::symbol_id> walking;
            for (const auto* current = &binding; !retained.contains(current->symbol);) {
                if (!walking.insert(current->symbol).second)
                    return false;
                chain.push_back(current);
                const auto* native = std::get_if<semantic::native_identity>(&current->declaration);
                if (!native || !native->owner)
                    break;
                const auto parent = bindings.find(*native->owner);
                if (parent == bindings.end())
                    return false;
                current = parent->second;
            }
            for (const auto* item : chain) {
                retained.insert(item->symbol);
                out.identities.push_back(*item);
            }
            return true;
        };
        std::set<std::tuple<semantic::symbol_id, semantic::context_id, std::string, std::uint64_t, std::uint64_t, semantic::type_id, std::string>> seen;
        for (const auto& sample : *report.observations) {
            if (sample.begin >= sample.end || sample.type_module.empty() || sample.type_name.empty() || !source_files.contains(sample.path) ||
                (sample.role != "parameter" && sample.role != "first_parameter" && sample.role != "return"))
                continue;
            const auto sep = sample.function.rfind('.');
            const auto name = sample.function.substr(sep == std::string::npos ? 0 : sep + 1);
            std::map<std::pair<semantic::symbol_id, semantic::context_id>, const source_declaration*> candidates;
            for (const auto& declared : declarations)
                if ((declared.kind == semantic::symbol_kind::make<"function">() || declared.kind == semantic::symbol_kind::make<"method">()) && declared.location.path == sample.path && declared.name == name &&
                    declared.location.begin <= sample.declaration && sample.declaration < declared.location.end)
                    candidates.try_emplace({declared.identity.symbol, declared.context}, &declared);
            if (candidates.size() != 1)
                continue;
            const auto& function = *candidates.begin()->second;
            if (!retain(function.identity))
                continue;
            if (files.insert(sample.path).second)
                out.unit.observed_dependencies.push_back({.kind = semantic::dependency_kind::source, .project = out.unit.recipe.unit.project,
                                                          .key = sample.path, .value = source_files.at(sample.path)});
            const auto qualified = sample.type_module + "." + sample.type_name;
            const auto [type, fresh] = types.try_emplace(qualified, out.unit.id + ":observed:" + qualified);
            if (fresh) {
                std::map<semantic::symbol_id, const source_declaration*> named;
                for (const auto& declared : declarations)
                    if (declared.kind == semantic::symbol_kind::make<"type">() && (declared.qualified_name == qualified ||
                        (sample.type_module == report.module && declared.location.path == sample.path && declared.name == sample.type_name)))
                        named.try_emplace(declared.identity.symbol, &declared);
                semantic::symbol_id id;
                if (named.size() == 1 && retain(named.begin()->second->identity)) {
                    const auto& cls = *named.begin()->second;
                    id = cls.identity.symbol;
                } else {
                    id = "python:observed:" + qualified;
                    semantic::native_identity identity{out.unit.recipe.unit.project, sample.type_module, out.unit.recipe.unit.provider, "python", "type/" + qualified};
                    out.identities.push_back({identity, id});
                    out.symbols.push_back({.id = id, .ownership = owner, .language = "python", .domain = "python", .module = sample.type_module,
                                           .name = sample.type_name, .qualified_name = qualified, .kind = "type", .type = type->second,
                                           .visibility = "public", .native_key = identity});
                }
                auto& nominal = out.types.emplace_back(semantic::records::type{
                    .id = type->second, .domain = "python", .kind = "named", .name = qualified, .declaration = std::move(id)});
                if (report.observed_surfaces)
                    if (const auto surface = report.observed_surfaces->find(qualified); surface != report.observed_surfaces->end())
                        nominal.extensions.emplace("python.surface", surface->second);
            }
            if (!seen.emplace(function.identity.symbol, function.context, sample.path, sample.begin, sample.end, type->second, sample.role).second)
                continue;
            out.typed_ranges.push_back({.ownership = owner, .range = {sample.path, sample.begin, sample.end}, .type = type->second,
                                        .confidence = "runtime_observed", .observation = semantic::records::observed_binding{
                                            function.identity.symbol, function.context, sample.role, sample.slot}});
        }
    }


    class reader : public language::reader_adapter {
    public:
        using reader_adapter::reader_adapter;

    protected:
        auto do_open(json::value initialization) -> coroutine::task<> override {
            settings_ = json::from_value<reader_settings>(initialization).value();
            co_return;
        }

        auto do_update(language::installation input, const std::vector<std::string>&) -> coroutine::task<> override {
            const auto found = input.model.find(settings_.artifact);
            if (found == input.model.end())
                throw std::invalid_argument("Python runtime reader requires a measured report");
            digest_ = found->second;
            installed_ = std::move(input);
            co_return;
        }

        auto do_units(const semantic::extent&) -> coroutine::task<std::vector<semantic::unit_key>> override {
            co_return std::vector<semantic::unit_key>{
                {state_.project, state_.stage, state_.provider, "evaluate", semantic::unit_kind::artifact, settings_.artifact}};
        }

        auto do_evaluate(const language::evaluation_request& request) -> coroutine::task<language::evaluated_unit> override {
            std::string bytes;
            for (auto part : durability::read_plain(objects_.path(digest_), constants::semantic_object_bytes))
                bytes.append(reinterpret_cast<const char*>(part.data()), part.size());
            const auto decoded = json::read<report>(bytes);
            if (!decoded || decoded->domain != "python" || decoded->module.empty())
                throw std::invalid_argument("invalid Python runtime report");

            semantic::recipe recipe{request.unit, state_.options};
            semantic::shard out;
            out.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
            out.unit.direct_inputs = {{semantic::input_kind::artifact, settings_.artifact, digest_}};
            out.unit.modules = {decoded->module};
            out.unit.coverage.requested = request.demand;
            out.unit.coverage.available.push_back({.contexts = {out.unit.context}, .facts = {std::string{"symbols"}, std::string{"definitions"}}});
            using declaration = std::tuple<semantic::identity_binding, semantic::context_id, semantic::locator>;
            std::map<std::string, std::map<std::pair<semantic::symbol_id, semantic::context_id>, declaration>> source;
            observation_sources observed_sources;
            out.unit.coverage.available.front().facts.emplace_back(std::string{"relations"});
            if (settings_.options.declarations || decoded->observations) {
                const auto port = installed_.source_facts.find("source_facts");
                if (decoded->observations && port == installed_.source_facts.end())
                    throw std::invalid_argument("Python test observations require the source_facts input");
                std::vector<const semantic::unit*> units;
                if (port != installed_.source_facts.end())
                    for (const auto& item : port->second) {
                        const auto& shard = *item->data;
                        units.push_back(&shard.unit);
                        if (shard.unit.recipe.unit.project != state_.project)
                            continue;
                        if (decoded->observations)
                            observed_sources.add(shard);
                        if (!settings_.options.declarations)
                            continue;
                        semantic::record_lookup lookup{shard};
                        for (const auto& symbol : shard.symbols) {
                            if (symbol.language != "python")
                                continue;
                            if (const auto* identity = lookup.identity(symbol.id))
                                for (const auto* at : lookup.positions(symbol.id)) {
                                    if (!at->location.source || !std::ranges::contains(at->roles, std::string{"definition"}))
                                        continue;
                                    if (!symbol.owner && settings_.options.declarations == at->location.source->path)
                                        source[symbol.name].try_emplace({symbol.id, symbol.ownership.context}, *identity, symbol.ownership.context,
                                                                        at->location);
                                }
                        }
                    }
                out.unit.observed_dependencies.push_back(channels::collection(state_.project, "source_facts", units,
                                                                               decoded->observations ? "declarations" : "linkage"));
            }
            out.unit.cacheable = true;
            const semantic::ownership owner{out.unit.id, out.unit.context, {.provider = "python-runtime", .kind = std::string{"artifact"}}};
            std::set<std::string> classes;
            for (const auto& row : decoded->symbols)
                if (row.kind == "type")
                    classes.insert(row.name);
            std::set<std::string> names;
            for (const auto& row : decoded->symbols) {
                if (row.name.empty() || !names.insert(row.name).second)
                    throw std::invalid_argument("Python runtime report has an empty or duplicate member name");
                auto name = row.name;
                if (row.kind == "method") {
                    if (!row.owner || !classes.contains(*row.owner) || !name.starts_with(*row.owner + ".") ||
                        name.size() <= row.owner->size() + 1)
                        throw std::invalid_argument("Python runtime method has no declared owner");
                    name.erase(0, row.owner->size() + 1);
                }
                const auto id = "python:" + row.name;
                const auto qualified = decoded->module + "." + row.name;
                semantic::native_identity identity{state_.project, decoded->module, state_.provider, "python", settings_.artifact + "/" + qualified};
                out.identities.push_back({identity, id});
                auto& symbol = out.symbols.emplace_back(semantic::records::symbol{.id = id,
                                                                                 .ownership = owner,
                                                                                 .language = "python",
                                                                                 .domain = "python",
                                                                                 .module = decoded->module,
                                                                                 .name = name,
                                                                                 .qualified_name = qualified,
                                                                                 .kind = row.kind == "callable" ? "function" : row.kind == "type" ? "class" :
                                                                                         row.kind == "method" ? "method" : "variable",
                                                                                 .owner = row.kind == "method" ? std::optional{"python:" + *row.owner} : std::nullopt,
                                                                                 .visibility = "public",
                                                                                 .native_key = identity});
                symbol.extensions.emplace("python.surface", row.surface);
                if (row.signatures && !row.signatures->empty())
                    symbol.extensions.emplace("python.signatures", json::to_value(*row.signatures));
                if (row.wrapper)
                    symbol.extensions.emplace("python.wrapper", *row.wrapper);
                if (row.native_entry && !row.wrapper) {
                    auto native = *row.native_entry;
                    native.library = std::filesystem::path{native.library}.filename().string();
                    symbol.extensions.emplace(std::string{from::linkage}, json::to_value(native));
                    symbol.extensions.emplace("python.library", native.library);
                } else if (row.wrapper) {
                    const auto& library = row.wrapper_library ? row.wrapper_library : decoded->library;
                    if (library)
                        symbol.extensions.emplace("python.library", std::filesystem::path{*library}.filename().string());
                    if (row.native_targets && !row.native_targets->empty()) {
                        auto targets = *row.native_targets;
                        std::erase_if(targets, [](const auto& target) { return !target.linkage_name || target.linkage_name->empty(); });
                        for (auto& target : targets)
                            target.library = std::filesystem::path{target.library}.filename().string();
                        std::ranges::sort(targets);
                        targets.erase(std::ranges::unique(targets).begin(), targets.end());
                        if (!targets.empty())
                            symbol.extensions.emplace("python.native_targets", json::to_value(targets));
                    }
                }
                out.occurrences.push_back({.id = out.unit.id + ":" + id,
                                           .ownership = owner,
                                           .symbol = id,
                                           .location = {.artifact = semantic::artifact_location{settings_.artifact, decoded->module, qualified}},
                                           .roles = {std::string{"definition"}}});
                if (const auto found = source.find(row.name); found != source.end() && found->second.size() == 1) {
                    const auto& [declaration, context, location] = found->second.begin()->second;
                    out.identities.push_back(declaration);
                    auto origin = owner;
                    origin.origin.location = location;
                    out.relations.push_back({.id = out.unit.id + ":emits:" + id,
                                             .kind = semantic::relation_kind::make<"emits">(),
                                             .ownership = std::move(origin),
                                             .from = declaration.symbol,
                                             .to = id,
                                             .resolution = std::string{"resolved"},
                                             .from_context = context,
                                             .to_context = out.unit.context});
                }
            }
            if (decoded->calls)
                publish_calls(out, owner, *decoded, names);
            if (decoded->observations)
                publish_observations(out, owner, *decoded, observed_sources.declarations, observed_sources.bindings, installed_.source_files);
            language::evaluated_unit result;
            result.shards.push_back(std::move(out));
            co_return result;
        }

    private:
        void publish_calls(semantic::shard& out, const semantic::ownership& owner, const report& report, const std::set<std::string>& members) const {
            out.unit.coverage.available.front().facts.emplace_back(std::string{"references"});
            std::set<std::string> callers, files, references;
            struct native_member {
                std::size_t index;
                bool wrapped;
                std::optional<entry> sole_target;
            };
            std::map<std::string, native_member> native_members;
            std::set<std::string> dispatches;
            for (std::size_t i = 0; i < out.symbols.size(); ++i) {
                const auto& symbol = out.symbols[i];
                native_member member{.index = i, .wrapped = symbol.extensions.contains("python.wrapper"), .sole_target = std::nullopt};
                if (member.wrapped) {
                    if (const auto found = symbol.extensions.find("python.native_targets"); found != symbol.extensions.end())
                        if (auto entries = json::from_value<std::vector<entry>>(found->second); entries && entries->size() == 1)
                            member.sole_target = std::move(entries->front());
                } else if (const auto found = symbol.extensions.find(std::string{from::linkage}); found != symbol.extensions.end()) {
                    if (auto decoded = json::from_value<entry>(found->second))
                        member.sole_target = std::move(*decoded);
                }
                if (member.wrapped || member.sole_target)
                    native_members.emplace(symbol.id, std::move(member));
            }
            for (const auto& call : *report.calls) {
                if (call.begin >= call.end || !members.contains(call.member) || !installed_.source_files.contains(call.path))
                    continue;
                const auto subject = call.path + "#" + std::to_string(call.first_line) + ":" + call.caller;
                const auto caller = "python:caller:" + subject;
                const auto target = "python:" + call.member;
                const auto reference = out.unit.id + ":call:" + subject + ":" + std::to_string(call.begin) + ":" + call.member;
                if (!references.insert(reference).second)
                    continue;
                std::map<std::string, semantic::records::call_dispatch> targets;
                const bool reported_unresolved = call.unresolved_native_dispatch.value_or(false);
                bool declared_target_observed = false;
                const auto member = native_members.find(target);
                const auto specialize = [&](const std::string& key, std::string_view kind, const std::vector<entry>& entries) {
                    const auto id = "python:" + std::string{kind} + ":" + key;
                    if (dispatches.insert(id).second) {
                        auto symbol = out.symbols[member->second.index];
                        auto identity = std::get<semantic::native_identity>(*symbol.native_key);
                        identity.native_key += "/" + std::string{kind} + "/" + key;
                        symbol.id = id;
                        symbol.native_key = identity;
                        if (kind == "dispatch") {
                            symbol.modifiers.push_back("runtime_observed");
                            symbol.extensions["python.library"] = entries.front().library;
                        }
                        symbol.extensions[kind == "dispatch" ? "python.observed_dispatch" : "python.candidate_dispatch"] = true;
                        symbol.extensions["python.native_targets"] = json::to_value(entries);
                        out.identities.push_back({identity, id});
                        out.symbols.push_back(std::move(symbol));
                    }
                    return id;
                };
                if (call.native_targets && member != native_members.end())
                    for (auto [native, dispatch] : *call.native_targets) {
                        native.library = std::filesystem::path{native.library}.filename().string();
                        if (native.library.empty())
                            continue;
                        if (!reported_unresolved && call.native_targets->size() == 1 && dispatch == "direct" && member->second.sole_target == native) {
                            targets.emplace(target, dispatch);
                            declared_target_observed = true;
                            continue;
                        }
                        if (!member->second.wrapped || !native.linkage_name || native.linkage_name->empty())
                            continue;
                        const auto key = json::write(std::tuple{call.member, native.library, *native.linkage_name});
                        const auto id = specialize(key, "dispatch", {native});
                        auto [position, inserted] = targets.try_emplace(id, dispatch);
                        if (!inserted && position->second != dispatch)
                            position->second = semantic::records::call_dispatch{"indirect"};
                    }
                const bool native_unresolved = reported_unresolved || (call.native_targets && targets.empty());
                auto candidates = call.native_candidates;
                auto complete = call.native_candidates_complete;
                auto unresolved_target = target;
                if (candidates) {
                    for (auto& candidate : *candidates)
                        candidate.library = std::filesystem::path{candidate.library}.filename().string();
                    if (std::erase_if(*candidates, [](const auto& candidate) { return candidate.library.empty(); }) || candidates->empty())
                        complete = false;
                    std::ranges::sort(*candidates);
                    candidates->erase(std::ranges::unique(*candidates).begin(), candidates->end());
                    if (native_unresolved && complete.value_or(false) && member != native_members.end() && member->second.wrapped)
                        unresolved_target = specialize(json::write(std::tuple{call.member, *candidates}), "candidates", *candidates);
                }
                if (targets.empty() || native_unresolved)
                    targets.emplace(unresolved_target, semantic::records::call_dispatch{"direct"});
                if (files.insert(call.path).second)
                    out.unit.observed_dependencies.push_back({.kind = semantic::dependency_kind::source,
                                                              .project = state_.project,
                                                              .key = call.path,
                                                              .value = installed_.source_files.at(call.path)});
                const semantic::locator location{.source = semantic::place{call.path, call.begin, call.end}};
                auto origin = owner;
                origin.origin.kind = std::string{"source"};
                origin.origin.location = location;
                if (callers.insert(caller).second) {
                    semantic::native_identity identity{state_.project, report.module, state_.provider, "python", "caller/" + subject};
                    out.identities.push_back({identity, caller});
                    out.symbols.push_back({.id = caller,
                                           .ownership = owner,
                                           .language = "python",
                                           .domain = "python",
                                           .module = report.module,
                                           .name = call.caller,
                                           .qualified_name = call.caller,
                                           .kind = call.caller == "<module>" ? "module" : "function",
                                           .visibility = "internal",
                                           .native_key = identity});
                }
                out.occurrences.push_back({.id = reference + ":occurrence",
                                           .ownership = origin,
                                           .symbol = caller,
                                           .location = location,
                                           .roles = {std::string{"call"}}});
                for (const auto& [selected, dispatch] : targets) {
                    const bool unresolved = native_unresolved && selected == unresolved_target;
                    const auto call_id = selected != target && !unresolved ? reference + ":" + selected : reference;
                    std::optional<semantic::records::relation_detail> detail;
                    if (unresolved)
                        detail = semantic::records::native_call_detail{.evidence = std::string{"unresolved"},
                                                                       .candidates = std::move(candidates), .candidates_complete = complete};
                    else if (selected != target || declared_target_observed)
                        detail = semantic::records::native_call_detail{.evidence = std::string{"observed"}, .dispatch = dispatch};
                    out.references.push_back({.id = call_id,
                                              .ownership = origin,
                                              .from = caller,
                                              .location = location,
                                              .target = unresolved ? target : selected,
                                              .roles = {std::string{"call"}},
                                              .resolution = std::string{"resolved"},
                                              .dispatch = dispatch,
                                              .target_context = unresolved && selected != target ? std::optional{owner.context} : std::nullopt});
                    out.relations.push_back({.id = call_id,
                                             .kind = semantic::relation_kind::make<"calls">(),
                                             .ownership = origin,
                                             .from = caller,
                                             .to = selected,
                                             .reference = call_id,
                                             .resolution = std::string{"resolved"},
                                             .detail = std::move(detail)});
                }
            }
        }

        reader_settings settings_;
        std::string digest_;
    };

    struct group {
        std::string project, module, library, native;
        std::vector<std::string> target_libraries;
        bool source = false;
        auto operator<=>(const group&) const = default;
    };

    struct options {
        // Runtime library filename -> native-artifact module configured in the analysis graph.
        std::map<std::string, std::string> libraries;
        // Python qualified name -> exact native linkage name for a wrapped entry.
        std::map<std::string, std::string> entries;
    };

    struct mechanism {
        using group = pybind::group;
        using options = pybind::options;
        static constexpr auto native_channel = "linkage";

        static auto captures(const semantic::records::symbol& symbol) -> std::vector<entry> {
            if (const auto found = symbol.extensions.find("python.native_targets"); found != symbol.extensions.end())
                if (const auto values = json::from_value<std::vector<entry>>(found->second); values)
                    return *values;
            return {};
        }

        static auto grouped(const std::string& project, const semantic::records::symbol& symbol) -> std::optional<group> {
            if (symbol.domain != from::name || !symbol.extensions.contains("python.library"))
                return {};
            auto result = group{.project = project, .module = symbol.module, .library = binding::property(symbol, "python.library")};
            for (const auto& target : captures(symbol))
                result.target_libraries.push_back(target.library);
            std::ranges::sort(result.target_libraries);
            result.target_libraries.erase(std::ranges::unique(result.target_libraries).begin(), result.target_libraries.end());
            return result;
        }
        static auto libraries(const options& config, const group& value) -> std::vector<std::string> {
            std::set<std::string> modules;
            if (const auto found = config.libraries.find(value.library); found != config.libraries.end())
                modules.insert(found->second);
            for (const auto& library : value.target_libraries)
                if (const auto found = config.libraries.find(library); found != config.libraries.end())
                    modules.insert(found->second);
            return {modules.begin(), modules.end()};
        }
        static auto resolve(const semantic::recipe& recipe, const options& config, const std::vector<const semantic::shard*>& inputs, const group& selected)
            -> semantic::shard {
            semantic::shard out;
            out.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
            out.unit.coverage.available.push_back({.contexts = {out.unit.context}, .facts = {std::string{"relations"}, std::string{"diagnostics"}}});
            const semantic::ownership owner{out.unit.id, out.unit.context, {.provider = "pybind", .kind = std::string{"resolver"}}};
            const auto modules = libraries(config, selected);
            std::vector<binding::target> definitions;
            binding::endpoints<to> ends{inputs, [](const auto&) { return false; }, [&](const binding::target& target) {
                                            if (target.symbol->kind == "function" && std::ranges::contains(modules, target.symbol->module))
                                                definitions.push_back(target);
                                        }};
            for (const auto* shard : inputs) {
                if (shard->unit.recipe.unit.project != selected.project)
                    continue;
                for (const auto& symbol : shard->symbols) {
                    auto group = grouped(selected.project, symbol);
                    if (!group || group->module != selected.module || group->library != selected.library ||
                        group->target_libraries != selected.target_libraries)
                        continue;
                    const bool candidate_dispatch = symbol.extensions.contains("python.candidate_dispatch");
                    const auto publish = [&](const std::string& library_name, const entry* native, const std::string* configured,
                                             const std::string& id) {
                        const auto module = config.libraries.find(library_name);
                        auto matches = [&](const binding::target& target) {
                            const auto& location = target.location;
                            if (module == config.libraries.end() || target.symbol->module != module->second)
                                return false;
                            if (configured)
                                return location.member == *configured;
                            return native && location.offset == native->offset && (!native->linkage_name || location.member == *native->linkage_name);
                        };
                        auto targets = definitions | std::views::filter(matches);
                        const auto count = std::ranges::distance(targets);
                        if (count != 1 && selected.native.empty()) {
                            ++out.unit.coverage.unresolved_count;
                            out.diagnostics.push_back({.id = id,
                                                       .producer = "pybind",
                                                       .code = count ? "entry_ambiguous" : "entry_missing",
                                                       .severity = "warning",
                                                       .message = "Python binding " + symbol.qualified_name + " requires one native definition",
                                                       .ownership = owner});
                        }
                        for (const auto& target : targets) {
                            if (target.shard->unit.id != selected.native)
                                continue;
                            ends.retain(out, *shard, symbol.id);
                            ends.retain(out, *target.shard, target.symbol->id);
                            semantic::records::library_selection library{.name = library_name, .strategy = std::string{"exact"}};
                            for (const auto& candidate : targets)
                                library.candidates.push_back(candidate.location);
                            if (count == 1)
                                library.selected = target.location;
                            semantic::records::binding_detail detail{.mechanism = std::string{"python"},
                                                                     .convention = std::string{"platform"},
                                                                     .library_selection = std::move(library),
                                                                     .entry_point = target.location.member};
                            out.relations.push_back({.id = id + ":" + target.symbol->id,
                                                     .kind = semantic::relation_kind::make<"binds">(),
                                                     .ownership = owner,
                                                     .from = symbol.id,
                                                     .to = target.symbol->id,
                                                     .resolution = std::string{count == 1 && !candidate_dispatch ? "resolved" : "candidate"},
                                                     .detail = std::move(detail),
                                                     .from_context = symbol.ownership.context,
                                                     .to_context = target.symbol->ownership.context});
                        }
                    };
                    const auto id = out.unit.id + ":" + symbol.id;
                    auto captured = captures(symbol);
                    if (const auto configured = config.entries.find(symbol.qualified_name);
                        configured != config.entries.end() && !candidate_dispatch && !symbol.extensions.contains("python.observed_dispatch")) {
                        publish(selected.library, nullptr, &configured->second, id);
                    } else if (!captured.empty()) {
                        for (std::size_t i = 0; i < captured.size(); ++i)
                            publish(captured[i].library, &captured[i], nullptr, id + ":" + std::to_string(i));
                    } else {
                        std::optional<entry> native;
                        if (const auto found = symbol.extensions.find(std::string{from::linkage}); found != symbol.extensions.end())
                            if (const auto value = json::from_value<entry>(found->second); value)
                                native = *value;
                        publish(selected.library, native ? &*native : nullptr, nullptr, id);
                    }
                }
            }
            return out;
        }
    };
}

export namespace language {
    auto project_python_test(std::string_view bytes, const std::string& project, const std::string& name,
                             const std::string& digest, const std::map<std::string, std::string>& files,
                             std::span<const std::shared_ptr<const semantic::shard>> shards) -> std::optional<semantic::shard> {
        const auto report = json::read<pybind::report>(bytes);
        if (!report || report->domain != "python" || report->module.empty() || !report->observations)
            throw std::invalid_argument("invalid Python test observation report");
        pybind::observation_sources source;
        for (const auto& shard : shards)
            source.add(*shard);
        semantic::shard out;
        semantic::recipe recipe{{project, "python-test-observations", "python-runtime", "evaluate", semantic::unit_kind::artifact, name}, "test-observations"};
        out.unit = semantic::make_unit(recipe, semantic::context_identifier(recipe));
        out.unit.direct_inputs = {{semantic::input_kind::artifact, name, digest}};
        out.unit.modules = {report->module};
        out.unit.coverage.available.push_back({.contexts = {out.unit.context}, .facts = {std::string{"symbols"}}});
        const semantic::ownership owner{out.unit.id, out.unit.context, {.provider = "python-runtime", .kind = std::string{"artifact"}}};
        pybind::publish_observations(out, owner, *report, source.declarations, source.bindings, files);
        if (out.typed_ranges.empty())
            return {};
        return out;
    }

    void validate_python_test_report(std::string_view bytes, const std::string& execution) {
        const auto report = json::read<pybind::report>(bytes);
        if (!report || report->domain != "python" || report->module.empty() || !report->observations || report->test_execution != execution)
            throw std::invalid_argument("invalid Python test observation report");
    }

    template <> struct server<"python-runtime"> : reads<domain<"python">> {
        using adapter = pybind::reader;
        using options = pybind::reader_options;
        static constexpr std::array<std::string_view, 6> facts{"symbols", "definitions", "references", "relations", "types", "typed_ranges"};
        static auto channels() -> std::vector<semantic::channel_definition> {
            return {linkage_channel(), {.name = "relations", .projections = {std::string{"relations"}}},
                    {.name = "observations", .projections = {std::string{"types"}, std::string{"typed_ranges"}}}};
        }
        static auto requirements(const options&) -> builds::requirements { return {}; }
        static auto executes_project_code(const options&) -> bool { return false; }
        static auto required_inputs(const options& value) -> std::vector<std::string> {
            return value.declarations ? std::vector<std::string>{"artifact", "source_facts"} : std::vector<std::string>{"artifact"};
        }
        static void validate(const options& value) {
            if (value.declarations)
                builds::relative(*value.declarations, false);
        }
        static auto launch(const options& value, const std::vector<builds::context>& inputs) -> program {
            if (inputs.size() != 1 || inputs.front().type != "python-runtime" || inputs.front().kind != "file")
                throw std::invalid_argument("Python runtime reader requires one report file");
            return {.provider = "python-runtime", .initialization = json::to_value(pybind::reader_settings{inputs.front().path, value})};
        }
    };
    template <> struct server<"pybind"> : binds<domain<"python">, domain<"native">> {
        using adapter = binding::resolver<pybind::mechanism>;
        using options = pybind::options;
        static auto consumed_facts(std::string_view) -> std::vector<semantic::fact_kind> { return {std::string{"symbols"}, std::string{"definitions"}}; }
        static void validate(const options& value) {
            for (const auto* mappings : {&value.libraries, &value.entries})
                for (const auto& [name, target] : *mappings)
                    if (name.empty() || target.empty())
                        throw std::invalid_argument("Python bindings require nonempty mapping keys and values");
        }
        static auto launch(const options& value, const std::vector<builds::context>&) -> program {
            return {.provider = "pybind", .initialization = json::to_value(value)};
        }
    };
    static_assert(domain_reader<server<"python-runtime">>);
    static_assert(bridge<server<"pybind">>);
}
