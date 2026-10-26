set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_LOAD_VCVARS_ENV ON)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../windows-xclang.cmake")
set(VCPKG_ENV_PASSTHROUGH XCLANG_ROOT)

include("${CMAKE_CURRENT_LIST_DIR}/../../../vcpkg-registry/triplets/httpant-source.cmake")
