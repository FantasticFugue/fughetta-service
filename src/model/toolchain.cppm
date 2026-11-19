export module toolchain;
import std;
import content;
import json;
import host;

export namespace toolchain {
    inline auto required_tools() -> std::vector<std::string> {
        return {"cmake",
                "ctest",
                "bazel",
                "ninja",
                "make",
                "cc",
                "c++",
                "gcc",
                "g++",
                "clang",
                "clang++",
                "cl",
                "link",
                "ld",
                "lld",
                "mold",
                "vcpkg",
                "python",
                "python3",
                "uv",
                "ruff",
                "ty",
                "clice",
                "tsc",
                "typescript-language-server",
                "node",
                "npm",
                "bun",
                "gradle",
                "java",
                "fughetta-kotlin",
                "dotnet",
                "git",
                "fughetta-llvm"};
    }

    struct sdk {
        std::string manifest;
        std::vector<std::string> roots;
    };

    struct binding {
        std::string name, object;
        std::vector<std::string> roots;
    };

    struct identity {
        std::string id;
        // Executables and their process environment. SDK bindings are consumed
        // separately by providers that report their actual external reads.
        std::string execution;
        std::string platform;
        std::map<std::string, std::string> executables;
        std::vector<std::string> tools;
        std::vector<binding> inputs;
    };

    struct member {
        const binding* input;
        std::string path;
    };

    // Select from installation observations; evaluating a provider does not
    // reopen executables. The caller supplies its effective process environment.
    auto select(const identity& runtime, const std::map<std::string, std::string>& executables, const std::map<std::string, std::string>& environment)
        -> std::string {
        if (runtime.platform.empty() || executables.empty())
            return {};
        using selected_tool = std::pair<std::string, std::string>;
        std::map<selected_tool, std::string> selected;
        for (const auto& [role, executable] : executables) {
            const auto path = std::filesystem::path{executable}.lexically_normal().generic_string();
            const auto found = runtime.executables.find(path);
            if (found == runtime.executables.end() || found->second.empty())
                return {};
            // The caller names the operation's actual tool roles. Reverse
            // lookup through worker mappings would include unused aliases of
            // the same executable and invalidate unrelated contributions.
            // Keep argv[0] dispatch: clang and clang++ may be one inode while
            // choosing different drivers. Parent directories only locate tools.
            selected.emplace(selected_tool{role, std::filesystem::path{path}.filename().generic_string()}, found->second);
        }
        return content::hash_of(json::write(std::tuple{runtime.platform, selected, environment})).str();
    }

    // Roots are normalized once when a worker installs its immutable SDKs.
    // Matching is lexical: exports can also describe missing files. Nested
    // bindings are allowed; the longest matching root owns the exported path.
    auto locate(std::span<const binding> inputs, const std::filesystem::path& path) -> std::optional<member> {
        const auto name = path.lexically_normal().generic_string();
        std::optional<member> result;
        std::size_t longest = 0;
        for (const auto& input : inputs)
            for (const auto& root : input.roots) {
                const auto prefix = root.ends_with('/') ? root : root + "/";
                if ((name == root || name.starts_with(prefix)) && (!result || root.size() > longest)) {
                    result = member{&input, name == root ? "" : name.substr(prefix.size())};
                    longest = root.size();
                }
            }
        return result;
    }

    auto digest(const std::filesystem::path& path) -> std::string {
        std::ifstream input{path, std::ios::binary};
        if (!input)
            throw std::runtime_error("cannot fingerprint toolchain file: " + path.string());
        content::sha256 hash;
        std::array<char, 64 * 1024> buffer;
        while (input.read(buffer.data(), buffer.size()) || input.gcount())
            hash.update(std::string_view{buffer.data(), static_cast<std::size_t>(input.gcount())});
        if (!input.eof())
            throw std::runtime_error("cannot finish toolchain fingerprint: " + path.string());
        return content::to_hex(hash.finish()).str();
    }

    // Managed SDK manifests identify immutable SDK releases. Hash actual bytes, never
    // executable names alone, and include the effective worker environment.
    auto inspect(const std::map<std::string, std::string>& resolved, const std::map<std::string, std::string>& environment,
                 const std::map<std::string, sdk>& sdk_manifests = {}) -> identity {
        identity out;
        std::map<std::string, std::string> digests, system;
        std::map<std::string, std::pair<std::string, std::string>> binaries;
        for (const auto& [name, path] : resolved) {
            try {
                auto canonical = std::filesystem::canonical(path).string();
                auto found = digests.find(canonical);
                if (found == digests.end())
                    found = digests.emplace(canonical, digest(canonical)).first;
                binaries[name] = {std::filesystem::path{path}.filename().generic_string(), found->second};
                const auto spelling = std::filesystem::path{path}.lexically_normal().generic_string();
                out.executables[spelling] = found->second;
                out.executables[std::filesystem::path{canonical}.generic_string()] = found->second;
                out.tools.push_back(name);
            } catch (const std::exception&) {
                // A missing or unreadable executable cannot be offered, but other tools remain usable.
            }
        }
        std::map<std::filesystem::path, std::string> sdk_roots;
        auto install_sdk = [&](std::string name, std::string object, const std::vector<std::string>& roots) {
            if (roots.empty())
                throw std::invalid_argument("SDK binding requires an installed root: " + name);
            binding input{.name = "sdk/" + name, .object = std::move(object)};
            std::optional<std::filesystem::path> directory;
            for (const auto& root : roots) {
                const auto path = std::filesystem::path{root};
                if (!path.is_absolute())
                    throw std::invalid_argument("SDK roots must be absolute: " + root);
                const auto canonical = std::filesystem::canonical(path);
                if (!std::filesystem::is_directory(canonical) || (directory && *directory != canonical))
                    throw std::invalid_argument("all roots of SDK " + name + " must resolve to the same directory");
                directory = canonical;
                const auto spelling = path.lexically_normal();
                input.roots.push_back((spelling.has_filename() ? spelling : spelling.parent_path()).generic_string());
                input.roots.push_back(canonical.generic_string());
            }
            std::ranges::sort(input.roots);
            input.roots.erase(std::unique(input.roots.begin(), input.roots.end()), input.roots.end());
            const auto [owner, inserted] = sdk_roots.emplace(*directory, input.name);
            if (!inserted)
                throw std::invalid_argument("SDK bindings share the same directory: " + owner->second + " and " + input.name);
            // The manifest identifies the SDK across workers; its root spellings
            // are interchangeable local bindings for that single directory.
            // Nested bindings use longest-prefix ownership in providers and locate().
            out.inputs.push_back(std::move(input));
        };
        for (const auto& [name, sdk] : sdk_manifests)
            install_sdk(name, digest(sdk.manifest), sdk.roots);
        // Distribution manifests include compiler runtime and system-header package versions.
        for (auto path : {"/var/lib/dpkg/status", "/var/lib/rpm/rpmdb.sqlite", "/usr/lib/sysimage/rpm/rpmdb.sqlite"})
            if (std::filesystem::is_regular_file(path))
                system[path] = digest(path);
        std::map<std::string, std::vector<std::string>> detected;
        for (std::string variable : {"JAVA_HOME", "ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_NDK_HOME"}) {
            auto found = environment.find(variable);
            if (found == environment.end())
                continue;
            auto name = variable == "ANDROID_HOME" || variable == "ANDROID_SDK_ROOT" ? "ANDROID_SDK" : variable;
            detected[name].push_back(found->second);
        }
        for (const auto& [name, roots] : detected) {
            std::map<std::string, std::string> manifests;
            std::set<std::filesystem::path> visited;
            for (const auto& spelling : roots) {
                const auto root = std::filesystem::path{spelling};
                if (!visited.insert(std::filesystem::weakly_canonical(root)).second)
                    continue;
                for (auto manifest : {"release", "source.properties"})
                    if (std::filesystem::is_regular_file(root / manifest))
                        manifests[manifest] = digest(root / manifest);
                for (auto group : {"platforms", "build-tools", "ndk", "cmdline-tools"}) {
                    std::error_code error;
                    for (const auto& entry : std::filesystem::directory_iterator(root / group, error)) {
                        auto manifest = entry.path() / "source.properties";
                        if (std::filesystem::is_regular_file(manifest))
                            manifests[manifest.lexically_relative(root).generic_string()] = digest(manifest);
                    }
                }
            }
            // Package manifests contribute to the enclosing SDK's identity.
            if (!manifests.empty())
                install_sdk(name, content::hash_of(json::write(manifests)).str(), roots);
        }
        out.platform = content::hash_of(json::write(std::tuple{std::string{host::os}, std::string{host::arch}, system})).str();
        out.execution = content::hash_of(json::write(std::tuple{out.platform, binaries, environment})).str();
        out.id = content::hash_of(json::write(std::tuple{out.execution, out.inputs})).str();
        return out;
    }
} // namespace toolchain
