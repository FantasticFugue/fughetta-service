#include "managed_session.hpp"

namespace {
    using namespace managed_test;

    constexpr std::string_view project = R"fixture(<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><TargetFramework>net10.0</TargetFramework><DebugType>portable</DebugType><Optimize>false</Optimize></PropertyGroup>
<ItemGroup><Compile Include="Library.fs"/></ItemGroup></Project>)fixture";

    constexpr std::string_view original = R"fixture(namespace Sample
open System.Runtime.InteropServices
[<Struct; StructLayout(LayoutKind.Sequential, Pack=1)>]
type NativeValue =
    val mutable value: int
    new(value) = { value = value }
type IValue =
    abstract Value: int with get
    abstract Transform: int -> int
[<AbstractClass>]
type BaseValue() =
    abstract Value: int with get
    default _.Value = 1
    abstract Transform: int -> int
    default _.Transform input = input + 1
type DerivedValue() =
    inherit BaseValue()
    let changed = Event<System.EventHandler, System.EventArgs>()
    let mutable stored = 4
    override _.Value = stored
    override _.Transform input = input + stored
    member _.Mutable with get() = stored and set(value) = stored <- value
    member _.Item with get(index: int) = stored + index
    [<CLIEvent>]
    member _.Changed = changed.Publish
    member this.Raise() = changed.Trigger(this, System.EventArgs.Empty)
    interface IValue with
        member this.Value = this.Value
        member this.Transform input = this.Transform input
[<AbstractClass>]
type GenericBase<'T>() =
    abstract Echo: 'T -> 'T
type GenericDerived() =
    inherit GenericBase<int>()
    override _.Echo value = value
type Shape = Point of x: int * y: int | Label of caption: string
type Model = { Count: int; Caption: string }
module Library =
    [<DllImport("native", EntryPoint="native_twice", CallingConvention=CallingConvention.Cdecl)>]
    extern int Native(int input)
    [<DllImport("native", EntryPoint="native_sum", CallingConvention=CallingConvention.Cdecl,
                CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)>]
    extern int Sum([<MarshalAs(UnmanagedType.LPUTF8Str)>] string text,
                   [<In; Out; MarshalAs(UnmanagedType.LPArray, SizeParamIndex=2s)>] int[] values,
                   int count, NativeValue& item)
    let twice input = input * 2
    let run input = twice input + Native input
    let identity (value: 'T) = value
)fixture";

    constexpr std::string_view native_source = R"fixture(#include <cstring>
struct NativeValue { int value; };
extern "C" int native_twice(int input) { return input * 2; }
extern "C" int native_sum(const char* text, int* values, int count, NativeValue* item) {
  int result = static_cast<int>(std::strlen(text)) + item->value;
  for (int i = 0; i < count; ++i) result += values[i];
  values[0] += 1; item->value += 2; return result;
}
)fixture";

    auto scenario(driver& runtime, const char** argv) -> coroutine::task<> {
        const auto dotnet = fs::canonical(argv[1]).string();
        const auto host = fs::absolute(argv[2]).string();
        const auto adapter = fs::absolute(argv[3]).string();
        const auto validator = fs::absolute(argv[4]).string();
        const auto reader = fs::absolute(argv[5]).string();
        const auto exporter = fs::absolute(argv[7]).string();
        const auto compiler = fs::absolute(argv[8]).string();
        const auto native_host = fs::absolute(argv[9]).string();
        workspace work{argv[6], "clr"};
        const auto& root = work.root;
        const auto sdk = fs::path{dotnet}.parent_path();
        runtime.configure(root, "sdk/dotnet", "sdk", sdk);
        runtime.env["LD_LIBRARY_PATH"] = root.string() + ":" + runtime.env["LD_LIBRARY_PATH"];

        test::spit(root / "Library.fsproj", project);
        test::spit(root / "Library.fs", original);
        test::spit(root / "native.cpp", native_source);
        const auto native = root / "libnative.so";
        co_await runtime.run({compiler, "-shared", "-fPIC", "-g", "native.cpp", "-o", native.string()}, root);
        test::spit(root / "native.json", co_await runtime.run({native_host, native.string()}, root));
        co_await runtime.export_model(root, "msbuild", dotnet, exporter, {"Library.fsproj"});

        conversation helper{runtime};
        helper.start({dotnet, host, "--adapter", adapter}, root);
        auto state = session("fsharp");
        co_await helper.call("open", opening(state, root, "sdk/dotnet", sdk));
        const auto requested = demand({"symbols", "definitions", "types"});
        co_await runtime.run({validator, "--unit", (root / "unit.json").string()}, root);
        auto unit = parse(test::slurp(root / "unit.json"));
        const auto output_dir = root / "compiled";
        const auto assembly = output_dir / "Library.dll";
        auto pdb = assembly;
        pdb.replace_extension(".pdb");

        std::vector<std::string> check_records{reader};
        for (int epoch = 1; epoch <= 2; ++epoch) {
            const auto suffix = std::to_string(epoch);
            const auto source = epoch == 1 ? std::string{original} : replaced(std::string{original}, "input * 2", "input * 3");
            const auto expected = epoch == 1 ? "12" : "15";
            test::spit(root / "Library.fs", source);
            state["epoch"] = epoch;
            co_await helper.call("update", installation(state, {document("Library.fs", "fsharp", source)}, "model",
                                                        {{"Library.fs", "source-" + suffix}}, "Library.fs"));
            const auto keys = co_await helper.call("units", {{"epoch", epoch}, {"demand", requested}});
            const auto& key = only(items(keys));
            fields(unit)["recipe"] = object{{"unit", key}, {"options", "options"}};
            fields(unit)["coverage"] = object{{"requested", requested}};
            const auto result = co_await helper.call("evaluate", {{"request", object{{"epoch", epoch}, {"slot_id", "slot"}, {"unit", key}}},
                                                                  {"unit", unit}});
            const auto& shard = only(items(result, "shards"));
            std::set<std::string> definitions;
            for (const auto& occurrence : items(shard, "occurrences"))
                definitions.insert(text(occurrence, "symbol"));
            for (const auto& binding : items(shard, "identities"))
                require(!fields(field(binding, "declaration")).get("anchor") || definitions.contains(text(binding, "symbol")),
                        "source identity must name a definition");
            require(no_errors(shard), json::write(field(shard, "diagnostics")));
            if (epoch == 1)
                require(!fs::exists(assembly), "source analysis must precede compilation");

            const auto artifact = root / ("artifact-" + suffix + ".json");
            const auto records = root / ("source-" + suffix + ".json");
            const auto snapshot = root / ("source-" + suffix + ".fs");
            test::spit(records, json::write(shard));
            test::spit(snapshot, source);
            co_await runtime.run({validator, "--shard", records.string()}, root);
            co_await runtime.run({dotnet, "build", "Library.fsproj", "--no-restore", "--nologo", "--output", output_dir.string()}, root);
            test::spit(artifact, co_await runtime.run({dotnet, host, "--clr", assembly.string(), pdb.string()}, root));
            std::string escaped;
            for (char byte : native.string()) {
                if (byte == '"')
                    escaped += '"';
                escaped += byte;
            }
            test::spit(root / "check.fsx", "#r \"compiled/Library.dll\"\n"
                       "open System.Runtime.InteropServices\nNativeLibrary.Load(@\"" + escaped + "\") |> ignore\n"
                       R"(let values = [| 3; 4 |]
let mutable item = Sample.NativeValue(5)
let marshaled = Sample.Library.Sum("é", values, values.Length, &item)
if marshaled <> 14 || values[0] <> 4 || item.value <> 7 then failwith "marshaling failed"
let tracked = Sample.DerivedValue()
let mutable fired = false
tracked.Changed.AddHandler(System.EventHandler(fun _ _ -> fired <- true))
tracked.Mutable <- 9
tracked.Raise()
if tracked.Item(2) <> 11 || (tracked :> Sample.IValue).Transform(1) <> 10 || not fired then failwith "member behavior failed"
if Sample.GenericDerived().Echo(7) <> 7 then failwith "generic override failed"
let actual = Sample.Library.run 3
if actual <> )" + expected + " then failwithf \"unexpected native result %d\" actual\nprintfn \"P/Invoke result: %d\" actual\n");
            const auto output = co_await runtime.run({dotnet, "fsi", "--exec", (root / "check.fsx").string()}, root);
            require(output.contains(std::string{"P/Invoke result: "} + expected), output);
            check_records.insert(check_records.end(), {artifact.string(), records.string(), snapshot.string()});
            if (epoch == 1)
                check_records.push_back((root / "native.json").string());
        }
        co_await helper.finish();
        co_await runtime.run(std::move(check_records), root);
        std::cout << "FCS source edits, CLR/PDB, real native P/Invoke execution and impact passed\n";
    }
}

int main(int argc, const char** argv) {
    try {
        test::require(argc == 10, "usage: clr-driver <dotnet> <host> <adapter> <validator> <reader> <work> <exporter> <compiler> <native-host>");
        native::context context;
        managed_test::driver runtime{context.get_scheduler()};
        test::require(cores::run(context, scenario(runtime, argv)), "CLR scenario stopped");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
