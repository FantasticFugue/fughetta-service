# Test registration. Included from the root CMakeLists.txt with
# add_subdirectory(tests) when FUGHETTA_BUILD_TESTS is on; every target here is built
# only for that.
#
# A suite is `add_suite(<name> <source under tests/> [SERVICE] [TIMEOUT <s>]
# [ENVIRONMENT <var=value>...] <targets it links>)`; it produces `<name>-test`,
# registered with the same name, and links the testing layer, which brings
# every layer up to gateway. SERVICE suites start the built fughetta-service.
# Sources that need a script driver are registered directly with add_test.
#
# tests/platform/<system>.cmake adds the platform's own tests. TEST_ENVIRONMENT
# collects what every test sees.

# The scripted drivers look for their helpers beside the services, so test
# binaries keep the flat layout the root build has.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(ut CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(TEST_ENVIRONMENT
  "SOURCE_TOKEN=source-test-secret"
  "TMPDIR=${CMAKE_BINARY_DIR}" "TEMP=${CMAKE_BINARY_DIR}" "TMP=${CMAKE_BINARY_DIR}"
  "TEST_PYTHON=${Python3_EXECUTABLE}")

add_layer(testing)
add_modules(testing testing support fixture launch)
# boost.ut's global module fragment differs per platform; the header itself is
# included in the module purview, as its module mode expects.
add_modules(testing testing/${PLATFORM_FAMILY} ut)
set_source_files_properties("${FUGHETTA_SOURCE}/testing/${PLATFORM_FAMILY}/ut.cppm" PROPERTIES
  COMPILE_DEFINITIONS BOOST_UT_CXX_MODULES=1 COMPILE_OPTIONS -Wno-include-angled-in-module-purview)
add_implementation(testing testing service)
target_link_libraries(testing PUBLIC gateway Boost::ut PRIVATE source)

function(add_suite NAME SOURCE)
  cmake_parse_arguments(PARSE_ARGV 2 SUITE "SERVICE" "TIMEOUT" "ENVIRONMENT")
  if(NOT SUITE_TIMEOUT)
    set(SUITE_TIMEOUT 120)
  endif()
  add_executable(${NAME}-test "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}")
  target_link_libraries(${NAME}-test PRIVATE testing ${SUITE_UNPARSED_ARGUMENTS})
  if(SUITE_SERVICE)
    target_compile_definitions(${NAME}-test PRIVATE SERVICE_EXECUTABLE="$<TARGET_FILE:fughetta-service>")
    add_dependencies(${NAME}-test fughetta-service)
  endif()
  add_test(NAME ${NAME} COMMAND ${NAME}-test)
  set_tests_properties(${NAME} PROPERTIES TIMEOUT ${SUITE_TIMEOUT} ENVIRONMENT "${SUITE_ENVIRONMENT}")
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
add_suite(managed model/managed.cpp)
add_suite(toolchain model/toolchain.cpp)
add_suite(tooling model/tooling.cpp)
add_suite(bindings model/bindings.cpp)
add_suite(config gateway/config.cpp)
add_suite(identity gateway/identity.cpp)
add_suite(enrollment gateway/enrollment.cpp)
add_suite(content model/content.cpp)
add_suite(delivery model/delivery.cpp)
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
add_suite(host platform/host.cpp ENVIRONMENT "HOST_TEST_VALUE=left=right")
add_suite(settings gateway/settings.cpp)
add_suite(meta gateway/meta.cpp)
add_suite(node source/node.cpp source unofficial::sqlite3::sqlite3)
add_suite(mail source/mail.cpp source unofficial::sqlite3::sqlite3)
add_suite(optimistic source/optimistic.cpp source unofficial::sqlite3::sqlite3)
add_suite(storage source/storage.cpp source)
add_suite(executor worker/executor.cpp worker)
add_suite(backend worker/backend.cpp worker)
# The language-server suites drive clice when it is found.
find_program(CLICE_TOOL clice)
if(CLICE_TOOL)
  set(CLICE_ENVIRONMENT "TEST_CLICE=${CLICE_TOOL}")
endif()
add_suite(lsp source/lsp.cpp ENVIRONMENT ${CLICE_ENVIRONMENT})
add_suite(server source/server.cpp source)

add_suite(analysis system/analysis.cpp SERVICE TIMEOUT 360 ENVIRONMENT ${CLICE_ENVIRONMENT})

add_suite(workflow system/workflow.cpp SERVICE)
add_suite(lifecycle system/lifecycle.cpp SERVICE)
add_suite(startup system/startup.cpp SERVICE)
add_suite(recovery system/recovery.cpp SERVICE)
add_suite(projects system/projects.cpp SERVICE unofficial::sqlite3::sqlite3)

include("${CMAKE_CURRENT_SOURCE_DIR}/platform/${CMAKE_SYSTEM_NAME}.cmake")

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
# Python drivers that exercise the real binaries through the services. The
# managed providers' scenarios run when cmake/providers.cmake built them.

# Each provider's model exporter runs against the sample projects.
function(add_model_test PROVIDER TOOL EXPORTER)
  add_test(NAME model.${PROVIDER} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/model/managed.py"
    "${PROVIDER}" "${TOOL}" "${EXPORTER}" --work "${CMAKE_BINARY_DIR}/providers/tests" --validator "$<TARGET_FILE:managed-test>")
  set_tests_properties(model.${PROVIDER} PROPERTIES TIMEOUT 300)
endfunction()

find_program(FUGHETTA_JAVAC javac)
if(FUGHETTA_JAVAC)
  add_executable(jvm-test model/jvm.cpp)
  target_link_libraries(jvm-test PRIVATE testing)
  add_test(NAME model.jvm COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/model/jvm.py"
    "$<TARGET_FILE:jvm-test>" "${FUGHETTA_JAVAC}" "${CMAKE_BINARY_DIR}/jvm-tests")
  set_tests_properties(model.jvm PROPERTIES TIMEOUT 120)
endif()

if(GRADLE_TOOL)
  add_test(NAME helper.kotlin COMMAND ${FUGHETTA_GRADLE} test)
  set_tests_properties(helper.kotlin PROPERTIES TIMEOUT 300 ENVIRONMENT
    "FUGHETTA_RECORD_VALIDATOR=$<TARGET_FILE:managed-test>;FUGHETTA_WORKER_TEST=$<TARGET_FILE:executor-test>")
  add_model_test(kotlin "${GRADLE_TOOL}" "${FUGHETTA_JVM_EXPORTER}")
  if(FUGHETTA_JAVAC AND TEST_MANAGED_HOSTS AND FUGHETTA_PROVIDER_llvm)
    cmake_path(GET FUGHETTA_JAVAC PARENT_PATH FUGHETTA_JAVA_BIN)
    add_test(NAME model.kotlin-jni COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/model/jvm.py" --kotlin
      "$<TARGET_FILE:jvm-test>" "${GRADLE_TOOL}" "${FUGHETTA_JVM_EXPORTER}" "${FUGHETTA_JAVA_BIN}/java"
      "${FUGHETTA_PROVIDER_jvm}" "${FUGHETTA_PROVIDER_kotlin}"
      "$<TARGET_FILE:managed-test>" "${CMAKE_CXX_COMPILER}" "${CMAKE_BINARY_DIR}/jvm-tests")
    set_tests_properties(model.kotlin-jni PROPERTIES TIMEOUT 600)
  endif()
endif()

if(DOTNET_TOOL)
  add_executable(clr-test model/clr.cpp)
  target_link_libraries(clr-test PRIVATE testing)
  if(TEST_MANAGED_HOSTS AND FUGHETTA_PROVIDER_llvm)
    add_test(NAME model.clr COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/model/clr.py"
      "${DOTNET_TOOL}" "${FUGHETTA_PROVIDER_clr}" "${FUGHETTA_PROVIDER_fsharp}" "$<TARGET_FILE:managed-test>"
      "$<TARGET_FILE:clr-test>" "${CMAKE_BINARY_DIR}/clr-tests"
      --exporter "${FUGHETTA_CLR_EXPORTER}" --compiler "${CMAKE_CXX_COMPILER}")
    set_tests_properties(model.clr PROPERTIES TIMEOUT 300)
  endif()
  add_test(NAME helper.fsharp COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/model/fsharp.py"
    "${DOTNET_TOOL}" "${FUGHETTA_PROVIDER_clr}" "${FUGHETTA_PROVIDER_fsharp}" "$<TARGET_FILE:managed-test>"
    --worker "$<TARGET_FILE:executor-test>" --work "${CMAKE_BINARY_DIR}/fsharp-tests" --exporter "${FUGHETTA_CLR_EXPORTER}")
  set_tests_properties(helper.fsharp PROPERTIES TIMEOUT 300)
  add_model_test(fsharp "${DOTNET_TOOL}" "${FUGHETTA_CLR_EXPORTER}")
endif()

# The analysis suite runs the whole managed flow when both providers and a JDK
# are present.
if(TEST_MANAGED_HOSTS AND GRADLE_TOOL AND DOTNET_TOOL AND FUGHETTA_JAVAC)
  file(REAL_PATH "${FUGHETTA_JAVAC}" FUGHETTA_JDK_BIN)
  cmake_path(GET FUGHETTA_JDK_BIN PARENT_PATH FUGHETTA_JDK_BIN)
  cmake_path(GET FUGHETTA_JDK_BIN PARENT_PATH FUGHETTA_JDK)
  set_property(TEST analysis APPEND PROPERTY ENVIRONMENT
    "TEST_GRADLE=${GRADLE_TOOL}"
    "TEST_GRADLE_MODULES=${JVM_OUTPUT}/home/caches/modules-2"
    "TEST_DOTNET=${DOTNET_TOOL}"
    "TEST_JAVA=${FUGHETTA_JDK_BIN}/java"
    "TEST_JDK=${FUGHETTA_JDK}"
    "TEST_CXX=${CMAKE_CXX_COMPILER}"
    "TEST_JVM_HOST=${FUGHETTA_PROVIDER_jvm}"
    "TEST_KOTLIN_ADAPTER=${FUGHETTA_PROVIDER_kotlin}"
    "TEST_CLR_HOST=${FUGHETTA_PROVIDER_clr}"
    "TEST_FSHARP_ADAPTER=${FUGHETTA_PROVIDER_fsharp}"
    "TEST_GRADLE_MODEL=${FUGHETTA_JVM_EXPORTER}"
    "TEST_MSBUILD_MODEL=${FUGHETTA_CLR_EXPORTER}")
endif()


## Shared environment
#
# Every test runs inside the build directory with the same token, so a service
# started by one driver is reachable by the next.

if(FUGHETTA_PROVIDER_llvm)
  list(APPEND TEST_ENVIRONMENT "TEST_LLVM_HOST=${FUGHETTA_PROVIDER_llvm}")
  # The fixtures compile with the clang driver of the LLVM package; the DLL
  # and Rust fixtures need lld-link and rustc.
  find_program(NATIVE_CLANG NAMES clang HINTS "${LLVM_PREFIX}/bin" NO_DEFAULT_PATH REQUIRED)
  find_program(NATIVE_LLD_LINK NAMES lld-link HINTS "${LLVM_PREFIX}/bin" NO_DEFAULT_PATH)
  find_program(NATIVE_RUSTC rustc)
  set(NATIVE_TEST_OPTIONS)
  if(NATIVE_LLD_LINK)
    list(APPEND NATIVE_TEST_OPTIONS --lld-link "${NATIVE_LLD_LINK}")
  endif()
  if(NATIVE_RUSTC)
    list(APPEND NATIVE_TEST_OPTIONS --rustc "${NATIVE_RUSTC}")
  endif()
  add_test(NAME model.llvm COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/model/llvm.py"
    "${FUGHETTA_PROVIDER_llvm}" "${NATIVE_CLANG}" "${CMAKE_BINARY_DIR}/llvm-tests" ${NATIVE_TEST_OPTIONS})
endif()

get_property(PROJECT_TESTS DIRECTORY PROPERTY TESTS)
set_property(TEST ${PROJECT_TESTS} APPEND PROPERTY ENVIRONMENT ${TEST_ENVIRONMENT})

# Beside the compiler first, since Windows does not put the clang tree on PATH.
cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH DISPATCH_CC_BIN)
find_program(DISPATCH_CC NAMES cc clang gcc HINTS "${DISPATCH_CC_BIN}" REQUIRED)
set_property(TEST dispatch tools APPEND PROPERTY ENVIRONMENT "TEST_CC=${DISPATCH_CC}")
target_compile_definitions(dispatch-test PRIVATE TEST_EXECUTABLE_SUFFIX="${CMAKE_EXECUTABLE_SUFFIX}")
