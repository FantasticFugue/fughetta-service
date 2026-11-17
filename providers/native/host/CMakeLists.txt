cmake_minimum_required(VERSION 3.28)
project(fughetta-native LANGUAGES CXX)

# LLVM's static libraries, so the host runs without an LLVM installation.
find_package(LLVM CONFIG REQUIRED)
llvm_map_components_to_libnames(LLVM_LIBRARIES
  Object DebugInfoDWARF DebugInfoPDB IRReader BitReader AsmParser Core Support TargetParser)

add_executable(fughetta-native fughetta-native.cpp)
target_compile_features(fughetta-native PRIVATE cxx_std_23)
target_include_directories(fughetta-native SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(fughetta-native PRIVATE ${LLVM_DEFINITIONS_LIST})
target_link_libraries(fughetta-native PRIVATE ${LLVM_LIBRARIES})
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(fughetta-native PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>)
endif()
