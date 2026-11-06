module;
#include <windows.h>
module isolation;
import std;
import async;
import winapi;

namespace isolation {
    struct scope::state {
        std::string identity;
        winapi::handle job;
        winapi::handle port;
        std::optional<std::uint32_t> cpu;
        mutable std::uint64_t failures = 0;
        bool closed = false;
        state(std::filesystem::path path, std::optional<std::uint64_t> memory, std::optional<std::uint32_t> limit) : cpu(limit) {
            std::uint64_t hash = 14695981039346656037ull;
            for (auto c : path.generic_u8string()) {
                hash ^= static_cast<unsigned char>(c);
                hash *= 1099511628211ull;
            }
            identity = std::format("Local\\job-{:016x}", hash);
            job = winapi::handle{::CreateJobObjectW(nullptr, winapi::wide(identity).c_str())};
            if (!job.value)
                winapi::fail("CreateJobObject");
            if (::GetLastError() == ERROR_ALREADY_EXISTS)
                throw std::runtime_error("isolation job already exists");
            winapi::limits(job.value, memory, cpu);
            port = winapi::handle{::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)};
            if (!port.value)
                winapi::fail("CreateIoCompletionPort job");
            JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{this, port.value};
            if (!::SetInformationJobObject(job.value, JobObjectAssociateCompletionPortInformation, &association, sizeof association))
                winapi::fail("associate job completion port");
        }
        ~state() {
            if (!closed)
                ::TerminateJobObject(job.value, 1);
        }
    };
    scope::scope(std::filesystem::path path, std::optional<std::uint64_t> memory, std::optional<std::uint32_t> cpu)
        : state_(std::make_unique<state>(std::move(path), memory, cpu)) {}
    scope::~scope() = default;
    auto scope::target() const -> isolation::target { return {state_->identity}; }
    auto scope::cpu_exhausted() const -> bool {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (!::QueryInformationJobObject(state_->job.value, JobObjectBasicAccountingInformation, &info, sizeof info, nullptr))
            winapi::fail("QueryInformationJobObject CPU");
        return state_->cpu && static_cast<std::uint64_t>(info.TotalUserTime.QuadPart + info.TotalKernelTime.QuadPart) >= std::uint64_t{*state_->cpu} * 10000000;
    }
    auto scope::memory_failures() const -> std::uint64_t {
        DWORD message;
        ULONG_PTR key;
        OVERLAPPED* process;
        while (::GetQueuedCompletionStatus(state_->port.value, &message, &key, &process, 0))
            if (message == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT || message == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT)
                ++state_->failures;
        return state_->failures;
    }
    auto scope::memory_pressure() const -> bool {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (!::GlobalMemoryStatusEx(&status))
            winapi::fail("GlobalMemoryStatusEx");
        return status.dwMemoryLoad >= 90 || memory_failures() != 0;
    }
    void scope::concurrency(std::uint32_t parallel) {
        auto processors = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (!parallel || !processors)
            throw std::invalid_argument("CPU concurrency must be positive");
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION limit{};
        limit.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        limit.CpuRate = static_cast<DWORD>(std::clamp<std::uint64_t>(std::uint64_t{parallel} * 10000 / processors, 1, 10000));
        if (!::SetInformationJobObject(state_->job.value, JobObjectCpuRateControlInformation, &limit, sizeof limit))
            winapi::fail("SetInformationJobObject CPU concurrency");
    }
    void scope::close() {
        if (state_->closed)
            return;
        if (!::TerminateJobObject(state_->job.value, 1))
            winapi::fail("TerminateJobObject");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        for (;;) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            if (!::QueryInformationJobObject(state_->job.value, JobObjectBasicAccountingInformation, &info, sizeof info, nullptr))
                winapi::fail("QueryInformationJobObject cleanup");
            if (!info.ActiveProcesses)
                break;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("job cleanup timed out");
            ::Sleep(1);
        }
        state_->closed = true;
    }
    struct pool::state {
        std::filesystem::path path;
        state(const std::filesystem::path& root, std::string_view key) : path(root / std::string{key}) {
            if (key.empty() || !std::ranges::all_of(key, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                throw std::invalid_argument("invalid worker isolation key");
        }
    };
    auto delegated() -> std::filesystem::path { return "workers"; }
    pool::pool(std::string_view key) : pool(delegated(), key) {}
    pool::pool(const std::filesystem::path& root, std::string_view key) : state_(std::make_unique<state>(root, key)) {}
    pool::~pool() = default;
    auto pool::start(std::optional<std::uint64_t> memory, std::optional<std::uint32_t> cpu) -> std::shared_ptr<scope> {
        static std::atomic<std::uint64_t> serial{0};
        return std::make_shared<scope>(state_->path / std::format("build-{}-{}", ::GetCurrentProcessId(), serial.fetch_add(1)), memory, cpu);
    }
} // namespace isolation
