import std;
import boost.ut;
import configuration;
import backend;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "execute child starts directly with its resource bounds and standard streams"_test = [] {
            const config::worker_backend limits{.max_memory_bytes = 1ull << 30, .max_cpu_seconds = 600};
            const auto command = backend::launch(limits, "/bin/service", "/work/config.json", "/work/execute", {{"PATH", "/usr/bin"}});
            expect(command.interactive);
            expect(command.argv == std::vector<std::string>{"/bin/service", "--role", "worker", "/work/config.json"});
            expect(command.cwd == "/work/execute" && command.env->at("PATH") == "/usr/bin");
            expect(command.max_memory_bytes == (1ull << 30) && command.max_cpu_seconds == 600);
        };
    };
}
auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"backend">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
