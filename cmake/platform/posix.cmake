# The POSIX provider Linux and macOS share; each adds its own units after it.
set(PLATFORM_FAMILY posix)
set(PLATFORM_MODULES posix/descriptor posix/posix)
set(PLATFORM_IMPLEMENTATION posix/uname posix/sigwait posix/create posix/flush)
set(GRADLE_LAUNCHERS gradle)
