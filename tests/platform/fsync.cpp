import std;
import boost.ut;
import durability;
import support;

namespace {
    using namespace boost::ut;
    static constexpr auto fsync_suite = [] {
        "publication sets and clears executable file mode"_test = [] {
            test::scratch scratch{"fsync-mode"};
            auto path = scratch.dir / "program";
            durability::durable_write(path, "executable", true);
            expect((std::filesystem::status(path).permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
            durability::durable_write(path, "data", false);
            expect((std::filesystem::status(path).permissions() & std::filesystem::perms::owner_exec) == std::filesystem::perms::none);
            expect(test::slurp(path) == "data");
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"fsync">{fsync_suite};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
