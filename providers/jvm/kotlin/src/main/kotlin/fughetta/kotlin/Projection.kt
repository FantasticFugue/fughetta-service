package fughetta.kotlin

import com.intellij.psi.PsiElement
import com.intellij.psi.PsiErrorElement
import com.intellij.openapi.util.TextRange
import org.jetbrains.kotlin.analysis.api.components.KaDiagnosticCheckerFilter
import com.intellij.psi.util.PsiTreeUtil
import kotlinx.serialization.json.*
import org.jetbrains.kotlin.analysis.api.KaConstantInitializerValue
import org.jetbrains.kotlin.analysis.api.KaSession
import org.jetbrains.kotlin.analysis.api.resolution.KaSuccessCallInfo
import org.jetbrains.kotlin.analysis.api.resolution.KaCompoundAccessCall
import org.jetbrains.kotlin.analysis.api.resolution.KaCompoundArrayAccessCall
import org.jetbrains.kotlin.analysis.api.resolution.KaFunctionCall
import org.jetbrains.kotlin.analysis.api.resolution.KaImplicitInvokeCall
import org.jetbrains.kotlin.analysis.api.resolution.KaSingleCall
import org.jetbrains.kotlin.analysis.api.symbols.*
import org.jetbrains.kotlin.analysis.api.types.KaClassType
import org.jetbrains.kotlin.analysis.api.symbols.markers.KaTypeParameterOwnerSymbol
import org.jetbrains.kotlin.idea.references.mainReference
import org.jetbrains.kotlin.idea.references.readWriteAccessWithFullExpression
import org.jetbrains.kotlin.util.OperatorNameConventions
import org.jetbrains.kotlin.resolve.references.ReferenceAccess
import org.jetbrains.kotlin.psi.*
import org.jetbrains.kotlin.lexer.KtTokens
import java.nio.file.Path

/** A unit's value records; no PSI or Analysis API object is retained here. */
class Projection(private val root: Path, private val project: String, private val unit: String,
                 private val context: String, generatedSources: Set<Path> = emptySet(), private val provider: String = "kotlin",
                 private val dependencies: Set<String> = emptySet()) {
    private val generated = generatedSources.map { it.toAbsolutePath().normalize() }.toSet()
    private val observationNames = sortedSetOf<String>()
    private val observationShapes = sortedSetOf<String>()
    private val observationSealedModules = sortedSetOf<String>()

    /** Declarations and classifier shapes this unit's lookups could resolve in its dependencies. */
    fun observationScope(): ObservationScope = ObservationScope(
        selectors = (observationNames.map { "name:$it" } + observationShapes.map { "type:$it" }).sorted(),
        sealedModules = observationSealedModules.toSet())
    val identities = Identities(project, provider)
    val types = Types()
    val symbols = linkedMapOf<String, JsonObject>()
    val occurrences = mutableListOf<JsonObject>()
    val references = mutableListOf<JsonObject>()
    val diagnostics = mutableListOf<JsonObject>()
    val scopes = mutableListOf<JsonObject>()
    val relations = mutableListOf<JsonObject>()
    val typedRanges = mutableListOf<JsonObject>()

    fun shard(template: JsonObject): JsonObject {
        require(template.getValue("id").jsonPrimitive.content == unit &&
            template.getValue("context").jsonPrimitive.content == context) { "unit template differs from projected records" }
        val boundReferences = references.map { reference ->
            val target = reference["target"]?.jsonPrimitive?.content
            if (target != null && symbols.containsKey(target))
                JsonObject(reference.toMutableMap().apply { put("target_context", JsonPrimitive(context)) })
            else reference
        }
        val rows = linkedMapOf(
            "symbols" to symbols.values.toList(), "types" to types.records.values.toList(),
            "occurrences" to occurrences, "references" to boundReferences, "scopes" to scopes,
            "relations" to relations, "diagnostics" to diagnostics)
        val contribution = buildJsonObject {
            putJsonArray("files") { sourceFiles.forEach { add(it) } }
            rows.forEach { (name, values) -> putJsonArray(name) { values.forEach { add(it.getValue("id")) } } }
        }
        val facts = listOf("symbols", "types", "definitions", "occurrences", "references", "scopes", "relations", "diagnostics", "typed_ranges")
        val requested = template.getValue("coverage").jsonObject.getValue("requested").jsonObject
        val available = buildJsonObject {
            putJsonArray("files") { sourceFiles.forEach { add(it) } }
            putJsonArray("modules") { add(template.getValue("recipe").jsonObject.getValue("unit").jsonObject.getValue("name")) }
            putJsonArray("contexts") { add(context) }; putJsonArray("projects") { add(project) }
            putJsonArray("facts") { facts.forEach { add(it) } }
        }
        val coverage = buildJsonObject {
            put("requested", requested); putJsonArray("available") { add(available) }
            putJsonArray("missing") {
                val unsupported = requested.getValue("facts").jsonArray.filter { it.jsonPrimitive.content !in facts }
                if (unsupported.isNotEmpty()) add(buildJsonObject {
                    put("scope", JsonObject(requested.toMutableMap().apply { put("facts", JsonArray(unsupported)) }))
                    putJsonArray("units") { add(unit) }; put("reason", "requested facts are not exported")
                    put("phase", "analysis"); put("deferred", false)
                })
            }
            put("unresolved_count", references.count { it.getValue("resolution").jsonPrimitive.content != "resolved" })
        }
        return buildJsonObject {
            put("id", "")
            put("unit", JsonObject(template.toMutableMap().apply {
                put("contributions", contribution); put("coverage", coverage)
            }))
            put("identities", JsonArray(identities.bindings.values.toList()))
            rows.forEach { (name, values) -> put(name, JsonArray(values)) }
            put("typed_ranges", JsonArray(typedRanges))
        }
    }

    private val sourceFiles = linkedSetOf<String>()

    fun file(session: KaSession, file: KtFile, module: (KaSymbol) -> String,
             anchor: (KaSymbol) -> JsonObject) = with(session) {
        fun observe(symbol: KaSymbol) {
            val owner = module(symbol)
            if (owner !in dependencies) return
            declarationName(symbol)?.let(observationNames::add)
            if (symbol is KaClassSymbol) {
                val classId = symbol.classId?.asFqNameString() ?: return
                when {
                    // Sealed subclasses are separate declarations in the same module; a new one
                    // changes exhaustiveness without naming anything this unit references.
                    symbol.modality == KaSymbolModality.SEALED -> observationSealedModules.add(owner)
                    // Members of these classifiers can appear without a name this unit references.
                    symbol.classKind == KaClassKind.INTERFACE || symbol.classKind == KaClassKind.ANNOTATION_CLASS ||
                        symbol.classKind == KaClassKind.ENUM_CLASS || symbol.modality == KaSymbolModality.ABSTRACT ->
                        observationShapes.add(classId)
                }
            }
        }
        fun id(symbol: KaSymbol): String {
            observe(symbol)
            return identities.identify(session, symbol, module, anchor)
        }
        fun parameterIdentity(parameter: KaTypeParameterSymbol): Pair<String, Int> {
            val owner = requireNotNull(parameter.containingDeclaration)
            val index = (owner as KaTypeParameterOwnerSymbol).typeParameters.indexOf(parameter)
            require(index >= 0) { "type parameter is absent from its binder" }
            return id(owner) to index
        }
        val text = file.text
        val path = root.relativize(Path.of(file.virtualFile.path)).toString().replace('\\', '/')
        sourceFiles.add(path)
        val origin = if (Path.of(file.virtualFile.path).toAbsolutePath().normalize() in generated) "generated" else "source"
        // Build the UTF-16 to scalar map once, including supplementary characters.
        val offsets = IntArray(text.length + 1)
        var cursor = 0
        var scalar = 0
        while (cursor < text.length) {
            val width = Character.charCount(text.codePointAt(cursor))
            repeat(width) { offsets[cursor + it] = scalar }
            cursor += width; scalar++
        }
        offsets[text.length] = scalar
        fun range(value: TextRange) = buildJsonObject {
            put("path", path); put("begin", offsets[value.startOffset]); put("end", offsets[value.endOffset])
        }
        fun place(element: PsiElement) = range(element.textRange)
        fun location(element: PsiElement) = buildJsonObject { put("source", place(element)); put("project", project) }
        fun ownership(element: PsiElement) = buildJsonObject {
            put("owner_unit", unit); put("context", context)
            putJsonObject("origin") { put("provider", provider); put("kind", origin); put("location", location(element)) }
        }
        fun diagnostic(element: PsiElement, code: String, severity: String, message: String, ranges: List<TextRange>) {
            diagnostics.add(buildJsonObject {
                put("id", "$unit:diagnostic:$path:${element.textOffset}:$code:${diagnostics.size}")
                put("producer", provider); put("code", code); put("severity", severity); put("message", message)
                put("ownership", ownership(element))
                put("location", buildJsonObject {
                    put("source", range(ranges.firstOrNull() ?: element.textRange)); put("project", project)
                })
                putJsonArray("related_locations") { ranges.drop(1).forEach { value -> add(buildJsonObject {
                    put("source", range(value)); put("project", project)
                }) } }
                put("fixes", buildJsonObject {})
            })
        }
        PsiTreeUtil.collectElementsOfType(file, PsiErrorElement::class.java).forEach {
            diagnostic(it, "SYNTAX", "error", it.errorDescription, emptyList())
        }
        file.diagnostics(KaDiagnosticCheckerFilter.EXTENDED_AND_COMMON_CHECKERS).forEach {
            diagnostic(it.psi, it.factoryName, it.severity.name.lowercase(), it.defaultMessage, it.textRanges.toList())
        }
        fun scopeKind(element: PsiElement): String? = when (element) {
            is KtFile -> "translation_unit"
            is KtClassOrObject -> "type"
            is KtNamedFunction, is KtPropertyAccessor, is KtConstructor<*> -> "function"
            is KtFunctionLiteral -> "lambda"
            is KtLoopExpression -> "loop"
            is KtCatchClause -> "catch"
            is KtBlockExpression -> "block"
            else -> null
        }
        val scopeNodes = listOf<PsiElement>(file) + PsiTreeUtil.collectElementsOfType(file, KtElement::class.java)
            .filter { it !== file && scopeKind(it) != null }
        val scopeIds = scopeNodes.associateWith { "$unit:scope:$path:${it.textOffset}:${scopeKind(it)}" }
        val members = scopeNodes.associateWith { mutableListOf<String>() }
        val lookup = scopeNodes.associateWith { mutableListOf<JsonObject>() }
        fun enclosingScope(element: PsiElement): PsiElement? =
            generateSequence(element.parent) { it.parent }.firstOrNull { it in scopeIds }
        fun recordedDeclaration(element: PsiElement): Boolean =
            (element is KtNamedDeclaration || element is KtPropertyAccessor || element is KtConstructor<*> || element is KtFunctionLiteral) &&
                !(element is KtParameter && element.isFunctionTypeParameter)
        fun enclosingDeclaration(element: PsiElement): KtDeclaration? =
            generateSequence(element.parent) { it.parent }.filterIsInstance<KtDeclaration>()
                .firstOrNull(::recordedDeclaration)
        fun exportDeclaration(declaration: KtDeclaration, symbol: KaDeclarationSymbol, implicit: Boolean = false) {
            val key = id(symbol)
            if (key in symbols) return
            val name = when {
                symbol is KaConstructorSymbol -> (symbol.containingDeclaration as? KaNamedClassSymbol)?.name?.asString()
                symbol is KaPropertyAccessorSymbol -> "${if (symbol is KaPropertyGetterSymbol) "get" else "set"}-${(symbol.containingDeclaration as KaPropertySymbol).name.asString()}"
                symbol is KaPropertySymbol -> symbol.name.asString()
                symbol is KaNamedFunctionSymbol -> symbol.name.asString()
                else -> when (declaration) {
                is KtFunctionLiteral -> "<lambda>"
                is KtNamedFunction -> declaration.name ?: "<anonymous>"
                is KtNamedDeclaration -> declaration.name
                else -> null
                }
            } ?: return
            val kind = when (symbol) {
                is KaClassLikeSymbol -> "type"
                is KaValueParameterSymbol, is KaTypeParameterSymbol -> "parameter"
                is KaConstructorSymbol -> "constructor"
                is KaPropertyAccessorSymbol -> "method"
                is KaFunctionSymbol -> if (symbol.containingDeclaration is KaClassSymbol) "method" else "function"
                is KaPropertySymbol -> "property"
                else -> "variable"
            }
            val declaringScope = if (implicit && declaration in scopeIds) declaration else enclosingScope(declaration)
            declaringScope?.let { members.getValue(it).add(key) }
            symbols[key] = buildJsonObject {
                put("id", key); put("ownership", ownership(declaration))
                put("language", "kotlin"); put("domain", "kotlin"); put("module", module(symbol))
                put("name", name)
                put("qualified_name", when (symbol) {
                    is KaPropertyAccessorSymbol -> {
                        val property = symbol.containingDeclaration as KaPropertySymbol
                        property.callableId?.let { "$it.${if (symbol is KaPropertyGetterSymbol) "get" else "set"}" } ?: name
                    }
                    is KaCallableSymbol -> symbol.callableId?.toString() ?: name
                    is KaClassLikeSymbol -> symbol.classId?.asFqNameString() ?: name
                    else -> name
                })
                put("kind", kind); put("visibility", symbol.visibility.toString())
                declaringScope?.let { put("declaring_scope", scopeIds.getValue(it)) }
                symbol.containingDeclaration?.let { put("owner", id(it)) }
                if (symbol is KaClassifierSymbol)
                    put("type", types.classifier(session, symbol, ::id, ::parameterIdentity))
                if (symbol is KaCallableSymbol) {
                    put("type", if (symbol is KaFunctionSymbol) types.callable(session, symbol, ::id, ::parameterIdentity)
                        else types.project(session, symbol.returnType, ::id, ::parameterIdentity))
                }
                putJsonArray("modifiers") {
                    if (implicit) add("implicit")
                    if (symbol.location == KaSymbolLocation.LOCAL) add("local")
                    if (symbol is KaVariableSymbol) add(if (symbol.isVal) "val" else "var")
                    if (symbol is KaPropertyAccessorSymbol && symbol.isInline) add("inline")
                    symbol.modality?.let { add(it.toString().lowercase()) }
                    if (symbol.isExpect) add("expect")
                    if (symbol.isActual) add("actual")
                    if (symbol.isExternal) add("external")
                    if (symbol is KaNamedFunctionSymbol) {
                        if (symbol.isSuspend) add("suspend")
                        if (symbol.isInline) add("inline")
                        if (symbol.isOverride) add("override")
                        if (symbol.isOperator) add("operator")
                        if (symbol.isInfix) add("infix")
                        if (symbol.isTailRec) add("tailrec")
                    }
                    if (symbol is KaNamedClassSymbol) {
                        if (symbol.isData) add("data")
                        if (symbol.isInline) add("value")
                        if (symbol.isInner) add("inner")
                        if (symbol.isFun) add("fun")
                    }
                    if (symbol is KaKotlinPropertySymbol) {
                        if (symbol.isLateInit) add("lateinit")
                        if (symbol.isConst) add("const")
                    }
                }
                putJsonArray("parameters") {
                    if (symbol is KaFunctionSymbol) {
                        symbol.valueParameters.forEachIndexed { index, value -> add(buildJsonObject {
                            put("group", if (symbol.contextParameters.isEmpty()) 0 else 1); put("index", index)
                            put("name", value.name.asString())
                            (value.psi as? KtParameter)?.defaultValue?.let { put("default_value", it.text) }
                            putJsonObject("annotations") { put("kotlin.has_default", value.hasDefaultValue) }
                        }) }
                    }
                }
                putJsonObject("extensions") {
                    if (symbol is KaKotlinPropertySymbol && symbol.isConst) {
                        (symbol.initializer as? KaConstantInitializerValue)?.let {
                            put("kotlin.constant_value", it.constant.render())
                        }
                    }
                }
                put("native_key", identities.bindings.getValue(key).getValue("declaration"))
            }
            fun relation(kind: String, target: KaSymbol) {
                val targetId = id(target)
                relations.add(buildJsonObject {
                    put("id", "$key:$kind:$targetId"); put("kind", kind); put("ownership", ownership(declaration))
                    put("from", key); put("to", targetId); put("resolution", "resolved"); put("from_context", context)
                    put("occurrence", "$unit:definition:$path:${declaration.textOffset}:$key")
                })
            }
            if (symbol is KaClassSymbol) symbol.superTypes.forEach { superType ->
                val base = (superType as? KaClassType)?.symbol
                if (base is KaClassSymbol) {
                    relation(if (base.classKind == KaClassKind.INTERFACE && symbol.classKind != KaClassKind.INTERFACE)
                        "implements" else "inherits", base)
                    lookup[declaration]?.add(buildJsonObject { put("kind", "base"); put("wildcard", false); put("target", id(base)) })
                }
            }
            if (symbol is KaCallableSymbol) symbol.directlyOverriddenSymbols.forEach { relation("overrides", it) }
            occurrences.add(buildJsonObject {
                put("id", "$unit:definition:$path:${declaration.textOffset}:$key")
                put("ownership", ownership(declaration)); put("symbol", key); put("location", location(declaration))
                putJsonArray("roles") { add("declaration"); add("definition"); if (implicit) add("implicit") }
                (declaration as? KtNamedDeclaration)?.nameIdentifier?.let { put("selection", place(it)) }
                if (!implicit) (declaration as? KtDeclarationWithBody)?.bodyExpression?.let { put("body", place(it)) }
            })
        }
        val declarations = PsiTreeUtil.collectElementsOfType(file, KtDeclaration::class.java).filter(::recordedDeclaration)
        // A dependency declaration can collide with a name this unit declares (accidental
        // override, ambiguous top-level); those names stay observable even without a reference.
        declarations.forEach { declaration ->
            val symbol = declaration.symbol
            if (symbol.location != KaSymbolLocation.LOCAL) declarationName(symbol)?.let(observationNames::add)
        }
        declarations.forEach { exportDeclaration(it, it.symbol) }
        for (declaration in declarations) {
            val symbol = declaration.symbol
            if (symbol is KaClassSymbol) {
                symbol.declaredMemberScope.constructors.forEach { constructor ->
                    exportDeclaration(declaration, constructor, implicit = true)
                }
                symbol.memberScope.callables.filterIsInstance<KaNamedFunctionSymbol>()
                    .filter { it.origin == KaSymbolOrigin.SOURCE_MEMBER_GENERATED && it.containingDeclaration == symbol }.forEach {
                        exportDeclaration(declaration, it, implicit = true)
                    }
                symbol.declaredMemberScope.callables.filterIsInstance<KaPropertySymbol>()
                    .filter { it.isFromPrimaryConstructor }.forEach { property ->
                        val source = property.psi as? KtDeclaration ?: declaration
                        exportDeclaration(source, property)
                        property.getter?.let { exportDeclaration(source, it, implicit = true) }
                        property.setter?.let { exportDeclaration(source, it, implicit = true) }
                    }
            }
            if (symbol is KaPropertySymbol) {
                symbol.getter?.let { exportDeclaration(declaration, it, implicit = true) }
                symbol.setter?.let { exportDeclaration(declaration, it, implicit = true) }
            }
        }
        file.importDirectives.forEach { directive ->
            directive.aliasName?.let(observationNames::add)
            directive.importedFqName?.asString()?.split('.')?.forEach(observationNames::add)
            val imported = directive.importedReference ?: return@forEach
            val selector = when (imported) {
                is KtDotQualifiedExpression -> imported.selectorExpression as? KtNameReferenceExpression
                is KtNameReferenceExpression -> imported
                else -> null
            } ?: return@forEach
            selector.mainReference.resolveToSymbols().map(::id).distinct().forEach { target ->
                lookup.getValue(file).add(buildJsonObject {
                    put("kind", "import"); put("target", target); put("wildcard", directive.isAllUnder)
                    if (!directive.isAllUnder) {
                        (directive.aliasName ?: directive.importedFqName?.shortName()?.asString())?.let { put("name", it) }
                    }
                })
            }
        }
        scopeNodes.forEach { node -> scopes.add(buildJsonObject {
            put("id", scopeIds.getValue(node)); put("ownership", ownership(node)); put("location", location(node))
            put("kind", requireNotNull(scopeKind(node)))
            enclosingScope(node)?.let { put("lexical_parent", scopeIds.getValue(it)) }
            if (node is KtDeclaration) put("semantic_owner", id(node.symbol))
            putJsonArray("declared_members") { members.getValue(node).distinct().forEach { add(it) } }
            put("lookup", JsonArray(lookup.getValue(node)))
        }) }
        fun resolvedCall(element: PsiElement, referenceId: String, invoked: KaFunctionSymbol, implicit: Boolean = true) {
            val target = id(invoked)
            val from = enclosingDeclaration(element)?.let { id(it.symbol) }
            references.add(buildJsonObject {
                put("id", referenceId); put("ownership", ownership(element)); put("location", location(element))
                from?.let { put("from", it) }; put("target", target)
                put("candidates", JsonArray(emptyList())); put("resolution", "resolved")
                putJsonArray("roles") { add("reference"); add("call"); if (implicit) add("implicit") }
            })
            if (from != null) relations.add(buildJsonObject {
                put("id", "$referenceId:call:$target"); put("kind", "calls"); put("ownership", ownership(element))
                put("from", from); put("to", target); put("reference", referenceId)
                put("resolution", "resolved"); put("from_context", context)
            })
        }
        for (delegation in PsiTreeUtil.collectElementsOfType(file, KtConstructorDelegationCall::class.java)) {
            val call = (delegation.resolveToCall() as? KaSuccessCallInfo)?.call as? KaFunctionCall<*> ?: continue
            resolvedCall(delegation, "$unit:delegation:$path:${delegation.textOffset}", call.signature.symbol, delegation.isImplicit)
        }
        val implicitInvocations = PsiTreeUtil.collectElementsOfType(file, KtCallExpression::class.java).mapNotNull { expression ->
            val call = (expression.resolveToCall() as? KaSuccessCallInfo)?.call as? KaImplicitInvokeCall
            call?.let { expression to it }
        }.toMap()
        implicitInvocations.forEach { (expression, call) ->
            resolvedCall(expression, "$unit:invoke:$path:${expression.textOffset}", call.signature.symbol)
        }
        for (expression in PsiTreeUtil.collectElementsOfType(file, KtSimpleNameExpression::class.java)) {
            observationNames.add(expression.getReferencedName())
            val operator = expression is KtOperationReferenceExpression
            if (operator && expression.operationSignTokenType != null && !expression.isConventionOperator()) continue
            val operatorCall = if (operator) ((expression.parent as? KtExpression)?.resolveToCall() as? KaSuccessCallInfo)?.call else null
            val compound = operatorCall as? KaCompoundAccessCall
            val resolved = when {
                compound != null -> listOf(compound.operationCall.signature.symbol)
                operatorCall is KaFunctionCall<*> -> listOf(operatorCall.signature.symbol)
                else -> expression.mainReference.resolveToSymbols().filter { !operator || it is KaFunctionSymbol }
            }
            val targets = resolved.map(::id).distinct()
            val imported = PsiTreeUtil.getParentOfType(expression, KtImportDirective::class.java) != null
            val access = if (!imported && expression.parent !is KtValueArgumentName && resolved.any { it is KaVariableSymbol })
                expression.readWriteAccessWithFullExpression(useResolveForReadWrite = false).let { (kind, full) ->
                    // Resolve the operator to an Analysis API symbol. Binary library
                    // declarations need not have the KtNamedFunction PSI used by the
                    // standalone IDE helper's plusAssign check.
                    if (kind == ReferenceAccess.READ_WRITE && full is KtBinaryExpression &&
                        full.operationReference.mainReference.resolveToSymbols().singleOrNull()?.let {
                            it is KaNamedFunctionSymbol && it.name in OperatorNameConventions.ASSIGNMENT_OPERATIONS
                        } == true) ReferenceAccess.READ else kind
                } else null
            val referenceId = "$unit:reference:$path:${expression.textOffset}"
            val from = enclosingDeclaration(expression)?.let { id(it.symbol) }
            val parentCall = expression.parent as? KtCallExpression
            val call = operator || (parentCall?.calleeExpression == expression && parentCall !in implicitInvocations)
            references.add(buildJsonObject {
                put("id", referenceId)
                put("ownership", ownership(expression)); put("location", location(expression))
                from?.let { put("from", it) }
                if (targets.size == 1) put("target", targets.single())
                putJsonArray("candidates") { if (targets.size > 1) targets.forEach { add(it) } }
                putJsonArray("roles") {
                    add("reference")
                    if (imported) add("import")
                    if (access == ReferenceAccess.READ || access == ReferenceAccess.READ_WRITE) add("read")
                    if (access == ReferenceAccess.WRITE || access == ReferenceAccess.READ_WRITE) add("write")
                    if (PsiTreeUtil.getParentOfType(expression, KtTypeReference::class.java) != null) add("type")
                    if (call) add("call")
                }
                put("resolution", when (targets.size) { 0 -> "unresolved"; 1 -> "resolved"; else -> "ambiguous" })
            })
            if (call && from != null) targets.forEach { target -> relations.add(buildJsonObject {
                put("id", "$referenceId:call:$target"); put("kind", "calls"); put("ownership", ownership(expression))
                put("from", from); put("to", target); put("reference", referenceId)
                put("resolution", if (targets.size == 1) "resolved" else "candidate")
                put("from_context", context)
            }) }
            if (compound is KaCompoundArrayAccessCall) {
                listOf("get" to compound.getterCall, "set" to compound.setterCall).forEach { (step, invoked) ->
                    resolvedCall(expression, "$referenceId:$step", invoked.signature.symbol)
                }
            }
        }
        for (expression in PsiTreeUtil.collectElementsOfType(file, KtExpression::class.java)) {
            if (expression is KtDeclaration || expression is KtOperationReferenceExpression) continue
            val type = expression.expressionType ?: continue
            val call = (expression.resolveToCall() as? KaSuccessCallInfo)?.call as? KaSingleCall<*, *>
            typedRanges.add(buildJsonObject {
                put("ownership", ownership(expression)); put("range", place(expression))
                put("type", types.project(session, type, ::id, ::parameterIdentity))
                call?.let { put("overload", id(it.signature.symbol)) }
                putJsonObject("substitutions") {
                    call?.typeArgumentsMapping?.forEach { (parameter, argument) ->
                        val (binder, index) = parameterIdentity(parameter)
                        put(types.parameter(parameter, binder, index), types.project(session, argument, ::id, ::parameterIdentity))
                    }
                }
            })
        }
        for (array in PsiTreeUtil.collectElementsOfType(file, KtArrayAccessExpression::class.java)) {
            val parent = array.parent
            val full = when {
                parent is KtBinaryExpression && parent.left == array && parent.operationToken in KtTokens.ALL_ASSIGNMENTS -> parent
                parent is KtUnaryExpression && parent.baseExpression == array && parent.operationToken in KtTokens.INCREMENT_AND_DECREMENT -> parent
                else -> array
            }
            val enclosing = (full.resolveToCall() as? KaSuccessCallInfo)?.call
            // Compound accesses are emitted at their operator, once per real call.
            if (enclosing is KaCompoundArrayAccessCall) continue
            val call = if (parent is KtBinaryExpression && parent.left == array && parent.operationToken == KtTokens.EQ)
                enclosing else (array.resolveToCall() as? KaSuccessCallInfo)?.call
            if (call is KaFunctionCall<*>)
                resolvedCall(array, "$unit:array:$path:${array.textOffset}", call.signature.symbol)
        }
        // Syntax that resolves through a convention name. The sets are conservative supersets:
        // an operator expression may select any arithmetic, assignment or comparison convention.
        if (PsiTreeUtil.collectElementsOfType(file, KtOperationReferenceExpression::class.java).isNotEmpty())
            observationNames.addAll(operatorNames)
        if (PsiTreeUtil.collectElementsOfType(file, KtForExpression::class.java).isNotEmpty())
            observationNames.addAll(listOf("iterator", "next"))
        if (PsiTreeUtil.collectElementsOfType(file, KtArrayAccessExpression::class.java).isNotEmpty())
            observationNames.addAll(listOf("get", "set"))
        val destructuring = PsiTreeUtil.collectElementsOfType(file, KtDestructuringDeclaration::class.java)
            .maxOfOrNull { it.entries.size } ?: 0
        for (index in 1..destructuring) observationNames.add("component$index")
        if (PsiTreeUtil.collectElementsOfType(file, KtProperty::class.java).any { it.delegate != null })
            observationNames.addAll(listOf("getValue", "setValue", "provideDelegate"))
        if (PsiTreeUtil.collectElementsOfType(file, KtStringTemplateExpression::class.java).any { template ->
                template.entries.any { entry -> entry is KtStringTemplateEntryWithExpression } })
            observationNames.add("toString")
        if (implicitInvocations.isNotEmpty() || PsiTreeUtil.collectElementsOfType(file, KtCallExpression::class.java)
                .any { expression -> expression.calleeExpression !is KtNameReferenceExpression })
            observationNames.add("invoke")
    }

    private companion object {
        // Any operator, comparison, range, assignment or increment syntax can select one of these
        // conventions; monitoring the superset keeps invalidation sound without mapping tokens.
        val operatorNames = listOf(
            "plus", "minus", "times", "div", "rem", "inc", "dec",
            "plusAssign", "minusAssign", "timesAssign", "divAssign", "remAssign",
            "unaryPlus", "unaryMinus", "equals", "compareTo", "contains", "rangeTo", "rangeUntil")
    }
}
