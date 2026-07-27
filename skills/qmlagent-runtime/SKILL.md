---
name: qmlagent-runtime
description: Use when building, repairing, creating, or verifying Qt Quick/QML apps with QmlAgent. Covers the agent-first qmlagent-launcher app/preview workflow, native qmlagent MCP tools, qmlagentctl fallback commands, QT_QML_DEBUG requirements, preview reload, binding diagnostics, input/workflow verification, and structured-first evidence discipline.
---

# QmlAgent Runtime Workflow

Use QmlAgent as the runtime evidence loop for Qt Quick work. Do not treat this
skill as the protocol reference; once native MCP tools are visible, follow
their descriptions for exact parameters.

## Required Setup

1. Confirm the `qmlagent_*` MCP tools are actually available (e.g.
   `qmlagent_target_status` is in your tool list). If they are missing, the
   server is not registered in this project/session — do not silently fall
   back to `qmlagentctl` and leave the user wondering why they see shell
   commands. Tell them to register it and restart the session:
   `claude mcp add qmlagent -s user -- <QT_BIN>/qmlagent-mcp --timeout 5000`
   (`-s user` registers it globally; the tools appear after a restart). Until
   then, `qmlagentctl` is the exact-equivalent fallback for every operation.
2. Find the Qt installation `bin/` that contains `qt-cmake`,
   `qmlagent-launcher`, `qmlagentctl`, and `qmlagent-mcp`.
3. Build the target with QML debugging enabled:
   `target_compile_definitions(myapp PRIVATE QT_QML_DEBUG)` in its CMake.
4. Prefer `qmlagent-launcher` for all agent-owned sessions.
5. After launch, call `qmlagent_target_status` first.

If the app prints `Debugging has not been enabled`, the target lacks that
compile definition; QmlAgent cannot attach until it is rebuilt with it.

## Launch Modes

Choose by what you are changing, to avoid a needless rebuild-relaunch loop:

- Iterating on QML only, and the file has no C++ the preview host can't
  supply (no C++-registered types, context properties, C++ models, or
  `loadFromModule` URI)? Use `preview` + `reload_preview` — it hot-reloads
  without a rebuild.
- Changing C++, or validating the shipped behavior before you trust a
  result? Use `app`. Preview does NOT run the app's C++ backend, so a QML
  file that depends on it will not load — the mode is faster, not equivalent.

Preview loads the QML file standalone, so types registered through the app's
`qt_add_qml_module` are not visible — most commonly a `pragma Singleton`
(e.g. a `Theme`). The engine does pick up a `qmldir` next to the file, so add
one (`singleton Theme 1.0 Theme.qml`) to preview such a file, or use `app`
mode where the module resolves normally.

Use the real app path when validating an application executable:

```sh
"$QT_BIN/qmlagent-launcher" app ./myapp -- --app-arg
```

Use preview mode for local QML-root iteration:

```sh
"$QT_BIN/qmlagent-launcher" preview Main.qml
```

Prefer a `Window`-root file for preview: it opens at the size the file
declares. An `Item`-root file gets a default 640x420 host window (there is no
size knob), so a larger design is clipped — wrap it in a `Window` that sets
`width`/`height`, which also keeps `reload_preview` targeting the same window.

Preview sessions can reload. After editing the root QML file or a file it
loads, request reload through native MCP:

```txt
qmlagent_preview_reload
```

or through shell fallback:

```sh
"$QT_BIN/qmlagentctl" reload-preview
```

`qmlagent-launcher app <executable>` does not support preview reload. Rebuild
or relaunch app sessions unless the app itself owns a reload boundary.

Do not pass app binaries to `preview`, and do not expect `app` sessions to
reload QML. That split is intentional and user-visible.

## MCP Workflow

Do not start `qmlagent-mcp` manually inside the task. It should be registered
with the agent runtime before the session starts.

Tool naming: MCP tools are `qmlagent_*` (underscores throughout, no dots, so
no agent runtime needs to rename them). Raw protocol names (`UI.query`,
`UI.waitFor`) are the JSON-RPC methods behind the tools, reachable via
`qmlagentctl call`.

After `qmlagent-launcher` starts exactly one live session, request/response MCP
tools route through it automatically. Do not call `qmlagent_connect_tcp` or
`qmlagent_connect_local_socket` for launcher-owned request/response work.
Direct attach is only for manually launched targets or streamed subscriptions
that require a persistent attached client.

When more than one launcher session may be live — most often your app running
alongside an integration-test target — automatic routing refuses and names the
live sessions. Pin yours by passing its id (from `qmlagent_target_status`) as
the `session` argument on every target-backed tool. Pinning also protects you
if your target exits mid-task: a pinned call fails cleanly instead of silently
routing to a surviving session and returning another app's scene as if it were
yours.

Normal order:

```txt
qmlagent_target_status
launch or confirm one qmlagent-launcher session
qmlagent_ui_query / qmlagent_ui_get_tree
qmlagent_ui_query_many for multiple verification reads
qmlagent_diagnostics_* / qmlagent_source_resolve / qmlagent_log_get_entries
qmlagent_input_* / qmlagent_workflow_*
patch source
rebuild/relaunch app session, or preview_reload for preview session
verify with runtime evidence
```

Use compact or summary output by default. Ask for fuller evidence only when the
summary says fields were omitted or when patching needs deeper source/runtime
facts.

When QmlAgent is built with `Qt6::Quick3D`, `qmlagent_ui_get_tree` and
`qmlagent_ui_query` include QtQuick3D frontend nodes below `View3D`
automatically. Use normal selectors (`id=`, `objectName=`, `sourceLocation=`)
for `Model`, camera, light, material, texture, and related nodes. These nodes
are structural/source evidence, not `QQuickItem` click targets. For `Model`
visibility/debugging, request `fields:["render3D"]` to get world bounds,
projected screen bounds, camera distance, and approximate frustum evidence.
Use `qmlagent_diagnostics_analyze_tree` or `qmlagent_diagnostics_analyze_node`
with `checks:["quick3D"]` to focus on View3D camera/light setup, Model
material/scale, texture source, and render3D-backed frustum evidence.
Repeater3D delegate nodes may carry `delegate.indexSource:"repeater3DObjectAt"`;
use the advertised `sourceLocation="..." index=N` selector for anonymous
repeated 3D delegates instead of visual order or session-local node ids.
Use `qmlagent_render_pick3d` or `Render.pick3D` on a `View3D` selector for
read-only hit-test evidence at a View3D-local coordinate; it does not deliver
input events.

On an input result, `delivered:true` is the action-success signal. The nested
`settle` object is render-loop-only (`verificationRole: render-loop-settle-only`):
`settle.ok:false` with `frame_not_observed_before_timeout` just means the click
produced no repaint — common for a state-only change — not that the input
failed. Confirm the real outcome with `UI.query`/`UI.waitFor`, never by reading
`settle.ok` as pass/fail.

## qmlagentctl Fallback

Use `qmlagentctl` for launcher/session control or when native MCP tools are not
available:

```sh
"$QT_BIN/qmlagentctl" sessions --format compact
"$QT_BIN/qmlagentctl" status --format compact
"$QT_BIN/qmlagentctl" methods
"$QT_BIN/qmlagentctl" query 'id="saveButton"' --property text --format compact
"$QT_BIN/qmlagentctl" query 'objectName="probe.cube"' --format compact
"$QT_BIN/qmlagentctl" query-many --params '{"queries":[{"selector":"id=\"saveButton\""},{"selector":"id=\"statusLabel\"","properties":["text"]}]}' --format compact
"$QT_BIN/qmlagentctl" wait 'id="detailsPopup"' --state found --timeout 1000
"$QT_BIN/qmlagentctl" click 'id="saveButton"'
"$QT_BIN/qmlagentctl" scroll-into-view 'id="saveButton"'
"$QT_BIN/qmlagentctl" long-press 'id="contextButton"' --hold-ms 900
"$QT_BIN/qmlagentctl" clear-text 'id="urlField"'
"$QT_BIN/qmlagentctl" type 'id="urlField"' --text 'go.dev'
"$QT_BIN/qmlagentctl" binding 'id="saveButton"' --property y --format compact
"$QT_BIN/qmlagentctl" call --help
"$QT_BIN/qmlagentctl" reload-preview
"$QT_BIN/qmlagentctl" stop
```

`qmlagentctl screenshot` exists, but it is fallback visual evidence. Use
structural UI, diagnostics, source, logs, and input/workflow evidence first.
Only request image bytes when visual evidence is explicitly needed. Prefer
`qmlagentctl screenshot --out shot.png --scale 0.5` for shell fallback so
base64 does not enter the agent context. For MCP screenshot bytes, pass
`scale` and/or `region` with `includeData:true`.

## Selector Grammar

One predicate, optional disambiguator. In stability order:

```txt
id="saveButton"                                # high
objectName="settings.save"                     # high
sourceLocation="src/Main.qml:44:7"             # medium, authored source line
sourceLocation="src/Main.qml:44:7" instance=2  # medium, Nth instance of line
type="Button"                                  # medium in small scopes
text="Save"                                    # low, may be translated
nodeId=42                                      # session-local only
id="delegateRow" index=0                       # delegate by model index
id="tableCell" row=0 column=1                  # table cell
```

Persist only high selectors across restarts; re-query low/`nodeId` handles
instead of storing them. Results carry per-selector `stability` labels and
ambiguous matches return `stableSelectorHints`. Compound predicates are not
supported.

## Evidence Discipline

- Prefer stable QML `id` selectors over session-local `nodeId`.
- For repeated delegates, try `id="delegateId" index=0` before adding
  automation-only `objectName`. For anonymous or repeated components without
  ids, use the `sourceLocation` selector the query result suggests.
- When geometry/state looks computed, call
  `qmlagent_diagnostics_analyze_binding` for the property. Treat classic
  `QQmlBinding` dependency values as runtime evidence. Treat
  `candidateIdentifiers` as low-confidence follow-up hints that must be
  verified through `UI.query`, diagnostics, or source evidence before patching.
- Inspect `bindingProvenance` attached to layout/visibility diagnostics before
  making a separate binding call; it may already name the computed property,
  expression, source location, and dependency limitations.
- Use `UI.waitFor` or workflow tools for transitions, popups, loaders,
  animations, and async post-action state. Do not use sleeps.
- Use `qmlagent_ui_query_many` or shell `qmlagentctl query-many` for multiple
  selectors/properties after one action instead of serial query calls.
- If click/read evidence reports `center_outside_viewport`, use
  `qmlagent_input_scroll_into_view` or shell `qmlagentctl scroll-into-view`,
  then retry the action/query. For not-yet-instantiated virtualized rows,
  wheel toward the row and re-query first.
- If a popup blocks input or a click reports `blocked_by_modal_popup`, and
  Escape or a dismiss button is unreliable, use `qmlagent_input_dismiss_popup`
  (or shell `qmlagentctl dismiss-popup`, `--all` for stacked popups). It
  reports `remainingPopupCount` so you can confirm the popup actually closed.
  Note menus and dialogs toggle: re-clicking the trigger while open closes it.
- To enter new text into an occupied field, use `qmlagent_input_type_text`
  with `replaceExisting=true` (one call), or shell `qmlagentctl clear-text`
  followed by `qmlagentctl type`, then verify with `UI.query` or `UI.waitFor`.
  `qmlagent_input_clear_text` remains for clearing without typing.
- Use input/workflow results plus `UI.query`, diagnostics, logs, or source
  evidence as proof. Runtime mutation is setup-only.
- Use `qmlagent_input_long_press` or `qmlagent_workflow_long_press_and_wait`
  for press-and-hold UI. Do not hand-roll mousePress + sleep + mouseRelease
  unless debugging the input primitive itself.
- For application logs, use `qmlagent_log_get_entries` (raw `Log.getEntries`).
  Never redirect the launcher's stdout/stderr to a file and grep it — the log
  service is the interface; it gives structured, cursored, deduplicated
  entries, and the launcher's own stream is not the app's log.
- If QML fails to load, inspect logs/status first. A dead process cannot answer
  `UI.*` queries.
- Keep one active workflow owner per target session; multiple agents can race
  semantically even when launcher routing works.
