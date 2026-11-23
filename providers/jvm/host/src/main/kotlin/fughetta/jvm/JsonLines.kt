package fughetta.jvm

import java.io.InputStream
import java.io.OutputStream
import java.io.ByteArrayOutputStream
import kotlinx.serialization.json.*

/** Wire framing shared with language::helper_adapter; diagnostics belong on stderr. */
class JsonLines(private val limit: Int = 64 shl 20) {
    init { require(limit > 0) }

    internal fun read(reader: InputStream): JsonObject? {
        val line = ByteArrayOutputStream()
        while (true) {
            val next = reader.read()
            if (next == -1) {
                require(line.size() == 0) { "helper request ended before its line delimiter" }
                return null
            }
            if (next == 10) break
            require(line.size() < limit) { "helper request exceeds the object limit" }
            line.write(next)
        }
        return Json.parseToJsonElement(line.toByteArray().decodeToString(throwOnInvalidSequence = true)).jsonObject
    }

    internal fun reply(writer: OutputStream, request: Long, result: JsonElement? = null, error: String? = null) {
        var bytes = buildJsonObject {
            put("request_id", request)
            if (error != null) put("error", error) else put("result", checkNotNull(result))
        }.toString().toByteArray(Charsets.UTF_8)
        if (bytes.size >= limit) {
            bytes = buildJsonObject {
                put("request_id", request); put("error", "helper response exceeds the object limit")
            }.toString().toByteArray(Charsets.UTF_8)
            require(bytes.size < limit) { "helper response limit cannot hold an error" }
        }
        synchronized(writer) {
            writer.write(bytes)
            writer.write(10)
            writer.flush()
        }
    }

    fun serve(input: InputStream, output: OutputStream, dispatch: (String, JsonObject) -> JsonElement) {
        val reader = input.buffered()
        val writer = output.buffered()
        while (true) {
            val message = read(reader) ?: return
            val request = message.getValue("request_id").jsonPrimitive.long
            val operation = message.getValue("operation").jsonPrimitive.content
            try {
                val result = dispatch(operation, message.getValue("params").jsonObject)
                reply(writer, request, result = result)
            } catch (error: Exception) {
                reply(writer, request, error = error.message ?: error.javaClass.simpleName)
            }
            if (operation == "close" || operation == "cancel") return
        }
    }
}
