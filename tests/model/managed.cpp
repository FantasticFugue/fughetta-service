import std;
import boost.ut;
import build;
import support;
import json;
import projectmodel;
import delivery;
import toolchain;
import records;
import helper;

namespace {
    using namespace boost::ut;
    const auto cases = [] {
        "model admission resolves project references and rejects output file duplicates"_test = [] {
            projectmodel::project_reference ref{.project = "library", .attributes = {{"target", "jvm"}}};
            projectmodel::document model{.producer = "test",
                                         .producer_version = "current",
                                         .units = {{.id = "main",
                                                    .project = "library",
                                                    .language = "kotlin",
                                                    .directory = "library",
                                                    .outputs = {"library/build/classes"},
                                                    .variants = {{{"target", "jvm"}}}},
                                                   {.id = "test", .project = "library", .language = "kotlin", .directory = "library"},
                                                   {.id = "app", .project = "app", .language = "kotlin", .directory = "app", .references = {ref}}}};
            auto read = [&] { return projectmodel::read(json::write(model)); };
            expect(read().units.size() == 3_u);
            model.units[1].variants = model.units[0].variants;
            expect(throws<std::invalid_argument>(read));
            ref.unit = "test";
            model.units[2].references = {ref};
            expect(read().units.size() == 3_u);
            ref.unit = "absent";
            model.units[2].references = {ref};
            expect(throws<std::invalid_argument>(read));
            ref.project = "skipped-android";
            model.units[2].references = {ref};
            expect(throws<std::invalid_argument>(read));
            model.units[2].references = {projectmodel::tree_reference{.path = "library/build/classes/Thing.class", .role = "classpath"}};
            expect(throws<std::invalid_argument>(read));
            model.units[2].references = {projectmodel::tree_reference{.path = "vendor/Library.jar", .role = "classpath"}};
            expect(read().units.size() == 3_u);
        };
        "managed models prepare independently of final compilation and bind worker tools"_test = [] {
            builds::configuration c;
            c.builders["gradle"] = builds::with<"gradle">{.options = {.generate_tasks = {"generateSources"},
                                                                      .generated = {{"sources", {"generated_sources", "build/generated", "directory"}}},
                                                                      .artifacts = {{"jar", {"jvm_artifact", "build/libs/app.jar"}}}}};
            c.tasks["dependencies"] = {.builder = "gradle", .operation = "prepare"};
            c.tasks["generate"] = {.builder = "gradle", .operation = "generate", .inputs = {{"dependencies", {"dependencies", "dependencies"}}}};
            c.tasks["model"] = {.builder = "gradle",
                                .operation = "model",
                                .inputs = {{"dependencies", {"dependencies", "dependencies"}}, {"sources", {"generate", "sources"}}}};
            c.tasks["compile"] = {.builder = "gradle",
                                  .operation = "compile",
                                  .inputs = {{"dependencies", {"dependencies", "dependencies"}}, {"sources", {"generate", "sources"}}}};
            c.operations["prepare"] = {.tasks = {"model"}};
            c.operations["build"] = {.tasks = {"compile"}};
            c.outputs = {{"model", {"model", "model"}}, {"jar", {"compile", "jar"}}};
            auto model = builds::prepare(c, {.operation = "prepare"});
            expect(model.tasks.size() == 3_u && model.tasks.back().name == "model");
            expect(!model.contexts.empty() >> fatal);
            expect(model.contexts.back().type == "project_model");
            expect(std::ranges::contains(model.needs.tools, "fughetta-gradle-model"));
            auto step = builds::locate(model.steps.back(), ::test::absolute("/tree"),
                                       {{"gradle", ::test::absolute("/tools/gradle")}, {"fughetta-gradle-model", ::test::absolute("/tools/model")}}, 2);
            expect(step.argv.front() == ::test::absolute("/tools/gradle"));
            expect(std::ranges::contains(step.argv, ::test::absolute("/tools/model")));
            expect(std::ranges::contains(step.argv, ::test::absolute("/tools/gradle")));
            expect(builds::prepare(c, {.operation = "build"}).contexts.back().type == "jvm_artifact");

            auto missing = c;
            missing.tasks["model"].inputs.erase("sources");
            expect(throws<std::invalid_argument>([&] { builds::validate(missing); }));
            missing = c;
            missing.tasks["model"].inputs["dependencies"] = {"generate", "sources"};
            expect(throws<std::invalid_argument>([&] { builds::validate(missing); }));
            c = {};
            c.builders["dotnet"] = builds::with<"msbuild">{.options = {.projects = {"app/App.fsproj"},
                                                                       .properties = {{"Configuration", "Debug"}, {"DefineConstants", "FIRST;SECOND"}},
                                                                       .artifacts = {{"assembly", {"clr_artifact", "app/bin/Debug/net10.0/App.dll"}}}}};
            c.tasks["restore"] = {.builder = "dotnet", .operation = "restore"};
            c.tasks["model"] = {.builder = "dotnet", .operation = "model", .inputs = {{"assets", {"restore", "assets"}}}};
            c.tasks["compile"] = {.builder = "dotnet", .operation = "compile", .inputs = {{"assets", {"restore", "assets"}}}};
            c.operations["prepare"] = {.tasks = {"model"}};
            c.outputs = {{"model", {"model", "model"}}};
            model = builds::prepare(c, {.operation = "prepare"});
            expect(model.tasks.size() == 2_u && model.contexts.back().type == "project_model");
            const auto invocations = builds::invocations(model.steps);
            expect(invocations.size() == 1_u);
            expect(invocations.front().tasks == std::vector<std::string>{"restore", "model"})
                << "typed restore and model tasks retain their identities inside one managed exporter invocation";
            expect(std::ranges::contains(model.steps.back().argv, "-property:DefineConstants=FIRST%3BSECOND"));
            expect(std::ranges::contains(model.steps.front().argv, "restore"));
            expect(!std::ranges::contains(model.steps.back().argv, "-target:Build"));
            c.tasks["model"].inputs.clear();
            expect(throws<std::invalid_argument>([&] { builds::validate(c); }));
        };
        "a model loads project code when a unit names a compiler plugin or type providers"_test = [] {
            projectmodel::document model{
                .producer = "test", .producer_version = "current", .units = {{.id = "main", .project = "app", .language = "fsharp", .directory = "."}}};
            expect(!projectmodel::loads_code(model));
            model.units[0].extensions["type_providers"] = json::value{false};
            expect(!projectmodel::loads_code(model));
            model.units[0].extensions["type_providers"] = json::value{true};
            expect(projectmodel::loads_code(model));
            model.units[0].extensions.clear();
            model.units[0].references = {projectmodel::artifact_reference{.object = {std::string(64, 'a'), "Library.dll"}, .role = "classpath"}};
            expect(!projectmodel::loads_code(model));
            for (auto name : {"kotlin-scripting-compiler-embeddable-2.3.0.jar", "kotlin-stdlib-2.3.0.jar", "kotlinx-coroutines-core-jvm-1.8.0.jar"})
                model.units[0].references.push_back(projectmodel::artifact_reference{.object = {std::string(64, 'c'), name}, .role = "plugin"});
            model.units[0].compiler_arguments = {"-Xplugin=/cache/kotlin-scripting-jvm-2.3.0.jar"};
            expect(!projectmodel::loads_code(model));
            model.units[0].compiler_arguments = {"-Xcompiler-plugin=/cache/kotlin-stdlib-2.3.0.jar,/work/tools/rewrite.jar=mode=fast"};
            expect(projectmodel::loads_code(model));
            model.units[0].compiler_arguments.clear();
            model.units[0].references.push_back(projectmodel::artifact_reference{.object = {std::string(64, 'b'), "plugin.jar"}, .role = "plugin"});
            expect(projectmodel::loads_code(model));
            model.units[0].references = {projectmodel::tree_reference{.path = "build/libs/kotlin-stdlib-local.jar", .role = "plugin"}};
            expect(projectmodel::loads_code(model));
        };
        "model ingestion checks typed references and resolves SDKs on the receiving worker"_test = [] {
            test::scratch temp{"project-model"};
            auto sdk = temp.dir / "sdk";
            test::spit(sdk / "ref/Core.dll", "installed sdk");
            auto hash = std::string(64, 'a');
            test::spit(temp.dir / "objects" / hash, "dependency");
            projectmodel::document model{
                .producer = "test",
                .producer_version = "current",
                .units = {{.id = "module",
                           .project = "app",
                           .language = "fsharp",
                           .directory = ".",
                           .sources = {"Library.fsi", "Library.fs"},
                           .references = {projectmodel::artifact_reference{.object = {hash, "Library.dll"}, .role = "classpath"},
                                          projectmodel::sdk_reference{.name = "sdk/dotnet", .path = "ref/Core.dll", .role = "classpath"}}}}};
            test::spit(temp.dir / "model.json", json::write(model));
            auto measured = builds::inspect({"model", "project_model", "model.json"}, temp.dir);
            expect(measured.units == 1_u);
            auto parsed = projectmodel::read(json::write(model));
            std::vector<toolchain::binding> bindings{{"sdk/dotnet", "sdk-object", {sdk.string()}}};
            projectmodel::installed(parsed, temp.dir / "objects", bindings);
            model.units[0].references = {projectmodel::artifact_reference{.object = {"invalid", "Library.dll"}, .role = "classpath"}};
            test::spit(temp.dir / "model.json", json::write(model));
            expect(throws<std::invalid_argument>([&] { builds::inspect({"model", "project_model", "model.json"}, temp.dir); }));
        };
    };
    template <class Params, class Result> void conforms(const json::value& params, const json::value& result) {
        (void)json::read<Params>(json::write(params)).value();
        (void)json::read<Result>(json::write(result)).value();
    }
    // Each line holds one helper exchange; both sides must read as the published wire types.
    auto conforming_transcript(const std::filesystem::path& path) -> std::size_t {
        namespace helper = language::helper;
        struct exchange {
            helper::message<json::value> message;
            helper::response<json::value> response;
        };
        std::ifstream file{path};
        std::size_t count = 0;
        for (std::string line; std::getline(file, line); ++count) {
            const auto [message, response] = json::read<exchange>(line).value();
            if (response.request_id != message.request_id || response.error || !response.result)
                throw std::runtime_error("helper exchange " + std::to_string(message.request_id) + " did not succeed");
            const auto& params = message.params;
            const auto& result = *response.result;
            if (message.operation == "open")
                conforms<helper::opening, json::value>(params, result);
            else if (message.operation == "update")
                conforms<helper::installation, json::value>(params, result);
            else if (message.operation == "units")
                conforms<helper::selection, std::vector<semantic::unit_key>>(params, result);
            else if (message.operation == "evaluate")
                conforms<helper::evaluation, helper::evaluation_result>(params, result);
            else if (message.operation == "observe")
                conforms<helper::observation, std::vector<semantic::input_object>>(params, result);
            // Graceful shutdown is a host convenience outside the worker contract.
            else if (message.operation != "close")
                throw std::runtime_error("helper transcript names unknown operation " + message.operation);
        }
        if (count == 0)
            throw std::runtime_error("helper transcript is empty");
        return count;
    }
} // namespace
auto main(int argc, const char** argv) -> int {
    // Keep argv access behind a built-in short circuit; UT overloads logical
    // operators for its comparison expressions in this translation unit.
    if (argc == 3 && std::string_view{argv[1]}.compare("--unit") == 0) {
        std::ofstream{argv[2]} << json::write(semantic::unit{.id = "unit", .context = "context"});
        return 0;
    }
    if (argc == 3 && (std::string_view{argv[1]}.compare("--shard") == 0 || std::string_view{argv[1]}.compare("--read-shard") == 0)) {
        try {
            std::ifstream file{argv[2]};
            auto shard = json::read<semantic::shard>(std::string{std::istreambuf_iterator<char>{file}, {}}).value();
            const auto actual = semantic::contributions(std::as_const(shard));
            if (actual != shard.unit.contributions || shard.symbols.empty() || (std::string_view{argv[1]}.compare("--shard") == 0 && shard.references.empty()))
                throw std::runtime_error("helper shard has inconsistent contributions");
            std::cout << "parsed " << shard.symbols.size() << " symbols and " << shard.references.size() << " references\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 3 && std::string_view{argv[1]}.compare("--helper") == 0) {
        try {
            std::cout << "validated " << conforming_transcript(argv[2]) << " helper exchanges\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 3 && std::string_view{argv[1]}.compare("--model") == 0) {
        try {
            std::ifstream file{argv[2]};
            auto model = projectmodel::read(std::string{std::istreambuf_iterator<char>{file}, {}});
            auto bindings = json::read<std::vector<toolchain::binding>>(std::getenv("FUGHETTA_SDKS")).value();
            const auto path = std::filesystem::path{argv[2]};
            const auto storage = path.parent_path() / ".admitted";
            delivery::capture_model(path.parent_path(), {"model", "project_model", path.filename().string()}, std::getenv("FUGHETTA_STAGING"), storage);
            projectmodel::installed(model, storage, bindings);
            std::cout << "validated " << model.units.size() << " model units\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    boost::ut::suite<"managed_models">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
