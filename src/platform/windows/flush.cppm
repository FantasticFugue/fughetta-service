module;
#include <aclapi.h>
#include <windows.h>
module durability;
import std;
import constants;
import utility;
import winapi;

namespace durability {
    auto is_executable(const std::filesystem::path& path) -> bool {
        auto stream = path;
        stream += L":executable";
        winapi::handle file{::CreateFileW(winapi::path(stream).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, 0, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) {
            auto error = ::GetLastError();
            if (error == ERROR_FILE_NOT_FOUND)
                return false;
            winapi::fail("read executable metadata", error);
        }
        char value = 0;
        DWORD count = 0;
        if (!::ReadFile(file.value, &value, 1, &count, nullptr))
            winapi::fail("read executable metadata");
        return count == 1 && value == '1';
    }
    void set_executable(const std::filesystem::path& path, bool executable) {
        auto stream = path;
        stream += L":executable";
        if (!executable) {
            if (!::DeleteFileW(winapi::path(stream).c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND)
                winapi::fail("clear executable metadata");
            return;
        }
        winapi::handle file{::CreateFileW(winapi::path(stream).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE)
            winapi::fail("create executable metadata");
        DWORD count = 0;
        if (!::WriteFile(file.value, "1", 1, &count, nullptr) || count != 1)
            winapi::fail("write executable metadata");
        if (!::FlushFileBuffers(file.value))
            winapi::fail("flush executable metadata");
    }
    namespace {
        auto open(const std::filesystem::path& path, DWORD access, DWORD disposition = OPEN_EXISTING) -> winapi::handle {
            winapi::handle file{::CreateFileW(winapi::path(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr)};
            if (file.value == INVALID_HANDLE_VALUE)
                winapi::fail("open durable file");
            return file;
        }
        void flush(HANDLE file) {
            if (!::FlushFileBuffers(file))
                winapi::fail("FlushFileBuffers");
        }
        void write(HANDLE file, std::span<const std::byte> bytes) {
            while (!bytes.empty()) {
                DWORD count = 0;
                if (!::WriteFile(file, bytes.data(), static_cast<DWORD>(std::min<std::size_t>(bytes.size(), MAXDWORD)), &count, nullptr))
                    winapi::fail("WriteFile durable data");
                if (!count)
                    throw std::runtime_error("zero byte durable write");
                bytes = bytes.subspan(count);
            }
        }
    } // namespace
    void write_part(const std::filesystem::path& path, std::uint64_t offset, std::span<const std::byte> bytes, bool complete) {
        auto file = open(path, GENERIC_WRITE, OPEN_ALWAYS);
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
            throw std::invalid_argument("file offset exceeds Windows limit");
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!::SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN))
            winapi::fail("SetFilePointerEx");
        write(file.value, bytes);
        if (complete)
            flush(file.value);
    }
    void sync_dir(const std::filesystem::path& dir) {
        auto path = dir.empty() ? std::filesystem::path{L"."} : dir;
        winapi::handle handle{::CreateFileW(winapi::path(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
        if (handle.value == INVALID_HANDLE_VALUE)
            winapi::fail("open publication directory");
        flush(handle.value);
    }
    void sync_filesystem(const std::filesystem::path& path) {
        std::vector<std::filesystem::path> directories{path};
        for (auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_symlink() && entry.is_directory())
                directories.push_back(entry.path());
            if (entry.is_symlink() || !entry.is_regular_file())
                continue;
            auto file = open(entry.path(), GENERIC_WRITE);
            flush(file.value);
        }
        for (auto& dir : std::views::reverse(directories))
            sync_dir(dir);
    }
    void durable_rename(const std::filesystem::path& from, const std::filesystem::path& to) {
        if (!::MoveFileExW(winapi::path(from).c_str(), winapi::path(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            winapi::fail("MoveFileEx publication");
        sync_dir(to.parent_path());
        if (from.parent_path() != to.parent_path())
            sync_dir(from.parent_path());
    }
    void durable_write(const std::filesystem::path& target, std::span<const std::byte> bytes, bool executable, std::string_view suffix) {
        auto temp = target;
        temp += suffix;
        try {
            {
                // A temporary left behind, or planted as a link, is removed by its own name and created anew, never written through.
                if (!::DeleteFileW(winapi::path(temp).c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND)
                    winapi::fail("remove a stale temporary");
                auto file = open(temp, GENERIC_WRITE, CREATE_NEW);
                write(file.value, bytes);
                flush(file.value);
            }
            set_executable(temp, executable);
            durable_rename(temp, target);
        } catch (...) {
            ::DeleteFileW(winapi::path(temp).c_str());
            throw;
        }
    }
    void durable_write(const std::filesystem::path& target, std::string_view text, bool executable, std::string_view suffix) {
        durable_write(target, std::as_bytes(std::span{text}), executable, suffix);
    }
    void durable_private_write(const std::filesystem::path& target, std::string_view text) {
        HANDLE token_value = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token_value))
            winapi::fail("read credential owner");
        winapi::handle token{token_value};
        DWORD size = 0;
        ::GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
        std::vector<std::byte> user(size);
        if (!::GetTokenInformation(token.value, TokenUser, user.data(), size, &size))
            winapi::fail("read credential owner SID");
        auto sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_ALL_ACCESS;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
        PACL acl = nullptr;
        auto status = ::SetEntriesInAclW(1, &access, nullptr, &acl);
        if (status != ERROR_SUCCESS)
            winapi::fail("create credential ACL", status);
        struct free_acl {
            PACL acl;
            ~free_acl() { ::LocalFree(acl); }
        } free{acl};
        SECURITY_DESCRIPTOR descriptor{};
        if (!::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) || !::SetSecurityDescriptorOwner(&descriptor, sid, FALSE) ||
            !::SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) || !::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
            winapi::fail("protect credential descriptor");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), &descriptor, FALSE};
        auto temporary = target;
        temporary += L".private-write";
        try {
            if (!::DeleteFileW(winapi::path(temporary).c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND)
                winapi::fail("remove credential temporary");
            {
                winapi::handle file{
                    ::CreateFileW(winapi::path(temporary).c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_FLAG_WRITE_THROUGH, nullptr)};
                if (file.value == INVALID_HANDLE_VALUE)
                    winapi::fail("create private credentials");
                write(file.value, std::as_bytes(std::span{text}));
                flush(file.value);
            }
            durable_rename(temporary, target);
        } catch (...) {
            ::DeleteFileW(winapi::path(temporary).c_str());
            throw;
        }
    }
    namespace {
        auto owner_only(HANDLE file) -> bool {
            PSID owner = nullptr;
            PACL acl = nullptr;
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            auto status =
                ::GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &descriptor);
            struct free_descriptor {
                PSECURITY_DESCRIPTOR value;
                ~free_descriptor() {
                    if (value)
                        ::LocalFree(value);
                }
            } free{descriptor};
            if (status != ERROR_SUCCESS || !owner || !acl || !::IsValidAcl(acl))
                return false;
            HANDLE value = nullptr;
            if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &value))
                return false;
            winapi::handle token{value};
            DWORD size = 0;
            ::GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
            std::vector<std::byte> user(size);
            if (!::GetTokenInformation(token.value, TokenUser, user.data(), size, &size))
                return false;
            auto sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
            if (!::EqualSid(owner, sid))
                return false;
            for (DWORD i = 0; i < acl->AceCount; ++i) {
                void* entry = nullptr;
                if (!::GetAce(acl, i, &entry))
                    return false;
                auto header = static_cast<ACE_HEADER*>(entry);
                if (header->AceType == ACCESS_DENIED_ACE_TYPE)
                    continue;
                if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || !::EqualSid(&static_cast<ACCESS_ALLOWED_ACE*>(entry)->SidStart, sid))
                    return false;
            }
            return true;
        }
        auto read_checked(std::filesystem::path path, std::uint64_t limit, bool secret) -> util::generator<std::span<const std::byte>> {
            winapi::handle file{::CreateFileW(winapi::path(path).c_str(), GENERIC_READ | (secret ? READ_CONTROL : 0), FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            if (file.value == INVALID_HANDLE_VALUE)
                throw unreadable{path.string()};
            if (secret && !owner_only(file.value))
                throw unreadable{path.string()};
            BY_HANDLE_FILE_INFORMATION info{};
            if (!::GetFileInformationByHandle(file.value, &info) || ::GetFileType(file.value) != FILE_TYPE_DISK ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
                throw unreadable{path.string()};
            auto size = (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
            if (size > limit)
                throw unreadable{path.string()};
            std::array<std::byte, 1 << 16> buffer;
            std::uint64_t total = 0;
            for (;;) {
                DWORD n = 0;
                if (!::ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr))
                    throw unreadable{path.string()};
                if (n == 0)
                    break;
                total += n;
                if (total > size)
                    throw unreadable{path.string()};
                co_yield std::span<const std::byte>{buffer.data(), n};
            }
            if (total != size)
                throw unreadable{path.string()};
        }
    } // namespace
    auto read_plain(const std::filesystem::path& path, std::uint64_t limit) -> util::generator<std::span<const std::byte>> {
        return read_checked(path, limit, false);
    }
    auto read_private(const std::filesystem::path& path, std::uint64_t limit) -> util::generator<std::span<const std::byte>> {
        return read_checked(path, limit, true);
    }
    void durable_append(const std::filesystem::path& path, std::string_view record) {
        bool fresh = !std::filesystem::exists(path);
        auto file = open(path, FILE_APPEND_DATA, OPEN_ALWAYS);
        write(file.value, std::as_bytes(std::span{record}));
        flush(file.value);
        file.reset();
        if (fresh)
            sync_dir(path.parent_path());
    }
    void durable_remove(const std::filesystem::path& path) {
        winapi::handle file{
            ::CreateFileW(winapi::path(path).c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) {
            auto error = ::GetLastError();
            if (error != ERROR_FILE_NOT_FOUND)
                winapi::fail("open file for deletion", error);
        } else {
            FILE_DISPOSITION_INFO_EX disposition{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
            if (!::SetFileInformationByHandle(file.value, FileDispositionInfoEx, &disposition, sizeof disposition))
                winapi::fail("delete durable data");
            file.reset();
        }
        sync_dir(path.parent_path());
    }
    void durable_directories(const std::filesystem::path& path) {
        std::vector<std::filesystem::path> made;
        for (auto p = path; !p.empty() && !std::filesystem::exists(p); p = p.parent_path()) {
            made.push_back(p);
            if (p == p.parent_path())
                break;
        }
        if (!path.empty())
            std::filesystem::create_directories(path);
        for (auto& p : made) {
            sync_dir(p);
            sync_dir(p.parent_path());
        }
    }
    struct dir_lock::state {
        winapi::handle file;
        state(const std::filesystem::path& dir, std::string_view what) {
            std::filesystem::create_directories(dir);
            file = open(dir / constants::lock_file, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS);
            OVERLAPPED offset{};
            if (!::LockFileEx(file.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &offset)) {
                if (::GetLastError() == ERROR_LOCK_VIOLATION)
                    throw lock_busy(std::format("{} {} is in use by another process", what, dir.string()));
                winapi::fail("LockFileEx directory ownership");
            }
        }
    };
    dir_lock::dir_lock(const std::filesystem::path& dir, std::string_view what) : state_(std::make_unique<state>(dir, what)) {}
    dir_lock::dir_lock(dir_lock&&) noexcept = default;
    auto dir_lock::operator=(dir_lock&&) noexcept -> dir_lock& = default;
    dir_lock::~dir_lock() = default;
} // namespace durability
namespace files {
    mapping::mapping(const std::filesystem::path& path) {
        winapi::handle file{::CreateFileW(winapi::path(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, 0, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE)
            winapi::fail("open mapped file");
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file.value, &size))
            winapi::fail("GetFileSizeEx");
        size_ = static_cast<std::size_t>(size.QuadPart);
        if (!size_)
            return;
        winapi::handle section{::CreateFileMappingW(file.value, nullptr, PAGE_READONLY, 0, 0, nullptr)};
        if (!section.value)
            winapi::fail("CreateFileMapping");
        data_ = static_cast<const std::byte*>(::MapViewOfFile(section.value, FILE_MAP_READ, 0, 0, 0));
        if (!data_)
            winapi::fail("MapViewOfFile");
    }
    mapping::~mapping() {
        if (data_)
            ::UnmapViewOfFile(data_);
    }
} // namespace files
