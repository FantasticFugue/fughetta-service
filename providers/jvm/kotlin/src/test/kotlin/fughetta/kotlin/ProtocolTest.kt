package fughetta.kotlin

import java.io.ByteArrayOutputStream
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.serialization.json.*
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

internal typealias MetadataRows = List<String>
internal fun MetadataRows.metadataSize(): Int = size
internal class FriendLibrary { fun value(): Int = 42 }
internal class AarCaller { fun callAar(value: String): Int = listOf(value).metadataSize() }

class ProtocolTest {
    @Test
    fun readsKotlinDeclarationsFromAndroidArchive() {
        fun zip(vararg entries: Pair<String, ByteArray>): ByteArray = ByteArrayOutputStream().also { bytes ->
            ZipOutputStream(bytes).use { archive ->
                for ((name, data) in entries) {
                    archive.putNextEntry(ZipEntry(name))
                    archive.write(data)
                }
            }
        }.toByteArray()
        fun classBytes(name: String) = checkNotNull(javaClass.getResourceAsStream("$name.class")).use { it.readBytes() }

        val path = Files.createTempFile("metadata-", ".aar")
        try {
            val primaryClass = classBytes("AarCaller")
            val primary = zip("fughetta/kotlin/AarCaller.class" to primaryClass,
                "classes.jar" to zip("fughetta/kotlin/FriendLibrary.class" to classBytes("FriendLibrary")))
            val declarationsJar = zip("fughetta/kotlin/ProtocolTestKt.class" to classBytes("ProtocolTestKt"))
            Files.write(path, zip("AndroidManifest.xml" to "<manifest package=\"fughetta.kotlin\"/>".toByteArray(),
                "libs/helper.jar" to declarationsJar, "classes.jar" to primary))
            val classes = Metadata.read(path, 0).getValue("classes").jsonArray.map { it.jsonObject }
                .associateBy { it.getValue("owner").jsonPrimitive.content }
            assertEquals(listOf("fughetta/kotlin/AarCaller", "fughetta/kotlin/ProtocolTestKt"), classes.keys.toList())
            val facade = classes.getValue("fughetta/kotlin/ProtocolTestKt")
            val declarations = facade.getValue("declarations").jsonArray.map { it.jsonObject }
                .associateBy { it.getValue("name").jsonPrimitive.content }
            assertEquals("type", declarations.getValue("MetadataRows").getValue("kind").jsonPrimitive.content)
            val signature = declarations.getValue("metadataSize").getValue("type")
            assertTrue(facade.getValue("types").jsonArray.map { it.jsonObject }.single { it["id"] == signature }.containsKey("receiver"))
            System.getenv("FUGHETTA_JVM_TEST")?.let { executable ->
                val child = ProcessBuilder(executable, "--aar", path.toString(), Path.of(System.getProperty("java.home"), "bin", "java").toString(),
                    System.getProperty("fughetta.adapter.jar")).inheritIO().start()
                try {
                    assertTrue(child.waitFor(90, TimeUnit.SECONDS), "native AAR test timed out")
                    assertEquals(0, child.exitValue())
                } finally { child.destroyForcibly() }
            }

            Files.write(path, zip("classes.jar" to primary, "libs/duplicate.jar" to zip("alias.class" to primaryClass)))
            val duplicate = assertFailsWith<IllegalArgumentException> { Metadata.read(path, 0) }
            assertEquals("duplicate JVM class in artifact: fughetta/kotlin/AarCaller", duplicate.message)
        } finally { Files.deleteIfExists(path) }
    }

    @Test
    fun readsKotlinUnitsFromMixedLanguageModel() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "mixed-model-")
        try {
            val classes = Files.createDirectories(root.resolve("library/build/classes"))
            val jdk = Path.of(System.getProperty("java.home"))
            val input = Model(root, root.resolve("objects"), mapOf("jdk" to jdk)).read(
                """
                {
                  "schema": 1,
                  "producer": "test",
                  "producer_version": "current",
                  "units": [
                    {
                      "id": "kotlin-main", "project": "app", "language": "kotlin", "variants": [],
                      "directory": "app", "sources": ["app/Main.kt"], "generated": {},
                      "compiler_arguments": [], "outputs": [], "extensions": {},
                      "references": [
                        {"kind": "sdk", "name": "jdk", "path": "", "role": "runtime"},
                        {"kind": "project", "project": "library", "unit": "", "role": "classpath",
                         "attributes": {"usage": "api"}}
                      ]
                    },
                    {
                      "id": "scala-main", "project": "library", "language": "scala",
                      "directory": "library", "variants": [{"usage": "api"}], "sources": ["library/Library.scala"],
                      "generated": {}, "references": [], "compiler_arguments": [], "extensions": {},
                      "outputs": ["library/build/classes", "library/build/resources"]
                    }
                  ]
                }
                """.trimIndent(),
                mapOf("library/build/classes/Library.class" to "class-object"),
            ).single()
            assertEquals("kotlin-main", input.id)
            assertEquals(emptyList(), input.dependencies)
            assertEquals(listOf(classes), input.libraries)
            assertEquals(mapOf("library/build/classes/Library.class" to "class-object"), input.binaryInputs)
        } finally {
            root.toFile().deleteRecursively()
        }
    }

    @Test
    fun subprocessInstallsModelAndEvaluatesUpdatedSource() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "protocol-")
        val source = "import kotlin.String as Text\nimport kotlin.collections.*\nfun answer() = listOf(42).first()\nval transform: (Int) -> Int = { it + 1 }\nclass Empty\nclass Box(var value: Int)\nfun make() = Empty()\nfun friendValue() = fughetta.kotlin.FriendLibrary().value()\n"
        Files.writeString(root.resolve("Main.kt"), source)
        Files.writeString(root.resolve("JavaApi.java"), "public class JavaApi { public static int value() { return 42; } }")
        val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
        val objects = Files.createDirectories(root.resolve("objects"))
        fun installAar(classes: ByteArray, extra: ByteArray? = null): String {
            val archive = ByteArrayOutputStream()
            ZipOutputStream(archive).use { aar ->
                aar.putNextEntry(ZipEntry("classes.jar"))
                aar.write(classes)
                if (extra != null) {
                    aar.putNextEntry(ZipEntry("libs/friend.jar"))
                    aar.write(extra)
                }
            }
            val bytes = archive.toByteArray()
            val hash = java.security.MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
            Files.write(objects.resolve(hash), bytes)
            return hash
        }
        val libraryHash = installAar(Files.readAllBytes(stdlib))
        fun jar(className: String): ByteArray = ByteArrayOutputStream().also { bytes ->
            ZipOutputStream(bytes).use { jar ->
                jar.putNextEntry(ZipEntry("fughetta/kotlin/$className.class"))
                checkNotNull(javaClass.getResourceAsStream("$className.class")).use { it.copyTo(jar) }
            }
        }.toByteArray()
        val primaryJar = jar("ProtocolTestKt")
        val friendHash = installAar(primaryJar, jar("FriendLibrary"))
        val friendClasspath = Files.write(objects.resolve("$friendHash-classes.jar"), primaryJar)
        val kotlinArguments = listOf("-cp", "${'$'}{object:$libraryHash}", "-Xfriend-paths=${'$'}{object:$friendHash}")
        val empty = JsonArray(emptyList())
        val model = buildJsonObject {
            put("schema", 1); put("producer", "test"); put("producer_version", "current")
            putJsonArray("units") { add(buildJsonObject {
                put("id", "main"); put("project", "project"); put("language", "kotlin"); put("directory", "")
                putJsonArray("sources") { add("Main.kt"); add("JavaApi.java") }; put("generated", buildJsonObject {})
                putJsonArray("references") {
                    add(buildJsonObject { put("kind", "sdk"); put("name", "jdk"); put("path", ""); put("role", "runtime") })
                    add(buildJsonObject {
                        put("kind", "artifact"); put("role", "classpath")
                        putJsonObject("object") { put("hash", libraryHash); put("name", "stdlib.aar") }
                    })
                    add(buildJsonObject {
                        put("kind", "artifact"); put("role", "friend")
                        putJsonObject("object") { put("hash", friendHash); put("name", "friend.aar") }
                    })
                }
                put("outputs", empty); put("compiler_arguments", JsonArray(kotlinArguments.map(::JsonPrimitive)))
                put("variants", empty); put("extensions", buildJsonObject {})
            }) }
        }
        val input = Model(root, objects, mapOf("jdk" to Path.of(System.getProperty("java.home"))))
            .read(model.toString(), emptyMap()).single()
        assertEquals(listOf("-cp", objects.resolve("$libraryHash-classes.jar").toString(), "-Xfriend-paths=$friendClasspath"),
            input.compilerArguments)
        Files.writeString(root.resolve("model.json"), model.toString())
        System.getenv("FUGHETTA_WORKER_TEST")?.let { executable ->
            val worker = ProcessBuilder(executable, "--kotlin", root.toString(),
                Path.of(System.getProperty("java.home"), "bin", "java").toString(),
                System.getProperty("fughetta.host.jar"), System.getProperty("fughetta.adapter.jar"),
                System.getProperty("java.home"), stdlib.parent.toString())
                .inheritIO().start()
            try {
                assertTrue(worker.waitFor(90, TimeUnit.SECONDS), "worker Kotlin analysis timed out")
                assertEquals(0, worker.exitValue())
            } finally { worker.destroyForcibly() }
        }
        val androidSdk = root.resolve("android-sdk")
        val androidJarPath = "platforms/android-35/android.jar"
        val androidJar = androidSdk.resolve(androidJarPath)
        Files.createDirectories(androidJar.parent)
        val contextSource = Files.writeString(root.resolve("Context.java"),
            "package android.content; public class Context { public String getPackageName() { return \"demo\"; } }")
        val androidClasses = Files.createDirectory(root.resolve("android-classes"))
        assertEquals(0, javax.tools.ToolProvider.getSystemJavaCompiler().run(null, null, null,
            "-d", androidClasses.toString(), contextSource.toString()))
        ZipOutputStream(Files.newOutputStream(androidJar)).use { jar ->
            jar.putNextEntry(ZipEntry("android/content/Context.class"))
            Files.copy(androidClasses.resolve("android/content/Context.class"), jar)
        }
        val sdkBindings = mapOf("jdk" to System.getProperty("java.home"), "sdk/android" to androidSdk.toString())
        // The subprocess session also consumes a source module, so its exported
        // read-set and the host's current-value observation share a real graph.
        Files.writeString(root.resolve("Dependency.kt"), "package dependency\nfun value(): Int = 1\n")
        val originalUnit = model.getValue("units").jsonArray.single().jsonObject
        val mainUnit = JsonObject(originalUnit + ("references" to JsonArray(originalUnit.getValue("references").jsonArray + buildJsonObject {
            put("kind", "sdk"); put("name", "sdk/android"); put("path", androidJarPath); put("role", "classpath")
        })))
        val dependency = JsonObject(mainUnit.toMutableMap().apply {
            put("id", JsonPrimitive("dependency")); put("project", JsonPrimitive("dependency"))
            put("sources", JsonArray(listOf(JsonPrimitive("Dependency.kt"))))
        })
        val main = JsonObject(mainUnit.toMutableMap().apply {
            put("references", JsonArray(mainUnit.getValue("references").jsonArray + buildJsonObject {
                put("kind", "project"); put("project", "dependency"); put("unit", "dependency"); put("role", "classpath")
                put("path", ""); put("attributes", buildJsonObject {})
            }))
        })
        val modelPath = Files.writeString(root.resolve("model.json"), JsonObject(model.toMutableMap().apply {
            put("units", JsonArray(listOf(main, dependency)))
        }).toString())
        System.getenv("FUGHETTA_RECORD_VALIDATOR")?.let { validator ->
            val bindings = JsonArray(sdkBindings.map { (name, location) -> buildJsonObject {
                put("name", name); put("object", name); putJsonArray("roots") { add(location) }
            } })
            val checked = ProcessBuilder(validator, "--model", modelPath.toString()).apply {
                environment()["FUGHETTA_SDKS"] = bindings.toString()
                environment()["FUGHETTA_STAGING"] = objects.toString()
            }.inheritIO().start()
            assertEquals(0, checked.waitFor())
        }
        val process = ProcessBuilder(Path.of(System.getProperty("java.home"), "bin", "java").toString(),
            "-Djava.io.tmpdir=$root", "-jar", System.getProperty("fughetta.host.jar"), "--adapter", System.getProperty("fughetta.adapter.jar"))
            .directory(root.toFile()).redirectError(ProcessBuilder.Redirect.INHERIT).start()
        try {
            val writer = process.outputStream.bufferedWriter()
            val reader = process.inputStream.bufferedReader()
            var next = 0
            val transcript = StringBuilder()
            fun call(operation: String, params: JsonObject): JsonElement {
                val request = ++next
                val message = buildJsonObject { put("request_id", request); put("operation", operation); put("params", params) }
                writer.write(message.toString())
                writer.newLine(); writer.flush()
                val reply = Json.parseToJsonElement(requireNotNull(reader.readLine()) { "helper exited during $operation" }).jsonObject
                transcript.append(buildJsonObject { put("message", message); put("response", reply) }).append('\n')
                assertEquals(request, reply.getValue("request_id").jsonPrimitive.int)
                assertFalse("error" in reply, reply["error"].toString())
                return reply.getValue("result")
            }
            fun state(epoch: Int) = buildJsonObject {
                put("id", "session"); put("slot", "slot"); put("project", "project"); put("provider", "workspace-kotlin")
                put("stage", "analysis"); put("options", "options"); put("snapshot", "snapshot-$epoch")
                put("epoch", epoch); put("inputs", empty); put("phase", "ready")
            }
            call("open", buildJsonObject {
                put("session", state(0)); putJsonObject("initialization") {}; putJsonObject("host") {
                    put("objects", root.resolve("objects").toString())
                    putJsonObject("sdks") { sdkBindings.forEach { (name, path) -> put(name, path) } }
                }
            })
            val demand = buildJsonObject {
                putJsonArray("files") { add("Main.kt") }; put("modules", empty); put("contexts", empty); put("projects", empty)
                putJsonArray("facts") { add("symbols"); add("types"); add("definitions"); add("typed_ranges") }
            }
            val sourceInputs = mutableMapOf<String, String>()
            var currentSource = source
            fun update(epoch: Int, text: String) {
                currentSource = text
                call("update", buildJsonObject {
                    putJsonObject("sources") { sourceInputs.forEach { (path, version) -> put(path, version) } }
                    put("session", state(epoch)); putJsonObject("artifacts") { put("model.json", "model-object") }
                    putJsonObject("generated") {}; putJsonObject("facts") {}; putJsonArray("changed") { add("Main.kt") }
                    putJsonArray("documents") { add(buildJsonObject { put("path", "Main.kt"); put("language", "kotlin"); put("text", text) }) }
                })
            }
            update(1, source + "fun androidAnswer(context: android.content.Context) = context.packageName + fughetta.kotlin.FriendLibrary().value()\n")
            val units = call("units", buildJsonObject { put("epoch", 1); put("demand", demand) }).jsonArray
            assertEquals(1, units.size)
            val key = units.single()
            val template = buildJsonObject {
                put("id", "unit"); put("context", "context")
                putJsonObject("recipe") { put("unit", key); put("options", "options") }
                listOf("direct_inputs", "observed_dependencies", "channel_definitions", "conventions", "output_channels", "modules").forEach { put(it, empty) }
                putJsonObject("contributions") {
                    listOf("files", "occurrences", "references", "scopes", "relations", "diagnostics", "symbols", "types").forEach { put(it, empty) }
                }
                putJsonObject("coverage") {
                    put("requested", demand); put("available", empty); put("missing", empty); put("unresolved_count", 0)
                }
                put("cacheable", true)
            }
            fun evaluate(epoch: Int): String {
                val result = call("evaluate", buildJsonObject {
                    put("unit", template); putJsonObject("request") {
                        putJsonObject("execution") { put("request_id", "request"); put("computation_id", "computation"); put("stage_id", "analysis") }
                        put("epoch", epoch); put("slot_id", "slot"); put("unit", key)
                        put("inputs", empty); put("demand", demand); putJsonObject("budget") {}; put("retained_only", false)
                    }
                }).jsonObject
                assertEquals(JsonArray(emptyList()), result.getValue("receipts"))
                val shard = result.getValue("shards").jsonArray.single().jsonObject
                assertTrue(shard.getValue("identities").jsonArray.all {
                    it.jsonObject.getValue("declaration").jsonObject.getValue("provider").jsonPrimitive.content == "workspace-kotlin"
                })
                listOf("symbols", "occurrences", "references", "diagnostics", "scopes", "relations", "typed_ranges").forEach { name ->
                    assertTrue(shard.getValue(name).jsonArray.all {
                        it.jsonObject.getValue("ownership").jsonObject.getValue("origin").jsonObject.getValue("provider").jsonPrimitive.content == "workspace-kotlin"
                    })
                }
                val libraryModule = if (epoch >= 3) "tree:vendor/stdlib.jar" else "object:$libraryHash"
                assertTrue(shard.getValue("identities").jsonArray.map { it.jsonObject }.any {
                    it.getValue("declaration").jsonObject["module"]?.jsonPrimitive?.content == libraryModule
                })
                if (epoch == 1) {
                    assertFalse(shard.getValue("diagnostics").jsonArray.any {
                        it.jsonObject["severity"]?.jsonPrimitive?.content == "error"
                    }, shard.getValue("diagnostics").toString())
                    assertTrue(shard.getValue("identities").jsonArray.any {
                        it.jsonObject.getValue("declaration").jsonObject["module"]?.jsonPrimitive?.content == "object:$friendHash"
                    })
                    val androidAnswer = shard.getValue("symbols").jsonArray.map { it.jsonObject }
                        .single { it.getValue("name").jsonPrimitive.content == "androidAnswer" }
                    val types = shard.getValue("types").jsonArray.map { it.jsonObject }.associateBy { it.getValue("id") }
                    val returned = types.getValue(types.getValue(androidAnswer.getValue("type")).getValue("return_type"))
                    assertEquals("kotlin.String", returned.getValue("name").jsonPrimitive.content)
                    val reads = shard.getValue("unit").jsonObject.getValue("observed_dependencies").jsonArray
                    assertEquals(setOf("lookup", "body"), reads.map { it.jsonObject.getValue("kind").jsonPrimitive.content }.toSet())
                    val producers = result.getValue("producer_keys").jsonObject
                    assertEquals(reads.map { it.jsonObject.getValue("key").jsonPrimitive.content }.toSet(), producers.keys)
                    val dependencyKey = JsonObject(key.jsonObject.toMutableMap().apply { put("name", JsonPrimitive("dependency")) })
                    assertTrue(producers.values.all { it.jsonArray == JsonArray(listOf(dependencyKey)) })
                    val observed = call("observe", buildJsonObject { put("session", state(epoch)); put("reads", reads) }).jsonArray
                    assertEquals(reads.map { it.jsonObject.getValue("value") }.toSet(), observed.map { it.jsonObject.getValue("object") }.toSet())
                    val lambda = shard.getValue("symbols").jsonArray.map { it.jsonObject }.single { it["name"]?.jsonPrimitive?.content == "<lambda>" }
                    assertEquals("function", lambda.getValue("kind").jsonPrimitive.content)
                    val parameter = shard.getValue("identities").jsonArray.map { it.jsonObject.getValue("declaration").jsonObject }
                        .single { identity -> identity["owner"] == lambda["id"] &&
                            identity["native_key"]?.jsonPrimitive?.content?.let { Json.parseToJsonElement(it).jsonObject["kind"]?.jsonPrimitive?.content } == "value_parameter" }
                    assertTrue(!Json.parseToJsonElement(parameter.getValue("native_key").jsonPrimitive.content).jsonObject.containsKey("owner"))
                    assertTrue(lambda.getValue("modifiers").jsonArray.contains(JsonPrimitive("local")))
                    assertTrue(shard.getValue("scopes").jsonArray.map { it.jsonObject }.any { it["semantic_owner"] == lambda["id"] })
                    assertTrue(shard.getValue("references").jsonArray.map { it.jsonObject }.any { it["from"] == lambda["id"] })
                    assertTrue(shard.getValue("identities").jsonArray.map { it.jsonObject }.any {
                        it["symbol"] == lambda["id"] && it.getValue("declaration").jsonObject.containsKey("anchor")
                    })
                }
                if (epoch >= 4) {
                    val types = shard.getValue("types").jsonArray.map { it.jsonObject }.associateBy { it.getValue("id").jsonPrimitive.content }
                    val function = shard.getValue("symbols").jsonArray.map { it.jsonObject }.single { it["name"]?.jsonPrimitive?.content == "vendored" }
                    val signature = types.getValue(function.getValue("type").jsonPrimitive.content)
                    var returned = types.getValue(signature.getValue("return_type").jsonPrimitive.content)
                    if (returned["kind"]?.jsonPrimitive?.content == "flexible") returned = types.getValue(returned.getValue("lower_bound").jsonPrimitive.content)
                    assertEquals(if (epoch == 4) "kotlin.Int" else "kotlin.String", returned["name"]?.jsonPrimitive?.content)
                }
                val answer = shard.getValue("symbols").jsonArray.map { it.jsonObject }.single { it["name"]?.jsonPrimitive?.content == "answer" }
                val type = shard.getValue("types").jsonArray.map { it.jsonObject }.single { it["id"] == answer["type"] }
                val listCall = shard.getValue("typed_ranges").jsonArray.map { it.jsonObject }.single { record ->
                    val range = record.getValue("range").jsonObject
                    val text = currentSource.substring(range.getValue("begin").jsonPrimitive.int, range.getValue("end").jsonPrimitive.int)
                    text.startsWith("listOf(") && text.endsWith(")") && !text.contains(".first")
                }
                assertTrue(listCall["overload"] != null)
                val substitutions = listCall.getValue("substitutions").jsonObject
                assertTrue(substitutions.isNotEmpty())
                assertTrue(substitutions.values.any { selected ->
                    shard.getValue("types").jsonArray.map { it.jsonObject }.any {
                        it["id"] == selected && it["name"]?.jsonPrimitive?.content == if (epoch == 1) "kotlin.Int" else "kotlin.String"
                    }
                })
                System.getenv("FUGHETTA_RECORD_VALIDATOR")?.let { validator ->
                    val path = Files.writeString(root.resolve("shard.json"), shard.toString())
                    val command = if (epoch == 1) listOf(validator, "--android-shard", path.toString(), friendHash)
                        else listOf(validator, "--read-shard", path.toString())
                    val checked = ProcessBuilder(command).inheritIO().start()
                    assertEquals(0, checked.waitFor())
                }
                return shard.getValue("types").jsonArray.map { it.jsonObject }.single { it["id"] == type["return_type"] }
                    .getValue("name").jsonPrimitive.content
            }
            assertEquals("kotlin.Int", evaluate(1))
            update(2, "fun answer() = listOf(\"updated\").first()\n")
            assertEquals("kotlin.String", evaluate(2))
            val vendored = root.resolve("vendor/stdlib.jar")
            Files.createDirectories(vendored.parent)
            Files.copy(stdlib, vendored)
            val treeModel = JsonObject(model.toMutableMap().apply {
                put("units", JsonArray(model.getValue("units").jsonArray.map { unit ->
                    JsonObject(unit.jsonObject.toMutableMap().apply {
                        put("references", JsonArray(unit.jsonObject.getValue("references").jsonArray.map { reference ->
                            if (reference.jsonObject["kind"]?.jsonPrimitive?.content == "artifact" &&
                                reference.jsonObject["role"]?.jsonPrimitive?.content == "classpath") buildJsonObject {
                                put("kind", "tree"); put("path", "vendor/stdlib.jar"); put("role", "classpath")
                            } else reference
                        }))
                    })
                }))
            })
            Files.writeString(root.resolve("model.json"), treeModel.toString())
            sourceInputs["vendor/stdlib.jar"] = "stdlib-input"
            update(3, currentSource)
            assertEquals("kotlin.String", evaluate(3))
            fun writeLibrary(type: String, value: String) {
                val input = root.resolve("vendor-src/Vendor.java")
                val classes = root.resolve("vendor-classes")
                Files.createDirectories(input.parent); Files.createDirectories(classes)
                Files.writeString(input, "public class Vendor { public static $type value() { return $value; } }")
                assertEquals(0, javax.tools.ToolProvider.getSystemJavaCompiler().run(null, null, null,
                    "-d", classes.toString(), input.toString()))
                java.util.jar.JarOutputStream(Files.newOutputStream(root.resolve("vendor/library.jar"))).use { jar ->
                    jar.putNextEntry(java.util.jar.JarEntry("Vendor.class"))
                    Files.copy(classes.resolve("Vendor.class"), jar)
                    jar.closeEntry()
                }
            }
            val mutableModel = JsonObject(treeModel.toMutableMap().apply {
                put("units", JsonArray(treeModel.getValue("units").jsonArray.map { unit ->
                    JsonObject(unit.jsonObject.toMutableMap().apply {
                        put("references", JsonArray(unit.jsonObject.getValue("references").jsonArray + buildJsonObject {
                            put("kind", "tree"); put("path", "vendor/library.jar"); put("role", "classpath")
                        }))
                    })
                }))
            })
            Files.writeString(root.resolve("model.json"), mutableModel.toString())
            writeLibrary("int", "42")
            sourceInputs["vendor/library.jar"] = "library-input-one"
            update(4, currentSource + "fun vendored() = Vendor.value()\n")
            assertEquals("kotlin.String", evaluate(4))
            writeLibrary("String", "\"updated\"")
            sourceInputs["vendor/library.jar"] = "library-input-two"
            update(5, currentSource)
            assertEquals("kotlin.String", evaluate(5))
            call("close", buildJsonObject {})
            assertTrue(process.waitFor(30, TimeUnit.SECONDS))
            assertEquals(0, process.exitValue())
            System.getenv("FUGHETTA_RECORD_VALIDATOR")?.let { validator ->
                val path = Files.writeString(root.resolve("transcript.jsonl"), transcript)
                assertEquals(0, ProcessBuilder(validator, "--helper", path.toString()).inheritIO().start().waitFor())
            }
        } finally {
            process.destroyForcibly()
            process.waitFor()
            root.toFile().deleteRecursively()
        }
    }
}
