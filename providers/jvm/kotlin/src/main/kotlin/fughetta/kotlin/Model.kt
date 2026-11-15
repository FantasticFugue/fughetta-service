package fughetta.kotlin

import kotlinx.serialization.json.*
import java.nio.file.Path

/** Resolves the shared project_model against this worker's installed inputs. */
class Model(private val tree: Path, private val objects: Path, private val sdks: Map<String, Path>) {
    private fun JsonObject.text(key: String) = getValue(key).jsonPrimitive.content
    private fun JsonObject.array(key: String) = getValue(key).jsonArray
    private fun objectPath(value: JsonObject) = objects.resolve(value.text("hash"))

    fun expand(argument: String): String = Regex("\\$\\{(tree|object:[^}]+|sdk:[^}]+)}").replace(argument) { match ->
        val key = match.groupValues[1]
        when {
            key == "tree" -> tree.toString()
            key.startsWith("object:") -> objects.resolve(key.removePrefix("object:")).toString()
            else -> requireNotNull(sdks[key.removePrefix("sdk:")]) { "missing SDK mapping: $key" }.toString()
        }
    }

    fun read(text: String, sources: Map<String, String>): List<ModuleInputs> {
        val model = Json.parseToJsonElement(text).jsonObject
        require(model.getValue("schema").jsonPrimitive.int == 1) { "unknown project model schema" }
        val units = model.array("units").map { it.jsonObject }
        fun resolve(ref: JsonObject): String {
            val matches = units.filter { unit ->
                unit.text("project") == ref.text("project") &&
                    if (ref.text("unit").isNotEmpty()) unit.text("id") == ref.text("unit")
                    else ref.getValue("attributes").jsonObject.isNotEmpty() &&
                        unit.array("variants").contains(ref.getValue("attributes"))
            }
            require(matches.size == 1) { "project reference requires one matching unit: ${ref.text("project")}" }
            return matches.single().text("id")
        }
        fun path(ref: JsonObject): Path = when (ref.text("kind")) {
            "artifact" -> objectPath(ref.getValue("object").jsonObject)
            "tree" -> tree.resolve(ref.text("path"))
            "sdk" -> requireNotNull(sdks[ref.text("name")]) { "missing SDK mapping: ${ref.text("name")}" }
                .resolve(ref.text("path"))
            else -> error("project reference is a module dependency")
        }
        return units.map { unit ->
            require(unit.text("language") == "kotlin") { "unsupported source module language: ${unit.text("language")}" }
            val refs = unit.array("references").map { it.jsonObject }
            val generated = unit.getValue("generated").jsonObject
            val runtime = refs.filter { it.text("role") == "runtime" && it.text("kind") == "sdk" }
            require(runtime.size == 1) { "Kotlin module requires one JDK reference: ${unit.text("id")}" }
            val treeBinaries = refs.filter { it.text("kind") == "tree" && it.text("role") in setOf("classpath", "friend", "plugin") }.map { Path.of(it.text("path")).normalize().toString().replace('\\', '/') }
            ModuleInputs(
                id = unit.text("id"),
                sources = unit.array("sources").map { source ->
                    val name = source.jsonPrimitive.content
                    // Logical paths remain stable; the environment reads generated
                    // text from its retained object when constructing virtual files.
                    tree.resolve(name)
                },
                libraries = refs.filter { it.text("kind") != "project" && it.text("role") == "classpath" }.map(::path),
                jdk = path(runtime.single()),
                dependencies = refs.filter { it.text("kind") == "project" && it.text("role") == "classpath" }.map(::resolve),
                friends = refs.filter { it.text("kind") == "project" && it.text("role") == "friend" }.map(::resolve),
                compilerArguments = unit.array("compiler_arguments").map { expand(it.jsonPrimitive.content) },
                plugins = refs.filter { it.text("role") == "plugin" }.map(::path),
                binaryFriends = refs.filter { it.text("kind") != "project" && it.text("role") == "friend" }.map(::path),
                generated = generated.entries.associate { tree.resolve(it.key) to objectPath(it.value.jsonObject) },
                binaryInputs = sources.filterKeys { file -> treeBinaries.any { it.isEmpty() || file == it || file.startsWith("$it/") } },
            )
        }
    }
}
