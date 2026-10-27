import std;
import boost.ut;
import allocation;

namespace {
    using namespace boost::ut;
    const auto cases = [] {
        "draining retains accepted work and permits control writes"_test = [] {
            allocation::budget initial;
            allocation::control control{initial, 4};
            expect(!control.enter(false));
            control.ready(4);
            auto work = control.enter(false);
            expect(work.has_value());
            control.drain();
            expect(!control.enter(false));
            auto status = control.enter(true);
            expect(status.has_value());
            auto next = initial;
            next.jobs = 4;
            auto observed = control.observe(5, next);
            expect(observed.phase == "draining" && observed.applied == initial && observed.desired == next);
            expect(observed.applied_revision == 4_u && observed.revision == 5_u && observed.active_requests == 1_u);
            work.reset();
            expect(!control.idle());
            status.reset();
            expect(control.idle());
        };
        "a saved budget is pending until the running instance applies it"_test = [] {
            allocation::budget initial;
            allocation::control control{initial, 0};
            control.ready(0);
            auto desired = initial;
            desired.workers = 0;
            auto observed = control.observe(1, desired);
            expect(observed.phase == "pending" && observed.applied_revision == 0_u);
            allocation::control restarted{desired, 1};
            expect(restarted.observe(1, desired).phase == "starting");
            restarted.ready(1);
            expect(restarted.observe(1, desired).phase == "ready");
        };
    };
} // namespace
int main(int argc, const char** argv) {
    boost::ut::suite<"control">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
