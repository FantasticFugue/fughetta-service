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
source process and a worker pool for one workspace. The hosted platform places
`cloud-gateway` in front of each workspace for accounts, sign-in, and billing.

The front ends are separate repositories beside this one, each a static app on
the component library `@fantasticfugue/components` (`../components`):

| Repository | What it is |
| --- | --- |
| `console` | The customers' console: workspaces, projects, settings, history, insights |
| `admin` | The operators' console: users, workspaces and the gateway across every tenant |
| `agent-cloud` | A coding agent as an installable web app, working on a synced copy of a worldline |
| `landing` | Marketing pages, docs, blog, pricing and legal, prerendered |

## Dependencies and build

Requires CMake 4.0+, Ninja, vcpkg, and the xclang toolchain.
The service defaults to `uring` on Linux (kernel 6.1 or later), `iocp` on Windows, and `kqueue` on
macOS. Select another mechanism with `-DHTTPANT_IO_BACKEND=NAME` when configuring the service.
The manifest installs liburing on Linux.
httpant is fetched from the head of its main branch with `FetchContent` until
it has a vcpkg port, and built as a subproject, so its module BMIs use this
build's compiler; set
`FETCHCONTENT_SOURCE_DIR_HTTPANT` to build a local checkout instead. The
remaining dependencies come from the builtin vcpkg registry. Dependencies build
with xclang through the triplets in `cmake/triplets`.
`cmake/toolchain.cmake` reads two environment variables: `XCLANG_ROOT` names
the xclang tree and `VCPKG_ROOT` the vcpkg checkout. `XCLANG_TARGET` selects the
target, the host's by default. `CMakePresets.json` places each build in
`build/<preset>`; machine paths belong in a Git-ignored `CMakeUserPresets.json`
that inherits its presets.
Build products and dependency working directories belong under `build/`.

From this directory:

```sh
cmake --preset debug
cmake --build --preset debug --target fughetta-service
```

To run the HTTP front suite from a regular developer shell:

```sh
cmake --build --preset debug --target front-test
python3 tests/run.py --test-dir build/debug -- -R '^front$' --output-on-failure
```

The test entry runs CTest as the current user; no cgroup delegation or sudo is required.

The debug preset builds with `-g0` and address and undefined-behavior
sanitizers. Frame pointers keep sanitizer stacks symbolized by function name;
no file/line, type, or local-variable debug information is emitted.
`XCLANG_SANITIZER` selects the sanitizers, with their matching runtimes.
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

## Service lifecycle and workspace boundary

One workspace is one execution environment, shared by its projects and repositories.
It groups related repositories so they can use one another without requiring a single
large monorepo. Project names and source histories organize work; they are not OS
security boundaries. The service schedules jobs, owns source and worker processes,
and handles cancellation and shutdown. It does not provision isolated environments.
Container and VM lifecycle management belongs to cloud-gateway in the future and
is not implemented here. Run untrusted work only in an externally managed environment.

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
owns its source and worker children. Cloud-gateway coordinates environment
recovery without racing the direct owner. Provisioning remains outside this
repository; see the [gateway environment design](../cloud-gateway/README.md#workspace-environment-control-design).

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

The checked-in configuration listens at `127.0.0.1:18100`. Connect an HTTP MCP
client to `http://127.0.0.1:18100/mcp` with `Authorization: Bearer <token>`.
REST routes use the same token under `/api/v1`; `/healthz` is public.
Workspace subscriptions use `GET /api/v1/events`; the event contract and shared
validation record are in the [hosted gateway guide](../cloud-gateway/README.md#workspace-events).
`--help` prints the public commands. The `--role` arguments are internal process
entry points used by the supervisor and process tests.

Hosted cores use `server.auth.mode: "trusted"` and must set
`server.auth.workspace` to their immutable workspace ID. Every authenticated
request must carry the matching `x-fughetta-workspace`, including requests with
the host role. See the gateway's [core configuration](../cloud-gateway/README.md#cells-where-each-core-listens).

A registered Worker speaks plain TCP to its `gateway` endpoint (`host:port`).
On a self-hosted machine that endpoint is the loopback relay run by `fug worker`,
which holds TLS, DNS, and OAuth and relays each connection over the public
`/w/{workspace}/workers/tunnel` WebSocket. The managed workers continue to use
their loopback TCP endpoints.

`native::context` and `runtime::context` select `concurrent::queue` from the
`mpmc` core module for native submissions and blocking file work. Native TCP,
UDP, file, scheduler, and process types use the same choice. The installed
package exports this module for cloud-gateway. HTTP session queues retain
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
native socket and bounds pending ciphertext at 128 KiB; cloud-gateway uses
these modules for its public tunnels and sends `close_notify` after a normal end. Certificate rejection uses
`http::tls::verification_failure`; the local `tls` module retains URL parsing. Tree coordination state is released after its last active
or waiting build; Core scheduling retains at most 256 recent tree hints per
Worker. The hosted transport and its deployment limits are documented in the
gateway's [Worker tunnel entry](../cloud-gateway/README.md#worker-tunnels).

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

Exit status 2 means the Core refused the identity (revoked, unknown, or for
another Workspace); `fug` does not restart a Worker that exits with it.

The Core uses a separate private listener for registered Workers:

```json
"server": {
  "auth": { "mode": "trusted", "workspace": "workspace-immutable-id" },
  "remote_worker_bind": "127.0.0.1",
  "remote_worker_port": 9091,
  "remote_worker_enrollment": false
}
```

Point the gateway's private Worker address at this listener. Enable
`remote_worker_enrollment` on the Core and tunnels on the gateway to register
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
and includes offline and revoked identities. Owners rename with
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
revocation. Local validation and remaining
limits are recorded in the gateway's [verification entry](../cloud-gateway/README.md#verification).

## C++, Python, TypeScript and Bun toolchains

Projects store build graphs and language-service configurations. Create one with
`PUT /api/v1/projects/{project}` and a revisioned settings value; source-file
generation operates on existing projects. Import the project's own source and
build manifests before executing its tasks.

| Component | Execution and configuration |
| --- | --- |
| CMake / CTest | Configure and build with explicit directories, presets, compilers, variables, and targets. The CMake builder's `test` object controls CTest selection, parallelism, timeout, and failure behavior. An empty test selection fails by default. |
| vcpkg | Install a manifest into `install_dir`; the task exports the installed directory. Point CMake at the selected checkout's `scripts/buildsystems/vcpkg.cmake`, bind `VCPKG_INSTALLED_DIR` to that output, and set `VCPKG_MANIFEST_INSTALL=OFF` when the graph already performs installation. |
| clice | Read compilation-database outputs derived from the bound build graph, with explicit cache, compile-flag additions/removals, executable, and environment settings. |
| TypeScript / tsc | Check a selected `tsconfig.json` without emitting, or compile into an explicit output directory. Compiler options are typed fields; terminal diagnostics remain available without being parsed as a structured report. |
| typescript-language-server | Analyze TypeScript, TSX, JavaScript, and JSX through LSP. Configure tsserver, inlay/completion preferences, implicit-project settings, locale, and memory through initialization options. |
| Bun | Build and test only. Every operation automatically performs a frozen-lockfile `bun install` first, with lifecycle scripts disabled by default. Build entrypoints, output, bundler options, test paths, and name filters are typed; there is no `run` operation or arbitrary command argument field. Tests use Bun's native JUnit reporter. |
| uv | Lock, sync, build distributions, and run a command. Environment and distribution directories are explicit; dependency groups, extras, interpreter selection, and offline execution are modeled separately. |
| ty | Run type checks with import environments and rule severity settings; warnings fail by default. Its language server has independent diagnostic, completion, inlay-hint, and configuration settings. |
| Ruff | Check lint and formatting without rewriting source. The language server has independent rules, formatting, code-action, and configuration settings. |

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
Dependency archives are not covered by `max_cache_bytes` or the checkout TTL.
Operators must provision capacity and prune old archives while cache users are
stopped. Keep the directory lock file; removing it can break exclusion between
processes. Automatic dependency-cache retention is not implemented yet.

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
configuration, build privately. Empty triplets use the first operator-admitted
triplet; specify it as the publisher's native host triplet. Package ABI matching
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

Results come from reports in fixed protocols, never from the text a tool prints
for people, so a new tool version does not break them. A step writes its reports
to `${reports}`, a directory outside the tree that is empty when the step
starts, and the worker reads them after the step, failed or not:

| Protocol | Written by | Gives |
| --- | --- | --- |
| `junit` | CTest (`--output-junit`), pytest (`--junit-xml`), Bun (`--reporter=junit`) | total, failed, failed test names |
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
include `timings` in seconds: `sync_seconds` measures source transfer from the
service to the Worker, `queue_seconds` the caller's wait for a job to start,
`dependency_seconds` dependency preparation (including its own scheduling), and
`worker_seconds` execution on the Worker, including checkout and analysis.
Dependency time is measured before the main job enters its queue. Missing stages
are absent; a cache hit reports zero work for that call. Shared callers receive
the shared execution's sync and Worker times, and their own queue wait. The call's
`reuse` identifies executed, shared and cached results. These stages do not
include local CLI scanning/upload or result delivery and do not sum to the full
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
3. `language` with `action: "scopes"` returns scope IDs, hierarchy, Unicode
   scalar ranges `[begin,end)` and opaque `reference` values. A reference binds
   one analysis configuration, source version, worldline, file identity and range;
   it expires after 600 seconds. IDs alone do not identify a lockable range.
4. `scope` with `action: "acquire"`, `scopes: [reference, ...]` and `operation_id`
   acquires the whole set atomically and returns `leases`. Source resolves the
   references; callers cannot supply coordinates. For whole-file changes use
   `files: [path, ...]`; for creation or path changes use `entries: [path, ...]`.
   File and entry requests also name the current `version`. File leases cover
   empty and binary files; entry leases cover the named path and descendants,
   including move destinations. Resource lists may accompany scope references.
   Overlap within one worldline returns `scope_conflict` immediately, with no
   partially acquired set. References must describe the current version;
   `scope_stale` means query the current source/scopes again.
5. Submit `worldline` `apply` with `observed`, `operation_id`, `leases: [token]`
   and range edits (`kind: "edit"`, `offset`, `count`, `text`). Offsets count
   Unicode scalars in `observed`. An edit must stay inside its held scope;
   whole-file replacement requires a lease covering the whole file. Other
   callers cannot edit leased ranges or their boundary anchors. Unleased,
   disjoint work remains possible. Renew the lease while working and release
   it when finished. Defaults are 120 seconds, with a maximum of 600 seconds.
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
`scope_expired`. History-changing operations require leases on that worldline
to be released first. Forking remains available. A changed input after a
refused operation uses a new operation ID.

Leases live in local coordination metadata, outside worldline history, source
versions and build cache keys. Acquiring or renewing a lease does not invalidate
analysis or build results. Range edits reuse the source engine's incremental
history; workers receive missing source objects and update their existing
language sessions. Equal immutable inputs retain job deduplication and build
cache reuse. `fresh` explicitly requests a clean build. Artifact grants remain
project-scoped even when the content is shared internally.

Worker language sessions negotiate symbol, diagnostic, definition, and reference
capabilities. Sessions remain attached to their hot directories and receive file
updates between requests. Queries return results for the selected source snapshot;
source nodes retain only temporary scope descriptors and coordination anchors,
not language-server sessions or complete semantic analysis results. Definitions
and references accept a file path, symbol name, and optional one-based line.
Executable mappings and prepared-tree environment bindings apply on workers. Each analysis configuration
contains `server` and a `build_configuration` reference. The provider selects
artifact-producing tasks and their dependencies from that build configuration;
there are no analysis `build`, `operation`, or `inputs` fields. Preparation and
builds share the project's worldline and build-configuration directory pool.
A ty server using a relative Python environment selects its uv environment
producer automatically. Saving settings validates the reference and graph
without executing preparation or analysis.

Workers report a runtime fingerprint before accepting work. It hashes provider-default
and configured executable contents, the platform, effective worker environment, distribution
package manifests, and recognized Java/Android SDK release manifests. Standalone
worker configuration can supply `sdk_manifests: {"name": "/path/to/manifest"}`
for other managed SDKs. Custom executables must be mapped in `tools`; missing or
unreadable executables are omitted from advertised capabilities. SDK releases
and tools must remain immutable while the
worker is connected; restart the worker after updating them. Project and step
environment overrides are included in the computation key.

Build and language computations use source content and the selected runtime,
independently of caller, worldline, or version. Every caller has its own audit
record, including cache/shared execution status; only actual executions consume
execution quota. Test and run calls each execute their runtime steps. Push-only
diagnostics without a matching document version return `stale: true` and are
not cached. Even version-matched pushed diagnostics are excluded from the shared
cache because a document version cannot identify changes to its dependencies. A
later query can observe diagnostics received in the background.
Invalid symbol selections return `invalid_record` without restarting the server.

`build` with `fresh: true` bypasses cached and in-flight results, retires the
selected directory's language session, and recreates that working directory
from the source snapshot before running the build. Shared dependency caches
remain available. Builds without `fresh` preserve generated outputs for incremental
execution. A clean build detaches its old directory under the store lock and
deletes it after releasing that lock. Worker startup reclaims detached directories
left by an interrupted deletion.

The Console exposes component-specific editors. Local executable versions,
behavioral acceptance, and remaining deployment limits are recorded once in the
[toolchain acceptance record](../console/DEPLOYMENT.md#c-and-python-toolchains--october-27-2026).

## Concurrency budget

The configuration owns the execution thread count and subprocess parallelism:

```json
"runtime": { "threads": 10, "jobs": 3, "workers": 2 }
```

| Setting | Allocation |
| --- | --- |
| `threads` | Execution threads across gateway, source, and workers. Each process gets one I/O thread. Gateway and source each reserve two blocking workers; each compute worker gets one. Remaining threads are split between the gateway and source blocking pools. Sleeping main and signal-watcher threads are outside this count. |
| `jobs` | Parallel build jobs, divided evenly among workers and passed to toolchains as parallelism hints; source runs no subprocess work. |
| `workers` | Worker process count. Each worker admits one task at a time, including its language-analysis phase. |

`threads` must be at least `6 + 2 * workers`; `jobs` must be positive and at least `workers`.
A file-only deployment can use `{ "threads": 6, "jobs": 1, "workers": 0 }`.
Budgets have no defaults. Local configuration determines the startup
budget; hosted controllers can update it through the host-only
`GET`/`PUT /api/v1/config/runtime` endpoint. PUT uses the returned numeric
`revision` and a budget in `value`. A stale revision returns 409; an accepted
change returns 202 until it is applied.

Status separates `desired` from `applied` and reports `starting`, `ready`,
`pending`, or `draining`. Accepted work finishes before internal processes are
rebuilt. New work receives 503 while draining; status reads, job inspection,
cancellation, and CORS preflight remain available. The main process retains its
PID and loads the persisted budget for the next process group. See the hosted
[runtime budget controls](../cloud-gateway/README.md#runtime-budgets).

The top-level `tools` map supplies executable paths to workers.

Build environments receive the assigned parallelism for CMake, CTest, Make,
and OpenMP. These are scheduling hints within the shared execution environment.

Gateway metadata stays in `server.storage`; source data lives in its `source/`
subdirectory, and worker caches in `workers/local-<index>/`. Internal listeners
use loopback. Back up the configuration and the complete storage directory.

## Source layout

`src/fughetta-service.cpp` is the executable entry. `src/service/` owns process
lifecycles; `src/gateway/` handles public requests; `src/source/` owns storage
and source history; `src/worker/` executes build and analysis tasks. Shared domain models live
in `src/model/`, execution helpers in `src/core/`, and I/O and platform providers
in `src/io/` and `src/platform/`. Tests live in `tests/`.

Development rules are in [AGENTS.md](AGENTS.md). Clients that sync files, `fug`
and `agent-cloud`, use `POST /api/v1/projects/{project}/worldline`.
The route uses `tools::worldline::input` from `src/gateway/tools.cppm`.

Debug and Release builds, service test suites, and CLI Worker connections
have passed locally on Linux and Windows. Automatic self-hosted
capacity detection is not implemented. Hosted budget control operates on
existing instances; host provisioning and whole-container memory allocation
remain deployment responsibilities.

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
Current transport, identity, and lifecycle evidence is maintained in the
gateway's [verification entry](../cloud-gateway/README.md#verification).
Production Core provisioning remains deployment work.

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
