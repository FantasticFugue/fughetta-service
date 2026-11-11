module;
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <unistd.h>

module posix;
import std;

namespace posix {
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
    auto exit_handle(int) -> int { return -1; }

    auto sync_filesystem(int descriptor) noexcept -> bool {
        ::sync();
        return ::fsync(descriptor) == 0;
    }

    // launchd has no readiness protocol.
    void notify(std::string_view) {}
} // namespace posix
