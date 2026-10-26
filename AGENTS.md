# AGENTS.md — fughetta-service

This repository builds one C++ executable, `fughetta-service`. Its gateway
process manages source and worker child processes. The console, the marketing site and the design system live in
their own repositories and are not built here.

## Where to look first

| Question | File |
| --- | --- |
| Usage, build, and service layout | `README.md` |
| The current worldline protocol | `../pwa-coding-agent/.work/worldline-protocol.md` |
| Per-directory conventions | the nearest `AGENTS.md` below this one |

## Dependencies

Dependencies come from vcpkg. `vcpkg.json` is the manifest, and
`vcpkg-configuration.json` loads the current `../vcpkg-registry/ports` overlays.
The `httpant[io,tls]` port installs the current sibling `../httpant` source tree.
The package uses Asio unless the port's `uring` feature selects io_uring and
installs liburing.
Consumers use `find_package(httpant CONFIG REQUIRED)` and the installed
`httpant::httpant`, `httpant::io`, `httpant::tls`, and `httpant::imports` targets.

Update existing port entries through `../vcpkg-registry/add-port-version`.
Creating versions requires explicit user approval. Source changes enter
httpant's cache identity through `triplets/httpant-source.cmake`.

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
- Runtime constants live in `src/core/constants.cppm`.
- `src/fughetta-service.cpp` is the executable entry. `src/service/` holds
  `gateway`, `source`, `worker`, and the process `runtime`.
- `allocation` owns the shared budget. Production pools and subprocess work
  must use that allocation.

## What is not here any more

The front ends (console, admin, pwa-coding-agent, landing and the component
library) are
their own repositories beside this one; see `README.md`. Frontend work does not
belong here. An API change a front end needs is a change to `/api/v1` or `/mcp`,
recorded in that front end's own repository.
