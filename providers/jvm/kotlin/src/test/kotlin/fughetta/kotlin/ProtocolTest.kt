package fughetta.kotlin

import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit
import kotlinx.serialization.json.*
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class ProtocolTest {
    @Test
    fun subprocessInstallsModelAndEvaluatesUpdatedSource() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "protocol-")
        val source = "import kotlin.String as Text\nimport kotlin.collections.*\nfun answer() = listOf(42).first()\nval transform: (Int) -> Int = { it + 1 }\nclass Empty\nclass Box(var value: Int)\nfun make() = Empty()\n"
        Files.writeString(root.resolve("Main.kt"), source)
        Files.writeString(root.resolve("JavaApi.java"), "public class JavaApi { public static int value() { return 42; } }")
        val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
        val libraryHash = java.security.MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(stdlib))
            .joinToString("") { "%02x".format(it) }
        val objects = Files.createDirectories(root.resolve("objects"))
        Files.copy(stdlib, objects.resolve(libraryHash))
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
                        putJsonObject("object") { put("hash", libraryHash); put("name", stdlib.fileName.toString()) }
                    })
                }
                put("outputs", empty); put("compiler_arguments", empty); put("variants", empty); put("extensions", buildJsonObject {})
            }) }
        }
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
        // The subprocess session also consumes a source module, so its exported
        // read-set and the host's current-value observation share a real graph.
        Files.writeString(root.resolve("Dependency.kt"), "package dependency\nfun value(): Int = 1\n")
        val mainUnit = model.getValue("units").jsonArray.single().jsonObject
        val dependency = JsonObject(mainUnit.toMutableMap().apply {
            put("id", JsonPrimitive("dependency")); put("project", JsonPrimitive("dependency"))
            put("sources", JsonArray(listOf(JsonPrimitive("Dependency.kt"))))
        })
        val main = JsonObject(mainUnit.toMutableMap().apply {
            put("references", JsonArray(mainUnit.getValue("references").jsonArray + buildJsonObject {
                put("kind", "project"); put("project", "dependency"); put("unit", "dependency"); put("role", "classpath")
                put("attributes", buildJsonObject {})
            }))
        })
        Files.writeString(root.resolve("model.json"), JsonObject(model.toMutableMap().apply {
            put("units", JsonArray(listOf(main, dependency)))
        }).toString())
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
                    putJsonObject("sdks") { put("jdk", System.getProperty("java.home")); put("stdlib", stdlib.parent.toString()) }
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
            update(1, source)
            val units = call("units", buildJsonObject { put("epoch", 1); put("demand", demand) }).jsonArray
            assertEquals(1, units.size)
            val key = units.single()
            val template = buildJsonObject {
                put("id", "unit"); put("context", "context")
                putJsonObject("recipe") { put("unit", key); put("options", "options") }
                listOf("direct_inputs", "observed_dependencies", "channel_definitions", "output_channels", "modules").forEach { put(it, empty) }
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
                val shard = result.getValue("shards").jsonArray.single().jsonObject
                assertTrue(shard.getValue("identities").jsonArray.all {
                    it.jsonObject.getValue("declaration").jsonObject.getValue("provider").jsonPrimitive.content == "workspace-kotlin"
                })
                listOf("symbols", "occurrences", "references", "diagnostics", "scopes", "relations", "typed_ranges").forEach { name ->
                    assertTrue(shard.getValue(name).jsonArray.all {
                        it.jsonObject.getValue("ownership").jsonObject.getValue("origin").jsonObject.getValue("provider").jsonPrimitive.content == "workspace-kotlin"
                    })
                }
                if (epoch >= 3) assertTrue(shard.getValue("identities").jsonArray.map { it.jsonObject }.any {
                    it.getValue("declaration").jsonObject["module"]?.jsonPrimitive?.content == "tree:vendor/stdlib.jar"
                })
                if (epoch == 1) {
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
                    // This fixture has no references; the typed reader still admits the wire structure.
                    val checked = ProcessBuilder(validator, "--read-shard", path.toString()).inheritIO().start()
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
                            if (reference.jsonObject["kind"]?.jsonPrimitive?.content == "artifact") buildJsonObject {
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
