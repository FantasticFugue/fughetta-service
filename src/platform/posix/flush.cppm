module;
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

module durability;
import std;
import constants;
import utility;
import descriptor;
import posix;

namespace durability {
    auto is_executable(const std::filesystem::path& path) -> bool {
        return (std::filesystem::status(path).permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
    }
    void set_executable(const std::filesystem::path& path, bool executable) {
        using std::filesystem::perms;
        auto mode = perms::owner_read | perms::owner_write | perms::group_read | perms::others_read;
        if (executable)
            mode |= perms::owner_exec | perms::group_exec | perms::others_exec;
        std::filesystem::permissions(path, mode);
    }
    using descriptor::fail;
    using descriptor::fd;
    using descriptor::open;
    using descriptor::sync;
    using descriptor::write_all;
    void write_part(const std::filesystem::path& path, std::uint64_t offset, std::span<const std::byte> bytes, bool complete) {
        auto f = open(path, O_WRONLY | O_CREAT);
        if (::lseek(f.get(), static_cast<off_t>(offset), SEEK_SET) < 0)
            fail("cannot seek in", path);
        write_all(f, bytes, path);
        if (complete)
            sync(f, path);
        f.close(path);
    }

    void sync_dir(const std::filesystem::path& dir) {
        auto d = open(dir.empty() ? std::filesystem::path{"."} : dir, O_RDONLY | O_DIRECTORY);
        sync(d, dir);
        d.close(dir);
    }

    void sync_filesystem(const std::filesystem::path& path) {
        auto f = open(path, O_RDONLY | O_DIRECTORY);
        if (!posix::sync_filesystem(f.get()))
            fail("cannot sync the filesystem of", path);
        f.close(path);
    }

    void durable_write(const std::filesystem::path& target, std::span<const std::byte> bytes, bool executable, std::string_view temp_suffix) {
        auto temp = target;
        temp += temp_suffix;
        try {
            // A temporary left behind, or planted as a link, is replaced, never written through.
            ::unlink(temp.c_str());
            auto f = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, executable ? 0755 : 0644);
            if (::fchmod(f.get(), executable ? 0755 : 0644) != 0)
                fail("cannot set the mode of", temp);
            write_all(f, bytes, temp);
            sync(f, temp);
            f.close(temp);
            if (::rename(temp.c_str(), target.c_str()) != 0)
                fail("cannot rename into", target);
        } catch (...) {
            ::unlink(temp.c_str());
            throw;
        }
        sync_dir(target.parent_path());
    }

    void durable_write(const std::filesystem::path& target, std::string_view text, bool executable, std::string_view temp_suffix) {
        durable_write(target, std::as_bytes(std::span{text}), executable, temp_suffix);
    }

    void durable_private_write(const std::filesystem::path& target, std::string_view text) {
        auto temporary = target;
        temporary += ".private-write";
        try {
            ::unlink(temporary.c_str());
            auto file = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
            if (::fchmod(file.get(), 0600) != 0)
                fail("cannot protect", temporary);
            write_all(file, std::as_bytes(std::span{text}), temporary);
            sync(file, temporary);
            file.close(temporary);
            if (::rename(temporary.c_str(), target.c_str()) != 0)
                fail("cannot publish", target);
        } catch (...) {
            ::unlink(temporary.c_str());
            throw;
        }
        sync_dir(target.parent_path());
    }

    struct append_file::state {
        std::filesystem::path path;
        fd file;
        bool fresh;
        explicit state(const std::filesystem::path& path) : path(path), file(-1), fresh(!std::filesystem::exists(path)) {
            file = open(path, O_WRONLY | O_CREAT | O_APPEND);
        }
    };
    append_file::append_file(const std::filesystem::path& path) : state_(std::make_unique<state>(path)) {}
    append_file::~append_file() = default;
    void append_file::append(std::string_view records) {
        write_all(state_->file, std::as_bytes(std::span{records}), state_->path);
        sync(state_->file, state_->path);
        if (state_->fresh) {
            sync_dir(state_->path.parent_path());
            state_->fresh = false;
        }
    }

    void durable_append(const std::filesystem::path& log, std::string_view record) {
        bool fresh = !std::filesystem::exists(log);
        auto f = open(log, O_WRONLY | O_CREAT | O_APPEND);
        write_all(f, std::as_bytes(std::span{record}), log);
        sync(f, log);
        f.close(log);
        if (fresh)
            sync_dir(log.parent_path());
    }

    namespace {
        auto read_checked(std::filesystem::path path, std::uint64_t limit, bool secret) -> util::generator<std::span<const std::byte>> {
            int raw = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (raw < 0)
                throw unreadable{path.string()};
            fd f{raw};
            struct ::stat st{};
            if (::fstat(f.get(), &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::uint64_t>(st.st_size) > limit)
                throw unreadable{path.string()};
            if (secret && (st.st_uid != ::geteuid() || (st.st_mode & 0777) != 0600))
                throw unreadable{path.string()};
            std::array<std::byte, 1 << 16> buffer;
            std::uint64_t total = 0;
            for (;;) {
                auto n = ::read(f.get(), buffer.data(), buffer.size());
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0)
                    throw unreadable{path.string()};
                if (n == 0)
                    break;
                total += static_cast<std::uint64_t>(n);
                if (total > static_cast<std::uint64_t>(st.st_size))
                    throw unreadable{path.string()};
                co_yield std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(n)};
            }
            if (total != static_cast<std::uint64_t>(st.st_size))
                throw unreadable{path.string()};
        }
    } // namespace
    auto read_plain(const std::filesystem::path& path, std::uint64_t limit) -> util::generator<std::span<const std::byte>> {
        return read_checked(path, limit, false);
    }
    auto read_private(const std::filesystem::path& path, std::uint64_t limit) -> util::generator<std::span<const std::byte>> {
        return read_checked(path, limit, true);
    }

    void durable_remove(const std::filesystem::path& p) {
        if (::unlink(p.c_str()) != 0 && errno != ENOENT)
            fail("cannot remove", p);
        sync_dir(p.parent_path());
    }

    void durable_rename(const std::filesystem::path& from, const std::filesystem::path& to) {
        if (::rename(from.c_str(), to.c_str()) != 0)
            fail("cannot rename", from);
        sync_dir(to.parent_path());
        if (from.parent_path() != to.parent_path())
            sync_dir(from.parent_path());
    }

    void durable_directories(const std::filesystem::path& dir) {
        std::vector<std::filesystem::path> made;
        for (auto p = dir; !p.empty() && !std::filesystem::exists(p); p = p.parent_path()) {
            made.push_back(p);
            if (p == p.parent_path())
                break;
        }
        std::filesystem::create_directories(dir);
        for (auto& p : made)
            sync_dir(p.parent_path());
    }

    struct dir_lock::state {
    public:
        explicit state(const std::filesystem::path& dir, std::string_view what) : path_(dir / constants::lock_file), f_(-1) {
            std::filesystem::create_directories(dir);
            f_ = open(path_, O_RDWR | O_CREAT);
            if (::flock(f_.get(), LOCK_EX | LOCK_NB) != 0) {
                if (errno == EWOULDBLOCK)
                    throw lock_busy(std::format("{} {} is in use by another process", what, dir.string()));
                fail("cannot lock", path_);
            }
            auto pid = std::format("{}\n", ::getpid());
            if (::ftruncate(f_.get(), 0) == 0)
                (void)::write(f_.get(), pid.data(), pid.size());
        }

    private:
        std::filesystem::path path_;
        fd f_;
    };
    dir_lock::dir_lock(const std::filesystem::path& dir, std::string_view what) : state_(std::make_unique<state>(dir, what)) {}
    dir_lock::dir_lock(dir_lock&&) noexcept = default;
    auto dir_lock::operator=(dir_lock&&) noexcept -> dir_lock& = default;
    dir_lock::~dir_lock() = default;
} // namespace durability

namespace files {
    mapping::mapping(const std::filesystem::path& p) {
        int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            throw std::system_error(errno, std::system_category(), p.string());
        struct stat st;
        if (::fstat(fd, &st) < 0) {
            ::close(fd);
            throw std::system_error(errno, std::system_category());
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_) {
            void* m = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m == MAP_FAILED) {
                ::close(fd);
                throw std::system_error(errno, std::system_category());
            }
            data_ = static_cast<const std::byte*>(m);
        }
        ::close(fd);
    }
    mapping::~mapping() {
        if (data_)
            ::munmap(const_cast<std::byte*>(data_), size_);
    }

} // namespace files
