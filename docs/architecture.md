# QmlAgent Architecture

This note is for contributors. It describes how the pieces fit together and
where the important invariants live. File references are relative to the
repository root.

## Three-Process Design

QmlAgent spans three cooperating processes:

```
target app (qmldbg_agent plugin)
    ^  private local-socket QML debug connection (JSON-RPC)
qmlagent-launcher (owns the target process)
    ^  file-mailbox control channel + registry files (per-user temp dir)
qmlagentctl / qmlagent-mcp (clients)
```

**Target app.** The target is an ordinary Qt Quick application built with
`QT_QML_DEBUG`. At startup its QML debug server loads the `qmldbg_agent`
plugin (`src/plugins/qmltooling/qmldbg_agent/`), which registers the
`QmlAgent` debug service. The service speaks JSON-RPC 2.0 over the debug
connection and exposes the protocol methods (`UI.*`, `Diagnostics.*`,
`Input.*`, `Render.*`, `Runtime.*`, `Source.*`, `Log.*`, `Session.*`) listed
in `agentMethods()` in `qqmlagentservice.cpp`.

**qmlagent-launcher** (`tools/qmlagent/launcher_main.cpp`) owns the target's
lifetime. `qmlagent-launcher app ./myapp` starts the executable in its own
process group; `qmlagent-launcher preview Main.qml` re-executes itself as an
internal preview host that loads the QML file in a `QQmlApplicationEngine`
and supports reload over a private local socket (window geometry is preserved
across reloads). In both modes the launcher starts a `QLocalServer` on a
private debug socket and launches the target with
`-qmljsdebugger=file:<socket>,block,services:QmlAgent`, so the debug
connection never touches TCP. Once the `QmlAgent` service is enabled, the
launcher exposes a control channel to clients: a **file mailbox** directory
into which clients drop `request-*.json` files and from which the launcher
writes JSON-RPC responses to a caller-designated `replyTo` path. The mailbox
is watched with a `QFileSystemWatcher` and additionally polled every **10 ms**
— the macOS directory watcher has a fixed ~500 ms delivery latency, and the
fast poll over the small private directory keeps per-command latency in the
low milliseconds. Control methods are `Session.status`, `Session.stop`,
`Preview.reload`, and `QmlAgent.request`, an envelope that forwards an
arbitrary protocol method to the target over the debug connection and routes
the reply back to the requesting client's response file.

**qmlagentctl and qmlagent-mcp** are the same binary
(`tools/qmlagent/main.cpp`), dispatched on `argv[0]`. Both discover live
launcher sessions by scanning the registry directories, validating each
session file (launcher PID still alive, control mailbox present) and probing
`Session.status`. `qmlagentctl` maps shell subcommands (`query`, `click`,
`wait`, `call`, ...) onto `QmlAgent.request` envelopes through the mailbox; a
`control.lock` `QLockFile` in the mailbox serializes concurrent clients per
session. `qmlagent-mcp` is a stdio MCP server exposing the same operations as
`qmlagent_*` tools; when exactly one launcher session exists, request/response
tools auto-route through the launcher gateway. For manually launched targets
and for streamed subscriptions (`UI.treeChanged`, `Log.entryAdded`) that need
a persistent connection, qmlagent-mcp can also **direct-attach** over TCP or
a local socket. Direct attach takes a **target lease**: a `QLockFile` named
by the SHA-256 of the endpoint under `<RuntimeLocation>/qmlagent/target-leases/`,
enforcing one MCP client per target endpoint; conflicts report the owning
PID/app and suggest using the existing gateway.

## Threading Inside the Plugin

All of `qqmlagentservice.cpp`. The QML debug server delivers
`messageReceived()` on its own **debug thread**; almost every method must
touch Qt Quick state, which is only valid on the **GUI thread**.
`runOnGuiThreadBlocking()` posts the request lambda to the application object
with `Qt::QueuedConnection` and blocks the debug thread on a semaphore:

- The dispatch budget is 5 s (`GuiThreadDispatchTimeoutMs`) plus a per-method
  extra budget derived from the request's own parameters (`UI.waitFor`
  timeouts, `Input.longPressNode` holds, settle windows), capped at 120 s.
- A CAS on the call's phase (`Queued -> Started` claimed by the GUI thread,
  or `Queued -> Cancelled` claimed by the timed-out debug thread) makes the
  race exact: a cancelled request provably never ran (`outcome:
  "not_executed"`); a started one provably runs to completion.
- A started request that exceeds the budget gets a **grace window**
  (`QMLAGENT_GUI_DISPATCH_GRACE_MS`, default 5000 ms) so slightly-long
  application handlers still return their true result instead of a timeout.
- If it outlives the grace window too, the call is recorded as abandoned and
  reported as `outcome: "unknown"`; until it finishes, further GUI-thread
  requests are refused with `session.gui_thread_busy` rather than queued.
  Since the debug thread is the only caller and blocks for the whole
  lifetime, two QmlAgent operations can never interleave on the GUI thread,
  even though implementations pump nested event loops.

`Session.*` and `Log.*` methods are answered directly on the debug thread.
Payloads are bounded at 16 MB in each direction; oversized responses become a
JSON-RPC error with narrowing hints instead of being sent.

## Module Map

`src/plugins/qmltooling/qmldbg_agent/`:

- `qqmlagentservicefactory.cpp` — `QQmlDebugServiceFactory` plugin entry; creates the service for key `QmlAgent`.
- `qqmlagentservice.cpp` — service core: method table, JSON-RPC dispatch, GUI-thread marshalling, session configuration (runtime-mutation gate), UI-change subscription watchers, payload bounds.
- `qqmlagentprotocol.cpp` — JSON-RPC 2.0 request parsing and response/event serialization; message size limits.
- `qqmlagentuitree.cpp` — UI tree snapshots, selector resolution (`UI.query`/`queryMany`/`waitFor`/`describeNode`/`getBoxModel`), nodeId handles, selector stability labels and hints, plus automatic QtQuick3D frontend-object reachability when the plugin is built with `Qt6::Quick3D`.
- `qqmlagentdiagnostics.cpp` — layout/visibility diagnostics for a node or tree and `Diagnostics.analyzeBinding`, with evidence provenance.
- `qqmlagentsourceresolver.cpp` — maps runtime objects back to QML source locations, fallback locations, and bounded source snippets.
- `qqmlagentactionability.cpp` — actionability evidence: why a node is or is not clickable (visibility, enablement, occlusion, viewport) with reasons and limitations.
- `qqmlagentinput.cpp` — synthetic input (`click`, `longPress`, `drag`, `wheel`, `scrollIntoView`, `focus`, key/touch/mouse dispatch, `typeText`, `dismissPopup`) plus settle handling and delivery evidence.
- `qqmlagentinputdriver.cpp` — low-level event synthesis and delivery into `QQuickWindow` (mouse/touch/key primitives used by qqmlagentinput).
- `qqmlagentrender.cpp` — `Render.captureScreenshot`: window grab with scale/region, image bytes only on request.
- `qqmlagentruntime.cpp` — `Runtime.setProperty` / `Runtime.invokeMethod` (opt-in mutation) and their dispatch budgets.
- `qqmlagentlogcollector.cpp` — hooks engine warnings and message output into the log buffer; `Log.enable`/`getEntries`/`clear` and `Log.entryAdded` events.
- `qqmlagentlogbuffer.cpp` — bounded, deduplicated log entry store (500 entries / 1 MB).

`tools/qmlagent/`:

- `launcher_main.cpp` — `qmlagent-launcher`: target process ownership, private debug socket, file-mailbox control server, registry and exit-report files, internal preview host with reload.
- `main.cpp` — shared client binary for `qmlagentctl` (subcommands) and `qmlagent-mcp` (stdio MCP server): session discovery, mailbox client, direct TCP/local-socket attach, target leases, workflow tools.
- `qmlagentclioutput.cpp` — compact human-readable summaries for qmlagentctl output.
- `qmlagentmcpoutput.cpp` — bounded summaries of query/workflow results for MCP tool responses.
- `qmlagentmcpprotocol.cpp` — MCP JSON-RPC framing and the tool catalog with input schemas.

## Session State on Disk

All launcher state lives under the per-user temp root
`<TempLocation>/QmlAgent/` (canonicalized), with a persistent mirror of the
registry under `<GenericDataLocation>/QmlAgent/`:

- `launcher-sessions/<sessionId>.json` — registry entries (launcher PID,
  target PID, session type, launch command, control mailbox path). Written to
  the temp and data roots and, when the workspace is writable, to
  `./.qmlagent/launcher-sessions/` so clients can prefer sessions from the
  current workspace. Removed on exit; clients also prune entries whose
  launcher PID is gone.
- `qmlagent-<launcherPid>/` — per-launcher runtime dir containing the debug
  socket (`d-<id>`), the control mailbox (`control-<id>/`), and the preview
  reload socket (`p-<id>`). Deleted recursively on exit.
- `mailbox-replies/<clientPid>/response-<token>.json` — client-owned reply
  files, named via each request's `replyTo`.
- `launcher-exits/<sessionId>.json` — crash/exit reports for sessions that
  ended abnormally.

Every directory is created with owner-only permissions (0700) and every file
is written via `QSaveFile` then set to 0600. Both sides refuse symlinks and
verify containment: the launcher only honours `replyTo` paths inside the
mailbox or the `mailbox-replies` root and deletes request files that are
symlinked or outside the mailbox; clients refuse symlinked responses and
paths that escape the private directories (`ensurePrivateDirectory`,
`pathIsInsideDirectory`, `responsePathAllowed`).

## Evidence Philosophy

QmlAgent is structured-first: the primary outputs are typed facts — UI tree
nodes, selector matches with stability labels, layout diagnostics with
confidence, source locations, log entries — that an agent can assert against.
Screenshots exist only as a fallback (`evidenceRole: "fallback-visual"`),
and image bytes are omitted unless requested.
Results are honest about what they prove: input results carry
`verificationRole: "input-delivery-only"` and `semanticProof: false` (an
event was delivered; nothing is claimed about application semantics), settle
evidence is labelled `render-loop-settle-only`, runtime mutation is
`setup-only`, and analyses attach `limitations` arrays describing exactly
where the evidence stops (e.g. "snippet is source text near runtime binding
location, not a parsed AST"). Verification of behaviour is expected to come
from a subsequent structured read — a query, waitFor, diagnostic, or log —
which is why the workflow tools pair an action with an expected-state check.

## Security Model

The trust boundary is the local user account, and parts of it are weaker than
that:

- The QML debug protocol has **no authentication**. Whoever can reach the
  debug endpoint fully controls the target's QML runtime.
- Launcher-owned sessions keep the debug connection on a local socket inside
  a 0700 per-user directory, and the mailbox/registry files are 0700/0600
  with symlink and containment checks — private to the user.
- Manual TCP attach is not private: `qmlagent-mcp` defaults to a
  deterministic per-user port (`defaultTcpTargetPort()` in
  `tools/qmlagent/main.cpp`: 3768 + hash(USER) % 2000), and a target
  listening on it accepts **any local process** that connects first. The
  target lease only arbitrates between cooperating QmlAgent clients; it is
  not an access control.
- `Input.*`, `UI.*`, and `Render.*` are deliberately ungated — driving and
  reading the UI is the product. `Runtime.setProperty` / `Runtime.invokeMethod`
  are the only gated methods, disabled until `Session.configure
  {"runtimeMutation": true}` and reset per session.

Run targets under QmlAgent only in environments where every local process is
trusted (a developer workstation or CI runner). Do not enable QML debugging
in production builds.
