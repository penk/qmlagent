// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#ifndef QQMLAGENTCANVASPAINTER_P_H
#define QQMLAGENTCANVASPAINTER_P_H

#include <QtCore/qjsonvalue.h>

QT_BEGIN_NAMESPACE

class QVariant;

namespace QQmlAgentCanvasPainter {

// Returns Undefined when the value is not a supported CanvasPainter type.
QJsonValue valueFromVariant(const QVariant &value);

} // namespace QQmlAgentCanvasPainter

QT_END_NAMESPACE

#endif // QQMLAGENTCANVASPAINTER_P_H
