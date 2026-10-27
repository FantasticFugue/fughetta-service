# Test registration. Included from the root CMakeLists.txt with
# add_subdirectory(tests) when BUILD_TESTING is on; every target here is built
# only for that.
#
# A suite is `add_suite(<name> <source under tests/> <targets it links>)`; it
# produces `<name>-test`, registered with the same name, and links the testing
# layer, which brings every layer up to gateway. Sources that need a
# script driver are registered directly with add_test.

# The scripted drivers look for their helpers beside the services, so test
# binaries keep the flat layout the root build has.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(ut CONFIG REQUIRED)

add_layer(testing)
add_modules(testing testing ut support fixture launch)
add_implementation(testing testing service)
target_link_libraries(testing PUBLIC gateway Boost::ut PRIVATE source)

function(add_suite NAME SOURCE)
  add_executable(${NAME}-test "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
  target_link_libraries(${NAME}-test PRIVATE testing ${ARGN})
  add_test(NAME ${NAME} COMMAND ${NAME}-test)
  set_tests_properties(${NAME} PROPERTIES TIMEOUT 120)
endfunction()

## Suites

add_suite(allocation model/allocation.cpp)
add_suite(control model/control.cpp)
add_suite(async core/async.cpp)
add_suite(boundary io/boundary.cpp)
add_suite(cancel gateway/cancel.cpp)
add_suite(build model/build.cpp)
add_suite(python model/python.cpp)
add_suite(typescript model/typescript.cpp)
add_suite(tooling model/tooling.cpp)
add_suite(config gateway/config.cpp)
add_suite(identity gateway/identity.cpp)
add_suite(enrollment gateway/enrollment.cpp)
add_suite(content model/content.cpp)
add_suite(durability source/durability.cpp source)
add_suite(edge io/edge.cpp)
add_suite(websocket io/websocket.cpp)
add_suite(resolver io/resolver.cpp)
add_suite(environment platform/environment.cpp)
add_suite(foundation core/foundation.cpp)
add_suite(mpmc core/mpmc.cpp)
add_suite(fleet gateway/fleet.cpp)
add_suite(tools gateway/tools.cpp source worker)
add_suite(backlog gateway/backlog.cpp)
add_suite(dispatch gateway/dispatch.cpp worker)
add_suite(front gateway/front.cpp)
add_suite(generation gateway/generation.cpp)
add_suite(host platform/host.cpp)
set_property(TEST host APPEND PROPERTY ENVIRONMENT "HOST_TEST_VALUE=left=right")
add_suite(settings gateway/settings.cpp)
add_suite(meta gateway/meta.cpp)
add_suite(node source/node.cpp source unofficial::sqlite3::sqlite3)
add_suite(storage source/storage.cpp source)
add_suite(executor worker/executor.cpp worker)
add_suite(preparation source/preparation.cpp source)
add_suite(lsp source/lsp.cpp)
file(GLOB CLICE_HINTS "$ENV{HOME}/.cache/clice/*/clice/bin")
find_program(CLICE_TOOL clice HINTS ${CLICE_HINTS})
if(CLICE_TOOL)
  set_property(TEST lsp APPEND PROPERTY ENVIRONMENT "TEST_CLICE=${CLICE_TOOL}")
endif()
add_suite(server source/server.cpp source)

add_suite(analysis system/analysis.cpp)
if(CLICE_TOOL)
  set_property(TEST analysis APPEND PROPERTY ENVIRONMENT "TEST_CLICE=${CLICE_TOOL}")
endif()
add_suite(workflow system/workflow.cpp)
add_suite(lifecycle system/lifecycle.cpp)
add_suite(startup system/startup.cpp)
add_suite(recovery system/recovery.cpp)
add_suite(isolation system/isolation.cpp unofficial::sqlite3::sqlite3)
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
  add_suite(spawn platform/spawn.cpp)
  add_suite(runner model/runner.cpp)
  add_suite(fsync platform/fsync.cpp)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
  add_executable(sigwait-test platform/sigwait.cpp)
  target_link_libraries(sigwait-test PRIVATE platform)
  add_test(NAME sigwait COMMAND sigwait-test)
  set_tests_properties(sigwait PROPERTIES TIMEOUT 10)
endif()

## Compile failures
#
# Each one must fail to build with the diagnostic it names.

function(add_compile_failure NAME DIAGNOSTIC)
  add_library(fail_${NAME} OBJECT EXCLUDE_FROM_ALL invalid/${NAME}.cpp)
  target_link_libraries(fail_${NAME} PRIVATE gateway)
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
target_link_libraries(shell PRIVATE source)

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
