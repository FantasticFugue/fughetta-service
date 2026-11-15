package fughetta.kotlin

import java.nio.file.Path
import java.nio.file.Files
import kotlinx.serialization.json.*
import org.jetbrains.kotlin.analysis.api.projectStructure.KaLibraryModule
import com.intellij.psi.PsiElement
import fughetta.jvm.Evaluation
import fughetta.jvm.LanguageAdapter

/** The Kotlin Analysis API projection served by the fughetta-jvm host. */
class KotlinAdapter : LanguageAdapter {
    private lateinit var root: Path
    private var state: JsonObject? = null
    private var workspace: Workspace? = null
    private lateinit var model: Model
    private lateinit var objects: Path
    private var sdks = emptyMap<String, Path>()
    private var sourceObjects = emptyMap<String, String>()

    private fun JsonObject.text(key: String) = getValue(key).jsonPrimitive.content
    private fun active() = checkNotNull(workspace) { "helper session is not open" }

    override fun open(root: Path, session: JsonObject, host: JsonObject, initialization: JsonElement?) {
        this.root = root
        objects = Path.of(host.text("objects")).toAbsolutePath().normalize()
        sdks = host.getValue("sdks").jsonObject.mapValues { Path.of(it.value.jsonPrimitive.content) }
        model = Model(root, objects, sdks)
        workspace = Workspace(root, session.text("project"), session.text("provider"))
        state = session
    }

    override fun install(params: JsonObject) {
        val current = active()
        val sources = params.getValue("sources").jsonObject.mapValues { it.value.jsonPrimitive.content }
        val inputs = params.getValue("artifacts").jsonObject.keys.sorted().flatMap { path ->
            model.read(Files.readString(root.resolve(path)), sources)
        }
        val documents = params.getValue("documents").jsonArray.associate { document ->
            val row = document.jsonObject
            root.resolve(row.text("path")) to row.text("text")
        }
        current.install(inputs, documents)
        sourceObjects = sources
        state = params.getValue("session").jsonObject
    }

    override fun units(files: Set<Path>) = active().units(files)

    override fun evaluate(session: JsonObject, template: JsonObject, module: String): Evaluation {
        val anchors = java.util.IdentityHashMap<PsiElement, JsonObject>()
        val result = active().evaluate(module, template.text("id"), template.text("context"),
            externalModule = { dependency ->
                if (dependency is KaLibraryModule) {
                    val path = Path.of(dependency.libraryName).toAbsolutePath().normalize()
                    when {
                        path.startsWith(objects) -> "object:" + objects.relativize(path).toString()
                        else -> {
                            val sdk = sdks.entries.filter { path.startsWith(it.value) }.maxByOrNull { it.value.nameCount }
                            when {
                                sdk != null -> "sdk:${sdk.key}:" + sdk.value.relativize(path).toString().replace('\\', '/')
                                path.startsWith(root) -> "tree:" + root.relativize(path).toString().replace('\\', '/')
                                else -> error("library has no installed object, SDK or tree identity: $path")
                            }
                        }
                    }
                } else "kotlin/builtins"
            }, anchor = { symbol ->
                val psi = requireNotNull(symbol.psi) { "declaration requires a native identity or source anchor" }
                anchors.getOrPut(psi) { buildJsonObject {
                    put("file", root.relativize(Path.of(psi.containingFile.virtualFile.path)).toString().replace('\\', '/'))
                    // Publication replaces these unit-local labels with source-node anchors.
                    put("declaration", "pending-${anchors.size}")
                } }
            })
        val shard = observedShard(result.shard(template), module, result.observationScope())
        return Evaluation(shard, producers(module, shard.getValue("unit").jsonObject.getValue("observed_dependencies").jsonArray))
    }

    override fun observe(session: JsonObject, reads: JsonArray): JsonArray {
        val values = linkedMapOf<String, JsonObject>()
        for (value in reads) {
            val read = value.jsonObject
            val module = read["scope"]?.jsonPrimitive?.content ?: continue
            val channel = read["channel"]?.jsonPrimitive?.content ?: continue
            val raw = read.text("key")
            val parts = keyParts(raw) ?: continue
            require(parts[0] == session.text("provider") && parts[1] == session.text("stage") && parts[2] == module) {
                "observation names another provider channel"
            }
            val selectors = when (parts.size) {
                4 -> {
                    require(parts[3] == channel) { "observation names another provider channel" }
                    null
                }
                5 -> {
                    require(channel == "declarations" && parts[3] == "declarations") { "observation names another provider channel" }
                    val selected = observationSelectors(read["search_path"]?.jsonArray?.map { it.jsonPrimitive.content }.orEmpty())
                    require(parts[4] == observationDigest(selected)) { "observation selectors differ from their digest" }
                    selected
                }
                else -> continue
            }
            val observed = active().observe(module, selectors) ?: continue
            val digest = when (channel) { "declarations" -> observed.declarations; "inline" -> observed.inlineBodies; else -> continue }
            values[raw] = buildJsonObject { put("kind", "channel"); put("name", raw); put("object", digest); put("type", "") }
        }
        return JsonArray(values.values.toList())
    }

    private fun producers(module: String, reads: JsonArray): Map<String, List<String>> {
        val dependencies = active().dependencies(module).toSet()
        val identity = checkNotNull(state)
        return buildMap {
            for (value in reads) {
                val raw = value.jsonObject["key"]?.jsonPrimitive?.content ?: continue
                val parts = keyParts(raw) ?: continue
                if (parts.size !in 4..5 || parts[0] != identity.text("provider") || parts[1] != identity.text("stage")) continue
                if (parts[2] !in dependencies || parts[3] !in listOf("declarations", "inline")) continue
                put(raw, listOf(parts[2]))
            }
        }
    }

    private fun keyParts(value: String): List<String>? =
        runCatching { Json.parseToJsonElement(value).jsonArray.map { it.jsonPrimitive.content } }.getOrNull()

    private fun channelKey(module: String, channel: String, selection: String? = null) = buildJsonArray {
        add(state!!.text("provider")); add(state!!.text("stage")); add(module); add(channel)
        if (selection != null) add(selection)
    }.toString()

    private fun observedShard(shard: JsonObject, module: String, scope: ObservationScope): JsonObject {
        val template = shard.getValue("unit").jsonObject
        val direct = template.getValue("direct_inputs").jsonArray.filterNot {
            val input = it.jsonObject
            input.text("kind") == "source" || (input.text("kind") == "file" && input.text("name") in sourceObjects)
        }.toMutableList()
        if (active().opaqueInputs(module)) direct.addAll(template.getValue("direct_inputs").jsonArray.filter { it.jsonObject.text("kind") == "source" })
        val reads = template.getValue("observed_dependencies").jsonArray.toMutableList()
        var complete = true
        fun path(file: Path) = root.relativize(file.toAbsolutePath().normalize()).toString().replace('\\', '/')
        fun fileInput(name: String, dependency: Boolean) {
            val value = sourceObjects[name]
            if (value == null) { complete = false; return }
            if (dependency) reads.add(buildJsonObject {
                put("kind", "source"); put("project", state!!.getValue("project")); put("key", name); put("value", value)
                put("search_path", JsonArray(emptyList())); put("producers", JsonArray(emptyList()))
            })
            else direct.add(buildJsonObject { put("kind", "file"); put("name", name); put("object", value); put("type", "") })
        }
        val selectors = scope.selectors
        val selection = observationDigest(selectors)
        fun channelRead(dependency: String, kind: String, channel: String, value: String, selected: List<String>?) = buildJsonObject {
            put("kind", kind); put("project", state!!.getValue("project"))
            put("key", channelKey(dependency, channel, selected?.let { selection })); put("value", value)
            put("channel", channel); put("scope", dependency); put("name", "*")
            putJsonArray("search_path") { selected?.forEach { add(it) } }
            putJsonArray("producers") {}
        }
        val own = active().moduleInputs(module)
        for (file in own.sources) if (file !in own.generated) fileInput(path(file), false)
        own.binaryInputs.forEach { (name, _) -> fileInput(name, false) }
        for (dependency in active().dependencies(module)) {
            val input = active().moduleInputs(dependency)
            // A sealed hierarchy can gain a subclass in the same module without naming a
            // declaration this unit references; those modules keep the whole-module digest.
            val full = dependency in scope.sealedModules
            val values = active().observe(dependency, if (full) null else selectors)
            if (values == null) {
                for (file in input.sources) if (file !in input.generated) fileInput(path(file), true)
            } else {
                reads.add(channelRead(dependency, "lookup", "declarations", values.declarations, if (full) null else selectors))
                reads.add(channelRead(dependency, "body", "inline", values.inlineBodies, null))
                // Java source frontends do not expose an equivalent summary here.
                for (file in input.sources) if (file.fileName.toString().endsWith(".java") && file !in input.generated) fileInput(path(file), true)
            }
            input.binaryInputs.forEach { (name, _) -> fileInput(name, true) }
        }
        return JsonObject(shard.toMutableMap().apply { put("unit", JsonObject(template.toMutableMap().apply {
            put("direct_inputs", JsonArray(direct.distinct()))
            put("observed_dependencies", JsonArray(reads.distinct()))
            put("cacheable", JsonPrimitive(template.getValue("cacheable").jsonPrimitive.boolean && complete && !active().opaqueInputs(module)))
        })) })
    }

    override fun close() {
        workspace?.close()
        workspace = null
        state = null
        sourceObjects = emptyMap()
        org.jetbrains.kotlin.analysis.api.standalone.disposeGlobalStandaloneApplicationServices()
    }
}
