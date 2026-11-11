module front;
import std;
import native;
import coroutine;
import configuration;
import utility;

// Publishes a configuration edit: core zero applies it on the disk pool, then every core takes the new snapshot.
namespace front {
    namespace {
        struct install_snapshot {
            config::slots* slots;
            std::size_t core;
            config::store::snapshot snapshot;
            auto operator()() const {
                slots->set(core, snapshot);
                return execution::just();
            }
        };
    }
    auto config_hub::apply(util::function_ref<void(config::store&)> f) -> coroutine::task<> {
        auto held = co_await lock_.lock();
        config::store::snapshot snap;
        co_await io::blocking(rt_.get_scheduler(0), pool_, [this, &snap, f] {
            f(store_);
            snap = store_.current();
        });
        // The store has changed, so every core takes the new snapshot even if the edit is cancelled now.
        co_await coroutine::under(coroutine::stop_token{}, publish(std::move(snap)));
    }

    auto config_hub::publish(config::store::snapshot snap) -> coroutine::task<> {
        set(0, snap);
        for (std::size_t c = 1; c < rt_.size(); ++c)
            co_await cores::call(rt_.get_scheduler(0), rt_.get_scheduler(c), install_snapshot{this, c, snap});
        events_.publish(8);
    }
} // namespace front
