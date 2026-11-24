module;
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>
#include <initguid.h>
#include <virtdisk.h>
#include <computestorage.h>

export module host_windows_job;
import std;
import configuration;
import host_operation;
import host_windows_firewall;
import winapi;

export namespace host_windows_job {
    namespace fs = std::filesystem;
    struct package {
        PSID sid{};
        explicit package(const std::string& id) {
            const auto name = winapi::wide("Fughetta." + id);
            auto status = ::CreateAppContainerProfile(name.c_str(), name.c_str(), L"Fughetta workspace", nullptr, 0, &sid);
            if (status == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
                status = ::DeriveAppContainerSidFromAppContainerName(name.c_str(), &sid);
            if (FAILED(status)) winapi::fail("create workspace AppContainer profile", status);
        }
        ~package() { ::FreeSid(sid); }
        package(const package&) = delete;
    };

    inline auto account_user() -> std::vector<std::byte> {
        winapi::handle account;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &account.value)) winapi::fail("open workspace account");
        DWORD bytes_needed{};
        ::GetTokenInformation(account.value, TokenUser, nullptr, 0, &bytes_needed);
        std::vector<std::byte> user(bytes_needed);
        if (!::GetTokenInformation(account.value, TokenUser, user.data(), bytes_needed, &bytes_needed))
            winapi::fail("read workspace account");
        return user;
    }

    inline void grant(const fs::path& path, PSID sid, DWORD rights, bool inherit, bool read_only = false) {
        winapi::handle file{::CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) winapi::fail("open workspace ACL");
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!::GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &attributes, sizeof(attributes)))
            winapi::fail("inspect workspace ACL target");
        if (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("workspace ACL target is a reparse point");
        PACL old{}, updated{};
        PSECURITY_DESCRIPTOR sd{};
        auto status = ::GetSecurityInfo(file.value, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old, nullptr, &sd);
        if (status != ERROR_SUCCESS) winapi::fail("read workspace DACL", status);
        std::unique_ptr<void, decltype(&::LocalFree)> owner{sd, ::LocalFree};
        if (!old) throw std::runtime_error("workspace directory requires a DACL");
        constexpr DWORD mutations = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                                    FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;
        const DWORD propagation = inherit ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
        bool allowed = false, denied = !read_only;
        for (DWORD i = 0; i < old->AceCount; ++i) {
            void* entry{};
            if (!::GetAce(old, i, &entry)) winapi::fail("read workspace ACE");
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceType != ACCESS_DENIED_ACE_TYPE)
                continue;
            if ((ace->Header.AceFlags & (INHERIT_ONLY_ACE | NO_PROPAGATE_INHERIT_ACE)) ||
                (ace->Header.AceFlags & propagation) != propagation || !::EqualSid(sid, const_cast<DWORD*>(&ace->SidStart)))
                continue;
            if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Mask == rights) allowed = true;
            if (ace->Header.AceType == ACCESS_DENIED_ACE_TYPE && (ace->Mask & mutations) == mutations) denied = true;
        }
        if (allowed && denied) return;
        EXPLICIT_ACCESSW ace{};
        ace.grfAccessPermissions = rights;
        ace.grfAccessMode = SET_ACCESS;
        ace.grfInheritance = propagation;
        ace.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ace.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        status = ::SetEntriesInAclW(1, &ace, old, &updated);
        if (status != ERROR_SUCCESS) winapi::fail("prepare AppContainer DACL", status);
        std::unique_ptr<void, decltype(&::LocalFree)> release{updated, ::LocalFree};
        std::unique_ptr<void, decltype(&::LocalFree)> protected_acl{nullptr, ::LocalFree};
        if (read_only) {
            ace.grfAccessMode = DENY_ACCESS;
            ace.grfAccessPermissions = mutations;
            PACL restricted{};
            status = ::SetEntriesInAclW(1, &ace, updated, &restricted);
            if (status != ERROR_SUCCESS) winapi::fail("protect read-only workspace mount", status);
            protected_acl.reset(restricted);
            updated = restricted;
        }
        status = ::SetSecurityInfo(file.value, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
        if (status != ERROR_SUCCESS) winapi::fail("grant workspace AppContainer access", status);
    }

    inline void integrity_label(const fs::path& path, const wchar_t* label) {
        PSECURITY_DESCRIPTOR sd{};
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(label, SDDL_REVISION_1, &sd, nullptr))
            winapi::fail("build workspace integrity label");
        std::unique_ptr<void, decltype(&::LocalFree)> owner{sd, ::LocalFree};
        BOOL present{}, defaulted{};
        PACL sacl{};
        if (!::GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted) || !present)
            throw std::runtime_error("missing workspace integrity label");
        const auto result = ::SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
            LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, sacl);
        if (result != ERROR_SUCCESS) winapi::fail("set workspace integrity label", result);
    }

    inline auto drive_for(const std::wstring& volume, bool assign) -> fs::path {
        const auto used = ::GetLogicalDrives();
        if (!used) winapi::fail("list workspace drive letters");
        for (unsigned i = 2; i < 26; ++i) {
            if (!(used & (1u << i))) continue;
            std::wstring drive{L"A:\\"};
            drive[0] += static_cast<wchar_t>(i);
            wchar_t target[MAX_PATH]{};
            if (::GetVolumeNameForVolumeMountPointW(drive.c_str(), target, MAX_PATH) && !::_wcsicmp(target, volume.c_str()))
                return drive;
        }
        if (!assign) return {};
        for (unsigned i = 3; i < 26; ++i) {
            if (used & (1u << i)) continue;
            std::wstring drive{L"A:\\"};
            drive[0] += static_cast<wchar_t>(i);
            if (::SetVolumeMountPointW(drive.c_str(), volume.c_str())) return drive;
            const auto code = ::GetLastError();
            if (code != ERROR_DIR_NOT_EMPTY && code != ERROR_ALREADY_ASSIGNED && code != ERROR_ALREADY_EXISTS)
                winapi::fail("assign workspace drive letter", code);
        }
        throw std::runtime_error("no drive letter is available for the workspace");
    }

    inline auto disk_handle(const fs::path& path) -> winapi::handle {
        VIRTUAL_STORAGE_TYPE type{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
        OPEN_VIRTUAL_DISK_PARAMETERS parameters{OPEN_VIRTUAL_DISK_VERSION_1};
        parameters.Version1.RWDepth = OPEN_VIRTUAL_DISK_RW_DEPTH_DEFAULT;
        winapi::handle disk;
        const auto status = ::OpenVirtualDisk(&type, path.c_str(), static_cast<VIRTUAL_DISK_ACCESS_MASK>(
            VIRTUAL_DISK_ACCESS_ATTACH_RW | VIRTUAL_DISK_ACCESS_DETACH | VIRTUAL_DISK_ACCESS_GET_INFO),
            OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, &disk.value);
        if (status != ERROR_SUCCESS) winapi::fail("open workspace scratch VHDX", status);
        return disk;
    }

    inline auto loaded(HANDLE disk) -> bool {
        GET_VIRTUAL_DISK_INFO info{GET_VIRTUAL_DISK_INFO_IS_LOADED};
        ULONG size = sizeof(info);
        const auto status = ::GetVirtualDiskInformation(disk, &size, &info, nullptr);
        if (status != ERROR_SUCCESS) winapi::fail("inspect attached workspace VHDX", status);
        return info.IsLoaded;
    }

    inline auto mount(const fs::path& vhd, const fs::path& root, const std::string& id) -> fs::path {
        const auto disk = disk_handle(vhd);
        package profile{id};
        if (!loaded(disk.value)) {
            PWSTR package_sid{};
            if (!::ConvertSidToStringSidW(profile.sid, &package_sid)) winapi::fail("format workspace volume identity");
            std::unique_ptr<wchar_t, decltype(&::LocalFree)> sid_owner{package_sid, ::LocalFree};
            auto account = account_user();
            PWSTR account_sid{};
            if (!::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(account.data())->User.Sid, &account_sid))
                winapi::fail("format workspace account");
            std::unique_ptr<wchar_t, decltype(&::LocalFree)> account_owner{account_sid, ::LocalFree};
            // The volume device also checks access before NTFS applies the per-directory DACL.
            // Atomic replacement and removal need DELETE on this private device as well as on the files.
            constexpr DWORD work_access = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | FILE_DELETE_CHILD | DELETE;
            const auto policy = std::format(L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x{:x};;;{})(A;;0x{:x};;;{})S:(ML;;NW;;;LW)",
                                            work_access, account_sid, work_access, package_sid);
            PSECURITY_DESCRIPTOR descriptor{};
            if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(policy.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
                winapi::fail("prepare workspace volume permissions");
            std::unique_ptr<void, decltype(&::LocalFree)> descriptor_owner{descriptor, ::LocalFree};
            ATTACH_VIRTUAL_DISK_PARAMETERS params{ATTACH_VIRTUAL_DISK_VERSION_1};
            const auto code = ::AttachVirtualDisk(disk.value, descriptor, static_cast<ATTACH_VIRTUAL_DISK_FLAG>(
                ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER | ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME), 0, &params, nullptr);
            if (code != ERROR_SUCCESS) winapi::fail("attach workspace scratch VHDX", code);
        }
        PWSTR volume{};
        const auto status = ::HcsGetLayerVhdMountPath(disk.value, &volume);
        if (FAILED(status)) winapi::fail("locate workspace scratch volume", status);
        std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> release{volume, ::CoTaskMemFree};
        std::wstring name{volume};
        if (!name.ends_with(L"\\")) name += L"\\";
        integrity_label(fs::path{name}, L"S:(ML;;NW;;;ME)");
        grant(fs::path{name}, profile.sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false);
        const auto dir = root / "disk";
        fs::create_directories(dir);
        const auto point = dir.wstring() + L"\\";
        wchar_t existing[MAX_PATH]{};
        if (::GetVolumeNameForVolumeMountPointW(point.c_str(), existing, MAX_PATH)) {
            if (::_wcsicmp(existing, name.c_str()) != 0)
                throw std::runtime_error("another volume occupies the workspace mount point");
        } else if ((::GetFileAttributesW(dir.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) ||
                   !::SetVolumeMountPointW(point.c_str(), name.c_str())) {
            winapi::fail("mount workspace scratch volume");
        }
        return drive_for(name, true);
    }

    inline auto job_name(const std::string& id) -> std::wstring { return L"Global\\Fughetta-" + winapi::wide(id); }

    inline auto pid(HANDLE job) -> DWORD {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (!::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr))
            winapi::fail("read workspace job");
        if (!info.ActiveProcesses) return 0;
        std::vector<ULONG_PTR> members(info.ActiveProcesses + 2);
        auto* pids = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(members.data());
        if (!::QueryInformationJobObject(job, JobObjectBasicProcessIdList, pids, static_cast<DWORD>(members.size() * sizeof(ULONG_PTR)), nullptr))
            winapi::fail("read workspace process IDs");
        return pids->NumberOfProcessIdsInList ? static_cast<DWORD>(pids->ProcessIdList[0]) : 0;
    }

    inline auto running_jobs() -> std::map<std::string, winapi::handle>& {
        static std::map<std::string, winapi::handle> jobs;
        return jobs;
    }

    inline void stop(const std::string& id, const fs::path& disk_path, const fs::path& root, DWORD timeout) {
        winapi::handle job{::OpenJobObjectW(JOB_OBJECT_QUERY | JOB_OBJECT_TERMINATE, FALSE, job_name(id).c_str())};
        if (!job.value && ::GetLastError() != ERROR_FILE_NOT_FOUND) winapi::fail("open workspace job");
        if (job.value && pid(job.value)) {
            if (!::TerminateJobObject(job.value, 1)) winapi::fail("stop workspace job");
            const auto deadline = ::GetTickCount64() + std::uint64_t{timeout} * 1000;
            while (pid(job.value)) {
                if (::GetTickCount64() >= deadline) throw std::runtime_error("workspace job did not exit");
                ::Sleep(10);
            }
        }
        running_jobs().erase(id);
        if (!disk_path.empty()) {
            const auto disk = disk_handle(disk_path);
            if (loaded(disk.value)) {
                const auto point = (root / "disk").wstring() + L"\\";
                wchar_t volume[MAX_PATH]{};
                if (::GetVolumeNameForVolumeMountPointW(point.c_str(), volume, MAX_PATH)) {
                    const auto drive = drive_for(volume, false);
                    if (!drive.empty() && !::DeleteVolumeMountPointW(drive.c_str())) winapi::fail("release workspace drive letter");
                    if (!::DeleteVolumeMountPointW(point.c_str())) winapi::fail("unmount workspace scratch volume");
                }
                const auto code = ::DetachVirtualDisk(disk.value, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
                if (code != ERROR_SUCCESS) winapi::fail("detach workspace scratch VHDX", code);
            }
        }
        host_windows_firewall::transaction{id}.remove();
        const auto removed = ::DeleteAppContainerProfile(winapi::wide("Fughetta." + id).c_str());
        if (FAILED(removed) && removed != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
            removed != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && removed != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
            winapi::fail("remove workspace AppContainer profile", removed);
    }

    inline auto start(const config::host_settings& host, const host_operation::operation& op, const std::string& id,
                      const fs::path& data, const fs::path& config, const fs::path& tools, const std::string& peer,
                      const std::string& loopback) -> DWORD {
        winapi::handle job{::CreateJobObjectW(nullptr, job_name(id).c_str())};
        if (!job.value) winapi::fail("create workspace job");
        if (auto running = pid(job.value)) {
            running_jobs().insert_or_assign(id, std::move(job));
            return running;
        }
        package profile{id};
        const auto mount_point = data.root_path();
        for (auto parent = data.parent_path(); parent != mount_point; parent = parent.parent_path()) {
            if (parent == parent.root_path()) throw std::runtime_error("workspace data is outside package storage");
            integrity_label(parent, L"S:(ML;;NW;;;ME)");
            grant(parent, profile.sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false);
        }
        integrity_label(data, L"S:(ML;OICI;NW;;;LW)");
        constexpr DWORD work_rights = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | FILE_DELETE_CHILD | DELETE;
        auto user = account_user();
        grant(data, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, work_rights, true);
        grant(data, profile.sid, work_rights, true);
        if (op.kind == host_operation::environment_kind::core) {
            const auto state = data / "core";
            fs::create_directory(state);
            if (::GetFileAttributesW(state.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
                throw std::runtime_error("workspace storage must be a directory");
            integrity_label(state, L"S:(ML;OICI;NW;;;LW)");
            grant(state, profile.sid, work_rights, true);
        }
        grant(host.rootfs, profile.sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true, true);
        grant(tools, profile.sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true, true);
        if (!host.shared_cache.empty()) grant(host.shared_cache, profile.sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true, true);
        host_windows_firewall::transaction{id}.install(profile.sid, host, op, peer, loopback);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        limit.JobMemoryLimit = op.contract.limits.memory_bytes;
        limit.BasicLimitInformation.ActiveProcessLimit = host.pids;
        if (!::SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limit, sizeof(limit)))
            winapi::fail("apply workspace memory and process limits");
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
        cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        cpu.CpuRate = static_cast<DWORD>(std::min<std::uint64_t>(10000, std::uint64_t{op.contract.limits.cpu_millis} * 10 /
            std::max<DWORD>(1, ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS))));
        if (!::SetInformationJobObject(job.value, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu)))
            winapi::fail("apply workspace CPU rate");
        SECURITY_CAPABILITIES security{};
        security.AppContainerSid = profile.sid;
        BYTE capabilities[2][SECURITY_MAX_SID_SIZE]{};
        SID_AND_ATTRIBUTES network[2]{};
        const auto add_capability = [&](WELL_KNOWN_SID_TYPE type) {
            const auto index = security.CapabilityCount;
            DWORD size = sizeof(capabilities[index]);
            if (!::CreateWellKnownSid(type, nullptr, capabilities[index], &size))
                winapi::fail("build workspace network capability");
            network[index] = {capabilities[index], SE_GROUP_ENABLED};
            security.Capabilities = network;
            ++security.CapabilityCount;
        };
        if (op.kind == host_operation::environment_kind::core || op.contract.boundary.network == "private") {
            add_capability(WinCapabilityPrivateNetworkClientServerSid);
            if (op.contract.boundary.network == "private")
                add_capability(WinCapabilityInternetClientSid);
        }
        SIZE_T bytes{};
        ::InitializeProcThreadAttributeList(nullptr, 3, 0, &bytes);
        std::vector<std::byte> storage(bytes);
        auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!::InitializeProcThreadAttributeList(attrs, 3, 0, &bytes)) winapi::fail("create workspace launch attributes");
        struct attributes { LPPROC_THREAD_ATTRIBUTE_LIST ptr; ~attributes() { ::DeleteProcThreadAttributeList(ptr); } } cleanup{attrs};
        if (!::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &security, sizeof(security), nullptr, nullptr) ||
            !::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.value, sizeof(HANDLE), nullptr, nullptr))
            winapi::fail("bind workspace AppContainer and job");
        const auto executable = fs::path{host.rootfs} / "fughetta-service.exe";
        auto command = L"\"" + executable.wstring() + L"\" " +
            (op.kind == host_operation::environment_kind::overflow ? L"--role worker " : L"--workspace-config ") + L"-";
        wchar_t system[MAX_PATH]{};
        if (!::GetWindowsDirectoryW(system, MAX_PATH)) winapi::fail("locate Windows directory");
        std::map<std::wstring, std::wstring> values{{L"HOME", data}, {L"PATH", (tools / "bin").wstring() + L";" + system + L"\\System32"},
            {L"SystemRoot", system}, {L"TEMP", data}, {L"TMP", data}, {L"USERPROFILE", data}};
        values[L"APPDATA"] = values[L"LOCALAPPDATA"] = data;
        values[L"SystemDrive"] = fs::path{system}.root_name().wstring();
        values[L"windir"] = system;
        values[L"ComSpec"] = (fs::path{system} / "System32" / "cmd.exe").wstring();
        for (const auto* name : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"ProgramData",
                                 L"CommonProgramFiles", L"CommonProgramFiles(x86)", L"CommonProgramW6432", L"PUBLIC",
                                 L"ALLUSERSPROFILE", L"PROCESSOR_ARCHITECTURE", L"OS"}) {
            const auto size = ::GetEnvironmentVariableW(name, nullptr, 0);
            if (!size) continue;
            std::wstring value(size, L'\0');
            const auto copied = ::GetEnvironmentVariableW(name, value.data(), size);
            if (copied && copied < size) { value.resize(copied); values.emplace(name, std::move(value)); }
        }
        winapi::handle ready{::CreateEventW(nullptr, TRUE, FALSE, nullptr)}, notify;
        if (!ready.value || !::DuplicateHandle(::GetCurrentProcess(), ready.value, ::GetCurrentProcess(), &notify.value,
            EVENT_MODIFY_STATE, TRUE, 0)) winapi::fail("create workspace readiness event");
        values[L"FUG_HOST_READY_HANDLE"] = std::to_wstring(reinterpret_cast<std::uintptr_t>(notify.value));
        std::wstring environment;
        for (const auto& [key, value] : values) {
            environment += key + L"=" + value;
            environment.push_back(L'\0');
        }
        environment.push_back(L'\0');
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        winapi::handle log{::CreateFileW((data / "supervisor.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inherit, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        winapi::handle input{::CreateFileW(config.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, &inherit,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (log.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE) winapi::fail("open workspace supervisor log");
        HANDLE handles[]{input.value, log.value, notify.value};
        if (!::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr))
            winapi::fail("bind workspace supervisor streams");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attrs;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log.value;
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            environment.data(), data.c_str(), &startup.StartupInfo, &process))
            winapi::fail("start workspace AppContainer");
        winapi::handle thread{process.hThread}, child{process.hProcess};
        HANDLE startup_events[]{child.value, ready.value};
        const auto result = ::WaitForMultipleObjects(2, startup_events, FALSE, host.timeout_seconds * 1000);
        if (result == WAIT_OBJECT_0) {
            DWORD code{};
            if (!::GetExitCodeProcess(child.value, &code)) winapi::fail("read workspace exit code");
            throw std::runtime_error(std::format("workspace process exited with code {} before readiness", code));
        }
        if (result == WAIT_TIMEOUT) throw std::runtime_error("workspace process did not become ready before the deadline");
        if (result != WAIT_OBJECT_0 + 1) winapi::fail("wait for workspace readiness");
        running_jobs().insert_or_assign(id, std::move(job));
        return process.dwProcessId;
    }
}
