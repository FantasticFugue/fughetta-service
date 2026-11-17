# The managed-language providers under providers/: one .NET build makes the
# fughetta-clr host and the F# plugin, one Gradle build the fughetta-jvm host
# and the Kotlin plugin. Each builds when its tool is found.
#
# FUGHETTA_PROVIDERS lists the providers built and FUGHETTA_PROVIDER_<name>
# names each one's main artifact; the tests and the install rules read them.

set(FUGHETTA_CLR_PROVIDERS "${PROJECT_SOURCE_DIR}/providers/clr")
set(FUGHETTA_CLR_EXPORTER "${FUGHETTA_CLR_PROVIDERS}/exporter/model.fsx")
set(FUGHETTA_JVM_PROVIDERS "${PROJECT_SOURCE_DIR}/providers/jvm")
set(FUGHETTA_JVM_EXPORTER "${FUGHETTA_JVM_PROVIDERS}/exporter/model.gradle")
set(FUGHETTA_PROVIDERS)

## .NET
#
# The SDK must be 11.0 or later, whose F# compiler names CLI events with E:
# documentation IDs, and the 10.0 reference pack must be installed beside it. Building the F# plugin builds the
# fughetta-clr host it references.

find_program(DOTNET_TOOL dotnet)
if(DOTNET_TOOL)
  execute_process(COMMAND "${DOTNET_TOOL}" --version
    WORKING_DIRECTORY "${FUGHETTA_CLR_PROVIDERS}"
    OUTPUT_VARIABLE DOTNET_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE DOTNET_VERSION_STATUS ERROR_VARIABLE DOTNET_VERSION_ERROR)
  if(NOT DOTNET_VERSION_STATUS EQUAL 0 OR NOT DOTNET_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+" OR DOTNET_VERSION VERSION_LESS 11.0)
    message(FATAL_ERROR "clr-providers require .NET SDK 11.0 or later; ${DOTNET_TOOL} reported '${DOTNET_VERSION}': ${DOTNET_VERSION_ERROR}")
  endif()
  set(CLR_OUTPUT "${PROJECT_BINARY_DIR}/clr")
  set(FUGHETTA_PROVIDER_clr "${CLR_OUTPUT}/fughetta-clr/fughetta-clr.dll")
  set(FUGHETTA_PROVIDER_fsharp "${CLR_OUTPUT}/fughetta-fsharp/fughetta-fsharp.dll")
  list(APPEND FUGHETTA_PROVIDERS clr fsharp)
  file(MAKE_DIRECTORY "${CLR_OUTPUT}/tmp")
  file(GLOB_RECURSE CLR_PROVIDER_SOURCES CONFIGURE_DEPENDS "${FUGHETTA_CLR_PROVIDERS}/host/*" "${FUGHETTA_CLR_PROVIDERS}/fsharp/*")
  add_custom_command(OUTPUT "${CLR_OUTPUT}/built.stamp"
    BYPRODUCTS "${FUGHETTA_PROVIDER_clr}" "${FUGHETTA_PROVIDER_fsharp}"
    COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${CLR_OUTPUT}/tmp"
      "${DOTNET_TOOL}" build "${FUGHETTA_CLR_PROVIDERS}/fsharp/fughetta-fsharp.fsproj"
      --nologo "-p:FughettaOutput=${CLR_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CLR_OUTPUT}/built.stamp"
    DEPENDS ${CLR_PROVIDER_SOURCES} "${FUGHETTA_CLR_PROVIDERS}/Directory.Build.props" "${DOTNET_TOOL}"
    VERBATIM)
  add_custom_target(clr-providers ALL DEPENDS "${CLR_OUTPUT}/built.stamp")
endif()

## Gradle
#
# JVM 25 toolchains require Gradle 9.1.
#
# Gradle unpacks native services into a lock file beside the libraries it loads,
# and extracts a distribution jar's natives again whenever their lock is gone;
# the Kotlin daemon keeps its run files under ~/.local/share. Both writes fail
# when the user's own directories belong to someone else, so the build keeps
# its home, daemon run files and temporary directory. FUGHETTA_GRADLE runs a
# task of the providers' build that way; the tests run theirs with it too.

find_program(GRADLE_TOOL gradle)
if(GRADLE_TOOL)
  set(JVM_OUTPUT "${PROJECT_BINARY_DIR}/jvm")
  set(GRADLE_ENV
    "JAVA_TOOL_OPTIONS=-Djava.io.tmpdir=${JVM_OUTPUT}/tmp -Dkotlin.daemon.options=runFilesPath=${JVM_OUTPUT}/daemon"
    "GRADLE_USER_HOME=${JVM_OUTPUT}/home")
  file(MAKE_DIRECTORY "${JVM_OUTPUT}/tmp" "${JVM_OUTPUT}/home" "${JVM_OUTPUT}/daemon")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${GRADLE_ENV} "${GRADLE_TOOL}" --version
    OUTPUT_VARIABLE GRADLE_VERSION_OUTPUT RESULT_VARIABLE GRADLE_VERSION_STATUS ERROR_VARIABLE GRADLE_VERSION_ERROR)
  string(REGEX MATCH "Gradle ([0-9]+\\.[0-9]+(\\.[0-9]+)?)" GRADLE_VERSION_MATCH "${GRADLE_VERSION_OUTPUT}")
  set(GRADLE_VERSION "${CMAKE_MATCH_1}")
  if(NOT GRADLE_VERSION_STATUS EQUAL 0 OR NOT GRADLE_VERSION_MATCH OR GRADLE_VERSION VERSION_LESS 9.1)
    message(FATAL_ERROR "jvm-providers require Gradle 9.1 or later for JVM 25; set GRADLE_TOOL to that executable. ${GRADLE_TOOL} reported '${GRADLE_VERSION}': ${GRADLE_VERSION_ERROR}")
  endif()
  set(FUGHETTA_GRADLE "${CMAKE_COMMAND}" -E env ${GRADLE_ENV}
    "${GRADLE_TOOL}" --no-daemon --console=plain
    --project-cache-dir "${JVM_OUTPUT}/gradle"
    "-Pkotlin.project.persistent.dir=${JVM_OUTPUT}/persistent"
    "-Pfughetta.buildDirectory=${JVM_OUTPUT}/output"
    -p "${FUGHETTA_JVM_PROVIDERS}")
  set(FUGHETTA_PROVIDER_jvm "${JVM_OUTPUT}/output/fughetta-jvm/install/fughetta-jvm/lib/fughetta-jvm.jar")
  set(FUGHETTA_PROVIDER_kotlin "${JVM_OUTPUT}/output/fughetta-kotlin/install/fughetta-kotlin/lib/fughetta-kotlin.jar")
  list(APPEND FUGHETTA_PROVIDERS jvm kotlin)
  file(GLOB_RECURSE JVM_PROVIDER_SOURCES CONFIGURE_DEPENDS "${FUGHETTA_JVM_PROVIDERS}/host/*" "${FUGHETTA_JVM_PROVIDERS}/kotlin/*")
  add_custom_command(OUTPUT "${JVM_OUTPUT}/built.stamp"
    BYPRODUCTS "${FUGHETTA_PROVIDER_jvm}" "${FUGHETTA_PROVIDER_kotlin}"
    COMMAND ${FUGHETTA_GRADLE} installDist
    COMMAND "${CMAKE_COMMAND}" -E touch "${JVM_OUTPUT}/built.stamp"
    DEPENDS ${JVM_PROVIDER_SOURCES} "${FUGHETTA_JVM_PROVIDERS}/build.gradle.kts" "${FUGHETTA_JVM_PROVIDERS}/settings.gradle.kts" "${GRADLE_TOOL}"
    VERBATIM)
  add_custom_target(jvm-providers ALL DEPENDS "${JVM_OUTPUT}/built.stamp")
endif()

## LLVM
#
# fughetta-native links LLVM's static libraries. It is a project of its own so
# it builds against the libstdc++ or libc++ the LLVM package was built with,
# not this build's toolchain file and flags. Only that project loads LLVM's
# package. The LLVM beside the compiler is found first; LLVM_DIR names another.

find_path(LLVM_DIR LLVMConfig.cmake HINTS "${CLANG_PREFIX}/lib/cmake/llvm" PATH_SUFFIXES lib/cmake/llvm)
if(LLVM_DIR)
  set(NATIVE_OUTPUT "${PROJECT_BINARY_DIR}/native")
  set(FUGHETTA_PROVIDER_native "${NATIVE_OUTPUT}/bin/fughetta-native${CMAKE_EXECUTABLE_SUFFIX}")
  list(APPEND FUGHETTA_PROVIDERS native)
  add_custom_command(OUTPUT "${FUGHETTA_PROVIDER_native}"
    COMMAND "${CMAKE_COMMAND}" -S "${PROJECT_SOURCE_DIR}/providers/native/host" -B "${NATIVE_OUTPUT}/build"
      -G "${CMAKE_GENERATOR}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}" -DCMAKE_BUILD_TYPE=Release
      "-DLLVM_DIR=${LLVM_DIR}" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${NATIVE_OUTPUT}/bin"
    COMMAND "${CMAKE_COMMAND}" --build "${NATIVE_OUTPUT}/build"
    DEPENDS "${PROJECT_SOURCE_DIR}/providers/native/host/CMakeLists.txt" "${PROJECT_SOURCE_DIR}/providers/native/host/fughetta-native.cpp"
    VERBATIM)
  add_custom_target(native-provider ALL DEPENDS "${FUGHETTA_PROVIDER_native}")
endif()
