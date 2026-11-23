package fughetta.jvm

import java.net.URLClassLoader
import java.nio.file.Files
import java.nio.file.Path
import java.util.ServiceLoader

fun main(args: Array<String>) {
    if (args.firstOrNull() == "--gradle") {
        require(args.size == 6) { "expected --gradle <installation> <user-home> <slot-state> <idle-ms> <jvm-cpu-option>" }
        serveGradle(Path.of(args[1]), Path.of(args[2]), Path.of(args[3]), args[4].toInt(), args[5])
        return
    }
    require(args.size == 2 && args[0] == "--adapter") { "expected --adapter <language-adapter.jar>" }
    val jar = Path.of(args[1]).toAbsolutePath().normalize()
    require(Files.isRegularFile(jar)) { "language adapter does not exist: $jar" }
    // Compiler and platform output must never share the protocol stream.
    val protocolOutput = System.out
    System.setOut(System.err)
    // Parent-first loading keeps the adapter interface and JSON model shared with the host;
    // the adapter manifest Class-Path brings its compiler.
    val loader = URLClassLoader(arrayOf(jar.toUri().toURL()), LanguageAdapter::class.java.classLoader)
    Thread.currentThread().contextClassLoader = loader
    val adapter = ServiceLoader.load(LanguageAdapter::class.java, loader).toList().singleOrNull()
        ?: error("$jar must provide exactly one ${LanguageAdapter::class.java.name}")
    Session(Path.of("").toAbsolutePath().normalize(), adapter).use { session ->
        JsonLines().serve(System.`in`, protocolOutput, session::dispatch)
    }
}
