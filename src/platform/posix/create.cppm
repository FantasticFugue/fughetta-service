module;
#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

module process;
import std;
import mpmc;
import async;
import httpant.io;

namespace native_process {
    struct child::state {
        state(native_io::engine<concurrent::queue>* engine, int pid, int out, bool conversation, int pidfd)
            : engine(engine), pid(pid), out(out), conversation(conversation), exit(pidfd) {}
        ~state() {
            if (!reaped) {
                ::kill(-pid, SIGKILL);
                while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
                }
            }
            engine->cancel_file(out);
            ::close(out);
            if (exit >= 0) { engine->cancel_file(exit); ::close(exit); }
        }
        native_io::engine<concurrent::queue>* engine;
        int pid;
        int out;
        // A conversation is one socket for both directions, so the engine's stream operations apply to it.
        bool conversation;
        int exit;
        bool reaped = false;
        auto reap() noexcept -> int {
            ::kill(-pid, SIGKILL);
            int status = 0;
            pid_t got;
            do {
                got = ::waitpid(pid, &status, 0);
            } while (got < 0 && errno == EINTR);
            reaped = true;
            if (got != pid)
                return -1;
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            if (WIFSIGNALED(status))
                return 128 + WTERMSIG(status);
            return -1;
        }
    };

    namespace {
        constexpr long open_pid_call = 434;
        constexpr long close_range_call = 436;
        [[noreturn]] void exec_child(char** args, char** envp, const char* dir, int out, int in, int file, const io::command& cmd, int parent) {
#ifdef __linux__
            if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent)
                ::_exit(126);
#else
            if (::getppid() != parent) ::_exit(126);
#endif
            ::setpgid(0, 0);
            ::dup2(in, 0);
            ::dup2(file >= 0 ? file : out, 1);
            // A conversation keeps stdout for replies and discards its diagnostics, as on Windows.
            if (cmd.interactive) {
                int null = ::open("/dev/null", O_WRONLY);
                if (null < 0 || ::dup2(null, 2) < 0)
                    ::_exit(126);
            } else
                ::dup2(out, 2);
#ifdef __linux__
            if (::syscall(close_range_call, 3u, ~0u, 0) != 0) {
                constexpr char error[] = "worker: cannot close inherited descriptors; command not started\n";
                ::write(2, error, sizeof(error) - 1);
                ::_exit(126);
            }
#else
            ::closefrom(3);
#endif
            if (cmd.max_memory_bytes) {
                rlimit l{*cmd.max_memory_bytes, *cmd.max_memory_bytes};
                if (::setrlimit(RLIMIT_AS, &l) != 0) {
                    constexpr char message[] = "worker: cannot set RLIMIT_AS; command not started\n";
                    ::write(2, message, sizeof(message) - 1);
                    ::_exit(126);
                }
            }
            if (cmd.max_cpu_seconds) {
                rlimit l{*cmd.max_cpu_seconds, *cmd.max_cpu_seconds};
                if (::setrlimit(RLIMIT_CPU, &l) != 0) {
                    constexpr char message[] = "worker: cannot set RLIMIT_CPU; command not started\n";
                    ::write(2, message, sizeof(message) - 1);
                    ::_exit(126);
                }
            }
            if (::chdir(dir) != 0)
                ::_exit(126);
            environ = envp;
            ::execvp(args[0], args);
            ::_exit(127);
        }
    } // namespace
    auto child::spawn(native_io::engine<concurrent::queue>& engine, const io::command& cmd) -> child {
        if (cmd.argv.empty())
            throw std::invalid_argument("spawn: empty command");
        std::vector<char*> args;
        for (auto& a : cmd.argv)
            args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        std::vector<std::string> env_text;
        std::vector<char*> env;
        if (cmd.env) {
            for (auto& [k, v] : *cmd.env)
                env_text.push_back(k + "=" + v);
            for (auto& e : env_text)
                env.push_back(e.data());
            env.push_back(nullptr);
        }
        char** envp = cmd.env ? env.data() : environ;
        auto dir = cmd.cwd.string();

        int out[2];
        if (cmd.interactive ? ::socketpair(AF_UNIX, SOCK_STREAM, 0, out) < 0 : ::pipe(out) < 0)
            throw std::system_error(errno, std::system_category(), "child output");
        ::fcntl(out[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(out[1], F_SETFD, FD_CLOEXEC);
        int in = cmd.interactive ? out[1] : ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (in < 0) {
            auto err = errno;
            ::close(out[0]);
            ::close(out[1]);
            throw std::system_error(err, std::system_category(), "open /dev/null");
        }
        int file = -1;
        if (cmd.standard_output) {
            file = ::open(cmd.standard_output->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (file < 0) {
                auto err = errno;
                ::close(out[0]);
                ::close(out[1]);
                if (!cmd.interactive)
                    ::close(in);
                throw std::system_error(err, std::system_category(), "open child standard output");
            }
        }
        auto parent = ::getpid();
        auto pid = ::fork();
        if (pid < 0) {
            auto err = errno;
            ::close(out[0]);
            ::close(out[1]);
            if (!cmd.interactive)
                ::close(in);
            if (file >= 0)
                ::close(file);
            throw std::system_error(err, std::system_category(), "fork");
        }
        if (pid == 0)
            exec_child(args.data(), envp, dir.c_str(), out[1], in, file, cmd, parent);
        ::setpgid(pid, pid);
        ::close(out[1]);
        if (!cmd.interactive)
            ::close(in);
        if (file >= 0)
            ::close(file);
        int pidfd = -1;
#ifdef __linux__
        pidfd = static_cast<int>(::syscall(open_pid_call, pid, 0));
        if (pidfd < 0) {
            auto err = errno;
            ::kill(-pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            ::close(out[0]);
            throw std::system_error(err, std::system_category(), "pidfd_open");
        }
#endif
        return child{std::make_unique<state>(&engine, pid, out[0], cmd.interactive, pidfd)};
    }

    child::child(std::unique_ptr<state> state) : state_(std::move(state)) {}
    child::child(child&&) noexcept = default;
    auto child::operator=(child&&) noexcept -> child& = default;
    child::~child() = default;
    auto child::id() const noexcept -> int { return state_->pid; }
    auto child::output() const noexcept -> native_io::handle { return state_->out; }
    auto child::input() const noexcept -> native_io::handle { return state_->conversation ? state_->out : -1; }
    void child::close_input() noexcept {
        if (state_->conversation)
            ::shutdown(state_->out, SHUT_WR);
    }
    auto child::reaped() const noexcept -> bool { return state_->reaped; }
    void child::terminate() noexcept {
        if (!state_->reaped)
            ::kill(state_->pid, SIGTERM);
    }
    void child::kill() noexcept {
        if (!state_->reaped)
            ::kill(-state_->pid, SIGKILL);
    }
    void child::begin_wait(native_io::request& request) noexcept {
        if (state_->exit < 0) {
            request.kind = native_io::operation::timer;
            request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10};
            state_->engine->start(request);
            return;
        }
        request.kind = native_io::operation::readable;
        request.file = state_->exit;
        state_->engine->start(request);
    }
    auto child::ready(native_io::request& request, std::error_code& error) noexcept -> bool {
        if (error)
            return true;
        siginfo_t info{};
        int result;
        do {
            result = ::waitid(P_PID, state_->pid, &info, WEXITED | WNOHANG | WNOWAIT);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            error = {errno, std::generic_category()};
        else if (info.si_pid == 0) {
            request.kind = native_io::operation::timer;
            request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1};
            request.result_ready = false;
            state_->engine->start(request);
            return false;
        }
        return true;
    }
    auto child::reap() noexcept -> int { return state_->reap(); }
} // namespace native_process
