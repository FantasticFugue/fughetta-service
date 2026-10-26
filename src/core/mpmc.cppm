export module mpmc;
import std;

export namespace concurrent {
    // Vyukov ring: each cell publishes its value and reuse through a sequence.
    template <class T, std::size_t Capacity = 4096> class queue {
        static_assert(Capacity >= 2 && std::has_single_bit(Capacity));
        static_assert(std::atomic<std::size_t>::is_always_lock_free);
        static_assert(std::is_nothrow_move_assignable_v<T>);
        struct cell {
            std::atomic<std::size_t> sequence{0};
            T value{};
        };
        std::unique_ptr<cell[]> cells_{std::make_unique<cell[]>(Capacity)};
        alignas(64) std::atomic<std::size_t> head_{0};
        alignas(64) std::atomic<std::size_t> tail_{0};
    public:
        queue() {
            for (std::size_t i = 0; i < Capacity; ++i)
                cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
        auto push(T value) noexcept -> bool {
            auto pos = tail_.load(std::memory_order_relaxed);
            cell* slot;
            for (;;) {
                slot = &cells_[pos & (Capacity - 1)];
                auto sequence = slot->sequence.load(std::memory_order_acquire);
                auto difference = std::bit_cast<std::make_signed_t<std::size_t>>(sequence - pos);
                if (difference == 0) {
                    if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
                } else if (difference < 0) return false;
                else pos = tail_.load(std::memory_order_relaxed);
            }
            slot->value = std::move(value);
            slot->sequence.store(pos + 1, std::memory_order_release);
            return true;
        }
        auto try_pop(T& value) noexcept -> bool {
            auto pos = head_.load(std::memory_order_relaxed);
            cell* slot;
            for (;;) {
                slot = &cells_[pos & (Capacity - 1)];
                auto sequence = slot->sequence.load(std::memory_order_acquire);
                auto difference = std::bit_cast<std::make_signed_t<std::size_t>>(sequence - (pos + 1));
                if (difference == 0) {
                    if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
                } else if (difference < 0) return false;
                else pos = head_.load(std::memory_order_relaxed);
            }
            value = std::move(slot->value);
            slot->sequence.store(pos + Capacity, std::memory_order_release);
            return true;
        }
    };
}
