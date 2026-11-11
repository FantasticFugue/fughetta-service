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

## Options
#
# A subproject builds the layers its parent links, not this test tree.

option(FUGHETTA_BUILD_TESTS "Build the testing tree" ${PROJECT_IS_TOP_LEVEL})
option(FUGHETTA_BUILD_BENCHMARKS "Build benchmarks" OFF)

## Dependencies and modules

include(cmake/dependencies.cmake)
set(FUGHETTA_SOURCE "${PROJECT_SOURCE_DIR}/src")
include(cmake/modules.cmake)

add_layer(service)
add_modules(service service gateway source worker runtime)
target_link_libraries(service PUBLIC gateway source worker PRIVATE argparse::argparse)
add_executable(fughetta-service src/fughetta-service.cpp)
target_link_libraries(fughetta-service PRIVATE service)

## Installation
#
# The service executable and its service-manager templates. A consumer such as
# the hosted gateway builds the layers from source as a subproject instead.

if(PROJECT_IS_TOP_LEVEL)
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
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/fughetta-service")
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
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(engine-bench bench/engine.cpp)
    target_link_libraries(engine-bench PRIVATE source)
  endif()
endif()
