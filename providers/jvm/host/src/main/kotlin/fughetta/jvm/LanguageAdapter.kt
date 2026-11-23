package fughetta.jvm

import java.nio.file.Path
import kotlinx.serialization.json.*

/**
 * A compiler projection loaded into the JVM host. The host owns framing, session identity,
 * epochs and unit keys; an adapter answers only with compiler data for named modules.
 * One adapter instance serves one helper session and is closed with it.
 */
interface LanguageAdapter : AutoCloseable {
    /** Starts compiler state rooted at [root]; [host] names the object store and SDK roots. */
    fun open(root: Path, session: JsonObject, host: JsonObject, initialization: JsonElement?)

    /** Installs one update: session, documents, sources, artifacts, generated files and facts. */
    fun install(params: JsonObject)

    /** Modules containing any of [files]; every module when [files] is empty. */
    fun units(files: Set<Path>): List<String>

    /** Projects [module] into one shard completing [template]. */
    fun evaluate(session: JsonObject, template: JsonObject, module: String): Evaluation

    /** Current channel inputs for previously observed [reads]; unknown reads are omitted. */
    fun observe(session: JsonObject, reads: JsonArray): JsonArray
}

/** [producers] maps an observed read key to the modules that produce it. */
class Evaluation(val shard: JsonObject, val producers: Map<String, List<String>>)
