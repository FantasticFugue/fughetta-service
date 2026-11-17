module;
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
module process;
import std;
import mpmc;
import async;
import httpant.io;
import winapi;

namespace native_process {
    namespace {
        auto sample_threads() -> std::optional<std::map<DWORD, std::uint32_t>> {
            winapi::handle snapshot{::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
            if (snapshot.value == INVALID_HANDLE_VALUE)
                return {};
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof entry;
            if (!::Process32FirstW(snapshot.value, &entry))
                return {};
            std::map<DWORD, std::uint32_t> result;
            do {
                result.emplace(entry.th32ProcessID, entry.cntThreads);
            } while (::Process32NextW(snapshot.value, &entry));
            if (::GetLastError() != ERROR_NO_MORE_FILES)
                return {};
            return result;
        }
        auto sample_process(HANDLE process, DWORD pid, const std::map<DWORD, std::uint32_t>& threads) -> std::optional<io::process_usage> {
            PROCESS_MEMORY_COUNTERS memory{};
            memory.cb = sizeof memory;
            FILETIME created{}, exited{}, kernel{}, user{};
            auto count = threads.find(pid);
            if (count == threads.end() || !::K32GetProcessMemoryInfo(process, &memory, sizeof memory) ||
                !::GetProcessTimes(process, &created, &exited, &kernel, &user))
                return {};
            auto ticks = [](FILETIME value) { return (std::uint64_t{value.dwHighDateTime} << 32) | value.dwLowDateTime; };
            return io::process_usage{memory.WorkingSetSize, (ticks(kernel) + ticks(user)) * 100, count->second, 1};
        }
        auto sample_job(HANDLE job) noexcept -> std::optional<io::process_usage> {
            try {
                auto threads = sample_threads();
                if (!threads)
                    return {};
                std::vector<std::byte> storage(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 16 * sizeof(ULONG_PTR));
                JOBOBJECT_BASIC_PROCESS_ID_LIST* members;
                for (;;) {
                    members = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
                    if (::QueryInformationJobObject(job, JobObjectBasicProcessIdList, members, static_cast<DWORD>(storage.size()), nullptr))
                        break;
                    if (::GetLastError() != ERROR_MORE_DATA)
                        return {};
                    storage.resize(storage.size() * 2);
                }
                io::process_usage result;
                for (DWORD index = 0; index < members->NumberOfProcessIdsInList; ++index) {
                    auto pid = static_cast<DWORD>(members->ProcessIdList[index]);
                    winapi::handle process{::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid)};
                    if (!process.value) {
                        if (::GetLastError() == ERROR_INVALID_PARAMETER)
                            continue;
                        return {};
                    }
                    auto value = sample_process(process.value, pid, *threads);
                    if (!value) {
                        if (::WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0)
                            continue;
                        return {};
                    }
                    result.resident_bytes += value->resident_bytes;
                    result.threads += value->threads;
                    ++result.processes;
                }
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                if (!::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof accounting, nullptr))
                    return {};
                result.cpu_nanoseconds = static_cast<std::uint64_t>(accounting.TotalKernelTime.QuadPart + accounting.TotalUserTime.QuadPart) * 100;
                return result;
            } catch (...) {
                return {};
            }
        }
        struct observation : io::process_record {
            winapi::handle job;
        };

        auto quote(std::string_view arg) -> std::wstring {
            auto text = winapi::wide(arg);
            if (text.find(L'\0') != text.npos)
                throw std::invalid_argument("NUL in command argument");
            std::wstring result = L"\"";
            std::size_t slashes = 0;
            for (auto c : text) {
                if (c == L'\\') {
                    ++slashes;
                    continue;
                }
                result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
                slashes = 0;
                result += c;
            }
            result.append(slashes * 2, L'\\');
            result += L'\"';
            return result;
        }
        struct attributes {
            std::vector<std::byte> storage;
            LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
            attributes(HANDLE* handles, std::size_t count) {
                SIZE_T size = 0;
                ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
                storage.resize(size);
                list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
                if (!::InitializeProcThreadAttributeList(list, 1, 0, &size)) {
                    list = nullptr;
                    winapi::fail("InitializeProcThreadAttributeList");
                }
                if (!::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, count * sizeof(HANDLE), nullptr, nullptr)) {
                    auto error = ::GetLastError();
                    ::DeleteProcThreadAttributeList(list);
                    list = nullptr;
                    winapi::fail("child handle list", error);
                }
            }
            ~attributes() {
                if (list)
                    ::DeleteProcThreadAttributeList(list);
            }
        };
    } // namespace
    auto sample(const io::process_record& process) noexcept -> std::optional<io::process_usage> { return sample_job(reinterpret_cast<HANDLE>(process.handle)); }
    auto self_usage() noexcept -> std::optional<io::process_usage> {
        try {
            auto threads = sample_threads();
            return threads ? sample_process(::GetCurrentProcess(), ::GetCurrentProcessId(), *threads) : std::nullopt;
        } catch (...) {
            return {};
        }
    }
    struct child::state {
        native_io::engine<concurrent::queue>* engine;
        winapi::handle process, out, in, job;
        DWORD pid = 0;
        std::atomic<bool> reaped{false};
        std::shared_ptr<observation> observed;
        std::shared_ptr<io::process_group> processes;
        void stop_observing() noexcept {
            if (observed) {
                std::lock_guard lock{observed->mutex};
                observed->live = false;
            }
        }
        void finished() noexcept {
            if (auto group = std::exchange(processes, {})) {
                std::lock_guard held{group->mutex};
                group->children.erase(static_cast<int>(pid));
            }
        }
        explicit state(native_io::engine<concurrent::queue>& e) : engine(&e) {}
        void close_stream(winapi::handle& stream) noexcept {
            auto value = std::exchange(stream.value, nullptr);
            if (value && value != INVALID_HANDLE_VALUE) {
                auto file = reinterpret_cast<native_io::handle>(value);
                engine->cancel_file(file);
                native_io::close_file(file);
            }
        }
        ~state() {
            stop_observing();
            close_stream(out);
            close_stream(in);
            if (job.value)
                ::TerminateJobObject(job.value, 1);
            if (process.value && !reaped)
                ::WaitForSingleObject(process.value, INFINITE);
            finished();
        }
    };
    auto child::spawn(native_io::engine<concurrent::queue>& engine, const io::command& cmd) -> child {
        if (cmd.argv.empty() || cmd.argv.front().empty())
            throw std::invalid_argument("spawn: empty command");
        // Analysis hosts that isolate the network are the POSIX ones; a Windows Worker serves both roles in one process.
        if (cmd.offline)
            throw std::invalid_argument("spawn: network isolation is not available on Windows");
        auto s = std::make_unique<state>(engine);
        s->job = winapi::handle{::CreateJobObjectW(nullptr, nullptr)};
        if (!s->job.value)
            winapi::fail("CreateJobObject child");
        winapi::limits(s->job.value, cmd.max_memory_bytes, cmd.max_cpu_seconds);
        static std::atomic<std::uint64_t> serial{0};
        auto name = std::format(L"\\\\.\\pipe\\child-{}-{}", ::GetCurrentProcessId(), serial.fetch_add(1));
        s->out = winapi::handle{::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr)};
        if (s->out.value == INVALID_HANDLE_VALUE)
            winapi::fail("CreateNamedPipe output");
        SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
        winapi::handle write{::CreateFileW(name.c_str(), GENERIC_WRITE, 0, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (write.value == INVALID_HANDLE_VALUE)
            winapi::fail("open child output");
        winapi::handle input, errors;
        if (cmd.interactive) {
            auto inbound = std::format(L"\\\\.\\pipe\\child-input-{}-{}", ::GetCurrentProcessId(), serial.fetch_add(1));
            s->in = winapi::handle{::CreateNamedPipeW(inbound.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                                      PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr)};
            if (s->in.value == INVALID_HANDLE_VALUE)
                winapi::fail("CreateNamedPipe input");
            input = winapi::handle{::CreateFileW(inbound.c_str(), GENERIC_READ, 0, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            // A conversation keeps stdout for replies; its diagnostics are discarded.
            errors = winapi::handle{::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr)};
            if (errors.value == INVALID_HANDLE_VALUE)
                winapi::fail("open child stderr");
        } else
            input = winapi::handle{::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr)};
        if (input.value == INVALID_HANDLE_VALUE)
            winapi::fail("open child stdin");
        winapi::handle file;
        if (cmd.standard_output) {
            file = winapi::handle{
                ::CreateFileW(cmd.standard_output->c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (file.value == INVALID_HANDLE_VALUE)
                winapi::fail("open child standard output");
        }
        HANDLE inherited[]{input.value, write.value, cmd.interactive ? errors.value : file.value};
        attributes attr{inherited, cmd.interactive || cmd.standard_output ? 3u : 2u};
        STARTUPINFOEXW start{};
        start.StartupInfo.cb = sizeof start;
        start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        start.StartupInfo.hStdInput = input.value;
        start.StartupInfo.hStdOutput = cmd.standard_output ? file.value : write.value;
        start.StartupInfo.hStdError = cmd.interactive ? errors.value : write.value;
        start.lpAttributeList = attr.list;
        std::wstring command;
        for (auto& arg : cmd.argv) {
            if (!command.empty())
                command += L' ';
            command += quote(arg);
        }
        std::vector<wchar_t> env;
        if (cmd.env) {
            std::vector<std::wstring> entries;
            for (auto& [key, value] : *cmd.env) {
                if (key.empty() || (key.find('=') != key.npos && key.front() != '=') || key.find('\0') != key.npos || value.find('\0') != value.npos)
                    throw std::invalid_argument("invalid child environment");
                entries.push_back(winapi::wide(key + "=" + value));
            }
            std::ranges::sort(entries, [](auto& a, auto& b) { return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
            for (auto& entry : entries) {
                env.insert(env.end(), entry.begin(), entry.end());
                env.push_back(L'\0');
            }
            env.push_back(L'\0');
            if (entries.empty())
                env.push_back(L'\0');
        }
        PROCESS_INFORMATION process{};
        auto cwd = cmd.cwd.empty() ? std::wstring{} : std::filesystem::absolute(cmd.cwd).wstring();
        if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                              CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                              cmd.env ? env.data() : nullptr, cwd.empty() ? nullptr : cwd.c_str(), &start.StartupInfo, &process))
            winapi::fail("CreateProcess");
        s->process = winapi::handle{process.hProcess};
        winapi::handle thread{process.hThread};
        s->pid = process.dwProcessId;
        if (!::AssignProcessToJobObject(s->job.value, process.hProcess)) {
            auto error = ::GetLastError();
            ::TerminateProcess(process.hProcess, 126);
            winapi::fail("AssignProcessToJobObject", error);
        }
        if (::ResumeThread(thread.value) == DWORD(-1)) {
            auto error = ::GetLastError();
            ::TerminateProcess(process.hProcess, 126);
            winapi::fail("ResumeThread", error);
        }
        if (cmd.processes) {
            s->observed = std::make_shared<observation>();
            HANDLE duplicate = nullptr;
            if (!::DuplicateHandle(::GetCurrentProcess(), s->job.value, ::GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
                winapi::fail("duplicate observed job");
            s->observed->job = winapi::handle{duplicate};
            s->observed->pid = static_cast<int>(s->pid);
            s->observed->handle = reinterpret_cast<std::uintptr_t>(duplicate);
            s->processes = cmd.processes;
            std::lock_guard held{cmd.processes->mutex};
            cmd.processes->children.emplace(static_cast<int>(s->pid), s->observed);
        }
        return child{std::move(s)};
    }
    child::child(std::unique_ptr<state> state) : state_(std::move(state)) {}
    child::child(child&&) noexcept = default;
    auto child::operator=(child&&) noexcept -> child& = default;
    child::~child() = default;
    auto child::id() const noexcept -> int { return static_cast<int>(state_->pid); }
    auto child::output() const noexcept -> native_io::handle { return reinterpret_cast<native_io::handle>(state_->out.value); }
    auto child::input() const noexcept -> native_io::handle { return reinterpret_cast<native_io::handle>(state_->in.value); }
    void child::close_input() noexcept { state_->close_stream(state_->in); }
    auto child::reaped() const noexcept -> bool { return state_->reaped; }
    void child::terminate() noexcept {
        if (!state_->reaped)
            ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, state_->pid);
    }
    void child::kill() noexcept {
        if (!state_->reaped)
            ::TerminateJobObject(state_->job.value, 1);
    }
    void child::begin_wait(native_io::request& r) noexcept {
        r.kind = native_io::operation::timer;
        r.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1};
        r.result_ready = false;
        state_->engine->start(r);
    }
    auto child::ready(native_io::request& r, std::error_code& error) noexcept -> bool {
        if (error)
            return true;
        auto result = ::WaitForSingleObject(state_->process.value, 0);
        if (result == WAIT_TIMEOUT) {
            begin_wait(r);
            return false;
        }
        if (result == WAIT_FAILED)
            error = {static_cast<int>(::GetLastError()), std::system_category()};
        return true;
    }
    auto child::reap() noexcept -> int {
        state_->stop_observing();
        ::TerminateJobObject(state_->job.value, 1);
        state_->finished();
        DWORD code = 0;
        if (!::GetExitCodeProcess(state_->process.value, &code))
            return -1;
        state_->reaped = true;
        return static_cast<int>(code);
    }
} // namespace native_process
