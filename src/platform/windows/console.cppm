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
                if (service_stop)
                    ::SetEvent(notification);
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
        std::wstring service_name;
        std::optional<std::chrono::steady_clock::time_point> stop_deadline;
        void report_locked(DWORD state, DWORD error = NO_ERROR) {
            if (state == SERVICE_STOP_PENDING && !stop_deadline)
                stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
            status.dwCurrentState = state;
            status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
            status.dwWin32ExitCode = error;
            bool pending = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
            status.dwWaitHint = pending ? 20000 : 0;
            status.dwCheckPoint = pending ? status.dwCheckPoint + 1 : 0;
            ::SetServiceStatus(handle, &status);
        }
        void report(DWORD state, DWORD error = NO_ERROR) {
            std::lock_guard lock{status_mutex};
            report_locked(state, error);
        }
        void WINAPI control(DWORD event) {
            if (event == SERVICE_CONTROL_STOP || event == SERVICE_CONTROL_SHUTDOWN) {
                report(SERVICE_STOP_PENDING);
                cores::console_handler(CTRL_SHUTDOWN_EVENT);
            } else if (event == SERVICE_CONTROL_INTERROGATE) {
                std::lock_guard lock{status_mutex};
                ::SetServiceStatus(handle, &status);
            }
        }
        void WINAPI start(DWORD, LPWSTR*) {
            handle = ::RegisterServiceCtrlHandlerW(service_name.c_str(), control);
            if (!handle)
                return;
            report(SERVICE_START_PENDING);
            std::jthread progress{[](std::stop_token token) {
                while (!token.stop_requested()) {
                    for (int i = 0; i < 10 && !token.stop_requested(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds{100});
                    std::lock_guard lock{status_mutex};
                    if (stop_deadline && std::chrono::steady_clock::now() >= *stop_deadline)
                        ::TerminateProcess(::GetCurrentProcess(), ERROR_TIMEOUT);
                    if (status.dwCurrentState == SERVICE_START_PENDING || status.dwCurrentState == SERVICE_STOP_PENDING)
                        report_locked(status.dwCurrentState);
                }
            }};
            result = entry(saved_argc, saved_argv);
            progress.request_stop();
            progress.join();
            current.store(phase::stopped);
            report(SERVICE_STOPPED, result ? ERROR_PROCESS_ABORTED : NO_ERROR);
        }
    } // namespace
    void publish(phase value) {
        if (current.exchange(value) == value)
            return;
        std::lock_guard lock{status_mutex};
        if (!handle || status.dwCurrentState == SERVICE_STOP_PENDING)
            return;
        if (value == phase::ready)
            report_locked(SERVICE_RUNNING);
        if (value == phase::stopping)
            report_locked(SERVICE_STOP_PENDING);
    }
    auto run(int argc, char** argv, int (*application)(int, char**), bool service, std::string_view instance) -> int {
        identity = instance;
        service_name.assign(instance.begin(), instance.end());
        if (!service)
            return application(argc, argv);
        saved_argc = argc;
        saved_argv = argv;
        entry = application;
        SERVICE_TABLE_ENTRYW table[]{{service_name.data(), start}, {nullptr, nullptr}};
        if (!::StartServiceCtrlDispatcherW(table))
            return 1;
        return result;
    }
} // namespace daemon
