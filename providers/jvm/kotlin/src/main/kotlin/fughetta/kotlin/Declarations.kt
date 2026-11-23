package fughetta.kotlin

import com.intellij.psi.search.GlobalSearchScope
import org.jetbrains.kotlin.analysis.api.KaPlatformInterface
import org.jetbrains.kotlin.analysis.api.platform.declarations.KotlinCompositeDeclarationProvider
import org.jetbrains.kotlin.analysis.api.platform.declarations.KotlinDeclarationProviderFactory
import org.jetbrains.kotlin.analysis.api.projectStructure.KaModule
import org.jetbrains.kotlin.analysis.api.standalone.StandaloneAnalysisAPISession
import org.jetbrains.kotlin.analysis.api.standalone.base.declarations.KotlinStandaloneDeclarationProviderFactory
import org.jetbrains.kotlin.psi.KtFile

/** File indexes own PSI references and must be replaced before publishing edits. */
@OptIn(KaPlatformInterface::class)
class Declarations(private val session: StandaloneAnalysisAPISession) : KotlinDeclarationProviderFactory {
    private val builtins = KotlinStandaloneDeclarationProviderFactory(
        session.project, session.coreApplicationEnvironment, emptyList())
    private val files = linkedMapOf<KtFile, KotlinDeclarationProviderFactory>()

    init {
        session.modulesWithFiles.values.flatten().filterIsInstance<KtFile>().forEach(::update)
    }

    fun update(file: KtFile) {
        files[file] = KotlinStandaloneDeclarationProviderFactory(
            session.project, session.coreApplicationEnvironment, listOf(file), skipBuiltins = true)
    }

    override fun createDeclarationProvider(scope: GlobalSearchScope, contextualModule: KaModule?) =
        KotlinCompositeDeclarationProvider.create(buildList {
            add(builtins.createDeclarationProvider(scope, contextualModule))
            files.forEach { (file, index) ->
                if (scope.contains(file.virtualFile)) add(index.createDeclarationProvider(scope, contextualModule))
            }
        })
}
