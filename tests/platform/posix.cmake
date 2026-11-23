# The signal wait links only the platform layer.
add_executable(sigwait-test platform/sigwait.cpp)
target_link_libraries(sigwait-test PRIVATE platform)
add_test(NAME platform.sigwait COMMAND sigwait-test)
set_tests_properties(platform.sigwait PROPERTIES TIMEOUT 10)
