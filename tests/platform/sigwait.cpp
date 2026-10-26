#include <pthread.h>
#include <signal.h>
#include <unistd.h>

import std;
import signals;

struct context {
    std::atomic<bool> stopped{false};
    void stop() noexcept { stopped.store(true); }
};

int main() {
    sigset_t previous{};
    ::pthread_sigmask(SIG_SETMASK, nullptr, &previous);
    for (int signal : {SIGINT, SIGTERM}) {
        context owner;
        {
            cores::signal_watch watch{owner};
            if (::kill(::getpid(), signal) != 0)
                return 1;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (!owner.stopped.load()) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return 2;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    }
    context unused;
    {
        cores::signal_watch watch{unused};
    }
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (unused.stopped.load())
        return 3;
    std::println("PASS: interrupt, termination, and destruction without a shutdown request");
}
