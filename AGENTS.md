# AGENTS.md — fughetta-service

This repository builds one C++ executable, `fughetta-service`. Its gateway
process manages source and worker child processes. The console, the marketing site and the design system live in
their own repositories and are not built here.

## Where to look first

| Question | File |
| --- | --- |
| Usage, build, and service layout | `README.md` |
| The current worldline protocol | `tools::worldline::input` in `src/gateway/tools.cppm`, exposed by `src/gateway/routes.cppm` |
| Per-directory conventions | the nearest `AGENTS.md` below this one |

## Dependencies

Third-party dependencies come from vcpkg; `vcpkg.json` is the manifest and
`vcpkg-configuration.json` pins the builtin registry. httpant is fetched from
the head of its main branch by `FetchContent` in `cmake/dependencies.cmake` and
built as a subproject, so its module BMIs are compiled by this build's compiler;
the manifest lists its dependencies too. `FetchContent` stays until httpant has
a vcpkg port. A local checkout replaces the fetched one through
`FETCHCONTENT_SOURCE_DIR_HTTPANT`. The service selects io_uring
on Linux, IOCP on Windows, and kqueue on macOS. The layers link
`httpant::httpant`, `httpant::io` and `httpant::tls`.

`cmake/toolchain.cmake` reads `XCLANG_ROOT` and `VCPKG_ROOT` from the
environment and writes the vcpkg triplet for `XCLANG_TARGET` into the build
directory. CI checks vcpkg out at the baseline in `vcpkg-configuration.json`,
which must be a microsoft/vcpkg commit.

## Build and verify

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

`async` imports the execution definitions exported by `httpant.io`. Native
resources and networking senders belong to that package; `native` adds Fughetta's
placement and reconnect policy. HTTP code awaits `http::coroutine` directly.
The application task promise uses `as_awaitable` to propagate cancellation
across that coroutine boundary. TLS uses `httpant.tls` over `web::stream` and
native sockets. The local `tls` module parses Worker gateway addresses. Keep
certificate rejection typed and send `close_notify` after normal session ends;
WebSocket peer closure reaches the jobs reader as `io::closed`.

Every source file must be reachable from a target in `CMakeLists.txt` or
`tests/CMakeLists.txt`. A `.cppm` that no target lists is dead code: either wire it in or delete it. `git ls-files 'src/*.cppm'`
and the module registrations in `CMakeLists.txt` must agree.

## Source rules

- Modules are `src/<layer>/<name>.cppm`; a single word, no `source_` prefix.
- An implementation unit sits beside its interface and is named for what it
  implements (`database.cppm` implements `local_node`).
- Each `src/<layer>/` directory is one CMake target (`cmake/modules.cmake`).
  Layers depend in one direction: core, platform, io, model, then gateway,
  source and worker, then service. A new import inside a layer needs no CMake
  change; an import that goes against this order is a design error.
- Runtime constants live in `src/core/constants.cppm`.
- `src/fughetta-service.cpp` is the executable entry. `src/service/` holds
  `gateway`, `source`, `worker`, and the process `runtime`.
- `allocation` owns the shared budget. Production pools and subprocess work
  must use that allocation.

## What is not here any more

The front ends (console, admin, agent-cloud, landing and the component
library) are
their own repositories beside this one; see `README.md`. Frontend work does not
belong here. An API change a front end needs is a change to `/api/v1` or `/mcp`,
recorded in that front end's own repository.
