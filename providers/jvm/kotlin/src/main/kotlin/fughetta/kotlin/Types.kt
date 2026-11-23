package fughetta.kotlin

import kotlinx.serialization.json.*
import org.jetbrains.kotlin.analysis.api.symbols.*
import org.jetbrains.kotlin.analysis.api.symbols.markers.KaTypeParameterOwnerSymbol
import org.jetbrains.kotlin.analysis.api.types.*
import java.security.MessageDigest

/** Only JSON values survive a projection; callers supply persistent declaration IDs. */
class Types {
    val records = linkedMapOf<String, JsonObject>()

    internal fun node(kind: String, fields: JsonObjectBuilder.() -> Unit = {}): String {
        val payload = buildJsonObject {
            put("domain", "kotlin"); put("kind", kind)
            listOf("arguments", "constraints", "elements", "type_parameters", "parameter_groups",
                "qualifiers", "application_arguments").forEach { put(it, JsonArray(emptyList())) }
            put("extensions", buildJsonObject {})
            fields()
        }
        val id = "type-" + MessageDigest.getInstance("SHA-256").digest(payload.toString().toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
        records.putIfAbsent(id, JsonObject(linkedMapOf<String, JsonElement>("id" to JsonPrimitive(id)).apply { putAll(payload) }))
        return id
    }

    fun parameter(symbol: KaTypeParameterSymbol, binder: String, index: Int): String = node("parameter") {
        put("name", symbol.name.asString()); put("binder", binder); put("parameter_index", index)
    }

    fun classifier(session: org.jetbrains.kotlin.analysis.api.KaSession, symbol: KaClassifierSymbol,
                   declaration: (KaSymbol) -> String, parameter: (KaTypeParameterSymbol) -> Pair<String, Int>): String = with(session) {
        fun visit(type: KaType) = project(session, type, declaration, parameter)
        when (symbol) {
            is KaTypeParameterSymbol -> {
                val (binder, index) = parameter(symbol)
                node("parameter") {
                    put("name", symbol.name.asString()); put("binder", binder); put("parameter_index", index)
                    putJsonArray("constraints") { symbol.upperBounds.forEach { add(visit(it)) } }
                    putJsonArray("qualifiers") { add(symbol.variance.toString()); if (symbol.isReified) add("reified") }
                }
            }
            is KaClassLikeSymbol -> node(if (symbol is KaTypeAliasSymbol) "alias" else "named") {
                put("declaration", declaration(symbol))
                put("name", symbol.classId?.asFqNameString() ?: (symbol as? KaNamedClassSymbol)?.name?.asString().orEmpty())
                if (symbol is KaTypeAliasSymbol) putJsonArray("arguments") { add(visit(symbol.expandedType)) }
                if (symbol is KaClassSymbol) putJsonArray("constraints") { symbol.superTypes.forEach { add(visit(it)) } }
                putJsonArray("type_parameters") {
                    (symbol as? KaTypeParameterOwnerSymbol)?.typeParameters?.forEach {
                        add(classifier(session, it, declaration, parameter))
                    }
                }
            }
        }
    }

    fun callable(session: org.jetbrains.kotlin.analysis.api.KaSession, symbol: KaFunctionSymbol,
                 declaration: (KaSymbol) -> String, parameter: (KaTypeParameterSymbol) -> Pair<String, Int>): String = with(session) {
        fun visit(type: KaType) = project(session, type, declaration, parameter)
        node("function") {
            put("return_type", visit(symbol.returnType))
            symbol.receiverParameter?.let { put("receiver", visit(it.returnType)) }
            putJsonArray("parameter_groups") {
                if (symbol.contextParameters.isNotEmpty()) add(buildJsonObject { putJsonArray("parameters") {
                    symbol.contextParameters.forEach { add(buildJsonObject {
                        put("type", visit(it.returnType)); putJsonArray("modifiers") { add("context") }
                    }) }
                } })
                add(buildJsonObject { putJsonArray("parameters") {
                    symbol.valueParameters.forEach { value -> add(buildJsonObject {
                        put("type", visit(value.returnType)); putJsonArray("modifiers") {
                            if (value.isVararg) add("vararg")
                            if (value.isNoinline) add("noinline")
                            if (value.isCrossinline) add("crossinline")
                        }
                    }) }
                } })
            }
            putJsonArray("type_parameters") {
                (symbol as? KaTypeParameterOwnerSymbol)?.typeParameters?.forEach {
                    add(classifier(session, it, declaration, parameter))
                }
            }
            putJsonArray("qualifiers") { if (symbol is KaNamedFunctionSymbol && symbol.isSuspend) add("suspend") }
        }
    }

    @OptIn(org.jetbrains.kotlin.analysis.api.KaNonPublicApi::class)
    fun project(session: org.jetbrains.kotlin.analysis.api.KaSession, type: KaType, declaration: (KaSymbol) -> String,
                parameter: (KaTypeParameterSymbol) -> Pair<String, Int>): String {
        fun visit(value: KaType): String = project(session, value, declaration, parameter)
        val core = when (type) {
            is KaFlexibleType -> node("flexible") {
                put("lower_bound", visit(type.lowerBound)); put("upper_bound", visit(type.upperBound))
            }
            is KaFunctionType -> node("function") {
                type.receiverType?.let { put("receiver", visit(it)) }
                put("return_type", visit(type.returnType))
                putJsonArray("parameter_groups") {
                    add(buildJsonObject { putJsonArray("parameters") {
                        type.parameterTypes.forEach { value -> add(buildJsonObject {
                            put("type", visit(value)); put("modifiers", JsonArray(emptyList()))
                        }) }
                    } })
                }
                putJsonArray("qualifiers") { if (type.isSuspend) add("suspend") }
            }
            is KaClassType -> node(if (type.typeArguments.isEmpty()) "named" else "application") {
                put("name", type.classId.asFqNameString()); put("declaration", declaration(type.symbol))
                putJsonArray("arguments") {
                    type.typeArguments.forEach { projection ->
                        if (projection is KaTypeArgumentWithVariance) add(visit(projection.type))
                    }
                }
                putJsonObject("extensions") { putJsonArray("kotlin.projections") {
                    type.typeArguments.forEach { projection -> add(buildJsonObject {
                        if (projection is KaTypeArgumentWithVariance) {
                            put("variance", projection.variance.toString()); put("type", visit(projection.type))
                        } else put("variance", "star")
                    }) }
                } }
            }
            is KaTypeParameterType -> node("parameter") {
                val (binder, index) = parameter(type.symbol)
                put("name", type.name.asString()); put("binder", binder); put("parameter_index", index)
            }
            is KaDefinitelyNotNullType -> node("intersection") {
                putJsonArray("elements") { add(visit(type.original)); add(visit(with(session) { builtinTypes.any })) }
                putJsonArray("qualifiers") { add("definitely_non_null") }
            }
            is KaIntersectionType -> node("intersection") {
                putJsonArray("elements") { type.conjuncts.forEach { add(visit(it)) } }
            }
            is KaErrorType -> node("error") { put("name", type.errorMessage) }
            else -> node("unresolved") { put("name", type.javaClass.simpleName) }
        }
        val nullable = if (type !is KaFlexibleType && with(session) { type.isMarkedNullable })
            node("nullable") { putJsonArray("arguments") { add(core) } } else core
        val alias = type.abbreviation
        return if (alias != null) node("alias") {
            put("name", alias.classId.asFqNameString()); put("declaration", declaration(alias.symbol))
            putJsonArray("arguments") { add(nullable) }
        } else nullable
    }
}
