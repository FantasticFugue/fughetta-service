cmake_minimum_required(VERSION 3.28)
project(fughetta-llvm LANGUAGES CXX)

# The Windows package's PDB library names the DIA SDK of a Visual Studio
# installation, which LLVM's package finds only inside a developer prompt. The
# host opens PDBs with the native session, so the DIA reader and the ATL it
# needs stay out of the link.
if(WIN32)
  find_program(VSWHERE vswhere HINTS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer" REQUIRED)
  execute_process(COMMAND "${VSWHERE}" -latest -products * -property installationPath
    OUTPUT_VARIABLE VISUAL_STUDIO OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  cmake_path(CONVERT "${VISUAL_STUDIO}/DIA SDK" TO_CMAKE_PATH_LIST MSVC_DIA_SDK_DIR NORMALIZE)
  set(MSVC_DIA_SDK_DIR "${MSVC_DIA_SDK_DIR}" CACHE PATH "DIA SDK of the Visual Studio installation")
endif()

# The Windows package links zlib and zstd without shipping them; the parent
# build passes its vcpkg tree, whose static libraries are found here first,
# since LLVM's own Findzstd takes a static zstd only by the name zstd_static.
if(WIN32)
  find_package(zstd CONFIG REQUIRED)
  set(ZLIB_USE_STATIC_LIBS ON)
  find_package(ZLIB REQUIRED)
endif()

# LLVM's static libraries, so the host runs without an LLVM installation.
find_package(LLVM CONFIG REQUIRED)
llvm_map_components_to_libnames(LLVM_LIBRARIES
  Object DebugInfoDWARF DebugInfoPDB IRReader BitReader AsmParser Core Support TargetParser)

add_executable(fughetta-llvm fughetta-llvm.cpp)
target_compile_features(fughetta-llvm PRIVATE cxx_std_23)
target_include_directories(fughetta-llvm SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(fughetta-llvm PRIVATE ${LLVM_DEFINITIONS_LIST})
target_link_libraries(fughetta-llvm PRIVATE ${LLVM_LIBRARIES})
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(fughetta-llvm PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>)
endif()
