cmake_minimum_required(VERSION 4.5)

function(run)
  execute_process(COMMAND ${ARGV} COMMAND_ERROR_IS_FATAL ANY)
endfunction()

function(capture result)
  execute_process(COMMAND ${ARGN} OUTPUT_VARIABLE output
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  set("${result}" "${output}" PARENT_SCOPE)
endfunction()

function(bundle_runtime)
  set(search ${RUNTIME_SEARCH})
  if(DEFINED ENV{CLANG_RUNTIME_DIR})
    list(APPEND search "$ENV{CLANG_RUNTIME_DIR}")
  elseif(DEFINED ENV{CLANG_ROOT})
    list(APPEND search "$ENV{CLANG_ROOT}/lib")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DSTAGE=${stage}" "-DSUFFIX=${EXE}"
    "-DSEARCH=${search}" "-DOUTPUT=${scratch}/runtime.txt"
    -P "${CMAKE_CURRENT_LIST_DIR}/runtime.cmake" COMMAND_ERROR_IS_FATAL ANY)
  file(STRINGS "${scratch}/runtime.txt" libraries)
  set(runtime "${stage}/libexec/fughetta-service/runtime")
  file(MAKE_DIRECTORY "${runtime}")
  if(CMAKE_HOST_WIN32)
    file(REAL_PATH "$ENV{SystemRoot}" windows)
    string(TOLOWER "${windows}/" windows)
  endif()
  foreach(library IN LISTS libraries)
    get_filename_component(name "${library}" NAME)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
        AND name MATCHES "^(ld-linux.*|lib(c|m|dl|pthread|rt|resolv|util|anl)[.]so[.].*)$")
      continue()
    endif()
    if(CMAKE_HOST_APPLE AND library MATCHES "^(/usr/lib/|/System/Library/)")
      continue()
    endif()
    if(CMAKE_HOST_WIN32)
      file(REAL_PATH "${library}" absolute)
      string(TOLOWER "${absolute}" absolute)
      string(TOLOWER "${name}" lower_name)
      string(FIND "${absolute}" "${windows}" system_library)
      if(system_library EQUAL 0 AND NOT lower_name MATCHES "^(msvcp|vcruntime|concrt)")
        continue()
      endif()
    endif()
    file(COPY_FILE "${library}" "${runtime}/${name}")
    list(APPEND copied "${runtime}/${name}")
    list(APPEND names "${name}")
  endforeach()

  if(CMAKE_HOST_WIN32)
    # Each executable resolves DLLs from its own directory.
    foreach(binary IN LISTS executables)
      get_filename_component(directory "${binary}" DIRECTORY)
      file(COPY ${copied} DESTINATION "${directory}")
    endforeach()
    file(REMOVE_RECURSE "${runtime}")
    return()
  endif()
  foreach(binary IN LISTS executables copied)
    get_filename_component(directory "${binary}" DIRECTORY)
    file(RELATIVE_PATH relative "${directory}" "${runtime}")
    if(NOT relative)
      set(relative ".")
    endif()
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
      # DT_RUNPATH applies to the dependencies of each object.
      run(patchelf --set-rpath "$ORIGIN/${relative}" "${binary}")
    else()
      capture(linked otool -L "${binary}")
      string(REGEX MATCHALL "\n[ \t]+[^\n]+" lines "${linked}")
      foreach(line IN LISTS lines)
        string(STRIP "${line}" name)
        string(REGEX REPLACE " [(]compatibility version.*$" "" name "${name}")
        get_filename_component(filename "${name}" NAME)
        if(filename IN_LIST names)
          run(install_name_tool -change "${name}" "@loader_path/${relative}/${filename}" "${binary}")
        endif()
      endforeach()
      if(binary IN_LIST copied)
        get_filename_component(name "${binary}" NAME)
        run(install_name_tool -id "@rpath/${name}" "${binary}")
      endif()
      run(codesign --force --sign - "${binary}")
    endif()
  endforeach()
endfunction()

function(worker_assets)
  foreach(binary IN LISTS executables)
    get_filename_component(name "${binary}" NAME_WE)
    set(target "${OUTPUT_DIR}/${name}-${platform}-${arch}${EXE}")
    file(COPY_FILE "${binary}" "${target}")
    if(NOT CMAKE_HOST_WIN32)
      set(strip llvm-strip)
      if(DEFINED ENV{CLANG_ROOT})
        set(strip "$ENV{CLANG_ROOT}/bin/llvm-strip")
      endif()
      file(CHMOD "${target}" PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
      run("${strip}" "${target}")
    endif()
    if(CMAKE_HOST_APPLE)
      run(codesign --force --sign - "${target}")
    endif()
  endforeach()
endfunction()

function(debian_package)
  set(root "${scratch}/deb")
  set(control "${root}/DEBIAN")
  file(MAKE_DIRECTORY "${control}" "${scratch}/debian")
  file(RENAME "${stage}" "${root}/usr")
  capture(architecture dpkg --print-architecture)
  # The package bundles non-system libraries. Derive the libc dependency from
  # the symbols used by the bundled objects and the two executables.
  file(WRITE "${scratch}/debian/control"
    "Source: fughetta-service\n\nPackage: fughetta-service\nArchitecture: any\n")
  set(runtime "${root}/usr/libexec/fughetta-service/runtime")
  file(GLOB native "${runtime}/*")
  list(APPEND native "${root}/usr/bin/fughetta-service"
    "${root}/usr/libexec/fughetta-service/llvm/fughetta-llvm")
  list(TRANSFORM native PREPEND "-e")
  execute_process(COMMAND dpkg-shlibdeps -O --ignore-missing-info "-l${runtime}" ${native}
    WORKING_DIRECTORY "${scratch}" OUTPUT_VARIABLE dependencies
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  string(REGEX MATCH "(^|[=,] *)libc6( \\([^)]*\\))?" libc_match "${dependencies}")
  string(REGEX REPLACE "^[=,] *" "" libc "${libc_match}")
  if(NOT libc)
    message(FATAL_ERROR "dpkg-shlibdeps did not report the libc dependency: ${dependencies}")
  endif()
  file(GLOB_RECURSE files LIST_DIRECTORIES false "${root}/*")
  set(bytes 0)
  foreach(path IN LISTS files)
    if(EXISTS "${path}")
      file(SIZE "${path}" size)
      math(EXPR bytes "${bytes} + ${size}")
    endif()
  endforeach()
  math(EXPR installed_size "${bytes} / 1024")
  file(WRITE "${control}/control"
    "Package: fughetta-service\nVersion: ${VERSION}\nArchitecture: ${architecture}\n"
    "Maintainer: FantasticFugue\nSection: devel\nPriority: optional\n"
    "Installed-Size: ${installed_size}\nDepends: ${libc}, adduser, init-system-helpers\n"
    "Homepage: https://github.com/FantasticFugue/fughetta-service\n"
    "Description: Workspace source, semantic analysis, and build service\n"
    " Includes the default workspace service and language provider hosts.\n")
  file(WRITE "${control}/postinst" [=[
#!/bin/sh
set -eu

case "$1" in
  configure)
    if ! getent passwd fughetta >/dev/null; then
      adduser --system --group --home /var/lib/fughetta-service --no-create-home fughetta
    fi
    install -d -o fughetta -g fughetta -m 0700 \
      /etc/fughetta-service/default /var/lib/fughetta-service/default
    config=/etc/fughetta-service/default/config.json
    if [ ! -e "$config" ]; then
      install -o fughetta -g fughetta -m 0600 \
        /usr/share/fughetta-service/default.json "$config"
    fi
    # Keep the account's home writable for language runtimes and build tools.
    chown fughetta:fughetta /var/lib/fughetta-service
    chmod 0700 /var/lib/fughetta-service
    if deb-systemd-helper debian-installed fughetta-service@default.service; then
      deb-systemd-helper unmask fughetta-service@default.service >/dev/null || true
      if deb-systemd-helper --quiet was-enabled fughetta-service@default.service; then
        deb-systemd-helper enable fughetta-service@default.service >/dev/null || true
      fi
    else
      deb-systemd-helper enable fughetta-service@default.service >/dev/null || true
    fi
    deb-systemd-helper update-state fughetta-service@default.service >/dev/null || true
    if [ -d /run/systemd/system ]; then
      systemctl --system daemon-reload
      if [ -n "${2:-}" ]; then
        # Restart the running workspaces that share the replaced executable.
        units=$(systemctl --system list-units --state=active --plain --no-legend 'fughetta-service@*.service' | awk '{print $1}')
        for unit in $units; do
          deb-systemd-invoke try-restart "$unit"
        done
      else
        deb-systemd-invoke start fughetta-service@default.service
      fi
    fi
    ;;
esac
]=])
  file(WRITE "${control}/prerm" [=[
#!/bin/sh
set -eu

case "$1" in
  remove|deconfigure)
    if [ -d /run/systemd/system ]; then
      deb-systemd-invoke stop 'fughetta-service@*.service'
    fi
    ;;
esac
]=])
  file(WRITE "${control}/postrm" [=[
#!/bin/sh
set -eu

case "$1" in
  remove)
    deb-systemd-helper mask fughetta-service@default.service >/dev/null || true
    ;;
  purge)
    deb-systemd-helper purge fughetta-service@default.service >/dev/null || true
    deb-systemd-helper unmask fughetta-service@default.service >/dev/null || true
    # Workspace data and its account survive removal, including purge.
    ;;
esac
if [ -d /run/systemd/system ]; then
  systemctl --system daemon-reload
fi
]=])
  file(CHMOD "${control}/postinst" "${control}/prerm" "${control}/postrm" PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  set(FUGHETTA_SERVICE_USER fughetta)
  set(CMAKE_INSTALL_FULL_BINDIR /usr/bin)
  set(CMAKE_INSTALL_FULL_SYSCONFDIR /etc)
  configure_file("${CMAKE_CURRENT_LIST_DIR}/../daemon/fughetta-service.service.in"
    "${root}/usr/lib/systemd/system/fughetta-service@.service" @ONLY)
  set(PREFIX /usr)
  set(STATE /var/lib/fughetta-service/default)
  configure_file("${CMAKE_CURRENT_LIST_DIR}/config.json.in"
    "${root}/usr/share/fughetta-service/default.json" @ONLY)
  run(dpkg-deb --root-owner-group --build "${root}"
    "${OUTPUT_DIR}/fughetta-service_${VERSION}_${architecture}.deb")
endfunction()

function(windows_package)
  file(COPY "${CMAKE_CURRENT_LIST_DIR}/windows/initialize.ps1" DESTINATION "${share}")
  run(wix build "${CMAKE_CURRENT_LIST_DIR}/windows/package.wxs" -arch "${arch}"
    -d "Stage=${stage}" -d "Version=${VERSION}" -ext WixToolset.Util.wixext
    -intermediateFolder "${scratch}/wix" -pdbtype none
    -out "${OUTPUT_DIR}/fughetta-service-${VERSION}-${arch}.msi")
endfunction()

function(macos_package)
  set(name "fughetta-service-${VERSION}-${platform}-${arch}.tar.gz")
  set(archive "${OUTPUT_DIR}/${name}")
  file(GLOB children RELATIVE "${stage}" "${stage}/*")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${archive}" --format=gnutar ${children}
    WORKING_DIRECTORY "${stage}" COMMAND_ERROR_IS_FATAL ANY)
  if(RELEASE_URL)
    string(REGEX REPLACE "/+$" "" base "${RELEASE_URL}")
    set(URL "${base}/${name}")
    file(SHA256 "${archive}" SHA256)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/homebrew/fughetta-service.rb.in"
      "${OUTPUT_DIR}/fughetta-service.rb" @ONLY)
  endif()
endfunction()

if(NOT BUILD_DIR)
  message(FATAL_ERROR "Supply -DBUILD_DIR=<configured build directory>")
endif()
if(DEFINED VERSION AND DEFINED BUILD_NUMBER)
  message(FATAL_ERROR "Supply either VERSION or BUILD_NUMBER")
elseif(DEFINED BUILD_NUMBER)
  file(STRINGS "${BUILD_DIR}/CMakeCache.txt" configured_version REGEX "^CMAKE_PROJECT_VERSION:STATIC=")
  if(NOT configured_version MATCHES "^CMAKE_PROJECT_VERSION:STATIC=([0-9]+[.][0-9]+)[.][0-9]+$")
    message(FATAL_ERROR "The build directory must contain a configured project version")
  endif()
  set(VERSION "${CMAKE_MATCH_1}.${BUILD_NUMBER}")
endif()
if(NOT VERSION MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)$")
  message(FATAL_ERROR "Supply VERSION with three numeric components or a BUILD_NUMBER")
endif()
if(CMAKE_MATCH_1 GREATER 255 OR CMAKE_MATCH_2 GREATER 255 OR CMAKE_MATCH_3 GREATER 65535)
  message(FATAL_ERROR "Package versions must fit MSI's 255.255.65535 fields")
endif()

set(EXE "")
if(CMAKE_HOST_WIN32 AND "$ENV{PROCESSOR_ARCHITECTURE}" STREQUAL "AMD64")
  set(platform win32)
  set(arch x64)
  set(EXE .exe)
elseif(CMAKE_HOST_UNIX)
  capture(machine uname -m)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND machine STREQUAL "x86_64")
    set(platform linux)
    set(arch x64)
  elseif(CMAKE_HOST_APPLE AND machine STREQUAL "arm64")
    set(platform darwin)
    set(arch arm64)
  endif()
endif()
if(NOT platform)
  message(FATAL_ERROR "Packages are built on Linux x64, Windows x64, or macOS arm64")
endif()
if(NOT OUTPUT_DIR)
  set(OUTPUT_DIR dist)
endif()
file(REAL_PATH "${BUILD_DIR}" BUILD_DIR)
file(REAL_PATH "${OUTPUT_DIR}" OUTPUT_DIR)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
string(RANDOM LENGTH 16 id)
set(scratch "${BUILD_DIR}/package-${id}")
set(stage "${scratch}/install")
set(share "${stage}/share/fughetta-service")
set(executables "${stage}/bin/fughetta-service${EXE}"
  "${stage}/libexec/fughetta-service/llvm/fughetta-llvm${EXE}")
run("${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${stage}")
file(REMOVE_RECURSE "${share}/daemon")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/config.json.in" DESTINATION "${share}")

# The initial configuration references the provider files required in the package.
set(PREFIX "${stage}")
set(STATE "")
file(READ "${share}/config.json.in" template)
string(CONFIGURE "${template}" config @ONLY)
string(JSON tool_count LENGTH "${config}" tools)
math(EXPR last_tool "${tool_count} - 1")
foreach(index RANGE ${last_tool})
  string(JSON tool MEMBER "${config}" tools ${index})
  string(JSON path GET "${config}" tools "${tool}")
  if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
    message(FATAL_ERROR "Build all language providers before packaging: ${path}")
  endif()
endforeach()

if(WORKER_ASSETS)
  worker_assets()
endif()
bundle_runtime()
if(CMAKE_HOST_WIN32)
  windows_package()
elseif(CMAKE_HOST_APPLE)
  macos_package()
else()
  debian_package()
endif()
file(REMOVE_RECURSE "${scratch}")
