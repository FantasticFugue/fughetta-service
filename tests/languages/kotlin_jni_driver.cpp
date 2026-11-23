#include "managed_session.hpp"

namespace {
    using namespace managed_test;

    constexpr std::string_view project = R"fixture(
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
)fixture";

    constexpr std::string_view library_project = R"fixture(
plugins { id 'org.jetbrains.kotlin.jvm' version '2.4.21' }
repositories { mavenCentral() }
kotlin { jvmToolchain(25) }
)fixture";

    constexpr std::string_view library_source = R"fixture(package library
class Box<T:CharSequence>(val value:T) {
    @JvmOverloads fun decorate(suffix:String?=null):String = value.toString()+suffix.orEmpty()
    companion object {
        @JvmField val answer = 0
        @JvmStatic fun create(value:String):Box<String> = Box(value)
    }
}
fun <T:CharSequence> choose(value:T):T=value
)fixture";

    constexpr std::string_view original = R"fixture(package sample
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
)fixture";

    constexpr std::string_view native_source = R"fixture(#include <jni.h>
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
)fixture";

    constexpr std::string_view inline_source = R"fixture(__attribute__((always_inline)) inline int inline_leaf(int value) {
    volatile int doubled = value * 2;
    return doubled + 1;
}
extern "C" int optimized(int value) { return inline_leaf(value) + 3; }
)fixture";

    auto scenario(driver& runtime, const char** argv) -> coroutine::task<> {
        const auto binary = fs::absolute(argv[1]).string();
        const std::string gradle = argv[2];
        const auto exporter = fs::absolute(argv[3]).string();
        const auto java = fs::canonical(argv[4]).string();
        const auto host = fs::absolute(argv[5]).string();
        const auto adapter = fs::absolute(argv[6]).string();
        const auto validator = fs::absolute(argv[7]).string();
        // LLVM selects its compiler mode from argv[0].
        const auto compiler = fs::absolute(argv[8]).string();
        const auto native_reader = fs::absolute(argv[10]).string();
        workspace work{argv[9], "kotlin-jni"}, dependency{argv[9], "kotlin-library"};
        const auto& root = work.root;
        const auto sdk = fs::path{java}.parent_path().parent_path();
        runtime.configure(root, "sdk/java", "jdk", sdk);
        runtime.env["JAVA_HOME"] = sdk.string();
        runtime.env["TEST_JAVA"] = java;
        runtime.env["JAVA_TOOL_OPTIONS"] = "-Djava.io.tmpdir=" + root.string() + " -Dkotlin.daemon.options=runFilesPath=" + (root / "daemon").string();
        runtime.env["GRADLE_USER_HOME"] = (root / "gradle-home").string();
        for (const auto& dir : {root, dependency.root})
            test::spit(dir / "gradle.properties", "org.gradle.java.installations.fromEnv=JAVA_HOME\n"
                       "org.gradle.java.installations.auto-detect=false\norg.gradle.java.installations.auto-download=false\n");

        const std::string plugin_repositories = "pluginManagement { repositories { mavenCentral(); gradlePluginPortal() } }\n";
        test::spit(dependency.root / "settings.gradle", plugin_repositories + "rootProject.name = 'library'\n");
        test::spit(dependency.root / "build.gradle", library_project);
        test::spit(dependency.root / "src/main/kotlin/library/Library.kt", library_source);
        co_await runtime.run({gradle, "--no-daemon", "--console=plain", "--max-workers=2", "-p", dependency.root.string(), "jar"}, root);
        const auto library = dependency.root / "build/libs/library.jar";
        test::spit(root / "build.gradle", replaced(std::string{project}, "\"@LIBRARY@\"", json::write(library.generic_string())));
        test::spit(root / "settings.gradle", plugin_repositories + "rootProject.name = 'sample'\n");
        test::spit(root / "library-metadata.json", co_await runtime.run({java, "-jar", adapter, "--jvm-metadata", "25", library.string()}, root));

        const std::string path = "src/main/kotlin/sample/Sample.kt";
        test::spit(root / path, original);
        const auto model = co_await runtime.export_model(root, "gradle", gradle, exporter);
        require(!fs::exists(root / "build/classes/kotlin/main"), "model preparation compiled Kotlin");
        const auto& unit = select(items(model, "units"), [](const value& row) {
            return !items(row, "sources").empty() && text(field(row, "extensions"), "compilation") == "main";
        }, "expected the main Kotlin compilation");

        test::spit(root / "native.cpp", native_source);
        const auto include = "-I" + (sdk / "include").string();
        const auto platform_include = "-I" + (sdk / "include/linux").string();
        co_await runtime.run({compiler, "-shared", "-fPIC", "-g", include, platform_include, "native.cpp", "-o", "libnative.so"}, root);
        co_await runtime.run({compiler, "-S", "-emit-llvm", "-fPIC", "-g", include, platform_include, "native.cpp", "-o", "native.ll"}, root);
        co_await runtime.run({compiler, "-c", "-fPIC", "-g", include, platform_include, "native.cpp", "-o", "native.o"}, root);
        test::spit(root / "native.json", co_await runtime.run({native_reader, "libnative.so", "--ir", "native.ll"}, root));
        test::spit(root / "native-object.json", co_await runtime.run({native_reader, "native.o"}, root));
        test::spit(root / "inline.cpp", inline_source);
        co_await runtime.run({compiler, "-O2", "-g", "-shared", "-fPIC", "inline.cpp", "-o", "inline.so"}, root);
        test::spit(root / "inline.json", co_await runtime.run({native_reader, "inline.so", "--lines"}, root));

        conversation helper{runtime};
        helper.start({java, "-jar", host, "--adapter", adapter}, root);
        auto state = session("registered-kotlin");
        co_await helper.call("open", opening(state, root, "sdk/java", sdk));
        const auto requested = demand({"symbols", "definitions", "types", "references", "relations"}, {path});
        co_await runtime.run({validator, "--unit", (root / "unit.json").string()}, root);
        auto unit_template = parse(test::slurp(root / "unit.json"));
        for (int epoch = 1; epoch <= 2; ++epoch) {
            const auto suffix = std::to_string(epoch);
            const auto source = epoch == 1 ? std::string{original}
                                           : replaced(std::string{original}, "Bridge().nativeTwice(value)", "Bridge().nativeTwice(value) + 1");
            const std::string expected = epoch == 1 ? "42" : "43";
            test::spit(root / path, source);
            state["epoch"] = epoch;
            co_await helper.call("update", installation(state, {document(path, "kotlin", source)}, "model", {}, path));
            const auto keys = co_await helper.call("units", {{"epoch", epoch}, {"demand", requested}});
            const auto& key = by(items(keys), "name", text(unit, "id"));
            fields(unit_template)["recipe"] = object{{"unit", key}, {"options", "options"}};
            fields(unit_template)["coverage"] = object{{"requested", requested}};
            const auto result = co_await helper.call("evaluate", {{"request", object{{"epoch", epoch}, {"slot_id", "slot"}, {"unit", key}}},
                                                                  {"unit", unit_template}});
            const auto& shard = only(items(result, "shards"));
            require(no_errors(shard), json::write(field(shard, "diagnostics")));
            const auto records = root / ("source-" + suffix + ".json");
            test::spit(records, json::write(shard));
            test::spit(root / ("source-" + suffix + ".kt"), source);
            co_await runtime.run({validator, "--shard", records.string()}, root);
            const auto output = co_await runtime.run({gradle, "--no-daemon", "--console=plain", "--max-workers=2", "verifyNative", "-Pexpected=" + expected}, root);
            require(output.contains("verified:" + expected), output);
            fs::copy_file(root / "build/libs/sample.jar", root / ("sample-" + suffix + ".jar"));
        }
        co_await helper.finish();
        co_await runtime.run({binary, "--kotlin", (root / "sample-1.jar").string(), (root / "sample-2.jar").string(),
                              (root / "source-1.json").string(), (root / "source-2.json").string(), (root / "source-1.kt").string(),
                              (root / "source-2.kt").string(), (root / "native.json").string(), (root / "native.ll").string(),
                              (root / "native-object.json").string(), library.string(), (root / "library-metadata.json").string(),
                              (root / "model/model.json").string(), adapter, (root / "inline.json").string(), (root / "inline.cpp").string()}, root);
        std::cout << "Kotlin source analysis, JAR, native JNI execution, binding and edit impact passed\n";
    }
}

int main(int argc, const char** argv) {
    try {
        test::require(argc == 11, "usage: kotlin-jni-driver <reader> <gradle> <exporter> <java> <host> <adapter> <validator> <compiler> <work> <native-host>");
        native::context context;
        managed_test::driver runtime{context.get_scheduler()};
        test::require(cores::run(context, scenario(runtime, argv)), "Kotlin JNI scenario stopped");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
