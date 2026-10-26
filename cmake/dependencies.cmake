# The packages fughetta-service's modules build against. The repository's own build and the
# installed package (fughetta-service-config.cmake) both include this file.
#
# Everything comes from vcpkg: the manifest is vcpkg.json, the reviewed overlay
# ports are cmake/ports/, and vcpkg-configuration.json adds the registry that
# serves httpant.

find_package(xclang CONFIG REQUIRED)

find_package(stdexec CONFIG REQUIRED)
find_package(argparse CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
find_package(llhttp CONFIG REQUIRED)
find_package(nghttp3 CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(c-ares CONFIG REQUIRED)

add_compile_definitions(STDEXEC_DEMANGLE_SENDER_NAMES)

# httpant arrives as an installed package: the port in the vcpkg registry next to
# this repository builds it with HTTPANT_ENABLE_IO, and its config file hands the
# consumer the interface sources to compile (module BMIs are not portable),
# together with the targets of the C libraries they wrap (nghttp2::nghttp2, ...).
find_package(httpant CONFIG REQUIRED)
