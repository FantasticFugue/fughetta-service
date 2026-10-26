# The dependency's target is the application's target: the triplet's
# architecture is what both the compiler and this file follow.
if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
  set(XCLANG_TARGET aarch64-unknown-linux-gnu)
  set(_xclang_system_processor aarch64)
else()
  set(XCLANG_TARGET x86_64-unknown-linux-gnu)
  set(_xclang_system_processor x86_64)
endif()
include("$ENV{XCLANG_ROOT}/lib/cmake/xclang/toolchain.cmake")

# These triplets set VCPKG_CMAKE_SYSTEM_NAME, so every dependency build is a
# cross build even on a host of the same target. CMake does not detect
# CMAKE_SYSTEM_PROCESSOR in a cross build, and a port that reads it unquoted
# fails when it is empty — msquic 2.4.8, for one, calls
# string(TOLOWER ${CMAKE_SYSTEM_PROCESSOR} SYSTEM_PROCESSOR), which then has no
# output variable at all. The target architecture is known here, so state it;
# this is the variable a cross build is supposed to get from its toolchain.
set(CMAKE_SYSTEM_PROCESSOR "${_xclang_system_processor}")
