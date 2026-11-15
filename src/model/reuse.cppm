export module reuse;
import std;
import records;
import provider;
import enumeration;

export namespace reuse {
    struct key {
        semantic::candidate_id candidate;
        std::string recipe;
        friend auto operator==(const key&, const key&) -> bool = default;
    };
    using channel_values = std::map<std::pair<semantic::unit_id, std::string>, std::string>;
    using collection_key = std::tuple<std::string, std::string, semantic::record_selection>;
    using collection_values = std::map<collection_key, std::string>;
    struct environment {
        const semantic::source_vector& sources;
        const semantic::input_vector& inputs;
        const channel_values& channels;
        std::string_view project;
        std::span<const language::input_validator> validators;
        collection_values collections;

        auto files(std::string_view project) const -> const std::map<std::string, std::string>* {
            auto found = sources.find(std::string{project});
            return found == sources.end() ? nullptr : &found->second;
        }
        auto installed(std::string_view owner) const -> const std::vector<semantic::input_object>& {
            static const std::vector<semantic::input_object> empty;
            auto found = inputs.find(std::string{owner.empty() ? project : owner});
            return found == inputs.end() ? empty : found->second;
        }
        auto input(std::string_view owner, semantic::input_kind kind, std::string_view name, std::string_view value) const -> bool {
            return !value.empty() &&
                   std::ranges::any_of(installed(owner), [&](const auto& input) { return input.kind == kind && input.name == name && input.object == value; });
        }
        auto file(std::string_view project, const std::string& path, std::string_view value) const -> bool {
            auto selected = files(project);
            if (!selected)
                return false;
            auto found = selected->find(path);
            return found != selected->end() && found->second == value;
        }
    };
    inline auto source(const semantic::dependency& read, const environment& current) -> bool {
        return current.file(read.project.empty() ? current.project : read.project, read.key, read.value);
    }
    inline auto collection(const semantic::dependency& read, const environment& current) -> bool {
        auto files = current.files(read.project.empty() ? current.project : read.project);
        return files && !read.scope && read.selection && read.value == enumeration::value(*files, *read.selection);
    }
    inline auto object(const semantic::dependency& read, const environment& current) -> bool {
        if (!read.scope)
            return false;
        return current.input(read.project, semantic::input_kind::artifact, *read.scope, read.value) ||
               current.input(read.project, semantic::input_kind::sdk, *read.scope, read.value) ||
               current.input(read.project, semantic::input_kind::generated, *read.scope, read.value);
    }
    inline auto absent_inputs(const semantic::dependency& read, const environment& current) -> bool {
        const auto prefix = read.key.ends_with('/') ? read.key : read.key + "/";
        return std::ranges::none_of(current.installed(read.project), [&](const auto& input) {
            return (input.kind == semantic::input_kind::artifact || input.kind == semantic::input_kind::generated) &&
                   (input.name == read.key || input.name.starts_with(prefix));
        });
    }
    inline auto absent(const semantic::dependency& read, const environment& current) -> bool {
        if (read.scope)
            return object(read, current);
        if (!read.value.empty())
            return false;
        auto files = current.files(read.project.empty() ? current.project : read.project);
        if (!files || files->contains(read.key))
            return false;
        const auto prefix = read.key.ends_with('/') ? read.key : read.key + "/";
        auto found = files->lower_bound(prefix);
        if (found != files->end() && found->first.starts_with(prefix))
            return false;
        return absent_inputs(read, current);
    }
    inline auto channel(const semantic::dependency& read, const environment& current) -> bool {
        if (!read.channel || read.value.empty())
            return false;
        // A frontend may expose a semantic value from its installed compiler
        // session without publishing a separate upstream stage contribution.
        if (!read.producer)
            return current.input(read.project, semantic::input_kind::channel, read.key, read.value);
        auto found = current.channels.find({*read.producer, *read.channel});
        return found != current.channels.end() && found->second == read.value;
    }
    inline auto model(const semantic::dependency& read, const environment& current) -> bool {
        return channel(read, current) || current.input(read.project, semantic::input_kind::project_model, read.key, read.value);
    }
    inline auto record_set(const semantic::dependency& read, const environment& current) -> bool {
        if (!read.channel || read.value.empty() || (!read.project.empty() && read.project != current.project))
            return false;
        const auto found = current.collections.find({read.key, *read.channel, read.records.value_or(semantic::record_selection{})});
        return found != current.collections.end() && found->second == read.value;
    }
    inline auto dependency(const semantic::dependency& read, const environment& current) -> bool {
        using validator = bool (*)(const semantic::dependency&, const environment&);
        static const std::map<semantic::dependency_kind, validator> validators{
            {semantic::dependency_kind::source, source},          {semantic::dependency_kind::enumeration, collection},
            {semantic::dependency_kind::artifact, object},        {semantic::dependency_kind::generated, object},
            {semantic::dependency_kind::negative_lookup, absent}, {semantic::dependency_kind::declaration, channel},
            {semantic::dependency_kind::lookup, channel},         {semantic::dependency_kind::constant, channel},
            {semantic::dependency_kind::body, channel},           {semantic::dependency_kind::project_model, model},
            {semantic::dependency_kind::record_set, record_set}};
        return validators.at(read.kind)(read, current);
    }
    inline auto source_read(const semantic::dependency& read) -> bool {
        return read.kind == semantic::dependency_kind::source ||
               (!read.scope && (read.kind == semantic::dependency_kind::enumeration || read.kind == semantic::dependency_kind::negative_lookup));
    }
    inline auto sources_match(const semantic::candidate& candidate, const environment& current) -> bool {
        for (const auto& unit : candidate.units) {
            for (const auto& input : unit.direct_inputs)
                if (input.kind == semantic::input_kind::file && !current.file(unit.recipe.unit.project, input.name, input.object))
                    return false;
            for (const auto& read : unit.observed_dependencies)
                if (source_read(read) && !dependency(read, current))
                    return false;
        }
        return true;
    }
    auto input(const semantic::input_object& required, std::string_view project, const environment& current) -> bool {
        if (required.kind == semantic::input_kind::file)
            return current.file(project, required.name, required.object);
        if (required.kind == semantic::input_kind::provider) {
            auto found = std::ranges::find(current.validators, required.type, &language::input_validator::type);
            return found != current.validators.end() && found->validate(required, current.installed(project));
        }
        return required.type.empty() && current.input(project, required.kind, required.name, required.object);
    }
    inline auto covers(const semantic::unit& unit, const semantic::extent& demand) -> bool {
        // requested describes the completed computation's boundary within this
        // unit. available.files may list every file of a complete
        // module; it is not a list of all files in the global stage demand.
        const auto& completed = unit.coverage.requested;
        auto dimension = [](const auto& previous, const auto& requested, const std::vector<std::string>& local) {
            if (previous.empty())
                return true;
            if (local.empty())
                return !requested.empty() && std::ranges::all_of(requested, [&](const auto& value) { return std::ranges::contains(previous, value); });
            return std::ranges::all_of(local, [&](const auto& value) {
                return (!requested.empty() && !std::ranges::contains(requested, value)) || std::ranges::contains(previous, value);
            });
        };
        const auto& key = unit.recipe.unit;
        const auto files = key.kind == semantic::unit_kind::file ? std::vector<std::string>{key.name} : std::vector<std::string>{};
        auto modules = unit.modules;
        if (modules.empty() && key.kind == semantic::unit_kind::module)
            modules.push_back(key.name);
        return (!completed.selector || completed.selector == demand.selector) && dimension(completed.files, demand.files, files) &&
               dimension(completed.modules, demand.modules, modules) && dimension(completed.contexts, demand.contexts, {unit.context}) &&
               dimension(completed.projects, demand.projects, {key.project});
    }
    inline auto accept(const semantic::candidate& candidate, const language::session_state& session, const semantic::extent& demand, const environment& current,
                       bool sources_checked = false) -> bool {
        if (!candidate.cacheable || candidate.units.empty() || !std::ranges::any_of(current.installed(session.project), [](const auto& input) {
                return input.kind == semantic::input_kind::runtime && !input.object.empty();
            }))
            return false;
        for (const auto& unit : candidate.units) {
            const auto& key = unit.recipe.unit;
            if (!unit.cacheable || unit.recipe != candidate.recipe || key.project != session.project || key.stage != session.stage ||
                key.provider != session.provider || unit.recipe.options != session.options || unit.direct_inputs.empty() || !unit.coverage.missing.empty() ||
                !covers(unit, demand))
                return false;
            for (const auto& required : demand.facts)
                if (!std::ranges::any_of(unit.coverage.available, [&](const auto& scope) {
                        return std::ranges::contains(scope.facts, required) && (scope.projects.empty() || std::ranges::contains(scope.projects, key.project)) &&
                               (scope.contexts.empty() || std::ranges::contains(scope.contexts, unit.context));
                    }))
                    return false;
            for (const auto& input : unit.direct_inputs)
                if (!(sources_checked && input.kind == semantic::input_kind::file) && !reuse::input(input, key.project, current))
                    return false;
            for (const auto& read : unit.observed_dependencies) {
                if (sources_checked && source_read(read)) {
                    // Unscoped negative lookups also depend on the worker's prepared inputs.
                    if (read.kind == semantic::dependency_kind::negative_lookup && !absent_inputs(read, current))
                        return false;
                } else if (!dependency(read, current))
                    return false;
            }
        }
        return true;
    }
} // namespace reuse
