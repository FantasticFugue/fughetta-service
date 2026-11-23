package fughetta.scala

import java.nio.file.Files
import java.nio.file.Path
import kotlinx.serialization.json.*

fun main(args: Array<String>) {
    require(args.size == 3 && args[0] in setOf("--jvm-metadata", "--jvm-metadata-list")) {
        "expected --jvm-metadata <java_release> <path> or --jvm-metadata-list <java_release> <path-list>"
    }
    val path = Path.of(args[2])
    val paths = if (args[0] == "--jvm-metadata") listOf(path) else
        Json.parseToJsonElement(Files.readString(path)).jsonArray.map { Path.of(it.jsonPrimitive.content) }
    println(Metadata.read(paths, args[1].toInt()))
}
