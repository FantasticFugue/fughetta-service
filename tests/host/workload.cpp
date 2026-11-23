#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yyjson.h>

import std;

namespace {
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;

    void require(bool condition, std::string_view message) {
        if (!condition)
            throw std::runtime_error(std::string{message});
    }

    struct socket_handle {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        socket_handle() { require(fd >= 0, "create TCP socket"); }
        socket_handle(const socket_handle&) = delete;
        ~socket_handle() { ::close(fd); }
    };

    auto address(std::string_view ip, unsigned port) -> sockaddr_in {
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = ::htons(static_cast<std::uint16_t>(port));
        require(::inet_pton(AF_INET, std::string{ip}.c_str(), &endpoint.sin_addr) == 1, "numeric IPv4 endpoint");
        return endpoint;
    }

    auto listen(socket_handle& socket) -> unsigned {
        auto endpoint = address("127.0.0.1", 0);
        auto length = static_cast<socklen_t>(sizeof(endpoint));
        require(::bind(socket.fd, reinterpret_cast<const sockaddr*>(&endpoint), length) == 0 &&
                ::listen(socket.fd, 1) == 0 &&
                ::getsockname(socket.fd, reinterpret_cast<sockaddr*>(&endpoint), &length) == 0, "listen on loopback");
        return ::ntohs(endpoint.sin_port);
    }

    auto connect(socket_handle& socket, std::string_view ip, unsigned port) -> bool {
        const auto endpoint = address(ip, port);
        require(::fcntl(socket.fd, F_SETFL, O_NONBLOCK) == 0, "set nonblocking test socket");
        if (::connect(socket.fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0)
            return true;
        pollfd event{socket.fd, POLLOUT, 0};
        int error = 0;
        auto length = static_cast<socklen_t>(sizeof(error));
        return errno == EINPROGRESS && ::poll(&event, 1, 800) == 1 &&
            ::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
    }

    auto reaches(std::string_view ip, unsigned port) -> bool {
        socket_handle peer;
        return connect(peer, ip, port);
    }

    auto read_only(const fs::path& path) -> bool {
        const int file = ::open(path.c_str(), O_RDWR);
        if (file < 0)
            return errno == EROFS;
        ::close(file);
        return false;
    }

    void publish(const fs::path& file, const std::map<std::string, bool>& result) {
        const auto draft = file.string() + ".tmp";
        {
            std::ofstream output{draft};
            std::string_view separator;
            output << '{' << std::boolalpha;
            for (const auto& [key, value] : result) {
                output << separator << std::quoted(key) << ':' << value;
                separator = ",";
            }
            output << '}';
        }
        fs::rename(draft, file);
    }

    auto wait_for(pid_t pid) -> int {
        int state = 0;
        while (::waitpid(pid, &state, 0) < 0)
            if (errno != EINTR)
                throw std::runtime_error("wait for fixture process");
        return state;
    }

    auto limits() -> std::map<std::string, bool> {
        std::vector<pid_t> children;
        bool pids = false;
        for (unsigned count = 0; count < 16; ++count) {
            const auto pid = ::fork();
            if (pid < 0) {
                pids = errno == EAGAIN && children.size() == 15;
                break;
            }
            if (!pid) {
                ::sleep(30);
                ::_exit(0);
            }
            children.push_back(pid);
        }
        for (const auto pid : children) {
            ::kill(pid, SIGKILL);
            wait_for(pid);
        }
        const auto pid = ::fork();
        require(pid >= 0, "fork memory-limit fixture");
        if (!pid) {
            auto data = std::make_unique<unsigned char[]>(256u << 20);
            volatile auto* memory = data.get();
            for (std::size_t offset = 0; offset < (256u << 20); offset += 4096)
                memory[offset] = 1;
            ::_exit(0);
        }
        const auto state = wait_for(pid);
        const int fd = ::open("/work/fill", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        require(fd >= 0, "open storage-limit fixture");
        const auto block = std::make_unique<char[]>(1u << 20);
        bool full = false;
        for (unsigned n = 0; n < 64 && !full; ++n)
            full = ::write(fd, block.get(), 1u << 20) < 0 && errno == ENOSPC;
        full = (::fsync(fd) != 0 && errno == ENOSPC) || full;
        ::close(fd);
        ::unlink("/work/fill");
        return {{"pids", pids}, {"memory", WIFSIGNALED(state) && WTERMSIG(state) == SIGKILL}, {"storage", full}};
    }

    void isolation_workload() {
        std::string ip;
        unsigned allowed = 0, host_allowed = 0, denied = 0;
        std::ifstream{"/opt/fug/toolchain/endpoints"} >> ip >> allowed >> host_allowed >> denied;
        socket_handle local;
        auto result = limits();
        result.insert({{"toolchain", read_only("/opt/fug/toolchain/marker")}, {"shared", read_only("/opt/fug/shared/marker")},
                       {"root", read_only("/marker")}, {"loopback", reaches("127.0.0.1", listen(local))},
                       {"allowed", reaches(ip, allowed)}, {"host_allowed", reaches(ip, host_allowed)}, {"denied", reaches(ip, denied)}});
        publish("/work/result.json", result);
    }

    void pool_workload(const fs::path& binding) {
        std::string ip;
        unsigned core = 0, denied = 0;
        std::uint32_t kind = 0;
        std::ifstream{"/opt/fug/toolchain/pool-endpoints"} >> ip >> core >> denied >> kind;
        publish("/work/warm.json", {{"core", reaches(ip, core)}, {"denied", reaches(ip, denied)}, {"binding", fs::exists(binding)}});
        const auto deadline = std::chrono::steady_clock::now() + 45s;
        while (!fs::exists(binding) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(25ms);
        const std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> cfg{
            yyjson_read_file(binding.c_str(), 0, nullptr, nullptr), yyjson_doc_free};
        require(cfg != nullptr, "load the Worker binding");
        const auto field = [&](const char* name) {
            const auto* value = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(cfg.get()), name));
            require(value != nullptr && *value, std::format("missing binding field {}", name));
            return value;
        };
        const std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> hello{
            yyjson_read_file("/opt/fug/toolchain/hello.json", 0, nullptr, nullptr), yyjson_doc_free};
        require(hello != nullptr, "load the Worker hello");
        const std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> message_doc{
            yyjson_doc_mut_copy(hello.get(), nullptr), yyjson_mut_doc_free};
        require(message_doc != nullptr, "copy the Worker hello");
        auto* value = yyjson_mut_doc_get_root(message_doc.get());
        for (const auto& [key, field_name] : {std::pair{"name", "name"}, std::pair{"auth", "token"}})
            require(yyjson_mut_obj_put(value, yyjson_mut_str(message_doc.get(), key), yyjson_mut_str(message_doc.get(), field(field_name))),
                    "set Worker hello identity");
        std::size_t size = 0;
        const std::unique_ptr<char, decltype(&std::free)> body{yyjson_mut_write(message_doc.get(), 0, &size), std::free};
        require(body != nullptr, "serialize the Worker hello");
        const auto length = static_cast<std::uint32_t>(size);
        std::vector<char> message(sizeof(length) + sizeof(kind) + sizeof(std::uint64_t));
        std::memcpy(message.data(), &length, sizeof(length));
        std::memcpy(message.data() + sizeof(length), &kind, sizeof(kind));
        message.append_range(std::span{body.get(), size});
        socket_handle peer;
        const bool connected = connect(peer, ip, core);
        require(connected && ::send(peer.fd, message.data(), message.size(), MSG_NOSIGNAL) ==
                      static_cast<ssize_t>(message.size()), "send managed Worker hello");
        publish("/work/claimed.json", {{"core", connected}, {"denied", reaches(ip, denied)},
                                      {"binding", field("gateway") == std::format("{}:{}", ip, core)}});
        for (;;)
            ::pause();
    }

}

int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view{argv[3]} == "--await-binding")
            pool_workload(argv[4]);
        else
            isolation_workload();
        for (;;)
            ::pause();
    } catch (const std::exception& error) {
        std::ofstream{"/work/fixture-error"} << error.what() << '\n';
        return 2;
    }
}
