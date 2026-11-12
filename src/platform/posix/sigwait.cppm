module;
#include <pthread.h>
#include <signal.h>

module signals;
import std;
import posix;

namespace cores {
    struct signal_watch::state {
        sigset_t set{};
        std::atomic<bool> done{false};
        std::jthread watcher;
        state(void* owner, void (*stop)(void*) noexcept) {
            sigemptyset(&set);
            sigaddset(&set, SIGINT);
            sigaddset(&set, SIGTERM);
            ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
            watcher = std::jthread([this, owner, stop] {
                int sig = 0;
                ::sigwait(&set, &sig);
                if (!done.load())
                    stop(owner);
            });
        }
        ~state() {
            done.store(true);
            ::pthread_kill(watcher.native_handle(), SIGTERM);
        }
    };
    signal_watch::signal_watch(void* owner, void (*stop)(void*) noexcept) : state_(std::make_unique<state>(owner, stop)) {}
    signal_watch::~signal_watch() = default;
} // namespace cores

namespace daemon {
    void publish(phase value) {
        if (current.exchange(value) == value)
            return;
        std::string message = "STATUS=" + std::string{name(value)};
        if (value == phase::ready)
            message += "\nREADY=1";
        if (value == phase::stopping)
            message += "\nSTOPPING=1";
        posix::notify(message);
    }
    auto run(int argc, char** argv, int (*application)(int, char**), bool, std::string_view instance) -> int {
        identity = instance;
        auto result = application(argc, argv);
        if (current.load() != phase::starting)
            publish(phase::stopped);
        return result;
    }
} // namespace daemon
