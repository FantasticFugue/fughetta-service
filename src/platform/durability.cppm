export module durability;
import std;
import constants;
import utility;

export namespace durability {
    auto is_executable(const std::filesystem::path& path) -> bool;
    void set_executable(const std::filesystem::path& path, bool executable);
    void write_part(const std::filesystem::path& path, std::uint64_t offset, std::span<const std::byte> bytes, bool complete);
    void sync_dir(const std::filesystem::path& dir);
    void sync_filesystem(const std::filesystem::path& path);
    void durable_write(const std::filesystem::path& target, std::span<const std::byte> bytes, bool executable = false,
                       std::string_view temp_suffix = constants::temporary_suffix);
    void durable_write(const std::filesystem::path& target, std::string_view text, bool executable = false,
                       std::string_view temp_suffix = constants::temporary_suffix);
    void durable_private_write(const std::filesystem::path& target,std::string_view text);
    // Why a file could not be read: not a regular file within the limit, not owner-only where required, unreadable, or resized while read.
    struct unreadable : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    // Yields `path` in pieces if it is a regular file, not a link, of at most `limit` bytes, checked on the opened file itself; a FIFO is
    // never waited on. Throws `unreadable` when it is anything else, or changes size while read.
    auto read_plain(const std::filesystem::path& path, std::uint64_t limit) -> util::generator<std::span<const std::byte>>;
    auto read_private(const std::filesystem::path& path,std::uint64_t limit) -> util::generator<std::span<const std::byte>>;
    void durable_append(const std::filesystem::path& log, std::string_view record);
    void durable_remove(const std::filesystem::path& path);
    void durable_rename(const std::filesystem::path& from, const std::filesystem::path& to);
    void durable_directories(const std::filesystem::path& dir);

    class dir_lock {
    public:
        explicit dir_lock(const std::filesystem::path& dir, std::string_view what);
        dir_lock(dir_lock&&) noexcept;
        auto operator=(dir_lock&&) noexcept -> dir_lock&;
        dir_lock(const dir_lock&) = delete;
        ~dir_lock();

    private:
        struct state;
        std::unique_ptr<state> state_;
    };
} // namespace durability

export namespace files {
    class mapping {
    public:
        explicit mapping(const std::filesystem::path& path);
        mapping(const mapping&) = delete;
        ~mapping();
        auto bytes() const noexcept -> std::span<const std::byte> { return {data_, size_}; }

    private:
        const std::byte* data_ = nullptr;
        std::size_t size_ = 0;
    };
} // namespace files
