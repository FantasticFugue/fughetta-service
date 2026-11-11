export module signals;
import std;

export namespace cores {
    class signal_watch {
    public:
        template <class Context> explicit signal_watch(Context& rt) : signal_watch(&rt, [](void* owner) noexcept { static_cast<Context*>(owner)->stop(); }) {}
        signal_watch(const signal_watch&) = delete;
        ~signal_watch();

    private:
        signal_watch(void* owner, void (*stop)(void*) noexcept);
        struct state;
        std::unique_ptr<state> state_;
    };
} // namespace cores

// The platform host owns the service-manager handshake; the application owns scheduling.
export namespace daemon {
    auto run(int argc, char** argv, int (*application)(int, char**), bool service, std::string_view identity) -> int;
    enum class phase { starting, initialized, ready, draining, stopping, stopped };
    inline std::atomic<phase> current{phase::starting};
    inline std::string identity;
    auto name(phase value) -> std::string_view;
    void publish(phase value);
}

namespace daemon {
    auto name(phase value) -> std::string_view {
        switch (value) {
        case phase::starting: return "starting";
        case phase::initialized: return "initialized";
        case phase::ready: return "ready";
        case phase::draining: return "draining";
        case phase::stopping: return "stopping";
        case phase::stopped: return "stopped";
        }
        return "starting";
    }
}
