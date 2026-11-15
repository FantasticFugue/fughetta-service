# Linux: io_uring and a systemd template unit.
include("${CMAKE_CURRENT_LIST_DIR}/posix.cmake")
list(APPEND PLATFORM_IMPLEMENTATION linux/posix linux/affinity)
set(HTTPANT_IO_BACKEND uring CACHE STRING "Native I/O mechanism")
set(DAEMON_DEFINITION fughetta-service.service)
set(DAEMON_INSTALL_NAME fughetta-service@.service)
set(PLATFORM_BENCHMARKS engine)
