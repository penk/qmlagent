// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#ifndef QQMLAGENTRENDER_P_H
#define QQMLAGENTRENDER_P_H

#include <QtCore/qjsonobject.h>

QT_BEGIN_NAMESPACE

class QQmlAgentRender
{
public:
    static QJsonObject captureScreenshot(const QJsonObject &params);
    static QJsonObject pick3D(const QJsonObject &params);
};

QT_END_NAMESPACE

#endif // QQMLAGENTRENDER_P_H
