export module posix;
import std;
import async;

// What Linux and Darwin do differently. Each platform directory implements it,
// and cmake/modules.cmake selects the directory.
export namespace posix {
    auto process_usage(int pid, bool group) noexcept -> std::optional<io::process_usage>;
    // The process environment; a forked child replaces it before exec.
    auto environment() noexcept -> char**&;
    auto executable() -> std::filesystem::path;

    // In a forked child: tie its life to the parent where the kernel can, and
    // report whether that parent is still the one that forked it.
    auto follow_parent(int parent) noexcept -> bool;
    // Measured before fork, so the child calls nothing but the closes.
    auto descriptor_bound() noexcept -> int;
    auto close_inherited(int bound) noexcept -> bool;
    // In a forked child: leave every network, loopback included, before exec.
    // A descriptor that becomes readable when the child exits, or -1 when the
    // exit has to be polled.
    auto exit_handle(int pid) -> int;

    auto sync_filesystem(int descriptor) noexcept -> bool;
    auto try_clone_file(const std::filesystem::path& source, const std::filesystem::path& destination) -> bool;
    // Tells the service manager the lifecycle message, where it listens for one.
    void notify(std::string_view message);
} // namespace posix
