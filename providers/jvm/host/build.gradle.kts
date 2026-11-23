plugins {
    application
    kotlin("jvm")
}

repositories {
    mavenCentral()
}

// Adapters load parent-first against this classpath, so they must build against the same versions.
dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")
    compileOnly(fileTree(checkNotNull(gradle.gradleHomeDir).resolve("lib")) {
        include("gradle-tooling-api-*.jar")
        exclude("gradle-tooling-api-provider-*.jar")
    })
}

kotlin {
    jvmToolchain(providers.gradleProperty("fughetta.javaVersion").get().toInt())
}

application { mainClass.set("fughetta.jvm.MainKt") }

tasks.jar {
    manifest.attributes(
        "Main-Class" to application.mainClass.get(),
        "Class-Path" to configurations.runtimeClasspath.get().files.joinToString(" ") { it.name },
    )
}
