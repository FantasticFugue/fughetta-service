package fughetta.kotlin

import kotlinx.serialization.json.*
import org.jetbrains.kotlin.analysis.api.KaSession
import org.jetbrains.kotlin.analysis.api.symbols.*
import java.security.MessageDigest
import org.jetbrains.kotlin.analysis.api.renderer.types.impl.KaTypeRendererForSource
import org.jetbrains.kotlin.analysis.api.symbols.markers.KaTypeParameterOwnerSymbol
import org.jetbrains.kotlin.types.Variance

/** The host supplies anchored identities for declarations without a language key. */
class Identities(private val project: String, private val provider: String = "kotlin") {
    val bindings = linkedMapOf<String, JsonObject>()

    fun identify(session: KaSession, symbol: KaSymbol, module: (KaSymbol) -> String,
                 anchor: (KaSymbol) -> JsonObject): String = with(session) {
        var ownerId: String? = null
        fun bindOwner(symbol: KaSymbol) { ownerId = identify(session, symbol, module, anchor) }
        val key = when (symbol) {
            is KaPackageSymbol -> buildJsonObject { put("kind", "package"); put("name", symbol.fqName.asString()) }
            is KaClassLikeSymbol -> symbol.classId?.takeUnless { it.isLocal }?.let { buildJsonObject {
                put("kind", if (symbol is KaTypeAliasSymbol) "alias" else "class")
                put("name", it.asString())
            } }
            is KaTypeParameterSymbol -> buildJsonObject {
                val owner = requireNotNull(symbol.containingDeclaration)
                put("kind", "type_parameter"); bindOwner(owner)
                put("index", (owner as KaTypeParameterOwnerSymbol).typeParameters.indexOf(symbol))
            }
            is KaValueParameterSymbol -> buildJsonObject {
                val owner = requireNotNull(symbol.containingDeclaration) as KaFunctionSymbol
                put("kind", "value_parameter"); bindOwner(owner)
                put("index", owner.valueParameters.indexOf(symbol))
            }
            is KaPropertyAccessorSymbol -> buildJsonObject {
                put("kind", if (symbol is KaPropertyGetterSymbol) "getter" else "setter")
                bindOwner(requireNotNull(symbol.containingDeclaration))
            }
            is KaBackingFieldSymbol -> buildJsonObject {
                put("kind", "backing_field"); bindOwner(symbol.owningProperty)
            }
            is KaConstructorSymbol -> buildJsonObject {
                put("kind", "constructor")
                bindOwner(requireNotNull(symbol.containingDeclaration))
                putJsonArray("parameters") { symbol.valueParameters.forEach {
                    add(it.returnType.render(KaTypeRendererForSource.WITH_QUALIFIED_NAMES_WITHOUT_PARAMETER_NAMES, Variance.INVARIANT))
                } }
            }
            is KaCallableSymbol -> symbol.callableId?.let { callable -> buildJsonObject {
                put("kind", if (symbol is KaFunctionSymbol) "function" else "property")
                put("name", callable.toString())
                if (symbol is KaTypeParameterOwnerSymbol) putJsonArray("type_parameters") {
                    symbol.typeParameters.forEach { parameter -> add(buildJsonObject {
                        put("variance", parameter.variance.toString())
                        putJsonArray("bounds") { parameter.upperBounds.forEach {
                            add(it.render(KaTypeRendererForSource.WITH_QUALIFIED_NAMES_WITHOUT_PARAMETER_NAMES, Variance.INVARIANT))
                        } }
                    }) }
                }
                symbol.receiverParameter?.let { put("receiver", it.returnType.render(KaTypeRendererForSource.WITH_QUALIFIED_NAMES_WITHOUT_PARAMETER_NAMES, Variance.INVARIANT)) }
                putJsonArray("contexts") { symbol.contextParameters.forEach { add(it.returnType.render(KaTypeRendererForSource.WITH_QUALIFIED_NAMES_WITHOUT_PARAMETER_NAMES, Variance.INVARIANT)) } }
                if (symbol is KaFunctionSymbol) putJsonArray("parameters") {
                    symbol.valueParameters.forEach { parameter -> add(buildJsonObject {
                        put("type", parameter.returnType.render(KaTypeRendererForSource.WITH_QUALIFIED_NAMES_WITHOUT_PARAMETER_NAMES, Variance.INVARIANT)); put("vararg", parameter.isVararg)
                    }) }
                }
            } }
            else -> null
        }
        val identity = buildJsonObject {
            put("project", project); put("module", module(symbol)); put("provider", provider)
            if (key == null) put("anchor", anchor(symbol))
            else {
                put("domain", "kotlin"); put("native_key", key.toString())
                ownerId?.let { put("owner", it) }
            }
        }
        val id = "symbol-" + MessageDigest.getInstance("SHA-256").digest(identity.toString().toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
        bindings.putIfAbsent(id, buildJsonObject { put("declaration", identity); put("symbol", id) })
        id
    }
}
