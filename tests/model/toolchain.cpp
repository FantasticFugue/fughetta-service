import std;
import boost.ut;
import toolchain;
import usage;
import support;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "runtime identities hash executable bytes SDK manifests and effective environment"_test = [] {
            test::scratch tmp{"toolchain"};
            test::spit(tmp.dir / "compiler", "compiler version one");
            test::spit(tmp.dir / "sdk", "sdk version one");
            std::map<std::string, std::string> tools{{"cc", (tmp.dir / "compiler").string()}};
            std::map<std::string, std::string> sdk{{"native", (tmp.dir / "sdk").string()}};
            tools["missing"] = (tmp.dir / "disappeared").string();
            auto first = toolchain::inspect(tools, {}, sdk);
            expect(first.tools == std::vector<std::string>{"cc"} && first.id.size() == 64u);
            expect(toolchain::inspect(tools, {}, sdk).id == first.id);
            test::spit(tmp.dir / "compiler", "compiler version two");
            auto second = toolchain::inspect(tools, {}, sdk);
            expect(second.id != first.id);
            test::spit(tmp.dir / "sdk", "sdk version two");
            auto third = toolchain::inspect(tools, {}, sdk);
            expect(third.id != second.id);
            expect(toolchain::inspect(tools, {{"TARGET", "other"}}, sdk).id != third.id);
        };
        "call records retain each caller without charging another execution and survive replay"_test = [] {
            test::scratch tmp{"calls"};
            {
                usage::ledger ledger{tmp.dir};
                ledger.record(usage::call_event{.project = "app",
                                                .actor = "alice",
                                                .worldline = "main",
                                                .version = "v1",
                                                .calculation = "same",
                                                .reuse = "executed",
                                                .state = "succeeded"});
                ledger.record(usage::call_event{
                    .project = "app", .actor = "bob", .worldline = "branch", .version = "v2", .calculation = "same", .reuse = "cache", .state = "succeeded"});
                expect(ledger.builds_last_hour("app") == 0u);
            }
            usage::ledger reopened{tmp.dir};
            auto events = reopened.events(0, 10);
            expect((events.events.size() == 2u) >> fatal);
            expect(events.events[0].call && events.events[1].call);
            expect(events.events[0].call->actor == "alice" && events.events[1].call->actor == "bob");
            expect(events.events[1].call->worldline == "branch" && events.events[1].call->version == "v2" && events.events[1].call->reuse == "cache");
        };
    };
} // namespace
auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"toolchain">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
