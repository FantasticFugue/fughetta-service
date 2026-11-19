# Windows: IOCP. Only this provider includes <windows.h>, and every executable
# that links the platform layer gets an 8 MiB main-thread stack.
set(PLATFORM_FAMILY windows)
set(PLATFORM_MODULES windows/winapi)
set(PLATFORM_IMPLEMENTATION windows/create windows/machine windows/console windows/group windows/flush)
set(PLATFORM_DEFINITIONS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C)
set(PLATFORM_LINK_OPTIONS "LINKER:/STACK:8388608")
set(PLATFORM_LIBRARIES advapi32)
set(HTTPANT_IO_BACKEND iocp CACHE STRING "Native I/O mechanism")
# Gradle installs both launchers. The Windows launcher runs through cmd.exe.
set(GRADLE_LAUNCHER gradle.bat)
set(GRADLE_LAUNCH_PREFIX "$ENV{ComSpec}" /d /c call)
