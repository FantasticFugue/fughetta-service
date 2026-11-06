# Every module of fughetta-service: its layer target, its units and what each layer
# depends on. The repository's build and the installed package both include this
# file, with FUGHETTA_SOURCE naming the directory that holds the module sources.

# A layer is one target named for its directory under src/. Modules inside a
# layer import each other freely; a layer imports only the layers it links, so
# the dependency direction is checked at this granularity.
function(add_layer TARGET)
  add_library(${TARGET} STATIC)
  # A consumer of the package builds only the layers it links.
  if(FUGHETTA_PACKAGE)
    set_property(TARGET ${TARGET} PROPERTY EXCLUDE_FROM_ALL ON)
  endif()
  target_link_libraries(${TARGET} PUBLIC xclang::std)
endfunction()

# Interface units, named without the extension, under src/<directory>/.
function(add_modules TARGET DIRECTORY)
  set(UNITS ${ARGN})
  list(TRANSFORM UNITS PREPEND "${FUGHETTA_SOURCE}/${DIRECTORY}/")
  list(TRANSFORM UNITS APPEND ".cppm")
  target_sources(${TARGET} PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${FUGHETTA_SOURCE}" FILES ${UNITS})
endfunction()

# Implementation units sit beside the interface they implement and are named
# for what they implement.
function(add_implementation TARGET DIRECTORY)
  set(UNITS ${ARGN})
  list(TRANSFORM UNITS PREPEND "${FUGHETTA_SOURCE}/${DIRECTORY}/")
  list(TRANSFORM UNITS APPEND ".cppm")
  target_sources(${TARGET} PRIVATE ${UNITS})
  set_source_files_properties(${UNITS} PROPERTIES LANGUAGE CXX)
endfunction()

# core: the execution model, reflection and the runtime constants
add_layer(core)
add_modules(core core async coroutine reflection meta constants utility mpmc)

# platform: operating system providers
add_layer(platform)
add_modules(platform platform environment host signals affinity durability process isolation)

# io
add_layer(io)
add_modules(io io native http websocket tls resolver)
add_implementation(io io entropy dns)

# model
add_layer(model)
add_modules(model model allocation content codec archive semantic world nodes provider report cmake bazel
  commands clice vcpkg typescript bun uv ty ruff build language protocol configuration usage runner replica delivery lsp toolchain)
add_implementation(model model crypto planning generator analysis)

# gateway
add_layer(gateway)
add_modules(gateway gateway fleet front mcp settings tools registry client keyring templates enrollment)
add_implementation(gateway gateway exchange routes broadcast backlog pairing)

# source
add_layer(source)
add_modules(source source server storage index kernel engine package)
add_implementation(source source database)

# worker
add_layer(worker)
add_modules(worker worker executor dependencies)

## Platform provider
#
# Each platform adds the units the platform layer needs. A platform without a
# provider fails here.

if(WIN32)
  add_modules(platform platform/windows winapi)
  add_implementation(platform platform/windows create job machine console group flush)
  target_compile_definitions(platform PRIVATE NTDDI_VERSION=0x0A00000C)
  target_link_libraries(platform PRIVATE advapi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_modules(platform platform/linux cgroup)
  add_modules(platform platform/posix descriptor)
  add_implementation(platform platform/linux pidfd hierarchy pthread fsync)
  add_implementation(platform platform/posix uname sigwait)
else()
  message(FATAL_ERROR "No platform provider for ${CMAKE_SYSTEM_NAME}")
endif()
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")

## How they depend
#
# PUBLIC is part of the interface, PRIVATE is implementation.

target_link_libraries(core PUBLIC STDEXEC::stdexec httpant::io reflectcpp::reflectcpp)
target_link_libraries(platform PUBLIC core)
target_link_libraries(io PUBLIC platform httpant::httpant httpant::tls PRIVATE unofficial-sodium::sodium c-ares::cares)
target_link_libraries(model PUBLIC io PRIVATE unofficial-sodium::sodium zstd::libzstd)
target_link_libraries(gateway PUBLIC model PRIVATE unofficial-sodium::sodium)
target_link_libraries(source PUBLIC model PRIVATE unofficial::sqlite3::sqlite3)
target_link_libraries(worker PUBLIC model)
