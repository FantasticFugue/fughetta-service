module;
#include <windows.h>
module signals;
import std;
import winapi;
namespace cores {
    namespace {
        std::mutex watcher_mutex;
        HANDLE notification = nullptr;
        bool service_stop = false;
        BOOL WINAPI console_handler(DWORD event) {
            if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT && event != CTRL_SHUTDOWN_EVENT)
                return FALSE;
            std::lock_guard lock{watcher_mutex};
            if (!notification) {
                service_stop = true;
                return TRUE;
            }
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
                if (service_stop) ::SetEvent(notification);
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

namespace daemon {
    namespace {
        SERVICE_STATUS_HANDLE handle = nullptr;
        SERVICE_STATUS status{SERVICE_WIN32_OWN_PROCESS};
        std::mutex status_mutex;
        int saved_argc;
        char** saved_argv;
        int (*entry)(int, char**);
        int result = 1;
        void report(DWORD state, DWORD error = NO_ERROR) {
            std::lock_guard lock{status_mutex};
            status.dwCurrentState = state;
            status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
            status.dwWin32ExitCode = error;
            status.dwWaitHint = state == SERVICE_STOP_PENDING ? 15000 : 0;
            ::SetServiceStatus(handle, &status);
        }
        void WINAPI control(DWORD event) {
            if (event == SERVICE_CONTROL_STOP || event == SERVICE_CONTROL_SHUTDOWN) {
                report(SERVICE_STOP_PENDING);
                cores::console_handler(CTRL_SHUTDOWN_EVENT);
            }
        }
        void WINAPI start(DWORD, LPWSTR*) {
            handle = ::RegisterServiceCtrlHandlerW(L"fughetta-service", control);
            if (!handle) return;
            report(SERVICE_RUNNING);
            result = entry(saved_argc, saved_argv);
            report(SERVICE_STOPPED, result ? ERROR_PROCESS_ABORTED : NO_ERROR);
        }
    }
    auto run(int argc, char** argv, int (*application)(int, char**), bool service) -> int {
        if (!service) return application(argc, argv);
        saved_argc = argc; saved_argv = argv; entry = application;
        SERVICE_TABLE_ENTRYW table[]{{const_cast<LPWSTR>(L"fughetta-service"), start}, {nullptr, nullptr}};
        if (!::StartServiceCtrlDispatcherW(table)) return 1;
        return result;
    }
}
