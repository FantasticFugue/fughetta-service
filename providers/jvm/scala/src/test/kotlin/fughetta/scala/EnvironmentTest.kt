package fughetta.scala

import java.nio.file.Files
import java.nio.file.Path
import java.util.jar.JarEntry
import java.util.jar.JarOutputStream
import kotlin.io.path.ExperimentalPathApi
import kotlin.io.path.deleteRecursively
import kotlin.test.*
import kotlinx.serialization.json.*
import scala.reflect.io.AbstractFile

internal val javaHome = Path.of(System.getProperty("java.home"))
internal val scalaLibrary = Path.of(System.getProperty("fughetta.scala.library"))

@OptIn(ExperimentalPathApi::class)
internal fun inWorkspace(test: (Path) -> Unit) {
    val root = Files.createTempDirectory("scala-workspace-")
    try { test(root) } finally { root.deleteRecursively() }
}

internal fun jdkReference() = buildJsonObject {
    put("kind", "sdk"); put("role", "runtime"); put("name", "jdk"); put("path", "")
}

internal fun scalaReference() = buildJsonObject {
    put("kind", "sdk"); put("role", "classpath"); put("name", "scala"); put("path", scalaLibrary.fileName.toString())
}

internal fun projectReference(module: String) = buildJsonObject {
    put("kind", "project"); put("role", "classpath"); put("project", module); put("unit", module)
    putJsonObject("attributes") {}
}

internal fun unit(id: String, sources: List<String>, dependencies: List<JsonObject> = emptyList(),
                  outputs: List<String> = emptyList(), language: String = "scala",
                  arguments: List<String> = emptyList(), generated: JsonObject = buildJsonObject {}): JsonObject = buildJsonObject {
    put("id", id); put("project", id); put("language", language); put("directory", ".")
    put("sources", sources.strings()); put("outputs", outputs.strings())
    putJsonArray("variants") { add(buildJsonObject { put("usage", "api") }) }
    put("references", JsonArray(listOf(jdkReference(), scalaReference()) + dependencies))
    put("generated", generated); put("compiler_arguments", arguments.strings())
}

internal fun model(vararg units: JsonObject) = buildJsonObject {
    put("schema", 1); put("producer", "test"); put("producer_version", "current")
    put("units", JsonArray(units.toList()))
}

class EnvironmentTest {
    @Test
    fun resolvesInstalledMixedLanguageModel() = inWorkspace { root ->
        val output = Files.createDirectories(root.resolve("java/classes"))
        val objects = Files.createDirectories(root.resolve("objects"))
        val hash = "1".repeat(64)
        val installedJar = Files.copy(scalaLibrary, objects.resolve(hash))
        val artifact = buildJsonObject {
            put("kind", "artifact"); put("role", "classpath")
            putJsonObject("object") { put("hash", hash); put("name", "library.jar") }
        }
        val generated = buildJsonObject { putJsonObject("Generated.scala") { put("hash", "generated-text") } }
        val parser = Model(root, objects, mapOf("jdk" to javaHome, "scala" to scalaLibrary.parent))
        val inputs = parser.read(model(
            unit("app", listOf("Generated.scala"), listOf(projectReference("java"), projectReference("base"), artifact),
                arguments = listOf("-P:plugin:${'$'}{tree}/configuration"), generated = generated),
            unit("java", emptyList(), outputs = listOf("java/classes"), language = "java"),
            unit("base", emptyList()),
        ).toString(), mapOf("java/classes/Api.class" to "java-object"))
        val app = inputs.first()
        assertEquals(listOf("app", "base"), inputs.map { it.id })
        assertEquals(listOf("base"), app.dependencies)
        assertEquals(listOf(scalaLibrary, output, installedJar), app.libraries.map { it.path })
        assertEquals(mapOf("java/classes/Api.class" to "java-object"), app.binaryInputs)
        assertEquals(javaHome, app.jdk)
        assertEquals(listOf("-P:plugin:$root/configuration"), app.compilerArguments)
        assertEquals(mapOf(root.resolve("Generated.scala") to objects.resolve("generated-text")), app.generated)
    }

    @Test
    fun compilesVirtualScalaAndJavaExportsAndReadsScalaMetadata() = inWorkspace { root ->
        val library = Files.copy(scalaLibrary, root.resolve("sdk-object"))
        val inputs = ModuleInputs("core", listOf(root.resolve("Core.scala"), root.resolve("JavaApi.java")),
            listOf(Library(library, "object:stdlib")), javaHome, emptyList())
        val sources = listOf(
            Source(inputs.sources[0], """package demo
                |trait Service { def answer(i: Int): Int }
                |object Core extends Service { override def answer(i: Int): Int = JavaApi.increment(i) }
                |""".trimMargin(), false),
            Source(inputs.sources[1], """package demo;
                |public class JavaApi { public static int increment(int i) { return Core$.MODULE$.answer(i) + 1; } }
                |""".trimMargin(), false),
        )
        var typed = 0
        val output = Environment(inputs, emptyList()).compile(sources, export = true, typed = { compilation ->
            assertNotNull(semanticValues(compilation))
            typed++
        }).use {
            assertTrue(it.messages.isEmpty(), it.messages.toString())
            assertNotNull(it.exports)
        }
        assertEquals(1, typed)
        val app = inputs.copy(id = "app", sources = listOf(root.resolve("program.sc")), dependencies = listOf("core"))
        Environment(app, listOf(output)).compile(listOf(
            Source(app.sources.single(), "#!/usr/bin/env scala\nval value = demo.JavaApi.increment(demo.Core.answer(1))\n", false),
        )).use { assertTrue(it.messages.isEmpty(), it.messages.toString()) }

        Workspace(root, "project", "scala").use { workspace ->
            val script = app.sources.single()
            val mill = root.resolve("build.sc")
            val cli = root.resolve("cli.sc")
            val edits = sources.associate { it.file to it.text } + mapOf(
                script to "val value = demo.JavaApi.increment(demo.Core.answer(1))\n",
                mill to "import mill._\n",
                cli to "//> using dep example:library:1\n",
            )
            workspace.update(listOf(inputs.copy(directory = root)), edits, emptyMap())
            val scriptUnit = "script:program.sc"
            assertEquals(listOf(scriptUnit), workspace.units(setOf(script, mill, cli)))
            val shard = workspace.evaluate(scriptUnit).shard(buildJsonObject {
                put("id", "script-unit"); put("context", "script-context")
                putJsonObject("coverage") { putJsonObject("requested") { putJsonArray("facts") {} } }
            })
            assertTrue(shard.getValue("diagnostics").jsonArray.isEmpty(), shard.toString())
        }

        fun writeJar(path: Path, includes: (String) -> Boolean) {
            JarOutputStream(Files.newOutputStream(path)).use { jar ->
                fun add(directory: AbstractFile, prefix: String = "") {
                    directory.items().forEach {
                        val name = prefix + it.name()
                        if (it.isDirectory()) add(it, "$name/")
                        else if (includes(name)) {
                            jar.putNextEntry(JarEntry(name)); jar.write(it.toByteArray()); jar.closeEntry()
                        }
                    }
                }
                add(output)
            }
        }
        val archive = root.resolve("core.jar")
        val primary = root.resolve("primary.jar")
        val extension = root.resolve("extension.jar")
        writeJar(archive) { true }
        writeJar(primary) { it != "demo/JavaApi.class" }
        writeJar(extension) { it == "demo/JavaApi.class" }
        val objects = Files.createDirectories(root.resolve("objects"))
        val hash = "2".repeat(64)
        JarOutputStream(Files.newOutputStream(objects.resolve(hash))).use { aar ->
            aar.putNextEntry(JarEntry("classes.jar"))
            Files.copy(primary, aar)
            aar.putNextEntry(JarEntry("libs/extension.jar"))
            Files.copy(extension, aar)
        }
        val artifact = buildJsonObject {
            put("kind", "artifact"); put("role", "friend")
            putJsonObject("object") { put("hash", hash); put("name", "android.zip") }
        }
        val model = model(unit("app", listOf("program.sc"), listOf(artifact), arguments = listOf("-classpath", "${'$'}{object:$hash}")))
        val resolved = Model(root, objects, mapOf("jdk" to javaHome, "scala" to scalaLibrary.parent))
            .read(model.toString(), emptyMap()).single()
        val merged = objects.resolve("$hash-classes.jar")
        assertEquals(Library(merged, "object:$hash"), resolved.libraries.last())
        assertEquals(listOf("-classpath", merged.toString()), resolved.compilerArguments)
        Environment(resolved, emptyList()).compile(listOf(
            Source(resolved.sources.single(), "val value = demo.JavaApi.increment(demo.Core.answer(1))\n", false),
        )).use { assertTrue(it.messages.isEmpty(), it.messages.toString()) }

        val list = root.resolve("binaries.json")
        Files.writeString(list, listOf(archive.toString()).strings().toString())
        for ((option, path) in listOf("--jvm-metadata" to archive, "--jvm-metadata-list" to list)) {
            val child = ProcessBuilder(javaHome.resolve("bin/java").toString(), "-jar", System.getProperty("fughetta.adapter.jar"),
                option, Runtime.version().feature().toString(), path.toString()).redirectError(ProcessBuilder.Redirect.INHERIT).start()
            val report = Json.parseToJsonElement(child.inputStream.bufferedReader().readText()).jsonObject
            assertEquals(0, child.waitFor())
            val classes = report.rows("classes").map { it.jsonObject }
            assertTrue(classes.all { it.text("language") == "scala" && it.text("domain") == "scala" })
            assertTrue(classes.none { it.text("owner") == "demo/JavaApi" })
            val core = classes.single { it.text("owner") == "demo/Core$" }
            val method = core.rows("declarations").map { it.jsonObject }.single { it.text("name") == "answer" }
            assertEquals("(I)I", method.rows("jvm_members").single().jsonObject.text("descriptor"))
        }
    }
}
