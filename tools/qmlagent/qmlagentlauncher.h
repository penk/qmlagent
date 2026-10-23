// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#ifndef QMLAGENTLAUNCHER_H
#define QMLAGENTLAUNCHER_H

#include <QtCore/qjsonarray.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qlist.h>
#include <QtCore/qstring.h>

struct LauncherSession
{
    QString id;
    QJsonObject metadata;
    QJsonObject status;
};

// Send a request to an existing launcher mailbox; report transport failures in error.
QJsonObject sendLauncherControlRequest(const QJsonObject &metadata, int timeoutMs,
                                      const QString &method, const QJsonObject &params,
                                      QString *error);

// Probe control endpoints without treating registry PIDs as local process identities.
QList<LauncherSession> discoverLauncherSessions(int timeoutMs);

// Serialize discovered sessions, including unreachable control-channel diagnostics.
QJsonArray launcherSessionSummaries(const QList<LauncherSession> &sessions);

// Select one reachable launcher, optionally pinned by a session id prefix.
bool resolveLauncherSession(int timeoutMs, LauncherSession *selected, QString *error,
                            const QString &wantedSessionId = {});

#endif // QMLAGENTLAUNCHER_H
