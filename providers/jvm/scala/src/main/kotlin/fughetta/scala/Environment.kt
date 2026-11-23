package fughetta.scala

import java.io.Closeable
import java.net.URI
import java.nio.file.FileSystems
import java.nio.file.Files
import java.nio.file.Path
import scala.Option
import scala.collection.IterableOnce
import scala.jdk.javaapi.CollectionConverters
import scala.reflect.internal.util.BatchSourceFile
import scala.reflect.internal.util.Position
import scala.reflect.internal.util.SourceFile
import scala.reflect.io.AbstractFile
import scala.reflect.io.VirtualDirectory
import scala.reflect.io.VirtualFile
import scala.reflect.io.ZipArchive
import scala.tools.nsc.Global
import scala.tools.nsc.Settings
import scala.tools.nsc.classpath.AggregateClassPath
import scala.tools.nsc.classpath.ClassPathFactory
import scala.tools.nsc.classpath.JrtClassPath
import scala.tools.nsc.classpath.VirtualDirectoryClassPath
import scala.tools.nsc.reporters.StoreReporter
import scala.runtime.AbstractFunction1
import scala.runtime.BoxedUnit

internal fun <T> IterableOnce<T>.items(): Sequence<T> = sequence {
    val iterator = iterator()
    while (iterator.hasNext()) yield(iterator.next())
}

internal fun <T> List<T>.scalaList() = CollectionConverters.asScala(this).toList()

internal class Source(val file: Path, val text: String, val generated: Boolean, logicalPath: String = file.fileName.toString()) {
    private val script = file.fileName.toString().endsWith(".sc")
    private val prefix = if (script) "object FughettaScript_${digest(logicalPath).take(20)} {\n" else ""
    private val content = if (script) prefix + text.replace(Regex("^#![^\\r\\n]*")) { " ".repeat(it.value.length) } + "\n}" else text
    val batch: SourceFile = BatchSourceFile(VirtualFile(file.fileName.toString(), file.wire()), content.toCharArray())
    private val offsets = IntArray(text.length + 1).also { map ->
        var index = 0
        var scalar = 0
        while (index < text.length) {
            val width = Character.charCount(text.codePointAt(index))
            repeat(width) { map[index++] = scalar }
            scalar++
        }
        map[index] = scalar
    }

    fun index(offset: Int) = (offset - prefix.length).coerceIn(0, text.length)
    fun scalar(index: Int) = offsets[index]
    fun contains(position: Position) = position.isDefined() && position.point() in prefix.length..(prefix.length + text.length)
    fun snippet(position: Position): String? =
        if (contains(position) && position.isRange()) text.substring(index(position.start()), index(position.end())) else null
}

internal data class Message(val severity: String, val text: String, val position: Position? = null, val code: String = "scalac")

internal class Compilation(
    val compiler: Global,
    val run: Global.Run,
    val sources: Map<String, Source>,
    val messages: List<Message>,
    val exports: VirtualDirectory?,
) : Closeable {
    val failed get() = messages.any { it.severity == "error" }
    override fun close() = compiler.close()
}

internal class Environment(private val input: ModuleInputs, private val upstream: List<VirtualDirectory>) {
    fun compile(sources: List<Source>, export: Boolean = false, typed: ((Compilation) -> Unit)? = null): Compilation {
        val settingsErrors = mutableListOf<String>()
        val settings = Settings(object : AbstractFunction1<String, BoxedUnit>() {
            override fun apply(message: String): BoxedUnit {
                settingsErrors.add(message)
                return BoxedUnit.UNIT
            }
        })
        val unused = settings.processArguments(input.compilerArguments.scalaList(), true)._2()
        if (!unused.isEmpty()) settingsErrors.add("unexpected Scala compiler arguments: ${unused.items().joinToString(" ")}")
        val output = VirtualDirectory("fughetta-${digest(input.id).take(24)}", Option.empty())
        val classpath = input.libraries.map { it.path }
        settings.classpath().`value_$eq`("")
        settings.usejavacp().`value_$eq`(false)
        settings.Yrangepos().`value_$eq`(true)
        settings.stopBefore().`value_$eq`(emptyList<String>().scalaList())
        settings.stopAfter().`value_$eq`((if (export) emptyList() else listOf("typer")).scalaList())
        settings.outputDirs().setSingleOutput(output)
        // The worker admits compiler code through project-model plugin references.
        settings.plugin().`value_$eq`(input.plugins.map { it.toString() }.scalaList())
        settings.pluginsDir().`value_$eq`("")
        val reporter = StoreReporter(settings)
        val indexed = sources.associateBy { it.file.wire() }
        lateinit var compiler: Global
        compiler = if (export && typed != null)
            ProjectionCompiler(settings, reporter) { run -> typed(Compilation(compiler, run, indexed, emptyList(), null)) }
        else Global(settings, reporter)
        try {
            val home = input.jdk.toRealPath()
            val current = Path.of(System.getProperty("java.home")).toRealPath()
            val factory = ClassPathFactory(settings, compiler.closeableRegistry())
            val base = compiler.classPath().let { path ->
                if (path is AggregateClassPath) path.aggregates().items().toList() else listOf(path)
            }.toMutableList()
            if (home != current) {
                val image = FileSystems.newFileSystem(URI.create("jrt:/"), mapOf("java.home" to home.toString()))
                val platform = JrtClassPath(image, false)
                compiler.closeableRegistry().registerCloseable(platform)
                base.removeAll { it.asURLs().items().any { url -> url.protocol == "jrt" } }
                base.add(0, platform)
            }
            val binaries = classpath.map { path ->
                val file = if (Files.isDirectory(path)) AbstractFile.getDirectory(scala.reflect.io.Path.apply(path.toFile()))
                else ZipArchive.fromFile(path.toFile())
                requireNotNull(file) { "cannot open Scala classpath entry: $path" }
                factory.newClassPath(file)
            }
            val entries = base + upstream.map(::VirtualDirectoryClassPath) + binaries
            compiler.platform().`currentClassPath_$eq`(Option.apply(AggregateClassPath.createAggregate(entries.scalaList())))
            val run = compiler.Run()
            if (settingsErrors.isEmpty()) run.compileSources(sources.map { it.batch }.scalaList())
            val scalaMessages = settingsErrors.map { Message("error", it) } + reporter.infos().items().map {
                Message(it.severity().toString().lowercase(), it.msg(), it.pos())
            }.toList()
            val messages = if (export && scalaMessages.none { it.severity == "error" })
                scalaMessages + compileJava(sources, input, output, upstream) else scalaMessages
            return Compilation(compiler, run, indexed, messages,
                output.takeIf { export && messages.none { it.severity == "error" } })
        } catch (failure: Throwable) {
            compiler.close()
            throw failure
        }
    }
}
