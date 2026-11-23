package fughetta.scala

import fughetta.jvm.Evaluation
import fughetta.jvm.LanguageAdapter
import java.nio.file.Files
import java.nio.file.Path
import kotlinx.serialization.json.*

class Adapter : LanguageAdapter {
    private lateinit var root: Path
    private lateinit var model: Model
    private lateinit var workspace: Workspace
    private var sources = emptyMap<String, String>()
    private var session: JsonObject? = null

    override fun open(root: Path, session: JsonObject, host: JsonObject, initialization: JsonElement?) {
        this.root = root
        this.session = session
        val objects = Path.of(host.text("objects")).toAbsolutePath().normalize()
        val sdks = host.getValue("sdks").jsonObject.mapValues { Path.of(it.value.jsonPrimitive.content).toAbsolutePath().normalize() }
        model = Model(root, objects, sdks)
        workspace = Workspace(root, session.text("project"), session.text("provider"))
    }

    override fun install(params: JsonObject) {
        val nextSources = params.getValue("sources").jsonObject.mapValues { it.value.jsonPrimitive.content }
        val inputs = params.getValue("artifacts").jsonObject.keys.sorted().flatMap {
            model.read(Files.readString(root.resolve(it)), nextSources)
        }
        val documents = params.rows("documents").associate {
            val document = it.jsonObject
            root.resolve(document.text("path")).normalize() to document.text("text")
        }
        workspace.update(inputs, documents, nextSources)
        sources = nextSources
        session = params.getValue("session").jsonObject
    }

    override fun units(files: Set<Path>) = workspace.units(files)

    private fun channelKey(session: JsonObject, scope: String, channel: String) =
        listOf(session.text("provider"), session.text("stage"), scope, channel).strings().toString()

    override fun observe(session: JsonObject, reads: JsonArray): JsonArray {
        val inputs = linkedMapOf<String, JsonObject>()
        reads.forEach { value ->
            val read = value.jsonObject
            val scope = read["scope"]?.jsonPrimitive?.contentOrNull ?: return@forEach
            val channel = read["channel"]?.jsonPrimitive?.contentOrNull ?: return@forEach
            if (channel !in setOf("declarations", "inline")) return@forEach
            val key = read.text("key")
            val parts = runCatching { Json.parseToJsonElement(key).jsonArray }.getOrNull() ?: return@forEach
            if (parts.size != 4 || !read["search_path"]?.jsonArray.isNullOrEmpty()) return@forEach
            require(key == channelKey(session, scope, channel)) { "observation names another provider channel" }
            val observed = workspace.observe(scope) ?: return@forEach
            inputs[key] = input("channel", key, if (channel == "declarations") observed.declarations else observed.inline)
        }
        return JsonArray(inputs.values.toList())
    }

    override fun evaluate(session: JsonObject, template: JsonObject, module: String): Evaluation {
        val projection = workspace.evaluate(module)
        val direct = template.rows("direct_inputs").filterNot {
            val input = it.jsonObject
            input.text("kind") == "source" || (input.text("kind") == "file" && input.text("name") in sources)
        }.toMutableList()
        val dependencies = template.rows("observed_dependencies").toMutableList()
        val producers = linkedMapOf<String, List<String>>()
        var complete = !workspace.opaque(module)

        fun source(path: String, observe: Boolean) {
            val value = sources[path]
            if (value == null) {
                complete = false
                return
            }
            if (observe) dependencies.add(buildJsonObject {
                put("kind", "source"); put("project", session.text("project")); put("key", path); put("value", value)
                putJsonArray("search_path") {}; putJsonArray("producers") {}
            }) else direct.add(input("file", path, value))
        }
        fun read(scope: String, kind: String, channel: String, value: String) {
            val key = channelKey(session, scope, channel)
            dependencies.add(buildJsonObject {
                put("kind", kind); put("project", session.text("project")); put("key", key); put("value", value)
                put("channel", channel); put("scope", scope); put("name", "*")
                putJsonArray("search_path") {}; putJsonArray("producers") {}
            })
            producers[key] = listOf(scope)
        }

        val own = workspace.input(module)
        own.sources.filterNot { it in own.generated }.forEach { source(root.relativize(it).wire(), false) }
        own.binaryInputs.keys.forEach { source(it, false) }
        for (id in workspace.dependencies(module)) {
            val dependency = workspace.input(id)
            val observed = workspace.observe(id)
            if (observed == null) {
                dependency.sources.filterNot { it in dependency.generated }.forEach { source(root.relativize(it).wire(), true) }
                if (dependency.generated.isNotEmpty()) complete = false
            } else {
                read(id, "lookup", "declarations", observed.declarations)
                read(id, "body", "inline", observed.inline)
                dependency.sources.filter { it.fileName.toString().endsWith(".java") && it !in dependency.generated }
                    .forEach { source(root.relativize(it).wire(), true) }
            }
            dependency.binaryInputs.keys.forEach { source(it, true) }
        }

        val unit = JsonObject(template.toMutableMap().apply {
            put("direct_inputs", JsonArray(direct.distinct()))
            put("observed_dependencies", JsonArray(dependencies.distinct()))
            put("cacheable", JsonPrimitive(template.getValue("cacheable").jsonPrimitive.boolean && complete))
        })
        return Evaluation(projection.shard(unit), producers)
    }

    private fun input(kind: String, name: String, value: String) = buildJsonObject {
        put("kind", kind); put("name", name); put("object", value); put("type", "")
    }

    override fun close() {
        if (session != null) workspace.close()
        session = null
        sources = emptyMap()
    }
}
