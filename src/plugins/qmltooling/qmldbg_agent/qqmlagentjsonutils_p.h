// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#ifndef QQMLAGENTJSONUTILS_P_H
#define QQMLAGENTJSONUTILS_P_H

#include <QtCore/qjsonvalue.h>
#include <QtCore/qstring.h>
#include <QtCore/qvariant.h>

QT_BEGIN_NAMESPACE

class QObject;

namespace QQmlAgentJsonUtils {

QJsonValue valueFromVariant(const QVariant &value);
QJsonValue propertyValue(QObject *object, const QString &propertyName);

} // namespace QQmlAgentJsonUtils

QT_END_NAMESPACE

#endif // QQMLAGENTJSONUTILS_P_H
