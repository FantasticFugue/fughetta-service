package fughetta.scala

import java.security.MessageDigest
import scala.reflect.internal.Trees.*

internal fun digest(text: String): String = MessageDigest.getInstance("SHA-256").digest(text.toByteArray(Charsets.UTF_8))
    .joinToString("") { "%02x".format(it) }

internal data class SemanticValues(val declarations: String, val inline: String)

internal fun semanticValues(compilation: Compilation): SemanticValues? {
    if (compilation.failed) return null
    val declarations = sortedSetOf<String>()
    val bodies = sortedSetOf<String>()
    var complete = true
    fun visit(tree: Tree, source: Source) {
        val symbol = tree.symbol()
        if (symbol != null && symbol.exists()) {
            when (tree) {
                is ClassDef, is ModuleDef -> {
                    val owner = if (tree is ModuleDef) symbol.moduleClass() else symbol
                    declarations.add("${symbol.fullName()}:${symbol.flags()}:${owner.info().parents()}")
                    owner.info().decls().items().filter { !it.isPrivate() }.forEach {
                        declarations.add("${it.fullName()}:${it.flags()}:${it.privateWithin()}:${it.info()}:${it.annotations()}")
                    }
                }
                is DefDef -> {
                    if (symbol.isMacro()) complete = false
                    val inline = symbol.annotations().items().any { it.atp().typeSymbol().fullName() == "scala.inline" }
                    if (inline || symbol.isDefaultGetter()) {
                        val body = source.snippet(tree.rhs().pos())
                        if (body == null) complete = false
                        else bodies.add("${symbol.fullName()}:${symbol.info()}:$body")
                    }
                    return
                }
                is ValDef -> return
            }
        }
        for (child in tree.nodes()) visit(child, source)
    }
    compilation.run.units().items().forEach { unit ->
        compilation.sources[unit.source().file().path()]?.let { visit(unit.body(), it) }
    }
    return if (complete) SemanticValues(digest(declarations.strings().toString()), digest(bodies.strings().toString())) else null
}
