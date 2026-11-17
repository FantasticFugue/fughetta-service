module;
#include <crt_externs.h>
#include <sandbox.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <libproc.h>
#include <sys/resource.h>
#include <mach/mach_time.h>

module posix;
import std;
import async;

namespace posix {
    namespace {
        auto sample_process(int pid, bool children) -> std::optional<io::process_usage> {
            rusage_info_v2 resource{};
            proc_taskinfo task{};
            mach_timebase_info_data_t clock{};
            if (::proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&resource)) != 0 ||
                ::proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof task) != sizeof task || ::mach_timebase_info(&clock) != KERN_SUCCESS || clock.denom == 0)
                return {};
            auto ticks = resource.ri_user_time + resource.ri_system_time;
            if (children)
                ticks += resource.ri_child_user_time + resource.ri_child_system_time;
            // Kernel task accounting uses Mach absolute time, including on Apple Silicon.
            auto nanos = static_cast<std::uint64_t>(static_cast<unsigned __int128>(ticks) * clock.numer / clock.denom);
            return io::process_usage{resource.ri_resident_size, nanos, static_cast<std::uint32_t>(task.pti_threadnum), 1};
        }
    } // namespace
    auto process_usage(int pid, bool group) noexcept -> std::optional<io::process_usage> {
        try {
            if (!group)
                return sample_process(pid, false);
            std::vector<int> members(64);
            int bytes;
            for (;;) {
                bytes = ::proc_listpids(PROC_PGRP_ONLY, static_cast<std::uint32_t>(pid), members.data(), static_cast<int>(members.size() * sizeof(int)));
                if (bytes <= 0)
                    return {};
                if (static_cast<std::size_t>(bytes) < members.size() * sizeof(int))
                    break;
                members.resize(members.size() * 2);
            }
            io::process_usage total;
            for (int index = 0; index < bytes / static_cast<int>(sizeof(int)); ++index) {
                auto member = members[index];
                if (member <= 0)
                    continue;
                auto sample = sample_process(member, true);
                if (!sample) {
                    if (::getpgid(member) != pid)
                        continue;
                    return {};
                }
                total.resident_bytes += sample->resident_bytes;
                total.cpu_nanoseconds += sample->cpu_nanoseconds;
                total.threads += sample->threads;
                total.processes += sample->processes;
            }
            return total.processes ? std::optional{total} : std::nullopt;
        } catch (...) {
            return {};
        }
    }

    auto environment() noexcept -> char**& { return *::_NSGetEnviron(); }
    auto executable() -> std::filesystem::path {
        std::uint32_t size = 0;
        ::_NSGetExecutablePath(nullptr, &size);
        std::string buffer(size, '\0');
        if (::_NSGetExecutablePath(buffer.data(), &size) != 0)
            throw std::runtime_error("cannot locate executable");
        return std::filesystem::canonical(buffer.c_str());
    }

    // Darwin has no parent-death signal; the check after fork is all there is.
    auto follow_parent(int parent) noexcept -> bool { return ::getppid() == parent; }
    auto descriptor_bound() noexcept -> int { return ::getdtablesize(); }
    // Darwin has neither close_range nor closefrom.
    auto close_inherited(int bound) noexcept -> bool {
        for (int fd = 3; fd < bound; ++fd)
            ::close(fd);
        return true;
    }
    // The system profile that denies every socket; the call is deprecated but remains the only one a process can apply
    // to itself.
    auto isolate_network() noexcept -> bool {
        char* error = nullptr;
        if (::sandbox_init(kSBXProfileNoNetwork, SANDBOX_NAMED, &error) == 0)
            return true;
        ::sandbox_free_error(error);
        return false;
    }
    auto exit_handle(int) -> int { return -1; }

    auto sync_filesystem(int descriptor) noexcept -> bool {
        ::sync();
        return ::fsync(descriptor) == 0;
    }

    // launchd has no readiness protocol.
    void notify(std::string_view) {}
} // namespace posix
