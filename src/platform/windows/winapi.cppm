module;
#include <windows.h>
export module winapi;
import std;

export namespace winapi {
    inline auto path(const std::filesystem::path& value) -> std::wstring {
        auto full = std::filesystem::absolute(value).lexically_normal().make_preferred().native();
        if (full.starts_with(L"\\\\?\\"))
            return full;
        if (full.starts_with(L"\\\\"))
            return L"\\\\?\\UNC\\" + full.substr(2);
        return L"\\\\?\\" + full;
    }
    [[noreturn]] inline void fail(const char* what, DWORD error = ::GetLastError()) {
        throw std::system_error(static_cast<int>(error), std::system_category(), what);
    }
    struct handle {
        HANDLE value = nullptr;
        explicit handle(HANDLE h = nullptr) noexcept : value(h) {}
        handle(const handle&) = delete;
        handle(handle&& h) noexcept : value(std::exchange(h.value, nullptr)) {}
        auto operator=(handle&& h) noexcept -> handle& {
            if (this != &h) {
                reset();
                value = std::exchange(h.value, nullptr);
            }
            return *this;
        }
        ~handle() { reset(); }
        void reset() noexcept {
            if (value && value != INVALID_HANDLE_VALUE)
                ::CloseHandle(value);
            value = nullptr;
        }
    };
    inline auto wide(std::string_view text) -> std::wstring {
        if (text.empty())
            return {};
        auto n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (!n)
            fail("UTF-8 conversion");
        std::wstring result(n, L'\0');
        if (!::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), n))
            fail("UTF-8 conversion");
        return result;
    }
    inline auto utf8(std::wstring_view text) -> std::string {
        if (text.empty())
            return {};
        auto n = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (!n)
            fail("UTF-16 conversion");
        std::string result(n, '\0');
        if (!::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), n, nullptr, nullptr))
            fail("UTF-16 conversion");
        return result;
    }
    inline void limits(HANDLE job, std::optional<std::uint64_t> memory, std::optional<std::uint32_t> cpu) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (memory) {
            info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            info.JobMemoryLimit = *memory;
        }
        if (cpu) {
            info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_TIME;
            info.BasicLimitInformation.PerJobUserTimeLimit.QuadPart = std::uint64_t{*cpu} * 10000000;
        }
        if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info))
            fail("SetInformationJobObject");
    }
} // namespace winapi
