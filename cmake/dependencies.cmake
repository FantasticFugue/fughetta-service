# The packages fughetta-service's modules build against. Third-party packages
# come from vcpkg.json, which also lists httpant's.

find_package(stdexec CONFIG REQUIRED)
find_package(argparse CONFIG REQUIRED)
find_package(yyjson CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(ada CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(roaring CONFIG REQUIRED)
find_package(c-ares CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)

include(FetchContent)

# Compile the checked-in parsers and scanners without regenerating the grammars.
function(fetch_tree_sitter_grammar NAME REPOSITORY REVISION SOURCE_SUBDIR)
  FetchContent_Declare(${NAME}
    URL "https://codeload.github.com/${REPOSITORY}/tar.gz/${REVISION}"
    SOURCE_SUBDIR ${SOURCE_SUBDIR})
endfunction()

function(add_tree_sitter_parser TARGET DIRECTORY)
  add_library(${TARGET} STATIC "${DIRECTORY}/parser.c")
  if(EXISTS "${DIRECTORY}/scanner.c")
    target_sources(${TARGET} PRIVATE "${DIRECTORY}/scanner.c")
  endif()
  target_include_directories(${TARGET} PRIVATE "${DIRECTORY}")
endfunction()

FetchContent_Declare(tree_sitter_cpp
  GIT_REPOSITORY https://github.com/FantasticFugue/tree-sitter-cpp.git
  GIT_TAG main
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR src)
# These archives contain the parser revisions used by the language regression tests.
fetch_tree_sitter_grammar(tree_sitter_rust tree-sitter/tree-sitter-rust 77a3747266f4d621d0757825e6b11edcbf991ca5 src)
fetch_tree_sitter_grammar(tree_sitter_python tree-sitter/tree-sitter-python 26855eabccb19c6abf499fbc5b8dc7cc9ab8bc64 src)
fetch_tree_sitter_grammar(tree_sitter_javascript tree-sitter/tree-sitter-javascript 58404d8cf191d69f2674a8fd507bd5776f46cb11 src)
fetch_tree_sitter_grammar(tree_sitter_typescript tree-sitter/tree-sitter-typescript 75b3874edb2dc714fb1fd77a32013d0f8699989f typescript/src)
fetch_tree_sitter_grammar(tree_sitter_kotlin fwcd/tree-sitter-kotlin d6af96d2e23ef0a8ff5ab512864378cf08fba311 src)
fetch_tree_sitter_grammar(tree_sitter_java tree-sitter/tree-sitter-java e10607b45ff745f5f876bfa3e94fbcc6b44bdc11 src)
fetch_tree_sitter_grammar(tree_sitter_c_sharp tree-sitter/tree-sitter-c-sharp e3971fcb53bf5573a7e0d4ff6db46d2a77a4bf71 src)
fetch_tree_sitter_grammar(tree_sitter_scala tree-sitter/tree-sitter-scala e6da4cc220b2e53d7bd4938688f1174cfa75500f src)
fetch_tree_sitter_grammar(tree_sitter_fsharp ionide/tree-sitter-fsharp a19412828aec9150b8696cb6597396092b36336e fsharp/src)
FetchContent_MakeAvailable(tree_sitter_cpp tree_sitter_rust tree_sitter_python tree_sitter_javascript tree_sitter_typescript
  tree_sitter_kotlin tree_sitter_java tree_sitter_c_sharp tree_sitter_scala tree_sitter_fsharp)
add_tree_sitter_parser(tree-sitter-cpp "${tree_sitter_cpp_SOURCE_DIR}/src")
add_tree_sitter_parser(tree-sitter-rust "${tree_sitter_rust_SOURCE_DIR}/src")
add_tree_sitter_parser(tree-sitter-python "${tree_sitter_python_SOURCE_DIR}/src")
add_tree_sitter_parser(tree-sitter-javascript "${tree_sitter_javascript_SOURCE_DIR}/src")
add_tree_sitter_parser(tree-sitter-typescript "${tree_sitter_typescript_SOURCE_DIR}/typescript/src")
add_tree_sitter_parser(tree-sitter-tsx "${tree_sitter_typescript_SOURCE_DIR}/tsx/src")
add_tree_sitter_parser(tree-sitter-kotlin "${tree_sitter_kotlin_SOURCE_DIR}/src")
add_tree_sitter_parser(tree-sitter-java "${tree_sitter_java_SOURCE_DIR}/src")
add_tree_sitter_parser(tree-sitter-c-sharp "${tree_sitter_c_sharp_SOURCE_DIR}/src")
add_tree_sitter_parser(tree-sitter-scala "${tree_sitter_scala_SOURCE_DIR}/src")
add_tree_sitter_parser(tree-sitter-fsharp "${tree_sitter_fsharp_SOURCE_DIR}/fsharp/src")
add_tree_sitter_parser(tree-sitter-fsharp-signature "${tree_sitter_fsharp_SOURCE_DIR}/fsharp_signature/src")

# httpant builds from source, since module BMIs are not portable between
# compilers, at the head of its main branch until it has a vcpkg port. A local
# checkout replaces it through FETCHCONTENT_SOURCE_DIR_HTTPANT. The platform
# description sets HTTPANT_IO_BACKEND.
set(HTTPANT_ENABLE_IO ON)
set(HTTPANT_ENABLE_TLS ON)
FetchContent_Declare(httpant
  GIT_REPOSITORY https://github.com/tsurumi-yizhou/httpant.git
  GIT_TAG ci/windows-stdexec-module-20261120
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(httpant)

# A compilation database states the language of every command, and a tool that reads one loads a module unit only when the
# command says C++ and the suffix is not one it assumes. httpant's module units keep .ixx, so each target that adds them
# states the language of its own file set.
foreach(HTTPANT_MODULE_TARGET httpant-stdexec httpant-imports httpant-asio httpant-tls-imports)
  get_target_property(HTTPANT_MODULE_UNITS ${HTTPANT_MODULE_TARGET} CXX_MODULE_SET)
  if(HTTPANT_MODULE_UNITS)
    set_source_files_properties(${HTTPANT_MODULE_UNITS} TARGET_DIRECTORY ${HTTPANT_MODULE_TARGET} PROPERTIES LANGUAGE CXX)
  endif()
endforeach()
