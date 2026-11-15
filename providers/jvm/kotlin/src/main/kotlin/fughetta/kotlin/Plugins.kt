package fughetta.kotlin

import java.net.URLClassLoader
import java.util.ServiceLoader
import org.jetbrains.kotlin.analysis.api.platform.projectStructure.KotlinCompilerPluginsProvider
import org.jetbrains.kotlin.analysis.api.projectStructure.KaModule
import org.jetbrains.kotlin.analysis.api.standalone.base.declarations.KotlinStandaloneFirCompilerPluginsProvider
import org.jetbrains.kotlin.cli.create
import org.jetbrains.kotlin.cli.extensionsStorage
import org.jetbrains.kotlin.cli.plugins.*
import org.jetbrains.kotlin.utils.topologicalSort
import org.jetbrains.kotlin.compiler.plugin.CommandLineProcessor
import org.jetbrains.kotlin.compiler.plugin.CompilerPluginRegistrar
import org.jetbrains.kotlin.compiler.plugin.ExperimentalCompilerApi
import org.jetbrains.kotlin.config.CommonConfigurationKeys
import org.jetbrains.kotlin.config.CompilerConfiguration
import org.jetbrains.kotlin.extensions.ExtensionPointDescriptor

/** Plugin configurations are module inputs, while their class loaders live with the environment. */
@OptIn(ExperimentalCompilerApi::class, org.jetbrains.kotlin.analysis.api.KaPlatformInterface::class)
class Plugins(inputs: List<ModuleInputs>, private val moduleId: (KaModule) -> String?) : KotlinCompilerPluginsProvider, AutoCloseable {
    private val loaders = mutableListOf<URLClassLoader>()
    private val configurations = mutableListOf<CompilerConfiguration>()
    private val providers = mutableMapOf<String, KotlinCompilerPluginsProvider>()
    val opaqueModules = mutableSetOf<String>()

    init {
        try {
            inputs.forEach { input ->
                val options = Options(input.compilerArguments)
                val modern = extractPluginClasspathAndOptions(options.parsed.pluginConfigurations.orEmpty().asIterable())
                val modernPaths = modern.flatMap { it.classpath }.map(java.nio.file.Path::of).toSet()
                val paths = (input.plugins.filterNot { it in modernPaths } +
                    options.parsed.pluginClasspaths.orEmpty().map(java.nio.file.Path::of)).distinct()
                val order = options.parsed.pluginOrderConstraints.orEmpty().map { raw ->
                    requireNotNull(extractPluginOrderConstraint(raw)) { "invalid plugin order constraint: $raw" }
                }
                if (paths.isNotEmpty() || modern.isNotEmpty() || !options.parsed.pluginOptions.isNullOrEmpty()) {
                    fun loader(paths: List<java.nio.file.Path>) = URLClassLoader(
                        paths.map { it.toUri().toURL() }.toTypedArray(), javaClass.classLoader).also { loaders.add(it) }
                    val configuration = CompilerConfiguration.create()
                    configurations.add(configuration)
                    configuration.put(CommonConfigurationKeys.LANGUAGE_VERSION_SETTINGS, options.language)
                    configuration.put(CommonConfigurationKeys.MODULE_NAME, options.parsed.moduleName ?: input.id)
                    val registrars = mutableListOf<CompilerPluginRegistrar>()
                    if (paths.isNotEmpty() || !options.parsed.pluginOptions.isNullOrEmpty()) {
                        val legacy = loader(paths)
                        registrars.addAll(ServiceLoader.load(CompilerPluginRegistrar::class.java, legacy).toList())
                        processCompilerPluginsOptions(configuration, options.parsed.pluginOptions.orEmpty().asIterable(),
                            ServiceLoader.load(CommandLineProcessor::class.java, legacy).toList())
                    }
                    modern.forEach { spec ->
                        val loaded = loader(spec.classpath.map(java.nio.file.Path::of))
                        val found = ServiceLoader.load(CompilerPluginRegistrar::class.java, loaded).toList()
                        require(found.size == 1) { "plugin configuration requires one registrar: ${spec.rawArgument}" }
                        val registrar = found.single()
                        val processors = ServiceLoader.load(CommandLineProcessor::class.java, loaded).toList()
                        require(processors.size <= 1) { "plugin configuration has multiple option processors: ${spec.rawArgument}" }
                        val processor = processors.singleOrNull()
                        require(processor == null || registrar.pluginId.isEmpty() || processor.pluginId == registrar.pluginId) {
                            "plugin registrar and option processor have different identities: ${spec.rawArgument}"
                        }
                        if (spec.options.isNotEmpty()) {
                            processCompilerPluginOptions(requireNotNull(processor) { "plugin has no option processor: ${spec.rawArgument}" },
                                spec.options, configuration)
                        }
                        registrars.add(registrar)
                    }
                    // KGP installs Kotlin's scripting registrar on regular JVM
                    // compilations too. Its FIR hooks configure/resolve scripts;
                    // with only regular sources and no script configuration it
                    // adds no unbound source reads. Keep its binaries and options
                    // as inputs; other registrars retain the opaque-input boundary.
                    val scriptingInputs = input.sources.any {
                        val name = it.fileName.toString()
                        !name.endsWith(".kt") && !name.endsWith(".java")
                    } || !options.parsed.scriptTemplates.isNullOrEmpty() ||
                        !options.parsed.scriptResolverEnvironment.isNullOrEmpty() ||
                        !options.parsed.defaultScriptExtension.isNullOrEmpty() ||
                        !options.parsed.expression.isNullOrEmpty() ||
                        !options.parsed.pluginOptions.isNullOrEmpty() || modern.any { it.options.isNotEmpty() }
                    if (registrars.any { it.pluginId != "kotlin.scripting" || scriptingInputs }) opaqueModules.add(input.id)
                    val named = registrars.filter { it.pluginId.isNotEmpty() }
                    require(named.map { it.pluginId }.distinct().size == named.size) { "duplicate compiler plugin identity" }
                    val byId = named.associateBy { it.pluginId }
                    val dependencies = order.filter { it.before in byId && it.after in byId }
                        .groupBy({ it.after }, { byId.getValue(it.before) })
                    val sorted = topologicalSort(registrars,
                        reportCycle = { error("compiler plugin order contains a cycle at ${it.pluginId}") },
                        dependencies = { dependencies[pluginId].orEmpty() }).asReversed()
                    configuration.addAll(CompilerPluginRegistrar.COMPILER_PLUGIN_REGISTRARS, sorted)
                    providers[input.id] = KotlinStandaloneFirCompilerPluginsProvider(configuration)
                }
            }
        } catch (failure: Throwable) {
            close()
            throw failure
        }
    }

    override fun <T : Any> getRegisteredExtensions(module: KaModule, extensionType: ExtensionPointDescriptor<T>): List<T> =
        providers[moduleId(module)]?.getRegisteredExtensions(module, extensionType).orEmpty()

    override fun isPluginOfTypeRegistered(module: KaModule, pluginType: KotlinCompilerPluginsProvider.CompilerPluginType): Boolean =
        providers[moduleId(module)]?.isPluginOfTypeRegistered(module, pluginType) ?: false

    override fun close() {
        try {
            configurations.forEach { configuration -> configuration.extensionsStorage?.disposables?.forEach { it.dispose() } }
        } finally {
            loaders.forEach { it.close() }
            configurations.clear()
            loaders.clear()
            providers.clear()
            opaqueModules.clear()
        }
    }
}
