rootProject.name = "fughetta-jvm-providers"

// The platform host and each language adapter are separate release artifacts.
include("fughetta-jvm", "fughetta-kotlin", "fughetta-scala")
project(":fughetta-jvm").projectDir = file("host")
project(":fughetta-kotlin").projectDir = file("kotlin")
project(":fughetta-scala").projectDir = file("scala")
