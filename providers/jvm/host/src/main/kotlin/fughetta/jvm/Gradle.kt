package fughetta.jvm

import java.io.OutputStream
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlinx.serialization.json.*
import org.gradle.tooling.BuildException
import org.gradle.tooling.CancellationTokenSource
import org.gradle.tooling.GradleConnector
import org.gradle.tooling.ProjectConnection
import org.gradle.tooling.internal.consumer.DefaultGradleConnector

internal fun serveGradle(installation: Path, home: Path, slotState: Path, idleMillis: Int, cpuOption: String) {
    val wire = JsonLines()
    val reader = System.`in`.buffered()
    val writer = System.out.buffered()
    System.setOut(System.err)
    System.setProperty("org.gradle.native", "false")
    // Gradle forwards mutable -D options after daemon startup. The Java launcher expands this file before startup.
    val daemonOptions = slotState.resolve("daemon-jvm.args")
    Files.writeString(daemonOptions, "-Dorg.gradle.native=false\n")
    val connector = GradleConnector.newConnector() as DefaultGradleConnector
    connector.daemonBaseDir(slotState.resolve("daemon").toFile())
    connector.daemonMaxIdleTime(idleMillis, TimeUnit.MILLISECONDS)
    val connection = connector
        .useInstallation(installation.toFile())
        .useGradleUserHomeDir(home.toFile())
        .forProjectDirectory(Path.of("").toAbsolutePath().toFile())
        .connect()
    val executor = Executors.newSingleThreadExecutor()
    val running = AtomicReference<CancellationTokenSource?>()
    try {
        while (true) {
            val request = wire.read(reader) ?: break
            val id = request.getValue("request_id").jsonPrimitive.long
            when (val operation = request.getValue("operation").jsonPrimitive.content) {
                "build" -> {
                    val params = request["params"] as? JsonObject
                    if (params == null) {
                        wire.reply(writer, id, error = "Gradle build requires params")
                        continue
                    }
                    val token = GradleConnector.newCancellationTokenSource()
                    if (!running.compareAndSet(null, token)) {
                        wire.reply(writer, id, error = "Gradle session already has an active build")
                        continue
                    }
                    executor.execute {
                        val result = runCatching { build(connection, token, params, daemonOptions, cpuOption) }
                        running.set(null)
                        result.fold(
                            { wire.reply(writer, id, result = it) },
                            { wire.reply(writer, id, error = it.message ?: it.javaClass.simpleName) },
                        )
                    }
                }
                "cancel" -> running.get()?.cancel()
                else -> wire.reply(writer, id, error = "unknown Gradle operation: $operation")
            }
        }
    } finally {
        running.get()?.cancel()
        executor.shutdown()
        try {
            connector.disconnect()
        } finally {
            connection.close()
        }
    }
}

private fun build(connection: ProjectConnection, token: CancellationTokenSource, params: JsonObject, daemonOptions: Path, cpuOption: String): JsonObject {
    val log = params["log"]?.jsonPrimitive?.contentOrNull?.let(Path::of)
    val output = BuildOutput(params.getValue("output_limit").jsonPrimitive.int, log, params.getValue("log_limit").jsonPrimitive.long)
    var exit = 0
    var reusable = true
    output.use {
        try {
            connection.newBuild()
                .addJvmArguments("@$daemonOptions", cpuOption)
                .withArguments(params.getValue("arguments").jsonArray.map { it.jsonPrimitive.content })
                .setEnvironmentVariables(params.getValue("environment").jsonObject.mapValues { it.value.jsonPrimitive.content })
                .setColorOutput(false)
                .setStandardOutput(output)
                .setStandardError(output)
                .withCancellationToken(token.token())
                .run()
        } catch (failure: Exception) {
            exit = 1
            reusable = failure is BuildException
            output.write(("\n" + (failure.message ?: failure.javaClass.simpleName) + "\n").encodeToByteArray())
        }
    }
    return buildJsonObject {
        put("exit_code", exit)
        put("reusable", reusable)
        put("output", output.text())
        put("truncated", output.truncated)
        put("log_truncated", output.logTruncated)
        put("log_error", output.logError)
    }
}

private class BuildOutput(limit: Int, path: Path?, private val logLimit: Long) : OutputStream() {
    private val tail = ByteArray(limit)
    private var size = 0
    private var next = 0
    private var logged = 0L
    var truncated = false
        private set
    var logTruncated = false
        private set
    var logError = ""
        private set
    private var log = path?.let {
        try { Files.newOutputStream(it) }
        catch (failure: java.io.IOException) { logError = failure.message.orEmpty(); null }
    }

    override fun write(value: Int) = write(byteArrayOf(value.toByte()), 0, 1)

    @Synchronized
    override fun write(bytes: ByteArray, offset: Int, length: Int) {
        log?.let { file ->
            val keep = minOf(length.toLong(), logLimit - logged).toInt()
            logTruncated = logTruncated || keep < length
            try {
                file.write(bytes, offset, keep)
                logged += keep
            } catch (failure: java.io.IOException) {
                close()
                logError = failure.message.orEmpty()
            }
        }
        if (length > tail.size - size) truncated = true
        if (tail.isEmpty()) return
        if (length >= tail.size) {
            bytes.copyInto(tail, 0, offset + length - tail.size, offset + length)
            size = tail.size
            next = 0
            return
        }
        val first = minOf(length, tail.size - next)
        bytes.copyInto(tail, next, offset, offset + first)
        bytes.copyInto(tail, 0, offset + first, offset + length)
        size = minOf(size + length, tail.size)
        next = (next + length) % tail.size
    }

    fun text(): String = if (size < tail.size) tail.decodeToString(endIndex = size)
        else (tail.copyOfRange(next, tail.size) + tail.copyOfRange(0, next)).decodeToString()

    @Synchronized
    override fun close() {
        val file = log ?: return
        log = null
        try { file.close() }
        catch (failure: java.io.IOException) { logError = failure.message.orEmpty() }
    }
}
