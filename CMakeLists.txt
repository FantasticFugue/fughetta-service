cmake_minimum_required(VERSION 4.0)
project(fughetta-service VERSION 0.1.0 LANGUAGES CXX)

## Language
#
# C++23 modules with the standard library as a module; the compiler comes from
# the xclang tree that cmake/toolchain.cmake selects.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra)
endif()

set(SANITIZERS "" CACHE STRING "Comma-separated sanitizers")
if(SANITIZERS)
  add_compile_options("-fsanitize=${SANITIZERS}")
  add_link_options("-fsanitize=${SANITIZERS}")
  if(CMAKE_CXX_COMPILER_TARGET MATCHES "windows-msvc$" AND SANITIZERS MATCHES "address")
    # xclang's /MT build imports UCRT by default. ASan's static CRT thunk
    # owns the heap entry points, so link UCRT statically for these builds.
    add_link_options("LINKER:/nodefaultlib:ucrt.lib")
    link_libraries(libucrt.lib)
  endif()
endif()

if(WIN32)
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  add_link_options("LINKER:/STACK:8388608")
endif()

## Options

include(CTest)
option(BUILD_BENCHMARKS "Build benchmarks" OFF)

## Dependencies and modules

include(cmake/dependencies.cmake)
set(FUGHETTA_SOURCE "${PROJECT_SOURCE_DIR}/src")
include(cmake/modules.cmake)

add_layer(service)
add_modules(service service gateway source worker runtime)
target_link_libraries(service PUBLIC gateway source worker PRIVATE argparse::argparse)
add_executable(fughetta-service src/fughetta-service.cpp)
target_link_libraries(fughetta-service PRIVATE service)

## Package
#
# The modules are installed as sources with the same module registration, so a
# consumer (the hosted gateway) compiles them with its own compiler: module BMIs
# are not portable. The services and the test support are not part of it.

include(GNUInstallDirs)
set(FUGHETTA_INSTANCE "" CACHE STRING "Workspace instance for launchd registration")
set(FUGHETTA_SERVICE_USER "fughetta" CACHE STRING "Native service account")
if(FUGHETTA_INSTANCE AND NOT FUGHETTA_INSTANCE MATCHES "^[A-Za-z0-9_-]+$")
  message(FATAL_ERROR "FUGHETTA_INSTANCE must contain letters, digits, underscores or hyphens")
endif()
# Service-manager definitions are installed as templates; registration belongs to deployment.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(DAEMON_DEFINITION fughetta-service.service)
elseif(APPLE AND FUGHETTA_INSTANCE)
  set(DAEMON_DEFINITION dev.fughetta.service.plist)
endif()
if(DAEMON_DEFINITION)
  configure_file("cmake/daemon/${DAEMON_DEFINITION}.in" "${DAEMON_DEFINITION}" @ONLY)
  if(APPLE)
    set(DAEMON_INSTALL_NAME "dev.fughetta.service.${FUGHETTA_INSTANCE}.plist")
  else()
    set(DAEMON_INSTALL_NAME "fughetta-service@.service")
  endif()
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${DAEMON_DEFINITION}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/fughetta-service/daemon" RENAME "${DAEMON_INSTALL_NAME}")
endif()
install(TARGETS fughetta-service RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
include(CMakePackageConfigHelpers)
install(DIRECTORY src/ DESTINATION "${CMAKE_INSTALL_DATADIR}/fughetta-service/src"
  FILES_MATCHING PATTERN "*.cppm" PATTERN "testing" EXCLUDE PATTERN "service" EXCLUDE)
install(FILES cmake/dependencies.cmake cmake/modules.cmake DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fughetta-service")
configure_package_config_file(cmake/fughetta-service-config.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/fughetta-service-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fughetta-service")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/fughetta-service-config-version.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fughetta-service-config.cmake" "${CMAKE_CURRENT_BINARY_DIR}/fughetta-service-config-version.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/fughetta-service")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/fughetta-service")

## Tests and benchmarks
#
# Tests and benchmarks have separate build options.

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(BUILD_BENCHMARKS)
  add_executable(crossing-bench bench/crossing.cpp)
  target_compile_definitions(crossing-bench PRIVATE ASIO_NO_DEPRECATED ASIO_NO_TS_EXECUTORS)
  target_link_libraries(crossing-bench PRIVATE io asio::asio)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(engine-bench bench/engine.cpp)
    target_link_libraries(engine-bench PRIVATE source)
  endif()
endif()
