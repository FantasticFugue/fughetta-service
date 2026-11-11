# The packages fughetta-service's modules build against. Third-party packages
# come from vcpkg.json, which also lists httpant's.

find_package(xclang CONFIG REQUIRED)
find_package(stdexec CONFIG REQUIRED)
find_package(argparse CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(c-ares CONFIG REQUIRED)

# httpant builds from source, since module BMIs are not portable between
# compilers, at the head of its main branch until it has a vcpkg port. A local
# checkout replaces it through FETCHCONTENT_SOURCE_DIR_HTTPANT.
include(FetchContent)
set(HTTPANT_ENABLE_IO ON)
set(HTTPANT_ENABLE_TLS ON)
if(WIN32)
  set(HTTPANT_IO_BACKEND iocp CACHE STRING "Native I/O mechanism")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(HTTPANT_IO_BACKEND uring CACHE STRING "Native I/O mechanism")
elseif(APPLE)
  set(HTTPANT_IO_BACKEND kqueue CACHE STRING "Native I/O mechanism")
endif()
FetchContent_Declare(httpant
  GIT_REPOSITORY https://github.com/tsurumi-yizhou/httpant.git
  GIT_TAG main
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(httpant)
