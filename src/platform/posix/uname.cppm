module;
#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#endif
#include <errno.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

module host;
import std;

namespace host {
    auto executable() -> std::filesystem::path {
#ifdef __APPLE__
        std::uint32_t size = 0;
        ::_NSGetExecutablePath(nullptr, &size);
        std::string buffer(size, '\0');
        if (::_NSGetExecutablePath(buffer.data(), &size) != 0)
            throw std::runtime_error("cannot locate executable");
        return std::filesystem::canonical(buffer.c_str());
#else
        return std::filesystem::canonical("/proc/self/exe");
#endif
    }
    const std::string_view default_data_root = "/srv/data";
    namespace {
        const struct utsname machine = [] {
            struct utsname result{};
            if (::uname(&result) != 0)
                throw std::system_error(errno, std::generic_category(), "uname");
            return result;
        }();
        const std::string system_name = [] {
            std::string result{machine.sysname};
            std::ranges::transform(result, result.begin(), [](unsigned char c) { return std::tolower(c); });
            return result;
        }();
        const std::string machine_name = [] {
            std::string_view name{machine.machine};
            if (name == "amd64")
                return std::string{"x86_64"};
            if (name == "aarch64")
                return std::string{"arm64"};
            return std::string{name};
        }();
    } // namespace
    const std::string_view os = system_name;
    const std::string_view arch = machine_name;
    void set_environment_if_absent(const std::string& name, const std::string& value) {
        if (::setenv(name.c_str(), value.c_str(), 0) != 0)
            throw std::runtime_error("cannot set .env variable " + name);
    }
    auto environment() -> std::map<std::string, std::string> {
        std::map<std::string, std::string> env;
        for (char** e = environ; *e; ++e) {
            std::string_view kv{*e};
            if (auto eq = kv.find('='); eq != std::string_view::npos)
                env.emplace(kv.substr(0, eq), kv.substr(eq + 1));
        }
        return env;
    }

    auto resolve_tools(const std::map<std::string, std::string>& mapped, const std::vector<std::string>& wanted) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> found;
        auto path = std::getenv("PATH");
        for (auto dir : std::views::split(std::string_view{path ? path : ""}, ':')) {
            std::error_code ec;
            auto folder = std::string{dir.begin(), dir.end()};
            if (folder.empty())
                folder = ".";
            if (!wanted.empty()) {
                for (const auto& name : wanted) {
                    auto file = std::filesystem::path{folder} / name;
                    if (std::filesystem::is_regular_file(file, ec) && !ec && ::access(file.c_str(), X_OK) == 0)
                        found.try_emplace(name, std::filesystem::absolute(file).string());
                }
                continue;
            }
            for (const auto& file : std::filesystem::directory_iterator(folder, ec)) {
                if (!wanted.empty() && !std::ranges::contains(wanted, file.path().filename().string()))
                    continue;
                if (!file.is_regular_file(ec) || ec || ::access(file.path().c_str(), X_OK) != 0)
                    continue;
                found.try_emplace(file.path().filename().string(), std::filesystem::absolute(file.path()).string());
            }
        }
        for (const auto& [name, path] : mapped) {
            found.erase(name);
            if (::access(path.c_str(), X_OK) == 0)
                found[name] = std::filesystem::absolute(path).string();
        }
        return found;
    }
    auto detect_tools(const std::map<std::string, std::string>& mapped) -> std::vector<std::string> {
        std::vector<std::string> names;
        for (const auto& [name, path] : resolve_tools(mapped))
            names.push_back(name);
        return names;
    }

    auto name() -> std::string {
        std::array<char, 256> buf{};
        ::gethostname(buf.data(), buf.size() - 1);
        return buf.data();
    }
} // namespace host
