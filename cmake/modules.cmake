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

# core: the execution model, wire codecs and runtime constants
add_layer(core)
add_modules(core core async coroutine fields meta constants utility mpmc value json xml)

# platform: operating system providers
add_layer(platform)
add_modules(platform platform environment host signals affinity durability process)

# io
add_layer(io)
add_modules(io io native http websocket tls resolver)
add_implementation(io io entropy dns)

# model: the domain foundation — content and records, the worldline model, the
# abstract toolchain and provider contracts. Nothing here knows a concrete
# build system or language.
add_layer(model)
add_modules(model model allocation archive channels codec commands content enumeration objects
  projectmodel provider publicrecord records report reuse runner toolchain version world)
add_implementation(model model crypto)

# toolchain: concrete build-system and package-manager adapters. (The abstract
# toolchain model they plug into is the toolchain module of the model layer.)
add_layer(toolchain)
add_modules(toolchain toolchain bazel bun cargo cmake gradle msbuild uv vcpkg)
add_implementation(toolchain toolchain generator)

# languages: the analysis interface (evaluation, lsp) and the knowledge of each
# language — compiler frontends, VM metadata and cross-language bindings.
add_layer(languages)
add_modules(languages languages bridge clice clr csharp evaluation fsharp helper jni jvm kmetadata kotlin llvm lsp
  pinvoke pybind ruff rust scala ty typescript)

# session: the facet registries that aggregate toolchains and languages (build,
# language, profiles), the semantic graph (semantic, indexing, nodes,
# knowledge) and the workspace protocol the upper layers speak.
add_layer(session)
add_modules(session session build configuration delivery indexing knowledge language nodes profiles
  protocol replica semantic usage)
add_implementation(session session planning analysis impact comparison reanalysis wire)

# gateway
add_layer(gateway)
add_modules(gateway gateway fleet front mcp settings tool publication toolbase worldlines dispatch queries tools registry client keyring templates enrollment publiccache)
add_implementation(gateway gateway exchange routes broadcast backlog pairing)

# source
add_layer(source)
add_modules(source source server storage kernel nodestore engineapi nodestate engine package)
add_implementation(source source database objects history coordination worldstate)

# worker
add_layer(worker)
add_modules(worker worker executor dependencies backend sibling)

# Host Executor protocol, journal, and platform provisioner.
add_layer(host_executor)
add_modules(host_executor host api journal operation ${HOST_MODULES})
target_link_libraries(host_executor PUBLIC session PRIVATE gateway ${HOST_LIBRARIES})

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

target_link_libraries(core PUBLIC STDEXEC::stdexec httpant::io PRIVATE yyjson::yyjson pugixml::pugixml)
target_link_libraries(platform PUBLIC core)
target_link_libraries(io PUBLIC platform httpant::httpant httpant::tls PRIVATE unofficial-sodium::sodium c-ares::cares ada::ada)
target_link_libraries(model PUBLIC io PRIVATE unofficial-sodium::sodium zstd::libzstd)
target_link_libraries(toolchain PUBLIC model)
target_link_libraries(languages PUBLIC model PRIVATE ada::ada libzip::zip)
target_link_libraries(session PUBLIC model toolchain languages PRIVATE unofficial::sqlite3::sqlite3)
target_link_libraries(gateway PUBLIC session PRIVATE unofficial-sodium::sodium)
target_link_libraries(source PUBLIC session PRIVATE unofficial::sqlite3::sqlite3 roaring::roaring)
target_link_libraries(worker PUBLIC session)
