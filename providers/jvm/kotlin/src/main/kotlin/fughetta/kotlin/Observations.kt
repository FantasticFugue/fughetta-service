package fughetta.kotlin

import com.intellij.psi.PsiErrorElement
import com.intellij.psi.util.PsiTreeUtil
import java.security.MessageDigest
import kotlinx.serialization.json.*
import org.jetbrains.kotlin.analysis.api.analyze
import org.jetbrains.kotlin.analysis.api.KaConstantInitializerValue
import org.jetbrains.kotlin.analysis.api.components.KaDiagnosticCheckerFilter
import org.jetbrains.kotlin.analysis.api.renderer.declarations.impl.KaDeclarationRendererForSource
import org.jetbrains.kotlin.analysis.api.symbols.*
import org.jetbrains.kotlin.analysis.api.symbols.markers.KaDeclarationContainerSymbol
import org.jetbrains.kotlin.analysis.api.symbols.markers.KaNamedSymbol
import org.jetbrains.kotlin.psi.*

/** Compiler values used for reuse, independent of published symbol IDs and locations. */
data class SemanticValues(val declarations: String, val inlineBodies: String)

/**
 * What a consumer observed in its dependencies. Selectors name the declarations a lookup could
 * resolve and the classifiers whose member sets a consumer inherits or exhausts. Modules that a
 * consumer references through a sealed hierarchy keep the whole-module digest: a new subclass is
 * not owned by the declaration the consumer named.
 */
data class ObservationScope(val selectors: List<String>, val sealedModules: Set<String>)

fun observationSelectors(selectors: List<String>): List<String> = selectors.distinct().sorted()

fun observationDigest(selectors: List<String>): String = MessageDigest.getInstance("SHA-256")
    .digest(JsonArray(observationSelectors(selectors).map(::JsonPrimitive)).toString().toByteArray(Charsets.UTF_8))
    .joinToString("") { "%02x".format(it) }

/** The lookup name of a declaration; members of a shape report their owning declaration. */
internal fun declarationName(symbol: KaSymbol): String? = when (symbol) {
    // Parameters and binders are never lookup names; their owner already holds the declaration.
    is KaTypeParameterSymbol, is KaValueParameterSymbol -> null
    is KaConstructorSymbol -> symbol.containingClassId?.shortClassName?.asString()
    is KaPropertyAccessorSymbol -> (symbol.psi as? KtPropertyAccessor)?.property?.name
    is KaBackingFieldSymbol -> symbol.owningProperty.name.asString()
    is KaNamedSymbol -> symbol.name.asString()
    else -> null
}

private fun scopeParts(selectors: List<String>?): Pair<Set<String>, Set<String>>? {
    if (selectors == null) return null
    val names = mutableSetOf<String>()
    val classes = mutableSetOf<String>()
    for (selector in selectors) when {
        selector.startsWith("name:") -> names.add(selector.removePrefix("name:"))
        selector.startsWith("type:") -> classes.add(selector.removePrefix("type:"))
        else -> throw IllegalArgumentException("unknown observation selector: $selector")
    }
    return names to classes
}

/**
 * Declaration and inline digests of one module. A null selector set keeps the whole-module
 * declaration digest; selectors narrow it to declarations a consumer could name, plus every
 * member of a classifier shape it observes. Inline bodies stay whole-module: a body can inline
 * further declarations that its consumer never names.
 */
fun semanticValues(environment: Environment, module: String, selectors: List<String>? = null): SemanticValues? {
    val selected = environment.modules[module] ?: return null
    val scope = scopeParts(selectors)
    val names = scope?.first
    val shapes = scope?.second
    val declarations = sortedSetOf<String>()
    val bodies = sortedSetOf<String>()
    for (file in environment.session.modulesWithFiles.getValue(selected).filterIsInstance<KtFile>()) {
        if (PsiTreeUtil.collectElementsOfType(file, PsiErrorElement::class.java).isNotEmpty()) return null
        val successful = analyze(file) {
            if (file.diagnostics(KaDiagnosticCheckerFilter.EXTENDED_AND_COMMON_CHECKERS).any { it.severity.name == "ERROR" }) return@analyze false
            val packageName = file.packageFqName.asString()
            // Package annotations affect resolution and declarations alike; they stay unconditional.
            declarations.add(buildJsonArray {
                add(packageName); add(JsonArray(file.annotationEntries.map { JsonPrimitive(it.text) }))
            }.toString())
            fun emit(entry: String, name: String?, shape: Boolean) {
                if (names == null || name == null || shape || name in names) declarations.add(entry)
            }
            var complete = true
            fun declaration(symbol: KaDeclarationSymbol, owner: String, inheritedShape: Boolean = false, hint: String? = null) {
                val classId = (symbol as? KaClassLikeSymbol)?.classId?.asFqNameString()
                val shape = inheritedShape || (classId != null && shapes != null && classId in shapes)
                // Accessors report their property's name; a synthesized accessor has no PSI of its own.
                val name = hint ?: declarationName(symbol)
                val signature = symbol.render(KaDeclarationRendererForSource.WITH_QUALIFIED_NAMES)
                val key = "$owner/$signature"
                emit(key, name, shape)
                if (symbol is KaFunctionSymbol) {
                    symbol.valueParameters.forEachIndexed { index, parameter ->
                        (parameter.psi as? KtParameter)?.defaultValue?.let {
                            emit(buildJsonArray { add(key); add(index); add(it.text) }.toString(), name, shape)
                        }
                    }
                }
                if (symbol is KaKotlinPropertySymbol) {
                    if (symbol.isConst) (symbol.initializer as? KaConstantInitializerValue)?.let {
                        emit(buildJsonArray { add(key); add(it.constant.render()) }.toString(), name, shape)
                    }
                    symbol.getter?.let { declaration(it, key, shape, name) }
                    symbol.setter?.let { declaration(it, key, shape, name) }
                }
                val consumedBody = (symbol is KaNamedFunctionSymbol && (symbol.isInline || symbol.contractEffects.isNotEmpty())) ||
                    (symbol is KaPropertyAccessorSymbol && symbol.isInline)
                if (consumedBody) {
                    val source = symbol.psi as? KtDeclarationWithBody
                    // A compiler-generated implementation without a source body
                    // keeps its module's source dependency instead of guessing it.
                    val body = source?.bodyExpression
                    if (body != null) bodies.add(buildJsonArray { add(key); add(body.text) }.toString())
                    else complete = false
                }
                if (symbol is KaDeclarationContainerSymbol)
                    symbol.combinedDeclaredMemberScope.declarations.forEach { declaration(it, key, shape) }
            }
            file.symbol.fileScope.declarations.forEach { declaration(it, packageName) }
            complete
        }
        if (!successful) return null
    }
    fun digest(values: Set<String>) = MessageDigest.getInstance("SHA-256")
        .digest(JsonArray(values.map(::JsonPrimitive)).toString().toByteArray(Charsets.UTF_8))
        .joinToString("") { "%02x".format(it) }
    return SemanticValues(digest(declarations), digest(bodies))
}
