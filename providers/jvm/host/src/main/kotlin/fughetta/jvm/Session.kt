package fughetta.jvm

import java.nio.file.Path
import kotlinx.serialization.json.*

/** Operations match language::helper; the worker serializes requests and owns cancellation. */
class Session(private val root: Path, private val adapter: LanguageAdapter) : AutoCloseable {
    private var state: JsonObject? = null
    private var installed = false
    private var closed = false

    private fun JsonObject.text(key: String) = getValue(key).jsonPrimitive.content
    private fun open() = checkNotNull(state) { "helper session is not open" }
    private fun ready(): JsonObject = open().also { check(installed) { "helper model is not installed" } }
    private fun epoch(params: JsonObject) {
        require(params.getValue("epoch") == open().getValue("epoch")) { "stale helper epoch" }
    }
    private fun same(session: JsonObject) {
        require(session.text("id") == open().text("id") && session.text("slot") == open().text("slot")) { "request belongs to another helper session" }
    }

    fun dispatch(operation: String, params: JsonObject): JsonElement = when (operation) {
        "open" -> {
            check(state == null) { "helper session is already open" }
            val session = params.getValue("session").jsonObject
            adapter.open(root, session, params.getValue("host").jsonObject, params["initialization"])
            state = session
            JsonObject(emptyMap())
        }
        "update" -> {
            val session = params.getValue("session").jsonObject
            same(session)
            adapter.install(params)
            state = session
            installed = true
            JsonObject(emptyMap())
        }
        "units" -> {
            epoch(params)
            ready()
            val files = params.getValue("demand").jsonObject.getValue("files").jsonArray
                .map { root.resolve(it.jsonPrimitive.content).normalize() }.toSet()
            JsonArray(adapter.units(files).map(::unitKey))
        }
        "evaluate" -> {
            val request = params.getValue("request").jsonObject
            epoch(request)
            val session = ready()
            require(request.text("slot_id") == session.text("slot")) { "evaluation belongs to another slot" }
            val result = adapter.evaluate(session, params.getValue("unit").jsonObject, request.getValue("unit").jsonObject.text("name"))
            buildJsonObject {
                put("session", session); put("request", request)
                putJsonArray("shards") { add(result.shard) }
                putJsonObject("producer_keys") {
                    result.producers.forEach { (read, modules) -> put(read, JsonArray(modules.map(::unitKey))) }
                }
                put("deferred", false)
            }
        }
        "observe" -> {
            val session = params.getValue("session").jsonObject
            epoch(session)
            same(session)
            adapter.observe(ready(), params.getValue("reads").jsonArray)
        }
        "close", "cancel" -> { close(); JsonObject(emptyMap()) }
        else -> throw IllegalArgumentException("unknown helper operation: $operation")
    }

    private fun unitKey(module: String) = buildJsonObject {
        val session = open()
        put("project", session.getValue("project")); put("stage", session.getValue("stage"))
        put("provider", session.getValue("provider")); put("operation", "evaluate")
        put("kind", "module"); put("name", module)
    }

    override fun close() {
        if (closed) return
        closed = true
        adapter.close()
    }
}
