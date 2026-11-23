package fughetta.scala

import java.util.IdentityHashMap
import kotlinx.serialization.json.*
import scala.reflect.internal.Symbols.Symbol
import scala.reflect.internal.Types.*

internal class Types(private val identities: Identities? = null) {
    val records = linkedMapOf<String, JsonObject>()
    private val ids = IdentityHashMap<Type, String>()
    private val active = java.util.Collections.newSetFromMap(IdentityHashMap<Type, Boolean>())

    fun node(kind: String, fields: JsonObjectBuilder.() -> Unit = {}): String {
        val payload = buildJsonObject {
            put("domain", "scala"); put("kind", kind)
            listOf("arguments", "constraints", "elements", "type_parameters", "parameter_groups",
                "qualifiers", "application_arguments").forEach { put(it, JsonArray(emptyList())) }
            putJsonObject("extensions") {}
            fields()
        }
        val id = "type-${digest(payload.toString())}"
        records.putIfAbsent(id, buildJsonObject { put("id", id); payload.forEach { (key, value) -> put(key, value) } })
        return id
    }

    fun parameter(symbol: Symbol, bounds: Boolean = false): String = node("parameter") {
        put("name", symbol.label())
        identities?.parameter(symbol)?.let { (binder, index) -> put("binder", binder); put("parameter_index", index) }
        if (bounds) {
            (symbol.info() as? TypeBounds)?.let {
                put("lower_bound", type(it.lo())); put("upper_bound", type(it.hi()))
                put("constraints", listOf(type(it.hi())).strings())
            }
            put("qualifiers", listOfNotNull(
                "covariant".takeIf { symbol.isCovariant() }, "contravariant".takeIf { symbol.isContravariant() },
            ).strings())
        }
    }

    fun classifier(symbol: Symbol): String {
        val identity = requireNotNull(identities)
        if (symbol.isTypeParameterOrSkolem()) return parameter(symbol, bounds = true)
        if (symbol.isAliasType()) return node("alias") {
            put("name", symbol.fullName()); put("declaration", identity.id(symbol))
            put("arguments", listOf(type(symbol.info().finalResultType())).strings())
            put("type_parameters", symbol.typeParams().items().map { parameter(it, bounds = true) }.toList().strings())
        }
        if (symbol.isAbstractType()) return node("parameter") {
            put("name", symbol.fullName()); put("declaration", identity.id(symbol))
            (symbol.info() as? TypeBounds)?.let {
                put("lower_bound", type(it.lo())); put("upper_bound", type(it.hi()))
            }
        }
        return node("named") {
            put("name", symbol.fullName()); put("declaration", identity.id(symbol))
            put("type_parameters", symbol.typeParams().items().map { parameter(it, bounds = true) }.toList().strings())
            put("constraints", symbol.info().parents().items().map(::type).toList().strings())
        }
    }

    fun callable(symbol: Symbol): String = signature(symbol.info(), symbol.owner().takeIf {
        it.usable() && it.isClass() && !it.packageSymbol()
    }?.thisType())

    private fun signature(value: Type, receiver: Type? = null): String {
        val parameters = value.typeParams().items().toList()
        val groups = value.paramss().items().map { it.items().toList() }.toList()
        return node(if (groups.size > 1) "scala:curried_signature" else "function") {
            put("return_type", type(value.finalResultType()))
            receiver?.let { put("receiver", type(it)) }
            put("type_parameters", parameters.map { parameter(it, bounds = true) }.strings())
            putJsonArray("parameter_groups") {
                groups.forEach { group -> add(buildJsonObject {
                    putJsonArray("parameters") { group.forEach { symbol -> add(buildJsonObject {
                        put("type", type(symbol.info()))
                        put("modifiers", listOfNotNull("implicit".takeIf { symbol.isImplicit() }).strings())
                    }) } }
                }) }
            }
        }
    }

    fun type(value: Type?): String {
        if (value == null || !value.known()) return node("unresolved")
        ids[value]?.let { return it }
        val symbol = value.typeSymbol()
        if (!active.add(value)) return node("named") {
            if (symbol.usable()) { put("name", symbol.fullName()); identities?.let { put("declaration", it.id(symbol)) } }
        }
        val id = try {
            when (value) {
                is MethodType, is NullaryMethodType, is PolyType -> signature(value)
                is TypeBounds -> node("parameter") { put("lower_bound", type(value.lo())); put("upper_bound", type(value.hi())) }
                is ExistentialType -> type(value.underlying())
                is AnnotatedType -> type(value.underlying())
                is ConstantType -> type(value.widen())
                is SingleType -> named(value.sym(), emptyList(), listOf("singleton"))
                is ThisType -> named(value.sym(), emptyList(), listOf("this"))
                is RefinedType -> node("named") {
                    put("constraints", value.parents().items().map(::type).toList().strings())
                    put("qualifiers", listOf("refined").strings())
                }
                is TypeRef -> named(value.sym(), value.args().items().toList(), emptyList(), value)
                else -> if (symbol.usable()) named(symbol, value.typeArgs().items().toList(), emptyList())
                    else node("unresolved") { put("name", value.toString()) }
            }
        } finally {
            active.remove(value)
        }
        ids[value] = id
        return id
    }

    private fun named(symbol: Symbol, args: List<Type>, qualifiers: List<String>, original: TypeRef? = null): String {
        if (!symbol.usable()) return node("unresolved")
        if (symbol.isTypeParameterOrSkolem()) return parameter(symbol)
        val name = symbol.fullName()
        val kind = when {
            name in primitiveDescriptors -> "primitive"
            name == "scala.<byname>" -> "scala:by_name"
            name.substringAfterLast('.').matches(Regex("Tuple[0-9]+")) && name.startsWith("scala.Tuple") -> "tuple"
            name.substringAfterLast('.').matches(Regex("Function[0-9]+")) && name.startsWith("scala.Function") && args.isNotEmpty() -> "function"
            symbol.isAliasType() -> "alias"
            args.isNotEmpty() -> "application"
            else -> "named"
        }
        return node(kind) {
            put("name", name)
            identities?.let { put("declaration", it.id(symbol)) }
            put("qualifiers", qualifiers.strings())
            if (args.isNotEmpty()) put("arguments", args.map(::type).strings())
            when (kind) {
                "alias" -> original?.dealias()?.takeUnless { it == original }?.let { put("constraints", listOf(type(it)).strings()) }
                "tuple" -> put("elements", args.map(::type).strings())
                "function" -> {
                    put("return_type", type(args.last()))
                    putJsonArray("parameter_groups") { add(buildJsonObject { putJsonArray("parameters") {
                        args.dropLast(1).forEach { arg -> add(buildJsonObject {
                            put("type", type(arg)); putJsonArray("modifiers") {}
                        }) }
                    } }) }
                }
            }
        }
    }
}
