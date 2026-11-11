import std;
import boost.ut;
import mpmc;

int main() {
    using namespace boost::ut;
    "bounded"_test = [] {
        concurrent::queue<unsigned, 2> queue;
        unsigned value = 0;
        expect(!queue.try_pop(value));
        expect(queue.push(7));
        expect(queue.push(9));
        expect(!queue.push(11));
        expect(queue.try_pop(value) && value == 7);
        expect(queue.push(11));
        expect(queue.try_pop(value) && value == 9);
        expect(queue.try_pop(value) && value == 11);
        expect(!queue.try_pop(value));
    };
    "producers_consumers"_test = [] {
        constexpr unsigned producers = 4, consumers = 4, count = 100000;
        concurrent::queue<unsigned, 64> queue;
        std::vector<std::atomic<unsigned>> seen(producers * count);
        std::atomic<unsigned> finished{0}, consumed{0}, invalid{0};
        std::barrier start{producers + consumers};
        std::vector<std::jthread> threads;
        for (unsigned p = 0; p < producers; ++p)
            threads.emplace_back([&, p] {
                start.arrive_and_wait();
                for (unsigned i = 0; i < count; ++i)
                    while (!queue.push(p * count + i))
                        std::this_thread::yield();
                finished.fetch_add(1, std::memory_order_release);
            });
        for (unsigned c = 0; c < consumers; ++c)
            threads.emplace_back([&] {
                start.arrive_and_wait();
                for (;;) {
                    unsigned value;
                    if (queue.try_pop(value)) {
                        if (value < seen.size())
                            seen[value].fetch_add(1, std::memory_order_relaxed);
                        else
                            invalid.fetch_add(1, std::memory_order_relaxed);
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    } else if (finished.load(std::memory_order_acquire) == producers && consumed.load(std::memory_order_relaxed) == producers * count)
                        break;
                    else
                        std::this_thread::yield();
                }
            });
        threads.clear();
        expect(consumed.load() == producers * count);
        expect(invalid.load() == 0_u);
        for (auto& value : seen)
            expect(value.load() == 1_u);
    };
}
