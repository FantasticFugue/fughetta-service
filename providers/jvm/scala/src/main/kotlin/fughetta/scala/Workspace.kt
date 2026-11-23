package fughetta.scala

import java.nio.file.Files
import java.nio.file.Path
import scala.reflect.io.VirtualDirectory

internal class Workspace(private val root: Path, private val project: String, private val provider: String) : AutoCloseable {
    private data class Installed(val input: ModuleInputs, val texts: Map<Path, String>)

    private data class Compiled(val projection: Projection, val exports: VirtualDirectory?, val values: SemanticValues?)

    private var installed = emptyMap<String, Installed>()
    private var documents = emptyMap<Path, String>()
    private var objects = emptyMap<String, String>()
    private val compiled = linkedMapOf<String, Compiled>()

    fun update(inputs: List<ModuleInputs>, edits: Map<Path, String>, sourceObjects: Map<String, String>) {
        val byId = inputs.associateBy { it.id }.toMutableMap()
        require(byId.size == inputs.size) { "duplicate Scala module id" }
        val modeledFiles = inputs.flatMap { it.sources }.toSet()
        (sourceObjects.keys.map { root.resolve(it).normalize() } + edits.keys).distinct()
            .filter { it.startsWith(root) && it !in modeledFiles && it.fileName.toString().endsWith(".sc") &&
                it.fileName.toString() != "build.sc" }
            .sorted().forEach { script ->
                val owner = inputs.filter { unit -> unit.directory?.let(script::startsWith) == true }
                    .maxWithOrNull(compareBy<ModuleInputs> { it.directory!!.nameCount }.thenByDescending { it.id })
                    ?: return@forEach
                val text = edits[script] ?: Files.readString(script)
                if (text.lineSequence().any { it.trimStart().startsWith("//>") }) return@forEach
                val id = "script:" + root.relativize(script).wire()
                require(byId.putIfAbsent(id, owner.copy(id = id, sources = owner.sources + listOf(script),
                    projectedSources = setOf(script))) == null) { "Scala script unit conflicts with a project unit: $id" }
            }
        val ordered = linkedSetOf<String>()
        val visiting = mutableSetOf<String>()
        fun visit(id: String) {
            if (id in ordered) return
            require(visiting.add(id)) { "cyclic Scala module dependency: $id" }
            requireNotNull(byId[id]) { "missing Scala module: $id" }.dependencies.forEach(::visit)
            visiting.remove(id)
            ordered.add(id)
        }
        byId.keys.forEach(::visit)

        val nextDocuments = mutableMapOf<Path, String>()
        val next = ordered.associateWith { id ->
            val input = byId.getValue(id)
            val previous = installed[id]
            val texts = input.sources.associateWith { path ->
                val name = root.relativize(path).wire()
                val generated = input.generated[path]
                val unchanged = previous != null && path in previous.texts &&
                    previous.input.generated[path] == generated && objects[name] == sourceObjects[name]
                val overlay = edits[path] ?: documents[path]?.takeIf { unchanged }
                if (overlay != null) nextDocuments[path] = overlay
                overlay ?: previous?.texts?.get(path)?.takeIf { unchanged && (generated != null || name in sourceObjects) }
                    ?: Files.readString(generated ?: path)
            }
            Installed(input, texts)
        }
        val oldProducers = installed.values.flatMap { it.input.dependencies }.toSet()
        val newProducers = next.values.flatMap { it.input.dependencies }.toSet()
        val dirty = ((installed.keys - next.keys) + (newProducers - oldProducers)).toMutableSet()
        next.forEach { (id, item) ->
            if (item != installed[id] || item.input.dependencies.any { it in dirty }) dirty.add(id)
        }
        dirty.forEach { compiled.remove(it) }
        installed = next
        documents = nextDocuments
        objects = sourceObjects.toMap()
    }

    fun units(files: Set<Path>) = installed.values.filter { unit ->
        files.isEmpty() || (unit.input.projectedSources ?: unit.input.sources).any { it in files }
    }.map { it.input.id }

    fun input(id: String) = requireNotNull(installed[id]) { "unknown Scala module: $id" }.input

    fun dependencies(id: String): List<String> = buildList {
        val seen = mutableSetOf<String>()
        fun visit(module: String) {
            if (!seen.add(module)) return
            add(module)
            input(module).dependencies.forEach(::visit)
        }
        input(id).dependencies.forEach(::visit)
    }

    private fun compilation(id: String): Compiled {
        compiled[id]?.let { return it }
        val item = installed.getValue(id)
        val upstream = dependencies(id).mapNotNull { dependency ->
            compilation(dependency).exports?.let { it to dependency }
        }
        val environment = Environment(item.input, upstream.map { it.first })
        val sources = item.texts.map { (file, text) ->
            Source(file, text, file in item.input.generated, root.relativize(file).wire())
        }
        val producerPaths = upstream.associate { (output, dependency) -> output.path() to dependency }
        val export = installed.values.any { id in it.input.dependencies } ||
            item.input.projectedSources == null && sources.any { it.file.toString().endsWith(".java") }
        var projection: Projection? = null
        var values: SemanticValues? = null
        fun capture(typed: Compilation) {
            projection = Projection(root, project, provider, item.input, typed, producerPaths)
            values = semanticValues(typed)
        }
        return environment.compile(sources, export, ::capture).use { result ->
            if (projection == null) capture(result)
            val records = checkNotNull(projection)
            records.diagnostics(result.messages)
            Compiled(records, result.exports, values.takeUnless { result.failed })
                .also { compiled[id] = it }
        }
    }

    fun observe(id: String): SemanticValues? {
        if (id !in installed || opaque(id)) return null
        return compilation(id).values
    }

    fun opaque(id: String) = (listOf(id) + dependencies(id)).any {
        val input = input(it)
        input.plugins.isNotEmpty() || input.compilerArguments.any { argument ->
            argument == "-opt" || argument.startsWith("-opt:") || argument.startsWith("-opt-inline-from") ||
                argument in setOf("-optimise", "-optimize")
        }
    }

    fun evaluate(id: String): Projection = compilation(id).projection

    override fun close() {
        compiled.clear()
        installed = emptyMap()
        documents = emptyMap()
        objects = emptyMap()
    }
}
