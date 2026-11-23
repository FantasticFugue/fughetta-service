package fughetta.scala

import java.io.ByteArrayInputStream
import java.io.OutputStream
import java.net.URI
import java.nio.file.Path
import java.util.Locale
import javax.tools.Diagnostic
import javax.tools.DiagnosticCollector
import javax.tools.ForwardingJavaFileManager
import javax.tools.JavaFileManager
import javax.tools.JavaFileObject
import javax.tools.SimpleJavaFileObject
import javax.tools.StandardLocation
import javax.tools.ToolProvider
import scala.reflect.internal.util.Position
import scala.reflect.io.AbstractFile
import scala.reflect.io.VirtualDirectory

internal fun compileJava(
    sources: List<Source>, input: ModuleInputs, output: VirtualDirectory, upstream: List<VirtualDirectory>,
): List<Message> {
    val javaSources = sources.filter { it.file.fileName.toString().endsWith(".java") }
    if (javaSources.isEmpty()) return emptyList()
    val compiler = requireNotNull(ToolProvider.getSystemJavaCompiler()) { "Scala mixed compilation requires a JDK" }
    val diagnostics = DiagnosticCollector<JavaFileObject>()
    val files = javaSources.associateBy { it.file.toUri() }
    val documents = files.map { (uri, source) ->
        object : SimpleJavaFileObject(uri, JavaFileObject.Kind.SOURCE) {
            override fun getCharContent(ignoreEncodingErrors: Boolean): CharSequence = source.text
        }
    }

    class ClassFile(val binaryName: String, val file: AbstractFile) :
        SimpleJavaFileObject(URI.create("mem:///" + binaryName.replace('.', '/') + ".class"), JavaFileObject.Kind.CLASS) {
        override fun openInputStream() = ByteArrayInputStream(file.toByteArray())
        override fun openOutputStream(): OutputStream = file.output()
    }

    val classes = linkedMapOf<String, ClassFile>()
    fun collect(directory: AbstractFile, prefix: String) {
        for (file in directory.items()) {
            if (file.isDirectory()) collect(file, prefix + file.name() + ".")
            else if (file.name().endsWith(".class")) {
                val name = prefix + file.name().removeSuffix(".class")
                classes.putIfAbsent(name, ClassFile(name, file))
            }
        }
    }
    (listOf(output) + upstream).forEach { collect(it, "") }
    compiler.getStandardFileManager(diagnostics, Locale.ROOT, Charsets.UTF_8).use { standard ->
        standard.setLocationFromPaths(StandardLocation.CLASS_PATH, input.libraries.map { it.path })
        val manager = object : ForwardingJavaFileManager<JavaFileManager>(standard) {
            override fun getJavaFileForInput(location: JavaFileManager.Location, className: String, kind: JavaFileObject.Kind): JavaFileObject? =
                if (location == StandardLocation.CLASS_PATH && kind == JavaFileObject.Kind.CLASS && className in classes)
                    classes[className] else super.getJavaFileForInput(location, className, kind)

            override fun getJavaFileForOutput(
                location: JavaFileManager.Location, className: String, kind: JavaFileObject.Kind, sibling: javax.tools.FileObject?,
            ): JavaFileObject {
                val parts = className.split('.')
                val directory = parts.dropLast(1).fold(output as AbstractFile) { parent, name -> parent.subdirectoryNamed(name) }
                return ClassFile(className, directory.fileNamed(parts.last() + kind.extension))
            }

            override fun inferBinaryName(location: JavaFileManager.Location, file: JavaFileObject): String =
                (file as? ClassFile)?.binaryName ?: super.inferBinaryName(location, file)

            override fun list(
                location: JavaFileManager.Location, packageName: String, kinds: MutableSet<JavaFileObject.Kind>, recurse: Boolean,
            ): Iterable<JavaFileObject> {
                val disk = super.list(location, packageName, kinds, recurse)
                if (location != StandardLocation.CLASS_PATH || JavaFileObject.Kind.CLASS !in kinds) return disk
                val memory = classes.values.filter {
                    val pkg = it.binaryName.substringBeforeLast('.', "")
                    pkg == packageName || (recurse && (packageName.isEmpty() || pkg.startsWith("$packageName.")))
                }
                return (memory + disk).distinctBy { inferBinaryName(location, it) }
            }
        }
        val options = mutableListOf("-proc:none")
        if (input.jdk.toRealPath() != Path.of(System.getProperty("java.home")).toRealPath()) {
            options.addAll(listOf("--system", input.jdk.toString()))
        }
        compiler.getTask(null, manager, diagnostics, options, null, documents).call()
    }
    return diagnostics.diagnostics.mapNotNull { diagnostic ->
        val severity = when (diagnostic.kind) {
            Diagnostic.Kind.ERROR -> "error"
            Diagnostic.Kind.WARNING, Diagnostic.Kind.MANDATORY_WARNING -> "warning"
            else -> return@mapNotNull null
        }
        val source = files[diagnostic.source?.toUri()]
        val position = if (source != null && diagnostic.position >= 0) {
            val point = diagnostic.position.toInt().coerceIn(0, source.text.length)
            val start = diagnostic.startPosition.toInt().coerceIn(0, point)
            val end = diagnostic.endPosition.toInt().coerceIn(point, source.text.length)
            Position.range(source.batch, start, point, end)
        } else null
        Message(severity, diagnostic.getMessage(Locale.ROOT), position, diagnostic.code)
    }
}
