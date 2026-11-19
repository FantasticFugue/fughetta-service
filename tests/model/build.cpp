import std;
import boost.ut;
import language;
import configuration;
import json;
import support;
import fixture;
import protocol;
import semantic;
import xml;

namespace {
    using namespace boost::ut;

    auto mixed() -> builds::configuration {
        auto c = fixture::cmake("build/cmake", "cc");
        auto& cm = fixture::options<"cmake">(c.builders.at("main"));
        cm.inputs = {{"header", "cxx_header"}};
        cm.variables["GENERATED_HEADER"] = "${input:header}";
        cm.export_compile_commands = true;
        cm.outputs["binary"] = {"executable", "build/cmake/app"};
        fixture::needs(c.builders.at("main")).platform.os = "linux";
        builds::config<"bazel"> bz{.build_dir = "build/bazel"};
        bz.operations["generate"] = {.command = "run", .targets = {"//:generate"}, .outputs = {{"header", {"cxx_header", "build/generated.h"}}}};
        bz.operations["package"] = {.command = "run",
                                    .targets = {"//:package"},
                                    .args = {"${input:binary}"},
                                    .inputs = {{"binary", "executable"}},
                                    .outputs = {{"package", {"archive", "build/package.bin"}}}};
        c.builders["bazel"] = builds::with<"bazel">{.options = std::move(bz)};
        fixture::needs(c.builders.at("bazel")).tools = {"sdk"};
        c.tasks["generate"] = {.builder = "bazel", .operation = "generate"};
        c.tasks.at("configure").inputs = {{"header", {"generate", "header"}}};
        c.tasks["package"] = {.builder = "bazel", .operation = "package", .inputs = {{"binary", {"build", "binary"}}}};
        c.operations["build"] = {.tasks = {"package"}};
        c.outputs = {{"database", {"configure", "compilation_database"}}, {"archive", {"package", "package"}}};
        return c;
    }

    static constexpr auto cases = [] {
        "worker requests route and preserve binary payloads"_test = [] {
            static_assert(std::same_as<jobs::reply_t<jobs::source_bytes>, jobs::authority_ack>);
            static_assert(std::same_as<jobs::reply_t<jobs::sync_offer>, jobs::sync_plan>);
            static_assert(std::same_as<jobs::reply_t<jobs::build_request>, jobs::build_report>);
            static_assert(jobs::code<jobs::heartbeat> == jobs::kind<"heartbeat">);
            jobs::hash_file file{.path = "dataset/file.cpp"};
            expect(jobs::locate(file) == "dataset");
            std::vector<std::byte> data{std::byte{0}, std::byte{255}};
            jobs::source_bytes push{{{.digest = content::hash_of(data), .offset = 2, .total = 4, .bytes = data}}};
            transport::frame frame{.payload = transport::encode(push)};
            auto decoded = transport::decode<jobs::source_bytes>(frame);
            expect(std::ranges::equal(decoded.parts.front().bytes, data) && decoded.parts.front().offset == 2u);
            frame.payload.pop_back();
            expect(throws<std::length_error>([&] { transport::decode<jobs::source_bytes>(frame); }));
            jobs::build_report report{.worker = "local", .tree = "work", .steps = {{.name = "build", .exit_code = 0}}};
            auto restored = transport::decode<jobs::build_report>({.payload = transport::encode(report)});
            expect(restored.ok() && restored.worker == "local");
            restored.steps.front().timed_out = true;
            expect(!restored.ok() && jobs::build_report{}.ok());
            language::request request{.config = {.build_configuration = "native",
                                                 .inputs = {{"compilation_database", {language::build_input{"native", "configure", "compilation_database"}}}}},
                                      .builds = {{"native", mixed()}}};
            auto prepared = transport::decode<language::request>({.payload = transport::encode(request)});
            expect(language::prepare(prepared.config, prepared.builds).inputs.size() == 1u);
        };

        "CMake and Bazel compose by typed inputs"_test = [] {
            auto c = mixed();
            auto plan = builds::prepare(c, {.operation = "build"});
            std::vector<std::string> order;
            for (auto& step : plan.steps)
                order.push_back(step.name);
            expect(order == std::vector<std::string>{"generate", "configure", "build", "package"});
            expect(plan.needs.platform.os == std::optional<std::string>{"linux"});
            for (auto tool : {"bazel", "cmake", "cc", "ninja", "sdk"})
                expect(std::ranges::contains(plan.needs.tools, tool));
            expect(plan.contexts.size() == 2u);
            expect(std::ranges::contains(plan.steps[1].argv, "-DGENERATED_HEADER=${tree}/build/generated.h"));
            expect(std::ranges::contains(plan.steps[3].argv, "${tree}/build/cmake/app"));
            expect(plan.steps[0].argv[1] == "--batch");
            expect(plan.steps[0].argv[2] == "--output_base=${tree}/build/bazel");
            c.operations["prepare"] = {.tasks = {"configure", "build"}};
            auto once = builds::prepare(c, {.operation = "prepare"});
            expect(once.steps.size() == 3u) << "shared dependencies expand once";
            auto prepared = language::prepare(
                {.build_configuration = "native", .inputs = {{"compilation_database", {language::build_input{"native", "configure", "compilation_database"}}}}},
                {{"native", mixed()}});
            expect(prepared.build.steps.size() == 2u && prepared.inputs[0].type == "compilation_database");
            expect(prepared.server.provider == "clice" && prepared.server.argv == std::vector<std::string>{"clice", "serve"});
            expect(json::write(prepared.server.initialization) == R"({"rules":[{"compile_commands":["build/cmake/compile_commands.json"]}]})");
            auto build = builds::locate(plan.steps[2], ::test::absolute("/tree"), {}, 3);
            expect(build.env.at("CMAKE_BUILD_PARALLEL_LEVEL") == "3") << "a step uses the jobs its runner allows";
            expect(std::ranges::contains(builds::locate(plan.steps[0], ::test::absolute("/tree"), {}, 3).argv, "--jobs=3"));
        };

        "analysis selects artifact producers and their dependencies without compiling"_test = [] {
            auto configuration = mixed();
            configuration.operations.erase("prepare");
            configuration.outputs.clear();
            auto plan = builds::prepare_inputs(configuration, {{{"configure", "compilation_database"}, "compilation_database"}});
            expect(plan.steps.size() == 2u && plan.tasks.size() == 2u);
            expect(plan.tasks.at(0).name == "generate" && plan.tasks.at(1).name == "configure");
            expect(plan.tasks.at(1).dependencies == std::vector<std::string>{"generate"});
            expect(plan.tasks.at(1).first_step == 1u && plan.tasks.at(1).step_count == 1u);
            expect(plan.contexts.size() == 1u && plan.contexts.at(0).path == "build/cmake/compile_commands.json");
            expect(std::ranges::any_of(plan.tasks.at(1).outputs, [](auto& output) { return output.type == "compilation_database"; }));
            expect(builds::prepare_inputs(configuration, {}).steps.empty());
            expect(
                throws<std::invalid_argument>([&] { builds::prepare_inputs(configuration, {{{"missing", "compilation_database"}, "compilation_database"}}); }));
            configuration.tasks["another"] = configuration.tasks.at("configure");
            expect(builds::prepare_inputs(configuration, {{{"configure", "compilation_database"}, "compilation_database"}}).tasks.size() == 2u)
                << "an unselected producer of the same type and path stays outside the plan";
            expect(throws<std::invalid_argument>([&] {
                builds::prepare_inputs(configuration, {{{"configure", "compilation_database"}, "compilation_database"},
                                                       {{"another", "compilation_database"}, "compilation_database"}});
            }));
            expect(
                throws<std::invalid_argument>([&] { builds::prepare_inputs(configuration, {{{"configure", "compilation_database"}, "python_environment"}}); }));
            configuration.builders["release"] = configuration.builders.at("main");
            fixture::options<"cmake">(configuration.builders.at("release")).build_dir = "build/release";
            configuration.tasks.at("another").builder = "release";
            auto paired = language::prepare({.build_configuration = "native",
                                             .inputs = {{"compilation_database",
                                                         {language::build_input{"native", "another", "compilation_database"},
                                                          language::build_input{"native", "configure", "compilation_database"}}}}},
                                            {{"native", configuration}});
            expect(paired.inputs.size() == 2u && paired.inputs[0].path == "build/release/compile_commands.json" &&
                   paired.inputs[1].path == "build/cmake/compile_commands.json")
                << "multiple producers retain the input order";
            expect(paired.build.tasks.size() == 3u && paired.build.tasks.front().name == "native/generate") << "shared prerequisites execute once";
        };

        "analysis consumes selected configurations with their own environments and limits"_test = [] {
            auto native = fixture::cmake("build/native");
            auto other = fixture::cmake("build/other");
            fixture::options<"cmake">(native.builders.at("main")).export_compile_commands = true;
            fixture::options<"cmake">(other.builders.at("main")).export_compile_commands = true;
            native.env = {{"PREPARE_OWNER", "native"}, {"NATIVE_ONLY", "yes"}};
            other.env = {{"PREPARE_OWNER", "other"}};
            native.limits.timeout_s = 120;
            other.limits.timeout_s = 60;
            std::map<std::string, builds::configuration> builds{{"native", native}, {"other", other}, {"unused", native}};
            language::stage stage{.build_configuration = "native",
                                  .inputs = {{"compilation_database",
                                              {language::build_input{"other", "configure", "compilation_database"},
                                               language::build_input{"native", "configure", "compilation_database"}}}},
                                  .execution = {.limits = {.timeout_s = 100}}};
            auto plan = language::prepare(stage, builds);
            expect(plan.inputs.size() == 2u && plan.inputs.front().name == "other/configure/compilation_database" &&
                   plan.inputs.back().name == "native/configure/compilation_database");
            expect(plan.build.tasks.size() == 2u && plan.build.tasks.front().name == "native/configure" && plan.build.tasks.back().name == "other/configure" &&
                   plan.build.tasks.back().first_step == 1u);
            expect(plan.build.env.empty() && plan.build.steps.front().env.at("PREPARE_OWNER") == "native" &&
                   plan.build.steps.back().env.at("PREPARE_OWNER") == "other" && !plan.build.steps.back().env.contains("NATIVE_ONLY") &&
                   plan.server.env.at("NATIVE_ONLY") == "yes");
            auto graph = language::expand({.stages = {{"main", stage}}, .outputs = {{"facts", {{"main", "facts"}}}}}, builds);
            expect(graph.nodes.back().dependencies ==
                   std::vector<std::string>{"directory/native/build/other/configure", "directory/native/build/native/configure"});
            expect(graph.nodes.back().execution->limits.timeout_s == 60u && graph.nodes.back().origins.at("limits.timeout_s") == "build/other");
            const auto identity = language::fingerprint(stage, builds);
            builds.at("unused").env["PREPARE_OWNER"] = "changed";
            expect(language::fingerprint(stage, builds) == identity && language::select_builds(stage, builds).size() == 2u);
        };

        "analysis profiles expand selected stages and shared preparation into one graph"_test = [] {
            language::stage cpp{.build_configuration = "native",
                                .inputs = {{"compilation_database", {language::build_input{"native", "configure", "compilation_database"}}}}};
            auto review = cpp;
            review.after = {"cpp"};
            language::configuration profile{
                .stages = {{"cpp", cpp}, {"review", review}, {"lint", {.server = language::with<"ruff">{}, .build_configuration = "native"}}},
                .outputs = {{"reviewed", {{"review", "facts"}}}, {"lint", {{"lint", "facts"}}}}};
            auto graph = language::expand(profile, {{"native", mixed()}}, {"reviewed"});
            expect(graph.stages == std::vector<std::string>{"cpp", "review"});
            expect(graph.nodes.size() == 4u && graph.outputs.size() == 1u);
            expect(graph.nodes[0].id == "directory/native/build/native/generate" && graph.nodes[1].id == "directory/native/build/native/configure");
            expect(graph.nodes[1].outputs.at("compilation_database") == "compilation_database");
            expect(graph.nodes.back().dependencies == std::vector<std::string>{"stage/cpp", "directory/native/build/native/configure"});
            expect(graph.nodes.back().outputs.at("facts") == "semantic_shard" && graph.nodes.back().server->provider == "clice");
            const auto saved = config::parse_as<language::configuration>(json::write(profile), "analysis");
            const auto saved_graph = language::expand(saved, {{"native", mixed()}}, {"reviewed"});
            expect(std::get<language::build_input>(saved_graph.nodes.back().inputs.at("compilation_database").front()).configuration == "native");
            auto linked = profile;
            linked.stages.at("review").inputs.at("compilation_database") = {language::reference{"cpp", "facts"}};
            linked = config::parse_as<language::configuration>(json::write(linked), "analysis");
            expect(std::get<language::reference>(linked.stages.at("review").inputs.at("compilation_database").front()).stage == "cpp");
            try {
                language::expand(linked, {{"native", mixed()}}, {"reviewed"});
                expect(false) << "semantic facts cannot supply a compilation database";
            } catch (const std::invalid_argument& error) {
                expect(std::string_view{error.what()}.contains("review/compilation_database requires compilation_database; cpp/facts produces semantic_shard"));
            }
            auto native = fixture::cmake("build/native");
            auto& native_options = fixture::options<"cmake">(native.builders.at("main"));
            native_options.export_compile_commands = true;
            native_options.outputs["app"] = {"native-artifact", "build/native/app"};
            auto artifact_profile = fixture::profile(cpp);
            artifact_profile.stages["artifact"] = {
                .server = language::with<"native-artifact">{.options = {.module = "app"}},
                .build_configuration = "native",
                .inputs = {{"artifact", {language::build_input{"native", "build", "app"}}}, {"source_facts", {language::reference{"main", "facts"}}}}};
            artifact_profile.outputs = {{"native", {{"artifact", "facts"}}}};
            const auto artifact_graph = language::expand(artifact_profile, {{"native", native}}, {"native"});
            expect(artifact_graph.stages == std::vector<std::string>{"main", "artifact"});
            expect(std::ranges::contains(artifact_graph.nodes.back().dependencies, "stage/main"));
            expect(std::ranges::contains(artifact_graph.directory_groups.at("native").tools, "fughetta-llvm"));
            auto cross_project = artifact_profile;
            cross_project.stages.at("artifact").inputs.at("source_facts") = {language::project_input{"library", "release", "cpp", "source", std::nullopt}};
            const auto restored_cross = config::parse_as<language::configuration>(json::write(cross_project), "analysis");
            const auto foreign_graph = language::expand(restored_cross, {{"native", native}}, {"native"});
            expect(foreign_graph.stages == std::vector<std::string>{"artifact"});
            const auto& foreign = std::get<language::project_input>(foreign_graph.nodes.back().inputs.at("source_facts").front());
            expect(foreign.project == "library" && foreign.worldline == "release" && foreign.configuration == "cpp" && foreign.output == "source");
            cross_project.stages.at("main").inputs.at("compilation_database") = {foreign};
            expect(throws<std::invalid_argument>([&] { language::expand(cross_project, {{"native", native}}, {"native"}); }))
                << "semantic project output cannot fill a compilation database input";
            language::defaults project{
                .fact_limit = 25, .relation_depth = 3, .execution = {.limits = {.timeout_s = 90}, .budget = {.units = 8}, .directory_group = "project"}};
            profile.defaults = {.fact_limit = 4, .execution = {.budget = {.units = 4}, .directory_group = "shared"}};
            profile.stages.at("review").execution = {
                .needs = builds::tools({"clang"}), .limits = {.timeout_s = 50}, .budget = {.units = 2}, .directory_group = "review"};
            auto build = mixed();
            build.limits.timeout_s = 60;
            auto expanded = language::expand(profile, {{"native", build}}, {"reviewed"}, project);
            const auto& execution = *expanded.nodes.back().execution;
            expect(expanded.defaults.fact_limit == 4u && expanded.defaults.relation_depth == 3u);
            expect(expanded.nodes.size() == 6u && expanded.directory_groups.size() == 2u);
            expect(expanded.nodes[2].execution->directory_group == "shared" && execution.directory_group == "review");
            expect(expanded.nodes[2].origins.at("directory_group") == "profile" && expanded.nodes.back().origins.at("directory_group") == "stage");
            expect(std::ranges::contains(expanded.directory_groups.at("review").tools, "clang"));
            expect(expanded.nodes[2].execution->limits.timeout_s == 60u && execution.limits.timeout_s == 50u);
            expect(execution.budget.units == 2u && std::ranges::contains(execution.needs.get().tools, "clang"));
            expect(expanded.origins.at("fact_limit") == "profile" && expanded.origins.at("relation_depth") == "project");
            expect(expanded.nodes[2].origins.at("limits.timeout_s") == "build/native" && expanded.nodes.back().origins.at("budget.units") == "stage");
            expect(semantic::tighten(execution.budget, {.units = 20}).units == 2u && semantic::tighten(execution.budget, {.units = 1}).units == 1u);
            auto controlled = language::resolve(cpp, project);
            expect(language::fingerprint(controlled, {{"native", build}}) == language::fingerprint(cpp, {{"native", mixed()}}));
            auto merged = profile;
            merged.stages.at("review").execution.directory_group = "shared";
            merged.stages.at("review").build_configuration = "other";
            std::get<language::build_input>(merged.stages.at("review").inputs.at("compilation_database").front()).configuration = "other";
            expect(throws<std::invalid_argument>([&] { language::expand(merged, {{"native", build}, {"other", build}}, {"reviewed"}, project); }))
                << "two configurations cannot overwrite each other's declared outputs in a shared directory";
            merged.stages.at("review").build_configuration = "native";
            std::get<language::build_input>(merged.stages.at("review").inputs.at("compilation_database").front()).configuration = "native";
            auto group_build = build;
            fixture::needs(group_build.builders.at("main")).platform = {};
            merged.stages.at("cpp").execution.needs.get().platform.os = "linux";
            merged.stages.at("review").execution.needs.get().platform.os = "windows";
            expect(throws<std::invalid_argument>([&] { language::expand(merged, {{"native", group_build}}, {"reviewed"}, project); }))
                << "a directory group needs one worker platform";
            profile.stages.at("cpp").after = {"review"};
            expect(throws<std::invalid_argument>([&] { language::expand(profile, {{"native", mixed()}}); }));
        };

        "graph and type failures are rejected before any execution"_test = [] {
            auto baseline = mixed();
            auto reject = [&](auto mutate) {
                auto c = baseline;
                mutate(c);
                expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "build"}); }));
            };
            reject([](auto& c) { c.tasks.at("configure").inputs.clear(); });
            reject([](auto& c) { c.tasks.at("configure").inputs.at("header").output = "missing"; });
            reject([](auto& c) { c.tasks.at("configure").inputs.at("header") = {"build", "binary"}; });
            reject([](auto& c) { c.tasks.at("generate").after = {"package"}; });
            reject([](auto& c) { c.tasks.at("package").builder = "missing"; });
            reject([](auto& c) { c.tasks.at("package").operation = "missing"; });
            reject([](auto& c) { fixture::needs(c.builders.at("bazel")).platform.os = "windows"; });
            reject([](auto& c) { c.outputs.at("archive").output = "missing"; });
            expect(throws<std::invalid_argument>([&] { builds::prepare(baseline, {.operation = "prepare", .parameters = {.targets = {"app"}}}); }));
            expect(throws<std::invalid_argument>([&] { builds::prepare(baseline, {.operation = "build", .parameters = {.labels = "unit"}}); }));
            expect(throws<std::invalid_argument>([&] {
                language::prepare({.build_configuration = "native",
                                   .inputs = {{"compilation_database", {language::build_input{"native", "configure", "compilation_database"}}}}},
                                  {{"native", {}}});
            })) << "clice requires an artifact producer";
            expect(throws<std::invalid_argument>([&] { language::prepare({}, {{"native", baseline}}); })) << "analysis requires a build reference";
            expect(!json::read<builds::builder>(R"({"adapter":"missing","options":{}})")) << "an unknown adapter is refused when parsed";
            expect(!json::read<language::provider>(R"({"provider":"clangd"})")) << "so is an unknown language provider";
        };

        "independent configurations preserve argv boundaries and local output identities"_test = [] {
            test::scratch tmp{"build-model"};
            auto c = mixed();
            c.env = {{"ROOT_ENV", "worker"}};
            fixture::env(c.builders.at("main")) = {{"NODE_ENV", "source"}};
            auto plan = builds::prepare(c, {.operation = "prepare"});
            auto local = builds::locate(plan.steps[1], tmp.dir / "tree with spaces", {{"cc", ::test::absolute("/opt/toolchain/bin/cc")}}, 1);
            expect(local.argv[0] == "cmake") << "an unmapped tool is found on PATH";
            expect(std::ranges::contains(local.argv, ("-DCMAKE_C_COMPILER=" + ::test::absolute("/opt/toolchain/bin/cc"))))
                << "a mapped tool is this machine's executable";
            expect(std::ranges::contains(local.argv, "-DGENERATED_HEADER=" +
                                                         (std::filesystem::absolute(tmp.dir / "tree with spaces") / "build/generated.h").generic_string()));
            expect(local.env.at("NODE_ENV") == "source" && plan.env.at("ROOT_ENV") == "worker");
            auto db = tmp.dir / "build/commands.json";
            test::spit(db, R"([{"directory":"/workspace","file":"main.c","arguments":["cc","-c","main.c"]}])");
            auto result = builds::inspect({"db", "compilation_database", "build/commands.json"}, tmp.dir);
            expect(result.units == 1u && !result.digest.empty());
            test::spit(db, R"([{"directory":"/workspace","file":"main.c","arguments":[]}])");
            expect(throws<std::runtime_error>([&] { builds::inspect({"db", "compilation_database", "build/commands.json"}, tmp.dir); }));
            test::spit(tmp.dir / "build/generated.h", "first");
            auto first = builds::inspect({"header", "cxx_header", "build/generated.h"}, tmp.dir);
            test::spit(tmp.dir / "build/generated.h", "second");
            expect(builds::inspect({"header", "cxx_header", "build/generated.h"}, tmp.dir).digest != first.digest);
            expect(builds::inspect({"sdk", "acme.sdk", "build", "directory"}, tmp.dir).kind == "directory");
        };

        "request parameters go to the operation's receiver only"_test = [] {
            auto c = mixed();
            c.operations["both"] = {.tasks = {"build", "generate"}};
            builds::validate(c);
            expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "both", .parameters = {.targets = {"app"}}}); }))
                << "several final tasks and no receiver";
            c.operations["both"].receiver = "build";
            auto plan = builds::prepare(c, {.operation = "both", .parameters = {.targets = {"app"}}});
            auto step = [&](std::string_view name) { return *std::ranges::find(plan.steps, name, &builds::step::name); };
            expect(std::ranges::contains(step("build").argv, "app"));
            expect(!std::ranges::contains(step("generate").argv, "app") && std::ranges::contains(step("generate").argv, "//:generate"));
            c.operations["both"].receiver = "missing";
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
        };

        "configurations name tools; each machine maps them"_test = [] {
            auto cmake = fixture::cmake("build/x", ::test::absolute("/usr/bin/cc"));
            expect(throws<std::invalid_argument>([&] { builds::validate(cmake); })) << "a host path";
            auto script = fixture::commands({"scripts/gen.sh", "--fast"});
            auto plan = builds::prepare(script, {.operation = "build"});
            expect(plan.needs.tools.empty()) << "a project file is no tool";
            expect(builds::locate(plan.steps[0], ::test::absolute("/tree"), {}, 1).argv[0] == ::test::absolute("/tree/scripts/gen.sh"));
            auto python = fixture::commands({"python", "-c", "pass"});
            plan = builds::prepare(python, {.operation = "build"});
            expect(plan.needs.tools == std::vector<std::string>{"python"} && plan.steps[0].argv[0] == "${tool:python}");
            expect(builds::locate(plan.steps[0], ::test::absolute("/tree"), {{"python", ::test::absolute("/opt/py/bin/python3")}}, 1).argv[0] ==
                   ::test::absolute("/opt/py/bin/python3"));
            expect(throws<std::invalid_argument>([] { builds::validate(fixture::commands({::test::absolute("/usr/bin/python3")})); }));
        };

        "an artifact format brings its own rules"_test = [] {
            builds::config<"command"> wrong{
                .operations = {{"build", {.argv = {"make"}, .outputs = {{"db", {"compilation_database", "build/db", "directory"}}}}}}};
            builds::configuration c;
            c.builders["main"] = builds::with<"command">{.options = std::move(wrong)};
            c.tasks["build"] = {.builder = "main", .operation = "build"};
            c.operations["build"] = {.tasks = {"build"}};
            expect(throws<std::invalid_argument>([&] { builds::validate(c); })) << "a database is a file";
        };

        "CMake presets and CTest selection become executable plans"_test = [] {
            auto c = fixture::cmake("build/cmake", "cc");
            auto& o = fixture::options<"cmake">(c.builders.at("main"));
            o.preset = "debug";
            o.toolchain_file = ::test::absolute("/opt/vcpkg/scripts/buildsystems/vcpkg.cmake");
            o.configuration = "Release";
            o.test.include = "^unit";
            o.test.labels = "fast";
            o.test.exclude_labels = "network";
            o.test.timeout = 30;
            auto p = builds::prepare(c, {.operation = "test", .parameters = {.filter = "^gateway", .parallel = 3}});
            auto& configure = p.steps.front().argv;
            expect(std::ranges::contains(configure, "--preset"));
            expect(std::ranges::contains(configure, ("-DCMAKE_TOOLCHAIN_FILE=" + ::test::absolute("/opt/vcpkg/scripts/buildsystems/vcpkg.cmake"))));
            expect(!std::ranges::contains(configure, "-G"));
            auto local = builds::locate(p.steps.back(), ::test::absolute("/work/project"), {{"ctest", ::test::absolute("/opt/cmake/bin/ctest")}}, 2,
                                        ::test::absolute("/work/reports"));
            auto after = [&](std::string_view flag) {
                auto at = std::ranges::find(local.argv, flag);
                return at == local.argv.end() || ++at == local.argv.end() ? std::string{} : *at;
            };
            expect(local.argv.front() == ::test::absolute("/opt/cmake/bin/ctest"));
            expect(after("--test-dir") == ::test::absolute("/work/project/build/cmake") && after("-R") == "^gateway");
            expect(after("-L") == "fast" && after("-LE") == "network" && after("-j") == "3");
            expect(after("--timeout") == "30" && after("-C") == "Release");
            expect(after("--output-junit") == ::test::absolute("/work/reports/ctest.xml") &&
                   local.reports == std::vector<builds::report>{{"junit", "ctest.xml"}});
            expect(std::ranges::contains(local.argv, "--no-tests=error"));
            expect(local.env.at("CTEST_PARALLEL_LEVEL") == "2");
        };
        "extra CMake and CTest options cannot replace structured settings"_test = [] {
            for (auto option : {"-Belsewhere", "-Selsewhere", "-GMakefiles", "--preset=other", "-DCMAKE_C_COMPILER=other", "-DCMAKE_BUILD_TYPE:STRING=Release",
                                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"}) {
                auto c = fixture::cmake("build/cmake");
                fixture::options<"cmake">(c.builders.at("main")).configure_options = {option};
                expect(throws<std::invalid_argument>([&] { builds::validate(c); })) << option;
            }
            for (auto option : {"--no-tests=ignore", "-j4", "--tests-regex=skip", "--test-dir=elsewhere", "-N", "--output-junit=other.xml"}) {
                auto c = fixture::cmake("build/cmake");
                expect(throws<std::invalid_argument>([&] { builds::prepare(c, {.operation = "test", .parameters = {.args = {option}}}); })) << option;
            }
            auto c = fixture::cmake("build/cmake");
            auto& o = fixture::options<"cmake">(c.builders.at("main"));
            o.test.no_tests_action = "ignore";
            o.build_options = {"--", "-k", "0"};
            builds::validate(c);
            expect(std::ranges::contains(builds::prepare(c, {.operation = "test"}).steps.back().argv, "--no-tests=ignore"));
            o.test.parallel = 0;
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
        };
        "CTest and Bazel tests report in JUnit and the Build Event Protocol"_test = [] {
            struct testcase {
                xml::attribute<std::string, "name"> name;
                xml::attribute<std::string, "classname"> classname;
                xml::element<std::optional<std::string>, "failure"> failure;
            };
            struct testsuite {
                xml::attribute<std::string, "name"> name;
                xml::elements<testcase, "testcase"> cases;
            };
            const testsuite typed{.name = {"typed"},
                                  .cases = {{testcase{.name = {"x<y"}, .classname = {"a&b"}, .failure = {std::string{"failure < &"}}},
                                             testcase{.name = {"passes"}, .classname = {"m"}, .failure = {}}}}};
            const auto xml_text = xml::write<"testsuite">(typed);
            const auto parsed = xml::read<testsuite, "testsuite">(xml_text);
            expect(parsed.cases.value.size() == 2u && parsed.cases.value[0].failure.value == "failure < &" &&
                   !parsed.cases.value[1].failure.value);
            auto typed_report = builds::report_protocols::read("junit", xml_text);
            expect(typed_report.tests && typed_report.tests->total == 2 && typed_report.tests->failed_tests == std::vector<std::string>{"a&b.x<y"});

            auto c = fixture::cmake("build/cmake");
            auto p = builds::prepare(c, {.operation = "test"});
            expect(p.steps.front().reports.empty() && p.steps.back().reports.size() == 1u) << "only the test step writes a report";

            auto ctest = builds::report_protocols::read("junit", R"(<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="Linux-c++" tests="3" failures="1" disabled="0" skipped="1" hostname="" time="0" timestamp="2026-10-27T10:00:00">
	<testcase name="unit" classname="unit" time="0.01" status="run"><system-out>ok</system-out></testcase>
	<testcase name="gateway" classname="gateway" time="0.2" status="fail"><failure message="Failed"/><system-out>assert</system-out></testcase>
	<testcase name="network" classname="network" time="0" status="notrun"><skipped message="Disabled"/></testcase>
</testsuite>)");
            expect((ctest.tests.has_value()) >> fatal);
            expect(ctest.tests->total == 3 && ctest.tests->failed == 1 && ctest.tests->failed_tests == std::vector<std::string>{"gateway"});

            auto bazel = builds::report_protocols::read(
                "bep", R"({"id":{"started":{}},"started":{"uuid":"1"}})"
                       "\n"
                       R"({"id":{"testSummary":{"label":"//pkg:unit","configuration":{"id":"k8"}}},"testSummary":{"overallStatus":"PASSED","totalRunCount":1}})"
                       "\n"
                       R"({"id":{"testSummary":{"label":"//pkg:slow","configuration":{"id":"k8"}}},"testSummary":{"overallStatus":"TIMEOUT"}})"
                       "\n"
                       R"({"id":{"testSummary":{"label":"//pkg:flaky","configuration":{"id":"k8"}}},"testSummary":{"overallStatus":"FLAKY"}})"
                       "\n"
                       R"({"id":{"testSummary":{"label":"//pkg:broken","configuration":{"id":"k8"}}},"testSummary":{"totalRunCount":0}})"
                       "\n");
            expect((bazel.tests.has_value()) >> fatal);
            expect(bazel.tests->total == 4 && bazel.tests->failed == 2);
            expect(bazel.tests->failed_tests == std::vector<std::string>{"//pkg:slow", "//pkg:broken"});
            auto suites = builds::report_protocols::read("junit", R"(<testsuites><testsuite name="unit"><testcase name="t" classname="m"/></testsuite>
<testsuite name="integration"><testcase name="t" classname="m"><failure/></testcase></testsuite></testsuites>)");
            expect(suites.tests->total == 2 && suites.tests->failed_tests == std::vector<std::string>{"m.t"}) << "cases of two suites stay apart";
            expect(throws([] { builds::report_protocols::read("junit", "<testsuite"); }));
        };
        "a worker reads the reports a step left, relative to the tree"_test = [] {
            test::scratch tmp{"build-reports"};
            auto c = fixture::cmake("build/cmake");
            auto& o = fixture::options<"cmake">(c.builders.at("main"));
            o.source_dir = "app";
            auto p = builds::prepare(c, {.operation = "test"});
            auto step = p.steps.back();
            step.reports.push_back({"codequality", "lint.json"});
            auto local = builds::locate(step, tmp.dir / "tree", {}, 1, tmp.dir / "reports");
            test::spit(tmp.dir / "reports/ctest.xml", R"(<testsuite><testcase name="a" classname="a"><error message="crash"/></testcase></testsuite>)");
            test::spit(tmp.dir / "reports/lint.json",
                       R"([{"description":"F401: unused","check_name":"F401","severity":"major",)"
                       R"("location":{"path":"src/m.py","positions":{"begin":{"line":1,"column":8}}}},)"
                       R"({"description":"outside","severity":"minor","location":{"path":"../../elsewhere.py","lines":{"begin":3}}}])");
            auto found = builds::collect(local, tmp.dir / "reports", tmp.dir / "tree");
            expect((found.tests && found.diagnostics.size() == 2u) >> fatal);
            expect(found.tests->failed_tests == std::vector<std::string>{"a"});
            expect(found.diagnostics[0].file == "app/src/m.py" && found.diagnostics[0].column == 8 && found.diagnostics[0].severity == "error");
            expect(found.diagnostics[1].file == "../../elsewhere.py" && found.diagnostics[1].line == 3 && found.diagnostics[1].severity == "warning");
            test::spit(tmp.dir / "reports/ctest.xml", "not xml");
            std::filesystem::remove(tmp.dir / "reports/lint.json");
            found = builds::collect(local, tmp.dir / "reports", tmp.dir / "tree");
            expect((found.diagnostics.size() == 1u && !found.tests) >> fatal) << "a missing report adds no result";
            expect(found.diagnostics[0].message.starts_with("the junit report ctest.xml could not be read"));
        };
        "a step's report is a plain file name of a known protocol"_test = [] {
            for (auto r : std::vector<builds::report>{{"junit", "../x.xml"}, {"junit", "a/b.xml"}, {"junit", ""}, {"tap", "t.tap"}, {"junit", "ctest.xml"}}) {
                auto c = fixture::cmake("build/cmake");
                c.tasks.at("test").reports = {r};
                expect(throws<std::invalid_argument>([&] { builds::validate(c); })) << r.file;
            }
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"build_model">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
