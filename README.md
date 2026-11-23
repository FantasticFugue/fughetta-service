# fughetta-service

Build and language services for coding agents, exposed over MCP.

Fughetta runs builds, tests, runs, and language analysis on remote workers with
configured toolchains—driving build systems, package managers, test runners, and
language servers through their published report formats—and returns structured
diagnostics and test results instead of terminal scrollback. Capability is
fine-grained (one provider per toolchain), while the agent-facing interface
stays a few MCP tools.

Orchestrators configure projects and bind tasks to server-side worldlines.
Agents use task execution credentials through `/mcp` to read and edit source,
inspect syntax, build and test a fixed revision, and retrieve its outputs. Source stays on the service; a local checkout or
`fug push` is not required. Agents share incremental worker trees and a queue
of deduplicated, cached jobs.

The `fughetta-service` executable runs a gateway process that manages one
source process and a worker pool for one workspace. The Console Worker serves
the hosted gateway for accounts, sign-in, billing, and workspace routing.

The front ends are static apps in separate repositories and share the
`@fantasticfugue/components-vue` package:

| Repository | What it is |
| --- | --- |
| `console` | Customer workspace UI and the `/admin` operator UI for managing users, workspaces, and the hosted gateway |
| `landing` | Marketing pages, docs, blog, pricing and legal, prerendered |

## Dependencies and build

Requires CMake 4.5+, Ninja, vcpkg, and clang 23 or later. Linux and macOS compile against libc++;
Windows uses upstream clang-cl with the MSVC STL and the Windows SDK of a Visual Studio installation.
The service defaults to `epoll` on Linux during development (deployments select `uring`, kernel 6.1 or later), `iocp` on Windows, and `kqueue` on
macOS. Select another mechanism with `-DHTTPANT_IO_BACKEND=NAME` when configuring the service.
The manifest installs liburing on Linux.
httpant is fetched from the head of its main branch with `FetchContent` until
it has a vcpkg port, and built as a subproject, so its module BMIs use this
build's compiler; set
`FETCHCONTENT_SOURCE_DIR_HTTPANT` to build a local checkout instead. The
remaining dependencies come from the builtin vcpkg registry at the
microsoft/vcpkg baseline in `vcpkg-configuration.json`; the vcpkg checkout must
contain that commit. The toolchain writes the dependency triplet for the host
into the build directory, so dependencies build with the same clang and flags.
`cmake/toolchain.cmake` reads two environment variables: `CLANG_ROOT` names a
clang tree to compile with and `VCPKG_ROOT` the vcpkg checkout. Without
`CLANG_ROOT` the platform's clang is used, and a `CMAKE_CXX_COMPILER` given to
CMake wins over both; either way CMake 4.5's `import std` support synthesizes
the std module from the standard library's manifest. vcpkg's downloads stay in the build
directory unless `VCPKG_DOWNLOADS` names a shared one. Each target system has a
description in `cmake/platform/<system>.cmake` (platform units, I/O backend,
service-manager template) and its own tests in `tests/platform/<system>.cmake`;
porting to a new system starts with those two files. `CMakePresets.json`
places each build in `build/<preset>`; machine paths belong in a Git-ignored
`CMakeUserPresets.json` that inherits its presets.
Build products and dependency working directories belong under `build/`.

From this directory:

```sh
cmake --preset debug
cmake --build --preset debug --target fughetta-service
```

To run the HTTP front suite from a regular developer shell:

```sh
cmake --build --preset debug --target gateway.front-test
ctest --preset debug -R '^gateway\.front$'
```

Run CTest as the current user.

The debug preset builds with address and undefined-behavior sanitizers.
`CLANG_SANITIZER` selects the sanitizers, with their matching runtimes, and
adds frame pointers so sanitizer stacks stay symbolized by function name.
`release` disables tests and sanitizers and enables benchmarks and
interprocedural optimization.

## Nightly builds

The nightly workflow builds `main` on each push and every day at 19:00 UTC
(03:00 Asia/Taipei). It can also be started manually on `main`.
Release builds run on native Linux x64, Windows x64, and macOS arm64 runners,
producing these assets:

- `fughetta-service-linux-x64`
- `fughetta-service-darwin-arm64`
- `fughetta-service-win32-x64.exe`

All three builds must succeed before publication. The workflow uploads the binaries
to a draft release, then publishes it as Latest. A failed build leaves the
previous published release available. Nightlies are development builds; they are
published without GitHub's prerelease flag so the CLI's default `releases/latest`
endpoint returns them.

Run `fug worker` to download the build for the current platform automatically.
Each run uses a distinct `nightly-<run-id>-<attempt>` tag and names the source
commit in its release notes. The CLI caches downloads by tag, so a new nightly
gets a new cache directory without manual removal. To use a binary you downloaded
yourself, pass its path:

```sh
fug worker --service /path/to/fughetta-service
```

## Native packages

Nightly releases also include a Debian amd64 package, a Windows x64 MSI, and a
macOS arm64 archive with its Homebrew formula. Package versions are
`0.1.<workflow run number>`. Packages install the full CMake installation tree,
including the JVM, CLR, LLVM, and Python provider files and native runtime
libraries. Java 25 and .NET 10 are needed when using the JVM and CLR providers;
project build tools remain separate installations.

Each package prepares a `default` workspace listening at `http://127.0.0.1:8080`,
with internal listeners on ports 9090 and 9091. Its configuration uses token
authentication and maps the installed provider paths. Resource allocation uses
the service's existing host-based defaults. Configuration and workspace data
survive upgrades and uninstall, including Debian purge.

| Package | Installation and startup | Configuration and data |
| --- | --- | --- |
| deb | `sudo apt install ./fughetta-service_VERSION_amd64.deb` creates the `fughetta` account and starts `fughetta-service@default`. | `/etc/fughetta-service/default/config.json`; `/var/lib/fughetta-service/default` |
| MSI | Run the installer as administrator. It registers and starts `fughetta-service-default` under LocalService. | `%ProgramData%\Fughetta\default\config.json`; `state` and `logs` beside it |
| Homebrew | Install the release formula in a tap, then run `brew services start fughetta-service`. It runs as the signed-in user. | `$(brew --prefix)/etc/fughetta-service/default/config.json`; `$(brew --prefix)/var/lib/fughetta-service/default` |

Replace `VERSION` with the downloaded package's version. Debian upgrades restart
running workspace instances; MSI upgrades stop and replace the installed service.
Homebrew uses its `opt` path so the service and provider mappings stay at the
same paths across upgrades. After a Homebrew upgrade, run
`brew services restart fughetta-service` to load the new executable.

For a local Homebrew tap, download `fughetta-service.rb` from the release, then:

```sh
brew tap-new local/fughetta
cp fughetta-service.rb "$(brew --repo local/fughetta)/Formula/fughetta-service.rb"
brew install local/fughetta/fughetta-service
brew services start fughetta-service
```

Replace the tap's formula with the one from a newer release before running
`brew upgrade fughetta-service`. The release formula is also ready to place in a
published tap. Publishing an APT repository, a Homebrew tap, or a WinGet manifest
is a separate distribution operation; the build produces the installable files.

Create an owner token once before using the API. Stop the service while changing
its configuration, run `fughetta-service token local owner CONFIG_PATH`, save the
printed token, and start the service again. On Debian, run the token command as
the service account (`sudo -u fughetta`); on Windows, use an administrator shell
and the executable under `%ProgramFiles%\Fughetta\bin`; on Homebrew, use the same
account as `brew services`. Use `systemctl`, `Restart-Service`, or `brew services`
for subsequent service operations. Additional workspace registration follows the
native instance instructions below.

Packaging is contained in `cmake/packaging/` and runs after building; it does not
change the service sources or the regular CMake install rules. To build packages
locally, make all provider targets with Java 25, Gradle, and the .NET SDKs described
above available at configuration time, then run:

```sh
cmake --preset release
cmake --build --preset release --target fughetta-service llvm-provider clr-providers jvm-providers --parallel 2
cmake -DBUILD_DIR=build/release -DOUTPUT_DIR=dist -DVERSION=0.1.0 -P cmake/packaging/package.cmake
```

Linux packaging uses `dpkg-deb`, `dpkg-shlibdeps`, and `patchelf`. Windows packaging
uses CMake, WiX and its `WixToolset.Util.wixext` extension;
set `CLANG_ROOT` to the compiler installation for `llvm-objdump`. On macOS, add
`-DRELEASE_URL=https://github.com/OWNER/REPO/releases/download/TAG` so the generated
formula points to the archive's published location. `-DRUNTIME_SEARCH=PATH` adds
a native library search directory. Temporary package files stay under the build
directory.

## Service lifecycle and workspace boundary

One workspace shares an execution environment across its projects and repositories.
The service admits and schedules jobs within the workspace budget, owns source and
Worker processes, and handles cancellation and shutdown. A self-hosted service runs
directly in the environment supplied by the operator. Core and Worker roles use
the isolation supplied by their deployment. For managed environments,
the Console Worker coordinates placement and the Linux [Host Executor](#host-executor)
enforces isolation and hard quotas.

### Native lifecycle design

The application publishes `starting`, `initialized`, `ready`, `draining`,
`stopping`, and `stopped` through the platform `daemon` interface. Initialization
completes after source and worker children start; readiness follows worker
registration and task admission. `/healthz` returns `lifecycle` and `instance`
alongside source and worker observations. Process existence does not imply readiness.
Budget changes publish draining and return to ready after rebuilding children;
they never report a final native stop. Final shutdown closes admission, allows
up to five seconds for accepted work, then cancels work and gives children ten
seconds to exit before killing them. Native definitions allow twenty seconds.
A startup stop cancels initialization. Unexpected child exits fail the process.

Each process has one direct restart owner. On bare hosts and within VMs, the
native service manager owns the service. In containers, run it as the foreground
main process, send SIGTERM, allow twenty seconds, and let the container runtime
own restarts; do not also run a native manager in that container. The service
owns its source and worker children. The cloud gateway coordinates environment
recovery with the Host Executor, which owns containerd provisioning on the host.

#### Instance registration and operation

Use a stable workspace slug of letters, digits, underscores or hyphens as
`--instance NAME`. The process identity and Windows SCM name are
`fughetta-service-NAME`. Without the option, foreground use has identity
`fughetta-service`. Every same-host workspace needs its own configuration,
storage, working directory, logs and listener ports, including internal source
and worker ports. `server.storage` should be absolute. Do not share writable
workspace directories. Create the service account and directories before
registration, grant only that account access to configuration secrets and data,
and keep the installed executable read-only. Registration requires host operator
privileges and is separate from `cmake --install`.

- **Linux / systemd:** installation supplies `fughetta-service@.service`. Copy it
  to `/etc/systemd/system/`, place configuration at the configured installation
  sysconf directory under `fughetta-service/NAME/config.json`, then run
  `systemctl daemon-reload` and `systemctl enable --now fughetta-service@NAME`.
  The template uses `Type=notify`, `NotifyAccess=main`, `READY=1` after admission
  opens, and `STOPPING=1` when final cancellation begins. `STATUS` reports
  application transitions. `StateDirectory` creates `/var/lib/fughetta-service/NAME`;
  stdout/stderr go to journald. `Restart=on-failure` owns recovery and
  `KillMode=mixed` gives the application the first opportunity to reap children.
  `FUGHETTA_SERVICE_USER` chooses the account and group at configure time.
  Stop with `systemctl stop fughetta-service@NAME`; disable before removal.
- **macOS / launchd:** configure with `-DFUGHETTA_INSTANCE=NAME` and optionally
  `-DFUGHETTA_SERVICE_USER=ACCOUNT`. Installation supplies
  `dev.fughetta.service.NAME.plist`. Copy it into `/Library/LaunchDaemons/`,
  owned by root and not writable by the service account, then run
  `launchctl bootstrap system /Library/LaunchDaemons/dev.fughetta.service.NAME.plist`.
  Pre-create `/var/lib/fughetta-service/NAME` and `/var/log/fughetta-service`
  with appropriate ownership, and the configured per-instance config path.
  launchd runs the foreground process, sends SIGTERM, and restarts failed exits;
  application readiness is available from `/healthz`. Logs use per-instance
  `.out.log` and `.err.log` files in `/var/log/fughetta-service`; arrange rotation.
  Remove with `launchctl bootout system/dev.fughetta.service.NAME` before deleting
  its plist. A configure without an instance does not emit a LaunchDaemon.
- **Windows / SCM:** pre-create a dedicated data directory, configuration and log
  directory, granting the service account access. Register an own-process service
  named `fughetta-service-NAME` with an absolute, correctly quoted command line:
  `"C:\Fughetta\fughetta-service.exe" --service --instance NAME --log-dir C:\FughettaData\NAME\logs C:\FughettaConfig\NAME.json`.
  Use `sc.exe create` with that `binPath` and `obj` set to the intended service
  identity, then `sc.exe start fughetta-service-NAME`. Configure SCM failure
  actions as the sole process restart owner. The host reports START_PENDING
  with progress checkpoints until application readiness, RUNNING afterwards,
  STOP_PENDING on stop/shutdown controls, and STOPPED with the application result.
  Pending states have wait hints and periodic checkpoints; INTERROGATE returns
  current status. Use absolute paths throughout: SCM does not provide a workspace
  working directory. `--log-dir` appends stdout/stderr to `service.out.log` and
  `service.err.log`; arrange rotation. Stop with `sc.exe stop` and remove with
  `sc.exe delete` using the same instance name. The Windows lifecycle adapter was built with
  xclang 23.1.3.6 on a Windows host and passed native SCM startup, interrogation,
  and shutdown tests. Windows also passes the service test suite and the CLI Worker
  connection flow.

Native references: [systemd notifications](https://www.freedesktop.org/software/systemd/man/latest/sd_notify.html),
[SCM state transitions](https://learn.microsoft.com/en-us/windows/win32/services/service-status-transitions),
and [launchd hosting](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html).

## Host Executor

Run the Host Executor as a privileged Linux service with an owner-only JSON
configuration file.

```sh
fughetta-service --role host /etc/fughetta/host.json
```

Set the following fields inside `host`.

- `host_id` identifies the static host entry in the cloud gateway.
  `capacity` advertises `cpu_millis`, `memory_bytes`, and `storage_bytes`.
  The gateway reserves capacity for the workspace's Core and overflow lanes.
- `bind` and `port` default to `127.0.0.1` and `8090`. Use a loopback
  or private IP address reachable through the gateway's private network.
  Set `token` to the host API bearer secret and `front_token` to the separate
  bearer secret the gateway sends to provisioned Cores.
- `socket` defaults to `/run/containerd/containerd.sock`. Set `name_space`
  to a reserved containerd namespace and `storage` to a dedicated directory
  for journals, volume images, and workspace Worker credentials.
- `rootfs` names the base filesystem directory. `toolchain_root` and
  `work_root` bound the toolchain and work volume paths the gateway can
  request. Each work volume uses its own empty mount point within
  `work_root`. Set optional `shared_cache` to a host directory and `tools`
  to a map of tool names to absolute paths inside the container.
- `volume_devices` bounds attached work volume device paths and defaults to
  `/dev/disk/by-id/`. Keep this directory and its device links under host
  administrator control.
- `isolation` has `runtimes`, `exclusive`, and `devices`. The runtime list
  names the installed shims. An exclusive host binds its placement to one
  workspace and that workspace's overflow Worker. Each device requested by a
  workload must be in the host list.
- `mapped_id` defaults to 65534 and maps workload root to an unprivileged host
  UID and GID. `pids` defaults to 512. `timeout_seconds` limits individual
  host utilities and defaults to 60. `operation_timeout_seconds` limits each
  provisioning attempt and defaults to 300.

The executor uses cgroup v2 for CPU, memory, swap, PID, and device policy. It
builds an OCI spec with isolated namespaces, a seccomp policy, empty capability
sets, and a read-only root filesystem. A `sandbox` selects
`io.containerd.runsc.v1` and a `vm` selects `io.containerd.kata.v2` when
offered. A shared-kernel `container` requires `io.containerd.runc.v2` on an
exclusive host. A VM placement on an exclusive host may use runc when Kata is
absent because the dedicated host supplies the workspace boundary.

Install `/usr/local/bin/fughetta-service` and its runtime dependencies in the
base filesystem. Build the service inside the root filesystem with
`HTTPANT_IO_BACKEND=epoll` or `asio` for the OCI seccomp policy. Include
mount targets `/proc`, `/dev`, `/etc/hosts`, `/work`,
`/opt/fug/toolchain` and, when configured, `/opt/fug/shared`. The toolchain
and shared cache mount read-only. The executor preallocates an ext4 image at
the requested `storage_bytes` limit, with at least 16 MiB including filesystem
overhead, and mounts it read-write at `/work`. It uses `fallocate`,
`mkfs.ext4`, `mount`, `umount`, `ip` and `nft`. Stop removes the
container and unmounts the volume before releasing its durable writer
reservation. The image and workspace data persist.

A start operation can supply `work_volume` with `device` and `size_bytes`.
For a Hetzner Volume, use its `/dev/disk/by-id/scsi-0HC_Volume_<id>` path.
The host waits up to `timeout_seconds` for the block device and uses its full
capacity for `/work`, less filesystem overhead. The device must meet both
`size_bytes` and the environment's `storage_bytes` limit. The host uses
`blkid` to identify signatures, creates ext4 on blank devices, and mounts
existing ext4 filesystems with their data intact. Other signatures and a
device mounted for another environment return 409. Stop and destroy unmount
the device and preserve its data. Start with the same volume on a replacement
host to reopen the workspace.

`network: none` supplies loopback. `network: private` adds a veth link and
nftables rules. Only IPv4 TCP destinations from the host or operation
`egress` lists are available for new outbound connections. Use `host:port`
entries. The executor resolves names when starting and publishes their
addresses in the workload's hosts file. Set `network_prefix` to the first
two octets of a nonoverlapping private /16, defaulting to `10.203`. Use host
IPv4 forwarding for routed gateway traffic, overflow Workers and destinations
beyond the local veth peer. Set `gateway_sources` to the gateway IPv4
addresses and route the workspace prefix through the host. The Core's HTTP and
personal Worker listeners accept those gateway sources and the local host.
Enable `remote_worker_enrollment` to enroll personal Workers. The paired
overflow Worker and claimed pool Workers can reach their Core's native
execution listener through its private redirect.

`GET /healthz` answers 200. All other requests require
`Authorization: Bearer <host token>`. `POST /v1/operations` accepts the
`start_stop` operation record for either lane and returns its observation.
When the answer would exceed 90 seconds, it returns 202 with
`{"operation_id": "..."}` and provisioning continues. Poll
`GET /v1/operations/{operation_id}` for the observation or 202 while the
operation runs. Unknown IDs return 404, invalid requests return 400, missing or
wrong tokens return 401, and journal conflicts return 409. Replaying a
completed operation returns its recorded observation. Reposting an unchanged
operation that failed retries its durable intent. `GET /v1/inventory`
returns the host ID, capacity, latest environment observations, and a `warm`
list with each available Worker's environment ID, profile, and state. A
running environment with a block device reports its `work_volume` path and
actual device size in bytes.

The controller chooses pool sizes and sends pool operations for Worker
profiles. `warm` starts a Worker with a private scratch volume, empty egress,
and no workspace, Core endpoint, or credentials. `claim` attaches the volume
at the workspace hot-slot path and opens egress to the Core's numeric IPv4 TCP
endpoint. The host then publishes the workspace credential and Core address
to the waiting Worker through its read-only handoff mount. The same task
continues as the workspace's managed Worker. A second assignment returns
409. `destroy` stops the Worker and removes its container, volume, and
network. The journal retains completed observations for replay.

Pool IDs use `warm-` followed by 64 lowercase hexadecimal digits. Operation
IDs append `-1` for warm, `-2` for claim or unclaimed destroy, and `-3` for
claimed destroy. A claimed Worker leaves the pool as soon as the host records
the claim.

Run the `host.api`, `host.oci`, `host.containerd`, and `host.volume` CTest
suites on a Linux test host. The containerd suite uses noninteractive sudo
and the local runc runtime to exercise mounts, quotas, egress, fencing, and
admission of a claimed Worker by a native Core. The volume suite also uses a
loop device to check data persistence and filesystem preservation. OCI unit
tests cover runsc and Kata selection.

## Run

For self-hosting, set `server.auth.mode` to `"token"` in your configuration;
`"trusted"` is for cores behind an authenticated hosted gateway. Create an owner
token, keep its printed value, and start the service:

```sh
build/debug/fughetta-service token local owner config.json
build/debug/fughetta-service config.json
```

The main process loads `.env` beside the configuration, generates the internal
source secret when `SOURCE_TOKEN` is unset, and starts source before the workers.
The HTTP listener serves requests after child startup. A child failure stops
the service with an error. Shutdown cancels work, signals children, and waits
up to ten seconds before terminating a remaining child. Linux child processes
also terminate when their parent dies.

To run the Core and Web Console as native processes on one machine, follow the
[Console self-hosting guide](../fughetta-console/README.md#single-machine-self-hosting).
The launcher initializes a token-authenticated Core and uses the service's
native process orchestration.

For a token-authenticated Core listening at `127.0.0.1:18100`, connect an HTTP
MCP client to `http://127.0.0.1:18100/mcp` with `Authorization: Bearer <token>`.
REST routes use the same token under `/api/v1`; `/healthz` is public. A
`tools/call` whose `Accept` includes `text/event-stream` is answered as an SSE
stream with a comment keepalive every 15 seconds, so proxies with idle
limits (Cloudflare closes after 100 seconds) keep long builds open; progress
notifications are added when the call includes a `progressToken`. Other requests
get a single JSON body.
Workspace subscriptions use `GET /api/v1/events` to receive server-sent events.
The Console Worker relays these subscriptions through
`GET /w/{workspace}/api/v1/events`.
`--help` prints the public commands. The `--role` arguments are internal process
entry points used by the supervisor and process tests.

Hosted cores set `server.auth.mode` to `"trusted"`, `server.auth.workspace` to
their immutable workspace ID, and `server.auth.front_token` to a secret shared
with the hosted gateway. The gateway sends the secret as
`Authorization: Bearer <front_token>` on every request except `GET /healthz`.
It replaces the client's authorization and caller headers with its own.
Core API requests use the gateway's caller headers and must include a matching
`x-fughetta-workspace`, including requests with the host role. Set the secret
in an owner-only Core configuration file. The [Host Executor configuration](#host-executor)
describes the corresponding secret for managed environments.

The gateway resolves a run through `POST /api/v1/executions/identity`, which
returns `x-fughetta-execution: scoped` on success. For task MCP requests it sends
the run credential in `x-fughetta-execution` alongside the front bearer and
workspace caller headers. The Core checks the credential against the source on
each request and binds tool calls to the resolved run, project, and worldline.

`fug worker` connects to the public `/w/{workspace}/workers/tunnel` WebSocket
and relays the local Worker process over loopback TCP. A Core operator can
connect a Worker directly on the private network by setting `gateway` to
`ws://127.0.0.1:9091/workers/tunnel` and set `front_token` in its private
configuration when the Core uses trusted mode.

`native::context` and `runtime::context` select `concurrent::queue` from the
`mpmc` core module for native submissions and blocking file work. Native TCP,
UDP, file, scheduler, and process types use the same choice. The installed
package exports this module. HTTP session queues retain
httpant's unsynchronized local default because one context thread owns them.

The queue uses a bounded Vyukov MPMC ring with 4,096 cells. Producers and
consumers reserve positions with relaxed atomic compare/exchange; acquire
loads and release stores on each cell's sequence order payload publication
and cell reuse. A full or unavailable cell returns `false` immediately, and
httpant reports `no_buffer_space`; there is no locking path on saturation.
A producer paused after reservation can delay visibility at the head.
The `mpmc` test uses four producers and four consumers, checks 400,000 unique
values through a 64-cell ring, and checks full, empty, and reuse transitions.

The `websocket` stream presents opaque bytes to the existing jobs channel,
splits output into 16 KiB binary messages, handles ping/pong and close, and
waits for transport writes to apply backpressure. `resolver` drives cancellable
c-ares queries on the native scheduler. `httpant.tls` wraps `web::stream` over a
native socket and bounds pending ciphertext at 128 KiB. Native TLS sessions
send `close_notify` after a normal end. Certificate rejection uses
`http::tls::verification_failure`; the local `tls` module retains URL parsing. Tree coordination state is released after its last active
or waiting build; Core scheduling retains at most 256 worker-reported slots per
Worker. See [Connect a self-hosted Worker](#connect-a-self-hosted-worker) for
enrollment and tunnel configuration.

## Clients and hosts

The `fug` command line client lives in its own repository,
[fughetta-cli](../fughetta-cli). It talks to a core directly or through a host that fronts
several cores. This is the whole contract a host offers to clients:

* **Workspace prefix.** `{origin}/w/{workspace}/...` reaches `/...` on that
  workspace's core: `/api/v1`, `/mcp`, `/mcp/config` and `/healthz`. A core
  reached directly has no prefix.
* **Bearer tokens.** Every request sends `Authorization: Bearer sk_...`.
  A core issues its own tokens (`fughetta-service token`); a host issues
  account tokens and authorizes each request before it reaches the core.
* **Sign-in.** `GET {origin}/.well-known/oauth-authorization-server` returns
  RFC 8414 metadata. Clients use the authorization code grant with PKCE
  (`S256`, `client_id=fug`, no client secret) and a loopback redirect
  `http://127.0.0.1:{port}/...`. The authorization request includes
  `response_type=code`, `code_challenge`, `code_challenge_method=S256`,
  `redirect_uri` and `state`. `POST {token_endpoint}` with a form body
  (`grant_type=authorization_code`, `code`, `code_verifier`, `redirect_uri`,
  `client_id`) returns `{"access_token": "sk_...", "token_type": "Bearer"}`.
* **Workspaces.** `GET {origin}/api/v1/workspaces` returns a JSON array of the
  workspaces the token may use, each with its `name` (the slug used in the prefix),
  `workspace_id` and `role`.

A core alone offers no sign-in or workspace listing; clients take its token
from the user and use no prefix.

A client whose write lost its reply settles it with
`POST /api/v1/projects/{project}/operations/{operation_id}/abandon`. The
answer is `{"outcome": "applied", "version", "receipt"}` when the operation
ran, or `{"outcome": "abandoned"}`, after which the operation never runs: its
request, arriving later, fails with `identity_conflict`, and so does a
`receipt` for it. Agents recover with
the worldline tool's `receipt` instead; this route is not one of their tools.

## Signed execution receipts

Registered remote-tunnel Workers derive an Ed25519 signing key from their
enrollment secret. The Core registry keeps the public key with the Worker
identity. At publication, the Core checks each receipt against the authenticated
connection, the workspace, the pinned source snapshot, and the submitted
recipe and output. Missing or invalid receipts reject the submission.
Revoking a Worker disables acceptance of new receipts from that identity.

The signed payload is a compact JSON array in this order:

```text
["fughetta/worker-execution-receipt", workspace_id, worker_id, snapshot,
 recipe_digest, subject_kind, subject_id, execution]
```

The public key and signature use lowercase hexadecimal. For
`semantic_shard`, the subject is the ID of the Worker-submitted shard and
`execution` binds the receipt to the analysis request. The Core records the
published shard ID alongside the receipt because publication resolves local
symbol IDs. Pass `execution_receipts: true` to the MCP `language` tool with a
view to read `shard_receipts` and `artifact_receipts`. The audit follows the
view's retention policy.

For `artifact_manifest`, the subject is the SHA-256 digest of the exact
`manifest` string returned with the audit record. That manifest describes
measured outputs, exports, project models, step logs, and prepared analysis
inputs. Build results and the retained build history at `GET /api/v1/builds`
expose `artifact_receipts` with the public key, signed payload fields, signature
and manifest.

## Public semantic shards

A Core can reuse public records through a shared trust domain. A unit qualifies
when its provider records a complete input set before evaluation. Every input
must be a digested tool, SDK, platform input, or public package. Workspace
sources, files, build artifacts, generated files, project models, and observed
dependencies on workspace records rule out publication. The published records
must form a closed graph, including their symbol and type references. An
eligible unit submitted with a verified receipt from a Worker enrolled with
the publishing Core can be uploaded after its workspace view is stored.

The JVM reader can evaluate an installed SDK archive or class file with
`jvm-artifact` options such as
`{"module":"java.logging","sdk":"jdk","sdk_archive":"jmods/java.logging.jmod"}`.
Configure that SDK on the Worker using `sdk_manifests` and omit the stage's build
and semantic input bindings. The reader hashes the archive before evaluation
and records the digest of the embedded reader. A hit supplies the records
without parsing the archive. SDK archives produce one shard per unit.

The cache looks up `index/<key_digest>` with one GET. The key is the SHA-256 of a
compact JSON array containing the public shard domain, stage, provider,
operation, unit kind and name, recipe options digest, and complete public
input set sorted by kind, name, and digest. A valid index points to
`shards/<shard_id>`, the compressed encoded shard. The publisher signs this
fixed-order compact JSON array with Ed25519:

```text
["fughetta/public-shard", key_digest, shard_id, shard_digest, input_set, publisher]
```

`shard_digest` hashes the compressed bytes. Publisher keys and signatures use
lowercase hexadecimal. R2 requests use path-style URLs, with the bucket
name before the object key, and AWS Signature Version 4 for authenticated
requests. Both PUTs use conditional creation, with the shard written first. A Core verifies the configured publisher, the key and input
set, and the shard bytes before importing the records through workspace
publication.

Add `public_shards` at the top level of the service configuration to enable the
feature:

```json
{
  "public_shards": {
    "endpoint": "https://ACCOUNT_ID.r2.cloudflarestorage.com",
    "bucket": "fughetta-public-shards",
    "trusted_publishers": ["PUBLISHER_ED25519_PUBLIC_KEY_HEX"],
    "shared_cache": "/opt/fug/shared",
    "cache_bytes": 1073741824
  }
}
```

Reads try the optional read-only `shared_cache`, the Core cache, then R2.
The Core cache defaults to `.public-shards` under state. Use `cache_root`
to choose its directory and `cache_bytes` to set its least-recently-used
eviction limit. A limit of zero disables Core-local caching. Omitting
`endpoint` and `bucket` enables filesystem-only reads. Failed verification
and misses fall through to local evaluation.

Create a dedicated bucket in Cloudflare's **R2 Object Storage** dashboard.
In **Manage API Tokens**, create a bucket-scoped **Object Read** token for
consumer Cores and a bucket-scoped **Object Read & Write** token for the
publisher. Set `FUG_R2_READ_ACCESS_KEY_ID` and
`FUG_R2_READ_SECRET_ACCESS_KEY` in consumer Core and operator environments.
The publisher uses `FUG_R2_ACCESS_KEY_ID` and `FUG_R2_SECRET_ACCESS_KEY` and
adds `"publish": {"secret_key_env": "FUG_PUBLIC_SHARD_SEED"}` to its
`public_shards` configuration. Set `FUG_PUBLIC_SHARD_SEED` to the publisher's
32-byte Ed25519 seed in hexadecimal and distribute its corresponding public
key through `trusted_publishers`. Use a dedicated publishing Core with
operator-controlled registered Workers and install the publishing SDK on
those Workers. [Cloudflare R2 token procedure](https://developers.cloudflare.com/r2/api/tokens/)

To prefill a directory before mounting it read-only, pass the desired index
keys or shard IDs to the service CLI. An index key fetches and verifies both
objects. A shard ID fills only the content-addressed shard, so its signed
index must also be available for lookup.

```sh
fughetta-service public-cache-fill config.json /var/cache/fughetta/shared KEY [KEY ...]
```

## Connect a self-hosted Worker

The execution machine runs the Worker through `fug`, the Fughetta CLI:

```sh
fug login --server https://gateway.example
fug worker --server https://gateway.example --workspace acme
```

`fug worker` registers the machine with `POST /api/v1/workers` (owner only,
body `{"name": "..."}`), which answers 201 with
`{"workspace_id", "worker_id", "secret"}` exactly once and `cache-control:
no-store`. The Core stores only the secret's hash. `fug` keeps the credential in
an owner-only file, downloads this binary from the GitHub release, writes a
private Worker configuration, and starts:

```sh
fughetta-service worker run --config /absolute/path/to/worker.json
```

The configuration names `gateway` (the loopback relay), `worker_id`,
`workspace_id`, `token` (the secret), `name`, `data_root`, `work_root`,
`compute_threads`, `parallel`, `max_inflight`, and `tools`. It must belong to
the current user with mode 0600 on Linux; Windows uses an owner-only ACL. Work
and data roots are partitioned by immutable Workspace ID and Worker ID. The
Worker opens no inbound listener.

Builds and project-code analyses run in an on-demand execute child over inherited
stdin/stdout. Other analyses stay in the front process. The Worker directly starts
the execute child using the same service binary. The child uses `<work_root>-execute`
and stops with its parent or after becoming idle. Front preparations and hot analysis
sessions retain their trees across execute GC and restarts. Worker memory reports
include both roles and their tracked subprocesses.

`worker_backend` in the service configuration (`backend` in a Worker file) accepts
`max_memory_bytes` and `max_cpu_seconds` as optional process limits for the execute
child. Both Worker roles run in the isolation boundary supplied by their
deployment. The Linux Host Executor supplies that boundary for managed overflow
Workers.

Exit status 2 means the Core refused the identity (revoked, unknown, or for
another Workspace); `fug` does not restart a Worker that exits with it.

The Core uses a separate private listener for registered Workers:

```json
"server": {
  "auth": { "mode": "trusted", "workspace": "workspace-immutable-id", "front_token": "<shared-secret>" },
  "remote_worker_bind": "127.0.0.1",
  "remote_worker_port": 9091,
  "remote_worker_enrollment": false
}
```

Use a separate Source port if it binds to the same address. Set
`remote_worker_port` to `0` to disable the tunnel listener. Point the gateway's
private Worker address at this listener and forward
`GET /workers/tunnel` with the `fughetta-worker.v1` WebSocket subprotocol.
The Core refuses upgrades with an `Origin` header. Binary frames transport
the Worker session in 16 KiB outbound chunks. The Core uses ping and pong
frames and closes a tunnel after 90 seconds of inactivity.
Enable `remote_worker_enrollment` on the Core and tunnels on the gateway to register
and admit remote identities. Turning `remote_worker_enrollment` off refuses new
registration and admission while retaining the registry and existing sessions. The managed listener and its
shared token remain separate.

The registry lives at `server.storage/worker-registry/registry.json`; include
it in Core backups. It stores credential hashes, creation and revocation
metadata, and recent connection times. Quiesce and stop the Core and its source service before copying the registry
and source state as one recovery set, preserve owner-only permissions, and
restore it under the same immutable Workspace ID. Credentials active in that
backup remain valid; recorded revocations remain denied. A backup taken before
a revocation lacks it: keep `remote_worker_enrollment` disabled until the
current revocations are restored or affected identities are revoked and
registered again.
The local recovery drill copies a registry backup into a fresh directory and
checks active and revoked credentials plus Workspace binding. Production
storage snapshots and full source recovery still require a deployment drill.

Core logs record registration, revocation, and authenticated
Worker connection metadata without credential material. `GET /api/v1/workers` is member-readable
and includes offline and revoked identities. Connected Workers report
`compute_threads`, the execution job budget `parallel`, and `max_inflight`. Owners rename with
`POST /api/v1/workers/{worker_id}/rename` and revoke with
`DELETE /api/v1/workers/{worker_id}`. Revocation removes dispatch eligibility
immediately and shuts down active sessions. Execution results and build ledger
events take their Worker identity from the Core's authenticated connection.

Build events retain the immutable Workspace ID, Project, Worldline, opaque
Version, content snapshot, operation, actor, and registered Worker ID. The
agent's build, test, and run results leave these out. Equal content on different Worldlines retains separate execution
identity. Sync rejects a Worker that substitutes another content snapshot.
Interrupted execution returns `outcome_unknown`; cancellation and failure
before dispatch remain distinct. Unknown and cancelled builds count toward
admission limits and have separate totals in usage and Prometheus.
Worker inflight series include the authenticated `worker_id` alongside the
display name, so same-name registered machines have distinct series. Managed
connections sharing the same display name have one series with summed inflight
requests; their total connection count remains separate.
Language queries prepare missing analysis within the original call on a worker.
Each caller holds a cancellation ticket; preparation has no separate operation
or durable receipt. When no worker meets the requirements, the call reports
`no_worker`. Source never executes preparation commands.

Remote defaults are a 15-second application heartbeat and a 90-second Core
stale threshold. `heartbeat_seconds` accepts 1–30 seconds. Temporary connection failures use bounded
exponential backoff with jitter; `reconnect_attempts` defaults to 30 and accepts
1–120 attempts, resetting after a stable session. Disabled admission and a
protocol mismatch are retried the same way, since the identity stays valid.
Credential refusal and revocation stop the Worker with exit status 2.
Reconnection does not replay interrupted jobs. Register again after
revocation.

## C++, Python, TypeScript and Bun toolchains

Projects store build graphs and language-service configurations. Create one with
`PUT /api/v1/projects/{project}` and a revisioned settings value; source-file
generation operates on existing projects. Import the project's own source and
build manifests before executing its tasks.

| Component | Execution and configuration |
| --- | --- |
| CMake / CTest | Configure and build with explicit directories, presets, compilers, variables, and targets. The CMake builder's `test` object controls CTest selection, parallelism, timeout, and failure behavior. An empty test selection fails by default. |
| vcpkg | Install a manifest into `install_dir`; the task exports the installed directory. Point CMake at the selected checkout's `scripts/buildsystems/vcpkg.cmake`, bind `VCPKG_INSTALLED_DIR` to that output, and set `VCPKG_MANIFEST_INSTALL=OFF` when the graph already performs installation. |
| clice | Read compilation-database outputs derived from the bound build graph, with compile-flag additions/removals, executable, and environment settings. The Worker runs clice in its `execute` role because toolchain discovery can launch the compiler named by the compilation database. |
| TypeScript / tsc | Check a selected `tsconfig.json` without emitting, or compile into an explicit output directory. Compiler options are typed fields; terminal diagnostics remain available without being parsed as a structured report. |
| typescript-language-server | Analyze TypeScript, TSX, JavaScript, and JSX through LSP. Configure tsserver, inlay/completion preferences, implicit-project settings, locale, and memory through initialization options. |
| Bun | Build and test only. Every operation automatically performs a frozen-lockfile `bun install` first, with lifecycle scripts disabled by default. Build entrypoints, output, bundler options, test paths, and name filters are typed; there is no `run` operation or arbitrary command argument field. Tests use Bun's native JUnit reporter. |
| uv | Lock, sync, build distributions, and run a command. Environment and distribution directories are explicit; dependency groups, extras, interpreter selection, and offline execution are modeled separately. |
| ty | Run type checks with import environments and rule severity settings; warnings fail by default. Its language server has independent diagnostic, completion, inlay-hint, and configuration settings. |
| Ruff | Check lint and formatting without rewriting source. The language server has independent rules, formatting, code-action, and configuration settings. |

The CPython runtime host is installed at `libexec/fughetta-service/python/runtime.py`.
Map that file to `python-runtime` in the Worker tools and run it from an
isolated execute role using a command task in the project directory.

```json
{
  "argv": ["python3", "-I", "${tool:python-runtime}", "mypackage", "--member", "connect", "--output", "runtime.json"],
  "outputs": {"runtime": {"type": "python-runtime", "path": "runtime.json"}}
}
```

The report describes selected members, callable signatures, and the one-hop
type surface with inherited members and descriptors. The report retains
registered Python overload signatures and inherited native method signatures.
Deferred annotations from checkout declarations are labeled with their source.
A CPython C function also reports its library and image-relative entry offset,
with a linkage name when the loader reports that exact entry. Omit `--member`
to report public members present in the module dictionary.

Bind the report to the `artifact` input of a `python-runtime` analysis stage
with `options: {}` to publish its symbols and definitions. To associate
runtime members with source declarations, set `options.declarations` to the
module's project-relative Python file or stub and bind the `ty` stage's facts
to `source_facts`. A `pybind` stage takes the runtime stage's facts on
`managed` and the `native-artifact` stage's facts on `native`. Set
`options.libraries` to map runtime library filenames to native stage module
names. CPython C entries resolve by offset and available linkage name.
On Linux, pass `--native-host ${tool:fughetta-llvm}` to the runtime command
to resolve free-function pointers held by pybind11 and nanobind wrappers.
Keep DWARF and the symbol table in the native extension for this step. The
bridge resolves captured overloads independently by library, linkage name,
and entry offset. On Linux x86-64,
selecting a class with native methods also publishes its public wrapped methods and
resolves captured nonvirtual C++ member functions. An inherited instance method
can be traced when the receiver is an instance of the selected class. For C++
lambdas the bridge uses the emitted call operator, or the native function that
contains its inlined body according to DWARF. With `--trace-import`, successful calls to
overloaded free functions and nonvirtual methods record the C++ target per call
site when the viable overloads lead to one implementation. Virtual calls also
resolve the receiver's native layout to identify the implementation.
Fixed-arity overloads can be ruled out by their required arguments,
recorded defaults, and positional or keyword restrictions.
Calling an instance method through its class includes the explicit receiver in
the Python argument count. The bridge accounts for that receiver when aligning
arguments with the C++ signature.
For calls with explicit keywords, the bridge can rule out fixed-arity overloads
whose binding records lack the supplied parameter names. Known keyword values
can also rule out a matching parameter's built-in caster. The same call can use
positional values to distinguish string and numeric overloads. The runtime uses
observed arguments or unambiguous literal loads and skips unknown positions.
Forwarded positional arguments can use the first expanded value reported by
CPython, or the tuple layout and known values directly visible in bytecode.
Expanded keyword arguments use proven keys and values even when a call merges
additional keywords from a dynamic mapping.
For numeric overloads, an exact floating-point value rules out integer casters. An
exact integer value can rule out an integer caster based on its width and
signedness. Explicit conversion restrictions are interpreted using each
wrapper's built-in caster rules, including numeric and Boolean arguments.
When surviving overloads have different implementations, the `calls` edge to
the Python callable reports `detail.evidence: "unresolved"`. Its `candidates`
list contains the known native entries that survived filtering for those
invocations. `candidates_complete` indicates whether every surviving entry
could be identified. With a complete list, following `binds` from the call
endpoint follows only those candidates. The source reference still identifies
the Python declaration, where all registered overloads are available. An incoming
`calls` query on that declaration returns the same edge with its scoped endpoint.
The same call site
can also return an edge with `detail.evidence: "observed"` to a binding with an
identified C++ target. Both details use `kind: "native_call"`. The observed
detail reports `dispatch` as `direct` or `virtual`, or `indirect` when both
routes can lead to the same target.
For pybind11, this includes registered subclasses that require a C++ base
conversion and extensions that import the base type's RTTI. For an explicit binding,
`options.entries` maps a fully qualified Python member such as
`mypackage.connect` to its C or C++ linkage name.
Expose the bridge facts in the profile output to query the `binds` edges.
Pass `--trace-import mypackage.caller` to record real calls to the selected
exports while importing that caller module. Run from the checkout root for
source locations. The stage publishes those call sites as `calls` edges, so a
view containing the runtime, bridge and native facts can traverse
`calls`, `binds` and `emits` to native source definitions. Query with
`depth: 3` and `direction: "both"` to follow `emits` from the artifact to the source.

To collect runtime types while running a Python test task, use the same Worker
host with the `harvest` subcommand. Select the functions by name. The test
runner still receives its own arguments after `--`.

```json
{
  "argv": ["python3", "-I", "${tool:python-runtime}", "harvest", "mypackage", "--function", "convert", "--test-module", "pytest", "--output", "runtime.json", "--", "-q", "tests"],
  "outputs": {"runtime": {"type": "python-runtime", "path": "runtime.json"}}
}
```

Include the command task in the build configuration's `test` operation. The
gateway retains its declared report for the exact tested worldline and version.
It updates current views with matching `ty` declarations and returns their IDs
in `runtime_observation_views`. A subsequent analysis with `ty` can attach the
retained report when those declarations first become available. `inspect`
returns `runtime_observed` ranges and the observed types' public member names.
Return types feed Producers, and first parameters feed Consumers. The host
writes the report after a successful test, and a publication failure appears
as `runtime_observation_error` alongside the test result.

The vcpkg provider binds its binary cache outside the hot checkout. Managed
workers share `<server.storage>/dependencies/vcpkg`. An independent worker can
set `dependency_cache` to an absolute shared-mount path; its vcpkg archives use
the `vcpkg` subdirectory. Without that setting, an independent worker uses
`<work_root>/dependencies/vcpkg`; registered workers use
`<work_root>/<workspace_id>/<worker_id>/dependencies/vcpkg` instead. Set
`dependency_cache` on every participating independent worker to share a mount.
A fresh build keeps this cache. Projects retain
their own install and build directories; archive keys and package compatibility
are determined by vcpkg. The provider sets `VCPKG_BINARY_SOURCES` for the install
step, including escaping reserved characters in vcpkg's source-list syntax.
Project `extra` and invocation arguments cannot override binary-cache sources,
disable binary caching, or request editable installs. Invocation response files
are rejected. The provider owns `VCPKG_FEATURE_FLAGS` and sets `binarycaching`
on the install step, overriding build-level feature flags. The cache placeholder is
reserved for the managed source string. CMake-driven manifest installation does
not use this path; use the vcpkg provider and disable CMake's manifest install as
described above.
Shared cache mounts must belong to the deployment's trusted build environment;
a writable mount is unsafe for mutually
untrusted build commands. Workers serialize vcpkg installations with a lock in
the shared cache directory, so a waiting install can restore packages published
by the preceding one. Waiting is cancellable and counts against the execution
deadline. The shared filesystem must support locks across participating hosts;
unrelated cache mounts cannot coordinate builds. This currently serializes all
vcpkg installs sharing that directory, including installs of different packages.
On an install, Workers prune writable vcpkg archives whose last modification
was more than 30 days ago. Each Worker runs this maintenance at most once an
hour under the install lock. Published and read-only upstream archives keep
their deployment-managed retention. The checkout size budget and TTL cover
checkout trees. Keep the directory lock file when maintaining a cache.

For a trusted publisher and separate consumers, set each consumer's
`dependency_upstream` to the absolute root of the published cache. The provider
reads its `vcpkg` subdirectory before the consumer's writable cache
(`dependency_cache` or its default);
new archives go only to that consumer cache. Locks also stay in the writable
cache. The two `vcpkg` subdirectories are rejected if their resolved paths overlap
before taking a lock. Configure the trusted publisher with `dependency_publish`
as described below. Consumer caches must stay private to their trust domain.
Mount the published root read-only in consumer environments: vcpkg's `read`
source mode governs vcpkg, not arbitrary project commands. This configuration
does not create or verify an OS filesystem boundary or provision a publisher.
The gateway schedules cache fills on connected publishers before consumer installs.
The pinned [vcpkg tool's files provider](https://github.com/microsoft/vcpkg-tool/blob/2026-09-26/src/vcpkg/binarycaching.cpp#L289)
publishes a completed archive by rename. Across filesystems it copies to a
temporary sibling of the destination before renaming. A shared mount must
preserve that atomic rename behavior. This source inspection does not verify
the deployed filesystem. Direct vcpkg writers can replace an existing ABI archive;
use the staged `dependency_publish` path below for no-replace publication.

A dedicated trusted worker can set `dependency_publish` to the published root
instead of `dependency_upstream`. Its writable cache stays private, defaulting
to `<worker-root>/publisher/dependencies` so it does not reuse the older general
cache. Configure `dependency_recipe` together with `dependency_publish`; its
executable, registry root, allowed triplets, optional absolute overlay paths and
environment belong to the operator. Start with an empty private cache; never
reuse one that held untrusted builds. The publisher rejects regular build and
source-upload RPCs and only accepts typed dependency selectors. Each fill uses
a fresh install tree, reads published archives and serializes installs with the published root's lock,
and after a successful install copies new ABI archives to temporary files on
the destination filesystem. It flushes each copy and creates the final entry
with a hard link that cannot replace an existing file. Later changes to private
archives do not change published copies. Publication errors appear in the
step's `cache_error` without discarding its exit status, and prevent reuse of
that build result. Each archive is limited to 8 GiB; publication visits at most
65,536 entries across the whole private cache and checks the execution deadline
while copying. A successful install triggers publication of all unpublished
archives in that trusted private cache, including completed dependency archives
left by earlier failed installs. It does not limit publication to the last
install's outputs. Prune the private cache while publishers are stopped before
it reaches the entry limit; remove malformed archives reported by `cache_error`
before retrying. Publication is atomic per archive, so entries committed before
a later archive fails remain available.
The gateway reads the consumer manifest from its pinned source snapshot and
projects it to package/feature/version selectors. Project commands, environment,
registry definitions and source files never reach the publisher. Provider steps
with custom overlays or opaque arguments, and snapshots with custom registry
configuration, build privately. Empty triplets use the sole operator-admitted
triplet. Publishers allowing several triplets require explicit target and host
selections on every request. Package ABI matching
remains vcpkg's responsibility. The builtin registry resolves selected versions
through the operator root's version database; a requested baseline selects
versions, not an arbitrary port script tree ([registry implementation](https://github.com/microsoft/vcpkg-tool/blob/2026-09-26/src/vcpkg/registries.cpp#L527)). A different consumer toolchain, environment or
port input causes a private miss instead of accepting an incompatible archive.

Fills share backlog tickets across equal concurrent requests, before the consumer
job enters the queue. They count against the same concurrency and build-rate/time quotas without
nested queue waits; shared fill work is charged once to its initiating project. Completed fills are not memoized: each later fill rechecks
the real archive store, including after operator pruning. Failed/unavailable
fills permit private builds; waiting is cancellable and consumes the caller's
timeout. Build results expose bounded per-step `dependency_cache` statuses; `filled`
means the publisher completed, not a measured consumer ABI hit. Warm-context
affinity remains primary; otherwise toolchain selection favors a live matching
publisher. Consumer mounts must be read-only at the OS boundary; only trusted
publisher processes may write the
published directory. Interrupted publication can leave `.publish-*` directories;
operators may remove them while publisher processes are stopped.

The recommended deployment boundary is one runtime per tenant or trust domain,
with a separate runtime for trusted dependency publishing. A runtime can be a
virtual machine or a sandboxed container; the service does not require a
particular orchestrator. Scheduling hints and vcpkg's `read` mode do not enforce
this filesystem boundary. The deployment owns it; the worker owns validation,
coordination and no-replace publication.

Merge these cache fields into a consumer's existing Worker configuration:

```json
{
  "work_root": "/var/lib/fughetta/worker",
  "dependency_cache": "/var/lib/fughetta/dependencies",
  "dependency_upstream": "/cache/published"
}
```

For the dedicated trusted publisher, use these fields instead:

```json
{
  "work_root": "/var/lib/fughetta/publisher",
  "dependency_cache": "/var/lib/fughetta/publisher/dependencies",
  "dependency_publish": "/cache/published",
  "dependency_recipe": {
    "executable": "/opt/vcpkg/vcpkg",
    "root": "/opt/vcpkg",
    "triplets": ["x64-linux", "arm64-linux"],
    "timeout_s": 1800
  }
}
```

The identical `/cache/published` paths refer to different runtime mount views
of the same published storage. The deployment must enforce these permissions:

| Resource | Consumer runtime | Publisher runtime |
| --- | --- | --- |
| Published cache, including any submounts | Read-only, with no writable alias exposed | Writable only to the trusted publisher |
| Checkout, reports and private cache | Writable; private to this trust domain | Writable; starts empty and receives only trusted inputs |
| Worker configuration and credentials | Own workspace only; operator-controlled configuration | Separate trusted workspace; unavailable to consumers |
| Gateway admission | Own workspace's requests | Dedicated publisher role; only typed dependency fills, excluded from general worker selection |

Before enabling cross-tenant reuse, exercise the boundary from an actual build
step in a disposable deployment: creation, replacement and deletion of cache
canary files must fail for the consumer, while reads succeed. Other tenants'
private storage and publisher credentials must be inaccessible. Then verify
that two independent publisher runtimes sharing the storage compile a cold ABI
once, consumers restore it, and interruption during publication exposes no
partial archive. These checks also establish the mount's lock and hard-link
semantics. The current local RPC test covers two worker services on one host;
it does not establish this deployment boundary or cross-host behavior. No such
deployment is provisioned by this repository yet.

Kotlin/JVM and F# build models use the `gradle` and `msbuild` adapters. Map
`fughetta-gradle-model` to the Gradle init script in
`<prefix>/libexec/fughetta-service`. Map `fughetta-msbuild-model` to
`msbuild/fughetta-msbuild-model.dll` under that same directory. The Worker runs
the exporter with `dotnet` and loads its packaged argument parser locally. Each adapter exposes
`source_dir`, `model_dir`, `executable` and `exporter` options.

| Adapter | Preparation output | Required model inputs | Model output |
| --- | --- | --- | --- |
| `gradle` | `prepare/dependencies`: `gradle_dependencies` | `dependencies`, plus configured generated ports | `model/model`: `project_model` file |
| `msbuild` | `restore/assets`: `nuget_assets` | `assets`, plus configured generated ports | `model/model`: `project_model` file |

Connect these ports through task `inputs`. The graph derives execution order
and rejects missing or mismatched bindings. `generated` declares outputs of
`generate` with type `generated_sources`; every declared port is required by
`model` and `compile`. A discovered Gradle generated source root must exist and
belong to a bound generated input. Gradle task lists are `prepare_tasks`,
`generate_tasks`, `compile_tasks` and `test_tasks`. MSBuild uses `projects`,
`properties` and `generate_targets`. C# runs Roslyn analyzers and generators in the execute role. Bound
`generated_sources` can supply files from `CompilerGeneratedFilesOutputPath`.
The helper uses current generator output for the compilation whenever the
model supplies the generator. Final compilation outputs are declared in `artifacts`.

Bind a graph task to the Gradle builder's `test` operation to run tests in the
slot's managed Gradle session. It runs the Gradle `test` task by default. Set
`test_tasks` to use project-specific tasks such as `integrationTest` or `run`,
and use `after` to order any required build or setup tasks. Gradle `Test` tasks
contribute their native JUnit XML, including results reused by Gradle and tests
run in included builds. Test compilation failures retain the build failure and
have no test tally when no test task produced a report. When `android_variant` is set,
aggregate `assemble` and `test` selectors use that variant's assemble and unit
test tasks, including project-qualified selectors. Specific task names select
the tasks to run.

Adjacent Gradle preparation/model steps with the same declared batch prefix,
working directory and environment run in one managed Gradle process. Their typed
tasks, receipts and outputs retain their original identities; the process result's
`tasks` lists all participating tasks, sharing its elapsed time, failure and log.
Generated-output and independent-report steps form batch boundaries. Each model
export still checks the dependency receipt against the current resolution.

The execute Worker keeps a Gradle Tooling API session and its daemon in the build
slot when `gradle` points to an installed Gradle distribution and `java` and
`fughetta-jvm` are mapped. Typed Gradle stages reuse that session across builds of
the slot while their build environment remains the same. Each slot has its own
daemon registry and Kotlin compiles inside the Gradle daemon. The daemon stays
in the Worker's process group on POSIX and job object on Windows. Slots share the Gradle caches in the configured `GRADLE_USER_HOME`, which defaults
to the execute Worker's `gradle` directory. The Worker reclaims idle sessions under
memory pressure and closes them when a slot is retired or the Worker stops.
Requests with a per-command CPU limit run as individual Gradle processes.

Set `model_source_dirs` on a Gradle builder when the contents of files under
those project-relative directories are used only by compilation and source
analysis, and the model and dependency resolution are determined by the other
tracked inputs. A warm Worker reuses the model and dependency receipt when a new
snapshot only changes the contents of existing `.kt` or `.java` files there.
Build scripts, dependencies and source inventories trigger preparation on change.
Projects with `prepare_tasks` or generated inputs prepare their model on each new
snapshot. Compile tasks always consume the requested snapshot.

Both exporters write `model.json`, using the `projectmodel` C++ contract:
`schema`, `producer`, `producer_version`, and ordered `units`. Units contain
ordered source paths, generated source objects, tagged project/artifact/SDK/tree
references, outputs, compiler arguments, published `variants` and language-specific `extensions`.
Tree references name a file without claiming project ownership and cannot name a
declared unit output or a file beneath it. Model intake resolves every project reference;
exporters omit project-produced classpath entries already represented by project references.
Consumers select
project units through `projectmodel::resolve`, using an explicit unit or the complete
published variant attributes; missing or ambiguous matches fail. MSBuild project
references must select one TFM, and all units must use the same .NET SDK version.
Artifact intake parses and validates this contract. Gradle project identities
include their build-tree path. The exporter records its tested KGP ranges,
checks the accessors it uses, and records the actual plugin version in each
unit's extensions. Model export recomputes the preparation receipt's digest of
resolved references, including included builds, and rejects changed resolution.
NuGet receipts contain SHA-256 digests of root and transitive project assets;
model export verifies those contents before evaluating projects.

External libraries, plugins and generated text are referenced as `{hash, name}`
objects (`artifact` references contain an `object` field). Preparation commands
receive `FUGHETTA_STAGING` and `FUGHETTA_SDKS`; exporters write objects only into
the step's staging directory. The worker hashes private copies before admitting
them into its existing export store. Model publication transfers missing objects
to the gateway through the export channel. A receiving worker fetches missing
objects before running a step with a `project_model` input or measuring the model.
CAS files are published read-only. Transfers and installations pin their models
and objects before collection can run; active language sessions retain their model
pins until update or close. Objects remain retained while any cached or pinned
model references them and are collected when its last model is evicted.
If a cached build's model or referenced object has been evicted, the gateway discards
that cached result and executes the build again during the same request.
Model files contain no dependency bundle.

JDKs and .NET reference packs use the worker's `sdk_manifests` mappings below,
with logical SDK names and relative paths. A selected SDK must have a mapping;
the consumer resolves it against its own installed roots. The provider host contract
allows nested SDK bindings: exporters assign each path to the longest matching root,
record that binding's name and relative path, and consumers resolve by that name.

Results come from reports in fixed protocols, never from the text a tool prints
for people, so a new tool version does not break them. A step writes its reports
to `${reports}`, a directory outside the tree that is empty when the step
starts, and the worker reads them after the step, failed or not:

| Protocol | Written by | Gives |
| --- | --- | --- |
| `junit` | CTest (`--output-junit`), pytest (`--junit-xml`), Bun (`--reporter=junit`), Gradle `Test` | total, failed, failed test names |
| `libtest` | Cargo's built-in test harness and rustdoc | total, failed, skipped, failed and skipped test names |
| `bep` | `bazel test` (`--build_event_json_file`) | the same, from each target's test summary |
| `codequality` | ty and Ruff (`--output-format gitlab`) | diagnostics with file, line, column, and severity |

Adapters add these flags and declare their reports themselves. A task that
starts a program its adapter does not know adds the report: a `uv run` or
`command` task that runs `pytest --junit-xml=${reports}/pytest.xml` sets
`"reports": [{"protocol": "junit", "file": "pytest.xml"}]`. Reports of every
step in a result are merged, so a configuration that compiles an extension with
CMake and tests it with pytest reports both. Compiler messages are not parsed:
they stay in the step's output for the agent to read. A new tool joins with its
own adapter module, and a new protocol only when no existing one fits.

Execution results and the member endpoint `GET /api/v1/calls?project=NAME&limit=50`
include `timings` in seconds. `client_sync_seconds` is the client-reported local checkout scan, upload and writeback duration, supplied as `fughetta/client_sync_seconds` in MCP call `_meta`. It excludes waiting for the local push lock and is shared across configurations of one call. The service accepts finite values from zero through 24 hours and ignores values outside that range. It is descriptive data, separate from service-measured durations. `sync_seconds` measures source transfer from the
service to the Worker, `queue_seconds` the caller's wait for a job to start,
`dependency_seconds` dependency preparation (including its own scheduling), and
`worker_seconds` total work on the Worker, including checkout and analysis. Within that total, `preparation_seconds` measures provider-marked setup steps (vcpkg install, uv lock/sync, Bun install and CMake configure), including dependency cache lock waits; `execution_seconds` measures other command steps and language/artifact analysis. Step measurements include command setup and report processing. Checkout and exported-file capture are outside these two sub-stages.
Dependency time is measured before the main job enters its queue. Missing stages
are absent; a cache hit reports zero work for that call. Shared callers receive
the shared execution's sync and Worker times, and their own queue wait. The call's
`reuse` identifies executed, shared and cached results. These stages do not
include result delivery and do not sum to the full
request duration. Call records retain the same bounded recent history as builds.

Agents configure projects with the `project`, `schema`, and `save_project` tools of
`/mcp` (also available on the configuration endpoint). `schema` returns the settings schema with the options of
every adapter and language server, so the tool list stays small; a refused save
names the field.

The orchestrator uses its account credential to configure the project and assign
work. `task(create)` binds a named task to an explicitly selected `worldline`.
`task(fork)` takes `task`, the new `worldline`, `from`, `version` and
`operation_id`; the fork and task binding commit together. A task's binding never
changes. Several tasks can share a worldline, and each task can have several runs.

`task(start)` takes `project`, `task` and `operation_id`, and returns a `run` and
`credential`. Give the agent this credential as its Bearer token on `/mcp`, rather
than the orchestrator's account token. The credential binds the project and
worldline on every call; omitted fields come from that binding and conflicting
explicit fields are refused. An account call without a run must select its
worldline explicitly: there is no implicit fallback to main. Run credentials
cannot administer tasks, access settings, fork other lines, or use REST routes.
`writable: false` on start restricts the run to source reads and computations.

Runs own their leases and queued requests independently, even when they share an
account, client or task. Task administration belongs to the authenticated account,
so changing the orchestrator client does not change task ownership. Reconnecting with the same credential resumes that run. `task(stop)` names
a `run`; `task(close)` closes all runs of the task. Both revoke source access and
release owned leases in one transaction. A delayed write is checked again by
source at publication. Revoked runs cannot be resurrected by replaying start;
a new execution needs a new operation ID. Source restart preserves tasks, runs,
references and live leases. Closing a task does not merge or delete its worldline.

The agent loop uses the execution credential on `/mcp`:

1. The orchestrator configures toolchains, analysis, tests and named operation
   `exports` using `schema`, `project` and `save_project` before starting the run.
   The agent can read available operations through `configurations`.
2. `worldline` with `inspect` lists files and returns a version; `read` returns
   text and file metadata. Pass `known_digest` on later cached whole-file reads to receive
   `unchanged: true` without repeating content. For a function-sized read, pass
   its scalar `offset` and `count`; `next_offset` supports byte-limited reads.
   Binary reads expose base64. `compare` takes `observed` and an optional target
   `version` and returns changed file metadata without contents. Successful
   edits and receipts also return only changed files, with before/after metadata
   recording deletions and renames; `inspect` lists a whole version.
   `diff` compares `from` at `version` against `worldline` at `observed` (each
   defaults to the current version). `promote` requires both fixed versions,
   `from`, and `operation_id`. It copies the source changes since the common
   history into one atomic target contribution, retaining unrelated target files.
   A path changed differently on both sides refuses the entire operation with
   `source_conflict`; a moved target head returns `stale_frontier`. Scope leases,
   publication policy, quotas and receipt recovery apply. The source history is
   not added to the target's parents.
3. `language` with `inspect` or `definitions` returns each definition's body
   text and registers its body range, and any returned source scopes, with
   source. `action: "scopes"` (or `include: ["scope"]`) also returns scope IDs,
   hierarchy, Unicode scalar ranges `[begin,end)` and opaque `reference`
   values. A reference binds one analysis configuration, source version,
   worldline, file identity and range, keeps the characters around that range,
   and expires after 600 seconds; analyzing the same text again refreshes it.
   IDs alone do not identify a lockable range.
4. Submit `worldline` `apply` with `observed`, `operation_id` and range edits
   (`kind: "edit"`, `offset`, `count`, `text`). Offsets count Unicode scalars
   in `observed`. A `kind: "replace"` change names `old_text` and `new_text`
   instead; the gateway finds `old_text` in the observed file and sends the
   matching range edit. It must occur exactly once in the search window: the
   whole file, the scalars from `offset` for `count`, or the structure a
   `scope` reference from `language` names, located in `observed` through its
   anchors. Zero or several matches fail with `invalid_record` and nothing is
   written. Without `leases`, source locks each text edit for its caller
   (the run, or the account and client) after the write: the lease covers the
   smallest registered scope that encloses the edit, skipping translation
   units, modules and namespaces, or only the edited characters when no scope
   encloses it or another caller holds part of that scope. Later edits by the
   same caller renew it for 120 seconds; `task(stop)`, `task(close)` or expiry
   releases it. Another caller whose edit touches the locked range or its
   boundary characters receives `scope_conflict` naming the scope, its holder
   and its expiry. Whole-file changes take no implicit lease but are checked
   against other callers' leases. The reply's `reach` is `body_only` when
   every change edits text inside a registered function body, so local
   diagnostics suffice; otherwise it is `declaration_touched`, and
   `language(impact, changes: ["declaration"])` finds the affected callers.
5. An orchestrator that divides work before dispatching it uses `scope` with
   `action: "acquire"`, `scopes: [reference, ...]` and `operation_id`. It
   acquires the whole set atomically and returns `leases`. Source resolves the
   references; callers cannot supply coordinates. For whole-file changes use
   `files: [path, ...]`; for creation or path changes use `entries: [path, ...]`.
   File and entry requests also name the current `version`. File leases cover
   empty and binary files; entry leases cover the named path and descendants,
   including move destinations. Resource lists may accompany scope references.
   Overlap within one worldline returns `scope_conflict` immediately, with no
   partially acquired set. References must describe the current version;
   `scope_stale` means query the current source/scopes again. An apply that
   passes `leases: [token]` must stay inside the held scopes; whole-file
   replacement requires a lease covering the whole file. A caller's own leases
   never refuse its edits. Renew the lease while working and release it when
   finished. Defaults are 120 seconds, with a maximum of 600 seconds.
   Renewing never changes the protected region. Replaying an acquisition after
   any token has expired or been released returns `scope_expired`; a new
   acquisition requires a new operation ID.
6. Pass the returned `version` explicitly to `language`, `build`, `test` and
   `run`. Concurrent edits advancing the worldline do not change that job's
   input. Read `receipt` with the same operation ID after an interrupted write;
   replaying identical input retrieves the recorded result without another edit.
7. Successful execution returns `exports` with project-scoped digests, paths,
   sizes and executable flags. Use `artifact` with project, digest, offset and
   optional length to retrieve base64 chunks (at most 64 KiB each). Decode
   them to a local file, continue at `next_offset` until `eof`, preserve the
   executable flag, and deploy using the local machine's tools.

Source checks leases in the same write transaction as publication. Their
boundaries use source character identities, so disjoint edits can move a scope
without invalidating its lease. Lease ranges returned by `scope` are relative
to that response's current version. A lease whose anchors disappeared is
invalid and must be released before acquiring a fresh analyzed range. Leases
survive source restarts until expiry; a missing/expired token returns
`scope_expired`. History-changing operations require other callers' explicit
leases on that worldline to be released first; implicit leases guard only text
edits. Forking remains available. A changed input after a
refused operation uses a new operation ID.

Leases live in local coordination metadata, outside worldline history, source
versions and build cache keys. Acquiring or renewing a lease does not invalidate
analysis or build results. Range edits reuse the source engine's incremental
history; workers receive missing source objects and update their existing
language sessions. Equal immutable inputs retain job deduplication and build
cache reuse. `fresh` explicitly requests a clean build. Artifact grants remain
project-scoped even when the content is shared internally.

Analysis queries join queued or running computations whose per-stage demands
cover their files, modules, contexts and requested facts. Sharing requires the
same project inputs, stage graph, toolchains, execution budgets and result
projection. Each caller retains its own wait ticket and query view; leaving
one ticket preserves the computation for the other callers.
Overlapping demands that need additional results wait for earlier computations
to commit, then consult the unit reuse directory. These waits hold no dispatch
slot and keep the earlier work alive while a successor still needs it.
C/C++ file-scoped symbol queries evaluate the selected translation unit; headers
select their owning translation units. Diagnostics and scope queries also honor
their file scope. A fixed view records the computation's file scope and reports
partial coverage when queried beyond it.
Partial queries return a `continuation` containing a view ID and cursor; query
parameters remain in the store. Send it to `language` with the project and optional budget
overrides. Analysis resumes the view's recorded source version, profile and
toolchains, preserving existing units in a newly published `next_view`. Query
traversal with a larger budget reads the same view. Pagination cursors continue
to address their original view.
Unit and node budgets limit each batch. Completed units remain published when
either budget is exhausted; missing coverage identifies the deferred units and files.
Continuing with the same unit budget advances the remaining work. Contributions
already retained by the view are reused when they still match the installed
inputs and requested data, without spending the unit or node budget again.
A unit is published whole, so its records must fit the remaining node budget.
The analysis time budget also returns partial coverage, including when it expires
before the provider is ready. Completed units remain available; the interrupted
session is retired and continuation resumes with the recorded inputs. The build's
execution deadline remains a build failure.

Authorized view reads renew a 24-hour pagination lease. Each worldline/profile's
current view, active queries and computation results retain their contributions.
During storage access, the gateway checks reclamation at most once per minute:
expired query views are withdrawn, and older unrooted unit candidates are evicted
under an 8 GiB shard budget. Shared shards and internal-stage checkpoints remain
while referenced. A continuation can reconstruct an evicted candidate from its
checkpoint. The gateway validates reuse against the snapshot and worker-reported inputs. Reclaimed
views return `status=expired` with `code=view_expired`.

Worker language sessions negotiate symbol, diagnostic, definition, and reference
capabilities. Each hot directory retains a separate session for each stage.
Switching stages preserves the other sessions; each session receives source and
prepared-input updates before its next evaluation. The worker enforces a total
session limit and reclaims idle sessions by age or memory pressure. Workers report
installed directories and ready stage sessions, including their source snapshot and
epoch. Each report replaces the previous inventory, withdrawing reclaimed state;
reconnection discovers retained directories from their installed manifests. Build
results publish updated inventory before their replies, allowing the next stage
to select the prepared directory immediately. Dispatch considers candidates across
routing cores. It prefers a ready session for the requested context and snapshot,
then that context, then an idle materialized directory, before comparing load.
When all eligible candidates have observations for the project, stage recipe and
warmth, it instead compares smoothed successful dispatch times, adjusted by
`1 + inflight / capacity`. Each worker retains at most 256 recent cost records;
failed or cancelled dispatches do not train this estimate. A directory group keeps
its selected worker for the computation, including waits for dispatch capacity,
so later stages consume the prepared files there. Inventory refresh runs
separately from the lease heartbeat. Queries
return results for the selected source snapshot;
source nodes retain only temporary scope descriptors and coordination anchors,
not language-server sessions or complete semantic analysis results. Queries accept
`selector: {"name": {"text": "render"}}` for symbol-prefix search and
`selector: {"entity": {"symbol_id": "…", "context": "…"}}` for a selected
declaration. A named definition/reference selector includes `name.path`; file
filters use `scope.files`. Results include a published `view`; pass it on later queries to
retain the same source and analysis inputs. Definitions, structured types and
references belong to the selected symbol and compilation context. Returned
source locations support navigation and edits.

`symbol_id` is an opaque workspace identity retained while any stored shard refers
to it. View leases, current views, active queries and reusable candidates keep
those shards alive. After the last reference is collected, an entity selector
using that ID returns `status: "expired"`, `code: "symbol_expired"`; select the
declaration again by name to obtain its current ID. Unknown, unretained IDs use
the same response. A retained ID may have no declaration in the selected view;
that remains a normal query result. An expired view returns `view_expired` first.

Application types retain ordered `application_arguments`: type and declaration
references, constant values and nested parameter packs. These references use
the published view's IDs; `inspect` expands their types, counting shared type
nodes once against the node budget.
Generic signatures expose ordered `type_parameters`. Each parameter links its
declaration, owning `binder` and position; nested templates retain the outer
parameters they use. Parameter nodes distinguish type, value and template
parameters, including unused parameters and packs.

`inspect` also returns `symbols`; other queries request them with
`include: ["declarations"]`. Each record retains its compilation context,
visibility and modifiers. Parameter details use group/position indices and retain
names, default argument text and annotations independently of signature types.
C++ attributes use the `cpp.attributes` extension on declarations and parameters.
Function redeclarations contribute defaults and attributes; definitions supply
parameter names when available.

For a selected callable, `inspect` also returns the public constructors, fields,
properties, enum members and methods of the types its parameters, receiver and
result name, one hop away. Pointers, references, nullability, aliases and
library wrappers such as `std::optional`, `kotlin.Result`, `Task` and F#
`option` are stripped first. Aliases expand to their underlying type. For a
selected type, `include: ["producers", "consumers"]` adds `producers`, the
constructors and callables that return the type, and `consumers`, the callables
that take it as receiver or first argument (instance methods take their owner).
Both lists are ordered by module, then visibility, and follow the requested
module and file scope. These symbols and the type records they reference share
the node budget.

If clice reports compiler errors in a selected translation unit, `inspect`
returns `partial` and includes a compiler diagnostic from that context, even
when the error is in a dependency outside the requested files. Affected items
have `type_confidence: "tentative"` and inferred ranges have
`confidence: "tentative"`. Language replies list identical available scopes
once and group missing entries with the same scope and reason, retaining their
unit IDs.

The query engine holds no language tables. Each provider declares its
`conventions` per language in its description: `visibility` values from most to
least visible (the first one is the public surface; undeclared values rank
last), the fully qualified `value_wrappers` stripped before one-hop expansion,
and the `scope_edges` kinds its scope records use. The gateway stamps a unit's
conventions onto its shards and refuses a shard whose scope records use an
undeclared edge kind. External registrations supply `conventions` in their
registration. Providers can use the shared kind names or emit language-specific
symbol, type and relation kinds as `<language>:<name>`. Query filters match
the published kind and its declared core kind. When a custom kind shares core query behavior, the
provider declares it in `symbol_traits`, `type_traits` or `relation_traits`.
The F# provider publishes `fsharp:record` and maps it to `type` for symbol
selection and producer and consumer discovery. Its `fsharp:curried_signature`
preserves the parameter groups in a callable that `inspect` can expand.
Relation traits control which edges impact analysis follows.

`typed_ranges` record what the compiler knows but the source does not spell:
local bindings and lambda parameters without a written type (C++ `auto`,
Kotlin `val`/`var` and implicit `it`, F# `let`, `use` and pattern bindings),
and generic, overloaded or virtual call sites with the chosen `overload`,
result `type` and generic `substitutions`. `inspect` returns the ranges inside
each selected definition's body, and `include: ["type"]` on `scopes` returns
those inside each returned scope. `inspect`, or `include: ["source"]` on other
actions, also fills each item's `source` with the text of its definition's
body, read from the view's snapshot.

References preserve either a resolved `target` or the provider's `candidates`.
Dependent overload sets are `ambiguous`; a single remaining candidate or a name
with no candidates stays `unresolved`. Queries for either overload return the
same reference ID, and an unselected reference query also includes names with no
candidates. Candidate call relations share that reference and retain their
`candidate` resolution. Resolved call references carry `direct`, `virtual` or
`indirect` dispatch when available.
Reference roles distinguish reads, writes, combined read/write uses, type uses
and imports. Write roles include assignments, increments and mutable arguments
identified by clice.
Constructor calls link direct, list and copy initialization to the selected
constructor. Default initialization and implicit conversions retain `call` and
`implicit` roles at their source location. Their call relations refer to the same
reference; the owning declaration identifies the initializer or enclosing body.

Scope queries also return `scopes`, with lexical parents, semantic owners,
declared members and lookup edges. Reopened namespaces, nested blocks and
unbraced branches retain separate regions. Lookup edges target published
declaration IDs for using declarations/directives, enum imports, base classes
and implicit namespace/enum lookup; the language provider performs exact name
resolution. Source locations in the result can be opened through the scope tool.

Use `action: "relations"` with `relations: ["calls", "overrides", "aliases"]`,
`direction: "outgoing" | "incoming" | "both"` and `depth` to traverse a selected
declaration's graph. Results include `relations`, intermediate entity
`items`, and linked references or declaration occurrences. `limit` and `cursor`
page edges within the same view. `include: ["relations"]` also adds the graph to
an inspection. Node and time budgets bound traversal; exhaustion returns partial
coverage so the query can be repeated with a larger budget.

Use `action: "impact"` with a symbol selector and `depth` for potential change
propagation. `changes: ["declaration"]` includes reference/type dependencies;
`changes: ["body"]` follows callers and bindings through source/artifact mappings.
Omitting `changes` includes both. The `impact` array separates semantic and
behavior paths, retaining one shortest path per reached entity and category.
Each step names evidence in the returned reference/relation records. Candidate
edges mark a path tentative. The selector and scope select the starting entity;
paths can cross contexts in the authorized view. `limit`/`cursor` page paths;
budget exhaustion returns partial coverage. With `from_view` and `to_view`,
impact classifies added/removed declarations and changes to declaration shape,
metadata, relations and unit inputs. `inputs` reports changed analysis inputs,
not a proven edit to one function body. Comparison results keep evidence and
snapshot vectors separated by view, including paths for removed declarations.
`comparison.reanalysis` reports changed direct inputs and observed dependencies
for units present in both views. Producer channel values are compared across
views, so unchanged outputs stop propagation. Missing producer evidence is
marked unavailable. Each unit lists its stage's associated build/test tasks;
these are configured task associations, not inferred test coverage.
`comparison.duplicate_types` suggests existing public types in the same
project when a newly added type has the same full shape of at least three
public instance fields or properties. Each match reports both declarations, their names,
the shared shape hash and the member count. The shape ignores member names and
order and preserves each field's type structure. `comparison.topology` reports
new cross-module dependency cycles and modules whose direct dependencies at
least doubled and grew by three or more. C++ declarations outside named modules
use their defining file when it can be resolved uniquely.
`comparison.degraded_types` reports existing parameter and return types that
changed to a language's configured escape type, along with newly introduced
unsafe casts recorded by its semantic provider. `limit` and `cursor` page the
comparison results. Traversal shares the request budget. Both views remain pinned
during the query.

Configured stdio helpers use the `external` server entry. Its options contain
`registration` (a provider description with its own `id`, helper adapter, roles,
ports, facts, operations, channels and configuration schema), `documents`
(extension → language ID), `argv`, mapped auxiliary `tools`, `env` and
`initialization`. The registration ID is the provider identity in sessions,
units, recipes and emitted identities; `external` only selects the host
transport. Commands use the same `${tool:name}` and `${tree}` expansion as
built-in helpers. Helpers receive the existing open/update/units/evaluate/close
JSON-line protocol and publish the common shard records. The `helper` key of
`language schema` describes every message, parameter and result of that protocol. Registrations declare
only the implemented `evaluate` operation; definition/reference queries consume
its exported records. Their initialization configuration is interpreted by the
helper. `language describe` with a project
and optional configuration includes its configured registrations. Names cannot
shadow built-in providers, and one profile must give each registered name one
contract. Declared ports participate in the same stage graph, input installation
and channel-based reuse as built-in providers.

`language` with `action: "schema"` returns JSON Schemas for the S7 request,
result, shard and view records without starting a worker. The shared records
describe structured types, occurrences, references, relations, diagnostics and
unit-owned contributions. Native declaration keys include project, module,
provider and identity domain; local declaration keys use source file/declaration
anchors and their owner. The gateway stores immutable unit contributions and
view manifests under `server.storage/.semantic`. Source declaration anchors
assign persistent symbol IDs during publication. clice exports structured
types, scopes and references through its worker session. For structured analysis,
the adapter runs `clice index` once per installed input epoch, then reads saved
TU contributions with `clice query`. Each TU supplies a declaration contribution
and separate file-record contributions under the same TU context. Declaration
channels retain evaluated constants and inline/template implementation inputs;
body-only edits can reuse the declaration shard. The live server owns the writable index;
CLI operations use its initialization overlay and the slot's environment.
Each saved TU has its own context, such as `cpp/tu/main.cpp`. Shared header
declarations retain one symbol ID and separate types, scopes and coverage for
each context. Select an item's context when inspecting an ambiguous declaration.
Headers already covered by installed TUs use those contributions. Diagnostic
requests and uncovered files use immediate semantic extraction. Index worker
counts follow the slot's parallel allocation.
The running query
actions are scopes, symbols, definitions, references, relations, diagnostics and inspect.
Reference queries page uses of the selected symbol with `limit` and `cursor`;
the cursor retains its view and query conditions. A source edit publishes a new
view while an existing view continues to answer from its original inputs.
`language` with `action: "describe"` returns registered provider descriptions;
`provider: "clice"` selects one. Each description includes its configuration
schema, typed input/output ports, languages, operations, exported facts, evaluation unit,
update granularity and tool requirements. The `schema` response also includes
these descriptions. LSP-based adapters negotiate their available facts with
the running server. A view records the facts the server supplied: querying
symbols or references in a diagnostics-only view returns partial coverage
with the missing fact, while its diagnostic query remains complete.
Kotlin uses Analysis API locality to keep FIR sessions across function-body edits
and invalidates consumers when inferred declarations or inline implementations
change. A cross-module declaration read is qualified by the declaration names the
consumer's lookups could reach, so unrelated declarations in a dependency keep the
consumer valid; inline bodies and sealed hierarchies keep whole-module reads. F#
file demands use file checking and export only those files; a subsequent
project demand fills in the remaining records. Reuse checks the contribution's
coverage as well as its inputs, so a partial module result cannot answer a broader
project demand.

Worker providers share open/update/evaluate/close sessions and declare any additional
resolve operation they implement. Kotlin and F# helpers export records through
evaluate; definitions and references query those records in the store. A
helper registration with `dependency_reads: true` also implements the `observe`
maintenance RPC. Before offering persistent candidates for reuse, the host sends
`{session, reads}` for their producer-free declaration, lookup, constant or body
channel dependencies. The helper observes its currently installed project and
returns an array of common input objects with `kind: "channel"`, `name` equal to
the dependency key, `object` equal to its current semantic value and empty `type`.
Unavailable reads are omitted; they cannot validate a cached result. The gateway
compares these values through the existing dependency validators. A helper that
does not declare this capability receives no observe calls; declaring it requires
the RPC to be implemented. Observe is separate from the registration's unit
`operations`, which remain `evaluate` for external helpers.
Their evaluate response also includes `producer_keys`, mapping an observed read
key to the producer's structured `unit_key` values. The helper uses the same keys
as its unit inventory; the host derives opaque unit IDs and stores them in the
read's `producers` provenance. Its scalar `producer` remains absent, so frontend
observations continue to validate through installed channel inputs. This explicit
provenance connects module changes to filtered reanalysis without interpreting
provider-private lookup scopes.

Each session has an ID and an input epoch: source or prepared-input changes advance
the epoch after synchronization, while repeated installation of unchanged
inputs preserves it. Evaluation binds the installed epoch and source/prepared
input objects to its demand and budget. File objects come from the installed
source manifest, including inputs without a language document. clice TU
contributions name their main file, runtime and prepared artifacts directly;
observed reads bind individual source objects or the containing artifact object.
SDK reads, missing files and source locations bind the named SDK object and its
relative member path. Unbound external reads keep their facts but prevent
result caching. Session messages carry
the source snapshot; individual file bindings come from TU contributions.
A shared job retains its computation ID across worker admission retries; each execution has a request ID, and the
analysis configuration names its stage. Units, progress, cancellation and the
final report carry these execution identities. The gateway checks their input
and execution binding before publication. LSP evaluation selects declaration
or diagnostic requests from the demanded facts; clice evaluates the demanded
contributions from its saved TU export. Diagnostic-only and uncovered-file
requests retain the immediate path. Interrupted operations
leave the session failed and the worker closes it before reuse.
Each evaluation also names its slot. Binary `facts_batch` chunks
carry at most 1 MiB of data, with a 64 MiB limit per serialized unit.
A separate `unit_committed` event identifies that object and lists its complete
contribution inventory. The receiver checks the slot, epoch and inventory
before anchoring and persisting the contribution; staged facts alone never
publish. A `completed` or `failed` event closes the evaluation, and its committed
unit count must agree with the final build reply. Failure discards uncommitted
staging. The final view selects committed contributions atomically; the build
reply contains their count and coverage summary. Cancellation uses the request's correlated cancel message,
stops the active evaluation and retains units already committed to storage.
Queries accept `budget: {"milliseconds": 5000, "units": 100, "nodes": 20000}`
to bound analysis time, source documents and returned semantic records. These
limits also account for clice's structured exports. Exceeding a document or
record budget returns `resource_limit`; an unfinished unit contributes no
records, while earlier committed units remain stored. On a fixed view, `nodes` bounds the returned symbols and type records, and exhausted traversal
is reported as partial coverage.

Executable mappings and prepared-tree environment bindings apply on workers.
An analysis profile contains named `stages` and `outputs`. Each stage specifies
`server`, `build_configuration`, typed `inputs` and optional `after` stage names.
Build input references select task output ports from named configurations:

```json
{
  "stages": {
    "cpp": {
      "server": { "provider": "clice", "options": {} },
      "build_configuration": "native",
      "inputs": {
        "compilation_database": [{ "configuration": "native", "task": "configure", "output": "compilation_database" }]
      },
      "after": []
    }
  },
  "outputs": { "source": [{ "stage": "cpp", "output": "facts" }] }
}
```

Each input names its producing build configuration, task and output. Clice can
consume compilation databases from several configurations; its cache is the
worker's, kept outside the build tree. Ty's `python_environment` port selects one uv task's output; direct
server launches with several environments require an explicit `python` selection. The planner preserves input order, expands only
the selected producers and their prerequisites, and keeps each preparation task's
environment and output ownership tied to its configuration. `build_configuration`
sets the stage's execution defaults and server environment. Every participating
configuration's process limits apply. The planner checks port types, output
references and stage dependency cycles.
`language` with `action: "plan"` returns the named analysis and preparation nodes,
their input bindings, dependencies, output types and commands. Input bindings retain
the producing configuration/task/output or stage/output reference; dependencies
record execution order. Its optional `outputs` list selects
profile outputs; omitting it selects all declared outputs. The same selection
controls queries and includes each selected stage's `after` prerequisites.

Kotlin and F# source stages consume the build task's `project_model` output.
Each runs as a platform host that loads one language adapter: map
`fughetta-jvm` and `fughetta-kotlin` to the JARs installed under
`libexec/fughetta-service/jvm` and `kotlin`, and `fughetta-clr` and
`fughetta-fsharp` to the assemblies under `clr` and `fsharp`, alongside `java`
or `dotnet`. The worker launches `java -jar fughetta-jvm.jar --adapter
fughetta-kotlin.jar` and `dotnet fughetta-clr.dll --adapter fughetta-fsharp.dll`.
CLR artifact stages run the same `fughetta-clr` host without an adapter.
Native artifact stages run `fughetta-llvm` from `libexec/fughetta-service/llvm`;
it is built when the configure step finds LLVM's CMake package beside the compiler
(on Debian and Ubuntu, `llvm-23-dev`).
Explicit worker tool mappings can name readable helper resources as well as
executables. Only declared artifact inputs
are installed as provider models; preparation receipts remain separate inputs to
the build tasks.

A `jvm-artifact` stage consumes a `jvm_artifact` through its `artifact` port;
a `clr-artifact` stage consumes a `clr_artifact` there and accepts an explicit
`portable_pdb` through `symbols`. Both accept `source_facts` from their source
stage to publish `emits` relationships. JVM association uses class line tables
and the primary SMAP stratum; CLR association uses the supplied Portable PDB.
JVM class signatures contribute scoped type parameters, bounds and parameterized
base types to the normal type graph. Class, field and method annotations appear
under `jvm.annotations`, with their descriptor, runtime visibility and tagged
element values; nested annotations and arrays retain their structure.
Source file and function-body ownership must be unambiguous. Select the source
profile output to query before compilation, and an output containing these
readers and the binding resolver to prepare linked facts on demand. A stage's
fact ports expose that stage's own contributions. The final query view retains
its upstream facts so source and artifact endpoints remain navigable.

Fact ports also accept an analysis output from another project:

```json
"native": [{
  "project": "native-lib",
  "worldline": "release",
  "configuration": "native",
  "output": "facts"
}]
```

The referenced output must have type `semantic_shard`. An optional `version`
selects a fixed source revision; otherwise each new call selects the dependency
worldline's current revision. The view retains every participating project's
snapshot, configuration and toolchain selection. Continuation keeps those
selections while a fresh call can observe later dependency changes. The gateway
checks the declared project edge and delegates only the reads needed by that
analysis; it does not grant the root task direct access to the other project.
Cross-project input changes and their configured build/test tasks appear in
view comparison and impact results.

A `native-artifact` stage reads an executable, shared library or relocatable object
(ELF, Mach-O or COFF) with `fughetta-llvm`, a host linked against LLVM's object,
DWARF, PDB and IR libraries. Declare that build output with type `native-artifact`, then bind
it to the stage's `artifact` port. The optional `source_facts` port accepts stage
outputs; the gateway exposes their committed shard objects and requests source symbols
and definitions from their producers. For example, alongside the `cpp` stage:

```json
"native": {
  "server": { "provider": "native-artifact", "options": { "module": "app" } },
  "build_configuration": "native",
  "inputs": {
    "artifact": [{ "configuration": "native", "task": "build", "output": "app" }],
    "source_facts": [{ "stage": "cpp", "output": "facts" }]
  }
}
```

Expose `native/facts` through a profile output to request it. Selecting only the
source output prepares compilation commands without linking. The reader records
linkage names, imports, exports and artifact locations. The host reads DWARF, a
Mach-O `.dSYM` bundle or a PDB for the debug positions of `emits` relationships to
source definitions. For a native definition with no debug line coverage, the
reader also uses the ABI name clice publishes in `native.linkage_name`. An exact
match to one source definition produces `emits` with `linkage_name` evidence.
Relationship endpoints carry their own contexts, and the selected
view retains the source shards needed to inspect either endpoint. Configure a worker
mapping for `fughetta-llvm`, or set the stage's `host` option. A separate `directory_group` allows the source and artifact
stages to run on different workers. Stage requests carry shard IDs; workers fetch
objects on demand and share their cached records. Each provider declares its output
channels. Source providers expose `declarations` and `definition_bodies` across all
shards of a unit: declarations omit display origins, while definition bodies retain
function identities and definition locations. Republishing identical values keeps
these channels stable. Native units consume `definition_bodies` and reuse stored facts when
those values, the measured artifact, toolchain and source files read for character
position conversion match. Their source document selection is an `enumeration`
read over the installed manifest, with content reads recorded separately. File-set
reads select named files or recursive directories; additions, removals and renames
withdraw their cached consumers, while changes outside the selection leave them
available. Candidates retain these reads in SQLite across gateway restarts.
Deferred units remain available to continuation.
The optional `ir` port accepts `llvm_ir`, either textual IR or bitcode. Direct calls and available static JNI
registration tables become native relationships and binding evidence.

A `pinvoke` stage consumes CLR facts through `managed` and native artifact facts
through `native`. Its `libraries` option maps each DLL import name to the native
reader's module name, for example `{"libraries":{"native":"app"}}`. Both ports
accept multiple stage outputs. The resolver emits `binds` relationships with
endpoint contexts, entry point, calling convention, library selection and
parameter slots. Missing or ambiguous exports produce diagnostics; ambiguous
matches remain candidate relationships. Scalar slots use identity conversion;
string, array and by-reference struct mappings retain their CLR marshal
descriptors, parameter directions, size parameters and layout. Other explicit
marshaling operations retain their metadata. Resolver reuse checks the worker
runtime and each input port's selected `linkage` channel collection. Collections
retain project/module selection and producer membership, including empty lookups.
JNI contributions are grouped by project and JVM module; P/Invoke contributions
by project, CLR module and import library. Adding or
withdrawing a library invalidates the result even when existing libraries remain
unchanged; producers outside the selected modules and unrelated call-graph
channels do not participate in this check.

The `jni` resolver uses the same `managed` and `native` fact ports for JVM and
native artifacts. Map a JVM module to its native modules with, for example,
`{"libraries":{"sample":["native"]}}`. It follows JNI short/long export naming,
including Unicode escaping and overloaded descriptors, and records the implicit
environment and receiver/class slots. Competing exports remain candidates.
Static exports and resolved `RegisterNatives` tables can bind JVM methods;
registration-table evidence can select a local native target without an export.

Both resolvers also bind source declarations before anything is compiled. Wire
Kotlin or F# source facts to `managed` and clice facts to `native`; the source
providers publish a `linkage` channel for this. A Kotlin `external` function
binds to a C++ function definition in the same project named by its JNI short
name, or else by one of its long names; the JVM class comes from the
declaration's package and classes, and a top-level function belongs to the
`<File>Kt` facade. `@file:JvmName`, `@JvmName` and `@JvmStatic` are not
followed; such declarations bind through their artifacts. An F#
`[<DllImport>]` function binds to a C++ definition at namespace scope whose name
is its entry point. Source bindings use the `source` library selection strategy,
need no `libraries` mapping, and have no artifact candidates or descriptor
parameter slots. A declaration without a source definition is skipped without
a diagnostic, since its native function may come from a prebuilt library;
competing definitions remain candidates. Artifact bindings are evaluated
separately and stay unchanged.

Project `analysis_defaults`, profile `defaults`, then stage `execution` supply
execution settings in that order. `fact_limit` and `relation_depth` control the
query defaults. `execution` contains `requires` (tools/platform), `limits`
(timeout, retained output, memory and CPU), and `budget` (milliseconds, units,
nodes). Required tools accumulate; platform selections use the nearest setting
and must satisfy the bound build's requirements. Build limits also cap stage
process limits. Request budgets can only tighten the configured budget for each
selected stage. Plan nodes report effective execution settings and their field
`origins`. Query defaults are stored in the published view and retained across
pages. Scheduling order, execution budgets and process limits are excluded from
the stage's semantic recipe.

Stages execute in dependency order against one pinned source version. Each stage
has its own semantic context; selected outputs become one immutable query view.
`execution.directory_group` inherits project and profile defaults and otherwise
uses the bound build configuration name. Stages assigned to the same group share
a directory and combine their worker requirements. Declared outputs from distinct
producers must have distinct paths within that group. Plan nodes identify their
directory group; `directory_groups` lists each group's required tools and platform.
Preparation task identities include both the group and the build configuration.
Directory pools belong to a project, directory group and selected runtime;
worldlines share these pools. An idle directory is selected by overlap with its
last installed source objects. The worker synchronizes from that directory's
actual installed manifest, restoring, replacing and removing tracked source
files before updating each stage's session epoch. Concurrent computations use
distinct reserved directories.
Set a build configuration's `relocatable_build_dirs` to the project-relative
directories it can move between checkouts. A new fork slot can seed those
directories from the fork-point snapshot of an idle slot with the same project,
build recipe, layout, and toolchain. The Worker uses native file cloning when
available and independent copying otherwise. A successful build retains its
fork reuse record across Worker restarts on the same volume. Keep directories with
checkout-specific absolute paths, such as a CMake build tree, out of this list.
One queued computation reserves a directory from each required group pool until
all selected stages finish, including when the stage order returns to an earlier
directory group. Within a live worker directory, subsequent stages reuse completed
preparation tasks for that computation and source snapshot. Plain builds,
source changes, failures and session reclamation discard these preparation
receipts. Each worker prepares its own directory when execution moves between
workers. Saving settings validates the graph without executing it.

A generating build task can set `generated_manifest: "generated.json"`. Its last
step writes `{"files":["generated/value.hpp"]}` to `${reports}/generated.json`;
paths are relative to the working tree and must lie within the task's declared
outputs. The list is the producer's complete current file contribution. An empty
list withdraws its former files while preserving other producers' files. Ownership
is retained across worker restarts and scoped by build configuration and task.
Each execution includes the complete producer inventory for its directory group;
producers leaving the selected graph withdraw their files before preparation.
Analysis stages sharing a directory use the same inventory so one stage preserves
another stage's outputs.
Generated file objects enter provider inputs, so changes and withdrawals update
session epochs and invalidate consumers. Each successful generation currently
assigns a new object to its reported files. Interrupted or invalid generation
leaves the slot unfinished; its next use retires sessions and recreates the tree
before installing source and running preparation again.

The shared semantic store retains up to four input states for each stage recipe
and unit. Candidates reference immutable shards independently of query views.
A stage batches new declaration anchors and publishes its completed units in one
view transaction. Reused units retain their stored anchors. Configuration names
remain in `view.configuration`; session and recipe `options` identify the effective
stage and build settings.
SQLite serves identities, views, recipes and leases by key. The identity cache is
bounded, and shard metadata, references and identity membership are stored in
tables. Collection follows removed references and a deletion queue; it does not
read surviving shard indexes. Query lease renewals are batched in memory.
Recipes have explicit recipe ID, project, options and access-time columns.
Accepted reused contributions refresh access times in one UPDATE per publication;
pressure evicts a batch in LRU order and collects released shards once.
A project-qualified `candidate_inputs` table indexes source content, file membership
and directory reads. Source changes mark only their readers for revalidation;
unchanged candidates retain their previous source check, including across restart.
Each querying project keeps its own last checked source vector, so querying another
project does not withdraw unrelated inputs. This metadata is collected when the
project no longer has candidates, and the project's in-memory resolved-source states
are dropped with it.
Candidate payload decoding and resolved source checks use bounded caches: resolved
results are remembered per querying project and exact source vector, so returning to
an already checked worldline state reuses them without decoding payloads or
rechecking, while the durable `dirty` and `source_valid` columns remain the source of record
across restart. Reclamation scans recipes in bounded `(accessed, key)` pages and
resolves each page's shard references with one query, adjusting shared-shard counts
as the page is walked. Runtime roots keep view, candidate and shard IDs in separate
sets.

Input kinds describe common categories. Provider-specific contracts use
`kind: "provider"` and a namespaced `type`, such as `clice/configuration`, with a
validator declared by that provider. Output-channel inputs use `kind: "channel"`.
Gradle/MSBuild project models use the installation's existing `model` or `projects`
fields; additional providers do not extend the installation structure.
The gateway selects source reads against project-qualified snapshots, then uses
installed inputs grouped by project to validate artifact, model and SDK reads.
After clice refreshes the unit inventory, the worker reports installed inputs. The
gateway's dependency validators compare source objects, prepared artifacts,
runtime/SDK inputs and recorded include reads, including missing include paths,
and return the reusable candidates. Clice also exports directories
enumerated by the compiler, including reads inherited from PCH and module inputs.
Implicit Clang modules retain those observations in their PCM; changed directory
membership rebuilds the affected cached modules before producing the consuming unit.
Directory observations participate in clice's index invalidation and persist across
restart; the service records their source-manifest membership, or the bound SDK or
generated input for directories supplied by those producers. A match skips semantic
export and publishes a new view over the existing shards. Reused records retain
their stored declaration anchors and count toward the node budget.
`language.reused_units` reports these reused contributions. Candidate metadata
survives gateway/worker restarts and is reclaimed independently of query views
under the shard budget.

Workers report a capability inventory and an installation ID before accepting
work. Configure distinct installations as `tools: {"java@17": "/opt/jdk-17/bin/java", "java@21": "/opt/jdk-21/bin/java"}`.
The Worker reads the JDK's `release` file and binds the selected installation's
`JAVA_HOME`. Discovering the same installation under several names for one tool
produces one instance. The configured instance ID takes precedence.
Projects express constraints in `requires.tools` and `requires.sdks`,
for example `{"tools":["java>=17,<18"],"sdks":[{"name":"native","version":"^2"}]}`.
Use `${tool:java}` and `${sdk:native}` to refer to the selected paths. Standalone Worker
configuration can supply
`sdk_manifests: {"native": {"manifest": "/opt/sdk/release.json", "roots": ["/opt/sdk"]}}`
for other managed SDKs. Each manifest identifies the immutable contents of its
installed directory. An SDK key has one or more slash-delimited name segments
and can end in `@version`. Use its logical name in `requires.sdks` and
`${sdk:name}`. A configured binding owns its installed root. For a binding named
`jdk`, the Worker reads the installed `release` file and supplies `JAVA_HOME`.
A binding may list several spellings of that directory,
including symlinks, but all must resolve to the same real directory. Different
bindings must have distinct canonical roots; nested roots use the provider host
contract above. This also applies to automatically detected Java/Android bindings. `ANDROID_HOME` and
`ANDROID_SDK_ROOT` describe one SDK and must resolve to the same directory. Android package
manifests contribute to the enclosing SDK identity. The manifest identifies the
SDK in semantic dependencies; installed roots bind it locally without rereading
individual SDK headers. A semantic unit records the content digest of each tool
it uses under that tool's normalized path. Process environment inputs record a
digest per variable supplied to those processes. In-process readers record the
artifacts or semantic collections they read. An executable without an
installation digest makes the affected unit ineligible for reuse.
C++ contributions record the clice processes and bind the exported compiler
and SDK reads to the Worker's installation observations. clice stores those
reads with its PCH, module artifacts, and saved TU exports. Diagnostic pulls
which omit compiler observations are evaluated each time. The exports also
report the compilation database commands each contribution used. When those
match clice's current inventory, Fughetta records the command inputs and the
generated files read by that contribution.
LSP providers keep their existing SDK inputs at installation scope. Directory
recipes include the platform and the digests of the executables selected by
their build steps. Changing an unused tool leaves a unit's recorded inputs and
reuse key unchanged.
Custom executables must be mapped in `tools`; missing or
unreadable executables are omitted from advertised capabilities. SDK releases
and tools must remain immutable while the
worker is connected; restart the worker after updating them. Project and step
environment overrides are included in the computation key.

The gateway schedules build and language computations by source content and
the selected installation, independently of caller, worldline, or version.
Every caller has its own audit
record, including cache/shared execution status; only actual executions consume
execution quota. Test and run calls each execute their runtime steps. Push-only
diagnostics without a matching document version return `stale: true` and are
not cached. Even version-matched pushed diagnostics are excluded from the shared
cache because a document version cannot identify changes to its dependencies. A
later query can observe diagnostics received in the background.
Invalid symbol selections return `invalid_record` without restarting the server.

`build` with `fresh: true` bypasses cached and in-flight results, retires the
selected directory's stage sessions, and recreates that working directory
from the source snapshot before running the build. Shared dependency caches
remain available. Builds without `fresh` preserve generated outputs for incremental
execution. A clean build detaches its old directory under the store lock and
deletes it after releasing that lock. Worker startup reclaims detached directories
left by an interrupted deletion.

To commit a formatter or generator's source changes, pass
`capture_worktree_changes: ["src", "include/generated.h"]` and a stable
`operation_id` to `build`, `test`, or `run`. The Worker starts from the selected
snapshot and captures files under those paths after successful execution.
Configured build directories, tool metadata and links are excluded. The
Gateway publishes one Worldline contribution and returns `worktree_commit`.
The same request and operation ID retrieve the receipt without running the
command again. If a captured source file changed since execution began, the
result includes `worktree_error_code` and the contribution is rejected. Use
Worldline history to identify the contribution for `reverse`.

The Console exposes component-specific editors. Local executable versions,
behavioral acceptance, and remaining deployment limits are recorded once in the
[toolchain acceptance record](../fughetta-console/DEPLOYMENT.md#c-and-python-toolchains--october-27-2026).

### Named source checkpoints

`worldline` action `reference` records a name in one worldline. Supply `version`
(an opaque version, a name, or `@name`), `operation_id`, optional `intent`, and
`reference: {name, kind, expected, attestations}`. `kind` is `tag` or `channel`;
`expected` is empty when creating a name and is the previous reference receipt
when moving a channel. Tags cannot be replaced, and names cannot change kind.
A stale channel update returns `stale_reference`.

Each attestation has `kind` (`build`, `test`, or `review`), `receipt` (empty when
there is no linked receipt), and a nonempty `conclusion`. These are statements by
the authenticated caller; recording one does not execute a build or test.
The result includes `references`, with actor, intent, selected version, creation
time, operation ID, current/previous receipt and a monotonic sequence. Pointer
updates and their operation bindings commit together without changing source
history. Replaying identical input or requesting `receipt` with the original
operation ID recovers the original selection even after a channel moves.

`references` lists the current names; `reference_history` lists their append-only
change records. Both page by numeric `after` sequence and `limit`, with
`truncated` indicating more results. Names are scoped to the selected worldline.
`inspect`, `read`, `compare`, `fork`, and `history`, plus build/test/run version
inputs, accept `@name` or a bare name. Execution resolves a channel once before
scheduling its fixed source input. Internal source snapshots are local derived
state: `identity` reports their digest as `state_snapshot` and their coverage as
`snapshot_events`, and a node opened with its rebuild flag replays the archive
instead of trusting them.

## Concurrency budget

The configuration owns the execution thread count and subprocess parallelism:

```json
"runtime": { "threads": 10, "jobs": 3, "workers": 2 }
```

| Setting | Allocation |
| --- | --- |
| `threads` | Execution threads across gateway, source, and workers. Each process gets one I/O thread. Gateway and source each reserve two blocking workers; each compute worker gets one. Remaining threads are split between the gateway and source blocking pools. Sleeping main and signal-watcher threads are outside this count. |
| `jobs` | Parallel build jobs, divided evenly among workers and passed to toolchains as parallelism hints; source runs no subprocess work. |
| `workers` | Worker process count. Each managed worker admits one task at a time, including its language-analysis phase. |
| `worker_memory_bytes` | Optional total resident-memory budget for managed workers, divided evenly between them. A standalone worker configures its share as `memory_bytes`. |

`threads` must be at least `6 + 2 * workers`; `jobs` must be positive and at least `workers`.
A file-only deployment can use `{ "threads": 6, "jobs": 1, "workers": 0 }`.
When a self-hosted managed entry omits the `runtime` object, it sizes its startup
budget from the available logical CPU count. It starts one Worker on up to
three CPUs and two on larger machines, with the remaining CPUs available for
build jobs and a minimum of one job. Set `runtime` to choose an explicit budget,
including an optional Worker memory limit. Hosted controllers can update their
budget through the host-only
`GET`/`PUT /api/v1/config/runtime` endpoint. PUT uses the returned numeric
`revision` and a budget in `value`. A stale revision returns 409; an accepted
change returns 202 until it is applied.

Status separates `desired` from `applied` and reports `starting`, `ready`,
`pending`, or `draining`. Accepted work finishes before internal processes are
rebuilt. New work receives 503 while draining; status reads, job inspection,
cancellation, and CORS preflight remain available. The main process retains its
PID and loads the persisted budget for the next process group. Hosted operators
manage workspace runtime budgets through the Console's `/admin` UI or
[`fugadmin`](../fughetta-console/fugadmin/README.md).

The top-level `tools` map supplies executable paths and readable tool resources
to workers, including helper JARs, assemblies and model-export scripts. Automatic
PATH discovery selects executables; interpreted resources need explicit mappings.

Build environments receive the assigned parallelism for CMake, CTest, Make,
and OpenMP; JVM `ActiveProcessorCount` and .NET `DOTNET_PROCESSOR_COUNT` receive
the same allocation for their runtime thread pools.

Memory admission distinguishes reservations from measured resident memory. A task
with `max_memory_bytes` reserves that allowance; an undeclared allowance reserves
the whole available task budget and runs alone. Idle language processes are
reclaimed in LRU order to make room. Worker and child-group resident memory are
reported with the session inventory; the existing reclaim loop stops work with
`resource_limit` when observed usage exceeds its budget. Existing platform process
limits remain separate from this resident-memory accounting. When an execute
role has started and its resident-memory report is pending, the front waits
up to five seconds for the report before returning `worker_overloaded`.

Gateway metadata stays in `server.storage`; source data lives in its `source/`
subdirectory, and worker caches in `workers/local-<index>/`. Internal listeners
use loopback. Back up the configuration and the complete storage directory.

### Local performance measurements

The comparison used the same Linux/WSL Debug configuration with sanitizers,
real Source and Worker processes, and loopback transport.

| Change | Workload and metric | Before | After | Reduction |
| --- | --- | ---: | ---: | ---: |
| Cached package usage and direct SQLite file accounting for source quota | Median latency of 35 single-file Worldline writes with 8,000 package files on disk | 286.691 ms | 259.591 ms | 9.5% |
| Reuse unchanged checkout manifests and skip the filesystem sync | Linux `syncfs` calls across six repeat checkouts of a 1,000-file snapshot | 6 | 0 | 100% |
| Send the snapshot ID for a retained Worker offer | Bytes per warm `sync.offer` frame, including its header, for a 1,000-file snapshot | 132,052 B | 131 B | 99.9% |
| Validate delegated reads with the origin Worldline head | Median latency of 40 Source RPCs with a 1,035-file origin | 35.121 ms | 1.376 ms | 96.1% |

The first checkout in the 1,000-file run issued one filesystem sync in both
builds. Repeat checkouts previously spent a median of 2.053 ms in `syncfs`.
Checkouts that change tracked files and builds that commit a durable warm
marker still sync. The first offer for an unknown snapshot sends the full
manifest. Median time in the warm synchronization stage went from 165.307 ms
to 152.045 ms, an 8.0% reduction. The cached package usage is loaded when
Source starts and refreshed by package operations. A source commit reads
the SQLite file sizes for the workspace's projects. These figures describe
the fixture workload.

The first field-declared MCP writer derived JSON field names and presence rules from
the response's C++ member types. It writes the envelope and the dynamic
result in one pass. A single-core Clang 23 `-O3` microbenchmark compared
serialization of the same synthetic results. It checked byte equality
against the previous writer before timing.

| Response bytes | Previous reflect-cpp writer | Field-declared writer | Speedup |
| ---: | ---: | ---: | ---: |
| 307 | 0.699 µs | 0.322 µs | 2.2× |
| 10,347 | 33.630 µs | 9.653 µs | 3.5× |
| 330,167 | 1,347.880 µs | 296.819 µs | 4.5× |
| 1,323,959 | 5,524.352 µs | 1,223.607 µs | 4.5× |

The same compiler took a median of 2.660 s for the previous isolated codec
translation unit and 2.037 s for the field-declared version over three
compiles. The corresponding object files were 144,256 and 27,768 bytes.

MCP now writes through the shared JSON encoder. In the same single-core
Clang 23 `-O3` setup, the two writers produced identical response bytes
for these synthetic replies.

| Response bytes | MCP writer → shared writer |
| ---: | ---: |
| 186 | 0.290 → 0.190 µs |
| 7,270 | 9.925 → 4.495 µs |
| 237,061 | 485.322 → 145.240 µs |
| 954,682 | 1,030.786 → 546.305 µs |

The shared JSON codec uses yyjson for full-document validation and
serialization. The vcpkg port enables yyjson's fast floating-point
conversion by default. yyjson 0.13.0 has no SIMD build option.
The project's C++23 field traversal drives JSON, XML and Schema
generation. Explicit field names live in the field types. Plain aggregate
fields use names resolved by Clang at compile time. Dynamic JSON uses an
owning value with insertion-ordered object members.

This historical single-core Clang 23 `-O3` comparison measured the
migration to the simdjson backend on typed synthetic records.

| JSON bytes | Full read before → after | Write before → after |
| ---: | ---: | ---: |
| 200 | 0.187 → 0.171 µs | 0.161 → 0.139 µs |
| 8,474 | 4.815 → 5.821 µs | 4.815 → 3.496 µs |
| 273,735 | 154.774 → 192.574 µs | 153.744 → 103.886 µs |
| 1,098,567 | 664.704 → 714.848 µs | 598.603 → 451.556 µs |

Generating a synthetic tool's input and output schemas together took
13.965 µs with the previous JSON round trip and 4.226 µs with direct
generation from type metadata under the same benchmark configuration.

JUnit XML declares attributes and child elements in its C++ field types.
The shared XML module uses that metadata for both directions and calls
pugixml directly to parse documents and write UTF-8.

The same single-core Clang 23 `-O3` setup measured XML parsing and
typed field mapping on synthetic JUnit reports. Both readers use
pugixml 1.16. The writer column measures the new XML API.

| Test cases | XML bytes | Read before → after | Write |
| ---: | ---: | ---: | ---: |
| 16 | 1,105 | 1.688 → 1.384 µs | 1.232 µs |
| 256 | 16,483 | 22.615 → 20.391 µs | 17.508 µs |
| 4,096 | 266,728 | 653.007 → 673.981 µs | 649.186 µs |

The reflect-cpp removal was measured against the preceding simdjson and
pugixml implementations using the same single-core Clang 23 `-O3` setup.
Typed JSON reading uses compile-time field dispatch. The writer also
precomputes the JSON spelling of property names. The synthetic
JSON fixtures produced identical bytes before and after.

| JSON bytes | Typed read before → after | Typed write before → after |
| ---: | ---: | ---: |
| 200 | 0.167 → 0.141 µs | 0.127 → 0.110 µs |
| 8,474 | 6.002 → 5.566 µs | 3.738 → 3.304 µs |
| 273,735 | 179.664 → 164.270 µs | 108.120 → 95.074 µs |
| 1,098,567 | 743.272 → 679.808 µs | 453.994 → 416.980 µs |

Generating the representative record's Schema took 3.463 → 1.239 µs.
Compiling the isolated JSON read, write and Schema fixture took a median
of 3.86 → 2.39 s across three compiles of each implementation. Converting
already-parsed dynamic JSON directly into typed gateway token records took
4.208 → 0.506 µs for 16 records and 75.676 → 10.723 µs for 256 records.

| JUnit test cases | Read before → after | Write before → after |
| ---: | ---: | ---: |
| 16 | 1.399 → 1.393 µs | 1.235 → 1.250 µs |
| 256 | 20.395 → 20.652 µs | 17.073 → 17.175 µs |
| 4,096 | 330.493 → 340.588 µs | 545.034 → 542.206 µs |

## Source layout

`src/fughetta-service.cpp` is the executable entry. `src/service/` owns process
lifecycles; `src/gateway/` handles public requests; `src/source/` owns storage
and source history; `src/worker/` executes build and analysis tasks. Shared domain models live
in `src/model/`, execution helpers in `src/core/`, and I/O and platform providers
in `src/io/` and `src/platform/`. Tests live in `tests/`.

Development rules are in [AGENTS.md](AGENTS.md). Clients that sync files, such as `fug`,
use `POST /api/v1/projects/{project}/worldline`.
The route uses `tools::worldline::input` from `src/gateway/tools.cppm`.

Debug and Release builds, service test suites, and CLI Worker connections
have passed locally on Linux and Windows. Hosted budget control operates on
existing instances. Deployments provision hosts and set whole-container memory
limits.

## Hosted Core worker interface checks

Batch A uses local validation. The `edge` suite exercises jobs frames with
1-, 7-, 16-, and 4096-byte writes, consecutive progress/cancel frames, 64-bit
correlation IDs, exactly 64 MiB of payload, and rejection of larger declared
payloads before reading their bodies. `fleet` exercises shared-token admission
and progress routing; `dispatch` exercises snapshot-bound execution and worker
cancellation; `startup` exercises managed process startup. All four suites passed
on October 24, 2026. The associated gateway `units` and `core` suites passed,
as did 51 Console slug and workspace-retirement tests.

`jobs::protocol_version` hashes message codes, not message fields.
These checks record the October 24 test runs. See the
[workspace deployment guide](../DEPLOY.md#production-resources) for the current
provisioning configuration.

### Exported operation files

A build configuration can name files or directories to deliver after a successful
operation. For example, an existing build operation can add:

```json
{
  "tasks": ["compile"],
  "exports": { "application": "build/bin/app", "assets": "build/assets" }
}
```

Paths are relative to the worker's working tree. Exports are independent of the
provider's typed analysis outputs. The worker copies regular files into immutable
content objects before returning the result. Declared paths through links or
outside the tree are refused. Links found inside an exported directory are
skipped and listed as relative paths in `export_skipped`; regular files are
still delivered. One operation exports at most 4096 files and 1 GiB. Export capture
failures appear in `export_error`, preserve the process result, and prevent result
caching. Failed runs also export declared files that they produced; missing
outputs after a failed execution are skipped without an extra error. Successful
executions must produce their declared outputs. Delivered files include each file's name,
relative path, digest, size, and executable flag.

The gateway validates and stores the bytes before returning the result. Members
receive existing stored objects without another worker transfer when the digest
and size match, including after a fresh build. Members
can read chunks through `GET /api/v1/projects/{project}/exports/{digest}` with
`offset` and `length` (at most 4 MiB). A project must have received a result for
that object, including a shared cache result, before this route serves it.
`fug` downloads and verifies these files under its ignored local state directory.

Worker step output also goes to asynchronous log files outside the build tree.
Steps with a truncated inline summary or more than 8 KiB of output return a
`log` object through the same verified download route, including failed steps.
Language preparation requests keep inline output without publishing log objects.
The disk log budget is 64 MiB per execution, shared by its steps. `log_truncated`
reports exhaustion of this budget independently of the inline `truncated`
flag. A log storage failure is reported in `log_error` without discarding the
process exit status or its captured inline output.

A transfer or project download-grant failure after execution sets that file's
`delivery_error`. The completed steps, diagnostics, and exit status remain in
the result. Results with transfer errors are excluded from new result-cache
entries. `fug` preserves these errors without issuing another download for the
unavailable file.

Remote artifact and log objects expire after seven days without use. Collection
also targets 20 GiB of stored objects, oldest first, while protecting objects
created or read in the last ten minutes. This is a soft cache target during
active transfers. Worker maintenance runs collection at most once per minute;
the gateway collects on delivery and download activity at that interval.
Interrupted partial uploads and capture scratch directories expire after a day.
Project grants are removed with their content objects. If an object expires
while its computation result is still cached, the next call reports the delivery
error and invalidates that result; a later call can compute it again. Local
files already downloaded by `fug` are retained until the user removes them.
