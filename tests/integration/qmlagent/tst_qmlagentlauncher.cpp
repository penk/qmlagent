// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include <QtCore/qdir.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qprocess.h>
#include <QtCore/qsavefile.h>
#include <QtCore/qtemporarydir.h>
#include <QtTest/qtest.h>

#include <limits>
#include <memory>

namespace {
constexpr int ProcessTimeoutMs = 5000;
constexpr qint64 UnobservablePid = std::numeric_limits<int>::max();

struct ProcessResult
{
    int exitCode = -1;
    QByteArray output;
    QByteArray error;
};

class OwnedProcess : public QProcess
{
public:
    ~OwnedProcess() override
    {
        if (state() == NotRunning)
            return;
        terminate();
        if (!waitForFinished(ProcessTimeoutMs)) {
            kill();
            waitForFinished(ProcessTimeoutMs);
        }
    }
};

static QJsonObject parseObject(const QByteArray &bytes)
{
    return QJsonDocument::fromJson(bytes).object();
}

static QByteArray jsonLine(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

static QJsonObject toolRequest(const QString &name, const QJsonObject &arguments)
{
    return {
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("id"), 2 },
        { QStringLiteral("method"), QStringLiteral("tools/call") },
        { QStringLiteral("params"), QJsonObject{
            { QStringLiteral("name"), name },
            { QStringLiteral("arguments"), arguments },
        } },
    };
}
} // namespace

class QmlAgentLauncherTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void reachableMailboxIgnoresRegistryPid_data();
    void reachableMailboxIgnoresRegistryPid();
    void unreachableMailboxRetainsRegistry_data();
    void unreachableMailboxRetainsRegistry();
    void stopDoesNotSignalRegistryPids_data();
    void stopDoesNotSignalRegistryPids();

private:
    void configure(QProcess *process) const;
    ProcessResult run(const QString &program, const QStringList &arguments,
                      const QByteArray &input = {}) const;
    ProcessResult ctl(const QStringList &arguments) const;
    QJsonObject mcp(const QString &tool, const QJsonObject &arguments) const;
    QStringList registryPaths(const QString &sessionId) const;
    bool writeRegistry(const QStringList &paths, const QJsonObject &metadata) const;
    void verifyRegistry(const QStringList &paths, const QByteArray &expected) const;
    QJsonObject launch(OwnedProcess *launcher) const;

    std::unique_ptr<QTemporaryDir> m_root;
    QProcessEnvironment m_environment;
};

void QmlAgentLauncherTest::init()
{
    m_root = std::make_unique<QTemporaryDir>();
    QVERIFY2(m_root->isValid(), qPrintable(m_root->errorString()));
    m_environment = QProcessEnvironment::systemEnvironment();
    const QDir root(m_root->path());
    const QStringList names{ QStringLiteral("tmp"), QStringLiteral("data"),
                             QStringLiteral("cache") };
    const QStringList variables{ QStringLiteral("TMPDIR"), QStringLiteral("XDG_DATA_HOME"),
                                 QStringLiteral("XDG_CACHE_HOME") };
    for (int i = 0; i < names.size(); ++i) {
        QVERIFY(root.mkpath(names.at(i)));
        m_environment.insert(variables.at(i), root.filePath(names.at(i)));
    }
    m_environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
}

void QmlAgentLauncherTest::configure(QProcess *process) const
{
    process->setWorkingDirectory(m_root->path());
    process->setProcessEnvironment(m_environment);
}

ProcessResult QmlAgentLauncherTest::run(const QString &program, const QStringList &arguments,
                                       const QByteArray &input) const
{
    OwnedProcess process;
    configure(&process);
    process.start(program, arguments);
    if (!process.waitForStarted(ProcessTimeoutMs))
        return { -1, {}, process.errorString().toUtf8() };
    if (!input.isEmpty())
        process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(ProcessTimeoutMs))
        return { -1, process.readAllStandardOutput(), process.errorString().toUtf8() };
    return { process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
             process.readAllStandardOutput(), process.readAllStandardError() };
}

ProcessResult QmlAgentLauncherTest::ctl(const QStringList &arguments) const
{
    QStringList options = arguments;
    options.append({ QStringLiteral("--format"), QStringLiteral("compact"),
                     QStringLiteral("--timeout"), QStringLiteral("1000") });
    return run(QString::fromLocal8Bit(TEST_QMLAGENTCTL_EXECUTABLE), options);
}

QJsonObject QmlAgentLauncherTest::mcp(const QString &tool, const QJsonObject &arguments) const
{
    const QByteArray input = QByteArrayLiteral(
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}\n")
            + jsonLine(toolRequest(tool, arguments));
    const ProcessResult result = run(QString::fromLocal8Bit(TEST_QMLAGENTMCP_EXECUTABLE),
                                    { QStringLiteral("--timeout"), QStringLiteral("1000") }, input);
    if (result.exitCode != 0)
        return { { QStringLiteral("processError"), QString::fromUtf8(result.error) } };
    for (const QByteArray &line : result.output.split('\n')) {
        const QJsonObject response = parseObject(line);
        if (response.value(QStringLiteral("id")).toInt() == 2)
            return response.value(QStringLiteral("result")).toObject();
    }
    return { { QStringLiteral("missingResponse"), QString::fromUtf8(result.output) } };
}

QStringList QmlAgentLauncherTest::registryPaths(const QString &sessionId) const
{
    const QDir root(m_root->path());
    const QString name = sessionId + QStringLiteral(".json");
    return {
        root.filePath(QStringLiteral(".qmlagent/launcher-sessions/") + name),
        root.filePath(QStringLiteral("tmp/QmlAgent/launcher-sessions/") + name),
        root.filePath(QStringLiteral("data/QmlAgent/launcher-sessions/") + name),
    };
}

bool QmlAgentLauncherTest::writeRegistry(const QStringList &paths,
                                        const QJsonObject &metadata) const
{
    for (const QString &path : paths) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(jsonLine(metadata)) < 0 || !file.commit())
            return false;
    }
    return true;
}

void QmlAgentLauncherTest::verifyRegistry(const QStringList &paths, const QByteArray &expected) const
{
    for (const QString &path : paths) {
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path + file.errorString()));
        QCOMPARE(file.readAll(), expected);
    }
}

QJsonObject QmlAgentLauncherTest::launch(OwnedProcess *launcher) const
{
    configure(launcher);
    launcher->start(QString::fromLocal8Bit(TEST_QMLAGENTLAUNCHER_EXECUTABLE), {
        QStringLiteral("--print-json"), QStringLiteral("app"),
        QString::fromLocal8Bit(TEST_QMLAGENTSMOKE_EXECUTABLE),
    });
    if (!launcher->waitForStarted(ProcessTimeoutMs))
        return {};
    QElapsedTimer timer;
    timer.start();
    QByteArray pending;
    while (timer.elapsed() < ProcessTimeoutMs) {
        pending += launcher->readAllStandardOutput();
        while (pending.contains('\n')) {
            const qsizetype end = pending.indexOf('\n');
            const QJsonObject event = parseObject(pending.left(end));
            pending.remove(0, end + 1);
            if (event.value(QStringLiteral("event")) == QLatin1String("sessionReady"))
                return event;
        }
        if (launcher->state() == QProcess::NotRunning)
            break;
        launcher->waitForReadyRead(ProcessTimeoutMs - int(timer.elapsed()));
    }
    return {};
}

void QmlAgentLauncherTest::reachableMailboxIgnoresRegistryPid_data()
{
    QTest::addColumn<bool>("useMcp");
    QTest::newRow("cli") << false;
    QTest::newRow("mcp") << true;
}

void QmlAgentLauncherTest::reachableMailboxIgnoresRegistryPid()
{
    QFETCH(bool, useMcp);
    OwnedProcess launcher;
    const QJsonObject ready = launch(&launcher);
    QVERIFY2(!ready.isEmpty(), launcher.readAllStandardError().constData());
    const QString sessionId = ready.value(QStringLiteral("launcherSession")).toString();
    const QStringList paths = registryPaths(sessionId);
    QFile file(paths.first());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject metadata = parseObject(file.readAll());
    file.close();
    metadata.insert(QStringLiteral("launcherPid"), UnobservablePid);
    QVERIFY(writeRegistry(paths, metadata));

    const ProcessResult sessions = ctl({ QStringLiteral("sessions") });
    QCOMPARE(sessions.exitCode, 0);
    const QJsonArray entries = parseObject(sessions.output).value(QStringLiteral("sessions")).toArray();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().toObject().value(QStringLiteral("controlReachable")).toBool(), true);
    const QString selector = QStringLiteral("objectName=\"smoke.content\"");
    const QJsonObject query = useMcp
            ? mcp(QStringLiteral("qmlagent_ui_query"), {
                { QStringLiteral("session"), sessionId },
                { QStringLiteral("selector"), selector },
                { QStringLiteral("properties"), QJsonArray{ QStringLiteral("visible") } },
                { QStringLiteral("verbosity"), QStringLiteral("full") },
              }).value(QStringLiteral("structuredContent")).toObject()
            : parseObject(ctl({ QStringLiteral("query"), selector, QStringLiteral("--session"),
                                sessionId, QStringLiteral("--property"), QStringLiteral("visible") })
                                  .output).value(QStringLiteral("result")).toObject();
    const QJsonArray matches = query.value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.first().toObject().value(QStringLiteral("properties")).toObject()
                     .value(QStringLiteral("visible")).toBool(), true);
    verifyRegistry(paths, jsonLine(metadata));
    QCOMPARE(ctl({ QStringLiteral("stop"), QStringLiteral("--session"), sessionId }).exitCode, 0);
}

void QmlAgentLauncherTest::unreachableMailboxRetainsRegistry_data()
{
    QTest::addColumn<bool>("createMailbox");
    QTest::newRow("missing") << false;
    QTest::newRow("unresponsive") << true;
}

void QmlAgentLauncherTest::unreachableMailboxRetainsRegistry()
{
    QFETCH(bool, createMailbox);
    const QString mailbox = QDir(m_root->path()).filePath(QStringLiteral("mailbox"));
    if (createMailbox)
        QVERIFY(QDir().mkpath(mailbox));
    const QStringList paths = registryPaths(QStringLiteral("unobservable"));
    const QJsonObject metadata{
        { QStringLiteral("launcherSession"), QStringLiteral("unobservable") },
        { QStringLiteral("launcherPid"), UnobservablePid },
        { QStringLiteral("controlMailbox"), mailbox },
    };
    QVERIFY(writeRegistry(paths, metadata));
    const ProcessResult result = ctl({ QStringLiteral("sessions") });
    QCOMPARE(result.exitCode, 0);
    const QJsonArray sessions = parseObject(result.output).value(QStringLiteral("sessions")).toArray();
    QCOMPARE(sessions.size(), 1);
    const QJsonObject status = sessions.first().toObject();
    QCOMPARE(status.value(QStringLiteral("controlReachable")).toBool(true), false);
    QVERIFY(!status.value(QStringLiteral("controlError")).toString().isEmpty());
    const QJsonObject failure = mcp(QStringLiteral("qmlagent_ui_query"), {
        { QStringLiteral("session"), QStringLiteral("unobservable") },
        { QStringLiteral("selector"), QStringLiteral("id=\"root\"") },
    });
    QCOMPARE(failure.value(QStringLiteral("isError")).toBool(), true);
    QVERIFY(failure.value(QStringLiteral("structuredContent")).toObject()
                    .value(QStringLiteral("error")).toString().contains(QStringLiteral("retained")));
    verifyRegistry(paths, jsonLine(metadata));
    QCOMPARE(QFileInfo::exists(mailbox), createMailbox);
}

void QmlAgentLauncherTest::stopDoesNotSignalRegistryPids_data()
{
    reachableMailboxIgnoresRegistryPid_data();
}

void QmlAgentLauncherTest::stopDoesNotSignalRegistryPids()
{
    QFETCH(bool, useMcp);
    OwnedProcess unrelated;
    configure(&unrelated);
    unrelated.start(QString::fromLocal8Bit(TEST_QMLAGENTSMOKE_EXECUTABLE), {});
    QVERIFY(unrelated.waitForStarted(ProcessTimeoutMs));
    const QStringList paths = registryPaths(QStringLiteral("foreign-pid"));
    const QJsonObject metadata{
        { QStringLiteral("launcherSession"), QStringLiteral("foreign-pid") },
        { QStringLiteral("launcherPid"), qint64(unrelated.processId()) },
        { QStringLiteral("targetPid"), qint64(unrelated.processId()) },
        { QStringLiteral("controlMailbox"), QDir(m_root->path()).filePath(QStringLiteral("absent")) },
    };
    QVERIFY(writeRegistry(paths, metadata));
    if (useMcp) {
        const QJsonObject result = mcp(QStringLiteral("qmlagent_launcher_stop"), {
            { QStringLiteral("session"), QStringLiteral("foreign-pid") },
        });
        QCOMPARE(result.value(QStringLiteral("isError")).toBool(), true);
    } else {
        const ProcessResult result = ctl({ QStringLiteral("stop"), QStringLiteral("--session"),
                                           QStringLiteral("foreign-pid") });
        QCOMPARE(result.exitCode, 1);
        QVERIFY(result.error.contains("retained"));
    }
    QCOMPARE(unrelated.waitForFinished(100), false);
    QCOMPARE(unrelated.state(), QProcess::Running);
    verifyRegistry(paths, jsonLine(metadata));
}

QTEST_GUILESS_MAIN(QmlAgentLauncherTest)
#include "tst_qmlagentlauncher.moc"
