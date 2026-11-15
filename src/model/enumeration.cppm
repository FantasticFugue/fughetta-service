export module enumeration;
import std;
import reflection;
import content;
import records;

// Membership comes from the installed source manifest, independently of file contents.
export namespace enumeration {
    inline auto members(const std::map<std::string, std::string>& sources, const semantic::file_set& selection) -> std::set<std::string> {
        std::set<std::string> out;
        for (const auto& path : selection.files)
            if (sources.contains(path))
                out.insert(path);
        for (const auto& directory : selection.directories) {
            const auto prefix = directory.empty() || directory == "." ? std::string{} : directory + "/";
            for (auto it = sources.lower_bound(prefix); it != sources.end() && it->first.starts_with(prefix); ++it)
                out.insert(it->first);
        }
        return out;
    }

    inline auto value(const std::map<std::string, std::string>& sources, const semantic::file_set& selection) -> std::string {
        return content::hash_of(rfl::json::write(members(sources, selection))).str();
    }

    inline auto read(std::string project, std::string key, const std::map<std::string, std::string>& sources, semantic::file_set selection)
        -> semantic::dependency {
        for (auto* paths : {&selection.files, &selection.directories}) {
            for (auto& name : *paths) {
                auto path = std::filesystem::path{name}.lexically_normal();
                if (!path.empty() && !path.has_filename())
                    path = path.parent_path();
                name = path.generic_string();
            }
            std::ranges::sort(*paths);
            paths->erase(std::unique(paths->begin(), paths->end()), paths->end());
        }
        return {.kind = semantic::dependency_kind::enumeration,
                .project = std::move(project),
                .key = std::move(key),
                .value = value(sources, selection),
                .selection = std::move(selection)};
    }
} // namespace enumeration
