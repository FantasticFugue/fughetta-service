import std;
import boost.ut;
import allocation;

namespace {
    using namespace boost::ut;
    const auto cases = [] {
        "the source reservation and worker shares fit the global job budget"_test = [] {
            for (std::uint32_t jobs = 2; jobs <= 64; ++jobs)
                for (std::uint32_t workers = 1; workers < jobs; ++workers) {
                    auto plan = allocation::distribute({.threads = 6 + 2 * workers, .jobs = jobs, .workers = workers});
                    std::uint32_t total = 1;
                    for (auto worker : plan.workers) {
                        expect(worker.parallel > 0_u);
                        total += worker.parallel;
                    }
                    expect(total == jobs);
                    expect(plan.source_disk >= 2_u && plan.gateway_disk >= 2_u);
                    expect(plan.gateway_io + plan.gateway_disk + plan.source_io + plan.source_disk + 2 * workers == 6 + 2 * workers);
                    expect(plan.workers.front().parallel - plan.workers.back().parallel <= 1_u);
                }
        };
        "extra execution threads stay inside the shared budget"_test = [] {
            auto plan = allocation::distribute({.threads = 13, .jobs = 8, .workers = 3});
            expect(plan.gateway_io + plan.gateway_disk + plan.source_io + plan.source_disk + 2 * plan.workers.size() == 13_u);
        };
        "file-only service needs no worker process"_test = [] {
            auto plan = allocation::distribute({.threads = 6, .jobs = 1, .workers = 0});
            expect(plan.workers.empty());
        };
        "invalid budgets fail before launching processes"_test = [] {
            for (auto budget : {allocation::budget{5, 1, 0}, allocation::budget{8, 0, 1}, allocation::budget{8, 1, 1}, allocation::budget{9, 3, 2}})
                expect(throws<std::invalid_argument>([&] { allocation::distribute(budget); }));
        };
    };
} // namespace
int main(int argc, const char** argv) {
    boost::ut::suite<"allocation">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
