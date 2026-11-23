package fughetta.jvm

import java.io.IOException
import java.io.OutputStream
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.nio.file.StandardCopyOption.ATOMIC_MOVE
import java.nio.file.StandardCopyOption.REPLACE_EXISTING
import java.util.jar.Attributes.Name
import java.util.jar.JarOutputStream
import java.util.jar.Manifest
import java.util.zip.ZipEntry
import java.util.zip.ZipException
import java.util.zip.ZipFile

/** Materializes the JVM classpath of an AAR under its original artifact identity. */
class JvmClasspath(private val objects: Path) {
    private val resolved = mutableMapOf<Pair<String, Boolean>, Path>()

    fun artifact(hash: String, name: String): Path {
        require(objectHash.matches(hash)) { "invalid JVM classpath artifact: $hash" }
        val aar = name.endsWith(".aar", ignoreCase = true)
        return resolved.getOrPut(hash to aar) { resolve(hash, aar) }
    }

    fun arguments(arguments: List<String>, tree: Path, sdks: Map<String, Path>, classpath: Map<String, Path>): List<String> =
        arguments.mapIndexed { index, argument ->
            val libraryOption = argument.substringBefore('=').substringBefore(':') in libraryOptions ||
                arguments.getOrNull(index - 1) in libraryOptions
            objectReference.replace(argument) { match ->
                val key = match.groupValues[1]
                when {
                    key == "tree" -> tree.toString()
                    key.startsWith("object:") -> {
                        val hash = key.removePrefix("object:")
                        (classpath[hash]?.takeIf { libraryOption } ?: objects.resolve(hash)).toString()
                    }
                    else -> requireNotNull(sdks[key.removePrefix("sdk:")]) { "missing SDK mapping: $key" }.toString()
                }
            }
        }

    private fun resolve(hash: String, aar: Boolean): Path {
        val path = objects.resolve(hash)
        val target = objects.resolve("$hash$classesSuffix")
        val archive = try {
            ZipFile(path.toFile())
        } catch (failure: ZipException) {
            if (aar) throw failure
            return path
        }
        archive.use { zip ->
            val topLevelClasses = zip.stream().anyMatch { !it.isDirectory && it.name.endsWith(".class") }
            if (!aar && topLevelClasses) return path
            val jars = zip.entries().asSequence().filter { !it.isDirectory && (it.name == "classes.jar" ||
                (it.name.startsWith("libs/") && it.name.endsWith(".jar"))) }.sortedBy { it.name }.toList()
            if (jars.isEmpty()) return path
            val merge = topLevelClasses || jars.size > 1
            if (!ready(target, merge)) publish(zip, jars, target, merge)
        }
        return target
    }

    private fun ready(target: Path, merged: Boolean): Boolean {
        if (!Files.isRegularFile(target, NOFOLLOW_LINKS)) return false
        if (!merged) return true
        return try {
            ZipFile(target.toFile()).use { it.comment == mergedClasspath }
        } catch (_: IOException) {
            false
        }
    }

    private fun publish(archive: ZipFile, jars: List<ZipEntry>, target: Path, merged: Boolean) {
        val staging = Files.createDirectories(objects.resolve("partial"))
        val temporary = Files.createTempFile(staging, "classes-", ".jar")
        try {
            val budget = UnpackBudget()
            if (merged) merge(archive, jars, temporary, staging, budget)
            else Files.newOutputStream(temporary).use { budget.copy(archive, jars.single(), it) }
            try {
                Files.move(temporary, target, ATOMIC_MOVE, REPLACE_EXISTING)
            } catch (failure: IOException) {
                if (!ready(target, merged)) throw failure
            }
        } finally {
            Files.deleteIfExists(temporary)
        }
    }

    private fun merge(archive: ZipFile, jars: List<ZipEntry>, target: Path, staging: Path, budget: UnpackBudget) {
        val manifest = Manifest().apply {
            mainAttributes[Name.MANIFEST_VERSION] = "1.0"
            mainAttributes[Name.MULTI_RELEASE] = "true"
        }
        JarOutputStream(Files.newOutputStream(target), manifest).use { output ->
            output.setComment(mergedClasspath)
            val owners = mutableMapOf<String, String>()
            val mappings = mutableSetOf<String>()
            var mappingNumber = 0
            fun append(zip: ZipFile, origin: String) {
                for (entry in zip.entries().asSequence().filterNot(ZipEntry::isDirectory).sortedBy { it.name }) {
                    var name = entry.name
                    when {
                        name.endsWith(".class") -> {
                            val logicalName = if (name.startsWith(releasePrefix)) name.removePrefix(releasePrefix).substringAfter('/') else name
                            val owner = owners.putIfAbsent(logicalName, origin)
                            require(owner == null || owner == origin) { "duplicate JVM class in artifact: ${logicalName.removeSuffix(".class")}" }
                        }
                        name.startsWith("META-INF/") && name.endsWith(".kotlin_module") -> {
                            while (!mappings.add(name)) name = "META-INF/fughetta-${++mappingNumber}.kotlin_module"
                        }
                        else -> continue
                    }
                    output.putNextEntry(ZipEntry(name))
                    budget.copy(zip, entry, output)
                    output.closeEntry()
                }
            }
            append(archive, "")
            for (entry in jars) {
                val nested = Files.createTempFile(staging, "aar-member-", ".jar")
                try {
                    Files.newOutputStream(nested).use { budget.copy(archive, entry, it) }
                    ZipFile(nested.toFile()).use { append(it, entry.name) }
                } finally {
                    Files.deleteIfExists(nested)
                }
            }
        }
    }

    private class UnpackBudget {
        private var remaining = maxClassesBytes
        private val buffer = ByteArray(DEFAULT_BUFFER_SIZE)

        fun copy(archive: ZipFile, entry: ZipEntry, output: OutputStream) {
            require(entry.size in 0..remaining) { "JVM archive data exceeds analysis budget: ${entry.name}" }
            remaining -= entry.size
            archive.getInputStream(entry).use { input ->
                var unread = entry.size
                while (unread > 0) {
                    val count = input.read(buffer, 0, minOf(unread, buffer.size.toLong()).toInt())
                    require(count > 0) { "truncated JVM archive entry: ${entry.name}" }
                    output.write(buffer, 0, count)
                    unread -= count
                }
                require(input.read() == -1) { "invalid JVM archive entry size: ${entry.name}" }
            }
        }
    }

    companion object {
        private const val classesSuffix = "-classes.jar"
        private const val maxClassesBytes = 64L shl 20
        private const val mergedClasspath = "Fughetta merged AAR classpath"
        private const val releasePrefix = "META-INF/versions/"
        private val objectHash = Regex("[0-9a-f]{64}")
        private val objectReference = Regex("\\$\\{(tree|object:[^}]+|sdk:[^}]+)}")
        private val libraryOptions = setOf("-cp", "-classpath", "--class-path", "-Xfriend-paths")

        fun artifactName(name: String): String = name.removeSuffix(classesSuffix).takeIf(objectHash::matches) ?: name
    }
}
