export module cargo;
import std;
import content;
import host;
export import provider;

namespace builds {
    template <> struct adapter<"cargo"> {
        struct options {
            std::string executable = "cargo";
            std::string project_dir = ".";
            std::optional<std::string> manifest_path;
            std::string build_dir = "build/fughetta/cargo";
            std::optional<std::string> cache_dir;
            std::vector<std::string> packages;
            std::vector<std::string> features;
            bool all_features = false;
            bool no_default_features = false;
            bool workspace = false;
            std::optional<std::string> profile;
            std::optional<std::string> target;
            bool locked = true;
            bool offline = false;
            std::map<std::string, artifact> artifacts;
        };
        static void validate(const options& o);
        static auto describe(const options& o) -> std::map<std::string, operation_spec>;
        static auto expand(const options& o, std::string_view operation, const parameters& p) -> std::vector<step>;
    };
    static_assert(system<adapter<"cargo">>);
} // namespace builds

namespace builds {
    namespace {
        void option_value(std::string_view value, std::string_view field) {
            if (value.empty() || value.starts_with('-') || value.contains("${") || value.contains('\0') || value.find_first_of("\n\r") != value.npos)
                throw std::invalid_argument(std::string{field} + " must be a single Cargo option value");
        }
    } // namespace

    void adapter<"cargo">::validate(const options& o) {
        if (o.executable.empty() || o.executable.contains('\0') || o.executable.contains("${"))
            throw std::invalid_argument("cargo executable is required");
        tools({o.executable});
        if (builds::in_project({o.executable}))
            content::checked_path(o.executable);
        relative(o.project_dir, true);
        if (o.manifest_path)
            relative(*o.manifest_path);
        relative(o.build_dir);
        if (o.cache_dir) {
            const auto& cache = *o.cache_dir;
            relative(cache);
            auto components = [](const std::string& spelling) {
                auto path = std::filesystem::path{spelling}.lexically_normal().generic_string();
                if (host::os == "windows")
                    std::ranges::transform(path, path.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                return path + '/';
            };
            const auto cache_path = components(cache), build_path = components(o.build_dir);
            if (cache_path.starts_with(build_path) || build_path.starts_with(cache_path))
                throw std::invalid_argument("cargo build_dir and cache_dir must be separate directories");
        }
        for (const auto& package : o.packages)
            option_value(package, "cargo package");
        for (const auto& feature : o.features)
            option_value(feature, "cargo feature");
        if (o.profile)
            option_value(*o.profile, "cargo profile");
        if (o.target)
            option_value(*o.target, "cargo target");
        for (const auto& [name, output] : o.artifacts)
            relative(output.path);
    }

    auto adapter<"cargo">::describe(const options& o) -> std::map<std::string, operation_spec> {
        operation_spec check{.needs = tools({o.executable}), .source_dirs = {o.project_dir}, .build_dirs = {o.build_dir}};
        if (o.cache_dir)
            check.build_dirs.push_back(*o.cache_dir);
        auto build = check, test = check;
        build.outputs = o.artifacts;
        test.parameters = {"targets", "filter"};
        return {{"check", std::move(check)}, {"build", std::move(build)}, {"test", std::move(test)}};
    }

    auto adapter<"cargo">::expand(const options& o, std::string_view operation, const parameters& p) -> std::vector<step> {
        if (operation != "check" && operation != "build" && operation != "test")
            throw std::out_of_range("no such cargo operation");
        step out{.name = std::string{operation}, .cwd = o.project_dir, .cargo = true};
        if (o.cache_dir)
            out.env["CARGO_BUILD_BUILD_DIR"] = "${tree}/" + *o.cache_dir;
        const auto manifest = o.manifest_path.value_or(o.project_dir.empty() || o.project_dir == "." ? "Cargo.toml" : o.project_dir + "/Cargo.toml");
        auto& argv = out.argv;
        argv = {tool(o.executable), std::string{operation},
                "--manifest-path",  "${tree}/" + manifest,
                "--target-dir",     "${tree}/" + o.build_dir,
                "--color",          "never",
                "--jobs",           "${jobs}"};
        if (o.locked)
            argv.push_back("--locked");
        if (o.offline)
            argv.push_back("--offline");
        if (o.workspace)
            argv.push_back("--workspace");
        for (const auto& package : o.packages)
            argv.insert(argv.end(), {"--package", package});
        if (o.all_features)
            argv.push_back("--all-features");
        if (o.no_default_features)
            argv.push_back("--no-default-features");
        if (!o.features.empty()) {
            auto features = o.features | std::views::join_with(',') | std::ranges::to<std::string>();
            argv.insert(argv.end(), {"--features", std::move(features)});
        }
        if (o.profile)
            argv.insert(argv.end(), {"--profile", *o.profile});
        if (o.target)
            argv.insert(argv.end(), {"--target", *o.target});
        if (operation == "test") {
            for (const auto& target : p.targets) {
                option_value(target, "cargo test target");
                argv.insert(argv.end(), {"--test", target});
            }
            argv.insert(argv.end(), {"--", "-Z", "unstable-options", "--format", "json"});
            if (p.filter && !p.filter->empty()) {
                option_value(*p.filter, "cargo test filter");
                argv.push_back(*p.filter);
            }
            out.standard_output = "cargo-tests.jsonl";
            out.reports = {{"libtest", "cargo-tests.jsonl"}};
        }
        return {std::move(out)};
    }
} // namespace builds
