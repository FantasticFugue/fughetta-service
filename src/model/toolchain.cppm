export module toolchain;
import std;
import content;
import reflection;
import host;

export namespace toolchain {
    inline auto required_tools() -> std::vector<std::string> {
        return {"cmake",   "ctest", "bazel",  "ninja",   "make",
                "cc",      "c++",   "gcc",    "g++",     "clang",
                "clang++", "cl",    "link",   "ld",      "lld",
                "mold",    "vcpkg", "python", "python3", "uv",
                "ruff",    "ty",    "clice",  "tsc",     "typescript-language-server",
                "node",    "npm",   "bun",    "git"};
    }

    struct identity {
        std::string id;
        std::vector<std::string> tools;
    };

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
                 const std::map<std::string, std::string>& sdk_manifests = {}) -> identity {
        identity out;
        std::map<std::string, std::string> digests, binaries, sdks;
        for (const auto& [name, path] : resolved) {
            try {
                auto canonical = std::filesystem::canonical(path).string();
                auto found = digests.find(canonical);
                if (found == digests.end())
                    found = digests.emplace(canonical, digest(canonical)).first;
                binaries[name] = found->second;
                out.tools.push_back(name);
            } catch (const std::exception&) {
                // A missing or unreadable executable cannot be offered, but other tools remain usable.
            }
        }
        for (const auto& [name, path] : sdk_manifests)
            sdks[name] = digest(path);
        // Distribution manifests include compiler runtime and system-header package versions.
        for (auto path : {"/var/lib/dpkg/status", "/var/lib/rpm/rpmdb.sqlite", "/usr/lib/sysimage/rpm/rpmdb.sqlite"})
            if (std::filesystem::is_regular_file(path))
                sdks[path] = digest(path);
        for (auto variable : {"JAVA_HOME", "ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_NDK_HOME"}) {
            auto found = environment.find(variable);
            if (found == environment.end())
                continue;
            auto root = std::filesystem::path{found->second};
            for (auto name : {"release", "source.properties"})
                if (std::filesystem::is_regular_file(root / name))
                    sdks[std::string{variable} + "/" + name] = digest(root / name);
            for (auto group : {"platforms", "build-tools", "ndk", "cmdline-tools"}) {
                std::error_code error;
                for (const auto& entry : std::filesystem::directory_iterator(root / group, error)) {
                    auto manifest = entry.path() / "source.properties";
                    if (std::filesystem::is_regular_file(manifest))
                        sdks[std::string{variable} + "/" + group + "/" + entry.path().filename().string()] = digest(manifest);
                }
            }
        }
        out.id = content::hash_of(rfl::json::write(std::tuple{std::string{host::os}, std::string{host::arch}, binaries, sdks, environment})).str();
        return out;
    }
} // namespace toolchain
