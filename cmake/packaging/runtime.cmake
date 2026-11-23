# Used on the installed tree, without configuring or rebuilding the project.
set(executables "${STAGE}/bin/fughetta-service${SUFFIX}"
  "${STAGE}/libexec/fughetta-service/llvm/fughetta-llvm${SUFFIX}")
if(CMAKE_HOST_WIN32)
  set(exclude "api-ms-.*" "ext-ms-.*")
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "$ENV{CLANG_ROOT}/bin/llvm-objdump.exe")
endif()
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES ${executables}
  DIRECTORIES ${SEARCH}
  PRE_EXCLUDE_REGEXES ${exclude}
  RESOLVED_DEPENDENCIES_VAR resolved
  UNRESOLVED_DEPENDENCIES_VAR unresolved)
if(unresolved)
  message(FATAL_ERROR "Missing runtime libraries: ${unresolved}")
endif()
string(REPLACE ";" "\n" resolved "${resolved}")
file(WRITE "${OUTPUT}" "${resolved}\n")
