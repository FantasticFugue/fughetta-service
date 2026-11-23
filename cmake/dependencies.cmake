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

# httpant builds from source, since module BMIs are not portable between
# compilers, at the head of its main branch until it has a vcpkg port. A local
# checkout replaces it through FETCHCONTENT_SOURCE_DIR_HTTPANT. The platform
# description sets HTTPANT_IO_BACKEND.
include(FetchContent)
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
