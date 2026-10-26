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
