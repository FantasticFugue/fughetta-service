export module toolchain;
import std;
import content;
import json;
import host;
export import version;

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
                "fughetta-jvm",
                "fughetta-kotlin",
                "fughetta-scala",
                "dotnet",
                "cargo",
                "rustc",
                "git",
                "fughetta-llvm"};
    }

    struct sdk {
        std::string manifest;
        std::vector<std::string> roots;
        std::vector<std::string> features;
        std::map<std::string, std::string> env;
    };

    struct capability {
        std::string name, id;
        std::optional<std::string> version;
        std::vector<std::string> features;
        bool preferred = false;
    };

    struct sdk_constraint {
        std::string name, version;
        friend auto operator==(const sdk_constraint&, const sdk_constraint&) -> bool = default;
    };

    inline auto valid_name(std::string_view name) -> bool {
        return !name.empty() && std::ranges::all_of(name, [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '+';
        });
    }

    inline auto valid_sdk_name(std::string_view name) -> bool {
        return !name.empty() && std::ranges::all_of(std::views::split(name, '/'), [](auto part) {
            const std::string_view component{part};
            return valid_name(component) && component != "." && component != "..";
        });
    }

    struct requirement {
        std::string name;
        version_constraint version;
        std::vector<std::string> features;

        auto matches(const capability& instance, const std::optional<semver>& installed) const -> bool {
            return version.matches(installed) &&
                   std::ranges::all_of(features, [&](const auto& feature) { return std::ranges::contains(instance.features, feature); });
        }
    };

    auto require_tool(std::string_view text) -> requirement {
        const auto end = text.find_first_of("<>=^~[ \t");
        requirement out{.name = std::string{text.substr(0, end)}};
        if (!valid_name(out.name))
            throw std::invalid_argument("invalid required tool: " + std::string{text});
        if (end == text.npos)
            return out;
        auto suffix = text.substr(end);
        if (const auto features = suffix.find('['); features != suffix.npos) {
            if (!suffix.ends_with(']'))
                throw std::invalid_argument("invalid required tool features: " + std::string{text});
            const auto list = suffix.substr(features + 1, suffix.size() - features - 2);
            if (list.empty())
                throw std::invalid_argument("required tool features are empty: " + std::string{text});
            for (const auto part : std::views::split(list, ',')) {
                const std::string_view feature{part};
                if (!valid_name(feature))
                    throw std::invalid_argument("invalid required tool feature: " + std::string{feature});
                out.features.emplace_back(feature);
            }
            suffix = suffix.substr(0, features);
        }
        const auto first = suffix.find_first_not_of(" \t");
        if (first != suffix.npos)
            out.version = version_constraint::parse(suffix.substr(first));
        return out;
    }

    auto require_sdk(const sdk_constraint& constraint) -> requirement {
        if (!valid_sdk_name(constraint.name))
            throw std::invalid_argument("invalid required SDK name: " + constraint.name);
        return {.name = constraint.name, .version = version_constraint::parse(constraint.version)};
    }

    struct binding {
        std::string name, object;
        std::vector<std::string> roots;
    };

    struct tool_instance {
        capability info;
        std::string path, digest;
        std::map<std::string, std::string> env;
    };

    struct sdk_instance {
        capability info;
        binding input;
        std::map<std::string, std::string> env;
    };

    struct catalog {
        std::vector<capability> tools, sdks;
    };

    struct resolved {
        std::map<std::string, std::string> tools, sdks;
        friend auto operator==(const resolved&, const resolved&) -> bool = default;
    };

    struct constraints {
        struct group {
            std::string expression;
            std::vector<requirement> rules;
        };
        std::map<std::string, group> tools, sdks;
    };

    auto parse_constraints(std::span<const std::string> tools, std::span<const sdk_constraint> sdks) -> constraints {
        constraints out;
        auto add = [](constraints::group& group, std::string_view expression, requirement rule) {
            if (!group.expression.empty())
                group.expression += " & ";
            group.expression += expression;
            group.rules.push_back(std::move(rule));
        };
        for (const auto& tool : tools) {
            auto rule = require_tool(tool);
            auto& group = out.tools[rule.name];
            add(group, tool, std::move(rule));
        }
        for (const auto& sdk : sdks)
            add(out.sdks[sdk.name], sdk.version.empty() ? sdk.name : sdk.name + " " + sdk.version, require_sdk(sdk));
        return out;
    }

    struct capability_gap {
        std::string requirement;
        std::vector<std::string> installed;
    };

    struct resolution {
        resolved selected;
        std::vector<capability_gap> missing;
    };

    auto resolve(const catalog& available, const constraints& requested, bool details = false) -> resolution {
        resolution out;
        auto choose = [&](const std::vector<capability>& inventory, const auto& groups, auto& locked, std::string_view kind) {
            for (const auto& [name, group] : groups) {
                const capability* best = nullptr;
                std::optional<semver> best_version;
                for (const auto& instance : inventory) {
                    if (instance.name != name)
                        continue;
                    const auto version = instance.version ? parse_version(*instance.version) : std::nullopt;
                    if (!std::ranges::all_of(group.rules, [&](const auto& rule) { return rule.matches(instance, version); }))
                        continue;
                    const auto rank = [&](const auto& info, const auto& v) {
                        return std::tuple{info.preferred, !v || v->prerelease.empty(), v, std::string_view{info.id}};
                    };
                    if (!best || rank(instance, version) > rank(*best, best_version)) {
                        best = &instance;
                        best_version = version;
                    }
                }
                if (best) {
                    locked.emplace(name, best->id);
                } else {
                    auto& gap = out.missing.emplace_back(std::string{kind} + " " + group.expression, std::vector<std::string>{});
                    if (!details)
                        continue;
                    for (const auto& instance : inventory)
                        if (instance.name == name) {
                            auto description = instance.version.value_or("version unknown");
                            for (const auto& feature : instance.features)
                                description += " [" + feature + "]";
                            gap.installed.push_back(std::move(description));
                        }
                }
            }
        };
        choose(available.tools, requested.tools, out.selected.tools, "tool");
        choose(available.sdks, requested.sdks, out.selected.sdks, "SDK");
        return out;
    }

    struct identity {
        std::string id;
        std::string platform;
        std::map<std::string, std::string> executables;
        std::vector<std::string> tools;
        std::vector<binding> inputs;
        std::vector<tool_instance> tool_inventory;
        std::vector<sdk_instance> sdk_inventory;
    };

    struct member {
        const binding* input;
        std::string path;
    };

    auto locate(std::span<const binding> inputs, const std::filesystem::path& path) -> std::optional<member>;

    auto capabilities(const identity& runtime) -> catalog {
        catalog out;
        for (const auto& tool : runtime.tool_inventory)
            out.tools.push_back(tool.info);
        for (const auto& sdk : runtime.sdk_inventory)
            out.sdks.push_back(sdk.info);
        return out;
    }

    struct bindings {
        std::map<std::string, std::string> tools, sdks, env;
        std::vector<binding> sdk_inputs;
    };

    auto bind(const identity& runtime, const resolved& selected, std::map<std::string, std::string> tools = {}) -> bindings {
        bindings out{.tools = std::move(tools)};
        auto merge = [&](const auto& environment) {
            for (const auto& [name, value] : environment) {
                const auto [existing, inserted] = out.env.emplace(name, value);
                if (!inserted && existing->second != value)
                    throw std::invalid_argument("selected toolchain instances disagree on " + name);
            }
        };
        for (const auto& [name, id] : selected.tools) {
            const auto tool = std::ranges::find_if(runtime.tool_inventory, [&](const auto& tool) { return tool.info.id == id && tool.info.name == name; });
            if (tool == runtime.tool_inventory.end())
                throw std::invalid_argument("selected tool is unavailable: " + id);
            out.tools[name] = tool->path;
            merge(tool->env);
        }
        for (const auto& [name, id] : selected.sdks) {
            const auto sdk = std::ranges::find_if(runtime.sdk_inventory, [&](const auto& sdk) { return sdk.info.id == id && sdk.info.name == name; });
            if (sdk == runtime.sdk_inventory.end())
                throw std::invalid_argument("selected SDK is unavailable: " + id);
            out.sdks[name] = sdk->input.roots.front();
            out.sdk_inputs.push_back(sdk->input);
            merge(sdk->env);
        }
        if (const auto java = out.env.find("JAVA_HOME"); java != out.env.end() && !out.sdks.contains("jdk"))
            if (auto sdk = locate(runtime.inputs, java->second); sdk && sdk->path.empty())
                out.sdk_inputs.push_back(*sdk->input);
        return out;
    }

    auto installation(std::string_view key, bool sdk = false) -> capability {
        const auto separator = key.find('@');
        capability out{.name = std::string{key.substr(0, separator)}, .id = std::string{key}, .preferred = separator == key.npos};
        if (!(sdk ? valid_sdk_name(out.name) : valid_name(out.name)))
            throw std::invalid_argument(std::string{sdk ? "invalid SDK name: " : "invalid toolchain name: "} + out.name);
        if (!out.preferred) {
            const auto version = key.substr(separator + 1);
            if (!parse_version(version))
                throw std::invalid_argument("invalid version in toolchain instance: " + out.id);
            out.version = std::string{version};
        }
        return out;
    }

    auto java_version(const std::filesystem::path& root) -> std::optional<std::string> {
        std::ifstream release{root / "release"};
        for (std::string line; std::getline(release, line);)
            if (line.starts_with("JAVA_VERSION=\"") && line.ends_with('"')) {
                auto version = line.substr(14, line.size() - 15);
                if (parse_version(version))
                    return version;
                break;
            }
        return std::nullopt;
    }

    auto discover(const std::map<std::string, std::string>& mapped) -> std::map<std::string, std::string> {
        const auto names = required_tools();
        const std::set<std::string, std::less<>> known{names.begin(), names.end()};
        auto available = host::resolve_tools(mapped);
        std::map<std::string, std::string> out;
        for (const auto& [name, path] : available) {
            if (mapped.contains(name) || known.contains(name)) {
                out.insert_or_assign(name, path);
                continue;
            }
            const auto number = name.find_first_of("0123456789");
            if (number == name.npos || number == 0 || !parse_version(std::string_view{name}.substr(number)))
                continue;
            const auto stem = name.substr(0, name[number - 1] == '-' ? number - 1 : number);
            if (known.contains(stem) && !mapped.contains(stem + "@" + name.substr(number)))
                out.try_emplace(stem + "@" + name.substr(number), path);
        }
        // A JDK next to the default installation can be runnable without being on PATH.
        if (const auto java = out.find("java"); java != out.end()) {
            std::error_code error;
            const auto executable = std::filesystem::canonical(java->second, error);
            if (!error) {
                const auto home = executable.parent_path().parent_path();
                std::set<std::filesystem::path> homes;
                if (java_version(home))
                    for (const auto& entry : std::filesystem::directory_iterator(home.parent_path(), error))
                        if (entry.is_directory(error) && !error)
                            homes.insert(entry.path());
                for (const auto& candidate : homes)
                    if (const auto version = java_version(candidate)) {
                        const auto path = candidate / "bin" / executable.filename();
                        if (!std::filesystem::is_regular_file(path, error))
                            continue;
                        auto id = "java@" + *version;
                        if (!mapped.contains(id))
                            out.try_emplace(std::move(id), path.string());
                    }
            }
        }
        return out;
    }

    auto tool_path(const std::filesystem::path& path) -> std::string { return path.lexically_normal().generic_string(); }

    auto tool_digest(const identity& runtime, const std::string& path) -> std::string {
        const auto found = runtime.executables.find(path);
        return found == runtime.executables.end() ? std::string{} : found->second;
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
                 const std::map<std::string, sdk>& sdk_manifests = {}, const std::map<std::string, std::string>& configured = {}) -> identity {
        identity out;
        std::map<std::string, std::string> digests;
        std::map<std::string, std::pair<std::string, std::string>> binaries;
        std::map<std::string, std::pair<capability, std::filesystem::path>> jdks;
        std::map<std::pair<std::string, std::string>, std::size_t> installations;
        for (const auto& [name, path] : resolved) {
            try {
                auto info = installation(name);
                auto canonical = std::filesystem::canonical(path).string();
                auto found = digests.find(canonical);
                if (found == digests.end())
                    found = digests.emplace(canonical, digest(canonical)).first;
                binaries[name] = {std::filesystem::path{path}.filename().generic_string(), found->second};
                const auto spelling = tool_path(path);
                out.executables[spelling] = found->second;
                out.executables[std::filesystem::path{canonical}.generic_string()] = found->second;
                if (!std::ranges::contains(out.tools, info.name))
                    out.tools.push_back(info.name);
                std::map<std::string, std::string> env;
                if (info.name == "java") {
                    const auto root = std::filesystem::path{canonical}.parent_path().parent_path();
                    if (auto version = java_version(root)) {
                        info.version = *version;
                        env.emplace("JAVA_HOME", root.generic_string());
                        auto sdk_info = installation("jdk@" + *version);
                        sdk_info.preferred = info.preferred;
                        auto [entry, inserted] = jdks.try_emplace(sdk_info.id, sdk_info, root);
                        if (!inserted && sdk_info.preferred)
                            entry->second = {std::move(sdk_info), root};
                    }
                }
                const auto [entry, inserted] = installations.try_emplace({info.name, canonical}, out.tool_inventory.size());
                tool_instance instance{std::move(info), spelling, found->second, std::move(env)};
                if (inserted) {
                    out.tool_inventory.push_back(std::move(instance));
                } else {
                    auto& previous = out.tool_inventory[entry->second];
                    auto rank = [&](const auto& tool) { return std::pair{configured.contains(tool.info.id), tool.info.preferred}; };
                    if (rank(instance) > rank(previous))
                        std::swap(instance, previous);
                    previous.info.preferred |= instance.info.preferred;
                    if (!previous.info.version)
                        previous.info.version = std::move(instance.info.version);
                }
            } catch (const std::exception&) {
                // A missing or unreadable executable cannot be offered, but other tools remain usable.
            }
        }
        std::map<std::filesystem::path, std::string> sdk_roots;
        auto install_sdk = [&](capability info, std::string object, const std::vector<std::string>& roots, std::map<std::string, std::string> env) {
            const auto& name = info.id;
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
            if (info.name == "jdk") {
                if (auto version = java_version(*directory))
                    info.version = std::move(version);
                env.try_emplace("JAVA_HOME", directory->generic_string());
            }
            // The manifest identifies the SDK across workers; its root spellings
            // are interchangeable local bindings for that single directory.
            // Nested bindings use longest-prefix ownership in providers and locate().
            out.inputs.push_back(input);
            out.sdk_inventory.push_back({std::move(info), std::move(input), std::move(env)});
        };
        for (const auto& [name, sdk] : sdk_manifests) {
            auto info = installation(name, true);
            info.features = sdk.features;
            install_sdk(std::move(info), digest(sdk.manifest), sdk.roots, sdk.env);
        }
        auto jdk_image = [](const std::filesystem::path& root) {
            struct fingerprint {
                std::uintmax_t bytes;
                std::filesystem::file_time_type modified;
                std::string object;
            };
            static std::mutex mutex;
            static std::map<std::filesystem::path, fingerprint> cache;
            std::map<std::string, std::string> files;
            for (auto member : {"release", "lib/modules", "lib/ct.sym", "lib/rt.jar", "jre/lib/rt.jar"}) {
                const auto candidate = root / member;
                if (!std::filesystem::is_regular_file(candidate))
                    continue;
                const auto path = std::filesystem::canonical(candidate);
                const auto bytes = std::filesystem::file_size(path);
                const auto modified = std::filesystem::last_write_time(path);
                std::lock_guard lock{mutex};
                auto found = cache.find(path);
                if (found == cache.end() || found->second.bytes != bytes || found->second.modified != modified)
                    found = cache.insert_or_assign(path, fingerprint{bytes, modified, digest(path)}).first;
                files[member] = found->second.object;
            }
            return files;
        };
        auto canonical_sdk_root = [](const std::filesystem::path& root) { return std::filesystem::weakly_canonical(root); };
        for (const auto& [id, jdk] : jdks) {
            const auto& [info, root] = jdk;
            if (sdk_manifests.contains(id) || sdk_roots.contains(canonical_sdk_root(root)))
                continue;
            install_sdk(info, content::hash_of(json::write(jdk_image(root))).str(), {root.generic_string()}, {{"JAVA_HOME", root.generic_string()}});
        }
        std::map<std::string, std::vector<std::string>> detected;
        for (std::string variable : {"JAVA_HOME", "ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_NDK_HOME"}) {
            auto found = environment.find(variable);
            if (found == environment.end())
                continue;
            if (variable == "JAVA_HOME" && sdk_roots.contains(canonical_sdk_root(found->second)))
                continue;
            auto name = variable == "ANDROID_HOME" || variable == "ANDROID_SDK_ROOT" ? "ANDROID_SDK" : variable;
            detected[name].push_back(found->second);
        }
        for (const auto& [name, roots] : detected) {
            std::map<std::string, std::string> manifests;
            std::set<std::filesystem::path> visited;
            for (const auto& spelling : roots) {
                const auto root = std::filesystem::path{spelling};
                if (!visited.insert(canonical_sdk_root(root)).second)
                    continue;
                if (name == "JAVA_HOME") {
                    manifests.merge(jdk_image(root));
                    continue;
                }
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
                install_sdk(installation(name), content::hash_of(json::write(manifests)).str(), roots, {});
        }
        out.platform = content::hash_of(json::write(std::tuple{host::os, host::arch})).str();
        out.id = content::hash_of(json::write(std::tuple{out.platform, binaries, environment, out.inputs})).str();
        return out;
    }
} // namespace toolchain
