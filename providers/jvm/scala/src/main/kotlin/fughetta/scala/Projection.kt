package fughetta.scala

import java.nio.file.Path
import java.util.IdentityHashMap
import kotlinx.serialization.json.*
import scala.reflect.internal.Symbols.Symbol
import scala.reflect.internal.Trees.*
import scala.reflect.internal.Trees.Function as Lambda
import scala.reflect.internal.Types.Type
import scala.reflect.internal.util.Position

internal class Projection(
    private val root: Path,
    private val project: String,
    private val provider: String,
    private val module: ModuleInputs,
    compilation: Compilation,
    producerPaths: Map<String, String>,
) {
    private val unit = "\u0000unit"
    private val context = "\u0000context"
    private val identities = Identities(project, provider, module, root, compilation.sources, producerPaths)
    private val types = Types(identities)
    private val symbols = linkedMapOf<String, JsonObject>()
    private val occurrences = mutableListOf<JsonObject>()
    private val references = mutableListOf<JsonObject>()
    private val relations = mutableListOf<JsonObject>()
    private val scopes = mutableListOf<JsonObject>()
    private val diagnostics = mutableListOf<JsonObject>()
    private val typedRanges = mutableListOf<JsonObject>()
    private val files = compilation.sources.values.filter { module.projectedSources?.contains(it.file) != false }
        .map { root.relativize(it.file).wire() }

    init {
        val definitions = compilation.run.units().items().mapNotNull { unit ->
            compilation.sources[unit.source().file().path()]?.takeIf { module.projectedSources?.contains(it.file) != false }
                ?.let { FileProjection(it) to unit.body() }
        }.toList()
        definitions.forEach { (file, body) -> file.register(body, null) }
        definitions.forEach { (file, body) -> file.project(body) }
    }

    fun diagnostics(messages: List<Message>) {
        messages.forEach { message ->
            val position = message.position
            val file = identities.source(position)
            if (file != null && module.projectedSources?.contains(file.file) == false) return@forEach
            val place = if (file != null && position != null) FileProjection(file).place(position) else null
            diagnostics.add(buildJsonObject {
                put("id", "$unit:diagnostic:${diagnostics.size}")
                put("producer", provider); put("code", message.code)
                put("severity", message.severity); put("message", message.text)
                put("location", location(place)); put("ownership", ownership(place, file))
                putJsonArray("related_locations") {}; putJsonObject("fixes") {}
            })
        }
    }

    fun shard(template: JsonObject): JsonObject {
        val owner = template.text("id")
        val contextId = template.text("context")
        fun bind(value: JsonElement): JsonElement = when (value) {
            is JsonObject -> JsonObject(value.mapValues { bind(it.value) })
            is JsonArray -> JsonArray(value.map(::bind))
            is JsonPrimitive -> when {
                !value.isString -> value
                value.content == context -> JsonPrimitive(contextId)
                value.content == unit || value.content.startsWith("$unit:") ->
                    JsonPrimitive(owner + value.content.removePrefix(unit))
                else -> value
            }
        }
        val rows = linkedMapOf(
            "symbols" to symbols.values.toList(), "types" to types.records.values.toList(),
            "occurrences" to occurrences, "references" to references.map { reference ->
                if (reference["target"]?.jsonPrimitive?.content in symbols)
                    JsonObject(reference + ("target_context" to JsonPrimitive(context)))
                else reference
            },
            "scopes" to scopes, "relations" to relations, "diagnostics" to diagnostics,
        )
        val facts = (rows.keys + listOf("definitions", "typed_ranges")).toSet()
        val requested = template.getValue("coverage").jsonObject.getValue("requested").jsonObject
        val unsupported = requested.rows("facts").filter { it.jsonPrimitive.content !in facts }
        return buildJsonObject {
            put("id", "")
            putJsonObject("unit") {
                template.forEach { (key, value) -> put(key, value) }
                put("modules", listOf(module.id).strings())
                putJsonObject("contributions") {
                    put("files", files.strings())
                    rows.forEach { (name, values) -> putJsonArray(name) { values.forEach { add(bind(it.getValue("id"))) } } }
                }
                putJsonObject("coverage") {
                    put("requested", requested)
                    putJsonArray("available") { add(buildJsonObject {
                        put("files", files.strings()); put("modules", listOf(module.id).strings())
                        put("contexts", listOf(contextId).strings()); put("projects", listOf(project).strings())
                        put("facts", facts.strings())
                    }) }
                    putJsonArray("missing") {
                        if (unsupported.isNotEmpty()) add(buildJsonObject {
                            put("scope", JsonObject(requested + ("facts" to JsonArray(unsupported))))
                            put("units", listOf(owner).strings()); put("phase", "analysis")
                            put("reason", "requested facts are not exported"); put("deferred", false)
                        })
                    }
                    put("unresolved_count", references.count { it.text("resolution") != "resolved" })
                }
            }
            put("identities", JsonArray(identities.bindings.values.toList()))
            rows.forEach { (name, values) -> put(name, bind(JsonArray(values))) }
            put("typed_ranges", bind(JsonArray(typedRanges)))
        }
    }

    private fun location(place: JsonObject?) = buildJsonObject { place?.let { put("source", it) }; put("project", project) }

    private fun ownership(place: JsonObject?, source: Source?) = buildJsonObject {
        put("owner_unit", unit); put("context", context)
        putJsonObject("origin") {
            put("provider", provider)
            put("kind", when { source == null -> "unknown"; source.generated -> "generated"; else -> "source" })
            put("location", location(place))
        }
    }

    private data class Declaration(val id: String, val name: String, val kind: String, val symbol: Symbol?, val implicit: Boolean)

    private class Scope(val id: String, val kind: String, val place: JsonObject, val parent: Scope?, val owner: String?) {
        val members = linkedSetOf<String>()
        val lookup = linkedSetOf<JsonObject>()

        fun lookup(kind: String, target: String, name: String? = null, wildcard: Boolean = false) {
            lookup.add(buildJsonObject {
                put("kind", kind); put("target", target); put("wildcard", wildcard)
                name?.let { put("name", it) }
            })
        }
    }

    private inner class FileProjection(private val source: Source) {
        private val path = root.relativize(source.file).wire()
        private val declarations = IdentityHashMap<Tree, Declaration>()
        private val ordinals = mutableMapOf<Triple<String?, String, String>, Int>()
        private val scopeRows = mutableListOf<Scope>()
        private val seenUses = mutableSetOf<Triple<JsonObject, String?, List<String>>>()
        private val seenTypes = mutableSetOf<Triple<JsonObject, String, String?>>()

        private fun range(begin: Int, end: Int) = buildJsonObject {
            put("path", path); put("begin", source.scalar(begin)); put("end", source.scalar(end))
        }

        fun place(position: Position): JsonObject = range(source.index(position.start()), source.index(position.end()))
        private fun real(position: Position): Boolean = identities.source(position) === source
        private fun place(tree: Tree): JsonObject? = tree.pos().takeIf(::real)?.let(::place)
        private fun owns(place: JsonObject) = ownership(place, source)

        private fun selection(position: Position, name: String): JsonObject? {
            if (!real(position)) return null
            val point = source.index(position.point())
            if (point == source.text.length) return null
            val token = if (source.text[point] == '`') "`$name`" else name
            if (token.isEmpty() || !source.text.startsWith(token, point)) return null
            val end = point + token.length
            if (end < source.text.length && Character.isUnicodeIdentifierPart(token.last()) &&
                Character.isUnicodeIdentifierPart(source.text.codePointAt(end))) return null
            return range(point, end)
        }

        private fun declarationSymbol(tree: Tree): Symbol? {
            val symbol = tree.symbol()?.takeIf { it.usable() && !it.isEffectiveRoot() && !it.isRootSymbol() } ?: return null
            if (symbol.isArtifact() || symbol.isDefaultGetter() || (symbol.isSynthetic() && !symbol.isPrimaryConstructor())) return null
            if (tree is DefDef && symbol.isAccessor() && tree.rhs().let { !it.isEmpty() }) return null
            return identities.canonical(symbol)
        }

        fun register(tree: Tree, owner: String?) {
            val defining = tree is PackageDef || tree is ClassDef || tree is ModuleDef || tree is DefDef ||
                tree is ValDef || tree is TypeDef || tree is Bind || tree is Lambda
            var parent = owner
            if (defining && real(tree.pos())) {
                val symbol = if (tree is Lambda) tree.symbol()?.takeIf { it.usable() } else declarationSymbol(tree)
                if (symbol != null || tree is Lambda) {
                    val kind = if (tree is Lambda) "function" else requireNotNull(symbol).kind()
                    val name = if (tree is Lambda) "<lambda>" else requireNotNull(symbol).label()
                    val implicit = tree is DefDef && symbol?.isPrimaryConstructor() == true && selection(tree.pos(), name) == null
                    val id = if (tree is Lambda || identities.local(requireNotNull(symbol))) {
                        val ordinal = ordinals.merge(Triple(owner, kind, name), 1, Int::plus)
                        identities.anchor(symbol, source, "$kind:$name:$ordinal", owner)
                    } else identities.id(requireNotNull(symbol))
                    declarations[tree] = Declaration(id, name, kind, symbol, implicit)
                    parent = id
                }
            }
            tree.nodes().forEach { register(it, parent) }
        }

        fun project(tree: Tree) {
            val file = scope("translation_unit", range(0, source.text.length), null, null)
            visit(tree, file, null)
            scopeRows.forEach { value -> scopes.add(buildJsonObject {
                put("id", value.id); put("kind", value.kind)
                put("location", location(value.place)); put("ownership", owns(value.place))
                value.parent?.let { put("lexical_parent", it.id) }; value.owner?.let { put("semantic_owner", it) }
                put("declared_members", value.members.strings()); put("lookup", JsonArray(value.lookup.toList()))
            }) }
        }

        private fun scope(kind: String, place: JsonObject, parent: Scope?, owner: String?): Scope =
            Scope("$unit:scope:$path:${scopeRows.size}", kind, place, parent, owner).also { scopeRows.add(it) }

        private fun visit(tree: Tree, parentScope: Scope, owner: String?, role: String? = null, callee: Boolean = false) {
            val declaration = declarations[tree]
            val token = place(tree)
            val scopeKind = when (tree) {
                is PackageDef -> "namespace".takeIf { declaration != null }
                is ClassDef, is ModuleDef -> "type".takeIf { declaration != null }
                is DefDef -> "function".takeIf { declaration != null }
                is Lambda -> "lambda"
                is Block -> "block".takeIf { tree.pos().isRange() }
                else -> null
            }
            val scope = if (scopeKind != null && token != null) scope(scopeKind, token, parentScope, declaration?.id ?: owner)
                else parentScope
            if (declaration != null && token != null) declare(tree, declaration, token, parentScope, scope, owner)
            val from = declaration?.id ?: owner
            when (tree) {
                is Apply -> {
                    if (!callee) call(tree, from)
                    visit(tree.`fun`(), scope, from, callee = true)
                    val implicit = tree is ApplyToImplicitArgs || tree is ApplyImplicitView
                    tree.args().items().forEach { visit(it, scope, from, if (implicit) "implicit" else null) }
                }
                is TypeApply -> {
                    if (!callee) call(tree, from)
                    visit(tree.`fun`(), scope, from, callee = true)
                    tree.args().items().forEach { visit(it, scope, from, "type") }
                }
                is Assign -> {
                    visit(tree.lhs(), scope, from, "write")
                    visit(tree.rhs(), scope, from)
                }
                is Import -> {
                    imports(tree, scope, from)
                    visit(tree.expr(), scope, from, "import")
                }
                is TypeTree -> {
                    val original = tree.original()
                    if (original != null && !original.isEmpty()) visit(original, scope, from, "type")
                    else if (!tree.wasEmpty() && token != null) reference(tree.tpe()?.typeSymbol(), token, from, listOf("type"))
                }
                is PackageDef -> tree.stats().items().forEach { visit(it, scope, from) }
                else -> {
                    if (token != null && !callee && (tree is Ident || tree is Select)) {
                        val symbol = tree.symbol()
                        val name = (tree as NameTree).name().decoded().trim()
                        val position = selection(tree.pos(), name)
                        if (position != null || role == "implicit" || role == "type") {
                            val use = position ?: token
                            val kind = role ?: if (tree.isType()) "type" else "read"
                            if (role == null && symbol.usable() && symbol.isMethod() && !symbol.isAccessor() &&
                                symbol.info() is scala.reflect.internal.Types.NullaryMethodType) call(tree, from)
                            else reference(symbol, use, from, listOf(kind))
                        }
                    }
                    tree.nodes().forEach { visit(it, scope, from, role?.takeIf { it == "type" || it == "import" }) }
                }
            }
        }

        private fun declare(tree: Tree, declaration: Declaration, place: JsonObject, parent: Scope, scope: Scope, owner: String?) {
            val (id, name, kind, symbol, implicit) = declaration
            val selected = selection(tree.pos(), name)
            val body = when (tree) { is DefDef -> tree.rhs(); is Lambda -> tree.body(); else -> null }
                ?.takeUnless { it.isEmpty() || implicit }?.let(::place)
            val type = when {
                tree is Lambda -> types.type(tree.tpe())
                symbol == null || symbol.packageSymbol() -> null
                symbol.isClass() || symbol.isType() -> types.classifier(symbol)
                symbol.isMethod() && !symbol.isAccessor() -> types.callable(symbol)
                else -> types.type((if (tree is ValDef) tree.symbol() else symbol).info().finalResultType())
            }
            val native = symbol?.annotations()?.items()?.any { it.atp().typeSymbol().fullName() == "scala.native" } == true
            val member = parent
            member.members.add(id)
            symbols.putIfAbsent(id, buildJsonObject {
                put("id", id); put("ownership", owns(place)); put("language", "scala"); put("domain", "scala")
                put("module", module.id); put("name", name); put("qualified_name", symbol?.fullName() ?: name)
                put("kind", kind); put("visibility", visibility(symbol))
                put("declaring_scope", member.id)
                (if (symbol == null || identities.local(symbol)) owner
                    else identities.owner(symbol)?.let(identities::id) ?: owner)?.let { put("owner", it) }
                type?.let { put("type", it) }
                put("modifiers", modifiers(tree, symbol, implicit, native).strings())
                putJsonArray("parameters") {
                    if (tree is DefDef) tree.vparamss().items().forEachIndexed { group, parameters ->
                        parameters.items().forEachIndexed { index, parameter -> add(buildJsonObject {
                            put("group", group); put("index", index); put("name", parameter.symbol().label())
                            source.snippet(parameter.rhs().pos())?.let { put("default_value", it) }
                            putJsonObject("annotations") { put("scala.has_default", parameter.symbol().hasDefault()) }
                        }) }
                    }
                }
                put("native_key", identities.declaration(id))
                putJsonObject("extensions") {
                    if (symbol != null && symbol.isMethod() && !symbol.isAccessor()) {
                        put("jvm.class", symbol.owner().jvmClass())
                        put("jvm.descriptor", "(${parameters(symbol)})" +
                            if (symbol.isConstructor()) "V" else symbol.info().finalResultType().descriptor(result = true))
                    }
                }
            })
            val occurrence = "$unit:occurrence:$path:${occurrences.size}"
            occurrences.add(buildJsonObject {
                put("id", occurrence); put("ownership", owns(place)); put("symbol", id)
                put("location", location(place)); put("declaration_kind", kind)
                putJsonArray("roles") {
                    add("declaration")
                    if (tree !is DefDef || !tree.rhs().isEmpty() || implicit) add("definition")
                    if (implicit) add("implicit")
                }
                selected?.let { put("selection", it) }
                body?.let { put("body", it) }
            })
            if (symbol != null) {
                val template = when (tree) { is ClassDef -> tree.impl(); is ModuleDef -> tree.impl(); else -> null }
                template?.parents()?.items()?.forEach { base ->
                    val target = base.tpe()?.typeSymbol()
                    if (target.usable() && target != symbol) {
                        val baseId = identities.id(requireNotNull(target))
                        val relation = if (target.isTraitOrInterface() && !symbol.isTrait()) "implements" else "inherits"
                        relation(relation, id, baseId, place, occurrence = occurrence)
                        scope.lookup("base", baseId)
                    }
                }
                if (symbol.isMethod() && !symbol.isConstructor()) symbol.allOverriddenSymbols().items().forEach {
                    relation("overrides", id, identities.id(it), place, occurrence = occurrence)
                }
                if (tree is ValDef && selected != null && (identities.local(symbol) || symbol.isValueParameter()) &&
                    (tree.tpt() as? TypeTree)?.wasEmpty() == true) typed(selected, tree.symbol().info())
            }
        }

        private fun visibility(symbol: Symbol?) = when {
            symbol == null || symbol.isPrivate() -> "private"
            symbol.isProtected() -> "protected"
            symbol.privateWithin().usable() -> if (symbol.privateWithin().packageSymbol()) "package" else "private"
            else -> "public"
        }

        private fun modifiers(tree: Tree, symbol: Symbol?, implicit: Boolean, native: Boolean) = buildList {
            if (implicit || symbol?.isImplicit() == true) add("implicit")
            if (symbol?.isFinal() == true) add("final")
            if (symbol?.isSealed() == true) add("sealed")
            if (symbol?.isAbstractClass() == true || symbol?.isDeferred() == true) add("abstract")
            if (symbol?.isCase() == true) add("case")
            if (symbol?.isLazy() == true) add("lazy")
            if (symbol?.isAnyOverride() == true) add("override")
            if (native) add("native")
            if (tree is Lambda || (symbol != null && identities.local(symbol))) add("local")
            if (tree is ValDef && !tree.symbol().isValueParameter()) add(if (tree.symbol().isMutable()) "var" else "val")
        }

        private fun imports(tree: Import, scope: Scope, owner: String?) {
            val expression = tree.expr()
            val qualifier = expression.symbol()?.takeIf { it.usable() } ?: expression.tpe()?.typeSymbol()
            if (!qualifier.usable()) return
            tree.selectors().items().forEach { selector ->
                val name = selector.name().decoded()
                if (name == "_") scope.lookup("import", identities.id(requireNotNull(qualifier)), wildcard = true)
                else {
                    val rename = selector.rename()?.decoded()
                    if (rename == "_") return@forEach
                    val candidates = listOf(
                        expression.tpe().member(selector.name().toTermName()),
                        expression.tpe().member(selector.name().toTypeName()),
                    ).filter { it.usable() }.flatMap { if (it.isOverloaded()) it.alternatives().items().toList() else listOf(it) }.distinct()
                    val at = source.index(selector.namePos())
                    val token = range(at, (at + name.length).coerceAtMost(source.text.length))
                    candidates.forEach {
                        scope.lookup("import", identities.id(it), rename ?: name)
                        reference(it, token, owner, listOf("import"))
                    }
                }
            }
        }

        private fun call(tree: Tree, from: String?) {
            val token = place(tree) ?: return
            var leaf = tree
            val typeArguments = mutableListOf<Type>()
            var implicit = false
            while (leaf is Apply || leaf is TypeApply) {
                leaf = when (val applied = leaf) {
                    is Apply -> {
                        implicit = implicit || applied is ApplyToImplicitArgs || applied is ApplyImplicitView
                        applied.`fun`()
                    }
                    is TypeApply -> {
                        typeArguments.addAll(applied.args().items().map { it.tpe() })
                        applied.`fun`()
                    }
                    else -> error("expected application")
                }
            }
            val symbol = leaf.symbol()
            val name = (leaf as? NameTree)?.name()?.decoded()?.trim()
            val selected = name?.let { selection(leaf.pos(), it) } ?: token
            val dispatch = when {
                !symbol.usable() -> null
                symbol.isConstructor() || symbol.isPrivate() || symbol.isFinal() || symbol.isStaticMember() -> "direct"
                symbol.owner().isTraitOrInterface() -> "interface"
                symbol.name().toString() == "apply" && symbol.owner().fullName().startsWith("scala.Function") -> "indirect"
                !symbol.owner().isModuleOrModuleClass() && symbol.owner().isClass() && !symbol.owner().isFinal() -> "virtual"
                else -> "direct"
            }
            val roles = buildList {
                add("call")
                if (implicit) add("implicit")
                if (symbol.usable() && symbol.isSetter()) add("write")
            }
            val reference = reference(symbol, selected, from, roles, dispatch) ?: return
            if (!symbol.usable()) return
            val target = identities.id(requireNotNull(symbol))
            if (from != null) {
                relation("calls", from, target, selected, reference = reference)
                if (name == "asInstanceOf" && selection(leaf.pos(), name) != null) {
                    val cast = typeArguments.firstOrNull()?.typeSymbol()
                    if (cast.usable()) relation("unsafe_cast", from, identities.id(requireNotNull(cast)), selected, reference = reference)
                }
            }
            val overloaded = symbol.owner().let { it.usable() && it.info().member(symbol.name()).isOverloaded() }
            if (typeArguments.isNotEmpty() || overloaded || implicit || dispatch in setOf("virtual", "interface", "indirect")) {
                val substitutions = symbol.typeParams().items().zip(typeArguments.asSequence()).toList()
                typed(token, tree.tpe(), target, substitutions)
            }
        }

        private fun reference(symbol: Symbol?, place: JsonObject, from: String?, roles: List<String>, dispatch: String? = null): String? {
            val target = symbol?.takeIf { it.usable() }?.let(identities::id)
            if (!seenUses.add(Triple(place, target, roles))) return null
            val id = "$unit:reference:$path:${references.size}"
            references.add(buildJsonObject {
                put("id", id); put("ownership", owns(place)); put("location", location(place))
                from?.let { put("from", it) }; target?.let { put("target", it) }
                putJsonArray("candidates") {}; put("roles", (listOf("reference") + roles).strings())
                put("resolution", if (target == null) "unresolved" else "resolved")
                dispatch?.let { put("dispatch", it) }
            })
            return id
        }

        private fun relation(kind: String, from: String, to: String, place: JsonObject, reference: String? = null, occurrence: String? = null) {
            relations.add(buildJsonObject {
                put("id", "$unit:relation:$path:${relations.size}"); put("kind", kind); put("ownership", owns(place))
                put("from", from); put("to", to); put("from_context", context); put("resolution", "resolved")
                reference?.let { put("reference", it) }; occurrence?.let { put("occurrence", it) }
            })
        }

        private fun typed(place: JsonObject, value: Type?, overload: String? = null, substitutions: List<Pair<Symbol, Type>> = emptyList()) {
            if (!value.known()) return
            val type = types.type(value)
            if (!seenTypes.add(Triple(place, type, overload))) return
            typedRanges.add(buildJsonObject {
                put("ownership", owns(place)); put("range", place); put("type", type)
                overload?.let { put("overload", it) }
                putJsonObject("substitutions") {
                    substitutions.forEach { (parameter, argument) -> put(types.parameter(parameter), types.type(argument)) }
                }
            })
        }
    }
}
