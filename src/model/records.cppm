export module records;
import std;
import reflection;
import content;

// Language data refers to the immutable snapshot analyzed by the worker.
export namespace semantic {
    // Where characters are in one text: a path and Unicode scalar offsets [begin, end).
    struct place {
        std::string path;
        std::uint64_t begin = 0, end = 0;
        auto operator<=>(const place&) const = default;
    };
    // IDs are opaque within a workspace. A context selects one interpretation of a declaration.
    using snapshot_id = std::string;
    using context_id = std::string;
    using symbol_id = std::string;
    using type_id = std::string;
    using unit_id = std::string;
    using shard_id = std::string;
    using view_id = std::string;
    using candidate_id = std::string;
    struct retained_objects {
        std::set<view_id> views;
        std::set<candidate_id> candidates;
        std::set<shard_id> shards;
    };
    using retention_roots = std::shared_ptr<retained_objects>;
    using slot_id = std::string;

    using symbol_kind = rfl::Literal<"other", "module", "namespace", "type", "function", "method", "constructor", "destructor", "variable", "parameter",
                                     "field", "property", "event", "enum_member", "macro", "label", "concept", "attribute", "operator">;
    using scope_kind = rfl::Literal<"other", "translation_unit", "module", "namespace", "type", "function", "block", "loop", "if", "switch", "catch", "lambda",
                                    "initializer", "template">;
    using role = rfl::Literal<"declaration", "definition", "reference", "call", "import", "implicit", "type", "qualifier", "read", "write">;
    using fact_kind = rfl::Literal<"symbols", "types", "definitions", "references", "scopes", "diagnostics", "relations", "occurrences", "typed_ranges">;
    using relation_kind = rfl::Literal<"calls", "overrides", "aliases", "inherits", "implements", "emits", "binds", "imports", "instantiates">;
    using query_action = rfl::Literal<"symbols", "definitions", "references", "scopes", "diagnostics", "inspect", "relations", "impact">;

    template <rfl::internal::StringLiteral... Names> auto tool_actions(rfl::Literal<Names...>*) -> rfl::Literal<Names..., "plan", "describe", "schema">;
    using tool_action = decltype(tool_actions(static_cast<query_action*>(nullptr)));

    enum class unit_kind { file, module, artifact, group };
    struct unit_key {
        std::string project, stage, provider, operation;
        unit_kind kind = unit_kind::file;
        std::string name;
        auto operator<=>(const unit_key&) const = default;
    };
    struct recipe {
        unit_key unit;
        // Fingerprint of the effective stage configuration and its build configuration.
        std::string options;
        auto operator<=>(const recipe&) const = default;
    };
    inline auto unit_identifier(const unit_key& key) -> unit_id {
        if (key.project.empty() || key.stage.empty() || key.provider.empty() || key.operation.empty() || key.name.empty())
            throw std::invalid_argument("a semantic unit requires project, stage, provider, operation and name");
        return "unit-" + content::hash_of(rfl::json::write(key)).str();
    }
    inline auto recipe_identifier(const recipe& value) -> std::string { return "recipe-" + content::hash_of(rfl::json::write(value)).str(); }

    struct source_anchor {
        std::string file, declaration;
        std::optional<symbol_id> owner;
        auto operator<=>(const source_anchor&) const = default;
    };
    struct native_identity {
        std::string project, module, provider, domain, native_key;
        // Relative keys (parameters, accessors, constructors) name their owner
        // directly; publication binds it before looking up this identity.
        std::optional<symbol_id> owner;
        auto operator<=>(const native_identity&) const = default;
    };
    struct anchored_identity {
        std::string project, module, provider;
        source_anchor anchor;
        auto operator<=>(const anchored_identity&) const = default;
    };
    // native_key must be recoverable across provider sessions; artifact keys include their
    // resolution context and artifact module. Local declarations use source anchors and owners.
    // Display names, offsets, overload order and process pointers never enter these keys.
    using declaration_identity = std::variant<native_identity, anchored_identity>;
    struct identity_binding {
        declaration_identity declaration;
        symbol_id symbol;
        std::optional<source_anchor> source;
    };

    struct named {
        std::string text;
        std::optional<std::string> module;
        std::optional<symbol_kind> kind;
        std::optional<std::string> path;
        bool operator==(const named&) const = default;
    };
    struct entity {
        symbol_id symbol_id;
        std::optional<context_id> context;
        bool operator==(const entity&) const = default;
    };
    struct selection {
        std::optional<named> name;
        std::optional<entity> entity;
        bool operator==(const selection&) const = default;
    };
    inline void validate(const selection& selected) {
        if (selected.name.has_value() == selected.entity.has_value())
            throw std::invalid_argument("selector requires exactly one of name or entity");
        if (selected.name && selected.name->text.empty())
            throw std::invalid_argument("selector.name requires text");
        if (selected.entity && selected.entity->symbol_id.empty())
            throw std::invalid_argument("selector.entity requires symbol_id");
    }

    struct snapshot {
        std::string project, worldline, version;
        snapshot_id snapshot;
    };
    using snapshot_vector = std::vector<snapshot>;
    using source_vector = std::map<std::string, std::map<std::string, std::string>>;
    inline auto snapshot_for(const snapshot_vector& snapshots, std::string_view project) -> const snapshot& {
        const auto found = std::ranges::find(snapshots, project, &snapshot::project);
        if (found == snapshots.end())
            throw std::invalid_argument("project is absent from the snapshot vector: " + std::string{project});
        return *found;
    }
    inline void normalize(snapshot_vector& snapshots) {
        if (snapshots.empty())
            throw std::invalid_argument("a semantic view requires pinned snapshots");
        std::ranges::sort(snapshots, {}, &snapshot::project);
        for (std::size_t i = 0; i < snapshots.size(); ++i) {
            const auto& pin = snapshots[i];
            if (pin.project.empty() || pin.version.empty() || pin.snapshot.empty())
                throw std::invalid_argument("a semantic snapshot requires project, version and snapshot");
            if (i && snapshots[i - 1].project == pin.project)
                throw std::invalid_argument("a snapshot vector pins each project once");
        }
    }
    struct extent {
        std::vector<std::string> modules, files, contexts;
        std::vector<fact_kind> facts;
        std::vector<std::string> projects;
        std::optional<semantic::selection> selector;
        friend auto operator==(const extent&, const extent&) -> bool = default;
    };
    struct missing {
        extent scope;
        std::vector<unit_id> units;
        std::string reason;
        rfl::Literal<"analysis", "query"> phase;
        bool deferred = false;
    };
    struct coverage {
        extent requested;
        std::vector<extent> available;
        std::vector<missing> missing;
        std::uint64_t unresolved_count = 0;
    };
    struct artifact_input {
        std::string project, stage, path;
        auto operator<=>(const artifact_input&) const = default;
    };
    struct artifact_location {
        std::string artifact, module, member;
        std::optional<std::uint64_t> offset;
        std::optional<artifact_input> input;
        auto operator<=>(const artifact_location&) const = default;
    };
    struct coordinates {
        std::uint64_t line = 0, column = 0, end_line = 0, end_column = 0;
        auto operator<=>(const coordinates&) const = default;
    };
    struct locator {
        std::optional<place> source;
        std::optional<artifact_location> artifact;
        std::optional<place> spelling, expansion;
        std::optional<std::string> project;
        std::optional<coordinates> position;
        auto operator<=>(const locator&) const = default;
    };
    struct origin {
        std::string provider;
        rfl::Literal<"unknown", "source", "artifact", "debug", "generated", "metadata", "resolver"> kind;
        std::optional<locator> location;
        std::optional<std::string> reference, occurrence, producer;
    };
    struct ownership {
        unit_id owner_unit;
        context_id context;
        semantic::origin origin;
    };
    using extensions = std::map<std::string, rfl::Generic>;

    // A published view selects immutable unit contributions.
    namespace records {
        struct parameter {
            type_id type;
            std::vector<std::string> modifiers;
        };
        struct parameter_group {
            std::vector<parameter> parameters;
        };
        struct parameter_detail {
            std::uint64_t group = 0, index = 0;
            std::optional<std::string> name, default_value;
            extensions annotations;
        };
        struct application_argument {
            rfl::Literal<"type", "integral", "declaration", "null_pointer", "template", "template_expansion", "pack", "expression"> kind;
            std::string value;
            std::optional<type_id> type;
            std::optional<symbol_id> declaration;
            std::vector<application_argument> elements;
        };
        struct type {
            type_id id;
            std::string domain;
            rfl::Literal<"primitive", "named", "parameter", "application", "function", "tuple", "array", "pointer", "reference", "nullable", "intersection",
                         "alias", "flexible", "unresolved", "error">
                kind;
            std::optional<std::string> name;
            std::optional<symbol_id> declaration, binder;
            std::optional<std::uint64_t> parameter_index, array_size;
            std::vector<type_id> arguments, constraints, elements, type_parameters;
            std::optional<type_id> receiver, return_type, lower_bound, upper_bound;
            std::vector<parameter_group> parameter_groups;
            std::vector<std::string> qualifiers;
            std::vector<application_argument> application_arguments;
            extensions extensions;
        };
        struct symbol {
            symbol_id id;
            ownership ownership;
            std::string language, domain, module, name, qualified_name;
            symbol_kind kind;
            std::optional<symbol_id> owner;
            std::optional<std::string> declaring_scope;
            std::optional<type_id> type;
            std::string visibility;
            std::vector<std::string> modifiers;
            std::vector<parameter_detail> parameters;
            std::optional<declaration_identity> native_key;
            extensions extensions;
        };
        struct occurrence {
            std::string id;
            ownership ownership;
            symbol_id symbol;
            locator location;
            std::vector<role> roles;
            std::optional<place> selection, body;
            std::optional<symbol_kind> declaration_kind;
        };
        struct reference {
            std::string id;
            ownership ownership;
            std::optional<symbol_id> from;
            locator location;
            std::optional<symbol_id> target;
            std::vector<symbol_id> candidates;
            std::vector<role> roles;
            rfl::Literal<"resolved", "ambiguous", "unresolved", "error"> resolution;
            std::optional<rfl::Literal<"direct", "virtual", "interface", "indirect">> dispatch;
            // This is the declaration endpoint. ownership.context belongs to the use.
            std::optional<context_id> target_context;
        };
        struct scope_edge {
            // Targets are declaration IDs; their scopes supply lookup members.
            rfl::Literal<"base", "using_namespace", "using_declaration", "using_enum", "inline_namespace", "anonymous_namespace", "unscoped_enum", "import">
                kind;
            std::string target;
            // Import lookup can expose a renamed member or every member of its target.
            std::optional<std::string> name;
            std::optional<bool> wildcard;
        };
        struct scope {
            std::string id;
            ownership ownership;
            std::optional<std::string> lexical_parent;
            std::optional<symbol_id> semantic_owner;
            locator location;
            std::vector<symbol_id> declared_members;
            std::vector<scope_edge> lookup;
            scope_kind kind;
            std::optional<std::string> reference;
        };
        struct parameter_slot {
            rfl::Literal<"parameter", "receiver", "class", "environment", "return_value"> kind;
            std::optional<std::uint64_t> index;
        };
        struct marshalling_descriptor {
            std::uint32_t native_type = 0;
            std::string name;
            std::vector<std::uint32_t> data;
            std::optional<std::uint32_t> element_type, size_parameter, size_constant;
            std::optional<std::string> marshaler, cookie;
        };
        using character_set = rfl::Literal<"none", "ansi", "unicode", "auto">;
        struct layout_field {
            std::string name, type;
            std::optional<std::uint64_t> offset;
            std::optional<marshalling_descriptor> native;
        };
        struct structure_layout {
            rfl::Literal<"auto", "sequential", "explicit"> kind;
            character_set charset;
            std::uint64_t packing = 0, size = 0;
            std::vector<layout_field> fields;
        };
        struct parameter_marshalling {
            std::string managed_type;
            rfl::Literal<"default", "in", "out", "in_out"> direction;
            bool optional = false, by_reference = false;
            std::optional<marshalling_descriptor> native;
            std::optional<structure_layout> layout;
        };
        struct import_marshalling {
            character_set charset;
            bool exact_spelling = false, set_last_error = false;
            std::optional<bool> best_fit_mapping, throw_on_unmappable_character;
        };
        struct jni_parameter {
            std::string native_type, descriptor;
            bool reference = false;
            std::uint64_t array_dimensions = 0;
            std::optional<std::string> element_descriptor;
        };
        struct parameter_mapping {
            parameter_slot from, to;
            rfl::Literal<"identity", "pointer", "string", "array", "custom"> conversion;
            std::optional<parameter_marshalling> marshalling;
            std::optional<jni_parameter> jni;
        };
        struct library_selection {
            std::string name;
            rfl::Literal<"exact", "search", "dynamic"> strategy;
            std::vector<artifact_location> candidates;
            std::optional<artifact_location> selected;
        };
        struct binding_detail {
            rfl::Literal<"binding"> kind;
            rfl::Literal<"jni_static", "jni_dynamic", "pinvoke"> mechanism;
            rfl::Literal<"platform", "cdecl", "stdcall", "fastcall", "thiscall", "vectorcall"> convention;
            std::vector<records::parameter_mapping> parameter_mapping;
            records::library_selection library_selection;
            std::optional<std::string> entry_point;
            std::optional<import_marshalling> marshalling;
        };
        struct emission_detail {
            rfl::Literal<"emission"> kind;
            rfl::Literal<"debug_location", "metadata_signature", "compilation_mapping", "metadata_accessor"> evidence;
            std::string file;
            std::uint64_t line = 0;
            std::optional<std::uint64_t> inline_depth;
        };
        using relation_detail = rfl::TaggedUnion<"kind", binding_detail, emission_detail>;
        struct relation {
            std::string id;
            relation_kind kind;
            ownership ownership;
            symbol_id from, to;
            std::optional<std::string> reference, occurrence;
            rfl::Literal<"resolved", "candidate", "unresolved"> resolution;
            std::optional<relation_detail> detail;
            std::optional<context_id> from_context, to_context;
        };
        struct diagnostic {
            std::string id, producer, code, severity, message;
            ownership ownership;
            locator location;
            std::vector<locator> related_locations;
            extensions fixes;
        };
        struct typed_range {
            ownership ownership;
            place range;
            type_id type;
            std::optional<symbol_id> overload;
            std::map<type_id, type_id> substitutions;
        };
    } // namespace records

    enum class input_kind { source, file, artifact, sdk, generated, project_model, runtime, tool, channel, provider };
    struct input_object {
        input_kind kind = input_kind::file;
        std::string name, object;
        // Namespaced input contract, required only for kind=provider (e.g. clice/configuration).
        std::string type;
        friend auto operator==(const input_object&, const input_object&) -> bool = default;
    };
    using input_vector = std::map<std::string, std::vector<input_object>>;
    struct file_set {
        // Project-relative file names and recursive directory selections in a source manifest.
        std::vector<std::string> files, directories;
        friend auto operator==(const file_set&, const file_set&) -> bool = default;
    };
    struct record_selection {
        // Selection within a typed fact port. Empty dimensions include every member.
        std::vector<std::string> projects, modules, units;
        auto operator<=>(const record_selection&) const = default;
    };
    enum class dependency_kind { source, declaration, lookup, negative_lookup, constant, body, project_model, generated, artifact, enumeration, record_set };
    struct dependency {
        dependency_kind kind = dependency_kind::source;
        std::string project, key, value;
        std::optional<unit_id> producer;
        std::optional<std::string> channel, scope, name;
        std::vector<std::string> search_path;
        std::optional<file_set> selection;
        // Complete producer membership for record_set reads.
        std::vector<unit_id> producers;
        std::optional<record_selection> records;
        friend auto operator==(const dependency&, const dependency&) -> bool = default;
    };
    struct channel_definition {
        std::string name;
        std::vector<rfl::Literal<"symbols", "types", "occurrences", "references", "scopes", "relations", "diagnostics", "typed_ranges", "definition_bodies",
                                 "declarations">>
            projections;
        rfl::Literal<"canonical_records"> digest;
        std::vector<symbol_kind> symbol_kinds;
        std::optional<std::vector<std::string>> symbol_extensions;
        friend auto operator==(const channel_definition&, const channel_definition&) -> bool = default;
    };
    struct channel {
        std::string name, value;
        std::vector<shard_id> shards;
    };
    struct contribution {
        std::vector<std::string> files, occurrences, references, scopes, relations, diagnostics;
        std::vector<symbol_id> symbols;
        std::vector<type_id> types;
        friend auto operator==(const contribution&, const contribution&) -> bool = default;
    };
    struct unit_resolution {
        rfl::Literal<"definitions", "references"> action;
        semantic::selection selector;
        std::vector<entity> targets;
    };
    struct unit {
        unit_id id;
        context_id context;
        semantic::recipe recipe;
        std::vector<input_object> direct_inputs;
        std::vector<dependency> observed_dependencies;
        std::vector<channel_definition> channel_definitions;
        std::vector<channel> output_channels;
        contribution contributions;
        semantic::coverage coverage;
        std::optional<unit_resolution> resolution;
        bool cacheable = true;
        // Complete module membership of all shards belonging to this unit.
        std::vector<std::string> modules;
    };
    inline auto context_identifier(const semantic::recipe& recipe, std::string partition = {}) -> context_id {
        return "context-" + content::hash_of(rfl::json::write(std::tuple{recipe.unit.project, recipe.unit.stage, recipe.unit.provider, recipe.options,
                                                                         std::move(partition)}))
                                .str();
    }
    inline auto make_unit(semantic::recipe recipe, context_id context) -> unit {
        auto id = unit_identifier(recipe.unit);
        return {.id = std::move(id), .context = std::move(context), .recipe = std::move(recipe)};
    }
    struct candidate {
        candidate_id id;
        semantic::recipe recipe;
        snapshot_vector snapshots;
        std::vector<shard_id> shards;
        std::int64_t accessed = 0;
        std::vector<semantic::unit> units;
        std::uint64_t nodes = 0;
        bool cacheable = true;
        bool retained = false;
    };
    struct shard {
        shard_id id;
        semantic::unit unit;
        std::vector<identity_binding> identities;
        std::vector<records::symbol> symbols;
        std::vector<records::type> types;
        std::vector<records::occurrence> occurrences;
        std::vector<records::reference> references;
        std::vector<records::scope> scopes;
        std::vector<records::relation> relations;
        std::vector<records::diagnostic> diagnostics;
        std::vector<records::typed_range> typed_ranges;
    };
    inline auto contributions(const semantic::shard& shard) -> semantic::contribution {
        semantic::contribution out{.files = shard.unit.contributions.files};
        auto collect = [](const auto& rows, auto& ids) {
            for (const auto& row : rows)
                ids.push_back(row.id);
        };
        collect(shard.symbols, out.symbols);
        collect(shard.types, out.types);
        collect(shard.occurrences, out.occurrences);
        collect(shard.references, out.references);
        collect(shard.scopes, out.scopes);
        collect(shard.relations, out.relations);
        collect(shard.diagnostics, out.diagnostics);
        return out;
    }
    inline void contributions(semantic::shard& shard) { shard.unit.contributions = contributions(std::as_const(shard)); }

    struct budget {
        std::optional<std::uint64_t> milliseconds, units, nodes;
    };
    inline auto tighten(budget value, const budget& ceiling) -> budget {
        auto cap = [](auto& into, const auto& from) {
            if (from)
                into = into ? std::min(*into, *from) : *from;
        };
        cap(value.milliseconds, ceiling.milliseconds);
        cap(value.units, ceiling.units);
        cap(value.nodes, ceiling.nodes);
        return value;
    }
    struct query_defaults {
        std::uint64_t fact_limit = 50, relation_depth = 1;
        semantic::budget budget;
    };
    struct resolution_target {
        semantic::named name;
        std::optional<place> occurrence;
    };
    struct request {
        std::optional<std::string> project, worldline, version, configuration;
        std::optional<view_id> view, from_view, to_view;
        query_action action;
        std::optional<selection> selector;
        std::optional<resolution_target> target;
        extent scope;
        std::vector<std::string> include, relations, changes;
        rfl::Literal<"outgoing", "incoming", "both"> direction;
        std::uint64_t depth = 1, limit = 50;
        std::optional<std::string> cursor, continuation;
        semantic::budget budget;
    };
    inline auto resolution_identifier(const request& request) -> std::string {
        return content::hash_of(rfl::json::write(std::tuple{request.action.str(), request.selector, request.scope})).str();
    }
    struct item {
        symbol_id symbol_id;
        context_id context;
        std::string language, domain, module, name;
        symbol_kind kind;
        std::optional<type_id> type_id;
        std::vector<locator> declarations, definitions;
    };
    struct continuation {
        view_id view;
        std::string cursor;
    };

    struct impact_step {
        entity from, to;
        rfl::Literal<"relation", "reference"> kind;
        std::string evidence;
    };
    struct impact_path {
        view_id view;
        rfl::Literal<"semantic", "behavior"> category;
        entity seed, target;
        std::vector<impact_step> steps;
        bool tentative = false;
    };
    struct view_change {
        entity subject;
        std::vector<rfl::Literal<"added", "removed", "declaration", "metadata", "relations", "inputs">> kinds;
    };
    struct impact_evidence {
        view_id view;
        snapshot_vector snapshots;
        std::vector<item> items;
        std::vector<records::relation> relations;
        std::vector<records::reference> references;
        std::vector<records::occurrence> occurrences;
        std::vector<records::symbol> symbols;
        std::vector<records::type> types;
        std::vector<records::typed_range> typed_ranges;
        std::vector<records::scope> scopes;
        std::vector<records::diagnostic> diagnostics;
    };
    struct input_delta {
        input_kind kind;
        std::string name, type;
        std::vector<std::string> before, after;
    };
    struct dependency_delta {
        dependency read;
        std::vector<std::string> before, after;
        std::vector<unit_id> producers;
        bool unavailable = false;
    };
    struct impact_task {
        std::string project, configuration, task, operation;
    };
    struct reanalysis_change {
        unit_id id;
        unit_key unit;
        std::vector<input_delta> inputs;
        std::vector<dependency_delta> dependencies;
        std::vector<impact_task> tasks;
    };
    struct impact_comparison {
        view_id from_view, to_view;
        std::vector<view_change> changes;
        std::vector<impact_evidence> evidence;
        std::vector<reanalysis_change> reanalysis;
    };
    struct result {
        std::optional<view_id> view;
        snapshot_vector snapshots;
        rfl::Literal<"complete", "partial", "failed", "ambiguous", "expired"> status;
        semantic::coverage coverage;
        std::vector<item> items;
        std::vector<records::symbol> symbols;
        std::vector<records::type> types;
        std::vector<records::typed_range> typed_ranges;
        std::vector<records::occurrence> occurrences;
        std::vector<records::reference> references;
        std::vector<records::scope> scopes;
        std::vector<records::relation> relations;
        std::vector<records::diagnostic> diagnostics;
        std::vector<impact_path> impact;
        std::optional<impact_comparison> comparison;
        std::uint64_t query_nodes = 0;
        std::optional<std::string> cursor, continuation;
        std::optional<view_id> next_view;
        std::optional<std::string> code, message;
    };

    inline auto required_facts(query_action action, const std::vector<std::string>& include = {}) -> std::vector<fact_kind> {
        using A = query_action;
        using F = fact_kind;
        static const std::map<A, std::vector<F>> actions{{A::make<"symbols">(), {F::make<"symbols">()}},
                                                         {A::make<"definitions">(), {F::make<"definitions">()}},
                                                         {A::make<"references">(), {F::make<"references">()}},
                                                         {A::make<"scopes">(), {F::make<"scopes">()}},
                                                         {A::make<"diagnostics">(), {F::make<"diagnostics">()}},
                                                         {A::make<"inspect">(), {F::make<"symbols">(), F::make<"definitions">(), F::make<"types">()}},
                                                         {A::make<"relations">(), {F::make<"relations">()}},
                                                         {A::make<"impact">(), {F::make<"symbols">(), F::make<"references">(), F::make<"relations">()}}};
        using projection = rfl::Literal<"type", "scope", "declarations", "definitions", "references", "diagnostics", "relations">;
        static const std::map<projection, F> projections{{projection::make<"type">(), F::make<"types">()},
                                                         {projection::make<"scope">(), F::make<"scopes">()},
                                                         {projection::make<"declarations">(), F::make<"symbols">()},
                                                         {projection::make<"definitions">(), F::make<"definitions">()},
                                                         {projection::make<"references">(), F::make<"references">()},
                                                         {projection::make<"diagnostics">(), F::make<"diagnostics">()},
                                                         {projection::make<"relations">(), F::make<"relations">()}};
        auto facts = actions.at(action);
        for (const auto& included : include) {
            const auto next = projections.at(projection{included});
            if (!std::ranges::contains(facts, next))
                facts.push_back(next);
        }
        return facts;
    }

} // namespace semantic
