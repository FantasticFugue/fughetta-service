import std;
import boost.ut;
import host;

namespace {
    using namespace boost::ut;

    static constexpr auto host_suite = [] {
        "environment snapshots preserve values containing equals signs"_test = [] {
            auto env = host::environment();
            auto expected = std::getenv("HOST_TEST_VALUE");
            expect(expected != nullptr) << fatal;
            if (!expected)
                return;
            expect(env.contains("HOST_TEST_VALUE"));
            if (env.contains("HOST_TEST_VALUE"))
                expect(env.at("HOST_TEST_VALUE") == expected);
            expect(!host::name().empty());
        };

        // Nothing here names a tool the build machine is assumed to have: the sorted, unique shape is
        // what is checked, and which names are present is the machine's business.
        "tool detection returns sorted unique executable names"_test = [] {
            auto tools = host::detect_tools();
            expect(std::ranges::is_sorted(tools));
            expect(std::ranges::adjacent_find(tools) == tools.end());
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"host">{host_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
