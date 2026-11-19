cmake_minimum_required(VERSION 4.5)
project(fughetta-service VERSION 0.1.0 LANGUAGES CXX)

## Language
#
# C++23 modules with the standard library as a module; cmake/toolchain.cmake
# picks clang — the platform's, or the tree named by CLANG_ROOT — and lets
# CMake synthesize std from libc++'s manifest.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # -Wall is off: clang 23 crashes in module AST reading with it on this tree.
  add_compile_options(-Wextra)
endif()

## Options
#
# A subproject builds the layers its parent links, not this test tree.

option(FUGHETTA_BUILD_TESTS "Build the testing tree" ${PROJECT_IS_TOP_LEVEL})
option(FUGHETTA_BUILD_BENCHMARKS "Build benchmarks" OFF)
set(FUGHETTA_INSTANCE "" CACHE STRING "Workspace instance for launchd registration")
if(NOT FUGHETTA_INSTANCE MATCHES "^[A-Za-z0-9_-]*$")
  message(FATAL_ERROR "FUGHETTA_INSTANCE must contain letters, digits, underscores or hyphens")
endif()

## Platform
#
# cmake/platform/<system>.cmake describes the target: its platform units and
# libraries, its I/O backend and its service-manager template. A system without
# a description is not supported.

include("${CMAKE_CURRENT_LIST_DIR}/cmake/platform/${CMAKE_SYSTEM_NAME}.cmake" OPTIONAL RESULT_VARIABLE PLATFORM_DESCRIPTION)
if(NOT PLATFORM_DESCRIPTION)
  message(FATAL_ERROR "No platform description for ${CMAKE_SYSTEM_NAME} under cmake/platform")
endif()

## Dependencies and modules

include(cmake/dependencies.cmake)
include(cmake/providers.cmake)
set(FUGHETTA_SOURCE "${PROJECT_SOURCE_DIR}/src")
include(cmake/modules.cmake)

add_layer(service)
add_modules(service service gateway source worker runtime)
target_link_libraries(service PUBLIC gateway source worker PRIVATE argparse::argparse)
add_executable(fughetta-service src/fughetta-service.cpp)
target_link_libraries(fughetta-service PRIVATE service argparse::argparse)

## Installation
#
# The service executable and its service-manager templates. A consumer such as
# the hosted gateway builds the layers from source as a subproject instead.

if(PROJECT_IS_TOP_LEVEL)
  include(GNUInstallDirs)
  set(FUGHETTA_SERVICE_USER "fughetta" CACHE STRING "Native service account")
  set(FUGHETTA_LIBEXEC "${CMAKE_INSTALL_LIBEXECDIR}/fughetta-service")
  # Service-manager definitions are installed as templates; registration
  # belongs to deployment.
  if(DAEMON_DEFINITION)
    configure_file("cmake/daemon/${DAEMON_DEFINITION}.in" "${DAEMON_DEFINITION}" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${DAEMON_DEFINITION}"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/fughetta-service/daemon" RENAME "${DAEMON_INSTALL_NAME}")
  endif()
  install(TARGETS fughetta-service RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime)
  install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/fughetta-service")
  install(PROGRAMS "${FUGHETTA_JVM_EXPORTER}" DESTINATION "${FUGHETTA_LIBEXEC}" RENAME fughetta-gradle-model)
  install(FILES providers/python/runtime.py DESTINATION "${FUGHETTA_LIBEXEC}/python" COMPONENT python)
  # Each provider installs the directory its artifact sits in.
  foreach(PROVIDER IN LISTS FUGHETTA_PROVIDERS)
    cmake_path(GET FUGHETTA_PROVIDER_${PROVIDER} PARENT_PATH PROVIDER_DIRECTORY)
    install(DIRECTORY "${PROVIDER_DIRECTORY}/" DESTINATION "${FUGHETTA_LIBEXEC}/${PROVIDER}"
      USE_SOURCE_PERMISSIONS COMPONENT "${PROVIDER}")
  endforeach()
endif()

## Tests and benchmarks
#
# Tests and benchmarks have separate build options.

if(FUGHETTA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(FUGHETTA_BUILD_BENCHMARKS)
  add_executable(crossing-bench bench/crossing.cpp)
  target_compile_definitions(crossing-bench PRIVATE ASIO_NO_DEPRECATED ASIO_NO_TS_EXECUTORS)
  target_link_libraries(crossing-bench PRIVATE io asio::asio)
  foreach(BENCHMARK IN LISTS PLATFORM_BENCHMARKS)
    add_executable(${BENCHMARK}-bench bench/${BENCHMARK}.cpp)
    target_link_libraries(${BENCHMARK}-bench PRIVATE source)
  endforeach()
endif()
