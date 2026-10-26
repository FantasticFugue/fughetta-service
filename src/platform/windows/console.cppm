module;
#include <windows.h>
module signals;
import std;
import winapi;
namespace cores {
    namespace {
        std::mutex watcher_mutex;
        HANDLE notification = nullptr;
        BOOL WINAPI console_handler(DWORD event) {
            if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT && event != CTRL_SHUTDOWN_EVENT)
                return FALSE;
            std::lock_guard lock{watcher_mutex};
            if (!notification)
                return FALSE;
            ::SetEvent(notification);
            return TRUE;
        }
    } // namespace
    struct signal_watch::state {
        winapi::handle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        std::jthread watcher;
        state(void* owner, void (*stop)(void*) noexcept) {
            if (!event.value)
                winapi::fail("CreateEvent signal watcher");
            {
                std::lock_guard lock{watcher_mutex};
                if (notification)
                    throw std::runtime_error("a console signal watcher already exists");
                if (!::SetConsoleCtrlHandler(console_handler, TRUE))
                    winapi::fail("SetConsoleCtrlHandler");
                notification = event.value;
            }
            try {
                watcher = std::jthread{[this, owner, stop](std::stop_token token) {
                    ::WaitForSingleObject(event.value, INFINITE);
                    if (!token.stop_requested())
                        stop(owner);
                }};
            } catch (...) {
                detach();
                throw;
            }
        }
        void detach() noexcept {
            std::lock_guard lock{watcher_mutex};
            ::SetConsoleCtrlHandler(console_handler, FALSE);
            notification = nullptr;
        }
        ~state() {
            detach();
            watcher.request_stop();
            ::SetEvent(event.value);
            watcher.join();
        }
    };
    signal_watch::signal_watch(void* owner, void (*stop)(void*) noexcept) : state_(std::make_unique<state>(owner, stop)) {}
    signal_watch::~signal_watch() = default;
} // namespace cores
