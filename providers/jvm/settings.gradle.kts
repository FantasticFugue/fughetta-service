pluginManagement {
    repositories {
        mavenCentral()
        gradlePluginPortal()
    }
}

rootProject.name = "fughetta-jvm-providers"

// The platform host and each language adapter are separate release artifacts.
include("fughetta-jvm", "fughetta-kotlin")
project(":fughetta-jvm").projectDir = file("host")
project(":fughetta-kotlin").projectDir = file("kotlin")
