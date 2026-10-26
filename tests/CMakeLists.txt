# Test registration. Included from the root CMakeLists.txt with
# add_subdirectory(tests) when BUILD_TESTING is on; every target here is built
# only for that.
#
# A suite is `add_suite(<name> <source under tests/> <targets it links>)`; it
# produces `<name>-test`, registered with the same name. Sources that need a
# script driver are registered directly with add_test.

# The scripted drivers look for their helpers beside the services, so test
# binaries keep the flat layout the root build has.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(ut CONFIG REQUIRED)

add_module(boost_ut "${PROJECT_SOURCE_DIR}/src/testing/ut.cppm")
target_link_libraries(boost_ut PUBLIC Boost::ut)

add_module(support "${PROJECT_SOURCE_DIR}/src/testing/support.cppm")
add_implementation(support "${PROJECT_SOURCE_DIR}/src/testing/service.cppm")
target_link_libraries(support PUBLIC native protocol PRIVATE server)

add_module(fixture "${PROJECT_SOURCE_DIR}/src/testing/fixture.cppm")
target_link_libraries(fixture PUBLIC configuration reflection)

add_module(launch "${PROJECT_SOURCE_DIR}/src/testing/launch.cppm")
target_link_libraries(launch PUBLIC support native coroutine http reflection configuration PRIVATE keyring usage)

function(add_suite NAME SOURCE)
  add_executable(${NAME}-test "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
  target_link_libraries(${NAME}-test PRIVATE boost_ut ${ARGN})
  add_test(NAME ${NAME} COMMAND ${NAME}-test)
  set_tests_properties(${NAME} PROPERTIES TIMEOUT 120)
endfunction()

## Suites

add_suite(allocation model/allocation.cpp allocation)
add_suite(control model/control.cpp allocation)
add_suite(async core/async.cpp async native support)
add_suite(boundary io/boundary.cpp coroutine http utility native)
add_suite(cancel gateway/cancel.cpp coroutine native tools front world mcp)
add_suite(build model/build.cpp commands language configuration reflection support fixture)
add_suite(config gateway/config.cpp coroutine native configuration usage registry front support fixture)
add_suite(identity gateway/identity.cpp configuration keyring)
add_suite(enrollment gateway/enrollment.cpp enrollment durability support)
add_suite(content model/content.cpp content protocol)
add_suite(durability source/durability.cpp coroutine native content durability protocol configuration usage storage support)
add_suite(edge io/edge.cpp coroutine native http protocol)
add_suite(websocket io/websocket.cpp websocket native protocol)
add_suite(resolver io/resolver.cpp resolver native tls)
add_suite(environment platform/environment.cpp environment support)
add_suite(foundation core/foundation.cpp coroutine native)
add_suite(mpmc core/mpmc.cpp mpmc)
add_suite(fleet gateway/fleet.cpp fleet native support)
add_suite(tools gateway/tools.cpp tools fleet executor server host support fixture content package reflection language commands)
add_suite(backlog gateway/backlog.cpp coroutine native configuration registry fleet tools world support)
add_suite(dispatch gateway/dispatch.cpp tools fleet executor host support fixture)
add_suite(front gateway/front.cpp fixture commands coroutine native http keyring configuration registry tools front support)
add_suite(generation gateway/generation.cpp templates)
add_suite(host platform/host.cpp host)
set_property(TEST host APPEND PROPERTY ENVIRONMENT "HOST_TEST_VALUE=left=right")
add_suite(settings gateway/settings.cpp settings fixture commands coroutine native http keyring configuration registry tools front support)
add_suite(meta gateway/meta.cpp coroutine native meta protocol mcp configuration registry tools reflection support)
add_suite(node source/node.cpp storage engine archive codec index kernel content support unofficial::sqlite3::sqlite3)
add_suite(storage source/storage.cpp content storage support)
add_suite(executor worker/executor.cpp executor replica archive coroutine native support)
add_suite(preparation source/preparation.cpp preparation language commands coroutine native support reflection fixture content nodes)
add_suite(lsp source/lsp.cpp lsp coroutine native semantic support reflection)
file(GLOB CLICE_HINTS "$ENV{HOME}/.cache/clice/*/clice/bin")
find_program(CLICE_TOOL clice HINTS ${CLICE_HINTS})
if(CLICE_TOOL)
  set_property(TEST lsp APPEND PROPERTY ENVIRONMENT "TEST_CLICE=${CLICE_TOOL}")
endif()
add_suite(server source/server.cpp coroutine native protocol configuration registry usage server tools mcp support)

add_suite(analysis system/analysis.cpp launch client language fixture content nodes semantic)
if(CLICE_TOOL)
  set_property(TEST analysis APPEND PROPERTY ENVIRONMENT "TEST_CLICE=${CLICE_TOOL}")
endif()
add_suite(workflow system/workflow.cpp launch fixture)
add_suite(lifecycle system/lifecycle.cpp launch protocol)
add_suite(startup system/startup.cpp launch host content environment)
add_suite(recovery system/recovery.cpp launch protocol content)
add_suite(isolation system/isolation.cpp launch protocol usage unofficial::sqlite3::sqlite3)
foreach(SUITE IN ITEMS analysis workflow lifecycle startup recovery isolation)
  target_compile_definitions(${SUITE}-test PRIVATE
    SERVICE_EXECUTABLE="$<TARGET_FILE:fughetta-service>"
    PROJECT_DIRECTORY="${PROJECT_SOURCE_DIR}")
  add_dependencies(${SUITE}-test fughetta-service)
endforeach()
find_program(T7_BAZEL_TOOL NAMES bazel bazelisk)
if(T7_BAZEL_TOOL AND NOT DEFINED ENV{TEST_BAZEL})
  set_property(TEST preparation APPEND PROPERTY ENVIRONMENT "TEST_BAZEL=${T7_BAZEL_TOOL}")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_suite(spawn platform/spawn.cpp coroutine native)
  add_suite(runner model/runner.cpp runner native support)
  add_suite(fsync platform/fsync.cpp durability support)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
  add_executable(sigwait-test platform/sigwait.cpp)
  target_link_libraries(sigwait-test PRIVATE signals)
  add_test(NAME sigwait COMMAND sigwait-test)
  set_tests_properties(sigwait PROPERTIES TIMEOUT 10)
endif()

## Compile failures
#
# Each one must fail to build with the diagnostic it names.

function(add_compile_failure NAME DIAGNOSTIC)
  add_library(fail_${NAME} OBJECT EXCLUDE_FROM_ALL invalid/${NAME}.cpp)
  target_link_libraries(fail_${NAME} PRIVATE coroutine mcp tools)
  add_test(NAME ${NAME} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target fail_${NAME})
  set_tests_properties(${NAME} PROPERTIES PASS_REGULAR_EXPRESSION "${DIAGNOSTIC}" RUN_SERIAL TRUE)
endfunction()

add_compile_failure(duplicate "two tools share a name")
add_compile_failure(naming "an MCP tool name is")

## Scripted scenarios
#
# Python drivers that exercise the real binaries through the services.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(shell source/shell.cpp)
target_link_libraries(shell PRIVATE engine reflection)

foreach(SCENARIO IN ITEMS atomicity recovery transfer)
  add_test(NAME scenario.${SCENARIO} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/scenario.py" "${CMAKE_BINARY_DIR}" "${SCENARIO}")
  set_tests_properties(scenario.${SCENARIO} PROPERTIES TIMEOUT 180)
endforeach()


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME managed-budget COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/system/processes.py"
    "$<TARGET_FILE:fughetta-service>" "${CMAKE_BINARY_DIR}")
  set_tests_properties(managed-budget PROPERTIES TIMEOUT 90)
endif()

## Shared environment
#
# Every test runs inside the build directory with the same token, so a service
# started by one driver is reachable by the next.

get_property(PROJECT_TESTS DIRECTORY PROPERTY TESTS)
set_property(TEST ${PROJECT_TESTS} APPEND PROPERTY ENVIRONMENT
  "SOURCE_TOKEN=source-test-secret"
  "TMPDIR=${CMAKE_BINARY_DIR}" "TEMP=${CMAKE_BINARY_DIR}" "TMP=${CMAKE_BINARY_DIR}")

find_program(DISPATCH_CC NAMES cc clang gcc REQUIRED)
set_property(TEST dispatch tools APPEND PROPERTY ENVIRONMENT "TEST_CC=${DISPATCH_CC}" "TEST_PYTHON=${Python3_EXECUTABLE}")
target_compile_definitions(dispatch-test PRIVATE TEST_EXECUTABLE_SUFFIX="${CMAKE_EXECUTABLE_SUFFIX}")
