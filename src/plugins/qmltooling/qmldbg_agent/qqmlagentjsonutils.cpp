// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qqmlagentjsonutils_p.h"

#include <QtCore/qjsonarray.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qmetaobject.h>
#include <QtCore/qobject.h>
#include <QtCore/qurl.h>
#include <QtGui/qcolor.h>
#include <QtGui/qfont.h>
#include <QtGui/qmatrix4x4.h>
#include <QtGui/qquaternion.h>
#include <QtGui/qvector2d.h>
#include <QtGui/qvector3d.h>
#include <QtQml/qqmllist.h>
#include <QtQml/qqmlproperty.h>

#include <private/qqmldata_p.h>
#include <private/qqmldebugservice_p.h>
#include <private/qqmlcontextdata_p.h>
#include <private/qqmlmetatype_p.h>

QT_BEGIN_NAMESPACE

namespace {

QString qmlIdForObject(const QObject *object)
{
    const QQmlData *data = QQmlData::get(object);
    if (!data || !data->outerContext)
        return {};
    return data->outerContext->findObjectId(object);
}

QString quotedSelectorValue(const QString &value)
{
    QString quoted;
    quoted.reserve(value.size() + 2);
    quoted.append(QLatin1Char('"'));
    for (const QChar ch : value) {
        if (ch == QLatin1Char('\\') || ch == QLatin1Char('"'))
            quoted.append(QLatin1Char('\\'));
        quoted.append(ch);
    }
    quoted.append(QLatin1Char('"'));
    return quoted;
}

QString preferredSelectorForObject(QObject *object, int nodeId, const QString &qmlId)
{
    if (!qmlId.isEmpty())
        return QStringLiteral("id=%1").arg(quotedSelectorValue(qmlId));
    if (!object->objectName().isEmpty())
        return QStringLiteral("objectName=%1").arg(quotedSelectorValue(object->objectName()));
    if (nodeId >= 0)
        return QStringLiteral("nodeId=%1").arg(nodeId);
    return {};
}

QString prettyTypeName(QObject *object)
{
    const QString qmlType = QQmlMetaType::prettyTypeName(object);
    if (!qmlType.isEmpty())
        return qmlType;
    return QString::fromUtf8(object->metaObject()->className());
}

QJsonObject objectReference(QObject *object)
{
    QJsonObject reference{
        { QStringLiteral("kind"), QStringLiteral("QObjectRef") },
    };
    if (!object) {
        reference.insert(QStringLiteral("valid"), false);
        return reference;
    }

    const int nodeId = QQmlDebugService::idForObject(object);
    const QString qmlId = qmlIdForObject(object);
    reference.insert(QStringLiteral("valid"), true);
    reference.insert(QStringLiteral("nodeId"), nodeId);
    reference.insert(QStringLiteral("type"), prettyTypeName(object));
    if (!qmlId.isEmpty())
        reference.insert(QStringLiteral("qmlId"), qmlId);
    if (!object->objectName().isEmpty())
        reference.insert(QStringLiteral("objectName"), object->objectName());
    const QString selector = preferredSelectorForObject(object, nodeId, qmlId);
    if (!selector.isEmpty())
        reference.insert(QStringLiteral("selector"), selector);
    return reference;
}

QJsonArray matrixRows(const QMatrix4x4 &matrix)
{
    QJsonArray rows;
    for (int row = 0; row < 4; ++row) {
        QJsonArray columns;
        for (int column = 0; column < 4; ++column)
            columns.append(matrix(row, column));
        rows.append(columns);
    }
    return rows;
}

QJsonArray objectListReferences(const QList<QObject *> &objects)
{
    QJsonArray array;
    for (QObject *object : objects)
        array.append(objectReference(object));
    return array;
}

QJsonObject listReferenceValue(const QQmlListReference &reference)
{
    QJsonObject object{
        { QStringLiteral("kind"), QStringLiteral("QObjectListRef") },
        { QStringLiteral("valid"), reference.isValid() },
        { QStringLiteral("readable"), reference.isReadable() },
    };
    if (!reference.isValid() || !reference.isReadable())
        return object;

    const qsizetype count = reference.count();
    object.insert(QStringLiteral("count"), int(count));
    QJsonArray items;
    for (qsizetype i = 0; i < count; ++i)
        items.append(objectReference(reference.at(i)));
    object.insert(QStringLiteral("items"), items);
    return object;
}

QJsonValue jsonValueFromVariantMap(const QVariantMap &map)
{
    QJsonObject object;
    for (auto it = map.constBegin(), end = map.constEnd(); it != end; ++it)
        object.insert(it.key(), QQmlAgentJsonUtils::valueFromVariant(it.value()));
    return object;
}

QJsonValue jsonValueFromVariantList(const QVariantList &list)
{
    QJsonArray array;
    for (const QVariant &entry : list)
        array.append(QQmlAgentJsonUtils::valueFromVariant(entry));
    return array;
}

} // namespace

namespace QQmlAgentJsonUtils {

QJsonValue valueFromVariant(const QVariant &value)
{
    if (!value.isValid())
        return QJsonValue();

    if (value.canConvert<QObject *>()) {
        if (QObject *object = qvariant_cast<QObject *>(value))
            return objectReference(object);
    }

    if (value.metaType() == QMetaType::fromType<QQmlListReference>())
        return listReferenceValue(qvariant_cast<QQmlListReference>(value));

    if (value.canConvert<QObjectList>())
        return objectListReferences(qvariant_cast<QObjectList>(value));

    switch (value.metaType().id()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return value.toLongLong();
    case QMetaType::Float:
    case QMetaType::Double:
        return value.toDouble();
    case QMetaType::QString:
        return value.toString();
    case QMetaType::QUrl:
        return value.toUrl().toString();
    case QMetaType::QColor: {
        const QColor color = qvariant_cast<QColor>(value);
        if (!color.isValid())
            return QJsonValue();
        if (color.alpha() == 255)
            return color.name(QColor::HexRgb);
        return color.name(QColor::HexArgb);
    }
    case QMetaType::QPoint:
    case QMetaType::QPointF: {
        const QPointF point = value.toPointF();
        return QJsonObject{
            { QStringLiteral("x"), point.x() },
            { QStringLiteral("y"), point.y() },
        };
    }
    case QMetaType::QVector3D: {
        const QVector3D vector = value.value<QVector3D>();
        return QJsonObject{
            { QStringLiteral("x"), vector.x() },
            { QStringLiteral("y"), vector.y() },
            { QStringLiteral("z"), vector.z() },
        };
    }
    case QMetaType::QVector2D: {
        const QVector2D vector = value.value<QVector2D>();
        return QJsonObject{
            { QStringLiteral("x"), vector.x() },
            { QStringLiteral("y"), vector.y() },
        };
    }
    case QMetaType::QQuaternion: {
        const QQuaternion quaternion = value.value<QQuaternion>();
        return QJsonObject{
            { QStringLiteral("scalar"), quaternion.scalar() },
            { QStringLiteral("x"), quaternion.x() },
            { QStringLiteral("y"), quaternion.y() },
            { QStringLiteral("z"), quaternion.z() },
        };
    }
    case QMetaType::QMatrix4x4:
        return matrixRows(value.value<QMatrix4x4>());
    case QMetaType::QSize:
    case QMetaType::QSizeF: {
        const QSizeF size = value.toSizeF();
        return QJsonObject{
            { QStringLiteral("width"), size.width() },
            { QStringLiteral("height"), size.height() },
        };
    }
    case QMetaType::QRect:
    case QMetaType::QRectF: {
        const QRectF rect = value.toRectF();
        return QJsonArray{ rect.x(), rect.y(), rect.width(), rect.height() };
    }
    case QMetaType::QVariantList:
        return jsonValueFromVariantList(value.toList());
    case QMetaType::QVariantMap:
        return jsonValueFromVariantMap(value.toMap());
    default:
        return QJsonValue::fromVariant(value);
    }
}

QJsonValue propertyValue(QObject *object, const QString &propertyName)
{
    if (!object || propertyName.isEmpty())
        return QJsonValue(QJsonValue::Undefined);

    const QByteArray propertyNameUtf8 = propertyName.toUtf8();
    const QQmlListReference listReference(object, propertyNameUtf8.constData());
    if (listReference.isValid())
        return listReferenceValue(listReference);

    const QQmlProperty qmlProperty(object, propertyName);
    if (qmlProperty.isValid())
        return valueFromVariant(qmlProperty.read());

    const QVariant value = object->property(propertyNameUtf8.constData());
    if (value.isValid())
        return valueFromVariant(value);
    return QJsonValue(QJsonValue::Undefined);
}

} // namespace QQmlAgentJsonUtils

QT_END_NAMESPACE
