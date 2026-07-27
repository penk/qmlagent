// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qmlagentclioutput.h"
#include "qmlagentmcpoutput.h"
#include "qmlagentmcpprotocol.h"
#include "qmlagentpaths.h"

#include <private/qqmldebugclient_p.h>
#include <private/qqmldebugconnection_p.h>

#include <QtCore/qcommandlineoption.h>
#include <QtCore/qcommandlineparser.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qdeadlinetimer.h>
#include <QtCore/qdir.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qjsonvalue.h>
#include <QtCore/qlockfile.h>
#include <QtCore/qqueue.h>
#include <QtCore/qsavefile.h>
#include <QtCore/qset.h>
#include <QtCore/qsocketnotifier.h>
#include <QtCore/qstandardpaths.h>
#include <QtCore/qstringlist.h>
#include <QtCore/qtextstream.h>
#include <QtCore/qthread.h>
#include <QtCore/qtimer.h>
#include <QtCore/quuid.h>
#include <QtNetwork/qabstractsocket.h>
#include <QtNetwork/qlocalsocket.h>
#include <QtNetwork/qtcpsocket.h>

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <signal.h>
#include <unistd.h>

static constexpr int LauncherControlReplySlackMs = 1000;

// Protocol version this client was written against; compared with the
// protocolVersion a live QmlAgent session reports via Session.getInfo.
static constexpr QLatin1StringView SupportedAgentProtocolVersion{ "0.1" };

using QmlAgentMcp::jsonError;
using QmlAgentMcp::jsonResponse;
using QmlAgentMcp::toolErrorResult;
using QmlAgentMcp::toolList;
using QmlAgentMcp::toolResult;

static int defaultTcpTargetPort()
{
    const QString user = qEnvironmentVariable("USER",
            qEnvironmentVariable("USERNAME", QStringLiteral("qmlagent")));
    qsizetype hash = 0;
    for (QChar ch : user)
        hash = (hash * 33) + ch.unicode();
    return 3768 + int(qAbs(hash) % 2000);
}

class QmlAgentClient : public QQmlDebugClient
{
    Q_OBJECT

public:
    explicit QmlAgentClient(QQmlDebugConnection *connection)
        : QQmlDebugClient(QStringLiteral("QmlAgent"), connection)
    {
    }

signals:
    void received(const QByteArray &message);

private:
    void messageReceived(const QByteArray &message) override
    {
        emit received(message);
    }
};

static QByteArray compactJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

static int fail(const QString &message)
{
    QTextStream(stderr) << message << Qt::endl;
    return 1;
}

class QmlAgentTargetLease
{
public:
    ~QmlAgentTargetLease() { release(); }
    QmlAgentTargetLease() = default;
    QmlAgentTargetLease(QmlAgentTargetLease &&) noexcept = default;
    QmlAgentTargetLease &operator=(QmlAgentTargetLease &&) noexcept = default;

    QmlAgentTargetLease(const QmlAgentTargetLease &) = delete;
    QmlAgentTargetLease &operator=(const QmlAgentTargetLease &) = delete;

    static QString tcpEndpoint(const QString &host, quint16 port)
    {
        return QStringLiteral("tcp:%1:%2").arg(host).arg(port);
    }

    static QString localSocketEndpoint(const QString &path)
    {
        const QFileInfo info(path);
        const QString normalized = info.isRelative() ? path : info.absoluteFilePath();
        return QStringLiteral("local:%1").arg(normalized);
    }

    bool acquire(const QString &endpoint, const QString &mode, QString *error)
    {
        release();

        const QString lockPath = lockPathForEndpoint(endpoint, error);
        if (lockPath.isEmpty())
            return false;

        auto lock = std::make_unique<QLockFile>(lockPath);
        lock->setStaleLockTime(5000);
        if (!lock->tryLock(0)) {
            *error = conflictMessage(endpoint, mode, *lock);
            return false;
        }

        m_endpoint = endpoint;
        m_mode = mode;
        m_lock = std::move(lock);
        return true;
    }

    void release()
    {
        if (m_lock)
            m_lock->unlock();
        m_lock.reset();
        m_endpoint.clear();
        m_mode.clear();
    }

    bool isLocked() const { return bool(m_lock); }
    QString endpoint() const { return m_endpoint; }
    QString mode() const { return m_mode; }

    QJsonObject toJson() const
    {
        QJsonObject object{
            { QStringLiteral("owned"), isLocked() },
        };
        if (!m_endpoint.isEmpty())
            object.insert(QStringLiteral("endpoint"), m_endpoint);
        if (!m_mode.isEmpty())
            object.insert(QStringLiteral("mode"), m_mode);
        if (isLocked())
            object.insert(QStringLiteral("ownerPid"), qint64(QCoreApplication::applicationPid()));
        return object;
    }

private:
    static QString leaseRoot(QString *error)
    {
        QString root = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (root.isEmpty())
            root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (root.isEmpty()) {
            *error = QStringLiteral("No writable runtime or application data location for QmlAgent target leases.");
            return {};
        }

        const QString path = QDir(root).filePath(QStringLiteral("qmlagent/target-leases"));
        QDir dir;
        if (!dir.mkpath(path)) {
            *error = QStringLiteral("Could not create QmlAgent target lease directory: %1").arg(path);
            return {};
        }
        return path;
    }

    static QString lockPathForEndpoint(const QString &endpoint, QString *error)
    {
        const QString root = leaseRoot(error);
        if (root.isEmpty())
            return {};

        const QByteArray digest = QCryptographicHash::hash(endpoint.toUtf8(),
                                                           QCryptographicHash::Sha256)
                                          .toHex();
        return QDir(root).filePath(QString::fromLatin1(digest) + QStringLiteral(".lock"));
    }

    static QString conflictMessage(const QString &endpoint, const QString &mode,
                                   QLockFile &lock)
    {
        qint64 pid = 0;
        QString hostname;
        QString appname;
        const bool hasOwner = lock.getLockInfo(&pid, &hostname, &appname);

        QString owner;
        if (hasOwner) {
            owner = QStringLiteral(" ownerPid=%1 ownerApp=%2 ownerHost=%3")
                            .arg(pid)
                            .arg(appname.isEmpty() ? QStringLiteral("<unknown>") : appname,
                                 hostname.isEmpty() ? QStringLiteral("<unknown>") : hostname);
        }

        return QStringLiteral("QmlAgent target endpoint is already owned: %1.%2 "
                              "Only one qmlagent-mcp gateway may attach "
                              "to one Qt QML debug target. Use the existing MCP gateway, call "
                              "qmlagent_disconnect on it, or relaunch the target before starting "
                              "another %3 client.")
                .arg(endpoint, owner, mode);
    }

    QString m_endpoint;
    QString m_mode;
    std::unique_ptr<QLockFile> m_lock;
};

static bool boolFromString(const QString &value, bool *ok)
{
    if (value == QLatin1String("true")) {
        *ok = true;
        return true;
    }
    if (value == QLatin1String("false")) {
        *ok = true;
        return false;
    }

    *ok = false;
    return false;
}

static QJsonValue jsonValueForExpectedValue(const QString &text)
{
    if (text.size() >= 2) {
        const QChar first = text.front();
        const QChar last = text.back();
        if ((first == QLatin1Char('"') && last == QLatin1Char('"'))
                || (first == QLatin1Char('\'') && last == QLatin1Char('\''))) {
            QString unquoted = text.mid(1, text.size() - 2);
            unquoted.replace(QStringLiteral("\\\""), QStringLiteral("\""));
            unquoted.replace(QStringLiteral("\\'"), QStringLiteral("'"));
            unquoted.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
            return unquoted;
        }
    }

    bool ok = false;
    const bool boolValue = boolFromString(text, &ok);
    if (ok)
        return boolValue;

    const double number = text.toDouble(&ok);
    if (ok)
        return number;

    return text;
}

struct Expectation
{
    QString property;
    QString op;
    QJsonValue value;
    bool valid = false;
};

static Expectation parseExpectation(const QString &text)
{
    static const QStringList stringOperators{
        QStringLiteral("startsWith"),
        QStringLiteral("endsWith"),
        QStringLiteral("contains"),
    };
    for (const QString &op : stringOperators) {
        const QString token = QLatin1Char(' ') + op + QLatin1Char(' ');
        const int index = text.indexOf(token);
        if (index <= 0 || index + token.size() >= text.size())
            continue;

        return {
            text.left(index).trimmed(),
            op,
            jsonValueForExpectedValue(text.mid(index + token.size()).trimmed()),
            true,
        };
    }

    static const QStringList operators{
        QStringLiteral(">="),
        QStringLiteral("<="),
        QStringLiteral("!="),
        QStringLiteral("="),
        QStringLiteral(">"),
        QStringLiteral("<"),
    };

    for (const QString &op : operators) {
        const int index = text.indexOf(op);
        if (index <= 0 || index + op.size() >= text.size())
            continue;

        return {
            text.left(index).trimmed(),
            op,
            jsonValueForExpectedValue(text.mid(index + op.size()).trimmed()),
            true,
        };
    }

    return {};
}

// Mirrors compareJsonValues/jsonValuesEqual/numericValue in
// qqmlagentuitree.cpp so a workflow before/after verdict and a UI.waitFor
// predicate agree on the same data: fuzzy numeric equality, numeric-string
// coercion for comparisons, bool/null identity, string fallback.
static bool expectationNumericValue(const QJsonValue &value, double *number)
{
    if (value.isDouble()) {
        *number = value.toDouble();
        return true;
    }
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok) {
            *number = parsed;
            return true;
        }
    }
    return false;
}

static bool expectationValuesEqual(const QJsonValue &left, const QJsonValue &right)
{
    double leftNumber = 0;
    double rightNumber = 0;
    if (expectationNumericValue(left, &leftNumber) && expectationNumericValue(right, &rightNumber))
        return qFuzzyCompare(leftNumber + 1.0, rightNumber + 1.0);
    if (left.isBool() || right.isBool())
        return left.toBool() == right.toBool();
    if (left.isNull() || right.isNull())
        return left.isNull() && right.isNull();
    return left.toString() == right.toString();
}

static bool compareExpectedValues(const QJsonValue &actual, const QString &op,
                                  const QJsonValue &expected)
{
    if (op == QLatin1String("="))
        return expectationValuesEqual(actual, expected);
    if (op == QLatin1String("!="))
        return !expectationValuesEqual(actual, expected);

    if (op == QLatin1String("contains") || op == QLatin1String("startsWith")
            || op == QLatin1String("endsWith")) {
        if (!actual.isString() || !expected.isString())
            return false;
        const QString actualString = actual.toString();
        const QString expectedString = expected.toString();
        if (op == QLatin1String("contains"))
            return actualString.contains(expectedString);
        if (op == QLatin1String("startsWith"))
            return actualString.startsWith(expectedString);
        return actualString.endsWith(expectedString);
    }

    double actualNumber = 0;
    double expectedNumber = 0;
    if (!expectationNumericValue(actual, &actualNumber)
            || !expectationNumericValue(expected, &expectedNumber)) {
        return false;
    }

    if (op == QLatin1String(">"))
        return actualNumber > expectedNumber;
    if (op == QLatin1String(">="))
        return actualNumber >= expectedNumber;
    if (op == QLatin1String("<"))
        return actualNumber < expectedNumber;
    if (op == QLatin1String("<="))
        return actualNumber <= expectedNumber;

    return false;
}

static QJsonValue valueForExpectation(const QJsonObject &queryResponse, const Expectation &expectation,
                                      int *matchCount, QJsonObject *node)
{
    const QJsonObject result = queryResponse.value(QStringLiteral("result")).toObject();
    const QJsonArray matches = result.value(QStringLiteral("matches")).toArray();
    if (matchCount)
        *matchCount = matches.size();
    if (matches.isEmpty())
        return {};

    const QJsonObject firstNode = matches.first().toObject();
    if (node)
        *node = firstNode;
    if (firstNode.contains(expectation.property))
        return firstNode.value(expectation.property);
    return firstNode.value(QStringLiteral("properties")).toObject().value(expectation.property);
}

static QJsonObject verificationObject(const QJsonObject &queryResponse,
                                      const Expectation &expectation)
{
    int matchCount = 0;
    const QJsonValue actual = valueForExpectation(queryResponse, expectation, &matchCount, nullptr);
    const bool matched = compareExpectedValues(actual, expectation.op, expectation.value);

    return {
        { QStringLiteral("passed"), matched },
        { QStringLiteral("property"), expectation.property },
        { QStringLiteral("operator"), expectation.op },
        { QStringLiteral("expected"), expectation.value },
        { QStringLiteral("actual"), actual },
        { QStringLiteral("matchCount"), matchCount },
    };
}

static QJsonObject makeRequest(int id, const QString &requestMethod, const QJsonObject &requestParams)
{
    return {
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("id"), id },
        { QStringLiteral("method"), requestMethod },
        { QStringLiteral("params"), requestParams },
    };
}

struct LauncherSession
{
    QString id;
    QJsonObject metadata;
    QJsonObject status;
};

static QJsonArray recentLauncherExitReports()
{
    QJsonArray reports;
    const QDir dir(launcherExitDir());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QStringList entries = dir.entryList({ QStringLiteral("*.json") }, QDir::Files,
                                              QDir::Time);
    for (const QString &entry : entries) {
        const QString path = dir.filePath(entry);
        const QFileInfo info(path);
        if (info.lastModified().toUTC().secsTo(now) > 600) {
            QFile::remove(path);
            continue;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            QFile::remove(path);
            continue;
        }
        reports.append(document.object());
    }
    return reports;
}

static QStringList launcherRegistryDirs()
{
    QStringList dirs;
    const QString workspaceStateDir = QDir::current().filePath(QStringLiteral(".qmlagent"));
    if (QFileInfo::exists(workspaceStateDir))
        dirs.append(QDir(workspaceStateDir).filePath(QStringLiteral("launcher-sessions")));

    for (const QString &globalDir : globalLauncherRegistryDirs()) {
        if (!dirs.contains(globalDir))
            dirs.append(globalDir);
    }
    return dirs;
}

static bool processExists(qint64 pid)
{
    if (pid <= 0)
        return false;
    errno = 0;
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
}

static bool terminatePid(qint64 pid)
{
    if (pid <= 0 || !processExists(pid))
        return false;
    return ::kill(pid_t(pid), SIGTERM) == 0;
}

static QJsonObject readLauncherMailboxResponse(const QString &responsePath, int timeoutMs,
                                               QString *error)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        if (QFileInfo(responsePath).isSymLink()) {
            *error = QStringLiteral("Refusing symlinked qmlagent-launcher mailbox response: %1")
                             .arg(responsePath);
            return {};
        }
        QFile responseFile(responsePath);
        if (responseFile.open(QIODevice::ReadOnly)) {
            const QByteArray payload = responseFile.readAll().trimmed();
            responseFile.close();
            QFile::remove(responsePath);

            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                *error = QStringLiteral("Invalid qmlagent-launcher mailbox response.");
                return {};
            }
            return document.object();
        }

        QThread::msleep(10);
    }

    *error = QStringLiteral("Timed out waiting for qmlagent-launcher mailbox response.");
    return {};
}

static QJsonObject sendLauncherMailboxControlRequest(const QString &mailboxDir, int timeoutMs,
                                                     const QString &method,
                                                     const QJsonObject &params, QString *error)
{
    if (mailboxDir.isEmpty()) {
        *error = QStringLiteral("qmlagent-launcher session has no control mailbox.");
        return {};
    }

    if (!ensurePrivateDirectory(mailboxDir, error))
        return {};

    QLockFile controlLock(QDir(mailboxDir).filePath(QStringLiteral("control.lock")));
    controlLock.setStaleLockTime(5000);
    if (!controlLock.tryLock(qMax(0, timeoutMs))) {
        *error = QStringLiteral("Timed out acquiring qmlagent-launcher control lease for %1. "
                                "Another agent process may be driving this launcher session.")
                         .arg(mailboxDir);
        return {};
    }

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString requestPath = QDir(mailboxDir).filePath(
            QStringLiteral("request-%1.json").arg(token));
    const QString replyDir = QDir(mailboxRepliesRoot())
            .filePath(QString::number(qint64(QCoreApplication::applicationPid())));
    if (!ensurePrivateDirectory(qmlAgentTempRoot(), error))
        return {};
    if (!ensurePrivateDirectory(mailboxRepliesRoot(), error))
        return {};
    if (!ensurePrivateDirectory(replyDir, error))
        return {};
    const QString responsePath = QDir(replyDir).filePath(
            QStringLiteral("response-%1.json").arg(token));

    if (!pathIsInsideDirectory(requestPath, mailboxDir)
            || !pathIsInsideDirectory(responsePath, replyDir)) {
        *error = QStringLiteral("Refusing qmlagent-launcher mailbox path outside private directory.");
        return {};
    }

    QSaveFile requestFile(requestPath);
    if (!requestFile.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Could not write qmlagent-launcher mailbox request: %1")
                         .arg(requestPath);
        return {};
    }
    QJsonObject request = makeRequest(1, method, params);
    request.insert(QStringLiteral("replyTo"), responsePath);
    requestFile.write(compactJson(request));
    requestFile.write("\n");
    if (!requestFile.commit()) {
        *error = QStringLiteral("Could not publish qmlagent-launcher mailbox request: %1")
                         .arg(requestPath);
        return {};
    }
    QFile::setPermissions(requestPath, privateFilePermissions());

    if (method == QLatin1String("Session.stop"))
        controlLock.unlock();

    return readLauncherMailboxResponse(responsePath, timeoutMs, error);
}

static QJsonObject sendLauncherControlRequest(const QJsonObject &metadata, int timeoutMs,
                                              const QString &method, const QJsonObject &params,
                                              QString *error)
{
    const QString mailboxDir = metadata.value(QStringLiteral("controlMailbox")).toString();
    if (!mailboxDir.isEmpty())
        return sendLauncherMailboxControlRequest(mailboxDir, timeoutMs, method, params, error);

    *error = QStringLiteral("qmlagent-launcher session has no supported control mailbox. "
                            "Restart the session with the current qmlagent-launcher.");
    return {};
}

static int addLauncherReplySlack(int semanticTimeoutMs)
{
    const int boundedSemanticTimeout =
            qMin(semanticTimeoutMs,
                 std::numeric_limits<int>::max() - LauncherControlReplySlackMs);
    return boundedSemanticTimeout + LauncherControlReplySlackMs;
}

static int boundedTargetSettleMs(const QJsonObject &targetParams, const QString &settleKey)
{
    const QJsonObject settle = targetParams.value(settleKey).toObject();
    return qBound(0, settle.value(QStringLiteral("timeoutMs")).toInt(50), 30000);
}

static int launcherControlTimeoutMs(const QString &method, const QJsonObject &params,
                                    int fallbackTimeoutMs)
{
    if (method != QLatin1String("QmlAgent.request"))
        return fallbackTimeoutMs;

    const QString targetMethod = params.value(QStringLiteral("method")).toString();
    const QJsonObject targetParams = params.value(QStringLiteral("params")).toObject();

    // Mirror the per-request GUI dispatch budgets the QmlAgent service grants
    // long-running requests so the gateway does not time out one layer below
    // a request that is still legitimately running on the target GUI thread.
    int semanticTimeoutMs = -1;
    if (targetMethod == QLatin1String("UI.waitFor")) {
        semanticTimeoutMs = qMax(0, targetParams.contains(QStringLiteral("timeoutMs"))
                ? targetParams.value(QStringLiteral("timeoutMs")).toInt(fallbackTimeoutMs)
                : targetParams.value(QStringLiteral("until")).toObject()
                        .value(QStringLiteral("timeoutMs")).toInt(fallbackTimeoutMs));
    } else if (targetMethod == QLatin1String("Input.longPressNode")) {
        semanticTimeoutMs = qBound(1, targetParams.value(QStringLiteral("holdMs")).toInt(900), 10000)
                + boundedTargetSettleMs(targetParams, QStringLiteral("settle"));
    } else if (targetMethod.startsWith(QLatin1String("Input."))
               || targetMethod.startsWith(QLatin1String("Runtime."))) {
        int settleBudgetMs = boundedTargetSettleMs(targetParams, QStringLiteral("settle"));
        if (targetMethod == QLatin1String("Input.typeText"))
            settleBudgetMs += boundedTargetSettleMs(targetParams, QStringLiteral("focusSettle"));
        semanticTimeoutMs = settleBudgetMs;
    }

    if (semanticTimeoutMs < 0)
        return fallbackTimeoutMs;
    return qMax(fallbackTimeoutMs, addLauncherReplySlack(semanticTimeoutMs));
}

static bool hasLauncherControlEndpoint(const QJsonObject &metadata)
{
    return !metadata.value(QStringLiteral("controlMailbox")).toString().isEmpty();
}

static QJsonObject launcherControlEndpointSummary(const QJsonObject &metadata)
{
    const QString mailboxDir = metadata.value(QStringLiteral("controlMailbox")).toString();
    if (!mailboxDir.isEmpty()) {
        return {
            { QStringLiteral("kind"), QStringLiteral("fileMailbox") },
            { QStringLiteral("path"), mailboxDir },
        };
    }

    return {
        { QStringLiteral("kind"), QStringLiteral("none") },
    };
}

static QJsonObject launcherControlEndpointSummary(const LauncherSession &session)
{
    QJsonObject endpoint = launcherControlEndpointSummary(session.metadata);
    const QJsonObject statusEndpoint = session.status.value(QStringLiteral("controlEndpoint")).toObject();
    if (!statusEndpoint.isEmpty())
        endpoint = statusEndpoint;
    return endpoint;
}

static QList<LauncherSession> discoverLauncherSessions(int timeoutMs)
{
    QList<LauncherSession> sessions;
    QSet<QString> seenSessionIds;
    for (const QString &registryDir : launcherRegistryDirs()) {
        const QDir dir(registryDir);
        const QStringList entries = dir.entryList({ QStringLiteral("*.json") }, QDir::Files);
        for (const QString &entry : entries) {
            const QString path = dir.filePath(entry);
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                QFile::remove(path);
                continue;
            }

            const QJsonObject metadata = document.object();
            const QString id = metadata.value(QStringLiteral("launcherSession")).toString();
            if (id.isEmpty() || !hasLauncherControlEndpoint(metadata)) {
                QFile::remove(path);
                continue;
            }
            if (seenSessionIds.contains(id))
                continue;
            const qint64 launcherPid = metadata.value(QStringLiteral("launcherPid")).toInteger(-1);
            if (launcherPid > 0 && !processExists(launcherPid)) {
                QFile::remove(path);
                continue;
            }

            QString statusError;
            const QJsonObject statusResponse = sendLauncherControlRequest(
                    metadata, qMin(timeoutMs, 1000), QStringLiteral("Session.status"), {},
                    &statusError);
            QJsonObject status;
            if (statusError.isEmpty()) {
                status = statusResponse.value(QStringLiteral("result")).toObject();
                status.insert(QStringLiteral("controlReachable"), true);
            } else {
                status = {
                    { QStringLiteral("controlReachable"), false },
                    { QStringLiteral("controlError"), statusError },
                    { QStringLiteral("sessionType"), metadata.value(QStringLiteral("sessionType")) },
                    { QStringLiteral("launchCommand"), metadata.value(QStringLiteral("launchCommand")) },
                    { QStringLiteral("reloadPreviewSupported"), metadata.value(QStringLiteral("reloadPreviewSupported")) },
                };
            }
            seenSessionIds.insert(id);
            sessions.append({ id, metadata, status });
        }
    }
    return sessions;
}

static QJsonArray launcherSessionSummaries(const QList<LauncherSession> &sessions)
{
    QJsonArray summaries;
    for (const LauncherSession &session : sessions) {
        summaries.append(QJsonObject{
            { QStringLiteral("launcherSession"), session.id },
            { QStringLiteral("controlReachable"), session.status.value(QStringLiteral("controlReachable")).toBool(true) },
            { QStringLiteral("controlError"), session.status.value(QStringLiteral("controlError")) },
            { QStringLiteral("controlEndpoint"), launcherControlEndpointSummary(session) },
            { QStringLiteral("sessionType"), session.status.value(QStringLiteral("sessionType")) },
            { QStringLiteral("launchCommand"), session.status.value(QStringLiteral("launchCommand")) },
            { QStringLiteral("reloadPreviewSupported"), session.status.value(QStringLiteral("reloadPreviewSupported")) },
            { QStringLiteral("previewRoot"), session.status.value(QStringLiteral("previewRoot")) },
            { QStringLiteral("targetPid"), session.status.value(QStringLiteral("targetPid")) },
            { QStringLiteral("services"), session.status.value(QStringLiteral("services")) },
        });
    }
    return summaries;
}

static bool resolveLauncherSession(int timeoutMs, LauncherSession *selected, QString *error,
                                   const QString &wantedSessionId = QString())
{
    QList<LauncherSession> reachable;
    for (const LauncherSession &session : discoverLauncherSessions(timeoutMs)) {
        if (!session.status.value(QStringLiteral("controlReachable")).toBool(true))
            continue;
        if (!wantedSessionId.isEmpty() && !session.id.startsWith(wantedSessionId))
            continue;
        reachable.append(session);
    }
    if (reachable.isEmpty()) {
        if (!wantedSessionId.isEmpty()) {
            *error = QStringLiteral("No live qmlagent-launcher session matches --session %1. "
                                    "Run qmlagentctl sessions to list live sessions.")
                             .arg(wantedSessionId);
            return false;
        }
        *error = QStringLiteral("No live qmlagent-launcher session found.\n\n"
                                "Start one of these first:\n"
                                "  qmlagent-launcher preview <Main.qml>\n"
                                "  qmlagent-launcher app <executable> [-- args...]");
        return false;
    }
    if (reachable.size() > 1) {
        *error = QStringLiteral("Multiple live qmlagent-launcher sessions found. "
                                "Pin one by its launcherSession id (qmlagentctl --session <id>, "
                                "or the MCP tools' session argument). Sessions: %1")
                         .arg(QString::fromUtf8(QJsonDocument(launcherSessionSummaries(reachable))
                                                    .toJson(QJsonDocument::Compact)));
        return false;
    }
    *selected = reachable.first();
    return true;
}

static QJsonObject stopUnreachableLauncherFallback(const LauncherSession &session)
{
    const qint64 launcherPid = session.metadata.value(QStringLiteral("launcherPid")).toInteger(-1);
    const qint64 targetPid = session.metadata.value(QStringLiteral("targetPid")).toInteger(-1);
    const bool launcherSignalSent = terminatePid(launcherPid);
    const bool targetSignalSent = terminatePid(targetPid);
    for (int i = 0; i < 20 && (processExists(launcherPid) || processExists(targetPid)); ++i)
        QThread::msleep(50);

    return {
        { QStringLiteral("action"), QStringLiteral("stop") },
        { QStringLiteral("ok"), launcherSignalSent || targetSignalSent },
        { QStringLiteral("mode"), QStringLiteral("process-fallback") },
        { QStringLiteral("reason"), QStringLiteral("launcher control channel was not reachable") },
        { QStringLiteral("launcherPid"), launcherPid },
        { QStringLiteral("targetPid"), targetPid },
        { QStringLiteral("launcherSignalSent"), launcherSignalSent },
        { QStringLiteral("targetSignalSent"), targetSignalSent },
        { QStringLiteral("launcherStillRunning"), processExists(launcherPid) },
        { QStringLiteral("targetStillRunning"), processExists(targetPid) },
    };
}

static bool resolveLauncherSessionForStop(int timeoutMs, LauncherSession *selected,
                                          QJsonObject *fallbackResult, QString *error,
                                          const QString &wantedSessionId = QString())
{
    QList<LauncherSession> sessions = discoverLauncherSessions(timeoutMs);
    if (!wantedSessionId.isEmpty()) {
        QList<LauncherSession> matching;
        for (const LauncherSession &session : sessions) {
            if (session.id.startsWith(wantedSessionId))
                matching.append(session);
        }
        sessions = matching;
    }
    QList<LauncherSession> reachable;
    for (const LauncherSession &session : sessions) {
        if (session.status.value(QStringLiteral("controlReachable")).toBool(true))
            reachable.append(session);
    }

    if (reachable.size() == 1) {
        *selected = reachable.first();
        return true;
    }
    if (reachable.size() > 1) {
        *error = QStringLiteral("Multiple live qmlagent-launcher sessions found. "
                                "Pin one with --session <id>. Sessions: %1")
                         .arg(QString::fromUtf8(QJsonDocument(launcherSessionSummaries(reachable))
                                                    .toJson(QJsonDocument::Compact)));
        return false;
    }

    QList<LauncherSession> processBacked;
    for (const LauncherSession &session : sessions) {
        const qint64 launcherPid = session.metadata.value(QStringLiteral("launcherPid")).toInteger(-1);
        const qint64 targetPid = session.metadata.value(QStringLiteral("targetPid")).toInteger(-1);
        if (processExists(launcherPid) || processExists(targetPid))
            processBacked.append(session);
    }
    if (processBacked.size() == 1) {
        *fallbackResult = stopUnreachableLauncherFallback(processBacked.first());
        return false;
    }

    if (sessions.isEmpty()) {
        *error = QStringLiteral("No live qmlagent-launcher session found.\n\n"
                                "Start one of these first:\n"
                                "  qmlagent-launcher preview <Main.qml>\n"
                                "  qmlagent-launcher app <executable> [-- args...]");
    } else {
        *error = QStringLiteral("No reachable qmlagent-launcher session found. Sessions: %1")
                         .arg(QString::fromUtf8(QJsonDocument(launcherSessionSummaries(sessions))
                                                    .toJson(QJsonDocument::Compact)));
    }
    return false;
}

static void printJsonObject(const QJsonObject &object, const QString &format)
{
    const QJsonDocument::JsonFormat jsonFormat = format == QLatin1String("compact")
            ? QJsonDocument::Compact
            : QJsonDocument::Indented;
    QTextStream stream(stdout);
    stream << QString::fromUtf8(QJsonDocument(object).toJson(jsonFormat));
    if (jsonFormat == QJsonDocument::Compact)
        stream << Qt::endl;
}

static QString argumentValue(const QStringList &arguments, const QString &name,
                             const QString &defaultValue = {})
{
    const int index = arguments.indexOf(name);
    if (index >= 0 && index + 1 < arguments.size())
        return arguments.at(index + 1);
    return defaultValue;
}

// Static fallback for when no live session is reachable. Keep in sync with
// agentMethods() in qqmlagentservice.cpp; a live session reports the
// authoritative list through Session.getInfo "features" (which also omits
// Runtime.* methods while runtime mutation is disabled).
static QStringList qmlAgentProtocolMethods()
{
    return {
        QStringLiteral("Session.getInfo"),
        QStringLiteral("Session.configure"),
        QStringLiteral("Session.reset"),
        QStringLiteral("Log.enable"),
        QStringLiteral("Log.getEntries"),
        QStringLiteral("Log.clear"),
        QStringLiteral("UI.getTree"),
        QStringLiteral("UI.query"),
        QStringLiteral("UI.queryMany"),
        QStringLiteral("UI.waitFor"),
        QStringLiteral("UI.describeNode"),
        QStringLiteral("UI.getBoxModel"),
        QStringLiteral("UI.subscribe"),
        QStringLiteral("UI.unsubscribe"),
        QStringLiteral("Diagnostics.analyzeNode"),
        QStringLiteral("Diagnostics.analyzeTree"),
        QStringLiteral("Diagnostics.analyzeBinding"),
        QStringLiteral("Input.clickNode"),
        QStringLiteral("Input.longPressNode"),
        QStringLiteral("Input.wheel"),
        QStringLiteral("Input.scrollIntoView"),
        QStringLiteral("Input.focusNode"),
        QStringLiteral("Input.dispatchMouseEvent"),
        QStringLiteral("Input.dragNode"),
        QStringLiteral("Input.dispatchTouchEvent"),
        QStringLiteral("Input.dispatchKeyEvent"),
        QStringLiteral("Input.typeText"),
        QStringLiteral("Input.dismissPopup"),
        QStringLiteral("Render.captureScreenshot"),
        QStringLiteral("Render.pick3D"),
        QStringLiteral("Runtime.setProperty"),
        QStringLiteral("Runtime.invokeMethod"),
        QStringLiteral("Source.resolveNode"),
    };
}

static void printProtocolMethods(QTextStream &stream)
{
    stream << "QmlAgent protocol methods (static list; run qmlagentctl methods "
              "with a live session for the authoritative set):\n";
    for (const QString &method : qmlAgentProtocolMethods())
        stream << "  " << method << '\n';
}

static void printCallHelp()
{
    QTextStream stream(stdout);
    stream << "Usage:\n"
           << "  qmlagentctl call <Method.Name> --params '{...}' [--format compact|pretty]\n\n";
    printProtocolMethods(stream);
    stream << "\nExamples:\n"
           << "  qmlagentctl call UI.query --params '{\"selector\":\"id=\\\"saveButton\\\"\"}'\n"
           << "  qmlagentctl call UI.query --params '{\"selector\":\"objectName=\\\"probe.cube\\\"\",\"fields\":[\"render3D\"]}'\n"
           << "  qmlagentctl call UI.queryMany --params '{\"queries\":[{\"selector\":\"id=\\\"saveButton\\\"\"},{\"selector\":\"id=\\\"statusLabel\\\"\",\"properties\":[\"text\"]}]}'\n"
           << "  qmlagentctl call Input.scrollIntoView --params '{\"selector\":\"id=\\\"saveButton\\\"\"}'\n"
           << "  qmlagentctl call UI.waitFor --params '{\"selector\":\"id=\\\"popup\\\"\",\"until\":{\"state\":\"found\"}}'\n"
           << "  qmlagentctl call Diagnostics.analyzeBinding --params '{\"selector\":\"id=\\\"panel\\\"\",\"property\":\"x\"}'\n"
           << "  qmlagentctl call Render.pick3D --params '{\"selector\":\"id=\\\"view3d\\\"\",\"modelSelector\":\"id=\\\"cube\\\"\",\"x\":160,\"y\":120,\"coordinateSpace\":\"auto\"}'\n"
           << "  qmlagentctl call Render.captureScreenshot --params '{\"scale\":0.5}'\n"
           << "\nScreenshot data is omitted by default; pass includeData:true only when\n"
           << "PNG bytes are needed, and prefer scale/region to bound them.\n";
}

static void printMethodsHelp(const QStringList &methods, const QString &origin,
                             const QString &versionWarning)
{
    QTextStream stream(stdout);
    stream << "Usage:\n"
           << "  qmlagentctl methods\n"
           << "  qmlagentctl capabilities\n\n";
    stream << "QmlAgent protocol methods (" << origin << "):\n";
    for (const QString &method : methods)
        stream << "  " << method << '\n';
    if (!versionWarning.isEmpty())
        stream << '\n' << versionWarning << '\n';
    stream << "\nHigh-leverage methods for agent loops:\n"
           << "  UI.queryMany             batch several selector/property reads\n"
           << "  Input.scrollIntoView     recover from center_outside_viewport on clipped content\n"
           << "  UI.getTree / UI.query    include QtQuick3D View3D scene nodes automatically when capabilities.qtQuick3D.enabled is true\n"
           << "  UI.query fields=render3D  Quick3D model world bounds, projected screen bounds, distance, and frustum evidence\n"
           << "  Diagnostics checks=quick3D  Quick3D camera/light/material/scale/texture/frustum evidence\n"
           << "  Render.pick3D            read-only View3D hit-test evidence for a viewport coordinate\n"
           << "\nMCP/tool equivalents:\n"
           << "  qmlagent_ui_query_many\n"
           << "  qmlagent_input_scroll_into_view\n"
           << "  qmlagent_ui_get_tree / qmlagent_ui_query for QtQuick3D structural/source evidence; request fields=[\"render3D\"] for model projection evidence\n"
           << "  qmlagent_diagnostics_analyze_tree / qmlagent_diagnostics_analyze_node with checks=[\"quick3D\"] for Quick3D issue evidence\n"
           << "  qmlagent_render_pick3d for read-only View3D hit-test evidence\n";
}

static int runCtlSubcommand(const QStringList &arguments)
{
    if (arguments.size() < 2 || arguments.at(1) == QLatin1String("help")
            || arguments.at(1) == QLatin1String("--help")) {
        QTextStream(stdout)
                << "Usage:\n"
                << "  qmlagentctl sessions\n"
                << "  qmlagentctl status\n"
                << "  qmlagentctl methods\n"
                << "  qmlagentctl query <selector> [--property name] [--format compact|pretty]\n"
                << "  qmlagentctl query-many --params '{\"queries\":[...]}' [--format compact|pretty]\n"
                << "  qmlagentctl binding <selector> --property name [--format compact|pretty]\n"
                << "  qmlagentctl click <selector>\n"
                << "  qmlagentctl scroll-into-view <selector>\n"
                << "  qmlagentctl long-press <selector> [--hold-ms ms]\n"
                << "  qmlagentctl drag <selector> --dx px [--dy px] [--steps n]\n"
                << "  qmlagentctl wheel <selector> [--dx delta] [--dy delta]\n"
                << "  qmlagentctl type <selector> --text value\n"
                << "  qmlagentctl clear-text <selector>\n"
                << "  qmlagentctl dismiss-popup [--all]\n"
                << "  qmlagentctl wait <selector> --state found|notFound [--timeout ms]\n"
                << "  qmlagentctl pick3d <view3d-selector> --x px --y px [--model-selector selector] [--coordinate-space auto|local|window]\n"
                << "  qmlagentctl screenshot [--window-id n] [--scale 0.5] [--region x,y,w,h] [--include-data] [--out file.png]\n"
                << "  qmlagentctl reload-preview\n"
                << "  qmlagentctl stop\n"
                << "  qmlagentctl call <Method.Name> --params '{...}'\n\n"
                << "QtQuick3D: when capabilities.qtQuick3D.enabled is true, UI.getTree and\n"
                << "UI.query include View3D scene frontend nodes automatically. These nodes\n"
                << "are structural/source evidence, not QQuickItem click targets. Request\n"
                << "fields=[\"render3D\"] on Model nodes for world bounds, projected screen\n"
                << "bounds, camera distance, and approximate frustum evidence. Use\n"
                << "qmlagentctl pick3d <view3d-selector> --x px --y px for read-only\n"
                << "View3D hit-test evidence. The default coordinate space is auto:\n"
                << "View3D-local first, then window logical pixels when the point lies\n"
                << "inside the View3D. Pass --model-selector to use the targeted\n"
                << "View3D.pick(x,y,model) overload.\n\n"
                << "Screenshot is fallback visual evidence. Use structural UI,\n"
                << "diagnostics, source, log, and input/workflow evidence first;\n"
                << "--include-data is opt-in to preserve agent context.\n"
                << "--scale/--region keep fallback visual bytes bounded.\n"
                << "--out writes PNG bytes to a file without printing base64 data.\n"
                << "If a click/read reports center_outside_viewport for an instantiated clipped target,\n"
                << "run qmlagentctl scroll-into-view <selector>, then retry the action or query.\n";
        return 0;
    }

    const QString command = arguments.at(1);

    const QString format = argumentValue(arguments, QStringLiteral("--format"), QStringLiteral("pretty"));
    const QString wantedSession = argumentValue(arguments, QStringLiteral("--session"));
    bool ok = false;
    const int timeoutMs = argumentValue(arguments, QStringLiteral("--timeout"), QStringLiteral("5000")).toInt(&ok);
    if (!ok || timeoutMs <= 0)
        return fail(QStringLiteral("--timeout must be a positive integer."));

    if (command == QLatin1String("methods") || command == QLatin1String("capabilities")) {
        // Prefer the live session's Session.getInfo "features": it is the
        // authoritative method list for the attached plugin build and omits
        // methods that are currently disabled (Runtime.* without mutation).
        // The static list is the offline fallback.
        QStringList methods = qmlAgentProtocolMethods();
        QString origin = QStringLiteral("static list, no live session reachable");
        QString versionWarning;
        LauncherSession methodsLauncher;
        QString methodsError;
        if (resolveLauncherSession(timeoutMs, &methodsLauncher, &methodsError, wantedSession)) {
            const QJsonObject controlParams{
                { QStringLiteral("method"), QStringLiteral("Session.getInfo") },
                { QStringLiteral("params"), QJsonObject{} },
            };
            QString controlError;
            const QJsonObject response = sendLauncherControlRequest(
                    methodsLauncher.metadata,
                    launcherControlTimeoutMs(QStringLiteral("QmlAgent.request"), controlParams,
                                             timeoutMs),
                    QStringLiteral("QmlAgent.request"), controlParams, &controlError);
            const QJsonObject info = response.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("result")).toObject();
            const QJsonArray features = info.value(QStringLiteral("features")).toArray();
            if (controlError.isEmpty() && !features.isEmpty()) {
                methods.clear();
                for (const QJsonValue &feature : features)
                    methods.append(feature.toString());
                const QString remoteVersion =
                        info.value(QStringLiteral("protocolVersion")).toString();
                origin = QStringLiteral("live session %1, protocolVersion %2")
                                 .arg(methodsLauncher.metadata
                                              .value(QStringLiteral("launcherSession")).toString(),
                                      remoteVersion);
                if (!remoteVersion.isEmpty()
                        && remoteVersion != SupportedAgentProtocolVersion) {
                    versionWarning = QStringLiteral(
                            "Warning: target reports protocolVersion %1 but this qmlagentctl "
                            "supports %2; methods or result shapes may differ.")
                                             .arg(remoteVersion, SupportedAgentProtocolVersion);
                }
            }
        }
        printMethodsHelp(methods, origin, versionWarning);
        return 0;
    }

    if (command == QLatin1String("sessions")) {
        const QList<LauncherSession> sessions = discoverLauncherSessions(timeoutMs);
        const QJsonArray recentExits = recentLauncherExitReports();
        printJsonObject({
            { QStringLiteral("ok"), true },
            { QStringLiteral("sessionCount"), sessions.size() },
            { QStringLiteral("sessions"), launcherSessionSummaries(sessions) },
            { QStringLiteral("recentExitCount"), recentExits.size() },
            { QStringLiteral("recentExits"), recentExits },
        }, format);
        return 0;
    }

    static const QSet<QString> launcherCommands{
        QStringLiteral("status"),
        QStringLiteral("stop"),
        QStringLiteral("reload-preview"),
        QStringLiteral("call"),
        QStringLiteral("query"),
        QStringLiteral("query-many"),
        QStringLiteral("inspect"),
        QStringLiteral("binding"),
        QStringLiteral("click"),
        QStringLiteral("scroll-into-view"),
        QStringLiteral("long-press"),
        QStringLiteral("drag"),
        QStringLiteral("wheel"),
        QStringLiteral("type"),
        QStringLiteral("clear-text"),
        QStringLiteral("dismiss-popup"),
        QStringLiteral("wait"),
        QStringLiteral("screenshot"),
    };
    if (!launcherCommands.contains(command))
        return fail(QStringLiteral("Unknown qmlagentctl command '%1'. Run qmlagentctl help.").arg(command));

    if (command == QLatin1String("call")
            && (arguments.size() < 3 || arguments.at(2) == QLatin1String("--help")
                || arguments.at(2) == QLatin1String("help"))) {
        printCallHelp();
        return 0;
    }

    LauncherSession launcher;
    QString launcherError;
    QJsonObject stopFallbackResult;
    if (command == QLatin1String("stop")) {
        if (!resolveLauncherSessionForStop(timeoutMs, &launcher, &stopFallbackResult,
                                           &launcherError, wantedSession)) {
            if (!stopFallbackResult.isEmpty()) {
                printJsonObject(stopFallbackResult, format);
                return stopFallbackResult.value(QStringLiteral("ok")).toBool(false) ? 0 : 1;
            }
            return fail(launcherError);
        }
    } else if (!resolveLauncherSession(timeoutMs, &launcher, &launcherError, wantedSession)) {
        return fail(launcherError);
    }


    QString controlMethod;
    QJsonObject controlParams;
    QString screenshotOutputPath;
    bool screenshotKeepDataInOutput = false;
    if (command == QLatin1String("status")) {
        controlMethod = QStringLiteral("Session.status");
    } else if (command == QLatin1String("stop")) {
        controlMethod = QStringLiteral("Session.stop");
    } else if (command == QLatin1String("reload-preview")) {
        controlMethod = QStringLiteral("Preview.reload");
        controlParams = {
            { QStringLiteral("timeoutMs"), timeoutMs },
        };
    } else {
        QString method;
        QJsonObject params;
        if (command == QLatin1String("call")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl call requires a JSON-RPC method name."));
            method = arguments.at(2);
            const QByteArray paramsBytes = argumentValue(arguments, QStringLiteral("--params"),
                                                        QStringLiteral("{}")).toUtf8();
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(paramsBytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return fail(QStringLiteral("--params must be a JSON object."));
            params = document.object();
        } else if (command == QLatin1String("query-many")) {
            if (!arguments.contains(QStringLiteral("--params")))
                return fail(QStringLiteral("qmlagentctl query-many requires --params JSON."));
            method = QStringLiteral("UI.queryMany");
            const QByteArray paramsBytes = argumentValue(arguments, QStringLiteral("--params")).toUtf8();
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(paramsBytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return fail(QStringLiteral("--params must be a JSON object."));
            params = document.object();
        } else if (command == QLatin1String("query") || command == QLatin1String("inspect")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl %1 requires a selector.").arg(command));
            method = QStringLiteral("UI.query");
            QJsonArray properties;
            const QString property = argumentValue(arguments, QStringLiteral("--property"));
            if (!property.isEmpty())
                properties.append(property);
            params = {
                { QStringLiteral("selector"), arguments.at(2) },
                { QStringLiteral("includeSource"), command == QLatin1String("inspect") },
                { QStringLiteral("properties"), properties },
            };
            if (!properties.isEmpty())
                params.insert(QStringLiteral("fields"), QJsonArray{
                    QStringLiteral("qmlId"),
                    QStringLiteral("objectName"),
                    QStringLiteral("kind"),
                    QStringLiteral("sceneKind"),
                    QStringLiteral("properties"),
                });
        } else if (command == QLatin1String("binding")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl binding requires a selector."));
            const QString property = argumentValue(arguments, QStringLiteral("--property"));
            if (property.isEmpty())
                return fail(QStringLiteral("qmlagentctl binding requires --property name."));
            method = QStringLiteral("Diagnostics.analyzeBinding");
            params = {
                { QStringLiteral("selector"), arguments.at(2) },
                { QStringLiteral("property"), property },
            };
        } else if (command == QLatin1String("click")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl click requires a selector."));
            method = QStringLiteral("Input.clickNode");
            params = { { QStringLiteral("selector"), arguments.at(2) } };
            if (arguments.contains(QStringLiteral("--px"))
                    || arguments.contains(QStringLiteral("--py"))) {
                bool pxOk = true;
                bool pyOk = true;
                const double px = argumentValue(arguments, QStringLiteral("--px"),
                                                QStringLiteral("0")).toDouble(&pxOk);
                const double py = argumentValue(arguments, QStringLiteral("--py"),
                                                QStringLiteral("0")).toDouble(&pyOk);
                if (!pxOk || !pyOk)
                    return fail(QStringLiteral("--px and --py must be numbers "
                                               "(item-local click point)."));
                params.insert(QStringLiteral("point"), QJsonArray{ px, py });
            }
        } else if (command == QLatin1String("scroll-into-view")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl scroll-into-view requires a selector."));
            method = QStringLiteral("Input.scrollIntoView");
            params = { { QStringLiteral("selector"), arguments.at(2) } };
        } else if (command == QLatin1String("long-press")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl long-press requires a selector."));
            bool holdOk = false;
            const int holdMs = argumentValue(arguments, QStringLiteral("--hold-ms"),
                                             QStringLiteral("900")).toInt(&holdOk);
            if (!holdOk || holdMs <= 0)
                return fail(QStringLiteral("--hold-ms must be a positive integer."));
            method = QStringLiteral("Input.longPressNode");
            params = {
                { QStringLiteral("selector"), arguments.at(2) },
                { QStringLiteral("holdMs"), holdMs },
            };
        } else if (command == QLatin1String("drag")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl drag requires a selector."));
            if (!arguments.contains(QStringLiteral("--dx"))
                    && !arguments.contains(QStringLiteral("--dy"))) {
                return fail(QStringLiteral("qmlagentctl drag requires --dx and/or --dy "
                                           "(pixels from the target center)."));
            }
            bool dxOk = true;
            bool dyOk = true;
            const double dx = argumentValue(arguments, QStringLiteral("--dx"),
                                            QStringLiteral("0")).toDouble(&dxOk);
            const double dy = argumentValue(arguments, QStringLiteral("--dy"),
                                            QStringLiteral("0")).toDouble(&dyOk);
            if (!dxOk || !dyOk)
                return fail(QStringLiteral("--dx and --dy must be numbers."));
            method = QStringLiteral("Input.dragNode");
            params = {
                { QStringLiteral("selector"), arguments.at(2) },
                { QStringLiteral("delta"), QJsonArray{ dx, dy } },
            };
            if (arguments.contains(QStringLiteral("--steps"))) {
                bool stepsOk = false;
                const int steps = argumentValue(arguments, QStringLiteral("--steps")).toInt(&stepsOk);
                if (!stepsOk || steps <= 0)
                    return fail(QStringLiteral("--steps must be a positive integer."));
                params.insert(QStringLiteral("steps"), steps);
            }
        } else if (command == QLatin1String("wheel")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl wheel requires a selector."));
            if (!arguments.contains(QStringLiteral("--dx"))
                    && !arguments.contains(QStringLiteral("--dy"))) {
                return fail(QStringLiteral("qmlagentctl wheel requires --dx and/or --dy "
                                           "(wheel delta; negative --dy scrolls down)."));
            }
            bool dxOk = true;
            bool dyOk = true;
            const int dx = argumentValue(arguments, QStringLiteral("--dx"),
                                         QStringLiteral("0")).toInt(&dxOk);
            const int dy = argumentValue(arguments, QStringLiteral("--dy"),
                                         QStringLiteral("0")).toInt(&dyOk);
            if (!dxOk || !dyOk)
                return fail(QStringLiteral("--dx and --dy must be integers."));
            method = QStringLiteral("Input.wheel");
            params = { { QStringLiteral("selector"), arguments.at(2) } };
            if (arguments.contains(QStringLiteral("--dx")))
                params.insert(QStringLiteral("deltaX"), dx);
            if (arguments.contains(QStringLiteral("--dy")))
                params.insert(QStringLiteral("deltaY"), dy);
        } else if (command == QLatin1String("type")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl type requires a selector."));
            if (!arguments.contains(QStringLiteral("--text")))
                return fail(QStringLiteral("qmlagentctl type requires --text value."));
            const QString text = argumentValue(arguments, QStringLiteral("--text"));
            method = QStringLiteral("Input.typeText");
            params = {
                { QStringLiteral("selector"), arguments.at(2) },
                { QStringLiteral("text"), text },
            };
        } else if (command == QLatin1String("clear-text")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl clear-text requires a selector."));
            method = QStringLiteral("Input.typeText");
            params = {
                { QStringLiteral("selector"), arguments.at(2) },
                { QStringLiteral("text"), QString() },
                { QStringLiteral("replaceExisting"), true },
            };
        } else if (command == QLatin1String("dismiss-popup")) {
            method = QStringLiteral("Input.dismissPopup");
            if (arguments.contains(QStringLiteral("--all")))
                params = { { QStringLiteral("all"), true } };
        } else if (command == QLatin1String("pick3d")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl pick3d requires a View3D selector."));
            bool xOk = false;
            bool yOk = false;
            const double x = argumentValue(arguments, QStringLiteral("--x")).toDouble(&xOk);
            const double y = argumentValue(arguments, QStringLiteral("--y")).toDouble(&yOk);
            if (!xOk || !yOk)
                return fail(QStringLiteral("qmlagentctl pick3d requires numeric --x and --y values."));
            method = QStringLiteral("Render.pick3D");
            params = {
                { QStringLiteral("selector"), arguments.at(2) },
                { QStringLiteral("x"), x },
                { QStringLiteral("y"), y },
            };
            const QString coordinateSpace = argumentValue(arguments, QStringLiteral("--coordinate-space"));
            if (!coordinateSpace.isEmpty())
                params.insert(QStringLiteral("coordinateSpace"), coordinateSpace);
            const QString modelSelector = argumentValue(arguments, QStringLiteral("--model-selector"));
            if (!modelSelector.isEmpty())
                params.insert(QStringLiteral("modelSelector"), modelSelector);
        } else if (command == QLatin1String("wait")) {
            if (arguments.size() < 3)
                return fail(QStringLiteral("qmlagentctl wait requires a selector."));
            method = QStringLiteral("UI.waitFor");
            params = {
                { QStringLiteral("selector"), arguments.at(2) },
                { QStringLiteral("until"), QJsonObject{
                    { QStringLiteral("state"), argumentValue(arguments, QStringLiteral("--state"), QStringLiteral("found")) },
                } },
                { QStringLiteral("timeoutMs"), timeoutMs },
            };
        } else if (command == QLatin1String("screenshot")) {
            method = QStringLiteral("Render.captureScreenshot");
            const QString windowId = argumentValue(arguments, QStringLiteral("--window-id"));
            screenshotOutputPath = argumentValue(arguments, QStringLiteral("--out"));
            screenshotKeepDataInOutput = arguments.contains(QStringLiteral("--include-data"));
            const QString scaleText = argumentValue(arguments, QStringLiteral("--scale"));
            if (!scaleText.isEmpty()) {
                bool scaleOk = false;
                const double scale = scaleText.toDouble(&scaleOk);
                if (!scaleOk || scale <= 0.0 || scale > 1.0)
                    return fail(QStringLiteral("--scale must be a number in (0, 1]."));
                params.insert(QStringLiteral("scale"), scale);
            }
            const QString regionText = argumentValue(arguments, QStringLiteral("--region"));
            if (!regionText.isEmpty()) {
                const QStringList parts = regionText.split(QLatin1Char(','));
                if (parts.size() != 4)
                    return fail(QStringLiteral("--region must be x,y,width,height."));
                bool regionOk = true;
                QJsonArray values;
                for (const QString &part : parts) {
                    bool valueOk = false;
                    const double value = part.trimmed().toDouble(&valueOk);
                    regionOk = regionOk && valueOk;
                    values.append(value);
                }
                if (!regionOk || values.at(2).toDouble() <= 0.0 || values.at(3).toDouble() <= 0.0)
                    return fail(QStringLiteral("--region must contain positive width and height."));
                params.insert(QStringLiteral("region"), QJsonObject{
                    { QStringLiteral("x"), values.at(0) },
                    { QStringLiteral("y"), values.at(1) },
                    { QStringLiteral("width"), values.at(2) },
                    { QStringLiteral("height"), values.at(3) },
                });
            }
            if (!windowId.isEmpty()) {
                bool windowOk = false;
                const int id = windowId.toInt(&windowOk);
                if (!windowOk || id <= 0)
                    return fail(QStringLiteral("--window-id must be a positive integer."));
                params.insert(QStringLiteral("windowId"), id);
            }
            params.insert(QStringLiteral("includeData"),
                          !screenshotOutputPath.isEmpty() || screenshotKeepDataInOutput);
        }

        controlMethod = QStringLiteral("QmlAgent.request");
        controlParams = {
            { QStringLiteral("method"), method },
            { QStringLiteral("params"), params },
        };
    }

    QString controlError;
    const int controlTimeoutMs = launcherControlTimeoutMs(controlMethod, controlParams, timeoutMs);
    const QJsonObject response = sendLauncherControlRequest(launcher.metadata, controlTimeoutMs,
                                                            controlMethod, controlParams,
                                                            &controlError);
    if (!controlError.isEmpty())
        return fail(controlError);

    QJsonObject result = response.value(QStringLiteral("result")).toObject();
    if (command == QLatin1String("status")) {
        const QJsonObject infoParams{
            { QStringLiteral("method"), QStringLiteral("Session.getInfo") },
            { QStringLiteral("params"), QJsonObject{} },
        };
        QString infoError;
        const QJsonObject infoResponse = sendLauncherControlRequest(
                launcher.metadata,
                launcherControlTimeoutMs(QStringLiteral("QmlAgent.request"), infoParams, timeoutMs),
                QStringLiteral("QmlAgent.request"),
                infoParams,
                &infoError);
        const QJsonObject targetResponse = infoResponse.value(QStringLiteral("result")).toObject();
        const QJsonObject sessionInfo = targetResponse.value(QStringLiteral("result")).toObject();
        if (!sessionInfo.isEmpty()) {
            result.insert(QStringLiteral("targetSessionInfo"), sessionInfo);
            const QJsonObject capabilities = sessionInfo.value(QStringLiteral("capabilities")).toObject();
            if (!capabilities.isEmpty())
                result.insert(QStringLiteral("capabilities"), capabilities);
            const QJsonArray features = sessionInfo.value(QStringLiteral("features")).toArray();
            if (!features.isEmpty())
                result.insert(QStringLiteral("features"), features);
        } else if (!infoError.isEmpty()) {
            result.insert(QStringLiteral("targetSessionInfoAvailable"), false);
            result.insert(QStringLiteral("targetSessionInfoError"), infoError);
        } else if (targetResponse.contains(QStringLiteral("error"))) {
            result.insert(QStringLiteral("targetSessionInfoAvailable"), false);
            result.insert(QStringLiteral("targetSessionInfoError"), targetResponse.value(QStringLiteral("error")));
        }
    }
    if (command == QLatin1String("screenshot") && !screenshotOutputPath.isEmpty()) {
        QJsonObject screenshot = result.value(QStringLiteral("result")).toObject();
        if (!screenshot.value(QStringLiteral("captured")).toBool(false))
            return fail(QStringLiteral("Screenshot capture failed: %1")
                                .arg(screenshot.value(QStringLiteral("reason")).toString(
                                        QStringLiteral("unknown"))));

        const QString data = screenshot.value(QStringLiteral("data")).toString();
        if (data.isEmpty())
            return fail(QStringLiteral("Screenshot response did not include PNG data."));

        const QByteArray png = QByteArray::fromBase64(data.toLatin1());
        if (png.isEmpty())
            return fail(QStringLiteral("Screenshot response contained invalid PNG data."));

        const QFileInfo outputInfo(screenshotOutputPath);
        const QDir outputDir = outputInfo.absoluteDir();
        if (!outputDir.exists() && !QDir().mkpath(outputDir.absolutePath()))
            return fail(QStringLiteral("Failed to create screenshot output directory '%1'.")
                                .arg(outputDir.absolutePath()));

        QFile file(screenshotOutputPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(QStringLiteral("Failed to open screenshot output '%1': %2")
                                .arg(screenshotOutputPath, file.errorString()));
        if (file.write(png) != png.size())
            return fail(QStringLiteral("Failed to write screenshot output '%1': %2")
                                .arg(screenshotOutputPath, file.errorString()));
        file.close();

        screenshot.insert(QStringLiteral("writtenTo"), outputInfo.absoluteFilePath());
        screenshot.insert(QStringLiteral("bytesWritten"), png.size());
        if (!screenshotKeepDataInOutput) {
            screenshot.remove(QStringLiteral("data"));
            screenshot.insert(QStringLiteral("dataOmitted"), true);
        }
        result.insert(QStringLiteral("result"), screenshot);
    }

    printJsonObject(result.isEmpty() ? response : result, format);
    const QJsonObject error = result.value(QStringLiteral("error")).toObject();
    if (!error.isEmpty())
        return 2;
    return 0;
}

static QJsonObject groupedWorkflowReport(const QString &kind,
                                         const QString &targetSelector,
                                         const QString &key,
                                         const QString &expectedSelector,
                                         const Expectation &expectation,
                                         const QJsonObject &targetQuery,
                                         const QJsonObject &beforeQuery,
                                         const QJsonObject &input,
                                         const QJsonArray &events,
                                         const QJsonObject &afterQuery)
{
    int beforeMatchCount = 0;
    int afterMatchCount = 0;
    QJsonObject beforeNode;
    QJsonObject afterNode;
    const QJsonValue beforeActual = valueForExpectation(beforeQuery, expectation,
                                                        &beforeMatchCount, &beforeNode);
    const QJsonValue afterActual = valueForExpectation(afterQuery, expectation,
                                                       &afterMatchCount, &afterNode);
    QJsonObject verification = verificationObject(afterQuery, expectation);
    verification.insert(QStringLiteral("before"), beforeActual);
    verification.insert(QStringLiteral("after"), afterActual);
    verification.insert(QStringLiteral("beforeMatchCount"), beforeMatchCount);

    const QJsonObject inputResult = input.value(QStringLiteral("result")).toObject();
    const bool delivered = inputResult.value(QStringLiteral("delivered")).toBool();
    const bool passed = verification.value(QStringLiteral("passed")).toBool();
    const bool unchanged = beforeActual == afterActual;

    QJsonArray issues;
    if (!delivered) {
        const QJsonArray diagnostics = inputResult.value(QStringLiteral("diagnostics")).toArray();
        for (const QJsonValue &diagnostic : diagnostics) {
            QJsonObject issue = diagnostic.toObject();
            issue.insert(QStringLiteral("phase"), QStringLiteral("input"));
            issues.append(issue);
        }
    }

    if (delivered && !passed && unchanged) {
        QJsonArray evidence{
            QStringLiteral("input.delivered=true"),
            QStringLiteral("events=%1").arg(events.size()),
            QStringLiteral("expected %1%2%3")
                    .arg(expectation.property, expectation.op,
                         jsonValueToSummaryString(expectation.value)),
            QStringLiteral("before.%1=%2")
                    .arg(expectation.property, jsonValueToSummaryString(beforeActual)),
            QStringLiteral("after.%1=%2")
                    .arg(expectation.property, jsonValueToSummaryString(afterActual)),
        };

        QJsonObject issue{
            { QStringLiteral("id"), QStringLiteral("state.no_change_after_action") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 0.95 },
            { QStringLiteral("message"),
              QStringLiteral("Input was delivered but the expected post-action state did not change.") },
            { QStringLiteral("evidence"), evidence },
        };

        const QJsonArray targetMatches = targetQuery.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("matches")).toArray();
        if (!targetMatches.isEmpty()) {
            const QJsonObject targetNode = targetMatches.first().toObject();
            issue.insert(QStringLiteral("targetNodeId"),
                         targetNode.value(QStringLiteral("nodeId")).toInt(-1));
        }
        if (!afterNode.isEmpty()) {
            issue.insert(QStringLiteral("nodeId"),
                         afterNode.value(QStringLiteral("nodeId")).toInt(-1));
            issue.insert(QStringLiteral("affectedNodes"), QJsonArray{ afterNode });
        }
        issues.append(issue);
    }

    QJsonObject report{
        { QStringLiteral("kind"), kind },
        { QStringLiteral("targetSelector"), targetSelector },
        { QStringLiteral("expectedSelector"), expectedSelector },
        { QStringLiteral("expectation"), QJsonObject{
            { QStringLiteral("property"), expectation.property },
            { QStringLiteral("operator"), expectation.op },
            { QStringLiteral("expected"), expectation.value },
        } },
        { QStringLiteral("target"), targetQuery },
        { QStringLiteral("before"), beforeQuery },
        { QStringLiteral("input"), inputResult },
        { QStringLiteral("events"), events },
        { QStringLiteral("after"), afterQuery },
        { QStringLiteral("verification"), verification },
        { QStringLiteral("issues"), issues },
    };
    if (!key.isEmpty())
        report.insert(QStringLiteral("key"), key);
    return report;
}

static QJsonObject clickAndWaitWorkflowReport(const QString &kind,
                                              const QString &targetSelector,
                                              const QString &waitSelector,
                                              const QJsonObject &until,
                                              const QJsonObject &targetQuery,
                                              const QJsonObject &input,
                                              const QJsonArray &events,
                                              const QJsonObject &wait)
{
    const QJsonObject inputResult = input.value(QStringLiteral("result")).toObject();
    const bool delivered = inputResult.value(QStringLiteral("delivered")).toBool();
    const QJsonObject waitResult = wait.value(QStringLiteral("result")).toObject();
    const bool waitPassed = waitResult.value(QStringLiteral("ok")).toBool();

    QJsonObject verification{
        { QStringLiteral("passed"), delivered && waitPassed },
        { QStringLiteral("waitOk"), waitPassed },
        { QStringLiteral("timedOut"), waitResult.value(QStringLiteral("timedOut")).toBool() },
        { QStringLiteral("reason"), waitResult.value(QStringLiteral("reason")).toString() },
    };

    QJsonArray issues;
    if (!delivered) {
        const QJsonArray diagnostics = inputResult.value(QStringLiteral("diagnostics")).toArray();
        for (const QJsonValue &diagnostic : diagnostics) {
            QJsonObject issue = diagnostic.toObject();
            issue.insert(QStringLiteral("phase"), QStringLiteral("input"));
            issues.append(issue);
        }
    }
    if (delivered && !waitPassed) {
        QJsonObject issue{
            { QStringLiteral("id"), QStringLiteral("workflow.wait_not_satisfied") },
            { QStringLiteral("severity"), QStringLiteral("error") },
            { QStringLiteral("confidence"), 0.95 },
            { QStringLiteral("phase"), QStringLiteral("wait") },
            { QStringLiteral("message"),
              QStringLiteral("Input was delivered but the expected wait predicate was not satisfied.") },
            { QStringLiteral("evidence"), QJsonArray{
                QStringLiteral("input.delivered=true"),
                QStringLiteral("events=%1").arg(events.size()),
                QStringLiteral("wait.reason=%1").arg(waitResult.value(QStringLiteral("reason")).toString()),
            } },
        };
        if (waitResult.contains(QStringLiteral("diagnostics")))
            issue.insert(QStringLiteral("diagnostics"), waitResult.value(QStringLiteral("diagnostics")));
        issues.append(issue);
    }

    return {
        { QStringLiteral("kind"), kind },
        { QStringLiteral("targetSelector"), targetSelector },
        { QStringLiteral("waitSelector"), waitSelector },
        { QStringLiteral("until"), until },
        { QStringLiteral("target"), targetQuery },
        { QStringLiteral("input"), inputResult },
        { QStringLiteral("events"), events },
        { QStringLiteral("wait"), waitResult },
        { QStringLiteral("verification"), verification },
        { QStringLiteral("issues"), issues },
    };
}

class QmlAgentMcpServer : public QObject
{
    Q_OBJECT

public:
    explicit QmlAgentMcpServer(int timeoutMs, QObject *parent = nullptr)
        : QObject(parent)
        , m_timeoutMs(timeoutMs)
    {
        m_commandTimeout.setSingleShot(true);
        connect(&m_commandTimeout, &QTimer::timeout,
                this, &QmlAgentMcpServer::targetCommandTimedOut);
        const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (flags < 0) {
            QTimer::singleShot(0, this, &QmlAgentMcpServer::handleInputClosed);
        } else {
            fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
            m_stdinNotifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
            connect(m_stdinNotifier, &QSocketNotifier::activated,
                    this, &QmlAgentMcpServer::handleStdinActivated);
        }
    }

private:
    struct PendingCall
    {
        enum class Kind {
            TargetCommand,
            WorkflowClick,
            WorkflowClickAndWait,
            WorkflowLongPressAndWait,
            WorkflowKey,
            Disconnect,
        };

        Kind kind = Kind::TargetCommand;
        QJsonValue mcpId;
        QString targetMethod;
        QJsonObject targetParams;
        int targetId = 0;
        QString targetSelector;
        QString expectedSelector;
        QString waitSelector;
        QJsonObject waitUntil;
        int waitTimeoutMs = -1;
        int holdMs = 900;
        QString key;
        QString verbosity;
        QString outPath;
        QString wantedSession;
        Expectation expectation;
    };

    struct WorkflowState
    {
        enum class Phase {
            TargetQuery,
            BeforeQuery,
            Subscribe,
            Input,
            Wait,
            AfterQuery,
            Unsubscribe,
        };

        PendingCall call;
        Phase phase = Phase::TargetQuery;
        int targetId = 0;
        QString targetMethod;
        bool subscribedBefore = false;
        bool temporarySubscriptionActive = false;
        bool finishingWithError = false;
        QJsonObject errorPayload;
        QJsonObject targetQuery;
        QJsonObject beforeQuery;
        QJsonObject input;
        QJsonObject wait;
        QJsonObject afterQuery;
        QJsonArray events;
    };

    static QJsonObject nodeRef(const QJsonObject &arguments, QString *error)
    {
        const bool hasSelector = arguments.contains(QStringLiteral("selector"))
                && !arguments.value(QStringLiteral("selector")).toString().isEmpty();
        const bool hasNodeId = arguments.contains(QStringLiteral("nodeId"))
                && !arguments.value(QStringLiteral("nodeId")).isNull();
        if (hasSelector == hasNodeId) {
            *error = QStringLiteral("Provide exactly one of selector or nodeId.");
            return {};
        }
        if (hasSelector)
            return { { QStringLiteral("selector"), arguments.value(QStringLiteral("selector")) } };
        return { { QStringLiteral("nodeId"), arguments.value(QStringLiteral("nodeId")).toInt() } };
    }

    static QJsonObject treeParams(const QJsonObject &arguments)
    {
        const bool selectorWithoutDepth = arguments.contains(QStringLiteral("selector"))
                && !arguments.contains(QStringLiteral("depth"));
        QJsonObject params{
            { QStringLiteral("depth"), selectorWithoutDepth
                    ? -1
                    : arguments.value(QStringLiteral("depth")).toInt(3) },
            { QStringLiteral("includeSource"), arguments.value(QStringLiteral("includeSource")).toBool(false) },
            { QStringLiteral("fields"), arguments.value(QStringLiteral("fields")).toArray(QJsonArray{
                QStringLiteral("nodeId"),
                QStringLiteral("qmlId"),
                QStringLiteral("objectName"),
                QStringLiteral("kind"),
                QStringLiteral("sceneKind"),
                QStringLiteral("type"),
                QStringLiteral("text"),
                QStringLiteral("bbox"),
                QStringLiteral("insideViewport"),
                QStringLiteral("viewport"),
                QStringLiteral("actionable"),
            }) },
            { QStringLiteral("maxNodes"), arguments.value(QStringLiteral("maxNodes")).toInt(500) },
            { QStringLiteral("collapseRepeated"), arguments.value(QStringLiteral("collapseRepeated")).toBool(true) },
        };
        for (const QString &key : { QStringLiteral("includeInvisible"),
                                    QStringLiteral("properties"),
                                    QStringLiteral("selector") }) {
            if (arguments.contains(key))
                params.insert(key, arguments.value(key));
        }
        return params;
    }

    static QJsonObject queryParams(const QJsonObject &arguments)
    {
        QJsonObject params{
            { QStringLiteral("selector"), arguments.value(QStringLiteral("selector")) },
            { QStringLiteral("includeSource"), arguments.value(QStringLiteral("includeSource")).toBool(true) },
            { QStringLiteral("fields"), arguments.value(QStringLiteral("fields")).toArray(QJsonArray{
                QStringLiteral("nodeId"),
                QStringLiteral("qmlId"),
                QStringLiteral("objectName"),
                QStringLiteral("kind"),
                QStringLiteral("sceneKind"),
                QStringLiteral("type"),
                QStringLiteral("text"),
                QStringLiteral("bbox"),
                QStringLiteral("insideViewport"),
                QStringLiteral("viewport"),
                QStringLiteral("actionable"),
                QStringLiteral("sourceLocation"),
            }) },
            { QStringLiteral("maxNodes"), arguments.value(QStringLiteral("maxNodes")).toInt(20) },
        };
        if (arguments.contains(QStringLiteral("properties")))
            params.insert(QStringLiteral("properties"), arguments.value(QStringLiteral("properties")));
        return params;
    }

    static bool requireStringArgument(const QJsonObject &arguments, const QString &name,
                                      QString *value, QString *error)
    {
        *value = arguments.value(name).toString();
        if (value->isEmpty()) {
            *error = QStringLiteral("Provide %1.").arg(name);
            return false;
        }
        return true;
    }

    static bool mapWorkflowCall(const QString &name, const QJsonObject &arguments,
                                PendingCall *call, QString *error)
    {
        if (name != QLatin1String("qmlagent_workflow_click")
                && name != QLatin1String("qmlagent_workflow_click_and_wait")
                && name != QLatin1String("qmlagent_workflow_long_press_and_wait")
                && name != QLatin1String("qmlagent_workflow_key")) {
            return false;
        }

        if (name == QLatin1String("qmlagent_workflow_click"))
            call->kind = PendingCall::Kind::WorkflowClick;
        else if (name == QLatin1String("qmlagent_workflow_click_and_wait"))
            call->kind = PendingCall::Kind::WorkflowClickAndWait;
        else if (name == QLatin1String("qmlagent_workflow_long_press_and_wait"))
            call->kind = PendingCall::Kind::WorkflowLongPressAndWait;
        else
            call->kind = PendingCall::Kind::WorkflowKey;
        if (!requireStringArgument(arguments, QStringLiteral("selector"),
                                   &call->targetSelector, error)) {
            return true;
        }
        if (call->kind == PendingCall::Kind::WorkflowClickAndWait
                || call->kind == PendingCall::Kind::WorkflowLongPressAndWait) {
            if (!requireStringArgument(arguments, QStringLiteral("waitSelector"),
                                       &call->waitSelector, error)) {
                return true;
            }
            call->waitUntil = arguments.value(QStringLiteral("until")).toObject();
            if (call->waitUntil.isEmpty()) {
                *error = QStringLiteral("Provide until.");
                return true;
            }
            if (arguments.contains(QStringLiteral("timeoutMs")))
                call->waitTimeoutMs = arguments.value(QStringLiteral("timeoutMs")).toInt(-1);
            if (call->kind == PendingCall::Kind::WorkflowLongPressAndWait)
                call->holdMs = arguments.value(QStringLiteral("holdMs")).toInt(900);
            call->verbosity = arguments.value(QStringLiteral("verbosity")).toString(QStringLiteral("summary"));
            return true;
        }
        if (call->kind == PendingCall::Kind::WorkflowKey
                && !requireStringArgument(arguments, QStringLiteral("key"),
                                          &call->key, error)) {
            return true;
        }
        if (!requireStringArgument(arguments, QStringLiteral("expectSelector"),
                                   &call->expectedSelector, error)) {
            return true;
        }

        QString expectationText;
        if (!requireStringArgument(arguments, QStringLiteral("expect"), &expectationText, error))
            return true;

        call->expectation = parseExpectation(expectationText);
        call->verbosity = arguments.value(QStringLiteral("verbosity")).toString(QStringLiteral("summary"));
        if (!call->expectation.valid)
            *error = QStringLiteral("Provide expect as property=value or a numeric comparison.");
        return true;
    }

    static bool mapToolCall(const QString &name, const QJsonObject &arguments,
                            QString *targetMethod, QJsonObject *targetParams,
                            QString *error)
    {
        if (name == QLatin1String("qmlagent_ui_get_tree")) {
            *targetMethod = QStringLiteral("UI.getTree");
            *targetParams = treeParams(arguments);
            return true;
        }
        if (name == QLatin1String("qmlagent_ui_query")) {
            *targetMethod = QStringLiteral("UI.query");
            *targetParams = queryParams(arguments);
            return true;
        }
        if (name == QLatin1String("qmlagent_ui_query_many")) {
            *targetMethod = QStringLiteral("UI.queryMany");
            const QJsonArray queries = arguments.value(QStringLiteral("queries")).toArray();
            if (queries.isEmpty()) {
                *error = QStringLiteral("Provide queries.");
                return false;
            }
            const QJsonObject defaults = arguments.value(QStringLiteral("defaults")).toObject();
            QJsonArray mappedQueries;
            for (const QJsonValue &entryValue : queries) {
                QJsonObject entry = entryValue.toObject();
                for (auto it = defaults.constBegin(), end = defaults.constEnd(); it != end; ++it) {
                    if (!entry.contains(it.key()))
                        entry.insert(it.key(), it.value());
                }
                if (entry.value(QStringLiteral("selector")).toString().isEmpty()) {
                    *error = QStringLiteral("Provide selector in every queries entry or in defaults.");
                    return false;
                }
                mappedQueries.append(queryParams(entry));
            }
            *targetParams = { { QStringLiteral("queries"), mappedQueries } };
            return true;
        }
        if (name == QLatin1String("qmlagent_ui_wait_for")) {
            *targetMethod = QStringLiteral("UI.waitFor");
            QString selector;
            if (!requireStringArgument(arguments, QStringLiteral("selector"), &selector, error))
                return false;
            const QJsonObject until = arguments.value(QStringLiteral("until")).toObject();
            if (until.isEmpty()) {
                *error = QStringLiteral("Provide until.");
                return false;
            }
            *targetParams = {
                { QStringLiteral("selector"), selector },
                { QStringLiteral("until"), until },
            };
            if (arguments.contains(QStringLiteral("timeoutMs")))
                targetParams->insert(QStringLiteral("timeoutMs"),
                                     arguments.value(QStringLiteral("timeoutMs")).toInt());
            return true;
        }
        if (name == QLatin1String("qmlagent_ui_subscribe")) {
            *targetMethod = QStringLiteral("UI.subscribe");
            *targetParams = {};
            return true;
        }
        if (name == QLatin1String("qmlagent_ui_unsubscribe")) {
            *targetMethod = QStringLiteral("UI.unsubscribe");
            *targetParams = {};
            return true;
        }
        if (name == QLatin1String("qmlagent_diagnostics_analyze_tree")) {
            *targetMethod = QStringLiteral("Diagnostics.analyzeTree");
            if (arguments.contains(QStringLiteral("includeInvisible")))
                targetParams->insert(QStringLiteral("includeInvisible"),
                                     arguments.value(QStringLiteral("includeInvisible")).toBool());
            if (arguments.contains(QStringLiteral("includeFrameworkIssues")))
                targetParams->insert(QStringLiteral("includeFrameworkIssues"),
                                     arguments.value(QStringLiteral("includeFrameworkIssues")).toBool());
            if (arguments.contains(QStringLiteral("issueScope")))
                targetParams->insert(QStringLiteral("issueScope"), arguments.value(QStringLiteral("issueScope")));
            targetParams->insert(QStringLiteral("verbosity"),
                                 arguments.value(QStringLiteral("verbosity"))
                                         .toString(QStringLiteral("summary")));
            if (arguments.contains(QStringLiteral("maxIssues")))
                targetParams->insert(QStringLiteral("maxIssues"), arguments.value(QStringLiteral("maxIssues")));
            if (arguments.contains(QStringLiteral("checks")))
                targetParams->insert(QStringLiteral("checks"), arguments.value(QStringLiteral("checks")));
            return true;
        }
        if (name == QLatin1String("qmlagent_diagnostics_analyze_node")) {
            *targetMethod = QStringLiteral("Diagnostics.analyzeNode");
            *targetParams = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            if (arguments.contains(QStringLiteral("checks")))
                targetParams->insert(QStringLiteral("checks"), arguments.value(QStringLiteral("checks")));
            return true;
        }
        if (name == QLatin1String("qmlagent_diagnostics_analyze_binding")) {
            *targetMethod = QStringLiteral("Diagnostics.analyzeBinding");
            *targetParams = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            QString property;
            if (!requireStringArgument(arguments, QStringLiteral("property"), &property, error))
                return false;
            targetParams->insert(QStringLiteral("property"), property);
            return true;
        }
        if (name == QLatin1String("qmlagent_runtime_enable_mutation")) {
            *targetMethod = QStringLiteral("Session.configure");
            *targetParams = {
                { QStringLiteral("runtimeMutation"),
                  arguments.value(QStringLiteral("enabled")).toBool(true) },
            };
            return true;
        }
        if (name == QLatin1String("qmlagent_input_click")) {
            *targetMethod = QStringLiteral("Input.clickNode");
            *targetParams = nodeRef(arguments, error);
            if (arguments.contains(QStringLiteral("point")))
                targetParams->insert(QStringLiteral("point"), arguments.value(QStringLiteral("point")));
            if (arguments.contains(QStringLiteral("settle")))
                targetParams->insert(QStringLiteral("settle"), arguments.value(QStringLiteral("settle")));
            return error->isEmpty();
        }
        if (name == QLatin1String("qmlagent_input_long_press")) {
            *targetMethod = QStringLiteral("Input.longPressNode");
            *targetParams = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            for (const QString &key : { QStringLiteral("holdMs"),
                                        QStringLiteral("button"),
                                        QStringLiteral("point"),
                                        QStringLiteral("modifiers"),
                                        QStringLiteral("settle") }) {
                if (arguments.contains(key))
                    targetParams->insert(key, arguments.value(key));
            }
            return true;
        }
        if (name == QLatin1String("qmlagent_input_wheel")) {
            *targetMethod = QStringLiteral("Input.wheel");
            *targetParams = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            for (const QString &key : { QStringLiteral("deltaX"),
                                        QStringLiteral("deltaY"),
                                        QStringLiteral("angleDelta"),
                                        QStringLiteral("pixelDelta"),
                                        QStringLiteral("modifiers") }) {
                if (arguments.contains(key))
                    targetParams->insert(key, arguments.value(key));
            }
            return true;
        }
        if (name == QLatin1String("qmlagent_input_scroll_into_view")) {
            *targetMethod = QStringLiteral("Input.scrollIntoView");
            *targetParams = nodeRef(arguments, error);
            return error->isEmpty();
        }
        if (name == QLatin1String("qmlagent_input_focus")) {
            *targetMethod = QStringLiteral("Input.focusNode");
            *targetParams = nodeRef(arguments, error);
            return error->isEmpty();
        }
        if (name == QLatin1String("qmlagent_input_mouse")) {
            *targetMethod = QStringLiteral("Input.dispatchMouseEvent");
            *targetParams = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            for (const QString &key : { QStringLiteral("type"),
                                        QStringLiteral("button"),
                                        QStringLiteral("buttons"),
                                        QStringLiteral("point"),
                                        QStringLiteral("modifiers") }) {
                if (arguments.contains(key))
                    targetParams->insert(key, arguments.value(key));
            }
            return true;
        }
        if (name == QLatin1String("qmlagent_input_drag")) {
            *targetMethod = QStringLiteral("Input.dragNode");
            *targetParams = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            for (const QString &key : { QStringLiteral("from"),
                                        QStringLiteral("to"),
                                        QStringLiteral("delta"),
                                        QStringLiteral("steps"),
                                        QStringLiteral("button"),
                                        QStringLiteral("modifiers") }) {
                if (arguments.contains(key))
                    targetParams->insert(key, arguments.value(key));
            }
            return true;
        }
        if (name == QLatin1String("qmlagent_input_touch")) {
            *targetMethod = QStringLiteral("Input.dispatchTouchEvent");
            *targetParams = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            for (const QString &key : { QStringLiteral("type"),
                                        QStringLiteral("points"),
                                        QStringLiteral("modifiers") }) {
                if (arguments.contains(key))
                    targetParams->insert(key, arguments.value(key));
            }
            return true;
        }
        if (name == QLatin1String("qmlagent_input_key")) {
            *targetMethod = QStringLiteral("Input.dispatchKeyEvent");
            const bool hasSelector = arguments.contains(QStringLiteral("selector"))
                    && !arguments.value(QStringLiteral("selector")).toString().isEmpty();
            const bool hasNodeId = arguments.contains(QStringLiteral("nodeId"))
                    && !arguments.value(QStringLiteral("nodeId")).isNull();
            if (hasSelector || hasNodeId) {
                *targetParams = nodeRef(arguments, error);
                if (!error->isEmpty())
                    return false;
            }
            for (const QString &key : { QStringLiteral("key"),
                                        QStringLiteral("keyCode"),
                                        QStringLiteral("text"),
                                        QStringLiteral("type"),
                                        QStringLiteral("modifiers") }) {
                if (arguments.contains(key))
                    targetParams->insert(key, arguments.value(key));
            }
            if (!targetParams->contains(QStringLiteral("key"))
                    && !targetParams->contains(QStringLiteral("keyCode"))) {
                *error = QStringLiteral("Provide key or keyCode.");
                return false;
            }
            return true;
        }
        if (name == QLatin1String("qmlagent_input_type_text")) {
            *targetMethod = QStringLiteral("Input.typeText");
            *targetParams = { { QStringLiteral("text"), arguments.value(QStringLiteral("text")) } };
            if (arguments.value(QStringLiteral("replaceExisting")).toBool(false))
                targetParams->insert(QStringLiteral("replaceExisting"), true);
            const bool hasSelector = arguments.contains(QStringLiteral("selector"))
                    && !arguments.value(QStringLiteral("selector")).toString().isEmpty();
            const bool hasNodeId = arguments.contains(QStringLiteral("nodeId"))
                    && !arguments.value(QStringLiteral("nodeId")).isNull();
            if (hasSelector || hasNodeId) {
                const QJsonObject ref = nodeRef(arguments, error);
                if (!error->isEmpty())
                    return false;
                for (auto it = ref.constBegin(), end = ref.constEnd(); it != end; ++it)
                    targetParams->insert(it.key(), it.value());
            }
            return true;
        }
        if (name == QLatin1String("qmlagent_input_clear_text")) {
            *targetMethod = QStringLiteral("Input.typeText");
            *targetParams = {
                { QStringLiteral("text"), QString() },
                { QStringLiteral("replaceExisting"), true },
            };
            const QJsonObject ref = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            for (auto it = ref.constBegin(), end = ref.constEnd(); it != end; ++it)
                targetParams->insert(it.key(), it.value());
            return true;
        }
        if (name == QLatin1String("qmlagent_input_dismiss_popup")) {
            *targetMethod = QStringLiteral("Input.dismissPopup");
            *targetParams = {};
            if (arguments.contains(QStringLiteral("all")))
                targetParams->insert(QStringLiteral("all"),
                                     arguments.value(QStringLiteral("all")));
            return true;
        }
        if (name == QLatin1String("qmlagent_runtime_set_property")) {
            *targetMethod = QStringLiteral("Runtime.setProperty");
            *targetParams = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            QString property;
            if (!requireStringArgument(arguments, QStringLiteral("property"), &property, error))
                return false;
            if (!arguments.contains(QStringLiteral("value"))) {
                *error = QStringLiteral("Provide value.");
                return false;
            }
            targetParams->insert(QStringLiteral("property"), property);
            targetParams->insert(QStringLiteral("value"), arguments.value(QStringLiteral("value")));
            if (arguments.contains(QStringLiteral("settle")))
                targetParams->insert(QStringLiteral("settle"), arguments.value(QStringLiteral("settle")));
            return true;
        }
        if (name == QLatin1String("qmlagent_runtime_invoke_method")) {
            *targetMethod = QStringLiteral("Runtime.invokeMethod");
            *targetParams = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            QString method;
            if (!requireStringArgument(arguments, QStringLiteral("method"), &method, error))
                return false;
            targetParams->insert(QStringLiteral("method"), method);
            targetParams->insert(QStringLiteral("args"),
                                 arguments.value(QStringLiteral("args")).toArray());
            if (arguments.contains(QStringLiteral("settle")))
                targetParams->insert(QStringLiteral("settle"), arguments.value(QStringLiteral("settle")));
            return true;
        }
        if (name == QLatin1String("qmlagent_log_enable")) {
            *targetMethod = QStringLiteral("Log.enable");
            if (arguments.contains(QStringLiteral("replayBuffered")))
                targetParams->insert(QStringLiteral("replayBuffered"),
                                     arguments.value(QStringLiteral("replayBuffered")).toBool());
            return true;
        }
        if (name == QLatin1String("qmlagent_log_get_entries")) {
            *targetMethod = QStringLiteral("Log.getEntries");
            if (arguments.contains(QStringLiteral("level")))
                targetParams->insert(QStringLiteral("level"), arguments.value(QStringLiteral("level")));
            if (arguments.contains(QStringLiteral("sinceTimestamp")))
                targetParams->insert(QStringLiteral("sinceTimestamp"),
                                     arguments.value(QStringLiteral("sinceTimestamp")));
            if (arguments.contains(QStringLiteral("maxEntries")))
                targetParams->insert(QStringLiteral("maxEntries"),
                                     arguments.value(QStringLiteral("maxEntries")));
            return true;
        }
        if (name == QLatin1String("qmlagent_render_capture_screenshot")) {
            *targetMethod = QStringLiteral("Render.captureScreenshot");
            if (arguments.contains(QStringLiteral("windowId")))
                targetParams->insert(QStringLiteral("windowId"),
                                     arguments.value(QStringLiteral("windowId")).toInt());
            if (arguments.contains(QStringLiteral("scale")))
                targetParams->insert(QStringLiteral("scale"), arguments.value(QStringLiteral("scale")));
            if (arguments.contains(QStringLiteral("region")))
                targetParams->insert(QStringLiteral("region"), arguments.value(QStringLiteral("region")));
            // outPath needs the PNG bytes to write, but they are stripped from
            // the tool result afterward so they never enter the agent context.
            const bool toFile = !arguments.value(QStringLiteral("outPath")).toString().isEmpty();
            targetParams->insert(QStringLiteral("includeData"),
                                 toFile || arguments.value(QStringLiteral("includeData")).toBool(false));
            return true;
        }
        if (name == QLatin1String("qmlagent_render_pick3d")) {
            *targetMethod = QStringLiteral("Render.pick3D");
            *targetParams = nodeRef(arguments, error);
            if (!error->isEmpty())
                return false;
            if (!arguments.contains(QStringLiteral("x")) || !arguments.contains(QStringLiteral("y"))) {
                *error = QStringLiteral("Provide x and y.");
                return false;
            }
            targetParams->insert(QStringLiteral("x"), arguments.value(QStringLiteral("x")));
            targetParams->insert(QStringLiteral("y"), arguments.value(QStringLiteral("y")));
            if (arguments.contains(QStringLiteral("coordinateSpace")))
                targetParams->insert(QStringLiteral("coordinateSpace"),
                                     arguments.value(QStringLiteral("coordinateSpace")));
            if (arguments.contains(QStringLiteral("modelSelector")))
                targetParams->insert(QStringLiteral("modelSelector"),
                                     arguments.value(QStringLiteral("modelSelector")));
            return true;
        }
        if (name == QLatin1String("qmlagent_source_resolve")) {
            *targetMethod = QStringLiteral("Source.resolveNode");
            *targetParams = nodeRef(arguments, error);
            return error->isEmpty();
        }

        *error = QStringLiteral("Unknown tool: %1").arg(name);
        return false;
    }

    static bool makePendingCall(const QJsonValue &requestId, const QString &name,
                                const QJsonObject &arguments, PendingCall *call,
                                QString *error)
    {
        call->mcpId = requestId;
        call->verbosity = arguments.value(QStringLiteral("verbosity")).toString(QStringLiteral("full"));
        // Session pin applies to every launcher-routed call, workflows included.
        call->wantedSession = arguments.value(QStringLiteral("session")).toString();
        if (mapWorkflowCall(name, arguments, call, error))
            return error->isEmpty();

        call->kind = PendingCall::Kind::TargetCommand;
        call->outPath = arguments.value(QStringLiteral("outPath")).toString();
        const bool mapped = mapToolCall(name, arguments, &call->targetMethod, &call->targetParams,
                                        error);
        if (mapped
                && (call->targetMethod == QLatin1String("UI.query")
                    || call->targetMethod == QLatin1String("UI.queryMany"))
                && !arguments.contains(QStringLiteral("verbosity"))) {
            call->verbosity = QStringLiteral("summary");
        }
        return mapped;
    }

    static QString workflowPhaseName(WorkflowState::Phase phase)
    {
        switch (phase) {
        case WorkflowState::Phase::TargetQuery:
            return QStringLiteral("target-query");
        case WorkflowState::Phase::BeforeQuery:
            return QStringLiteral("before-query");
        case WorkflowState::Phase::Subscribe:
            return QStringLiteral("subscribe");
        case WorkflowState::Phase::Input:
            return QStringLiteral("input");
        case WorkflowState::Phase::Wait:
            return QStringLiteral("wait");
        case WorkflowState::Phase::AfterQuery:
            return QStringLiteral("after-query");
        case WorkflowState::Phase::Unsubscribe:
            return QStringLiteral("unsubscribe");
        }
        return QStringLiteral("unknown");
    }

    void writeMessage(const QJsonObject &message)
    {
        QTextStream stream(stdout);
        stream << QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact))
               << Qt::endl;
        stream.flush();
    }

    void handleInputLine(const QByteArray &lineBytes)
    {
        const QByteArray line = lineBytes.trimmed();
        if (line.isEmpty())
            return;

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            writeMessage(jsonError({}, -32700, QStringLiteral("Parse error"),
                                   parseError.errorString()));
            return;
        }
        handleMcpRequest(document.object());
    }

    void handleMcpRequest(const QJsonObject &request)
    {
        const QJsonValue requestId = request.value(QStringLiteral("id"));
        const QString method = request.value(QStringLiteral("method")).toString();

        if (method == QLatin1String("initialize")) {
            writeMessage(jsonResponse(requestId, QJsonObject{
                { QStringLiteral("protocolVersion"), QStringLiteral("2024-11-05") },
                { QStringLiteral("capabilities"), QJsonObject{ { QStringLiteral("tools"), QJsonObject{} } } },
                { QStringLiteral("serverInfo"), QJsonObject{
                    { QStringLiteral("name"), QStringLiteral("qmlagent") },
                    { QStringLiteral("version"), QStringLiteral("0.1") },
                } },
            }));
            return;
        }
        if (method == QLatin1String("shutdown")) {
            writeMessage(jsonResponse(requestId, QJsonValue::Null));
            quitIfInputClosedAndIdle();
            return;
        }
        if (method == QLatin1String("notifications/initialized"))
            return;
        if (method == QLatin1String("notifications/exit")) {
            QCoreApplication::quit();
            return;
        }
        if (method == QLatin1String("tools/list")) {
            writeMessage(jsonResponse(requestId, QJsonObject{ { QStringLiteral("tools"), toolList() } }));
            return;
        }
        if (method == QLatin1String("tools/call")) {
            const QJsonObject params = request.value(QStringLiteral("params")).toObject();
            const QString name = params.value(QStringLiteral("name")).toString();
            const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();

            if (handleMcpControlTool(requestId, name, arguments))
                return;

            PendingCall call;
            QString error;
            if (!makePendingCall(requestId, name, arguments, &call, &error)) {
                writeMessage(jsonResponse(requestId, toolErrorResult(error)));
                return;
            }

            if (!isTargetConnected()) {
                routePendingCallViaLauncher(requestId, call);
                return;
            }

            m_queue.enqueue(call);
            dispatchNextTargetCommand();
            return;
        }

        if (!requestId.isUndefined())
            writeMessage(jsonError(requestId, -32601,
                                   QStringLiteral("Method not found: %1").arg(method)));
    }

    bool handleMcpControlTool(const QJsonValue &requestId, const QString &name,
                              const QJsonObject &arguments)
    {
        if (name == QLatin1String("qmlagent_target_status")) {
            writeMessage(jsonResponse(requestId, toolResult(targetStatus())));
            return true;
        }

        if (name == QLatin1String("qmlagent_preview_reload")) {
            LauncherSession launcher;
            QString launcherError;
            if (!resolveLauncherSession(arguments.value(QStringLiteral("timeoutMs")).toInt(m_timeoutMs),
                                        &launcher, &launcherError,
                                        arguments.value(QStringLiteral("session")).toString())) {
                writeMessage(jsonResponse(requestId, toolErrorResult(launcherError)));
                return true;
            }
            QString controlError;
            const QJsonObject response = sendLauncherControlRequest(
                    launcher.metadata,
                    arguments.value(QStringLiteral("timeoutMs")).toInt(m_timeoutMs),
                    QStringLiteral("Preview.reload"),
                    {
                        { QStringLiteral("timeoutMs"), arguments.value(QStringLiteral("timeoutMs")).toInt(m_timeoutMs) },
                    },
                    &controlError);
            if (!controlError.isEmpty()) {
                writeMessage(jsonResponse(requestId, toolErrorResult(controlError)));
                return true;
            }
            const QJsonObject result = response.value(QStringLiteral("result")).toObject();
            writeMessage(jsonResponse(requestId, toolResult(result,
                                                            !result.value(QStringLiteral("error")).toObject().isEmpty())));
            return true;
        }

        if (name == QLatin1String("qmlagent_launcher_stop")) {
            LauncherSession launcher;
            QString launcherError;
            QJsonObject fallbackResult;
            if (!resolveLauncherSessionForStop(m_timeoutMs, &launcher, &fallbackResult,
                                               &launcherError)) {
                if (!fallbackResult.isEmpty()) {
                    writeMessage(jsonResponse(requestId, toolResult(fallbackResult,
                            !fallbackResult.value(QStringLiteral("ok")).toBool(false))));
                    return true;
                }
                writeMessage(jsonResponse(requestId, toolErrorResult(launcherError)));
                return true;
            }
            QString controlError;
            const QJsonObject response = sendLauncherControlRequest(
                    launcher.metadata, m_timeoutMs, QStringLiteral("Session.stop"), {},
                    &controlError);
            if (!controlError.isEmpty()) {
                writeMessage(jsonResponse(requestId, toolErrorResult(controlError)));
                return true;
            }
            writeMessage(jsonResponse(requestId, toolResult(response.value(QStringLiteral("result")).toObject())));
            return true;
        }

        if (name == QLatin1String("qmlagent_disconnect")) {
            if (m_currentCall.has_value() || m_workflow.has_value() || !m_queue.isEmpty()) {
                PendingCall call;
                call.kind = PendingCall::Kind::Disconnect;
                call.mcpId = requestId;
                m_queue.enqueue(call);
                dispatchNextTargetCommand();
                return true;
            }
            disconnectTarget(QStringLiteral("client-request"));
            writeMessage(jsonResponse(requestId, toolResult(targetStatus())));
            return true;
        }

        if (name != QLatin1String("qmlagent_connect_tcp")
                && name != QLatin1String("qmlagent_connect_local_socket")) {
            return false;
        }

        if (m_currentCall.has_value() || m_workflow.has_value() || !m_queue.isEmpty()) {
            writeMessage(jsonResponse(requestId, toolErrorResult(
                    QStringLiteral("Cannot connect while target commands are in flight."))));
            return true;
        }

        if (name == QLatin1String("qmlagent_connect_local_socket")) {
            const QString path = arguments.value(QStringLiteral("path")).toString();
            if (path.isEmpty()) {
                writeMessage(jsonResponse(requestId, toolErrorResult(
                        QStringLiteral("Provide path."))));
                return true;
            }

            const int timeoutMs = arguments.contains(QStringLiteral("timeoutMs"))
                    ? arguments.value(QStringLiteral("timeoutMs")).toInt(-1)
                    : m_timeoutMs;
            if (timeoutMs <= 0) {
                writeMessage(jsonResponse(requestId, toolErrorResult(
                        QStringLiteral("Provide timeoutMs as a positive integer."))));
                return true;
            }

            const QJsonObject result = connectLocalSocketTarget(path, timeoutMs);
            writeMessage(jsonResponse(requestId, toolResult(result,
                                                            !result.value(QStringLiteral("connected"))
                                                                     .toBool())));
            return true;
        }

        const int port = arguments.contains(QStringLiteral("port"))
                ? arguments.value(QStringLiteral("port")).toInt(-1)
                : defaultTcpTargetPort();
        if (port <= 0 || port > 65535) {
            writeMessage(jsonResponse(requestId, toolErrorResult(
                    QStringLiteral("Provide port as an integer between 1 and 65535."))));
            return true;
        }

        const int timeoutMs = arguments.contains(QStringLiteral("timeoutMs"))
                ? arguments.value(QStringLiteral("timeoutMs")).toInt(-1)
                : m_timeoutMs;
        if (timeoutMs <= 0) {
            writeMessage(jsonResponse(requestId, toolErrorResult(
                    QStringLiteral("Provide timeoutMs as a positive integer."))));
            return true;
        }

        const QString host = arguments.value(QStringLiteral("host")).toString(
                QStringLiteral("127.0.0.1"));
        if (host.isEmpty()) {
            writeMessage(jsonResponse(requestId, toolErrorResult(QStringLiteral("Provide host."))));
            return true;
        }

        const QJsonObject result = connectTcpTarget(host, quint16(port), timeoutMs);
        writeMessage(jsonResponse(requestId, toolResult(result,
                                                        !result.value(QStringLiteral("connected"))
                                                                 .toBool())));
        return true;
    }

    bool isTargetConnected() const
    {
        return m_connection && m_connection->isConnected() && m_client
                && m_client->state() == QQmlDebugClient::Enabled;
    }

    QString notConnectedMessage() const
    {
        return QStringLiteral("QmlAgent target is not connected and no single live "
                              "qmlagent-launcher gateway was available. Start one launcher session "
                              "with qmlagent-launcher preview <Main.qml> or qmlagent-launcher app "
                              "<executable> [-- args...], then call qmlagent_ui_query or other "
                              "target-backed tools directly. For manually launched targets, attach "
                              "this MCP server with qmlagent_connect_tcp or qmlagent_connect_local_socket.");
    }

    QJsonObject agentToolGuide() const
    {
        return {
            { QStringLiteral("advertisedTools"), advertisedToolNames() },
            { QStringLiteral("discoverabilityContract"),
              QStringLiteral("Every qmlagent_* tool named in this guide should appear in advertisedTools. If a lazy agent runtime has not exposed one yet, search by the exact tool name from advertisedTools.") },
            { QStringLiteral("inspect"), QStringLiteral("qmlagent_ui_query") },
            { QStringLiteral("batchVerificationReads"),
              QStringLiteral("qmlagent_ui_query_many") },
            { QStringLiteral("tree"), QStringLiteral("qmlagent_ui_get_tree") },
            { QStringLiteral("qtQuick3D"),
              QStringLiteral("When target capabilities.qtQuick3D.enabled is true, qmlagent_ui_get_tree and qmlagent_ui_query include View3D scene Model/camera/light/material frontend nodes automatically; use them as structural/source evidence, not click targets. Request fields=[\"render3D\"] for Model projection evidence, checks=[\"quick3D\"] on diagnostics tools for camera/light/material/scale/texture/frustum issues, and qmlagent_render_pick3d for read-only View3D hit-test evidence.") },
            { QStringLiteral("quick3DDiagnostics"),
              QStringLiteral("qmlagent_diagnostics_analyze_tree or qmlagent_diagnostics_analyze_node with checks=[\"quick3D\"]") },
            { QStringLiteral("quick3DHitTest"), QStringLiteral("qmlagent_render_pick3d") },
            { QStringLiteral("click"), QStringLiteral("qmlagent_input_click") },
            { QStringLiteral("longPress"), QStringLiteral("qmlagent_input_long_press") },
            { QStringLiteral("dragSliderHandleSwipe"),
              QStringLiteral("qmlagent_input_drag") },
            { QStringLiteral("scrollFlickableListViewGridViewTableViewTreeView"),
              QStringLiteral("qmlagent_input_wheel") },
            { QStringLiteral("scrollClippedTargetIntoView"),
              QStringLiteral("qmlagent_input_scroll_into_view") },
            { QStringLiteral("waitForSelectorOrProperty"),
              QStringLiteral("qmlagent_ui_wait_for") },
            { QStringLiteral("clickAndWaitForDrawerMenuPopupDialogLoaderTransition"),
              QStringLiteral("qmlagent_workflow_click_and_wait") },
            { QStringLiteral("longPressAndWaitForContextMenuAlternateAction"),
              QStringLiteral("qmlagent_workflow_long_press_and_wait") },
            { QStringLiteral("immediateClickVerification"),
              QStringLiteral("qmlagent_workflow_click") },
            { QStringLiteral("keyboardWorkflow"), QStringLiteral("qmlagent_workflow_key") },
            { QStringLiteral("computedBindingProvenance"),
              QStringLiteral("qmlagent_diagnostics_analyze_binding") },
            { QStringLiteral("visualEvidencePolicy"),
              QStringLiteral("Use structural tools first: ui_query/ui_get_tree, diagnostics, source, logs, input/workflow verification. qmlagent_render_capture_screenshot and qmlagentctl screenshot are fallback evidence after structured evidence is insufficient or the task is explicitly visual. Use scale/region or qmlagentctl --out to keep image bytes out of agent context.") },
            { QStringLiteral("toolSearchHints"), QJsonArray{
                QStringLiteral("qmlagent_workflow_click_and_wait Drawer Popup Dialog ComboBox QQuickPopupItem transition wait"),
                QStringLiteral("qmlagent_workflow_long_press_and_wait long press press-and-hold MouseArea onPressAndHold context menu alternate action"),
                QStringLiteral("qmlagent_ui_query_many batch multiple selectors properties verification"),
                QStringLiteral("qmlagent_input_long_press press-and-hold long press"),
                QStringLiteral("qmlagent_input_scroll_into_view center_outside_viewport clipped content retry click"),
                QStringLiteral("qmlagent_ui_wait_for wait until selector found property visible"),
                QStringLiteral("qmlagent_ui_query popup contents ItemDelegate MenuItem visible choices"),
                QStringLiteral("qmlagent_input_drag Slider RangeSlider Dial handle drag"),
                QStringLiteral("qmlagent_input_wheel Flickable ListView GridView TableView TreeView scroll"),
                QStringLiteral("qmlagent_input_focus TextField TextArea before type text"),
                QStringLiteral("qmlagent_render_pick3d QtQuick3D View3D pick hit test Model screen coordinate"),
                QStringLiteral("qmlagent_render_capture_screenshot fallback visual evidence only structured first"),
            } },
            { QStringLiteral("fallbacks"), QJsonObject{
                { QStringLiteral("workflow_click_and_wait_missing"),
                  QStringLiteral("Use qmlagent_input_click followed by qmlagent_ui_wait_for.") },
                { QStringLiteral("workflow_long_press_and_wait_missing"),
                  QStringLiteral("Use qmlagent_input_long_press followed by qmlagent_ui_wait_for.") },
                { QStringLiteral("center_outside_viewport"),
                  QStringLiteral("Use qmlagent_input_scroll_into_view for instantiated clipped content, then retry the click/read. For virtualized rows that have no node yet, wheel toward them and re-query first.") },
                { QStringLiteral("visualEvidenceNeeded"),
                  QStringLiteral("Use qmlagent_render_capture_screenshot only after structured runtime evidence is insufficient; default output omits PNG data. If includeData:true is necessary, pass scale and/or region.") },
            } },
        };
    }

    static QJsonArray advertisedToolNames()
    {
        QJsonArray names;
        const QJsonArray tools = toolList();
        for (const QJsonValue &toolValue : tools) {
            const QString name = toolValue.toObject().value(QStringLiteral("name")).toString();
            if (!name.isEmpty())
                names.append(name);
        }
        return names;
    }

    QJsonObject targetStatus() const
    {
        QJsonObject status{
            { QStringLiteral("connected"), isTargetConnected() },
            { QStringLiteral("uiSubscribed"), m_uiSubscribed },
            { QStringLiteral("debugConnectionPolicy"), QJsonObject{
                { QStringLiteral("singleClientPerTarget"), true },
                { QStringLiteral("guidance"),
                  QStringLiteral("Use one qmlagent-mcp gateway per target process. If another agent or CLI client is attached, disconnect it or relaunch the target before attaching this MCP server.") },
            } },
        };
        if (!m_host.isEmpty())
            status.insert(QStringLiteral("host"), m_host);
        if (m_port > 0)
            status.insert(QStringLiteral("port"), int(m_port));
        if (!m_socketPath.isEmpty())
            status.insert(QStringLiteral("localSocket"), m_socketPath);
        if (!m_disconnectReason.isEmpty())
            status.insert(QStringLiteral("lastDisconnectReason"), m_disconnectReason);
        if (!m_lastError.isEmpty())
            status.insert(QStringLiteral("lastError"), m_lastError);
        status.insert(QStringLiteral("agentToolGuide"), agentToolGuide());
        const QList<LauncherSession> launcherSessions = discoverLauncherSessions(qMin(m_timeoutMs, 1000));
        const QJsonArray recentLauncherExits = recentLauncherExitReports();
        int reachableLauncherCount = 0;
        QJsonObject singleLauncher;
        LauncherSession singleReachableLauncher;
        for (const LauncherSession &session : launcherSessions) {
            if (session.status.value(QStringLiteral("controlReachable")).toBool(true)) {
                ++reachableLauncherCount;
                singleReachableLauncher = session;
                singleLauncher = launcherSessionSummaries({ session }).first().toObject();
            }
        }
        QJsonObject launcherGateway{
            { QStringLiteral("available"), reachableLauncherCount == 1 },
            { QStringLiteral("reachableSessionCount"), reachableLauncherCount },
            { QStringLiteral("routing"),
              reachableLauncherCount == 1
                      ? QStringLiteral("Target-backed MCP tools route through qmlagent-launcher automatically when no direct attach is active.")
                      : QStringLiteral("Start exactly one qmlagent-launcher session for automatic gateway routing.") },
            { QStringLiteral("session"), reachableLauncherCount == 1 ? QJsonValue(singleLauncher) : QJsonValue() },
        };
        QJsonObject launcherSessionInfo;
        // With several live sessions, list them all so an agent can pin one
        // (qmlagentctl --session <id>) without a separate discovery tool.
        if (reachableLauncherCount > 1) {
            launcherGateway.insert(QStringLiteral("sessions"),
                                   launcherSessionSummaries(launcherSessions));
        } else if (reachableLauncherCount == 1 && !isTargetConnected()) {
            QString requestError;
            const QJsonObject response = sendLauncherAgentRequest(
                    singleReachableLauncher,
                    QStringLiteral("Session.getInfo"),
                    {},
                    qMin(m_timeoutMs, 1000),
                    &requestError);
            launcherSessionInfo = response.value(QStringLiteral("result")).toObject();
            if (!launcherSessionInfo.isEmpty()) {
                launcherGateway.insert(QStringLiteral("targetSessionInfoAvailable"), true);
            } else if (!requestError.isEmpty()) {
                launcherGateway.insert(QStringLiteral("targetSessionInfoAvailable"), false);
                launcherGateway.insert(QStringLiteral("targetSessionInfoError"), requestError);
            }
        }
        status.insert(QStringLiteral("launcherGateway"), launcherGateway);
        if (!recentLauncherExits.isEmpty()) {
            status.insert(QStringLiteral("recentLauncherExitCount"), recentLauncherExits.size());
            status.insert(QStringLiteral("recentLauncherExits"), recentLauncherExits);
        }
        if (!isTargetConnected()) {
            status.insert(QStringLiteral("nextStep"), reachableLauncherCount == 1
                    ? QStringLiteral("A single qmlagent-launcher gateway is available. Call target-backed request/response tools such as qmlagent_ui_query, qmlagent_ui_wait_for, qmlagent_input_click, qmlagent_input_long_press, qmlagent_input_drag, qmlagent_input_wheel, qmlagent_preview_reload, or workflow tools directly. Use qmlagent_connect_tcp/connect_local_socket only for manually launched targets or streamed subscriptions.")
                    : notConnectedMessage());
        }
        status.insert(QStringLiteral("attachTools"), QJsonObject{
            { QStringLiteral("tools"), QJsonArray{
                QStringLiteral("qmlagent_connect_tcp"),
                QStringLiteral("qmlagent_connect_local_socket"),
            } },
            { QStringLiteral("toolSearchHints"), QJsonArray{
                QStringLiteral("qmlagent_connect_tcp attach target"),
                QStringLiteral("qmlagent_connect_local_socket attach target"),
            } },
        });
        if (!m_recentLogEntries.isEmpty()) {
            status.insert(QStringLiteral("recentLogEntries"), m_recentLogEntries);
            status.insert(QStringLiteral("recentLogEntryCount"), m_recentLogEntries.size());
        }
        if (!m_recentEvents.isEmpty()) {
            status.insert(QStringLiteral("recentEvents"), m_recentEvents);
            status.insert(QStringLiteral("recentEventCount"), m_recentEvents.size());
        }
        if (m_connection) {
            status.insert(QStringLiteral("transportConnected"), m_connection->isConnected());
            status.insert(QStringLiteral("transportConnecting"), m_connection->isConnecting());
        }
        status.insert(QStringLiteral("targetLease"), m_targetLease.toJson());
        if (m_client) {
            status.insert(QStringLiteral("serviceEnabled"),
                          m_client->state() == QQmlDebugClient::Enabled);
            status.insert(QStringLiteral("serviceState"), int(m_client->state()));
        }
        const QJsonObject sessionInfo = !m_lastSessionInfo.isEmpty()
                ? m_lastSessionInfo
                : launcherSessionInfo;
        if (!sessionInfo.isEmpty()) {
            status.insert(QStringLiteral("targetSessionInfo"), sessionInfo);
            const QJsonObject capabilities = sessionInfo.value(QStringLiteral("capabilities")).toObject();
            if (!capabilities.isEmpty())
                status.insert(QStringLiteral("capabilities"), capabilities);
            const QJsonArray features = sessionInfo.value(QStringLiteral("features")).toArray();
            if (!features.isEmpty())
                status.insert(QStringLiteral("features"), features);
        }
        return status;
    }

    QJsonObject sendLauncherAgentRequest(const LauncherSession &launcher, const QString &method,
                                         const QJsonObject &params, int timeoutMs,
                                         QString *error) const
    {
        const QJsonObject controlParams{
            { QStringLiteral("method"), method },
            { QStringLiteral("params"), params },
        };
        const QJsonObject controlResponse = sendLauncherControlRequest(
                launcher.metadata,
                launcherControlTimeoutMs(QStringLiteral("QmlAgent.request"), controlParams,
                                         timeoutMs),
                QStringLiteral("QmlAgent.request"), controlParams, error);
        if (!error->isEmpty())
            return {};
        return controlResponse.value(QStringLiteral("result")).toObject();
    }

    bool launcherRouteUnsupportedStreamingTool(const PendingCall &call, QString *message) const
    {
        if (call.kind != PendingCall::Kind::TargetCommand)
            return false;
        if (call.targetMethod == QLatin1String("UI.subscribe")
                || call.targetMethod == QLatin1String("UI.unsubscribe")
                || call.targetMethod == QLatin1String("Log.enable")) {
            *message = QStringLiteral("%1 requires a direct persistent MCP attach because it streams events to this MCP process. "
                                      "Use qmlagent_connect_tcp or qmlagent_connect_local_socket for subscriptions. "
                                      "Request/response tools such as qmlagent_ui_query, qmlagent_ui_wait_for, "
                                      "qmlagent_input_click, qmlagent_preview_reload, and qmlagent_workflow_click_and_wait "
                                      "can route through qmlagent-launcher automatically.")
                               .arg(call.targetMethod);
            return true;
        }
        return false;
    }

    // When a screenshot tool call carried outPath, write the PNG to that file
    // and strip the base64 from the tool result so image bytes never flood the
    // agent context (mirrors qmlagentctl screenshot --out for MCP).
    static QJsonValue applyScreenshotOutPath(const QJsonValue &payload, const PendingCall &call)
    {
        if (call.outPath.isEmpty()
                || call.targetMethod != QLatin1String("Render.captureScreenshot")
                || !payload.isObject()) {
            return payload;
        }
        QJsonObject screenshot = payload.toObject();
        if (!screenshot.value(QStringLiteral("captured")).toBool(false))
            return payload;

        const QByteArray png = QByteArray::fromBase64(
                screenshot.value(QStringLiteral("data")).toString().toLatin1());
        const QFileInfo outputInfo(call.outPath);
        QString writeError;
        if (png.isEmpty()) {
            writeError = QStringLiteral("screenshot response contained no PNG bytes to write");
        } else {
            const QDir outputDir = outputInfo.absoluteDir();
            if (!outputDir.exists() && !QDir().mkpath(outputDir.absolutePath())) {
                writeError = QStringLiteral("could not create directory %1")
                                     .arg(outputDir.absolutePath());
            } else {
                QFile file(call.outPath);
                if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        || file.write(png) != png.size()) {
                    writeError = QStringLiteral("could not write %1: %2")
                                         .arg(call.outPath, file.errorString());
                }
            }
        }

        screenshot.remove(QStringLiteral("data"));
        screenshot.insert(QStringLiteral("dataOmitted"), true);
        if (writeError.isEmpty()) {
            screenshot.insert(QStringLiteral("writtenTo"), outputInfo.absoluteFilePath());
            screenshot.insert(QStringLiteral("bytesWritten"), png.size());
        } else {
            screenshot.insert(QStringLiteral("outPathError"), writeError);
        }
        return screenshot;
    }

    void writeLauncherTargetResponse(const QJsonValue &requestId, const PendingCall &call,
                                     const QJsonObject &targetResponse)
    {
        const bool isError = targetResponse.contains(QStringLiteral("error"));
        QJsonValue payload = isError ? QJsonValue(targetResponse)
                                     : targetResponse.value(QStringLiteral("result"));
        if (!isError && call.verbosity == QLatin1String("summary")
                && call.targetMethod == QLatin1String("UI.query")) {
            payload = summarizedQueryResult(payload.toObject());
        }
        if (!isError && call.verbosity == QLatin1String("summary")
                && call.targetMethod == QLatin1String("UI.queryMany")) {
            payload = summarizedQueryManyResult(payload.toObject());
        }
        if (!isError)
            payload = applyScreenshotOutPath(payload, call);
        writeMessage(jsonResponse(requestId, toolResult(payload, isError)));
    }

    void routePendingCallViaLauncher(const QJsonValue &requestId, const PendingCall &call)
    {
        QString unsupported;
        if (launcherRouteUnsupportedStreamingTool(call, &unsupported)) {
            writeMessage(jsonResponse(requestId, toolErrorResult(unsupported)));
            return;
        }

        LauncherSession launcher;
        QString launcherError;
        if (!resolveLauncherSession(m_timeoutMs, &launcher, &launcherError, call.wantedSession)) {
            writeMessage(jsonResponse(requestId, toolErrorResult(
                    QStringLiteral("%1\n\nLauncher gateway status: %2")
                            .arg(notConnectedMessage(), launcherError))));
            return;
        }

        if (call.kind == PendingCall::Kind::TargetCommand) {
            QString requestError;
            const QJsonObject targetResponse = sendLauncherAgentRequest(
                    launcher, call.targetMethod, call.targetParams, m_timeoutMs, &requestError);
            if (!requestError.isEmpty()) {
                writeMessage(jsonResponse(requestId, toolErrorResult(requestError)));
                return;
            }
            writeLauncherTargetResponse(requestId, call, targetResponse);
            return;
        }

        routeWorkflowViaLauncher(requestId, launcher, call);
    }

    bool launcherWorkflowRequest(const LauncherSession &launcher, const QString &method,
                                 const QJsonObject &params, QJsonObject *targetResponse,
                                 QString *error) const
    {
        *targetResponse = sendLauncherAgentRequest(launcher, method, params, m_timeoutMs, error);
        if (!error->isEmpty())
            return false;
        if (targetResponse->contains(QStringLiteral("error"))) {
            *error = QStringLiteral("%1 failed through qmlagent-launcher gateway.").arg(method);
            return false;
        }
        return true;
    }

    void routeWorkflowViaLauncher(const QJsonValue &requestId, const LauncherSession &launcher,
                                  const PendingCall &call)
    {
        QJsonObject targetQuery;
        QString error;
        if (!launcherWorkflowRequest(launcher, QStringLiteral("UI.query"), {
                { QStringLiteral("selector"), call.targetSelector },
                { QStringLiteral("includeSource"), true },
            }, &targetQuery, &error)) {
            writeMessage(jsonResponse(requestId, toolResult(QJsonObject{
                { QStringLiteral("error"), error },
                { QStringLiteral("targetResponse"), targetQuery },
            }, true)));
            return;
        }

        const QJsonArray events;
        const QJsonArray properties{ call.expectation.property };
        QJsonObject beforeQuery;
        if (call.kind != PendingCall::Kind::WorkflowClickAndWait
                && call.kind != PendingCall::Kind::WorkflowLongPressAndWait) {
            if (!launcherWorkflowRequest(launcher, QStringLiteral("UI.query"), {
                    { QStringLiteral("selector"), call.expectedSelector },
                    { QStringLiteral("includeSource"), true },
                    { QStringLiteral("properties"), properties },
                }, &beforeQuery, &error)) {
                writeMessage(jsonResponse(requestId, toolResult(QJsonObject{
                    { QStringLiteral("error"), error },
                    { QStringLiteral("phase"), QStringLiteral("before-query") },
                    { QStringLiteral("targetResponse"), beforeQuery },
                }, true)));
                return;
            }
        }

        QJsonObject input;
        if (call.kind == PendingCall::Kind::WorkflowClick
                || call.kind == PendingCall::Kind::WorkflowClickAndWait) {
            if (!launcherWorkflowRequest(launcher, QStringLiteral("Input.clickNode"), {
                    { QStringLiteral("selector"), call.targetSelector },
                }, &input, &error)) {
                writeMessage(jsonResponse(requestId, toolResult(QJsonObject{
                    { QStringLiteral("error"), error },
                    { QStringLiteral("phase"), QStringLiteral("input") },
                    { QStringLiteral("targetResponse"), input },
                }, true)));
                return;
            }
        } else if (call.kind == PendingCall::Kind::WorkflowLongPressAndWait) {
            if (!launcherWorkflowRequest(launcher, QStringLiteral("Input.longPressNode"), {
                    { QStringLiteral("selector"), call.targetSelector },
                    { QStringLiteral("holdMs"), call.holdMs },
                }, &input, &error)) {
                writeMessage(jsonResponse(requestId, toolResult(QJsonObject{
                    { QStringLiteral("error"), error },
                    { QStringLiteral("phase"), QStringLiteral("input") },
                    { QStringLiteral("targetResponse"), input },
                }, true)));
                return;
            }
        } else {
            if (!launcherWorkflowRequest(launcher, QStringLiteral("Input.dispatchKeyEvent"), {
                    { QStringLiteral("selector"), call.targetSelector },
                    { QStringLiteral("key"), call.key },
                }, &input, &error)) {
                writeMessage(jsonResponse(requestId, toolResult(QJsonObject{
                    { QStringLiteral("error"), error },
                    { QStringLiteral("phase"), QStringLiteral("input") },
                    { QStringLiteral("targetResponse"), input },
                }, true)));
                return;
            }
        }

        if (call.kind == PendingCall::Kind::WorkflowClickAndWait
                || call.kind == PendingCall::Kind::WorkflowLongPressAndWait) {
            QJsonObject waitParams{
                { QStringLiteral("selector"), call.waitSelector },
                { QStringLiteral("until"), call.waitUntil },
            };
            if (call.waitTimeoutMs >= 0)
                waitParams.insert(QStringLiteral("timeoutMs"), call.waitTimeoutMs);

            QJsonObject wait;
            if (!launcherWorkflowRequest(launcher, QStringLiteral("UI.waitFor"), waitParams,
                                         &wait, &error)) {
                writeMessage(jsonResponse(requestId, toolResult(QJsonObject{
                    { QStringLiteral("error"), error },
                    { QStringLiteral("phase"), QStringLiteral("wait") },
                    { QStringLiteral("targetResponse"), wait },
                }, true)));
                return;
            }

            const QString kind = call.kind == PendingCall::Kind::WorkflowLongPressAndWait
                    ? QStringLiteral("long-press-and-wait")
                    : QStringLiteral("click-and-wait");
            QJsonObject report = clickAndWaitWorkflowReport(kind,
                                                            call.targetSelector,
                                                            call.waitSelector,
                                                            call.waitUntil,
                                                            targetQuery,
                                                            input,
                                                            events,
                                                            wait);
            report.insert(QStringLiteral("gateway"), QStringLiteral("qmlagent-launcher"));
            report.insert(QStringLiteral("eventStream"), QJsonObject{
                { QStringLiteral("included"), false },
                { QStringLiteral("reason"),
                  QStringLiteral("Launcher-routed workflows use request/response calls; use direct MCP attach for streamed event capture.") },
            });
            writeMessage(jsonResponse(requestId, toolResult(
                    call.verbosity == QLatin1String("summary")
                            ? summarizedWorkflowReport(report)
                            : report)));
            return;
        }

        QJsonObject afterQuery;
        if (!launcherWorkflowRequest(launcher, QStringLiteral("UI.query"), {
                { QStringLiteral("selector"), call.expectedSelector },
                { QStringLiteral("includeSource"), true },
                { QStringLiteral("properties"), properties },
            }, &afterQuery, &error)) {
            writeMessage(jsonResponse(requestId, toolResult(QJsonObject{
                { QStringLiteral("error"), error },
                { QStringLiteral("phase"), QStringLiteral("after-query") },
                { QStringLiteral("targetResponse"), afterQuery },
            }, true)));
            return;
        }

        const QString kind = call.kind == PendingCall::Kind::WorkflowClick
                ? QStringLiteral("click-and-verify")
                : QStringLiteral("key-and-verify");
        QJsonObject report = groupedWorkflowReport(kind, call.targetSelector, call.key,
                                                   call.expectedSelector,
                                                   call.expectation,
                                                   targetQuery,
                                                   beforeQuery,
                                                   input,
                                                   events,
                                                   afterQuery);
        report.insert(QStringLiteral("gateway"), QStringLiteral("qmlagent-launcher"));
        report.insert(QStringLiteral("eventStream"), QJsonObject{
            { QStringLiteral("included"), false },
            { QStringLiteral("reason"),
              QStringLiteral("Launcher-routed workflows use request/response calls; use direct MCP attach for streamed event capture.") },
        });
        writeMessage(jsonResponse(requestId, toolResult(
                call.verbosity == QLatin1String("summary")
                        ? summarizedWorkflowReport(report)
                        : report)));
    }

    static void appendBounded(QJsonArray *array, const QJsonObject &value, int limit = 20)
    {
        array->append(value);
        while (array->size() > limit)
            array->removeFirst();
    }

    QJsonObject connectTcpTarget(const QString &host, quint16 port, int timeoutMs)
    {
        const QString endpoint = QmlAgentTargetLease::tcpEndpoint(host, port);
        if (m_targetLease.isLocked() && m_targetLease.endpoint() == endpoint
                && isTargetConnected()) {
            return targetStatus();
        }

        QmlAgentTargetLease newLease;
        QString leaseError;
        if (!newLease.acquire(endpoint, QStringLiteral("mcp"), &leaseError)) {
            m_lastError = leaseError;
            return targetStatus();
        }

        disconnectTarget(QStringLiteral("reconnect"));
        m_recentEvents = {};
        m_recentLogEntries = {};
        m_lastSessionInfo = {};
        m_host = host;
        m_port = port;
        m_socketPath.clear();
        m_lastError.clear();
        m_disconnectReason.clear();
        m_targetLease = std::move(newLease);

        createTargetConnection();
        m_connection->connectToHost(host, port);
        if (!m_connection->waitForConnected(timeoutMs)) {
            m_lastError = QStringLiteral("Timed out connecting to QmlAgent target at %1:%2. "
                                         "If the target is already running, it may already have "
                                         "an attached QML debug client.")
                                  .arg(host)
                                  .arg(port);
            disconnectTarget(QStringLiteral("connect-timeout"));
            return targetStatus();
        }

        if (m_client->state() != QQmlDebugClient::Enabled) {
            m_lastError = QStringLiteral("Connected to %1:%2, but the QmlAgent service is not "
                                         "enabled. Either the target was launched without "
                                         "services:QmlAgent in -qmljsdebugger, or the "
                                         "qmldbg_agent plugin is missing from the Qt the "
                                         "target links against "
                                         "(<Qt prefix>/plugins/qmltooling/).")
                                  .arg(host)
                                  .arg(port);
            disconnectTarget(QStringLiteral("service-not-enabled"));
            return targetStatus();
        }

        refreshTargetSessionInfo(qMin(timeoutMs, 1000));
        enableStartupLogReplay();
        return targetStatus();
    }

    QJsonObject connectLocalSocketTarget(const QString &path, int timeoutMs)
    {
        const QString endpoint = QmlAgentTargetLease::localSocketEndpoint(path);
        if (m_targetLease.isLocked() && m_targetLease.endpoint() == endpoint
                && isTargetConnected()) {
            return targetStatus();
        }

        const QFileInfo socketInfo(path);
        if (socketInfo.exists() || socketInfo.isSymLink()) {
            m_lastError = QStringLiteral("Refusing to listen on existing local socket path %1. "
                                         "qmlagent-mcp will not remove caller-supplied paths; "
                                         "delete stale sockets yourself or choose a fresh path.")
                                  .arg(path);
            return targetStatus();
        }

        QmlAgentTargetLease newLease;
        QString leaseError;
        if (!newLease.acquire(endpoint, QStringLiteral("mcp"), &leaseError)) {
            m_lastError = leaseError;
            return targetStatus();
        }

        disconnectTarget(QStringLiteral("reconnect"));
        m_recentEvents = {};
        m_recentLogEntries = {};
        m_lastSessionInfo = {};
        m_host.clear();
        m_port = 0;
        m_socketPath = path;
        m_lastError.clear();
        m_disconnectReason.clear();
        m_targetLease = std::move(newLease);

        createTargetConnection();
        m_connection->startLocalServer(path);
        if (!m_connection->waitForConnected(timeoutMs)) {
            m_lastError = QStringLiteral("Timed out waiting for QmlAgent target on local socket %1.")
                                  .arg(path);
            disconnectTarget(QStringLiteral("connect-timeout"));
            return targetStatus();
        }

        if (m_client->state() != QQmlDebugClient::Enabled) {
            m_lastError = QStringLiteral("Connected on local socket %1, but the QmlAgent service "
                                         "is not enabled. Either the target was launched without "
                                         "services:QmlAgent in -qmljsdebugger, or the "
                                         "qmldbg_agent plugin is missing from the Qt the target "
                                         "links against (<Qt prefix>/plugins/qmltooling/).")
                                  .arg(path);
            disconnectTarget(QStringLiteral("service-not-enabled"));
            return targetStatus();
        }

        refreshTargetSessionInfo(qMin(timeoutMs, 1000));
        enableStartupLogReplay();
        return targetStatus();
    }

    void refreshTargetSessionInfo(int timeoutMs)
    {
        if (!m_client || m_currentCall.has_value() || m_workflow.has_value())
            return;

        const int targetId = ++m_nextTargetId;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QMetaObject::Connection responseConnection;
        responseConnection = connect(m_client.get(), &QmlAgentClient::received, this,
                                     [this, targetId, &loop](const QByteArray &message) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(message, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return;
            const QJsonObject response = document.object();
            if (response.value(QStringLiteral("id")).toInt(-1) != targetId)
                return;
            const QJsonObject result = response.value(QStringLiteral("result")).toObject();
            if (!result.isEmpty())
                m_lastSessionInfo = result;
            loop.quit();
        });
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(qMax(1, timeoutMs));
        m_client->sendMessage(compactJson(makeRequest(targetId, QStringLiteral("Session.getInfo"), {})));
        loop.exec();
        disconnect(responseConnection);
    }

    void enableStartupLogReplay()
    {
        if (!m_client)
            return;
        const int targetId = ++m_nextTargetId;
        m_client->sendMessage(compactJson(makeRequest(targetId, QStringLiteral("Log.enable"), {
            { QStringLiteral("replayBuffered"), true },
        })));
    }

    void createTargetConnection()
    {
        m_connection = std::make_unique<QQmlDebugConnection>();
        m_client = std::make_unique<QmlAgentClient>(m_connection.get());
        connect(m_client.get(), &QmlAgentClient::received,
                this, &QmlAgentMcpServer::handleTargetMessage);
        connect(m_connection.get(), &QQmlDebugConnection::disconnected, this, [this]() {
            m_disconnectReason = QStringLiteral("target-disconnected");
            m_uiSubscribed = false;
            m_targetLease.release();
            if (m_currentCall.has_value()) {
                writeMessage(jsonResponse(m_currentCall->mcpId,
                                          toolErrorResult(QStringLiteral("QmlAgent target disconnected while waiting for %1.")
                                                                  .arg(m_currentCall->targetMethod))));
                m_currentCall.reset();
            }
            if (m_workflow.has_value()) {
                const QJsonValue mcpId = m_workflow->call.mcpId;
                m_workflow.reset();
                writeMessage(jsonResponse(mcpId, toolErrorResult(
                        QStringLiteral("QmlAgent target disconnected during workflow."))));
            }
        });
        connect(m_connection.get(), &QQmlDebugConnection::socketError, this,
                [this](QAbstractSocket::SocketError error) {
            m_lastError = QStringLiteral("socket error %1").arg(int(error));
        });
    }

    void disconnectTarget(const QString &reason)
    {
        m_commandTimeout.stop();
        m_currentCall.reset();
        m_workflow.reset();
        m_queue.clear();
        m_uiSubscribed = false;
        m_lastSessionInfo = {};
        if (m_connection)
            m_connection->close();
        m_client.reset();
        m_connection.reset();
        m_targetLease.release();
        if (reason != QLatin1String("reconnect"))
            m_disconnectReason = reason;
    }

    void dispatchNextTargetCommand()
    {
        if (m_currentCall.has_value() || m_workflow.has_value())
            return;

        if (m_queue.isEmpty()) {
            quitIfInputClosedAndIdle();
            return;
        }

        PendingCall call = m_queue.dequeue();
        if (call.kind == PendingCall::Kind::Disconnect) {
            disconnectTarget(QStringLiteral("client-request"));
            writeMessage(jsonResponse(call.mcpId, toolResult(targetStatus())));
            dispatchNextTargetCommand();
            return;
        }

        if (call.kind != PendingCall::Kind::TargetCommand) {
            startWorkflow(call);
            return;
        }

        call.targetId = ++m_nextTargetId;
        m_currentCall = call;
        m_commandTimeout.start(m_timeoutMs);
        m_client->sendMessage(compactJson(makeRequest(m_currentCall->targetId,
                                                       m_currentCall->targetMethod,
                                                       m_currentCall->targetParams)));
    }

    void handleTargetMessage(const QByteArray &message)
    {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(message, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            return;

        const QJsonObject object = document.object();
        if (!object.contains(QStringLiteral("id"))) {
            if (m_workflow.has_value())
                m_workflow->events.append(object);
            const QString targetMethod = object.value(QStringLiteral("method")).toString();
            appendBounded(&m_recentEvents, object);
            if (targetMethod == QLatin1String("Log.entryAdded")) {
                const QJsonObject entry = object.value(QStringLiteral("params")).toObject()
                        .value(QStringLiteral("entry")).toObject();
                if (!entry.isEmpty())
                    appendBounded(&m_recentLogEntries, entry);
            }
            writeMessage({
                { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
                { QStringLiteral("method"), QStringLiteral("notifications/message") },
                { QStringLiteral("params"), QJsonObject{
                    { QStringLiteral("level"), QStringLiteral("info") },
                    { QStringLiteral("logger"), QStringLiteral("qmlagent") },
                    { QStringLiteral("message"), targetMethod.isEmpty()
                      ? QStringLiteral("QmlAgent event")
                      : QStringLiteral("QmlAgent event: %1").arg(targetMethod) },
                    { QStringLiteral("data"), object },
                } },
            });
            return;
        }

        if (m_workflow.has_value()
                && object.value(QStringLiteral("id")).toInt(-1) == m_workflow->targetId) {
            handleWorkflowResponse(object);
            return;
        }

        if (!m_currentCall.has_value()
                || object.value(QStringLiteral("id")).toInt(-1) != m_currentCall->targetId) {
            return;
        }

        m_commandTimeout.stop();
        updateSubscriptionState(*m_currentCall, object);
        const bool isError = object.contains(QStringLiteral("error"));
        QJsonValue payload = isError ? QJsonValue(object) : object.value(QStringLiteral("result"));
        if (!isError && m_currentCall->verbosity == QLatin1String("summary")
                && m_currentCall->targetMethod == QLatin1String("UI.query")) {
            payload = summarizedQueryResult(payload.toObject());
        }
        if (!isError && m_currentCall->verbosity == QLatin1String("summary")
                && m_currentCall->targetMethod == QLatin1String("UI.queryMany")) {
            payload = summarizedQueryManyResult(payload.toObject());
        }
        if (!isError)
            payload = applyScreenshotOutPath(payload, *m_currentCall);
        writeMessage(jsonResponse(m_currentCall->mcpId, toolResult(payload, isError)));
        m_currentCall.reset();
        dispatchNextTargetCommand();
    }

    void handleInputClosed()
    {
        m_inputClosed = true;
        quitIfInputClosedAndIdle();
    }

    void handleStdinActivated()
    {
        if (m_stdinNotifier)
            m_stdinNotifier->setEnabled(false);

        char buffer[4096];
        while (true) {
            const ssize_t bytesRead = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (bytesRead > 0) {
                m_stdinBuffer.append(buffer, qsizetype(bytesRead));
                processBufferedInputLines();
                continue;
            }

            if (bytesRead == 0) {
                processBufferedInputLines(true);
                if (m_stdinNotifier)
                    m_stdinNotifier->setEnabled(false);
                handleInputClosed();
                return;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (m_stdinNotifier && !m_inputClosed)
                    m_stdinNotifier->setEnabled(true);
                return;
            }

            if (m_stdinNotifier)
                m_stdinNotifier->setEnabled(false);
            handleInputClosed();
            return;
        }
    }

    void processBufferedInputLines(bool flushRemainder = false)
    {
        while (true) {
            const qsizetype newline = m_stdinBuffer.indexOf('\n');
            if (newline < 0)
                break;

            const QByteArray line = m_stdinBuffer.left(newline + 1);
            m_stdinBuffer.remove(0, newline + 1);
            if (!line.trimmed().isEmpty())
                handleInputLine(line);
        }

        if (flushRemainder && !m_stdinBuffer.trimmed().isEmpty()) {
            const QByteArray line = m_stdinBuffer;
            m_stdinBuffer.clear();
            handleInputLine(line);
        }
    }

    void quitIfInputClosedAndIdle()
    {
        if (m_inputClosed && !m_currentCall.has_value() && !m_workflow.has_value()
                && m_queue.isEmpty()) {
            QCoreApplication::quit();
        }
    }

    void targetCommandTimedOut()
    {
        if (m_workflow.has_value()) {
            finishWorkflowWithError(QStringLiteral("Timed out waiting for %1 response.")
                                            .arg(m_workflow->targetMethod),
                                    {}, false);
            return;
        }

        if (!m_currentCall.has_value())
            return;

        writeMessage(jsonResponse(m_currentCall->mcpId,
                                  toolErrorResult(QStringLiteral("Timed out waiting for %1 response.")
                                                          .arg(m_currentCall->targetMethod))));
        m_currentCall.reset();
        dispatchNextTargetCommand();
    }

    void updateSubscriptionState(const PendingCall &call, const QJsonObject &response)
    {
        if (response.contains(QStringLiteral("error")))
            return;
        if (call.targetMethod == QLatin1String("UI.subscribe"))
            m_uiSubscribed = true;
        else if (call.targetMethod == QLatin1String("UI.unsubscribe"))
            m_uiSubscribed = false;
    }

    void startWorkflow(const PendingCall &call)
    {
        WorkflowState state;
        state.call = call;
        state.subscribedBefore = m_uiSubscribed;
        m_workflow = state;
        sendWorkflowRequest(WorkflowState::Phase::TargetQuery, QStringLiteral("UI.query"), {
            { QStringLiteral("selector"), call.targetSelector },
            { QStringLiteral("includeSource"), true },
        });
    }

    void sendWorkflowRequest(WorkflowState::Phase phase, const QString &method,
                             const QJsonObject &params)
    {
        if (!m_workflow.has_value())
            return;

        m_workflow->phase = phase;
        m_workflow->targetId = ++m_nextTargetId;
        m_workflow->targetMethod = method;
        m_commandTimeout.start(m_timeoutMs);
        m_client->sendMessage(compactJson(makeRequest(m_workflow->targetId, method, params)));
    }

    void handleWorkflowResponse(const QJsonObject &response)
    {
        m_commandTimeout.stop();
        if (!m_workflow.has_value())
            return;

        if (m_workflow->finishingWithError
                && m_workflow->phase == WorkflowState::Phase::Unsubscribe) {
            if (!response.contains(QStringLiteral("error")))
                m_uiSubscribed = false;
            finishWorkflowWithErrorPayload(m_workflow->errorPayload);
            return;
        }

        if (response.contains(QStringLiteral("error"))) {
            finishWorkflowWithError(QStringLiteral("%1 failed in %2.")
                                            .arg(m_workflow->targetMethod,
                                                 workflowPhaseName(m_workflow->phase)),
                                    response);
            return;
        }

        const PendingCall call = m_workflow->call;
        const QJsonArray properties{ call.expectation.property };
        switch (m_workflow->phase) {
        case WorkflowState::Phase::TargetQuery:
            m_workflow->targetQuery = response;
            if (call.kind == PendingCall::Kind::WorkflowClickAndWait
                    || call.kind == PendingCall::Kind::WorkflowLongPressAndWait) {
                if (m_workflow->subscribedBefore) {
                    sendWorkflowInput();
                    return;
                }
                sendWorkflowRequest(WorkflowState::Phase::Subscribe, QStringLiteral("UI.subscribe"), {});
                return;
            }
            sendWorkflowRequest(WorkflowState::Phase::BeforeQuery, QStringLiteral("UI.query"), {
                { QStringLiteral("selector"), call.expectedSelector },
                { QStringLiteral("includeSource"), true },
                { QStringLiteral("properties"), properties },
            });
            return;
        case WorkflowState::Phase::BeforeQuery:
            m_workflow->beforeQuery = response;
            if (m_workflow->subscribedBefore) {
                sendWorkflowInput();
                return;
            }
            sendWorkflowRequest(WorkflowState::Phase::Subscribe, QStringLiteral("UI.subscribe"), {});
            return;
        case WorkflowState::Phase::Subscribe:
            m_uiSubscribed = true;
            m_workflow->temporarySubscriptionActive = true;
            sendWorkflowInput();
            return;
        case WorkflowState::Phase::Input:
            m_workflow->input = response;
            if (call.kind == PendingCall::Kind::WorkflowClickAndWait
                    || call.kind == PendingCall::Kind::WorkflowLongPressAndWait) {
                QJsonObject waitParams{
                    { QStringLiteral("selector"), call.waitSelector },
                    { QStringLiteral("until"), call.waitUntil },
                };
                if (call.waitTimeoutMs >= 0)
                    waitParams.insert(QStringLiteral("timeoutMs"), call.waitTimeoutMs);
                sendWorkflowRequest(WorkflowState::Phase::Wait, QStringLiteral("UI.waitFor"),
                                    waitParams);
                return;
            }
            sendWorkflowRequest(WorkflowState::Phase::AfterQuery, QStringLiteral("UI.query"), {
                { QStringLiteral("selector"), call.expectedSelector },
                { QStringLiteral("includeSource"), true },
                { QStringLiteral("properties"), properties },
            });
            return;
        case WorkflowState::Phase::Wait:
            m_workflow->wait = response;
            if (m_workflow->subscribedBefore) {
                finishWorkflow();
                return;
            }
            sendWorkflowRequest(WorkflowState::Phase::Unsubscribe, QStringLiteral("UI.unsubscribe"), {});
            return;
        case WorkflowState::Phase::AfterQuery:
            m_workflow->afterQuery = response;
            if (m_workflow->subscribedBefore) {
                finishWorkflow();
                return;
            }
            sendWorkflowRequest(WorkflowState::Phase::Unsubscribe, QStringLiteral("UI.unsubscribe"), {});
            return;
        case WorkflowState::Phase::Unsubscribe:
            m_uiSubscribed = false;
            finishWorkflow();
            return;
        }
    }

    void sendWorkflowInput()
    {
        if (!m_workflow.has_value())
            return;

        const PendingCall call = m_workflow->call;
        if (call.kind == PendingCall::Kind::WorkflowClick
                || call.kind == PendingCall::Kind::WorkflowClickAndWait) {
            sendWorkflowRequest(WorkflowState::Phase::Input, QStringLiteral("Input.clickNode"), {
                { QStringLiteral("selector"), call.targetSelector },
            });
            return;
        }
        if (call.kind == PendingCall::Kind::WorkflowLongPressAndWait) {
            sendWorkflowRequest(WorkflowState::Phase::Input, QStringLiteral("Input.longPressNode"), {
                { QStringLiteral("selector"), call.targetSelector },
                { QStringLiteral("holdMs"), call.holdMs },
            });
            return;
        }

        sendWorkflowRequest(WorkflowState::Phase::Input, QStringLiteral("Input.dispatchKeyEvent"), {
            { QStringLiteral("selector"), call.targetSelector },
            { QStringLiteral("key"), call.key },
        });
    }

    void finishWorkflow()
    {
        if (!m_workflow.has_value())
            return;

        const PendingCall call = m_workflow->call;
        if (call.kind == PendingCall::Kind::WorkflowClickAndWait
                || call.kind == PendingCall::Kind::WorkflowLongPressAndWait) {
            const QString kind = call.kind == PendingCall::Kind::WorkflowLongPressAndWait
                    ? QStringLiteral("long-press-and-wait")
                    : QStringLiteral("click-and-wait");
            const QJsonObject report = clickAndWaitWorkflowReport(kind,
                                                                  call.targetSelector,
                                                                  call.waitSelector,
                                                                  call.waitUntil,
                                                                  m_workflow->targetQuery,
                                                                  m_workflow->input,
                                                                  m_workflow->events,
                                                                  m_workflow->wait);
            writeMessage(jsonResponse(call.mcpId, toolResult(
                    call.verbosity == QLatin1String("summary")
                            ? summarizedWorkflowReport(report)
                            : report)));
            m_workflow.reset();
            dispatchNextTargetCommand();
            return;
        }
        const QString kind = call.kind == PendingCall::Kind::WorkflowClick
                ? QStringLiteral("click-and-verify")
                : QStringLiteral("key-and-verify");
        const QJsonObject report = groupedWorkflowReport(kind, call.targetSelector, call.key,
                                                         call.expectedSelector,
                                                         call.expectation,
                                                         m_workflow->targetQuery,
                                                         m_workflow->beforeQuery,
                                                         m_workflow->input,
                                                         m_workflow->events,
                                                         m_workflow->afterQuery);
        writeMessage(jsonResponse(call.mcpId, toolResult(
                call.verbosity == QLatin1String("summary")
                        ? summarizedWorkflowReport(report)
                        : report)));
        m_workflow.reset();
        dispatchNextTargetCommand();
    }

    void finishWorkflowWithError(const QString &message, const QJsonObject &targetResponse = {},
                                 bool cleanupTemporarySubscription = true)
    {
        if (!m_workflow.has_value())
            return;

        QJsonObject payload{
            { QStringLiteral("error"), message },
            { QStringLiteral("phase"), workflowPhaseName(m_workflow->phase) },
        };
        if (!targetResponse.isEmpty())
            payload.insert(QStringLiteral("targetResponse"), targetResponse);
        if (cleanupTemporarySubscription
                && m_workflow->temporarySubscriptionActive
                && !m_workflow->finishingWithError
                && m_workflow->phase != WorkflowState::Phase::Unsubscribe) {
            m_workflow->finishingWithError = true;
            m_workflow->errorPayload = payload;
            sendWorkflowRequest(WorkflowState::Phase::Unsubscribe,
                                QStringLiteral("UI.unsubscribe"), {});
            return;
        }

        finishWorkflowWithErrorPayload(payload);
    }

    void finishWorkflowWithErrorPayload(const QJsonObject &payload)
    {
        if (!m_workflow.has_value())
            return;

        const QJsonValue mcpId = m_workflow->call.mcpId;
        m_workflow.reset();
        writeMessage(jsonResponse(mcpId, toolResult(payload, true)));
        dispatchNextTargetCommand();
    }

    std::unique_ptr<QQmlDebugConnection> m_connection;
    std::unique_ptr<QmlAgentClient> m_client;
    int m_timeoutMs = 5000;
    QString m_host;
    quint16 m_port = 0;
    QString m_socketPath;
    QString m_lastError;
    QString m_disconnectReason;
    QSocketNotifier *m_stdinNotifier = nullptr;
    QByteArray m_stdinBuffer;
    QQueue<PendingCall> m_queue;
    std::optional<PendingCall> m_currentCall;
    std::optional<WorkflowState> m_workflow;
    QJsonArray m_recentEvents;
    QJsonArray m_recentLogEntries;
    QJsonObject m_lastSessionInfo;
    QTimer m_commandTimeout;
    int m_nextTargetId = 0;
    bool m_uiSubscribed = false;
    bool m_inputClosed = false;
    QmlAgentTargetLease m_targetLease;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString executableName = QFileInfo(QString::fromLocal8Bit(argv[0])).baseName();
    QCoreApplication::setApplicationName(executableName);
    QCoreApplication::setApplicationVersion(QStringLiteral(QMLAGENT_VERSION_STR));

    QStringList rawArguments = QCoreApplication::arguments();
    if (executableName == QLatin1String("qmlagentctl"))
        return runCtlSubcommand(rawArguments);

    if (executableName == QLatin1String("qmlagent-mcp")) {
        QCommandLineParser parser;
        parser.setApplicationDescription(QStringLiteral("QmlAgent stdio MCP server."));
        parser.addHelpOption();
        parser.addVersionOption();
        const QCommandLineOption timeoutOption(QStringLiteral("timeout"),
                                               QStringLiteral("Connection/request timeout in milliseconds."),
                                               QStringLiteral("ms"), QStringLiteral("5000"));
        parser.addOption(timeoutOption);
        parser.process(rawArguments);

        bool ok = false;
        const int timeoutMs = parser.value(timeoutOption).toInt(&ok);
        if (!ok || timeoutMs <= 0)
            return fail(QStringLiteral("--timeout must be a positive integer."));

        QmlAgentMcpServer server(timeoutMs, &app);
        return app.exec();
    }

    return fail(QStringLiteral("Unsupported QmlAgent executable name '%1'. Use qmlagentctl or qmlagent-mcp.")
                        .arg(executableName));
}

#include "main.moc"
