package fughetta.scala

import java.nio.file.Path
import java.util.IdentityHashMap
import kotlinx.serialization.json.*
import scala.reflect.internal.Symbols.Symbol
import scala.reflect.internal.Trees.Tree
import scala.reflect.internal.Types.Type
import scala.reflect.internal.Types.TypeBounds
import scala.reflect.internal.util.Position

internal fun Tree.nodes() = children().items().filterIsInstance<Tree>()
internal fun Symbol?.usable() = this != null && exists() && !isError()
internal fun Type?.known() = this != null && !isErroneous() && javaClass.simpleName !in setOf("NoType$", "NoPrefix$")
internal fun Symbol.label() = if (isConstructor()) owner().name().decoded().trim() else name().decoded().trim()
internal fun Symbol.packageSymbol() = hasPackageFlag() || isPackageClass()
internal fun Symbol.kind(): String = when {
    packageSymbol() -> "namespace"
    isAnonymousFunction() -> "function"
    isTypeParameterOrSkolem() || isValueParameter() -> "parameter"
    isModuleOrModuleClass() -> "scala:object"
    isTrait() -> "scala:trait"
    isClass() -> "type"
    isType() -> "scala:type_member"
    isConstructor() -> "constructor"
    isAccessor() -> "property"
    isMethod() -> if (owner().isClass()) "method" else "function"
    owner().isClass() -> if (isJavaDefined()) "field" else "property"
    else -> "variable"
}

internal val primitiveDescriptors = mapOf(
    "scala.Boolean" to "Z", "scala.Byte" to "B", "scala.Char" to "C", "scala.Double" to "D",
    "scala.Float" to "F", "scala.Int" to "I", "scala.Long" to "J", "scala.Short" to "S", "scala.Unit" to "V",
)

internal fun Symbol.jvmClass(): String = javaBinaryNameString().replace('.', '/') +
    if (isModuleOrModuleClass() && !javaBinaryNameString().endsWith('$')) "$" else ""

internal fun Type.descriptor(result: Boolean = false): String {
    val type = dealias().widen()
    val symbol = type.typeSymbol()
    if (!symbol.usable()) return "Ljava/lang/Object;"
    val name = symbol.fullName()
    primitiveDescriptors[name]?.let { return if (it == "V" && !result) "Lscala/runtime/BoxedUnit;" else it }
    return when (name) {
        "scala.<byname>" -> "Lscala/Function0;"
        "scala.<repeated>" -> "Lscala/collection/immutable/Seq;"
        "scala.<repeated...>", "scala.Array" -> {
            val component = type.typeArgs().items().firstOrNull()
            if (component == null || component.typeSymbol().isTypeParameterOrSkolem()) "Ljava/lang/Object;"
            else "[" + component.descriptor()
        }
        "scala.Nothing" -> "Lscala/runtime/Nothing$;"
        "scala.Null" -> "Lscala/runtime/Null$;"
        "scala.Any", "scala.AnyRef", "scala.AnyVal" -> "Ljava/lang/Object;"
        else -> when {
            symbol.isClass() -> "L${symbol.jvmClass()};"
            symbol.info() is TypeBounds -> {
                val bound = (symbol.info() as TypeBounds).hi()
                if (bound.typeSymbol() == symbol) "Ljava/lang/Object;" else bound.descriptor()
            }
            else -> "Ljava/lang/Object;"
        }
    }
}

internal fun nativeKey(kind: String, name: String, signature: String? = null) = buildJsonObject {
    put("kind", kind); put("name", name)
    signature?.let { put("parameters", it) }
}.toString()

internal class Identities(
    private val project: String,
    private val provider: String,
    private val module: ModuleInputs,
    private val root: Path,
    private val sources: Map<String, Source>,
    private val producerPaths: Map<String, String>,
) {
    private val ids = IdentityHashMap<Symbol, String>()
    private val anchors = IdentityHashMap<Symbol, JsonObject>()
    val bindings = linkedMapOf<String, JsonObject>()

    fun source(position: Position?): Source? =
        position?.takeIf { it.isDefined() }?.let { sources[it.source().file().path()] }?.takeIf { it.contains(position) }

    fun canonical(symbol: Symbol): Symbol = when {
        symbol.isPackageClass() || (symbol.isModuleOrModuleClass() && symbol.isClass()) ->
            symbol.sourceModule().takeIf { it.usable() } ?: symbol
        symbol.isTypeSkolem() -> symbol.deSkolemize().takeIf { it.usable() } ?: symbol
        symbol.isTerm() && !symbol.isMethod() && !symbol.isValueParameter() && symbol.owner().let { it.usable() && it.isClass() } ->
            symbol.getterIn(symbol.owner()).takeIf { it.usable() } ?: symbol
        else -> symbol
    }

    fun owner(symbol: Symbol): Symbol? = symbol.owner()?.takeIf { it.usable() && !it.isEffectiveRoot() && !it.isRootSymbol() }
        ?.let(::canonical)?.takeUnless { it == symbol }

    fun local(symbol: Symbol): Boolean =
        symbol.isAnonymousFunction() || symbol.isLocalToBlock() || generateSequence(owner(symbol), ::owner).any {
            it.isAnonymousFunction() || it.isLocalToBlock() || it.name().toString().startsWith("FughettaScript_") ||
                (it.isMethod() && !symbol.isParameter())
        }

    fun module(symbol: Symbol): String {
        if (symbol.packageSymbol()) return "scala/packages"
        val chain = generateSequence(symbol, ::owner).takeWhile { !it.packageSymbol() }.toList()
        if (chain.any { source(it.pos()) != null }) return module.id
        for (candidate in chain.asReversed()) {
            val file = candidate.associatedFile() ?: continue
            val path = file.path().replace('\\', '/')
            producerPaths.entries.firstOrNull { (directory, _) -> path.startsWith("$directory/") }?.let { return it.value }
            val container = file.underlyingSource().let { if (it.isDefined()) it.get().path() else path }
            module.libraries.firstOrNull { library ->
                container == library.path.toString() || path == library.path.wire() ||
                    path.startsWith("${library.path.wire()}/") || path.startsWith("${library.path.wire()}(")
            }?.let { return it.module }
        }
        return if (symbol.fullName().startsWith("java.") || symbol.fullName().startsWith("javax.")) "scala/jdk" else "scala/builtins"
    }

    fun anchor(symbol: Symbol?, source: Source, declaration: String, owner: String?): String {
        val value = sourceAnchor(source, declaration, owner)
        if (symbol != null) anchors[canonical(symbol)] = value
        return if (symbol == null) bind(anchored(value)) else id(symbol)
    }

    private fun sourceAnchor(source: Source, declaration: String, owner: String?) = buildJsonObject {
        put("file", root.relativize(source.file).wire()); put("declaration", declaration)
        owner?.let { put("owner", it) }
    }

    private fun anchored(value: JsonObject) = buildJsonObject {
        put("project", project); put("module", module.id); put("provider", provider); put("anchor", value)
    }

    fun id(raw: Symbol): String {
        val symbol = canonical(raw)
        ids[symbol]?.let { return it }
        val parent = owner(symbol)
        val parentId = parent?.let(::id)
        val anchor = anchors[symbol] ?: if (local(symbol)) source(symbol.pos())?.let {
            sourceAnchor(it, "${symbol.kind()}:${symbol.label()}:${it.index(symbol.pos().point())}", parentId)
        } else null
        val declaration = if (anchor != null) anchored(anchor) else buildJsonObject {
            put("project", project); put("module", module(symbol)); put("provider", provider)
            put("domain", "scala"); put("native_key", key(symbol))
            if (!symbol.packageSymbol() && !symbol.isClass() && !symbol.isModuleOrModuleClass()) {
                parentId?.let { put("owner", it) }
            }
        }
        return bind(declaration).also { ids[symbol] = it }
    }

    fun parameter(symbol: Symbol): Pair<String, Int>? {
        val canonical = canonical(symbol)
        val binder = owner(canonical) ?: return null
        val parameters = binder.typeParams().items().toList()
        val index = parameters.indexOfFirst { it == canonical || it.name() == canonical.name() }
        return if (index >= 0) id(binder) to index else null
    }

    private fun key(symbol: Symbol): String {
        val kind = symbol.kind()
        return when {
            symbol.packageSymbol() -> nativeKey(kind, symbol.fullName())
            symbol.isTypeParameterOrSkolem() ->
                nativeKey("type_parameter", parameter(symbol)?.second?.toString() ?: symbol.name().toString())
            symbol.isValueParameter() -> {
                val groups = symbol.owner().paramss().items().map { it.items().toList() }.toList()
                val group = groups.indexOfFirst { parameters -> parameters.any { it == symbol || it.name() == symbol.name() } }
                val index = groups.getOrNull(group)?.indexOfFirst { it == symbol || it.name() == symbol.name() } ?: -1
                nativeKey("value_parameter", if (index >= 0) "$group:$index" else symbol.name().toString())
            }
            symbol.isClass() || symbol.isModuleOrModuleClass() ->
                nativeKey(kind, (if (symbol.isModuleOrModuleClass() && !symbol.isClass()) symbol.moduleClass() else symbol).jvmClass())
            symbol.isMethod() -> nativeKey(kind, symbol.name().toString(), parameters(symbol))
            else -> nativeKey(kind, symbol.name().toString().trim())
        }
    }

    private fun bind(declaration: JsonObject): String {
        val id = "symbol-${digest(declaration.toString())}"
        bindings.putIfAbsent(id, buildJsonObject { put("declaration", declaration); put("symbol", id) })
        return id
    }

    fun declaration(id: String) = bindings.getValue(id).getValue("declaration")
}

internal fun parameters(symbol: Symbol): String = symbol.paramss().items().flatMap { it.items() }
    .joinToString(separator = "") { it.info().descriptor() }
