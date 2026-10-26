# Every module of fughetta-service: its target, its units and what it depends on. The
# repository's build and the installed package both include this file, with
# FUGHETTA_SOURCE naming the directory that holds the module sources.

# A module is one target. Its interface unit sits in src/<layer>/, an
# implementation unit sits beside it and is named for what it implements.
function(add_module TARGET SOURCE)
  add_library(${TARGET} STATIC)
  # A consumer of the package builds only the modules it links.
  if(FUGHETTA_PACKAGE)
    set_property(TARGET ${TARGET} PROPERTY EXCLUDE_FROM_ALL ON)
  endif()
  target_link_libraries(${TARGET} PUBLIC xclang::std)
  target_sources(${TARGET} PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${FUGHETTA_SOURCE}" FILES "${SOURCE}")
endfunction()

function(add_implementation TARGET)
  target_sources(${TARGET} PRIVATE ${ARGN})
  set_source_files_properties(${ARGN} PROPERTIES LANGUAGE CXX)
endfunction()

# core: the execution model, reflection and the runtime constants
foreach(MODULE IN ITEMS async coroutine reflection meta constants utility mpmc)
  add_module(${MODULE} "${FUGHETTA_SOURCE}/core/${MODULE}.cppm")
endforeach()

# platform: operating system providers
foreach(MODULE IN ITEMS environment host signals affinity durability process isolation)
  add_module(${MODULE} "${FUGHETTA_SOURCE}/platform/${MODULE}.cppm")
endforeach()

# io
add_module(native ${FUGHETTA_SOURCE}/io/native.cppm)
add_module(http ${FUGHETTA_SOURCE}/io/http.cppm)
add_module(websocket ${FUGHETTA_SOURCE}/io/websocket.cppm)
add_implementation(websocket ${FUGHETTA_SOURCE}/io/entropy.cppm)
add_module(tls ${FUGHETTA_SOURCE}/io/tls.cppm)
add_module(resolver ${FUGHETTA_SOURCE}/io/resolver.cppm)
add_implementation(resolver ${FUGHETTA_SOURCE}/io/dns.cppm)

# model
foreach(MODULE IN ITEMS allocation content semantic world nodes provider cmake bazel commands clice build language protocol configuration usage runner replica lsp)
  add_module(${MODULE} "${FUGHETTA_SOURCE}/model/${MODULE}.cppm")
endforeach()

add_implementation(build ${FUGHETTA_SOURCE}/model/planning.cppm)
add_implementation(cmake ${FUGHETTA_SOURCE}/model/generator.cppm)
add_implementation(language ${FUGHETTA_SOURCE}/model/analysis.cppm)

# gateway
foreach(MODULE IN ITEMS fleet front mcp settings tools registry client keyring templates enrollment)
  add_module(${MODULE} "${FUGHETTA_SOURCE}/gateway/${MODULE}.cppm")
endforeach()
add_implementation(client ${FUGHETTA_SOURCE}/gateway/exchange.cppm)
add_implementation(front ${FUGHETTA_SOURCE}/gateway/routes.cppm ${FUGHETTA_SOURCE}/gateway/broadcast.cppm)
add_implementation(tools ${FUGHETTA_SOURCE}/gateway/backlog.cppm)
add_implementation(enrollment ${FUGHETTA_SOURCE}/gateway/pairing.cppm)

# source
foreach(MODULE IN ITEMS server storage codec index kernel archive engine preparation survey package)
  add_module(${MODULE} "${FUGHETTA_SOURCE}/source/${MODULE}.cppm")
endforeach()
add_implementation(codec ${FUGHETTA_SOURCE}/source/crypto.cppm)
add_implementation(engine ${FUGHETTA_SOURCE}/source/database.cppm)

add_module(executor ${FUGHETTA_SOURCE}/worker/executor.cppm)

## Platform provider
#
# Each platform adds the implementation units its targets need. A platform
# without a provider fails here.

if(WIN32)
  add_implementation(process ${FUGHETTA_SOURCE}/platform/windows/create.cppm)
  add_implementation(isolation ${FUGHETTA_SOURCE}/platform/windows/job.cppm)
  add_implementation(host ${FUGHETTA_SOURCE}/platform/windows/machine.cppm)
  add_implementation(signals ${FUGHETTA_SOURCE}/platform/windows/console.cppm)
  add_implementation(affinity ${FUGHETTA_SOURCE}/platform/windows/group.cppm)
  add_implementation(durability ${FUGHETTA_SOURCE}/platform/windows/flush.cppm)
  add_module(winapi ${FUGHETTA_SOURCE}/platform/windows/winapi.cppm)
  set(PROCESS_PROVIDER winapi)
  foreach(TARGET IN ITEMS host signals affinity durability process isolation winapi)
    target_compile_definitions(${TARGET} PRIVATE NTDDI_VERSION=0x0A00000C)
  endforeach()
  foreach(TARGET IN ITEMS host signals durability)
    target_link_libraries(${TARGET} PRIVATE winapi)
  endforeach()
  target_link_libraries(durability PRIVATE advapi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_implementation(process ${FUGHETTA_SOURCE}/platform/linux/pidfd.cppm)
  add_implementation(isolation ${FUGHETTA_SOURCE}/platform/linux/hierarchy.cppm)
  add_module(cgroup ${FUGHETTA_SOURCE}/platform/linux/cgroup.cppm)
  target_link_libraries(cgroup PUBLIC async PRIVATE descriptor)
  set(PROCESS_PROVIDER cgroup descriptor)
  add_implementation(host ${FUGHETTA_SOURCE}/platform/posix/uname.cppm)
  add_implementation(signals ${FUGHETTA_SOURCE}/platform/posix/sigwait.cppm)
  add_implementation(affinity ${FUGHETTA_SOURCE}/platform/linux/pthread.cppm)
  add_implementation(durability ${FUGHETTA_SOURCE}/platform/linux/fsync.cppm)
  add_module(descriptor ${FUGHETTA_SOURCE}/platform/posix/descriptor.cppm)
  target_link_libraries(durability PRIVATE descriptor)
else()
  message(FATAL_ERROR "No platform provider for ${CMAKE_SYSTEM_NAME}")
endif()
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")

## How they depend
#
# PUBLIC is part of the interface, PRIVATE is implementation. Every target is
# named once.

target_link_libraries(async PUBLIC STDEXEC::stdexec httpant::io utility)
target_link_libraries(coroutine PUBLIC async)
target_link_libraries(reflection PUBLIC reflectcpp::reflectcpp)

target_link_libraries(environment PRIVATE host)
target_link_libraries(affinity PUBLIC async)
target_link_libraries(durability PUBLIC utility PRIVATE constants)
target_link_libraries(process PUBLIC async httpant::io mpmc PRIVATE ${PROCESS_PROVIDER})
target_link_libraries(isolation PUBLIC async PRIVATE ${PROCESS_PROVIDER})
target_link_libraries(native PUBLIC async httpant::io mpmc affinity process)
target_link_libraries(http PUBLIC coroutine meta httpant::httpant)
target_link_libraries(websocket PUBLIC coroutine http PRIVATE unofficial-sodium::sodium)
target_link_libraries(tls PUBLIC httpant::tls)
target_link_libraries(resolver PUBLIC coroutine native PRIVATE c-ares::cares)

target_link_libraries(content PRIVATE unofficial-sodium::sodium)
target_link_libraries(codec PUBLIC content PRIVATE unofficial-sodium::sodium)
target_link_libraries(index PUBLIC codec)
target_link_libraries(kernel PUBLIC codec index content)
target_link_libraries(archive PUBLIC codec PRIVATE native durability zstd::libzstd)
target_link_libraries(nodes PUBLIC semantic)
target_link_libraries(engine PUBLIC nodes codec index kernel archive utility
  PRIVATE durability content unofficial::sqlite3::sqlite3 reflection)
target_link_libraries(world PUBLIC content)
target_link_libraries(templates PUBLIC world content)
target_link_libraries(provider PUBLIC meta reflection content)
target_link_libraries(cmake PUBLIC provider PRIVATE reflection)
target_link_libraries(bazel PUBLIC provider)
target_link_libraries(commands PUBLIC provider)
target_link_libraries(clice PUBLIC provider PRIVATE meta reflection)
target_link_libraries(build PUBLIC provider cmake bazel commands PRIVATE meta reflection content)
target_link_libraries(language PUBLIC build clice PRIVATE meta reflection content)
target_link_libraries(replica PUBLIC content protocol durability PRIVATE constants archive async native)
target_link_libraries(executor PUBLIC native replica runner coroutine protocol isolation PRIVATE host lsp)
target_link_libraries(preparation PUBLIC native storage replica runner language coroutine PRIVATE host reflection durability)
target_link_libraries(runner PUBLIC native async coroutine isolation protocol)
target_link_libraries(protocol PUBLIC coroutine meta content reflection world nodes utility build language)
target_link_libraries(configuration PUBLIC language reflection durability async PRIVATE host constants)
target_link_libraries(usage PUBLIC configuration reflection durability)
target_link_libraries(keyring PUBLIC content configuration world)
target_link_libraries(enrollment PUBLIC content durability reflection PRIVATE constants unofficial-sodium::sodium)

target_link_libraries(client PUBLIC coroutine protocol codec PRIVATE native)
target_link_libraries(storage PUBLIC content configuration usage durability protocol engine archive
  PRIVATE native constants reflection)
target_link_libraries(registry PUBLIC coroutine content configuration usage durability protocol client utility
  PRIVATE native constants)
target_link_libraries(mcp PUBLIC coroutine http reflection world utility)
target_link_libraries(fleet PUBLIC native coroutine protocol enrollment configuration PRIVATE constants)
target_link_libraries(tools PUBLIC native fleet commands templates coroutine meta protocol mcp configuration usage registry reflection world PRIVATE archive)
target_link_libraries(settings PUBLIC mcp tools http configuration reflection meta world)
target_link_libraries(front PUBLIC native settings fleet keyring coroutine http mcp configuration world registry tools protocol reflection
  enrollment PRIVATE constants meta usage commands client codec nodes)
target_link_libraries(lsp PUBLIC native coroutine reflection semantic)
target_link_libraries(survey PUBLIC semantic engine)
target_link_libraries(package PUBLIC content durability reflection nodes engine codec constants)
target_link_libraries(server PUBLIC native storage coroutine protocol configuration usage preparation replica survey lsp package host)

target_link_libraries(configuration PUBLIC allocation)
target_link_libraries(preparation PRIVATE isolation)
target_link_libraries(lsp PRIVATE isolation)
target_link_libraries(server PRIVATE isolation)
