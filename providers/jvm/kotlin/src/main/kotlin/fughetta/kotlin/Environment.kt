package fughetta.kotlin

import com.intellij.openapi.util.Disposer
import com.intellij.openapi.application.ApplicationManager
import org.jetbrains.kotlin.psi.KtPsiFactory
import com.intellij.pom.PomModel
import com.intellij.pom.core.impl.PomModelImpl
import org.jetbrains.kotlin.analysis.api.platform.modification.publishModuleOutOfBlockModificationEvent
import org.jetbrains.kotlin.analysis.api.platform.modification.KaSourceModificationService
import org.jetbrains.kotlin.analysis.api.platform.modification.KaSourceModificationLocality
import org.jetbrains.kotlin.analysis.api.platform.modification.KaElementModificationType
import com.intellij.psi.PsiElement
import com.intellij.psi.util.PsiTreeUtil
import org.jetbrains.kotlin.psi.KtElement
import org.jetbrains.kotlin.psi.KtModifierListOwner
import org.jetbrains.kotlin.lexer.KtTokens
import org.jetbrains.kotlin.analysis.api.projectStructure.KaSourceModule
import org.jetbrains.kotlin.analysis.api.standalone.StandaloneAnalysisAPISession
import org.jetbrains.kotlin.analysis.api.standalone.buildStandaloneAnalysisAPISession
import org.jetbrains.kotlin.analysis.project.structure.builder.buildKtLibraryModule
import org.jetbrains.kotlin.analysis.project.structure.builder.buildKtSdkModule
import org.jetbrains.kotlin.analysis.project.structure.builder.buildKtSourceModule
import org.jetbrains.kotlin.platform.jvm.JvmPlatforms
import java.nio.file.Path
import java.nio.file.Files
import com.intellij.testFramework.LightVirtualFile
import com.intellij.openapi.fileTypes.FileTypeRegistry

/** Host-resolved inputs: library paths point to retained objects or mapped SDKs. */
data class ModuleInputs(
    val id: String,
    val sources: List<Path>,
    val libraries: List<Path>,
    val jdk: Path,
    val dependencies: List<String>,
    val friends: List<String>,
    val compilerArguments: List<String> = emptyList(),
    val plugins: List<Path> = emptyList(),
    val binaryFriends: List<Path> = emptyList(),
    val generated: Map<Path, Path> = emptyMap(),
    val binaryInputs: Map<String, String> = emptyMap(),
)

data class SourceChanges(val modules: Set<String>, val declarations: Set<String>)

/** Owns all PSI and Analysis API objects for one installed project graph. */
@OptIn(org.jetbrains.kotlin.analysis.api.KaPlatformInterface::class)
class Environment(inputs: List<ModuleInputs>, refreshArchives: Boolean = false, documents: Map<Path, String> = emptyMap()) : AutoCloseable {
    private val disposable = Disposer.newDisposable("fughetta-kotlin")
    val modules = linkedMapOf<String, KaSourceModule>()
    val session: StandaloneAnalysisAPISession
    private val plugins = Plugins(inputs) { module -> modules.entries.find { it.value == module }?.key }
    private val declarations: Declarations
    val opaqueModules: Set<String> get() = plugins.opaqueModules
    private val files: Map<Path, List<com.intellij.psi.PsiFile>>
    private val fileOwners: Map<com.intellij.psi.PsiFile, List<KaSourceModule>>

    init {
        try {
            val byId = inputs.associateBy { it.id }
            require(byId.size == inputs.size) { "duplicate Kotlin module id" }
            session = buildStandaloneAnalysisAPISession(disposable) {
                registerProjectService(org.jetbrains.kotlin.analysis.api.platform.projectStructure.KotlinCompilerPluginsProvider::class.java, plugins)
                registerProjectExtensionPoint(
                    com.intellij.openapi.extensions.ExtensionPointName.create<com.intellij.psi.PsiTreeChangeListener>("com.intellij.psi.treeChangeListener"),
                    com.intellij.psi.PsiTreeChangeListener::class.java)
                registerProjectService(com.intellij.pom.tree.TreeAspect::class.java, com.intellij.pom.tree.TreeAspect())
                registerProjectService(PomModel::class.java, PomModelImpl(project))
                buildKtModuleProvider {
                    if (refreshArchives) (coreApplicationEnvironment.jarFileSystem as com.intellij.openapi.vfs.impl.jar.CoreJarFileSystem).clearHandlersCache()
                    platform = JvmPlatforms.defaultJvmPlatform
                    val sdks = inputs.map { it.jdk }.distinct().associateWith { home ->
                        addModule(buildKtSdkModule {
                            libraryName = home.toString()
                            platform = JvmPlatforms.defaultJvmPlatform
                            addBinaryRootsFromJdkHome(home, isJre = false)
                        })
                    }
                    val libraries = inputs.flatMap { it.libraries + it.binaryFriends }.distinct().associateWith { path ->
                        // CAS paths have no suffix. Select the archive file system
                        // directly instead of letting Standalone infer it from .jar.
                        val archive = if (Files.isDirectory(path)) null else requireNotNull(
                            coreApplicationEnvironment.jarFileSystem.findFileByPath(
                                path.toAbsolutePath().normalize().toString().replace('\\', '/') + "!/")) {
                            "cannot open JVM library archive: $path"
                        }
                        addModule(buildKtLibraryModule {
                            libraryName = path.toString()
                            platform = JvmPlatforms.defaultJvmPlatform
                            if (archive == null) addBinaryRoot(path) else addBinaryVirtualFile(archive)
                        })
                    }
                    // CoreLocalVirtualFile is read-only. PSI edits belong to this
                    // session's virtual files, never to source or CAS inodes.
                    val generated = mutableMapOf<Path, Path>()
                    inputs.forEach { input -> input.generated.forEach { (path, objectPath) ->
                        val previous = generated.put(path, objectPath)
                        require(previous == null || previous == objectPath) { "conflicting generated source: $path" }
                    } }
                    val sourceFiles = inputs.flatMap { it.sources }.distinct().associateWith { path ->
                        object : LightVirtualFile(path.fileName.toString(),
                            if (path.fileName.toString().endsWith(".java")) com.intellij.ide.highlighter.JavaFileType.INSTANCE
                            else FileTypeRegistry.getInstance().getFileTypeByFileName(path.fileName.toString()),
                            documents[path.toAbsolutePath().normalize()] ?: Files.readString(generated[path] ?: path)) {
                            override fun getPath(): String = path.toAbsolutePath().normalize().toString()
                        }
                    }
                    val visiting = mutableSetOf<String>()
                    fun install(id: String): KaSourceModule {
                        modules[id]?.let { return it }
                        val input = requireNotNull(byId[id]) { "missing Kotlin module: $id" }
                        require(visiting.add(id)) { "cyclic Kotlin module dependency: $id" }
                        val dependencies = input.dependencies.map(::install)
                        val friends = input.friends.map(::install)
                        val options = Options(input.compilerArguments)
                        val module = addModule(buildKtSourceModule {
                            moduleName = options.parsed.moduleName ?: id
                            platform = options.platform
                            languageVersionSettings = options.language
                            addSourceVirtualFiles(input.sources.map { sourceFiles.getValue(it) })
                            addRegularDependency(sdks.getValue(input.jdk))
                            input.libraries.forEach { addRegularDependency(libraries.getValue(it)) }
                            dependencies.forEach { addRegularDependency(it) }
                            friends.forEach { addFriendDependency(it) }
                            input.binaryFriends.forEach { addFriendDependency(libraries.getValue(it)) }
                        })
                        modules[id] = module
                        visiting.remove(id)
                        return module
                    }
                    inputs.forEach { install(it.id) }
                }
            }
            files = session.modulesWithFiles.values.flatten().distinct().groupBy {
                Path.of(it.virtualFile.path).toAbsolutePath().normalize()
            }
            fileOwners = session.modulesWithFiles.flatMap { (module, sources) -> sources.map { it to module } }
                .groupBy({ it.first }, { it.second })
            declarations = Declarations(session)
            val project = session.project as com.intellij.mock.MockProject
            val service = org.jetbrains.kotlin.analysis.api.platform.declarations.KotlinDeclarationProviderFactory::class.java
            project.picoContainer.unregisterComponent(service.name)
            project.registerService(service, declarations)
        } catch (error: Throwable) {
            try { Disposer.dispose(disposable) } finally { plugins.close() }
            throw error
        }
    }

    fun sourceTexts(): Map<Path, String> = files.mapValues { (_, copies) -> copies.single().text }

    // Standalone's single-file Java index caches package/class names. Member
    // edits use normal modification events; name-set changes require new roots.
    fun javaNamesChanged(changed: Map<Path, String>): Boolean {
        fun names(file: com.intellij.psi.PsiJavaFile): Pair<String, Set<String>> {
            fun classes(values: Array<com.intellij.psi.PsiClass>): List<String> = values.flatMap {
                listOfNotNull(it.qualifiedName) + classes(it.innerClasses)
            }
            return file.packageName to classes(file.classes).toSet()
        }
        return changed.any { (path, text) ->
            files[path.toAbsolutePath().normalize()].orEmpty().filterIsInstance<com.intellij.psi.PsiJavaFile>().any { file ->
                file.text != text && names(file) != names(com.intellij.psi.PsiFileFactory.getInstance(session.project)
                    .createFileFromText(file.name, file.fileType, text) as com.intellij.psi.PsiJavaFile)
            }
        }
    }

    /** Text updates keep this module graph and its library modules installed. */
    fun update(changed: Map<Path, String>): SourceChanges {
        val edits = changed.flatMap { (path, text) ->
            val matched = requireNotNull(files[path.toAbsolutePath().normalize()]) {
                "source membership changed; reinstall the module graph: $path"
            }
            matched.filter { it.text != text }.map { it to text }
        }
        if (edits.isEmpty()) return SourceChanges(emptySet(), emptySet())
        val affected = edits.flatMap { (file, _) -> fileOwners.getValue(file) }.toSet()
        val declarationsChanged = opaqueModules.map { modules.getValue(it) }.toMutableSet()
        val application = ApplicationManager.getApplication()
        application.invokeAndWait {
            application.runWriteAction {
                com.intellij.openapi.command.CommandProcessor.getInstance().runUndoTransparentAction {
                    edits.forEach { (file, text) ->
                        val replacement = if (file is org.jetbrains.kotlin.psi.KtFile)
                            KtPsiFactory(session.project).createFile(file.name, text)
                        else com.intellij.psi.PsiFileFactory.getInstance(session.project).createFileFromText(file.name, file.fileType, text)
                        val oldText = file.text
                        val splice = if (file is org.jetbrains.kotlin.psi.KtFile) splice(file, replacement, oldText, text) else null
                        (file.virtualFile as LightVirtualFile).setContent(this, text, false)
                        if (splice != null) {
                            val (old, next) = splice
                            val previous = old
                            val inserted = next.node.clone() as com.intellij.lang.ASTNode
                            old.node.treeParent.replaceChild(old.node, inserted)
                            val installed = inserted.psi
                            val service = KaSourceModificationService.getInstance(session.project)
                            val locality = service.detectLocality(installed, KaElementModificationType.ElementReplaced(previous))
                            service.handleInvalidation(installed, locality)
                            // Inline implementations and constants are consumed
                            // across module boundaries even when local FIR can stay.
                            val exportedBody = generateSequence(installed) { it.parent }.filterIsInstance<KtModifierListOwner>()
                                .any { it.hasModifier(KtTokens.INLINE_KEYWORD) || it.hasModifier(KtTokens.CONST_KEYWORD) }
                            val owners = fileOwners.getValue(file).distinct()
                            // A shared source can belong to several compilations;
                            // the element service invalidates one containing module.
                            if (owners.size > 1) owners.forEach { it.publishModuleOutOfBlockModificationEvent() }
                            if (locality is KaSourceModificationLocality.OutOfBlock || exportedBody || owners.size > 1)
                                declarationsChanged.addAll(owners)
                        } else {
                            file.node.replaceAllChildrenToChildrenOf(replacement.node)
                            declarationsChanged.addAll(fileOwners.getValue(file))
                            fileOwners.getValue(file).forEach { it.publishModuleOutOfBlockModificationEvent() }
                        }
                    }
                }
                edits.forEach { (file, _) ->
                    if (file is org.jetbrains.kotlin.psi.KtFile) declarations.update(file)
                }
                // Registrars have no fine file-read contract. Their module keeps
                // the installed source envelope until those reads are available.
                opaqueModules.forEach { modules.getValue(it).publishModuleOutOfBlockModificationEvent() }
                // Standalone does not deliver the IDE write-finished callback.
                // Finish the compiler's queued in-block invalidations before
                // the next analysis reuses this session and its PSI mappings.
                org.jetbrains.kotlin.analysis.low.level.api.fir.file.structure.LLFirDeclarationModificationService.getInstance(session.project)
                    .flushDeferredModifications()
            }
        }
        return SourceChanges(modules.filterValues { it in affected }.keys + opaqueModules,
            modules.filterValues { it in declarationsChanged }.keys)
    }

    // Locate the syntax subtree spanning the text edit. This only preserves
    // unaffected PSI; the compiler's modification service decides whether its
    // semantic effect is local, including inferred signatures and contracts.
    private fun splice(file: org.jetbrains.kotlin.psi.KtFile, next: com.intellij.psi.PsiFile,
                            before: String, after: String): Pair<PsiElement, PsiElement>? {
        if (before.isEmpty() || after.isEmpty()) return null
        var start = 0
        while (start < minOf(before.length, after.length) && before[start] == after[start]) start++
        var suffix = 0
        while (suffix < minOf(before.length, after.length) - start &&
            before[before.lastIndex - suffix] == after[after.lastIndex - suffix]) suffix++
        val oldEnd = maxOf(start + 1, before.length - suffix).coerceAtMost(before.length)
        val newEnd = maxOf(start + 1, after.length - suffix).coerceAtMost(after.length)
        val oldFirst = file.findElementAt(start.coerceAtMost(before.lastIndex)) ?: return null
        val oldLast = file.findElementAt(oldEnd - 1) ?: return null
        val newFirst = next.findElementAt(start.coerceAtMost(after.lastIndex)) ?: return null
        val newLast = next.findElementAt(newEnd - 1) ?: return null
        val oldCommon = PsiTreeUtil.findCommonParent(oldFirst, oldLast) ?: return null
        val newCommon = PsiTreeUtil.findCommonParent(newFirst, newLast) ?: return null
        val candidates = generateSequence(newCommon) { it.parent }.filterIsInstance<KtElement>().toList()
        for (old in generateSequence(oldCommon) { it.parent }.filterIsInstance<KtElement>()) {
            if (old is org.jetbrains.kotlin.psi.KtFile) break
            val range = old.textRange
            val found = candidates.singleOrNull { it.javaClass == old.javaClass &&
                it.textRange.startOffset == range.startOffset &&
                it.textRange.endOffset == range.endOffset + after.length - before.length }
            if (found != null) return old to found
        }
        return null
    }

    override fun close() {
        try { Disposer.dispose(disposable) } finally { plugins.close() }
    }
}
