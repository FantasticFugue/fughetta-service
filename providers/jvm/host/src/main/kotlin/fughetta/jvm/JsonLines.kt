package fughetta.jvm

import java.io.InputStream
import java.io.OutputStream
import java.io.ByteArrayOutputStream
import kotlinx.serialization.json.*

/** Wire framing shared with language::helper_adapter; diagnostics belong on stderr. */
class JsonLines(private val limit: Int = 64 shl 20) {
    init { require(limit > 0) }

    fun serve(input: InputStream, output: OutputStream, dispatch: (String, JsonObject) -> JsonElement) {
        val reader = input.buffered()
        val writer = output.buffered()
        while (true) {
            val line = ByteArrayOutputStream()
            while (true) {
                val next = reader.read()
                if (next == -1) {
                    require(line.size() == 0) { "helper request ended before its line delimiter" }
                    writer.flush()
                    return
                }
                if (next == 10) break
                require(line.size() < limit) { "helper request exceeds the semantic object limit" }
                line.write(next)
            }
            val message = Json.parseToJsonElement(line.toByteArray().decodeToString(throwOnInvalidSequence = true)).jsonObject
            val request = message.getValue("request_id").jsonPrimitive.long
            val operation = message.getValue("operation").jsonPrimitive.content
            val response = try {
                val result = dispatch(operation, message.getValue("params").jsonObject)
                buildJsonObject { put("request_id", request); put("result", result) }
            } catch (error: Exception) {
                buildJsonObject { put("request_id", request); put("error", error.message ?: error.javaClass.simpleName) }
            }
            var bytes = response.toString().toByteArray(Charsets.UTF_8)
            if (bytes.size >= limit) {
                bytes = buildJsonObject {
                    put("request_id", request); put("error", "helper response exceeds the semantic object limit")
                }.toString().toByteArray(Charsets.UTF_8)
                require(bytes.size < limit) { "helper response limit cannot hold an error" }
            }
            writer.write(bytes)
            writer.write(10)
            writer.flush()
            if (operation == "close" || operation == "cancel") return
        }
    }
}
