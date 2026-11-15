# macOS: kqueue, and a launchd definition for the workspace instance the
# build names.
include("${CMAKE_CURRENT_LIST_DIR}/posix.cmake")
list(APPEND PLATFORM_IMPLEMENTATION darwin/posix darwin/affinity)
set(HTTPANT_IO_BACKEND kqueue CACHE STRING "Native I/O mechanism")
if(FUGHETTA_INSTANCE)
  set(DAEMON_DEFINITION dev.fughetta.service.plist)
  set(DAEMON_INSTALL_NAME "dev.fughetta.service.${FUGHETTA_INSTANCE}.plist")
endif()
