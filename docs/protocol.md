# QmlAgent Protocol Reference

Protocol version: **0.1** (reported by `Session.getInfo`). JSON-RPC 2.0. This document is generated from the implementation in `src/plugins/qmltooling/qmldbg_agent/` and `tools/qmlagent/`.

## 1. Transport & framing

QmlAgent is a Qt `qmltooling` debug service. Messages are JSON-RPC 2.0 objects, one per Qt debug-connection packet, exchanged with the service named **`QmlAgent`**.

- Launch the target with `-qmljsdebugger=file:<socket>,block,services:QmlAgent` (block mode; the target waits for the client). `qmlagent-launcher` does this for you.
- One debug connection, one client; requests are handled serially on the debug thread.
- Requests must be objects: `{"jsonrpc":"2.0","id":<any>,"method":"<Domain.method>","params":{...}}`. `params`, when present, must be an object. A request without `id` is a notification: it is executed but gets no response.
- Responses: `{"jsonrpc":"2.0","id":...,"result":{...}}` or `{"jsonrpc":"2.0","id":...,"error":{"code":...,"message":...,"data":{...}}}`.
- Server-initiated events are JSON-RPC notifications (no `id`): `Session.reset`, `Session.payloadDropped`, `Log.entryAdded`, `UI.treeChanged`.

JSON-RPC error codes:

| Code | Meaning |
|---|---|
| -32700 | Parse error (invalid JSON) |
| -32600 | Invalid request (missing `jsonrpc:"2.0"` or string `method`) |
| -32602 | Invalid params (`params` present but not an object) |
| -32601 | Method not found (not in the method list below) |
| -32000 | Request payload too large (`data`: `actualBytes`, `maxBytes`, `hint`) |
| -32001 | Response payload too large (`data`: `method`, `actualBytes`, `maxBytes`, `hints`) |

Payload bounds: **16 MiB** inbound and outbound (`MaxInboundMessageBytes` / `MaxOutboundMessageBytes`, both `16 * 1024 * 1024`). An oversized *response* is replaced by a -32001 error with follow-up hints (narrow the selector, use `fields`/`maxNodes`/`verbosity:"summary"`, etc.). An oversized *event* is dropped and replaced by a `Session.payloadDropped` notification.

### Launcher control mailbox

`qmlagent-launcher preview <Main.qml>` / `qmlagent-launcher app <executable> [-- args]` starts the target and exposes a second, file-based control channel (used by `qmlagentctl` and the MCP server). The mailbox directory is published in the launcher session registry (`.qmlagent/launcher-sessions/`, plus temp/data fallbacks) as `controlEndpoint: {kind:"fileMailbox", path:...}`.

- Write a request as `request-*.json` (single JSON-RPC-shaped object) into the mailbox dir; the launcher polls (~10 ms) and answers in `response-*.json` (or the sandboxed `replyTo` path). Responses are `{"jsonrpc":"2.0","id":<control id>,"result":{...}}`; control-level failures put `{ok:false, error:{id,message}}` in `result`.
- Control methods:

| Method | Purpose |
|---|---|
| `Session.status` | Launcher/session status: `sessionReady`, `sessionType` (`preview`/`application`), `launchCommand`, `reloadPreviewSupported`, `previewRoot`, `debugSocket`, `controlEndpoint`, `targetPid`, `targetRunning`, `services.QmlAgent`, `nextActions` |
| `Session.stop` | Terminates the target; result `{ok, action:"stop", targetPid}` |
| `QmlAgent.request` | Forwards `params.method` + `params.params` to the in-target QmlAgent service over the debug connection and relays the JSON-RPC response verbatim |
| `Preview.reload` | Preview sessions only. Reloads the root QML file; params: `timeoutMs` (default 3000). Result includes `ok`, `root`, `rootKind`, `rootStatePreserved:false` (contract: root is re-instantiated), `windowPreserved`, `errors`, `subscriptionsInvalidated:true`. Also sends a `Session.reset` notification to the target service |

Launcher control error ids: see §5.

## 2. Sessions & versioning

**`Session.getInfo`** (no params) returns:

```
{
  "service": "QmlAgent",
  "protocolVersion": "0.1",
  "qtVersion": "...",
  "sessionId": "<uuid>",          // new per service instance
  "processId": <pid>,
  "features": [ "Session.getInfo", ... ],   // every dispatchable method
  "capabilities": {
    "runtimeMutation":  { "enabled": bool, "verificationRole": "setup-only",
                          "configureWith": "Session.configure runtimeMutation=true" },
    "renderScreenshot": { "enabled": true, "evidenceRole": "fallback-supporting" },
    "qtQuick3D":        { "enabled": bool, "evidenceRole": "structural-source",
                          "methods": ["UI.getTree", "UI.query", "UI.queryMany"] },
    "payloadLimits":    { "maxInboundMessageBytes", "maxOutboundMessageBytes", "overflowBehavior" }
  }
}
```

`features` omits `Runtime.setProperty` and `Runtime.invokeMethod` while runtime mutation is disabled — feature-detect instead of hardcoding the list.

**`Session.configure`** — params: `runtimeMutation` (bool). Only applied if the key is present. Returns the full `Session.getInfo` payload.

**`Session.reset`** — params: `reason` (string, default `"client-request"`). Clears UI subscription + watchers, the log buffer, and disables runtime mutation. Result: `{reset:true, reason, subscriptionsInvalidated:true}`. A `Session.reset` event with the same fields is also emitted (and whenever the debug service state drops out of Enabled, reason `"debug-service-state-changed"`; the launcher emits reason `"preview-reloaded"` after `Preview.reload`). After this event all `nodeId`s and subscriptions must be treated as invalid.

Calling `Runtime.*` while mutation is disabled returns `{ok:false, mode:"whitebox", verificationRole:"setup-only"}` with diagnostic `runtime.capability_disabled` (not a JSON-RPC error).

## 3. Selector grammar

A selector is one *primary* predicate, optionally followed by disambiguating qualifiers. Values may be double-quoted (`id="x"`) or bare non-whitespace tokens. Supported forms (from `selectorSyntaxExamples()`):

```
id="saveButton"
objectName="settings.save"
type="Button"
text="Save"
nodeId=42
sourceLocation="src/Main.qml:44:7"
sourceLocation="src/Main.qml:44:7" instance=2
id="delegateRoot" index=4
id="tableCell" row=0 column=1
```

| Primary | Matches | Stability |
|---|---|---|
| `nodeId=N` (also bare `nodeId N`) | session-local node id | high, but session/reset-scoped |
| `id=` / `qmlId=` | authored QML id from the instantiating document | high if unique in tree, else medium |
| `objectName=` | `QObject::objectName` | high if unique, else medium |
| `type=` | pretty QML type name or any runtime `typeAliases` entry (e.g. `Button` for `QQuickButton`) | medium |
| `text=` | `text` property | low (may be translated) |
| `visualPath=` | `/Type[i]/Type[j]...` path | low (depends on sibling order) |
| `sourceLocation="file:line[:col]"` | authored source location | medium if unique; low when the line repeats across instances |

Qualifiers (non-negative ints):

- `index=N` — delegate model index; only matches nodes whose delegate metadata has `indexSource:"modelIndex"`.
- `row=N column=M` — table-like delegate cell; both required together; only with `cellSource:"delegateRowColumn"`. Mutually exclusive with `index`.
- `instance=N` — ordinal among repeated (non-delegate) instances sharing one source location (`sourceInstance` field on nodes); ordinal is stable within a session.

Not supported: compound predicates (`and`/`or`), parentheses, CSS-style selectors. Invalid syntax yields diagnostic `selector.invalid` with `supportedForms` and `limitations`.

Every emitted node carries a `selectors` array of `{kind, value, stability, reason?}`; delegate-local ids that repeat are downgraded to medium and an indexed selector (`id+index`, `sourceLocation+index`, `id+row+column`, `sourceLocation+instance`, ...) is appended.

## 4. Method reference

Node-targeted methods take **exactly one of** `nodeId` (int) or `selector` (string); violations yield `noderef.invalid`, dead ids `noderef.node_not_found` / `noderef.node_not_live`, selector misses `selector.not_found` / `selector.ambiguous`. Selector resolution for inputs queries with `includeInvisible:true`.

### Session

| Method | Params | Result |
|---|---|---|
| `Session.getInfo` | — | see §2 |
| `Session.configure` | `runtimeMutation` bool | `Session.getInfo` payload |
| `Session.reset` | `reason` string = `"client-request"` | `{reset, reason, subscriptionsInvalidated}` |

### Log

| Method | Params | Result |
|---|---|---|
| `Log.enable` | `replayBuffered` bool = false | `{enabled:true, replayed:N}`; enables `Log.entryAdded` events; `replayBuffered` re-emits buffered entries as events |
| `Log.getEntries` | `level` string (filter), `sinceTimestamp` double = -1, `maxEntries` int = -1 | `{entries[], entryCount, nextSinceTimestamp, skippedBeforeCursor, skippedByLevel, truncated, omittedEntryCount}` |
| `Log.clear` | — | `{cleared:true}` |

Entries: `{level: debug|info|warning|error|fatal, category, text, sourceLocation, timestamp}` (`timestamp` = seconds since epoch, fractional). Cursor semantics: entries with `timestamp <= sinceTimestamp` are skipped; pass the returned `nextSinceTimestamp` back to page forward. Buffer holds max **500 entries / 1 MiB**, oldest evicted; duplicates (same level|text|file|line) are dropped. Captured: QML engine warnings plus qml-ish Qt messages (any `*qml*` category, `ReferenceError:`/`TypeError:`/`QML ` messages); non-QML messages only at warning and above.

### UI

**`UI.getTree`** — params: `depth` int = -1 (unlimited), `includeInvisible` bool = false, `includeSource` bool = true, `properties` string[] (extra properties per node, dotted paths like `font.pixelSize` allowed), `fields` string[] (projection; empty = all), `maxNodes` int = -1, `collapseRepeated` bool = false (runs of ≥3 identical siblings become one `{kind:"RepeatedNodes", count, collapsed:true}` summary), `selector` string (prunes tree to matches; ancestors kept with `matchAncestor:true`).
Result: `{windows:[{windowId, title, width, height, devicePixelRatio, window:<window node>, root:<tree>}], nodeCount, truncated?, omittedNodeCount?, nextHints?}`. Node fields include `nodeId, windowId, kind, type, typeAliases, styleItem?, objectName, qmlId?, implementationId?, visualPath, text?, enabled?, visible, opacity, bbox, insideViewport, viewport, selectors, sourceLocation?, sourceInstance?, frameworkInternal?, delegate?, properties?, children` (`actionable`/`interactable` computed only when requested via `fields`).

When the QmlAgent plugin is built with `Qt6::Quick3D`, QtQuick3D frontend objects below `View3D` are included automatically. They are tagged with `kind:"QQuick3DObject"` and `sceneKind:"QtQuick3D"` when applicable. QtQuick3D nodes are structural/source evidence only: they do not have a 2D `bbox` and must not be treated as `QQuickItem` input targets.

**`UI.query`** — params: `selector` (required), `includeInvisible` = false, `includeSource` = true, `properties`, `fields`, `maxNodes` = -1, `depth` = 0 (children depth of each match), `includeStyleItems` bool = false.
Result: `{matches[], diagnostics[], truncated?, styleItemMatchesExcluded?, styleItemExclusionNote?, nextHints?}`. Behaviors: `type=` matches exclude native style items when exactly one authored control remains; matches all on one ancestor chain collapse to the outermost control; no match ⇒ `selector.not_found` with up to 20 ranked `candidateSelectors`; >1 match ⇒ `selector.ambiguous` with `matchCount`, `stableSelectorHints`, and globally-unique `indexedSelectors` suggestions.

**`UI.queryMany`** — params: `queries` object[] (1–50, each a `UI.query` params object), `defaults` object (merged into each entry). Result: `{results[], resultCount}` aligned with the input order. Errors: `batch.queries_required`, `batch.too_many_queries`. Dispatch budget: 250 ms per query.

**`UI.waitFor`** — params: `selector` (required), `until` (required), `timeoutMs` int = 1000, clamped 0–30000 (also read from `until.timeoutMs`). `until` forms:
- `{state:"found"}` / `{state:"notFound"}`
- `{property:"<name>", op:"<op>", value:<v>}` with ops `=` (alias `==`), `!=`, `>`, `>=`, `<`, `<=`, `contains`, `startsWith`, `endsWith`. Property predicates require exactly one match and query with `includeInvisible:true` (if exactly one match is visible, it wins).

Re-evaluates on frame swaps and on the target property's notify signal. Result: `{ok, timedOut, reason, selector, until, elapsedMs, timeoutMs, attempts, framesObserved, matchCount, diagnostics, property?, actual?, node?, nextHints?}`. `reason` ∈ `predicate_satisfied, state_not_satisfied, target_not_found, target_ambiguous, property_not_found, property_not_satisfied, unsupported_comparison, invalid_until`. Diagnostics: `wait.*` (§5).

**`UI.describeNode`** — params: `nodeId` int. Result: `{node}` (full node with `includeInvisible`/`includeSource`; `node:null` if the id is dead).

**`UI.getBoxModel`** — params: `nodeId`|`selector`. Result: `{node, bbox:[x,y,w,h], viewport:[x,y,w,h], insideViewport, viewportState}`; on failure `bbox`/`viewport` are null plus `issues`.

**`UI.subscribe`** — params ignored. Result: `{enabled:true, events:["UI.treeChanged"], mode:"coalesced-runtime-observation"}`. Emits coalesced `UI.treeChanged` events `{reason, sequence, windowId?}`; reasons: `window-geometry-changed, window-visible-changed, object-name-changed, item-geometry-changed, item-visible-changed, item-opacity-changed, item-children-changed, watcher-depth-limit`. **`UI.unsubscribe`** → `{enabled:false}`. Subscriptions are invalidated by `Session.reset`.

### Diagnostics

**`Diagnostics.analyzeNode`** — params: `nodeId`|`selector`, `checks` string[]. Check names (aliases in parentheses): `minSize` (`layout.minimum_size`), `insideViewport` (`layout.viewport`), `childExceedsParent` (`layout.clipping`), `overlap` (`layout.overlap`), `excessiveSpacer` (`layout.excessive_spacer`), `actionable`/`interactable` (`input.actionability`), `textElided` (`text.elide`), `clickable`. Empty `checks` runs the default set. Result: `{node, issues[]}`.
Issue shape: `{id, severity: error|warning, confidence: 0..1, nodeId?, message, evidence[], sourceLocation?}` plus, per check, `evidenceProfile {kind, basis, limitations}`, `actionability {ok, reasons, limitations}`, `blameChain[] {nodeId, type, sourceLocation, evidence}`, `bbox`, `patchDirection`, `repairHints[] {kind, confidence, reason, ...}`, `bindingProvenance[]` (compact form of `Diagnostics.analyzeBinding` provenance for blamed properties), `target`.

**`Diagnostics.analyzeTree`** — params: `includeInvisible` bool = false, `includeFrameworkIssues` bool = false, `issueScope` `"application"` (default) | `"all"`, `checks` string[] (plus `log.entries`), `verbosity` `"evidence"` (default) | `"summary"`, `maxIssues` int = 20 (summary mode, clamped 0–100).
Walks every window, runs `analyzeNode` per node, promotes repair-relevant log entries (warnings/errors with category `qml` or JS `ReferenceError`/`TypeError`) as `qml.warning` issues. Framework-owned issues are suppressed unless `includeFrameworkIssues`/`issueScope:"all"` (up to 20 summarized in `suppressedFrameworkIssues`). Result: `{issues[], summary{ok, issueCount, logEntryCount, promotedLogIssueCount, ignoredLogEntryCount, issueScope, suppressedFrameworkIssueCount, suppressedFrameworkIssueDetailsTruncated, suppressedFrameworkIssues, ran, verbosity, returnedIssueCount?, omittedIssueCount?, moreAvailable?, omittedFields?, nextHints?}}`. Summary mode strips `evidence`, `blameChain`, `repairHints`, `bindingProvenance`, `target.children`.

**`Diagnostics.analyzeBinding`** — params: `nodeId`|`selector`, `property` string (required).
Result: `{ok, property, value, node, target{nodeId, selector?}, provenance, assignmentSite, diagnostics?}`.
`provenance`: `{property, objectSourceLocation, kind: "binding"|"runtimeValue", isBinding, confidence, limitations[], ...}`; for bindings also `bindingKind` (e.g. `qpropertyBinding`), `sourceLocation`, `expression?`, `expressionSourceLocation?`, `candidateIdentifiers?`, `dependencies[]` (property/value pairs for QML bindings; empty + `dependencySummary` for bindable properties), `dependencyLimitations`; for runtime values `sourceAssignment {expression, sourceLocation}?`.
`assignmentSite`: the ranked "edit here" answer — `{sourceLocation, method: "bindingLocation"|"sourceAssignment"|"objectDeclaration", note?}`. Errors: `binding.target_not_found`, `binding.property_required`, `binding.property_not_found`.

### Input

Common behavior: target resolved via `nodeId`|`selector`; actionability is checked before dispatch (failure reasons §5); events go through `QQmlAgentInputDriver` into the target item's `QQuickWindow`; after dispatch the implementation waits for one `frameSwapped` or the settle timeout. Common params: `settle {timeoutMs}` (default **50**, clamp 0–30000), `modifiers` string[] (`shift`, `control`/`ctrl`, `alt`, `meta`). Buttons: `left` (default), `right`, `middle`, `back`, `forward` (+ `none` for `dispatchMouseEvent`).
Common success result: `{delivered:true, ok, node, deliveryWindow, mode, settle{strategy:"frameSwappedOrTimeout", framesAfterAction, elapsedMs, timedOut, reason}, postDispatch{targetLive, actionabilityRechecked, actionable?, actionabilityReasons?, point?, deliveryWindow?, verificationRole:"post-dispatch evidence only"}, verificationRole:"input-delivery-only", semanticProof:false, nextHints}`.
Common failure result: `{delivered:false, reason, diagnostics[], nextHints?}`.

| Method | Extra params | Extra result / notes |
|---|---|---|
| `Input.clickNode` | — | Press+release at bbox center; `point` |
| `Input.longPressNode` | `holdMs` int = 900 (1–10000), `button`, `point` [x,y] item coords (default center), `modifiers` | `holdMs, heldElapsedMs, framesDuringHold, releaseSent, itemPoint, button`; diag `input.not_long_pressable`; extra reasons `invalid_duration`, `invalid_button`, `point_outside_viewport` |
| `Input.wheel` | one of `pixelDelta`/`angleDelta` ([x,y] or {x,y}) or `deltaX`/`deltaY` (angle), `modifiers` | `pixelDelta, angleDelta, eventsSent`; diag `input.not_wheelable`; reason `invalid_delta` |
| `Input.scrollIntoView` | — | Adjusts ancestor Flickables (origin-aware) to center the item: `scrolled, insideViewportAfter, adjustments[{flickableNodeId, contentX/YBefore/After}]`; `reason` = `no_scrollable_ancestor` \| `target_still_outside_viewport`. Only works for instantiated items; virtualized rows without nodes need `Input.wheel` + re-query |
| `Input.focusNode` | — | `forceActiveFocus` on the item (or an editable control's contentItem): `{focused, activeFocus, focusProperty, windowLocalActiveFocus, focusTarget?, limitations?}`; diag `input.not_focusable`; reason `focus_rejected` |
| `Input.dispatchMouseEvent` | `type` = `mousePress`\|`mouseMove`\|`mouseRelease` (required), `button`, `buttons` string[], `point` (item coords, default center), `modifiers` | diag `input.not_mouse_dispatchable`; reasons `invalid_type`, `invalid_button`, `point_outside_viewport` |
| `Input.dragNode` | exactly one of `to` \| `delta`; `from` (default center); `steps` int = 2 (clamp 2–32); `button`; `modifiers` | Press–move…–release along the path; every waypoint is actionability-checked; `from, to, steps, itemPoints, windowPoints, eventsSent`; diag `input.not_draggable`; reason `invalid_points` |
| `Input.dispatchTouchEvent` | `type` = `touchBegin`\|`touchUpdate`\|`touchEnd`\|`touchCancel` (required); `points` [{`id` ≥0, `point` [x,y], `state?` = pressed\|updated\|stationary\|released}] (1–16, optional for touchCancel); `modifiers` | diag `input.not_touch_dispatchable`; reasons `invalid_type`, `invalid_points`, `invalid_state` |
| `Input.dispatchKeyEvent` | `key` string (QKeySequence portable name; aliases Escape/Delete/Insert/PageUp/PageDown/Enter/Spacebar accepted) or `keyCode` int; `type` = `keyClick` (default)\|`keyPress`\|`keyRelease`; `text`; `modifiers`; optional `nodeId`/`selector` (focuses target first) | `keyCode, text, type, focus?`; diag `input.not_deliverable`; reasons `invalid_key`, `invalid_type`, `focus_failed`, `unknown_window` |
| `Input.typeText` | `text` (required unless `replaceExisting`), optional `nodeId`/`selector` (focus first), `replaceExisting` bool = false (selectAll-then-type; empty text = clear), `focusSettle {timeoutMs}`, `settle` | `unitsSent, targetKind (window\|editableItem\|item), focus?, replaceExisting?, cursorSetup?`; diags `input.not_editable`, `input.window_fallback` (keys went to the window — proves nothing about a field), `input.replace_selection_failed`; reasons `invalid_text`, `focus_failed`, `not_editable`, `replace_selection_failed` |
| `Input.dismissPopup` | `all` bool = false (topmost only vs all, top-down) | Programmatic `close()` on visible Qt Quick popups + ~350 ms settle: `{dismissed, dismissedCount, popups[{type, modal, objectName?}], popupCountBefore, remainingPopupCount, method:"popup-close"}`; `reason:"no_visible_popup"`; diag `popup.not_dismissed` when the visible count did not drop |

### Render

**`Render.captureScreenshot`** — params: `windowId` int (1-based; default: first Quick window), `scale` double (0 < s ≤ 1), `region` `{x,y,width,height}` (logical/device-independent coords, clipped to the window), `includeData` bool = **false**.
Result: `{captured:true, windowId, format:"png", encoding:"base64", width, height, devicePixelRatio, originalWidth, originalHeight, scale, byteSize, region?, evidenceRole:"fallback-visual", primaryOracle:false, structuredFirst:true}` plus either `data` (base64 PNG, only with `includeData:true`) or `dataOmitted:true` + `nextHints` telling how to fetch bytes. Failures: `{captured:false, reason, windowId}` with reason ∈ `window_not_found, grab_failed, invalid_region, region_outside_window, invalid_scale, encode_failed`.

### Runtime (gated)

Both methods require `Session.configure {runtimeMutation:true}` first; otherwise they return `runtime.capability_disabled`. They are whitebox **setup** tools: results carry `mode:"whitebox"`, `verificationRole:"setup-only"` and never count as behavior verification. Base result on failure: `{ok:false, mode, verificationRole, diagnostics, target?}`.

**`Runtime.setProperty`** — params: `nodeId`|`selector`, `property` string, `value` (primitive JSON, or array/object of primitives), `settle {timeoutMs}` = 50.
Result: `{ok:true, target{nodeId, selector?}, property, before, after, sourceLocation, settle}`. Errors: `runtime.target_not_found/target_ambiguous`, `runtime.property_not_found`, `runtime.property_read_only`, `runtime.unsupported_argument_type`, `runtime.property_type_mismatch`, `runtime.invocation_failed`.

**`Runtime.invokeMethod`** — params: `nodeId`|`selector`, `method` string (name only, public methods/slots), `args` array of primitives (≤10), `settle`.
Overload chosen by arg count + convertibility; ambiguity is an error. Result: `{ok:true, target, method, returnValue, settle}`. Errors: `runtime.method_not_found`, `runtime.method_ambiguous`, `runtime.unsupported_argument_type`, `runtime.invocation_failed`, `runtime.target_not_found/target_ambiguous`.

### Source

**`Source.resolveNode`** — params: `nodeId`|`selector`.
Result: `{location, fallbackLocations[], node?, issues?}`.
`location`: `{file, line?, column?, method, confidence: 0..1, limitations[]}`. `method` values include `qqmldata-direct` (0.85–0.90), `qqmldata-delegate`, `qqmldata-dynamic` (0.65, component template — creation call site unavailable), `component-url` (0.40, no line), `cpp-object` / unknown (0.0).
`fallbackLocations`: currently at most one `objectName-source-scan` entry (0.75) — a static scan for a unique `objectName:` binding line; points at the objectName property, not necessarily the declaration.

## 5. Error & diagnostic id catalog

Diagnostics are objects `{id, severity, confidence, message, ...}` embedded in results (input/GUI failures are *results*, not JSON-RPC errors). Recovery hints, where the code attaches them, appear as `hints`, `nextHints`, `candidateSelectors`, `stableSelectorHints`, or `supportedForms`.

### session.* / runtime capability

| Id | Meaning | Recovery |
|---|---|---|
| `session.gui_thread_timeout` | GUI thread did not run (or finish) the request within the dispatch budget (+ grace). See §7 for `outcome` | Check `Log.getEntries` / target state; relaunch if unresponsive; re-verify with `UI.query` when responsive |
| `session.gui_thread_busy` | A previous over-budget request is still executing on the GUI thread; new GUI-thread requests are refused | Retry once the target is responsive; the pending request already returned `outcome:"unknown"` |
| `session.service_destroyed` | Service torn down before a queued GUI request ran | — |
| `runtime.capability_disabled` | `Runtime.*` called while mutation disabled | `Session.configure {"runtimeMutation":true}` |

### selector.* / noderef.* / batch.*

| Id | Meaning | Recovery |
|---|---|---|
| `selector.invalid` | Unsupported selector syntax | `supportedForms` + `limitations` list valid forms |
| `selector.not_found` | No node matched | up to 20 ranked `candidateSelectors` from the live tree |
| `selector.ambiguous` | Multiple matches (warning) | `stableSelectorHints`, `indexedSelectors` (globally unique `id+index` / `row+column` suggestions) |
| `noderef.invalid` | Not exactly one of `nodeId` / `selector` | — |
| `noderef.node_not_found` | `nodeId` does not resolve to a live object | re-query; ids die on reload/reset |
| `noderef.node_not_live` | Selector matched a snapshot whose object died | re-query |
| `batch.queries_required` | `UI.queryMany` without a non-empty `queries` array | — |
| `batch.too_many_queries` | > 50 queries | `hint`: split the batch |

### wait.*

`wait.selector_missing`, `wait.until_missing`, `wait.unsupported_until`, `wait.unsupported_operator`, `wait.value_missing` (invalid request → `reason:"invalid_until"`); `wait.target_ambiguous` (property predicates need exactly one match), `wait.property_not_found`, `wait.unsupported_comparison` (type mismatch for the operator).

### input.* / popup.*

Per-method failure diagnostic ids: `input.not_clickable`, `input.not_long_pressable`, `input.not_wheelable`, `input.not_mouse_dispatchable`, `input.not_draggable`, `input.not_touch_dispatchable`, `input.not_deliverable` (key), `input.not_focusable`, `input.not_editable`, `input.window_fallback` (warning), `input.replace_selection_failed`, `input.not_actionable` (Diagnostics). `popup.not_dismissed` (warning): `close()` did not reduce the visible-popup count.

Actionability failure `reason` values (also embedded as `actionabilityReasons` / issue evidence):

| Reason | Meaning | Recovery hint attached |
|---|---|---|
| `not_visible` | `visible == false` | — |
| `disabled` / `disabled_ancestor` | item or an ancestor has `enabled == false` | — |
| `opacity_zero` / `opacity_zero_ancestor` | opacity ≤ 0.01 (self / ancestor) | — |
| `zero_size` | width or height ≤ 0.5 | — |
| `unknown_window` / `no_window` | item not attached to a `QQuickWindow` | — |
| `center_outside_viewport` / `point_outside_viewport` | action point outside the window viewport | `nextHints`: `Input.scrollIntoView`, `Input.wheel` |
| `blocked_by_item` | another visible input-accepting item covers the point (center-point paint-order hit test; the control's own fill-area MouseArea is exempt) | evidence names the `blockingItem`; click it if it belongs to the same control |
| `blocked_by_modal_popup` | a visible modal popup outside the target's popup blocks it | `nextHints`: `Input.dismissPopup` |
| `not_qquickitem` | target is a plain `QObject` | — |

### layout.* / text / qml (Diagnostics issues)

`layout.overlap` (warning 0.60, center-point evidence, `blameChain`/`repairHints`), `layout.invisible_ancestor` (0.90), `layout.opacity_zero` (0.90), `layout.zero_size` (0.90), `layout.outside_viewport` (with `viewportBlameChain`), `layout.child_exceeds_parent` (0.90), `layout.excessive_spacer` (warning 0.75), `layout.text_elided` (warning 0.95), `qml.warning` (log entry promoted by `analyzeTree`).

### binding.* / runtime.*

`binding.target_not_found`, `binding.property_required`, `binding.property_not_found`; `runtime.target_not_found`, `runtime.target_ambiguous`, `runtime.property_not_found`, `runtime.property_read_only`, `runtime.property_type_mismatch`, `runtime.unsupported_argument_type`, `runtime.method_not_found`, `runtime.method_ambiguous`, `runtime.invocation_failed`.

### Launcher control channel (`qmlagent-launcher`)

| Id | Meaning |
|---|---|
| `control.invalid_json` | Mailbox request file is not valid JSON |
| `control.unknown_method` | Not one of `Session.status`, `Session.stop`, `QmlAgent.request`, `Preview.reload` |
| `session.not_ready` | Target debug session not connected/enabled yet (everything except status/stop) |
| `request.method_missing` | `QmlAgent.request` without `params.method` |
| `preview.reload_not_supported` | Session was started with `app`, not `preview` (`requiredLaunchCommand`, `nextActions` attached) |
| `preview.reload_connect_failed` / `preview.reload_timeout` / `preview.reload_invalid_response` | Preview shell socket failures |

## 6. Evidence conventions

- **`verificationRole`** states what a success proves: `"input-delivery-only"` (event reached the window — not that the app reacted), `"render-loop-settle-only"` (`settle` objects), `"post-dispatch evidence only"` (`postDispatch`), `"setup-only"` (`Runtime.*`). Screenshots carry `evidenceRole:"fallback-visual"`, `primaryOracle:false`, `structuredFirst:true`.
- **`semanticProof:false`** on every input success: verify outcomes with `UI.waitFor`/`UI.query` (the attached `nextHints` say exactly that).
- **`confidence`** (0..1) accompanies diagnostics, source locations, provenance, and repair hints; 1.0 is reserved for facts read directly from runtime state.
- **`limitations`** (string[]) enumerate known blind spots of the evidence method (e.g. "center-point paint-order approximation", "does not prove full-area occlusion"). Treat absent evidence as unknown, not false.
- **`nextHints`** are machine-followable follow-ups: `[{method, params?, reason, tool?, cli?}]` — protocol method plus optional MCP tool / `qmlagentctl` equivalents.
- **Selector `stability`**: `high` (unique authored id/objectName, nodeId), `medium` (type, unique source location, indexed delegate selectors), `low` (text, visualPath, repeated source lines). `reason` explains downgrades.
- **`settle`** objects: `{strategy:"frameSwappedOrTimeout", framesAfterAction, elapsedMs, timedOut, reason}` — a rendered frame after the action, not semantic proof.
- **`acceptsInput`** (`{ok:true, via:["acceptedButtons"|"knownInputType"|"pointerHandler", ...]}`) marks nodes that plausibly receive pointer input — the same evidence the occlusion detector uses, exposed for click-target discovery. Sparse: absent means no input evidence, not unknown. Plausibility only: it does not prove a signal handler reacts to the input.
- **Payload discipline**: 16 MiB caps both directions (§1). Screenshot bytes are omitted by default (`dataOmitted:true`); trees/queries offer `fields`, `properties`, `maxNodes`, `depth`, `collapseRepeated`, `verbosity:"summary"` projections; oversized responses come back as -32001 with hints instead of data.

## 7. GUI dispatch semantics

All UI/Input/Diagnostics/Render/Runtime/Source methods execute as a blocking call on the target's GUI thread (`Session.*` and `Log.*` do not). The debug thread waits:

```
deadline = 5000 ms (base)
         + per-method budget, clamped to [0, 120000] ms:
             UI.waitFor      -> bounded timeoutMs (max 30000)
             UI.queryMany    -> 250 ms x queries (max 50)
             Input.*         -> settle timeoutMs (+ holdMs for longPressNode,
                                + focusSettle timeoutMs for typeText)
             Runtime.*       -> settle timeoutMs
```

If the deadline passes, the result is `{ok:false, timedOut:true}` with diagnostic `session.gui_thread_timeout` and an **`outcome`** field:

- **`outcome:"not_executed"`** — the GUI thread never claimed the queued call (CAS-cancelled) or it could not be queued (`queued:false`). It provably did not and will not run.
- **`outcome:"unknown"`** — the call *started* but did not finish within the deadline plus a grace window (default **5000 ms**, override `QMLAGENT_GUI_DISPATCH_GRACE_MS`). Its effects may still land; do not treat this as proof it did not execute. Re-verify state with `UI.query` when the target responds again.

Only one GUI-thread request runs at a time; the debug thread blocks for the request's whole lifetime, so QmlAgent operations never interleave (even though implementations pump nested event loops for waits/settles). A request abandoned as `unknown` is remembered: until it finishes, every further GUI-thread request is refused immediately with `session.gui_thread_busy` (diagnostic reports the pending method and elapsed ms) instead of queuing behind it.

## Appendix: MCP tool → protocol method map

The `qmlagent` MCP server (`tools/qmlagent`) maps tools onto protocol methods:

| MCP tool | Protocol method |
|---|---|
| `qmlagent_ui_get_tree` | `UI.getTree` |
| `qmlagent_ui_query` | `UI.query` |
| `qmlagent_ui_query_many` | `UI.queryMany` |
| `qmlagent_ui_wait_for` | `UI.waitFor` |
| `qmlagent_ui_subscribe` / `qmlagent_ui_unsubscribe` | `UI.subscribe` / `UI.unsubscribe` |
| `qmlagent_diagnostics_analyze_tree` | `Diagnostics.analyzeTree` |
| `qmlagent_diagnostics_analyze_node` | `Diagnostics.analyzeNode` |
| `qmlagent_diagnostics_analyze_binding` | `Diagnostics.analyzeBinding` |
| `qmlagent_runtime_enable_mutation` | `Session.configure` (`runtimeMutation`) |
| `qmlagent_input_click` | `Input.clickNode` |
| `qmlagent_input_long_press` | `Input.longPressNode` |
| `qmlagent_input_wheel` | `Input.wheel` |
| `qmlagent_input_scroll_into_view` | `Input.scrollIntoView` |
| `qmlagent_input_focus` | `Input.focusNode` |
| `qmlagent_input_mouse` | `Input.dispatchMouseEvent` |
| `qmlagent_input_drag` | `Input.dragNode` |
| `qmlagent_input_touch` | `Input.dispatchTouchEvent` |
| `qmlagent_input_key` | `Input.dispatchKeyEvent` |
| `qmlagent_input_type_text` | `Input.typeText` |
| `qmlagent_input_clear_text` | `Input.typeText` (`text:"", replaceExisting:true`) |
| `qmlagent_input_dismiss_popup` | `Input.dismissPopup` |
| `qmlagent_runtime_set_property` | `Runtime.setProperty` |
| `qmlagent_runtime_invoke_method` | `Runtime.invokeMethod` |
| `qmlagent_log_enable` / `qmlagent_log_get_entries` | `Log.enable` / `Log.getEntries` |
| `qmlagent_render_capture_screenshot` | `Render.captureScreenshot` |
| `qmlagent_source_resolve` | `Source.resolveNode` |
| `qmlagent_launcher_stop` / `qmlagent_preview_reload` | launcher control `Session.stop` / `Preview.reload` (launcher discovery is embedded in `qmlagent_target_status`) |
| `qmlagent_workflow_click` / `workflow_click_and_wait` / `workflow_long_press_and_wait` / `workflow_key` | client-side compositions (query → input → wait → verify) of the methods above; no dedicated protocol method |
| `qmlagent_connect_tcp` / `connect_local_socket` / `disconnect` / `target_status` | MCP connection management; no protocol method |
