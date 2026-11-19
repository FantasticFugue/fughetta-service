"""Exercise evaluated models across source projects, generated inputs and a fresh worker tree."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


def run(args, cwd):
    result = subprocess.run([str(arg) for arg in args], cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def put(root, name, text):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def export(root, kind, tool, exporter, projects=(), generated=(), prepare=True):
    shutil.rmtree(os.environ["FUGHETTA_STAGING"], ignore_errors=True)
    if kind == "gradle":
        common = [tool, "--no-daemon", "--console=plain", "--no-configuration-cache", "--max-workers=2", "--init-script",
                  exporter, "-Dfughetta.root=" + str(root)]
        receipt = root / "model/dependencies.json"
        common += ["-Dfughetta.prepare.output=" + str(receipt), "-Dfughetta.model.output=" + str(root / "model/model.json")]
        run(common + ["-Dfughetta.dependencies=" + str(receipt)] +
            ["-Dfughetta.generated." + str(i) + "=" + str(root / path) for i, path in enumerate(generated)] +
            (["fughettaPrepare", "fughettaModel"] if prepare else ["fughettaModel"]), root)
    else:
        common = [tool, exporter]
        flags = ["--tool", tool, "--root", root, "--project-dir", root, "--jobs", "2"]
        for project in projects:
            flags += ["--project", project]
        receipt = root / "model/assets.json"
        flags += ["--restore-output", receipt, "--model-output", root / "model/model.json", "--assets", receipt]
        run(common + flags + ["--operations"] + (["restore", "model"] if prepare else ["model"]), root)
    return json.loads((root / "model/model.json").read_text(encoding="utf-8"))


def kotlin(root, tool):
    put(root, "settings.gradle", "rootProject.name = 'managed-test'\ninclude 'library', 'app'\nincludeBuild 'one'\nincludeBuild 'two'\n")
    put(root, "build.gradle", """
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
            output.text = 'package generated\\nclass Generated\\n'
        }
    }
}
""")
    for name in ("one", "two"):
        put(root, name + "/settings.gradle", "rootProject.name = '" + name + "'\n")
        put(root, name + "/build.gradle", """
plugins { id 'org.jetbrains.kotlin.multiplatform' version '2.4.21' }
group = 'sample'
repositories { mavenCentral() }
kotlin { jvm(); jvmToolchain(25) }
""")
        put(root, name + "/src/jvmMain/kotlin/Other.kt", "package other\nclass Other\n")
    put(root, "library/src/main/kotlin/Library.kt", "package sample\nfun value(): Int = unfinished(\n")
    put(root, "app/src/main/kotlin/App.kt", "package sample\nfun use() = value()\n")
    run([tool, "--no-daemon", "--console=plain", "--max-workers=2", ":app:generate"], root)
    model = export(root, "gradle", tool, args.exporter, generated=["app/build/generated"])
    app = next(unit for unit in model["units"] if unit["project"] == ":app" and unit["extensions"]["compilation"] == "main")
    library = next(unit for unit in model["units"] if unit["project"] == ":library" and unit["extensions"]["compilation"] == "main")
    dependencies = [r for r in app["references"] if r["kind"] == "project" and r["attributes"]]
    assert {r["project"] for r in dependencies} == {":library", ":one", ":two"}
    assert {u["project"] for u in model["units"]} >= {":one", ":two"}
    assert len({u["id"] for u in model["units"]}) == len(model["units"])
    assert not any(r["kind"] == "project" and r["project"] == app["project"] for r in app["references"])
    assert dependencies[0]["attributes"]["org.jetbrains.kotlin.platform.type"] == "jvm"
    assert library["sources"] == ["library/src/main/kotlin/Library.kt"]
    assert any(r["kind"] == "sdk" and r["role"] == "runtime" for r in app["references"])
    assert app["extensions"]["jvm_target"] == "25"
    assert "app/build/generated/Generated.kt" in app["generated"]
    assert app["compiler_arguments"] and app["extensions"]["kgp_version"] == "2.4.21"
    assert not list(root.glob("*/build/libs/*.jar"))
    check_objects(root, model)
    original = {r["object"]["name"]: r["object"]["hash"] for u in model["units"] for r in u["references"] if r["kind"] == "artifact"}
    objects = Path(os.environ["FUGHETTA_STAGING"])
    resolution = json.loads((root / "model/dependencies.json").read_text())["resolution"]
    dependency = root.parent / (root.name + "-external.jar")
    try:
        with zipfile.ZipFile(dependency, "w") as jar:
            jar.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n")
        with (root / "one/build.gradle").open("a") as build:
            build.write("\nkotlin.sourceSets.jvmMain.dependencies { implementation files(" + json.dumps(str(dependency)) + ") }\n")
        changed = export(root, "gradle", tool, args.exporter, generated=["app/build/generated"])
        updated = {r["object"]["name"]: r["object"]["hash"] for u in changed["units"] for r in u["references"] if r["kind"] == "artifact"}
        assert original.items() <= updated.items()
        assert dependency.name in updated
        assert json.loads((root / "model/dependencies.json").read_text())["resolution"] != resolution
        check_objects(root, changed)
        with zipfile.ZipFile(dependency, "a") as jar:
            jar.writestr("changed.txt", "new resolved contents")
        try:
            export(root, "gradle", tool, args.exporter, generated=["app/build/generated"], prepare=False)
            raise AssertionError("stale composite dependency receipt was accepted")
        except RuntimeError as error:
            assert "Dependency resolution differs" in str(error)
        project_build = root / "one/build.gradle"
        before = project_build.read_text()
        try:
            with project_build.open("a") as build:
                build.write("\ntasks.named('compileKotlinJvm') { libraries.from(files('unclassified.jar')) }\n")
            try:
                export(root, "gradle", tool, args.exporter, generated=["app/build/generated"])
                raise AssertionError("unclassified compiler classpath input was omitted")
            except RuntimeError as error:
                assert "Unclassified compiler classpath input" in str(error)
        finally:
            project_build.write_text(before)
        with (root / "build.gradle").open("a") as build:
            build.write("\nproject(':library') { apply plugin: 'java-test-fixtures' }\n"
                        "project(':app') { dependencies { testImplementation(testFixtures(project(':library'))) } }\n")
        try:
            export(root, "gradle", tool, args.exporter, generated=["app/build/generated"])
            raise AssertionError("unexported project variant was accepted")
        except RuntimeError as error:
            assert "no unique exported JVM compilation" in str(error)
    finally:
        dependency.unlink(missing_ok=True)



def check_objects(root, model):
    assert model["schema"] == 1
    assert not (root / "model/dependencies").exists()
    assert str(root) not in json.dumps(model)
    assert "${tree}-" not in json.dumps(model)
    objects = Path(os.environ["FUGHETTA_STAGING"])
    if args.validator:
        print(run([args.validator, "--model", root / "model/model.json"], root).strip())
    for unit in model["units"]:
        for ref in [r["object"] for r in unit["references"] if r["kind"] == "artifact"] + list(unit["generated"].values()):
            assert (objects / ref["hash"]).is_file()
        for ref in unit["references"]:
            if ref["kind"] == "sdk":
                binding = next(b for b in json.loads(os.environ["FUGHETTA_SDKS"]) if b["name"] == ref["name"])
                assert any((Path(path) / ref["path"]).exists() for path in binding["roots"])


def fsharp(root, tool):
    repeated = subprocess.run([str(tool), str(args.exporter),
                               "--root", str(root), "--root", str(root), "--operations", "model"], cwd=root, text=True, capture_output=True)
    assert repeated.returncode != 0 and "--root" in repeated.stderr
    put(root, "library/Library.fsproj", """<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup>
<ItemGroup><Compile Include="Library.fsi"/><Compile Include="Library.fs"/></ItemGroup>
</Project>""")
    put(root, "library/Library.fsi", "namespace Sample\nmodule Library =\n    val value: int\n")
    put(root, "library/Library.fs", "namespace Sample\nmodule Library =\n    let value = unfinished(\n")
    put(root, "app/App.fsproj", """<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup>
<ItemGroup><Compile Include="App.fs"/><ProjectReference Include="../library/Library.fsproj" SetTargetFramework="RuntimeIdentifier=;TargetFramework=net10.0"/></ItemGroup>
</Project>""")
    put(root, "app/App.fs", "namespace Sample\nmodule App =\n    let result = Library.value\n")
    run([tool, "restore", "app/App.fsproj", "--verbosity", "quiet"], root)
    model = export(root, "msbuild", tool, args.exporter, ["app/App.fsproj"])
    app = next(unit for unit in model["units"] if unit["extensions"]["assembly"] == "App")
    library = next(unit for unit in model["units"] if unit["extensions"]["assembly"] == "Library")
    assert library["sources"][-2:] == ["library/Library.fsi", "library/Library.fs"]
    assert [r["unit"] for r in app["references"] if r["kind"] == "project" and r["unit"]] == [library["id"]]
    assert library["generated"] and library["references"] and library["compiler_arguments"]
    assert not list(root.glob("*/bin/**/*.dll"))
    assert any(r["kind"] == "sdk" for r in app["references"])
    assert "" not in library["extensions"]["defines"]
    assert library["extensions"]["type_providers"] is False and app["extensions"]["type_providers"] is False
    check_objects(root, model)
    receipt = json.loads((root / "model/assets.json").read_text())
    assert {entry["path"] for entry in receipt["files"]} == {"app/obj/project.assets.json", "library/obj/project.assets.json"}
    with (root / "library/obj/project.assets.json").open("a") as assets:
        assets.write("\n")
    try:
        export(root, "msbuild", tool, args.exporter, ["app/App.fsproj"], prepare=False)
        raise AssertionError("stale transitive restore receipt was accepted")
    except RuntimeError as error:
        assert "Restored assets differ" in str(error)
    # Updating a project model replaces copied generated text and preserves ordered input.
    put(root, "library/Last.fs", "namespace Sample\nmodule Last = let n = 2\n")
    project = root / "library/Library.fsproj"
    project.write_text(project.read_text().replace('</ItemGroup>', '<Compile Include="Last.fs"/></ItemGroup>'))
    changed = export(root, "msbuild", tool, args.exporter, ["app/App.fsproj"])
    updated = next(unit for unit in changed["units"] if unit["extensions"]["assembly"] == "Library")
    assert updated["sources"][-3:] == ["library/Library.fsi", "library/Library.fs", "library/Last.fs"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=["kotlin", "fsharp"])
    parser.add_argument("tool")
    parser.add_argument("exporter", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--validator")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.work, prefix=args.kind + "-") as directory:
        root = Path(directory).resolve()
        objects = root.parent / (root.name + "-objects")
        sdk = Path(shutil.which("java")).resolve().parent.parent if args.kind == "kotlin" else Path(args.tool).resolve().parent
        os.environ["FUGHETTA_STAGING"] = str(objects)
        os.environ["FUGHETTA_SDKS"] = json.dumps([{"name": "sdk/java" if args.kind == "kotlin" else "sdk/dotnet", "object": "installed-sdk", "roots": [str(sdk)]}])
        if args.kind == "kotlin":
            # Gradle keeps its home, the Kotlin daemon its run files, under the step's own root.
            os.environ["GRADLE_USER_HOME"] = str(root / "gradle-home")
            os.environ["JAVA_TOOL_OPTIONS"] = "-Dkotlin.daemon.options=runFilesPath=" + str(root / "daemon")
        try:
            globals()[args.kind](root, args.tool)
        finally:
            shutil.rmtree(objects, ignore_errors=True)
    print(args.kind + " evaluated model passed")
