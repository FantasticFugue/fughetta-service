import std;
import boost.ut;
import language;
import reflection;
import fixture;

namespace {
    using namespace boost::ut;
    auto configured() -> builds::configuration {
        auto c = fixture::cmake("build/debug", "cc");
        auto& cm = fixture::options<"cmake">(c.builders.at("main"));
        cm.export_compile_commands = true;
        cm.toolchain_file = "/opt/vcpkg/scripts/buildsystems/vcpkg.cmake";
        cm.inputs = {{"packages", "directory"}};
        cm.variables = {{"VCPKG_INSTALLED_DIR", "${input:packages}"}, {"VCPKG_MANIFEST_INSTALL", "OFF"}};
        c.builders["deps"] = builds::with<"vcpkg">{.options = {.root = "/opt/vcpkg", .install_dir = "build/installed", .triplet = "x64-linux"}};
        c.tasks["dependencies"] = {.builder = "deps", .operation = "install"};
        c.tasks.at("configure").inputs = {{"packages", {"dependencies", "installed"}}};
        c.outputs = {{"database", {"configure", "compilation_database"}}};
        return c;
    }
    static constexpr auto cases = [] {
        "vcpkg installation feeds the CMake configuration on a worker"_test = [] {
            auto c = configured();
            auto& v = fixture::options<"vcpkg">(c.builders.at("deps"));
            v.features = {"tls"};
            v.default_features = false;
            v.buildtrees_dir = "build/trees";
            auto plan = builds::prepare(c, {.operation = "build"});
            expect(plan.steps.size() == 3u);
            auto install = builds::locate(plan.steps.front(), "/work/project", {{"vcpkg", "/opt/tools/vcpkg"}}, 3);
            expect(install.argv.front() == "/opt/tools/vcpkg" && install.env.at("VCPKG_MAX_CONCURRENCY") == "3");
            expect(std::ranges::contains(install.argv, "--vcpkg-root=/opt/vcpkg"));
            expect(std::ranges::contains(install.argv, "--x-install-root=/work/project/build/installed"));
            expect(std::ranges::contains(install.argv, "--x-feature=tls") && std::ranges::contains(install.argv, "--x-no-default-features"));
            expect(std::ranges::contains(plan.steps[1].argv, "-DVCPKG_INSTALLED_DIR=${tree}/build/installed"));
            expect(plan.steps.front().outputs.size() == 1u);
            v.extra = {"--triplet=other"};
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
            v.extra.clear();
            v.install_dir.clear();
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
        };
        "clice initialization and executable mapping use the prepared project"_test = [] {
            language::configuration c{
                .server = language::with<"clice">{.options = {.args = {"--log-level", "off"}, .cache_dir = "build/clice", .append = {"-std=c++23"}}},
                .build = configured(),
                .operation = "prepare",
                .inputs = {"database"}};
            auto plan = language::prepare(c);
            auto json = rfl::json::write(plan.server.initialization);
            expect(json.contains("build/debug/compile_commands.json") && json.contains("-std=c++23"));
            auto local = language::locate(plan.server, "/work/project", {{"clice", "/opt/clice/bin/clice"}}, 2);
            expect(local.argv == std::vector<std::string>{"/opt/clice/bin/clice", "serve", "--log-level", "off"});
            expect(rfl::json::write(local.initialization) == json);
            c.inputs.clear();
            expect(throws<std::invalid_argument>([&] { language::prepare(c); }));
        };
    };
} // namespace
auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"tooling">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
