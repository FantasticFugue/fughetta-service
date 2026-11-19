# Windows tests delivery against its own file semantics, and runs commands
# through Git's sh and a helper that echoes its command line.
add_suite(delivery-windows model/delivery_windows.cpp)
find_program(TEST_SHELL NAMES sh PATHS "$ENV{ProgramFiles}/Git/bin" REQUIRED)
add_executable(command-helper platform/command.cpp)
target_compile_definitions(command-helper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
list(APPEND TEST_ENVIRONMENT "TEST_SHELL=${TEST_SHELL}" "TEST_COMMAND_HELPER=$<TARGET_FILE:command-helper>")
set_property(TEST dispatch APPEND PROPERTY ENVIRONMENT "RC=${CMAKE_RC_COMPILER}")
