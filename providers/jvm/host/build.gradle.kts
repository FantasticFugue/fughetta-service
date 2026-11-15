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
}

kotlin {
    jvmToolchain(25)
}

application { mainClass.set("fughetta.jvm.MainKt") }

tasks.jar {
    manifest.attributes(
        "Main-Class" to application.mainClass.get(),
        "Class-Path" to configurations.runtimeClasspath.get().files.joinToString(" ") { it.name },
    )
}
