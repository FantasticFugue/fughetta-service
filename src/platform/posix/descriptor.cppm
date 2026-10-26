module;
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

export module descriptor;
import std;

export namespace descriptor {
    [[noreturn]] inline void fail(std::string_view what, const std::filesystem::path& p, int err = errno) {
        throw std::system_error(err, std::generic_category(), std::format("{} {}", what, p.string()));
    }

    class fd {
    public:
        explicit fd(int f) noexcept : f_(f) {}
        fd(fd&& o) noexcept : f_(std::exchange(o.f_, -1)) {}
        fd& operator=(fd&& o) noexcept {
            if (this != &o) {
                drop();
                f_ = std::exchange(o.f_, -1);
            }
            return *this;
        }
        ~fd() { drop(); }

        auto get() const noexcept -> int { return f_; }
        void close(const std::filesystem::path& p) {
            if (::close(std::exchange(f_, -1)) != 0)
                fail("cannot close", p);
        }

    private:
        void drop() noexcept {
            if (f_ >= 0)
                ::close(f_);
        }
        int f_;
    };

    inline auto open(const std::filesystem::path& p, int flags, ::mode_t mode = 0644) -> fd {
        int f = ::open(p.c_str(), flags | O_CLOEXEC, mode);
        if (f < 0)
            fail("cannot open", p);
        return fd{f};
    }

    inline void write_all(const fd& f, std::span<const std::byte> bytes, const std::filesystem::path& p) {
        while (!bytes.empty()) {
            auto n = ::write(f.get(), bytes.data(), bytes.size());
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fail("cannot write", p);
            }
            bytes = bytes.subspan(static_cast<std::size_t>(n));
        }
    }

    inline void sync(const fd& f, const std::filesystem::path& p) {
        if (::fsync(f.get()) != 0)
            fail("cannot sync", p);
    }

} // namespace descriptor
