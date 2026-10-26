include("${CMAKE_CURRENT_LIST_DIR}/windows-xclang.cmake")
set(CMAKE_C_COMPILER "$ENV{XCLANG_ROOT}/bin/clang-cl.exe")
set(CMAKE_CXX_COMPILER "$ENV{XCLANG_ROOT}/bin/clang-cl.exe")
set(CMAKE_AR "$ENV{XCLANG_ROOT}/bin/llvm-lib.exe")
