#include "gradle.hpp"

// Drives model export for the managed providers against sample projects, then
// asserts the exported model. Ported from the removed tests/model/managed.py. Standard output
// of every child goes to a file so the validated stdout stays separate from stderr.
import std;
import launch;
import host;
import records;

#include "managed_session.hpp"

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
        managed_test::wrap_gradle(argv);
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
        put(root, "Workspace.sln", R"(Microsoft Visual Studio Solution File, Format Version 12.00
Project("{F2A71F9B-5D33-465A-A702-920D77279786}") = "App", "app\App.fsproj", "{164BA5DA-533A-471A-814B-649C67534755}"
EndProject
Global
EndGlobal
)");
        check_run({tool, "restore", "app/App.fsproj", "--verbosity", "quiet"}, root);
        auto model = export_model(root, "msbuild", tool, exporter, {"Workspace.sln", "app/App.fsproj"});
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

    auto check_csharp_session(managed_test::driver& runtime, const fs::path& root, const std::string& tool,
                              const std::string& host_dll, const std::string& adapter_dll, const json::value& model,
                              std::string_view library, std::string_view app) -> coroutine::task<> {
        namespace managed = managed_test;
        using json::object;
        using json::value;
        const auto transcript_path = root / "transcript.jsonl";
        std::ofstream transcript{transcript_path, std::ios::binary};
        test::require(bool(transcript), "cannot open C# helper transcript");
        managed::conversation helper{runtime, &transcript};
        helper.start({tool, fs::absolute(host_dll).string(), "--adapter", fs::absolute(adapter_dll).string()}, root);
        auto state = managed::session("csharp");
        state["snapshot"] = "snapshot";
        state["inputs"] = json::array{};
        state["phase"] = "ready";
        auto opening = managed::opening(state, root, "sdk/dotnet", fs::path{tool}.parent_path());
        std::get<object>(opening["host"].data)["objects"] = staging.string();
        co_await helper.call("open", std::move(opening));

        int epoch = 0;
        auto update = [&](std::string_view source) -> coroutine::task<> {
            state["epoch"] = ++epoch;
            co_await helper.call("update", managed::installation(
                state, {managed::document("library/Library.cs", "csharp", source), managed::document("app/App.cs", "csharp", app)},
                "model-object", {{"library/Library.cs", std::format("library-source-{}", epoch)}, {"app/App.cs", "app-source"},
                                 {"app/Check.csx", "script-source"}, {"Standalone.csx", "unowned-script"}}, "library/Library.cs"));
        };
        co_await update(library);
        const auto requested = managed::demand({"symbols", "types", "references", "diagnostics"});
        const auto keys = co_await helper.call("units", {{"epoch", epoch}, {"demand", requested}});
        test::require(managed::items(keys).size() == 3 &&
                          managed::optional_field(managed::by(managed::items(keys), "name", "script:app/Check.csx"), "name") == value{"script:app/Check.csx"},
                      "C# helper must select both project units and the script");
        auto evaluate = [&](std::string_view project, std::string_view source) -> coroutine::task<value> {
            const auto id = project.starts_with("script:") ? std::string{project}
                : managed::text(managed::by(managed::items(model, "units"), "project", project), "id");
            const auto& key = managed::by(managed::items(keys), "name", id);
            auto demand = requested;
            demand["files"] = json::array{source};
            auto selected = json::to_value(semantic::unit{.id = id, .context = id,
                .direct_inputs = {{.kind = semantic::input_kind::artifact, .name = "model/model.json", .object = "model-object", .type = "project_model"}}});
            auto& selection = managed::fields(selected);
            selection["recipe"] = object{{"unit", key}, {"options", "options"}};
            managed::fields(selection["coverage"])["requested"] = demand;
            object request{{"execution", object{{"request_id", "request"}, {"computation_id", "computation"}, {"stage_id", "source"}}},
                           {"slot_id", "slot"}, {"unit", key}, {"epoch", epoch}, {"inputs", json::array{}}, {"demand", demand},
                           {"budget", object{}}, {"retained_only", false}};
            const auto reply = co_await helper.call("evaluate", {{"request", std::move(request)}, {"unit", std::move(selected)}});
            const auto& shard = managed::only(managed::items(reply, "shards"));
            test::require(managed::no_errors(shard), json::write(managed::field(shard, "diagnostics")));
            if (!validator.empty()) {
                const auto path = root / "shard.json";
                test::spit(path, json::write(shard));
                co_await runtime.run({validator.string(), "--shard", path.string()}, root);
            }
            co_return shard;
        };
        const auto library_shard = co_await evaluate("library/Library.csproj", "library/Library.cs");
        const auto app_shard = co_await evaluate("app/App.csproj", "app/App.cs");
        test::require(std::ranges::any_of(managed::items(library_shard, "diagnostics"), [](const auto& diagnostic) {
            return managed::text(diagnostic, "code") == "CA1822";
        }), "C# helper publishes configured analyzer diagnostics");
        const auto script_shard = co_await evaluate("script:app/Check.csx", "app/Check.csx");
        test::require(!managed::items(script_shard, "references").empty() &&
                          std::ranges::any_of(managed::items(script_shard, "diagnostics"), [](const auto& diagnostic) {
                              return managed::text(diagnostic, "code") == "FUGCS1001";
                          }), "C# script resolves project references and identifies an unresolved NuGet directive");
        const auto& native_add = managed::by(managed::items(library_shard, "symbols"), "name", "NativeAdd");
        const auto& metadata = managed::field(native_add, "extensions");
        test::require(std::ranges::contains(managed::items(native_add, "modifiers"), value{"pinvoke"}) &&
                          managed::text(metadata, "clr.pinvoke.library") == "native" &&
                          managed::text(metadata, "clr.pinvoke.entry_point") == "native_add" &&
                          managed::text(metadata, "clr.pinvoke.convention") == "cdecl",
                      "C# P/Invoke metadata");
        const auto& decorate = managed::by(managed::items(library_shard, "symbols"), "name", "Decorate");
        test::require(std::ranges::any_of(managed::items(app_shard, "references"), [&](const auto& reference) {
            return managed::optional_field(reference, "target") == managed::field(decorate, "id") &&
                   std::ranges::contains(managed::items(reference, "roles"), value{"call"});
        }), "C# extension method resolves to the library declaration");
        const auto& local = managed::by(managed::items(app_shard, "symbols"), "name", "local");
        const auto& local_declaration = managed::by(managed::items(app_shard, "occurrences"), "symbol", managed::text(local, "id"));
        const auto& echo = managed::by(managed::items(library_shard, "symbols"), "name", "Echo");
        const auto& typed = managed::items(app_shard, "typed_ranges");
        test::require(std::ranges::any_of(typed, [&](const auto& range) {
            return managed::field(range, "type") == managed::field(local, "type") &&
                   managed::field(range, "range") == managed::field(local_declaration, "selection");
        }), "C# inferred local type");
        test::require(std::ranges::any_of(typed, [&](const auto& range) {
            return managed::optional_field(range, "overload") == managed::field(echo, "id") &&
                   !managed::fields(managed::field(range, "substitutions")).empty();
        }), "C# generic call type substitutions");

        const auto& reads = managed::items(managed::field(app_shard, "unit"), "observed_dependencies");
        const auto& declarations = managed::by(reads, "channel", "declarations");
        const auto& inlined = managed::by(reads, "channel", "inline");
        auto observe = [&]() -> coroutine::task<std::pair<std::string, std::string>> {
            auto reply = co_await helper.call("observe", {{"session", state}, {"reads", reads}});
            const auto& inputs = managed::items(reply);
            co_return std::pair{managed::text(managed::by(inputs, "name", managed::text(declarations, "key")), "object"),
                                managed::text(managed::by(inputs, "name", managed::text(inlined, "key")), "object")};
        };
        const auto original = co_await observe();
        test::require(original == std::pair{managed::text(declarations, "value"), managed::text(inlined, "value")},
                      "C# observations return declaration and inline digests");
        const auto body_edit = managed::replaced(std::string{library}, "value.Trim()", "value.ToUpperInvariant()");
        co_await update(body_edit);
        test::require((co_await observe()) == original, "C# body edit retains both digests");
        co_await update(managed::replaced(body_edit, "Offset = 2", "Offset = 3"));
        const auto constant_edit = co_await observe();
        test::require(constant_edit.first == original.first && constant_edit.second != original.second, "C# constant edit changes the inline digest");
        co_await update(managed::replaced(body_edit, "Capture() => true", "Capture() => input is not null"));
        test::require((co_await observe()).first != original.first, "C# primary-constructor capture changes the struct layout digest");
        co_await update(managed::replaced(body_edit, "public bool Capture()", "private event System.Action? Event; public bool Capture()"));
        test::require((co_await observe()).first != original.first, "C# private field-like event changes the struct layout digest");
        co_await helper.finish();
        transcript.close();
        if (!validator.empty())
            co_await runtime.run({validator.string(), "--helper", transcript_path.string()}, root);
    }

    void csharp(const fs::path& root, const std::string& tool, const std::string& exporter, const std::string& host_dll,
                const std::string& adapter_dll) {
        constexpr auto library = R"(using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
namespace Sample;
public record Payload(string Text);
public struct Layout(object input)
{
    public bool Capture() => true;
}
public class AnalyzerSubject
{
    public int Constant() => 42;
}
public static partial class Library
{
    [GeneratedRegex("foo")]
    private static partial Regex Pattern();
    public static bool HasMatch(string text) => Pattern().IsMatch(text);
    public const int Offset = 2;
    public static T Echo<T>(T value) => value;
    public static string Decorate(this string value) => value.Trim();

    [DllImport("native", EntryPoint = "native_add", CallingConvention = CallingConvention.Cdecl)]
    public static extern int NativeAdd(int a, int b);
}
)";
        constexpr auto app = R"(using Sample;
namespace Consumer;
public static class App
{
    public static string Run()
    {
        var local = new Payload(" example ");
        return Library.Echo(local.Text.Decorate()) + Library.Offset.ToString();
    }
}
)";
        put(root, "library/Library.csproj", R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup>
</Project>)");
        put(root, "app/App.csproj", R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup>
  <ItemGroup><ProjectReference Include="../library/Library.csproj" /></ItemGroup>
</Project>)");
        put(root, "Workspace.slnx", R"(<Solution><Folder Name="/src/"><Project Path="library/Library.csproj" /><Project Path="app/App.csproj" /></Folder></Solution>)");
        put(root, "library/Library.cs", library);
        put(root, "app/App.cs", app);
        put(root, "app/Check.csx", "#r \"nuget:Fughetta.Unrestored.Package\"\nusing Sample;\nvar example = Library.Echo(42);\n");
        put(root, "Standalone.csx", "val unsupported = 0;\n");
        put(root, ".editorconfig", "root = true\n[*.cs]\ndotnet_diagnostic.CA1822.severity = warning\n"
                                    "dotnet_code_quality.CA1822.api_surface = public, internal, private\n");
        const auto model = export_model(root, "msbuild", tool, exporter, {"Workspace.slnx"});
        const auto units = array(model, "units");
        test::require(units.size() == 2, "solution exports both C# units");
        const auto& library_unit = managed_test::by(units, "project", "library/Library.csproj");
        const auto& app_unit = managed_test::by(units, "project", "app/App.csproj");
        auto check_unit = [&](const json::value& unit, std::string_view source) {
            test::require(string(unit, "language") == "csharp" && std::ranges::contains(array(unit, "sources"), json::value{source}) &&
                              !array(unit, "compiler_arguments").empty(),
                          "C# unit has sources and compiler arguments");
            test::require(std::ranges::any_of(array(unit, "references"), [](const auto& ref) { return string(ref, "kind") == "sdk"; }),
                          "C# unit has SDK references");
        };
        check_unit(library_unit, "library/Library.cs");
        check_unit(app_unit, "app/App.cs");
        test::require(std::ranges::any_of(array(app_unit, "references"), [&](const auto& ref) {
            return string(ref, "kind") == "project" && string(ref, "unit") == string(library_unit, "id");
        }), "C# app references the library project unit");
        check_objects(root, model);
        if (!host_dll.empty() && !adapter_dll.empty()) {
            native::context context;
            managed_test::driver runtime{context.get_scheduler()};
            runtime.env = child_env;
            test::require(cores::run(context, check_csharp_session(runtime, root, tool, host_dll, adapter_dll, model, library, app)),
                          "C# helper scenario stopped");
        }
        test::require(std::ranges::none_of(fs::recursive_directory_iterator(root), [](const auto& entry) {
            return entry.is_regular_file() && entry.path().extension() == ".dll";
        }), "C# analysis leaves project DLLs unbuilt");
        std::cout << "C# scenario passed\n";
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
    auto zip_bytes(std::vector<std::pair<std::string, std::string>> entries) -> std::string {
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
        return out;
    }
    void write_zip(const fs::path& path, std::vector<std::pair<std::string, std::string>> entries) {
        std::ofstream{path, std::ios::binary} << zip_bytes(std::move(entries));
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
        put(root, "settings.gradle",
            "rootProject.name = 'managed-test'\ninclude 'library', 'app'\nincludeBuild 'one'\nincludeBuild 'two'\n");
        put(root, "build.gradle", R"(
plugins { id 'org.jetbrains.kotlin.jvm' version '2.4.21' apply false }
subprojects {
    apply plugin: 'org.jetbrains.kotlin.jvm'
    repositories { mavenCentral() }
    kotlin { jvmToolchain(25) }
}
project(':app') {
    dependencies { implementation project(':library'); implementation 'sample:one:1'; implementation 'sample:two:1'; implementation files('libs/local.aar') }
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
            put(root, std::string{name} + "/settings.gradle", std::format("rootProject.name = '{}'\n", name));
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
        fs::create_directories(root / "app/libs");
        write_zip(root / "app/libs/local.aar", {{"classes.jar", zip_bytes({})}});
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
        test::require(original.contains("local.aar"), "local AAR is retained as a project model artifact");
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

    void scala(const fs::path& root, const std::string& tool, const std::string& exporter) {
        put(root, "settings.gradle", "rootProject.name = 'scala-model-test'\ninclude 'library', 'app'\n");
        put(root, "build.gradle", R"(
subprojects {
    apply plugin: 'scala'
    repositories { mavenCentral() }
    java { toolchain { languageVersion = JavaLanguageVersion.of(25) } }
    scala { scalaVersion = '2.13.16' }
}
project(':app') {
    dependencies { implementation project(':library') }
    sourceSets.main.scala.srcDir(layout.buildDirectory.dir('generated'))
    tasks.register('generateScala') {
        def source = layout.buildDirectory.file('generated/Generated.scala')
        outputs.file(source)
        doLast {
            def output = source.get().asFile
            output.parentFile.mkdirs()
            output.text = 'package sample\nobject Generated { val value = 2 }\n'
        }
    }
}
)");
        put(root, "library/src/main/scala/Library.scala", "package sample\nobject Library { val value = 1 }\n");
        put(root, "app/src/main/scala/App.scala", "package sample\nobject App { val value = Library.value + Generated.value }\n");
        check_run({tool, "--no-daemon", "--console=plain", "--max-workers=2", ":app:generateScala"}, root);
        auto model = export_model(root, "gradle", tool, exporter, {}, {"app/build/generated"});
        const auto units = array(model, "units");
        auto main_unit = [&](std::string_view project) -> const json::value& {
            auto unit = std::ranges::find_if(units, [&](const auto& candidate) {
                return string(candidate, "project") == project && string(candidate, "language") == "scala" &&
                       string(field(candidate, "extensions"), "compilation") == "main";
            });
            test::require(unit != units.end(), std::format("missing Scala main unit for {}", project));
            return *unit;
        };
        const auto& library = main_unit(":library");
        const auto& app = main_unit(":app");
        test::require(array(library, "sources") == std::vector<json::value>{json::value{"library/src/main/scala/Library.scala"}},
                      "library Scala sources");
        test::require(field(app, "generated").to_object().value().get("app/build/generated/Generated.scala").has_value(),
                      "generated Scala source");
        const auto references = array(app, "references");
        test::require(std::ranges::any_of(references, [](const auto& ref) {
            return string(ref, "kind") == "project" && string(ref, "role") == "classpath" && string(ref, "project") == ":library";
        }), "app Scala project reference");
        test::require(std::ranges::any_of(references, [](const auto& ref) {
            return string(ref, "kind") == "sdk" && string(ref, "role") == "runtime" && string(ref, "name") == "sdk/java";
        }), "app Scala runtime JDK reference");
        check_objects(root, model);
        std::cout << "scala evaluated model passed\n";
    }
} // namespace

int main(int argc, const char** argv) {
    try {
        test::require(argc >= 5, "usage: managed-driver <kind> <tool> <exporter> --work PATH [--validator PATH] [--java PATH] [--host PATH --adapter PATH]");
        std::string kind = argv[1], tool = argv[2], exporter = argv[3];
        fs::path work;
        std::string validator_path;
        std::string java_path;
        std::string host_dll, adapter_dll;
        for (int index = 4; index < argc; ++index) {
            std::string_view option = argv[index];
            test::require(index + 1 < argc, "missing value for " + std::string{option});
            if (option == "--work")
                work = argv[++index];
            else if (option == "--validator")
                validator_path = argv[++index];
            else if (option == "--java")
                java_path = argv[++index];
            else if (option == "--host")
                host_dll = argv[++index];
            else if (option == "--adapter")
                adapter_dll = argv[++index];
            else
                test::require(false, "unknown option " + std::string{option});
        }
        test::require(!work.empty(), "--work required");
        managed_test::workspace temp{work, kind};
        const auto& root = temp.root;
        auto objects = root.parent_path() / (root.filename().string() + "-objects");
        staging = objects;
        validator = validator_path.empty() ? fs::path{} : fs::path{validator_path};
        child_env = host::environment();
        child_env["FUGHETTA_STAGING"] = objects.string();
        child_env["TMPDIR"] = root.string();
        if (kind == "kotlin" || kind == "scala") {
            fs::path sdk;
            if (!java_path.empty())
                sdk = fs::canonical(java_path).parent_path().parent_path();
            else if (auto* home = std::getenv("JAVA_HOME"); home && *home)
                sdk = fs::path{home};
            else {
                auto found = host::resolve_tools({{"java", "java"}});
                test::require(found.contains("java"), "java not found for " + kind);
                sdk = fs::canonical(found.at("java")).parent_path().parent_path();
            }
            child_env["JAVA_HOME"] = sdk.string();
            if (kind == "scala" && !child_env.contains("GRADLE_RO_DEP_CACHE")) {
                fs::path gradle_home;
                if (auto* home = std::getenv("GRADLE_USER_HOME"); home && *home)
                    gradle_home = home;
                else if (auto* home = std::getenv("HOME"); home && *home)
                    gradle_home = fs::path{home} / ".gradle";
                else if (auto* profile = std::getenv("USERPROFILE"); profile && *profile)
                    gradle_home = fs::path{profile} / ".gradle";
                auto modules = gradle_home / "caches/modules-2";
                if (!gradle_home.empty() && fs::is_directory(modules))
                    child_env["GRADLE_RO_DEP_CACHE"] = fs::absolute(modules).string();
            }
            child_env["GRADLE_USER_HOME"] = (root / "gradle-home").string();
            child_env["JAVA_TOOL_OPTIONS"] = "-Dkotlin.daemon.options=runFilesPath=" + (root / "daemon").string();
            child_env["FUGHETTA_SDKS"] = json::write(json::array{json::value{json::object{
                {"name", "sdk/java"}, {"object", "installed-sdk"}, {"roots", json::array{json::value{sdk.string()}}}}}});
            put(root, "gradle.properties",
                "org.gradle.java.installations.fromEnv=JAVA_HOME\norg.gradle.java.installations.auto-detect=false\n"
                "org.gradle.java.installations.auto-download=false\n");
        } else {
            tool = fs::canonical(tool).string();
            auto sdk = fs::path{tool}.parent_path();
            child_env["FUGHETTA_SDKS"] = json::write(json::array{json::value{json::object{
                {"name", "sdk/dotnet"}, {"object", "installed-sdk"}, {"roots", json::array{json::value{sdk.string()}}}}}});
        }
        try {
            if (kind == "fsharp")
                fsharp(root, tool, exporter);
            else if (kind == "csharp")
                csharp(root, tool, exporter, host_dll, adapter_dll);
            else if (kind == "kotlin")
                kotlin(root, tool, exporter);
            else if (kind == "scala")
                scala(root, tool, exporter);
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
