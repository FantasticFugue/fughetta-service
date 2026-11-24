# Windows: IOCP. Only this provider includes <windows.h>, and every executable
# that links the platform layer gets an 8 MiB main-thread stack.
set(PLATFORM_FAMILY windows)
set(PLATFORM_MODULES windows/winapi)
set(PLATFORM_IMPLEMENTATION windows/create windows/machine windows/console windows/group windows/flush)
set(PLATFORM_DEFINITIONS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C)
set(PLATFORM_LINK_OPTIONS "LINKER:/STACK:8388608")
set(PLATFORM_LIBRARIES advapi32)
set(HTTPANT_IO_BACKEND iocp CACHE STRING "Native I/O mechanism")
# Prefer native launchers and run batch wrappers through the command interpreter.
set(GRADLE_LAUNCHERS gradle.exe gradle.bat gradle.cmd)
set(GRADLE_BATCH_PREFIX "$ENV{ComSpec}" /d /c call)
set(HOST_MODULES windows/transport windows/network windows/firewall windows/job windows/backend containerd protobuf)
set(HOST_LIBRARIES httpant::imports computenetwork ole32 virtdisk computestorage fwpuclnt userenv uuid delayimp)
set(HOST_LINK_OPTIONS "LINKER:/DELAYLOAD:computenetwork.dll" "LINKER:/DELAYLOAD:computestorage.dll"
    "LINKER:/DELAYLOAD:virtdisk.dll" "LINKER:/DELAYLOAD:fwpuclnt.dll" "LINKER:/DELAYLOAD:userenv.dll")
set(HOST_SERVICE managed)
