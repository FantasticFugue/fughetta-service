# The managed-language providers under providers/: one .NET build makes the
# fughetta-clr host and the F# and C# plugins, one Gradle build the fughetta-jvm host
# and the Kotlin and Scala plugins. Each builds when its tool is found.
#
# FUGHETTA_PROVIDERS lists the providers built and FUGHETTA_PROVIDER_<name>
# names each one's main artifact; the tests and the install rules read them.

set(FUGHETTA_CLR_PROVIDERS "${PROJECT_SOURCE_DIR}/providers/clr")
set(FUGHETTA_JVM_PROVIDERS "${PROJECT_SOURCE_DIR}/providers/jvm")
set(FUGHETTA_JVM_EXPORTER "${FUGHETTA_JVM_PROVIDERS}/exporter/model.gradle")
set(FUGHETTA_PROVIDERS)

## .NET
#
# Install the selected target framework's reference pack beside the build SDK.
# Building the F# plugin also builds the fughetta-clr host it references.

find_program(DOTNET_TOOL dotnet)
if(DOTNET_TOOL)
  set(CLR_SETTINGS "${FUGHETTA_CLR_PROVIDERS}/Directory.Build.props")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CLR_SETTINGS}")
  file(STRINGS "${CLR_SETTINGS}" CLR_SDK_SETTING REGEX "<FughettaMinimumSdkVersion>")
  string(REGEX MATCH "<FughettaMinimumSdkVersion>([0-9]+\\.[0-9]+)</FughettaMinimumSdkVersion>" CLR_SDK_MATCH "${CLR_SDK_SETTING}")
  if(NOT CLR_SDK_MATCH)
    message(FATAL_ERROR "Set FughettaMinimumSdkVersion in ${CLR_SETTINGS}")
  endif()
  set(CLR_MINIMUM_SDK "${CMAKE_MATCH_1}")
  execute_process(COMMAND "${DOTNET_TOOL}" --version
    WORKING_DIRECTORY "${FUGHETTA_CLR_PROVIDERS}"
    OUTPUT_VARIABLE DOTNET_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE DOTNET_VERSION_STATUS ERROR_VARIABLE DOTNET_VERSION_ERROR)
  if(NOT DOTNET_VERSION_STATUS EQUAL 0 OR NOT DOTNET_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+" OR DOTNET_VERSION VERSION_LESS CLR_MINIMUM_SDK)
    message(FATAL_ERROR "clr-providers require .NET SDK ${CLR_MINIMUM_SDK} or later; ${DOTNET_TOOL} reported '${DOTNET_VERSION}': ${DOTNET_VERSION_ERROR}")
  endif()
  set(CLR_OUTPUT "${PROJECT_BINARY_DIR}/clr")
  set(FUGHETTA_PROVIDER_clr "${CLR_OUTPUT}/fughetta-clr/fughetta-clr.dll")
  set(FUGHETTA_PROVIDER_fsharp "${CLR_OUTPUT}/fughetta-fsharp/fughetta-fsharp.dll")
  set(FUGHETTA_PROVIDER_csharp "${CLR_OUTPUT}/fughetta-csharp/fughetta-csharp.dll")
  set(FUGHETTA_CLR_EXPORTER "${CLR_OUTPUT}/fughetta-msbuild-model/fughetta-msbuild-model.dll")
  set(FUGHETTA_PROVIDER_msbuild "${FUGHETTA_CLR_EXPORTER}")
  list(APPEND FUGHETTA_PROVIDERS clr fsharp csharp msbuild)
  file(MAKE_DIRECTORY "${CLR_OUTPUT}/tmp")
  file(GLOB_RECURSE CLR_PROVIDER_SOURCES CONFIGURE_DEPENDS
    "${FUGHETTA_CLR_PROVIDERS}/host/*" "${FUGHETTA_CLR_PROVIDERS}/fsharp/*"
    "${FUGHETTA_CLR_PROVIDERS}/csharp/*" "${FUGHETTA_CLR_PROVIDERS}/exporter/*")
  add_custom_command(OUTPUT "${CLR_OUTPUT}/built.stamp"
    BYPRODUCTS "${FUGHETTA_PROVIDER_clr}" "${FUGHETTA_PROVIDER_fsharp}" "${FUGHETTA_PROVIDER_csharp}" "${FUGHETTA_CLR_EXPORTER}"
    COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${CLR_OUTPUT}/tmp"
      "${DOTNET_TOOL}" build "${FUGHETTA_CLR_PROVIDERS}/fsharp/fughetta-fsharp.fsproj"
      --nologo "-p:FughettaOutput=${CLR_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${CLR_OUTPUT}/tmp"
      "${DOTNET_TOOL}" build "${FUGHETTA_CLR_PROVIDERS}/csharp/fughetta-csharp.csproj"
      --nologo "-p:FughettaOutput=${CLR_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${CLR_OUTPUT}/tmp"
      "${DOTNET_TOOL}" build "${FUGHETTA_CLR_PROVIDERS}/exporter/fughetta-msbuild-model.fsproj"
      --nologo "-p:FughettaOutput=${CLR_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CLR_OUTPUT}/built.stamp"
    DEPENDS ${CLR_PROVIDER_SOURCES} "${CLR_SETTINGS}" "${DOTNET_TOOL}"
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

find_program(GRADLE_TOOL NAMES ${GRADLE_LAUNCHERS} NAMES_PER_DIR)
if(GRADLE_TOOL)
  cmake_path(NATIVE_PATH GRADLE_TOOL GRADLE_NATIVE_TOOL)
  set(GRADLE_COMMAND "${GRADLE_NATIVE_TOOL}")
  cmake_path(GET GRADLE_TOOL EXTENSION LAST_ONLY GRADLE_SUFFIX)
  string(TOLOWER "${GRADLE_SUFFIX}" GRADLE_SUFFIX)
  if(GRADLE_BATCH_PREFIX AND GRADLE_SUFFIX MATCHES "^\\.(bat|cmd)$")
    list(PREPEND GRADLE_COMMAND ${GRADLE_BATCH_PREFIX})
  endif()
  set(JVM_OUTPUT "${PROJECT_BINARY_DIR}/jvm")
  set(FUGHETTA_GRADLE_USER_HOME "${JVM_OUTPUT}/home" CACHE PATH "Gradle user home for the provider build")
  set(GRADLE_ENV
    "JAVA_TOOL_OPTIONS=-Djava.io.tmpdir=${JVM_OUTPUT}/tmp -Dkotlin.daemon.options=runFilesPath=${JVM_OUTPUT}/daemon"
    "GRADLE_USER_HOME=${FUGHETTA_GRADLE_USER_HOME}")
  file(MAKE_DIRECTORY "${JVM_OUTPUT}/tmp" "${FUGHETTA_GRADLE_USER_HOME}" "${JVM_OUTPUT}/daemon")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${GRADLE_ENV} ${GRADLE_COMMAND} --version
    OUTPUT_VARIABLE GRADLE_VERSION_OUTPUT RESULT_VARIABLE GRADLE_VERSION_STATUS ERROR_VARIABLE GRADLE_VERSION_ERROR)
  string(REGEX MATCH "Gradle ([0-9]+\\.[0-9]+(\\.[0-9]+)?)" GRADLE_VERSION_MATCH "${GRADLE_VERSION_OUTPUT}")
  set(GRADLE_VERSION "${CMAKE_MATCH_1}")
  if(NOT GRADLE_VERSION_STATUS EQUAL 0 OR NOT GRADLE_VERSION_MATCH OR GRADLE_VERSION VERSION_LESS 9.1)
    message(FATAL_ERROR "jvm-providers require Gradle 9.1 or later for JVM 25; set GRADLE_TOOL to that executable. ${GRADLE_TOOL} reported '${GRADLE_VERSION}': ${GRADLE_VERSION_ERROR}")
  endif()
  set(FUGHETTA_GRADLE "${CMAKE_COMMAND}" -E env ${GRADLE_ENV}
    ${GRADLE_COMMAND} --no-daemon --console=plain
    --project-cache-dir "${JVM_OUTPUT}/gradle"
    "-Pkotlin.project.persistent.dir=${JVM_OUTPUT}/persistent"
    "-Pfughetta.buildDirectory=${JVM_OUTPUT}/output"
    -p "${FUGHETTA_JVM_PROVIDERS}")
  set(FUGHETTA_PROVIDER_jvm "${JVM_OUTPUT}/output/fughetta-jvm/install/fughetta-jvm/lib/fughetta-jvm.jar")
  set(FUGHETTA_PROVIDER_kotlin "${JVM_OUTPUT}/output/fughetta-kotlin/install/fughetta-kotlin/lib/fughetta-kotlin.jar")
  set(FUGHETTA_PROVIDER_scala "${JVM_OUTPUT}/output/fughetta-scala/install/fughetta-scala/lib/fughetta-scala.jar")
  list(APPEND FUGHETTA_PROVIDERS jvm kotlin scala)
  file(GLOB_RECURSE JVM_PROVIDER_SOURCES CONFIGURE_DEPENDS
    "${FUGHETTA_JVM_PROVIDERS}/host/*" "${FUGHETTA_JVM_PROVIDERS}/kotlin/*" "${FUGHETTA_JVM_PROVIDERS}/scala/*")
  add_custom_command(OUTPUT "${JVM_OUTPUT}/built.stamp"
    BYPRODUCTS "${FUGHETTA_PROVIDER_jvm}" "${FUGHETTA_PROVIDER_kotlin}" "${FUGHETTA_PROVIDER_scala}"
    COMMAND ${FUGHETTA_GRADLE} installDist
    COMMAND "${CMAKE_COMMAND}" -E touch "${JVM_OUTPUT}/built.stamp"
    DEPENDS ${JVM_PROVIDER_SOURCES} "${FUGHETTA_JVM_PROVIDERS}/build.gradle.kts" "${FUGHETTA_JVM_PROVIDERS}/settings.gradle.kts"
      "${FUGHETTA_JVM_PROVIDERS}/gradle.properties" "${GRADLE_TOOL}"
    VERBATIM)
  add_custom_target(jvm-providers ALL DEPENDS "${JVM_OUTPUT}/built.stamp")
endif()

## LLVM
#
# fughetta-llvm links LLVM's static libraries. It is a project of its own,
# compiled by the package's own driver, so it builds against the standard
# library and runtime the package was built with (libstdc++ on Linux, the MSVC
# STL and static CRT on Windows), not this build's toolchain file and flags.
# Only that project loads LLVM's package. The LLVM beside the compiler is found
# first; LLVM_DIR names another.

find_path(LLVM_DIR LLVMConfig.cmake HINTS "${CLANG_PREFIX}/lib/cmake/llvm" PATH_SUFFIXES lib/cmake/llvm)
if(LLVM_DIR)
  cmake_path(SET LLVM_PREFIX NORMALIZE "${LLVM_DIR}/../../..")
  find_program(LLVM_CXX_COMPILER NAMES ${CLANG_DRIVERS_${CMAKE_HOST_SYSTEM_NAME}} HINTS "${LLVM_PREFIX}/bin" NO_DEFAULT_PATH NAMES_PER_DIR REQUIRED)
  cmake_path(GET LLVM_CXX_COMPILER FILENAME LLVM_C_DRIVER)
  string(REPLACE "clang++" "clang" LLVM_C_DRIVER "${LLVM_C_DRIVER}")
  find_program(LLVM_C_COMPILER NAMES "${LLVM_C_DRIVER}" HINTS "${LLVM_PREFIX}/bin" NO_DEFAULT_PATH REQUIRED)
  set(LLVM_HOST_OPTIONS "-DCMAKE_C_COMPILER=${LLVM_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${LLVM_CXX_COMPILER}")
  if(WIN32)
    list(APPEND LLVM_HOST_OPTIONS "-DCMAKE_PREFIX_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
  endif()
  set(LLVM_OUTPUT "${PROJECT_BINARY_DIR}/llvm")
  set(FUGHETTA_PROVIDER_llvm "${LLVM_OUTPUT}/bin/fughetta-llvm${CMAKE_EXECUTABLE_SUFFIX}")
  list(APPEND FUGHETTA_PROVIDERS llvm)
  add_custom_command(OUTPUT "${FUGHETTA_PROVIDER_llvm}"
    COMMAND "${CMAKE_COMMAND}" -S "${PROJECT_SOURCE_DIR}/providers/llvm/host" -B "${LLVM_OUTPUT}/build"
      -G "${CMAKE_GENERATOR}" ${LLVM_HOST_OPTIONS} -DCMAKE_BUILD_TYPE=Release
      "-DLLVM_DIR=${LLVM_DIR}" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${LLVM_OUTPUT}/bin"
    COMMAND "${CMAKE_COMMAND}" --build "${LLVM_OUTPUT}/build"
    DEPENDS "${PROJECT_SOURCE_DIR}/providers/llvm/host/CMakeLists.txt" "${PROJECT_SOURCE_DIR}/providers/llvm/host/fughetta-llvm.cpp"
    VERBATIM)
  add_custom_target(llvm-provider ALL DEPENDS "${FUGHETTA_PROVIDER_llvm}")
endif()
