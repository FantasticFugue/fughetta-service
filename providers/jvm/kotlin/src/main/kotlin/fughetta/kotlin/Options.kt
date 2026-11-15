package fughetta.kotlin

import org.jetbrains.kotlin.cli.common.arguments.K2JVMCompilerArguments
import org.jetbrains.kotlin.cli.common.arguments.CommonCompilerArgumentsConfigurator
import org.jetbrains.kotlin.cli.common.arguments.parseCommandLineArguments
import org.jetbrains.kotlin.cli.common.arguments.validateArguments
import org.jetbrains.kotlin.cli.common.arguments.toLanguageVersionSettings
import org.jetbrains.kotlin.config.LanguageVersionSettings
import org.jetbrains.kotlin.config.JvmTarget
import org.jetbrains.kotlin.diagnostics.KtSourcelessDiagnosticFactory
import org.jetbrains.kotlin.diagnostics.Severity
import org.jetbrains.kotlin.platform.jvm.JvmPlatforms

class Options(arguments: List<String>) {
    val parsed = K2JVMCompilerArguments().apply { parseCommandLineArguments(arguments, this) }
    init {
        validateArguments(parsed.errors)?.let { throw IllegalArgumentException(it) }
    }
    private val reporter = object : CommonCompilerArgumentsConfigurator.Reporter {
        override fun reportWarning(message: String) { System.err.println(message) }
        override fun reportError(message: String) { throw IllegalArgumentException(message) }
        override fun report(factory: KtSourcelessDiagnosticFactory, message: String) {
            if (factory.severity == Severity.ERROR) reportError(message) else reportWarning(message)
        }
        override fun info(message: String) { System.err.println(message) }
        override fun withLanguageVersionSettings(settings: LanguageVersionSettings) = this
    }
    val language = parsed.toLanguageVersionSettings(reporter)
    val platform = JvmPlatforms.jvmPlatformByTargetVersion(parsed.jvmTarget?.let {
        requireNotNull(JvmTarget.fromString(it)) { "unknown JVM target: $it" }
    } ?: JvmTarget.DEFAULT)
}
