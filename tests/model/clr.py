"""Compile F# after source analysis, then query CLR IL/PDB and source emission records."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import tempfile
from managed import export, put, run

parser = argparse.ArgumentParser()
parser.add_argument("dotnet")
parser.add_argument("host")
parser.add_argument("adapter")
parser.add_argument("validator")
parser.add_argument("reader")
parser.add_argument("work")
parser.add_argument("--exporter", required=True)
parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
parser.add_argument("--native-host", default=os.environ.get("TEST_NATIVE_HOST", "fughetta-native"))
args = parser.parse_args()
dotnet, host, adapter, validator, reader, work = args.dotnet, args.host, args.adapter, args.validator, args.reader, args.work
dotnet, host, adapter, validator, reader = [str(Path(value).resolve()) for value in (dotnet, host, adapter, validator, reader)]
Path(work).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="clr-", dir=work) as directory:
    root = Path(directory).resolve()
    objects = root / "objects"
    sdk = str(Path(dotnet).parent)
    os.environ.update(TMPDIR=str(root), FUGHETTA_STAGING=str(objects),
        FUGHETTA_SDKS=json.dumps([{"name": "sdk/dotnet", "object": "sdk", "roots": [sdk]}]))
    put(root, "Library.fsproj", '''<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><TargetFramework>net10.0</TargetFramework><DebugType>portable</DebugType><Optimize>false</Optimize></PropertyGroup>
<ItemGroup><Compile Include="Library.fs"/></ItemGroup></Project>''')
    source = '''namespace Sample
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
'''
    put(root, "Library.fs", source)
    put(root, "native.cpp", '\n'.join([
        '#include <cstring>',
        'struct NativeValue { int value; };',
        'extern "C" int native_twice(int input) { return input * 2; }',
        'extern "C" int native_sum(const char* text, int* values, int count, NativeValue* item) {',
        '  int result = static_cast<int>(std::strlen(text)) + item->value;',
        '  for (int i = 0; i < count; ++i) result += values[i];',
        '  values[0] += 1; item->value += 2; return result;',
        '}', '']))
    native = root / "libnative.so"
    run([args.compiler, "-shared", "-fPIC", "-g", root / "native.cpp", "-o", native], root)
    native_report = run([args.native_host, native], root)
    put(root, "native.json", native_report)
    model = export(root, "msbuild", dotnet, args.exporter, ["Library.fsproj"])
    process = subprocess.Popen([dotnet, host, "--adapter", adapter], cwd=root, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    sequence = 0
    def call(operation, params):
        global sequence
        sequence += 1
        process.stdin.write(json.dumps(dict(request_id=sequence, operation=operation, params=params)) + "\n")
        process.stdin.flush()
        response = json.loads(process.stdout.readline())
        assert "error" not in response, response
        return response["result"]
    try:
        session = dict(id="session", slot="slot", project="app", provider="fsharp", stage="source", options="options", epoch=0)
        call("open", dict(session=session, initialization={}, host=dict(objects=str(objects), sdks={"sdk/dotnet": sdk})))
        demand = dict(files=[], modules=[], contexts=[], projects=[], facts=["symbols", "definitions", "types"])
        run([validator, "--unit", root / "unit.json"], root)
        template = json.loads((root / "unit.json").read_text())
        results = []
        for epoch, text, expected in [(1, source, 12), (2, source.replace("input * 2", "input * 3"), 15)]:
            put(root, "Library.fs", text)
            session["epoch"] = epoch
            call("update", dict(session=session, documents=[dict(path="Library.fs", language="fsharp", text=text)],
                                artifacts={"model/model.json": "model"}, sources={"Library.fs": "source-" + str(epoch)},
                                generated={}, changed=["Library.fs"], facts={}))
            key, = call("units", dict(epoch=epoch, demand=demand))
            template.update(recipe=dict(unit=key, options="options"), coverage=dict(requested=demand))
            result = call("evaluate", dict(request=dict(epoch=epoch, slot_id="slot", unit=key), unit=template))
            shard, = result["shards"]
            definitions = {row["symbol"] for row in shard["occurrences"]}
            for binding in shard["identities"]:
                assert "anchor" not in binding["declaration"] or binding["symbol"] in definitions, binding
            assert not [d for d in shard["diagnostics"] if d["severity"] == "error"], shard["diagnostics"]
            # The source query precedes the first compilation, and the edited
            # source is checked by the same helper session before rebuilding.
            if epoch == 1:
                assert not (root / "bin/Debug/net10.0/Library.dll").exists()
            source_json = root / ("source-" + str(epoch) + ".json")
            source_text = root / ("source-" + str(epoch) + ".fs")
            put(root, source_json.name, json.dumps(shard))
            put(root, source_text.name, text)
            run([validator, "--shard", source_json], root)
            run([dotnet, "build", "Library.fsproj", "--no-restore", "--nologo"], root)
            assembly = root / "bin/Debug/net10.0/Library.dll"
            report = run([dotnet, host, "--clr", assembly, assembly.with_suffix(".pdb")], root)
            artifact_json = root / ("artifact-" + str(epoch) + ".json")
            put(root, artifact_json.name, report)
            put(root, "check.fsx", '#r "bin/Debug/net10.0/Library.dll"\n'
                'open System.Runtime.InteropServices\n'
                'NativeLibrary.Load(@"' + str(native).replace('"', '""') + '") |> ignore\n'
                'let values = [| 3; 4 |]\n'
                'let mutable item = Sample.NativeValue(5)\n'
                'let marshaled = Sample.Library.Sum("é", values, values.Length, &item)\n'
                'if marshaled <> 14 || values[0] <> 4 || item.value <> 7 then failwith "marshaling failed"\n'
                'let tracked = Sample.DerivedValue()\n'
                'let mutable fired = false\n'
                'tracked.Changed.AddHandler(System.EventHandler(fun _ _ -> fired <- true))\n'
                'tracked.Mutable <- 9\n'
                'tracked.Raise()\n'
                'if tracked.Item(2) <> 11 || (tracked :> Sample.IValue).Transform(1) <> 10 || not fired then failwith "member behavior failed"\n'
                'if Sample.GenericDerived().Echo(7) <> 7 then failwith "generic override failed"\n'
                'let actual = Sample.Library.run 3\n'
                'if actual <> ' + str(expected) + ' then failwithf "unexpected native result %d" actual\n'
                'printfn "P/Invoke result: %d" actual\n')
            environment = os.environ.copy()
            environment["LD_LIBRARY_PATH"] = str(root) + os.pathsep + environment.get("LD_LIBRARY_PATH", "")
            executed = subprocess.run([dotnet, "fsi", "--exec", str(root / "check.fsx")], cwd=root,
                                      env=environment, text=True, capture_output=True)
            assert executed.returncode == 0, executed.stdout + executed.stderr
            assert "P/Invoke result: " + str(expected) in executed.stdout
            results.extend([str(artifact_json), str(source_json), str(source_text)])
        call("close", {})
        assert process.wait(timeout=15) == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    subprocess.run([reader, *results[:3], str(root / "native.json"), *results[3:]], check=True)

print("FCS source edits, CLR/PDB, real native P/Invoke execution and impact passed")
