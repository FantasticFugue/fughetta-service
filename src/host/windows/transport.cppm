module;
#include <windows.h>

export module containerd_transport;
import std;
import winapi;

export namespace containerd_transport {
    using clock = std::chrono::steady_clock;

    class connection {
        clock::time_point deadline_;
        winapi::handle pipe_;

        winapi::handle read_event_{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        OVERLAPPED read_op_{};
        std::array<std::uint8_t, 16384> input_{};
        DWORD count_{};
        std::size_t offset_{};
        bool pending_{};

        void begin_read() {
            ::ResetEvent(read_event_.value);
            read_op_ = {};
            read_op_.hEvent = read_event_.value;
            offset_ = count_ = 0;
            if (!::ReadFile(pipe_.value, input_.data(), static_cast<DWORD>(input_.size()), &count_, &read_op_)) {
                if (::GetLastError() != ERROR_IO_PENDING)
                    winapi::fail("read containerd pipe");
                pending_ = true;
            }
        }

        auto remaining() const -> DWORD {
            const auto ms = std::chrono::ceil<std::chrono::milliseconds>(deadline_ - clock::now()).count();
            if (ms <= 0)
                throw std::runtime_error("containerd RPC deadline exceeded");
            return static_cast<DWORD>(ms);
        }

        template<class Start>
        auto transfer(Start start) const -> DWORD {
            winapi::handle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            if (!event.value)
                winapi::fail("containerd pipe event");
            OVERLAPPED op{};
            op.hEvent = event.value;
            DWORD size{};
            const auto timeout = remaining();
            if (!start(op, size)) {
                if (::GetLastError() != ERROR_IO_PENDING)
                    winapi::fail("containerd pipe transfer");
                if (::WaitForSingleObject(event.value, timeout) != WAIT_OBJECT_0) {
                    ::CancelIoEx(pipe_.value, &op);
                    ::GetOverlappedResult(pipe_.value, &op, &size, TRUE);
                    throw std::runtime_error("containerd RPC deadline exceeded");
                }
                if (!::GetOverlappedResult(pipe_.value, &op, &size, FALSE))
                    winapi::fail("containerd pipe completion");
            }
            if (!size)
                throw std::runtime_error("containerd closed the pipe before completing the RPC");
            return size;
        }

    public:
        connection(std::string_view path, clock::time_point deadline) : deadline_(deadline) {
            if (!path.starts_with(R"(\\.\pipe\)") || path.contains('\0'))
                throw std::invalid_argument("containerd requires a local named pipe");
            const auto name = winapi::wide(path);
            for (;;) {
                pipe_ = winapi::handle{::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)};
                if (pipe_.value != INVALID_HANDLE_VALUE)
                    break;
                if (::GetLastError() != ERROR_PIPE_BUSY || !::WaitNamedPipeW(name.c_str(), remaining()))
                    winapi::fail("connect to containerd pipe");
            }
            if (!read_event_.value) winapi::fail("containerd pipe read event");
            // containerd can write its HTTP/2 settings before reading the client preface.
            begin_read();
        }

        ~connection() {
            if (pending_) {
                ::CancelIoEx(pipe_.value, &read_op_);
                ::GetOverlappedResult(pipe_.value, &read_op_, &count_, TRUE);
            }
        }

        void send(std::span<const std::uint8_t> bytes) const {
            while (!bytes.empty()) {
                const auto size = transfer([&](OVERLAPPED& op, DWORD& used) {
                    return ::WriteFile(pipe_.value, bytes.data(), static_cast<DWORD>(bytes.size()), &used, &op);
                });
                bytes = bytes.subspan(size);
            }
        }

        auto receive(std::span<std::uint8_t> bytes) -> std::size_t {
            if (pending_) {
                if (::WaitForSingleObject(read_event_.value, remaining()) != WAIT_OBJECT_0)
                    throw std::runtime_error("containerd pipe read timed out");
                pending_ = false;
                if (!::GetOverlappedResult(pipe_.value, &read_op_, &count_, FALSE))
                    winapi::fail("containerd pipe read completion");
            }
            if (!count_) throw std::runtime_error("containerd closed the pipe");
            const auto size = (std::min)(bytes.size(), count_ - offset_);
            std::copy_n(input_.data() + offset_, size, bytes.data());
            offset_ += size;
            if (offset_ == count_) begin_read();
            return size;
        }
    };
}
