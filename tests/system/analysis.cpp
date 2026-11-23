import launch;
import boost.ut;
import client;
import protocol;
import language;
import fixture;
import content;
import nodes;
import semantic;
import knowledge;
import tools;
import host;

namespace {
    using namespace boost::ut;
    auto start_source(const std::filesystem::path& root, std::uint16_t port) {
        std::vector<std::string> args{"--port", std::to_string(port), "--storage", (root / "source").string()};
        auto process = std::make_unique<test::child>(test::source_command(SERVICE_EXECUTABLE, root, std::move(args)));
        test::eventually([&] {
            test::require(!process->finished(), process->output());
            return process->output().contains("source: internal file service");
        });
        test::ready(*process, port);
        return process;
    }

    void managed_flow(bool kotlin) {
        test::scratch tmp{kotlin ? "gateway-kotlin-jni" : "gateway-fsharp-pinvoke"};
        const auto source_port = test::port(), port = test::port();
        auto source = start_source(tmp.dir, source_port);
        auto doc = test::document(tmp.dir, port, source_port);
        auto& settings = doc.projects.at("app");
        settings.build_configurations.clear();
        settings.analysis.clear();
        const auto compiled = tmp.dir / "compiled";
        const auto configured = tmp.dir / "configured";
        const auto gradle_processes = tmp.dir / "gradle-processes";
        const auto ran = tmp.dir / "ran";
        builds::configuration build;
        const std::string managed_artifact = kotlin ? "build/libs/managed-e2e.jar" : "bin/Debug/net10.0/Library.dll";
        if (kotlin) {
            build.builders["managed"] = builds::with<"gradle">{
                .options = {.model_dir = "build/model", .compile_tasks = {"jar"}, .test_tasks = {"run"},
                            .artifacts = {{"binary", {"jvm_artifact", managed_artifact}}},
                            .model_source_dirs = {{"src/main/kotlin", "library/src/main/kotlin"}}}};
            build.tasks["prepare"] = {.builder = "managed", .operation = "prepare"};
            build.tasks["model"] = {.builder = "managed", .operation = "model", .inputs = {{"dependencies", {"prepare", "dependencies"}}}};
            build.tasks["compile"] = {.builder = "managed", .operation = "compile", .inputs = {{"dependencies", {"prepare", "dependencies"}}}};
        } else {
            build.builders["managed"] = builds::with<"msbuild">{
                .options = {.model_dir = "build/model",
                            .projects = {"Library.fsproj"},
                            .artifacts = {{"binary", {"clr_artifact", managed_artifact}}, {"symbols", {"portable_pdb", "bin/Debug/net10.0/Library.pdb"}}}}};
            build.tasks["prepare"] = {.builder = "managed", .operation = "restore"};
            build.tasks["model"] = {.builder = "managed", .operation = "model", .inputs = {{"assets", {"prepare", "assets"}}}};
            build.tasks["compile"] = {.builder = "managed", .operation = "compile", .inputs = {{"assets", {"prepare", "assets"}}}};
        }
        builds::config<"command"> commands;
        commands.operations["native"] = {.argv = {"cxx", "-shared", "-fPIC", "-g", "native.cpp", "-o", "libnative.so"},
                                         .outputs = {{"binary", {"native-artifact", "libnative.so"}}}};
        if (!kotlin)
            commands.operations["test"] = {.argv = {"dotnet", "fsi", "--exec", "check.fsx"}, .env = {{"LD_LIBRARY_PATH", "${tree}"}}};
        build.builders["commands"] = builds::with<"command">{.options = commands};
        build.tasks["native"] = {.builder = "commands", .operation = "native"};
        build.tasks["test"] = {.builder = kotlin ? "managed" : "commands", .operation = "test", .after = {"compile", "native"}};
        build.operations = {{"build", {.tasks = {"compile", "native"}, .receiver = "compile"}}, {"test", {.tasks = {"test"}}}};
        settings.build_configurations["managed"] = build;
        settings.default_configurations = {"managed"};
        language::configuration profile;
        profile.stages["source"] = {.server = kotlin ? language::provider{language::with<"kotlin">{}} : language::provider{language::with<"fsharp">{}},
                                    .build_configuration = "managed",
                                    .inputs = {{"project_model", {language::build_input{"managed", "model", "model"}}}}};
        if (kotlin) {
            auto registration = language::describe_provider(profile.stages.at("source").server);
            registration.id = "configured-kotlin";
            profile.stages["source"].server =
                language::with<"external">{.options = {.registration = std::move(registration),
                                                       .documents = {{"kt", "kotlin"}, {"java", "java"}},
                                                       .argv = {"java", "-jar", "${tool:fughetta-jvm}", "--adapter", "${tool:fughetta-kotlin}"},
                                                       .tools = {"fughetta-jvm", "fughetta-kotlin"}}};
        }
        profile.stages["managed"] = {
            .server = kotlin ? language::provider{language::with<"jvm-artifact">{.options = {.module = "managed"}}}
                             : language::provider{language::with<"clr-artifact">{.options = {.module = "managed"}}},
            .build_configuration = "managed",
            .inputs = {{"artifact", {language::build_input{"managed", "compile", "binary"}}}, {"source_facts", {language::reference{"source", "facts"}}}}};
        if (!kotlin)
            profile.stages["managed"].inputs["symbols"] = {language::build_input{"managed", "compile", "symbols"}};
        profile.stages["binding"] = {.server = kotlin ? language::provider{language::with<"jni">{.options = {{{"managed", {"native"}}}}}}
                                                      : language::provider{language::with<"pinvoke">{.options = {{{"native", "native"}}}}},
                                     .build_configuration = "managed",
                                     .inputs = {{"managed", {language::reference{"managed", "facts"}}},
                                                {"native", {language::project_input{"native-lib", "release", "native", "exports", std::nullopt}}}}};
        profile.defaults.execution.directory_group = "managed";
        profile.outputs = {{"source", {{"source", "facts"}}}, {"linked", {{"source", "facts"}, {"managed", "facts"}, {"binding", "facts"}}}};
        settings.analysis["managed"] = profile;
        builds::configuration native_build;
        builds::config<"command"> native_commands;
        native_commands.operations["build"] = commands.operations.at("native");
        native_build.builders["native"] = builds::with<"command">{.options = native_commands};
        native_build.tasks["build"] = {.builder = "native", .operation = "build"};
        native_build.operations["build"] = {.tasks = {"build"}};
        config::settings native_settings;
        native_settings.build_configurations["native"] = native_build;
        native_settings.default_configurations = {"native"};
        native_settings.analysis["native"] = fixture::profile(language::stage{.server = language::with<"native-artifact">{.options = {.module = "native"}},
                                                                              .build_configuration = "native",
                                                                              .inputs = {{"artifact", {language::build_input{"native", "build", "binary"}}}}});
        native_settings.analysis.at("native").outputs = {{"exports", {{"main", "facts"}}}};
        doc.projects["native-lib"] = std::move(native_settings);
        test::spit(tmp.dir / "config.json", json::write(doc));
        test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {"config.json"})};
        test::ready(gateway, port);
        config::worker worker_config;
        worker_config.gateway = std::format("127.0.0.1:{}", doc.server.worker_port);
        worker_config.name = kotlin ? "kotlin-jni" : "fsharp-pinvoke";
        worker_config.work_root = (tmp.dir / "worker").string();
        worker_config.parallel = 2;
        worker_config.tools["cxx"] = std::getenv("TEST_CXX");
        worker_config.tools["fughetta-llvm"] = std::getenv("TEST_LLVM_HOST");
        worker_config.env["TMPDIR"] = (tmp.dir / "temp").string();
        worker_config.env["JAVA_TOOL_OPTIONS"] =
            "-Djava.io.tmpdir=" + (tmp.dir / "temp").string() + " -Dkotlin.daemon.options=runFilesPath=" + (tmp.dir / "daemon").string();
        std::filesystem::create_directories(tmp.dir / "temp");
        // Reuse downloaded modules when available. The prepare task also works with a fresh Gradle home.
        const auto gradle_home = tmp.dir / "gradle";
        worker_config.env["GRADLE_USER_HOME"] = gradle_home.string();
        std::filesystem::create_directories(gradle_home);
        if (const auto* cache = std::getenv("TEST_GRADLE_MODULES"); kotlin && cache && std::filesystem::is_directory(cache)) {
            const auto modules = gradle_home / "caches" / "modules-2";
            std::filesystem::create_directories(modules);
            for (const auto& entry : std::filesystem::directory_iterator(cache))
                if (entry.path().extension() != ".lock" && entry.path().filename() != "gc.properties")
                    std::filesystem::copy(entry.path(), modules / entry.path().filename(), std::filesystem::copy_options::recursive);
        }
        const auto sdk_root = std::filesystem::path{kotlin ? std::getenv("TEST_JDK") : std::getenv("TEST_DOTNET")};
        const auto sdk = kotlin ? sdk_root : std::filesystem::canonical(sdk_root).parent_path();
        test::spit(tmp.dir / "sdk.json", kotlin ? "{\"sdk\":\"jdk\"}" : "{\"sdk\":\"dotnet\"}");
        worker_config.sdk_manifests[kotlin ? "sdk/java" : "sdk/dotnet"] = {.manifest = (tmp.dir / "sdk.json").string(), .roots = {sdk.string()}};
        if (kotlin) {
            worker_config.env["FUGHETTA_TEST_JAVA_HOME"] = sdk.string();
            worker_config.tools["java"] = std::getenv("TEST_JAVA");
            worker_config.tools["gradle"] = std::getenv("TEST_GRADLE");
            worker_config.tools["fughetta-jvm"] = std::getenv("TEST_JVM_HOST");
            worker_config.tools["fughetta-kotlin"] = std::getenv("TEST_KOTLIN_ADAPTER");
            worker_config.tools["fughetta-gradle-model"] = std::getenv("TEST_GRADLE_MODEL");
        } else {
            worker_config.tools["dotnet"] = std::getenv("TEST_DOTNET");
            worker_config.tools["fughetta-clr"] = std::getenv("TEST_CLR_HOST");
            worker_config.tools["fughetta-fsharp"] = std::getenv("TEST_FSHARP_ADAPTER");
            worker_config.tools["fughetta-msbuild-model"] = std::getenv("TEST_MSBUILD_MODEL");
        }
        struct worker_status {
            std::string name;
            bool online;
        };
        auto online_workers = [&] { return test::decode<std::vector<worker_status>>(test::request(port, http::method::GET, "/api/v1/workers").body); };
        auto start_worker = [&] {
            test::spit(tmp.dir / "worker.json", json::write(worker_config));
            auto command = test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {"worker.json"});
            if (kotlin) {
                command.env = host::environment();
                command.env->erase("JAVA_HOME");
            }
            auto process = std::make_unique<test::child>(std::move(command));
            test::eventually(
                [&] {
                    test::require(!process->finished(), process->output());
                    return std::ranges::any_of(online_workers(), [&](const auto& current) { return current.online && current.name == worker_config.name; });
                },
                std::chrono::seconds{60});
            return process;
        };
        auto worker = start_worker();
        std::map<std::string, std::string> files;
        files["native.cpp"] = "extern \"C\" int native_twice(int input) { return input * 2; }\n"
                              "extern \"C\" int Java_sample_Bridge_nativeTwice(void*, void*, int input) { return native_twice(input); }\n";
        const std::string path = kotlin ? "src/main/kotlin/Main.kt" : "Library.fs";
        if (kotlin) {
            files["settings.gradle"] = "pluginManagement { repositories { mavenCentral(); gradlePluginPortal() } }\n"
                                       "rootProject.name = 'managed-e2e'\ninclude 'library'\n";
            files["gradle.properties"] = "org.gradle.java.installations.fromEnv=FUGHETTA_TEST_JAVA_HOME\n"
                                         "org.gradle.java.installations.auto-detect=false\norg.gradle.java.installations.auto-download=false\n";
            files["build.gradle"] = "plugins { id 'org.jetbrains.kotlin.jvm' version '2.4.21'; id 'application' }\n"
                                    "repositories { mavenCentral() }\nkotlin { jvmToolchain(25) }\n"
                                    "subprojects { apply plugin: 'org.jetbrains.kotlin.jvm'; repositories { mavenCentral() }; kotlin { jvmToolchain(25) } }\n"
                                    "dependencies { implementation project(':library') }\n"
                                    "application { mainClass = 'sample.MainKt'; applicationDefaultJvmArgs = ['-Djava.library.path=' + projectDir] }\n"
                                    "gradle.projectsEvaluated { new File(" +
                                    json::write(configured.string()) +
                                    ").append('configure\\n'); new File(" +
                                    json::write(gradle_processes.string()) + ").append(ProcessHandle.current().pid().toString() + '\\n') }\n"
                                    "tasks.named('compileKotlin') { doLast { new File(" +
                                    json::write(compiled.string()) + ").append('compile\\n') } }\n";
            files[path] = "package sample\nimport dependency.scale\nimport dependency.adjust\nimport dependency.choose\n"
                          "fun fromLibrary() = choose(adjust(scale(3)))\nobject Bridge {\n    external fun nativeTwice(input: Int): Int\n"
                          "    fun run(input: Int) = input * 2 + nativeTwice(input)\n}\n"
                          "fun inferred(values: List<Int>): Int {\n    val first = values.first()\n    var total = 0\n"
                          "    for (value in values) total = total + value\n    val pair = first to total\n"
                          "    return listOf(first, pair.second).maxOf { it }\n}\n"
                          "fun main() { System.loadLibrary(\"native\"); check(Bridge.run(3) == 12); java.io.File(" +
                          json::write(ran.string()) + ").appendText(\"run\\n\") }\n";
            files["library/src/main/kotlin/Library.kt"] = "package dependency\n"
                                                          "fun scale(input: Int): Int = input * 2\n"
                                                          "inline fun adjust(input: Int): Int = input + 1\n"
                                                          "fun choose(value: Any): Any = value\n";
        } else {
            files["Library.fsproj"] =
                "<Project "
                "Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><TargetFramework>net10.0</TargetFramework><DebugType>portable</DebugType><Optimize>false</Optimize></"
                "PropertyGroup>"
                "<ItemGroup><Compile Include=\"Shared.fs\"/><Compile Include=\"Library.fs\"/></ItemGroup><Target Name=\"RecordCompilation\" "
                "AfterTargets=\"CoreCompile\" Condition=\"'$(DesignTimeBuild)' != 'true' and '$(SkipCompilerExecution)' != 'true'\"><WriteLinesToFile File=\"" +
                compiled.string() + "\" Lines=\"compile\" Overwrite=\"false\"/></Target></Project>";
            files["Shared.fs"] = "namespace Sample\nmodule Shared =\n    let separateFile = 17\n";
            files[path] = "namespace Sample\nopen System.Runtime.InteropServices\nmodule Library =\n"
                          "    [<DllImport(\"native\", EntryPoint=\"native_twice\", CallingConvention=CallingConvention.Cdecl)>]\n"
                          "    extern int Native(int input)\n    let run input = input * 2 + Native input\n"
                          "    let inferred (values: int list) =\n        let first = List.head values\n        let (named: int) = first\n"
                          "        let total = named + 1\n        List.map string values, total\n"
                          "    type Ticket = { Number: int }\n"
                          "    let issueTicket number = Some { Number = number }\n"
                          "    let readTicket (ticket: Ticket) offset = ticket.Number + offset\n";
            files["check.fsx"] = "#r \"bin/Debug/net10.0/Library.dll\"\n"
                                 "if Sample.Library.run 3 <> 12 then failwith \"wrong native result\"\nSystem.IO.File.AppendAllText(" +
                                 json::write(ran.string()) + ", \"run\\n\")\n";
        }
        auto initial = test::decode<tools::worldline::output>(test::tool(port, "worldline", R"({"project":"app","action":"inspect"})"));
        tools::worldline::input create{.project = "app", .action = "apply", .observed = initial.version, .operation_id = "create-managed"};
        create.changes = std::vector<tools::change>{};
        for (const auto& [name, text] : files)
            create.changes.value()->push_back({.kind = "create", .path = name, .text = text});
        auto created = test::decode<tools::worldline::output>(test::tool(port, "worldline", json::write(create)));
        const auto native_initial = test::decode<tools::worldline::output>(test::tool(port, "worldline", R"({"project":"native-lib","action":"inspect"})"));
        const auto native_created = test::decode<tools::worldline::output>(
            test::tool(port, "worldline",
                       json::write(tools::worldline::input{
                           .project = "native-lib",
                           .action = std::string{"apply"},
                           .observed = native_initial.version,
                           .operation_id = "create-native-project",
                           .changes = std::vector<tools::change>{{.kind = "create", .path = "native.cpp", .text = files.at("native.cpp")}}})));
        test::tool(port, "task",
                   "{\"project\":\"native-lib\",\"action\":\"fork\",\"task\":\"native\",\"worldline\":\"release\",\"from\":\"main\",\"version\":" +
                       json::write(native_created.version) + ",\"operation_id\":\"fork-native-release\"}");
        const auto native_version =
            test::decode<tools::worldline::output>(test::tool(port, "worldline", R"({"project":"native-lib","action":"inspect","worldline":"release"})"))
                .version;
        test::tool(port, "task", R"({"project":"app","action":"create","task":"managed","worldline":"main","operation_id":"bind-managed"})");
        const auto run = test::decode<tools::task::output>(
            test::tool(port, "task", R"({"project":"app","action":"start","task":"managed","operation_id":"start-managed"})"));
        const auto analysis_timeout = std::chrono::seconds{language::timeout(profile.defaults.execution.limits)};
        auto agent = [&](const std::string& name, const std::string& args) {
            return test::tool(port, name, args, false, *run.credential, analysis_timeout);
        };
        auto query = [&](std::string output) {
            return test::decode<tools::language_query::output>(
                agent("language", json::write(tools::language_query::input{.project = "app",
                                                                                .configuration = "managed",
                                                                                .action = std::string{"symbols"},
                                                                                .outputs = std::vector<std::string>{output},
                                                                                .include = std::vector<std::string>{"declarations"},
                                                                                .limit = 250})));
        };
        auto timing = [&](std::string_view phase, const tools::language_query::output& result) {
            std::cerr << (kotlin ? "Kotlin" : "F#") << " managed flow " << phase << ": " << json::write(result.timings)
                      << "; reused_units=" << result.reused_units << '\n';
        };
        if (kotlin) {
            const auto registered = test::decode<tools::language_query::output>(
                agent("language", json::write(tools::language_query::input{
                                      .project = "app", .configuration = "managed", .action = std::string{"describe"}, .provider = "configured-kotlin"})));
            test::require(registered.providers && registered.providers->size() == 1 && registered.providers->front().id == "configured-kotlin" &&
                              registered.providers->front().adapter == language::adapter_kind::helper,
                          "configured helper registration was not described");
        }
        if (!kotlin) {
            const auto focused = test::decode<tools::language_query::output>(agent(
                "language", json::write(tools::language_query::input{.project = "app",
                                                                          .configuration = "managed",
                                                                          .action = std::string{"symbols"},
                                                                          .scope = tools::language_query::scope_filter{.files = std::vector<std::string>{path}},
                                                                          .outputs = std::vector<std::string>{"source"},
                                                                          .include = std::vector<std::string>{"declarations"},
                                                                          .limit = 250})));
            test::require(!focused.items.empty() && !std::ranges::any_of(focused.items, [](const auto& item) { return item.name == "separateFile"; }),
                          "F# file demand exported an unrelated file");
            expect(!std::filesystem::exists(compiled)) << "file demand keeps final compilation deferred";
        }
        auto source_only = query("source");
        if (!kotlin)
            test::require(std::ranges::any_of(source_only.items, [](const auto& item) { return item.name == "separateFile"; }),
                          "project demand reused incomplete file-only F# records");
        timing("source", source_only);
        test::require(source_only.view.has_value() && source_only.analyzed && !source_only.items.empty(), "managed source query produced no view");
        const semantic::selection read_ticket{.name = semantic::named{.text = "readTicket"}};
        auto check_ticket_surface = [](const auto& result) {
            const auto callable = std::ranges::find(result.items, "readTicket", &semantic::item::name);
            test::require(callable != result.items.end() && callable->type_id, "F# inspect lost the selected callable");
            const auto signature = std::ranges::find(result.types, *callable->type_id, &semantic::records::type::id);
            test::require(signature != result.types.end() && signature->kind == "fsharp:curried_signature" && signature->parameter_groups.size() == 2 &&
                              std::ranges::contains(result.symbols, "Number", &semantic::records::symbol::name) &&
                              std::ranges::contains(result.producers, "issueTicket", &semantic::records::symbol::name),
                          "F# inspect did not expand the curried signature and its option-wrapped producer");
        };
        if (!kotlin) {
            check_ticket_surface(test::decode<tools::language_query::output>(
                agent("language", json::write(tools::language_query::input{
                                      .project = "app", .action = std::string{"inspect"}, .selector = read_ticket, .view = source_only.view}))));
            const auto ticket = test::decode<tools::language_query::output>(
                agent("language", json::write(tools::language_query::input{
                                      .project = "app", .action = std::string{"inspect"},
                                      .selector = semantic::selection{.name = semantic::named{.text = "Ticket", .kind = semantic::symbol_kind{"type"}}},
                                      .view = source_only.view, .include = std::vector<std::string>{"consumers"}})));
            test::require(ticket.items.size() == 1 && ticket.items.front().kind == "fsharp:record" &&
                              std::ranges::contains(ticket.consumers, "readTicket", &semantic::records::symbol::name),
                          "F# record lost its published kind or curried consumer in a type query");
        }
        expect(!std::filesystem::exists(compiled)) << "source-only output must prepare the model without final compilation";
        if (kotlin)
            expect(test::slurp(configured) == "configure\n") << "the declared prepare and model tasks share one Gradle build evaluation";
        const auto imported = std::ranges::find_if(source_only.items, [&](const auto& item) {
            return item.name == (kotlin ? "nativeTwice" : "Native") && item.domain == (kotlin ? "kotlin" : "fsharp");
        });
        test::require(imported != source_only.items.end(), "source view has no managed native declaration");
        const auto inferred = test::decode<tools::language_query::output>(
            agent("language", json::write(tools::language_query::input{.project = "app",
                                                                            .configuration = "managed",
                                                                            .action = std::string{"inspect"},
                                                                            .selector = semantic::selection{.name = semantic::named{"inferred"}},
                                                                            .outputs = std::vector<std::string>{"source"}})));
        const auto spelled = inferred.typed_ranges |
                             std::views::transform([&](const auto& row) { return files.at(path).substr(row.range.begin, row.range.end - row.range.begin); }) |
                             std::ranges::to<std::set<std::string>>();
        const std::set<std::string> expected = kotlin ? std::set<std::string>{"first", "total", "value", "pair", "first()", "first to total", "it"}
                                                      : std::set<std::string>{"first", "total", "List.head values", "List.map string values"};
        expect(std::ranges::includes(spelled, expected) && !spelled.contains("named") &&
               std::ranges::none_of(spelled, [](const std::string& text) { return text.contains('+'); }))
            << "typed ranges cover only unwritten types: " << json::write(spelled);
        if (kotlin)
            test::require(std::ranges::any_of(
                              source_only.symbols,
                              [&](const auto& symbol) { return symbol.id == imported->symbol_id && symbol.ownership.origin.provider == "configured-kotlin"; }),
                          "helper records lost the registered provider identity");
        for (const auto& action : {"definitions", "references"}) {
            const auto selected = test::decode<tools::language_query::output>(
                agent("language", json::write(tools::language_query::input{
                                      .project = "app",
                                      .configuration = "managed",
                                      .action = std::string{action},
                                      .selector = semantic::selection{.entity = semantic::entity{imported->symbol_id, imported->context}},
                                      .outputs = std::vector<std::string>{"source"},
                                      .limit = 250})));
            test::require(!selected.items.empty(), "managed source lookup lost its selected declaration");
            if (std::string_view{action} == "references")
                test::require(!selected.references.empty(), "managed source reference query produced no caller");
            else
                test::require(!selected.items.front().definitions.empty(), "managed source definition query produced no source location");
        }
        expect(!std::filesystem::exists(compiled)) << "source definitions and references keep final compilation deferred";
        expect(test::tool(port, "language", R"({"project":"native-lib","configuration":"native","action":"symbols"})", true, *run.credential)
                   .contains("permission_denied"))
            << "the app execution cannot directly read another project without its declared dependency grant";
        const auto pending_link = test::decode<tools::language_query::output>(
            agent("language", json::write(tools::language_query::input{.project = "app",
                                                                            .configuration = "managed",
                                                                            .action = std::string{"symbols"},
                                                                            .outputs = std::vector<std::string>{"linked"},
                                                                            .include = std::vector<std::string>{"declarations"},
                                                                            .limit = 250,
                                                                            .budget = semantic::budget{.units = 0}})));
        test::require(pending_link.status == "partial" && pending_link.continuation && pending_link.snapshots.size() == 2 &&
                          semantic::snapshot_for(pending_link.snapshots, "native-lib").worldline == "release" &&
                          semantic::snapshot_for(pending_link.snapshots, "native-lib").version == *native_version,
                      "the partial graph did not pin the separately authorized native project");
        const auto native_lease =
            test::decode<tools::scope::output>(test::tool(port, "scope",
                                                          json::write(tools::scope::input{.project = "native-lib",
                                                                                               .action = std::string{"acquire"},
                                                                                               .worldline = std::string{"release"},
                                                                                               .files = std::vector<std::string>{"native.cpp"},
                                                                                               .version = native_version,
                                                                                               .operation_id = "edit-native-dependency"})));
        auto changed_native_text = files.at("native.cpp");
        changed_native_text.replace(changed_native_text.find("input * 2"), 9, "input * 3");
        const auto native_edited = test::decode<tools::worldline::output>(
            test::tool(port, "worldline",
                       json::write(tools::worldline::input{
                           .project = "native-lib",
                           .action = std::string{"apply"},
                           .worldline = std::string{"release"},
                           .observed = native_version,
                           .leases = std::vector<std::string>{native_lease.leases.front().token},
                           .operation_id = "change-native-dependency",
                           .changes = std::vector<tools::change>{{.kind = "write", .path = "native.cpp", .text = changed_native_text}}})));
        auto linked = test::decode<tools::language_query::output>(
            agent("language", json::write(tools::language_query::input{
                                  .project = "app", .continuation = pending_link.continuation, .budget = semantic::budget{.units = 512}})));
        test::require(linked.status == "complete" && semantic::snapshot_for(linked.snapshots, "native-lib").version == *native_version,
                      "continuation did not complete with its original native project snapshot; first=" + json::write(pending_link.snapshots) +
                          " resumed=" + json::write(linked));
        const auto refreshed_link = query("linked");
        test::require(refreshed_link.status == "complete" && refreshed_link.version == linked.version &&
                          semantic::snapshot_for(refreshed_link.snapshots, "native-lib").version == *native_edited.version,
                      "a fresh graph did not select the native project's updated snapshot");
        const auto native_comparison = test::decode<tools::language_query::output>(
            agent("language",
                  json::write(tools::language_query::input{
                      .project = "app", .action = std::string{"impact"}, .from_view = linked.view, .to_view = refreshed_link.view, .limit = 250, .depth = 5})));
        test::require(native_comparison.comparison && std::ranges::any_of(native_comparison.comparison->reanalysis,
                                                                          [](const auto& unit) {
                                                                              return unit.unit.project == "native-lib" && !unit.inputs.empty() &&
                                                                                     std::ranges::any_of(unit.tasks, [](const auto& task) {
                                                                                         return task.project == "native-lib" &&
                                                                                                task.configuration == "native" && task.operation == "build";
                                                                                     });
                                                                          }),
                      "cross-project impact did not retain the native input change and its build task");
        const auto retained_link = test::decode<tools::language_query::output>(agent(
            "language", json::write(tools::language_query::input{.project = "app", .action = std::string{"symbols"}, .view = linked.view, .limit = 250})));
        expect(semantic::snapshot_for(retained_link.snapshots, "native-lib").version == *native_version)
            << "the old view keeps the separately authorized dependency snapshot after a new call";
        timing("linked", linked);
        test::require(linked.view.has_value() && std::filesystem::exists(compiled), "linked output did not run the configured managed compilation");
        const std::string entry = kotlin ? "Java_sample_Bridge_nativeTwice" : "native_twice";
        const auto native = std::ranges::find_if(linked.items, [&](const auto& item) { return item.domain == "native" && item.name == entry; });
        test::require(native != linked.items.end(), "linked view has no native export");
        auto impact = test::decode<tools::language_query::output>(
            agent("language",
                  json::write(tools::language_query::input{.project = "app",
                                                                .action = std::string{"impact"},
                                                                .selector = semantic::selection{.entity = semantic::entity{native->symbol_id, native->context}},
                                                                .view = linked.view,
                                                                .changes = std::vector<std::string>{"body"},
                                                                .depth = 5,
                                                                .limit = 250})));
        test::require(std::ranges::any_of(impact.items, [&](const auto& item) { return item.domain == (kotlin ? "kotlin" : "fsharp") && item.name == "run"; }),
                      "gateway impact did not reach the managed source function through binds/calls/emits");
        auto leased = test::decode<tools::scope::output>(agent("scope", json::write(tools::scope::input{.project = "app",
                                                                                                             .action = std::string{"acquire"},
                                                                                                             .files = std::vector<std::string>{path},
                                                                                                             .version = created.version,
                                                                                                             .operation_id = "edit-managed"})));
        auto edited_text = files.at(path);
        edited_text.replace(edited_text.find("input * 2"), 9, "input * 3");
        if (kotlin)
            edited_text.replace(edited_text.find("== 12"), 5, "== 15");
        tools::worldline::input edit{.project = "app",
                                     .action = "apply",
                                     .observed = created.version,
                                     .leases = std::vector<std::string>{leased.leases.front().token},
                                     .operation_id = "change-managed",
                                     .changes = std::vector<tools::change>{{.kind = "write", .path = path, .text = edited_text}}};
        // F#'s test script follows the edited program's expected result in the
        // same observed source mutation, under its own file lease.
        if (!kotlin) {
            auto script_lease =
                test::decode<tools::scope::output>(agent("scope", json::write(tools::scope::input{.project = "app",
                                                                                                       .action = std::string{"acquire"},
                                                                                                       .files = std::vector<std::string>{"check.fsx"},
                                                                                                       .version = created.version,
                                                                                                       .operation_id = "edit-check"})));
            edit.leases.value()->push_back(script_lease.leases.front().token);
            auto script = files.at("check.fsx");
            script.replace(script.find("<> 12"), 5, "<> 15");
            edit.changes.value()->push_back({.kind = "write", .path = "check.fsx", .text = script});
        }
        const auto edited = test::decode<tools::worldline::output>(agent("worldline", json::write(edit)));
        const auto before_compile = test::slurp(compiled);
        const auto before_configuration = kotlin ? test::slurp(configured) : "";
        auto updated_source = query("source");
        timing("edited source", updated_source);
        if (kotlin)
            expect(test::slurp(configured) == before_configuration && updated_source.timings.preparation_seconds == 0.0)
                << "a source content edit reuses the prepared model while analyzing the new snapshot";
        expect(updated_source.version == edited.version && updated_source.view != source_only.view && test::slurp(compiled) == before_compile)
            << "editing and rechecking source keeps compilation deferred";
        auto updated = query("linked");
        timing("edited linked", updated);
        expect(updated.version == edited.version && updated.view != linked.view && test::slurp(compiled) != before_compile);
        const auto compared = test::decode<tools::language_query::output>(
            agent("language", json::write(tools::language_query::input{
                                  .project = "app", .action = std::string{"impact"}, .from_view = linked.view, .to_view = updated.view, .limit = 250})));
        expect(compared.comparison.has_value() && !compared.comparison->reanalysis.empty());
        const auto pinned = "{\"project\":\"app\",\"configurations\":[\"managed\"],\"version\":" + json::write(edited.version) + "}";
        const auto built = test::decode<tools::results>(agent("build", pinned));
        const auto tester_before = kotlin ? test::slurp(gradle_processes) : std::string{};
        const auto tested = test::decode<tools::results>(agent("test", pinned));
        expect(built.ok && tested.ok && tested.revision == *edited.version && std::filesystem::exists(ran));
        if (kotlin) {
            const auto builds = test::slurp(gradle_processes);
            const auto delimiter = builds.find('\n');
            test::require(delimiter != std::string::npos && builds.size() > tester_before.size() &&
                              std::ranges::all_of(std::views::split(std::string_view{builds}.substr(0, builds.size() - 1), '\n'), [&](auto pid) {
                                  return std::ranges::equal(pid, std::string_view{builds}.substr(0, delimiter));
                              }),
                          "Gradle model export, linked builds and JNI execution did not share the slot's daemon across snapshots");
            const auto& result = tested.results.at(0);
            const auto step = std::ranges::find(result.steps, "test", &tools::step_result::name);
            test::require(step != result.steps.end(), "Kotlin JNI execution has no test step timing");
            std::println(std::cerr, "Kotlin managed flow JNI test: step_seconds={:.6f}, worker_seconds={:.6f}", step->seconds, result.seconds);
        }
        struct persisted_reuse {
            semantic::unit_id consumer, producer;
            semantic::view_id initial, before, body_only, inlined, overload, previous, returned;
        };
        std::optional<persisted_reuse> persistence;
        if (kotlin) {
            const auto consumer = std::ranges::find(updated_source.symbols, "fromLibrary", &semantic::records::symbol::name);
            test::require(consumer != updated_source.symbols.end(), "source module has no cross-module consumer");
            persistence = persisted_reuse{.consumer = consumer->ownership.owner_unit, .initial = *source_only.view, .before = *updated_source.view};
            const std::string library_path = "library/src/main/kotlin/Library.kt";
            auto library_lease =
                test::decode<tools::scope::output>(agent("scope", json::write(tools::scope::input{.project = "app",
                                                                                                       .action = std::string{"acquire"},
                                                                                                       .files = std::vector<std::string>{library_path},
                                                                                                       .version = edited.version,
                                                                                                       .operation_id = "edit-read-library"})));
            auto version = edited.version;
            auto write_library = [&](std::string text, const std::string& operation) {
                const auto result = test::decode<tools::worldline::output>(
                    agent("worldline", json::write(tools::worldline::input{
                                           .project = "app",
                                           .action = std::string{"apply"},
                                           .observed = version,
                                           .leases = std::vector<std::string>{library_lease.leases.front().token},
                                           .operation_id = operation,
                                           .changes = std::vector<tools::change>{{.kind = "write", .path = library_path, .text = std::move(text)}}})));
                version = result.version;
            };
            auto read_source = [&] {
                return test::decode<tools::language_query::output>(
                    agent("language", json::write(tools::language_query::input{.project = "app",
                                                                                    .configuration = "managed",
                                                                                    .action = std::string{"symbols"},
                                                                                    .outputs = std::vector<std::string>{"source"},
                                                                                    .include = std::vector<std::string>{"declarations", "type"},
                                                                                    .limit = 250})));
            };
            auto body_text = files.at(library_path);
            body_text.replace(body_text.find("input * 2"), 9, "input + input");
            write_library(body_text, "body-only-library-edit");
            test::require(worker->stop() == 0, "first managed worker did not stop");
            test::eventually([&] { return std::ranges::none_of(online_workers(), [](const auto& current) { return current.online; }); });
            worker_config.name = "kotlin-jni-cold";
            worker_config.work_root = (tmp.dir / "other-worker").string();
            worker = start_worker();
            const auto body_only = read_source();
            timing("cold worker body-only dependency edit", body_only);
            test::require(body_only.analyzed && body_only.version == version && body_only.view != updated_source.view && body_only.reused_units > 0,
                          "cold worker did not reuse a persisted source contribution after a body-only dependency edit");
            persistence->body_only = *body_only.view;
            auto filtered_reanalysis = [&](const tools::language_query::output& before, const tools::language_query::output& after, const std::string& name,
                                           semantic::dependency_kind kind) {
                const auto producer = std::ranges::find(before.symbols, name, &semantic::records::symbol::name);
                test::require(producer != before.symbols.end(), "comparison has no selected dependency producer");
                persistence->producer = producer->ownership.owner_unit;
                const auto filtered = test::decode<tools::language_query::output>(
                    agent("language", json::write(tools::language_query::input{
                                          .project = "app",
                                          .action = std::string{"impact"},
                                          .selector = semantic::selection{.entity = semantic::entity{producer->id, producer->ownership.context}},
                                          .from_view = before.view,
                                          .to_view = after.view,
                                          .limit = 250})));
                test::require(filtered.comparison.has_value(), "producer-filtered impact returned no comparison");
                const auto changed = std::ranges::find(filtered.comparison->reanalysis, persistence->consumer, &semantic::reanalysis_change::id);
                test::require(changed != filtered.comparison->reanalysis.end(), "producer-filtered impact lost its semantic consumer");
                test::require(std::ranges::any_of(changed->dependencies,
                                                  [&](const auto& dependency) {
                                                      return dependency.read.kind == kind && !dependency.read.producer && dependency.read.channel &&
                                                             std::ranges::contains(dependency.read.producers, producer->ownership.owner_unit) &&
                                                             std::ranges::contains(dependency.producers, producer->ownership.owner_unit);
                                                  }),
                              "compiler observation did not retain the producer unit supplied by its typed unit key");
            };
            auto inline_text = body_text;
            inline_text.replace(inline_text.find("input + 1"), 9, "input + 2");
            write_library(inline_text, "inline-library-body");
            const auto inlined = read_source();
            persistence->inlined = *inlined.view;
            filtered_reanalysis(body_only, inlined, "adjust", semantic::dependency_kind::body);
            write_library(inline_text + "fun choose(value: Int): Int = value\n", "library-overload");
            const auto overloaded = read_source();
            persistence->overload = *overloaded.view;
            filtered_reanalysis(inlined, overloaded, "choose", semantic::dependency_kind::lookup);
            const auto selected = std::ranges::find(overloaded.symbols, "fromLibrary", &semantic::records::symbol::name);
            test::require(selected != overloaded.symbols.end() && selected->type.has_value(), "new overload lost the consumer's inferred type");
            const auto callable = std::ranges::find(overloaded.types, *selected->type, &semantic::records::type::id);
            test::require(callable != overloaded.types.end() && callable->return_type.has_value(), "consumer has no function result type");
            const auto result_type = std::ranges::find(overloaded.types, *callable->return_type, &semantic::records::type::id);
            test::require(result_type != overloaded.types.end() && result_type->name == std::optional<std::string>{"kotlin.Int"},
                          "new more-specific overload did not update the consumer's inferred result");
            test::tool(port, "task",
                       "{\"project\":\"app\",\"action\":\"fork\",\"task\":\"previous\",\"worldline\":\"previous\",\"from\":\"main\",\"version\":" +
                           json::write(created.version) + ",\"operation_id\":\"fork-managed-previous\"}");
            const auto previous_run = test::decode<tools::task::output>(
                test::tool(port, "task", R"({"project":"app","action":"start","task":"previous","operation_id":"start-managed-previous"})"));
            const auto previous = test::decode<tools::language_query::output>(
                test::tool(port, "language",
                           json::write(tools::language_query::input{.project = "app",
                                                                         .configuration = "managed",
                                                                         .action = std::string{"symbols"},
                                                                         .outputs = std::vector<std::string>{"source"},
                                                                         .include = std::vector<std::string>{"declarations"},
                                                                         .limit = 250}),
                           false, *previous_run.credential));
            timing("previous worldline source", previous);
            const auto returned = query("source");
            timing("return to edited worldline source", returned);
            expect(previous.version == created.version && returned.version == version && !previous.items.empty() && !returned.items.empty())
                << "alternating worldlines keeps each prepared model and source result attached to its requested snapshot";
            persistence->previous = *previous.view;
            persistence->returned = *returned.view;

            const auto build_lease = test::decode<tools::scope::output>(
                agent("scope", json::write(tools::scope::input{.project = "app", .action = "acquire", .files = std::vector<std::string>{"build.gradle"},
                                                                .version = version, .operation_id = "edit-gradle-script"})));
            const auto before_build_edit = test::slurp(configured);
            const auto build_edit = test::decode<tools::worldline::output>(
                agent("worldline", json::write(tools::worldline::input{
                                       .project = "app", .action = "apply", .observed = version,
                                       .leases = std::vector<std::string>{build_lease.leases.front().token}, .operation_id = "change-gradle-script",
                                       .changes = std::vector<tools::change>{{.kind = "write", .path = "build.gradle", .text = files.at("build.gradle") + "\n// revised build\n"}}})));
            const auto changed_model = query("source");
            timing("edited Gradle script", changed_model);
            expect(changed_model.version == build_edit.version && test::slurp(configured) == before_build_edit + "configure\n")
                << "a build script edit reevaluates the Gradle model";
        }
        test::require(worker->stop() == 0 && gateway.stop() == 0 && source->stop() == 0, "managed process shutdown failed");
        semantic::store stored{tmp.dir / "state/.semantic"};
        if (!kotlin)
            check_ticket_surface(stored.load(*source_only.view)->query({.action = "inspect", .selector = read_ticket}));
        if (persistence) {
            auto contribution = [&](const std::string& view) { return stored.describe(view).units.at(persistence->consumer); };
            expect(contribution(persistence->body_only) == contribution(persistence->before))
                << "body-only dependency edits reuse the exact gateway-persisted consumer shards on a cold worker";
            expect(contribution(persistence->inlined) != contribution(persistence->body_only) &&
                   contribution(persistence->overload) != contribution(persistence->inlined))
                << "inline implementation and lookup candidate changes invalidate persisted consumers";
            expect(contribution(persistence->previous) == contribution(persistence->initial) &&
                   contribution(persistence->returned) == contribution(persistence->overload))
                << "worldline switches select each snapshot's persisted consumer contribution";
            const auto first = stored.load(persistence->before);
            bool lookup = false, body = false;
            for (const auto& piece : first->pieces)
                if (piece->unit.id == persistence->consumer) {
                    for (const auto& read : piece->unit.observed_dependencies) {
                        const auto attributed = std::ranges::contains(read.producers, persistence->producer);
                        lookup |= read.kind == semantic::dependency_kind::lookup && read.channel.has_value() && !read.producer && attributed;
                        body |= read.kind == semantic::dependency_kind::body && read.channel.has_value() && !read.producer && attributed;
                    }
                    test::require(std::ranges::none_of(piece->unit.direct_inputs, [](const auto& input) { return input.kind == semantic::input_kind::source; }),
                                  "source module still binds the entire project snapshot");
                }
            test::require(lookup && body, "persisted consumer did not record compiler declaration and inline reads");
        }
    }

    static constexpr auto cases = [] {
        "Kotlin stage graph defers compilation and connects edits to JNI execution"_test = [] {
            for (const auto* name : {"TEST_GRADLE", "TEST_JVM_HOST", "TEST_KOTLIN_ADAPTER", "TEST_CXX", "TEST_JAVA", "TEST_JDK", "TEST_LLVM_HOST"})
                if (!std::getenv(name)) {
                    skip / "Kotlin gateway requires the installed JVM toolchain"_test = [] {};
                    return;
                }
            managed_flow(true);
        };
        "FSharp stage graph defers compilation and connects edits to PInvoke execution"_test = [] {
            for (const auto* name : {"TEST_DOTNET", "TEST_CLR_HOST", "TEST_FSHARP_ADAPTER", "TEST_CXX", "TEST_LLVM_HOST"})
                if (!std::getenv(name)) {
                    skip / "FSharp gateway requires the installed CLR toolchain"_test = [] {};
                    return;
                }
            managed_flow(false);
        };
        "task credentials bind concurrent runs, survive restart and revoke source access"_test = [] {
            test::scratch tmp{"task-context"};
            auto source_port = test::port(), port = test::port();
            auto source = start_source(tmp.dir, source_port);
            auto doc = test::document(tmp.dir, port, source_port);
            doc.projects["app.task"] = doc.projects.at("app");
            doc.projects.erase("app");
            test::spit(tmp.dir / "config.json", json::write(doc));
            test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {"config.json"})};
            test::ready(gateway, port);
            auto initial = test::decode<tools::worldline::output>(test::tool(port, "worldline", R"({"project":"app.task","action":"inspect"})"));
            auto missing = test::request(
                port, http::method::POST, "/mcp",
                R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"worldline","arguments":{"project":"app.task","action":"inspect"}}})");
            expect(missing.body.contains("context_required"));
            expect(test::request(port, http::method::POST, "/api/v1/projects/app.task/node", R"({"action":"scope_register","analyzed_scopes":[]})").status ==
                   403);
            auto fork = "{\"project\":\"app.task\",\"action\":\"fork\",\"task\":\"parser\",\"worldline\":\"task-parser\",\"from\":\"main\",\"version\":" +
                        json::write(initial.version) + ",\"operation_id\":\"task-fork\"}";
            test::tool(port, "task", fork);
            test::tool(port, "task", fork);
            auto start = [&](std::string operation) {
                return test::decode<tools::task::output>(test::tool(
                    port, "task", "{\"project\":\"app.task\",\"action\":\"start\",\"task\":\"parser\",\"operation_id\":" + json::write(operation) + "}"));
            };
            auto a = start("run-a"), b = start("run-b");
            expect(a.run != b.run && a.credential != b.credential && start("run-a").credential == a.credential);
            auto agent = [&](std::string name, std::string args, bool error = false, bool second = false) {
                return test::tool(port, name, args, error, *(second ? b.credential : a.credential));
            };
            auto tree = test::decode<tools::worldline::output>(agent("worldline", R"({"action":"inspect"})"));
            expect(tree.worldline == "task-parser");
            expect(agent("worldline", R"({"action":"inspect","worldline":"main"})", true).contains("permission_denied"));
            expect(agent("worldline", R"({"action":"inspect","project":"elsewhere"})", true).contains("permission_denied"));
            expect(agent("task", R"({"project":"app.task","action":"start","task":"parser","operation_id":"escape"})", true).contains("permission_denied"));
            auto rest = test::request(port, http::method::POST, "/api/v1/projects/app.task/worldline",
                                      R"({"project":"app.task","action":"inspect","worldline":"main"})", *a.credential);
            expect(rest.status == 403);
            auto acquired = test::decode<tools::scope::output>(agent(
                "scope", "{\"action\":\"acquire\",\"entries\":[\"new.txt\"],\"version\":" + json::write(tree.version) + ",\"operation_id\":\"entry\"}"));
            auto mutation = "{\"action\":\"apply\",\"observed\":" + json::write(tree.version) + ",\"operation_id\":\"create\",\"leases\":[" +
                            json::write(acquired.leases[0].token) + "],\"changes\":[{\"kind\":\"create\",\"path\":\"new.txt\",\"text\":\"\"}]}";
            expect(agent("worldline", mutation, true, true).contains("scope_expired"));
            auto created = test::decode<tools::worldline::output>(agent("worldline", mutation));
            expect(created.version.has_value());
            auto observer = test::decode<tools::task::output>(
                test::tool(port, "task", R"({"project":"app.task","action":"start","task":"parser","operation_id":"observer","writable":false})"));
            auto observed = test::decode<tools::worldline::output>(test::tool(port, "worldline", R"({"action":"inspect"})", false, *observer.credential));
            expect(observed.version == created.version);
            expect(test::tool(port, "worldline",
                              "{\"action\":\"apply\",\"observed\":" + json::write(created.version) +
                                  ",\"operation_id\":\"readonly-write\",\"changes\":[{\"kind\":\"create\",\"path\":\"readonly.txt\",\"text\":\"forbidden\"}]}",
                              true, *observer.credential)
                       .contains("permission_denied"));
            expect(source->stop() == 0);
            source = start_source(tmp.dir, source_port);
            expect(test::decode<tools::worldline::output>(agent("worldline", R"({"action":"read","path":"new.txt"})")).text == "");
            auto main = test::decode<tools::worldline::output>(test::tool(port, "worldline", R"({"project":"app.task","action":"inspect"})"));
            expect(main.version == initial.version);
            test::tool(port, "task", "{\"project\":\"app.task\",\"action\":\"stop\",\"task\":\"parser\",\"run\":" + json::write(a.run) + "}");
            auto revoked = test::request(port, http::method::POST, "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", *a.credential);
            expect(revoked.status == 401);
            auto remaining = test::decode<tools::scope::output>(agent("scope", R"({"action":"list"})", false, true));
            expect(remaining.leases.empty());
            auto file = test::decode<tools::scope::output>(
                agent("scope",
                      "{\"action\":\"acquire\",\"files\":[\"new.txt\"],\"version\":" + json::write(created.version) + ",\"operation_id\":\"empty-file\"}",
                      false, true));
            expect(file.leases.size() == 1u && file.leases[0].kind == "file");
            test::tool(port, "task", R"({"project":"app.task","action":"close","task":"parser"})");
            expect(test::request(port, http::method::POST, "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", *b.credential).status == 401);
            expect(gateway.stop() == 0 && source->stop() == 0);
        };

        "language preparation requires a worker and never executes on source"_test = [] {
            test::scratch tmp{"analysis-routing"};
            auto source_port = test::port(), port = test::port();
            auto source = start_source(tmp.dir, source_port);
            auto doc = test::document(tmp.dir, port, source_port);
            auto& settings = doc.projects.at("app");
            settings.build_configurations["native"] = fixture::cmake("build/native");
            fixture::options<"cmake">(settings.build_configurations["native"].builders.at("main")).export_compile_commands = true;
            settings.analysis["cpp"] =
                fixture::profile({.build_configuration = "native",
                                  .inputs = {{"compilation_database", {language::build_input{"native", "configure", "compilation_database"}}}}});
            test::spit(tmp.dir / "config.json", json::write(doc));
            test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {"config.json"})};
            test::ready(gateway, port);
            auto error = test::tool(port, "language", R"({"project":"app","configuration":"cpp"})", true);
            expect(error.contains("no_worker")) << error;
            auto contract = test::decode<tools::language_query::output>(test::tool(port, "language", R"({"action":"schema"})"));
            expect(contract.schema && !contract.analyzed);
            if (contract.schema)
                expect(contract.schema->to_object().value().get("shard").has_value());
            auto described = test::decode<tools::language_query::output>(test::tool(port, "language", R"({"action":"describe","provider":"ruff"})"));
            test::require(described.providers && described.providers->size() == 1, "provider description missing");
            expect(described.providers->front().facts == std::vector<semantic::fact_kind>{std::string{"diagnostics"}} && !described.analyzed);
            expect(!std::filesystem::exists(tmp.dir / "source/.analysis"));
            expect(gateway.stop() == 0 && source->stop() == 0);
        };

        "one MCP connection edits, analyzes, builds, tests and retrieves a local executable"_test = [] {
            auto clice = std::getenv("TEST_CLICE");
            auto native = std::getenv("TEST_LLVM_HOST");
            if (!clice || !native) {
                skip / "real worker analysis requires TEST_CLICE and TEST_LLVM_HOST"_test = [] {};
                return;
            }
            test::scratch tmp{"worker-analysis"};
            auto source_port = test::port(), port = test::port();
            auto source = start_source(tmp.dir, source_port);
            auto doc = test::document(tmp.dir, port, source_port);
            auto& settings = doc.projects.at("app");
            settings.build_configurations["native"] = fixture::cmake("build/native");
            auto& cmake = fixture::options<"cmake">(settings.build_configurations["native"].builders.at("main"));
            cmake.export_compile_commands = true;
            settings.analysis["cpp"] =
                fixture::profile({.server = fixture::clice("clice"),
                                  .build_configuration = "native",
                                  .inputs = {{"compilation_database", {language::build_input{"native", "configure", "compilation_database"}}}}});
            settings.build_configurations["alternate"] = fixture::cmake("build/alternate");
            fixture::options<"cmake">(settings.build_configurations.at("alternate").builders.at("main")).export_compile_commands = true;
            cmake.outputs["app"] = {"native-artifact", "build/native/app"};
            auto artifact_profile = settings.analysis.at("cpp");
            artifact_profile.stages["artifact"] = {
                .server = language::with<"native-artifact">{.options = {.module = "app"}},
                .build_configuration = "native",
                .inputs = {{"artifact", {language::build_input{"native", "build", "app"}}}, {"source_facts", {language::reference{"main", "facts"}}}}};
            artifact_profile.stages.at("artifact").execution.directory_group = "artifacts";
            artifact_profile.outputs = {{"native", {{"artifact", "facts"}}}};
            settings.analysis["artifact"] = artifact_profile;
            auto without_debug = settings.build_configurations.at("native");
            auto& release = fixture::options<"cmake">(without_debug.builders.at("main"));
            release.build_dir = "build/without-debug";
            release.build_type = "Release";
            release.outputs.at("app").path = "build/without-debug/app";
            settings.build_configurations["without-debug"] = std::move(without_debug);
            auto& release_stage = artifact_profile.stages.at("artifact");
            release_stage.build_configuration = "without-debug";
            release_stage.inputs["artifact"] = {language::build_input{"without-debug", "build", "app"}};
            settings.analysis["artifact-without-debug"] = std::move(artifact_profile);
            auto paired = settings.analysis.at("cpp");
            paired.stages["side"] = paired.stages.at("main");
            std::get<language::build_input>(paired.stages.at("side").inputs.at("compilation_database").front()).configuration = "alternate";
            paired.stages.at("side").after = {"main"};
            paired.stages["mirror"] = paired.stages.at("main");
            paired.stages.at("mirror").after = {"side"};
            paired.defaults = {.fact_limit = 1, .relation_depth = 2, .execution = {.budget = {.units = 128}, .directory_group = "workspace"}};
            paired.outputs = {{"primary", {{"main", "facts"}}}, {"combined", {{"main", "facts"}, {"mirror", "facts"}}}};
            settings.analysis["paired"] = paired;
            auto isolated = paired;
            isolated.stages.at("mirror").execution.directory_group = "review";
            settings.analysis["isolated"] = isolated;
            settings.default_configurations = {"native"};
            settings.build_configurations["native"].operations.at("build").exports = {{"app", "build/native/app"}};
            settings.build_configurations["native"].operations.at("test").exports = {{"app", "build/native/app"}};
            auto project_settings = settings;
            settings = {};
            test::spit(tmp.dir / "config.json", json::write(doc));
            test::child gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {"config.json"})};
            test::ready(gateway, port);
            config::worker worker_config;
            worker_config.gateway = std::format("127.0.0.1:{}", doc.server.worker_port);
            worker_config.name = "source-reader";
            worker_config.work_root = (tmp.dir / "worker").string();
            worker_config.tools["clice"] = clice;
            worker_config.tools["fughetta-llvm"] = native;
            worker_config.tools["python3"] = std::getenv("TEST_PYTHON") ? std::getenv("TEST_PYTHON") : "/usr/bin/python3";
            test::spit(tmp.dir / "sdk/include/answer.hpp", "#pragma once\ninline constexpr int expected_answer = 42;\n");
            test::spit(tmp.dir / "sdk/manifest.json", R"({"name":"test-sdk","release":"one"})");
            worker_config.sdk_manifests["native-test"] = {.manifest = (tmp.dir / "sdk/manifest.json").string(), .roots = {(tmp.dir / "sdk").string()}};
            const auto artifact_reads = tmp.dir / "artifact-reads";
            const auto reader = tmp.dir / "native-reader";
            test::spit(reader, "#!/bin/sh\nprintf 'native\\n' >> \"$ARTIFACT_READER_LOG\"\nexec \"$TEST_LLVM_HOST\" \"$@\"\n");
            std::filesystem::permissions(reader, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
            auto artifact_config = worker_config;
            artifact_config.name = "artifact-reader";
            artifact_config.work_root = (tmp.dir / "artifact-worker").string();
            artifact_config.tools["clice"] = (tmp.dir / "uninstalled-clice").string();
            artifact_config.tools["fughetta-llvm"] = reader.string();
            artifact_config.tools["unused"] = worker_config.tools.at("python3");
            artifact_config.env["ARTIFACT_READER_LOG"] = artifact_reads.string();
            artifact_config.env["TEST_LLVM_HOST"] = native;
            worker_config.tools["fughetta-llvm"] = (tmp.dir / "uninstalled-native").string();
            test::spit(tmp.dir / "artifact-worker.json", json::write(artifact_config));
            auto artifact_worker = std::make_unique<test::child>(test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {"artifact-worker.json"}));
            test::spit(tmp.dir / "worker.json", json::write(worker_config));
            test::child worker{test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {"worker.json"})};
            // Registration follows toolchain inspection, including the local compiler build.
            test::eventually(
                [&] {
                    test::require(!worker.finished(), worker.output());
                    test::require(!artifact_worker->finished(), artifact_worker->output());
                    auto workers = test::request(port, http::method::GET, "/api/v1/workers");
                    return workers.status == 200 && json::read<std::vector<json::value>>(workers.body).value().size() == 2;
                },
                std::chrono::seconds{60});
            // Only MCP requests supply configuration and source; no local source sync or source REST writes.
            auto current = test::tool(port, "project", R"({"project":"app"})");
            struct revision {
                std::uint64_t revision;
            };
            test::tool(port, "save_project",
                       "{\"project\":\"app\",\"body\":{\"revision\":" + std::to_string(test::decode<revision>(current).revision) +
                           ",\"value\":" + config::write(project_settings) + "}}");
            std::string credential = "sk_test";
            auto agent = [&](std::string name, std::string args, bool error = false) { return test::tool(port, name, args, error, credential); };
            auto line = [&](std::string args) { return test::decode<tools::worldline::output>(agent("worldline", std::move(args))); };
            auto initial = line(R"({"project":"app","action":"inspect"})");
            const std::string include = "#include <answer.hpp>\n";
            const std::string before =
                include +
                "int twice(int x) { return x * 3; }\nint main() { return twice(20) + twice(1) == expected_answer ? 0 : 1; }\n#include \"shared.hpp\"\n";
            const std::string after = include + "int twice(int x) { int factor = 2; return x * factor; }\nint main() { return twice(20) + twice(1) == "
                                                "expected_answer ? 0 : 1; }\n#include \"shared.hpp\"\n";
            const std::string shared_header = "#pragma once\nusing Value = VALUE_TYPE;\nstatic Value selected_value = 1;\n";
            const std::string other_source = "#include \"shared.hpp\"\nlong other() { return selected_value; }\n"
                                             "int marker = 0;\ntemplate<class> struct Tag {};\n"
                                             "template<template<class> class C, int* P, int N, int* Empty, typename... Ts> struct Packet {};\n"
                                             "Packet<Tag, &marker, 7, nullptr, long, short> packet;\n"
                                             "template<class T> T first(T value) { return value; }\n"
                                             "template<class T> T second(T value) { return value; }\n"
                                             "int plain() { return 0; }\n"
                                             "template<class T> struct Outer {\n"
                                             "  template<class U> T combine(T left, U right) { return left; }\n"
                                             "  template<class U> int unused() { return 0; }\n};\n"
                                             "int choose(int x) { return x; }\nlong choose(long x) { return x; }\n"
                                             "template<class T> int deferred(T value) { return choose(value); }\n"
                                             "int resolved() { return choose(choose(1)); }\n"
                                             "int narrow(int);\nint narrow(int, int);\n"
                                             "template<class T> int one_candidate(T value) { return narrow(value); }\n"
                                             "template<class T> int unbound(T value) { return unknown(value); }\n"
                                             "struct Receiver { virtual int invoke() { return 1; } };\n"
                                             "int calls(Receiver& r, int (*callback)(int)) { return r.invoke() + r.Receiver::invoke() + callback(1); }\n"
                                             "namespace library {\nint exported(int v) { return v; }\nlong exported(long v) { return v; }\n"
                                             "struct Base { int inherited = 0; int method(); };\n"
                                             "inline namespace extension { int extra = 0; }\nenum Choice { yes, no };\n}\n"
                                             "namespace alias = library;\n"
                                             "namespace facade { using namespace alias; using library::exported; }\n"
                                             "namespace facade { int own = 0; }\n"
                                             "struct Derived : library::Base { using library::Base::inherited; };\n"
                                             "int library::Base::method() { return inherited; }\n"
                                             "int lexical() { int local = 0; { using namespace library; int local = extra; local += yes; }\n"
                                             "  { int local = 2; local += facade::own; } if (local) int branch = 1; else int branch = 2; return local; }\n"
                                             "using BaseAlias = library::Base;\nusing BaseAgain = BaseAlias;\n"
                                             "struct Override : Receiver { int invoke() override { return 1; } };\n"
                                             "int observed = 0;\nvoid mutate(int& x) { x = 1; }\n"
                                             "int usage() { observed = 1; observed += 2; ++observed; mutate(observed); return observed; }\n"
                                             "struct Widget {\n"
                                             "  explicit Widget(int = 4);\n  Widget(const Widget&);\n"
                                             "  [[nodiscard]] int score([[maybe_unused]] int = 1 + 2) const;\n"
                                             "protected:\n  mutable int cached;\nprivate:\n  static int secret;\n};\n"
                                             "Widget::Widget(int amount) : cached(amount) {}\n"
                                             "Widget::Widget(const Widget& original) : cached(original.cached) {}\n"
                                             "int Widget::score(int adjustment) const { return cached + adjustment; }\n"
                                             "int Widget::secret = 0;\n"
                                             "void construct() { Widget direct(1); Widget listed{2}; Widget defaulted; Widget copied = direct; }\n"
                                             "struct Converted { Converted(int) {} };\nvoid consume(Converted) {}\n"
                                             "void convert() { consume(3); }\n"
                                             "#if __has_include(\"optional.hpp\")\n#include \"optional.hpp\"\n#endif\n";
            const auto preparations = tmp.dir / "preparations";
            const auto preparation_directories = tmp.dir / "preparation-directories";
            auto last_directory = [&] {
                std::istringstream lines{test::slurp(preparation_directories)};
                std::string line, last;
                while (std::getline(lines, line))
                    if (!line.empty())
                        last = line;
                test::require(!last.empty(), "configuration did not report its installed source directory");
                return std::filesystem::path{last};
            };
            const std::string cmake_source = "file(APPEND \"" + preparations.generic_string() + "\" \"configure\\n\")\nfile(APPEND \"" +
                                             preparation_directories.generic_string() +
                                             "\" \"${CMAKE_CURRENT_SOURCE_DIR}\\n\")\n"
                                             "cmake_minimum_required(VERSION 3.20)\nproject(mcp_loop CXX)\nadd_executable(app "
                                             "main.cpp other.cpp)\n"
                                             // Keep debug locations relative when a worker receives a different build directory.
                                             "target_compile_options(app PRIVATE -nostdinc \"-fdebug-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.\" "
                                             "\"-fdebug-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=.\")\n"
                                             "set_source_files_properties(main.cpp PROPERTIES COMPILE_DEFINITIONS VALUE_TYPE=int)\n"
                                             "set_source_files_properties(other.cpp PROPERTIES COMPILE_DEFINITIONS VALUE_TYPE=long)\n"
                                             "target_include_directories(app PRIVATE \"" +
                                             (tmp.dir / "sdk/include").generic_string() + "\")\nenable_testing()\nadd_test(NAME answer COMMAND app)\n";
            auto created = line("{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + json::write(initial.version) +
                                ",\"operation_id\":\"create\",\"changes\":[{\"kind\":\"create\",\"path\":\"main.cpp\",\"text\":" + json::write(before) +
                                "},{\"kind\":\"create\",\"path\":\"CMakeLists.txt\",\"text\":" + json::write(cmake_source) +
                                "},{\"kind\":\"create\",\"path\":\"shared.hpp\",\"text\":" + json::write(shared_header) +
                                "},{\"kind\":\"create\",\"path\":\"other.cpp\",\"text\":" + json::write(other_source) + "}]}");
            test::tool(port, "task", R"({"project":"app","action":"create","task":"fix","worldline":"main","operation_id":"bind"})");
            auto execution_run =
                test::decode<tools::task::output>(test::tool(port, "task", R"({"project":"app","action":"start","task":"fix","operation_id":"start"})"));
            credential = *execution_run.credential;
            auto read = line(R"({"project":"app","action":"read","path":"main.cpp"})");
            expect(read.text == before);
            auto waiting = test::decode<tools::language_query::output>(
                agent("language", R"({"project":"app","configuration":"cpp","action":"symbols","budget":{"units":0}})"));
            test::require(waiting.status == "partial" && waiting.view && waiting.continuation, "zero unit budget publishes pending coverage");
            const auto execute_cache = std::filesystem::path{worker_config.work_root + "-execute"} / "analysis";
            test::require(std::filesystem::is_directory(execute_cache) &&
                              std::ranges::any_of(std::filesystem::recursive_directory_iterator{execute_cache},
                                                  [](const auto& item) { return item.is_directory() && item.path().filename() == "clice"; }),
                          "clice with compiler discovery must run in the execute role");
            auto deferred_parts = [](const auto& result, std::string_view path = {}) {
                std::size_t count = 0;
                for (const auto& missing : result.coverage.missing)
                    if (missing.deferred && (path.empty() || std::ranges::contains(missing.scope.files, path)))
                        count += missing.units.size();
                return count;
            };
            const auto total_parts = deferred_parts(waiting);
            const auto main_parts = deferred_parts(waiting, "main.cpp");
            const auto other_parts = total_parts - main_parts;
            test::require(waiting.items.empty() && main_parts > 1 && other_parts > 1,
                          "both TUs must expose independently deferred semantic contributions: " + json::write(waiting));
            auto batch = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"continuation\":" + json::write(waiting.continuation) + ",\"budget\":{\"units\":1}}"));
            test::require(batch.status == "partial" && batch.continuation, "one unit publishes results and the remaining work");
            expect(!batch.items.empty() && deferred_parts(batch) == total_parts - 1);
            auto completed_file = test::decode<tools::language_query::output>(agent(
                "language", "{\"project\":\"app\",\"action\":\"symbols\",\"scope\":{\"files\":[\"main.cpp\"]},\"view\":" + json::write(batch.view) + "}"));
            expect(completed_file.status == "partial" && completed_file.continuation)
                << "a declaration part cannot hide deferred source-location and scope contributions";
            auto completed_batch = batch;
            std::size_t rounds = 1;
            while (completed_batch.status == "partial") {
                test::require(completed_batch.continuation.has_value() && rounds < static_cast<std::size_t>(total_parts),
                              "contribution continuation did not advance");
                completed_batch = test::decode<tools::language_query::output>(
                    agent("language", "{\"project\":\"app\",\"continuation\":" + json::write(completed_batch.continuation) + "}"));
                ++rounds;
            }
            expect(rounds == static_cast<std::size_t>(total_parts) && completed_batch.reused_units == static_cast<std::uint64_t>(total_parts - 1) &&
                   completed_batch.next_view == completed_batch.view && completed_batch.version == waiting.version && completed_batch.view != batch.view)
                << "the same one-contribution budget advances past every retained part";
            auto focused = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"configuration\":\"cpp\",\"action\":\"symbols\",\"budget\":{\"units\":" + std::to_string(main_parts) +
                                      "},\"scope\":{\"files\":[\"main.cpp\"]}}"));
            expect(focused.status == "complete" && focused.view && !focused.items.empty());
            auto outside = test::decode<tools::language_query::output>(
                agent("language",
                      "{\"project\":\"app\",\"action\":\"symbols\",\"scope\":{\"files\":[\"other.cpp\"]},\"view\":" + json::write(focused.view) + "}"));
            test::require(outside.status == "partial" && outside.continuation && !outside.coverage.missing.empty(), "outside scope offers continuation");
            auto supplemented = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"continuation\":" + json::write(outside.continuation) +
                                      ",\"budget\":{\"units\":" + std::to_string(total_parts) + "}}"));
            expect(supplemented.status == "complete" && supplemented.next_view == supplemented.view && supplemented.view != focused.view &&
                   supplemented.version == focused.version && supplemented.environment == focused.environment && !supplemented.items.empty());
            auto retained = test::decode<tools::language_query::output>(agent(
                "language",
                "{\"project\":\"app\",\"action\":\"symbols\",\"scope\":{\"files\":[\"main.cpp\"]},\"view\":" + json::write(supplemented.view) + "}"));
            expect(retained.status == "complete" && json::write(retained.items) == json::write(focused.items))
                << "supplementing retains previously published units";
            auto original = test::decode<tools::language_query::output>(
                agent("language",
                      "{\"project\":\"app\",\"action\":\"symbols\",\"scope\":{\"files\":[\"other.cpp\"]},\"view\":" + json::write(focused.view) + "}"));
            expect(original.status == "partial") << "continuation leaves the original view unchanged";
            auto incomplete_project = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"action\":\"symbols\",\"view\":" + json::write(focused.view) + "}"));
            test::require(incomplete_project.continuation.has_value(), "project expansion offers continuation");
            auto symbols = agent("language", "{\"project\":\"app\",\"configuration\":\"cpp\",\"action\":\"symbols\",\"limit\":250,\"budget\":{\"units\":" +
                                                 std::to_string(total_parts) + "}}");
            expect(symbols.contains("twice") && symbols.contains(R"("analyzed":true)")) << symbols;
            auto published = test::decode<tools::language_query::output>(symbols);
            expect(published.reused_units == static_cast<std::uint64_t>(total_parts)) << "expanding to the project reuses both committed TUs";
            test::require(published.view.has_value(), "analysis did not publish a view");
            const auto expired_symbol = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"action\":\"inspect\",\"selector\":{\"entity\":{\"symbol_id\":\"sym-unretained\"}},\"view\":" +
                                      json::write(published.view) + "}"));
            expect(expired_symbol.status == "expired" && expired_symbol.code == "symbol_expired");
            const auto source_directory = last_directory();
            expect(!std::filesystem::exists(source_directory / "build/native/app")) << "source analysis does not link an executable";
            auto emitted = test::decode<tools::language_query::output>(agent(
                "language", R"({"project":"app","configuration":"artifact","action":"relations","selector":{"name":{"text":"twice"}},"relations":["emits"]})"));
            test::require(emitted.relations.size() == 1, "debug information did not connect source to native artifact");
            const auto& emission = emitted.relations.front();
            expect(emission.detail && std::get<semantic::records::emission_detail>(emission.detail->variant()).evidence == "debug_location");
            expect(emission.from_context.has_value() && *emission.from_context != emission.ownership.context);
            expect(std::ranges::any_of(emitted.items, [](const auto& item) { return item.domain == "native" && item.name == "_Z5twicei"; }));
            expect(last_directory().string().starts_with(artifact_config.work_root) && source_directory.string().starts_with(worker_config.work_root) &&
                   std::filesystem::exists(last_directory() / "build/native/app"))
                << "source shards cross workers and directory groups before native source mapping";
            auto deferred_artifact = test::decode<tools::language_query::output>(agent(
                "language",
                R"({"project":"app","configuration":"artifact","action":"relations","selector":{"name":{"text":"twice"}},"relations":["emits"],"scope":{"files":["main.cpp"]},"budget":{"units":0}})"));
            test::require(deferred_artifact.status == "partial" && deferred_artifact.continuation,
                          "zero budget preserves the source and artifact work for continuation");
            expect(deferred_artifact.coverage.requested.files == std::vector<std::string>{"main.cpp"})
                << "upstream project demand does not erase the caller's file scope";
            auto reused_artifact = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"continuation\":" + json::write(deferred_artifact.continuation) +
                                      ",\"budget\":{\"units\":" + std::to_string(total_parts + 1) + "}}"));
            expect(reused_artifact.status == "complete" && reused_artifact.reused_units == static_cast<std::uint64_t>(total_parts + 1) &&
                   reused_artifact.relations.size() == 1u && test::slurp(artifact_reads) == "native\n")
                << "continuation reuses both source units and the native unit without invoking LLVM again: " << reused_artifact.status
                << " reused=" << reused_artifact.reused_units << " relations=" << reused_artifact.relations.size() << " reads=" << test::slurp(artifact_reads)
                << " missing=" << json::write(reused_artifact.coverage.missing);
            auto restart_artifact_worker = [&] {
                test::require(artifact_worker->stop() == 0, "artifact worker did not stop");
                artifact_config.name += "-restarted";
                test::spit(tmp.dir / "artifact-worker.json", json::write(artifact_config));
                artifact_worker = std::make_unique<test::child>(test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {"artifact-worker.json"}));
                test::eventually(
                    [&] {
                        test::require(!artifact_worker->finished(), artifact_worker->output());
                        const auto workers = test::request(port, http::method::GET, "/api/v1/workers");
                        return workers.status == 200 && workers.body.contains(artifact_config.name);
                    },
                    std::chrono::seconds{60});
            };
            auto artifact_relations = [&] {
                return test::decode<tools::language_query::output>(
                    agent("language",
                          R"({"project":"app","configuration":"artifact","action":"relations","selector":{"name":{"text":"twice"}},"relations":["emits"]})"));
            };
            artifact_config.tools["unused"] = reader.string();
            restart_artifact_worker();
            const auto after_unrelated_tool = artifact_relations();
            expect(after_unrelated_tool.status == "complete" && after_unrelated_tool.reused_units == static_cast<std::uint64_t>(total_parts + 1) &&
                   after_unrelated_tool.relations.size() == 1u && test::slurp(artifact_reads) == "native\n")
                << "an unrelated tool change preserves the native unit across worker restart: " << after_unrelated_tool.status
                << " reused=" << after_unrelated_tool.reused_units << " reads=" << test::slurp(artifact_reads);
            test::spit(reader, test::slurp(reader) + "# reader installation changed\n");
            restart_artifact_worker();
            const auto after_reader_change = artifact_relations();
            expect(after_reader_change.status == "complete" && after_reader_change.reused_units == static_cast<std::uint64_t>(total_parts) &&
                   after_reader_change.relations.size() == 1u && test::slurp(artifact_reads) == "native\nnative\n")
                << "changing the reader reruns native extraction while reusing source units: " << after_reader_change.status
                << " reused=" << after_reader_change.reused_units << " reads=" << test::slurp(artifact_reads);
            tools::language_query::input reverse_emission{.project = "app",
                                                          .action = std::string{"relations"},
                                                          .selector = semantic::selection{.entity = semantic::entity{emission.to, emission.ownership.context}},
                                                          .view = emitted.view,
                                                          .relations = std::vector<semantic::relation_kind>{"emits"},
                                                          .direction = std::string{"incoming"}};
            const auto back = test::decode<tools::language_query::output>(agent("language", json::write(reverse_emission)));
            expect(back.relations.size() == 1u && std::ranges::any_of(back.items, [](const auto& item) { return item.name == "twice"; }));
            auto plan = test::decode<tools::language_query::output>(
                agent("language", R"({"project":"app","configuration":"paired","action":"plan","outputs":["primary"]})"));
            expect(plan.plan && plan.plan->stages == std::vector<std::string>{"main"});
            expect(plan.plan->directory_groups.size() == 1u && plan.plan->directory_groups.contains("workspace"));
            const auto before_preparation = test::slurp(preparations);
            auto combined_batch = test::decode<tools::language_query::output>(agent(
                "language",
                R"({"project":"app","configuration":"paired","action":"symbols","outputs":["combined"],"limit":2,"budget":{"units":1},"selector":{"name":{"text":"twice"}}})"));
            test::require(combined_batch.status == "partial" && combined_batch.continuation, "a graph preserves each stage's remaining work");
            expect(test::slurp(preparations) == before_preparation + "configure\nconfigure\n")
                << "one directory prepares each configuration's same-named task once";
            auto combined = combined_batch;
            std::size_t graph_rounds = 0;
            while (combined.status == "partial") {
                test::require(combined.continuation.has_value() && graph_rounds < static_cast<std::size_t>(3 * total_parts),
                              "stage contribution continuation did not advance");
                combined = test::decode<tools::language_query::output>(
                    agent("language", "{\"project\":\"app\",\"continuation\":" + json::write(combined.continuation) + "}"));
                ++graph_rounds;
            }
            test::require(combined.status == "complete" && combined.items.size() == 2,
                          "profile outputs must preserve both stage interpretations: " + json::write(combined));
            expect(combined.items[0].symbol_id == combined.items[1].symbol_id && combined.items[0].context != combined.items[1].context);
            expect(combined.snapshots.size() == 1u && combined.version == published.version);
            expect(test::slurp(preparations) == before_preparation + "configure\nconfigure\n")
                << "a continuation on the same snapshot reuses the preparation while retaining completed contributions";
            const auto workspace_directory = last_directory();
            const auto before_isolated = test::slurp(preparations);
            auto separate = test::decode<tools::language_query::output>(agent(
                "language",
                R"({"project":"app","configuration":"isolated","action":"symbols","outputs":["combined"],"limit":2,"selector":{"name":{"text":"twice"}}})"));
            expect(separate.status == "complete" && separate.items.size() == 2u && test::slurp(preparations) == before_isolated + "configure\n")
                << "only the stage assigned to a separate directory prepares; the shared directory is already prepared";
            tools::language_query::input default_page{.project = "app",
                                                      .action = std::string{"symbols"},
                                                      .selector = semantic::selection{.name = semantic::named{.text = "twice"}},
                                                      .view = combined.view};
            auto first_page = test::decode<tools::language_query::output>(agent("language", json::write(default_page)));
            test::require(first_page.items.size() == 1 && first_page.cursor.has_value(), "fixed view must retain its profile page size");
            default_page.cursor = first_page.cursor;
            auto second_page = test::decode<tools::language_query::output>(agent("language", json::write(default_page)));
            expect(second_page.items.size() == 1u && second_page.items.front().context != first_page.items.front().context);

            const auto main_symbol = std::ranges::find(published.items, "main", &semantic::item::name);
            test::require(main_symbol != published.items.end(), "the main TU has no entry symbol");
            const auto main_context = main_symbol->context;
            std::vector<semantic::item> variants;
            auto header_view = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"configuration\":\"cpp\",\"action\":\"symbols\",\"limit\":250,\"budget\":{\"units\":" +
                                      std::to_string(total_parts) + "},\"scope\":{\"files\":[\"shared.hpp\"]}}"));
            expect(header_view.status == "complete" && header_view.reused_units == static_cast<std::uint64_t>(total_parts));
            for (const auto& item : header_view.items)
                if (item.name == "selected_value")
                    variants.push_back(item);
            test::require(variants.size() == 2, "the shared header must retain exactly its two TU interpretations");
            expect(variants[0].symbol_id == variants[1].symbol_id && variants[0].context != variants[1].context);
            expect(variants[0].type_id != variants[1].type_id);
            tools::language_query::input header_query{.project = "app", .action = std::string{"inspect"}};
            header_query.view = published.view;
            header_query.selector = semantic::selection{.entity = semantic::entity{.symbol_id = variants.front().symbol_id}};
            auto ambiguous = test::decode<tools::language_query::output>(agent("language", json::write(header_query)));
            expect(ambiguous.status == "ambiguous" && ambiguous.items.size() == 2u);
            for (const auto& variant : variants) {
                header_query.selector.value()->entity->context = variant.context;
                auto selected = test::decode<tools::language_query::output>(agent("language", json::write(header_query)));
                test::require(selected.status == "complete" && selected.items.size() == 1, "context did not select a complete header interpretation");
                auto alias = std::ranges::find(selected.types, *selected.items.front().type_id, &semantic::records::type::id);
                test::require(alias != selected.types.end() && alias->kind == "alias" && alias->arguments.size() == 1, "header alias type missing");
                auto primitive = std::ranges::find(selected.types, alias->arguments.front(), &semantic::records::type::id);
                expect(primitive != selected.types.end() && primitive->name == (variant.context == main_context ? "int" : "long"));
            }
            tools::language_query::input packet_query{
                .project = "app", .action = std::string{"inspect"}, .selector = semantic::selection{.name = semantic::named{"packet"}}, .view = published.view};
            auto packet = test::decode<tools::language_query::output>(agent("language", json::write(packet_query)));
            test::require(packet.status == "complete" && packet.items.size() == 1 && !packet.types.empty(), "template instance missing");
            auto bounded_packet = packet_query;
            bounded_packet.budget = semantic::budget{.nodes = 0};
            auto partial_packet = test::decode<tools::language_query::output>(agent("language", json::write(bounded_packet)));
            test::require(partial_packet.status == "partial" && partial_packet.continuation, "type expansion offers a query continuation");
            auto continued_packet = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"continuation\":" + json::write(partial_packet.continuation) + ",\"budget\":{\"nodes\":1000}}"));
            expect(continued_packet.status == "complete" && continued_packet.view == packet.view && !continued_packet.next_view &&
                   json::write(continued_packet.types) == json::write(packet.types))
                << "query budget continuation expands the existing immutable view";
            const auto& arguments = packet.types.front().application_arguments;
            test::require(arguments.size() == 5 && arguments[4].kind == "pack" && arguments[4].elements.size() == 2,
                          "ordered template arguments and parameter pack missing");
            expect(arguments[2].kind == "integral" && arguments[2].value == "7");
            expect(arguments[0].kind == "template" && arguments[3].kind == "null_pointer" && arguments[3].value == "nullptr");
            auto type_name = [&](const auto& argument) {
                test::require(argument.type.has_value(), "template argument type missing");
                auto type = std::ranges::find(packet.types, *argument.type, &semantic::records::type::id);
                test::require(type != packet.types.end(), "template argument type was not expanded");
                return type->name;
            };
            expect(type_name(arguments[2]) == "int" && type_name(arguments[4].elements[0]) == "long" && type_name(arguments[4].elements[1]) == "short");
            test::require(arguments[1].type.has_value() && arguments[3].type == arguments[1].type, "pointer argument type missing");
            auto pointer = std::ranges::find(packet.types, *arguments[1].type, &semantic::records::type::id);
            expect(pointer != packet.types.end() && pointer->kind == "pointer" && pointer->arguments == std::vector{*arguments[2].type});
            for (std::size_t i = 0; i < 2; ++i) {
                test::require(arguments[i].declaration.has_value(), "declaration argument missing");
                packet_query.selector = semantic::selection{.entity = semantic::entity{*arguments[i].declaration, packet.items.front().context}};
                auto target = test::decode<tools::language_query::output>(agent("language", json::write(packet_query)));
                expect(target.items.size() == 1u && target.items.front().name == (i == 0 ? "Tag" : "marker"));
            }
            auto generic = [&](std::string name, std::optional<std::string> kind = {}) {
                tools::language_query::input query{.project = "app",
                                                   .action = std::string{"inspect"},
                                                   .selector = semantic::selection{.name = semantic::named{.text = name, .kind = kind}},
                                                   .view = published.view};
                auto result = test::decode<tools::language_query::output>(agent("language", json::write(query)));
                test::require(result.status == "complete" && result.items.size() == 1 && !result.types.empty(),
                              "generic declaration missing: " + name + " (" + result.status + ", " + std::to_string(result.items.size()) + " candidates)");
                return result;
            };
            auto type_node = [&](const auto& result, const auto& id) -> const semantic::records::type& {
                auto found = std::ranges::find(result.types, id, &semantic::records::type::id);
                test::require(found != result.types.end(), "generic type node missing");
                return *found;
            };
            auto first_template = generic("first"), second_template = generic("second");
            const auto& first_signature = first_template.types.front();
            const auto& second_signature = second_template.types.front();
            test::require(first_signature.type_parameters.size() == 1 && second_signature.type_parameters.size() == 1, "function template parameters missing");
            const auto& first_parameter = type_node(first_template, first_signature.type_parameters.front());
            const auto& second_parameter = type_node(second_template, second_signature.type_parameters.front());
            expect(first_parameter.binder == first_template.items.front().symbol_id && second_parameter.binder == second_template.items.front().symbol_id);
            expect(first_parameter.id != second_parameter.id && first_parameter.parameter_index == 0u && second_parameter.parameter_index == 0u);
            expect(first_signature.return_type == first_parameter.id && first_signature.parameter_groups.front().parameters.front().type == first_parameter.id);
            auto outer = generic("Outer"), combine = generic("combine"), unused = generic("unused");
            const auto& method = combine.types.front();
            test::require(method.type_parameters.size() == 1 && method.return_type && method.parameter_groups.front().parameters.size() == 2,
                          "nested template signature missing");
            const auto& outer_parameter = type_node(combine, *method.return_type);
            const auto& inner_parameter = type_node(combine, method.type_parameters.front());
            expect(outer_parameter.binder == outer.items.front().symbol_id && inner_parameter.binder == combine.items.front().symbol_id);
            expect(method.parameter_groups.front().parameters[0].type == outer_parameter.id &&
                   method.parameter_groups.front().parameters[1].type == inner_parameter.id);
            test::require(unused.types.front().type_parameters.size() == 1, "unused template parameter missing from signature");
            expect(type_node(unused, unused.types.front().type_parameters.front()).binder == unused.items.front().symbol_id);
            auto packet_template = generic("Packet", "type");
            test::require(packet_template.types.front().type_parameters.size() == 5, "mixed template head missing");
            for (std::size_t i = 0; i < 5; ++i) {
                const auto& parameter = type_node(packet_template, packet_template.types.front().type_parameters[i]);
                expect(parameter.binder == packet_template.items.front().symbol_id && parameter.parameter_index == i);
            }
            const auto& template_parameter = type_node(packet_template, packet_template.types.front().type_parameters.front());
            test::require(template_parameter.type_parameters.size() == 1, "template-template parameter head missing");
            expect(type_node(packet_template, template_parameter.type_parameters.front()).binder == template_parameter.declaration);
            std::vector<semantic::item> choices;
            for (const auto& item : published.items)
                if (item.name == "choose")
                    choices.push_back(item);
            test::require(choices.size() == 2, "overload declarations missing");
            std::string candidate_reference;
            std::size_t resolved_calls = 0;
            for (const auto& choice : choices) {
                tools::language_query::input query{.project = "app",
                                                   .action = std::string{"references"},
                                                   .selector = semantic::selection{.entity = semantic::entity{choice.symbol_id, choice.context}},
                                                   .view = published.view};
                auto uses = test::decode<tools::language_query::output>(agent("language", json::write(query)));
                auto candidate = std::ranges::find_if(uses.references, [](const auto& use) { return use.resolution == "ambiguous"; });
                test::require(candidate != uses.references.end(), "dependent overload candidates missing");
                expect(!candidate->target && candidate->candidates.size() == 2u && !candidate->dispatch &&
                       candidate->roles == std::vector<semantic::role>{std::string{"call"}});
                expect(std::ranges::contains(candidate->candidates, choices[0].symbol_id) &&
                       std::ranges::contains(candidate->candidates, choices[1].symbol_id));
                if (candidate_reference.empty())
                    candidate_reference = candidate->id;
                else
                    expect(candidate_reference == candidate->id);
                for (const auto& use : uses.references)
                    if (use.resolution == "resolved" && use.dispatch == "direct")
                        ++resolved_calls;
            }
            expect(resolved_calls == 2u);
            tools::language_query::input reference_query{.project = "app",
                                                         .action = std::string{"references"},
                                                         .scope = tools::language_query::scope_filter{.files = std::vector<std::string>{"other.cpp"}},
                                                         .limit = 250,
                                                         .view = published.view};
            auto all_uses = test::decode<tools::language_query::output>(agent("language", json::write(reference_query)));
            auto one_candidate = generic("one_candidate"), unbound = generic("unbound"), calls = generic("calls");
            auto use_from = [&](const auto& function) -> const semantic::records::reference& {
                auto found = std::ranges::find_if(all_uses.references, [&](const auto& use) {
                    return use.from == function.items.front().symbol_id && std::ranges::contains(use.roles, std::string{"call"});
                });
                test::require(found != all_uses.references.end(), "unresolved call missing");
                return *found;
            };
            const auto& single = use_from(one_candidate);
            const auto& unknown = use_from(unbound);
            expect(single.resolution == "unresolved" && !single.target && single.candidates.size() == 1u && !single.dispatch);
            expect(unknown.resolution == "unresolved" && !unknown.target && unknown.candidates.empty());
            std::set<std::string> dispatches;
            for (const auto& use : all_uses.references)
                if (use.from == calls.items.front().symbol_id && use.dispatch)
                    dispatches.insert(use.dispatch->str());
            expect(dispatches == std::set<std::string>{"direct", "indirect", "virtual"});
            tools::language_query::input scope_query{.project = "app",
                                                     .action = std::string{"scopes"},
                                                     .scope = tools::language_query::scope_filter{.files = std::vector<std::string>{"other.cpp"}},
                                                     .view = published.view};
            auto scope_result = test::decode<tools::language_query::output>(agent("language", json::write(scope_query)));
            for (auto cursor = scope_result.cursor; cursor;) {
                scope_query.cursor = cursor;
                auto page = test::decode<tools::language_query::output>(agent("language", json::write(scope_query)));
                scope_result.scopes.insert(scope_result.scopes.end(), page.scopes.begin(), page.scopes.end());
                cursor = std::move(page.cursor);
            }
            auto entity = [&](std::string_view name, std::string_view kind = "") -> std::string {
                auto found =
                    std::ranges::find_if(published.items, [&](const auto& item) { return item.name == name && (kind.empty() || item.kind == kind); });
                test::require(found != published.items.end(), "scope declaration missing");
                return found->symbol_id;
            };
            auto scopes_for = [&](const std::string& owner) {
                std::vector<const semantic::records::scope*> scopes;
                for (const auto& scope : scope_result.scopes)
                    if (scope.semantic_owner == owner)
                        scopes.push_back(&scope);
                return scopes;
            };
            auto edge = [](const auto& scope, std::string_view kind, const std::string& target) {
                return std::ranges::any_of(scope.lookup, [&](const auto& edge) { return edge.kind == kind && edge.target == target; });
            };
            auto facade = scopes_for(entity("facade"));
            test::require(facade.size() == 2,
                          std::format("expected two reopened namespace scopes, received {} of {} scopes", facade.size(), scope_result.scopes.size()));
            expect(facade[0]->id != facade[1]->id && edge(*facade[0], "using_namespace", entity("library")) && facade[1]->lookup.empty());
            std::size_t imported = 0;
            for (const auto& choice : published.items)
                if (choice.name == "exported" && edge(*facade[0], "using_declaration", choice.symbol_id))
                    ++imported;
            expect(imported == 2u);
            auto derived = scopes_for(entity("Derived"));
            test::require(derived.size() == 1, "derived scope missing");
            expect(edge(*derived.front(), "base", entity("Base")) && edge(*derived.front(), "using_declaration", entity("inherited", "field")));
            auto library = scopes_for(entity("library"));
            test::require(library.size() == 1, "library scope missing");
            expect(edge(*library.front(), "inline_namespace", entity("extension")) && edge(*library.front(), "unscoped_enum", entity("Choice")));
            auto lexical = scopes_for(entity("lexical"));
            test::require(lexical.size() == 7, "function and nested block scopes missing");
            expect(lexical[1]->lexical_parent == lexical[0]->id && lexical[2]->lexical_parent == lexical[1]->id &&
                   lexical[3]->lexical_parent == lexical[1]->id);
            expect(lexical[1]->declared_members.size() == 1u && lexical[2]->declared_members.size() == 1u && lexical[3]->declared_members.size() == 1u &&
                   lexical[1]->declared_members != lexical[2]->declared_members && lexical[2]->declared_members != lexical[3]->declared_members);
            expect(edge(*lexical[2], "using_namespace", entity("library")) && lexical[1]->lookup.empty() && lexical[3]->lookup.empty());
            expect(lexical[4]->lexical_parent == lexical[1]->id && lexical[5]->lexical_parent == lexical[4]->id &&
                   lexical[6]->lexical_parent == lexical[4]->id && lexical[5]->declared_members != lexical[6]->declared_members);
            auto method_scopes = scopes_for(entity("method"));
            test::require(method_scopes.size() == 3, "out-of-line method scopes missing");
            expect(method_scopes[0]->lexical_parent != method_scopes[1]->lexical_parent && method_scopes[2]->lexical_parent == method_scopes[1]->id);
            tools::language_query::input role_query{.project = "app",
                                                    .action = std::string{"references"},
                                                    .selector = semantic::selection{.name = semantic::named{"observed"}},
                                                    .view = published.view};
            auto observed = test::decode<tools::language_query::output>(agent("language", json::write(role_query)));
            std::map<std::vector<semantic::role>, std::size_t> roles;
            for (const auto& use : observed.references)
                ++roles[use.roles];
            expect(roles[std::vector<semantic::role>{std::string{"write"}}] == 1u &&
                   roles[std::vector<semantic::role>{std::string{"read"}, std::string{"write"}}] == 3u &&
                   roles[std::vector<semantic::role>{std::string{"read"}}] == 1u);
            tools::language_query::input relation_query{.project = "app",
                                                        .configuration = "cpp",
                                                        .action = std::string{"relations"},
                                                        .selector = semantic::selection{.name = semantic::named{"BaseAgain"}},
                                                        .limit = 1,
                                                        .relations = std::vector<semantic::relation_kind>{"aliases"},
                                                        .depth = 2};
            auto alias_first = test::decode<tools::language_query::output>(agent("language", json::write(relation_query)));
            test::require(alias_first.analyzed && alias_first.view.has_value() && alias_first.relations.size() == 1 && alias_first.cursor.has_value(),
                          "fresh alias graph did not publish a paginated view");
            relation_query.view = alias_first.view;
            relation_query.cursor = alias_first.cursor;
            auto alias_second = test::decode<tools::language_query::output>(agent("language", json::write(relation_query)));
            test::require(alias_second.relations.size() == 1 && !alias_second.cursor, "alias graph continuation missing");
            std::set<std::pair<std::string, std::string>> aliases{{alias_first.relations[0].from, alias_first.relations[0].to},
                                                                  {alias_second.relations[0].from, alias_second.relations[0].to}};
            expect(aliases.contains({entity("BaseAgain"), entity("BaseAlias")}) && aliases.contains({entity("BaseAlias"), entity("Base")}));
            expect(alias_first.references.size() == 1u && alias_first.references.front().roles == std::vector<semantic::role>{std::string{"type"}} &&
                   alias_first.relations.front().reference == alias_first.references.front().id);
            relation_query.cursor.reset();
            relation_query.selector = semantic::selection{.name = semantic::named{"Override::invoke"}};
            relation_query.relations = std::vector<semantic::relation_kind>{"overrides"};
            auto overrides = test::decode<tools::language_query::output>(agent("language", json::write(relation_query)));
            test::require(overrides.relations.size() == 1, "override relationship missing");
            const auto& overridden = overrides.relations.front();
            expect(overridden.occurrence.has_value() &&
                   std::ranges::contains(overrides.occurrences, *overridden.occurrence, &semantic::records::occurrence::id));
            relation_query.selector = semantic::selection{.name = semantic::named{"deferred"}};
            relation_query.relations = std::vector<semantic::relation_kind>{"calls"};
            relation_query.limit = 50;
            auto candidates = test::decode<tools::language_query::output>(agent("language", json::write(relation_query)));
            test::require(candidates.relations.size() == 2 && candidates.references.size() == 1, "candidate call graph missing");
            expect(candidates.relations[0].resolution == "candidate" && candidates.relations[1].resolution == "candidate" &&
                   candidates.relations[0].reference == candidates.relations[1].reference && candidates.items.size() == 3u);
            tools::language_query::input declaration_query{.project = "app",
                                                           .action = std::string{"inspect"},
                                                           .selector = semantic::selection{.name = semantic::named{"Widget::score"}},
                                                           .view = published.view};
            auto score = test::decode<tools::language_query::output>(agent("language", json::write(declaration_query)));
            test::require(score.items.size() == 1, "method selection did not resolve to one declaration");
            const auto& score_item = score.items.front();
            const auto declaration = std::ranges::find_if(score.symbols, [&](const auto& symbol) {
                return symbol.id == score_item.symbol_id && symbol.ownership.context == score_item.context;
            });
            test::require(declaration != score.symbols.end() && declaration->parameters.size() == 1, "method declaration details missing");
            const auto& score_record = *declaration;
            const auto& parameter = score_record.parameters.front();
            expect(score_record.visibility == "public" && std::ranges::contains(score_record.modifiers, "const") &&
                   score_record.extensions.contains("cpp.attributes"));
            expect(parameter.group == 0u && parameter.index == 0u && parameter.name == "adjustment" && parameter.default_value == "1 + 2" &&
                   parameter.annotations.contains("cpp.attributes"));
            declaration_query.selector = semantic::selection{.name = semantic::named{"Widget::cached"}};
            auto cached = test::decode<tools::language_query::output>(agent("language", json::write(declaration_query)));
            test::require(cached.symbols.size() == 1, "field declaration details missing");
            expect(cached.symbols.front().visibility == "protected" && std::ranges::contains(cached.symbols.front().modifiers, "mutable"));
            declaration_query.selector = semantic::selection{.name = semantic::named{"Widget::secret"}};
            auto secret = test::decode<tools::language_query::output>(agent("language", json::write(declaration_query)));
            test::require(secret.symbols.size() == 1, "static member declaration details missing");
            expect(secret.symbols.front().visibility == "private" && std::ranges::contains(secret.symbols.front().modifiers, "static"));
            relation_query.depth = 1;
            relation_query.include = std::vector<std::string>{"declarations"};
            for (const auto& name : {"direct", "listed", "defaulted", "copied", "convert"}) {
                relation_query.selector = semantic::selection{.name = semantic::named{name}};
                auto construction = test::decode<tools::language_query::output>(agent("language", json::write(relation_query)));
                const auto constructor = std::ranges::find_if(construction.relations, [&](const auto& relation) {
                    return std::ranges::any_of(construction.items, [&](const auto& item) {
                        return item.symbol_id == relation.to && item.kind == "constructor" && (item.name == "Widget" || item.name == "Converted");
                    });
                });
                test::require(constructor != construction.relations.end() && constructor->reference.has_value(), "constructor call source missing");
                auto source = std::ranges::find(construction.references, *constructor->reference, &semantic::records::reference::id);
                test::require(source != construction.references.end(), "constructor source reference missing");
                expect(source->from == constructor->from && source->target == constructor->to && source->dispatch == "direct" &&
                       std::ranges::contains(source->roles, semantic::role::make<"call">()) &&
                       construction.relations.size() == (std::string_view{name} == "convert" ? 2u : 1u));
                if (std::string_view{name} == "defaulted" || std::string_view{name} == "convert")
                    expect(std::ranges::contains(source->roles, semantic::role::make<"implicit">()));
                if (std::string_view{name} == "direct") {
                    auto target = std::ranges::find(construction.symbols, constructor->to, &semantic::records::symbol::id);
                    test::require(target != construction.symbols.end() && target->parameters.size() == 1, "constructor declaration missing");
                    expect(std::ranges::contains(target->modifiers, "explicit") && target->parameters.front().name == "amount" &&
                           target->parameters.front().default_value == "4");
                }
            }
            auto sdk_symbol = std::ranges::find(published.items, "expected_answer", &semantic::item::name);
            test::require(sdk_symbol != published.items.end(), "SDK declaration missing from published symbols");
            tools::language_query::input sdk_query{.project = "app",
                                                   .action = std::string{"inspect"},
                                                   .selector = semantic::selection{.entity = semantic::entity{.symbol_id = sdk_symbol->symbol_id}},
                                                   .view = published.view};
            auto sdk_detail = test::decode<tools::language_query::output>(agent("language", json::write(sdk_query)));
            expect(std::ranges::any_of(sdk_detail.occurrences, [](const auto& occurrence) {
                const auto& location = occurrence.location.artifact;
                return location && !location->artifact.empty() && location->module == "sdk/native-test" && location->member == "include/answer.hpp";
            })) << "SDK declarations retain their object and member location in the fixed view";
            auto timed = test::decode<tools::language_query::output>(
                agent("language", R"({"project":"app","configuration":"cpp","action":"symbols","budget":{"milliseconds":0}})"));
            test::require(timed.status == "partial" && timed.continuation, "time exhaustion before provider initialization publishes a resumable view");
            auto limited =
                test::decode<tools::language_query::output>(agent("language", "{\"project\":\"app\",\"continuation\":" + json::write(timed.continuation) +
                                                                                  ",\"budget\":{\"nodes\":0,\"milliseconds\":60000}}"));
            test::require(limited.status == "partial" && limited.continuation && limited.items.empty(),
                          "a node budget applies to cached contributions and preserves pending work");
            auto replenished = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"continuation\":" + json::write(limited.continuation) + ",\"budget\":{\"nodes\":1000000}}"));
            expect(replenished.status == "complete" && replenished.version == timed.version && replenished.next_view == replenished.view)
                << "node and time budget continuation completes the pinned computation";
            auto twice = std::ranges::find(published.items, "twice", &semantic::item::name);
            test::require(twice != published.items.end(), "published symbol missing");
            auto symbol_id = twice->symbol_id;
            tools::language_query::input inspect{.project = "app", .action = std::string{"inspect"}};
            inspect.view = published.view;
            inspect.selector = semantic::selection{.entity = semantic::entity{.symbol_id = symbol_id}};
            auto detail = test::decode<tools::language_query::output>(agent("language", json::write(inspect)));
            expect(detail.view == published.view && !detail.analyzed && detail.status == "complete");
            test::require(detail.items.size() == 1 && detail.items.front().symbol_id == symbol_id, "inspect did not select the published symbol");
            expect(!detail.items.front().definitions.empty() && detail.items.front().definitions.front().position->line == 2u);
            test::require(detail.items.front().type_id.has_value(), "declaration has no structured type");
            auto signature = std::ranges::find(detail.types, *detail.items.front().type_id, &semantic::records::type::id);
            test::require(signature != detail.types.end() && signature->kind == "function", "function signature missing");
            test::require(signature->return_type.has_value() && signature->parameter_groups.size() == 1 &&
                              signature->parameter_groups.front().parameters.size() == 1,
                          "function signature is incomplete");
            auto integer = std::ranges::find(detail.types, *signature->return_type, &semantic::records::type::id);
            expect(integer != detail.types.end() && integer->kind == "primitive" && integer->name == "int");
            expect(signature->parameter_groups.front().parameters.front().type == *signature->return_type);
            expect(detail.items.front().source && detail.items.front().source->contains("return x * 3;"))
                << "inspect returns the definition's body text: " << json::write(detail.items.front().source);
            auto references = inspect;
            references.action = std::string{"references"};
            references.limit = 1;
            auto first_use = test::decode<tools::language_query::output>(agent("language", json::write(references)));
            test::require(first_use.references.size() == 1 && first_use.cursor.has_value(), "reference query did not paginate uses");
            expect(first_use.references.front().target == symbol_id);
            expect(first_use.references.front().location.position->line == 3u);
            references.cursor = first_use.cursor;
            auto scopes = test::decode<tools::language_query::output>(
                agent("language", R"({"project":"app","configuration":"cpp","action":"scopes","scope":{"files":["main.cpp"]}})"));
            test::require(!scopes.scopes.empty(), "no semantic scopes");
            const std::string replaced_expression = "return x * 3";
            const std::string replacement_expression = "int factor = 2; return x * factor";
            const auto replacement = before.find(replaced_expression);
            auto selected = std::ranges::find_if(scopes.scopes, [&](const auto& scope) {
                return scope.kind == "function" && scope.location.source && scope.location.source->begin <= replacement &&
                       scope.location.source->end > replacement;
            });
            test::require(selected != scopes.scopes.end(), "function scope missing");
            auto range = *selected->location.source;
            auto scoped_text = line("{\"project\":\"app\",\"action\":\"read\",\"path\":\"main.cpp\",\"version\":" + json::write(scopes.version) +
                                    ",\"offset\":" + std::to_string(range.begin) + ",\"count\":" + std::to_string(range.end - range.begin) + "}");
            expect(scoped_text.text == before.substr(range.begin, range.end - range.begin));
            tools::scope::input acquire{
                .project = "app", .action = std::string{"acquire"}, .scopes = std::vector<std::string>{*selected->reference}, .operation_id = "claim"};
            auto leased = test::decode<tools::scope::output>(agent("scope", json::write(acquire)));
            auto replay = test::decode<tools::scope::output>(agent("scope", json::write(acquire)));
            expect(leased.version == *created.version && replay.leases.at(0).token == leased.leases.at(0).token);
            expect(leased.leases.at(0).begin == range.begin && leased.leases.at(0).end == range.end);
            auto token = leased.leases.at(0).token;
            auto edit = [&](std::string worldline, std::string operation, bool held) {
                return "{\"project\":\"app\",\"action\":\"apply\",\"worldline\":" + json::write(worldline) +
                       ",\"observed\":" + json::write(created.version) + ",\"operation_id\":" + json::write(operation) +
                       (held ? ",\"leases\":[" + json::write(token) + "]" : "") +
                       ",\"changes\":[{\"kind\":\"edit\",\"path\":\"main.cpp\",\"offset\":" + std::to_string(replacement) +
                       ",\"count\":" + std::to_string(replaced_expression.size()) + ",\"text\":" + json::write(replacement_expression) + "}]}";
            };
            // A lease persists across a source restart and fences another caller's writes.
            expect(source->stop() == 0);
            source = start_source(tmp.dir, source_port);
            auto refused = test::tool(port, "worldline", edit("main", "unclaimed", false), true);
            expect(refused.contains("scope_conflict"));
            test::tool(port, "worldline", R"({"project":"app","action":"fork","name":"parallel","operation_id":"fork"})");
            auto independent = test::decode<tools::worldline::output>(test::tool(port, "worldline", edit("parallel", "parallel-fix", false)));
            expect(independent.version.has_value());
            // A disjoint edit in this file moves the function without invalidating its anchored lease.
            const std::string prefix = "// concurrent note\n";
            line("{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + json::write(created.version) +
                 ",\"operation_id\":\"notes\",\"changes\":[{\"kind\":\"edit\",\"path\":\"main.cpp\",\"offset\":0,\"count\":0,\"text\":" +
                 json::write(prefix) + "}]}");
            auto edited = line(edit("main", "fix", true));
            auto reread = line("{\"project\":\"app\",\"action\":\"read\",\"path\":\"main.cpp\",\"version\":" + json::write(edited.version) +
                               ",\"known_digest\":" + json::write(read.digest) + "}");
            expect(reread.text == prefix + after);
            auto compared = line("{\"project\":\"app\",\"action\":\"compare\",\"observed\":" + json::write(created.version) +
                                 ",\"version\":" + json::write(edited.version) + "}");
            expect(compared.changes && compared.changes->size() == 1u && compared.files && compared.files->size() == 1u);
            expect(edited.files && edited.files->size() == 1u && edited.files->at(0).path == "main.cpp");
            auto unchanged = line("{\"project\":\"app\",\"action\":\"read\",\"path\":\"main.cpp\",\"known_digest\":" + json::write(reread.digest) + "}");
            expect(unchanged.unchanged.value_or(false) && !unchanged.text && !unchanged.data_base64);
            auto renewed = test::decode<tools::scope::output>(
                agent("scope", json::write(tools::scope::input{.project = "app", .action = std::string{"renew"}, .token = token})));
            expect(renewed.version == *edited.version && renewed.leases.at(0).begin == range.begin + prefix.size());
            agent("scope", json::write(tools::scope::input{.project = "app", .action = std::string{"release"}, .token = token}));
            expect(agent("worldline", edit("main", "expired-fix", true), true).contains("scope_expired"));
            auto historical = test::decode<tools::language_query::output>(agent("language", json::write(inspect)));
            expect(historical.version == created.version && historical.view == published.view);
            auto second_use = test::decode<tools::language_query::output>(agent("language", json::write(references)));
            test::require(second_use.references.size() == 1 && !second_use.cursor, "old view reference continuation failed");
            expect(second_use.view == published.view && second_use.references.front().id != first_use.references.front().id);
            expect(second_use.references.front().location.position->line == 3u);
            inspect.view.value().reset();
            inspect.configuration = "cpp";
            inspect.version = edited.version;
            auto updated = test::decode<tools::language_query::output>(agent("language", json::write(inspect)));
            expect(updated.items.size() == 1u && updated.items.front().symbol_id == symbol_id);
            expect(updated.reused_units == static_cast<std::uint64_t>(total_parts - 1))
                << "editing a body reuses declarations in that TU and every unchanged file-context contribution";
            expect(updated.view != published.view && updated.version == edited.version);
            const auto changed_artifact = test::decode<tools::language_query::output>(agent(
                "language", R"({"project":"app","configuration":"artifact","action":"relations","selector":{"name":{"text":"twice"}},"relations":["emits"]})"));
            expect(changed_artifact.version == edited.version && changed_artifact.relations.size() == 1u &&
                   test::slurp(artifact_reads) == "native\nnative\nnative\n")
                << "changed artifact and source shards replace the native contribution";
            const auto linked_without_debug = test::decode<tools::language_query::output>(agent(
                "language",
                R"({"project":"app","configuration":"artifact-without-debug","action":"relations","relations":["emits"],"direction":"outgoing","limit":250})"));
            std::set<std::string> linked_names;
            std::set<semantic::symbol_id> source_overloads;
            for (const auto& edge : linked_without_debug.relations) {
                test::require(edge.detail && std::get<semantic::records::emission_detail>(edge.detail->variant()).evidence == "linkage_name",
                              "the executable without debug info must use linkage evidence");
                for (const auto& item : linked_without_debug.items)
                    if (item.symbol_id == edge.to && item.domain == "native" && item.name.starts_with("_Z6choose")) {
                        linked_names.insert(item.name);
                        source_overloads.insert(edge.from);
                    }
            }
            expect(source_overloads.size() == 2u && linked_names == std::set<std::string>{"_Z6choosei", "_Z6choosel"})
                << config::write(linked_without_debug);
            references.view = updated.view;
            references.cursor.reset();
            auto moved_use = test::decode<tools::language_query::output>(agent("language", json::write(references)));
            expect(moved_use.references.size() == 1 && moved_use.references.front().location.position->line == 4u);
            tools::language_query::input alternating{.project = "app",
                                                     .worldline = "parallel",
                                                     .configuration = "paired",
                                                     .action = std::string{"symbols"},
                                                     .selector = semantic::selection{.name = semantic::named{.text = "twice"}},
                                                     .outputs = {{"combined"}},
                                                     .limit = 2};
            auto branch = test::decode<tools::language_query::output>(test::tool(port, "language", json::write(alternating)));
            expect(branch.status == "complete" && branch.version == independent.version && branch.reused_units > 0u && branch.items.size() == 2u &&
                   branch.snapshots.size() == 1u && branch.snapshots.front().worldline == "parallel");
            expect(last_directory() == workspace_directory && test::slurp(workspace_directory / "main.cpp") == after)
                << "a different worldline uses the existing profile slot and installs its source";
            alternating.worldline = "main";
            auto returned = test::decode<tools::language_query::output>(test::tool(port, "language", json::write(alternating)));
            expect(returned.status == "complete" && returned.version == edited.version && returned.items.size() == 2u && returned.snapshots.size() == 1u &&
                   returned.snapshots.front().worldline == "main");
            expect(last_directory() == workspace_directory && test::slurp(workspace_directory / "main.cpp") == prefix + after)
                << "returning to main synchronizes from the slot's actual installed source";
            auto pinned = "{\"project\":\"app\",\"configurations\":[\"native\"],\"version\":" + json::write(edited.version) + "}";
            struct build_output {
                std::vector<builds::exported_file> exports;
            };
            struct execution {
                bool ok;
                std::string revision;
                std::vector<build_output> results;
            };
            auto built = test::decode<execution>(agent("build", pinned));
            expect(built.ok && built.revision == *edited.version);
            auto tested = test::decode<execution>(agent("test", pinned));
            expect(tested.ok && tested.revision == *edited.version) << config::write(tested);
            test::require(tested.ok && !tested.results.at(0).exports.empty(), "test produced no artifact");
            auto exported = tested.results.at(0).exports.at(0);
            expect(exported.executable && exported.delivery_error.empty());
            auto chunk_args = [&](std::string project, std::uint64_t offset, std::uint64_t length) {
                return "{\"project\":" + json::write(project) + ",\"digest\":" + json::write(exported.digest) +
                       ",\"offset\":" + std::to_string(offset) + ",\"length\":" + std::to_string(length) + "}";
            };
            agent("artifact", chunk_args("app", 0, 0), true);
            agent("artifact", chunk_args("app", exported.size + 1, 1024), true);
            std::string binary;
            for (std::uint64_t offset = 0;;) {
                auto chunk = test::decode<tools::artifact::output>(agent("artifact", chunk_args("app", offset, 4096)));
                test::require(chunk.offset == offset && chunk.total == exported.size, "artifact range differs");
                auto bytes = tools::lines::unbase64(chunk.data_base64);
                test::require(chunk.next_offset == offset + bytes.size(), "artifact did not advance");
                binary += bytes;
                if (chunk.eof)
                    break;
                offset = chunk.next_offset;
            }
            expect(binary.size() == exported.size);
            auto local = tmp.dir / "downloaded-app";
            test::spit(local, binary);
            std::filesystem::permissions(local, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
            test::child deployed{test::command(local, tmp.dir)};
            expect(deployed.wait() == 0) << "the retrieved executable runs locally";
            expect(!std::filesystem::exists(tmp.dir / "source/.analysis"));
            project_settings.analysis.at("cpp").defaults.fact_limit = 1;
            auto changed_profile = test::tool(port, "project", R"({"project":"app"})");
            test::tool(port, "save_project",
                       "{\"project\":\"app\",\"body\":{\"revision\":" + std::to_string(test::decode<revision>(changed_profile).revision) +
                           ",\"value\":" + config::write(project_settings) + "}}");
            expect(artifact_worker->stop() == 0 && worker.stop() == 0 && gateway.stop() == 0);
            {
                semantic::store stored{std::filesystem::path{doc.server.storage} / ".semantic", {.bytes = 0}, [] {
                                           return std::chrono::system_clock::now() - std::chrono::hours{48};
                                       }};
                stored.renew(*waiting.view);
                const auto artifact_view = stored.load(*changed_artifact.view);
                const auto cpp_before = stored.load(*published.view);
                const auto cpp_after = stored.load(*updated.view);
                auto declaration_channel = [&](const auto& view) -> std::string {
                    for (const auto& piece : view->pieces) {
                        const auto& unit = piece->unit;
                        const auto key = json::read<language::cpp::contribution_key>(unit.recipe.unit.name);
                        if (key && key->translation_unit == "main.cpp" && key->contribution == "declarations") {
                            const auto channel = std::ranges::find(unit.output_channels, "declarations", &semantic::channel::name);
                            test::require(channel != unit.output_channels.end(), "C++ declaration channel missing");
                            return channel->value;
                        }
                    }
                    throw std::runtime_error("C++ main declaration contribution missing");
                };
                expect(declaration_channel(cpp_before) == declaration_channel(cpp_after))
                    << "body-only and source-position edits preserve the declaration channel across real clice TU recomputation";
                std::string declaration_shard;
                for (const auto& piece : cpp_before->pieces) {
                    const auto key = json::read<language::cpp::contribution_key>(piece->unit.recipe.unit.name);
                    if (key && key->translation_unit == "main.cpp" && key->contribution == "declarations")
                        declaration_shard = piece->id;
                }
                expect(!declaration_shard.empty() && std::ranges::any_of(cpp_after->pieces, [&](const auto& piece) { return piece->id == declaration_shard; }))
                    << "a real edit reuses the same immutable declaration shard within the recomputed TU";
                expect(std::ranges::any_of(cpp_after->pieces, [](const auto& piece) {
                    return std::ranges::any_of(piece->symbols(), [](const auto& symbol) {
                        return symbol.name == "expected_answer" && symbol.extensions.contains("cpp.constant_digest");
                    });
                })) << "clice's evaluated SDK constant reaches the persistent declaration input";
                expect(std::ranges::any_of(artifact_view->pieces, [](const auto& piece) {
                    return piece->unit.recipe.unit.provider == "native-artifact" &&
                           std::ranges::any_of(piece->unit.observed_dependencies,
                                               [](const auto& read) {
                                                   return read.kind == semantic::dependency_kind::enumeration && read.selection &&
                                                          std::ranges::contains(read.selection->files, "main.cpp");
                                               }) &&
                           std::ranges::any_of(piece->unit.observed_dependencies, [](const auto& read) {
                               return read.kind == semantic::dependency_kind::source && read.key == "main.cpp" && !read.value.empty();
                           });
                })) << "native source mapping persists both file membership and content reads";
                const auto pinned = stored.load(*incomplete_project.view);
                std::map<std::string, std::string> sources;
                for (const auto& piece : pinned->pieces) {
                    for (const auto& input : piece->unit.direct_inputs)
                        if (input.kind == semantic::input_kind::file)
                            sources[input.name] = input.object;
                    for (const auto& read : piece->unit.observed_dependencies)
                        if (read.kind == semantic::dependency_kind::source)
                            sources[read.key] = read.value;
                }
                const auto& unit = pinned->pieces.front()->unit;
                expect(std::ranges::any_of(unit.direct_inputs, [](const auto& input) {
                    return input.kind == semantic::input_kind::tool && !input.object.empty();
                })) << "clice compiler observations reach the persisted Fughetta contribution";
                expect(std::ranges::any_of(unit.direct_inputs,
                                           [](const auto& input) {
                                               return input.kind == semantic::input_kind::provider && input.type == "clice/configuration" &&
                                                      !input.object.empty();
                                           }) &&
                       std::ranges::none_of(unit.direct_inputs, [](const auto& input) { return input.kind == semantic::input_kind::artifact; }))
                    << "clice's selected compilation records replace the whole preparation artifact input";
                const auto options = unit.recipe.options;
                stored.sweep();
                test::require(!stored.candidates("app", options, {{"app", sources}}).empty(),
                              "byte pressure must retain candidates whose shards are pinned by the checkpoint");
            }
            test::child restored_gateway{test::gateway_command(SERVICE_EXECUTABLE, tmp.dir, {"config.json"})};
            test::ready(restored_gateway, port);
            test::child restored_worker{test::worker_command(SERVICE_EXECUTABLE, tmp.dir, {"worker.json"})};
            test::eventually(
                [&] {
                    test::require(!restored_worker.finished(), restored_worker.output());
                    auto workers = test::request(port, http::method::GET, "/api/v1/workers");
                    return workers.status == 200 && workers.body != "[]";
                },
                std::chrono::seconds{60});
            auto continued_project = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"continuation\":" + json::write(incomplete_project.continuation) +
                                      ",\"budget\":{\"units\":" + std::to_string(other_parts) + "}}"));
            expect(continued_project.status == "complete" && continued_project.next_view == continued_project.view &&
                   continued_project.version == created.version && continued_project.environment == focused.environment &&
                   continued_project.items.size() > 1u && continued_project.reused_units == static_cast<std::uint64_t>(total_parts))
                << "continuation reuses the contributions retained by checkpoint views after restart: " << config::write(continued_project);
            auto expired = test::decode<tools::language_query::output>(
                agent("language", "{\"project\":\"app\",\"action\":\"symbols\",\"view\":" + json::write(waiting.view) + "}"));
            expect(expired.status == "expired" && expired.code == "view_expired") << "automatic collection withdraws an expired query view after restart";
            auto recovered = test::decode<tools::language_query::output>(agent("language", json::write(inspect)));
            expect(recovered.status == "complete" && recovered.reused_units == static_cast<std::uint64_t>(total_parts) && recovered.version == edited.version &&
                   recovered.items.size() == 1u && recovered.items.front().symbol_id == symbol_id)
                << "restarted services reuse retained contributions for the previously analyzed edited revision: " << config::write(recovered);
            // The restarted gateway has no completed tasks for this composed profile.
            auto main_query = alternating;
            alternating.worldline = "parallel";
            auto concurrent = std::async(
                std::launch::async, [&] { return test::decode<tools::language_query::output>(test::tool(port, "language", json::write(main_query))); });
            branch = test::decode<tools::language_query::output>(test::tool(port, "language", json::write(alternating)));
            returned = concurrent.get();
            expect(branch.status == "complete" && branch.version == independent.version && branch.items.size() == 2u && branch.snapshots.size() == 1u &&
                   branch.snapshots.front().worldline == "parallel")
                << config::write(branch);
            expect(returned.status == "complete" && returned.version == edited.version && returned.items.size() == 2u && returned.snapshots.size() == 1u &&
                   returned.snapshots.front().worldline == "main")
                << config::write(returned);
            for (auto [result, definition_line] : {std::pair{&branch, 2u}, std::pair{&returned, 3u}})
                expect(std::ranges::all_of(result->items, [&](const auto& item) {
                    return item.definitions.size() == 1u && item.definitions.front().source->path == "main.cpp" &&
                           item.definitions.front().position->line == definition_line;
                })) << "both output stages retain their worldline's source positions during concurrent queries";
            expect(branch.view != returned.view) << "concurrent worldlines publish separate views";
            auto added = line("{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + json::write(edited.version) +
                              ",\"operation_id\":\"optional-header\",\"changes\":[{\"kind\":\"create\",\"path\":\"optional.hpp\",\"text\":\"inline int "
                              "optional_value = 7;\\n\"}]}");
            auto optional_query = R"({"project":"app","configuration":"cpp","action":"symbols","limit":250,"selector":{"name":{"text":"optional_value"}}})";
            auto appeared = test::decode<tools::language_query::output>(agent("language", optional_query));
            expect(appeared.status == "complete" && appeared.reused_units >= static_cast<std::uint64_t>(main_parts) && !appeared.items.empty())
                << "a newly present include invalidates the TU that observed its absence: " << appeared.status << " reused=" << appeared.reused_units
                << " items=" << appeared.items.size();
            line("{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + json::write(added.version) +
                 ",\"operation_id\":\"remove-optional-header\",\"changes\":[{\"kind\":\"remove\",\"path\":\"optional.hpp\"}]}");
            auto withdrawn = test::decode<tools::language_query::output>(agent("language", optional_query));
            expect(withdrawn.status == "complete" && withdrawn.reused_units == static_cast<std::uint64_t>(total_parts) && withdrawn.items.empty())
                << "removing the include restores the earlier contribution without retaining the deleted declaration";
            const std::string generator = R"python(import json, pathlib, sys
directory = pathlib.Path("generated")
directory.mkdir(exist_ok=True)
files = []
if pathlib.Path("choice.txt").read_text() == "yes":
    (directory / "value.hpp").write_text("inline int generated_value = 9;\n")
    files.append("generated/value.hpp")
pathlib.Path(sys.argv[1]).write_text(json.dumps({"files": files}))
)python";
            const std::string generated_include = "\n#if __has_include(\"generated/value.hpp\")\n#include \"generated/value.hpp\"\n#endif\n";
            auto generated_source =
                line("{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + json::write(withdrawn.version) +
                     ",\"operation_id\":\"add-generator\",\"changes\":[{\"kind\":\"create\",\"path\":\"generate.py\",\"text\":" + json::write(generator) +
                     "},{\"kind\":\"create\",\"path\":\"choice.txt\",\"text\":\"yes\"},{\"kind\":\"edit\",\"path\":\"other.cpp\",\"offset\":" +
                     std::to_string(other_source.size()) + ",\"count\":0,\"text\":" + json::write(generated_include) + "}]}");
            auto& generated_build = project_settings.build_configurations.at("native");
            builds::config<"command"> generation_options;
            generation_options.operations["generate"] = {.argv = {"python3", "generate.py", "${reports}/generated.json"},
                                                         .outputs = {{"headers", {"directory", "generated", "directory"}}}};
            generated_build.builders["generate"] = builds::with<"command">{.options = std::move(generation_options)};
            generated_build.tasks["generate"] = {.builder = "generate", .operation = "generate", .generated_manifest = "generated.json"};
            generated_build.tasks.at("configure").after.push_back("generate");
            auto latest_settings = test::tool(port, "project", R"({"project":"app"})");
            test::tool(port, "save_project",
                       "{\"project\":\"app\",\"body\":{\"revision\":" + std::to_string(test::decode<revision>(latest_settings).revision) +
                           ",\"value\":" + config::write(project_settings) + "}}");
            const auto generated_query =
                R"({"project":"app","configuration":"cpp","action":"symbols","limit":250,"selector":{"name":{"text":"generated_value"}}})";
            auto generated = test::decode<tools::language_query::output>(agent("language", generated_query));
            expect(generated.status == "complete" && !generated.items.empty());
            const auto generated_with_header = generated.view;
            const auto generated_directory = last_directory();
            auto paired_generated = test::decode<tools::language_query::output>(agent(
                "language",
                R"({"project":"app","configuration":"paired","action":"symbols","outputs":["combined"],"limit":250,"selector":{"name":{"text":"generated_value"}}})"));
            expect(paired_generated.status == "complete" && paired_generated.items.size() == 2u)
                << "another stage in the same directory retains the graph's generated files";
            auto withdrawn_generated =
                line("{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + json::write(generated_source.version) +
                     ",\"operation_id\":\"withdraw-generated\",\"changes\":[{\"kind\":\"edit\",\"path\":\"choice.txt\",\"offset\":0,\"count\":3,\"text\":"
                     "\"no\"}]}");
            auto retracted = test::decode<tools::language_query::output>(agent("language", generated_query));
            expect(retracted.status == "complete" && retracted.items.empty() && retracted.reused_units >= static_cast<std::uint64_t>(main_parts) &&
                   !std::filesystem::exists(generated_directory / "generated/value.hpp"))
                << "the producer retracts its former header before analysis and preserves the unrelated TU";
            line(
                "{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + json::write(withdrawn_generated.version) +
                ",\"operation_id\":\"restore-generated\",\"changes\":[{\"kind\":\"edit\",\"path\":\"choice.txt\",\"offset\":0,\"count\":2,\"text\":\"yes\"}]}");
            generated = test::decode<tools::language_query::output>(agent("language", generated_query));
            expect(generated.status == "complete" && !generated.items.empty());
            std::erase(generated_build.tasks.at("configure").after, "generate");
            latest_settings = test::tool(port, "project", R"({"project":"app"})");
            test::tool(port, "save_project",
                       "{\"project\":\"app\",\"body\":{\"revision\":" + std::to_string(test::decode<revision>(latest_settings).revision) +
                           ",\"value\":" + config::write(project_settings) + "}}");
            auto retired = test::decode<tools::language_query::output>(agent("language", generated_query));
            expect(retired.status == "complete" && retired.items.empty() && last_directory() == generated_directory &&
                   !std::filesystem::exists(generated_directory / "generated/value.hpp"))
                << "a producer leaving the graph withdraws its files from the reused directory";
            // A pure sealed body edit bypasses compilation completely via CRDT anchor reprojection.
            {
                auto current_main = line(R"({"project":"app","action":"read","path":"main.cpp"})");
                const std::string old_factor = "int factor = 2;";
                const std::string new_factor = "int factor = 2;\n    factor += 3;";
                const auto factor_pos = current_main.text->find(old_factor);
                test::require(factor_pos != std::string::npos, "factor statement missing");
                tools::language_query::input baseline = inspect;
                baseline.version = current_main.version;
                auto base_view = test::decode<tools::language_query::output>(agent("language", json::write(baseline)));
                // Without a syntax tree a comment edit is not proven comment-only, so inspect analyzes the new version.
                auto comment_edit = line("{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + json::write(current_main.version) +
                                         ",\"operation_id\":\"comment-only-zero-compile\",\"changes\":[{\"kind\":\"edit\",\"path\":\"main.cpp\",\"offset\":0,"
                                         "\"count\":0,\"text\":\"// zero-compile comment\\n\"}]}");
                tools::language_query::input comment_query = inspect;
                comment_query.version = comment_edit.version;
                auto comment_reprojected = test::decode<tools::language_query::output>(agent("language", json::write(comment_query)));
                expect(comment_reprojected.version == comment_edit.version) << "inspect answers the commented version";
                // Pure sealed body edit on the original base version (concurrently checked on a fresh fork or before comment).
                const auto shifted_factor_pos = factor_pos + std::string{"// zero-compile comment\n"}.size();
                auto body_only_edit = line("{\"project\":\"app\",\"action\":\"apply\",\"observed\":" + json::write(comment_edit.version) +
                                           ",\"operation_id\":\"sealed-body-only\",\"changes\":[{\"kind\":\"edit\",\"path\":\"main.cpp\",\"offset\":" +
                                           std::to_string(shifted_factor_pos) + ",\"count\":" + std::to_string(old_factor.size()) +
                                           ",\"text\":" + json::write(new_factor) + "}]}");
                expect(body_only_edit.reach && *body_only_edit.reach == "body_only");
                // Re-anchor baseline on comment_edit's view? No: verify sealed body edit on base_view by checking a pure body edit directly.
                // Note: from base_view to body_only_edit includes both comment + body, so test sealed_body_only on a clean fork of current_main.
                test::tool(port, "worldline",
                           "{\"project\":\"app\",\"action\":\"fork\",\"name\":\"sealed-check\",\"worldline\":\"main\",\"version\":" +
                               json::write(current_main.version) + ",\"operation_id\":\"fork-sealed-check\"}");
                tools::language_query::input fork_base = inspect;
                fork_base.worldline = "sealed-check";
                fork_base.version.value().reset();
                auto fork_view = test::decode<tools::language_query::output>(test::tool(port, "language", json::write(fork_base)));
                auto fork_body_edit = test::decode<tools::worldline::output>(test::tool(
                    port, "worldline",
                    "{\"project\":\"app\",\"action\":\"apply\",\"worldline\":\"sealed-check\",\"observed\":" + json::write(current_main.version) +
                        ",\"operation_id\":\"fork-sealed-body\",\"changes\":[{\"kind\":\"edit\",\"path\":\"main.cpp\",\"offset\":" +
                        std::to_string(factor_pos) + ",\"count\":" + std::to_string(old_factor.size()) + ",\"text\":" + json::write(new_factor) + "}]}"));
                expect(fork_body_edit.reach && *fork_body_edit.reach == "body_only");
                tools::language_query::input zero_compile = fork_base;
                zero_compile.version = fork_body_edit.version;
                auto reprojected = test::decode<tools::language_query::output>(test::tool(port, "language", json::write(zero_compile)));
                expect(!reprojected.analyzed && reprojected.view == fork_view.view && reprojected.version == fork_body_edit.version)
                    << "sealed body edit must answer inspect with zero compilation";
                expect(reprojected.items.size() == 1u && reprojected.items.front().source &&
                       reprojected.items.front().source->contains("factor += 3;"))
                    << "zero-compile inspect must reproject body anchors and return updated body text";
            }
            expect(restored_worker.stop() == 0 && restored_gateway.stop() == 0 && source->stop() == 0);
            {
                semantic::store persisted{std::filesystem::path{doc.server.storage} / ".semantic"};
                auto parts = [&](const auto& id, const std::optional<std::string>& tu = {}) {
                    std::map<semantic::unit_id, std::vector<semantic::shard_id>> out;
                    const auto loaded = persisted.load(*id);
                    for (const auto& piece : loaded->pieces) {
                        const auto& unit = piece->unit;
                        const auto key = json::read<language::cpp::contribution_key>(unit.recipe.unit.name);
                        if (key && (!tu || key->translation_unit == *tu))
                            out[unit.id].push_back(piece->id);
                    }
                    for (auto& [unit, shards] : out)
                        std::ranges::sort(shards);
                    return out;
                };
                expect(bool(parts(recovered.view, "main.cpp") == parts(appeared.view, "main.cpp")))
                    << "an optional include retains every unaffected main-TU immutable contribution";
                expect(bool(parts(recovered.view) == parts(withdrawn.view))) << "withdrawing the include restores the exact earlier complete contribution set";
                expect(bool(parts(generated_with_header, "main.cpp") == parts(retracted.view, "main.cpp")))
                    << "generated-output retraction retains unrelated TU parts by identity";
            }
        };
    };
} // namespace

auto main(int argc, const char** argv) -> int {
    boost::ut::suite<"analysis">{cases};
    return boost::ut::cfg<>.run({.argc = argc, .argv = argv}) ? 1 : 0;
}
