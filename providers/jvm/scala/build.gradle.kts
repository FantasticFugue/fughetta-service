plugins {
    application
    kotlin("jvm")
}

repositories { mavenCentral() }

val scalaCompiler by configurations.creating
val phaseSource = fileTree("src/main/scala") { include("**/*.scala") }
val phaseClasses = layout.buildDirectory.dir("scala-phase")
val compileScalaPhase by tasks.registering(JavaExec::class) {
    inputs.files(phaseSource).withPathSensitivity(PathSensitivity.RELATIVE)
    outputs.dir(phaseClasses)
    classpath = scalaCompiler
    mainClass.set("scala.tools.nsc.Main")
    javaLauncher.set(javaToolchains.launcherFor {
        languageVersion.set(JavaLanguageVersion.of(providers.gradleProperty("fughetta.javaVersion").get().toInt()))
    })
    doFirst {
        val output = phaseClasses.get().asFile
        output.mkdirs()
        args("-classpath", classpath.asPath, "-d", output.absolutePath)
        args(phaseSource.files.map { it.absolutePath })
    }
}

sourceSets.main {
    output.dir(mapOf("builtBy" to compileScalaPhase), phaseClasses)
}

dependencies {
    compileOnly(project(":fughetta-jvm"))
    testImplementation(project(":fughetta-jvm"))
    scalaCompiler("org.scala-lang:scala-compiler:2.13.16")
    compileOnly(files(phaseClasses).builtBy(compileScalaPhase))
    implementation("org.scala-lang:scala-compiler:2.13.16")
    implementation("org.scala-lang:scala-reflect:2.13.16")
    implementation("org.scala-lang:scala-library:2.13.16")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")
    testImplementation(kotlin("test-junit5"))
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

kotlin {
    jvmToolchain(providers.gradleProperty("fughetta.javaVersion").get().toInt())
}

application { mainClass.set("fughetta.scala.MainKt") }

tasks.jar {
    manifest.attributes(
        "Main-Class" to application.mainClass.get(),
        "Class-Path" to configurations.runtimeClasspath.get().files.joinToString(" ") { it.name },
    )
}

tasks.test {
    useJUnitPlatform()
    val host = project(":fughetta-jvm")
    dependsOn(tasks.installDist, host.tasks.named("installDist"))
    systemProperty("fughetta.host.jar", host.layout.buildDirectory.file("install/fughetta-jvm/lib/fughetta-jvm.jar").get().asFile.absolutePath)
    systemProperty("fughetta.adapter.jar", layout.buildDirectory.file("install/fughetta-scala/lib/fughetta-scala.jar").get().asFile.absolutePath)
    systemProperty("fughetta.scala.library", configurations.runtimeClasspath.get().files.single { it.name == "scala-library-2.13.16.jar" }.absolutePath)
    providers.environmentVariable("FUGHETTA_RECORD_VALIDATOR").orNull?.let { inputs.file(it) }
    val scratch = layout.buildDirectory.dir("test-work")
    doFirst { scratch.get().asFile.mkdirs() }
    systemProperty("java.io.tmpdir", scratch.get().asFile.absolutePath)
}
