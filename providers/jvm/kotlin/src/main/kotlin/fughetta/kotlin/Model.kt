package fughetta.kotlin

import fughetta.jvm.JvmClasspath
import kotlinx.serialization.json.*
import java.nio.file.Files
import java.nio.file.Path

/** Resolves the shared project_model against this worker's installed inputs. */
class Model(private val tree: Path, private val objects: Path, private val sdks: Map<String, Path>) {
    private fun JsonObject.text(key: String) = getValue(key).jsonPrimitive.content
    private fun JsonObject.array(key: String) = getValue(key).jsonArray
    private fun objectPath(value: JsonObject) = objects.resolve(value.text("hash"))

    fun read(text: String, sources: Map<String, String>): List<ModuleInputs> {
        val model = Json.parseToJsonElement(text).jsonObject
        require(model.getValue("schema").jsonPrimitive.int == 1) { "unknown project model schema" }
        val units = model.array("units").map { it.jsonObject }
        val kotlinUnits = units.filter { it.text("language") == "kotlin" }.associateBy { it.text("id") }
        require(kotlinUnits.isNotEmpty()) { "project_model has no Kotlin compilation units" }
        val classpath = JvmClasspath(objects)
        fun resolve(ref: JsonObject): JsonObject {
            val matches = units.filter { unit ->
                unit.text("project") == ref.text("project") &&
                    if (ref.text("unit").isNotEmpty()) unit.text("id") == ref.text("unit")
                    else ref.getValue("attributes").jsonObject.isNotEmpty() &&
                        unit.array("variants").contains(ref.getValue("attributes"))
            }
            require(matches.size == 1) { "project reference requires one matching unit: ${ref.text("project")}" }
            return matches.single()
        }
        fun path(ref: JsonObject): Path = when (ref.text("kind")) {
            "artifact" -> {
                val value = ref.getValue("object").jsonObject
                if (ref.text("role") in setOf("classpath", "friend")) classpath.artifact(value.text("hash"), value.text("name"))
                else objectPath(value)
            }
            "tree" -> tree.resolve(ref.text("path"))
            "sdk" -> requireNotNull(sdks[ref.text("name")]) { "missing SDK mapping: ${ref.text("name")}" }
                .resolve(ref.text("path"))
            else -> error("project reference is a module dependency")
        }
        return kotlinUnits.values.map { unit ->
            val refs = unit.array("references").map { it.jsonObject }
            val generated = unit.getValue("generated").jsonObject
            val runtime = refs.filter { it.text("role") == "runtime" && it.text("kind") == "sdk" }
            require(runtime.size == 1) { "Kotlin module requires one JDK reference: ${unit.text("id")}" }
            val projects = refs.filter { it.text("kind") == "project" && it.text("role") in setOf("classpath", "friend") }
                .groupBy({ it.text("role") }, ::resolve)
            fun moduleDependencies(role: String) = projects[role].orEmpty().map { it.text("id") }.filter { it in kotlinUnits }
            fun outputs(project: JsonObject) = project["outputs"]?.jsonArray.orEmpty()
                .map { Path.of(it.jsonPrimitive.content).normalize() }
            val sourceOutputs = (moduleDependencies("classpath") + moduleDependencies("friend"))
                .flatMap { outputs(kotlinUnits.getValue(it)) }.toSet() + outputs(unit)
            fun binaryDirectories(role: String) = projects[role].orEmpty()
                .filter { it.text("id") !in kotlinUnits }
                .flatMap(::outputs)
                .filter { it !in sourceOutputs && Files.isDirectory(tree.resolve(it)) }
                .distinct()
            val projectLibraries = binaryDirectories("classpath")
            val projectFriends = binaryDirectories("friend")
            val binaryRefs = refs.filter { it.text("kind") != "project" && it.text("role") in setOf("classpath", "friend") }
            val binaryPaths = binaryRefs.associateWith(::path)
            val classpathObjects = binaryRefs.filter { it.text("kind") == "artifact" }.associate { ref ->
                ref.getValue("object").jsonObject.text("hash") to binaryPaths.getValue(ref)
            }
            fun binaries(role: String) = binaryRefs.filter { it.text("role") == role }.map(binaryPaths::getValue)
            val treeBinaries = (refs.filter { it.text("kind") == "tree" && it.text("role") in setOf("classpath", "friend", "plugin") }
                .map { Path.of(it.text("path")).normalize() } + projectLibraries + projectFriends)
                .map { it.toString().replace('\\', '/') }
            ModuleInputs(
                id = unit.text("id"),
                sources = unit.array("sources").map { source ->
                    val name = source.jsonPrimitive.content
                    // Logical paths remain stable; the environment reads generated
                    // text from its retained object when constructing virtual files.
                    tree.resolve(name)
                },
                libraries = binaries("classpath") + projectLibraries.map(tree::resolve),
                jdk = path(runtime.single()),
                dependencies = moduleDependencies("classpath"),
                friends = moduleDependencies("friend"),
                compilerArguments = classpath.arguments(unit.array("compiler_arguments").map { it.jsonPrimitive.content }, tree, sdks, classpathObjects),
                plugins = refs.filter { it.text("role") == "plugin" }.map(::path),
                binaryFriends = binaries("friend") + projectFriends.map(tree::resolve),
                generated = generated.entries.associate { tree.resolve(it.key) to objectPath(it.value.jsonObject) },
                binaryInputs = sources.filterKeys { file -> treeBinaries.any { it.isEmpty() || file == it || file.startsWith("$it/") } },
            )
        }
    }
}
