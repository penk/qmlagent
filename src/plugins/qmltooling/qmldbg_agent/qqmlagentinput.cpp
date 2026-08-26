// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qqmlagentinput_p.h"

#include "qqmlagentactionability_p.h"
#include "qqmlagentdiagnostics_p.h"
#include "qqmlagentinputdriver_p.h"
#include "qqmlagentuitree_p.h"

#include <QtCore/qcoreapplication.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qpointer.h>
#include <QtCore/qtimer.h>
#include <QtCore/qvector.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qkeysequence.h>
#include <QtGui/qwindow.h>
#include <QtQuick/qquickitem.h>
#include <QtQuick/qquickwindow.h>
#include <QtQuick/private/qquickflickable_p.h>
#include <QtQuickTemplates2/private/qquickcontrol_p.h>
#include <QtQuickTemplates2/private/qquickoverlay_p.h>
#include <QtQuickTemplates2/private/qquickpopup_p.h>

#include <private/qqmldebugservice_p.h>

#include <algorithm>

QT_BEGIN_NAMESPACE

static constexpr int DefaultSettleTimeoutMs = 50;
static constexpr int MaxSettleTimeoutMs = 30000;
static constexpr int DefaultLongPressHoldMs = 900;
static constexpr int MaxLongPressHoldMs = 10000;

static int boundedSettleTimeoutMs(const QJsonObject &params,
                                  const QString &settleKey = QStringLiteral("settle"))
{
    const QJsonObject settle = params.value(settleKey).toObject();
    return qBound(0, settle.value(QStringLiteral("timeoutMs")).toInt(DefaultSettleTimeoutMs),
                  MaxSettleTimeoutMs);
}

int QQmlAgentInput::dispatchBudgetMs(const QString &method, const QJsonObject &params)
{
    int budgetMs = boundedSettleTimeoutMs(params);
    if (method == QLatin1String("Input.longPressNode")) {
        budgetMs += qBound(1, params.value(QStringLiteral("holdMs")).toInt(DefaultLongPressHoldMs),
                           MaxLongPressHoldMs);
    }
    if (method == QLatin1String("Input.typeText"))
        budgetMs += boundedSettleTimeoutMs(params, QStringLiteral("focusSettle"));
    return budgetMs;
}

static QRectF itemBoxInWindow(const QQuickItem *item)
{
    if (!item)
        return {};
    return item->mapRectToScene(QRectF(QPointF(0, 0), QSizeF(item->width(), item->height())));
}

static QString actionabilityFailureMessage(const QString &reason)
{
    if (reason == QLatin1String("not_visible"))
        return QStringLiteral("Node is not visible.");
    if (reason == QLatin1String("disabled"))
        return QStringLiteral("Node is disabled.");
    if (reason == QLatin1String("disabled_ancestor"))
        return QStringLiteral("Node has a disabled ancestor.");
    if (reason == QLatin1String("opacity_zero"))
        return QStringLiteral("Node has near-zero opacity.");
    if (reason == QLatin1String("opacity_zero_ancestor"))
        return QStringLiteral("Node has an ancestor with near-zero opacity.");
    if (reason == QLatin1String("zero_size"))
        return QStringLiteral("Node has zero or near-zero size.");
    if (reason == QLatin1String("unknown_window") || reason == QLatin1String("no_window"))
        return QStringLiteral("Node is not attached to a QQuickWindow.");
    if (reason == QLatin1String("center_outside_viewport"))
        return QStringLiteral("Node center is outside the viewport.");
    if (reason == QLatin1String("blocked_by_item"))
        return QStringLiteral("Requested input point is covered by another visible item. "
                              "If the blockingItem in evidence is interactive and belongs "
                              "to the same control, click that item directly.");
    if (reason == QLatin1String("blocked_by_modal_popup"))
        return QStringLiteral("Node is blocked by a visible modal popup.");
    return {};
}

static QJsonArray actionabilityFailureHints(const QString &reason)
{
    if (reason == QLatin1String("center_outside_viewport")) {
        return {
            QJsonObject{
                { QStringLiteral("method"), QStringLiteral("Input.scrollIntoView") },
                { QStringLiteral("tool"), QStringLiteral("qmlagent_input_scroll_into_view") },
                { QStringLiteral("cli"), QStringLiteral("qmlagentctl scroll-into-view '<selector>'") },
                { QStringLiteral("reason"), QStringLiteral("Scroll instantiated clipped content into view, then retry the input or query.") },
            },
            QJsonObject{
                { QStringLiteral("method"), QStringLiteral("Input.wheel") },
                { QStringLiteral("tool"), QStringLiteral("qmlagent_input_wheel") },
                { QStringLiteral("reason"), QStringLiteral("For virtualized rows that have no node yet, wheel toward the row and re-query first.") },
            },
        };
    }
    if (reason == QLatin1String("blocked_by_modal_popup")) {
        return {
            QJsonObject{
                { QStringLiteral("method"), QStringLiteral("Input.dismissPopup") },
                { QStringLiteral("tool"), QStringLiteral("qmlagent_input_dismiss_popup") },
                { QStringLiteral("cli"), QStringLiteral("qmlagentctl dismiss-popup") },
                { QStringLiteral("reason"), QStringLiteral("Close the topmost popup, then retry the input. If the popup is the intended target, interact with its controls instead.") },
            },
        };
    }
    return {};
}

static QJsonObject failure(const QString &reason, int nodeId, const QJsonArray &evidence)
{
    const QString message = [reason]() {
        if (reason == QLatin1String("node_not_found"))
            return QStringLiteral("Node does not exist in this session.");
        if (reason == QLatin1String("not_qquickitem"))
            return QStringLiteral("Node is not a QQuickItem.");
        const QString actionabilityMessage = actionabilityFailureMessage(reason);
        if (!actionabilityMessage.isEmpty())
            return actionabilityMessage;
        return QStringLiteral("Input cannot be delivered to this node.");
    }();

    QJsonObject result{
        { QStringLiteral("delivered"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("input.not_clickable") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("nodeId"), nodeId },
            { QStringLiteral("message"), message },
            { QStringLiteral("evidence"), evidence },
        } } },
    };
    const QJsonArray nextHints = actionabilityFailureHints(reason);
    if (!nextHints.isEmpty())
        result.insert(QStringLiteral("nextHints"), nextHints);
    return result;
}

static QJsonObject longPressFailure(const QString &reason, int nodeId, const QJsonArray &evidence)
{
    const QString message = [reason]() {
        if (reason == QLatin1String("invalid_duration"))
            return QStringLiteral("Long press duration must be between 1 and 10000 milliseconds.");
        if (reason == QLatin1String("invalid_button"))
            return QStringLiteral("Long press input uses an unsupported button name.");
        if (reason == QLatin1String("node_not_found"))
            return QStringLiteral("Node does not exist in this session.");
        if (reason == QLatin1String("not_qquickitem"))
            return QStringLiteral("Node is not a QQuickItem.");
        const QString actionabilityMessage = actionabilityFailureMessage(reason);
        if (!actionabilityMessage.isEmpty())
            return actionabilityMessage;
        if (reason == QLatin1String("point_outside_viewport"))
            return QStringLiteral("Long press point is outside the viewport.");
        return QStringLiteral("Long press input cannot be delivered to this node.");
    }();

    QJsonObject result{
        { QStringLiteral("delivered"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("input.not_long_pressable") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("nodeId"), nodeId },
            { QStringLiteral("message"), message },
            { QStringLiteral("evidence"), evidence },
        } } },
    };
    const QJsonArray nextHints = actionabilityFailureHints(reason);
    if (!nextHints.isEmpty())
        result.insert(QStringLiteral("nextHints"), nextHints);
    return result;
}

static QJsonObject failureWithDiagnostics(const QString &reason, const QJsonArray &diagnostics)
{
    return {
        { QStringLiteral("delivered"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), diagnostics },
    };
}

static QJsonArray semanticVerificationHints()
{
    return {
        QJsonObject{
            { QStringLiteral("method"), QStringLiteral("UI.waitFor") },
            { QStringLiteral("reason"), QStringLiteral("Wait for the expected semantic UI state after input.") },
        },
        QJsonObject{
            { QStringLiteral("method"), QStringLiteral("UI.query") },
            { QStringLiteral("reason"), QStringLiteral("Query the changed node/property as final behavior evidence.") },
        },
        QJsonObject{
            { QStringLiteral("method"), QStringLiteral("Diagnostics.analyzeTree") },
            { QStringLiteral("reason"), QStringLiteral("Check for layout/input issues if input delivery did not produce the expected state.") },
        },
    };
}

static QJsonObject successfulInputResult(QJsonObject result)
{
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("verificationRole"), QStringLiteral("input-delivery-only"));
    result.insert(QStringLiteral("semanticProof"), false);
    result.insert(QStringLiteral("nextHints"), semanticVerificationHints());
    return result;
}

static QJsonObject inputFailureFromActionability(
        const QJsonArray &reasons, int nodeId,
        QJsonObject (*failureFactory)(const QString &, int, const QJsonArray &))
{
    if (reasons.isEmpty())
        return {};

    const QJsonObject reason = reasons.at(0).toObject();
    QString reasonId = reason.value(QStringLiteral("id")).toString(QStringLiteral("not_actionable"));
    if (reasonId == QLatin1String("no_window"))
        reasonId = QStringLiteral("unknown_window");
    return failureFactory(reasonId,
                          nodeId, reason.value(QStringLiteral("evidence")).toArray());
}

static QJsonObject deliveryWindowEvidence(const QQuickItem *item);

static QJsonObject postDispatchTargetState(int nodeId, const QPointF *windowPoint = nullptr)
{
    QJsonObject state{
        { QStringLiteral("targetLive"), false },
        { QStringLiteral("actionabilityRechecked"), false },
        { QStringLiteral("verificationRole"), QStringLiteral("post-dispatch evidence only") },
    };

    QObject *postObject = QQmlAgentUiTree::objectForNodeId(nodeId);
    if (!postObject)
        return state;

    state.insert(QStringLiteral("targetLive"), true);
    QQuickItem *postItem = qobject_cast<QQuickItem *>(postObject);
    if (!postItem || !postItem->window()) {
        if (postItem)
            state.insert(QStringLiteral("deliveryWindow"), deliveryWindowEvidence(postItem));
        return state;
    }

    state.insert(QStringLiteral("actionabilityRechecked"), true);
    state.insert(QStringLiteral("deliveryWindow"), deliveryWindowEvidence(postItem));
    // If a successful input changed the layout, the target may have moved out
    // from under the dispatch point. Rechecking actionability AT that stale
    // point reports whatever item now sits there as a blocker, which misreads
    // a successful action as "blocked_by_item". Detect the move and recheck at
    // the target's current position instead. (F-025)
    const bool pointStale = windowPoint && postItem
            && !itemBoxInWindow(postItem).contains(*windowPoint);
    if (windowPoint) {
        state.insert(QStringLiteral("point"), QJsonArray{ windowPoint->x(), windowPoint->y() });
        if (pointStale)
            state.insert(QStringLiteral("targetMovedSinceDispatch"), true);
    }
    const QJsonArray reasons = (windowPoint && !pointStale)
            ? QQmlAgentActionability::reasonsAtPoint(postObject, *windowPoint)
            : QQmlAgentActionability::reasons(postObject);
    state.insert(QStringLiteral("actionabilityReasons"), reasons);
    state.insert(QStringLiteral("actionable"), reasons.isEmpty());
    return state;
}

static QJsonObject wheelFailure(const QString &reason, int nodeId, const QJsonArray &evidence)
{
    const QString message = [reason]() {
        if (reason == QLatin1String("invalid_delta"))
            return QStringLiteral("Wheel input requires a non-zero pixelDelta or angleDelta.");
        if (reason == QLatin1String("node_not_found"))
            return QStringLiteral("Node does not exist in this session.");
        if (reason == QLatin1String("not_qquickitem"))
            return QStringLiteral("Node is not a QQuickItem.");
        const QString actionabilityMessage = actionabilityFailureMessage(reason);
        if (!actionabilityMessage.isEmpty())
            return actionabilityMessage;
        return QStringLiteral("Wheel input cannot be delivered to this node.");
    }();

    QJsonObject result{
        { QStringLiteral("delivered"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("input.not_wheelable") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("nodeId"), nodeId },
            { QStringLiteral("message"), message },
            { QStringLiteral("evidence"), evidence },
        } } },
    };
    const QJsonArray nextHints = actionabilityFailureHints(reason);
    if (!nextHints.isEmpty())
        result.insert(QStringLiteral("nextHints"), nextHints);
    return result;
}

static QJsonObject keyFailure(const QString &reason, const QJsonArray &evidence)
{
    QJsonObject result{
        { QStringLiteral("delivered"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("input.not_deliverable") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("evidence"), evidence },
        } } },
    };
    const QJsonArray nextHints = actionabilityFailureHints(reason);
    if (!nextHints.isEmpty())
        result.insert(QStringLiteral("nextHints"), nextHints);
    return result;
}

static QJsonObject mouseFailure(const QString &reason, int nodeId, const QJsonArray &evidence)
{
    const QString message = [reason]() {
        if (reason == QLatin1String("invalid_type"))
            return QStringLiteral("Mouse input type must be mousePress, mouseMove, or mouseRelease.");
        if (reason == QLatin1String("invalid_button"))
            return QStringLiteral("Mouse input uses an unsupported button name.");
        if (reason == QLatin1String("node_not_found"))
            return QStringLiteral("Node does not exist in this session.");
        if (reason == QLatin1String("not_qquickitem"))
            return QStringLiteral("Node is not a QQuickItem.");
        const QString actionabilityMessage = actionabilityFailureMessage(reason);
        if (!actionabilityMessage.isEmpty())
            return actionabilityMessage;
        if (reason == QLatin1String("point_outside_viewport"))
            return QStringLiteral("Mouse event point is outside the viewport.");
        return QStringLiteral("Mouse input cannot be delivered to this node.");
    }();

    QJsonObject result{
        { QStringLiteral("delivered"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("input.not_mouse_dispatchable") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("nodeId"), nodeId },
            { QStringLiteral("message"), message },
            { QStringLiteral("evidence"), evidence },
        } } },
    };
    const QJsonArray nextHints = actionabilityFailureHints(reason);
    if (!nextHints.isEmpty())
        result.insert(QStringLiteral("nextHints"), nextHints);
    return result;
}

static QJsonObject dragFailure(const QString &reason, int nodeId, const QJsonArray &evidence)
{
    const QString message = [reason]() {
        if (reason == QLatin1String("invalid_points"))
            return QStringLiteral("Drag input requires a target point or delta.");
        if (reason == QLatin1String("invalid_button"))
            return QStringLiteral("Drag input uses an unsupported button name.");
        if (reason == QLatin1String("node_not_found"))
            return QStringLiteral("Node does not exist in this session.");
        if (reason == QLatin1String("not_qquickitem"))
            return QStringLiteral("Node is not a QQuickItem.");
        const QString actionabilityMessage = actionabilityFailureMessage(reason);
        if (!actionabilityMessage.isEmpty())
            return actionabilityMessage;
        if (reason == QLatin1String("point_outside_viewport"))
            return QStringLiteral("One or more drag points are outside the viewport.");
        return QStringLiteral("Drag input cannot be delivered to this node.");
    }();

    return {
        { QStringLiteral("delivered"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("input.not_draggable") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("nodeId"), nodeId },
            { QStringLiteral("message"), message },
            { QStringLiteral("evidence"), evidence },
        } } },
    };
}

static QJsonObject touchFailure(const QString &reason, int nodeId, const QJsonArray &evidence)
{
    const QString message = [reason]() {
        if (reason == QLatin1String("invalid_type"))
            return QStringLiteral("Touch input type must be touchBegin, touchUpdate, touchEnd, or touchCancel.");
        if (reason == QLatin1String("invalid_points"))
            return QStringLiteral("Touch input requires one to sixteen valid touch points.");
        if (reason == QLatin1String("invalid_state"))
            return QStringLiteral("Touch point state must be pressed, updated, stationary, or released.");
        if (reason == QLatin1String("node_not_found"))
            return QStringLiteral("Node does not exist in this session.");
        if (reason == QLatin1String("not_qquickitem"))
            return QStringLiteral("Node is not a QQuickItem.");
        const QString actionabilityMessage = actionabilityFailureMessage(reason);
        if (!actionabilityMessage.isEmpty())
            return actionabilityMessage;
        if (reason == QLatin1String("point_outside_viewport"))
            return QStringLiteral("One or more touch points are outside the viewport.");
        return QStringLiteral("Touch input cannot be delivered to this node.");
    }();

    return {
        { QStringLiteral("delivered"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("input.not_touch_dispatchable") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("nodeId"), nodeId },
            { QStringLiteral("message"), message },
            { QStringLiteral("evidence"), evidence },
        } } },
    };
}

static QJsonObject focusFailure(const QString &reason, int nodeId, const QJsonArray &evidence)
{
    QString message = actionabilityFailureMessage(reason);
    if (message.isEmpty())
        message = QStringLiteral("Node cannot be focused for keyboard input.");

    return {
        { QStringLiteral("focused"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("input.not_focusable") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("nodeId"), nodeId },
            { QStringLiteral("message"), message },
            { QStringLiteral("evidence"), evidence },
        } } },
    };
}

static QJsonObject focusFailureWithDiagnostics(const QString &reason, const QJsonArray &diagnostics)
{
    return {
        { QStringLiteral("focused"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), diagnostics },
    };
}

static bool hasNodeReference(const QJsonObject &params)
{
    return (params.contains(QStringLiteral("nodeId"))
            && !params.value(QStringLiteral("nodeId")).isUndefined())
            || !params.value(QStringLiteral("selector")).toString().isEmpty();
}

static bool boolProperty(const QObject *object, const char *name, bool *value)
{
    const int index = object ? object->metaObject()->indexOfProperty(name) : -1;
    if (index < 0)
        return false;

    *value = object->property(name).toBool();
    return true;
}

static bool hasProperty(const QObject *object, const char *name)
{
    return object && object->metaObject()->indexOfProperty(name) >= 0;
}

static bool isEditableTextObject(const QObject *object)
{
    if (!hasProperty(object, "text"))
        return false;
    if (hasProperty(object, "readOnly") || hasProperty(object, "cursorPosition"))
        return true;
    return false;
}

static QQuickItem *keyboardFocusItemFor(QQuickItem *item)
{
    QQuickControl *control = qobject_cast<QQuickControl *>(item);
    QQuickItem *contentItem = control ? control->contentItem() : nullptr;
    if (contentItem && contentItem != item && isEditableTextObject(contentItem))
        return contentItem;
    return item;
}

static bool itemContainsOrIs(QQuickItem *ancestor, QQuickItem *item)
{
    return ancestor && item && (ancestor == item || ancestor->isAncestorOf(item));
}

static QJsonObject notEditableFailure(int nodeId, const QJsonObject &node)
{
    QJsonObject result{
        { QStringLiteral("delivered"), false },
        { QStringLiteral("reason"), QStringLiteral("not_editable") },
        { QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("input.not_editable") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("nodeId"), nodeId },
            { QStringLiteral("message"), QStringLiteral("Text input is read-only.") },
            { QStringLiteral("evidence"), QJsonArray{ QStringLiteral("readOnly=true") } },
        } } },
    };
    if (!node.isEmpty())
        result.insert(QStringLiteral("node"), node);
    return result;
}

static QQuickWindow *targetQuickWindow()
{
    if (QWindow *focusWindow = QGuiApplication::focusWindow()) {
        if (QQuickWindow *quickWindow = qobject_cast<QQuickWindow *>(focusWindow)) {
            if (quickWindow->contentItem() && quickWindow->width() > 0 && quickWindow->height() > 0)
                return quickWindow;
        }
    }

    QQuickWindow *found = nullptr;
    const QWindowList windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        QQuickWindow *quickWindow = qobject_cast<QQuickWindow *>(window);
        if (!quickWindow || !quickWindow->contentItem()
                || quickWindow->width() <= 0 || quickWindow->height() <= 0) {
            continue;
        }
        if (found)
            return nullptr;
        found = quickWindow;
    }

    return found;
}

static void prepareWindowForInput(QQuickWindow *window)
{
    if (!window || window->isActive())
        return;

    window->requestActivate();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

template <typename Fn>
static QJsonObject runInputAndSettle(QQuickWindow *window, const QJsonObject &params, Fn &&fn)
{
    int framesAfterAction = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    QEventLoop settleLoop;
    QPointer<QQuickWindow> guardedWindow(window);
    const int settleTimeoutMs = boundedSettleTimeoutMs(params);

    QObject::connect(window, &QQuickWindow::frameSwapped, &settleLoop, [&]() {
        ++framesAfterAction;
        settleLoop.quit();
    });

    prepareWindowForInput(window);
    fn(&elapsed);

    if (!guardedWindow) {
        const QString reason = framesAfterAction == 0
                ? QStringLiteral("target_window_destroyed_before_frame")
                : QStringLiteral("target_window_destroyed_after_frame");
        return {
            { QStringLiteral("ok"), framesAfterAction > 0 },
            { QStringLiteral("strategy"), QStringLiteral("frameSwappedOrTimeout") },
            { QStringLiteral("framesAfterAction"), framesAfterAction },
            { QStringLiteral("elapsedMs"), int(elapsed.elapsed()) },
            { QStringLiteral("timedOut"), framesAfterAction == 0 },
            { QStringLiteral("reason"), reason },
            { QStringLiteral("verificationRole"), QStringLiteral("render-loop-settle-only") },
            { QStringLiteral("semanticProof"), false },
            { QStringLiteral("targetWindowDestroyed"), true },
            { QStringLiteral("nextHints"), semanticVerificationHints() },
        };
    }

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &settleLoop, &QEventLoop::quit);
    timeout.start(settleTimeoutMs);
    settleLoop.exec(QEventLoop::ExcludeUserInputEvents);

    const QString reason = framesAfterAction == 0
            ? QStringLiteral("frame_not_observed_before_timeout")
            : QStringLiteral("frame_observed");
    return {
        { QStringLiteral("ok"), framesAfterAction > 0 },
        { QStringLiteral("strategy"), QStringLiteral("frameSwappedOrTimeout") },
        { QStringLiteral("framesAfterAction"), framesAfterAction },
        { QStringLiteral("elapsedMs"), int(elapsed.elapsed()) },
        { QStringLiteral("timedOut"), framesAfterAction == 0 },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("verificationRole"), QStringLiteral("render-loop-settle-only") },
        { QStringLiteral("semanticProof"), false },
        { QStringLiteral("nextHints"), framesAfterAction == 0 ? semanticVerificationHints() : QJsonArray() },
    };
}

static Qt::KeyboardModifiers modifiersFromParams(const QJsonObject &params)
{
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    const QJsonArray modifierArray = params.value(QStringLiteral("modifiers")).toArray();
    for (const QJsonValue &modifierValue : modifierArray) {
        const QString modifier = modifierValue.toString().toLower();
        if (modifier == QLatin1String("shift"))
            modifiers |= Qt::ShiftModifier;
        else if (modifier == QLatin1String("control") || modifier == QLatin1String("ctrl"))
            modifiers |= Qt::ControlModifier;
        else if (modifier == QLatin1String("alt"))
            modifiers |= Qt::AltModifier;
        else if (modifier == QLatin1String("meta"))
            modifiers |= Qt::MetaModifier;
    }
    return modifiers;
}

static bool keyFromParams(const QJsonObject &params, int *key,
                          Qt::KeyboardModifiers *modifiers, QString *text)
{
    *modifiers = modifiersFromParams(params);
    *text = params.value(QStringLiteral("text")).toString();

    const int keyCode = params.value(QStringLiteral("keyCode")).toInt(0);
    if (keyCode > 0) {
        *key = keyCode;
        return true;
    }

    QString keyText = params.value(QStringLiteral("key")).toString();
    if (keyText.isEmpty())
        return false;

    // Agents write common key names interchangeably with QKeySequence's
    // portable names; accept the obvious aliases instead of refusing.
    static const QHash<QString, QString> keyAliases{
        { QStringLiteral("escape"), QStringLiteral("Esc") },
        { QStringLiteral("delete"), QStringLiteral("Del") },
        { QStringLiteral("insert"), QStringLiteral("Ins") },
        { QStringLiteral("pageup"), QStringLiteral("PgUp") },
        { QStringLiteral("pagedown"), QStringLiteral("PgDown") },
        { QStringLiteral("page up"), QStringLiteral("PgUp") },
        { QStringLiteral("page down"), QStringLiteral("PgDown") },
        { QStringLiteral("return"), QStringLiteral("Return") },
        { QStringLiteral("enter"), QStringLiteral("Enter") },
        { QStringLiteral("spacebar"), QStringLiteral("Space") },
    };
    const QString alias = keyAliases.value(keyText.toLower());
    if (!alias.isEmpty())
        keyText = alias;

    const QKeySequence sequence = QKeySequence::fromString(keyText, QKeySequence::PortableText);
    if (sequence.isEmpty())
        return false;

    const QKeyCombination combination = sequence[0];
    *key = combination.key();
    *modifiers |= combination.keyboardModifiers();
    if (text->isEmpty() && keyText.size() == 1)
        *text = keyText;
    return *key > 0;
}

static bool appendNextTextUnit(const QString &text, qsizetype *index, QString *unit)
{
    if (*index >= text.size())
        return false;

    const QChar current = text.at(*index);
    if (current.isHighSurrogate() && *index + 1 < text.size()
            && text.at(*index + 1).isLowSurrogate()) {
        *unit = QString(current) + text.at(*index + 1);
        *index += 2;
        return true;
    }

    *unit = QString(current);
    ++*index;
    return true;
}

static int keyForTextUnit(const QString &unit)
{
    if (unit == QLatin1String("\n") || unit == QLatin1String("\r"))
        return Qt::Key_Return;
    if (unit == QLatin1String("\t"))
        return Qt::Key_Tab;
    if (unit == QLatin1String(" "))
        return Qt::Key_Space;

    const QKeySequence sequence = QKeySequence::fromString(unit, QKeySequence::PortableText);
    if (!sequence.isEmpty() && sequence[0].key() > 0)
        return sequence[0].key();

    return Qt::Key_unknown;
}

static QPoint pointFromParams(const QJsonObject &params, const QString &name)
{
    const QJsonValue value = params.value(name);
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        if (array.size() >= 2)
            return QPoint(array.at(0).toInt(), array.at(1).toInt());
        return {};
    }

    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        return QPoint(object.value(QStringLiteral("x")).toInt(),
                      object.value(QStringLiteral("y")).toInt());
    }

    return {};
}

static bool hasPointParam(const QJsonObject &params, const QString &name)
{
    const QJsonValue value = params.value(name);
    if (value.isArray())
        return value.toArray().size() >= 2;
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        return object.contains(QStringLiteral("x")) && object.contains(QStringLiteral("y"));
    }
    return false;
}

static QPointF pointFFromParams(const QJsonObject &params, const QString &name)
{
    const QJsonValue value = params.value(name);
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        if (array.size() >= 2)
            return QPointF(array.at(0).toDouble(), array.at(1).toDouble());
        return {};
    }

    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        return QPointF(object.value(QStringLiteral("x")).toDouble(),
                       object.value(QStringLiteral("y")).toDouble());
    }

    return {};
}

static bool pointFFromValue(const QJsonValue &value, QPointF *point)
{
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        if (array.size() < 2)
            return false;
        *point = QPointF(array.at(0).toDouble(), array.at(1).toDouble());
        return true;
    }

    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (!object.contains(QStringLiteral("x")) || !object.contains(QStringLiteral("y")))
            return false;
        *point = QPointF(object.value(QStringLiteral("x")).toDouble(),
                         object.value(QStringLiteral("y")).toDouble());
        return true;
    }

    return false;
}

static Qt::MouseButton mouseButtonFromString(const QString &name, bool *ok)
{
    const QString normalized = name.toLower();
    *ok = true;
    if (normalized.isEmpty() || normalized == QLatin1String("left"))
        return Qt::LeftButton;
    if (normalized == QLatin1String("right"))
        return Qt::RightButton;
    if (normalized == QLatin1String("middle"))
        return Qt::MiddleButton;
    if (normalized == QLatin1String("back"))
        return Qt::BackButton;
    if (normalized == QLatin1String("forward"))
        return Qt::ForwardButton;
    if (normalized == QLatin1String("none"))
        return Qt::NoButton;
    *ok = false;
    return Qt::NoButton;
}

static Qt::MouseButtons mouseButtonsFromParams(
        const QJsonObject &params, Qt::MouseButtons defaultButtons, bool *ok)
{
    *ok = true;
    if (!params.contains(QStringLiteral("buttons")))
        return defaultButtons;

    Qt::MouseButtons buttons = Qt::NoButton;
    const QJsonArray buttonArray = params.value(QStringLiteral("buttons")).toArray();
    for (const QJsonValue &buttonValue : buttonArray) {
        bool buttonOk = false;
        const Qt::MouseButton button = mouseButtonFromString(buttonValue.toString(), &buttonOk);
        if (!buttonOk) {
            *ok = false;
            return Qt::NoButton;
        }
        buttons |= button;
    }
    return buttons;
}

static QJsonArray pointArray(const QPointF &point)
{
    return { point.x(), point.y() };
}

static int quickWindowId(const QQuickWindow *targetWindow)
{
    if (!targetWindow)
        return -1;

    int windowId = 0;
    const QWindowList windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        if (qobject_cast<QQuickWindow *>(window))
            ++windowId;
        if (window == targetWindow)
            return windowId;
    }
    return -1;
}

static QQuickWindow *quickWindowForId(int requestedWindowId)
{
    if (requestedWindowId <= 0)
        return nullptr;

    int windowId = 0;
    const QWindowList windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        QQuickWindow *quickWindow = qobject_cast<QQuickWindow *>(window);
        if (!quickWindow)
            continue;
        ++windowId;
        if (windowId == requestedWindowId)
            return quickWindow;
    }
    return nullptr;
}

static QQuickWindow *requestedQuickWindow(const QJsonObject &params, int *requestedWindowId,
                                          QJsonObject *failure)
{
    *requestedWindowId = -1;
    *failure = {};
    if (!params.contains(QStringLiteral("windowId")))
        return nullptr;

    *requestedWindowId = params.value(QStringLiteral("windowId")).toInt(-1);
    if (*requestedWindowId <= 0) {
        *failure = keyFailure(QStringLiteral("invalid_window_id"),
                              { QStringLiteral("windowId must be a positive integer") });
        return nullptr;
    }

    QQuickWindow *window = quickWindowForId(*requestedWindowId);
    if (!window) {
        *failure = keyFailure(QStringLiteral("window_not_found"),
                              { QStringLiteral("no QQuickWindow has windowId=%1")
                                        .arg(*requestedWindowId) });
    }
    return window;
}

static QJsonObject keyboardWindowMismatch(int requestedWindowId, QQuickWindow *targetWindow)
{
    return keyFailure(QStringLiteral("window_mismatch"),
                      { QStringLiteral("requested windowId=%1").arg(requestedWindowId),
                        QStringLiteral("target belongs to windowId=%1")
                                .arg(quickWindowId(targetWindow)) });
}

static QJsonObject deliveryWindowEvidence(const QQuickWindow *window, const QString &source)
{
    QJsonObject evidence{
        { QStringLiteral("available"), window != nullptr },
        { QStringLiteral("source"), source },
    };
    if (!window)
        return evidence;

    evidence.insert(QStringLiteral("windowId"), quickWindowId(window));
    evidence.insert(QStringLiteral("title"), window->title());
    evidence.insert(QStringLiteral("size"), QJsonArray{ window->width(), window->height() });
    return evidence;
}

static QJsonObject deliveryWindowEvidence(const QQuickItem *item)
{
    QQuickWindow *window = item ? item->window() : nullptr;
    QJsonObject evidence = deliveryWindowEvidence(window, QStringLiteral("target-item-window"));
    evidence.insert(QStringLiteral("limitation"),
                    QStringLiteral("QmlAgent delivers to the target item's current QQuickWindow; popup and multi-window correctness must be verified with post-dispatch UI evidence."));
    return evidence;
}

static bool touchStateFromString(const QString &stateName, QEventPoint::State *state)
{
    const QString normalized = stateName.toLower();
    if (normalized == QLatin1String("pressed")) {
        *state = QEventPoint::State::Pressed;
        return true;
    }
    if (normalized == QLatin1String("updated") || normalized == QLatin1String("moved")) {
        *state = QEventPoint::State::Updated;
        return true;
    }
    if (normalized == QLatin1String("stationary")) {
        *state = QEventPoint::State::Stationary;
        return true;
    }
    if (normalized == QLatin1String("released")) {
        *state = QEventPoint::State::Released;
        return true;
    }
    return false;
}

static QEventPoint::State defaultTouchPointState(QEvent::Type type)
{
    if (type == QEvent::TouchBegin)
        return QEventPoint::State::Pressed;
    if (type == QEvent::TouchEnd)
        return QEventPoint::State::Released;
    if (type == QEvent::TouchUpdate)
        return QEventPoint::State::Updated;
    return QEventPoint::State::Stationary;
}

QJsonObject QQmlAgentInput::clickNode(const QJsonObject &params)
{
    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.issues.isEmpty())
        return failure(ref.failureReason, ref.nodeId, ref.issues);

    const int nodeId = ref.nodeId;
    QObject *object = ref.object;
    if (!object)
        return failure(QStringLiteral("node_not_found"), nodeId, { QStringLiteral("node_not_found") });
    QQuickItem *item = qobject_cast<QQuickItem *>(object);
    if (!item)
        return failure(QStringLiteral("not_qquickitem"), nodeId, { QStringLiteral("not_qquickitem") });

    QQuickWindow *window = item->window();
    if (!window)
        return failure(QStringLiteral("unknown_window"), nodeId, { QStringLiteral("window=null") });

    // An optional item-local `point` aims the click inside a wide hit area
    // instead of the center; without it the center is used and behavior is
    // unchanged. A custom point is gated on actionability AT that point, since
    // the center's blocker state does not describe an offset.
    const QRectF bbox = itemBoxInWindow(item);
    QPointF actionPoint = bbox.center();
    QJsonArray actionabilityReasons;
    if (hasPointParam(params, QStringLiteral("point"))) {
        const QPointF itemPoint = pointFFromParams(params, QStringLiteral("point"));
        const QPointF windowPoint = item->mapToScene(itemPoint);
        const QRectF viewport(QPointF(0, 0), window->size());
        if (!viewport.contains(windowPoint)) {
            return failure(QStringLiteral("point_outside_viewport"), nodeId,
                           { QStringLiteral("point=[%1,%2]").arg(windowPoint.x()).arg(windowPoint.y()),
                             QStringLiteral("viewport=[0,0,%1,%2]")
                                     .arg(viewport.width()).arg(viewport.height()) });
        }
        actionPoint = windowPoint;
        actionabilityReasons = QQmlAgentActionability::reasonsAtPoint(object, windowPoint);
    } else {
        actionabilityReasons = QQmlAgentActionability::reasons(object);
    }
    if (!actionabilityReasons.isEmpty())
        return inputFailureFromActionability(actionabilityReasons, nodeId, failure);

    const QJsonObject deliveryWindow = deliveryWindowEvidence(item);
    QPointer<QQuickWindow> guardedWindow(window);
    const QJsonObject settle = runInputAndSettle(window, params, [&](QElapsedTimer *elapsed) {
        if (!guardedWindow)
            return;
        QQmlAgentInputDriver::mouse(guardedWindow.data(), actionPoint, QEvent::MouseButtonPress,
                                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier, elapsed);
        if (!guardedWindow)
            return;
        QQmlAgentInputDriver::mouse(guardedWindow.data(), actionPoint, QEvent::MouseButtonRelease,
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier, elapsed);
    });

    return successfulInputResult({
        { QStringLiteral("delivered"), true },
        { QStringLiteral("node"), ref.node },
        { QStringLiteral("point"), QJsonArray{ actionPoint.x(), actionPoint.y() } },
        { QStringLiteral("deliveryWindow"), deliveryWindow },
        { QStringLiteral("mode"), QQmlAgentInputDriver::mode() },
        { QStringLiteral("settle"), settle },
        { QStringLiteral("postDispatch"), postDispatchTargetState(nodeId, &actionPoint) },
    });
}

QJsonObject QQmlAgentInput::longPressNode(const QJsonObject &params)
{
    const int holdMs = params.value(QStringLiteral("holdMs")).toInt(DefaultLongPressHoldMs);
    if (holdMs < 1 || holdMs > MaxLongPressHoldMs)
        return longPressFailure(QStringLiteral("invalid_duration"), -1,
                                { QStringLiteral("holdMs=%1").arg(holdMs) });

    bool buttonOk = false;
    const Qt::MouseButton button = mouseButtonFromString(
            params.value(QStringLiteral("button")).toString(QStringLiteral("left")), &buttonOk);
    if (!buttonOk || button == Qt::NoButton)
        return longPressFailure(QStringLiteral("invalid_button"), -1,
                                { QStringLiteral("button must be left, right, middle, back, or forward") });

    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.issues.isEmpty())
        return failureWithDiagnostics(ref.failureReason, ref.issues);

    const int nodeId = ref.nodeId;
    QObject *object = ref.object;
    QQuickItem *item = qobject_cast<QQuickItem *>(object);
    if (!object)
        return longPressFailure(QStringLiteral("node_not_found"), nodeId, { QStringLiteral("node_not_found") });
    if (!item)
        return longPressFailure(QStringLiteral("not_qquickitem"), nodeId, { QStringLiteral("not_qquickitem") });

    QQuickWindow *window = item->window();
    if (!window)
        return longPressFailure(QStringLiteral("unknown_window"), nodeId, { QStringLiteral("window=null") });

    const QPointF itemPoint = hasPointParam(params, QStringLiteral("point"))
            ? pointFFromParams(params, QStringLiteral("point"))
            : QPointF(item->width() / 2, item->height() / 2);
    const QPointF windowPoint = item->mapToScene(itemPoint);
    const QRectF viewport(QPointF(0, 0), window->size());
    if (!viewport.contains(windowPoint)) {
        return longPressFailure(QStringLiteral("point_outside_viewport"), nodeId,
                                { QStringLiteral("point=[%1,%2]").arg(windowPoint.x()).arg(windowPoint.y()),
                                  QStringLiteral("viewport=[0,0,%1,%2]").arg(viewport.width()).arg(viewport.height()) });
    }

    const QJsonArray actionabilityReasons =
            QQmlAgentActionability::reasonsAtPoint(object, windowPoint);
    if (!actionabilityReasons.isEmpty())
        return inputFailureFromActionability(actionabilityReasons, nodeId, longPressFailure);

    const Qt::KeyboardModifiers modifiers = modifiersFromParams(params);
    const QJsonObject deliveryWindow = deliveryWindowEvidence(item);
    QPointer<QQuickWindow> guardedWindow(window);
    int framesDuringHold = 0;
    QElapsedTimer holdElapsed;
    holdElapsed.start();
    QEventLoop holdLoop;
    QObject::connect(window, &QQuickWindow::frameSwapped, &holdLoop, [&]() {
        ++framesDuringHold;
    });

    QQmlAgentInputDriver::mouse(window, windowPoint, QEvent::MouseButtonPress, button, button,
                                modifiers, &holdElapsed);

    QTimer holdTimer;
    holdTimer.setSingleShot(true);
    QObject::connect(&holdTimer, &QTimer::timeout, &holdLoop, &QEventLoop::quit);
    holdTimer.start(holdMs);
    holdLoop.exec(QEventLoop::ExcludeUserInputEvents);

    const bool releaseSent = guardedWindow;
    const QJsonObject settle = guardedWindow
            ? runInputAndSettle(guardedWindow.data(), params, [&](QElapsedTimer *elapsed) {
                  if (!guardedWindow)
                      return;
                  QQmlAgentInputDriver::mouse(guardedWindow.data(), windowPoint,
                                              QEvent::MouseButtonRelease, button,
                                              Qt::NoButton, modifiers, elapsed);
              })
            : QJsonObject{
                  { QStringLiteral("ok"), framesDuringHold > 0 },
                  { QStringLiteral("strategy"), QStringLiteral("frameSwappedOrTimeout") },
                  { QStringLiteral("framesAfterAction"), framesDuringHold },
                  { QStringLiteral("elapsedMs"), int(holdElapsed.elapsed()) },
                  { QStringLiteral("timedOut"), framesDuringHold == 0 },
                  { QStringLiteral("reason"), framesDuringHold == 0
                            ? QStringLiteral("target_window_destroyed_before_frame")
                            : QStringLiteral("target_window_destroyed_after_frame") },
                  { QStringLiteral("verificationRole"), QStringLiteral("render-loop-settle-only") },
                  { QStringLiteral("semanticProof"), false },
                  { QStringLiteral("targetWindowDestroyed"), true },
                  { QStringLiteral("nextHints"), semanticVerificationHints() },
              };

    return successfulInputResult({
        { QStringLiteral("delivered"), true },
        { QStringLiteral("node"), ref.node },
        { QStringLiteral("point"), pointArray(windowPoint) },
        { QStringLiteral("itemPoint"), pointArray(itemPoint) },
        { QStringLiteral("deliveryWindow"), deliveryWindow },
        { QStringLiteral("button"), params.value(QStringLiteral("button")).toString(QStringLiteral("left")) },
        { QStringLiteral("holdMs"), holdMs },
        { QStringLiteral("heldElapsedMs"), int(holdElapsed.elapsed()) },
        { QStringLiteral("framesDuringHold"), framesDuringHold },
        { QStringLiteral("releaseSent"), releaseSent },
        { QStringLiteral("mode"), QQmlAgentInputDriver::mode() },
        { QStringLiteral("settle"), settle },
        { QStringLiteral("postDispatch"), postDispatchTargetState(nodeId, &windowPoint) },
    });
}

QJsonObject QQmlAgentInput::dragNode(const QJsonObject &params)
{
    const bool hasTo = hasPointParam(params, QStringLiteral("to"));
    const bool hasDelta = hasPointParam(params, QStringLiteral("delta"));
    if (hasTo == hasDelta) {
        return dragFailure(QStringLiteral("invalid_points"), -1,
                           { QStringLiteral("provide exactly one of to or delta") });
    }

    bool buttonOk = false;
    const Qt::MouseButton button = mouseButtonFromString(
            params.value(QStringLiteral("button")).toString(QStringLiteral("left")), &buttonOk);
    if (!buttonOk || button == Qt::NoButton)
        return dragFailure(QStringLiteral("invalid_button"), -1,
                           { QStringLiteral("button must be left, right, middle, back, or forward") });

    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.issues.isEmpty())
        return failureWithDiagnostics(ref.failureReason, ref.issues);

    const int nodeId = ref.nodeId;
    QObject *object = ref.object;
    QQuickItem *item = qobject_cast<QQuickItem *>(object);
    if (!object)
        return dragFailure(QStringLiteral("node_not_found"), nodeId, { QStringLiteral("node_not_found") });
    if (!item)
        return dragFailure(QStringLiteral("not_qquickitem"), nodeId, { QStringLiteral("not_qquickitem") });

    QQuickWindow *window = item->window();
    if (!window)
        return dragFailure(QStringLiteral("unknown_window"), nodeId, { QStringLiteral("window=null") });

    const QPointF from = hasPointParam(params, QStringLiteral("from"))
            ? pointFFromParams(params, QStringLiteral("from"))
            : QPointF(item->width() / 2, item->height() / 2);
    const QPointF to = hasTo ? pointFFromParams(params, QStringLiteral("to"))
                             : from + pointFFromParams(params, QStringLiteral("delta"));
    const int steps = std::clamp(params.value(QStringLiteral("steps")).toInt(2), 2, 32);

    QJsonArray itemPoints;
    QJsonArray windowPoints;
    QVector<QPointF> path;
    path.reserve(steps + 1);
    const QRectF viewport(QPointF(0, 0), window->size());
    for (int i = 0; i <= steps; ++i) {
        const qreal progress = qreal(i) / qreal(steps);
        const QPointF itemPoint = from + (to - from) * progress;
        const QPointF windowPoint = item->mapToScene(itemPoint);
        if (!viewport.contains(windowPoint)) {
            return dragFailure(QStringLiteral("point_outside_viewport"), nodeId,
                               { QStringLiteral("point=[%1,%2]").arg(windowPoint.x()).arg(windowPoint.y()),
                                 QStringLiteral("viewport=[0,0,%1,%2]").arg(viewport.width()).arg(viewport.height()) });
        }
        // Gate actionability on the press point only. Once pressed, a
        // DragHandler or MouseArea keeps the pointer grab, so the path
        // legitimately crosses other items — checking every point rejected
        // valid drags that start on a small handle (grabbers, slider thumbs,
        // splitters) and travel past it.
        if (i == 0) {
            const QJsonArray actionabilityReasons =
                    QQmlAgentActionability::reasonsAtPoint(object, windowPoint);
            if (!actionabilityReasons.isEmpty())
                return inputFailureFromActionability(actionabilityReasons, nodeId, dragFailure);
        }
        itemPoints.append(pointArray(itemPoint));
        windowPoints.append(pointArray(windowPoint));
        path.append(windowPoint);
    }

    const Qt::KeyboardModifiers modifiers = modifiersFromParams(params);
    const QJsonObject deliveryWindow = deliveryWindowEvidence(item);
    QPointer<QQuickWindow> guardedWindow(window);
    int eventsSent = 0;
    const QJsonObject settle = runInputAndSettle(window, params, [&](QElapsedTimer *elapsed) {
        if (!guardedWindow)
            return;
        QQmlAgentInputDriver::mouse(guardedWindow.data(), path.first(), QEvent::MouseButtonPress,
                                    button, button, modifiers, elapsed);
        ++eventsSent;
        for (int i = 1; i < path.size(); ++i) {
            if (!guardedWindow)
                return;
            QQmlAgentInputDriver::mouse(guardedWindow.data(), path.at(i), QEvent::MouseMove, Qt::NoButton,
                                        button, modifiers, elapsed);
            ++eventsSent;
        }
        if (!guardedWindow)
            return;
        QQmlAgentInputDriver::mouse(guardedWindow.data(), path.last(), QEvent::MouseButtonRelease,
                                    button, Qt::NoButton, modifiers, elapsed);
        ++eventsSent;
    });
    const QPointF finalWindowPoint = path.constLast();

    return successfulInputResult({
        { QStringLiteral("delivered"), true },
        { QStringLiteral("node"), ref.node },
        { QStringLiteral("button"), params.value(QStringLiteral("button")).toString(QStringLiteral("left")) },
        { QStringLiteral("from"), pointArray(from) },
        { QStringLiteral("to"), pointArray(to) },
        { QStringLiteral("steps"), steps },
        { QStringLiteral("itemPoints"), itemPoints },
        { QStringLiteral("windowPoints"), windowPoints },
        { QStringLiteral("deliveryWindow"), deliveryWindow },
        { QStringLiteral("eventsSent"), eventsSent },
        { QStringLiteral("mode"), QQmlAgentInputDriver::mode() },
        { QStringLiteral("settle"), settle },
        { QStringLiteral("postDispatch"), postDispatchTargetState(nodeId, &finalWindowPoint) },
    });
}

QJsonObject QQmlAgentInput::dispatchMouseEvent(const QJsonObject &params)
{
    const QString typeName = params.value(QStringLiteral("type")).toString();
    QEvent::Type eventType = QEvent::None;
    Qt::MouseButtons defaultButtons = Qt::NoButton;
    if (typeName == QLatin1String("mousePress")) {
        eventType = QEvent::MouseButtonPress;
        defaultButtons = Qt::LeftButton;
    } else if (typeName == QLatin1String("mouseMove")) {
        eventType = QEvent::MouseMove;
        defaultButtons = Qt::LeftButton;
    } else if (typeName == QLatin1String("mouseRelease")) {
        eventType = QEvent::MouseButtonRelease;
    } else {
        return mouseFailure(QStringLiteral("invalid_type"), -1,
                            { QStringLiteral("type must be mousePress, mouseMove, or mouseRelease") });
    }

    bool buttonOk = false;
    Qt::MouseButton button = mouseButtonFromString(
            params.value(QStringLiteral("button")).toString(QStringLiteral("left")), &buttonOk);
    if (!buttonOk)
        return mouseFailure(QStringLiteral("invalid_button"), -1,
                            { QStringLiteral("button must be left, right, middle, back, forward, or none") });
    if (eventType == QEvent::MouseMove)
        button = Qt::NoButton;

    bool buttonsOk = false;
    const Qt::MouseButtons buttons = mouseButtonsFromParams(params, defaultButtons, &buttonsOk);
    if (!buttonsOk)
        return mouseFailure(QStringLiteral("invalid_button"), -1,
                            { QStringLiteral("buttons must contain left, right, middle, back, forward, or none") });

    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.issues.isEmpty())
        return failureWithDiagnostics(ref.failureReason, ref.issues);

    const int nodeId = ref.nodeId;
    QObject *object = ref.object;
    QQuickItem *item = qobject_cast<QQuickItem *>(object);
    if (!object)
        return mouseFailure(QStringLiteral("node_not_found"), nodeId, { QStringLiteral("node_not_found") });
    if (!item)
        return mouseFailure(QStringLiteral("not_qquickitem"), nodeId, { QStringLiteral("not_qquickitem") });

    QQuickWindow *window = item->window();
    if (!window)
        return mouseFailure(QStringLiteral("unknown_window"), nodeId, { QStringLiteral("window=null") });

    const QPointF itemPoint = hasPointParam(params, QStringLiteral("point"))
            ? pointFFromParams(params, QStringLiteral("point"))
            : QPointF(item->width() / 2, item->height() / 2);
    const QPointF windowPoint = item->mapToScene(itemPoint);
    const QRectF viewport(QPointF(0, 0), window->size());
    if (!viewport.contains(windowPoint)) {
        return mouseFailure(QStringLiteral("point_outside_viewport"), nodeId,
                            { QStringLiteral("point=[%1,%2]").arg(windowPoint.x()).arg(windowPoint.y()),
                              QStringLiteral("viewport=[0,0,%1,%2]").arg(viewport.width()).arg(viewport.height()) });
    }
    const QJsonArray actionabilityReasons =
            QQmlAgentActionability::reasonsAtPoint(object, windowPoint);
    if (!actionabilityReasons.isEmpty())
        return inputFailureFromActionability(actionabilityReasons, nodeId, mouseFailure);

    const Qt::KeyboardModifiers modifiers = modifiersFromParams(params);
    const QJsonObject deliveryWindow = deliveryWindowEvidence(item);
    QPointer<QQuickWindow> guardedWindow(window);
    const QJsonObject settle = runInputAndSettle(window, params, [&](QElapsedTimer *elapsed) {
        if (!guardedWindow)
            return;
        QQmlAgentInputDriver::mouse(guardedWindow.data(), windowPoint, eventType, button, buttons,
                                    modifiers, elapsed);
    });

    return successfulInputResult({
        { QStringLiteral("delivered"), true },
        { QStringLiteral("node"), ref.node },
        { QStringLiteral("type"), typeName },
        { QStringLiteral("point"), QJsonArray{ windowPoint.x(), windowPoint.y() } },
        { QStringLiteral("itemPoint"), QJsonArray{ itemPoint.x(), itemPoint.y() } },
        { QStringLiteral("deliveryWindow"), deliveryWindow },
        { QStringLiteral("mode"), QQmlAgentInputDriver::mode() },
        { QStringLiteral("settle"), settle },
        { QStringLiteral("postDispatch"), postDispatchTargetState(nodeId, &windowPoint) },
    });
}

QJsonObject QQmlAgentInput::dispatchTouchEvent(const QJsonObject &params)
{
    const QString typeName = params.value(QStringLiteral("type")).toString();
    QEvent::Type eventType = QEvent::None;
    if (typeName == QLatin1String("touchBegin"))
        eventType = QEvent::TouchBegin;
    else if (typeName == QLatin1String("touchUpdate"))
        eventType = QEvent::TouchUpdate;
    else if (typeName == QLatin1String("touchEnd"))
        eventType = QEvent::TouchEnd;
    else if (typeName == QLatin1String("touchCancel"))
        eventType = QEvent::TouchCancel;
    else
        return touchFailure(QStringLiteral("invalid_type"), -1,
                            { QStringLiteral("type must be touchBegin, touchUpdate, touchEnd, or touchCancel") });

    const QJsonArray pointParams = params.value(QStringLiteral("points")).toArray();
    if (eventType != QEvent::TouchCancel && (pointParams.isEmpty() || pointParams.size() > 16))
        return touchFailure(QStringLiteral("invalid_points"), -1,
                            { QStringLiteral("points must contain 1..16 entries") });

    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.issues.isEmpty())
        return failureWithDiagnostics(ref.failureReason, ref.issues);

    const int nodeId = ref.nodeId;
    QObject *object = ref.object;
    QQuickItem *item = qobject_cast<QQuickItem *>(object);
    if (!object)
        return touchFailure(QStringLiteral("node_not_found"), nodeId, { QStringLiteral("node_not_found") });
    if (!item)
        return touchFailure(QStringLiteral("not_qquickitem"), nodeId, { QStringLiteral("not_qquickitem") });

    QQuickWindow *window = item->window();
    if (!window)
        return touchFailure(QStringLiteral("unknown_window"), nodeId, { QStringLiteral("window=null") });

    const QRectF viewport(QPointF(0, 0), window->size());
    QList<QQmlAgentInputDriver::TouchPoint> eventPoints;
    QJsonArray points;
    for (const QJsonValue &pointValue : pointParams) {
        if (!pointValue.isObject())
            return touchFailure(QStringLiteral("invalid_points"), nodeId,
                                { QStringLiteral("each point must be an object") });

        const QJsonObject pointObject = pointValue.toObject();
        const int pointId = pointObject.value(QStringLiteral("id")).toInt(-1);
        if (pointId < 0)
            return touchFailure(QStringLiteral("invalid_points"), nodeId,
                                { QStringLiteral("each point requires id >= 0") });

        QPointF itemPoint;
        if (!pointFFromValue(pointObject.value(QStringLiteral("point")), &itemPoint)) {
            return touchFailure(QStringLiteral("invalid_points"), nodeId,
                                { QStringLiteral("each point requires point:[x,y]") });
        }

        QEventPoint::State state = defaultTouchPointState(eventType);
        const QString stateName = pointObject.value(QStringLiteral("state")).toString();
        if (!stateName.isEmpty() && !touchStateFromString(stateName, &state))
            return touchFailure(QStringLiteral("invalid_state"), nodeId,
                                { QStringLiteral("state must be pressed, updated, stationary, or released") });

        const QPointF windowPoint = item->mapToScene(itemPoint);
        if (!viewport.contains(windowPoint)) {
            return touchFailure(QStringLiteral("point_outside_viewport"), nodeId,
                                { QStringLiteral("point=[%1,%2]").arg(windowPoint.x()).arg(windowPoint.y()),
                                  QStringLiteral("viewport=[0,0,%1,%2]").arg(viewport.width()).arg(viewport.height()) });
        }
        const QJsonArray actionabilityReasons =
                QQmlAgentActionability::reasonsAtPoint(object, windowPoint);
        if (!actionabilityReasons.isEmpty())
            return inputFailureFromActionability(actionabilityReasons, nodeId, touchFailure);

        const QPointF globalPoint = window->mapToGlobal(windowPoint.toPoint());
        eventPoints.append(QQmlAgentInputDriver::TouchPoint{
            pointId,
            state,
            windowPoint,
            globalPoint,
        });
        points.append(QJsonObject{
            { QStringLiteral("id"), pointId },
            { QStringLiteral("state"), stateName.isEmpty()
                    ? (state == QEventPoint::State::Pressed ? QStringLiteral("pressed")
                       : state == QEventPoint::State::Updated ? QStringLiteral("updated")
                       : state == QEventPoint::State::Released ? QStringLiteral("released")
                       : QStringLiteral("stationary"))
                    : stateName },
            { QStringLiteral("itemPoint"), pointArray(itemPoint) },
            { QStringLiteral("windowPoint"), pointArray(windowPoint) },
        });
    }

    const Qt::KeyboardModifiers modifiers = modifiersFromParams(params);
    const QJsonObject deliveryWindow = deliveryWindowEvidence(item);
    QPointer<QQuickWindow> guardedWindow(window);
    const QJsonObject settle = runInputAndSettle(window, params, [&](QElapsedTimer *) {
        if (!guardedWindow)
            return;
        QQmlAgentInputDriver::touch(guardedWindow.data(), eventType, eventPoints, modifiers);
    });
    const QPointF postPoint = eventPoints.isEmpty() ? QPointF() : eventPoints.constLast().windowPoint;

    return successfulInputResult({
        { QStringLiteral("delivered"), true },
        { QStringLiteral("node"), ref.node },
        { QStringLiteral("type"), typeName },
        { QStringLiteral("points"), points },
        { QStringLiteral("deliveryWindow"), deliveryWindow },
        { QStringLiteral("mode"), QQmlAgentInputDriver::mode() },
        { QStringLiteral("settle"), settle },
        { QStringLiteral("postDispatch"), eventPoints.isEmpty()
                  ? postDispatchTargetState(nodeId)
                  : postDispatchTargetState(nodeId, &postPoint) },
    });
}

QJsonObject QQmlAgentInput::wheel(const QJsonObject &params)
{
    const QPoint pixelDelta = pointFromParams(params, QStringLiteral("pixelDelta"));
    QPoint angleDelta = pointFromParams(params, QStringLiteral("angleDelta"));
    if (params.contains(QStringLiteral("deltaX")) || params.contains(QStringLiteral("deltaY"))) {
        angleDelta = QPoint(params.value(QStringLiteral("deltaX")).toInt(0),
                            params.value(QStringLiteral("deltaY")).toInt(0));
    }
    if (pixelDelta.isNull() && angleDelta.isNull())
        return wheelFailure(QStringLiteral("invalid_delta"), -1,
                            { QStringLiteral("provide pixelDelta, angleDelta, deltaX, or deltaY") });

    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.issues.isEmpty())
        return failureWithDiagnostics(ref.failureReason, ref.issues);

    const int nodeId = ref.nodeId;
    QObject *object = ref.object;
    if (!object)
        return wheelFailure(QStringLiteral("node_not_found"), nodeId, { QStringLiteral("node_not_found") });
    const QJsonArray actionabilityReasons = QQmlAgentActionability::reasons(object);
    if (!actionabilityReasons.isEmpty())
        return inputFailureFromActionability(actionabilityReasons, nodeId, wheelFailure);

    QQuickItem *item = qobject_cast<QQuickItem *>(object);
    if (!item)
        return wheelFailure(QStringLiteral("not_qquickitem"), nodeId, { QStringLiteral("not_qquickitem") });

    QQuickWindow *window = item->window();
    if (!window)
        return wheelFailure(QStringLiteral("unknown_window"), nodeId, { QStringLiteral("window=null") });

    const QRectF bbox = itemBoxInWindow(item);
    const QPointF center = bbox.center();
    const QRectF viewport(QPointF(0, 0), window->size());
    if (!viewport.contains(center)) {
        return wheelFailure(QStringLiteral("center_outside_viewport"), nodeId,
                            { QStringLiteral("bbox=[%1,%2,%3,%4]").arg(bbox.x()).arg(bbox.y()).arg(bbox.width()).arg(bbox.height()),
                              QStringLiteral("center=[%1,%2]").arg(center.x()).arg(center.y()),
                              QStringLiteral("viewport=[0,0,%1,%2]").arg(viewport.width()).arg(viewport.height()) });
    }

    const Qt::KeyboardModifiers modifiers = modifiersFromParams(params);
    const QJsonObject deliveryWindow = deliveryWindowEvidence(item);
    QPointer<QQuickWindow> guardedWindow(window);
    int eventsSent = 0;
    const QJsonObject settle = runInputAndSettle(window, params, [&](QElapsedTimer *elapsed) {
        if (!guardedWindow)
            return;
        QQmlAgentInputDriver::mouse(guardedWindow.data(), center, QEvent::MouseMove, Qt::NoButton,
                                    Qt::NoButton, modifiers, elapsed);
        ++eventsSent;
        if (!guardedWindow)
            return;
        QQmlAgentInputDriver::wheel(guardedWindow.data(), center, pixelDelta, angleDelta, modifiers);
        ++eventsSent;
    });

    return successfulInputResult({
        { QStringLiteral("delivered"), true },
        { QStringLiteral("node"), ref.node },
        { QStringLiteral("point"), QJsonArray{ center.x(), center.y() } },
        { QStringLiteral("pixelDelta"), QJsonArray{ pixelDelta.x(), pixelDelta.y() } },
        { QStringLiteral("angleDelta"), QJsonArray{ angleDelta.x(), angleDelta.y() } },
        { QStringLiteral("deliveryWindow"), deliveryWindow },
        { QStringLiteral("eventsSent"), eventsSent },
        { QStringLiteral("mode"), QQmlAgentInputDriver::mode() },
        { QStringLiteral("settle"), settle },
        { QStringLiteral("postDispatch"), postDispatchTargetState(nodeId, &center) },
    });
}

QJsonObject QQmlAgentInput::scrollIntoView(const QJsonObject &params)
{
    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.issues.isEmpty())
        return failureWithDiagnostics(ref.failureReason, ref.issues);

    const int nodeId = ref.nodeId;
    QQuickItem *item = qobject_cast<QQuickItem *>(ref.object);
    if (!ref.object)
        return failure(QStringLiteral("node_not_found"), nodeId, { QStringLiteral("node_not_found") });
    if (!item)
        return failure(QStringLiteral("not_qquickitem"), nodeId, { QStringLiteral("not_qquickitem") });

    QQuickWindow *window = item->window();
    if (!window)
        return failure(QStringLiteral("unknown_window"), nodeId, { QStringLiteral("window=null") });

    // Walk ancestor flickables nearest-first and adjust their content
    // position so the item's center lands inside each viewport. This is
    // deterministic navigation for instantiated-but-clipped content; rows a
    // view has not created yet have no node to resolve, so they still need
    // wheel scrolling or view-side positioning first.
    QJsonArray adjustments;
    const QJsonObject settle = runInputAndSettle(window, params, [&](QElapsedTimer *) {
        for (QQuickItem *ancestor = item->parentItem(); ancestor;
                ancestor = ancestor->parentItem()) {
            QQuickFlickable *flickable = qobject_cast<QQuickFlickable *>(ancestor);
            if (!flickable || !flickable->contentItem())
                continue;

            const QPointF itemInContent =
                    item->mapToItem(flickable->contentItem(), QPointF(0, 0));
            QJsonObject adjustment{
                { QStringLiteral("flickableNodeId"),
                  QQmlDebugService::idForObject(flickable) },
            };

            // Valid content positions start at origin, not zero: ListView
            // shifts originY as delegates with estimated sizes come and go,
            // so clamping to [0, extent] scrolls long dynamic lists wrong.
            const qreal minY = flickable->originY();
            const qreal maxY = minY + qMax<qreal>(0, flickable->contentHeight()
                                                     - flickable->height());
            if (maxY > minY) {
                const qreal targetY = qBound<qreal>(
                        minY,
                        itemInContent.y() - (flickable->height() - item->height()) / 2,
                        maxY);
                adjustment.insert(QStringLiteral("contentYBefore"), flickable->contentY());
                flickable->setContentY(targetY);
                adjustment.insert(QStringLiteral("contentYAfter"), flickable->contentY());
            }
            const qreal minX = flickable->originX();
            const qreal maxX = minX + qMax<qreal>(0, flickable->contentWidth()
                                                     - flickable->width());
            if (maxX > minX) {
                const qreal targetX = qBound<qreal>(
                        minX,
                        itemInContent.x() - (flickable->width() - item->width()) / 2,
                        maxX);
                adjustment.insert(QStringLiteral("contentXBefore"), flickable->contentX());
                flickable->setContentX(targetX);
                adjustment.insert(QStringLiteral("contentXAfter"), flickable->contentX());
            }
            if (adjustment.size() > 1)
                adjustments.append(adjustment);
        }
    });

    // Final-position evidence measured after settle: snapping views can
    // revert a programmatic content move, so callers must see where the
    // item actually ended up rather than trusting the adjustment.
    bool insideViewportAfter = false;
    if (item->window()) {
        const QRectF box = item->mapRectToScene(
                QRectF(QPointF(0, 0), QSizeF(item->width(), item->height())));
        const QRectF viewport(QPointF(0, 0), item->window()->size());
        insideViewportAfter = viewport.intersects(box)
                && viewport.contains(box.center());
    }

    QJsonObject result{
        { QStringLiteral("delivered"), true },
        { QStringLiteral("scrolled"), !adjustments.isEmpty() },
        { QStringLiteral("insideViewportAfter"), insideViewportAfter },
        { QStringLiteral("adjustments"), adjustments },
        { QStringLiteral("node"), ref.node },
        { QStringLiteral("settle"), settle },
        { QStringLiteral("postDispatch"), postDispatchTargetState(nodeId) },
    };
    if (adjustments.isEmpty())
        result.insert(QStringLiteral("reason"), QStringLiteral("no_scrollable_ancestor"));
    else if (!insideViewportAfter)
        result.insert(QStringLiteral("reason"), QStringLiteral("target_still_outside_viewport"));
    return result;
}

QJsonObject QQmlAgentInput::typeText(const QJsonObject &params)
{
    const QString text = params.value(QStringLiteral("text")).toString();
    const bool replaceExisting = params.value(QStringLiteral("replaceExisting")).toBool(false);
    if (text.isEmpty() && !replaceExisting)
        return keyFailure(QStringLiteral("invalid_text"),
                          { QStringLiteral("text must be a non-empty string") });

    int requestedWindowId = -1;
    QJsonObject windowFailure;
    QQuickWindow *requestedWindow = requestedQuickWindow(params, &requestedWindowId,
                                                         &windowFailure);
    if (!windowFailure.isEmpty())
        return windowFailure;

    const bool hasNodeId = params.contains(QStringLiteral("nodeId"))
            && !params.value(QStringLiteral("nodeId")).isUndefined();
    const bool hasSelector = !params.value(QStringLiteral("selector")).toString().isEmpty();
    QJsonObject focusResult;
    QQuickWindow *inputWindow = nullptr;
    int targetNodeId = -1;
    QQuickItem *targetItem = nullptr;
    QQuickItem *targetKeyItem = nullptr;
    if (hasNodeId || hasSelector) {
        const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
        if (!ref.issues.isEmpty())
            return failureWithDiagnostics(ref.failureReason, ref.issues);
        targetNodeId = ref.nodeId;

        QQuickItem *resolvedItem = qobject_cast<QQuickItem *>(ref.object);
        QQuickWindow *resolvedWindow = resolvedItem ? resolvedItem->window() : nullptr;
        if (requestedWindow && resolvedWindow && requestedWindow != resolvedWindow)
            return keyboardWindowMismatch(requestedWindowId, resolvedWindow);

        QJsonObject focusParams{
            { QStringLiteral("settle"), params.value(QStringLiteral("focusSettle")).toObject() },
        };
        if (hasNodeId)
            focusParams.insert(QStringLiteral("nodeId"), ref.nodeId);
        else
            focusParams.insert(QStringLiteral("selector"),
                               params.value(QStringLiteral("selector")).toString());

        auto focusFailureHints = [&](const QQmlAgentUiTree::NodeRef &targetRef) {
            QJsonArray hints{
                QJsonObject{
                    { QStringLiteral("method"), QStringLiteral("Input.focusNode") },
                    { QStringLiteral("params"), QJsonObject{
                        { QStringLiteral("nodeId"), targetRef.nodeId },
                    } },
                    { QStringLiteral("reason"), QStringLiteral("Focus this text target explicitly, then retry Input.typeText without relying on click-to-focus.") },
                },
            };
            if (hasSelector) {
                hints.append(QJsonObject{
                    { QStringLiteral("method"), QStringLiteral("Input.focusNode") },
                    { QStringLiteral("params"), QJsonObject{
                        { QStringLiteral("selector"),
                          params.value(QStringLiteral("selector")).toString() },
                    } },
                    { QStringLiteral("reason"), QStringLiteral("Selector-first focus retry for agents that avoid session-local node ids across relaunches.") },
                });
            }
            return hints;
        };

        focusResult = focusNode(focusParams);
        if (!focusResult.value(QStringLiteral("focused")).toBool()) {
            QJsonObject result = keyFailure(QStringLiteral("focus_failed"),
                                            { QStringLiteral("Input.focusNode did not focus target") });
            result.insert(QStringLiteral("focus"), focusResult);
            result.insert(QStringLiteral("nextHints"), focusFailureHints(ref));
            return result;
        }

        bool readOnly = false;
        if (boolProperty(ref.object, "readOnly", &readOnly) && readOnly)
            return notEditableFailure(ref.nodeId, ref.node);

        targetItem = qobject_cast<QQuickItem *>(ref.object);
        inputWindow = targetItem ? targetItem->window() : nullptr;
        targetKeyItem = targetItem ? keyboardFocusItemFor(targetItem) : nullptr;
        QQuickItem *activeFocusItem = inputWindow ? inputWindow->activeFocusItem() : nullptr;
        if (activeFocusItem && !itemContainsOrIs(targetItem, activeFocusItem)) {
            QJsonObject result = keyFailure(QStringLiteral("focus_failed"),
                                            { QStringLiteral("target did not receive active focus after click"),
                                              QStringLiteral("activeFocusItem=%1")
                                                      .arg(activeFocusItem
                                                           ? QString::fromUtf8(activeFocusItem->metaObject()->className())
                                                           : QStringLiteral("<none>")) });
            result.insert(QStringLiteral("focus"), focusResult);
            result.insert(QStringLiteral("nextHints"), focusFailureHints(ref));
            return result;
        }
    }

    QQuickWindow *window = inputWindow ? inputWindow
                                       : requestedWindow ? requestedWindow : targetQuickWindow();
    if (!window) {
        return keyFailure(QStringLiteral("unknown_window"),
                          { QStringLiteral("no focused or unique nonzero QQuickWindow with contentItem") });
    }

    int unitsSent = 0;
    QJsonObject replacement;
    bool movedCursorToEnd = false;
    QString targetKind;
    QPointer<QQuickWindow> guardedWindow(window);
    const QJsonObject settle = runInputAndSettle(window, params, [&](QElapsedTimer *) {
        if (!guardedWindow)
            return;
        QQuickItem *activeFocusItem = guardedWindow->activeFocusItem();
        QObject *keyTarget = targetKeyItem
                ? (itemContainsOrIs(targetItem, activeFocusItem)
                           ? static_cast<QObject *>(activeFocusItem)
                           : static_cast<QObject *>(targetKeyItem))
                : activeFocusItem ? static_cast<QObject *>(activeFocusItem)
                                  : static_cast<QObject *>(guardedWindow.data());
        if (keyTarget == static_cast<QObject *>(guardedWindow.data()))
            targetKind = QStringLiteral("window");
        else if (isEditableTextObject(keyTarget))
            targetKind = QStringLiteral("editableItem");
        else
            targetKind = QStringLiteral("item");
        QPointer<QObject> guardedKeyTarget(keyTarget);
        if (replaceExisting) {
            QObject *selectionTarget = activeFocusItem ? static_cast<QObject *>(activeFocusItem)
                                                       : keyTarget;
            const bool selected = selectionTarget
                    && QMetaObject::invokeMethod(selectionTarget, "selectAll",
                                                 Qt::DirectConnection);
            replacement = {
                { QStringLiteral("requested"), true },
                { QStringLiteral("mode"), QStringLiteral("selectAll-then-type") },
                { QStringLiteral("selectionMethod"), QStringLiteral("Q_INVOKABLE selectAll") },
                { QStringLiteral("selectionApplied"), selected },
            };
            if (!selected)
                return;
            if (text.isEmpty()) {
                if (!guardedKeyTarget)
                    return;
                QQmlAgentInputDriver::key(guardedKeyTarget.data(), QEvent::KeyPress, Qt::Key_Backspace,
                                          Qt::NoModifier, {});
                if (!guardedKeyTarget)
                    return;
                QQmlAgentInputDriver::key(guardedKeyTarget.data(), QEvent::KeyRelease, Qt::Key_Backspace,
                                          Qt::NoModifier, {});
                return;
            }
        } else if (targetKeyItem && isEditableTextObject(keyTarget)) {
            QQmlAgentInputDriver::key(guardedKeyTarget.data(), QEvent::KeyPress, Qt::Key_End,
                                      Qt::NoModifier, {});
            if (!guardedKeyTarget)
                return;
            QQmlAgentInputDriver::key(guardedKeyTarget.data(), QEvent::KeyRelease, Qt::Key_End,
                                      Qt::NoModifier, {});
            movedCursorToEnd = true;
        }
        QString unit;
        qsizetype index = 0;
        while (appendNextTextUnit(text, &index, &unit)) {
            if (!guardedKeyTarget)
                return;
            if (keyTarget != static_cast<QObject *>(guardedWindow.data())
                    && isEditableTextObject(keyTarget)) {
                QQmlAgentInputDriver::inputText(guardedKeyTarget.data(), unit);
            } else {
                const int key = keyForTextUnit(unit);
                QQmlAgentInputDriver::key(guardedKeyTarget.data(), QEvent::KeyPress, key,
                                          Qt::NoModifier, unit);
                if (!guardedKeyTarget)
                    return;
                QQmlAgentInputDriver::key(guardedKeyTarget.data(), QEvent::KeyRelease, key,
                                          Qt::NoModifier, unit);
            }
            ++unitsSent;
        }
    });

    QJsonObject result{
        { QStringLiteral("delivered"), !replaceExisting
                  || replacement.value(QStringLiteral("selectionApplied")).toBool(false) },
        { QStringLiteral("text"), text },
        { QStringLiteral("unitsSent"), unitsSent },
        { QStringLiteral("mode"), QQmlAgentInputDriver::mode() },
        { QStringLiteral("settle"), settle },
    };
    if (!targetKind.isEmpty())
        result.insert(QStringLiteral("targetKind"), targetKind);
    if (targetKind == QLatin1String("window")) {
        // Keys went to the window because no editable target was resolved;
        // delivery is honest but proves nothing about any text field.
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("input.window_fallback") },
            { QStringLiteral("severity"), QStringLiteral("warning") },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("message"),
              QStringLiteral("No editable target received the key events; they were delivered to the window. Target a text item or verify the text state explicitly.") },
        } });
    }
    result.insert(QStringLiteral("deliveryWindow"),
                  targetItem
                          ? deliveryWindowEvidence(targetItem)
                          : deliveryWindowEvidence(
                                    window, requestedWindow
                                            ? QStringLiteral("requested-window-id")
                                            : QStringLiteral("focused-or-unique-window")));
    if (targetNodeId > 0)
        result.insert(QStringLiteral("postDispatch"), postDispatchTargetState(targetNodeId));
    if (replaceExisting) {
        result.insert(QStringLiteral("replaceExisting"), replacement);
        if (!replacement.value(QStringLiteral("selectionApplied")).toBool(false)) {
            result.insert(QStringLiteral("reason"), QStringLiteral("replace_selection_failed"));
            result.insert(QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
                { QStringLiteral("id"), QStringLiteral("input.replace_selection_failed") },
                { QStringLiteral("severity"), QStringLiteral("error") },
                { QStringLiteral("confidence"), 1.0 },
                { QStringLiteral("message"),
                  QStringLiteral("Focused text item does not expose selectAll().") },
            } });
        }
    }
    if (movedCursorToEnd) {
        result.insert(QStringLiteral("cursorSetup"), QJsonObject{
            { QStringLiteral("method"), QStringLiteral("Key_End") },
            { QStringLiteral("reason"), QStringLiteral("deterministic targeted typeText insertion point") },
        });
    }
    if (!focusResult.isEmpty())
        result.insert(QStringLiteral("focus"), focusResult);
    return result;
}

QJsonObject QQmlAgentInput::focusNode(const QJsonObject &params)
{
    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.issues.isEmpty())
        return focusFailureWithDiagnostics(ref.failureReason, ref.issues);

    const int nodeId = ref.nodeId;
    QObject *object = ref.object;
    QQuickItem *item = qobject_cast<QQuickItem *>(object);
    if (!object)
        return focusFailure(QStringLiteral("node_not_found"), nodeId, { QStringLiteral("node_not_found") });
    const QJsonArray actionabilityReasons = QQmlAgentActionability::reasons(object);
    if (!actionabilityReasons.isEmpty())
        return inputFailureFromActionability(actionabilityReasons, nodeId, focusFailure);

    if (!item)
        return focusFailure(QStringLiteral("not_qquickitem"), nodeId, { QStringLiteral("not_qquickitem") });

    QQuickItem *focusItem = keyboardFocusItemFor(item);
    QQuickWindow *window = focusItem->window();
    prepareWindowForInput(window);
    focusItem->forceActiveFocus(Qt::OtherFocusReason);
    const bool focusProperty = focusItem->hasFocus();
    const bool activeFocus = focusItem->hasActiveFocus();
    const bool windowLocalFocus = window && window->activeFocusItem() == focusItem;
    if (!focusProperty && !windowLocalFocus)
        return focusFailure(QStringLiteral("focus_rejected"), nodeId,
                            { QStringLiteral("windowLocalActiveFocus=false"),
                              QStringLiteral("hasActiveFocus=%1")
                                      .arg(activeFocus
                                           ? QStringLiteral("true")
                                           : QStringLiteral("false")) });

    QJsonObject result{
        { QStringLiteral("focused"), true },
        { QStringLiteral("node"), ref.node },
        { QStringLiteral("deliveryWindow"), deliveryWindowEvidence(item) },
        { QStringLiteral("mode"), QStringLiteral("qquickitem-force-active-focus") },
        { QStringLiteral("activeFocus"), activeFocus },
        { QStringLiteral("focusProperty"), focusProperty },
        { QStringLiteral("windowLocalActiveFocus"), windowLocalFocus },
        { QStringLiteral("postDispatch"), postDispatchTargetState(nodeId) },
    };
    if (!windowLocalFocus) {
        result.insert(QStringLiteral("limitations"), QJsonArray{
            QStringLiteral("The target item accepted focus, but its window did not expose it as activeFocusItem. Targeted key input will be delivered directly to the focused item through Qt's event path."),
        });
    }
    if (focusItem != item) {
        result.insert(QStringLiteral("focusTarget"), QJsonObject{
            { QStringLiteral("type"), QString::fromUtf8(focusItem->metaObject()->className()) },
            { QStringLiteral("objectName"), focusItem->objectName() },
            { QStringLiteral("reason"), QStringLiteral("editable-control-content-item") },
        });
    }
    return result;
}

QJsonObject QQmlAgentInput::dispatchKeyEvent(const QJsonObject &params)
{
    int key = 0;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    QString text;
    if (!keyFromParams(params, &key, &modifiers, &text)) {
        return keyFailure(QStringLiteral("invalid_key"),
                          { QStringLiteral("provide keyCode or a QKeySequence-compatible key"),
                            QStringLiteral("common aliases are accepted: Escape, Delete, Insert, PageUp, PageDown, Enter, Spacebar") });
    }

    const QString type = params.value(QStringLiteral("type")).toString(QStringLiteral("keyClick"));
    if (type != QLatin1String("keyClick") && type != QLatin1String("keyPress")
            && type != QLatin1String("keyRelease")) {
        return keyFailure(QStringLiteral("invalid_type"),
                          { QStringLiteral("type must be keyClick, keyPress, or keyRelease") });
    }

    int requestedWindowId = -1;
    QJsonObject windowFailure;
    QQuickWindow *requestedWindow = requestedQuickWindow(params, &requestedWindowId,
                                                         &windowFailure);
    if (!windowFailure.isEmpty())
        return windowFailure;

    QJsonObject focusResult;
    QQuickWindow *window = nullptr;
    QQuickItem *targetKeyItem = nullptr;
    if (hasNodeReference(params)) {
        const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
        if (!ref.issues.isEmpty())
            return failureWithDiagnostics(ref.failureReason, ref.issues);

        QQuickItem *item = qobject_cast<QQuickItem *>(ref.object);
        QQuickWindow *itemWindow = item ? item->window() : nullptr;
        if (requestedWindow && itemWindow && requestedWindow != itemWindow)
            return keyboardWindowMismatch(requestedWindowId, itemWindow);

        focusResult = focusNode(params);
        if (!focusResult.value(QStringLiteral("focused")).toBool()) {
            QJsonObject result = keyFailure(QStringLiteral("focus_failed"),
                                            { QStringLiteral("Input.focusNode did not focus target") });
            result.insert(QStringLiteral("focus"), focusResult);
            return result;
        }

        if (item) {
            window = item->window();
            targetKeyItem = keyboardFocusItemFor(item);
        }
    }

    if (!window)
        window = requestedWindow ? requestedWindow : targetQuickWindow();
    if (!window) {
        return keyFailure(QStringLiteral("unknown_window"),
                          { QStringLiteral("no focused or unique nonzero QQuickWindow with contentItem") });
    }

    QPointer<QQuickWindow> guardedWindow(window);
    const QJsonObject settle = runInputAndSettle(window, params, [&](QElapsedTimer *) {
        if (!guardedWindow)
            return;
        QQuickItem *activeFocusItem = guardedWindow->activeFocusItem();
        QObject *keyTarget = targetKeyItem
                ? (itemContainsOrIs(targetKeyItem, activeFocusItem)
                           ? static_cast<QObject *>(activeFocusItem)
                           : static_cast<QObject *>(targetKeyItem))
                : activeFocusItem ? static_cast<QObject *>(activeFocusItem)
                                  : static_cast<QObject *>(guardedWindow.data());
        QPointer<QObject> guardedKeyTarget(keyTarget);
        if (type == QLatin1String("keyClick") || type == QLatin1String("keyPress"))
            QQmlAgentInputDriver::key(guardedKeyTarget.data(), QEvent::KeyPress, key, modifiers, text);
        if (!guardedKeyTarget)
            return;
        if (type == QLatin1String("keyClick") || type == QLatin1String("keyRelease"))
            QQmlAgentInputDriver::key(guardedKeyTarget.data(), QEvent::KeyRelease, key, modifiers, text);
    });

    QJsonObject result = successfulInputResult({
        { QStringLiteral("delivered"), true },
        { QStringLiteral("keyCode"), key },
        { QStringLiteral("text"), text },
        { QStringLiteral("type"), type },
        { QStringLiteral("mode"), QQmlAgentInputDriver::mode() },
        { QStringLiteral("settle"), settle },
        { QStringLiteral("deliveryWindow"),
          deliveryWindowEvidence(
                  window, targetKeyItem
                          ? QStringLiteral("target-item-window")
                          : requestedWindow ? QStringLiteral("requested-window-id")
                                            : QStringLiteral("focused-or-unique-window")) },
    });
    if (!focusResult.isEmpty())
        result.insert(QStringLiteral("focus"), focusResult);
    return result;
}

static QList<QQuickPopup *> visiblePopupsInScene()
{
    // Overlay children are stacked low-to-high z, so the scene's topmost
    // popup is last. Collected across every window's overlay.
    QList<QQuickPopup *> popups;
    const QWindowList windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        QQuickWindow *quickWindow = qobject_cast<QQuickWindow *>(window);
        if (!quickWindow || !quickWindow->contentItem())
            continue;
        QVector<QQuickItem *> stack{ quickWindow->contentItem() };
        QSet<QQuickItem *> seen;
        while (!stack.isEmpty()) {
            QQuickItem *item = stack.takeLast();
            if (!item || seen.contains(item))
                continue;
            seen.insert(item);
            if (QQuickOverlay *overlay = qobject_cast<QQuickOverlay *>(item)) {
                const QList<QQuickItem *> children = overlay->childItems();
                for (QQuickItem *child : children) {
                    QQuickPopup *popup = qobject_cast<QQuickPopup *>(child->parent());
                    if (popup && popup->isVisible() && !popups.contains(popup))
                        popups.append(popup);
                }
            }
            const QList<QQuickItem *> children = item->childItems();
            for (QQuickItem *child : children)
                stack.append(child);
        }
    }
    return popups;
}

static QJsonObject popupEvidence(QQuickPopup *popup)
{
    QJsonObject info{
        { QStringLiteral("type"),
          QString::fromUtf8(popup->metaObject()->className()) },
        { QStringLiteral("modal"), popup->isModal() },
    };
    if (!popup->objectName().isEmpty())
        info.insert(QStringLiteral("objectName"), popup->objectName());
    return info;
}

QJsonObject QQmlAgentInput::dismissPopup(const QJsonObject &params)
{
    // F-011: a generic, evidence-backed dismissal route. Esc is not honored
    // by every popup and dismiss buttons are app-specific, so a wedged modal
    // can block all further input. close() is programmatic and honors the
    // popup regardless of closePolicy; the result reports whether the scene's
    // visible-popup count actually dropped, not merely that a key was sent.
    const bool all = params.value(QStringLiteral("all")).toBool(false);
    const QList<QQuickPopup *> before = visiblePopupsInScene();
    if (before.isEmpty()) {
        return {
            { QStringLiteral("dismissed"), false },
            { QStringLiteral("reason"), QStringLiteral("no_visible_popup") },
            { QStringLiteral("popupCount"), 0 },
        };
    }

    QList<QQuickPopup *> targets;
    if (all) {
        for (auto it = before.crbegin(); it != before.crend(); ++it)
            targets.append(*it);
    } else {
        targets.append(before.last());
    }

    QJsonArray dismissed;
    for (QQuickPopup *popup : targets) {
        dismissed.append(popupEvidence(popup));
        popup->close();
    }

    // Let exit transitions settle so the count reflects closed popups, not
    // ones mid-animation. Bounded; the service budget covers it.
    QElapsedTimer timer;
    timer.start();
    const int settleMs = 350;
    while (timer.elapsed() < settleMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    const int remaining = visiblePopupsInScene().size();
    QJsonObject result{
        { QStringLiteral("dismissed"), remaining < before.size() },
        { QStringLiteral("dismissedCount"), dismissed.size() },
        { QStringLiteral("popups"), dismissed },
        { QStringLiteral("popupCountBefore"), int(before.size()) },
        { QStringLiteral("remainingPopupCount"), remaining },
        { QStringLiteral("method"), QStringLiteral("popup-close") },
    };
    if (remaining >= before.size()) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("popup.not_dismissed") },
            { QStringLiteral("severity"), QStringLiteral("warning") },
            { QStringLiteral("message"),
              QStringLiteral("close() did not reduce the visible popup count; "
                             "the popup may reopen from a binding or be a "
                             "non-Popup overlay.") },
        } });
    }
    return result;
}

QT_END_NAMESPACE
