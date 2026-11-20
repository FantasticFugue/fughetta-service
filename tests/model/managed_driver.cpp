// Drive the managed model exporters against sample projects.
#include "gradle.hpp"
import host;

namespace {
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;

    int sequence = 0;
    std::map<std::string, std::string> child_env;
    fs::path staging;
    fs::path validator;

    struct outcome {
        int code = 0;
        std::string out;
        std::string err;
    };

    auto run(std::vector<std::string> argv, const fs::path& cwd) -> outcome {
        auto out = cwd / std::format("stdout-{}.txt", sequence++);
        auto command = test::command(argv.front(), cwd, std::vector<std::string>(argv.begin() + 1, argv.end()));
        command.env = child_env;
        command.standard_output = out;
        test::child child{std::move(command)};
        auto code = child.wait(600s);
        auto err = child.output();
        return {code, test::slurp(out), std::move(err)};
    }

    void check_run(std::vector<std::string> argv, const fs::path& cwd) {
        auto result = run(std::move(argv), cwd);
        if (result.code)
            throw std::runtime_error(result.out + result.err);
    }

    void put(const fs::path& root, std::string_view name, std::string_view text) { test::spit(root / name, text); }

    auto parse_file(const fs::path& path) -> json::value {
        auto text = test::slurp(path);
        auto result = json::read<json::value>(text);
        test::require(bool(result), text);
        return std::move(*result);
    }

    auto field(const json::value& value, std::string_view key) -> json::value {
        auto result = value.to_object().value().get(key);
        test::require(bool(result), std::format("missing {} in {}", key, json::write(value)));
        return std::move(*result);
    }

    auto string(const json::value& value, std::string_view key) -> std::string { return field(value, key).to_string().value(); }
    auto boolean(const json::value& value, std::string_view key) -> bool { return field(value, key).to_bool().value(); }
    auto array(const json::value& value, std::string_view key) -> std::vector<json::value> { return field(value, key).to_array().value(); }

    auto null_or_missing(const json::value& value, std::string_view key) -> bool {
        auto result = value.to_object().value().get(key);
        return !result || *result == json::value{};
    }

    auto export_model(const fs::path& root, std::string_view kind, const std::string& tool, const std::string& exporter,
                      std::vector<std::string> projects = {}, std::vector<std::string> generated = {}, bool prepare = true) -> json::value {
        fs::remove_all(staging);
        if (kind == "gradle") {
            std::vector<std::string> argv{tool, "--no-daemon", "--console=plain", "--no-configuration-cache", "--max-workers=2",
                                          "--init-script", exporter, "-Dfughetta.root=" + root.string()};
            auto receipt = (root / "model/dependencies.json").string();
            argv.push_back("-Dfughetta.prepare.output=" + receipt);
            argv.push_back("-Dfughetta.model.output=" + (root / "model/model.json").string());
            argv.push_back("-Dfughetta.dependencies=" + receipt);
            for (std::size_t index = 0; index < generated.size(); ++index)
                argv.push_back(std::format("-Dfughetta.generated.{}={}", index, (root / generated[index]).string()));
            if (prepare) {
                argv.push_back("fughettaPrepare");
                argv.push_back("fughettaModel");
            } else {
                argv.push_back("fughettaModel");
            }
            managed_test::wrap_gradle(argv);
            check_run(std::move(argv), root);
        } else {
            std::vector<std::string> argv{tool, exporter, "--tool", tool, "--root", root.string(), "--project-dir", root.string(),
                                          "--jobs", "2"};
            for (const auto& project : projects)
                argv.insert(argv.end(), {"--project", project});
            auto receipt = (root / "model/assets.json").string();
            argv.insert(argv.end(), {"--restore-output", receipt, "--model-output", (root / "model/model.json").string(), "--assets",
                                     receipt, "--operations"});
            if (prepare)
                argv.insert(argv.end(), {"restore", "model"});
            else
                argv.push_back("model");
            check_run(std::move(argv), root);
        }
        return parse_file(root / "model/model.json");
    }

    void check_objects(const fs::path& root, const json::value& model) {
        test::require(field(model, "schema").to_int64().value() == 1, "schema");
        test::require(!fs::exists(root / "model/dependencies"), "model/dependencies must not exist");
        auto serialized = json::write(model);
        test::require(serialized.find(root.string()) == std::string::npos, "root path leaked into model");
        test::require(serialized.find("${tree}-") == std::string::npos, "tree prefix leaked into model");
        if (!validator.empty())
            check_run({validator.string(), "--model", (root / "model/model.json").string()}, root);
        auto sdks = json::read<json::value>(child_env["FUGHETTA_SDKS"]);
        test::require(bool(sdks), "bad FUGHETTA_SDKS");
        for (const auto& unit : array(model, "units")) {
            std::vector<json::value> objects;
            for (const auto& ref : array(unit, "references"))
                if (string(ref, "kind") == "artifact")
                    objects.push_back(field(ref, "object"));
            for (const auto& value : field(unit, "generated").to_object().value())
                objects.push_back(value.second);
            for (const auto& ref : objects)
                test::require(fs::is_regular_file(staging / string(ref, "hash")), "missing object " + string(ref, "hash"));
            for (const auto& ref : array(unit, "references")) {
                if (string(ref, "kind") != "sdk")
                    continue;
                auto name = string(ref, "name");
                auto path = string(ref, "path");
                bool found = false;
                for (const auto& binding : sdks->to_array().value()) {
                    if (string(binding, "name") != name)
                        continue;
                    for (const auto& bind_root : array(binding, "roots"))
                        if (fs::exists(fs::path{bind_root.to_string().value()} / path))
                            found = true;
                }
                test::require(found, "sdk root for " + name);
            }
        }
    }

    auto fsharp(const fs::path& root, const std::string& tool, const std::string& exporter) {
        auto repeated = run({tool, exporter, "--root", root.string(), "--root", root.string(), "--operations", "model"}, root);
        test::require(repeated.code != 0 && repeated.err.find("--root") != std::string::npos, "duplicate --root must be rejected");
        put(root, "library/Library.fsproj", R"(<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup>
<ItemGroup><Compile Include="Library.fsi"/><Compile Include="Library.fs"/></ItemGroup>
</Project>)");
        put(root, "library/Library.fsi", "namespace Sample\nmodule Library =\n    val value: int\n");
        put(root, "library/Library.fs", "namespace Sample\nmodule Library =\n    let value = unfinished(\n");
        put(root, "app/App.fsproj", R"(<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup>
<ItemGroup><Compile Include="App.fs"/><ProjectReference Include="../library/Library.fsproj" SetTargetFramework="RuntimeIdentifier=;TargetFramework=net10.0"/></ItemGroup>
</Project>)");
        put(root, "app/App.fs", "namespace Sample\nmodule App =\n    let result = Library.value\n");
        check_run({tool, "restore", "app/App.fsproj", "--verbosity", "quiet"}, root);
        auto model = export_model(root, "msbuild", tool, exporter, {"app/App.fsproj"});
        auto find_unit = [&](std::string_view assembly) -> json::value {
            for (const auto& unit : array(model, "units"))
                if (field(unit, "extensions").to_object().value().get("assembly").value_or(json::value{}).to_string().value_or("") == assembly)
                    return unit;
            throw std::runtime_error("missing assembly " + std::string{assembly});
        };
        auto app = find_unit("App");
        auto library = find_unit("Library");
        auto sources = array(library, "sources");
        test::require(sources.size() >= 2 && sources[sources.size() - 2].to_string().value() == "library/Library.fsi" &&
                          sources[sources.size() - 1].to_string().value() == "library/Library.fs",
                      "library ordered sources");
        std::vector<std::string> project_units;
        for (const auto& ref : array(app, "references"))
            if (string(ref, "kind") == "project" && !null_or_missing(ref, "unit"))
                project_units.push_back(string(ref, "unit"));
        test::require(project_units.size() == 1 && project_units[0] == string(library, "id"), "app project reference");
        test::require(!field(library, "generated").to_object().value().empty() && !array(library, "references").empty() &&
                          !array(library, "compiler_arguments").empty(),
                      "library inputs");
        bool sdk = false;
        for (const auto& ref : array(app, "references"))
            if (string(ref, "kind") == "sdk")
                sdk = true;
        test::require(sdk, "app sdk reference");
        for (const auto& define : field(field(library, "extensions"), "defines").to_array().value())
            test::require(define.to_string().value_or("x") != "", "empty define");
        test::require(!boolean(field(library, "extensions"), "type_providers") && !boolean(field(app, "extensions"), "type_providers"),
                      "type providers off");
        check_objects(root, model);
        auto receipt = parse_file(root / "model/assets.json");
        std::set<std::string> files;
        for (const auto& entry : array(receipt, "files"))
            files.insert(string(entry, "path"));
        test::require(files == std::set<std::string>{"app/obj/project.assets.json", "library/obj/project.assets.json"}, "assets receipt");
        {
            std::ofstream assets{root / "library/obj/project.assets.json", std::ios::app};
            assets << "\n";
        }
        bool rejected = false;
        try {
            export_model(root, "msbuild", tool, exporter, {"app/App.fsproj"}, {}, false);
        } catch (const std::exception& error) {
            rejected = std::string{error.what()}.find("Restored assets differ") != std::string::npos;
        }
        test::require(rejected, "stale assets receipt must be rejected");
        put(root, "library/Last.fs", "namespace Sample\nmodule Last = let n = 2\n");
        auto project = root / "library/Library.fsproj";
        auto text = test::slurp(project);
        auto at = text.find("</ItemGroup>");
        test::require(at != std::string::npos, "fsproj ItemGroup");
        text.insert(at, "<Compile Include=\"Last.fs\"/>");
        test::spit(project, text);
        auto changed = export_model(root, "msbuild", tool, exporter, {"app/App.fsproj"});
        json::value updated;
        for (const auto& unit : array(changed, "units"))
            if (field(unit, "extensions").to_object().value().get("assembly").value_or(json::value{}).to_string().value_or("") == "Library")
                updated = unit;
        auto changed_sources = array(updated, "sources");
        test::require(changed_sources.size() >= 3 &&
                          changed_sources[changed_sources.size() - 3].to_string().value() == "library/Library.fsi" &&
                          changed_sources[changed_sources.size() - 2].to_string().value() == "library/Library.fs" &&
                          changed_sources[changed_sources.size() - 1].to_string().value() == "library/Last.fs",
                      "updated ordered sources");
        std::cout << "fsharp evaluated model passed\n";
    }

    // ---- minimal uncompressed ZIP writer for the artifact-fingerprint jar ----
    auto crc32_of(std::string_view data) -> std::uint32_t {
        static const auto table = [] {
            std::array<std::uint32_t, 256> value{};
            for (std::uint32_t index = 0; index < 256; ++index) {
                std::uint32_t current = index;
                for (int bit = 0; bit < 8; ++bit)
                    current = (current & 1) ? 0xEDB88320u ^ (current >> 1) : current >> 1;
                value[index] = current;
            }
            return value;
        }();
        std::uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char byte : data)
            crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
    void le16(std::string& out, std::uint16_t value) {
        out.push_back(char(value & 0xFF));
        out.push_back(char((value >> 8) & 0xFF));
    }
    void le32(std::string& out, std::uint32_t value) {
        for (int index = 0; index < 4; ++index)
            out.push_back(char((value >> (8 * index)) & 0xFF));
    }
    void write_zip(const fs::path& path, std::vector<std::pair<std::string, std::string>> entries) {
        struct record { std::string name; std::uint32_t crc = 0, size = 0, offset = 0; };
        std::string out;
        std::vector<record> directory;
        for (const auto& [name, data] : entries) {
            record row{name, crc32_of(data), static_cast<std::uint32_t>(data.size()), static_cast<std::uint32_t>(out.size())};
            le32(out, 0x04034b50); le16(out, 20); le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0);
            le32(out, row.crc); le32(out, row.size); le32(out, row.size); le16(out, static_cast<std::uint16_t>(name.size())); le16(out, 0);
            out += name; out += data; directory.push_back(row);
        }
        auto start = static_cast<std::uint32_t>(out.size());
        for (const auto& row : directory) {
            le32(out, 0x02014b50); le16(out, 20); le16(out, 20); le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0);
            le32(out, row.crc); le32(out, row.size); le32(out, row.size); le16(out, static_cast<std::uint16_t>(row.name.size()));
            le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0); le32(out, 0); le32(out, row.offset); out += row.name;
        }
        auto size = static_cast<std::uint32_t>(out.size()) - start;
        le32(out, 0x06054b50); le16(out, 0); le16(out, 0); le16(out, static_cast<std::uint16_t>(directory.size()));
        le16(out, static_cast<std::uint16_t>(directory.size())); le32(out, size); le32(out, start); le16(out, 0);
        std::ofstream{path, std::ios::binary} << out;
    }

    auto artifact_map(const json::value& model) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> found;
        for (const auto& unit : array(model, "units"))
            for (const auto& ref : array(unit, "references"))
                if (string(ref, "kind") == "artifact") {
                    auto object = field(ref, "object");
                    found[string(object, "name")] = string(object, "hash");
                }
        return found;
    }

    void kotlin(const fs::path& root, const std::string& tool, const std::string& exporter) {
        const std::string plugin_repositories = "pluginManagement { repositories { mavenCentral(); gradlePluginPortal() } }\n";
        put(root, "gradle.properties",
            "org.gradle.java.installations.fromEnv=JAVA_HOME\norg.gradle.java.installations.auto-detect=false\n"
            "org.gradle.java.installations.auto-download=false\n");
        put(root, "settings.gradle", plugin_repositories +
            "rootProject.name = 'managed-test'\ninclude 'library', 'app'\nincludeBuild 'one'\nincludeBuild 'two'\n");
        put(root, "build.gradle", R"(
plugins { id 'org.jetbrains.kotlin.jvm' version '2.4.21' apply false }
subprojects {
    apply plugin: 'org.jetbrains.kotlin.jvm'
    repositories { mavenCentral() }
    kotlin { jvmToolchain(25) }
}
project(':app') {
    dependencies { implementation project(':library'); implementation 'sample:one:1'; implementation 'sample:two:1' }
    kotlin.sourceSets.main.kotlin.srcDir(layout.buildDirectory.dir('generated'))
    tasks.register('generate') {
        doLast {
            def output = layout.buildDirectory.file('generated/Generated.kt').get().asFile
            output.parentFile.mkdirs()
            output.text = 'package generated\nclass Generated\n'
        }
    }
}
)");
        for (std::string_view name : {"one", "two"}) {
            put(root, std::string{name} + "/settings.gradle", plugin_repositories + std::format("rootProject.name = '{}'\n", name));
            put(root, std::string{name} + "/build.gradle", R"(
plugins { id 'org.jetbrains.kotlin.multiplatform' version '2.4.21' }
group = 'sample'
repositories { mavenCentral() }
kotlin { jvm(); jvmToolchain(25) }
)");
            put(root, std::string{name} + "/src/jvmMain/kotlin/Other.kt", "package other\nclass Other\n");
        }
        put(root, "library/src/main/kotlin/Library.kt", "package sample\nfun value(): Int = unfinished(\n");
        put(root, "app/src/main/kotlin/App.kt", "package sample\nfun use() = value()\n");
        check_run({tool, "--no-daemon", "--console=plain", "--max-workers=2", ":app:generate"}, root);
        auto model = export_model(root, "gradle", tool, exporter, {}, {"app/build/generated"});
        auto unit_at = [&](std::string_view project, std::string_view compilation) -> json::value {
            for (const auto& unit : array(model, "units"))
                if (string(unit, "project") == project &&
                    field(unit, "extensions").to_object().value().get("compilation").value_or(json::value{}).to_string().value_or("") == compilation)
                    return unit;
            throw std::runtime_error(std::format("no unit {} {}", project, compilation));
        };
        auto app = unit_at(":app", "main");
        auto library = unit_at(":library", "main");
        std::vector<json::value> dependencies;
        for (const auto& ref : array(app, "references"))
            if (string(ref, "kind") == "project" && !null_or_missing(ref, "attributes"))
                dependencies.push_back(ref);
        std::set<std::string> projects;
        for (const auto& ref : dependencies)
            projects.insert(string(ref, "project"));
        test::require(projects == std::set<std::string>{":library", ":one", ":two"}, "app project dependencies");
        std::set<std::string> unit_projects;
        std::set<std::string> ids;
        for (const auto& unit : array(model, "units")) {
            unit_projects.insert(string(unit, "project"));
            ids.insert(string(unit, "id"));
        }
        test::require(unit_projects.contains(":one") && unit_projects.contains(":two"), "included builds");
        test::require(ids.size() == array(model, "units").size(), "duplicate unit ids");
        for (const auto& ref : array(app, "references"))
            test::require(!(string(ref, "kind") == "project" && string(ref, "project") == string(app, "project")), "self dependency");
        test::require(field(dependencies.front(), "attributes").to_object().value().get("org.jetbrains.kotlin.platform.type").value().to_string().value() ==
                          "jvm",
                      "platform type");
        auto library_sources = array(library, "sources");
        test::require(library_sources.size() == 1 && library_sources.front().to_string().value() == "library/src/main/kotlin/Library.kt",
                      "library sources");
        bool runtime = false;
        for (const auto& ref : array(app, "references"))
            if (string(ref, "kind") == "sdk" && string(ref, "role") == "runtime")
                runtime = true;
        test::require(runtime, "runtime sdk");
        test::require(string(field(app, "extensions"), "jvm_target") == "25", "jvm target");
        test::require(field(app, "generated").to_object().value().get("app/build/generated/Generated.kt").has_value(), "generated source");
        test::require(!array(app, "compiler_arguments").empty() && string(field(app, "extensions"), "kgp_version") == "2.4.21", "compiler arguments");
        for (const auto& sub : fs::directory_iterator(root))
            if (sub.is_directory() && fs::exists(sub.path() / "build/libs"))
                for (const auto& artifact : fs::directory_iterator(sub.path() / "build/libs"))
                    test::require(artifact.path().extension() != ".jar", "unexpected built jar");
        check_objects(root, model);

        auto original = artifact_map(model);
        auto resolution = string(parse_file(root / "model/dependencies.json"), "resolution");
        auto dependency = root.parent_path() / (root.filename().string() + "-external.jar");
        try {
            write_zip(dependency, {{"META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n"}});
            {
                std::ofstream build{root / "one/build.gradle", std::ios::app};
                build << "\nkotlin.sourceSets.jvmMain.dependencies { implementation files(" << json::write(dependency.string()) << ") }\n";
            }
            auto changed = export_model(root, "gradle", tool, exporter, {}, {"app/build/generated"});
            auto updated = artifact_map(changed);
            for (const auto& [name, hash] : original)
                test::require(updated.contains(name) && updated[name] == hash, "artifact changed " + name);
            test::require(updated.contains(dependency.filename().string()), "external jar artifact");
            test::require(string(parse_file(root / "model/dependencies.json"), "resolution") != resolution, "resolution unchanged");
            check_objects(root, changed);
            write_zip(dependency, {{"META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n"}, {"changed.txt", "new resolved contents"}});
            bool stale = false;
            try {
                export_model(root, "gradle", tool, exporter, {}, {"app/build/generated"}, false);
            } catch (const std::exception& error) {
                stale = std::string{error.what()}.find("Dependency resolution differs") != std::string::npos;
            }
            test::require(stale, "stale composite dependency receipt accepted");
            auto project_build = root / "one/build.gradle";
            auto before = test::slurp(project_build);
            {
                std::ofstream build{project_build, std::ios::app};
                build << "\ntasks.named('compileKotlinJvm') { libraries.from(files('unclassified.jar')) }\n";
            }
            bool unclassified = false;
            try {
                export_model(root, "gradle", tool, exporter, {}, {"app/build/generated"});
            } catch (const std::exception& error) {
                unclassified = std::string{error.what()}.find("Unclassified compiler classpath input") != std::string::npos;
            }
            test::spit(project_build, before);
            test::require(unclassified, "unclassified compiler classpath input accepted");
            {
                std::ofstream build{root / "build.gradle", std::ios::app};
                build << "\nproject(':library') { apply plugin: 'java-test-fixtures' }\n"
                         "project(':app') { dependencies { testImplementation(testFixtures(project(':library'))) } }\n";
            }
            bool variant = false;
            try {
                export_model(root, "gradle", tool, exporter, {}, {"app/build/generated"});
            } catch (const std::exception& error) {
                variant = std::string{error.what()}.find("no unique exported JVM compilation") != std::string::npos;
            }
            test::require(variant, "unexported project variant accepted");
        } catch (...) {
            fs::remove(dependency);
            throw;
        }
        fs::remove(dependency);
        std::cout << "kotlin evaluated model passed\n";
    }
} // namespace

int main(int argc, const char** argv) {
    try {
        test::require(argc >= 5, "usage: managed-driver <kind> <tool> <exporter> --work PATH [--validator PATH]");
        std::string kind = argv[1], tool = argv[2], exporter = argv[3];
        fs::path work;
        std::string validator_path;
        std::string java_path;
        for (int index = 4; index < argc; ++index) {
            std::string_view option = argv[index];
            if (option == "--work")
                work = argv[++index];
            else if (option == "--validator")
                validator_path = argv[++index];
            else if (option == "--java")
                java_path = argv[++index];
            else
                test::require(false, "unknown option " + std::string{option});
        }
        test::require(!work.empty(), "--work required");
        test::scratch temp{kind, fs::absolute(work)};
        auto root = temp.dir;
        auto objects = root.parent_path() / (root.filename().string() + "-objects");
        staging = objects;
        validator = validator_path.empty() ? fs::path{} : fs::path{validator_path};
        child_env = host::environment();
        child_env["FUGHETTA_STAGING"] = objects.string();
        child_env["TMPDIR"] = root.string();
        if (kind == "kotlin") {
            fs::path sdk;
            if (!java_path.empty())
                sdk = fs::path{java_path}.parent_path().parent_path();
            else if (auto* home = std::getenv("JAVA_HOME"); home && *home)
                sdk = fs::path{home};
            else {
                auto found = host::resolve_tools({{"java", "java"}});
                test::require(found.contains("java"), "java not found for kotlin");
                sdk = fs::path{found["java"]}.parent_path().parent_path();
            }
            child_env["JAVA_HOME"] = sdk.string();
            child_env["GRADLE_USER_HOME"] = (root / "gradle-home").string();
            child_env["JAVA_TOOL_OPTIONS"] = "-Dkotlin.daemon.options=runFilesPath=" + (root / "daemon").string();
            child_env["FUGHETTA_SDKS"] = json::write(json::array{json::value{json::object{
                {"name", "sdk/java"}, {"object", "installed-sdk"}, {"roots", json::array{json::value{sdk.string()}}}}}});
        } else {
            auto sdk = fs::path{tool}.parent_path();
            child_env["FUGHETTA_SDKS"] = json::write(json::array{json::value{json::object{
                {"name", "sdk/dotnet"}, {"object", "installed-sdk"}, {"roots", json::array{json::value{sdk.string()}}}}}});
        }
        try {
            if (kind == "fsharp")
                fsharp(root, tool, exporter);
            else if (kind == "kotlin")
                kotlin(root, tool, exporter);
            else
                test::require(false, "unsupported kind " + kind);
        } catch (...) {
            fs::remove_all(objects);
            throw;
        }
        fs::remove_all(objects);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
