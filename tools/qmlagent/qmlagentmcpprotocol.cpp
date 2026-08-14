// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qmlagentmcpprotocol.h"

#include <QtCore/qjsondocument.h>
#include <QtCore/qvariant.h>

namespace QmlAgentMcp {

static QJsonObject schema(const QJsonObject &properties, const QJsonArray &required = {})
{
    return {
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), properties },
        { QStringLiteral("required"), required },
        { QStringLiteral("additionalProperties"), false },
    };
}

static QJsonObject nodeRefSchema()
{
    return {
        { QStringLiteral("selector"), QJsonObject{
            { QStringLiteral("type"), QStringLiteral("string") },
            { QStringLiteral("description"), QStringLiteral("QmlAgent selector, for example id=\"saveButton\". Repeated delegates may use id/objectName plus index, for example id=\"boxRect\" index=0, when UI.query exposes delegate.index metadata.") },
        } },
        { QStringLiteral("nodeId"), QJsonObject{
            { QStringLiteral("type"), QStringLiteral("integer") },
            { QStringLiteral("description"), QStringLiteral("Session-local node id. Prefer selector when possible.") },
        } },
    };
}

static QJsonObject withNodeRef(QJsonObject properties)
{
    const QJsonObject nodeRef = nodeRefSchema();
    for (auto it = nodeRef.constBegin(), end = nodeRef.constEnd(); it != end; ++it)
        properties.insert(it.key(), it.value());
    return properties;
}

static QJsonObject waitUntilSchema()
{
    return {
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("description"),
          QStringLiteral("Predicate to wait for: state:\"found\"/\"notFound\", or property/op/value such as {\"property\":\"visible\",\"op\":\"=\",\"value\":true}. String properties also support contains, startsWith, and endsWith.") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("state"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("string") },
                { QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("found"),
                    QStringLiteral("notFound"),
                } },
            } },
            { QStringLiteral("property"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
            { QStringLiteral("op"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("string") },
                { QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("="),
                    QStringLiteral("=="),
                    QStringLiteral("!="),
                    QStringLiteral(">"),
                    QStringLiteral(">="),
                    QStringLiteral("<"),
                    QStringLiteral("<="),
                    QStringLiteral("contains"),
                    QStringLiteral("startsWith"),
                    QStringLiteral("endsWith"),
                } },
            } },
            { QStringLiteral("value"), QJsonObject{
                { QStringLiteral("description"), QStringLiteral("Expected primitive JSON value for property comparisons.") },
            } },
            { QStringLiteral("timeoutMs"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
        } },
        { QStringLiteral("additionalProperties"), false },
    };
}

static QJsonObject tool(const QString &name, const QString &description,
                        const QJsonObject &inputSchema)
{
    return {
        { QStringLiteral("name"), name },
        { QStringLiteral("description"), description },
        { QStringLiteral("inputSchema"), inputSchema },
    };
}

QJsonArray toolList()
{
    const QJsonObject stringArray{
        { QStringLiteral("type"), QStringLiteral("array") },
        { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
    };

    QJsonArray tools{
        tool(QStringLiteral("qmlagent_connect_tcp"),
             QStringLiteral("Attach directly over TCP to an app launched manually with -qmljsdebugger=port:<port>,host:<host>,services:QmlAgent; pass the exact port from that command. Not needed for qmlagent-launcher sessions — those auto-route. Use only for manual targets or streamed subscriptions (qmlagent_ui_subscribe, qmlagent_log_enable). Verify with qmlagent_target_status."),
             schema({
                 { QStringLiteral("host"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("TCP host. Defaults to 127.0.0.1.") },
                 } },
                 { QStringLiteral("port"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("integer") },
                     { QStringLiteral("description"), QStringLiteral("TCP port from the target launch command. When omitted, qmlagent-mcp uses a deterministic per-user fallback port; launcher sessions do not need this.") },
                 } },
                 { QStringLiteral("timeoutMs"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("integer") },
                     { QStringLiteral("description"), QStringLiteral("Connection timeout in milliseconds.") },
                 } },
             })),
        tool(QStringLiteral("qmlagent_connect_local_socket"),
             QStringLiteral("Attach directly over a local socket to an app launched manually with -qmljsdebugger=file:<path>,services:QmlAgent. Not needed for qmlagent-launcher sessions — those auto-route. Use only for manual targets or streamed subscriptions. Verify with qmlagent_target_status."),
             schema({
                 { QStringLiteral("path"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("Local socket path or name passed to -qmljsdebugger=file:<path>.") },
                 } },
                 { QStringLiteral("timeoutMs"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("integer") },
                     { QStringLiteral("description"), QStringLiteral("Connection timeout in milliseconds.") },
                 } },
             }, { QStringLiteral("path") })),
        tool(QStringLiteral("qmlagent_disconnect"),
             QStringLiteral("Detach from the current QmlAgent target. Use before relaunching or switching target processes."),
             schema({})),
        tool(QStringLiteral("qmlagent_target_status"),
             QStringLiteral("Start here: report attach state and qmlagent-launcher gateway routing. With exactly one launcher session, target-backed tools route through it automatically; with several, launcherGateway.sessions lists ids for pinning. Direct attach is only for manually launched targets or streamed subscriptions."),
             schema({})),
        tool(QStringLiteral("qmlagent_preview_reload"),
             QStringLiteral("Reload the root QML file for a session started exactly with qmlagent-launcher preview <Main.qml>. This does not work for qmlagent-launcher app <executable> sessions; those must rebuild/relaunch unless the app owns its own reload boundary."),
             schema({
                 { QStringLiteral("timeoutMs"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("integer") },
                     { QStringLiteral("description"), QStringLiteral("Reload timeout in milliseconds. Verify the new state separately with qmlagent_ui_wait_for or qmlagent_ui_query.") },
                 } },
             })),
        tool(QStringLiteral("qmlagent_launcher_stop"),
             QStringLiteral("Stop the current qmlagent-launcher session discovered in the workspace. Use this to close a preview or application session without manual process killing."),
             schema({})),
        tool(QStringLiteral("qmlagent_ui_get_tree"),
             QStringLiteral("Get a projected Qt Quick UI tree. Keep fields/maxNodes bounded unless full evidence is required. If selector is supplied without depth, the adapter searches the full tree and returns the matched branch. When this QmlAgent build has QtQuick3D support, View3D scenes include Model/camera/light/material frontend nodes automatically. The View3D boundary is tagged renderKind=\"QtQuick3D\"; scene descendants use kind=\"QQuick3DObject\" and are structural evidence, not QQuickItem click targets. Request fields=[\"render3D\"] for Quick3D Model world bounds, projected screen bounds, camera distance, and approximate frustum evidence. QtCanvasPainter item subclasses are tagged renderKind=\"QtCanvasPainter\". Request app-exposed paint-state properties to inspect structured brushes, patterns, shadows, images, and bounded path evidence; this is application state, not an observed draw-call stream."),
             schema({
                 { QStringLiteral("depth"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 { QStringLiteral("includeInvisible"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                 { QStringLiteral("includeSource"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                 { QStringLiteral("fields"), stringArray },
                 { QStringLiteral("properties"), stringArray },
                 { QStringLiteral("selector"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("maxNodes"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 { QStringLiteral("collapseRepeated"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
             })),
        tool(QStringLiteral("qmlagent_ui_query"),
             QStringLiteral("Query nodes by stable QmlAgent selector. Defaults to verbosity=\"summary\" to protect agent context; ask for verbosity=\"full\" only when omittedFields/nextHints say deeper node evidence is needed. Prefer selector over nodeId across restarts. When this QmlAgent build has QtQuick3D support, View3D scenes include Model/camera/light/material frontend nodes automatically. The View3D boundary is tagged renderKind=\"QtQuick3D\"; scene descendants use kind=\"QQuick3DObject\" and are structural evidence, not QQuickItem click targets. Request fields=[\"render3D\"] on Quick3D Model nodes for world bounds, projected screen bounds, camera distance, and approximate frustum evidence. QtCanvasPainter item subclasses are tagged renderKind=\"QtCanvasPainter\". Request app-exposed paint-state properties to inspect structured brushes, patterns, shadows, images, and bounded path evidence; this is application state, not an observed draw-call stream. For repeated delegates, try id/objectName plus index such as id=\"boxRect\" index=0 before adding objectName or using session-local nodeId. Requested properties are returned even when fields is projected. If a single qmlagent-launcher session exists, this routes through the launcher automatically; no direct attach is needed. Use this to find selectors before qmlagent_input_drag, qmlagent_input_wheel, qmlagent_ui_wait_for, or qmlagent_workflow_click_and_wait."),
             schema({
                 { QStringLiteral("selector"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("includeSource"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                 { QStringLiteral("fields"), stringArray },
                 { QStringLiteral("properties"), stringArray },
                 { QStringLiteral("maxNodes"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 { QStringLiteral("verbosity"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("default"), QStringLiteral("summary") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("full"),
                         QStringLiteral("summary"),
                     } },
                 } },
             }, { QStringLiteral("selector") })),
        tool(QStringLiteral("qmlagent_ui_query_many"),
             QStringLiteral("Batch verification reads: run several UI.query selectors in one round trip and return results aligned with the queries array. Prefer this over sequential qmlagent_ui_query calls when checking multiple nodes/properties after an action; it saves agent round trips and tokens. Per-entry fields mirror qmlagent_ui_query; defaults applies shared options to entries that omit them. At most 50 queries per batch. Defaults to verbosity=\"summary\"."),
             schema({
                 { QStringLiteral("queries"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("description"), QStringLiteral("Per-entry UI.query parameters; selector is required in each entry unless supplied through defaults.") },
                     { QStringLiteral("items"), QJsonObject{
                         { QStringLiteral("type"), QStringLiteral("object") },
                         { QStringLiteral("properties"), QJsonObject{
                             { QStringLiteral("selector"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                             { QStringLiteral("includeSource"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                             { QStringLiteral("fields"), stringArray },
                             { QStringLiteral("properties"), stringArray },
                             { QStringLiteral("maxNodes"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                         } },
                         { QStringLiteral("additionalProperties"), false },
                     } },
                 } },
                 { QStringLiteral("defaults"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("object") },
                     { QStringLiteral("description"), QStringLiteral("Shared UI.query options merged into entries that do not set them, for example {\"properties\":[\"text\"],\"maxNodes\":5}.") },
                 } },
                 { QStringLiteral("verbosity"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("default"), QStringLiteral("summary") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("full"),
                         QStringLiteral("summary"),
                     } },
                 } },
             }, { QStringLiteral("queries") })),
        tool(QStringLiteral("qmlagent_ui_wait_for"),
             QStringLiteral("Wait until a selector is found/notFound or one matched node's property satisfies a predicate (=, !=, >, >=, <, <=, contains, startsWith, endsWith). Use after input, transitions, loaders, popups, and animations instead of sleeps or retry loops. Cannot wait on multiple nodes at once — property predicates require exactly one match. Timeout results carry nextHints for UI.query/Diagnostics follow-up."),
             schema({
                 { QStringLiteral("selector"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("until"), waitUntilSchema() },
                 { QStringLiteral("timeoutMs"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("integer") },
                     { QStringLiteral("description"), QStringLiteral("Maximum wait in milliseconds. Defaults to the service default when omitted.") },
                 } },
             }, { QStringLiteral("selector"), QStringLiteral("until") })),
        tool(QStringLiteral("qmlagent_render_pick3d"),
             QStringLiteral("Read-only QtQuick3D View3D hit test at a logical-pixel coordinate. coordinateSpace defaults to auto: try View3D-local first, then window coordinates when the point lies inside the View3D. Pass modelSelector to use View3D.pick(x,y,model) for targeted Model evidence. Use after UI.query fields=[\"render3D\"] when you need to connect screen-space evidence to a Model selector. This does not deliver input events and 3D Model nodes remain non-QQuickItem click targets."),
             schema({
                 { QStringLiteral("selector"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("Selector for the View3D node to pick within.") },
                 } },
                 { QStringLiteral("modelSelector"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("Optional selector for a QtQuick3D Model. When present, QmlAgent invokes the targeted View3D.pick(x,y,model) overload.") },
                 } },
                 { QStringLiteral("x"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("number") },
                     { QStringLiteral("description"), QStringLiteral("X coordinate in the selected coordinateSpace.") },
                 } },
                 { QStringLiteral("y"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("number") },
                     { QStringLiteral("description"), QStringLiteral("Y coordinate in the selected coordinateSpace.") },
                 } },
                 { QStringLiteral("coordinateSpace"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("default"), QStringLiteral("auto") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("auto"),
                         QStringLiteral("local"),
                         QStringLiteral("view3d-local-logical-pixels"),
                         QStringLiteral("window"),
                         QStringLiteral("window-logical-pixels"),
                     } },
                     { QStringLiteral("description"), QStringLiteral("Coordinate interpretation. auto tries local first and falls back to window logical pixels if the point is inside the View3D.") },
                 } },
             }, { QStringLiteral("selector"), QStringLiteral("x"), QStringLiteral("y") })),
        tool(QStringLiteral("qmlagent_ui_subscribe"),
             QStringLiteral("Subscribe to coalesced QmlAgent UI.treeChanged events on this persistent connection."),
             schema({})),
        tool(QStringLiteral("qmlagent_ui_unsubscribe"),
             QStringLiteral("Unsubscribe from QmlAgent UI.treeChanged events on this persistent connection."),
             schema({})),
        tool(QStringLiteral("qmlagent_diagnostics_analyze_tree"),
             QStringLiteral("Analyze the runtime tree for structured layout/input/log issues. Defaults to application repair scope. Use verbosity:\"summary\" for bounded agent-loop output; use evidence/full only when patch-ready detail is needed. When capabilities.qtQuick3D.enabled is true, pass checks:[\"quick3D\"] to focus on View3D camera/light setup, Model material/scale, texture source, and render3D-backed frustum evidence. Pass includeFrameworkIssues:true or issueScope:\"all\" when developing Qt Quick Controls/framework internals."),
             schema({
                 { QStringLiteral("checks"), stringArray },
                 { QStringLiteral("includeInvisible"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                 { QStringLiteral("includeFrameworkIssues"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                 { QStringLiteral("maxIssues"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 { QStringLiteral("issueScope"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("enum"), QJsonArray{ QStringLiteral("application"), QStringLiteral("all") } },
                 } },
                 { QStringLiteral("verbosity"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("default"), QStringLiteral("summary") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("summary"),
                         QStringLiteral("evidence"),
                         QStringLiteral("full"),
                     } },
                 } },
             })),
        tool(QStringLiteral("qmlagent_diagnostics_analyze_node"),
             QStringLiteral("Analyze one node by selector or nodeId and return evidence-backed issues. For QtQuick3D nodes, pass checks:[\"quick3D\"] to focus on View3D camera/light setup, Model material/scale, texture source, and render3D-backed frustum evidence."),
             schema(withNodeRef({ { QStringLiteral("checks"), stringArray } }))),
        tool(QStringLiteral("qmlagent_diagnostics_analyze_binding"),
             QStringLiteral("Resolve runtime binding provenance for one property. Use this when geometry/state looks computed: it reports active QQmlBinding or Qt bindable-property binding evidence, current value, source location, bounded source snippet, candidate identifier follow-up hints, currently captured dependency values when Qt exposes them, and bindable-property dependency summaries when values are not exposed. Start from result.assignmentSite: it is the ranked answer to \"which file:line do I edit to change this value\" for both live bindings and literal assignments; the provenance dossier is for verification. Source-token identifiers are hints, not dependency proof. This is structured repair evidence; it does not mutate the app."),
             schema(withNodeRef({
                 { QStringLiteral("property"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("Property to inspect, for example x, y, width, height, visible, enabled, text, or color.") },
                 } },
             }), { QStringLiteral("property") })),
        tool(QStringLiteral("qmlagent_runtime_enable_mutation"),
             QStringLiteral("Explicitly enable setup-only Runtime.* mutation commands for this QmlAgent debug session. Required before qmlagent_runtime_set_property or qmlagent_runtime_invoke_method."),
             schema({
                 { QStringLiteral("enabled"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("boolean") },
                     { QStringLiteral("description"), QStringLiteral("Defaults to true. Set false to disable Runtime.* mutation again.") },
                 } },
             })),
        tool(QStringLiteral("qmlagent_input_click"),
             QStringLiteral("Click one node through Qt synthetic input. A delivered click returns ok:true for input delivery only; settle.timedOut means no frame was observed before the settle timeout and is not semantic proof of failure. For transitions, Drawer/Menu/Popup/Dialog open-close, loaders, or async state, prefer qmlagent_workflow_click_and_wait; if that tool is not visible in lazy native-tool discovery, call qmlagent_input_click then qmlagent_ui_wait_for."),
             schema(withNodeRef({
                 { QStringLiteral("point"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") } } },
                     { QStringLiteral("description"), QStringLiteral("Optional item-local [x,y] to click instead of the node center. Use for a wide hit area or when the center is occluded but an offset is clear. Actionability is checked at this point.") },
                 } },
                 { QStringLiteral("settle"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("object") },
                     { QStringLiteral("description"), QStringLiteral("Optional settle tuning such as timeoutMs. This affects frame-settle evidence only, not semantic verification.") },
                 } },
             }))),
        tool(QStringLiteral("qmlagent_input_long_press"),
             QStringLiteral("Long-press one node through Qt synthetic mouse input as one atomic action: press, hold, release cleanup, then settle. Use for MouseArea.onPressAndHold, context actions, press-and-hold affordances, and mobile-style UI. Prefer this over manual qmlagent_input_mouse press/wait/release sequences so agents do not leave a held button after interruption. Verify post-action state with qmlagent_ui_wait_for or use qmlagent_workflow_long_press_and_wait."),
             schema(withNodeRef({
                 { QStringLiteral("holdMs"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("integer") },
                     { QStringLiteral("description"), QStringLiteral("Hold duration in milliseconds. Defaults to 900.") },
                 } },
                 { QStringLiteral("button"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("point"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") } } },
                 } },
                 { QStringLiteral("modifiers"), stringArray },
             }))),
        tool(QStringLiteral("qmlagent_input_wheel"),
             QStringLiteral("Dispatch one wheel event at a selector/node center through Qt synthetic input. Use negative deltaY to scroll down in normal Qt Quick Flickable/ListView/GridView/TableView/TreeView/ScrollView content. After scrolling, verify with qmlagent_ui_wait_for or qmlagent_ui_query."),
             schema(withNodeRef({
                 { QStringLiteral("deltaX"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 { QStringLiteral("deltaY"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 { QStringLiteral("angleDelta"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 } },
                 { QStringLiteral("pixelDelta"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 } },
                 { QStringLiteral("modifiers"), stringArray },
             }))),
        tool(QStringLiteral("qmlagent_input_scroll_into_view"),
             QStringLiteral("Scroll a selector/node into view by adjusting its ancestor Flickable/ListView content positions deterministically, then settle. Use when a click or read fails with center_outside_viewport on instantiated-but-clipped content. Rows a virtualized view has not created yet have no node to target: wheel toward them with qmlagent_input_wheel first, then re-query. Verify the final state with qmlagent_ui_query or qmlagent_ui_wait_for."),
             schema(withNodeRef({}))),
        tool(QStringLiteral("qmlagent_input_focus"),
             QStringLiteral("Focus one QQuickItem by selector or nodeId for keyboard input."),
             schema(withNodeRef({}))),
        tool(QStringLiteral("qmlagent_input_mouse"),
             QStringLiteral("Dispatch one low-level mouse press/move/release event at a selector/node item point through Qt synthetic input, for a single event you cannot express with the higher-level tools. Do NOT hand-roll a drag from press/move/release here: a manual mouse sequence does not drive Qt gesture handlers (DragHandler stays inactive). Use qmlagent_input_drag for drags, which activates them correctly."),
             schema(withNodeRef({
                 { QStringLiteral("type"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("mousePress"),
                         QStringLiteral("mouseMove"),
                         QStringLiteral("mouseRelease"),
                     } },
                 } },
                 { QStringLiteral("button"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("buttons"), stringArray },
                 { QStringLiteral("point"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") } } },
                 } },
                 { QStringLiteral("modifiers"), stringArray },
             }), { QStringLiteral("type") })),
        tool(QStringLiteral("qmlagent_input_drag"),
             QStringLiteral("Drag one selector/node through Qt mouse input. Use for Slider, RangeSlider, Dial, splitters, handles, swipe gestures, and draggable controls. Provide item-local to:[x,y] or delta:[dx,dy]; the tool sends press, at least two moves, and release, then returns settle metadata. After animated drag state, verify with qmlagent_ui_wait_for or qmlagent_ui_query."),
             schema(withNodeRef({
                 { QStringLiteral("from"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") } } },
                 } },
                 { QStringLiteral("to"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") } } },
                 } },
                 { QStringLiteral("delta"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") } } },
                 } },
                 { QStringLiteral("steps"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 { QStringLiteral("button"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("modifiers"), stringArray },
             }))),
        tool(QStringLiteral("qmlagent_input_touch"),
             QStringLiteral("Dispatch one bounded touch begin/update/end/cancel event at selector/node item-local points through Qt synthetic input. Use explicit point ids and states for multipoint sequences."),
             schema(withNodeRef({
                 { QStringLiteral("type"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("touchBegin"),
                         QStringLiteral("touchUpdate"),
                         QStringLiteral("touchEnd"),
                         QStringLiteral("touchCancel"),
                     } },
                 } },
                 { QStringLiteral("points"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("items"), QJsonObject{
                         { QStringLiteral("type"), QStringLiteral("object") },
                         { QStringLiteral("properties"), QJsonObject{
                             { QStringLiteral("id"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                             { QStringLiteral("state"), QJsonObject{
                                 { QStringLiteral("type"), QStringLiteral("string") },
                                 { QStringLiteral("enum"), QJsonArray{
                                     QStringLiteral("pressed"),
                                     QStringLiteral("updated"),
                                     QStringLiteral("stationary"),
                                     QStringLiteral("released"),
                                 } },
                             } },
                             { QStringLiteral("point"), QJsonObject{
                                 { QStringLiteral("type"), QStringLiteral("array") },
                                 { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") } } },
                             } },
                         } },
                     } },
                 } },
                 { QStringLiteral("modifiers"), stringArray },
             }), { QStringLiteral("type"), QStringLiteral("points") })),
        tool(QStringLiteral("qmlagent_input_key"),
             QStringLiteral("Dispatch a key event, optionally targeting selector/nodeId first."),
             schema(withNodeRef({
                 { QStringLiteral("key"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("keyCode"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 { QStringLiteral("text"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("type"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("keyClick"),
                         QStringLiteral("keyPress"),
                         QStringLiteral("keyRelease"),
                     } },
                 } },
                 { QStringLiteral("modifiers"), stringArray },
             }))),
        tool(QStringLiteral("qmlagent_input_type_text"),
             QStringLiteral("Type text through synthetic key input, optionally targeting selector/nodeId first. Pass replaceExisting=true to clear the field's current content first in the same call; without it, text lands at the target's normal cursor/selection state. If click-to-focus fails, call qmlagent_input_focus on the same selector/nodeId, then retry; focus_failed results include nextHints. Verify final text with qmlagent_ui_query or qmlagent_ui_wait_for."),
             schema(withNodeRef({
                        { QStringLiteral("text"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                        { QStringLiteral("replaceExisting"), QJsonObject{
                            { QStringLiteral("type"), QStringLiteral("boolean") },
                            { QStringLiteral("description"), QStringLiteral("Clear existing content before typing (one call instead of clear_text + type_text).") },
                        } },
                    }),
                    { QStringLiteral("text") })),
        tool(QStringLiteral("qmlagent_input_clear_text"),
             QStringLiteral("Clear a TextInput/TextField/TextArea-style target through the same Input.typeText path: focus target, select existing text when available, then delete through Qt key input. Use qmlagent_input_type_text afterward to enter new text, and verify final text/state with qmlagent_ui_query or qmlagent_ui_wait_for."),
             schema(withNodeRef({}))),
        tool(QStringLiteral("qmlagent_input_dismiss_popup"),
             QStringLiteral("Close the topmost visible popup (Menu/Dialog/Drawer/Popup) generically when Esc or a dismiss button is not reliable, unblocking further input. Pass all=true to close every stacked popup. The result reports popupCountBefore/remainingPopupCount so you can confirm the popup actually closed; a popup.not_dismissed diagnostic means it survived (reopened from a binding, or a non-Popup overlay). Verify follow-up state with qmlagent_ui_query or qmlagent_ui_wait_for."),
             schema({ { QStringLiteral("all"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } } })),
        tool(QStringLiteral("qmlagent_workflow_click"),
             QStringLiteral("Click a target selector and verify an immediate expected state in one dispatcher-owned workflow report. Use verbosity=\"summary\" for normal agent loops; use full only when deep evidence is needed. For Drawer/Menu/Popup/Dialog transitions, loaders, animated controls, or delayed availability, use qmlagent_workflow_click_and_wait instead of this qmlagent_workflow_click tool."),
             schema({
                 { QStringLiteral("selector"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("expectSelector"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("expect"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("Property expectation such as visible=true or text!=\"\".") },
                 } },
                 { QStringLiteral("verbosity"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("full"),
                         QStringLiteral("summary"),
                     } },
                 } },
             }, { QStringLiteral("selector"), QStringLiteral("expectSelector"), QStringLiteral("expect") })),
        tool(QStringLiteral("qmlagent_workflow_click_and_wait"),
             QStringLiteral("Agent-first compressed workflow for transitions/popups: click a target selector through Qt input, subscribe to UI events, then UI.waitFor a semantic predicate in one report. Prefer this over qmlagent_input_click plus manual sleeps/retries for Drawer/Menu/Popup transitions, animations, loaders, and post-click async state. For Controls popups, prefer waiting for type=\"QQuickPopupItem\" or another generic popup/container selector, then query visible ItemDelegate/MenuItem/etc. contents because platform styles do not expose one uniform item type."),
             schema({
                 { QStringLiteral("selector"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("Target selector to click.") },
                 } },
                 { QStringLiteral("waitSelector"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("Selector whose state/property should satisfy the wait predicate after the click.") },
                 } },
                 { QStringLiteral("until"), waitUntilSchema() },
                 { QStringLiteral("timeoutMs"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 { QStringLiteral("verbosity"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("full"),
                         QStringLiteral("summary"),
                     } },
                 } },
             }, { QStringLiteral("selector"), QStringLiteral("waitSelector"), QStringLiteral("until") })),
        tool(QStringLiteral("qmlagent_workflow_long_press_and_wait"),
             QStringLiteral("Agent-first compressed workflow for press-and-hold UI: long-press a target selector, release safely, then UI.waitFor a semantic predicate in one report. Use for MouseArea.onPressAndHold, context menus, mobile-style alternate actions, and hidden affordances. Prefer this over qmlagent_input_mouse press + sleep + release."),
             schema({
                 { QStringLiteral("selector"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("Target selector to long-press.") },
                 } },
                 { QStringLiteral("waitSelector"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("Selector whose state/property should satisfy the wait predicate after the long press.") },
                 } },
                 { QStringLiteral("until"), waitUntilSchema() },
                 { QStringLiteral("holdMs"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("integer") },
                     { QStringLiteral("description"), QStringLiteral("Hold duration in milliseconds. Defaults to 900.") },
                 } },
                 { QStringLiteral("timeoutMs"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
                 { QStringLiteral("verbosity"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("full"),
                         QStringLiteral("summary"),
                     } },
                 } },
             }, { QStringLiteral("selector"), QStringLiteral("waitSelector"), QStringLiteral("until") })),
        tool(QStringLiteral("qmlagent_workflow_key"),
             QStringLiteral("Dispatch a key to a target selector and verify expected state in one dispatcher-owned workflow report. Expectations support property=value, numeric comparisons, and string operators such as text contains \"google\" / text startsWith \"https://\". Use verbosity=\"summary\" for normal agent loops; use full only when deep evidence is needed."),
             schema({
                 { QStringLiteral("selector"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("key"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("expectSelector"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                { QStringLiteral("expect"), QJsonObject{
                    { QStringLiteral("type"), QStringLiteral("string") },
                    { QStringLiteral("description"), QStringLiteral("Property expectation such as visible=true, text!=\"\", text contains \"google\", or text startsWith \"https://\".") },
                } },
                 { QStringLiteral("verbosity"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("enum"), QJsonArray{
                         QStringLiteral("full"),
                         QStringLiteral("summary"),
                     } },
                 } },
             }, { QStringLiteral("selector"), QStringLiteral("key"), QStringLiteral("expectSelector"), QStringLiteral("expect") })),
        tool(QStringLiteral("qmlagent_runtime_set_property"),
             QStringLiteral("White-box setup only: set one QObject/QML property by selector or nodeId. Requires qmlagent_runtime_enable_mutation first. Verify final behavior with UI/Input/Diagnostics/Log evidence; do not use as proof that a user can interact with the UI."),
             schema(withNodeRef({
                 { QStringLiteral("property"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("value"), QJsonObject{ { QStringLiteral("description"), QStringLiteral("Primitive JSON value, or simple primitive array/object when QVariant conversion is trivial.") } } },
                 { QStringLiteral("settle"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("object") } } },
             }), { QStringLiteral("property"), QStringLiteral("value") })),
        tool(QStringLiteral("qmlagent_runtime_invoke_method"),
             QStringLiteral("White-box setup only: invoke a public slot/Q_INVOKABLE by selector or nodeId with primitive args. Requires qmlagent_runtime_enable_mutation first. Verify final behavior with UI/Input/Diagnostics/Log evidence; do not use as proof that a control is clickable."),
             schema(withNodeRef({
                 { QStringLiteral("method"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("args"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("array") },
                     { QStringLiteral("items"), QJsonObject{} },
                 } },
                 { QStringLiteral("settle"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("object") } } },
             }), { QStringLiteral("method") })),
        tool(QStringLiteral("qmlagent_log_enable"),
             QStringLiteral("Enable QmlAgent log events on this persistent connection."),
             schema({ { QStringLiteral("replayBuffered"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } } })),
        tool(QStringLiteral("qmlagent_log_get_entries"),
             QStringLiteral("Return buffered QML/log entries. Log.enable is not required first. Use sinceTimestamp from the previous result's nextSinceTimestamp to fetch only new entries in long agent loops."),
             schema({
                 { QStringLiteral("level"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                 { QStringLiteral("sinceTimestamp"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") } } },
                 { QStringLiteral("maxEntries"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") } } },
             })),
        tool(QStringLiteral("qmlagent_render_capture_screenshot"),
             QStringLiteral("Fallback visual evidence: capture a QQuickWindow screenshot. Do not use this as the primary oracle. First use qmlagent_ui_query/ui_get_tree, diagnostics, logs, source, and input/workflow tools; request screenshot data only when structured evidence is insufficient or the task is explicitly visual. By default this returns metadata without PNG data. When you need the actual image, pass outPath to write the PNG to a file (bytes are stripped from the result, so your context stays clean); use includeData:true only if you truly need base64 inline, and prefer scale/region either way."),
             schema({
                 { QStringLiteral("windowId"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("integer") },
                     { QStringLiteral("description"), QStringLiteral("1-based QQuickWindow id. Defaults to the first QQuickWindow.") },
                 } },
                 { QStringLiteral("outPath"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("string") },
                     { QStringLiteral("description"), QStringLiteral("Write the PNG to this file path and return writtenTo/bytesWritten instead of base64. The preferred way to get the image without spending context on it.") },
                 } },
                 { QStringLiteral("includeData"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("boolean") },
                     { QStringLiteral("description"), QStringLiteral("When true, include base64 PNG data inline. Defaults to false to preserve agent context budget; prefer outPath.") },
                 } },
                 { QStringLiteral("scale"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("number") },
                     { QStringLiteral("description"), QStringLiteral("Optional downscale factor in (0, 1]. Use values such as 0.5 or 0.25 for token-safe fallback visual evidence.") },
                 } },
                 { QStringLiteral("region"), QJsonObject{
                     { QStringLiteral("type"), QStringLiteral("object") },
                     { QStringLiteral("description"), QStringLiteral("Optional window-local logical-pixel crop: {x,y,width,height}. Useful for fallback visual evidence around one UI area.") },
                 } },
             })),
        tool(QStringLiteral("qmlagent_source_resolve"),
             QStringLiteral("Resolve one node back to source with method/confidence/limitations."),
             schema(withNodeRef({}))),
    };

    // Every launcher-routed tool accepts an optional session pin so an agent
    // running alongside other live sessions (e.g. a test target) can address
    // its own, and a dying target cannot silently reroute the call to another
    // app. The direct-attach connect tools carry their own endpoint instead.
    const QJsonObject sessionProperty{
        { QStringLiteral("type"), QStringLiteral("string") },
        { QStringLiteral("description"),
          QStringLiteral("Optional launcher session id to pin this call to (an id prefix is "
                         "enough; list live sessions and their ids with qmlagent_target_status). "
                         "Without it, routing needs exactly one live launcher session and reports "
                         "the choices when several are live.") },
    };
    const QSet<QString> directAttachTools{
        QStringLiteral("qmlagent_connect_tcp"),
        QStringLiteral("qmlagent_connect_local_socket"),
    };
    for (QJsonValueRef toolValue : tools) {
        QJsonObject toolObject = toolValue.toObject();
        if (directAttachTools.contains(toolObject.value(QStringLiteral("name")).toString()))
            continue;
        QJsonObject inputSchema = toolObject.value(QStringLiteral("inputSchema")).toObject();
        QJsonObject properties = inputSchema.value(QStringLiteral("properties")).toObject();
        properties.insert(QStringLiteral("session"), sessionProperty);
        inputSchema.insert(QStringLiteral("properties"), properties);
        toolObject.insert(QStringLiteral("inputSchema"), inputSchema);
        toolValue = toolObject;
    }
    return tools;
}

QJsonObject jsonResponse(const QJsonValue &id, const QJsonValue &result)
{
    return {
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("id"), id },
        { QStringLiteral("result"), result },
    };
}

QJsonObject jsonError(const QJsonValue &id, int code, const QString &message,
                      const QJsonValue &data)
{
    QJsonObject error{
        { QStringLiteral("code"), code },
        { QStringLiteral("message"), message },
    };
    if (!data.isUndefined())
        error.insert(QStringLiteral("data"), data);
    return {
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("id"), id },
        { QStringLiteral("error"), error },
    };
}

QJsonObject toolResult(const QJsonValue &payload, bool isError)
{
    QString text = isError ? QStringLiteral("QmlAgent tool failed. See structuredContent.")
                           : QStringLiteral("QmlAgent tool result. See structuredContent.");
    if (isError && payload.isObject()) {
        const QString error = payload.toObject().value(QStringLiteral("error")).toString();
        if (!error.isEmpty())
            text = QStringLiteral("QmlAgent tool failed: %1").arg(error);
    } else if (!payload.isObject() && !payload.isArray()) {
        text = payload.toVariant().toString();
    }

    QJsonObject result{
        { QStringLiteral("content"), QJsonArray{ QJsonObject{
            { QStringLiteral("type"), QStringLiteral("text") },
            { QStringLiteral("text"), text },
        } } },
    };
    if (payload.isObject() || payload.isArray())
        result.insert(QStringLiteral("structuredContent"), payload);
    if (isError)
        result.insert(QStringLiteral("isError"), true);
    return result;
}

QJsonObject toolErrorResult(const QString &message)
{
    return toolResult(QJsonObject{ { QStringLiteral("error"), message } }, true);
}

} // namespace QmlAgentMcp
