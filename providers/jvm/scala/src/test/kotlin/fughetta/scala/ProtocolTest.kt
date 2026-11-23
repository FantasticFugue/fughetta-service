package fughetta.scala

import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit
import kotlin.test.*
import kotlinx.serialization.json.*

class ProtocolTest {
    @Test
    fun projectsScalaSemanticsAndObservesProducerChannelsAcrossUpdates() = inWorkspace { root ->
        val upstream = """
            package dep
            trait Base { def run(x: Int): Int }
            object Values {
              implicit val suffix: String = "!"
              @inline def inlineValue(x: Int): Int = x + 10
              def plain(x: Int): Int = x + 1
              def pick[A](x: => A)(implicit suffix: String): (A, String) = (x, suffix)
              def choose(x: Int): Int = x
              def choose(x: String): String = x
            }
        """.trimIndent()
        val source = """
            package app
            import dep.{Base => Parent, Values}
            import Values._
            final class Main extends Parent {
              @native def nativeCall(x: Int): Long
              override def run(x: Int): Int = {
                val emoji = "🙂"; val inferred = x + 1
                val f = List(1).map(n => n + inferred)
                var local = pick(inferred)._1
                local = choose(local)
                plain(inlineValue(local)).asInstanceOf[Int] + f.head
              }
              def withDefault(x: Int = 42)(implicit name: String): String = name + x
            }
        """.trimIndent()
        Files.writeString(root.resolve("Main.scala"), source)
        Files.writeString(root.resolve("Values.scala"), upstream)
        Files.writeString(root.resolve("model.json"), model(
            unit("app", listOf("Main.scala"), listOf(projectReference("dep"))),
            unit("dep", listOf("Values.scala")),
        ).toString())
        val objects = Files.createDirectories(root.resolve("objects"))
        val child = ProcessBuilder(javaHome.resolve("bin/java").toString(), "-jar", System.getProperty("fughetta.host.jar"),
            "--adapter", System.getProperty("fughetta.adapter.jar"))
            .directory(root.toFile()).redirectError(ProcessBuilder.Redirect.INHERIT).start()
        try {
            Transcript(root, child, objects).use { protocol ->
                val demand = protocol.demand("Main.scala")
                protocol.update(1, "Main.scala" to source, "Values.scala" to upstream)
                val key = protocol.call("units", buildJsonObject {
                    put("epoch", 1); put("demand", demand)
                }).jsonArray.single().jsonObject
                assertEquals("app", key.text("name"))
                val depKey = JsonObject(key + ("name" to JsonPrimitive("dep")))
                val result = protocol.evaluate(1, key, demand)
                val shard = protocol.shard(result, "app")
                val dependency = protocol.shard(protocol.evaluate(1, depKey, protocol.demand("Values.scala")), "dep")
                assertEquals(emptyList(), shard.rows("diagnostics"), shard.rows("diagnostics").toString())
                val appSymbols = shard.rows("symbols").map { it.jsonObject }
                val native = appSymbols.single { it.text("name") == "nativeCall" }
                assertContains(native.rows("modifiers").map { it.jsonPrimitive.content }, "native")
                assertEquals("app/Main", native.getValue("extensions").jsonObject.text("jvm.class"))
                assertEquals("(I)J", native.getValue("extensions").jsonObject.text("jvm.descriptor"))
                val default = appSymbols.single { it.text("name") == "withDefault" }
                assertEquals(listOf(0, 1), default.rows("parameters").map { it.jsonObject.getValue("group").jsonPrimitive.int })
                assertEquals("42", default.rows("parameters").first().jsonObject.text("default_value"))
                assertTrue(default.rows("parameters").first().jsonObject.getValue("annotations").jsonObject.getValue("scala.has_default").jsonPrimitive.boolean)
                val inferred = appSymbols.single { it.text("name") == "inferred" }
                assertEquals("Main.scala", inferred.getValue("native_key").jsonObject.getValue("anchor").jsonObject.text("file"))
                assertTrue(appSymbols.any { it.text("name") == "<lambda>" })
                val occurrence = shard.rows("occurrences").map { it.jsonObject }.first { it.text("symbol") == inferred.text("id") }
                val selected = occurrence.getValue("selection").jsonObject
                val offset = source.indexOf("inferred")
                assertEquals(source.codePointCount(0, offset), selected.getValue("begin").jsonPrimitive.int)
                assertTrue(shard.rows("occurrences").map { it.jsonObject }.any { "body" in it })
                val relations = shard.rows("relations").map { it.jsonObject.text("kind") }.toSet()
                assertTrue(relations.containsAll(listOf("calls", "implements", "overrides", "unsafe_cast")), relations.toString())
                val scopes = shard.rows("scopes").map { it.jsonObject }
                assertTrue(scopes.any { it.text("kind") == "lambda" && "lexical_parent" in it })
                assertTrue(scopes.flatMap { it.rows("lookup") }.any { it.jsonObject["name"] == JsonPrimitive("Parent") })
                val useRoles = shard.rows("references").flatMap { it.jsonObject.rows("roles") }.map { it.jsonPrimitive.content }.toSet()
                assertTrue(useRoles.containsAll(listOf("read", "write", "call", "import", "type", "implicit")), useRoles.toString())
                assertTrue(shard.rows("typed_ranges").any { it.jsonObject.getValue("substitutions").jsonObject.isNotEmpty() })
                val types = dependency.rows("types").map { it.jsonObject.text("kind") }.toSet()
                assertTrue(types.containsAll(setOf("scala:curried_signature", "scala:by_name", "tuple")), types.toString())
                val pick = dependency.rows("symbols").map { it.jsonObject }.single { it.text("name") == "pick" }.text("id")
                assertTrue(shard.rows("references").any { it.jsonObject["target"] == JsonPrimitive(pick) },
                    "upstream and downstream symbols must use the same declaration ID")

                val reads = shard.getValue("unit").jsonObject.rows("observed_dependencies")
                val producers = result.getValue("producer_keys").jsonObject
                assertEquals(2, reads.size)
                assertEquals(reads.map { it.jsonObject.text("key") }.toSet(), producers.keys)
                assertTrue(producers.values.all { it.jsonArray == JsonArray(listOf(depKey)) })
                val initial = protocol.observe(1, reads)
                protocol.update(2, "Values.scala" to upstream.replace("x + 1\n", "x + 2\n"))
                assertEquals(initial, protocol.observe(2, reads))
                protocol.update(3, "Values.scala" to upstream.replace("x + 10", "x + 20"))
                val edited = protocol.observe(3, reads)
                assertEquals(initial["declarations"], edited["declarations"])
                assertNotEquals(initial["inline"], edited["inline"])

                protocol.update(4, "Main.scala" to source.replace("x + 1", "missingName + 1"))
                val broken = protocol.shard(protocol.evaluate(4, key, demand), "broken")
                assertTrue(broken.rows("diagnostics").any {
                    it.jsonObject.text("severity") == "error" && "missingName" in it.jsonObject.text("message")
                })
                protocol.call("close", buildJsonObject {})
                protocol.validate("--helper", "transcript.jsonl", protocol.transcript.toString())
            }
            assertTrue(child.waitFor(15, TimeUnit.SECONDS))
            assertEquals(0, child.exitValue())
        } finally {
            child.destroyForcibly()
        }
    }

    private class Transcript(private val root: Path, child: Process, objects: Path) : AutoCloseable {
        private val reader = child.inputStream.bufferedReader()
        private val writer = child.outputStream.bufferedWriter()
        private var request = 0
        private val sources = linkedMapOf<String, String>()
        val transcript = StringBuilder()
        private val empty = JsonArray(emptyList())

        init {
            call("open", buildJsonObject {
                put("session", state(0)); putJsonObject("initialization") {}
                putJsonObject("host") {
                    put("objects", objects.toString())
                    putJsonObject("sdks") { put("jdk", javaHome.toString()); put("scala", scalaLibrary.parent.toString()) }
                }
            })
        }

        private fun state(epoch: Int) = buildJsonObject {
            put("id", "session"); put("slot", "slot"); put("project", "project"); put("provider", "scala")
            put("stage", "analysis"); put("options", "options"); put("snapshot", "snapshot-$epoch")
            put("epoch", epoch); put("inputs", empty); put("phase", "ready")
        }

        fun update(epoch: Int, vararg documents: Pair<String, String>) {
            documents.forEach { (path, _) -> sources[path] = "$path-$epoch" }
            call("update", buildJsonObject {
                put("session", state(epoch))
                putJsonObject("sources") { sources.forEach { (path, value) -> put(path, value) } }
                putJsonObject("artifacts") { put("model.json", "model-object") }
                putJsonObject("generated") {}; putJsonObject("facts") {}; put("changed", documents.map { it.first }.strings())
                putJsonArray("documents") { documents.forEach { (path, text) -> add(buildJsonObject {
                    put("path", path); put("language", "scala"); put("text", text)
                }) } }
            })
        }

        fun demand(file: String) = buildJsonObject {
            put("files", listOf(file).strings()); put("projects", empty); put("contexts", empty); put("modules", empty)
            put("facts", listOf("symbols", "types", "definitions", "typed_ranges", "diagnostics").strings())
        }

        fun evaluate(epoch: Int, key: JsonObject, demand: JsonObject): JsonObject = call("evaluate", buildJsonObject {
            putJsonObject("unit") {
                put("id", "unit-${key.text("name")}"); put("context", "context")
                putJsonObject("recipe") { put("unit", key); put("options", "options") }
                listOf("direct_inputs", "observed_dependencies", "channel_definitions", "conventions", "output_channels", "modules")
                    .forEach { put(it, empty) }
                putJsonObject("contributions") {
                    listOf("files", "occurrences", "references", "scopes", "relations", "diagnostics", "symbols", "types").forEach { put(it, empty) }
                }
                putJsonObject("coverage") { put("requested", demand); put("available", empty); put("missing", empty); put("unresolved_count", 0) }
                put("cacheable", true)
            }
            putJsonObject("request") {
                putJsonObject("execution") { put("request_id", "request"); put("computation_id", "computation"); put("stage_id", "analysis") }
                put("epoch", epoch); put("slot_id", "slot"); put("unit", key)
                put("inputs", empty); put("demand", demand); putJsonObject("budget") {}; put("retained_only", false)
            }
        }).jsonObject

        fun observe(epoch: Int, reads: JsonArray) = call("observe", buildJsonObject {
            put("session", state(epoch)); put("reads", reads)
        }).jsonArray.associate { value ->
            val input = value.jsonObject
            Json.parseToJsonElement(input.text("name")).jsonArray.last().jsonPrimitive.content to input.text("object")
        }

        fun call(operation: String, params: JsonObject): JsonElement {
            val message = buildJsonObject { put("request_id", ++request); put("operation", operation); put("params", params) }
            writer.write(message.toString()); writer.newLine(); writer.flush()
            val reply = Json.parseToJsonElement(requireNotNull(reader.readLine()) { "helper exited during $operation" }).jsonObject
            transcript.append(buildJsonObject { put("message", message); put("response", reply) }).append('\n')
            assertEquals(request, reply.getValue("request_id").jsonPrimitive.int)
            assertFalse("error" in reply, reply["error"].toString())
            return reply.getValue("result")
        }

        fun shard(result: JsonObject, name: String): JsonObject {
            val shard = result.rows("shards").single().jsonObject
            validate("--read-shard", "$name.json", shard.toString())
            return shard
        }

        fun validate(mode: String, name: String, content: String) {
            val validator = System.getenv("FUGHETTA_RECORD_VALIDATOR") ?: return
            val path = Files.writeString(root.resolve(name), content)
            assertEquals(0, ProcessBuilder(validator, mode, path.toString()).inheritIO().start().waitFor())
        }

        override fun close() {
            writer.close()
            reader.close()
        }
    }
}
