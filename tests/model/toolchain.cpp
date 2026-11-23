import std;
import boost.ut;
import build;
import toolchain;
import usage;
import projectmodel;
import support;

namespace {
    using namespace boost::ut;
    static constexpr auto cases = [] {
        "a Worker resolves all constraints for each installed instance"_test = [] {
            using toolchain::parse_version;
            using toolchain::version_constraint;
            expect(parse_version("1.0.0-rc.2") < parse_version("1.0.0-rc.10"));
            expect(parse_version("1.0.0-rc.10") < parse_version("1.0.0+build.1"));
            expect(version_constraint::parse("^0.2.3").matches(parse_version("0.2.9")) &&
                   !version_constraint::parse("^0.2.3").matches(parse_version("0.3.0")));
            expect(version_constraint::parse("~2.13").matches(parse_version("2.13.16")) &&
                   !version_constraint::parse("~2.13").matches(parse_version("2.14.0")));
            expect(throws<std::invalid_argument>([] { version_constraint::parse(">=17,"); }));

            toolchain::catalog available{.tools = {toolchain::installation("java@17.0.12"), toolchain::installation("java@21.0.4"),
                                                    {.name = "fughetta-scala", .id = "fughetta-scala", .features = {"scala-3"}}},
                                        .sdks = {toolchain::installation("jdk@17.0.12"), toolchain::installation("jdk@21.0.4")}};
            const std::vector<std::string> tools{"java>=17,<22", "java 21.x", "fughetta-scala[scala-3]"};
            const std::vector<toolchain::sdk_constraint> sdks{{"jdk", "^21"}};
            const auto result = toolchain::resolve(available, toolchain::parse_constraints(tools, sdks));
            expect(result.missing.empty());
            expect(result.selected.tools.at("java") == "java@21.0.4" && result.selected.sdks.at("jdk") == "jdk@21.0.4");
            const auto constraints = toolchain::parse_constraints(std::vector<std::string>{"java>=22"}, sdks);
            const auto missing = toolchain::resolve(available, constraints, true);
            expect(missing.missing.size() == 1_u && missing.missing.front().requirement == "tool java>=22" &&
                   missing.missing.front().installed == std::vector<std::string>{"17.0.12", "21.0.4"});
        };
        "dispatch binds each requested JDK with its tool and environment"_test = [] {
            test::scratch tmp{"jdk-bindings"};
            const auto java17 = tmp.dir / "jdk17", java21 = tmp.dir / "jdk21";
            test::spit(java17 / "bin/java", "java 17");
            test::spit(java17 / "release", "JAVA_VERSION=\"17.0.12\"\n");
            test::spit(java21 / "bin/java", "java 21");
            test::spit(java21 / "release", "JAVA_VERSION=\"21.0.4\"\n");
            const std::map<std::string, std::string> configured{{"java@17", (java17 / "bin/java").string()}, {"java@21", (java21 / "bin/java").string()}};
            auto discovered = configured;
            discovered["java"] = discovered["java@17.0.12"] = configured.at("java@17");
            const auto runtime = toolchain::inspect(discovered, {}, {{"jdk", {.manifest = (java17 / "release").string(), .roots = {java17.string()}}}},
                                                     configured);
            const auto available = toolchain::capabilities(runtime);
            expect(available.tools.size() == 2_u);
            const auto preferred = toolchain::resolve(available, toolchain::parse_constraints(std::vector<std::string>{"java"}, {}));
            expect(preferred.selected.tools.at("java") == "java@17");
            expect(available.sdks.size() == 2_u);
            expect(available.sdks.front().id == "jdk" && available.sdks.front().version == "17.0.12");
            for (const auto& [tool, sdk, root] : {std::tuple{"java>=17,<18", "^17", java17}, std::tuple{"java>=21", "^21", java21}}) {
                const auto constraints = toolchain::parse_constraints(std::vector<std::string>{tool}, std::vector<toolchain::sdk_constraint>{{"jdk", sdk}});
                const auto resolution = toolchain::resolve(available, constraints);
                expect(resolution.missing.empty() >> fatal);
                const auto bound = toolchain::bind(runtime, resolution.selected);
                const auto command = builds::locate({.argv = {"${tool:java}", "${sdk:jdk}"}}, tmp.dir, bound.tools, 1, {}, {}, {}, bound.sdks);
                expect(command.argv == std::vector<std::string>{(root / "bin/java").generic_string(), std::filesystem::canonical(root).generic_string()});
                expect(bound.env.at("JAVA_HOME") == std::filesystem::canonical(root).generic_string());
            }
        };
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

        "installation identity covers tools and SDK bindings"_test = [] {
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
            expect(second.id != first.id && !std::ranges::equal(second.executables, first.executables));
            test::spit(tmp.dir / "sdk", "sdk version two");
            auto third = toolchain::inspect(tools, {}, sdk);
            expect(third.id != second.id && std::ranges::equal(third.executables, second.executables) && third.inputs.front().object != second.inputs.front().object);
            std::filesystem::create_directory(tmp.dir / "headers");
            sdk.at("native").roots = {(tmp.dir / "headers").string()};
            auto relocated = toolchain::inspect(tools, {}, sdk);
            expect(std::ranges::equal(relocated.executables, third.executables) && relocated.inputs.front().object == third.inputs.front().object && relocated.id != third.id);
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
