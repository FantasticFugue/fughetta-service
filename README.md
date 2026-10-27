# fughetta-service

A remote development service for coding agents, exposed over MCP.

The first release serves builds, tests, and runs: agents edit files on their
own machine, [fug](../fughetta-cli) sends them, and several agents share one
queue of deduplicated, cached jobs on warm workers. Behind that sits the
longer-term layer the service is built on: worldlines that hold each project's
code and let agents work on it together, and language analysis that reads
symbols and scopes from those versions. Its file and worldline tools serve
clients that sync code (`fug`, `agent-cloud`); they are not yet the main agent
interface.

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

Requires Linux, CMake 4.0+, Ninja, vcpkg, and the xclang toolchain.
The debug and release presets select httpant's `asio` I/O backend.
Keep `httpant` and `vcpkg-registry` beside this repository. The manifest selects
`httpant[io,tls]`; `vcpkg-configuration.json` loads `../vcpkg-registry/ports`.
The httpant port installs the current sibling checkout, and its source files
participate in vcpkg's package cache key.
The configured dependency installation is under `build/debug/vcpkg_installed/`.
Build products and dependency working directories belong under `build/`.

From this directory:

```sh
cmake --preset debug
cmake --build --preset debug --target fughetta-service
```

To run the HTTP front suite from a regular developer shell:

```sh
cmake --build build/debug --target front-test -j2
python3 tests/run.py --test-dir build/debug -- -R '^front$' --output-on-failure
```

The test entry requires the existing writable CPU/memory delegation at
`/sys/fs/cgroup/workers` and `sudo` authorization (`sudo -v` if needed). Run the
entry as your normal user. It creates a private leaf, uses a privileged launcher
only to place itself there, and executes CTest with your UID, GID, groups, and
environment. It does not build or create build directories. CTest arguments
follow `--`; `--launcher-timeout SECONDS` bounds the entire invocation. The
supervisor cleans up its own leaf and descendants on completion, timeout, and
handled signals while preserving the test exit status.

The debug preset uses line-table debug information with address and
undefined-behavior sanitizers. File/line backtraces are available; full type and
local-variable debug information is not emitted. The Git-ignored local presets specify this
workstation's compiler and vcpkg paths, target, triplet, build options, and
installation prefix (`build/package`). Configuration requires no shell exports
or extra `-D` arguments. `release` disables tests and sanitizers and enables
benchmarks and interprocedural optimization.

## Run

Create an owner token, keep its printed value, and start the service:

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

On Linux a registered Worker isolates builds under the cgroup it starts in.
`fug` starts it with `systemd-run --user --scope -p Delegate=yes`; the Worker
moves itself into a `service` child, enables `cpu` and `memory` for its
children, and refuses to start when the cgroup is not delegated to the user.
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
Language preparation persists its completed, failed, cancelled, or unknown
receipt in a bounded interval even when its caller cancels. Repeating its
operation retrieves that receipt instead of rerunning the command.

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
| clice | Read compilation-database outputs from the preparation graph, with explicit cache, compile-flag additions/removals, executable, and environment settings. |
| TypeScript / tsc | Check a selected `tsconfig.json` without emitting, or compile into an explicit output directory. Compiler options are typed fields; terminal diagnostics remain available without being parsed as a structured report. |
| typescript-language-server | Analyze TypeScript, TSX, JavaScript, and JSX through LSP. Configure tsserver, inlay/completion preferences, implicit-project settings, locale, and memory through initialization options. |
| Bun | Build and test only. Every operation automatically performs a frozen-lockfile `bun install` first, with lifecycle scripts disabled by default. Build entrypoints, output, bundler options, test paths, and name filters are typed; there is no `run` operation or arbitrary command argument field. Tests use Bun's native JUnit reporter. |
| uv | Lock, sync, build distributions, and run a command. Environment and distribution directories are explicit; dependency groups, extras, interpreter selection, and offline execution are modeled separately. |
| ty | Run type checks with import environments and rule severity settings; warnings fail by default. Its language server has independent diagnostic, completion, inlay-hint, and configuration settings. |
| Ruff | Check lint and formatting without rewriting source. The language server has independent rules, formatting, code-action, and configuration settings. |

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

Agents configure projects with the `project`, `schema`, and `save_project` tools of
the configuration MCP. `schema` returns the settings schema with the options of
every adapter and language server, so the tool list stays small; a refused save
names the field.

Language services negotiate symbol and diagnostic capabilities, collect pull or
push diagnostics, and publish them into the semantic outline. Diagnostics retain
source anchors and survive source-service restart. Executable mappings and
prepared-tree environment bindings apply on both the source and worker paths.
Saving settings does not execute preparation or analysis.

The Console exposes component-specific editors. Local executable versions,
behavioral acceptance, and remaining deployment limits are recorded once in the
[toolchain acceptance record](../console/DEPLOYMENT.md#c-and-python-toolchains--october-27-2026).

## Concurrency budget

The configuration owns the execution thread budget and subprocess CPU budget:

```json
"runtime": { "threads": 10, "jobs": 3, "workers": 2 }
```

| Setting | Allocation |
| --- | --- |
| `threads` | Execution threads across gateway, source, and workers. Each process gets one I/O thread. Gateway and source each reserve two blocking workers; each compute worker gets one. Remaining threads are split between the gateway and source blocking pools. Sleeping main and signal-watcher threads are outside this count. |
| `jobs` | CPU execution shares for subprocess work. Source preparation and language analysis share one serialized slot; the remainder is divided evenly among workers. |
| `workers` | Worker process count. Each worker admits one task at a time, including its language-analysis phase. |

`threads` must be at least `6 + 2 * workers`; `jobs` must exceed `workers`.
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

The top-level `tools` map supplies executable paths to source and workers.

Build environments receive the assigned parallelism for CMake, CTest, Make,
and OpenMP. Linux cgroup CPU quotas apply to the task process tree, including
language servers. Compute workers and local preparation require a writable
cgroup v2 delegation at `/sys/fs/cgroup/workers` with CPU and memory controllers.
Run the service and tests from a leaf inside that delegation so children can
enter their task cgroups. Directory ownership alone does not permit moving a
child from a process outside the delegated tree.

Gateway metadata stays in `server.storage`; source data lives in its `source/`
subdirectory, and worker caches in `workers/local-<index>/`. Internal listeners
use loopback. Back up the configuration and the complete storage directory.

## Source layout

`src/fughetta-service.cpp` is the executable entry. `src/service/` owns process
lifecycles; `src/gateway/` handles public requests; `src/source/` owns storage
and source preparation; `src/worker/` executes tasks. Shared domain models live
in `src/model/`, execution helpers in `src/core/`, and I/O and platform providers
in `src/io/` and `src/platform/`. Tests live in `tests/`.

Development rules are in [AGENTS.md](AGENTS.md). Clients that sync files, `fug`
and `agent-cloud`, use the `worldline` MCP tool; its `tools/list` schema is the
contract.

The local debug preset configured and generated successfully. Release
configuration and Windows execution remain unverified. Automatic self-hosted
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
Production Core provisioning and Windows execution remain outside these local
checks.
