// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qqmlagentrender_p.h"

#include "qqmlagentjsonutils_p.h"
#include "qqmlagentuitree_p.h"

#include <QtCore/qbuffer.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qmetaobject.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qimage.h>
#include <QtGui/qwindow.h>
#include <QtQuick/qquickitem.h>
#include <QtQuick/qquickwindow.h>

QT_BEGIN_NAMESPACE

static QQuickWindow *quickWindowForId(int requestedWindowId, int *actualWindowId)
{
    int windowId = 0;
    QQuickWindow *firstWindow = nullptr;
    const QWindowList windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        QQuickWindow *quickWindow = qobject_cast<QQuickWindow *>(window);
        if (!quickWindow || !quickWindow->contentItem())
            continue;

        ++windowId;
        if (!firstWindow)
            firstWindow = quickWindow;
        if (requestedWindowId > 0 && requestedWindowId == windowId) {
            *actualWindowId = windowId;
            return quickWindow;
        }
    }

    if (requestedWindowId <= 0 && firstWindow) {
        *actualWindowId = 1;
        return firstWindow;
    }

    *actualWindowId = requestedWindowId;
    return nullptr;
}

static QJsonObject failure(const QString &reason, int windowId)
{
    return {
        { QStringLiteral("captured"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("windowId"), windowId },
    };
}

static QJsonObject diagnostic(const QString &id, const QString &message,
                              const QString &severity = QStringLiteral("error"))
{
    return {
        { QStringLiteral("id"), id },
        { QStringLiteral("severity"), severity },
        { QStringLiteral("confidence"), 1.0 },
        { QStringLiteral("message"), message },
    };
}

static QJsonObject pick3DFailure(const QString &reason, const QString &diagnosticId,
                                 const QString &message, const QJsonObject &target = {})
{
    QJsonObject result{
        { QStringLiteral("ok"), false },
        { QStringLiteral("hit"), false },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("diagnostics"), QJsonArray{ diagnostic(diagnosticId, message) } },
    };
    if (!target.isEmpty())
        result.insert(QStringLiteral("target"), target);
    return result;
}

static bool isQuick3DViewportObject(QObject *object)
{
    return object && object->inherits("QQuick3DViewport");
}

static bool isQuick3DModelObject(QObject *object)
{
    return object && object->inherits("QQuick3DModel");
}

static QString hitTypeName(int hitType)
{
    switch (hitType) {
    case 0:
        return QStringLiteral("Null");
    case 1:
        return QStringLiteral("Model");
    case 2:
        return QStringLiteral("Item");
    default:
        return QStringLiteral("Unknown");
    }
}

static QVariant gadgetProperty(const QMetaObject *metaObject, const void *gadget,
                               const char *propertyName)
{
    if (!metaObject || !gadget)
        return {};
    const int index = metaObject->indexOfProperty(propertyName);
    if (index < 0)
        return {};
    return metaObject->property(index).readOnGadget(gadget);
}

static QJsonObject nodeForHitObject(QObject *object, int windowId)
{
    if (!object)
        return {};
    QSet<QObject *> seen;
    return QQmlAgentUiTree::nodeForObject(object, windowId, 0, true, true, {}, &seen);
}

static QJsonObject pickResultValue(const QMetaType &pickType, const void *pickResult, int windowId)
{
    QJsonObject result{
        { QStringLiteral("available"), true },
    };
    const QMetaObject *metaObject = pickType.metaObject();
    if (!metaObject) {
        result.insert(QStringLiteral("available"), false);
        result.insert(QStringLiteral("reason"), QStringLiteral("pick_result_metaobject_unavailable"));
        return result;
    }

    const QVariant objectHitVariant = gadgetProperty(metaObject, pickResult, "objectHit");
    QObject *objectHit = qvariant_cast<QObject *>(objectHitVariant);
    const QVariant itemHitVariant = gadgetProperty(metaObject, pickResult, "itemHit");
    QObject *itemHit = qvariant_cast<QObject *>(itemHitVariant);
    const int hitType = gadgetProperty(metaObject, pickResult, "hitType").toInt(0);
    const bool hit = objectHit || itemHit || hitType != 0;

    result.insert(QStringLiteral("hit"), hit);
    result.insert(QStringLiteral("hitType"), QJsonObject{
        { QStringLiteral("value"), hitType },
        { QStringLiteral("name"), hitTypeName(hitType) },
    });
    result.insert(QStringLiteral("objectHit"), QQmlAgentJsonUtils::valueFromVariant(objectHitVariant));
    result.insert(QStringLiteral("itemHit"), QQmlAgentJsonUtils::valueFromVariant(itemHitVariant));
    if (objectHit)
        result.insert(QStringLiteral("objectHitNode"), nodeForHitObject(objectHit, windowId));
    if (itemHit)
        result.insert(QStringLiteral("itemHitNode"), nodeForHitObject(itemHit, windowId));

    for (const char *propertyName : { "distance", "uvPosition", "scenePosition", "position",
                                      "normal", "sceneNormal", "instanceIndex" }) {
        const QVariant value = gadgetProperty(metaObject, pickResult, propertyName);
        if (value.isValid())
            result.insert(QString::fromLatin1(propertyName),
                          QQmlAgentJsonUtils::valueFromVariant(value));
    }

    return result;
}

static QJsonObject pointObject(const QPointF &point)
{
    return {
        { QStringLiteral("x"), point.x() },
        { QStringLiteral("y"), point.y() },
    };
}

static QRectF itemRectInWindow(QQuickItem *item)
{
    if (!item)
        return {};
    return QRectF(item->mapToScene(QPointF(0, 0)), QSizeF(item->width(), item->height()));
}

struct PickInvocation
{
    bool invoked = false;
    QJsonObject pick;
};

static PickInvocation invokePick(QObject *viewport, const QMetaMethod &method,
                                 const QMetaType &pickType, const QPointF &localPoint,
                                 int windowId, QObject *modelObject = nullptr)
{
    void *storage = pickType.create();
    if (!storage)
        return {};

    const float x = float(localPoint.x());
    const float y = float(localPoint.y());
    const char *returnTypeName = method.typeName();
    if (!returnTypeName || !returnTypeName[0])
        returnTypeName = pickType.name();
    bool invoked = false;
    if (modelObject) {
        invoked = method.invoke(viewport,
                                Qt::DirectConnection,
                                QGenericReturnArgument(returnTypeName, storage),
                                QGenericArgument("float", &x),
                                QGenericArgument("float", &y),
                                QGenericArgument("QQuick3DModel*", &modelObject));
    } else {
        invoked = method.invoke(viewport,
                                Qt::DirectConnection,
                                QGenericReturnArgument(returnTypeName, storage),
                                QGenericArgument("float", &x),
                                QGenericArgument("float", &y));
    }
    QJsonObject pick = invoked ? pickResultValue(pickType, storage, windowId) : QJsonObject{};
    pickType.destroy(storage);
    return { invoked, pick };
}

QJsonObject QQmlAgentRender::captureScreenshot(const QJsonObject &params)
{
    int windowId = -1;
    QQuickWindow *window = quickWindowForId(params.value(QStringLiteral("windowId")).toInt(-1),
                                           &windowId);
    if (!window)
        return failure(QStringLiteral("window_not_found"), windowId);

    QImage image = window->grabWindow();
    if (image.isNull())
        return failure(QStringLiteral("grab_failed"), windowId);

    const int originalWidth = image.width();
    const int originalHeight = image.height();
    const double devicePixelRatio = image.devicePixelRatio();

    if (params.contains(QStringLiteral("region"))) {
        if (!params.value(QStringLiteral("region")).isObject())
            return failure(QStringLiteral("invalid_region"), windowId);

        const QJsonObject region = params.value(QStringLiteral("region")).toObject();
        const double x = region.value(QStringLiteral("x")).toDouble();
        const double y = region.value(QStringLiteral("y")).toDouble();
        const double width = region.value(QStringLiteral("width")).toDouble();
        const double height = region.value(QStringLiteral("height")).toDouble();
        if (width <= 0 || height <= 0)
            return failure(QStringLiteral("invalid_region"), windowId);

        const QRect requestedRect(qRound(x * devicePixelRatio),
                                  qRound(y * devicePixelRatio),
                                  qRound(width * devicePixelRatio),
                                  qRound(height * devicePixelRatio));
        const QRect clippedRect = requestedRect.intersected(image.rect());
        if (clippedRect.isEmpty())
            return failure(QStringLiteral("region_outside_window"), windowId);

        image = image.copy(clippedRect);
        image.setDevicePixelRatio(devicePixelRatio);
    }

    double scale = 1.0;
    if (params.contains(QStringLiteral("scale"))) {
        scale = params.value(QStringLiteral("scale")).toDouble(1.0);
        if (scale <= 0.0 || scale > 1.0)
            return failure(QStringLiteral("invalid_scale"), windowId);
        if (scale < 1.0) {
            const QSize scaledSize(qMax(1, qRound(image.width() * scale)),
                                   qMax(1, qRound(image.height() * scale)));
            image = image.scaled(scaledSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            image.setDevicePixelRatio(devicePixelRatio * scale);
        }
    }

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return failure(QStringLiteral("encode_failed"), windowId);

    QJsonObject result{
        { QStringLiteral("captured"), true },
        { QStringLiteral("windowId"), windowId },
        { QStringLiteral("format"), QStringLiteral("png") },
        { QStringLiteral("encoding"), QStringLiteral("base64") },
        { QStringLiteral("width"), image.width() },
        { QStringLiteral("height"), image.height() },
        { QStringLiteral("devicePixelRatio"), image.devicePixelRatio() },
        { QStringLiteral("originalWidth"), originalWidth },
        { QStringLiteral("originalHeight"), originalHeight },
        { QStringLiteral("scale"), scale },
        { QStringLiteral("byteSize"), png.size() },
        { QStringLiteral("evidenceRole"), QStringLiteral("fallback-visual") },
        { QStringLiteral("primaryOracle"), false },
        { QStringLiteral("structuredFirst"), true },
    };
    if (params.contains(QStringLiteral("region")))
        result.insert(QStringLiteral("region"), params.value(QStringLiteral("region")).toObject());
    // Base64 bytes are excluded unless explicitly requested with
    // includeData:true (same flag as the MCP tool). Blowing image bytes into
    // an agent's context by default is the exact failure this evidence
    // surface is designed to prevent.
    if (params.value(QStringLiteral("includeData")).toBool(false)) {
        result.insert(QStringLiteral("data"), QString::fromLatin1(png.toBase64()));
    } else {
        result.insert(QStringLiteral("dataOmitted"), true);
        result.insert(QStringLiteral("nextHints"), QJsonArray{
            QJsonObject{
                { QStringLiteral("method"), QStringLiteral("Render.captureScreenshot") },
                { QStringLiteral("params"), QJsonObject{
                    { QStringLiteral("windowId"), windowId },
                    { QStringLiteral("includeData"), true },
                } },
                { QStringLiteral("reason"), QStringLiteral("Request PNG data only when visual fallback evidence is needed.") },
            },
        });
    }
    return result;
}

QJsonObject QQmlAgentRender::pick3D(const QJsonObject &params)
{
    if (!params.contains(QStringLiteral("x")) || !params.contains(QStringLiteral("y"))) {
        return pick3DFailure(QStringLiteral("point_required"),
                             QStringLiteral("render.pick3d_point_required"),
                             QStringLiteral("Render.pick3D requires x and y in View3D-local logical pixels."));
    }
    const QJsonValue xValue = params.value(QStringLiteral("x"));
    const QJsonValue yValue = params.value(QStringLiteral("y"));
    if (!xValue.isDouble() || !yValue.isDouble()) {
        return pick3DFailure(QStringLiteral("invalid_point"),
                             QStringLiteral("render.pick3d_invalid_point"),
                             QStringLiteral("Render.pick3D x and y must be numbers."));
    }

    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    QJsonObject target{
        { QStringLiteral("selector"), params.value(QStringLiteral("selector")) },
        { QStringLiteral("nodeId"), ref.nodeId },
    };
    if (!ref.node.isEmpty())
        target.insert(QStringLiteral("node"), ref.node);
    if (!ref.issues.isEmpty()) {
        return {
            { QStringLiteral("ok"), false },
            { QStringLiteral("hit"), false },
            { QStringLiteral("reason"), QStringLiteral("target_not_found") },
            { QStringLiteral("target"), target },
            { QStringLiteral("diagnostics"), ref.issues },
        };
    }

    if (!isQuick3DViewportObject(ref.object)) {
        return pick3DFailure(QStringLiteral("target_not_view3d"),
                             QStringLiteral("render.pick3d_target_not_view3d"),
                             QStringLiteral("Render.pick3D target must be a QtQuick3D View3D node."),
                             target);
    }

    const bool hasModelSelector = params.contains(QStringLiteral("modelSelector"))
            && !params.value(QStringLiteral("modelSelector")).toString().isEmpty();
    const bool hasModelNodeId = params.contains(QStringLiteral("modelNodeId"))
            && !params.value(QStringLiteral("modelNodeId")).isUndefined()
            && !params.value(QStringLiteral("modelNodeId")).isNull();
    QObject *modelObject = nullptr;
    QJsonObject modelTarget;
    if (hasModelSelector || hasModelNodeId) {
        if (hasModelSelector == hasModelNodeId) {
            return pick3DFailure(QStringLiteral("invalid_model_noderef"),
                                 QStringLiteral("render.pick3d_invalid_model_noderef"),
                                 QStringLiteral("Provide exactly one of modelSelector or modelNodeId."),
                                 target);
        }

        QJsonObject modelParams;
        if (hasModelSelector)
            modelParams.insert(QStringLiteral("selector"), params.value(QStringLiteral("modelSelector")));
        else
            modelParams.insert(QStringLiteral("nodeId"), params.value(QStringLiteral("modelNodeId")));
        const QQmlAgentUiTree::NodeRef modelRef = QQmlAgentUiTree::resolveNodeRef(modelParams);
        modelTarget = {
            { QStringLiteral("selector"), params.value(QStringLiteral("modelSelector")) },
            { QStringLiteral("nodeId"), modelRef.nodeId },
        };
        if (!modelRef.node.isEmpty())
            modelTarget.insert(QStringLiteral("node"), modelRef.node);
        if (!modelRef.issues.isEmpty()) {
            QJsonObject failure = pick3DFailure(
                    QStringLiteral("model_not_found"),
                    QStringLiteral("render.pick3d_model_not_found"),
                    QStringLiteral("Render.pick3D modelSelector/modelNodeId did not resolve to exactly one live node."),
                    target);
            failure.insert(QStringLiteral("modelTarget"), modelTarget);
            return failure;
        }
        if (!isQuick3DModelObject(modelRef.object)) {
            QJsonObject failure = pick3DFailure(
                    QStringLiteral("model_target_not_model"),
                    QStringLiteral("render.pick3d_model_target_not_model"),
                    QStringLiteral("Render.pick3D modelSelector/modelNodeId must resolve to a QtQuick3D Model."),
                    target);
            failure.insert(QStringLiteral("modelTarget"), modelTarget);
            return failure;
        }
        modelObject = modelRef.object;
    }

    const QMetaObject *metaObject = ref.object->metaObject();
    const int methodIndex = modelObject
            ? metaObject->indexOfMethod("pick(float,float,QQuick3DModel*)")
            : metaObject->indexOfMethod("pick(float,float)");
    if (methodIndex < 0) {
        return pick3DFailure(QStringLiteral("pick_method_unavailable"),
                             QStringLiteral("render.pick3d_method_unavailable"),
                             modelObject
                                     ? QStringLiteral("The target View3D does not expose pick(float,float,QQuick3DModel*).")
                                     : QStringLiteral("The target View3D does not expose pick(float,float)."),
                             target);
    }

    const QMetaType pickType = QMetaType::fromName("QQuick3DPickResult");
    if (!pickType.isValid()) {
        return pick3DFailure(QStringLiteral("pick_result_type_unavailable"),
                             QStringLiteral("render.pick3d_result_type_unavailable"),
                             QStringLiteral("QQuick3DPickResult is not registered in this target."),
                             target);
    }

    void *storage = pickType.create();
    if (!storage) {
        return pick3DFailure(QStringLiteral("pick_result_storage_failed"),
                             QStringLiteral("render.pick3d_result_storage_failed"),
                             QStringLiteral("Could not allocate QQuick3DPickResult storage."),
                             target);
    }
    pickType.destroy(storage);

    const QMetaMethod method = metaObject->method(methodIndex);
    const int windowId = ref.node.value(QStringLiteral("windowId")).toInt(-1);
    const QPointF requestedPoint(xValue.toDouble(), yValue.toDouble());
    QQuickItem *viewportItem = qobject_cast<QQuickItem *>(ref.object);
    if (viewportItem && !viewportItem->window()) {
        return pick3DFailure(QStringLiteral("viewport_not_in_window"),
                             QStringLiteral("render.pick3d_viewport_not_in_window"),
                             QStringLiteral("Render.pick3D target View3D is not attached to a window."),
                             target);
    }

    const QString requestedCoordinateSpace =
            params.value(QStringLiteral("coordinateSpace")).toString(QStringLiteral("auto"));
    const bool windowOnly = requestedCoordinateSpace == QLatin1String("window")
            || requestedCoordinateSpace == QLatin1String("window-logical-pixels");
    const bool localOnly = requestedCoordinateSpace == QLatin1String("local")
            || requestedCoordinateSpace == QLatin1String("view3d-local-logical-pixels");
    if (!windowOnly && !localOnly && requestedCoordinateSpace != QLatin1String("auto")) {
        return pick3DFailure(QStringLiteral("invalid_coordinate_space"),
                             QStringLiteral("render.pick3d_invalid_coordinate_space"),
                             QStringLiteral("Render.pick3D coordinateSpace must be auto, local, view3d-local-logical-pixels, window, or window-logical-pixels."),
                             target);
    }

    const QRectF viewportWindowRect = itemRectInWindow(viewportItem);
    QJsonArray attempts;
    QJsonObject pick;
    bool invoked = false;
    bool selectedHit = false;
    QString usedCoordinateSpace;
    QPointF usedLocalPoint;

    auto appendAttempt = [&](const QString &coordinateSpace, const QPointF &localPoint,
                             const QString &reason = QString()) {
        const PickInvocation attempt = invokePick(ref.object, method, pickType, localPoint,
                                                  windowId, modelObject);
        QJsonObject attemptObject{
            { QStringLiteral("coordinateSpace"), coordinateSpace },
            { QStringLiteral("localPoint"), pointObject(localPoint) },
            { QStringLiteral("invoked"), attempt.invoked },
            { QStringLiteral("method"), QString::fromLatin1(method.methodSignature()) },
        };
        if (!reason.isEmpty())
            attemptObject.insert(QStringLiteral("reason"), reason);
        if (attempt.invoked)
            attemptObject.insert(QStringLiteral("hit"), attempt.pick.value(QStringLiteral("hit")).toBool(false));
        attempts.append(attemptObject);
        if (!invoked)
            invoked = attempt.invoked;
        const bool attemptHit = attempt.pick.value(QStringLiteral("hit")).toBool(false);
        if (attempt.invoked && (pick.isEmpty() || (!selectedHit && attemptHit))) {
            pick = attempt.pick;
            usedCoordinateSpace = coordinateSpace;
            usedLocalPoint = localPoint;
            selectedHit = attemptHit;
        }
    };

    if (windowOnly) {
        appendAttempt(QStringLiteral("window-logical-pixels"),
                      requestedPoint - viewportWindowRect.topLeft());
    } else {
        appendAttempt(QStringLiteral("view3d-local-logical-pixels"), requestedPoint);
        if (!localOnly && !pick.value(QStringLiteral("hit")).toBool(false)
                && viewportItem && viewportWindowRect.contains(requestedPoint)) {
            appendAttempt(QStringLiteral("window-logical-pixels"),
                          requestedPoint - viewportWindowRect.topLeft(),
                          QStringLiteral("local_point_missed_and_window_point_was_inside_view3d"));
        }
    }

    if (!invoked) {
        return pick3DFailure(QStringLiteral("pick_invocation_failed"),
                             QStringLiteral("render.pick3d_invocation_failed"),
                             QStringLiteral("View3D pick(float,float) invocation failed."),
                             target);
    }

    QJsonObject result{
        { QStringLiteral("ok"), true },
        { QStringLiteral("hit"), pick.value(QStringLiteral("hit")).toBool(false) },
        { QStringLiteral("mode"), QStringLiteral("read-only") },
        { QStringLiteral("evidenceRole"), QStringLiteral("3d-pick") },
        { QStringLiteral("method"), QString::fromLatin1(method.methodSignature()) },
        { QStringLiteral("targetedModel"), bool(modelObject) },
        { QStringLiteral("coordinateSpace"), usedCoordinateSpace },
        { QStringLiteral("requestedCoordinateSpace"), requestedCoordinateSpace },
        { QStringLiteral("point"), QJsonObject{
            { QStringLiteral("x"), xValue.toDouble() },
            { QStringLiteral("y"), yValue.toDouble() },
        } },
        { QStringLiteral("localPoint"), pointObject(usedLocalPoint) },
        { QStringLiteral("viewportWindowBounds"), QJsonArray{
            viewportWindowRect.x(), viewportWindowRect.y(),
            viewportWindowRect.width(), viewportWindowRect.height(),
        } },
        { QStringLiteral("attempts"), attempts },
        { QStringLiteral("target"), target },
        { QStringLiteral("pick"), pick },
        { QStringLiteral("limitations"), QJsonArray{
            QStringLiteral("picking reports Quick3D hit-test evidence only; it does not deliver input events"),
            QStringLiteral("a miss can mean no model under the point, non-pickable content, or an unrendered/offscreen scene"),
            QStringLiteral("coordinateSpace:auto first tries View3D-local logical pixels, then window logical pixels if that point lies inside the View3D"),
        } },
    };
    if (!modelTarget.isEmpty())
        result.insert(QStringLiteral("modelTarget"), modelTarget);
    return result;
}

QT_END_NAMESPACE
