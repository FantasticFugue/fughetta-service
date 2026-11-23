package fughetta.kotlin

import java.nio.file.Path
import java.nio.file.Files
import kotlinx.serialization.json.*

fun main(args: Array<String>) {
    if (args.firstOrNull() == "--jvm-metadata-list") {
        require(args.size == 3) { "--jvm-metadata-list requires java_release and one path-list file" }
        val paths = Json.parseToJsonElement(Files.readString(Path.of(args[2]))).jsonArray.map { Path.of(it.jsonPrimitive.content) }
        println(Metadata.readMany(paths, args[1].toInt()))
        return
    }
    require(args.size == 3 && args[0] == "--jvm-metadata") {
        "expected --jvm-metadata or --jvm-metadata-list; helper sessions run in fughetta-jvm --adapter"
    }
    println(Metadata.read(Path.of(args[2]), args[1].toInt()))
}
