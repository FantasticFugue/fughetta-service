plugins {
    kotlin("jvm") version "2.4.21" apply false
}

subprojects {
    providers.gradleProperty("fughetta.buildDirectory").orNull?.let {
        layout.buildDirectory.set(file("$it/${project.name}"))
    }
}
