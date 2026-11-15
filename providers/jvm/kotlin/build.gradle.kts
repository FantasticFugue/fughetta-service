plugins {
    application
    kotlin("jvm")
}

repositories {
    mavenCentral()
    maven("https://www.jetbrains.com/intellij-repository/releases")
    maven("https://packages.jetbrains.team/maven/p/ij/intellij-dependencies")
}

dependencies {
    // The fughetta-jvm host provides the adapter interface at run time.
    compileOnly(project(":fughetta-jvm"))
    testImplementation(project(":fughetta-jvm"))
    implementation("org.jetbrains.kotlinx:kotlinx-collections-immutable-jvm:0.5.1")
    implementation("com.github.ben-manes.caffeine:caffeine:2.9.3")
    testImplementation(kotlin("test-junit5"))
    testRuntimeOnly("org.jetbrains.kotlin:kotlin-allopen-compiler-plugin:2.4.21")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
    implementation("org.jetbrains.kotlin:analysis-api-standalone-for-ide:2.4.21@jar")
    implementation("org.jetbrains.kotlin:analysis-api-for-ide:2.4.21@jar")
    implementation("org.jetbrains.kotlin:analysis-api-impl-base-for-ide:2.4.21@jar")
    implementation("org.jetbrains.kotlin:analysis-api-k2-for-ide:2.4.21@jar")
    implementation("org.jetbrains.kotlin:analysis-api-platform-interface-for-ide:2.4.21@jar")
    implementation("org.jetbrains.kotlin:low-level-api-fir-for-ide:2.4.21@jar")
    implementation("org.jetbrains.kotlin:symbol-light-classes-for-ide:2.4.21@jar")
    implementation("org.jetbrains.kotlin:kotlin-compiler-common-for-ide:2.4.21@jar")
    implementation("org.jetbrains.kotlin:kotlin-compiler-ir-for-ide:2.4.21@jar")
    implementation("org.jetbrains.kotlin:kotlin-compiler-fir-for-ide:2.4.21@jar")
    implementation("org.jetbrains.kotlin:kotlin-compiler-fe10-for-ide:2.4.21@jar")
    listOf("util-rt", "util-class-loader", "util-xml-dom", "util-base", "util", "core", "extensions").forEach {
        implementation("com.jetbrains.intellij.platform:$it:251.27812.49")
    }
    implementation("com.jetbrains.intellij.java:java-psi:251.27812.49")
    implementation("com.jetbrains.intellij.java:java-frontback-psi:251.27812.49")
    implementation("com.jetbrains.intellij.platform:core-impl:251.27812.49")
    implementation("com.jetbrains.intellij.java:java-psi-impl:251.27812.49")
    implementation("com.jetbrains.intellij.java:java-frontback-psi-impl:251.27812.49")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")
}

kotlin {
    jvmToolchain(25)
    compilerOptions {
        optIn.add("org.jetbrains.kotlin.analysis.api.KaExperimentalApi")
        optIn.add("org.jetbrains.kotlin.analysis.api.KaImplementationDetail")
    }
}


tasks.test {
    useJUnitPlatform()
    systemProperty("fughetta.test.allopen", configurations.testRuntimeClasspath.get().files.single { it.name.startsWith("kotlin-allopen-compiler-plugin-") }.absolutePath)
    val host = project(":fughetta-jvm")
    dependsOn(tasks.installDist, host.tasks.named("installDist"))
    systemProperty("fughetta.host.jar", host.layout.buildDirectory.file("install/fughetta-jvm/lib/fughetta-jvm.jar").get().asFile.absolutePath)
    systemProperty("fughetta.adapter.jar", layout.buildDirectory.file("install/fughetta-kotlin/lib/fughetta-kotlin.jar").get().asFile.absolutePath)
    listOf("FUGHETTA_RECORD_VALIDATOR", "FUGHETTA_WORKER_TEST").forEach { name ->
        providers.environmentVariable(name).orNull?.let { inputs.file(it) }
    }
    val scratch = layout.buildDirectory.dir("test-work")
    doFirst { scratch.get().asFile.mkdirs() }
    systemProperty("java.io.tmpdir", scratch.get().asFile.absolutePath)
}

application { mainClass.set("fughetta.kotlin.MainKt") }

tasks.jar {
    manifest.attributes(
        "Main-Class" to application.mainClass.get(),
        "Class-Path" to configurations.runtimeClasspath.get().files.joinToString(" ") { it.name },
    )
}
