export module vcpkg;
import std;
import reflection;
import constants;
export import provider;

export namespace builds {
    struct vcpkg_dependency {
        std::string name;
        std::vector<std::string> features;
        rfl::Rename<"default-features", bool> defaults = true;
        bool host = false;
        std::optional<std::string> platform;
        rfl::Rename<"version>=", std::optional<std::string>> minimum;
    };
    using vcpkg_reference = std::variant<std::string, vcpkg_dependency>;
    struct vcpkg_feature {
        std::string description = "Dependency feature";
        std::vector<vcpkg_reference> dependencies;
    };
    struct vcpkg_override {
        std::string name;
        std::optional<std::string> version;
        rfl::Rename<"version-string", std::optional<std::string>> version_string;
        rfl::Rename<"version-semver", std::optional<std::string>> version_semver;
        rfl::Rename<"version-date", std::optional<std::string>> version_date;
        rfl::Rename<"port-version", std::optional<std::uint32_t>> port_version;
    };
    // Only declarative registry selectors cross the publisher boundary.
    struct vcpkg_manifest {
        std::vector<vcpkg_reference> dependencies;
        std::map<std::string, vcpkg_feature> features;
        rfl::Rename<"default-features", std::vector<std::string>> defaults;
        std::vector<vcpkg_override> overrides;
        rfl::Rename<"builtin-baseline", std::optional<std::string>> baseline;
    };
    struct dependency_recipe {
        std::string executable;
        std::string root;
        std::vector<std::string> triplets;
        std::vector<std::string> overlay_ports;
        std::vector<std::string> overlay_triplets;
        environment env;
        std::uint32_t timeout_s = 1800;
        bool offline = false;
    };
    inline void dependency_name(std::string_view value) {
        if (value.empty() || value.size() > 128 || value.front() == '-' || value.back() == '-' ||
            !std::ranges::all_of(value, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }))
            throw std::invalid_argument("invalid dependency identifier");
    }
    inline void validate(const dependency_recipe& recipe) {
        if (!builds::absolute_path(recipe.executable) || !builds::absolute_path(recipe.root) || recipe.triplets.empty() || recipe.timeout_s == 0)
            throw std::invalid_argument("dependency recipe requires absolute executable/root, allowed triplets and a positive timeout");
        for (const auto& triplet : recipe.triplets)
            dependency_name(triplet);
        for (const auto& paths : {recipe.overlay_ports, recipe.overlay_triplets})
            for (const auto& path : paths)
                if (!builds::absolute_path(path))
                    throw std::invalid_argument("publisher overlays must be absolute operator paths");
        validate_environment(recipe.env);
        if (recipe.env.contains("VCPKG_FEATURE_FLAGS"))
            throw std::invalid_argument("dependency feature flags are managed");
    }
    inline void validate(const vcpkg_manifest& manifest) {
        std::size_t count = 0;
        auto references = [&](const auto& entries) {
            for (const auto& entry : entries) {
                if (++count > constants::dependency_manifest_entries)
                    throw std::invalid_argument("too many dependency selectors");
                std::visit(
                    [&](const auto& value) {
                        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, std::string>)
                            dependency_name(value);
                        else {
                            dependency_name(value.name);
                            for (const auto& feature : value.features)
                                dependency_name(feature);
                            if (value.platform && (value.platform->size() > 256 || !std::ranges::all_of(*value.platform, [](unsigned char c) {
                                                       return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || std::string_view{"_ !&|()"}.contains(c);
                                                   })))
                                throw std::invalid_argument("invalid dependency platform expression");
                        }
                    },
                    entry);
            }
        };
        references(manifest.dependencies);
        for (const auto& [name, feature] : manifest.features) {
            dependency_name(name);
            references(feature.dependencies);
        }
        for (const auto& feature : manifest.defaults.get())
            dependency_name(feature);
        for (const auto& value : manifest.overrides)
            dependency_name(value.name);
        if (manifest.baseline.get() &&
            (manifest.baseline.get()->size() != 40 || manifest.baseline.get()->find_first_not_of("0123456789abcdef") != std::string::npos))
            throw std::invalid_argument("invalid requested dependency baseline");
        if (rfl::json::write(manifest).size() > constants::dependency_manifest_bytes)
            throw std::invalid_argument("dependency manifest exceeds size limit");
    }
    inline auto dependency_manifest(std::string_view text) -> vcpkg_manifest {
        if (text.size() > constants::dependency_manifest_bytes)
            throw std::invalid_argument("dependency manifest exceeds size limit");
        auto raw = rfl::json::read<rfl::Generic>(text);
        if (!raw)
            throw std::invalid_argument("invalid dependency manifest");
        auto object = raw->to_object();
        if (!object)
            throw std::invalid_argument("dependency manifest must be an object");
        for (const auto& [name, value] : *object)
            if (name == "vcpkg-configuration")
                throw std::invalid_argument("custom registries stay in the private cache");
        auto parsed = rfl::json::read<vcpkg_manifest, rfl::DefaultIfMissing>(text);
        if (!parsed)
            throw std::invalid_argument("unsupported dependency manifest");
        validate(*parsed);
        return *parsed;
    }
} // namespace builds

namespace builds {
    // vcpkg in manifest mode: install reads the vcpkg.json of manifest_dir and lays the packages out under install_dir. Every
    // directory it writes is a build directory of the step, so a tree already holding one is refused.
    // What it installs is what the manifest records: a triplet names settings of this machine, never a version or a baseline.
    template <> struct adapter<"vcpkg"> {
        struct options {
            std::string executable = "vcpkg";
            // The vcpkg checkout, absolute on this machine or relative to the project; empty, vcpkg uses the root it belongs to.
            std::optional<std::string> root;
            std::string manifest_dir = ".";
            // Required, and never a directory that holds the manifest.
            std::string install_dir;
            std::string triplet;
            std::string host_triplet;
            std::vector<std::string> features;
            std::vector<std::string> overlay_ports;
            std::vector<std::string> overlay_triplets;
            std::optional<std::string> buildtrees_dir;
            std::optional<std::string> packages_dir;
            std::optional<std::string> downloads_dir;
            // Further options of `vcpkg install` no field above composes, each one argument.
            std::vector<std::string> extra;
            environment env;
            bool offline = false;
            // Install the manifest's default features too; false passes --x-no-default-features.
            bool default_features = true;
        };
        static void validate(const options& c) {
            relative(c.manifest_dir, true);
            relative(c.install_dir);
            if (c.executable.empty())
                throw std::invalid_argument("vcpkg executable is required");
            if (c.root && !builds::absolute_path(*c.root))
                relative(*c.root, true);
            word(c.triplet, "triplet");
            word(c.host_triplet, "host triplet");
            for (auto& feature : c.features)
                word(feature, "feature");
            for (auto& port : c.overlay_ports)
                relative(port, true);
            for (auto& triplet : c.overlay_triplets)
                relative(triplet, true);
            for (auto& dir : {c.buildtrees_dir, c.packages_dir, c.downloads_dir})
                if (dir)
                    relative(*dir);
            for (auto& option : c.extra)
                if (!option.starts_with("--") || !field(option).empty())
                    throw std::invalid_argument("vcpkg extra takes an option no field composes: " + option);
            validate_environment(c.env);
            if (c.env.contains("VCPKG_FEATURE_FLAGS"))
                throw std::invalid_argument("vcpkg feature flags are managed by the worker");
        }
        static auto describe(const options& c) -> std::map<std::string, operation_spec> {
            operation_spec install{.outputs = {{"installed", {"directory", c.install_dir, "directory"}}},
                                   .needs = tools({c.executable}),
                                   .source_dirs = {c.manifest_dir},
                                   .build_dirs = {c.install_dir}};
            for (auto& dir : {c.buildtrees_dir, c.packages_dir, c.downloads_dir})
                if (dir)
                    install.build_dirs.push_back(*dir);
            return {{"install", std::move(install)}};
        }
        static auto expand(const options& c, std::string_view operation, const parameters& p) -> std::vector<step> {
            if (operation != "install")
                throw std::out_of_range("no such operation");
            for (const auto& argument : p.args)
                if (argument.starts_with('@') || cache_option(argument))
                    throw std::invalid_argument("vcpkg binary caching is managed by the worker; response files are not allowed");
            auto tree = [](const std::string& p) { return "${tree}/" + p; };
            step out{.name = "install", .cwd = c.manifest_dir, .preparation = true};
            out.argv = {tool(c.executable), "install", "--x-manifest-root=" + tree(c.manifest_dir), "--x-install-root=" + tree(c.install_dir)};
            if (!c.triplet.empty())
                out.argv.push_back("--triplet=" + c.triplet);
            if (!c.host_triplet.empty())
                out.argv.push_back("--host-triplet=" + c.host_triplet);
            for (auto& port : c.overlay_ports)
                out.argv.push_back("--overlay-ports=" + tree(port));
            for (auto& triplet : c.overlay_triplets)
                out.argv.push_back("--overlay-triplets=" + tree(triplet));
            if (c.buildtrees_dir)
                out.argv.push_back("--x-buildtrees-root=" + tree(*c.buildtrees_dir));
            if (c.packages_dir)
                out.argv.push_back("--x-packages-root=" + tree(*c.packages_dir));
            if (c.downloads_dir)
                out.argv.push_back("--downloads-root=" + tree(*c.downloads_dir));
            if (c.root) {
                auto& root = *c.root;
                out.argv.push_back("--vcpkg-root=" + (builds::absolute_path(root) ? root : tree(root)));
            }
            for (auto& feature : c.features)
                out.argv.push_back("--x-feature=" + feature);
            if (!c.default_features)
                out.argv.push_back("--x-no-default-features");
            if (c.offline)
                out.argv.push_back("--no-downloads");
            out.argv.insert(out.argv.end(), c.extra.begin(), c.extra.end());
            out.argv.insert(out.argv.end(), p.args.begin(), p.args.end());
            // The step's own environment; the runner turns ${jobs} into the count it allows, which vcpkg applies per package.
            out.env = c.env;
            out.env["VCPKG_MAX_CONCURRENCY"] = "${jobs}";
            out.env["VCPKG_BINARY_SOURCES"] = "clear;files,${cache:vcpkg},readwrite";
            out.env["VCPKG_FEATURE_FLAGS"] = "binarycaching";
            // Custom ports/triplets and opaque arguments stay in the tenant's private cache.
            if (c.overlay_ports.empty() && c.overlay_triplets.empty() && c.extra.empty() && p.args.empty())
                out.dependencies = dependency_input{c.manifest_dir, c.triplet, c.host_triplet, c.features, c.default_features};
            return {std::move(out)};
        }

    private:
        static auto cache_option(std::string_view option) -> bool {
            std::string normalized{option};
            std::ranges::transform(normalized, normalized.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (normalized.starts_with("--x-"))
                normalized.erase(2, 2);
            return normalized.starts_with("--binarysource") || normalized.starts_with("--binarycaching") || normalized.starts_with("--no-binarycaching") ||
                   normalized.starts_with("--feature-flags") || normalized.starts_with("--editable");
        }
        // A triplet or feature name: one word vcpkg looks up, not a path.
        static void word(const std::string& value, std::string_view what) {
            auto plain = [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '+'; };
            if (!value.empty() && !std::ranges::all_of(value, plain))
                throw std::invalid_argument(std::string{"invalid vcpkg "} + std::string{what} + ": " + value);
        }
        // The field that composes the option, empty when none does: extra states no setting twice.
        static auto field(std::string_view option) -> std::string_view {
            if (cache_option(option))
                return "binary cache";
            for (auto [composed_by, option_name] :
                 {std::pair{"manifest-root", "--x-manifest-root"}, std::pair{"install-root", "--x-install-root"}, std::pair{"triplet", "--triplet"},
                  std::pair{"host-triplet", "--host-triplet"}, std::pair{"overlay-ports", "--overlay-ports"},
                  std::pair{"overlay-triplets", "--overlay-triplets"}, std::pair{"buildtrees-root", "--x-buildtrees-root"},
                  std::pair{"packages-root", "--x-packages-root"}, std::pair{"downloads-root", "--downloads-root"}, std::pair{"vcpkg-root", "--vcpkg-root"},
                  std::pair{"feature", "--x-feature"}, std::pair{"no-default-features", "--x-no-default-features"},
                  std::pair{"no-downloads", "--no-downloads"}})
                if (option == option_name || option.starts_with(std::string{option_name}))
                    return composed_by;
            return {};
        }
    };
    static_assert(system<adapter<"vcpkg">>);
} // namespace builds
