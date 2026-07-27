// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qqmlagentruntime_p.h"

#include "qqmlagentjsonutils_p.h"
#include "qqmlagentsourceresolver_p.h"
#include "qqmlagentuitree_p.h"

#include <QtCore/qelapsedtimer.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qmetaobject.h>
#include <QtCore/qtimer.h>
#include <QtCore/qvariant.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qvector3d.h>
#include <QtQuick/qquickitem.h>
#include <QtQuick/qquickwindow.h>

#include <cmath>
#include <limits>

QT_BEGIN_NAMESPACE

namespace {

constexpr int DefaultSettleTimeoutMs = 50;
constexpr int MaxSettleTimeoutMs = 30000;

int boundedSettleTimeoutMs(const QJsonObject &params)
{
    const QJsonObject settle = params.value(QStringLiteral("settle")).toObject();
    return qBound(0, settle.value(QStringLiteral("timeoutMs")).toInt(DefaultSettleTimeoutMs),
                  MaxSettleTimeoutMs);
}

QJsonObject runtimeDiagnostic(const QString &id, const QString &message)
{
    return {
        { QStringLiteral("id"), id },
        { QStringLiteral("severity"), QStringLiteral("error") },
        { QStringLiteral("confidence"), 1.0 },
        { QStringLiteral("message"), message },
    };
}

QJsonObject baseResult()
{
    return {
        { QStringLiteral("ok"), false },
        { QStringLiteral("mode"), QStringLiteral("whitebox") },
        { QStringLiteral("verificationRole"), QStringLiteral("setup-only") },
    };
}

QJsonObject targetObject(const QJsonObject &params, const QQmlAgentUiTree::NodeRef &ref)
{
    QJsonObject target{
        { QStringLiteral("nodeId"), ref.nodeId },
    };
    const QString selector = params.value(QStringLiteral("selector")).toString();
    if (!selector.isEmpty())
        target.insert(QStringLiteral("selector"), selector);
    return target;
}

QQuickWindow *windowForObject(QObject *object)
{
    if (QQuickItem *item = qobject_cast<QQuickItem *>(object))
        return item->window();

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

template <typename Fn>
QJsonObject runAndSettle(QObject *object, const QJsonObject &params, Fn &&fn)
{
    int framesAfterAction = 0;
    QElapsedTimer elapsed;
    elapsed.start();

    QQuickWindow *window = windowForObject(object);
    QEventLoop settleLoop;
    QMetaObject::Connection frameConnection;
    if (window) {
        frameConnection = QObject::connect(window, &QQuickWindow::frameSwapped,
                                          &settleLoop, [&]() {
            ++framesAfterAction;
            settleLoop.quit();
        });
    }

    fn();

    const int settleTimeoutMs = boundedSettleTimeoutMs(params);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &settleLoop, &QEventLoop::quit);
    timeout.start(settleTimeoutMs);
    settleLoop.exec(QEventLoop::ExcludeUserInputEvents);

    if (frameConnection)
        QObject::disconnect(frameConnection);

    return {
        { QStringLiteral("strategy"), QStringLiteral("frameSwappedOrTimeout") },
        { QStringLiteral("framesAfterAction"), framesAfterAction },
        { QStringLiteral("elapsedMs"), int(elapsed.elapsed()) },
        { QStringLiteral("timedOut"), framesAfterAction == 0 },
    };
}

bool isPrimitiveJsonValue(const QJsonValue &value)
{
    return value.isNull() || value.isBool() || value.isDouble() || value.isString();
}

QVariant variantFromJson(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isBool())
        return value.toBool();
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (qIsFinite(number) && std::floor(number) == number
                && number >= std::numeric_limits<int>::min()
                && number <= std::numeric_limits<int>::max()) {
            return int(number);
        }
        return number;
    }
    if (value.isArray())
        return value.toArray().toVariantList();
    if (value.isObject())
        return value.toObject().toVariantMap();
    return QVariant();
}

bool variantNumber(const QVariant &value, float *number)
{
    bool ok = false;
    const double parsed = value.toDouble(&ok);
    if (!ok || !qIsFinite(parsed)
            || parsed < -std::numeric_limits<float>::max()
            || parsed > std::numeric_limits<float>::max()) {
        return false;
    }
    *number = float(parsed);
    return true;
}

bool vector3DFromVariant(const QVariant &value, QVector3D *vector)
{
    if (value.metaType() == QMetaType::fromType<QVector3D>()) {
        *vector = value.value<QVector3D>();
        return true;
    }

    if (value.canConvert<QVariantMap>()) {
        const QVariantMap map = value.toMap();
        if (!map.contains(QStringLiteral("x")) || !map.contains(QStringLiteral("y"))
                || !map.contains(QStringLiteral("z"))) {
            return false;
        }
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!variantNumber(map.value(QStringLiteral("x")), &x)
                || !variantNumber(map.value(QStringLiteral("y")), &y)
                || !variantNumber(map.value(QStringLiteral("z")), &z)) {
            return false;
        }
        *vector = QVector3D(x, y, z);
        return true;
    }

    if (value.canConvert<QVariantList>()) {
        const QVariantList list = value.toList();
        if (list.size() != 3)
            return false;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!variantNumber(list.at(0), &x) || !variantNumber(list.at(1), &y)
                || !variantNumber(list.at(2), &z)) {
            return false;
        }
        *vector = QVector3D(x, y, z);
        return true;
    }

    return false;
}

bool jsonValueSupportedForRuntime(const QJsonValue &value)
{
    if (isPrimitiveJsonValue(value))
        return true;
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (const QJsonValue &entry : array) {
            if (!isPrimitiveJsonValue(entry))
                return false;
        }
        return true;
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(), end = object.constEnd(); it != end; ++it) {
            if (!isPrimitiveJsonValue(it.value()))
                return false;
        }
        return true;
    }
    return false;
}

bool convertVariant(QVariant *value, QMetaType targetType)
{
    if (!targetType.isValid() || targetType.id() == QMetaType::QVariant)
        return true;
    if (!value->isValid())
        return true;
    if (value->metaType() == targetType)
        return true;
    if (targetType == QMetaType::fromType<QVector3D>()) {
        QVector3D vector;
        if (!vector3DFromVariant(*value, &vector))
            return false;
        *value = QVariant::fromValue(vector);
        return true;
    }
    return value->convert(targetType);
}

QJsonValue jsonValueFromVariant(const QVariant &value)
{
    if (!value.isValid())
        return QJsonValue();
    return QQmlAgentJsonUtils::valueFromVariant(value);
}

QJsonObject failedTargetResult(const QJsonObject &params, const QQmlAgentUiTree::NodeRef &ref,
                               const QString &notFoundId, const QString &ambiguousId)
{
    QJsonObject result = baseResult();
    QJsonArray diagnostics = ref.issues;
    for (QJsonValueRef diagnosticValue : diagnostics) {
        QJsonObject diagnostic = diagnosticValue.toObject();
        const QString id = diagnostic.value(QStringLiteral("id")).toString();
        if (id == QLatin1String("selector.not_found"))
            diagnostic.insert(QStringLiteral("id"), notFoundId);
        else if (id == QLatin1String("selector.ambiguous"))
            diagnostic.insert(QStringLiteral("id"), ambiguousId);
        diagnosticValue = diagnostic;
    }
    if (diagnostics.isEmpty()) {
        diagnostics.append(runtimeDiagnostic(
                notFoundId, QStringLiteral("Runtime target could not be resolved.")));
    }
    result.insert(QStringLiteral("diagnostics"), diagnostics);
    const QString selector = params.value(QStringLiteral("selector")).toString();
    if (!selector.isEmpty())
        result.insert(QStringLiteral("target"), QJsonObject{ { QStringLiteral("selector"), selector } });
    return result;
}

QList<QMetaMethod> matchingRuntimeMethods(QObject *object, const QString &methodName, int argumentCount,
                                          bool *nameFound)
{
    QList<QMetaMethod> matches;
    const QMetaObject *metaObject = object->metaObject();
    const QByteArray methodUtf8 = methodName.toUtf8();
    for (int i = 0, count = metaObject->methodCount(); i < count; ++i) {
        const QMetaMethod method = metaObject->method(i);
        if (method.access() != QMetaMethod::Public)
            continue;
        if (method.methodType() != QMetaMethod::Method && method.methodType() != QMetaMethod::Slot)
            continue;
        if (method.name() != methodUtf8)
            continue;

        *nameFound = true;
        if (method.parameterCount() == argumentCount)
            matches.append(method);
    }
    return matches;
}

} // namespace

int QQmlAgentRuntime::dispatchBudgetMs(const QJsonObject &params)
{
    return boundedSettleTimeoutMs(params);
}

QJsonObject QQmlAgentRuntime::setProperty(const QJsonObject &params)
{
    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.object)
        return failedTargetResult(params, ref, QStringLiteral("runtime.target_not_found"),
                                  QStringLiteral("runtime.target_ambiguous"));

    QJsonObject result = baseResult();
    result.insert(QStringLiteral("target"), targetObject(params, ref));

    const QString propertyName = params.value(QStringLiteral("property")).toString();
    if (propertyName.isEmpty()) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                QStringLiteral("runtime.property_not_found"),
                QStringLiteral("Property name is required.")) });
        return result;
    }
    result.insert(QStringLiteral("property"), propertyName);

    const QMetaObject *metaObject = ref.object->metaObject();
    const int propertyIndex = metaObject->indexOfProperty(propertyName.toUtf8().constData());
    if (propertyIndex < 0) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                QStringLiteral("runtime.property_not_found"),
                QStringLiteral("Property does not exist on runtime target.")) });
        return result;
    }

    const QMetaProperty property = metaObject->property(propertyIndex);
    if (!property.isWritable()) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                QStringLiteral("runtime.property_read_only"),
                QStringLiteral("Property is not writable.")) });
        return result;
    }

    const QJsonValue jsonValue = params.value(QStringLiteral("value"));
    if (!jsonValueSupportedForRuntime(jsonValue)) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                QStringLiteral("runtime.unsupported_argument_type"),
                QStringLiteral("Runtime.setProperty supports primitive JSON values and simple primitive arrays/objects only.")) });
        return result;
    }

    QVariant convertedValue = variantFromJson(jsonValue);
    if (!convertVariant(&convertedValue, property.metaType())) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                QStringLiteral("runtime.property_type_mismatch"),
                QStringLiteral("JSON value cannot be converted to the target property type.")) });
        return result;
    }

    const QVariant before = property.read(ref.object);
    bool wrote = false;
    const QJsonObject settle = runAndSettle(ref.object, params, [&]() {
        wrote = property.write(ref.object, convertedValue);
    });
    if (!wrote) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                QStringLiteral("runtime.invocation_failed"),
                QStringLiteral("QObject property write failed.")) });
        result.insert(QStringLiteral("settle"), settle);
        return result;
    }

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("before"), jsonValueFromVariant(before));
    result.insert(QStringLiteral("after"), jsonValueFromVariant(property.read(ref.object)));
    result.insert(QStringLiteral("sourceLocation"),
                  QQmlAgentSourceResolver::sourceLocationForObject(ref.object));
    result.insert(QStringLiteral("settle"), settle);
    return result;
}

QJsonObject QQmlAgentRuntime::invokeMethod(const QJsonObject &params)
{
    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.object)
        return failedTargetResult(params, ref, QStringLiteral("runtime.target_not_found"),
                                  QStringLiteral("runtime.target_ambiguous"));

    QJsonObject result = baseResult();
    result.insert(QStringLiteral("target"), targetObject(params, ref));

    const QString methodName = params.value(QStringLiteral("method")).toString();
    if (methodName.isEmpty()) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                QStringLiteral("runtime.method_not_found"),
                QStringLiteral("Method name is required.")) });
        return result;
    }
    result.insert(QStringLiteral("method"), methodName);

    const QJsonArray jsonArgs = params.value(QStringLiteral("args")).toArray();
    for (const QJsonValue &arg : jsonArgs) {
        if (!isPrimitiveJsonValue(arg)) {
            result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                    QStringLiteral("runtime.unsupported_argument_type"),
                    QStringLiteral("Runtime.invokeMethod supports primitive JSON arguments only.")) });
            return result;
        }
    }

    bool methodNameFound = false;
    const QList<QMetaMethod> candidates = matchingRuntimeMethods(
            ref.object, methodName, jsonArgs.size(), &methodNameFound);
    if (candidates.isEmpty()) {
        const QString id = methodNameFound
                ? QStringLiteral("runtime.unsupported_argument_type")
                : QStringLiteral("runtime.method_not_found");
        const QString message = methodNameFound
                ? QStringLiteral("No overload accepts the supplied argument count.")
                : QStringLiteral("Method does not exist on runtime target.");
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                id, message) });
        return result;
    }

    QMetaMethod selectedMethod;
    QVarLengthArray<QVariant, 10> convertedArgs;
    for (const QMetaMethod &candidate : candidates) {
        QVarLengthArray<QVariant, 10> candidateArgs;
        bool compatible = true;
        for (int i = 0; i < candidate.parameterCount(); ++i) {
            QVariant arg = variantFromJson(jsonArgs.at(i));
            if (!convertVariant(&arg, candidate.parameterMetaType(i))) {
                compatible = false;
                break;
            }
            candidateArgs.append(arg);
        }
        if (!compatible)
            continue;
        if (selectedMethod.isValid()) {
            result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                    QStringLiteral("runtime.method_ambiguous"),
                    QStringLiteral("Multiple public methods match the supplied arguments.")) });
            return result;
        }
        selectedMethod = candidate;
        convertedArgs = candidateArgs;
    }

    if (!selectedMethod.isValid()) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                QStringLiteral("runtime.unsupported_argument_type"),
                QStringLiteral("Arguments cannot be converted to a public method overload.")) });
        return result;
    }

    QVariant returnValue;
    if (selectedMethod.returnMetaType().isValid()
            && selectedMethod.returnMetaType().id() != QMetaType::Void) {
        if (selectedMethod.returnMetaType().id() != QMetaType::QVariant)
            returnValue = QVariant(selectedMethod.returnMetaType());
    }

    bool invoked = false;
    const QJsonObject settle = runAndSettle(ref.object, params, [&]() {
        QGenericArgument arguments[10];
        for (qsizetype i = 0; i < convertedArgs.size(); ++i) {
            const QMetaType parameterType = selectedMethod.parameterMetaType(int(i));
            if (parameterType.id() == QMetaType::QVariant) {
                arguments[i] = QGenericArgument(parameterType.name(), &convertedArgs[i]);
            } else {
                arguments[i] = QGenericArgument(parameterType.name(),
                                                const_cast<void *>(convertedArgs.at(i).constData()));
            }
        }

        if (selectedMethod.returnMetaType().isValid()
                && selectedMethod.returnMetaType().id() != QMetaType::Void) {
            QGenericReturnArgument returnArgument(
                    selectedMethod.returnMetaType().name(),
                    selectedMethod.returnMetaType().id() == QMetaType::QVariant
                            ? static_cast<void *>(&returnValue)
                            : returnValue.data());
            invoked = selectedMethod.invoke(
                    ref.object, Qt::DirectConnection, returnArgument,
                    arguments[0], arguments[1], arguments[2], arguments[3], arguments[4],
                    arguments[5], arguments[6], arguments[7], arguments[8], arguments[9]);
        } else {
            invoked = selectedMethod.invoke(
                    ref.object, Qt::DirectConnection,
                    arguments[0], arguments[1], arguments[2], arguments[3], arguments[4],
                    arguments[5], arguments[6], arguments[7], arguments[8], arguments[9]);
        }
    });

    if (!invoked) {
        result.insert(QStringLiteral("diagnostics"), QJsonArray{ runtimeDiagnostic(
                QStringLiteral("runtime.invocation_failed"),
                QStringLiteral("QMetaMethod invocation failed.")) });
        result.insert(QStringLiteral("settle"), settle);
        return result;
    }

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("returnValue"), jsonValueFromVariant(returnValue));
    result.insert(QStringLiteral("settle"), settle);
    return result;
}

QT_END_NAMESPACE
