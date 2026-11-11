module;
#include <stdlib.h>
#include <windows.h>
module host;
import std;
import winapi;

namespace host {
    auto executable() -> std::filesystem::path {
        std::wstring buffer(32768, L'\0');
        auto size = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!size || size == buffer.size())
            winapi::fail("GetModuleFileName");
        buffer.resize(size);
        return std::filesystem::path{buffer};
    }
    const std::string_view default_data_root = "C:/ProgramData/service";
    const std::string_view os = "windows";
    namespace {
        const std::string architecture = [] {
            SYSTEM_INFO info{};
            ::GetNativeSystemInfo(&info);
            switch (info.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_AMD64:
                return std::string{"x86_64"};
            case PROCESSOR_ARCHITECTURE_ARM64:
                return std::string{"arm64"};
            case PROCESSOR_ARCHITECTURE_INTEL:
                return std::string{"x86"};
            default:
                return std::to_string(info.wProcessorArchitecture);
            }
        }();
    } // namespace
    const std::string_view arch = architecture;
    auto environment() -> std::map<std::string, std::string> {
        auto block = ::GetEnvironmentStringsW();
        if (!block)
            winapi::fail("GetEnvironmentStrings");
        std::map<std::string, std::string> result;
        try {
            for (auto p = block; *p; p += std::wcslen(p) + 1) {
                std::wstring_view entry{p};
                auto eq = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
                if (eq != entry.npos)
                    result.emplace(winapi::utf8(entry.substr(0, eq)), winapi::utf8(entry.substr(eq + 1)));
            }
        } catch (...) {
            ::FreeEnvironmentStringsW(block);
            throw;
        }
        ::FreeEnvironmentStringsW(block);
        return result;
    }
    void set_environment_if_absent(const std::string& name, const std::string& value) {
        auto key = winapi::wide(name);
        ::SetLastError(ERROR_SUCCESS);
        auto size = ::GetEnvironmentVariableW(key.c_str(), nullptr, 0);
        if (size || ::GetLastError() != ERROR_ENVVAR_NOT_FOUND)
            return;
        if (auto error = ::_putenv_s(name.c_str(), value.c_str()))
            throw std::system_error(error, std::generic_category(), "update CRT environment");
        if (!::SetEnvironmentVariableW(key.c_str(), winapi::wide(value).c_str()))
            winapi::fail("SetEnvironmentVariable");
    }
    auto resolve_tools(const std::map<std::string, std::string>& mapped, const std::vector<std::string>& wanted) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> found;
        auto path = std::getenv("PATH");
        for (auto dir : std::views::split(std::string_view{path ? path : ""}, ';')) {
            std::error_code ec;
            auto folder = std::string{dir.begin(), dir.end()};
            if (folder.empty())
                continue;
            if (!wanted.empty()) {
                for (const auto& name : wanted)
                    for (const auto extension : {".exe", ".com"}) {
                        auto file = std::filesystem::path{folder} / (name + extension);
                        if (std::filesystem::is_regular_file(file, ec) && !ec)
                            found.try_emplace(name, std::filesystem::absolute(file).string());
                    }
                continue;
            }
            for (const auto& file : std::filesystem::directory_iterator(folder, ec)) {
                if (!file.is_regular_file(ec) || ec)
                    continue;
                auto extension = file.path().extension().string();
                std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return std::tolower(c); });
                if (extension != ".exe" && extension != ".com")
                    continue;
                auto name = file.path().stem().string();
                std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
                if (!wanted.empty() && !std::ranges::contains(wanted, name))
                    continue;
                found.try_emplace(name, std::filesystem::absolute(file.path()).string());
            }
        }
        for (const auto& [name, path] : mapped) {
            found.erase(name);
            auto wanted = winapi::wide(path);
            auto count = ::SearchPathW(nullptr, wanted.c_str(), L".exe", 0, nullptr, nullptr);
            if (!count)
                continue;
            std::wstring resolved(count + 1, L'\0');
            auto used = ::SearchPathW(nullptr, wanted.c_str(), L".exe", static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
            if (used && used < resolved.size())
                found[name] = winapi::utf8(std::wstring_view{resolved.data(), used});
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
        DWORD count = 0;
        ::GetComputerNameExW(ComputerNameDnsHostname, nullptr, &count);
        std::wstring text(count, L'\0');
        if (!::GetComputerNameExW(ComputerNameDnsHostname, text.data(), &count))
            winapi::fail("GetComputerNameEx");
        return winapi::utf8(std::wstring_view{text.data(), count});
    }
} // namespace host
