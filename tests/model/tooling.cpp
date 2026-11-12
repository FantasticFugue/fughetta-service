import std;
import support;
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
        cm.toolchain_file = ::test::absolute("/opt/vcpkg/scripts/buildsystems/vcpkg.cmake");
        cm.inputs = {{"packages", "directory"}};
        cm.variables = {{"VCPKG_INSTALLED_DIR", "${input:packages}"}, {"VCPKG_MANIFEST_INSTALL", "OFF"}};
        c.builders["deps"] =
            builds::with<"vcpkg">{.options = {.root = ::test::absolute("/opt/vcpkg"), .install_dir = "build/installed", .triplet = "x64-linux"}};
        c.tasks["dependencies"] = {.builder = "deps", .operation = "install"};
        c.tasks.at("configure").inputs = {{"packages", {"dependencies", "installed"}}};
        c.outputs = {{"database", {"configure", "compilation_database"}}};
        return c;
    }
    static constexpr auto cases = [] {
        "dependency manifests contain selectors but never registry configuration or commands"_test = [] {
            auto manifest = builds::dependency_manifest(
                R"({"name":"tenant","dependencies":["fmt",{"name":"zlib","features":["core"],"default-features":false}],"features":{"tests":{"description":"tests","dependencies":["catch2"]}},"default-features":["tests"],"command":"untrusted","overrides":[{"name":"fmt","version":"10.0.0"}]})");
            const auto encoded = rfl::json::write(manifest);
            expect(encoded.contains("fmt") && encoded.contains("catch2") && !encoded.contains("untrusted") && !encoded.contains("tenant"));
            for (const auto* text : {R"({"vcpkg-configuration":{}})", R"({"dependencies":["../escape"]})",
                                     R"json({"dependencies":[{"name":"fmt","platform":"$(command)"}]})json", R"({"builtin-baseline":"not-a-hash"})"})
                expect(throws<std::invalid_argument>([&] { builds::dependency_manifest(text); }));
        };
        "dependency filling is described only for supported provider inputs"_test = [] {
            auto c = configured();
            auto& options = fixture::options<"vcpkg">(c.builders.at("deps"));
            auto plan = builds::prepare(c, {.operation = "build"});
            expect(plan.steps.front().dependencies.has_value());
            expect(plan.steps.front().dependencies->triplet == "x64-linux");
            options.overlay_ports = {"ports"};
            expect(!builds::prepare(c, {.operation = "build"}).steps.front().dependencies);
            options.overlay_ports.clear();
            options.extra = {"--dry-run"};
            expect(!builds::prepare(c, {.operation = "build"}).steps.front().dependencies);
        };
        "vcpkg installation feeds the CMake configuration on a worker"_test = [] {
            auto c = configured();
            auto& v = fixture::options<"vcpkg">(c.builders.at("deps"));
            v.features = {"tls"};
            v.default_features = false;
            v.buildtrees_dir = "build/trees";
            auto plan = builds::prepare(c, {.operation = "build"});
            expect(plan.steps.size() == 3u);
            auto install = builds::locate(plan.steps.front(), ::test::absolute("/work/project"), {{"vcpkg", ::test::absolute("/opt/tools/vcpkg")}}, 3, {},
                                          ::test::absolute("/shared/dependencies"));
            expect(install.env.at("VCPKG_BINARY_SOURCES") == ("clear;files," + ::test::absolute("/shared/dependencies/vcpkg") + ",readwrite"));
            expect(install.env.at("VCPKG_FEATURE_FLAGS") == "binarycaching");
            auto consumer =
                builds::locate(plan.steps.front(), ::test::absolute("/work/project"), {}, 1, {}, "/private/dependencies", "/published/cache,stable");
            expect(consumer.env.at("VCPKG_BINARY_SOURCES") == ("clear;files," + ::test::absolute("/published/cache`,stable/vcpkg") + ",read;files," +
                                                               ::test::absolute("/private/dependencies/vcpkg") + ",readwrite"));
            for (const auto* writable : {"/published", "/published/sub/..", "/published/vcpkg/private"})
                expect(throws<std::invalid_argument>(
                    [&] { builds::locate(plan.steps.front(), ::test::absolute("/work/project"), {}, 1, {}, writable, "/published"); }));
            expect(throws<std::invalid_argument>(
                [&] { builds::locate(plan.steps.front(), ::test::absolute("/work/project"), {}, 1, {}, "/published", "/published/vcpkg/nested"); }));
            auto escaped = builds::locate(plan.steps.front(), ::test::absolute("/work/other"), {}, 1, {}, ::test::absolute("/shared/cache,semi;tick`"));
            expect(escaped.env.at("VCPKG_BINARY_SOURCES") == ("clear;files," + ::test::absolute("/shared/cache`,semi`;tick``/vcpkg") + ",readwrite"));
            auto literal =
                builds::locate(plan.steps.front(), ::test::absolute("/work/other"), {}, 1, {}, ::test::absolute("/shared/${tree}/${jobs}/${tool:cc}"));
            expect(literal.env.at("VCPKG_BINARY_SOURCES") == ("clear;files," + ::test::absolute("/shared/${tree}/${jobs}/${tool:cc}/vcpkg") + ",readwrite"));
            auto argument = plan.steps.front();
            argument.argv.push_back("${cache:vcpkg}");
            expect(throws<std::invalid_argument>([&] { builds::locate(argument, ::test::absolute("/tree"), {}, 1, {}, ::test::absolute("/shared")); }));
            auto environment = plan.steps.front();
            environment.env["OTHER"] = "${cache:vcpkg}";
            expect(throws<std::invalid_argument>([&] { builds::locate(environment, ::test::absolute("/tree"), {}, 1, {}, ::test::absolute("/shared")); }));
            expect(throws<std::invalid_argument>([&] { builds::locate(plan.steps.front(), ::test::absolute("/work/project"), {}, 1); }));
            expect(install.argv.front() == ::test::absolute("/opt/tools/vcpkg") && install.env.at("VCPKG_MAX_CONCURRENCY") == "3");
            expect(std::ranges::contains(install.argv, ("--vcpkg-root=" + ::test::absolute("/opt/vcpkg"))));
            expect(std::ranges::contains(install.argv, ("--x-install-root=" + ::test::absolute("/work/project/build/installed"))));
            expect(std::ranges::contains(install.argv, "--x-feature=tls") && std::ranges::contains(install.argv, "--x-no-default-features"));
            expect(std::ranges::contains(plan.steps[1].argv, "-DVCPKG_INSTALLED_DIR=${tree}/build/installed"));
            expect(plan.steps.front().outputs.size() == 1u);
            v.extra = {"--triplet=other"};
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
            v.extra.clear();
            for (const auto* option : {"--binarysource=clear", "--binarysource=files,/other,readwrite", "--binarycaching", "--no-binarycaching",
                                       "--feature-flags=-binarycaching", "--editable"}) {
                v.extra = {option};
                expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
            }
            v.extra.clear();
            for (const auto* argument : {"--binarysource=clear", "@${tree}/options.rsp", "--BINARYSOURCE=clear", "--x-binarysource=clear"}) {
                c.tasks.at("dependencies").parameters.args = {argument};
                expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "build"}); }));
            }
            c.tasks.at("dependencies").parameters.args.clear();
            v.env["VCPKG_FEATURE_FLAGS"] = "-binarycaching";
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
            v.env.clear();
            v.install_dir.clear();
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
        };
        "clice initialization and executable mapping use the prepared project"_test = [] {
            language::configuration c{
                .server = language::with<"clice">{.options = {.args = {"--log-level", "off"}, .cache_dir = "build/clice", .append = {"-std=c++23"}}},
                .build_configuration = "native"};
            auto plan = language::prepare(c, configured());
            auto json = rfl::json::write(plan.server.initialization);
            expect(json.contains("build/debug/compile_commands.json") && json.contains("-std=c++23"));
            auto local = language::locate(plan.server, ::test::absolute("/work/project"), {{"clice", ::test::absolute("/opt/clice/bin/clice")}}, 2);
            expect(local.argv == std::vector<std::string>{::test::absolute("/opt/clice/bin/clice"), "serve", "--log-level", "off"});
            expect(rfl::json::write(local.initialization) == json);
            c.build_configuration.clear();
            expect(throws<std::invalid_argument>([&] { language::prepare(c, configured()); }));
        };
    };
} // namespace
auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"tooling">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
