package fughetta.scala

import fughetta.jvm.JvmClasspath
import java.nio.file.Files
import java.nio.file.Path
import kotlinx.serialization.json.*

internal fun JsonObject.text(key: String) = getValue(key).jsonPrimitive.content
internal fun JsonObject.rows(key: String) = getValue(key).jsonArray
internal fun Path.wire() = toString().replace('\\', '/')
internal fun Iterable<String>.strings() = JsonArray(map(::JsonPrimitive))

internal data class Library(val path: Path, val module: String)

internal data class ModuleInputs(
    val id: String,
    val sources: List<Path>,
    val libraries: List<Library>,
    val jdk: Path,
    val dependencies: List<String>,
    val compilerArguments: List<String> = emptyList(),
    val plugins: List<Path> = emptyList(),
    val generated: Map<Path, Path> = emptyMap(),
    val binaryInputs: Map<String, String> = emptyMap(),
    val directory: Path? = null,
    val projectedSources: Set<Path>? = null,
)

internal class Model(private val root: Path, private val objects: Path, private val sdks: Map<String, Path>) {
    private fun objectPath(value: JsonObject) = objects.resolve(value.text("hash")).normalize()
    private fun sdk(name: String) = requireNotNull(sdks[name]) { "missing SDK mapping: $name" }

    fun read(text: String, sources: Map<String, String>): List<ModuleInputs> {
        val model = Json.parseToJsonElement(text).jsonObject
        require(model.getValue("schema").jsonPrimitive.int == 1) { "unknown project model schema" }
        val units = model.rows("units").map { it.jsonObject }
        val scalaUnits = units.filter { it.text("language") == "scala" }
        require(scalaUnits.isNotEmpty()) { "project_model has no Scala compilation units" }
        val ids = scalaUnits.map { it.text("id") }.toSet()
        val archives = JvmClasspath(objects)
        fun resolve(ref: JsonObject): JsonObject {
            val matches = units.filter { unit ->
                unit.text("project") == ref.text("project") &&
                    if (ref.text("unit").isNotEmpty()) unit.text("id") == ref.text("unit")
                    else ref.getValue("attributes").jsonObject.isNotEmpty() &&
                        unit.rows("variants").contains(ref.getValue("attributes"))
            }
            require(matches.size == 1) { "project reference requires one matching unit: ${ref.text("project")}" }
            return matches.single()
        }
        fun library(ref: JsonObject) = when (ref.text("kind")) {
            "artifact" -> {
                val value = ref.getValue("object").jsonObject
                val hash = value.text("hash")
                val path = if (ref.text("role") in setOf("classpath", "friend")) archives.artifact(hash, value.text("name")) else objectPath(value)
                Library(path, "object:$hash")
            }
            "tree" -> {
                val path = root.resolve(ref.text("path")).normalize()
                Library(path, "tree:${root.relativize(path).wire()}")
            }
            "sdk" -> Library(sdk(ref.text("name")).resolve(ref.text("path")).normalize(),
                "sdk:${ref.text("name")}:${ref.text("path")}")
            else -> error("unexpected Scala input kind: ${ref.text("kind")}")
        }
        fun outputs(unit: JsonObject) = unit["outputs"]?.jsonArray.orEmpty()
            .map { root.resolve(it.jsonPrimitive.content).normalize() }
        return scalaUnits.map { unit ->
            val refs = unit.rows("references").map { it.jsonObject }
            val runtime = refs.filter { it.text("role") == "runtime" && it.text("kind") == "sdk" }
            require(runtime.size == 1) { "Scala module requires one JDK reference: ${unit.text("id")}" }
            val classpath = refs.filter { it.text("role") in setOf("classpath", "friend") }
            val projects = classpath.filter { it.text("kind") == "project" }.map(::resolve)
            val sourceOutputs = (projects.filter { it.text("id") in ids } + unit).flatMap(::outputs).toSet()
            val libraries = classpath.flatMap { ref ->
                if (ref.text("kind") != "project") listOf(library(ref))
                else resolve(ref).takeUnless { it.text("id") in ids }?.let(::outputs).orEmpty()
                    .filter { it !in sourceOutputs && Files.isDirectory(it) }
                    .map { Library(it, "tree:${root.relativize(it).wire()}") }
            }.distinct()
            val classpathObjects = libraries.filter { it.module.startsWith("object:") }.associate { it.module.removePrefix("object:") to it.path }
            val plugins = refs.filter { it.text("role") == "plugin" }.map(::library)
            val projectBinaries = projects.filter { it.text("id") !in ids }.flatMap(::outputs).filter { it !in sourceOutputs }
            val treeBinaries = ((libraries + plugins).filter { it.module.startsWith("tree:") }.map { it.path } + projectBinaries).toSet()
            ModuleInputs(
                id = unit.text("id"),
                sources = unit.rows("sources").map { root.resolve(it.jsonPrimitive.content).normalize() }.distinct(),
                libraries = libraries,
                jdk = library(runtime.single()).path,
                dependencies = projects.map { it.text("id") }.filter { it in ids }.distinct(),
                compilerArguments = archives.arguments(unit.rows("compiler_arguments").map { it.jsonPrimitive.content }, root, sdks, classpathObjects),
                plugins = plugins.map { it.path },
                generated = unit.getValue("generated").jsonObject.map { (name, value) ->
                    root.resolve(name).normalize() to objectPath(value.jsonObject)
                }.toMap(),
                directory = root.resolve(unit["directory"]?.jsonPrimitive?.content ?: ".").normalize(),
                binaryInputs = sources.filterKeys { name -> treeBinaries.any { root.resolve(name).normalize().startsWith(it) } },
            )
        }
    }
}
