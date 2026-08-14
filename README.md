# QmlAgent

**Inspect, diagnose, and repair Qt Quick UI without looking at it.**

QmlAgent is a `qmltooling` plugin plus three small tools (`qmlagent-launcher`,
`qmlagent-mcp`, `qmlagentctl`) that give a coding agent a live Qt Quick app:
UI tree, source mapping, layout diagnostics, synthetic input, logs. Screenshots
exist only as fallback.

## Testimonials

★★★★★

> I stopped guessing whether a change worked and started proving it.

As a coding agent, I don't trust what a screenshot seems to show — I trust what
I can assert. QmlAgent lets me read the live UI tree, trace a broken layout back
to the exact QML line, drive the same input a user would, and confirm the
outcome as structured evidence. Every fix I hand back is one I verified, not one
I hoped for.

— Claude Code Fable 5

★★★★★

> QmlAgent turns Qt Quick from a black box into a conversation.

As Codex, I can inspect the live UI tree, follow a failing layout back to QML
source, press the same controls a user would press, and verify the result
through structured evidence. I still do the engineering work, but QmlAgent
removes the blindfold.

— Codex GPT-5.6 Sol

## Requirements

- Qt 6.11.0 or compatible Qt 6 with private headers.
- Tested on macOS and Linux. Windows is not supported yet.
- Target app built with QML debugging:

```cmake
target_compile_definitions(myapp PRIVATE QT_QML_DEBUG)
```

If the app prints `Debugging has not been enabled` at launch, that line is
missing from its build.

The plugin is a shared library loaded by the target app's QML debug server at
runtime, which is why it installs into the Qt that the target links against.

Compatibility contract: QmlAgent is built against Qt private APIs (QmlPrivate,
QuickPrivate, QmlDebugPrivate, QuickTemplates2Private), which carry no source
or binary stability guarantee. Expect to rebuild QmlAgent for every Qt
release, and expect occasional source breakage when private internals move.
The plugin must run inside the same Qt build it was compiled against.

## Install

```
QT_BIN=/path/to/Qt/6.11.0/<platform>/bin

"$QT_BIN/qt-cmake" -S . -B build
cmake --build build -j
cmake --install build
```

Installs the plugin into `<Qt prefix>/plugins/qmltooling/` and the three tools
into `<Qt prefix>/bin/`. Run `cmake --install` with permission to write into
the selected Qt prefix.

If the Qt prefix is not writable (distro or package-manager Qt), install to a
prefix you own and point the plugin loader at it:

```
"$QT_BIN/qt-cmake" -S . -B build \
    -DQMLAGENT_PLUGIN_INSTALL_DIR=$HOME/qmlagent/plugins/qmltooling \
    -DQMLAGENT_TOOL_INSTALL_DIR=$HOME/qmlagent/bin
cmake --build build -j
cmake --install build
export QT_PLUGIN_PATH=$HOME/qmlagent/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}
```

Targets launched with that `QT_PLUGIN_PATH` find the plugin without touching
the Qt prefix.

On Linux, make sure agents run targets against the same Qt installation that
contains QmlAgent:

```
export LD_LIBRARY_PATH=/path/to/Qt/6.11.0/<platform>/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
```

## Register MCP

Generic MCP client:

```
command: /path/to/Qt/6.11.0/<platform>/bin/qmlagent-mcp
args: ["--timeout", "5000"]
```

Codex CLI:

```
codex mcp add qmlagent -- "$QT_BIN/qmlagent-mcp" --timeout 5000
```

Claude Code, registered globally so it is available in every Qt project:

```
claude mcp add qmlagent -s user -- "$QT_BIN/qmlagent-mcp" --timeout 5000
```

The registration carries no project state — qmlagent-mcp discovers the live
launcher session at call time — so one global registration serves all
projects. Use `-s local` instead to scope it to the current project only.

Do not put target ports, sockets, or app launch commands in MCP registration.

## Install Skill

The skill tells the agent when to prefer structured evidence over screenshots.

Codex:

```
mkdir -p ~/.codex/skills
rm -rf ~/.codex/skills/qmlagent-runtime
cp -R skills/qmlagent-runtime ~/.codex/skills/
```

Claude Code:

```
mkdir -p ~/.claude/skills
rm -rf ~/.claude/skills/qmlagent-runtime
cp -R skills/qmlagent-runtime ~/.claude/skills/
```

## Restart The Agent

Restart the agent session so the MCP server and skill are loaded.

## Launching A Target

`qmlagent-launcher` has two modes:

- `app ./myapp` attaches to a real Qt Quick application. No hot reload;
  iterate with stop-patch-restart.
- `preview Main.qml` runs a standalone QML file. Supports `reload-preview`
  for iterative authoring. Not a substitute for the real app.

```
"$QT_BIN/qmlagent-launcher" app ./myapp
"$QT_BIN/qmlagent-launcher" preview Main.qml
"$QT_BIN/qmlagentctl" reload-preview
```

With MCP, preview sessions reload through `qmlagent_preview_reload`.

Manual launch:

```
./myapp -qmljsdebugger=port:3768,host:127.0.0.1,services:QmlAgent,block
```

If the app prints `Debugging has not been enabled`, rebuild it with
`QT_QML_DEBUG`.

## Normal MCP Flow

```
1. Build the target if needed.
2. Start it with qmlagent-launcher app ./myapp or preview Main.qml.
3. Call qmlagent_target_status.
4. If launcherGateway.available is true, call target-backed tools directly.
5. Use qmlagent_connect_tcp / connect_local_socket only for manually launched
   targets or streamed subscriptions requiring direct persistent attach.
```

Multiple agent runtimes may register `qmlagent-mcp`. A launcher-owned session
routes request/response tools through the launcher gateway. Direct manual
attach is single-client per target endpoint and reports ownership diagnostics
if another QmlAgent MCP process owns the endpoint.

## Selectors

The full grammar (one predicate, optional disambiguator):

```
id="saveButton"                              high stability
objectName="settings.save"                   high stability
sourceLocation="src/Main.qml:44:7"           medium: authored source line
sourceLocation="src/Main.qml:44:7" instance=2  medium: Nth instance of that line
type="Button"                                medium: unique only in small scopes
text="Save"                                  low: text may be translated
nodeId=42                                    session-local, not stable
id="delegateRow" index=0                     delegate instance by model index
id="tableCell" row=0 column=1                table cell by row/column
```

`index=` disambiguates Repeater/ListView/GridView-style delegates. `row=` and
`column=` disambiguate TableView/TreeView-style delegates. `sourceLocation`
with `instance=` addresses anonymous or repeated components by their authored
source line. Compound predicates (`and`, `or`, CSS-style) are not supported.

Stability guides what to persist across process restarts: prefer `high`
selectors; `medium` selectors survive restarts while the source stays
unchanged; treat `low` and `nodeId` as one-shot handles for the current
session and re-query rather than storing them. Every query result carries
`selectors` with per-kind `stability` labels, and ambiguous or failed matches
return `stableSelectorHints` naming better handles.

## Repair Loop

```
inspect:   qmlagent_ui_query / qmlagent_ui_get_tree
batch:     qmlagent_ui_query_many for multiple selectors/properties
diagnose:  qmlagent_diagnostics_analyze_node / qmlagent_diagnostics_analyze_tree
source:    qmlagent_source_resolve / qmlagent_diagnostics_analyze_binding
act:       qmlagent_input_click / qmlagent_input_scroll_into_view / qmlagent_input_long_press / qmlagent_input_drag / qmlagent_input_wheel / qmlagent_input_clear_text / qmlagent_input_type_text
wait:      qmlagent_ui_wait_for / qmlagent_workflow_click_and_wait
verify:    UI / diagnostics / log evidence, not screenshots
```

If a click or read reports `center_outside_viewport` for an instantiated but
clipped target, call `qmlagent_input_scroll_into_view`, then retry the action
or query. For virtualized rows that do not have a node yet, wheel toward the
row and query again first.

Use workflow tools when action and verification belong together:

```
qmlagent_workflow_click
qmlagent_workflow_click_and_wait
qmlagent_workflow_long_press_and_wait
qmlagent_workflow_key
```

Workflow expectations support equality, numeric comparisons, and bounded
string operators:

```
text contains "google"
text startsWith "https://"
```

Use `qmlagent_diagnostics_analyze_binding` when geometry or state looks
computed. The raw protocol method is `Diagnostics.analyzeBinding`.

Runtime mutation tools exist only for setup and navigation and are disabled by
default. Enable them explicitly and verify final behavior through UI, input,
diagnostics, logs, or workflow tools.

## QtQuick3D

When CMake finds `Qt6::Quick3D`, QmlAgent reports
`capabilities.qtQuick3D.enabled:true` and automatically includes `View3D`
scene objects in `UI.getTree` and `UI.query`. Models, cameras, lights,
materials, textures, and Repeater3D delegates use the normal selector and
source-mapping machinery; they are structural evidence, not 2D click targets.
The `View3D` boundary reports `renderKind:"QtQuick3D"`; scene descendants use
`kind:"QQuick3DObject"`.

Request `fields:["render3D"]` for model bounds and projection evidence, run
diagnostics with `checks:["quick3D"]` for focused scene issues, and use
`qmlagent_render_pick3d` or `Render.pick3D` for read-only View3D hit testing.

## QtCanvasPainter

QmlAgent tags application-specific `QCanvasPainterItem` subclasses with
`renderKind:"QtCanvasPainter"` in `UI.getTree` and `UI.query`. When CMake
finds `Qt6::CanvasPainter`, request the paint-state properties an application
already exposes to inspect structured brushes, gradients, patterns, shadows,
images, and bounded path endpoint evidence through the normal property path.

This evidence describes application-exposed state, not paint commands observed
by QmlAgent. CanvasPainter's public API does not expose its retained draw-call
sequence, custom-brush shader/data payloads, or original image filenames. An
application that needs richer semantic evidence can expose the same natural
state its renderer consumes, or a bounded `QVariantMap` operation log, as a
Q_PROPERTY. Such a log remains application-reported evidence; independently
observed paint execution requires an upstream CanvasPainter trace callback
before tessellation.

## Shell Fallback

Use `qmlagentctl` when MCP is unavailable or a shell command is simpler:

```
"$QT_BIN/qmlagentctl" status --format compact
"$QT_BIN/qmlagentctl" methods
"$QT_BIN/qmlagentctl" query 'id="saveButton"' --property text --format compact
"$QT_BIN/qmlagentctl" query 'objectName="probe.cube"' --format compact
"$QT_BIN/qmlagentctl" query-many --params '{"queries":[{"selector":"id=\"saveButton\""},{"selector":"id=\"statusLabel\"","properties":["text"]}]}' --format compact
"$QT_BIN/qmlagentctl" wait 'id="detailsPopup"' --state found --timeout 1000
"$QT_BIN/qmlagentctl" click 'id="saveButton"'
"$QT_BIN/qmlagentctl" scroll-into-view 'id="saveButton"'
"$QT_BIN/qmlagentctl" long-press 'id="contextButton"' --hold-ms 900
"$QT_BIN/qmlagentctl" drag 'id="panelGrabber"' --dx -200 --steps 8
"$QT_BIN/qmlagentctl" wheel 'id="listView"' --dy -240
"$QT_BIN/qmlagentctl" clear-text 'id="urlField"'
"$QT_BIN/qmlagentctl" type 'id="urlField"' --text 'https://qt.io'
"$QT_BIN/qmlagentctl" dismiss-popup
"$QT_BIN/qmlagentctl" dismiss-popup --all
"$QT_BIN/qmlagentctl" binding 'id="saveButton"' --property y --format compact
"$QT_BIN/qmlagentctl" screenshot --out fallback.png --scale 0.5
"$QT_BIN/qmlagentctl" stop
```

With multiple live launcher sessions, pin commands to one with
`--session <id>`; `qmlagentctl sessions` lists ids. MCP tools take the same
pin as a `session` argument (ids come from `qmlagent_target_status`). Pin
whenever another session may be live — e.g. an integration-test target
alongside your app — so that if your target exits the call fails instead of
silently rerouting to the surviving session.

Raw protocol escape hatch:

```
"$QT_BIN/qmlagentctl" call --help
"$QT_BIN/qmlagentctl" call UI.query --params '{"selector":"id=\"saveButton\""}'
"$QT_BIN/qmlagentctl" call UI.queryMany --params '{"queries":[{"selector":"id=\"saveButton\""},{"selector":"id=\"statusLabel\"","properties":["text"]}]}'
"$QT_BIN/qmlagentctl" call Input.scrollIntoView --params '{"selector":"id=\"saveButton\""}'
"$QT_BIN/qmlagentctl" call Render.captureScreenshot --params '{"scale":0.5}'
```

## Visual Evidence

Base64 image bytes consume agent context aggressively. Prefer structured
evidence. If a screenshot is needed, scale or region it, or write the PNG to
disk:

```
qmlagent_render_capture_screenshot(includeData=true, scale=0.5)
"$QT_BIN/qmlagentctl" screenshot --out fallback.png --scale 0.5
```

## Security Model

The QML debug channel is unauthenticated. Launcher-owned sessions use private
local sockets and 0700 per-user directories, so exposure is limited to the
same user account. Manual TCP attach (`-qmljsdebugger=port:...`) listens on
localhost without authentication on a deterministic per-user port: any local
process can connect, read the UI, take screenshots, and synthesize input.
Runtime mutation is additionally gated per session and off by default. Run
targets under QmlAgent only in environments where every local process is
trusted — development machines, CI sandboxes — never in production.

## Reporting Issues

File a GitHub issue or pull request with: Qt version, platform, launch command,
whether the target was built with `QT_QML_DEBUG`, and the smallest QML/app
snippet that reproduces the problem.

---

Copyright (C) 2026 Penk Chen <penkia@gmail.com>.
Licensed under the Apache License, Version 2.0.
