"""Compile a real JVM artifact and query the reader's published records."""
from pathlib import Path
import subprocess
import json
import os
import shutil
import sys
import tempfile
import zipfile
import re

def kotlin_flow(binary, gradle, exporter, java, host, helper, validator, compiler, work):
    from managed import export, put, run
    binary, java, host, helper, validator = [str(Path(value).resolve()) for value in (binary, java, host, helper, validator)]
    # The compiler may be an argv[0]-selected LLVM driver symlink.
    compiler = str(Path(compiler).absolute())
    Path(work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="kotlin-jni-", dir=work) as directory, tempfile.TemporaryDirectory(prefix="kotlin-library-", dir=work) as library_directory:
        root = Path(directory).resolve()
        library_root = Path(library_directory).resolve()
        sdk = str(Path(java).parent.parent)
        objects = root / "objects"
        # Gradle keeps its home, the Kotlin daemon its run files, under the step's own root.
        os.environ.update(TMPDIR=str(root), TEST_JAVA=java, FUGHETTA_STAGING=str(objects),
            JAVA_TOOL_OPTIONS="-Djava.io.tmpdir=" + str(root) + " -Dkotlin.daemon.options=runFilesPath=" + str(root / "daemon"),
            GRADLE_USER_HOME=str(root / "gradle-home"),
            FUGHETTA_SDKS=json.dumps([dict(name="sdk/java", object="jdk", roots=[sdk])]))
        put(root, "settings.gradle", "rootProject.name = 'sample'\n")
        put(root, "build.gradle", """
plugins { id 'org.jetbrains.kotlin.jvm' version '2.4.21' }
repositories { mavenCentral() }
kotlin { jvmToolchain(25) }
dependencies { implementation(files("@LIBRARY@")) }
tasks.register('verifyNative', JavaExec) {
    dependsOn tasks.jar
    classpath = files(tasks.jar.archiveFile) + configurations.runtimeClasspath
    mainClass = 'sample.SampleKt'
    jvmArgs '-Djava.library.path=' + rootDir
    args providers.gradleProperty('expected').getOrElse('42')
}
""")
        # This is a binary library, built separately from the source model.
        put(library_root, "settings.gradle", "rootProject.name = 'library'\n")
        put(library_root, "build.gradle", """
plugins { id 'org.jetbrains.kotlin.jvm' version '2.4.21' }
repositories { mavenCentral() }
kotlin { jvmToolchain(25) }
""")
        put(library_root, "src/main/kotlin/library/Library.kt", """package library
class Box<T:CharSequence>(val value:T) {
    @JvmOverloads fun decorate(suffix:String?=null):String = value.toString()+suffix.orEmpty()
    companion object {
        @JvmField val answer = 0
        @JvmStatic fun create(value:String):Box<String> = Box(value)
    }
}
fun <T:CharSequence> choose(value:T):T=value
""")
        run([gradle, "--no-daemon", "--console=plain", "--max-workers=2", "-p", library_root, "jar"], root)
        library = library_root / "build/libs/library.jar"
        script = root / "build.gradle"
        script.write_text(script.read_text().replace("@LIBRARY@", library.as_posix()))
        put(root, "library-metadata.json", run([java, "-jar", helper, "--jvm-metadata", "25", library], root))
        source = """package sample
import library.Box
import library.choose
class Bridge {
    external fun nativeTwice(value: Int): Int
}
fun run(value: Int): Int = Bridge().nativeTwice(value) + Box(choose("ready")).decorate().length - 5 + Box.answer
fun main(args: Array<String>) {
    System.loadLibrary("native")
    check(run(21) == args.single().toInt())
    println("verified:" + run(21))
}
"""
        path = "src/main/kotlin/sample/Sample.kt"
        put(root, path, source)
        model = export(root, "gradle", gradle, exporter)
        assert not (root / "build/classes/kotlin/main").exists(), "model preparation compiled Kotlin"
        unit = next(unit for unit in model["units"] if unit["sources"] and unit["extensions"]["compilation"] == "main")
        put(root, "native.cpp", """#include <jni.h>
extern "C" {
static __attribute__((noinline)) jint native_core(jint value) { return value * 2; }
__attribute__((visibility("hidden"))) jint registered_twice(JNIEnv*, jobject, jint value) { return native_core(value); }
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass klass = env->FindClass("sample/Bridge");
    if (!klass) return JNI_ERR;
    static const JNINativeMethod methods[] = {{const_cast<char*>("nativeTwice"), const_cast<char*>("(I)I"), reinterpret_cast<void*>(registered_twice)}};
    if (env->RegisterNatives(klass, methods, 1) != JNI_OK) return JNI_ERR;
    return JNI_VERSION_1_6;
}
}
""")
        run([compiler, "-shared", "-fPIC", "-g", "-I" + sdk + "/include", "-I" + sdk + "/include/linux", "native.cpp", "-o", "libnative.so"], root)
        run([compiler, "-S", "-emit-llvm", "-fPIC", "-g", "-I" + sdk + "/include", "-I" + sdk + "/include/linux", "native.cpp", "-o", "native.ll"], root)
        run([compiler, "-c", "-fPIC", "-g", "-I" + sdk + "/include", "-I" + sdk + "/include/linux", "native.cpp", "-o", "native.o"], root)
        readobj = os.environ.get("TEST_LLVM_READOBJ") or shutil.which("llvm-readobj")
        if not readobj:
            raise RuntimeError("the Kotlin JNI flow requires llvm-readobj")
        put(root, "native.json", run([readobj, "--elf-output-style=JSON", "--file-headers", "--symbols", "--dyn-symbols", "libnative.so"], root))
        put(root, "native-object.json", run([readobj, "--elf-output-style=JSON", "--file-headers", "--symbols", "--dyn-symbols", "native.o"], root))
        put(root, "inline.cpp", """__attribute__((always_inline)) inline int inline_leaf(int value) {
    volatile int doubled = value * 2;
    return doubled + 1;
}
extern "C" int optimized(int value) { return inline_leaf(value) + 3; }
""")
        run([compiler, "-O2", "-g", "-shared", "-fPIC", "inline.cpp", "-o", "inline.so"], root)
        objdump = os.environ.get("TEST_LLVM_OBJDUMP") or shutil.which("llvm-objdump")
        symbolizer = os.environ.get("TEST_LLVM_SYMBOLIZER") or shutil.which("llvm-symbolizer")
        if not objdump or not symbolizer:
            raise RuntimeError("native inline mapping requires llvm-objdump and llvm-symbolizer")
        instructions = run([objdump, "--disassemble", "--no-show-raw-insn", "inline.so"], root)
        put(root, "inline.asm", instructions)
        addresses = sorted(set(re.findall(r"^\s*([0-9a-fA-F]+):", instructions, re.M)))
        put(root, "inline-locations.json", run([symbolizer, "--output-style=JSON", "--inlines", "--no-demangle", "--obj=inline.so", *["0x" + at for at in addresses]], root))
        put(root, "inline.json", run([readobj, "--elf-output-style=JSON", "--file-headers", "--symbols", "--dyn-symbols", "inline.so"], root))
        with (root / "helper.log").open("w") as errors:
            process = subprocess.Popen([java, "-jar", host, "--adapter", helper], cwd=root, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors)
            sequence = 0
            def call(operation, params):
                nonlocal sequence
                sequence += 1
                process.stdin.write(json.dumps(dict(request_id=sequence, operation=operation, params=params)) + "\n")
                process.stdin.flush()
                line = process.stdout.readline()
                if not line:
                    raise RuntimeError((root / "helper.log").read_text())
                response = json.loads(line)
                assert "error" not in response, response
                return response["result"]
            try:
                session = dict(id="session", slot="slot", project="app", provider="registered-kotlin", stage="source", options="options", epoch=0)
                call("open", dict(session=session, initialization={}, host=dict(objects=str(objects), sdks={"sdk/java": sdk})))
                demand = dict(files=[path], modules=[], contexts=[], projects=[], facts=["symbols", "definitions", "types", "references", "relations"])
                run([validator, "--unit", root / "unit.json"], root)
                for epoch, text, expected in ((1, source, 42), (2, source.replace("Bridge().nativeTwice(value)", "Bridge().nativeTwice(value) + 1"), 43)):
                    put(root, path, text)
                    session["epoch"] = epoch
                    call("update", dict(session=session, documents=[dict(path=path, language="kotlin", text=text)],
                        artifacts={"model/model.json": "model"}, sources={}, generated={}, changed=[path], facts={}))
                    keys = call("units", dict(epoch=epoch, demand=demand))
                    key = next(key for key in keys if key["name"] == unit["id"])
                    template = json.loads((root / "unit.json").read_text())
                    template.update(recipe=dict(unit=key, options="options"), coverage=dict(requested=demand))
                    result = call("evaluate", dict(request=dict(epoch=epoch, slot_id="slot", unit=key), unit=template))
                    shard, = result["shards"]
                    assert not [d for d in shard["diagnostics"] if d["severity"] == "error"], shard["diagnostics"]
                    put(root, f"source-{epoch}.json", json.dumps(shard))
                    put(root, f"source-{epoch}.kt", text)
                    run([validator, "--shard", root / f"source-{epoch}.json"], root)
                    result = run([gradle, "--no-daemon", "--console=plain", "--max-workers=2", "verifyNative", "-Pexpected=" + str(expected)], root)
                    assert "verified:" + str(expected) in result, result
                    shutil.copyfile(root / "build/libs/sample.jar", root / f"sample-{epoch}.jar")
                call("close", {})
                assert process.wait(timeout=15) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        subprocess.run([binary, "--kotlin", str(root / "sample-1.jar"), str(root / "sample-2.jar"),
            str(root / "source-1.json"), str(root / "source-2.json"), str(root / "source-1.kt"), str(root / "source-2.kt"), str(root / "native.json"), str(root / "native.ll"), str(root / "native-object.json"), str(library),
            str(root / "library-metadata.json"), str(root / "model/model.json"), helper,
            str(root / "inline.json"), str(root / "inline-locations.json"), str(root / "inline.cpp"), str(root / "inline.asm")], check=True)
    print("Kotlin source analysis, JAR, native JNI execution, binding and edit impact passed")

if len(sys.argv) > 1 and sys.argv[1] == "--kotlin":
    kotlin_flow(*sys.argv[2:])
    sys.exit(0)

binary, javac, work = sys.argv[1:]
Path(work).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="jvm-", dir=work) as directory:
    root = Path(directory)
    source = root / "Sample.java"
    source.write_text("""package sample;
import java.lang.annotation.*;
@Retention(RetentionPolicy.RUNTIME) @interface Details {
    int number(); String text(); Class<?> type(); RetentionPolicy policy();
    Nested nested(); int[] values(); float fraction(); double precise(); long large(); boolean enabled();
}
@interface Nested { String value(); }
@Retention(RetentionPolicy.CLASS) @interface Hidden { String value(); }
@Details(number=-7, text="class\\0😀", type=String[].class, policy=RetentionPolicy.RUNTIME,
    nested=@Nested("inside"), values={1, 2}, fraction=1.5f, precise=2.25, large=9223372036854775806L, enabled=true)
@Hidden("invisible")
public class Sample<T extends Number & Comparable<T>>
    extends java.util.ArrayList<java.util.List<? super T[]>> implements java.io.Serializable {
    @Hidden("field") public int field;
    @Hidden("method") public static int twice(int value) { return value * 2; }
    public int run(int value) {
        switch (value) {
            case 0: return twice(1);
            case 7: return twice(2);
            default: return twice(value) + Support.adjust(value);
        }
    }
    public String text(int value) { return "value=" + value; }
    public native int nativeEntry(long value);
    public native int nativeEntry(int value);
    public static native Object[] méthod_λ(String[] values, String text, int[] ints, int[][] matrix, Class<?> type, Object object, Throwable error);
}
""", encoding="utf-8")
    support = root / "Support.java"
    support.write_text("package sample; public class Support { public static final int VALUE = 1; public static int adjust(int value) { return value + VALUE; } }", encoding="utf-8")
    subprocess.run([javac, "-g", "-h", str(root), "-d", str(root), str(source), str(support)], check=True)
    member = root / "sample/Sample.class"
    jar = root / "sample.jar"
    with zipfile.ZipFile(jar, "w", compression=zipfile.ZIP_DEFLATED) as output:
        output.write(member, "sample/Sample.class")
        output.write(root / "sample/Support.class", "sample/Support.class")
    support.write_text(support.read_text().replace("VALUE = 1", "VALUE = 2"), encoding="utf-8")
    subprocess.run([javac, "-g", "-d", str(root), str(support)], check=True)
    changed = root / "changed.jar"
    with zipfile.ZipFile(changed, "w", compression=zipfile.ZIP_STORED) as output:
        # A different entry order and compression must preserve the unchanged class.
        output.write(root / "sample/Support.class", "sample/Support.class")
        output.write(member, "sample/Sample.class")
    subprocess.run([binary, str(member), str(jar), str(root / "sample_Sample.h"), str(changed)], check=True)
