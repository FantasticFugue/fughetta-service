module;
#include <zstd.h>
export module archive;
import std;
import codec;
import async;
import native;
import durability;

export namespace nodes::archive {
    inline constexpr std::size_t limit = 256 * 1024 * 1024;
    auto pack(std::string_view plain) -> std::string {
        if (plain.size() > limit)
            fail(nodes::code::resource_limit, "compressed object exceeds plaintext limit");
        std::string out{"DCZ1"};
        codec::integer(out, plain.size());
        auto start = out.size();
        out.resize(start + ZSTD_compressBound(plain.size()));
        struct compressor_pool {
            std::array<ZSTD_CCtx*, 19> contexts{};
            ~compressor_pool() {
                for (auto* context : contexts)
                    ZSTD_freeCCtx(context);
            }
        };
        static thread_local compressor_pool pool;
        auto bucket = std::bit_width(std::max<std::size_t>(plain.size(), 1024) - 1) - 10;
        auto& compressor = pool.contexts[bucket];
        if (!compressor)
            compressor = ZSTD_createCCtx();
        if (!compressor)
            fail(nodes::code::storage_failure, "compression context allocation failed");
        auto size = ZSTD_compressCCtx(compressor, out.data() + start, out.size() - start, plain.data(), plain.size(), 3);
        if (ZSTD_isError(size))
            fail(nodes::code::storage_failure, ZSTD_getErrorName(size));
        out.resize(start + size);
        return out;
    }
    // `most` bounds the plain size the frame may declare, before anything is allocated for it.
    auto unpack(std::string_view stored, std::uint64_t most = limit) -> std::string {
        if (stored.size() < 12 || !stored.starts_with("DCZ1"))
            fail(nodes::code::invalid_record, "compressed object framing");
        std::uint64_t size = 0;
        for (int i = 7; i >= 0; --i)
            size = (size << 8) | static_cast<unsigned char>(stored[4 + i]);
        if (size > std::min(most, limit))
            fail(nodes::code::resource_limit, "compressed object plaintext limit");
        std::string out(size, '\0');
        static thread_local std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> decompressor{ZSTD_createDCtx(), ZSTD_freeDCtx};
        if (!decompressor)
            fail(nodes::code::storage_failure, "decompression context allocation failed");
        auto read = ZSTD_decompressDCtx(decompressor.get(), out.data(), out.size(), stored.data() + 12, stored.size() - 12);
        if (ZSTD_isError(read) || read != size)
            fail(nodes::code::invalid_record, "compressed object is damaged");
        return out;
    }
    class files {
        static auto context() -> native::context& {
            static thread_local native::context resource;
            return resource;
        }

    public:
        auto read(const std::filesystem::path& path) -> std::string {
            auto size = std::filesystem::file_size(path);
            if (size > limit)
                fail(nodes::code::resource_limit, "file read exceeds object limit");
            auto file = io::open_file(context().get_scheduler(), path);
            std::string out(size, '\0');
            std::size_t offset = 0;
            while (offset < size) {
                auto span = std::as_writable_bytes(std::span{out}).subspan(offset, std::min<std::size_t>(1024 * 1024, size - offset));
                auto count = cores::run(context(), io::async_read_at(file, span, offset));
                if (!count || !*count)
                    fail(nodes::code::storage_failure, "short asynchronous file read");
                offset += *count;
            }
            return out;
        }
        void write(const std::filesystem::path& target, std::string_view data, bool executable = false) {
            auto temp = target;
            temp += ".node-pending";
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(temp)))
                fail(nodes::code::storage_failure, "temporary file is a symbolic link");
            if (std::filesystem::exists(temp))
                durability::durable_remove(temp);
            try {
                {
                    auto file = io::open_file(context().get_scheduler(), temp, true);
                    std::size_t offset = 0;
                    while (offset < data.size()) {
                        auto span = std::as_bytes(std::span{data}).subspan(offset, std::min<std::size_t>(1024 * 1024, data.size() - offset));
                        auto count = cores::run(context(), io::async_write_at(file, span, offset));
                        if (!count || !*count)
                            fail(nodes::code::storage_failure, "short asynchronous file write");
                        offset += *count;
                    }
                }
                durability::set_executable(temp, executable);
                durability::write_part(temp, 0, {}, true);
                durability::durable_rename(temp, target);
            } catch (...) {
                if (std::filesystem::exists(temp))
                    durability::durable_remove(temp);
                throw;
            }
        }
    };
    class file_buffer : public std::streambuf {
        native::context context_;
        native::file file_;
        std::filesystem::path path_;
        std::uint64_t offset_ = 0;
        std::array<char, 64 * 1024> buffer_{};
        bool output_;

    protected:
        auto underflow() -> int_type override {
            if (output_)
                return traits_type::eof();
            auto bytes = std::as_writable_bytes(std::span{buffer_});
            auto count = cores::run(context_, io::async_read_at(file_, bytes, offset_));
            if (!count)
                fail(nodes::code::storage_failure, "asynchronous input cancelled");
            if (!*count)
                return traits_type::eof();
            offset_ += *count;
            setg(buffer_.data(), buffer_.data(), buffer_.data() + *count);
            return traits_type::to_int_type(*gptr());
        }
        auto xsputn(const char* data, std::streamsize size) -> std::streamsize override {
            if (!output_)
                return 0;
            std::size_t written = 0;
            while (written < static_cast<std::size_t>(size)) {
                auto n = std::min<std::size_t>(1024 * 1024, size - written);
                auto bytes = std::as_bytes(std::span{data + written, n});
                auto count = cores::run(context_, io::async_write_at(file_, bytes, offset_));
                if (!count || !*count)
                    fail(nodes::code::storage_failure, "short asynchronous output");
                offset_ += *count;
                written += *count;
            }
            return written;
        }
        auto overflow(int_type ch) -> int_type override {
            if (traits_type::eq_int_type(ch, traits_type::eof()))
                return traits_type::not_eof(ch);
            char c = traits_type::to_char_type(ch);
            return xsputn(&c, 1) == 1 ? ch : traits_type::eof();
        }

    public:
        file_buffer(const std::filesystem::path& path, bool output)
            : file_(io::open_file(context_.get_scheduler(), path, output)), path_(path), output_(output) {
            if (output)
                std::filesystem::resize_file(path, 0);
        }
        void finish() {
            if (output_) {
                file_.close();
                durability::write_part(path_, 0, {}, true);
                durability::sync_dir(path_.parent_path());
                output_ = false;
            }
        }
    };
    class input_file {
        file_buffer buffer_;

    public:
        std::istream stream;
        explicit input_file(const std::filesystem::path& path) : buffer_(path, false), stream(&buffer_) { stream.exceptions(std::ios::badbit); }
    };
    class output_file {
        file_buffer buffer_;

    public:
        std::ostream stream;
        explicit output_file(const std::filesystem::path& path) : buffer_(path, true), stream(&buffer_) {
            stream.exceptions(std::ios::badbit | std::ios::failbit);
        }
        void finish() {
            stream.flush();
            buffer_.finish();
        }
    };
} // namespace nodes::archive
