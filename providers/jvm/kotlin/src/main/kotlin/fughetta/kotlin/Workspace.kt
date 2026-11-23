package fughetta.kotlin

import java.nio.file.Path
import kotlinx.serialization.json.*
import org.jetbrains.kotlin.analysis.api.analyze
import org.jetbrains.kotlin.analysis.api.platform.modification.publishModuleOutOfBlockModificationEvent
import org.jetbrains.kotlin.analysis.api.projectStructure.KaModule
import org.jetbrains.kotlin.analysis.api.symbols.KaSymbol
import org.jetbrains.kotlin.psi.KtFile
import org.jetbrains.kotlin.utils.exceptions.KotlinIllegalArgumentExceptionWithAttachments

/** The helper owns one installed graph; text-only installations keep its environment. */
class Workspace(private val root: Path, private val project: String, private val provider: String = "kotlin") : AutoCloseable {
    private var inputs: List<ModuleInputs> = emptyList()
    private var environment: Environment? = null
    private data class Evaluated(val unit: String, val context: String, val records: Projection)
    // One value projection per module. Never retain KaSession objects or grow
    // a second copy for each query/context requested by the host.
    private val evaluated = mutableMapOf<String, Evaluated>()
    private var consumers = emptyMap<String, Set<String>>()
    private val observations = mutableMapOf<Pair<String, List<String>?>, SemanticValues?>()

    private fun invalidate(changed: Set<String>) {
        val affected = changed.toMutableSet()
        val pending = ArrayDeque(changed)
        while (pending.isNotEmpty()) {
            consumers[pending.removeFirst()].orEmpty().forEach {
                if (affected.add(it)) pending.addLast(it)
            }
        }
        affected.forEach { module -> evaluated.remove(module); observations.keys.removeIf { it.first == module } }
    }

    fun install(next: List<ModuleInputs>, documents: Map<Path, String>) {
        val current = environment
        if (current != null && next == inputs && !current.javaNamesChanged(documents)) {
            try {
                val changed = current.update(documents)
                changed.modules.forEach { module -> evaluated.remove(module); observations.keys.removeIf { it.first == module } }
                invalidate(changed.declarations)
            } catch (failure: Throwable) {
                evaluated.clear()
                throw failure
            }
            return
        }
        val nextSources = next.flatMap { it.sources }.map { it.toAbsolutePath().normalize() }.toSet()
        val oldGenerated = inputs.flatMap { it.generated.entries }.associate { it.key.toAbsolutePath().normalize() to it.value }
        val nextGenerated = next.flatMap { it.generated.entries }.associate { it.key.toAbsolutePath().normalize() to it.value }
        val texts = current?.sourceTexts().orEmpty().filterKeys { it in nextSources && oldGenerated[it] == nextGenerated[it] } +
            documents.mapKeys { it.key.toAbsolutePath().normalize() }
        val replacement = Environment(next, refreshArchives = current != null &&
            next.associate { it.id to it.binaryInputs } != inputs.associate { it.id to it.binaryInputs }, documents = texts)
        try {
            replacement.update(documents)
        } catch (error: Throwable) {
            replacement.close()
            throw error
        }
        environment = replacement
        inputs = next.toList()
        consumers = buildMap<String, MutableSet<String>> {
            next.forEach { input -> (input.dependencies + input.friends).forEach { dependency ->
                getOrPut(dependency) { mutableSetOf() }.add(input.id)
            } }
        }
        evaluated.clear()
        observations.clear()
        current?.close()
    }

    fun moduleInputs(module: String): ModuleInputs = inputs.single { it.id == module }

    fun dependencies(module: String): List<String> {
        val selected = linkedSetOf<String>()
        fun visit(id: String) {
            val input = moduleInputs(id)
            (input.dependencies + input.friends).forEach { if (selected.add(it)) visit(it) }
        }
        visit(module)
        return selected.toList()
    }

    fun opaqueInputs(module: String) = (listOf(module) + dependencies(module)).any { it in checkNotNull(environment).opaqueModules }

    @OptIn(org.jetbrains.kotlin.analysis.api.KaPlatformInterface::class)
    fun observe(module: String, selectors: List<String>? = null): SemanticValues? {
        if (inputs.none { it.id == module } || opaqueInputs(module)) return null
        val key = module to selectors
        if (!observations.containsKey(key)) {
            val installed = checkNotNull(environment)
            observations[key] = try {
                semanticValues(installed, module, selectors)
            } catch (stale: KotlinIllegalArgumentExceptionWithAttachments) {
                if (stale.message?.contains("must be valid") != true) throw stale
                // Standalone keeps a module's analysis session cached across unrelated write
                // actions but never delivers the compiler's write-finished callback. Republishing
                // the module's out-of-block invalidation is what makes its next read coherent.
                installed.modules[module]?.publishModuleOutOfBlockModificationEvent()
                semanticValues(installed, module, selectors)
            }
        }
        return observations[key]
    }

    fun units(files: Set<Path> = emptySet()): List<String> = inputs.filter { input ->
        files.isEmpty() || input.sources.any { it in files }
    }.map { it.id }

    fun evaluate(module: String, unit: String, context: String,
                 externalModule: (KaModule) -> String, anchor: (KaSymbol) -> JsonObject): Projection {
        val installed = checkNotNull(environment) { "Kotlin workspace has not been installed" }
        val selected = requireNotNull(installed.modules[module]) { "unknown Kotlin module: $module" }
        evaluated[module]?.let { if (it.unit == unit && it.context == context) return it.records }
        val moduleIds = installed.modules.entries.associate { it.value to it.key }
        val result = Projection(root, project, unit, context, inputs.single { it.id == module }.generated.keys, provider,
            dependencies(module).toSet())
        for (file in installed.session.modulesWithFiles.getValue(selected).filterIsInstance<KtFile>()) {
            analyze(file) {
                result.file(this, file, module = { symbol ->
                    val owner = symbol.containingModule
                    moduleIds[owner] ?: externalModule(owner)
                }, anchor = anchor)
            }
        }
        evaluated[module] = Evaluated(unit, context, result)
        return result
    }

    override fun close() {
        environment?.close()
        environment = null
        inputs = emptyList()
        consumers = emptyMap()
        evaluated.clear()
        observations.clear()
    }
}
