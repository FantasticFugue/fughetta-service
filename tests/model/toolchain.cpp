import std;
import boost.ut;
import toolchain;
import usage;
import projectmodel;
import support;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "SDK bindings reject shared directories and select the longest nested root"_test = [] {
            test::scratch tmp{"sdk-roots"};
            const auto first = tmp.dir / "first", second = tmp.dir / "second";
            test::spit(first / "release", "first SDK");
            test::spit(first / "headers/file.h", "first header");
            test::spit(second / "release", "second SDK");
            test::spit(second / "headers/file.h", "second header");
            std::map<std::string, toolchain::sdk> inputs{{"one", {(first / "release").string(), {first.string()}}}};
            inputs.at("one").roots.push_back(second.string());
            expect(throws<std::invalid_argument>([&] { toolchain::inspect({}, {}, inputs); }));
            inputs.at("one").roots.pop_back();
            inputs["two"] = {(second / "release").string(), {first.string()}};
            expect(throws<std::invalid_argument>([&] { toolchain::inspect({}, {}, inputs); }));
            inputs.at("two").roots = {(first / "headers").string()};
            const auto nested = toolchain::inspect({}, {}, inputs);
            const auto header = toolchain::locate(nested.inputs, first / "headers/file.h");
            const auto release = toolchain::locate(nested.inputs, first / "release");
            expect((header && release) >> fatal);
            expect(header->input->name == "sdk/two" && header->path == "file.h");
            expect(release->input->name == "sdk/one" && release->path == "release");
            expect(test::slurp(projectmodel::sdk_path(nested.inputs, {.name = header->input->name, .path = header->path})) == "first header");
            inputs.at("two").roots = {second.string()};
            std::error_code error;
            std::filesystem::create_directory_symlink(first, tmp.dir / "alias", error);
            if (!error) {
                inputs.at("two").roots = {(tmp.dir / "alias").string()};
                expect(throws<std::invalid_argument>([&] { toolchain::inspect({}, {}, inputs); }));
                inputs.at("two").roots = {second.string()};
                inputs.at("one").roots.push_back((tmp.dir / "alias").string());
            }
            const auto runtime = toolchain::inspect({}, {}, inputs);
            expect(test::slurp(projectmodel::sdk_path(runtime.inputs, {.name = "sdk/one", .path = "headers/file.h"})) == "first header");
            expect(test::slurp(projectmodel::sdk_path(runtime.inputs, {.name = "sdk/two", .path = "headers/file.h"})) == "second header");
            test::spit(first / "platforms/api/source.properties", "platform release");
            const auto detected = toolchain::inspect({}, {{"ANDROID_HOME", first.string()}, {"ANDROID_SDK_ROOT", first.string()}});
            expect(detected.inputs.size() == 1_u &&
                   detected.inputs.front().roots == std::vector<std::string>{std::filesystem::canonical(first).generic_string()});
            expect(throws<std::invalid_argument>([&] { toolchain::inspect({}, {{"ANDROID_HOME", first.string()}, {"ANDROID_SDK_ROOT", second.string()}}); }));
        };

        "SDK bindings change independently from the execution runtime"_test = [] {
            test::scratch tmp{"toolchain"};
            test::spit(tmp.dir / "compiler", "compiler version one");
            test::spit(tmp.dir / "sdk", "sdk version one");
            std::map<std::string, std::string> tools{{"cc", (tmp.dir / "compiler").string()}};
            std::map<std::string, toolchain::sdk> sdk{{"native", {.manifest = (tmp.dir / "sdk").string(), .roots = {tmp.dir.string()}}}};
            tools["missing"] = (tmp.dir / "disappeared").string();
            auto first = toolchain::inspect(tools, {}, sdk);
            expect(first.tools == std::vector<std::string>{"cc"} && first.id.size() == 64u);
            expect(toolchain::inspect(tools, {}, sdk).id == first.id);
            test::spit(tmp.dir / "compiler", "compiler version two");
            auto second = toolchain::inspect(tools, {}, sdk);
            expect(second.id != first.id && second.execution != first.execution);
            test::spit(tmp.dir / "sdk", "sdk version two");
            auto third = toolchain::inspect(tools, {}, sdk);
            expect(third.id != second.id && third.execution == second.execution && third.inputs.front().object != second.inputs.front().object);
            std::filesystem::create_directory(tmp.dir / "headers");
            sdk.at("native").roots = {(tmp.dir / "headers").string()};
            auto relocated = toolchain::inspect(tools, {}, sdk);
            expect(relocated.execution == third.execution && relocated.inputs.front().object == third.inputs.front().object && relocated.id != third.id);
            expect(toolchain::inspect(tools, {{"TARGET", "other"}}, sdk).execution != third.execution);
        };
        "provider execution follows selected tools and its effective environment"_test = [] {
            toolchain::identity runtime{.platform = "system",
                                        .executables = {{"/tools/reader", "reader"}, {"/tools/symbolizer", "symbols"}, {"/tools/java", "java"}}};
            const std::map<std::string, std::string> commands{{"reader", "/tools/reader"}, {"symbolizer", "/tools/symbolizer"}};
            const auto first = toolchain::select(runtime, commands, {});
            auto relocated = runtime;
            relocated.executables = {{"/another-worker/reader", "reader"}, {"/another-worker/symbolizer", "symbols"}};
            const std::map<std::string, std::string> moved{{"reader", "/another-worker/reader"}, {"symbolizer", "/another-worker/symbolizer"}};
            expect(toolchain::select(relocated, moved, {}) == first) << "logical tools retain their identity after moving worker installations";
            auto different = moved;
            auto role = different.extract("reader");
            role.key() = "different-role";
            different.insert(std::move(role));
            expect(toolchain::select(relocated, different, {}) != first);
            toolchain::identity drivers{.platform = "system", .executables = {{"/tools/clang", "one-binary"}, {"/tools/clang++", "one-binary"}}};
            expect(toolchain::select(drivers, {{"compiler", "/tools/clang"}}, {}) != toolchain::select(drivers, {{"compiler", "/tools/clang++"}}, {}))
                << "argv0 keeps multicall compiler modes distinct";
            runtime.executables.at("/tools/java") = "changed";
            expect(!first.empty() && toolchain::select(runtime, commands, {}) == first);
            runtime.executables.at("/tools/symbolizer") = "changed";
            const auto changed = toolchain::select(runtime, commands, {});
            expect(changed != first && toolchain::select(runtime, commands, {{"TARGET", "other"}}) != changed);
            runtime.executables.erase("/tools/reader");
            expect(toolchain::select(runtime, commands, {}).empty());
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
