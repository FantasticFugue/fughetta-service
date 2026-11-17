# Every module of fughetta-service: its layer target, its units and what each layer
# depends on. FUGHETTA_SOURCE names the directory that holds the module sources.

# A layer is one target named for its directory under src/. Modules inside a
# layer import each other freely; a layer imports only the layers it links, so
# the dependency direction is checked at this granularity.
function(add_layer TARGET)
  add_library(${TARGET} STATIC)
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
  # clang reads a .cppm file as an interface unit unless CMake passes the
  # language to the compiler.
  set_source_files_properties(${UNITS} PROPERTIES LANGUAGE CXX)
endfunction()

# core: the execution model, reflection and the runtime constants
add_layer(core)
add_modules(core core async coroutine reflection meta constants utility mpmc)

# platform: operating system providers
add_layer(platform)
add_modules(platform platform environment host signals affinity durability process)

# io
add_layer(io)
add_modules(io io native http websocket tls resolver)
add_implementation(io io entropy dns)

# model
add_layer(model)
add_modules(model model allocation content codec archive records objects profiles semantic enumeration reuse channels indexing knowledge world nodes provider report cmake bazel
  commands clice artifact jvm kmetadata clr bridge pinvoke jni vcpkg typescript bun uv ty ruff gradle msbuild projectmodel build language protocol configuration usage runner replica delivery lsp evaluation helper kotlin fsharp toolchain)
add_implementation(model model crypto planning generator analysis impact comparison reanalysis)

# gateway
add_layer(gateway)
add_modules(gateway gateway fleet front mcp settings tool publication toolbase worldlines dispatch queries tools registry client keyring templates enrollment)
add_implementation(gateway gateway exchange routes broadcast backlog pairing)

# source
add_layer(source)
add_modules(source source server storage kernel nodestore engineapi nodestate engine package)
add_implementation(source source database objects history coordination worldstate)

# worker
add_layer(worker)
add_modules(worker worker executor dependencies backend sibling)

## Platform provider
#
# The units, definitions, link options and libraries the platform layer adds,
# from the description the root build includes.

add_modules(platform platform ${PLATFORM_MODULES})
add_implementation(platform platform ${PLATFORM_IMPLEMENTATION})
target_compile_definitions(platform PRIVATE ${PLATFORM_DEFINITIONS})
target_link_options(platform INTERFACE ${PLATFORM_LINK_OPTIONS})
target_link_libraries(platform PRIVATE ${PLATFORM_LIBRARIES})

## How they depend
#
# PUBLIC is part of the interface, PRIVATE is implementation.

target_link_libraries(core PUBLIC STDEXEC::stdexec httpant::io reflectcpp::reflectcpp)
target_link_libraries(platform PUBLIC core)
target_link_libraries(io PUBLIC platform httpant::httpant httpant::tls PRIVATE unofficial-sodium::sodium c-ares::cares)
target_link_libraries(model PUBLIC io PRIVATE unofficial-sodium::sodium ZLIB::ZLIB zstd::libzstd unofficial::sqlite3::sqlite3)
target_link_libraries(gateway PUBLIC model PRIVATE unofficial-sodium::sodium)
target_link_libraries(source PUBLIC model PRIVATE unofficial::sqlite3::sqlite3)
target_link_libraries(worker PUBLIC model)
