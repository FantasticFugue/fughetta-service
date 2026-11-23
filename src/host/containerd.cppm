module;
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

export module containerd;
import std;
import descriptor;
import protobuf;
import httpant.dependencies.nghttp2;

export namespace containerd {
    inline constexpr std::uint32_t task_running = 2, task_stopped = 3;
    struct task {
        protobuf::field<2, std::string> id;
        protobuf::field<3, std::uint32_t> pid;
        protobuf::field<4, std::uint32_t> status;
    };
    struct mount {
        protobuf::field<1, std::string> type;
        protobuf::field<2, std::string> source;
        protobuf::field<3, std::string> target;
        protobuf::field<4, std::vector<std::string>> options;
    };

    struct workload {
        std::string runtime;
        std::string spec;
        std::vector<mount> rootfs;
    };
}

namespace containerd::detail {
    using clock = std::chrono::steady_clock;
    constexpr auto message_limit = 1u << 20;

    [[noreturn]] void socket_error(std::string_view operation) {
        throw std::system_error(errno, std::generic_category(), std::string{operation});
    }

    class connection {
    public:
        connection(std::string_view path, clock::time_point deadline) : deadline_(deadline) {
            sockaddr_un address{};
            if (!std::filesystem::path{path}.is_absolute() || path.size() >= sizeof address.sun_path || path.contains('\0'))
                throw std::invalid_argument("containerd requires an absolute Unix socket path");
            address.sun_family = AF_UNIX;
            std::ranges::copy(path, address.sun_path);
            fd_ = descriptor::fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
            if (fd_.get() < 0)
                socket_error("containerd socket");
            if (::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) < 0) {
                if (errno != EINPROGRESS)
                    socket_error("containerd connect");
                wait(POLLOUT);
                int error = 0;
                socklen_t len = sizeof error;
                if (::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &error, &len) < 0)
                    socket_error("containerd connect status");
                if (error)
                    throw std::system_error(error, std::generic_category(), "containerd connect");
            }
        }

        void send(std::span<const std::uint8_t> bytes) const {
            while (!bytes.empty()) {
                wait(POLLOUT);
                const auto size = ::send(fd_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
                if (size > 0)
                    bytes = bytes.subspan(size);
                else if (size == 0 || (errno != EINTR && errno != EAGAIN))
                    socket_error("containerd send");
            }
        }

        auto receive(std::span<std::uint8_t> bytes) const -> std::size_t {
            for (;;) {
                wait(POLLIN);
                const auto size = ::recv(fd_.get(), bytes.data(), bytes.size(), 0);
                if (size > 0)
                    return size;
                if (size == 0)
                    throw std::runtime_error("containerd closed the connection before completing the RPC");
                if (errno != EINTR && errno != EAGAIN)
                    socket_error("containerd receive");
            }
        }

    private:
        void wait(short events) const {
            for (;;) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline_ - clock::now());
                if (remaining.count() <= 0)
                    throw std::runtime_error("containerd RPC deadline exceeded");
                pollfd entry{fd_.get(), events, 0};
                const auto result = ::poll(&entry, 1, static_cast<int>(remaining.count()));
                if (result > 0)
                    return;
                if (result < 0 && errno != EINTR)
                    socket_error("containerd poll");
            }
        }

        descriptor::fd fd_{-1};
        clock::time_point deadline_;
    };

    void check(nghttp2_ssize result) {
        if (result < 0)
            throw std::runtime_error(std::format("containerd HTTP/2 error ({})", result));
    }

    auto frame(std::string_view message) -> std::string {
        if (message.size() > message_limit)
            throw std::length_error("containerd request exceeds the message limit");
        std::string body(5, '\0');
        for (unsigned i = 0; i < 4; ++i)
            body[4 - i] = static_cast<char>((message.size() >> (8 * i)) & 255);
        body.append(message);
        return body;
    }

    auto unframe(std::string_view response) -> std::string_view {
        if (response.size() < 5 || response.front() != '\0')
            throw std::runtime_error("invalid containerd gRPC response frame");
        std::uint32_t size = 0;
        for (auto byte : response.substr(1, 4))
            size = (size << 8) | static_cast<unsigned char>(byte);
        if (size != response.size() - 5)
            throw std::runtime_error("containerd gRPC response must contain one complete message");
        return response.substr(5);
    }

    auto header(std::string_view name, std::string_view value) -> nghttp2_nv {
        return {reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())), reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
                name.size(), value.size(), NGHTTP2_NV_FLAG_NONE};
    }

    struct exchange {
        std::string_view outgoing;
        std::string incoming, grpc_message;
        std::exception_ptr failure;
        std::optional<unsigned> grpc_status;
        bool http_ok = false, grpc_content = false, stream_closed = false;
        std::uint32_t close_code = 0;
    };

    auto status_message(std::string_view text) -> std::string {
        std::string result;
        for (std::size_t i = 0; i < text.size(); ++i) {
            auto byte = static_cast<unsigned char>(text[i]);
            if (byte == '%' && text.size() - i >= 3) {
                unsigned decoded = 0;
                const auto [end, error] = std::from_chars(text.data() + i + 1, text.data() + i + 3, decoded, 16);
                if (error == std::errc{} && end == text.data() + i + 3) {
                    byte = static_cast<unsigned char>(decoded);
                    i += 2;
                }
            }
            result.push_back(byte >= 0x20 && byte != 0x7f ? static_cast<char>(byte) : ' ');
        }
        return result;
    }

    struct identity { protobuf::field<1, std::string> id; };

    auto rpc(std::string_view socket, std::string_view ns, std::string_view method, std::string_view message) -> std::optional<std::string> {
        auto data = frame(message);
        exchange state{};
        state.outgoing = data;
        nghttp2_session_callbacks* callbacks_raw = nullptr;
        check(nghttp2_session_callbacks_new(&callbacks_raw));
        const std::unique_ptr<nghttp2_session_callbacks, decltype(&nghttp2_session_callbacks_del)> callbacks{callbacks_raw, nghttp2_session_callbacks_del};
        nghttp2_session_callbacks_set_on_header_callback(callbacks.get(), [](nghttp2_session*, const nghttp2_frame* frame, const std::uint8_t* name,
                std::size_t name_length, const std::uint8_t* value, std::size_t value_length, std::uint8_t, void* user) -> int {
            auto& state = *static_cast<exchange*>(user);
            const std::string_view key{reinterpret_cast<const char*>(name), name_length};
            const std::string_view text{reinterpret_cast<const char*>(value), value_length};
            if (key == ":status")
                state.http_ok = text == "200";
            else if (key == "content-type")
                state.grpc_content = text == "application/grpc" || text.starts_with("application/grpc+");
            else if (key == "grpc-status") {
                unsigned code = 0;
                const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), code);
                if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM) || state.grpc_status || error != std::errc{} || end != text.data() + text.size())
                    return NGHTTP2_ERR_CALLBACK_FAILURE;
                state.grpc_status = code;
            } else if (key == "grpc-message" && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
                try {
                    state.grpc_message = status_message(text.substr(0, 4096));
                } catch (...) {
                    state.failure = std::current_exception();
                    return NGHTTP2_ERR_CALLBACK_FAILURE;
                }
            }
            return 0;
        });
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks.get(), [](nghttp2_session*, std::uint8_t, std::int32_t,
                const std::uint8_t* bytes, std::size_t size, void* user) -> int {
            auto& state = *static_cast<exchange*>(user);
            if (size > message_limit + 5 - state.incoming.size())
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            try {
                state.incoming.append(reinterpret_cast<const char*>(bytes), size);
            } catch (...) {
                state.failure = std::current_exception();
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            return 0;
        });
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks.get(), [](nghttp2_session*, std::int32_t, std::uint32_t code, void* user) -> int {
            auto& state = *static_cast<exchange*>(user);
            state.stream_closed = true;
            state.close_code = code;
            return 0;
        });
        nghttp2_session* session_raw = nullptr;
        check(nghttp2_session_client_new2(&session_raw, callbacks.get(), &state, nullptr));
        const std::unique_ptr<nghttp2_session, decltype(&nghttp2_session_del)> session{session_raw, nghttp2_session_del};
        const nghttp2_settings_entry settings[]{{NGHTTP2_SETTINGS_ENABLE_PUSH, 0}, {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, 16u << 10}};
        check(nghttp2_submit_settings(session.get(), NGHTTP2_FLAG_NONE, settings, std::size(settings)));
        const std::array fields{header(":method", "POST"), header(":scheme", "http"), header(":authority", "localhost"), header(":path", method),
                                header("content-type", "application/grpc"), header("te", "trailers"), header("containerd-namespace", ns),
                                header("grpc-timeout", "10S")};
        const nghttp2_data_provider2 body{.source = {}, .read_callback = [](nghttp2_session*, std::int32_t, std::uint8_t* out, std::size_t size,
                std::uint32_t* flags, nghttp2_data_source*, void* user) -> nghttp2_ssize {
            auto& outgoing = static_cast<exchange*>(user)->outgoing;
            const auto chunk = outgoing.substr(0, size);
            std::ranges::copy(chunk, out);
            outgoing.remove_prefix(chunk.size());
            if (outgoing.empty())
                *flags |= NGHTTP2_DATA_FLAG_EOF;
            return chunk.size();
        }};
        check(nghttp2_submit_request2(session.get(), nullptr, fields.data(), fields.size(), &body, nullptr));
        const connection peer{socket, clock::now() + std::chrono::seconds{10}};
        while (!state.stream_closed) {
            for (;;) {
                const std::uint8_t* bytes = nullptr;
                const auto size = nghttp2_session_mem_send2(session.get(), &bytes);
                check(size);
                if (!size)
                    break;
                peer.send({bytes, static_cast<std::size_t>(size)});
            }
            std::array<std::uint8_t, 16u << 10> input;
            const auto size = peer.receive(input);
            const auto consumed = nghttp2_session_mem_recv2(session.get(), input.data(), size);
            if (state.failure)
                std::rethrow_exception(state.failure);
            check(consumed);
            if (static_cast<std::size_t>(consumed) != size)
                throw std::runtime_error("incomplete containerd HTTP/2 receive");
        }
        if (state.close_code || !state.http_ok || !state.grpc_content || !state.grpc_status)
            throw std::runtime_error("containerd did not complete the gRPC response");
        if (*state.grpc_status == 5 && state.incoming.empty())
            return std::nullopt;
        if (*state.grpc_status)
            throw std::runtime_error(std::format("containerd gRPC status {} for {}{}{}", *state.grpc_status, method,
                                                 state.grpc_message.empty() ? "" : ": ", state.grpc_message));
        return std::string{unframe(state.incoming)};
    }

    struct any {
        protobuf::field<1, std::string> type_url;
        protobuf::field<2, std::string> value;
    };
    struct label {
        protobuf::field<1, std::string> key;
        protobuf::field<2, std::string> value;
    };
    struct runtime { protobuf::field<1, std::string> name; };
    struct metadata {
        protobuf::field<1, std::string> id;
        protobuf::field<2, std::vector<label>> labels;
        protobuf::field<4, std::optional<runtime>> engine;
        protobuf::field<5, std::optional<any>> spec;
    };
    struct container_message { protobuf::field<1, std::optional<metadata>> container; };
    struct task_create {
        protobuf::field<1, std::string> id;
        protobuf::field<3, std::vector<mount>> rootfs;
    };
    struct task_kill {
        protobuf::field<1, std::string> id;
        protobuf::field<3, std::uint32_t> signal;
        protobuf::field<4, bool> all;
    };

    constexpr std::string_view owner_label = "io.fughetta.environment";
    constexpr std::string_view spec_type = "types.containerd.io/opencontainers/runtime-spec/1/Spec";
    constexpr std::uint32_t created = 1, running = task_running, stopped = task_stopped;

    template <class Request> auto task_call(std::string_view socket, std::string_view ns, std::string_view method, const Request& request) {
        return rpc(socket, ns, std::string{"/containerd.services.tasks.v1.Tasks/"} + std::string{method}, protobuf::write(request));
    }

    template <class Request> auto container_call(std::string_view socket, std::string_view ns, std::string_view method, const Request& request) {
        return rpc(socket, ns, std::string{"/containerd.services.containers.v1.Containers/"} + std::string{method}, protobuf::write(request));
    }

    auto owned_container(std::string_view socket, std::string_view ns, const identity& id) -> std::optional<metadata> {
        auto response = container_call(socket, ns, "Get", id);
        if (!response)
            return {};
        auto result = protobuf::read<container_message>(*response);
        if (!result.container.get() || result.container.get()->id.get() != id.id.get() ||
            !std::ranges::any_of(result.container.get()->labels.get(), [&](const label& tag) {
                return tag.key.get() == owner_label && tag.value.get() == id.id.get();
            }))
            throw std::runtime_error("containerd container is not owned by this Fughetta environment");
        return std::move(result.container).get();
    }
}

export namespace containerd {
    auto get_task(std::string_view socket, std::string_view ns, std::string_view id) -> std::optional<task> {
        if (ns.empty() || id.empty())
            throw std::invalid_argument("containerd requires a namespace and task ID");
        struct reply { protobuf::field<1, std::optional<task>> process; };
        auto response = detail::task_call(socket, ns, "Get", detail::identity{std::string{id}});
        if (!response)
            return std::nullopt;
        auto decoded = protobuf::read<reply>(*response);
        auto result = std::move(decoded.process).get();
        if (!result || result->id.get() != id)
            throw std::runtime_error("containerd returned a different task identity");
        return result;
    }

    auto start(std::string_view socket, std::string_view ns, const std::string& id, const workload& work) -> task {
        using namespace detail;
        if (ns.empty() || id.empty() || work.runtime.empty() || work.spec.empty() || work.rootfs.empty())
            throw std::invalid_argument("containerd requires a prepared runtime, OCI spec, and root filesystem");
        const identity key{id};
        auto record = owned_container(socket, ns, key);
        auto state = get_task(socket, ns, id);
        if (record) {
            if (!record->engine.get() || record->engine.get()->name.get() != work.runtime || !record->spec.get() ||
                record->spec.get()->type_url.get() != spec_type || record->spec.get()->value.get() != work.spec)
                throw std::runtime_error("containerd environment already has a different runtime spec");
        } else {
            if (state)
                throw std::runtime_error("cannot adopt a task without its Fughetta container record");
            metadata definition{.id = id, .labels = std::vector{label{owner_label, id}}, .engine = runtime{work.runtime},
                                .spec = any{spec_type, work.spec}};
            if (!container_call(socket, ns, "Create", container_message{std::move(definition)}))
                throw std::runtime_error("containerd did not create the environment container");
        }
        if (state && state->status.get() == stopped) {
            (void)task_call(socket, ns, "Delete", key);
            state.reset();
        }
        if (!state && !task_call(socket, ns, "Create", task_create{id, work.rootfs}))
            throw std::runtime_error("containerd did not create the environment task");
        if ((!state || state->status.get() == created) && !task_call(socket, ns, "Start", key))
            throw std::runtime_error("containerd environment task disappeared before start");
        state = get_task(socket, ns, id);
        if (!state || state->status.get() != running || !state->pid.get())
            throw std::runtime_error("containerd environment task did not reach running state");
        return *state;
    }

    void stop(std::string_view socket, std::string_view ns, const std::string& id) {
        using namespace detail;
        const identity key{id};
        const auto record = owned_container(socket, ns, key);
        if (auto state = get_task(socket, ns, id)) {
            if (!record)
                throw std::runtime_error("cannot stop a task without its Fughetta container record");
            if (state->status.get() != stopped) {
                (void)task_call(socket, ns, "Kill", task_kill{id, std::uint32_t{SIGKILL}, true});
                (void)task_call(socket, ns, "Wait", key);
            }
            (void)task_call(socket, ns, "Delete", key);
        }
        if (record)
            (void)container_call(socket, ns, "Delete", key);
        if (get_task(socket, ns, id) || owned_container(socket, ns, key))
            throw std::runtime_error("containerd environment has not been reaped");
    }
}
