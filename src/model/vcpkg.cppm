export module vcpkg;
import std;
export import provider;

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
            if (c.root && !std::filesystem::path(*c.root).is_absolute())
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
            auto tree = [](const std::string& p) { return "${tree}/" + p; };
            step out{.name = "install", .cwd = c.manifest_dir};
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
                out.argv.push_back("--vcpkg-root=" + (std::filesystem::path(root).is_absolute() ? root : tree(root)));
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
            return {std::move(out)};
        }

    private:
        // A triplet or feature name: one word vcpkg looks up, not a path.
        static void word(const std::string& value, std::string_view what) {
            auto plain = [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '+'; };
            if (!value.empty() && !std::ranges::all_of(value, plain))
                throw std::invalid_argument(std::string{"invalid vcpkg "} + std::string{what} + ": " + value);
        }
        // The field that composes the option, empty when none does: extra states no setting twice.
        static auto field(std::string_view option) -> std::string_view {
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
