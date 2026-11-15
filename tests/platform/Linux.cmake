# Linux runs the process, runner and fsync suites, the managed budget, and the
# scenarios that load the managed hosts.
include("${CMAKE_CURRENT_LIST_DIR}/posix.cmake")
add_suite(spawn platform/spawn.cpp)
add_suite(runner model/runner.cpp)
add_suite(fsync platform/fsync.cpp)
add_test(NAME managed-budget COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/system/processes.py"
  "$<TARGET_FILE:fughetta-service>" "${CMAKE_BINARY_DIR}")
set_tests_properties(managed-budget PROPERTIES TIMEOUT 90)
set(TEST_MANAGED_HOSTS ON)
