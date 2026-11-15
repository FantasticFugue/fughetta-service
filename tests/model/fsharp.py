"""Exercise model export, FCS ordered source checking, edits, and typed record ingestion."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
from managed import export, put

parser = argparse.ArgumentParser()
parser.add_argument("dotnet", type=Path)
parser.add_argument("host", type=Path)
parser.add_argument("adapter", type=Path)
parser.add_argument("validator", type=Path)
parser.add_argument("--work", type=Path, required=True)
parser.add_argument("--exporter", type=Path, required=True)
parser.add_argument("--worker", type=Path)
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="fcs-", dir=args.work) as directory:
    root = Path(directory).resolve()
    dotnet, host, adapter, validator = (str(path.resolve()) for path in (args.dotnet, args.host, args.adapter, args.validator))
    helper = [dotnet, host, "--adapter", adapter]
    objects = root / "objects"
    sdk = str(Path(dotnet).parent)
    os.environ["FUGHETTA_STAGING"] = str(objects)
    os.environ["FUGHETTA_SDKS"] = json.dumps([{"name": "sdk/dotnet", "object": "installed-sdk", "roots": [sdk]}])
    os.environ["TMPDIR"] = str(root)
    put(root, "App.fsproj", '''<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup>
<ItemGroup><Compile Include="Library.fs"/><Compile Include="App.fs"/></ItemGroup>
</Project>''')
    library = "namespace Sample\nmodule Library =\n    let value = 42\n"
    consumer = "namespace Sample\nmodule App =\n    let result = Library.value\n    let scoped (input: int) =\n        let local = input + 1\n        local\n"
    consumer += """
type Payload<'T when 'T : comparison> = { Value: 'T; Count: int }
type Shape = Point of x: int * y: int | Label of caption: string
[<Measure>] type kg
[<Measure>] type second
module Features =
    let private hidden (left: int) right = left + right
    let curried (left: int, right: int) (label: string) = label + string (left + right)
    let inline comparable<'T when 'T : comparison> (value: 'T) = compare value value
    let inline length< ^T when ^T : (member Length : int)> (value: ^T) = (^T : (member Length : int) value)
    let speed (distance: float<kg>) (elapsed: float<second>) = distance / elapsed
    let (|Even|Odd|) (value: int) = if value % 2 = 0 then Even else Odd
    let classify value = match value with Even -> 1 | Odd -> 0
"""
    put(root, "Library.fs", library)
    put(root, "App.fs", consumer)
    model = export(root, "msbuild", dotnet, args.exporter, ["App.fsproj"])
    assert not list(root.glob("bin/**/*.dll")), "analysis must precede final compilation"
    project, = model["units"]
    if args.worker:
        subprocess.run([str(args.worker.resolve()), "--fsharp", str(root), dotnet, host, adapter, sdk], check=True)
    process = subprocess.Popen(helper, cwd=root, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    sequence = 0
    transcript = root / "transcript.jsonl"
    def call(operation, params):
        global sequence
        sequence += 1
        message = {"request_id": sequence, "operation": operation, "params": params}
        process.stdin.write(json.dumps(message) + "\n")
        process.stdin.flush()
        line = process.stdout.readline()
        assert line, f"helper exited during {operation}"
        response = json.loads(line)
        with transcript.open("a") as exchanges:
            exchanges.write(json.dumps({"message": message, "response": response}) + "\n")
        assert response["request_id"] == sequence
        assert "error" not in response, response
        return response["result"]
    session = {"id": "session", "slot": "slot", "project": "app", "provider": "workspace-fcs", "stage": "source", "options": "options",
               "snapshot": "snapshot", "epoch": 0, "inputs": [], "phase": "ready"}
    demand = {"files": [], "modules": [], "contexts": [], "projects": [], "facts": ["symbols", "types", "references", "diagnostics"]}
    try:
        call("open", {"session": session, "initialization": {}, "host": {"objects": str(objects), "sdks": {"sdk/dotnet": sdk}}})
        def update(text, signature=None):
            session["epoch"] += 1
            documents = [{"path": "Library.fs", "language": "fsharp", "text": text},
                         {"path": "App.fs", "language": "fsharp", "text": consumer}]
            if signature is not None:
                documents.append({"path": "Library.fsi", "language": "fsharp", "text": signature})
            call("update", {"session": session, "documents": documents,
                            "artifacts": {"model/model.json": "model-object"}, "sources": {}, "generated": {}, "changed": ["Library.fs"], "facts": {}})
        update(library)
        key, = call("units", {"epoch": session["epoch"], "demand": demand})
        assert key["provider"] == session["provider"]
        subprocess.run([validator, "--unit", str(root / "unit.json")], check=True)
        template = json.loads((root / "unit.json").read_text())
        template.update(recipe={"unit": key, "options": "options"}, coverage={"requested": demand, "available": [], "missing": [], "unresolved_count": 0})
        def evaluate(files=()):
            selected = json.loads(json.dumps(template))
            selected["coverage"]["requested"]["files"] = list(files)
            request = {"execution": {"request_id": "request", "computation_id": "computation", "stage_id": "source"}, "slot_id": "slot",
                       "unit": key, "epoch": session["epoch"], "inputs": [], "demand": selected["coverage"]["requested"], "budget": {}, "retained_only": False}
            response = call("evaluate", {"request": request, "unit": selected})
            shard, = response["shards"]
            producer_keys = response["producer_keys"]
            assert set(producer_keys) == {read["key"] for read in shard["unit"]["observed_dependencies"]}
            for read in shard["unit"]["observed_dependencies"]:
                expected = dict(project=session["project"], stage=session["stage"], provider=session["provider"], operation="evaluate", kind="module", name=read["scope"])
                assert producer_keys[read["key"]] == [expected]
                assert expected["name"] in {unit["id"] for unit in model["units"]}
                assert read["producers"] == [], "the host binds typed producer keys to canonical unit ids"
            declared_symbols = {symbol["id"] for symbol in shard["symbols"]}
            for reference in shard["references"]:
                assert reference.get("target_context") == (selected["context"] if reference.get("target") in declared_symbols else None)
            assert all(identity["declaration"]["provider"] == session["provider"] for identity in shard["identities"])
            assert all(row["ownership"]["origin"]["provider"] == session["provider"] for row in shard["symbols"])
            assert all(row["producer"] == session["provider"] for row in shard["diagnostics"])
            put(root, "shard.json", json.dumps(shard))
            subprocess.run([validator, "--shard", str(root / "shard.json")], check=True)
            return shard
        before = evaluate()
        assert not [d for d in before["diagnostics"] if d["severity"] == "error"], before["diagnostics"]
        value, = [s for s in before["symbols"] if s["qualified_name"] == "Sample.Library.value"]
        result, = [s for s in before["symbols"] if s["name"] == "result"]
        assert any(r.get("target") == value["id"] for r in before["references"])
        function, = [symbol for symbol in before["symbols"] if symbol["name"] == "scoped"]
        function_scope, = [scope for scope in before["scopes"] if scope.get("semantic_owner") == function["id"]]
        assert function_scope["kind"] == "function"
        scope_table = {scope["id"]: scope for scope in before["scopes"]}
        assert scope_table[function_scope["lexical_parent"]]["kind"] == "translation_unit"
        function_members = [symbol for symbol in before["symbols"] if symbol.get("owner") == function["id"]]
        assert {symbol["name"] for symbol in function_members} >= {"input", "local"}
        assert all(symbol["declaring_scope"] == function_scope["id"] and symbol["id"] in function_scope["declared_members"] for symbol in function_members)
        assert any(symbol["name"] == "input" and symbol["kind"] == "parameter" for symbol in function_members)
        assert any(scope.get("semantic_owner") == result["id"] and scope["kind"] == "initializer" for scope in before["scopes"])
        type_table = {typ["id"]: typ for typ in before["types"]}
        def named(name):
            matches = [symbol for symbol in before["symbols"] if symbol["name"] == name]
            assert len(matches) == 1, (name, matches)
            return matches[0]
        curried = named("curried")
        assert curried["visibility"] == "public" and named("hidden")["visibility"] == "private"
        assert [len(group["parameters"]) for group in type_table[curried["type"]]["parameter_groups"]] == [2, 1]
        assert [(parameter["group"], parameter["index"], parameter["name"]) for parameter in curried["parameters"]] == [(0, 0, "left"), (0, 1, "right"), (1, 0, "label")]
        payload = named("Payload")
        payload_type = type_table[payload["type"]]
        assert payload_type["declaration"] == payload["id"] and "record" in payload["modifiers"]
        generic, = [type_table[identifier] for identifier in payload_type["type_parameters"]]
        assert generic["binder"] == payload["id"] and generic["parameter_index"] == 0
        assert "comparison" in generic["qualifiers"]
        length_type = type_table[named("length")["type"]]
        length_generic, = [type_table[identifier] for identifier in length_type["type_parameters"]]
        member_constraint, = [item for item in length_generic["extensions"]["fsharp.constraints"] if item["kind"] == "member"]
        assert member_constraint["name"] == "get_Length" and member_constraint["result"] in type_table
        returned = type_table[type_table[named("speed")["type"]]["return_type"]]
        measure, = [type_table[identifier] for identifier in returned["arguments"]]
        assert "measure" in measure["qualifiers"] and len(measure["arguments"]) == 2
        inverse = type_table[measure["arguments"][1]]
        assert "measure" in inverse["qualifiers"] and len(inverse["arguments"]) == 1
        even, odd = named("Even"), named("Odd")
        assert even["extensions"]["fsharp.active_pattern"] == {"names": ["Even", "Odd"], "index": 0, "total": True}
        assert odd["extensions"]["fsharp.active_pattern"]["index"] == 1
        shape = named("Shape")
        for case_name, case_index in [("Point", 0), ("Label", 1)]:
            case = named(case_name)
            assert case["extensions"]["fsharp.sequence"] == case_index
            assert case["extensions"]["fsharp.declaring_entity"] == shape["native_key"]["native_key"]
        definitions = {occurrence["symbol"] for occurrence in before["occurrences"]}
        for name, variant, sequence in [("x", 0, 0), ("y", 0, 1), ("caption", 1, 0)]:
            field = named(name)
            assert field["kind"] == "field" and "anchor" in field["native_key"] and field["id"] in definitions
            assert field["extensions"]["fsharp.variant"] == variant and field["extensions"]["fsharp.sequence"] == sequence
            assert field["extensions"]["fsharp.declaring_entity"] == shape["native_key"]["native_key"]
        assert all(symbol["extensions"]["fsharp.assembly"] == "App" for symbol in before["symbols"])
        model_path = root / "model/model.json"
        original_model = model_path.read_text()
        missing_reference_model = json.loads(original_model)
        missing_reference_model["units"][0]["compiler_arguments"].append("-r:${tree}/MissingReference.dll")
        model_path.write_text(json.dumps(missing_reference_model))
        update(library)
        missing_reference = evaluate()
        project_diagnostics = [item for item in missing_reference["diagnostics"] if "MissingReference.dll" in item["message"]]
        assert project_diagnostics, missing_reference["diagnostics"]
        assert all(item["location"] == {"project": session["project"]} and item["ownership"]["origin"]["kind"] == "unknown" for item in project_diagnostics)
        model_path.write_text(original_model)
        update(library)
        local = evaluate(["App.fs"])
        assert local["unit"]["contributions"]["files"] == ["App.fs"]
        assert local["unit"]["coverage"]["available"][0]["files"] == ["App.fs"]
        assert [s["name"] for s in local["symbols"] if s["name"] == "result"] == ["result"]
        assert not [s for s in local["symbols"] if s["qualified_name"] == "Sample.Library.value"], "file demand must not publish the prefix's definitions"
        update(library.replace("42", '"changed"'))
        after = evaluate(["App.fs"])
        changed, = [s for s in after["symbols"] if s["name"] == "result"]
        types = {t["id"]: t for t in after["types"]}
        assert types[changed["type"]]["name"] == "Microsoft.FSharp.Core.string", types[changed["type"]]
        assert result["id"] == changed["id"]
        complete = evaluate()
        assert {"Library.fs", "App.fs"} <= set(complete["unit"]["contributions"]["files"])
        assert any(s["qualified_name"] == "Sample.Library.value" for s in complete["symbols"])
        update(library.replace("42", "missingValue"))
        broken = evaluate()
        assert any(d["severity"] == "error" for d in broken["diagnostics"])
        assert (root / "Library.fs").read_text() == library, "document updates must stay in the helper's snapshot"
        # FCS owns the .fsi boundary and ordered prefix. A consumer file can be
        # queried independently of a broken implementation, while a project
        # request must still expose that implementation's diagnostic.
        signature = "namespace Sample\nmodule Library =\n    val value: int\n"
        put(root, "Library.fsi", signature)
        project_file = root / "App.fsproj"
        project_file.write_text(project_file.read_text().replace('<Compile Include="Library.fs"/>', '<Compile Include="Library.fsi"/><Compile Include="Library.fs"/>'))
        export(root, "msbuild", dotnet, args.exporter, ["App.fsproj"])
        update(library.replace("42", '"wrong implementation"'), signature)
        bounded = evaluate(["App.fs"])
        assert not [d for d in bounded["diagnostics"] if d["severity"] == "error"], bounded["diagnostics"]
        public_result, = [s for s in bounded["symbols"] if s["name"] == "result"]
        assert {t["id"]: t for t in bounded["types"]}[public_result["type"]]["name"] == "Microsoft.FSharp.Core.int"
        assert any(d["severity"] == "error" for d in evaluate()["diagnostics"])
        update(library.replace("42", '"changed"'), signature.replace("int", "string"))
        changed_signature = evaluate(["App.fs"])
        public_result, = [s for s in changed_signature["symbols"] if s["name"] == "result"]
        assert {t["id"]: t for t in changed_signature["types"]}[public_result["type"]]["name"] == "Microsoft.FSharp.Core.string"
        update(library, signature)
        restored = evaluate(["App.fs"])
        public_result, = [s for s in restored["symbols"] if s["name"] == "result"]
        assert {t["id"]: t for t in restored["types"]}[public_result["type"]]["name"] == "Microsoft.FSharp.Core.int"
        # Persisted reads come from the referenced project's compiler symbols,
        # not the whole source snapshot. Exercise changes through the same
        # helper session, then observe the old read-set as another worker would.
        put(root, "dependency/Dependency.fsproj", '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup><ItemGroup><Compile Include="Dependency.fs"/></ItemGroup></Project>')
        dependency = "namespace Cross\nmodule Dependency =\n    [<Literal>]\n    let Code = 1\n    let compute (value: int) = value + 1\n    let inline twice (value: int) = value + value\n"
        consume = "namespace Cross\nmodule Consumer =\n    let result = Dependency.compute 2 + Dependency.twice 3\n"
        put(root, "dependency/Dependency.fs", dependency)
        put(root, "Consumer.fs", consume)
        put(root, "App.fsproj", '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup><ItemGroup><ProjectReference Include="dependency/Dependency.fsproj"/><Compile Include="Consumer.fs"/></ItemGroup></Project>')
        cross_model = export(root, "msbuild", dotnet, args.exporter, ["App.fsproj"])
        model = cross_model
        consumer_unit, = [unit for unit in cross_model["units"] if "Consumer.fs" in unit["sources"]]
        source_ids = {path: "source:" + path for unit in cross_model["units"] for path in unit["sources"]}
        def cross_update(text, revision):
            session["epoch"] += 1
            source_ids["dependency/Dependency.fs"] = revision
            call("update", {"session": session,
                            "documents": [{"path": "dependency/Dependency.fs", "language": "fsharp", "text": text},
                                          {"path": "Consumer.fs", "language": "fsharp", "text": consume}],
                            "artifacts": {"model/model.json": "cross-model"}, "sources": source_ids, "generated": {}, "changed": ["dependency/Dependency.fs"], "facts": {}})
        cross_update(dependency, "dependency-one")
        keys = call("units", {"epoch": session["epoch"], "demand": demand})
        key, = [unit for unit in keys if unit["name"] == consumer_unit["id"]]
        template["recipe"]["unit"] = key
        template["direct_inputs"] = [{"kind": "source", "name": "app", "object": "snapshot", "type": ""}]
        initial = evaluate()
        assert not [d for d in initial["diagnostics"] if d["severity"] == "error"], initial["diagnostics"]
        consumer_key = key
        dependency_unit, = [unit for unit in keys if unit["name"] != consumer_key["name"]]
        key = dependency_unit
        template["recipe"]["unit"] = key
        dependency_shard = evaluate()
        compute, = [symbol for symbol in dependency_shard["symbols"] if symbol["qualified_name"] == "Cross.Dependency.compute"]
        key = consumer_key
        template["recipe"]["unit"] = key
        assert any(reference.get("target") == compute["id"] for reference in initial["references"]), "cross-project references must target the producer's actual declaration identity"
        reads = initial["unit"]["observed_dependencies"]
        assert {read["channel"] for read in reads} == {"declarations", "inline"}, reads
        assert not [item for item in initial["unit"]["direct_inputs"] if item["kind"] == "source"]
        assert not [item for item in initial["unit"]["direct_inputs"] if item["name"] == "dependency/Dependency.fs"]
        def observed():
            return {item["name"]: item["object"] for item in call("observe", {"session": session, "reads": reads})}
        baseline = {read["key"]: read["value"] for read in reads}
        assert observed() == baseline
        body_edit = dependency.replace("value + 1", "value + 2")
        cross_update(body_edit, "dependency-two")
        assert observed() == baseline, "body-only implementation edits must retain declaration and inline channels"
        inlined = body_edit.replace("value + value", "value * 2")
        cross_update(inlined, "dependency-inline")
        inline_values = observed()
        assert all((inline_values[read["key"]] != read["value"]) == (read["channel"] == "inline") for read in reads)
        added = body_edit + "    let introduced (value: int) = value\n"
        cross_update(added, "dependency-added")
        assert any(observed()[read["key"]] != read["value"] for read in reads if read["channel"] == "declarations"), "new lookup candidates change the declaration bucket"
        signature_edit = body_edit.replace("(value: int) = value + 2", "(value: int64) = value + 2L")
        cross_update(signature_edit, "dependency-signature")
        assert any(observed()[read["key"]] != read["value"] for read in reads if read["channel"] == "declarations")
        cross_update(dependency.replace("let Code = 1", "let Code = 2"), "dependency-constant")
        assert any(observed()[read["key"]] != read["value"] for read in reads if read["channel"] == "declarations")
        complex_source = dependency + "    type Payload = { Value: int }\n"
        cross_update(complex_source, "dependency-complex")
        assert observed() == {}, "unmodeled compiler type representations must invalidate the old channel reads"
        fallback = evaluate()
        assert any(item["name"] == "dependency/Dependency.fs" for item in fallback["unit"]["direct_inputs"]), "unsupported signatures retain exact dependency source inputs"
        call("close", {})
        assert process.wait(timeout=15) == 0
        with tempfile.TemporaryDirectory(prefix="fcs-worker-", dir=args.work) as next_directory:
            next_root = Path(next_directory).resolve()
            # Transfer the model's data, not the running process's
            # TMPDIR (which can contain .NET debugger FIFOs and sockets).
            paths = {"model/model.json"}
            object_ids = set()
            for unit in cross_model["units"]:
                paths.update(unit["sources"])
                paths.add(unit["project"])
                object_ids.update(item["hash"] for item in unit["generated"].values())
                for reference in unit["references"]:
                    if reference["kind"] == "artifact":
                        object_ids.add(reference["object"]["hash"])
                    elif reference["kind"] == "tree":
                        paths.add(reference["path"])
            paths.update("objects/" + object_id for object_id in object_ids)
            for path in paths:
                source, destination = root / path, next_root / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)
            process = subprocess.Popen(helper, cwd=next_root, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            session.update(id="worker-two-session", slot="worker-two-slot", epoch=0)
            call("open", {"session": session, "initialization": {}, "host": {"objects": str(next_root / "objects"), "sdks": {"sdk/dotnet": sdk}}})
            cross_update(body_edit, "worker-two-dependency")
            assert observed() == baseline, "persisted semantic reads must reproduce in a fresh helper and worker tree"
            call("close", {})
            assert process.wait(timeout=15) == 0
        subprocess.run([validator, "--helper", str(transcript)], check=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
print("F# model, ordered edits, references and record ingestion passed")
