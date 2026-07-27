// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include <private/qqmldebugclient_p.h>
#include <private/qqmldebugconnection_p.h>

#include <QtCore/qdeadlinetimer.h>
#include <QtCore/qdir.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qprocess.h>
#include <QtCore/qset.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qtenvironmentvariables.h>
#include <QtCore/qtimer.h>
#include <QtCore/quuid.h>
#include <QtNetwork/qhostaddress.h>
#include <QtNetwork/qtcpserver.h>
#include <QtTest/qtest.h>

#include <functional>
#include <optional>

static constexpr int ProcessReadyTimeoutMs = 10000;
static constexpr int DebugConnectionTimeoutMs = 5000;
static constexpr int RequestTimeoutMs = 5000;
static constexpr int ProcessShutdownTimeoutMs = 3000;

class QmlAgentDebugClient : public QQmlDebugClient
{
    Q_OBJECT

public:
    explicit QmlAgentDebugClient(QQmlDebugConnection *connection)
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

class QmlAgentIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void clickNodeDeliversSyntheticInput();
    void inputTargetsMultipleWindows();
    void uiSubscribeEmitsCoalescedChangeEvents();
    void uiTreeSupportsProjectionBoundsAndCollapse();
    void uiQuerySerializesRequestedValueTypes();
    void uiQueryDefaultsToVisibleNodes();
    void diagnosticsAnalyzeBindingReportsRuntimeProvenance();
    void sourceResolverClassifiesLoadedDelegateAndDynamicNodes();
    void sourceResolveNodeReportsObjectNameFallback();
    void qtQuickControlsExposeAuthoredIds();
    void diagnosticsReportLayoutFailures();
    void diagnosticsFixturesMatchExpectedIssues();
    void diagnosticsAnalyzeNodeHonorsChecks();
    void renderCaptureScreenshot();
    void logGetEntriesSupportsCursor();
    void selectorNotFoundRanksStableCandidates();
    void referenceClientConvenienceCommands();
    void referenceClientMcpPersistentMode();
    void referenceClientMcpRoutesThroughLauncher();
    void referenceClientPreviewReloadRefreshesSingleton();
    void referenceClientClicksOutOfBoundsUnclippedChild();
    void referenceClientClicksItemLocalPoint();
    void referenceClientMcpWorkflowReports();
    void referenceClientReportsSingleClientConflict();
    void referenceClientMcpRefusesExistingLocalSocketPath();
    void referenceClientMcpConnectsLocalSocket();
    void guiDispatchReportsTruthfulOutcomes();

private:
    static std::optional<QJsonObject> invoke(QmlAgentDebugClient *client, const QString &method,
                                             const QJsonObject &params, int id,
                                             QString *errorMessage,
                                             int timeoutMs = RequestTimeoutMs);
};

static QByteArray waitForOutput(QProcess *process, const QByteArray &marker)
{
    QByteArray output = process->readAll();
    QDeadlineTimer deadline(ProcessReadyTimeoutMs);

    while (!output.contains(marker) && !deadline.hasExpired()) {
        if (process->state() == QProcess::NotRunning) {
            output += process->readAll();
            break;
        }

        if (!process->waitForReadyRead(int(deadline.remainingTime())))
            continue;

        output += process->readAll();
    }

    return output;
}

static QString qmlagentMcpExecutable()
{
    return QString::fromLocal8Bit(TEST_QMLAGENTMCP_EXECUTABLE);
}

static QString qmlagentCtlExecutable()
{
    return QString::fromLocal8Bit(TEST_QMLAGENTCTL_EXECUTABLE);
}

static QString qmlagentLauncherExecutable()
{
    return QString::fromLocal8Bit(TEST_QMLAGENTLAUNCHER_EXECUTABLE);
}

static bool waitForEnabled(QQmlDebugClient *client);

class SmokeAppRunner
{
public:
    ~SmokeAppRunner()
    {
        stop();
    }

    bool start(QString *errorMessage)
    {
        const QString smokeApp = qEnvironmentVariable("QMLAGENT_SMOKE_APP");
        if (smokeApp.isEmpty()) {
            *errorMessage = QStringLiteral("Missing QMLAGENT_SMOKE_APP.");
            return false;
        }

        QTcpServer portServer;
        if (!portServer.listen(QHostAddress::LocalHost)) {
            *errorMessage = portServer.errorString();
            return false;
        }
        m_port = portServer.serverPort();
        portServer.close();

        return startWithExecutable(
                smokeApp,
                QStringLiteral("-qmljsdebugger=port:%1,host:127.0.0.1,services:QmlAgent,block")
                        .arg(m_port),
                QByteArrayLiteral("Waiting for connection on port"), errorMessage);
    }

    bool startControlsSmoke(QString *errorMessage)
    {
        const QString controlsApp = qEnvironmentVariable("QMLAGENT_CONTROLS_APP");
        if (controlsApp.isEmpty()) {
            *errorMessage = QStringLiteral("Missing QMLAGENT_CONTROLS_APP.");
            return false;
        }

        QTcpServer portServer;
        if (!portServer.listen(QHostAddress::LocalHost)) {
            *errorMessage = portServer.errorString();
            return false;
        }
        m_port = portServer.serverPort();
        portServer.close();

        // Controls smoke intentionally exercises the target app with the real/default
        // platform QPA. The parent test process may run under offscreen in CI, but
        // popups, focus, style geometry, and render-backed interaction bugs need a
        // real target window to be meaningful.
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("QT_QPA_PLATFORM"));
        return startWithExecutable(
                controlsApp,
                QStringLiteral("-qmljsdebugger=port:%1,host:127.0.0.1,services:QmlAgent,block")
                        .arg(m_port),
                QByteArrayLiteral("Waiting for connection on port"), errorMessage, {},
                environment);
    }

    bool startLocalSocket(const QString &socketName, QString *errorMessage)
    {
        const QString smokeApp = qEnvironmentVariable("QMLAGENT_SMOKE_APP");
        if (smokeApp.isEmpty()) {
            *errorMessage = QStringLiteral("Missing QMLAGENT_SMOKE_APP.");
            return false;
        }

        return startWithExecutable(
                smokeApp,
                QStringLiteral("-qmljsdebugger=file:%1,services:QmlAgent,block").arg(socketName),
                QStringLiteral("Connecting to socket %1").arg(socketName).toUtf8(), errorMessage);
    }

    bool startLauncherPreview(const QString &qmlFile, QString *errorMessage,
                              const QString &workingDirectory = {})
    {
        const QString launcher = qmlagentLauncherExecutable();
        if (launcher.isEmpty()) {
            *errorMessage = QStringLiteral("Missing qmlagent-launcher test binary path.");
            return false;
        }

        m_process.setProcessChannelMode(QProcess::MergedChannels);
        if (!workingDirectory.isEmpty())
            m_process.setWorkingDirectory(workingDirectory);
        m_process.start(launcher, {
            QStringLiteral("--print-json"),
            QStringLiteral("--timeout"), QString::number(RequestTimeoutMs),
            QStringLiteral("preview"),
            qmlFile,
        });
        if (!m_process.waitForStarted()) {
            *errorMessage = m_process.errorString();
            return false;
        }

        const QByteArray output = waitForOutput(&m_process, QByteArrayLiteral("sessionReady"));
        if (!output.contains("sessionReady")) {
            *errorMessage = QString::fromUtf8(output);
            return false;
        }

        return true;
    }

    bool startLauncherApp(const QString &executable, QString *errorMessage,
                          const QStringList &applicationArguments = {})
    {
        const QString launcher = qmlagentLauncherExecutable();
        if (launcher.isEmpty()) {
            *errorMessage = QStringLiteral("Missing qmlagent-launcher test binary path.");
            return false;
        }

        QStringList arguments{
            QStringLiteral("--print-json"),
            QStringLiteral("--timeout"), QString::number(RequestTimeoutMs),
            QStringLiteral("app"),
            executable,
        };
        arguments.append(applicationArguments);

        m_process.setProcessChannelMode(QProcess::MergedChannels);
        m_process.start(launcher, arguments);
        if (!m_process.waitForStarted()) {
            *errorMessage = m_process.errorString();
            return false;
        }

        const QByteArray output = waitForOutput(&m_process, QByteArrayLiteral("sessionReady"));
        if (!output.contains("sessionReady")) {
            *errorMessage = QString::fromUtf8(output);
            return false;
        }

        return true;
    }

    bool startDiagnosticFixture(const QString &fixtureName, QString *errorMessage)
    {
        const QString fixtureApp = qEnvironmentVariable("QMLAGENT_FIXTURE_APP");
        if (fixtureApp.isEmpty()) {
            *errorMessage = QStringLiteral("Missing QMLAGENT_FIXTURE_APP.");
            return false;
        }

        const QString fixturesDir = qEnvironmentVariable("QMLAGENT_FIXTURES_DIR");
        if (fixturesDir.isEmpty()) {
            *errorMessage = QStringLiteral("Missing QMLAGENT_FIXTURES_DIR.");
            return false;
        }

        const QString qmlFile = QDir(fixturesDir).filePath(
                QStringLiteral("%1/Main.qml").arg(fixtureName));
        if (!QFileInfo::exists(qmlFile)) {
            *errorMessage = QStringLiteral("Missing diagnostic fixture QML: %1").arg(qmlFile);
            return false;
        }

        QTcpServer portServer;
        if (!portServer.listen(QHostAddress::LocalHost)) {
            *errorMessage = portServer.errorString();
            return false;
        }
        m_port = portServer.serverPort();
        portServer.close();

        return startWithExecutable(
                fixtureApp,
                QStringLiteral("-qmljsdebugger=port:%1,host:127.0.0.1,services:QmlAgent,block")
                        .arg(m_port),
                QByteArrayLiteral("Waiting for connection on port"), errorMessage,
                { qmlFile });
    }

    bool startWorkflowFixture(const QString &fixtureName, QString *errorMessage)
    {
        const QString fixtureApp = qEnvironmentVariable("QMLAGENT_FIXTURE_APP");
        if (fixtureApp.isEmpty()) {
            *errorMessage = QStringLiteral("Missing QMLAGENT_FIXTURE_APP.");
            return false;
        }

        const QString fixturesDir = qEnvironmentVariable("QMLAGENT_WORKFLOW_FIXTURES_DIR");
        if (fixturesDir.isEmpty()) {
            *errorMessage = QStringLiteral("Missing QMLAGENT_WORKFLOW_FIXTURES_DIR.");
            return false;
        }

        const QString qmlFile = QDir(fixturesDir).filePath(
                QStringLiteral("%1/Main.qml").arg(fixtureName));
        if (!QFileInfo::exists(qmlFile)) {
            *errorMessage = QStringLiteral("Missing workflow fixture QML: %1").arg(qmlFile);
            return false;
        }

        QTcpServer portServer;
        if (!portServer.listen(QHostAddress::LocalHost)) {
            *errorMessage = portServer.errorString();
            return false;
        }
        m_port = portServer.serverPort();
        portServer.close();

        return startWithExecutable(
                fixtureApp,
                QStringLiteral("-qmljsdebugger=port:%1,host:127.0.0.1,services:QmlAgent,block")
                        .arg(m_port),
                QByteArrayLiteral("Waiting for connection on port"), errorMessage,
                { qmlFile });
    }

    quint16 port() const { return m_port; }

    QByteArray readAllOutput()
    {
        return m_process.readAll();
    }

private:
    bool startWithExecutable(const QString &executable, const QString &debuggerArgument,
                             const QByteArray &readyMarker, QString *errorMessage,
                             const QStringList &applicationArguments = {},
                             const std::optional<QProcessEnvironment> &environment = std::nullopt)
    {
        m_process.setProcessChannelMode(QProcess::MergedChannels);
        if (environment.has_value())
            m_process.setProcessEnvironment(*environment);
        QStringList arguments{ debuggerArgument };
        arguments.append(applicationArguments);
        m_process.start(executable, arguments);
        if (!m_process.waitForStarted()) {
            *errorMessage = m_process.errorString();
            return false;
        }

        const QByteArray output = waitForOutput(&m_process, readyMarker);
        if (!output.contains(readyMarker)) {
            *errorMessage = QString::fromUtf8(output);
            return false;
        }

        return true;
    }

    void stop()
    {
        if (m_process.state() == QProcess::NotRunning)
            return;
        m_process.terminate();
        if (!m_process.waitForFinished(ProcessShutdownTimeoutMs)) {
            m_process.kill();
            m_process.waitForFinished();
        }
    }

    QProcess m_process;
    quint16 m_port = 0;
};

static bool connectToQmlAgent(quint16 port, QQmlDebugConnection *connection,
                              QmlAgentDebugClient *client, QString *errorMessage)
{
    connection->connectToHost(QStringLiteral("127.0.0.1"), port);
    if (!connection->waitForConnected(DebugConnectionTimeoutMs)) {
        *errorMessage = QStringLiteral("Timed out connecting to QML debug server.");
        return false;
    }
    if (!waitForEnabled(client)) {
        *errorMessage = QStringLiteral("QmlAgent service did not become enabled.");
        return false;
    }
    return true;
}

static QString selectorValue(const QJsonObject &node, const QString &kind)
{
    const QJsonArray selectors = node.value(QStringLiteral("selectors")).toArray();
    for (const QJsonValue &selectorValue : selectors) {
        const QJsonObject selector = selectorValue.toObject();
        if (selector.value(QStringLiteral("kind")).toString() == kind)
            return selector.value(QStringLiteral("value")).toString();
    }
    return {};
}

static QString sourceSelectorWithLineDelta(const QString &value, int delta)
{
    const int lastColon = value.lastIndexOf(QLatin1Char(':'));
    if (lastColon < 0)
        return {};

    bool ok = false;
    const int trailingNumber = value.mid(lastColon + 1).toInt(&ok);
    if (!ok || trailingNumber <= 0)
        return {};

    const QString beforeTrailingNumber = value.left(lastColon);
    const int previousColon = beforeTrailingNumber.lastIndexOf(QLatin1Char(':'));
    if (previousColon >= 0) {
        bool lineOk = false;
        const int line = beforeTrailingNumber.mid(previousColon + 1).toInt(&lineOk);
        if (lineOk && line > 0) {
            return QStringLiteral("%1:%2:%3")
                    .arg(beforeTrailingNumber.left(previousColon))
                    .arg(line + delta)
                    .arg(trailingNumber);
        }
    }

    return QStringLiteral("%1:%2").arg(beforeTrailingNumber).arg(trailingNumber + delta);
}

static bool waitForEnabled(QQmlDebugClient *client)
{
    if (client->state() == QQmlDebugClient::Enabled)
        return true;

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);

    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(client, &QQmlDebugClient::stateChanged, &loop,
                     [&](QQmlDebugClient::State state) {
        if (state == QQmlDebugClient::Enabled)
            loop.quit();
    });

    timeout.start(DebugConnectionTimeoutMs);
    loop.exec();
    return client->state() == QQmlDebugClient::Enabled;
}

static QJsonObject readJsonObject(const QString &fileName, QString *errorMessage)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        *errorMessage = file.errorString();
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *errorMessage = QStringLiteral("%1: %2").arg(fileName, parseError.errorString());
        return {};
    }

    return document.object();
}

static bool evidenceContains(const QJsonObject &issue, const QString &needle)
{
    const QJsonArray evidence = issue.value(QStringLiteral("evidence")).toArray();
    for (const QJsonValue &value : evidence) {
        if (value.toString().contains(needle))
            return true;
    }
    return false;
}

std::optional<QJsonObject> QmlAgentIntegrationTest::invoke(
        QmlAgentDebugClient *client, const QString &method, const QJsonObject &params, int id,
        QString *errorMessage, int timeoutMs)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);

    std::optional<QJsonObject> response;
    const QMetaObject::Connection timeoutConnection =
            QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        *errorMessage = QStringLiteral("Timed out waiting for %1 response").arg(method);
        loop.quit();
    });
    const QMetaObject::Connection receivedConnection =
            QObject::connect(client, &QmlAgentDebugClient::received, &loop,
                             [&](const QByteArray &message) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(message, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            return;

        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("id")).toInt(-1) != id)
            return;

        response = object;
        loop.quit();
    });

    const QJsonObject request{
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("id"), id },
        { QStringLiteral("method"), method },
        { QStringLiteral("params"), params },
    };

    timeout.start(timeoutMs);
    client->sendMessage(QJsonDocument(request).toJson(QJsonDocument::Compact));
    if (!response.has_value())
        loop.exec();

    QObject::disconnect(receivedConnection);
    QObject::disconnect(timeoutConnection);

    return response;
}

void QmlAgentIntegrationTest::clickNodeDeliversSyntheticInput()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto infoResponse = invoke(&client, QStringLiteral("Session.getInfo"), {}, 1,
                                     &errorMessage);
    QVERIFY2(infoResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(infoResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("service")).toString(),
             QStringLiteral("QmlAgent"));
    QVERIFY(infoResponse->value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("features")).toArray()
                    .contains(QStringLiteral("UI.waitFor")));
    QVERIFY(infoResponse->value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("features")).toArray()
                    .contains(QStringLiteral("Input.longPressNode")));
    QVERIFY(infoResponse->value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("capabilities")).toObject()
                    .value(QStringLiteral("payloadLimits")).toObject()
                    .value(QStringLiteral("maxOutboundMessageBytes")).toInt() > 0);

    const auto textInputResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("includeSource"), false },
    }, 2, &errorMessage);
    QVERIFY2(textInputResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray textInputMatches = textInputResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(textInputMatches.size(), 1);
    const int textInputNodeId = textInputMatches.at(0).toObject()
            .value(QStringLiteral("nodeId")).toInt(-1);
    QVERIFY(textInputNodeId > 0);

    const auto typeTextResponse = invoke(&client, QStringLiteral("Input.typeText"), {
        { QStringLiteral("nodeId"), textInputNodeId },
        { QStringLiteral("text"), QStringLiteral("ab") },
    }, 3, &errorMessage);
    QVERIFY2(typeTextResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject typeTextResult = typeTextResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(typeTextResult.value(QStringLiteral("delivered")).toBool(), true);
    QVERIFY2(typeTextResult.value(QStringLiteral("postDispatch")).toObject()
                     .value(QStringLiteral("targetLive")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(typeTextResult)
                                                  .toJson(QJsonDocument::Compact))));

    const auto keyResponse = invoke(&client, QStringLiteral("Input.dispatchKeyEvent"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("keyCode"), 67 },
        { QStringLiteral("text"), QStringLiteral("c") },
    }, 4, &errorMessage);
    QVERIFY2(keyResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(keyResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    QCOMPARE(keyResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("focus")).toObject()
                     .value(QStringLiteral("focused")).toBool(),
             true);

    const auto focusResponse = invoke(&client, QStringLiteral("Input.focusNode"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
    }, 5, &errorMessage);
    QVERIFY2(focusResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject focusResult = focusResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(focusResult.value(QStringLiteral("focused")).toBool(), true);
    QVERIFY2(focusResult.value(QStringLiteral("postDispatch")).toObject()
                     .value(QStringLiteral("targetLive")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(focusResult)
                                                  .toJson(QJsonDocument::Compact))));

    const auto keyedQueryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("text") } },
    }, 6, &errorMessage);
    QVERIFY2(keyedQueryResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray keyedMatches = keyedQueryResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(keyedMatches.size(), 1);
    QCOMPARE(keyedMatches.at(0).toObject().value(QStringLiteral("properties")).toObject()
                     .value(QStringLiteral("text")).toString(),
             QStringLiteral("abc"));

    const auto replaceTextResponse = invoke(&client, QStringLiteral("Input.typeText"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("text"), QStringLiteral("xy") },
        { QStringLiteral("replaceExisting"), true },
    }, 70, &errorMessage);
    QVERIFY2(replaceTextResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject replaceTextResult = replaceTextResponse->value(QStringLiteral("result"))
            .toObject();
    QCOMPARE(replaceTextResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(replaceTextResult.value(QStringLiteral("replaceExisting")).toObject()
                     .value(QStringLiteral("selectionApplied")).toBool(),
             true);

    const auto replaceWaitResponse = invoke(&client, QStringLiteral("UI.waitFor"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("property"), QStringLiteral("text") },
            { QStringLiteral("op"), QStringLiteral("contains") },
            { QStringLiteral("value"), QStringLiteral("x") },
        } },
        { QStringLiteral("timeoutMs"), 1000 },
    }, 71, &errorMessage);
    QVERIFY2(replaceWaitResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject replaceWaitResult = replaceWaitResponse->value(QStringLiteral("result"))
            .toObject();
    QCOMPARE(replaceWaitResult.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(replaceWaitResult.value(QStringLiteral("actual")).toString(), QStringLiteral("xy"));

    const auto clearTextResponse = invoke(&client, QStringLiteral("Input.typeText"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("text"), QString() },
        { QStringLiteral("replaceExisting"), true },
    }, 72, &errorMessage);
    QVERIFY2(clearTextResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(clearTextResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);

    const auto readOnlyTypeResponse = invoke(&client, QStringLiteral("Input.typeText"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.readOnlyTextInput\"") },
        { QStringLiteral("text"), QStringLiteral("x") },
    }, 7, &errorMessage);
    QVERIFY2(readOnlyTypeResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject readOnlyTypeResult = readOnlyTypeResponse->value(QStringLiteral("result"))
            .toObject();
    QCOMPARE(readOnlyTypeResult.value(QStringLiteral("delivered")).toBool(), false);
    QCOMPARE(readOnlyTypeResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("not_editable"));
    QCOMPARE(readOnlyTypeResult.value(QStringLiteral("diagnostics")).toArray()
                     .at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_editable"));

    const auto focusFailedTypeResponse = invoke(&client, QStringLiteral("Input.typeText"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.noClickFocusTextInput\"") },
        { QStringLiteral("text"), QStringLiteral("x") },
    }, 8, &errorMessage);
    QVERIFY2(focusFailedTypeResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject focusFailedTypeResult = focusFailedTypeResponse->value(QStringLiteral("result"))
            .toObject();
    QCOMPARE(focusFailedTypeResult.value(QStringLiteral("delivered")).toBool(), false);
    QCOMPARE(focusFailedTypeResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("focus_failed"));
    const QJsonObject refusedFocus = focusFailedTypeResult.value(QStringLiteral("focus")).toObject();
    QCOMPARE(refusedFocus.value(QStringLiteral("focused")).toBool(true), false);
    QVERIFY2(!refusedFocus.value(QStringLiteral("reason")).toString().isEmpty(),
             qPrintable(QString::fromUtf8(QJsonDocument(focusFailedTypeResult)
                                                  .toJson(QJsonDocument::Compact))));
    const QJsonArray focusFailureHints =
            focusFailedTypeResult.value(QStringLiteral("nextHints")).toArray();
    QVERIFY2(!focusFailureHints.isEmpty(),
             qPrintable(QString::fromUtf8(QJsonDocument(focusFailedTypeResult)
                                                  .toJson(QJsonDocument::Compact))));
    QCOMPARE(focusFailureHints.at(0).toObject().value(QStringLiteral("method")).toString(),
             QStringLiteral("Input.focusNode"));

    const auto flickableBeforeResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeFlickable\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("contentY") } },
    }, 8, &errorMessage);
    QVERIFY2(flickableBeforeResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject flickableBefore = flickableBeforeResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject();

    const auto wheelResponse = invoke(&client, QStringLiteral("Input.wheel"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeFlickable\"") },
        { QStringLiteral("deltaY"), -120 },
    }, 9, &errorMessage);
    QVERIFY2(wheelResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject wheelResult = wheelResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(wheelResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(wheelResult.value(QStringLiteral("mode")).toString(), QStringLiteral("synthetic-qt-event"));
    QVERIFY2(wheelResult.value(QStringLiteral("postDispatch")).toObject()
                     .value(QStringLiteral("targetLive")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(wheelResult)
                                                  .toJson(QJsonDocument::Compact))));

    const auto flickableAfterResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeFlickable\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("contentY") } },
    }, 10, &errorMessage);
    QVERIFY2(flickableAfterResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject flickableAfter = flickableAfterResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject();
    QVERIFY2(flickableAfter.value(QStringLiteral("contentY")).toDouble()
                     > flickableBefore.value(QStringLiteral("contentY")).toDouble(),
             qPrintable(QStringLiteral("Expected wheel to increase contentY: before=%1 after=%2")
                                .arg(flickableBefore.value(QStringLiteral("contentY")).toDouble())
                                .arg(flickableAfter.value(QStringLiteral("contentY")).toDouble())));

    const auto dragBeforeResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeDragTarget\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("x") } },
    }, 11, &errorMessage);
    QVERIFY2(dragBeforeResponse.has_value(), qPrintable(errorMessage));
    const double dragBeforeX = dragBeforeResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("x")).toDouble();

    const auto mousePressResponse = invoke(&client, QStringLiteral("Input.dispatchMouseEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeDragArea\"") },
        { QStringLiteral("type"), QStringLiteral("mousePress") },
    }, 12, &errorMessage);
    QVERIFY2(mousePressResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject mousePressResult = mousePressResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(mousePressResult.value(QStringLiteral("delivered")).toBool(), true);
    QVERIFY2(mousePressResult.value(QStringLiteral("postDispatch")).toObject()
                     .value(QStringLiteral("targetLive")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(mousePressResult)
                                                  .toJson(QJsonDocument::Compact))));

    const auto mouseMoveResponse = invoke(&client, QStringLiteral("Input.dispatchMouseEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeDragArea\"") },
        { QStringLiteral("type"), QStringLiteral("mouseMove") },
        { QStringLiteral("point"), QJsonArray{ 92, 12 } },
        { QStringLiteral("buttons"), QJsonArray{ QStringLiteral("left") } },
    }, 13, &errorMessage);
    QVERIFY2(mouseMoveResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(mouseMoveResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);

    const auto secondMouseMoveResponse = invoke(&client, QStringLiteral("Input.dispatchMouseEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeDragArea\"") },
        { QStringLiteral("type"), QStringLiteral("mouseMove") },
        { QStringLiteral("point"), QJsonArray{ 120, 12 } },
        { QStringLiteral("buttons"), QJsonArray{ QStringLiteral("left") } },
    }, 14, &errorMessage);
    QVERIFY2(secondMouseMoveResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(secondMouseMoveResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);

    const auto mouseReleaseResponse = invoke(&client, QStringLiteral("Input.dispatchMouseEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeDragArea\"") },
        { QStringLiteral("type"), QStringLiteral("mouseRelease") },
    }, 15, &errorMessage);
    QVERIFY2(mouseReleaseResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(mouseReleaseResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);

    const auto dragAfterResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeDragTarget\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("x") } },
    }, 16, &errorMessage);
    QVERIFY2(dragAfterResponse.has_value(), qPrintable(errorMessage));
    const double dragAfterX = dragAfterResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("x")).toDouble();
    QVERIFY2(dragAfterX > dragBeforeX,
             qPrintable(QStringLiteral("Expected drag target x to increase: before=%1 after=%2")
                                .arg(dragBeforeX).arg(dragAfterX)));

    const auto dragCommandBeforeResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeDragCommandTarget\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("x") } },
    }, 17, &errorMessage);
    QVERIFY2(dragCommandBeforeResponse.has_value(), qPrintable(errorMessage));
    const double dragCommandBeforeX = dragCommandBeforeResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("x")).toDouble();

    const auto dragCommandResponse = invoke(&client, QStringLiteral("Input.dragNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeDragCommandArea\"") },
        { QStringLiteral("to"), QJsonArray{ 90, 10 } },
    }, 18, &errorMessage);
    QVERIFY2(dragCommandResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject dragCommandResult = dragCommandResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(dragCommandResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(dragCommandResult.value(QStringLiteral("eventsSent")).toInt(), 4);
    QVERIFY2(dragCommandResult.value(QStringLiteral("postDispatch")).toObject()
                     .value(QStringLiteral("targetLive")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(dragCommandResult)
                                                  .toJson(QJsonDocument::Compact))));

    const auto dragCommandAfterResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeDragCommandTarget\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("x") } },
    }, 19, &errorMessage);
    QVERIFY2(dragCommandAfterResponse.has_value(), qPrintable(errorMessage));
    const double dragCommandAfterX = dragCommandAfterResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("x")).toDouble();
    QVERIFY2(dragCommandAfterX > dragCommandBeforeX,
             qPrintable(QStringLiteral("Expected drag command target x to increase: before=%1 after=%2")
                                .arg(dragCommandBeforeX).arg(dragCommandAfterX)));

    // F-024: a drag that travels past its small origin handle must not be
    // refused. smoke.grabber is a 24px DragHandler-driven handle; dragging it
    // ~70px leaves the handle, which the old path-wide actionability preflight
    // rejected as not_draggable. The press point is actionable, so the drag
    // must deliver and the DragHandler must activate and move it.
    const auto grabberDragResponse = invoke(&client, QStringLiteral("Input.dragNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeGrabber\"") },
        { QStringLiteral("delta"), QJsonArray{ 70, 0 } },
        { QStringLiteral("steps"), 10 },
    }, 90, &errorMessage);
    QVERIFY2(grabberDragResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(grabberDragResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(false),
             true);
    const auto grabberStateResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"root\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("grabberDragActivated") } },
    }, 91, &errorMessage);
    QVERIFY2(grabberStateResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(grabberStateResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("matches")).toArray().at(0).toObject()
                     .value(QStringLiteral("properties")).toObject()
                     .value(QStringLiteral("grabberDragActivated")).toBool(false),
             true);
    const auto grabberAfterResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeGrabber\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("x") } },
    }, 92, &errorMessage);
    QVERIFY2(grabberAfterResponse.has_value(), qPrintable(errorMessage));
    QVERIFY2(grabberAfterResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("matches")).toArray().at(0).toObject()
                     .value(QStringLiteral("properties")).toObject()
                     .value(QStringLiteral("x")).toDouble() > 0.0,
             "Expected the grabber to move along its DragHandler axis.");

    // F-025: clicking smoke.movingButton jumps it out from under the click
    // point. Post-dispatch must flag targetMovedSinceDispatch and recheck the
    // target where it now is (actionable), not report the vacated point as
    // blocked_by_item.
    const auto movingClickResponse = invoke(&client, QStringLiteral("Input.clickNode"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.movingButton\"") },
    }, 93, &errorMessage);
    QVERIFY2(movingClickResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject movingClickResult =
            movingClickResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(movingClickResult.value(QStringLiteral("delivered")).toBool(false), true);
    const QJsonObject movingPostDispatch =
            movingClickResult.value(QStringLiteral("postDispatch")).toObject();
    QCOMPARE(movingPostDispatch.value(QStringLiteral("targetMovedSinceDispatch")).toBool(false),
             true);
    QVERIFY2(movingPostDispatch.value(QStringLiteral("actionable")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(movingPostDispatch)
                                                  .toJson(QJsonDocument::Compact))));

    const auto touchBeginResponse = invoke(&client, QStringLiteral("Input.dispatchTouchEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeTouchArea\"") },
        { QStringLiteral("type"), QStringLiteral("touchBegin") },
        { QStringLiteral("points"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), 1 },
                { QStringLiteral("state"), QStringLiteral("pressed") },
                { QStringLiteral("point"), QJsonArray{ 15, 20 } },
            },
        } },
    }, 20, &errorMessage);
    QVERIFY2(touchBeginResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(touchBeginResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);

    const auto touchUpdateResponse = invoke(&client, QStringLiteral("Input.dispatchTouchEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeTouchArea\"") },
        { QStringLiteral("type"), QStringLiteral("touchUpdate") },
        { QStringLiteral("points"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), 1 },
                { QStringLiteral("state"), QStringLiteral("stationary") },
                { QStringLiteral("point"), QJsonArray{ 15, 20 } },
            },
            QJsonObject{
                { QStringLiteral("id"), 2 },
                { QStringLiteral("state"), QStringLiteral("pressed") },
                { QStringLiteral("point"), QJsonArray{ 55, 20 } },
            },
        } },
    }, 21, &errorMessage);
    QVERIFY2(touchUpdateResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(touchUpdateResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);

    const auto touchSecondUpdateResponse = invoke(&client, QStringLiteral("Input.dispatchTouchEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeTouchArea\"") },
        { QStringLiteral("type"), QStringLiteral("touchUpdate") },
        { QStringLiteral("points"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), 1 },
                { QStringLiteral("state"), QStringLiteral("stationary") },
                { QStringLiteral("point"), QJsonArray{ 15, 20 } },
            },
            QJsonObject{
                { QStringLiteral("id"), 2 },
                { QStringLiteral("state"), QStringLiteral("updated") },
                { QStringLiteral("point"), QJsonArray{ 65, 20 } },
            },
        } },
    }, 22, &errorMessage);
    QVERIFY2(touchSecondUpdateResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(touchSecondUpdateResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);

    const auto touchEndResponse = invoke(&client, QStringLiteral("Input.dispatchTouchEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeTouchArea\"") },
        { QStringLiteral("type"), QStringLiteral("touchEnd") },
        { QStringLiteral("points"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), 1 },
                { QStringLiteral("state"), QStringLiteral("released") },
                { QStringLiteral("point"), QJsonArray{ 15, 20 } },
            },
            QJsonObject{
                { QStringLiteral("id"), 2 },
                { QStringLiteral("state"), QStringLiteral("released") },
                { QStringLiteral("point"), QJsonArray{ 65, 20 } },
            },
        } },
    }, 23, &errorMessage);
    QVERIFY2(touchEndResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject touchEndResult = touchEndResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(touchEndResult.value(QStringLiteral("delivered")).toBool(), true);
    QVERIFY2(touchEndResult.value(QStringLiteral("postDispatch")).toObject()
                     .value(QStringLiteral("targetLive")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(touchEndResult)
                                                  .toJson(QJsonDocument::Compact))));

    const auto touchProbeResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeTouchProbe\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{
            QStringLiteral("pressedCount"),
            QStringLiteral("span"),
            QStringLiteral("released"),
        } },
    }, 24, &errorMessage);
    QVERIFY2(touchProbeResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject touchProbeProperties = touchProbeResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject();
    QCOMPARE(touchProbeProperties.value(QStringLiteral("pressedCount")).toInt(), 2);
    QVERIFY2(touchProbeProperties.value(QStringLiteral("span")).toDouble() >= 40.0,
             qPrintable(QStringLiteral("Expected multipoint touch span to be recorded: %1")
                                .arg(touchProbeProperties.value(QStringLiteral("span")).toDouble())));
    QCOMPARE(touchProbeProperties.value(QStringLiteral("released")).toBool(), true);

    const auto delegateQueryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"indexedDelegateTap\"") },
        { QStringLiteral("includeSource"), true },
    }, 25, &errorMessage);
    QVERIFY2(delegateQueryResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray delegateMatches = delegateQueryResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray();
    QCOMPARE(delegateMatches.size(), 2);
    QCOMPARE(delegateQueryResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("diagnostics")).toArray()
                     .at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("selector.ambiguous"));
    const QJsonArray indexedSelectors = delegateQueryResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("diagnostics")).toArray()
            .at(0).toObject().value(QStringLiteral("indexedSelectors")).toArray();
    QCOMPARE(indexedSelectors.size(), 2);
    QCOMPARE(indexedSelectors.at(1).toObject().value(QStringLiteral("selector")).toString(),
             QStringLiteral("id=\"indexedDelegateTap\" index=1"));
    const QJsonArray stableSelectorHints = delegateQueryResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("diagnostics")).toArray()
            .at(0).toObject().value(QStringLiteral("stableSelectorHints")).toArray();
    QVERIFY2(!stableSelectorHints.isEmpty(), qPrintable(errorMessage));
    QVERIFY2(stableSelectorHints.at(0).toObject().value(QStringLiteral("reason")).toString()
                     .contains(QStringLiteral("Authored QML ids")),
             qPrintable(QString::fromUtf8(QJsonDocument(stableSelectorHints)
                                                  .toJson(QJsonDocument::Compact))));

    // "Delta" appears on the outer delegate Text and its inner mirror Text on
    // one ancestor chain; text matches collapse onto the outermost label owner.
    const auto ambiguousTextDelegateResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("text=\"Delta\"") },
        { QStringLiteral("includeSource"), true },
    }, 26, &errorMessage);
    QVERIFY2(ambiguousTextDelegateResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject ambiguousTextResult = ambiguousTextDelegateResponse
            ->value(QStringLiteral("result")).toObject();
    const QJsonArray collapsedTextMatches = ambiguousTextResult
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(collapsedTextMatches.size(), 1);
    QCOMPARE(collapsedTextMatches.at(0).toObject().value(QStringLiteral("qmlId")).toString(),
             QStringLiteral("indexedTextContainer"));
    QVERIFY2(ambiguousTextResult.value(QStringLiteral("diagnostics")).toArray().isEmpty(),
             qPrintable(QString::fromUtf8(QJsonDocument(
                     ambiguousTextResult.value(QStringLiteral("diagnostics")).toArray())
                                                  .toJson(QJsonDocument::Compact))));

    const auto textDelegateIndexResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"indexedTextContainer\" index=1") },
        { QStringLiteral("includeSource"), false },
    }, 27, &errorMessage);
    QVERIFY2(textDelegateIndexResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray textDelegateMatches = textDelegateIndexResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray();
    QCOMPARE(textDelegateMatches.size(), 1);
    QCOMPARE(textDelegateMatches.at(0).toObject().value(QStringLiteral("qmlId")).toString(),
             QStringLiteral("indexedTextContainer"));

    const auto delegateIndexResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"indexedDelegateTap\" index=1") },
        { QStringLiteral("includeSource"), false },
    }, 28, &errorMessage);
    QVERIFY2(delegateIndexResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray indexedDelegateMatches = delegateIndexResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray();
    QCOMPARE(indexedDelegateMatches.size(), 1);
    const QJsonObject delegateNode = indexedDelegateMatches.at(0).toObject();
    QCOMPARE(delegateNode.value(QStringLiteral("qmlId")).toString(),
             QStringLiteral("indexedDelegateTap"));
    QCOMPARE(delegateNode.value(QStringLiteral("delegate")).toObject()
                     .value(QStringLiteral("index")).toInt(-1),
             1);

    const auto delegateClickResponse = invoke(&client, QStringLiteral("Input.clickNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"indexedDelegateTap\" index=1") },
    }, 29, &errorMessage);
    QVERIFY2(delegateClickResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(delegateClickResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);

    const auto delegateWaitResponse = invoke(&client, QStringLiteral("UI.waitFor"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"indexedDelegateProbe\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("property"), QStringLiteral("selectedIndex") },
            { QStringLiteral("op"), QStringLiteral("=") },
            { QStringLiteral("value"), 1 },
        } },
        { QStringLiteral("timeoutMs"), 250 },
    }, 30, &errorMessage);
    QVERIFY2(delegateWaitResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject delegateWaitResult =
            delegateWaitResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(delegateWaitResult.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(delegateWaitResult.value(QStringLiteral("timedOut")).toBool(), false);
    QCOMPARE(delegateWaitResult.value(QStringLiteral("actual")).toInt(-1), 1);

    const auto delegateWaitTimeoutResponse = invoke(&client, QStringLiteral("UI.waitFor"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"indexedDelegateProbe\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("property"), QStringLiteral("selectedIndex") },
            { QStringLiteral("op"), QStringLiteral("=") },
            { QStringLiteral("value"), 99 },
        } },
        { QStringLiteral("timeoutMs"), 25 },
    }, 31, &errorMessage);
    QVERIFY2(delegateWaitTimeoutResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject delegateWaitTimeoutResult =
            delegateWaitTimeoutResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(delegateWaitTimeoutResult.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(delegateWaitTimeoutResult.value(QStringLiteral("timedOut")).toBool(), true);
    QCOMPARE(delegateWaitTimeoutResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("property_not_satisfied"));
    QCOMPARE(delegateWaitTimeoutResult.value(QStringLiteral("actual")).toInt(-1), 1);
    QVERIFY2(!delegateWaitTimeoutResult.value(QStringLiteral("nextHints")).toArray().isEmpty(),
             qPrintable(QString::fromUtf8(QJsonDocument(delegateWaitTimeoutResult).toJson())));

    const auto delegateProbeResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"indexedDelegateProbe\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("selectedIndex") } },
    }, 32, &errorMessage);
    QVERIFY2(delegateProbeResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject delegateProbeProperties = delegateProbeResponse
            ->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray().at(0).toObject()
            .value(QStringLiteral("properties")).toObject();
    QCOMPARE(delegateProbeProperties.value(QStringLiteral("selectedIndex")).toInt(-1), 1);

    const auto runtimeDisabledResponse = invoke(&client, QStringLiteral("Runtime.setProperty"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"runtimeProbe\"") },
        { QStringLiteral("property"), QStringLiteral("label") },
        { QStringLiteral("value"), QStringLiteral("blocked") },
    }, 33, &errorMessage);
    QVERIFY2(runtimeDisabledResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject runtimeDisabledResult =
            runtimeDisabledResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(runtimeDisabledResult.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(runtimeDisabledResult.value(QStringLiteral("diagnostics")).toArray().at(0).toObject()
                     .value(QStringLiteral("id")).toString(),
             QStringLiteral("runtime.capability_disabled"));
    QCOMPARE(runtimeDisabledResult.value(QStringLiteral("mode")).toString(),
             QStringLiteral("whitebox"));
    QCOMPARE(runtimeDisabledResult.value(QStringLiteral("verificationRole")).toString(),
             QStringLiteral("setup-only"));

    const auto runtimeInvokeDisabledResponse = invoke(&client, QStringLiteral("Runtime.invokeMethod"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"runtimeProbe\"") },
        { QStringLiteral("method"), QStringLiteral("setRuntimeCounter") },
        { QStringLiteral("args"), QJsonArray{ 1 } },
    }, 34, &errorMessage);
    QVERIFY2(runtimeInvokeDisabledResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject runtimeInvokeDisabledResult =
            runtimeInvokeDisabledResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(runtimeInvokeDisabledResult.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(runtimeInvokeDisabledResult.value(QStringLiteral("mode")).toString(),
             QStringLiteral("whitebox"));
    QCOMPARE(runtimeInvokeDisabledResult.value(QStringLiteral("verificationRole")).toString(),
             QStringLiteral("setup-only"));
    QCOMPARE(runtimeInvokeDisabledResult.value(QStringLiteral("diagnostics")).toArray().at(0).toObject()
                     .value(QStringLiteral("id")).toString(),
             QStringLiteral("runtime.capability_disabled"));

    const auto runtimeEnableResponse = invoke(&client, QStringLiteral("Session.configure"), {
        { QStringLiteral("runtimeMutation"), true },
    }, 35, &errorMessage);
    QVERIFY2(runtimeEnableResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(runtimeEnableResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("capabilities")).toObject()
                     .value(QStringLiteral("runtimeMutation")).toObject()
                     .value(QStringLiteral("enabled")).toBool(),
             true);

    const auto runtimeSetResponse = invoke(&client, QStringLiteral("Runtime.setProperty"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"runtimeProbe\"") },
        { QStringLiteral("property"), QStringLiteral("label") },
        { QStringLiteral("value"), QStringLiteral("patched") },
    }, 36, &errorMessage);
    QVERIFY2(runtimeSetResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject runtimeSetResult = runtimeSetResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(runtimeSetResult.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(runtimeSetResult.value(QStringLiteral("mode")).toString(), QStringLiteral("whitebox"));
    QCOMPARE(runtimeSetResult.value(QStringLiteral("verificationRole")).toString(),
             QStringLiteral("setup-only"));
    QCOMPARE(runtimeSetResult.value(QStringLiteral("before")).toString(), QStringLiteral("idle"));
    QCOMPARE(runtimeSetResult.value(QStringLiteral("after")).toString(), QStringLiteral("patched"));
    QVERIFY(runtimeSetResult.value(QStringLiteral("settle")).toObject()
                    .contains(QStringLiteral("framesAfterAction")));
    QVERIFY(runtimeSetResult.value(QStringLiteral("sourceLocation")).toObject()
                    .value(QStringLiteral("confidence")).toDouble() > 0.0);

    const auto missingRuntimePropertyResponse = invoke(&client, QStringLiteral("Runtime.setProperty"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"runtimeProbe\"") },
        { QStringLiteral("property"), QStringLiteral("doesNotExist") },
        { QStringLiteral("value"), true },
    }, 37, &errorMessage);
    QVERIFY2(missingRuntimePropertyResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject missingRuntimePropertyResult = missingRuntimePropertyResponse
            ->value(QStringLiteral("result")).toObject();
    QCOMPARE(missingRuntimePropertyResult.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(missingRuntimePropertyResult.value(QStringLiteral("diagnostics")).toArray()
                     .at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("runtime.property_not_found"));

    const auto runtimeInvokeResponse = invoke(&client, QStringLiteral("Runtime.invokeMethod"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"runtimeProbe\"") },
        { QStringLiteral("method"), QStringLiteral("setRuntimeCounter") },
        { QStringLiteral("args"), QJsonArray{ 7 } },
    }, 38, &errorMessage);
    QVERIFY2(runtimeInvokeResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject runtimeInvokeResult = runtimeInvokeResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(runtimeInvokeResult.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(runtimeInvokeResult.value(QStringLiteral("mode")).toString(), QStringLiteral("whitebox"));
    QCOMPARE(runtimeInvokeResult.value(QStringLiteral("verificationRole")).toString(),
             QStringLiteral("setup-only"));
    QCOMPARE(runtimeInvokeResult.value(QStringLiteral("returnValue")).toInt(-1), 7);

    const auto runtimeProbeResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"runtimeProbe\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{
            QStringLiteral("counter"),
            QStringLiteral("label"),
        } },
    }, 39, &errorMessage);
    QVERIFY2(runtimeProbeResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject runtimeProbeProperties = runtimeProbeResponse
            ->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray().at(0).toObject()
            .value(QStringLiteral("properties")).toObject();
    QCOMPARE(runtimeProbeProperties.value(QStringLiteral("counter")).toInt(-1), 7);
    QCOMPARE(runtimeProbeProperties.value(QStringLiteral("label")).toString(),
             QStringLiteral("method-7"));

    const auto queryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.clickArea\"") },
        { QStringLiteral("includeSource"), false },
    }, 39, &errorMessage);
    QVERIFY2(queryResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray matches = queryResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    const int nodeId = matches.at(0).toObject().value(QStringLiteral("nodeId")).toInt(-1);
    QVERIFY(nodeId > 0);
    // Input-affordance discovery (F-023): a MouseArea reads as accepting
    // input; a plain Text does not.
    const QJsonObject clickAreaAffordance = matches.at(0).toObject()
            .value(QStringLiteral("acceptsInput")).toObject();
    QCOMPARE(clickAreaAffordance.value(QStringLiteral("ok")).toBool(false), true);
    QVERIFY2(clickAreaAffordance.value(QStringLiteral("via")).toArray()
                     .contains(QStringLiteral("knownInputType")),
             qPrintable(QString::fromUtf8(QJsonDocument(clickAreaAffordance)
                                                  .toJson(QJsonDocument::Compact))));
    const auto textAffordanceResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("text=\"Press me\"") },
        { QStringLiteral("includeSource"), false },
    }, 74, &errorMessage);
    QVERIFY2(textAffordanceResponse.has_value(), qPrintable(errorMessage));
    // Sparse emission: non-accepting nodes omit the field instead of
    // flooding every node with {ok:false}.
    QVERIFY(!textAffordanceResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("matches")).toArray().at(0).toObject()
                     .contains(QStringLiteral("acceptsInput")));

    const auto clickResponse = invoke(&client, QStringLiteral("Input.clickNode"), {
        { QStringLiteral("nodeId"), nodeId },
    }, 40, &errorMessage);
    QVERIFY2(clickResponse.has_value(), qPrintable(errorMessage));

    const QJsonObject clickResult = clickResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(clickResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(clickResult.value(QStringLiteral("mode")).toString(), QStringLiteral("synthetic-qt-event"));
    const QJsonObject postDispatch = clickResult.value(QStringLiteral("postDispatch")).toObject();
    QCOMPARE(postDispatch.value(QStringLiteral("targetLive")).toBool(false), true);
    QCOMPARE(postDispatch.value(QStringLiteral("actionabilityRechecked")).toBool(false), true);
    QCOMPARE(postDispatch.value(QStringLiteral("verificationRole")).toString(),
             QStringLiteral("post-dispatch evidence only"));

    const auto clickedQueryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.clicked\"") },
        { QStringLiteral("includeSource"), false },
    }, 41, &errorMessage);
    QVERIFY2(clickedQueryResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray clickedMatches = clickedQueryResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(clickedMatches.size(), 1);
}

void QmlAgentIntegrationTest::inputTargetsMultipleWindows()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto primaryQuery = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.clickArea\"") },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("windowId"),
            QStringLiteral("objectName"),
        } },
    }, 1, &errorMessage);
    QVERIFY2(primaryQuery.has_value(), qPrintable(errorMessage));
    const QJsonObject primaryNode = primaryQuery->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray().at(0).toObject();

    const auto secondaryQuery = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.secondaryWindowClickArea\"") },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("windowId"),
            QStringLiteral("objectName"),
        } },
    }, 2, &errorMessage);
    QVERIFY2(secondaryQuery.has_value(), qPrintable(errorMessage));
    const QJsonObject secondaryNode = secondaryQuery->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray().at(0).toObject();

    QVERIFY(primaryNode.value(QStringLiteral("windowId")).toInt(-1) > 0);
    QVERIFY(secondaryNode.value(QStringLiteral("windowId")).toInt(-1) > 0);
    QVERIFY2(primaryNode.value(QStringLiteral("windowId")).toInt()
                     != secondaryNode.value(QStringLiteral("windowId")).toInt(),
             qPrintable(QString::fromUtf8(QJsonDocument(QJsonObject{
                 { QStringLiteral("primary"), primaryNode },
                 { QStringLiteral("secondary"), secondaryNode },
             }).toJson(QJsonDocument::Compact))));

    const auto primaryClick = invoke(&client, QStringLiteral("Input.clickNode"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.clickArea\"") },
    }, 3, &errorMessage);
    QVERIFY2(primaryClick.has_value(), qPrintable(errorMessage));
    const QJsonObject primaryClickResult = primaryClick->value(QStringLiteral("result")).toObject();
    QCOMPARE(primaryClickResult.value(QStringLiteral("delivered")).toBool(false), true);
    QCOMPARE(primaryClickResult.value(QStringLiteral("deliveryWindow")).toObject()
                     .value(QStringLiteral("windowId")).toInt(-1),
             primaryNode.value(QStringLiteral("windowId")).toInt(-2));

    const auto secondaryClick = invoke(&client, QStringLiteral("Input.clickNode"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.secondaryWindowClickArea\"") },
    }, 4, &errorMessage);
    QVERIFY2(secondaryClick.has_value(), qPrintable(errorMessage));
    const QJsonObject secondaryClickResult = secondaryClick->value(QStringLiteral("result")).toObject();
    QCOMPARE(secondaryClickResult.value(QStringLiteral("delivered")).toBool(false), true);
    QCOMPARE(secondaryClickResult.value(QStringLiteral("deliveryWindow")).toObject()
                     .value(QStringLiteral("windowId")).toInt(-1),
             secondaryNode.value(QStringLiteral("windowId")).toInt(-2));

    const auto primaryVerify = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.clicked\"") },
        { QStringLiteral("includeSource"), false },
    }, 5, &errorMessage);
    QVERIFY2(primaryVerify.has_value(), qPrintable(errorMessage));
    QCOMPARE(primaryVerify->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("matches")).toArray().size(),
             1);

    const auto secondaryVerify = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.secondaryWindowClicked\"") },
        { QStringLiteral("includeSource"), false },
    }, 6, &errorMessage);
    QVERIFY2(secondaryVerify.has_value(), qPrintable(errorMessage));
    QCOMPARE(secondaryVerify->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("matches")).toArray().size(),
             1);
}

void QmlAgentIntegrationTest::uiSubscribeEmitsCoalescedChangeEvents()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto subscribeResponse = invoke(&client, QStringLiteral("UI.subscribe"), {}, 1,
                                          &errorMessage);
    QVERIFY2(subscribeResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(subscribeResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("enabled")).toBool(),
             true);

    auto invokeAndWaitForTreeChanged = [&](const QString &method, const QJsonObject &params,
                                           int id) {
        std::optional<QJsonObject> uiEvent;
        QEventLoop eventLoop;
        QTimer eventTimeout;
        eventTimeout.setSingleShot(true);
        QObject::connect(&eventTimeout, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
        const QMetaObject::Connection eventConnection = QObject::connect(
                &client, &QmlAgentDebugClient::received, &eventLoop, [&](const QByteArray &message) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(message, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return;

            const QJsonObject object = document.object();
            if (object.value(QStringLiteral("method")).toString() != QLatin1String("UI.treeChanged"))
                return;

            uiEvent = object;
            eventLoop.quit();
        });
        const auto response = invoke(&client, method, params, id, &errorMessage);
        eventTimeout.start(RequestTimeoutMs);
        if (!uiEvent.has_value())
            eventLoop.exec();
        QObject::disconnect(eventConnection);
        return std::pair<std::optional<QJsonObject>, std::optional<QJsonObject>>{ response, uiEvent };
    };

    const auto clickResult = invokeAndWaitForTreeChanged(QStringLiteral("Input.clickNode"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.clickArea\"") },
    }, 2);
    const auto clickResponse = clickResult.first;
    QVERIFY2(clickResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(clickResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);

    const std::optional<QJsonObject> uiEvent = clickResult.second;
    QVERIFY2(uiEvent.has_value(), "Expected UI.treeChanged after subscribed runtime UI change.");
    const QJsonObject params = uiEvent->value(QStringLiteral("params")).toObject();
    QVERIFY2(params.value(QStringLiteral("sequence")).toInt(0) > 0,
             "Expected UI.treeChanged sequence.");
    QVERIFY2(!params.value(QStringLiteral("reason")).toString().isEmpty(),
             "Expected UI.treeChanged reason.");

    const auto addDynamicResult = invokeAndWaitForTreeChanged(QStringLiteral("Input.clickNode"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.addSubscribedDynamic\"") },
    }, 3);
    const auto addDynamicResponse = addDynamicResult.first;
    QVERIFY2(addDynamicResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject addDynamicObject = addDynamicResponse->value(QStringLiteral("result")).toObject();
    QVERIFY2(addDynamicObject.value(QStringLiteral("delivered")).toBool(),
             qPrintable(QString::fromUtf8(QJsonDocument(addDynamicObject).toJson(QJsonDocument::Compact))));
    QVERIFY2(addDynamicResult.second.has_value(),
             "Expected UI.treeChanged after subscribed dynamic child creation.");

    const auto dynamicQueryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.subscribedDynamicClickArea\"") },
    }, 4, &errorMessage);
    QVERIFY2(dynamicQueryResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(dynamicQueryResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("matches")).toArray().size(),
             1);

    const auto dynamicClickResult = invokeAndWaitForTreeChanged(QStringLiteral("Input.clickNode"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.subscribedDynamicClickArea\"") },
    }, 5);
    const auto dynamicClickResponse = dynamicClickResult.first;
    QVERIFY2(dynamicClickResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject dynamicClickObject = dynamicClickResponse->value(QStringLiteral("result")).toObject();
    QVERIFY2(dynamicClickObject.value(QStringLiteral("delivered")).toBool(),
             qPrintable(QString::fromUtf8(QJsonDocument(dynamicClickObject).toJson(QJsonDocument::Compact))));
    const std::optional<QJsonObject> dynamicClickEvent = dynamicClickResult.second;
    QVERIFY2(dynamicClickEvent.has_value(),
             "Expected watcher refresh to observe changes on dynamically added child.");

    const auto unsubscribeResponse = invoke(&client, QStringLiteral("UI.unsubscribe"), {}, 6,
                                            &errorMessage);
    QVERIFY2(unsubscribeResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(unsubscribeResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("enabled")).toBool(),
             false);
}

void QmlAgentIntegrationTest::uiTreeSupportsProjectionBoundsAndCollapse()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto projectedResponse = invoke(&client, QStringLiteral("UI.getTree"), {
        { QStringLiteral("depth"), -1 },
        { QStringLiteral("includeSource"), true },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("type"),
            QStringLiteral("objectName"),
        } },
        { QStringLiteral("maxNodes"), 5 },
    }, 1, &errorMessage);
    QVERIFY2(projectedResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject projected = projectedResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(projected.value(QStringLiteral("truncated")).toBool(), true);
    QCOMPARE(projected.value(QStringLiteral("nodeCount")).toInt(), 5);
    QVERIFY(projected.value(QStringLiteral("omittedNodeCount")).toInt() > 0);
    const QJsonObject projectedRoot = projected.value(QStringLiteral("windows")).toArray()
            .first().toObject().value(QStringLiteral("root")).toObject();
    QVERIFY(projectedRoot.contains(QStringLiteral("nodeId")));
    QVERIFY(projectedRoot.contains(QStringLiteral("children")));
    QVERIFY(!projectedRoot.contains(QStringLiteral("sourceLocation")));
    QVERIFY(!projectedRoot.contains(QStringLiteral("selectors")));

    const auto collapsedResponse = invoke(&client, QStringLiteral("UI.getTree"), {
        { QStringLiteral("depth"), -1 },
        { QStringLiteral("includeSource"), true },
        { QStringLiteral("collapseRepeated"), true },
    }, 2, &errorMessage);
    QVERIFY2(collapsedResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray collapsedWindows = collapsedResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("windows")).toArray();

    std::function<bool(const QJsonObject &)> hasCollapsedRepeatedDelegate =
            [&](const QJsonObject &node) {
        if (node.value(QStringLiteral("collapsed")).toBool()
                && node.value(QStringLiteral("objectName")).toString()
                    == QLatin1String("smoke.repeatedDelegate")
                && node.value(QStringLiteral("count")).toInt() == 3) {
            const QJsonArray nextHints = node.value(QStringLiteral("nextHints")).toArray();
            if (nextHints.isEmpty())
                return false;
            const QJsonObject firstHint = nextHints.at(0).toObject();
            if (firstHint.value(QStringLiteral("method")).toString()
                    != QLatin1String("UI.query")) {
                return false;
            }
            return true;
        }
        const QJsonArray children = node.value(QStringLiteral("children")).toArray();
        for (const QJsonValue &child : children) {
            if (hasCollapsedRepeatedDelegate(child.toObject()))
                return true;
        }
        return false;
    };
    bool foundCollapsedDelegate = false;
    for (const QJsonValue &windowValue : collapsedWindows) {
        if (hasCollapsedRepeatedDelegate(windowValue.toObject().value(QStringLiteral("root")).toObject())) {
            foundCollapsedDelegate = true;
            break;
        }
    }
    QVERIFY2(foundCollapsedDelegate,
             qPrintable(QString::fromUtf8(QJsonDocument(QJsonObject{
                 { QStringLiteral("windows"), collapsedWindows },
             }).toJson(QJsonDocument::Compact))));

    const auto queryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("includeSource"), true },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("objectName"),
        } },
        { QStringLiteral("maxNodes"), 1 },
    }, 3, &errorMessage);
    QVERIFY2(queryResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray matches = queryResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    const QJsonObject match = matches.at(0).toObject();
    QVERIFY(match.contains(QStringLiteral("nodeId")));
    QCOMPARE(match.value(QStringLiteral("objectName")).toString(), QStringLiteral("smoke.textInput"));
    QVERIFY(!match.contains(QStringLiteral("sourceLocation")));
    QVERIFY(!match.contains(QStringLiteral("selectors")));
}

void QmlAgentIntegrationTest::diagnosticsAnalyzeBindingReportsRuntimeProvenance()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto bindingResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeBinding"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"bindingProbe\"") },
        { QStringLiteral("property"), QStringLiteral("x") },
    }, 1, &errorMessage);
    QVERIFY2(bindingResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject bindingResult = bindingResponse->value(QStringLiteral("result")).toObject();
    QVERIFY2(bindingResult.value(QStringLiteral("ok")).toBool(),
             qPrintable(QString::fromUtf8(QJsonDocument(bindingResult).toJson())));
    QCOMPARE(bindingResult.value(QStringLiteral("value")).toInt(-1), 73);

    const QJsonObject bindingProvenance =
            bindingResult.value(QStringLiteral("provenance")).toObject();
    QCOMPARE(bindingProvenance.value(QStringLiteral("kind")).toString(),
             QStringLiteral("binding"));
    QCOMPARE(bindingProvenance.value(QStringLiteral("isBinding")).toBool(), true);
    QCOMPARE(bindingProvenance.value(QStringLiteral("bindingKind")).toString(),
             QStringLiteral("qpropertyBinding"));
    QVERIFY2(bindingProvenance.value(QStringLiteral("sourceLocation")).toObject()
                    .value(QStringLiteral("confidence")).toDouble() > 0.0,
             qPrintable(QString::fromUtf8(QJsonDocument(bindingResult).toJson())));
    QVERIFY2(bindingProvenance.value(QStringLiteral("expression")).toString()
                    .contains(QStringLiteral("root.bindingBase + 3")),
             qPrintable(QString::fromUtf8(QJsonDocument(bindingResult).toJson())));

    QVERIFY2(bindingProvenance.value(QStringLiteral("dependencyLimitations")).toArray()
                    .at(0).toString().contains(QStringLiteral("not yet exposed")),
             qPrintable(QString::fromUtf8(QJsonDocument(bindingResult).toJson())));

    const auto literalResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeBinding"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"literalProbe\"") },
        { QStringLiteral("property"), QStringLiteral("x") },
    }, 2, &errorMessage);
    QVERIFY2(literalResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject literalResult = literalResponse->value(QStringLiteral("result")).toObject();
    QVERIFY2(literalResult.value(QStringLiteral("ok")).toBool(),
             qPrintable(QString::fromUtf8(QJsonDocument(literalResult).toJson())));
    QCOMPARE(literalResult.value(QStringLiteral("value")).toInt(-1), 14);

    const QJsonObject literalProvenance =
            literalResult.value(QStringLiteral("provenance")).toObject();
    QCOMPARE(literalProvenance.value(QStringLiteral("kind")).toString(),
             QStringLiteral("runtimeValue"));
    QCOMPARE(literalProvenance.value(QStringLiteral("isBinding")).toBool(), false);
    QVERIFY2(literalProvenance.value(QStringLiteral("sourceAssignment")).toObject()
                    .value(QStringLiteral("expression")).toString()
                    .contains(QStringLiteral("14")),
             qPrintable(QString::fromUtf8(QJsonDocument(literalResult).toJson())));

    const auto literalYResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeBinding"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"literalProbe\"") },
        { QStringLiteral("property"), QStringLiteral("y") },
    }, 4, &errorMessage);
    QVERIFY2(literalYResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject literalYResult = literalYResponse->value(QStringLiteral("result")).toObject();
    QVERIFY2(literalYResult.value(QStringLiteral("ok")).toBool(),
             qPrintable(QString::fromUtf8(QJsonDocument(literalYResult).toJson())));
    const QJsonObject literalYProvenance =
            literalYResult.value(QStringLiteral("provenance")).toObject();
    QCOMPARE(literalYProvenance.value(QStringLiteral("sourceAssignment")).toObject()
                     .value(QStringLiteral("expression")).toString(),
             QStringLiteral("102"));
    QVERIFY2(!literalYProvenance.value(QStringLiteral("sourceAssignment")).toObject()
                     .value(QStringLiteral("expression")).toString()
                     .contains(QStringLiteral("777")),
             qPrintable(QString::fromUtf8(QJsonDocument(literalYResult).toJson())));

    const auto missingPropertyResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeBinding"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"literalProbe\"") },
        { QStringLiteral("property"), QStringLiteral("doesNotExist") },
    }, 5, &errorMessage);
    QVERIFY2(missingPropertyResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject missingPropertyResult =
            missingPropertyResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(missingPropertyResult.value(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(missingPropertyResult.value(QStringLiteral("diagnostics")).toArray().at(0).toObject()
                     .value(QStringLiteral("id")).toString(),
             QStringLiteral("binding.property_not_found"));
}

void QmlAgentIntegrationTest::uiQuerySerializesRequestedValueTypes()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto rectangleResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.content\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{
            QStringLiteral("color"),
        } },
    }, 1, &errorMessage);
    QVERIFY2(rectangleResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray rectangleMatches = rectangleResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE_EQ(rectangleMatches.size(), 1);

    const QJsonObject rectangleProperties = rectangleMatches.at(0).toObject()
            .value(QStringLiteral("properties")).toObject();
    QCOMPARE(rectangleProperties.value(QStringLiteral("color")).toString(),
             QStringLiteral("#1f6feb"));

    const auto scaledResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.scaledProbe\"") },
        { QStringLiteral("includeSource"), false },
    }, 2, &errorMessage);
    QVERIFY2(scaledResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray scaledMatches = scaledResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE_EQ(scaledMatches.size(), 1);
    const QJsonArray scaledBox = scaledMatches.at(0).toObject().value(QStringLiteral("bbox")).toArray();
    QCOMPARE(scaledBox.at(2).toDouble(), 50.0);
    QCOMPARE(scaledBox.at(3).toDouble(), 20.0);

    const auto valueTypeResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"valueTypeItem\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{
            QStringLiteral("agentColor"),
            QStringLiteral("agentTransparentColor"),
            QStringLiteral("agentPoint"),
            QStringLiteral("agentSize"),
            QStringLiteral("agentRect"),
        } },
    }, 3, &errorMessage);
    QVERIFY2(valueTypeResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray valueTypeMatches = valueTypeResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE_EQ(valueTypeMatches.size(), 1);

    const QJsonObject valueTypeProperties = valueTypeMatches.at(0).toObject()
            .value(QStringLiteral("properties")).toObject();
    QCOMPARE(valueTypeProperties.value(QStringLiteral("agentColor")).toString(),
             QStringLiteral("#336699"));
    QCOMPARE(valueTypeProperties.value(QStringLiteral("agentTransparentColor")).toString(),
             QStringLiteral("#80336699"));

    const QJsonObject point = valueTypeProperties.value(QStringLiteral("agentPoint")).toObject();
    QCOMPARE(point.value(QStringLiteral("x")).toDouble(), 2.0);
    QCOMPARE(point.value(QStringLiteral("y")).toDouble(), 3.0);

    const QJsonObject size = valueTypeProperties.value(QStringLiteral("agentSize")).toObject();
    QCOMPARE(size.value(QStringLiteral("width")).toDouble(), 10.0);
    QCOMPARE(size.value(QStringLiteral("height")).toDouble(), 11.0);

    const QJsonArray rect = valueTypeProperties.value(QStringLiteral("agentRect")).toArray();
    QCOMPARE(rect.size(), 4);
    QCOMPARE(rect.at(0).toDouble(), 1.0);
    QCOMPARE(rect.at(1).toDouble(), 2.0);
    QCOMPARE(rect.at(2).toDouble(), 3.0);
    QCOMPARE(rect.at(3).toDouble(), 4.0);
}

void QmlAgentIntegrationTest::uiQueryDefaultsToVisibleNodes()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto defaultResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"hiddenProbe\"") },
        { QStringLiteral("includeSource"), false },
    }, 1, &errorMessage);
    QVERIFY2(defaultResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(defaultResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("matches")).toArray().size(),
             0);

    const auto invisibleResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"hiddenProbe\"") },
        { QStringLiteral("includeInvisible"), true },
        { QStringLiteral("includeSource"), false },
    }, 2, &errorMessage);
    QVERIFY2(invisibleResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray matches = invisibleResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.at(0).toObject().value(QStringLiteral("qmlId")).toString(),
             QStringLiteral("hiddenProbe"));
}

void QmlAgentIntegrationTest::sourceResolverClassifiesLoadedDelegateAndDynamicNodes()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    auto querySourceMethod = [&](const QString &objectName, int id,
                                 QString *failure) -> std::optional<QString> {
        QString errorMessage;
        const auto response = invoke(&client, QStringLiteral("UI.query"), {
            { QStringLiteral("selector"), QStringLiteral("objectName=\"%1\"").arg(objectName) },
            { QStringLiteral("includeSource"), true },
        }, id, &errorMessage);
        if (!response.has_value()) {
            *failure = errorMessage;
            return std::nullopt;
        }

        const QJsonArray matches = response->value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("matches")).toArray();
        if (matches.size() != 1) {
            *failure = QStringLiteral("Expected one match for %1, got %2")
                    .arg(objectName).arg(matches.size());
            return std::nullopt;
        }
        return matches.at(0).toObject().value(QStringLiteral("sourceLocation")).toObject()
                .value(QStringLiteral("method")).toString();
    };

    QString failure;
    const std::optional<QString> directMethod =
            querySourceMethod(QStringLiteral("smoke.content"), 1, &failure);
    QVERIFY2(directMethod.has_value(), qPrintable(failure));
    QCOMPARE(*directMethod, QStringLiteral("qqmldata-direct"));

    const std::optional<QString> loadedMethod =
            querySourceMethod(QStringLiteral("smoke.loaded"), 2, &failure);
    QVERIFY2(loadedMethod.has_value(), qPrintable(failure));
    QCOMPARE(*loadedMethod, QStringLiteral("qqmldata-loaded"));

    const std::optional<QString> delegateMethod =
            querySourceMethod(QStringLiteral("smoke.delegate"), 3, &failure);
    QVERIFY2(delegateMethod.has_value(), qPrintable(failure));
    QCOMPARE(*delegateMethod, QStringLiteral("qqmldata-delegate"));

    const std::optional<QString> dynamicMethod =
            querySourceMethod(QStringLiteral("smoke.dynamic"), 4, &failure);
    QVERIFY2(dynamicMethod.has_value(), qPrintable(failure));
    QCOMPARE(*dynamicMethod, QStringLiteral("qqmldata-dynamic"));
}

void QmlAgentIntegrationTest::sourceResolveNodeReportsObjectNameFallback()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto queryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("includeSource"), false },
    }, 1, &errorMessage);
    QVERIFY2(queryResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray matches = queryResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    const int nodeId = matches.at(0).toObject().value(QStringLiteral("nodeId")).toInt(-1);
    QVERIFY(nodeId > 0);

    const auto sourceResponse = invoke(&client, QStringLiteral("Source.resolveNode"), {
        { QStringLiteral("nodeId"), nodeId },
    }, 2, &errorMessage);
    QVERIFY2(sourceResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray fallbackLocations = sourceResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("fallbackLocations")).toArray();
    QCOMPARE(fallbackLocations.size(), 1);

    const QJsonObject fallback = fallbackLocations.at(0).toObject();
    QCOMPARE(fallback.value(QStringLiteral("method")).toString(),
             QStringLiteral("objectName-source-scan"));
    QVERIFY(fallback.value(QStringLiteral("line")).toInt(-1) > 0);
    QVERIFY(fallback.value(QStringLiteral("confidence")).toDouble() < 0.95);

    const auto dynamicQueryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.content\"") },
        { QStringLiteral("includeSource"), false },
    }, 3, &errorMessage);
    QVERIFY2(dynamicQueryResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray dynamicMatches = dynamicQueryResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(dynamicMatches.size(), 1);
    const int dynamicNodeId = dynamicMatches.at(0).toObject().value(QStringLiteral("nodeId")).toInt(-1);
    QVERIFY(dynamicNodeId > 0);

    const auto dynamicSourceResponse = invoke(&client, QStringLiteral("Source.resolveNode"), {
        { QStringLiteral("nodeId"), dynamicNodeId },
    }, 4, &errorMessage);
    QVERIFY2(dynamicSourceResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray dynamicFallbackLocations =
            dynamicSourceResponse->value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("fallbackLocations")).toArray();
    QCOMPARE(dynamicFallbackLocations.size(), 0);
}

void QmlAgentIntegrationTest::qtQuickControlsExposeAuthoredIds()
{
    QString errorMessage;
    SmokeAppRunner controls;
    QVERIFY2(controls.startControlsSmoke(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(controls.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    struct ExpectedControl
    {
        QString qmlId;
        QString type;
        int delegateIndex = -1;
    };
    const QList<ExpectedControl> expectedControls{
        { QStringLiteral("controlsButton"), QStringLiteral("Button") },
        { QStringLiteral("controlsToolButton"), QStringLiteral("ToolButton") },
        { QStringLiteral("controlsRoundButton"), QStringLiteral("RoundButton") },
        { QStringLiteral("controlsDelayButton"), QStringLiteral("DelayButton") },
        { QStringLiteral("controlsCheckBox"), QStringLiteral("CheckBox") },
        { QStringLiteral("controlsRadioButton"), QStringLiteral("RadioButton") },
        { QStringLiteral("controlsSwitch"), QStringLiteral("Switch") },
        { QStringLiteral("controlsFrame"), QStringLiteral("Frame") },
        { QStringLiteral("controlsGroupBox"), QStringLiteral("GroupBox") },
        { QStringLiteral("controlsToolBar"), QStringLiteral("ToolBar") },
        { QStringLiteral("controlsToolBarButton"), QStringLiteral("ToolButton") },
        { QStringLiteral("controlsToolSeparator"), QStringLiteral("ToolSeparator") },
        { QStringLiteral("controlsSlider"), QStringLiteral("Slider") },
        { QStringLiteral("controlsRangeSlider"), QStringLiteral("RangeSlider") },
        { QStringLiteral("controlsDial"), QStringLiteral("Dial") },
        { QStringLiteral("controlsSpinBox"), QStringLiteral("SpinBox") },
        { QStringLiteral("controlsDoubleSpinBox"), QStringLiteral("DoubleSpinBox") },
        { QStringLiteral("controlsTextField"), QStringLiteral("TextField") },
        { QStringLiteral("controlsSearchField"), QStringLiteral("SearchField") },
        { QStringLiteral("controlsTextArea"), QStringLiteral("TextArea") },
        { QStringLiteral("controlsComboBox"), QStringLiteral("ComboBox") },
        { QStringLiteral("controlsTumbler"), QStringLiteral("Tumbler") },
        { QStringLiteral("controlsCheckDelegate"), QStringLiteral("CheckDelegate") },
        { QStringLiteral("controlsItemDelegate"), QStringLiteral("ItemDelegate") },
        { QStringLiteral("controlsRadioDelegate"), QStringLiteral("RadioDelegate") },
        { QStringLiteral("controlsSwitchDelegate"), QStringLiteral("SwitchDelegate") },
        { QStringLiteral("controlsSwipeDelegate"), QStringLiteral("SwipeDelegate") },
        { QStringLiteral("controlsPageIndicator"), QStringLiteral("PageIndicator") },
        { QStringLiteral("controlsProgressBar"), QStringLiteral("ProgressBar") },
        { QStringLiteral("controlsBusyIndicator"), QStringLiteral("BusyIndicator") },
        { QStringLiteral("controlsScrollView"), QStringLiteral("ScrollView") },
        { QStringLiteral("controlsScrollTextArea"), QStringLiteral("TextArea") },
        { QStringLiteral("controlsFlickableListView"), QStringLiteral("ListView") },
        { QStringLiteral("controlsFlickableListDelegate"), QStringLiteral("ItemDelegate"), 0 },
        { QStringLiteral("controlsScrollBar"), QStringLiteral("ScrollBar") },
        { QStringLiteral("controlsScrollIndicator"), QStringLiteral("ScrollIndicator") },
        { QStringLiteral("controlsTabBar"), QStringLiteral("TabBar") },
        { QStringLiteral("controlsTabButton"), QStringLiteral("TabButton") },
        { QStringLiteral("controlsSecondTabButton"), QStringLiteral("TabButton") },
        { QStringLiteral("controlsSwipeView"), QStringLiteral("SwipeView") },
        { QStringLiteral("controlsPage"), QStringLiteral("Page") },
        { QStringLiteral("controlsSecondPage"), QStringLiteral("Page") },
        { QStringLiteral("controlsStackView"), QStringLiteral("StackView") },
        { QStringLiteral("controlsStackLabel"), QStringLiteral("Label") },
        { QStringLiteral("controlsSplitView"), QStringLiteral("SplitView") },
        { QStringLiteral("controlsSplitLeftPane"), QStringLiteral("Pane") },
        { QStringLiteral("controlsSplitRightPane"), QStringLiteral("Pane") },
        { QStringLiteral("controlsHorizontalHeaderView"), QStringLiteral("HorizontalHeaderView") },
        { QStringLiteral("controlsVerticalHeaderView"), QStringLiteral("VerticalHeaderView") },
        { QStringLiteral("controlsTableView"), QStringLiteral("TableView") },
        { QStringLiteral("controlsTreeView"), QStringLiteral("TreeView") },
        { QStringLiteral("controlsOpenPopupButton"), QStringLiteral("Button") },
        { QStringLiteral("controlsOpenDialogButton"), QStringLiteral("Button") },
        { QStringLiteral("controlsOpenDrawerButton"), QStringLiteral("Button") },
        { QStringLiteral("controlsOpenMenuButton"), QStringLiteral("Button") },
        { QStringLiteral("controlsToolTipButton"), QStringLiteral("Button") },
        { QStringLiteral("controlsMenuBar"), QStringLiteral("MenuBar") },
        { QStringLiteral("controlsMenuBarItem"), QStringLiteral("MenuBarItem") },
        { QStringLiteral("controlsStatus"), QStringLiteral("Label") },
    };

    int requestId = 1;
    struct QueryFirstResult
    {
        QJsonObject node;
        QString failure;

        bool ok() const { return failure.isEmpty(); }
    };
    struct PropertyValueResult
    {
        QJsonValue value;
        QString failure;

        bool ok() const { return failure.isEmpty(); }
    };
    auto queryFirst = [&](const QString &selector, const QJsonArray &properties = {}) {
        QJsonObject params{
            { QStringLiteral("selector"), selector },
            { QStringLiteral("includeSource"), true },
        };
        if (!properties.isEmpty())
            params.insert(QStringLiteral("properties"), properties);
        const auto response = invoke(&client, QStringLiteral("UI.query"), params, requestId++,
                                     &errorMessage);
        if (!response.has_value()) {
            return QueryFirstResult{
                {},
                QStringLiteral("UI.query(%1) failed: %2\nTarget output:\n%3")
                        .arg(selector, errorMessage,
                             QString::fromUtf8(controls.readAllOutput())),
            };
        }
        const QJsonObject result = response->value(QStringLiteral("result")).toObject();
        const QJsonArray matches = result.value(QStringLiteral("matches")).toArray();
        if (matches.size() != 1)
            return QueryFirstResult{
                {},
                QStringLiteral("UI.query(%1) expected one match, got %2: %3")
                        .arg(selector)
                        .arg(matches.size())
                        .arg(QString::fromUtf8(QJsonDocument(result)
                                                       .toJson(QJsonDocument::Compact))),
            };
        return QueryFirstResult{ matches.at(0).toObject(), {} };
    };
    auto propertyValue = [&](const QString &qmlId, const QString &property) {
        const QueryFirstResult result = queryFirst(QStringLiteral("id=\"%1\"").arg(qmlId),
                                                   QJsonArray{ property });
        if (!result.ok())
            return PropertyValueResult{ {}, result.failure };
        const QJsonObject node = result.node;
        if (node.contains(property))
            return PropertyValueResult{ node.value(property), {} };
        const QJsonObject propertiesObject = node.value(QStringLiteral("properties")).toObject();
        if (!propertiesObject.contains(property)) {
            return PropertyValueResult{
                {},
                QStringLiteral("UI.query(id=\"%1\") did not return requested property %2: %3")
                        .arg(qmlId, property,
                             QString::fromUtf8(QJsonDocument(node)
                                                       .toJson(QJsonDocument::Compact))),
            };
        }
        return PropertyValueResult{ propertiesObject.value(property), {} };
    };
    auto clickSelectorAndVerify = [&](const QString &selector, const QString &label) {
        const auto response = invoke(&client, QStringLiteral("Input.clickNode"), {
            { QStringLiteral("selector"), selector },
        }, requestId++, &errorMessage);
        QVERIFY2(response.has_value(), qPrintable(errorMessage));
        const QJsonObject result = response->value(QStringLiteral("result")).toObject();
        QVERIFY2(result.value(QStringLiteral("delivered")).toBool(),
                 qPrintable(QStringLiteral("%1 click failed: %2")
                                    .arg(label,
                                         QString::fromUtf8(QJsonDocument(result)
                                                                  .toJson(QJsonDocument::Compact)))));
        QCOMPARE(result.value(QStringLiteral("mode")).toString(),
                 QStringLiteral("synthetic-qt-event"));
    };
    auto clickAndVerify = [&](const QString &qmlId) {
        clickSelectorAndVerify(QStringLiteral("id=\"%1\"").arg(qmlId), qmlId);
    };
    auto waitForFound = [&](const QString &selector, int timeoutMs = 1000) {
        const auto response = invoke(&client, QStringLiteral("UI.waitFor"), {
            { QStringLiteral("selector"), selector },
            { QStringLiteral("until"), QJsonObject{
                { QStringLiteral("state"), QStringLiteral("found") },
            } },
            { QStringLiteral("timeoutMs"), timeoutMs },
        }, requestId++, &errorMessage);
        QVERIFY2(response.has_value(), qPrintable(errorMessage));
        const QJsonObject result = response->value(QStringLiteral("result")).toObject();
        QVERIFY2(result.value(QStringLiteral("ok")).toBool(false),
                 qPrintable(QStringLiteral("UI.waitFor(%1) failed: %2")
                                    .arg(selector,
                                         QString::fromUtf8(QJsonDocument(result)
                                                                  .toJson(QJsonDocument::Compact)))));
    };
    auto hasReason = [](const QJsonArray &reasons, const QString &id) {
        for (const QJsonValue &reasonValue : reasons) {
            if (reasonValue.toObject().value(QStringLiteral("id")).toString() == id)
                return true;
        }
        return false;
    };

    for (const auto &expectedControl : expectedControls) {
        QString selector = QStringLiteral("id=\"%1\"").arg(expectedControl.qmlId);
        if (expectedControl.delegateIndex >= 0)
            selector += QStringLiteral(" index=%1").arg(expectedControl.delegateIndex);
        const QueryFirstResult query = queryFirst(selector);
        QVERIFY2(query.ok(), qPrintable(query.failure));
        const QJsonObject node = query.node;
        QCOMPARE(node.value(QStringLiteral("qmlId")).toString(), expectedControl.qmlId);
        QVERIFY2(node.value(QStringLiteral("type")).toString().contains(expectedControl.type),
                 qPrintable(QStringLiteral("Expected %1 to resolve to %2, got %3")
                                    .arg(expectedControl.qmlId,
                                         expectedControl.type,
                                         node.value(QStringLiteral("type")).toString())));
        if (expectedControl.delegateIndex >= 0) {
            QCOMPARE(node.value(QStringLiteral("delegate")).toObject()
                             .value(QStringLiteral("index")).toInt(-1),
                     expectedControl.delegateIndex);
        }
        QCOMPARE(node.value(QStringLiteral("sourceLocation")).toObject()
                         .value(QStringLiteral("method")).toString(),
                 expectedControl.delegateIndex >= 0 ? QStringLiteral("qqmldata-delegate")
                                                    : QStringLiteral("qqmldata-direct"));
        if (!node.value(QStringLiteral("frameworkInternal")).toBool(false)) {
            QCOMPARE(node.value(QStringLiteral("sourceLocation")).toObject()
                             .value(QStringLiteral("file")).toString().endsWith(
                                     QStringLiteral("ControlsSmoke.qml")),
                     true);
        }
    }

    const auto exactButtonTypeResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("type=\"Button\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("maxNodes"), 50 },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("type"),
            QStringLiteral("typeAliases"),
            QStringLiteral("qmlId"),
        } },
    }, requestId++, &errorMessage);
    QVERIFY2(exactButtonTypeResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray exactButtonMatches =
            exactButtonTypeResponse->value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("matches")).toArray();
    QVERIFY2(!exactButtonMatches.isEmpty(), "Expected exact type=\"Button\" to find Button controls.");
    for (const QJsonValue &matchValue : exactButtonMatches) {
        const QJsonObject node = matchValue.toObject();
        const QString type = node.value(QStringLiteral("type")).toString();
        const QJsonArray aliases = node.value(QStringLiteral("typeAliases")).toArray();
        bool exactTypeMatch = type == QLatin1String("Button");
        for (const QJsonValue &alias : aliases)
            exactTypeMatch = exactTypeMatch || alias.toString() == QLatin1String("Button");
        QVERIFY2(exactTypeMatch,
                 qPrintable(QStringLiteral("type=\"Button\" must not use substring matching: %1")
                                    .arg(QString::fromUtf8(QJsonDocument(node)
                                                                   .toJson(QJsonDocument::Compact)))));
        QVERIFY2(type != QLatin1String("ToolButton")
                         && type != QLatin1String("RoundButton")
                         && type != QLatin1String("RadioButton")
                         && type != QLatin1String("DelayButton"),
                 qPrintable(QStringLiteral("type=\"Button\" must not match %1").arg(type)));
    }

    auto queryRepeatedDelegateIds = [&](const QString &qmlId, const QString &type) {
        const auto response = invoke(&client, QStringLiteral("UI.query"), {
            { QStringLiteral("selector"), QStringLiteral("id=\"%1\"").arg(qmlId) },
            { QStringLiteral("includeSource"), true },
            { QStringLiteral("maxNodes"), 10 },
        }, requestId++, &errorMessage);
        QVERIFY2(response.has_value(), qPrintable(errorMessage));
        const QJsonArray matches = response->value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("matches")).toArray();
        QVERIFY2(!matches.isEmpty(),
                 qPrintable(QStringLiteral("Expected repeated delegate id %1").arg(qmlId)));
        for (const QJsonValue &matchValue : matches) {
            const QJsonObject node = matchValue.toObject();
            QCOMPARE(node.value(QStringLiteral("qmlId")).toString(), qmlId);
            QVERIFY2(node.value(QStringLiteral("type")).toString().contains(type),
                     qPrintable(QStringLiteral("Expected %1 to resolve to %2, got %3")
                                        .arg(qmlId, type,
                                             node.value(QStringLiteral("type")).toString())));
            QCOMPARE(node.value(QStringLiteral("sourceLocation")).toObject()
                             .value(QStringLiteral("method")).toString(),
                     QStringLiteral("qqmldata-delegate"));
            QVERIFY2(node.value(QStringLiteral("delegate")).toObject()
                             .value(QStringLiteral("indexSource")).toString()
                             != QLatin1String("modelIndex"),
                     "TableView/TreeView delegates need row/column selector semantics, not ListView index.");
        }
    };
    queryRepeatedDelegateIds(QStringLiteral("controlsTableViewDelegate"),
                             QStringLiteral("TableViewDelegate"));
    queryRepeatedDelegateIds(QStringLiteral("controlsTreeViewDelegate"),
                             QStringLiteral("TreeViewDelegate"));

    auto queryCellDelegate = [&](const QString &selector, QJsonObject *node) {
        const QueryFirstResult query = queryFirst(selector);
        QVERIFY2(query.ok(), qPrintable(query.failure));
        const QJsonObject delegate = query.node.value(QStringLiteral("delegate")).toObject();
        QCOMPARE(delegate.value(QStringLiteral("row")).toInt(-1), 0);
        QCOMPARE(delegate.value(QStringLiteral("column")).toInt(-1), 0);
        QCOMPARE(delegate.value(QStringLiteral("cellSource")).toString(),
                 QStringLiteral("delegateRowColumn"));
        *node = query.node;
    };
    QJsonObject tableCell;
    queryCellDelegate(QStringLiteral("id=\"controlsTableViewDelegate\" row=0 column=0"),
                      &tableCell);
    QCOMPARE(tableCell.value(QStringLiteral("qmlId")).toString(),
             QStringLiteral("controlsTableViewDelegate"));
    QJsonObject treeCell;
    queryCellDelegate(QStringLiteral("id=\"controlsTreeViewDelegate\" row=0 column=0"),
                      &treeCell);
    QCOMPARE(treeCell.value(QStringLiteral("qmlId")).toString(),
             QStringLiteral("controlsTreeViewDelegate"));

    clickSelectorAndVerify(QStringLiteral("id=\"controlsTableViewDelegate\" row=0 column=0"),
                           QStringLiteral("controlsTableViewDelegate row=0 column=0"));
    clickSelectorAndVerify(QStringLiteral("id=\"controlsTreeViewDelegate\" row=0 column=0"),
                           QStringLiteral("controlsTreeViewDelegate row=0 column=0"));

    clickAndVerify(QStringLiteral("controlsButton"));
    clickAndVerify(QStringLiteral("controlsToolButton"));
    clickAndVerify(QStringLiteral("controlsRoundButton"));
    clickAndVerify(QStringLiteral("controlsCheckBox"));
    clickAndVerify(QStringLiteral("controlsRadioButton"));
    clickAndVerify(QStringLiteral("controlsSwitch"));
    clickAndVerify(QStringLiteral("controlsItemDelegate"));
    clickAndVerify(QStringLiteral("controlsCheckDelegate"));
    clickAndVerify(QStringLiteral("controlsRadioDelegate"));
    clickAndVerify(QStringLiteral("controlsSwitchDelegate"));

    clickAndVerify(QStringLiteral("controlsOpenPopupButton"));
    waitForFound(QStringLiteral("id=\"controlsPopupCloseButton\""));
    const auto blockedButtonResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsButton\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("qmlId"),
            QStringLiteral("actionable"),
            QStringLiteral("interactable"),
        } },
    }, requestId++, &errorMessage);
    QVERIFY2(blockedButtonResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject blockedButtonNode = blockedButtonResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray().at(0).toObject();
    QCOMPARE(blockedButtonNode.value(QStringLiteral("actionable")).toBool(true), false);
    const QJsonArray blockedReasons = blockedButtonNode.value(QStringLiteral("interactable"))
            .toObject().value(QStringLiteral("reasons")).toArray();
    QVERIFY2(hasReason(blockedReasons, QStringLiteral("blocked_by_modal_popup")),
             qPrintable(QStringLiteral("Expected popup overlay to block controlsButton: %1")
                                .arg(QString::fromUtf8(QJsonDocument(blockedButtonNode)
                                                       .toJson(QJsonDocument::Compact)))));
    const auto blockedDiagnosticsResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsButton\"") },
        { QStringLiteral("checks"), QJsonArray{ QStringLiteral("actionable") } },
    }, requestId++, &errorMessage);
    QVERIFY2(blockedDiagnosticsResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray blockedIssues = blockedDiagnosticsResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("issues")).toArray();
    QCOMPARE(blockedIssues.size(), 1);
    QCOMPARE(blockedIssues.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_actionable"));
    QVERIFY2(!blockedIssues.at(0).toObject().value(QStringLiteral("evidenceProfile")).toObject().isEmpty(),
             qPrintable(QStringLiteral("Expected modal-popup actionability diagnostic to carry evidenceProfile: %1")
                                .arg(QString::fromUtf8(QJsonDocument(blockedIssues.at(0).toObject())
                                                       .toJson(QJsonDocument::Compact)))));
    QVERIFY2(hasReason(blockedIssues.at(0).toObject().value(QStringLiteral("actionability"))
                              .toObject().value(QStringLiteral("reasons")).toArray(),
                       QStringLiteral("blocked_by_modal_popup")),
             qPrintable(QStringLiteral("Expected actionable diagnostics to include blocked_by_modal_popup: %1")
                                .arg(QString::fromUtf8(QJsonDocument(blockedIssues.at(0).toObject())
                                                       .toJson(QJsonDocument::Compact)))));
    const auto blockedClickResponse = invoke(&client, QStringLiteral("Input.clickNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsButton\"") },
    }, requestId++, &errorMessage);
    QVERIFY2(blockedClickResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject blockedClickResult = blockedClickResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(blockedClickResult.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(blockedClickResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("blocked_by_modal_popup"));
    clickAndVerify(QStringLiteral("controlsPopupCloseButton"));
    const auto popupClosedResponse = invoke(&client, QStringLiteral("UI.waitFor"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsPopupCloseButton\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("state"), QStringLiteral("notFound") },
        } },
        { QStringLiteral("timeoutMs"), 1000 },
    }, requestId++, &errorMessage);
    QVERIFY2(popupClosedResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject popupClosed = popupClosedResponse->value(QStringLiteral("result")).toObject();
    QVERIFY2(popupClosed.value(QStringLiteral("ok")).toBool(false),
             qPrintable(QStringLiteral("Expected popup close button to disappear after close click: %1")
                                .arg(QString::fromUtf8(QJsonDocument(popupClosed)
                                                       .toJson(QJsonDocument::Compact)))));
    clickAndVerify(QStringLiteral("controlsOpenDialogButton"));
    waitForFound(QStringLiteral("id=\"controlsDialogConfirmButton\""));
    clickAndVerify(QStringLiteral("controlsDialogConfirmButton"));
    clickAndVerify(QStringLiteral("controlsOpenDrawerButton"));
    waitForFound(QStringLiteral("id=\"controlsDrawerCloseButton\""));
    clickAndVerify(QStringLiteral("controlsDrawerCloseButton"));

    clickAndVerify(QStringLiteral("controlsOpenMenuButton"));
    waitForFound(QStringLiteral("id=\"controlsStandaloneMenuItem\""));
    clickAndVerify(QStringLiteral("controlsStandaloneMenuItem"));

    clickAndVerify(QStringLiteral("controlsComboBox"));
    waitForFound(QStringLiteral("text=\"Two\""));
    const auto comboChoiceQueryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("text=\"Two\"") },
        { QStringLiteral("includeSource"), true },
    }, requestId++, &errorMessage);
    QVERIFY2(comboChoiceQueryResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject comboChoiceQueryResult = comboChoiceQueryResponse
            ->value(QStringLiteral("result")).toObject();
    // The delegate label and its ItemDelegate share one ancestor chain, so the
    // text match collapses onto the outermost delegate: a single unambiguous
    // actionable match with no ambiguity diagnostics. Qt Quick Controls
    // ComboBox popup delegates are framework-created and may not expose a
    // unique app-stable selector, so click the bounded session-local nodeId
    // instead of adding app-only objectNames.
    const QJsonArray comboChoiceDiagnostics = comboChoiceQueryResult
            .value(QStringLiteral("diagnostics")).toArray();
    QVERIFY2(comboChoiceDiagnostics.isEmpty(),
             qPrintable(QString::fromUtf8(QJsonDocument(comboChoiceQueryResult)
                                          .toJson(QJsonDocument::Compact))));
    const QJsonArray comboChoiceMatches =
            comboChoiceQueryResult.value(QStringLiteral("matches")).toArray();
    QCOMPARE(comboChoiceMatches.size(), 1);
    int comboChoiceNodeId = -1;
    for (const QJsonValue &matchValue : comboChoiceMatches) {
        const QJsonObject match = matchValue.toObject();
        if (match.value(QStringLiteral("type")).toString().contains(
                    QLatin1String("ItemDelegate"))) {
            comboChoiceNodeId = match.value(QStringLiteral("nodeId")).toInt(-1);
            break;
        }
    }
    QVERIFY2(comboChoiceNodeId > 0,
             qPrintable(QString::fromUtf8(QJsonDocument(comboChoiceQueryResult)
                                          .toJson(QJsonDocument::Compact))));
    QJsonObject comboClickParams;
    comboClickParams.insert(QStringLiteral("nodeId"), comboChoiceNodeId);
    const auto comboChoiceResponse = invoke(&client, QStringLiteral("Input.clickNode"),
                                            comboClickParams, requestId++, &errorMessage);
    QVERIFY2(comboChoiceResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject comboChoiceResult = comboChoiceResponse->value(QStringLiteral("result")).toObject();
    QVERIFY2(comboChoiceResult.value(QStringLiteral("delivered")).toBool(false),
             qPrintable(QStringLiteral("ComboBox popup choice click failed: %1")
                                .arg(QString::fromUtf8(QJsonDocument(comboChoiceResult)
                                                       .toJson(QJsonDocument::Compact)))));

    const auto toolTipHoverResponse = invoke(&client, QStringLiteral("Input.dispatchMouseEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsToolTipButton\"") },
        { QStringLiteral("type"), QStringLiteral("mouseMove") },
        { QStringLiteral("point"), QJsonArray{ 32, 16 } },
    }, requestId++, &errorMessage);
    QVERIFY2(toolTipHoverResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject toolTipHoverResult =
            toolTipHoverResponse->value(QStringLiteral("result")).toObject();
    QVERIFY2(toolTipHoverResult.value(QStringLiteral("delivered")).toBool(false),
             qPrintable(QStringLiteral("ToolTip trigger hover failed: %1")
                                .arg(QString::fromUtf8(QJsonDocument(toolTipHoverResult)
                                                       .toJson(QJsonDocument::Compact)))));
    waitForFound(QStringLiteral("id=\"controlsToolTipLabel\""));

    const auto statusResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsStatus\"") },
    }, requestId++, &errorMessage);
    QVERIFY2(statusResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray statusMatches = statusResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE_EQ(statusMatches.size(), 1);
    const QueryFirstResult statusState = queryFirst(
            QStringLiteral("id=\"controlsStatus\""),
            QJsonArray{
                QStringLiteral("buttonState"),
                QStringLiteral("toolButtonState"),
                QStringLiteral("roundButtonState"),
                QStringLiteral("delegateState"),
                QStringLiteral("popupState"),
                QStringLiteral("dialogState"),
                QStringLiteral("drawerState"),
                QStringLiteral("menuCount"),
                QStringLiteral("comboCount"),
                QStringLiteral("comboIndex"),
                QStringLiteral("comboText"),
                QStringLiteral("toolTipHovered"),
                QStringLiteral("rangeFirstValue"),
                QStringLiteral("rangeSecondValue"),
                QStringLiteral("dialValue"),
            });
    QVERIFY2(statusState.ok(), qPrintable(statusState.failure));
    const QJsonObject statusProperties = statusState.node.value(QStringLiteral("properties")).toObject();
    QCOMPARE(statusProperties.value(QStringLiteral("buttonState")).toBool(), true);
    QCOMPARE(statusProperties.value(QStringLiteral("toolButtonState")).toBool(), true);
    QCOMPARE(statusProperties.value(QStringLiteral("roundButtonState")).toBool(), true);
    QCOMPARE(statusProperties.value(QStringLiteral("delegateState")).toBool(), true);
    QCOMPARE(statusProperties.value(QStringLiteral("popupState")).toBool(), true);
    QCOMPARE(statusProperties.value(QStringLiteral("dialogState")).toBool(), true);
    QCOMPARE(statusProperties.value(QStringLiteral("drawerState")).toBool(), true);
    QCOMPARE(statusProperties.value(QStringLiteral("menuCount")).toInt(), 1);
    QCOMPARE(statusProperties.value(QStringLiteral("comboCount")).toInt(), 1);
    QCOMPARE(statusProperties.value(QStringLiteral("comboIndex")).toInt(), 1);
    QCOMPARE(statusProperties.value(QStringLiteral("comboText")).toString(), QStringLiteral("Two"));
    QCOMPARE(statusProperties.value(QStringLiteral("toolTipHovered")).toBool(), true);
    QCOMPARE(statusProperties.value(QStringLiteral("rangeFirstValue")).toDouble(), 2.0);
    QCOMPARE(statusProperties.value(QStringLiteral("rangeSecondValue")).toDouble(), 8.0);
    QCOMPARE(statusProperties.value(QStringLiteral("dialValue")).toDouble(), 3.0);
    const PropertyValueResult checkBoxChecked =
            propertyValue(QStringLiteral("controlsCheckBox"), QStringLiteral("checked"));
    QVERIFY2(checkBoxChecked.ok(), qPrintable(checkBoxChecked.failure));
    QCOMPARE(checkBoxChecked.value.toBool(), true);
    const PropertyValueResult radioButtonChecked =
            propertyValue(QStringLiteral("controlsRadioButton"), QStringLiteral("checked"));
    QVERIFY2(radioButtonChecked.ok(), qPrintable(radioButtonChecked.failure));
    QCOMPARE(radioButtonChecked.value.toBool(), true);
    const PropertyValueResult switchChecked =
            propertyValue(QStringLiteral("controlsSwitch"), QStringLiteral("checked"));
    QVERIFY2(switchChecked.ok(), qPrintable(switchChecked.failure));
    QCOMPARE(switchChecked.value.toBool(), true);
    const auto switchTouchBeginResponse = invoke(&client, QStringLiteral("Input.dispatchTouchEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsSwitch\"") },
        { QStringLiteral("type"), QStringLiteral("touchBegin") },
        { QStringLiteral("points"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), 1 },
            { QStringLiteral("state"), QStringLiteral("pressed") },
            { QStringLiteral("point"), QJsonArray{ 26, 14 } },
        } } },
    }, requestId++, &errorMessage);
    QVERIFY2(switchTouchBeginResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(switchTouchBeginResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    const auto switchTouchEndResponse = invoke(&client, QStringLiteral("Input.dispatchTouchEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsSwitch\"") },
        { QStringLiteral("type"), QStringLiteral("touchEnd") },
        { QStringLiteral("points"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), 1 },
            { QStringLiteral("state"), QStringLiteral("released") },
            { QStringLiteral("point"), QJsonArray{ 26, 14 } },
        } } },
    }, requestId++, &errorMessage);
    QVERIFY2(switchTouchEndResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(switchTouchEndResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    const PropertyValueResult switchTouched =
            propertyValue(QStringLiteral("controlsSwitch"), QStringLiteral("checked"));
    QVERIFY2(switchTouched.ok(), qPrintable(switchTouched.failure));
    QCOMPARE(switchTouched.value.toBool(), false);
    const PropertyValueResult checkDelegateChecked =
            propertyValue(QStringLiteral("controlsCheckDelegate"), QStringLiteral("checked"));
    QVERIFY2(checkDelegateChecked.ok(), qPrintable(checkDelegateChecked.failure));
    QCOMPARE(checkDelegateChecked.value.toBool(), true);
    const PropertyValueResult switchDelegateChecked =
            propertyValue(QStringLiteral("controlsSwitchDelegate"), QStringLiteral("checked"));
    QVERIFY2(switchDelegateChecked.ok(), qPrintable(switchDelegateChecked.failure));
    QCOMPARE(switchDelegateChecked.value.toBool(), true);
    const PropertyValueResult radioDelegateChecked =
            propertyValue(QStringLiteral("controlsRadioDelegate"), QStringLiteral("checked"));
    QVERIFY2(radioDelegateChecked.ok(), qPrintable(radioDelegateChecked.failure));
    QCOMPARE(radioDelegateChecked.value.toBool(), true);

    const auto typeTextFieldResponse = invoke(&client, QStringLiteral("Input.typeText"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsTextField\"") },
        { QStringLiteral("text"), QStringLiteral("X") },
    }, requestId++, &errorMessage);
    QVERIFY2(typeTextFieldResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(typeTextFieldResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    const PropertyValueResult textFieldText =
            propertyValue(QStringLiteral("controlsTextField"), QStringLiteral("text"));
    QVERIFY2(textFieldText.ok(), qPrintable(textFieldText.failure));
    QVERIFY(textFieldText.value.toString().endsWith(QLatin1Char('X')));

    const auto typeSearchResponse = invoke(&client, QStringLiteral("Input.typeText"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsSearchField\"") },
        { QStringLiteral("text"), QStringLiteral("Y") },
    }, requestId++, &errorMessage);
    QVERIFY2(typeSearchResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(typeSearchResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    const PropertyValueResult searchFieldText =
            propertyValue(QStringLiteral("controlsSearchField"), QStringLiteral("text"));
    QVERIFY2(searchFieldText.ok(), qPrintable(searchFieldText.failure));
    QVERIFY2(searchFieldText.value.toString().endsWith(QLatin1Char('Y')),
             qPrintable(QStringLiteral("Expected SearchField text to end with Y, actual=%1, typeResult=%2")
                                .arg(searchFieldText.value.toString(),
                                     QString::fromUtf8(QJsonDocument(
                                             typeSearchResponse->value(QStringLiteral("result")).toObject())
                                                           .toJson(QJsonDocument::Compact)))));

    const auto typeTextAreaResponse = invoke(&client, QStringLiteral("Input.typeText"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsTextArea\"") },
        { QStringLiteral("text"), QStringLiteral("Z") },
    }, requestId++, &errorMessage);
    QVERIFY2(typeTextAreaResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(typeTextAreaResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    const PropertyValueResult textAreaText =
            propertyValue(QStringLiteral("controlsTextArea"), QStringLiteral("text"));
    QVERIFY2(textAreaText.ok(), qPrintable(textAreaText.failure));
    QVERIFY(textAreaText.value.toString().endsWith(QLatin1Char('Z')));

    const PropertyValueResult spinBeforeProperty =
            propertyValue(QStringLiteral("controlsSpinBox"), QStringLiteral("value"));
    QVERIFY2(spinBeforeProperty.ok(), qPrintable(spinBeforeProperty.failure));
    const double spinBefore = spinBeforeProperty.value.toDouble();
    const auto spinKeyResponse = invoke(&client, QStringLiteral("Input.dispatchKeyEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsSpinBox\"") },
        { QStringLiteral("keyCode"), 16777235 },
    }, requestId++, &errorMessage);
    QVERIFY2(spinKeyResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(spinKeyResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    const PropertyValueResult spinAfterProperty =
            propertyValue(QStringLiteral("controlsSpinBox"), QStringLiteral("value"));
    QVERIFY2(spinAfterProperty.ok(), qPrintable(spinAfterProperty.failure));
    QVERIFY2(spinAfterProperty.value.toDouble() >= spinBefore,
             "Expected SpinBox Up key to keep or increase value.");

    const PropertyValueResult doubleSpinBeforeProperty =
            propertyValue(QStringLiteral("controlsDoubleSpinBox"), QStringLiteral("value"));
    QVERIFY2(doubleSpinBeforeProperty.ok(), qPrintable(doubleSpinBeforeProperty.failure));
    const double doubleSpinBefore = doubleSpinBeforeProperty.value.toDouble();
    const auto doubleSpinKeyResponse = invoke(&client, QStringLiteral("Input.dispatchKeyEvent"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsDoubleSpinBox\"") },
        { QStringLiteral("keyCode"), 16777235 },
    }, requestId++, &errorMessage);
    QVERIFY2(doubleSpinKeyResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(doubleSpinKeyResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    const PropertyValueResult doubleSpinAfterProperty =
            propertyValue(QStringLiteral("controlsDoubleSpinBox"), QStringLiteral("value"));
    QVERIFY2(doubleSpinAfterProperty.ok(), qPrintable(doubleSpinAfterProperty.failure));
    QVERIFY2(doubleSpinAfterProperty.value.toDouble() >= doubleSpinBefore,
             "Expected DoubleSpinBox Up key to keep or increase value.");

    const auto sliderBeforeResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsSlider\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("value") } },
    }, requestId++, &errorMessage);
    QVERIFY2(sliderBeforeResponse.has_value(), qPrintable(errorMessage));
    const double sliderBeforeValue = sliderBeforeResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray().at(0).toObject()
            .value(QStringLiteral("properties")).toObject().value(QStringLiteral("value")).toDouble();

    const auto sliderDragResponse = invoke(&client, QStringLiteral("Input.dragNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsSlider\"") },
        { QStringLiteral("from"), QJsonArray{ 40, 14 } },
        { QStringLiteral("to"), QJsonArray{ 180, 14 } },
        { QStringLiteral("steps"), 5 },
    }, requestId++, &errorMessage);
    QVERIFY2(sliderDragResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject sliderDragResult = sliderDragResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(sliderDragResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(sliderDragResult.value(QStringLiteral("mode")).toString(),
             QStringLiteral("synthetic-qt-event"));

    const auto sliderAfterResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsSlider\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("value") } },
    }, requestId++, &errorMessage);
    QVERIFY2(sliderAfterResponse.has_value(), qPrintable(errorMessage));
    const double sliderAfterValue = sliderAfterResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray().at(0).toObject()
            .value(QStringLiteral("properties")).toObject().value(QStringLiteral("value")).toDouble();
    QVERIFY2(sliderAfterValue > sliderBeforeValue,
             qPrintable(QStringLiteral("Expected Controls Slider drag to increase value: before=%1 after=%2")
                                .arg(sliderBeforeValue).arg(sliderAfterValue)));

    const PropertyValueResult rangeFirstBeforeProperty =
            propertyValue(QStringLiteral("controlsStatus"), QStringLiteral("rangeFirstValue"));
    QVERIFY2(rangeFirstBeforeProperty.ok(), qPrintable(rangeFirstBeforeProperty.failure));
    const double rangeFirstBefore = rangeFirstBeforeProperty.value.toDouble();
    const auto rangeFirstDragResponse = invoke(&client, QStringLiteral("Input.dragNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsRangeSlider\"") },
        { QStringLiteral("from"), QJsonArray{ 44, 12 } },
        { QStringLiteral("to"), QJsonArray{ 110, 12 } },
        { QStringLiteral("steps"), 10 },
    }, requestId++, &errorMessage);
    QVERIFY2(rangeFirstDragResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject rangeFirstDragResult =
            rangeFirstDragResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(rangeFirstDragResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(rangeFirstDragResult.value(QStringLiteral("mode")).toString(),
             QStringLiteral("synthetic-qt-event"));
    const PropertyValueResult rangeFirstAfterProperty =
            propertyValue(QStringLiteral("controlsStatus"), QStringLiteral("rangeFirstValue"));
    QVERIFY2(rangeFirstAfterProperty.ok(), qPrintable(rangeFirstAfterProperty.failure));
    QVERIFY2(rangeFirstAfterProperty.value.toDouble() > rangeFirstBefore,
             qPrintable(QStringLiteral("Expected RangeSlider first handle drag to increase value: before=%1 after=%2")
                                .arg(rangeFirstBefore)
                                .arg(rangeFirstAfterProperty.value.toDouble())));

    const PropertyValueResult rangeSecondBeforeProperty =
            propertyValue(QStringLiteral("controlsStatus"), QStringLiteral("rangeSecondValue"));
    QVERIFY2(rangeSecondBeforeProperty.ok(), qPrintable(rangeSecondBeforeProperty.failure));
    const double rangeSecondBefore = rangeSecondBeforeProperty.value.toDouble();
    const auto rangeSecondDragResponse = invoke(&client, QStringLiteral("Input.dragNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsRangeSlider\"") },
        { QStringLiteral("from"), QJsonArray{ 176, 12 } },
        { QStringLiteral("to"), QJsonArray{ 132, 12 } },
        { QStringLiteral("steps"), 10 },
    }, requestId++, &errorMessage);
    QVERIFY2(rangeSecondDragResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject rangeSecondDragResult =
            rangeSecondDragResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(rangeSecondDragResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(rangeSecondDragResult.value(QStringLiteral("mode")).toString(),
             QStringLiteral("synthetic-qt-event"));
    const PropertyValueResult rangeSecondAfterProperty =
            propertyValue(QStringLiteral("controlsStatus"), QStringLiteral("rangeSecondValue"));
    QVERIFY2(rangeSecondAfterProperty.ok(), qPrintable(rangeSecondAfterProperty.failure));
    QVERIFY2(rangeSecondAfterProperty.value.toDouble() < rangeSecondBefore,
             qPrintable(QStringLiteral("Expected RangeSlider second handle drag to decrease value: before=%1 after=%2")
                                .arg(rangeSecondBefore)
                                .arg(rangeSecondAfterProperty.value.toDouble())));

    const PropertyValueResult dialBeforeProperty =
            propertyValue(QStringLiteral("controlsDial"), QStringLiteral("value"));
    QVERIFY2(dialBeforeProperty.ok(), qPrintable(dialBeforeProperty.failure));
    const double dialBefore = dialBeforeProperty.value.toDouble();
    const auto dialDragResponse = invoke(&client, QStringLiteral("Input.dragNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsDial\"") },
        { QStringLiteral("from"), QJsonArray{ 36, 8 } },
        { QStringLiteral("to"), QJsonArray{ 64, 36 } },
        { QStringLiteral("steps"), 10 },
    }, requestId++, &errorMessage);
    QVERIFY2(dialDragResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject dialDragResult = dialDragResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(dialDragResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(dialDragResult.value(QStringLiteral("mode")).toString(),
             QStringLiteral("synthetic-qt-event"));
    const PropertyValueResult dialAfterProperty =
            propertyValue(QStringLiteral("controlsDial"), QStringLiteral("value"));
    QVERIFY2(dialAfterProperty.ok(), qPrintable(dialAfterProperty.failure));
    QVERIFY2(dialAfterProperty.value.toDouble() > dialBefore,
             qPrintable(QStringLiteral("Expected Dial drag to increase value: before=%1 after=%2")
                                .arg(dialBefore)
                                .arg(dialAfterProperty.value.toDouble())));

    const PropertyValueResult swipeBeforeProperty =
            propertyValue(QStringLiteral("controlsSwipeDelegate"), QStringLiteral("swipePosition"));
    QVERIFY2(swipeBeforeProperty.ok(), qPrintable(swipeBeforeProperty.failure));
    const double swipeBefore = swipeBeforeProperty.value.toDouble();
    const auto swipeDragResponse = invoke(&client, QStringLiteral("Input.dragNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsSwipeDelegate\"") },
        { QStringLiteral("from"), QJsonArray{ 190, 20 } },
        { QStringLiteral("to"), QJsonArray{ 20, 20 } },
        { QStringLiteral("steps"), 8 },
    }, requestId++, &errorMessage);
    QVERIFY2(swipeDragResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject swipeDragResult = swipeDragResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(swipeDragResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(swipeDragResult.value(QStringLiteral("mode")).toString(),
             QStringLiteral("synthetic-qt-event"));
    const PropertyValueResult swipeAfterProperty =
            propertyValue(QStringLiteral("controlsSwipeDelegate"), QStringLiteral("swipePosition"));
    QVERIFY2(swipeAfterProperty.ok(), qPrintable(swipeAfterProperty.failure));
    QVERIFY2(swipeAfterProperty.value.toDouble() < swipeBefore,
             qPrintable(QStringLiteral("Expected SwipeDelegate drag to move swipe.position left: before=%1 after=%2")
                                .arg(swipeBefore)
                                .arg(swipeAfterProperty.value.toDouble())));

    const PropertyValueResult tumblerBeforeProperty =
            propertyValue(QStringLiteral("controlsTumbler"), QStringLiteral("currentIndex"));
    QVERIFY2(tumblerBeforeProperty.ok(), qPrintable(tumblerBeforeProperty.failure));
    const double tumblerBefore = tumblerBeforeProperty.value.toDouble();
    const auto tumblerDragResponse = invoke(&client, QStringLiteral("Input.dragNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsTumbler\"") },
        { QStringLiteral("from"), QJsonArray{ 45, 72 } },
        { QStringLiteral("to"), QJsonArray{ 45, 8 } },
        { QStringLiteral("steps"), 12 },
    }, requestId++, &errorMessage);
    QVERIFY2(tumblerDragResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(tumblerDragResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    // Tumbler snaps to the new index through an animation after release, so
    // wait for the semantic state instead of reading currentIndex immediately.
    const auto tumblerChangedResponse = invoke(&client, QStringLiteral("UI.waitFor"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsTumbler\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("property"), QStringLiteral("currentIndex") },
            { QStringLiteral("op"), QStringLiteral("!=") },
            { QStringLiteral("value"), tumblerBefore },
        } },
        { QStringLiteral("timeoutMs"), 2000 },
    }, requestId++, &errorMessage);
    QVERIFY2(tumblerChangedResponse.has_value(), qPrintable(errorMessage));
    QVERIFY2(tumblerChangedResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("ok")).toBool(false),
             qPrintable(QStringLiteral("Expected Tumbler drag input to change currentIndex: %1")
                                .arg(QString::fromUtf8(QJsonDocument(
                                        tumblerChangedResponse->value(QStringLiteral("result"))
                                                .toObject())
                                                               .toJson(QJsonDocument::Compact)))));

    const PropertyValueResult listViewContentBeforeProperty =
            propertyValue(QStringLiteral("controlsFlickableListView"), QStringLiteral("contentY"));
    QVERIFY2(listViewContentBeforeProperty.ok(), qPrintable(listViewContentBeforeProperty.failure));
    const double listViewContentBefore = listViewContentBeforeProperty.value.toDouble();
    const auto listViewWheelResponse = invoke(&client, QStringLiteral("Input.wheel"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsFlickableListView\"") },
        { QStringLiteral("deltaY"), -240 },
    }, requestId++, &errorMessage);
    QVERIFY2(listViewWheelResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject listViewWheelResult =
            listViewWheelResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(listViewWheelResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(listViewWheelResult.value(QStringLiteral("mode")).toString(),
             QStringLiteral("synthetic-qt-event"));
    // The wheel starts a flick animation; contentY moves over the following
    // frames, so wait for the semantic state instead of reading immediately.
    const auto listViewScrolledResponse = invoke(&client, QStringLiteral("UI.waitFor"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsFlickableListView\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("property"), QStringLiteral("contentY") },
            { QStringLiteral("op"), QStringLiteral(">") },
            { QStringLiteral("value"), listViewContentBefore },
        } },
        { QStringLiteral("timeoutMs"), 2000 },
    }, requestId++, &errorMessage);
    QVERIFY2(listViewScrolledResponse.has_value(), qPrintable(errorMessage));
    QVERIFY2(listViewScrolledResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("ok")).toBool(false),
             qPrintable(QStringLiteral("Expected Controls ListView wheel to increase contentY: %1")
                                .arg(QString::fromUtf8(QJsonDocument(
                                        listViewScrolledResponse->value(QStringLiteral("result"))
                                                .toObject())
                                                               .toJson(QJsonDocument::Compact)))));

    const PropertyValueResult scrollBarBeforeProperty =
            propertyValue(QStringLiteral("controlsScrollBar"), QStringLiteral("position"));
    QVERIFY2(scrollBarBeforeProperty.ok(), qPrintable(scrollBarBeforeProperty.failure));
    const double scrollBarBefore = scrollBarBeforeProperty.value.toDouble();
    const auto scrollBarDragResponse = invoke(&client, QStringLiteral("Input.dragNode"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"controlsScrollBar\"") },
        { QStringLiteral("from"), QJsonArray{ 62, 7 } },
        { QStringLiteral("to"), QJsonArray{ 138, 7 } },
        { QStringLiteral("steps"), 8 },
    }, requestId++, &errorMessage);
    QVERIFY2(scrollBarDragResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject scrollBarDragResult =
            scrollBarDragResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(scrollBarDragResult.value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(scrollBarDragResult.value(QStringLiteral("mode")).toString(),
             QStringLiteral("synthetic-qt-event"));
    const PropertyValueResult scrollBarAfterProperty =
            propertyValue(QStringLiteral("controlsScrollBar"), QStringLiteral("position"));
    QVERIFY2(scrollBarAfterProperty.ok(), qPrintable(scrollBarAfterProperty.failure));
    QVERIFY2(scrollBarAfterProperty.value.toDouble() > scrollBarBefore,
             qPrintable(QStringLiteral("Expected ScrollBar drag to increase position: before=%1 after=%2")
                                .arg(scrollBarBefore)
                                .arg(scrollBarAfterProperty.value.toDouble())));

    clickAndVerify(QStringLiteral("controlsSecondTabButton"));
    const PropertyValueResult tabBarIndex =
            propertyValue(QStringLiteral("controlsTabBar"), QStringLiteral("currentIndex"));
    QVERIFY2(tabBarIndex.ok(), qPrintable(tabBarIndex.failure));
    QCOMPARE(tabBarIndex.value.toInt(), 1);
    const PropertyValueResult swipeViewIndex =
            propertyValue(QStringLiteral("controlsSwipeView"), QStringLiteral("currentIndex"));
    QVERIFY2(swipeViewIndex.ok(), qPrintable(swipeViewIndex.failure));
    QCOMPARE(swipeViewIndex.value.toInt(), 1);

    const auto treeResponse = invoke(&client, QStringLiteral("UI.getTree"), {
        { QStringLiteral("depth"), -1 },
        { QStringLiteral("includeInvisible"), true },
        { QStringLiteral("includeSource"), true },
    }, requestId++, &errorMessage);
    QVERIFY2(treeResponse.has_value(), qPrintable(errorMessage));

    QSet<int> anonymousFillerNodeIds;
    std::function<void(const QJsonObject &)> collectAnonymousFillers = [&](const QJsonObject &node) {
        const QJsonArray bbox = node.value(QStringLiteral("bbox")).toArray();
        const bool oneDimensional =
                bbox.size() == 4
                && ((bbox.at(2).toDouble() <= 0.5 && bbox.at(3).toDouble() > 0.5)
                    || (bbox.at(3).toDouble() <= 0.5 && bbox.at(2).toDouble() > 0.5));
        const QJsonObject sourceLocation = node.value(QStringLiteral("sourceLocation")).toObject();
        if (node.value(QStringLiteral("type")).toString() == QLatin1String("QQuickItem")
            && node.value(QStringLiteral("qmlId")).toString().isEmpty()
            && node.value(QStringLiteral("objectName")).toString().isEmpty()
            && sourceLocation.value(QStringLiteral("file")).toString().endsWith(
                    QStringLiteral("ControlsSmoke.qml"))
            && sourceLocation.value(QStringLiteral("method")).toString()
                    == QLatin1String("qqmldata-direct")
            && oneDimensional) {
            anonymousFillerNodeIds.insert(node.value(QStringLiteral("nodeId")).toInt(-1));
        }

        const QJsonArray children = node.value(QStringLiteral("children")).toArray();
        for (const QJsonValue &child : children)
            collectAnonymousFillers(child.toObject());
    };
    const QJsonArray windows = treeResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("windows")).toArray();
    for (const QJsonValue &window : windows)
        collectAnonymousFillers(window.toObject().value(QStringLiteral("root")).toObject());

    const auto diagnosticsResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeTree"),
                                            {}, requestId++, &errorMessage);
    QVERIFY2(diagnosticsResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject diagnosticsResult = diagnosticsResponse->value(QStringLiteral("result")).toObject();
    const QJsonArray issues = diagnosticsResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("issues")).toArray();
    for (const QJsonValue &issueValue : issues) {
        const QJsonObject issue = issueValue.toObject();
        QVERIFY2(!anonymousFillerNodeIds.contains(issue.value(QStringLiteral("nodeId")).toInt(-1)),
                 qPrintable(QStringLiteral("Diagnostics should not report anonymous layout filler: %1")
                                    .arg(QString::fromUtf8(QJsonDocument(issue)
                                                           .toJson(QJsonDocument::Compact)))));
        const QString issueFile = issue.value(QStringLiteral("sourceLocation")).toObject()
                .value(QStringLiteral("file")).toString();
        QVERIFY2(!issueFile.startsWith(QLatin1String("qrc:/qt-project.org/imports/"))
                         && !issueFile.contains(QLatin1String("/QtQuick/Controls/")),
                 qPrintable(QStringLiteral("Default tree diagnostics should not promote Qt framework implementation state: %1")
                                    .arg(QString::fromUtf8(QJsonDocument(issue)
                                                           .toJson(QJsonDocument::Compact)))));
    }
    // This scene has no genuine layout overlap under the Basic style. An
    // earlier controlsPage/SwipeView overlap was a native-macOS-style artifact,
    // and the closed SwipeDelegate's center read as blocked_by_item only because
    // the hit test pruned its out-of-bounds descendant subtree — a false
    // positive the F-028 fix removes. Overlap promotion on real overlaps is
    // covered by the diagnostics group and by
    // referenceClientClicksOutOfBoundsUnclippedChild; here we only bound noise.
    int overlapIssueCount = 0;
    for (const QJsonValue &issueValue : issues) {
        if (issueValue.toObject().value(QStringLiteral("id")).toString()
                == QLatin1String("layout.overlap"))
            ++overlapIssueCount;
    }
    QVERIFY2(overlapIssueCount <= 3,
             qPrintable(QStringLiteral("Overlap diagnostics should stay bounded and app-actionable, got %1 issues: %2")
                                .arg(overlapIssueCount)
                                .arg(QString::fromUtf8(QJsonDocument(diagnosticsResult)
                                                       .toJson(QJsonDocument::Compact)))));

    const QJsonObject defaultSummary = diagnosticsResult.value(QStringLiteral("summary")).toObject();
    QCOMPARE(defaultSummary.value(QStringLiteral("issueScope")).toString(),
             QStringLiteral("application"));
    QVERIFY2(defaultSummary.value(QStringLiteral("suppressedFrameworkIssueCount")).toInt() > 0,
             qPrintable(QStringLiteral("Expected Controls smoke app to exercise suppressed framework diagnostics: %1")
                                .arg(QString::fromUtf8(QJsonDocument(defaultSummary)
                                                       .toJson(QJsonDocument::Compact)))));

    const QJsonObject suppressedIssue = defaultSummary
            .value(QStringLiteral("suppressedFrameworkIssues")).toArray().at(0).toObject();
    const int suppressedNodeId = suppressedIssue.value(QStringLiteral("nodeId")).toInt(-1);
    QVERIFY(suppressedNodeId > 0);

    const auto directFrameworkResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeNode"), {
        { QStringLiteral("nodeId"), suppressedNodeId },
    }, requestId++, &errorMessage);
    QVERIFY2(directFrameworkResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray directFrameworkIssues = directFrameworkResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("issues")).toArray();
    QVERIFY2(!directFrameworkIssues.isEmpty(),
             "Direct node diagnostics must remain available for Qt Quick Controls internals.");

    const auto frameworkScopeResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeTree"), {
        { QStringLiteral("includeFrameworkIssues"), true },
    }, requestId++, &errorMessage);
    QVERIFY2(frameworkScopeResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject frameworkScopeResult =
            frameworkScopeResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(frameworkScopeResult.value(QStringLiteral("summary")).toObject()
                     .value(QStringLiteral("issueScope")).toString(),
             QStringLiteral("all"));
    QCOMPARE(frameworkScopeResult.value(QStringLiteral("summary")).toObject()
                     .value(QStringLiteral("suppressedFrameworkIssueCount")).toInt(),
             0);
    bool foundSuppressedNode = false;
    const QJsonArray frameworkScopeIssues =
            frameworkScopeResult.value(QStringLiteral("issues")).toArray();
    for (const QJsonValue &issueValue : frameworkScopeIssues) {
        if (issueValue.toObject().value(QStringLiteral("nodeId")).toInt(-1) == suppressedNodeId) {
            foundSuppressedNode = true;
            break;
        }
    }
    QVERIFY2(foundSuppressedNode,
             "Framework-scope tree diagnostics must expose suppressed internal Controls issues.");
}

void QmlAgentIntegrationTest::diagnosticsReportLayoutFailures()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto response = invoke(&client, QStringLiteral("Diagnostics.analyzeTree"), {}, 1,
                                 &errorMessage);
    QVERIFY2(response.has_value(), qPrintable(errorMessage));

    const auto treeResponse = invoke(&client, QStringLiteral("UI.getTree"), {
        { QStringLiteral("depth"), -1 },
        { QStringLiteral("includeInvisible"), true },
    }, 2, &errorMessage);
    QVERIFY2(treeResponse.has_value(), qPrintable(errorMessage));

    QSet<int> repeaterNodeIds;
    std::function<void(const QJsonObject &)> collectRepeaterNodes = [&](const QJsonObject &node) {
        if (node.value(QStringLiteral("type")).toString().contains(QStringLiteral("Repeater")))
            repeaterNodeIds.insert(node.value(QStringLiteral("nodeId")).toInt(-1));
        const QJsonArray children = node.value(QStringLiteral("children")).toArray();
        for (const QJsonValue &child : children)
            collectRepeaterNodes(child.toObject());
    };
    // Debug-service object ids are assigned on demand starting at 0, so id 0
    // is a valid node, not a marker for Qt's internal root item. Collect the
    // actual window root ids to verify diagnostics never blame them.
    QSet<int> rootNodeIds;
    const QJsonArray windows = treeResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("windows")).toArray();
    for (const QJsonValue &window : windows) {
        const QJsonObject root = window.toObject().value(QStringLiteral("root")).toObject();
        rootNodeIds.insert(root.value(QStringLiteral("nodeId")).toInt(-1));
        collectRepeaterNodes(root);
    }
    QVERIFY2(!repeaterNodeIds.isEmpty(), "Expected smoke app to expose Repeater nodes.");

    const QJsonArray issues = response->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("issues")).toArray();
    const QJsonObject summary = response->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("summary")).toObject();
    QCOMPARE(summary.value(QStringLiteral("issueCount")).toInt(), issues.size());
    QCOMPARE(summary.value(QStringLiteral("ok")).toBool(), issues.isEmpty());
    QVERIFY(summary.value(QStringLiteral("ran")).toArray().contains(
            QStringLiteral("layout.viewport")));
    QSet<QString> issueIds;
    QHash<QString, QJsonObject> issueById;
    for (const QJsonValue &issueValue : issues) {
        const QJsonObject issue = issueValue.toObject();
        const QString id = issue.value(QStringLiteral("id")).toString();
        QVERIFY2(!rootNodeIds.contains(issue.value(QStringLiteral("nodeId")).toInt(-1)),
                 qPrintable(QStringLiteral("Diagnostics should not report Qt's internal root item: %1")
                                    .arg(QString::fromUtf8(QJsonDocument(issue).toJson(QJsonDocument::Compact)))));
        QVERIFY2(!repeaterNodeIds.contains(issue.value(QStringLiteral("nodeId")).toInt(-1)),
                 qPrintable(QStringLiteral("Diagnostics should not report Repeater infrastructure: %1")
                                    .arg(QString::fromUtf8(QJsonDocument(issue).toJson(QJsonDocument::Compact)))));
        issueIds.insert(id);
        issueById.insert(id, issue);
    }

    QVERIFY2(issueIds.contains(QStringLiteral("layout.child_exceeds_parent")),
             "Expected clipped child overflow diagnostic.");
    QVERIFY2(issueIds.contains(QStringLiteral("layout.outside_viewport")),
             "Expected outside viewport diagnostic.");
    QVERIFY2(issueIds.contains(QStringLiteral("layout.excessive_spacer")),
             "Expected excessive spacer diagnostic.");
    QVERIFY2(!issueById.value(QStringLiteral("layout.child_exceeds_parent"))
                      .value(QStringLiteral("blameChain")).toArray().isEmpty(),
             "Expected clipped child diagnostic to include factual blame chain.");
    QVERIFY2(!issueById.value(QStringLiteral("layout.child_exceeds_parent"))
                      .value(QStringLiteral("evidenceProfile")).toObject().isEmpty(),
             "Expected clipped child diagnostic to include evidenceProfile.");
    QVERIFY2(!issueById.value(QStringLiteral("layout.outside_viewport"))
                      .value(QStringLiteral("blameChain")).toArray().isEmpty(),
             "Expected outside viewport diagnostic to include factual blame chain.");
    QCOMPARE(issueById.value(QStringLiteral("layout.outside_viewport"))
                     .value(QStringLiteral("confidence")).toDouble(),
             0.90);
    QVERIFY2(!issueById.value(QStringLiteral("layout.outside_viewport"))
                      .value(QStringLiteral("evidenceProfile")).toObject().isEmpty(),
             "Expected outside viewport diagnostic to describe its evidence model.");

    const auto summaryResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeTree"), {
        { QStringLiteral("verbosity"), QStringLiteral("summary") },
        { QStringLiteral("maxIssues"), 1 },
    }, 3, &errorMessage);
    QVERIFY2(summaryResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject summaryResult = summaryResponse->value(QStringLiteral("result")).toObject();
    const QJsonObject compactSummary = summaryResult.value(QStringLiteral("summary")).toObject();
    QCOMPARE(compactSummary.value(QStringLiteral("verbosity")).toString(),
             QStringLiteral("summary"));
    const int summaryIssueCount = compactSummary.value(QStringLiteral("issueCount")).toInt(-1);
    const int summaryReturnedIssueCount =
            compactSummary.value(QStringLiteral("returnedIssueCount")).toInt(-1);
    QVERIFY(summaryIssueCount >= 0);
    QCOMPARE(summaryReturnedIssueCount, qMin(1, summaryIssueCount));
    QCOMPARE(compactSummary.value(QStringLiteral("moreAvailable")).toBool(),
             summaryIssueCount > summaryReturnedIssueCount);
    QVERIFY2(compactSummary.value(QStringLiteral("omittedFields")).toArray()
                     .contains(QStringLiteral("evidence")),
             qPrintable(QString::fromUtf8(QJsonDocument(summaryResult)
                                                  .toJson(QJsonDocument::Compact))));
    const QJsonArray compactIssues = summaryResult.value(QStringLiteral("issues")).toArray();
    QCOMPARE(compactIssues.size(), summaryReturnedIssueCount);
    if (!compactIssues.isEmpty()) {
        const QJsonObject compactIssue = compactIssues.at(0).toObject();
        QVERIFY2(!compactIssue.contains(QStringLiteral("evidence")),
                 qPrintable(QString::fromUtf8(QJsonDocument(compactIssue)
                                                      .toJson(QJsonDocument::Compact))));
        QVERIFY2(!compactIssue.contains(QStringLiteral("blameChain")),
                 qPrintable(QString::fromUtf8(QJsonDocument(compactIssue)
                                                      .toJson(QJsonDocument::Compact))));
    }

    const auto overlapOnlyResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeTree"), {
        { QStringLiteral("checks"), QJsonArray{ QStringLiteral("overlap") } },
    }, 4, &errorMessage);
    QVERIFY2(overlapOnlyResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject overlapOnlyResult =
            overlapOnlyResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(overlapOnlyResult.value(QStringLiteral("summary")).toObject()
                     .value(QStringLiteral("ran")).toArray(),
             QJsonArray{ QStringLiteral("layout.overlap") });
    const QJsonArray overlapOnlyIssues =
            overlapOnlyResult.value(QStringLiteral("issues")).toArray();
    for (const QJsonValue &issueValue : overlapOnlyIssues) {
        QVERIFY2(issueValue.toObject().value(QStringLiteral("id")).toString()
                         != QLatin1String("layout.outside_viewport"),
                 qPrintable(QString::fromUtf8(QJsonDocument(overlapOnlyResult)
                                                      .toJson(QJsonDocument::Compact))));
    }
}

void QmlAgentIntegrationTest::diagnosticsFixturesMatchExpectedIssues()
{
    const QString fixturesDir = qEnvironmentVariable("QMLAGENT_FIXTURES_DIR");
    QVERIFY2(!fixturesDir.isEmpty(), "Missing QMLAGENT_FIXTURES_DIR.");

    QDir fixtures(fixturesDir);
    const QStringList fixtureNames = fixtures.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                        QDir::Name);
    QVERIFY2(!fixtureNames.isEmpty(), "Expected diagnostic fixtures.");

    for (const QString &fixtureName : fixtureNames) {
        QString errorMessage;
        const QJsonObject expected = readJsonObject(
                fixtures.filePath(QStringLiteral("%1/expected.json").arg(fixtureName)),
                &errorMessage);
        QVERIFY2(!expected.isEmpty(), qPrintable(QStringLiteral("%1: %2")
                                                 .arg(fixtureName, errorMessage)));

        SmokeAppRunner fixture;
        QVERIFY2(fixture.startDiagnosticFixture(fixtureName, &errorMessage),
                 qPrintable(QStringLiteral("%1: %2").arg(fixtureName, errorMessage)));

        QQmlDebugConnection connection;
        QmlAgentDebugClient client(&connection);
        QVERIFY2(connectToQmlAgent(fixture.port(), &connection, &client, &errorMessage),
                 qPrintable(QStringLiteral("%1: %2").arg(fixtureName, errorMessage)));

        int requestId = 1;
        // Force one render pass so geometry is stable before asserting diagnostics.
        const auto renderResponse = invoke(&client, QStringLiteral("Render.captureScreenshot"),
                                           {}, requestId++, &errorMessage);
        QVERIFY2(renderResponse.has_value(), qPrintable(QStringLiteral("%1: %2")
                                                        .arg(fixtureName, errorMessage)));

        const QString command = expected.value(QStringLiteral("command")).toString();
        QVERIFY2(!command.isEmpty(), qPrintable(QStringLiteral("%1: missing command")
                                                .arg(fixtureName)));

        const auto response = invoke(&client, command,
                                     expected.value(QStringLiteral("params")).toObject(),
                                     requestId++, &errorMessage);
        QVERIFY2(response.has_value(), qPrintable(QStringLiteral("%1: %2")
                                                  .arg(fixtureName, errorMessage)));
        const QJsonObject result = response->value(QStringLiteral("result")).toObject();

        const QJsonObject expectedBinding = expected.value(QStringLiteral("expectedBinding"))
                .toObject();
        if (!expectedBinding.isEmpty()) {
            const bool expectedOk = expectedBinding.value(QStringLiteral("ok")).toBool(true);
            QVERIFY2(result.value(QStringLiteral("ok")).toBool() == expectedOk,
                     qPrintable(QStringLiteral("%1: binding ok mismatch in %2")
                                        .arg(fixtureName,
                                             QString::fromUtf8(QJsonDocument(result)
                                                                       .toJson(QJsonDocument::Compact)))));
            QCOMPARE(result.value(QStringLiteral("property")).toString(),
                     expectedBinding.value(QStringLiteral("property")).toString());
            if (expectedBinding.contains(QStringLiteral("value")))
                QCOMPARE(result.value(QStringLiteral("value")), expectedBinding.value(QStringLiteral("value")));

            const QJsonObject provenance = result.value(QStringLiteral("provenance")).toObject();
            QCOMPARE(provenance.value(QStringLiteral("kind")).toString(),
                     expectedBinding.value(QStringLiteral("provenanceKind")).toString());

            const QString bindingKind = expectedBinding.value(QStringLiteral("bindingKind")).toString();
            if (!bindingKind.isEmpty())
                QCOMPARE(provenance.value(QStringLiteral("bindingKind")).toString(), bindingKind);

            const QString expressionContains =
                    expectedBinding.value(QStringLiteral("expressionContains")).toString();
            if (!expressionContains.isEmpty()) {
                QVERIFY2(provenance.value(QStringLiteral("expression")).toString()
                                .contains(expressionContains),
                         qPrintable(QStringLiteral("%1: binding expression mismatch in %2")
                                            .arg(fixtureName,
                                                 QString::fromUtf8(QJsonDocument(result)
                                                                   .toJson(QJsonDocument::Compact)))));
            }

            const QString sourceAssignmentContains =
                    expectedBinding.value(QStringLiteral("sourceAssignmentContains")).toString();
            if (!sourceAssignmentContains.isEmpty()) {
                QVERIFY2(provenance.value(QStringLiteral("sourceAssignment")).toObject()
                                .value(QStringLiteral("expression")).toString()
                                .contains(sourceAssignmentContains),
                         qPrintable(QStringLiteral("%1: source assignment mismatch in %2")
                                            .arg(fixtureName,
                                                 QString::fromUtf8(QJsonDocument(result)
                                                                   .toJson(QJsonDocument::Compact)))));
            }

            const QString sourceMethod =
                    expectedBinding.value(QStringLiteral("sourceMethod")).toString();
            if (!sourceMethod.isEmpty())
                QCOMPARE(provenance.value(QStringLiteral("sourceLocation")).toObject()
                                 .value(QStringLiteral("method")).toString(),
                         sourceMethod);

            const QString dependencyLimitation =
                    expectedBinding.value(QStringLiteral("dependencyLimitationContains")).toString();
            if (!dependencyLimitation.isEmpty()) {
                bool foundLimitation = false;
                const QJsonArray limitations =
                        provenance.value(QStringLiteral("dependencyLimitations")).toArray();
                for (const QJsonValue &limitation : limitations) {
                    if (limitation.toString().contains(dependencyLimitation)) {
                        foundLimitation = true;
                        break;
                    }
                }
                QVERIFY2(foundLimitation,
                         qPrintable(QStringLiteral("%1: missing dependency limitation in %2")
                                            .arg(fixtureName,
                                                 QString::fromUtf8(QJsonDocument(result)
                                                                   .toJson(QJsonDocument::Compact)))));
            }

            if (expectedBinding.contains(QStringLiteral("dependencySummaryHasRuntimeDependencies"))) {
                const bool expectedDependencies =
                        expectedBinding.value(QStringLiteral("dependencySummaryHasRuntimeDependencies")).toBool();
                const QJsonObject dependencySummary =
                        provenance.value(QStringLiteral("dependencySummary")).toObject();
                QCOMPARE(dependencySummary.value(QStringLiteral("hasRuntimeDependencies")).toBool(),
                         expectedDependencies);
            }

            const QString candidateIdentifier =
                    expectedBinding.value(QStringLiteral("candidateIdentifier")).toString();
            if (!candidateIdentifier.isEmpty()) {
                bool foundCandidate = false;
                const QJsonArray candidates =
                        provenance.value(QStringLiteral("candidateIdentifiers")).toArray();
                for (const QJsonValue &candidateValue : candidates) {
                    if (candidateValue.toObject().value(QStringLiteral("name")).toString()
                            == candidateIdentifier) {
                        foundCandidate = true;
                        break;
                    }
                }
                QVERIFY2(foundCandidate,
                         qPrintable(QStringLiteral("%1: missing candidate identifier in %2")
                                            .arg(fixtureName,
                                                 QString::fromUtf8(QJsonDocument(result)
                                                                   .toJson(QJsonDocument::Compact)))));
            }

            const QJsonObject expectedDependency =
                    expectedBinding.value(QStringLiteral("dependency")).toObject();
            if (!expectedDependency.isEmpty()) {
                bool foundDependency = false;
                const QJsonArray dependencies = provenance.value(QStringLiteral("dependencies")).toArray();
                for (const QJsonValue &dependencyValue : dependencies) {
                    const QJsonObject dependency = dependencyValue.toObject();
                    const bool propertyMatches = dependency.value(QStringLiteral("property")).toString()
                            == expectedDependency.value(QStringLiteral("property")).toString();
                    const bool valueMatches = dependency.value(QStringLiteral("value"))
                            == expectedDependency.value(QStringLiteral("value"));
                    const QString expectedSelector =
                            expectedDependency.value(QStringLiteral("selector")).toString();
                    const bool selectorMatches = expectedSelector.isEmpty()
                            || dependency.value(QStringLiteral("selector")).toString() == expectedSelector;
                    if (propertyMatches && valueMatches && selectorMatches) {
                        foundDependency = true;
                        if (!expectedSelector.isEmpty()) {
                            QVERIFY2(!dependency.value(QStringLiteral("nextHints")).toArray().isEmpty(),
                                     qPrintable(QStringLiteral("%1: dependency selector has no nextHints in %2")
                                                        .arg(fixtureName,
                                                             QString::fromUtf8(QJsonDocument(result)
                                                                               .toJson(QJsonDocument::Compact)))));
                        }
                        break;
                    }
                }
                QVERIFY2(foundDependency,
                         qPrintable(QStringLiteral("%1: missing dependency in %2")
                                            .arg(fixtureName,
                                                 QString::fromUtf8(QJsonDocument(result)
                                                                   .toJson(QJsonDocument::Compact)))));
            }
            continue;
        }

        const QJsonArray issues = result.value(QStringLiteral("issues")).toArray();
        const QJsonArray expectedIssues = expected.value(QStringLiteral("expectedIssues"))
                .toArray();
        QVERIFY2(!expectedIssues.isEmpty(), qPrintable(QStringLiteral("%1: no expected issues")
                                                       .arg(fixtureName)));

        auto issueMatches = [&](const QJsonObject &issue, const QJsonObject &expectation) {
            if (issue.value(QStringLiteral("id")).toString()
                != expectation.value(QStringLiteral("id")).toString()) {
                return false;
            }

            const QString messageContains =
                    expectation.value(QStringLiteral("messageContains")).toString();
            if (!messageContains.isEmpty()
                && !issue.value(QStringLiteral("message")).toString().contains(messageContains)) {
                return false;
            }

            const QString evidenceNeedle =
                    expectation.value(QStringLiteral("evidenceContains")).toString();
            if (!evidenceNeedle.isEmpty() && !evidenceContains(issue, evidenceNeedle))
                return false;

            const QString sourceMethod =
                    expectation.value(QStringLiteral("sourceMethod")).toString();
            if (!sourceMethod.isEmpty()
                && issue.value(QStringLiteral("sourceLocation")).toObject()
                           .value(QStringLiteral("method")).toString() != sourceMethod) {
                return false;
            }

            if (expectation.value(QStringLiteral("requiresBlameChain")).toBool(false)
                && issue.value(QStringLiteral("blameChain")).toArray().isEmpty()) {
                return false;
            }

            if (expectation.value(QStringLiteral("requiresRepairHints")).toBool(false)
                && issue.value(QStringLiteral("repairHints")).toArray().isEmpty()) {
                return false;
            }

            const QString repairHintKind =
                    expectation.value(QStringLiteral("repairHintKind")).toString();
            if (!repairHintKind.isEmpty()) {
                bool foundRepairHint = false;
                const QJsonArray repairHints = issue.value(QStringLiteral("repairHints")).toArray();
                for (const QJsonValue &hintValue : repairHints) {
                    if (hintValue.toObject().value(QStringLiteral("kind")).toString()
                            == repairHintKind) {
                        foundRepairHint = true;
                        break;
                    }
                }
                if (!foundRepairHint)
                    return false;
            }

            if (expectation.value(QStringLiteral("requiresBindingProvenance")).toBool(false)
                && issue.value(QStringLiteral("bindingProvenance")).toArray().isEmpty()) {
                return false;
            }

            const QString bindingExpressionNeedle =
                    expectation.value(QStringLiteral("bindingExpressionContains")).toString();
            if (!bindingExpressionNeedle.isEmpty()) {
                bool foundBindingExpression = false;
                const QJsonArray bindingEvidence =
                        issue.value(QStringLiteral("bindingProvenance")).toArray();
                for (const QJsonValue &bindingValue : bindingEvidence) {
                    if (bindingValue.toObject().value(QStringLiteral("expression")).toString()
                            .contains(bindingExpressionNeedle)) {
                        foundBindingExpression = true;
                        break;
                    }
                }
                if (!foundBindingExpression)
                    return false;
            }

            const QString nodeObjectName =
                    expectation.value(QStringLiteral("nodeObjectName")).toString();
            if (!nodeObjectName.isEmpty()) {
                const int nodeId = issue.value(QStringLiteral("nodeId")).toInt(-1);
                if (nodeId <= 0)
                    return false;

                const auto nodeResponse = invoke(&client, QStringLiteral("UI.describeNode"),
                                                 { { QStringLiteral("nodeId"), nodeId } },
                                                 requestId++, &errorMessage);
                if (!nodeResponse.has_value())
                    return false;

                const QJsonObject node = nodeResponse->value(QStringLiteral("result"))
                        .toObject().value(QStringLiteral("node")).toObject();
                if (node.value(QStringLiteral("objectName")).toString() != nodeObjectName)
                    return false;
            }

            return true;
        };

        for (const QJsonValue &expectedIssueValue : expectedIssues) {
            const QJsonObject expectedIssue = expectedIssueValue.toObject();
            bool found = false;
            for (const QJsonValue &issueValue : issues) {
                if (issueMatches(issueValue.toObject(), expectedIssue)) {
                    found = true;
                    break;
                }
            }

            QVERIFY2(found,
                     qPrintable(QStringLiteral("%1: missing expected issue %2 in %3")
                                .arg(fixtureName,
                                     QString::fromUtf8(QJsonDocument(expectedIssue)
                                                       .toJson(QJsonDocument::Compact)),
                                     QString::fromUtf8(QJsonDocument(QJsonObject{
                                             { QStringLiteral("issues"), issues }
                                     }).toJson(QJsonDocument::Compact)))));
        }
    }
}

void QmlAgentIntegrationTest::diagnosticsAnalyzeNodeHonorsChecks()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto queryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.offscreenAfterSpacer\"") },
        { QStringLiteral("includeSource"), false },
    }, 1, &errorMessage);
    QVERIFY2(queryResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray matches = queryResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    const QJsonObject offscreenNode = matches.at(0).toObject();
    const int nodeId = offscreenNode.value(QStringLiteral("nodeId")).toInt(-1);
    QVERIFY(nodeId > 0);

    const auto actionabilityQueryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.centerOffscreen\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("actionable"),
            QStringLiteral("interactable"),
        } },
    }, 2, &errorMessage);
    QVERIFY2(actionabilityQueryResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject centerOffscreenNode = actionabilityQueryResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray().at(0).toObject();
    const int centerOffscreenNodeId = centerOffscreenNode.value(QStringLiteral("nodeId")).toInt(-1);
    QVERIFY(centerOffscreenNodeId > 0);
    QCOMPARE(centerOffscreenNode.value(QStringLiteral("actionable")).toBool(), false);
    const QJsonArray interactableReasons = centerOffscreenNode.value(QStringLiteral("interactable"))
            .toObject().value(QStringLiteral("reasons")).toArray();
    QVERIFY(!interactableReasons.isEmpty());
    QCOMPARE(interactableReasons.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("center_outside_viewport"));
    auto hasReason = [](const QJsonArray &reasons, const QString &id) {
        for (const QJsonValue &reasonValue : reasons) {
            if (reasonValue.toObject().value(QStringLiteral("id")).toString() == id)
                return true;
        }
        return false;
    };

    const auto partiallyVisibleResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.partiallyVisiblePressArea\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("objectName"),
            QStringLiteral("insideViewport"),
            QStringLiteral("viewport"),
            QStringLiteral("actionable"),
            QStringLiteral("interactable"),
        } },
    }, 3, &errorMessage);
    QVERIFY2(partiallyVisibleResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject partiallyVisibleNode = partiallyVisibleResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray().at(0).toObject();
    const int partiallyVisibleNodeId = partiallyVisibleNode.value(QStringLiteral("nodeId")).toInt(-1);
    QVERIFY(partiallyVisibleNodeId > 0);
    QCOMPARE(partiallyVisibleNode.value(QStringLiteral("insideViewport")).toBool(true), false);
    QCOMPARE(partiallyVisibleNode.value(QStringLiteral("actionable")).toBool(false), true);
    const QJsonObject partialViewport = partiallyVisibleNode.value(QStringLiteral("viewport")).toObject();
    QCOMPARE(partialViewport.value(QStringLiteral("fullyInside")).toBool(true), false);
    QCOMPARE(partialViewport.value(QStringLiteral("partiallyInside")).toBool(false), true);
    QCOMPARE(partialViewport.value(QStringLiteral("centerInside")).toBool(false), true);
    const QJsonObject partialActionability = partiallyVisibleNode.value(QStringLiteral("interactable")).toObject();
    QCOMPARE(partialActionability.value(QStringLiteral("reasons")).toArray().size(), 0);
    QCOMPARE(partialActionability.value(QStringLiteral("viewport")).toObject()
                     .value(QStringLiteral("actionPointInside")).toBool(false), true);

    const auto partiallyVisibleClickResponse = invoke(&client, QStringLiteral("Input.clickNode"), {
        { QStringLiteral("nodeId"), partiallyVisibleNodeId },
    }, 4, &errorMessage);
    QVERIFY2(partiallyVisibleClickResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject partiallyVisibleClickResult =
            partiallyVisibleClickResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(partiallyVisibleClickResult.value(QStringLiteral("delivered")).toBool(false), true);
    const QJsonObject deliveredViewport = partiallyVisibleClickResult.value(QStringLiteral("node")).toObject()
            .value(QStringLiteral("viewport")).toObject();
    QCOMPARE(deliveredViewport.value(QStringLiteral("fullyInside")).toBool(true), false);
    QCOMPARE(deliveredViewport.value(QStringLiteral("partiallyInside")).toBool(false), true);
    QCOMPARE(deliveredViewport.value(QStringLiteral("centerInside")).toBool(false), true);

    const auto longPressResponse = invoke(&client, QStringLiteral("Input.longPressNode"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.longPressArea\"") },
        { QStringLiteral("holdMs"), 900 },
    }, 5, &errorMessage);
    QVERIFY2(longPressResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject longPressResult = longPressResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(longPressResult.value(QStringLiteral("delivered")).toBool(false), true);
    QCOMPARE(longPressResult.value(QStringLiteral("releaseSent")).toBool(false), true);
    QVERIFY2(longPressResult.value(QStringLiteral("heldElapsedMs")).toInt() >= 800,
             qPrintable(QString::fromUtf8(QJsonDocument(longPressResult)
                                          .toJson(QJsonDocument::Compact))));

    const auto longPressVerifyResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.longPressed\"") },
        { QStringLiteral("includeSource"), false },
    }, 6, &errorMessage);
    QVERIFY2(longPressVerifyResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(longPressVerifyResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("matches")).toArray().size(), 1);

    const auto genericBlockedResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.genericBlockedTarget\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("objectName"),
            QStringLiteral("actionable"),
            QStringLiteral("interactable"),
        } },
    }, 7, &errorMessage);
    QVERIFY2(genericBlockedResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject genericBlockedNode = genericBlockedResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray().at(0).toObject();
    const int genericBlockedNodeId = genericBlockedNode.value(QStringLiteral("nodeId")).toInt(-1);
    QVERIFY(genericBlockedNodeId > 0);
    QCOMPARE(genericBlockedNode.value(QStringLiteral("actionable")).toBool(true), false);
    QVERIFY2(hasReason(genericBlockedNode.value(QStringLiteral("interactable")).toObject()
                               .value(QStringLiteral("reasons")).toArray(),
                       QStringLiteral("blocked_by_item")),
             qPrintable(QStringLiteral("Expected generic blocker evidence: %1")
                                .arg(QString::fromUtf8(QJsonDocument(genericBlockedNode)
                                                       .toJson(QJsonDocument::Compact)))));

    // Contrast pair: a label under its own control's anchors.fill MouseArea is
    // NOT occluded — the enclosing surface belongs to a container that owns
    // the label, so the click must go through and reach the control.
    const auto labelledControlQueryResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("text=\"Press me\"") },
        { QStringLiteral("includeSource"), false },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("actionable"),
        } },
    }, 71, &errorMessage);
    QVERIFY2(labelledControlQueryResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray labelledControlMatches = labelledControlQueryResponse
            ->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(labelledControlMatches.size(), 1);
    QCOMPARE(labelledControlMatches.at(0).toObject()
                     .value(QStringLiteral("actionable")).toBool(false), true);
    const auto labelledControlClickResponse = invoke(&client, QStringLiteral("Input.clickNode"), {
        { QStringLiteral("selector"), QStringLiteral("text=\"Press me\"") },
    }, 72, &errorMessage);
    QVERIFY2(labelledControlClickResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(labelledControlClickResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("delivered")).toBool(false), true);
    const auto labelledControlVerifyResponse = invoke(&client, QStringLiteral("UI.waitFor"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.labelledControlClicked\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("state"), QStringLiteral("found") },
        } },
        { QStringLiteral("timeoutMs"), 1000 },
    }, 73, &errorMessage);
    QVERIFY2(labelledControlVerifyResponse.has_value(), qPrintable(errorMessage));
    QVERIFY2(labelledControlVerifyResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("ok")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(
                     labelledControlVerifyResponse->value(QStringLiteral("result")).toObject())
                                                  .toJson(QJsonDocument::Compact))));

    const auto minSizeResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeNode"), {
        { QStringLiteral("nodeId"), nodeId },
        { QStringLiteral("checks"), QJsonArray{ QStringLiteral("minSize") } },
    }, 8, &errorMessage);
    QVERIFY2(minSizeResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(minSizeResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("issues")).toArray().size(), 0);

    const auto viewportResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeNode"), {
        { QStringLiteral("nodeId"), nodeId },
        { QStringLiteral("checks"), QJsonArray{ QStringLiteral("insideViewport") } },
    }, 9, &errorMessage);
    QVERIFY2(viewportResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray issues = viewportResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("issues")).toArray();
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("layout.outside_viewport"));

    const auto actionableResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeNode"), {
        { QStringLiteral("nodeId"), centerOffscreenNodeId },
        { QStringLiteral("checks"), QJsonArray{ QStringLiteral("actionable") } },
    }, 10, &errorMessage);
    QVERIFY2(actionableResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray actionableIssues = actionableResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("issues")).toArray();
    QCOMPARE(actionableIssues.size(), 1);
    QCOMPARE(actionableIssues.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_actionable"));
    QCOMPARE(actionableIssues.at(0).toObject().value(QStringLiteral("actionability"))
                     .toObject().value(QStringLiteral("reasons")).toArray()
                     .at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("center_outside_viewport"));

    const auto genericActionableResponse = invoke(&client, QStringLiteral("Diagnostics.analyzeNode"), {
        { QStringLiteral("nodeId"), genericBlockedNodeId },
        { QStringLiteral("checks"), QJsonArray{ QStringLiteral("actionable") } },
    }, 11, &errorMessage);
    QVERIFY2(genericActionableResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray genericActionableIssues = genericActionableResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("issues")).toArray();
    QCOMPARE(genericActionableIssues.size(), 1);
    QCOMPARE(genericActionableIssues.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_actionable"));
    QVERIFY2(hasReason(genericActionableIssues.at(0).toObject()
                               .value(QStringLiteral("actionability")).toObject()
                               .value(QStringLiteral("reasons")).toArray(),
                       QStringLiteral("blocked_by_item")),
             qPrintable(QStringLiteral("Expected generic blocked_by_item diagnostics: %1")
                                .arg(QString::fromUtf8(QJsonDocument(genericActionableIssues.at(0).toObject())
                                                       .toJson(QJsonDocument::Compact)))));

    const auto genericClickResponse = invoke(&client, QStringLiteral("Input.clickNode"), {
        { QStringLiteral("nodeId"), genericBlockedNodeId },
    }, 12, &errorMessage);
    QVERIFY2(genericClickResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject genericClickResult =
            genericClickResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(genericClickResult.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(genericClickResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("blocked_by_item"));

    const auto genericWheelResponse = invoke(&client, QStringLiteral("Input.wheel"), {
        { QStringLiteral("nodeId"), genericBlockedNodeId },
        { QStringLiteral("deltaY"), -120 },
    }, 13, &errorMessage);
    QVERIFY2(genericWheelResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject genericWheelResult =
            genericWheelResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(genericWheelResult.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(genericWheelResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("blocked_by_item"));

    const auto genericMouseResponse = invoke(&client, QStringLiteral("Input.dispatchMouseEvent"), {
        { QStringLiteral("nodeId"), genericBlockedNodeId },
        { QStringLiteral("type"), QStringLiteral("mousePress") },
    }, 14, &errorMessage);
    QVERIFY2(genericMouseResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject genericMouseResult =
            genericMouseResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(genericMouseResult.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(genericMouseResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("blocked_by_item"));

    const auto genericDragResponse = invoke(&client, QStringLiteral("Input.dragNode"), {
        { QStringLiteral("nodeId"), genericBlockedNodeId },
        { QStringLiteral("delta"), QJsonArray{ 1, 0 } },
    }, 15, &errorMessage);
    QVERIFY2(genericDragResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject genericDragResult =
            genericDragResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(genericDragResult.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(genericDragResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("blocked_by_item"));

    const auto genericTouchResponse = invoke(&client, QStringLiteral("Input.dispatchTouchEvent"), {
        { QStringLiteral("nodeId"), genericBlockedNodeId },
        { QStringLiteral("type"), QStringLiteral("touchBegin") },
        { QStringLiteral("points"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), 0 },
            { QStringLiteral("point"), QJsonArray{ 16, 12 } },
        } } },
    }, 16, &errorMessage);
    QVERIFY2(genericTouchResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject genericTouchResult =
            genericTouchResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(genericTouchResult.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(genericTouchResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("blocked_by_item"));

    const auto genericFocusResponse = invoke(&client, QStringLiteral("Input.focusNode"), {
        { QStringLiteral("nodeId"), genericBlockedNodeId },
    }, 17, &errorMessage);
    QVERIFY2(genericFocusResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject genericFocusResult =
            genericFocusResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(genericFocusResult.value(QStringLiteral("focused")).toBool(true), false);
    QCOMPARE(genericFocusResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("blocked_by_item"));
}

void QmlAgentIntegrationTest::renderCaptureScreenshot()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto mainWindowQuery = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.content\"") },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("windowId"),
            QStringLiteral("objectName"),
        } },
    }, 1, &errorMessage);
    QVERIFY2(mainWindowQuery.has_value(), qPrintable(errorMessage));
    const int mainWindowId = mainWindowQuery->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray().at(0).toObject()
            .value(QStringLiteral("windowId")).toInt(-1);
    QVERIFY(mainWindowId > 0);

    // Default call carries metadata only: base64 bytes must be requested
    // explicitly with includeData:true so raw protocol users cannot blow
    // image bytes into an agent context by accident.
    const auto response = invoke(&client, QStringLiteral("Render.captureScreenshot"), {
        { QStringLiteral("windowId"), mainWindowId },
    }, 2,
                                 &errorMessage);
    QVERIFY2(response.has_value(), qPrintable(errorMessage));

    const QJsonObject result = response->value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("captured")).toBool(), true);
    QCOMPARE(result.value(QStringLiteral("format")).toString(), QStringLiteral("png"));
    QCOMPARE(result.value(QStringLiteral("encoding")).toString(), QStringLiteral("base64"));
    QCOMPARE(result.value(QStringLiteral("width")).toInt(), 320);
    QCOMPARE(result.value(QStringLiteral("height")).toInt(), 240);
    QCOMPARE(result.value(QStringLiteral("evidenceRole")).toString(),
             QStringLiteral("fallback-visual"));
    QCOMPARE(result.value(QStringLiteral("primaryOracle")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("structuredFirst")).toBool(false), true);
    QCOMPARE(result.value(QStringLiteral("dataOmitted")).toBool(false), true);
    QVERIFY2(!result.contains(QStringLiteral("data")),
             qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));

    const auto includeDataResponse = invoke(&client, QStringLiteral("Render.captureScreenshot"), {
        { QStringLiteral("windowId"), mainWindowId },
        { QStringLiteral("includeData"), true },
    }, 12, &errorMessage);
    QVERIFY2(includeDataResponse.has_value(), qPrintable(errorMessage));
    const QByteArray png = QByteArray::fromBase64(
            includeDataResponse->value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("data")).toString().toLatin1());
    QVERIFY2(png.startsWith(QByteArray::fromHex("89504e470d0a1a0a")), png.constData());

    const auto metadataResponse = invoke(&client, QStringLiteral("Render.captureScreenshot"), {
        { QStringLiteral("includeData"), false },
        { QStringLiteral("windowId"), mainWindowId },
    }, 3, &errorMessage);
    QVERIFY2(metadataResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject metadataResult = metadataResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(metadataResult.value(QStringLiteral("captured")).toBool(), true);
    QCOMPARE(metadataResult.value(QStringLiteral("dataOmitted")).toBool(), true);
    QVERIFY2(metadataResult.value(QStringLiteral("byteSize")).toInt() > 0,
             qPrintable(QString::fromUtf8(QJsonDocument(metadataResult).toJson())));
    QVERIFY2(!metadataResult.contains(QStringLiteral("data")),
             qPrintable(QString::fromUtf8(QJsonDocument(metadataResult).toJson())));
    QCOMPARE(metadataResult.value(QStringLiteral("evidenceRole")).toString(),
             QStringLiteral("fallback-visual"));

    const auto scaledRegionResponse = invoke(&client, QStringLiteral("Render.captureScreenshot"), {
        { QStringLiteral("windowId"), mainWindowId },
        { QStringLiteral("scale"), 0.5 },
        { QStringLiteral("region"), QJsonObject{
            { QStringLiteral("x"), 0 },
            { QStringLiteral("y"), 0 },
            { QStringLiteral("width"), 160 },
            { QStringLiteral("height"), 120 },
        } },
    }, 4, &errorMessage);
    QVERIFY2(scaledRegionResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject scaledRegionResult =
            scaledRegionResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(scaledRegionResult.value(QStringLiteral("captured")).toBool(), true);
    QCOMPARE(scaledRegionResult.value(QStringLiteral("scale")).toDouble(), 0.5);
    QCOMPARE(scaledRegionResult.value(QStringLiteral("region")).toObject()
                     .value(QStringLiteral("width")).toInt(), 160);
    QVERIFY2(scaledRegionResult.value(QStringLiteral("width")).toInt()
                    < result.value(QStringLiteral("width")).toInt(),
             qPrintable(QString::fromUtf8(QJsonDocument(scaledRegionResult).toJson())));
    QVERIFY2(scaledRegionResult.value(QStringLiteral("height")).toInt()
                    < result.value(QStringLiteral("height")).toInt(),
             qPrintable(QString::fromUtf8(QJsonDocument(scaledRegionResult).toJson())));
    QVERIFY2(scaledRegionResult.value(QStringLiteral("byteSize")).toInt()
                    < metadataResult.value(QStringLiteral("byteSize")).toInt(),
             qPrintable(QString::fromUtf8(QJsonDocument(scaledRegionResult).toJson())));
}

void QmlAgentIntegrationTest::logGetEntriesSupportsCursor()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto firstResponse = invoke(&client, QStringLiteral("Log.getEntries"), {}, 1,
                                      &errorMessage);
    QVERIFY2(firstResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject firstResult = firstResponse->value(QStringLiteral("result")).toObject();
    QVERIFY2(firstResult.value(QStringLiteral("entryCount")).toInt() > 0,
             qPrintable(QString::fromUtf8(QJsonDocument(firstResult).toJson())));
    const double cursor = firstResult.value(QStringLiteral("nextSinceTimestamp")).toDouble(-1.0);
    QVERIFY2(cursor > 0.0, qPrintable(QString::fromUtf8(QJsonDocument(firstResult).toJson())));

    const auto cursorResponse = invoke(&client, QStringLiteral("Log.getEntries"), {
        { QStringLiteral("sinceTimestamp"), cursor },
    }, 2, &errorMessage);
    QVERIFY2(cursorResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject cursorResult = cursorResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(cursorResult.value(QStringLiteral("entryCount")).toInt(), 0);
    QVERIFY2(cursorResult.value(QStringLiteral("skippedBeforeCursor")).toInt() > 0,
             qPrintable(QString::fromUtf8(QJsonDocument(cursorResult).toJson())));

    const auto truncatedResponse = invoke(&client, QStringLiteral("Log.getEntries"), {
        { QStringLiteral("maxEntries"), 0 },
    }, 3, &errorMessage);
    QVERIFY2(truncatedResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject truncatedResult = truncatedResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(truncatedResult.value(QStringLiteral("entryCount")).toInt(), 0);
    QCOMPARE(truncatedResult.value(QStringLiteral("truncated")).toBool(), true);
    QVERIFY2(truncatedResult.value(QStringLiteral("omittedEntryCount")).toInt() > 0,
             qPrintable(QString::fromUtf8(QJsonDocument(truncatedResult).toJson())));
}

void QmlAgentIntegrationTest::selectorNotFoundRanksStableCandidates()
{
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    const auto response = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.childOverflo\"") },
        { QStringLiteral("includeSource"), false },
    }, 1, &errorMessage);
    QVERIFY2(response.has_value(), qPrintable(errorMessage));

    const QJsonObject result = response->value(QStringLiteral("result")).toObject();
    QVERIFY(result.value(QStringLiteral("matches")).toArray().isEmpty());
    const QJsonArray diagnostics = result.value(QStringLiteral("diagnostics")).toArray();
    QCOMPARE(diagnostics.size(), 1);

    const QJsonArray candidates = diagnostics.at(0).toObject()
            .value(QStringLiteral("candidateSelectors")).toArray();
    QVERIFY(!candidates.isEmpty());
    QCOMPARE(candidates.at(0).toObject().value(QStringLiteral("selector")).toString(),
             QStringLiteral("objectName=\"smoke.childOverflow\""));

    const auto invalidSelectorResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"content\" and text=\"Save\"") },
        { QStringLiteral("includeSource"), false },
    }, 8, &errorMessage);
    QVERIFY2(invalidSelectorResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject invalidResult = invalidSelectorResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(invalidResult.value(QStringLiteral("matches")).toArray().size(), 0);
    const QJsonObject invalidDiagnostic = invalidResult.value(QStringLiteral("diagnostics"))
            .toArray().at(0).toObject();
    QCOMPARE(invalidDiagnostic.value(QStringLiteral("id")).toString(),
             QStringLiteral("selector.invalid"));
    QCOMPARE(invalidDiagnostic.value(QStringLiteral("selector")).toString(),
             QStringLiteral("id=\"content\" and text=\"Save\""));
    QVERIFY2(invalidDiagnostic.value(QStringLiteral("supportedForms")).toArray().contains(
                     QStringLiteral("id=\"delegateRoot\" index=4")),
             qPrintable(QString::fromUtf8(QJsonDocument(invalidDiagnostic).toJson())));
    QVERIFY2(invalidDiagnostic.value(QStringLiteral("limitations")).toArray().at(0).toString()
                     .contains(QStringLiteral("Compound predicates")),
             qPrintable(QString::fromUtf8(QJsonDocument(invalidDiagnostic).toJson())));

    const auto exactResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.childOverflow\"") },
        { QStringLiteral("includeSource"), true },
    }, 2, &errorMessage);
    QVERIFY2(exactResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray exactMatches = exactResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(exactMatches.size(), 1);
    const QJsonObject exactNode = exactMatches.at(0).toObject();
    const int exactNodeId = exactNode.value(QStringLiteral("nodeId")).toInt(-1);
    QVERIFY(exactNodeId > 0);

    const auto idResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"content\"") },
        { QStringLiteral("includeSource"), false },
    }, 9, &errorMessage);
    QVERIFY2(idResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray idMatches = idResponse->value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("matches")).toArray();
    QCOMPARE(idMatches.size(), 1);
    QCOMPARE(idMatches.at(0).toObject().value(QStringLiteral("qmlId")).toString(),
             QStringLiteral("content"));
    QCOMPARE(selectorValue(idMatches.at(0).toObject(), QStringLiteral("id")),
             QStringLiteral("content"));

    const QString visualPath = selectorValue(exactNode, QStringLiteral("visualPath"));
    QVERIFY2(!visualPath.isEmpty(), "Expected a visualPath selector.");
    const auto visualPathResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("visualPath=\"%1\"").arg(visualPath) },
        { QStringLiteral("includeSource"), false },
    }, 3, &errorMessage);
    QVERIFY2(visualPathResponse.has_value(), qPrintable(errorMessage));
    const QJsonArray visualPathMatches = visualPathResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray();
    QCOMPARE(visualPathMatches.size(), 1);
    QCOMPARE(visualPathMatches.at(0).toObject().value(QStringLiteral("nodeId")).toInt(-1),
             exactNodeId);

    const QString sourceLocation = selectorValue(exactNode, QStringLiteral("sourceLocation"));
    QVERIFY2(!sourceLocation.isEmpty(), "Expected a sourceLocation selector.");
    const auto sourceLocationResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("sourceLocation=\"%1\"").arg(sourceLocation) },
        { QStringLiteral("includeSource"), true },
    }, 4, &errorMessage);
    QVERIFY2(sourceLocationResponse.has_value(), qPrintable(errorMessage));

    bool foundSourceMatch = false;
    const QJsonArray sourceLocationMatches = sourceLocationResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("matches")).toArray();
    for (const QJsonValue &match : sourceLocationMatches) {
        if (match.toObject().value(QStringLiteral("nodeId")).toInt(-1) == exactNodeId)
            foundSourceMatch = true;
    }
    QVERIFY2(foundSourceMatch, "Expected sourceLocation query to include the original node.");

    const QString nearbySourceLocation = sourceSelectorWithLineDelta(sourceLocation, 1);
    QVERIFY2(!nearbySourceLocation.isEmpty(), "Expected a line-bearing sourceLocation selector.");
    const auto nearbySourceResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("sourceLocation=\"%1\"").arg(nearbySourceLocation) },
        { QStringLiteral("includeSource"), true },
    }, 5, &errorMessage);
    QVERIFY2(nearbySourceResponse.has_value(), qPrintable(errorMessage));

    const QJsonArray nearbyDiagnostics = nearbySourceResponse->value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("diagnostics")).toArray();
    QCOMPARE(nearbyDiagnostics.size(), 1);
    const QJsonArray sourceCandidates = nearbyDiagnostics.at(0).toObject()
            .value(QStringLiteral("candidateSelectors")).toArray();
    QVERIFY(!sourceCandidates.isEmpty());
    QCOMPARE(sourceCandidates.at(0).toObject().value(QStringLiteral("selector")).toString(),
             QStringLiteral("sourceLocation=\"%1\"").arg(sourceLocation));
}

void QmlAgentIntegrationTest::referenceClientConvenienceCommands()
{
    const QString qmlagentClient = qmlagentCtlExecutable();
    QVERIFY2(!qmlagentClient.isEmpty(), "Missing qmlagentctl test binary path.");
    const QString smokeApp = qEnvironmentVariable("QMLAGENT_SMOKE_APP");
    QVERIFY2(!smokeApp.isEmpty(), "Missing QMLAGENT_SMOKE_APP.");

    QString errorMessage;
    SmokeAppRunner launcher;
    QVERIFY2(launcher.startLauncherApp(smokeApp, &errorMessage), qPrintable(errorMessage));

    auto runClient = [&](const QStringList &arguments) -> QByteArray {
        QProcess client;
        client.setProcessChannelMode(QProcess::MergedChannels);
        client.start(qmlagentClient, arguments);
        if (!client.waitForStarted())
            return QByteArrayLiteral("CLIENT_START_FAILED: ") + client.errorString().toUtf8();
        if (!client.waitForFinished(RequestTimeoutMs))
            return QByteArrayLiteral("CLIENT_TIMEOUT: ") + client.errorString().toUtf8();
        if (client.exitStatus() != QProcess::NormalExit || client.exitCode() != 0) {
            return QByteArrayLiteral("CLIENT_FAILED: ")
                    + QByteArray::number(client.exitCode()) + '\n'
                    + client.readAllStandardOutput();
        }
        return client.readAllStandardOutput();
    };

    const QByteArray callHelpOutput = runClient({
        QStringLiteral("call"),
        QStringLiteral("--help"),
    });
    QVERIFY2(callHelpOutput.contains("QmlAgent protocol methods"),
             callHelpOutput.constData());
    QVERIFY2(callHelpOutput.contains("UI.query"), callHelpOutput.constData());
    QVERIFY2(callHelpOutput.contains("Diagnostics.analyzeBinding"),
             callHelpOutput.constData());
    QVERIFY2(callHelpOutput.contains("Render.captureScreenshot"),
             callHelpOutput.constData());

    const QByteArray statusOutput = runClient({
        QStringLiteral("status"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(statusOutput.contains("\"sessionType\":\"application\""), statusOutput.constData());
    QVERIFY2(statusOutput.contains("\"reloadPreviewSupported\":false"), statusOutput.constData());
    QVERIFY2(statusOutput.contains("\"capabilities\":{"), statusOutput.constData());
    QVERIFY2(statusOutput.contains("\"qtQuick3D\":{"), statusOutput.constData());
    QVERIFY2(statusOutput.contains("\"targetSessionInfo\":{"), statusOutput.constData());

    const QByteArray inspectOutput = runClient({
        QStringLiteral("inspect"),
        QStringLiteral("objectName=\"smoke.clickArea\""),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(inspectOutput.contains("\"matches\":["), inspectOutput.constData());
    QVERIFY2(inspectOutput.contains("smoke.clickArea"), inspectOutput.constData());

    const QByteArray treeSummaryOutput = runClient({
        QStringLiteral("call"),
        QStringLiteral("UI.getTree"),
        QStringLiteral("--params"), QStringLiteral("{\"depth\":1,\"includeSource\":true}"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(treeSummaryOutput.contains("\"windows\""), treeSummaryOutput.constData());
    QVERIFY2(treeSummaryOutput.contains("QQuickRootItem"), treeSummaryOutput.constData());

    const QByteArray screenshotSummaryOutput = runClient({
        QStringLiteral("screenshot"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(screenshotSummaryOutput.contains("\"captured\":true"),
             screenshotSummaryOutput.constData());
    QVERIFY2(screenshotSummaryOutput.contains("\"format\":\"png\""),
             screenshotSummaryOutput.constData());
    QVERIFY2(screenshotSummaryOutput.contains("\"dataOmitted\":true"),
             screenshotSummaryOutput.constData());
    QVERIFY2(!screenshotSummaryOutput.contains("\"data\":\""),
             screenshotSummaryOutput.constData());

    QTemporaryDir screenshotDir;
    QVERIFY2(screenshotDir.isValid(), qPrintable(screenshotDir.errorString()));
    const QString screenshotPath = screenshotDir.filePath(QStringLiteral("fallback-visual.png"));
    const QByteArray screenshotFileOutput = runClient({
        QStringLiteral("screenshot"),
        QStringLiteral("--out"), screenshotPath,
        QStringLiteral("--scale"), QStringLiteral("0.5"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(screenshotFileOutput.contains("\"captured\":true"),
             screenshotFileOutput.constData());
    QVERIFY2(screenshotFileOutput.contains("\"writtenTo\":"),
             screenshotFileOutput.constData());
    QVERIFY2(screenshotFileOutput.contains("\"bytesWritten\":"),
             screenshotFileOutput.constData());
    QVERIFY2(screenshotFileOutput.contains("\"scale\":0.5"),
             screenshotFileOutput.constData());
    QVERIFY2(!screenshotFileOutput.contains("\"data\":\""),
             screenshotFileOutput.constData());
    QFile screenshotFile(screenshotPath);
    QVERIFY2(screenshotFile.open(QIODevice::ReadOnly), qPrintable(screenshotFile.errorString()));
    const QByteArray pngHeader = screenshotFile.read(8);
    QVERIFY2(pngHeader == QByteArray::fromHex("89504e470d0a1a0a"), pngHeader.constData());

    const QByteArray diagnoseOutput = runClient({
        QStringLiteral("call"),
        QStringLiteral("Diagnostics.analyzeNode"),
        QStringLiteral("--params"), QStringLiteral("{\"selector\":\"objectName=\\\"smoke.offscreenAfterSpacer\\\"\"}"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(diagnoseOutput.contains("layout.outside_viewport"), diagnoseOutput.constData());

    const QByteArray bindingOutput = runClient({
        QStringLiteral("binding"),
        QStringLiteral("id=\"bindingProbe\""),
        QStringLiteral("--property"), QStringLiteral("x"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(bindingOutput.contains("\"provenance\""), bindingOutput.constData());
    QVERIFY2(bindingOutput.contains("root.bindingBase + 3"), bindingOutput.constData());

    const QByteArray repairEvidenceOutput = runClient({
        QStringLiteral("call"),
        QStringLiteral("UI.query"),
        QStringLiteral("--params"), QStringLiteral("{\"selector\":\"objectName=\\\"smoke.offscreenAfterSpacer\\\"\",\"includeSource\":true}"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(repairEvidenceOutput.contains("\"sourceLocation\""), repairEvidenceOutput.constData());
    QVERIFY2(repairEvidenceOutput.contains("smoke.offscreenAfterSpacer"),
             repairEvidenceOutput.constData());

    const QByteArray clickOutput = runClient({
        QStringLiteral("click"),
        QStringLiteral("objectName=\"smoke.clickArea\""),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(clickOutput.contains("\"delivered\":true"), clickOutput.constData());

    const QByteArray focusOutput = runClient({
        QStringLiteral("call"),
        QStringLiteral("Input.focusNode"),
        QStringLiteral("--params"), QStringLiteral("{\"selector\":\"objectName=\\\"smoke.textInput\\\"\"}"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(focusOutput.contains("\"focused\":true"), focusOutput.constData());

    const QByteArray keyOutput = runClient({
        QStringLiteral("call"),
        QStringLiteral("Input.dispatchKeyEvent"),
        QStringLiteral("--params"), QStringLiteral("{\"selector\":\"objectName=\\\"smoke.textInput\\\"\",\"key\":\"z\"}"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(keyOutput.contains("\"delivered\":true"), keyOutput.constData());

    const QByteArray workflowKeyOutput = runClient({
        QStringLiteral("call"),
        QStringLiteral("Input.dispatchKeyEvent"),
        QStringLiteral("--params"), QStringLiteral("{\"selector\":\"objectName=\\\"smoke.textInput\\\"\",\"key\":\"y\"}"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(workflowKeyOutput.contains("\"delivered\":true"), workflowKeyOutput.constData());

    const QByteArray clearTextOutput = runClient({
        QStringLiteral("clear-text"),
        QStringLiteral("objectName=\"smoke.textInput\""),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(clearTextOutput.contains("\"delivered\":true"), clearTextOutput.constData());
    QVERIFY2(clearTextOutput.contains("\"selectionApplied\":true"),
             clearTextOutput.constData());

    const QByteArray clearVerifyOutput = runClient({
        QStringLiteral("query"),
        QStringLiteral("objectName=\"smoke.textInput\""),
        QStringLiteral("--property"), QStringLiteral("text"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(clearVerifyOutput.contains("\"text\":\"\""),
             clearVerifyOutput.constData());

    const QByteArray typeTextOutput = runClient({
        QStringLiteral("type"),
        QStringLiteral("objectName=\"smoke.textInput\""),
        QStringLiteral("--text"), QStringLiteral("client"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(typeTextOutput.contains("\"delivered\":true"), typeTextOutput.constData());

    const QByteArray typeVerifyOutput = runClient({
        QStringLiteral("query"),
        QStringLiteral("objectName=\"smoke.textInput\""),
        QStringLiteral("--property"), QStringLiteral("text"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(typeVerifyOutput.contains("\"text\":\"client\""),
             typeVerifyOutput.constData());

    const QByteArray verifyOutput = runClient({
        QStringLiteral("query"),
        QStringLiteral("objectName=\"smoke.clicked\""),
        QStringLiteral("--property"), QStringLiteral("visible"),
        QStringLiteral("--format"), QStringLiteral("compact"),
    });
    QVERIFY2(verifyOutput.contains("\"visible\":true"), verifyOutput.constData());
}

static QJsonObject mcpRequest(int id, const QString &method, const QJsonObject &params = {})
{
    QJsonObject request{
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("id"), id },
        { QStringLiteral("method"), method },
    };
    if (!params.isEmpty())
        request.insert(QStringLiteral("params"), params);
    return request;
}

static QByteArray compactObject(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

static QJsonObject mcpToolCall(int id, const QString &name, const QJsonObject &arguments)
{
    return mcpRequest(id, QStringLiteral("tools/call"), {
        { QStringLiteral("name"), name },
        { QStringLiteral("arguments"), arguments },
    });
}

static QHash<int, QJsonObject> parseMcpResponses(const QByteArray &output,
                                                 QJsonArray *notifications = nullptr)
{
    QHash<int, QJsonObject> responses;
    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray &line : lines) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line.trimmed(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            continue;
        const QJsonObject object = document.object();
        if (object.contains(QStringLiteral("id")))
            responses.insert(object.value(QStringLiteral("id")).toInt(-1), object);
        else if (notifications
                 && object.value(QStringLiteral("method")).toString()
                         == QLatin1String("notifications/message")) {
            notifications->append(object);
        }
    }
    return responses;
}

void QmlAgentIntegrationTest::referenceClientMcpRoutesThroughLauncher()
{
    const QString qmlagentMcp = qmlagentMcpExecutable();
    QVERIFY2(!qmlagentMcp.isEmpty(), "Missing qmlagent-mcp test binary path.");

    QTemporaryDir previewDir;
    QVERIFY2(previewDir.isValid(), qPrintable(previewDir.errorString()));
    const QString qmlFile = QDir(previewDir.path()).filePath(QStringLiteral("Main.qml"));
    QFile file(qmlFile);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Text), qPrintable(file.errorString()));
    file.write(R"(
import QtQuick

Window {
    width: 160
    height: 120
    visible: true

    Rectangle {
        id: previewBackground
        anchors.fill: parent
        color: "#336699"
    }
}
)");
    file.close();

    QString errorMessage;
    SmokeAppRunner launcher;
    QTemporaryDir launcherWorkingDir;
    QVERIFY2(launcherWorkingDir.isValid(), qPrintable(launcherWorkingDir.errorString()));
    QVERIFY2(launcher.startLauncherPreview(qmlFile, &errorMessage, launcherWorkingDir.path()),
             qPrintable(errorMessage));

    QProcess client;
    client.setProcessChannelMode(QProcess::MergedChannels);
    client.start(qmlagentMcp, {
        QStringLiteral("--timeout"), QString::number(RequestTimeoutMs),
    });
    QVERIFY2(client.waitForStarted(), qPrintable(client.errorString()));

    auto writeRequest = [&](const QJsonObject &request) {
        client.write(compactObject(request));
        client.write("\n");
        QVERIFY2(client.waitForBytesWritten(RequestTimeoutMs), qPrintable(client.errorString()));
    };

    writeRequest(mcpRequest(1, QStringLiteral("initialize")));
    writeRequest(mcpToolCall(2, QStringLiteral("qmlagent_target_status"), {}));
    writeRequest(mcpToolCall(3, QStringLiteral("qmlagent_ui_query"), {
        { QStringLiteral("selector"), QStringLiteral("type=\"Rectangle\"") },
    }));
    QByteArray output = waitForOutput(&client, QByteArrayLiteral("\"id\":3"));
    writeRequest(mcpToolCall(4, QStringLiteral("qmlagent_preview_reload"), {
        { QStringLiteral("timeoutMs"), RequestTimeoutMs },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":4"));
    writeRequest(mcpToolCall(5, QStringLiteral("qmlagent_ui_subscribe"), {}));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":5"));
    writeRequest(mcpRequest(6, QStringLiteral("shutdown")));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":6"));
    writeRequest({
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("method"), QStringLiteral("notifications/exit") },
    });
    if (!client.waitForFinished(ProcessShutdownTimeoutMs)) {
        client.kill();
        client.waitForFinished();
    }

    const QHash<int, QJsonObject> responses = parseMcpResponses(output);
    const QJsonObject status = responses.value(2).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(status.value(QStringLiteral("connected")).toBool(), false);
    const QJsonObject gateway = status.value(QStringLiteral("launcherGateway")).toObject();
    QCOMPARE(gateway.value(QStringLiteral("available")).toBool(), true);
    QVERIFY2(status.value(QStringLiteral("capabilities")).toObject()
                    .contains(QStringLiteral("qtQuick3D")),
             output.constData());
    QVERIFY2(status.value(QStringLiteral("targetSessionInfo")).toObject()
                    .value(QStringLiteral("capabilities")).toObject()
                    .contains(QStringLiteral("qtQuick3D")),
             output.constData());
    QVERIFY2(status.value(QStringLiteral("nextStep")).toString()
                     .contains(QStringLiteral("Call target-backed request/response tools")),
             output.constData());

    const QJsonObject query = responses.value(3).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QVERIFY2(query.value(QStringLiteral("matchCount")).toInt() > 0, output.constData());
    QCOMPARE(query.value(QStringLiteral("moreAvailable")).toBool(), true);
    QVERIFY2(query.value(QStringLiteral("omittedFields")).toArray()
                     .contains(QStringLiteral("children")),
             output.constData());

    const QJsonObject reload = responses.value(4).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(reload.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(reload.value(QStringLiteral("loader")).toString(), QStringLiteral("QQmlApplicationEngine"));
    // Reload re-creates the window but restores the previous geometry.
    QCOMPARE(reload.value(QStringLiteral("windowPreserved")).toBool(), true);

    const QJsonObject subscribeResult = responses.value(5).value(QStringLiteral("result")).toObject();
    QCOMPARE(subscribeResult.value(QStringLiteral("isError")).toBool(), true);
    QVERIFY2(subscribeResult.value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("error")).toString()
                     .contains(QStringLiteral("requires a direct persistent MCP attach")),
             output.constData());
}

void QmlAgentIntegrationTest::referenceClientPreviewReloadRefreshesSingleton()
{
    const QString qmlagentMcp = qmlagentMcpExecutable();
    QVERIFY2(!qmlagentMcp.isEmpty(), "Missing qmlagent-mcp test binary path.");

    QTemporaryDir previewDir;
    QVERIFY2(previewDir.isValid(), qPrintable(previewDir.errorString()));
    const QDir dir(previewDir.path());
    auto writeFile = [&](const QString &name, const QByteArray &content) {
        QFile file(dir.filePath(name));
        QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(file.errorString()));
        file.write(content);
        file.close();
    };
    // A pragma Singleton registered through the directory qmldir, read into a
    // probe property. Editing it and reloading must serve the new instance;
    // clearComponentCache() alone keeps the old singleton (F-027).
    writeFile(QStringLiteral("qmldir"), "singleton Theme 1.0 Theme.qml\n");
    writeFile(QStringLiteral("Theme.qml"),
              "pragma Singleton\nimport QtQuick\nQtObject { property int tag: 1 }\n");
    writeFile(QStringLiteral("Main.qml"), R"(
import QtQuick
import "."

Window {
    width: 200
    height: 200
    visible: true
    Item {
        objectName: "singletonProbe"
        property int probeTag: Theme.tag
    }
}
)");

    QString errorMessage;
    SmokeAppRunner launcher;
    QTemporaryDir launcherWorkingDir;
    QVERIFY2(launcherWorkingDir.isValid(), qPrintable(launcherWorkingDir.errorString()));
    QVERIFY2(launcher.startLauncherPreview(dir.filePath(QStringLiteral("Main.qml")), &errorMessage,
                                           launcherWorkingDir.path()),
             qPrintable(errorMessage));

    QProcess client;
    client.setProcessChannelMode(QProcess::MergedChannels);
    client.start(qmlagentMcp, { QStringLiteral("--timeout"), QString::number(RequestTimeoutMs) });
    QVERIFY2(client.waitForStarted(), qPrintable(client.errorString()));

    auto writeRequest = [&](const QJsonObject &request) {
        client.write(compactObject(request));
        client.write("\n");
        QVERIFY2(client.waitForBytesWritten(RequestTimeoutMs), qPrintable(client.errorString()));
    };
    const QJsonObject probeQuery{
        { QStringLiteral("selector"), QStringLiteral("objectName=\"singletonProbe\"") },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("probeTag") } },
        { QStringLiteral("verbosity"), QStringLiteral("full") },
    };

    writeRequest(mcpRequest(1, QStringLiteral("initialize")));
    writeRequest(mcpToolCall(2, QStringLiteral("qmlagent_ui_query"), probeQuery));
    QByteArray output = waitForOutput(&client, QByteArrayLiteral("\"id\":2"));

    // Edit the singleton between load and reload.
    writeFile(QStringLiteral("Theme.qml"),
              "pragma Singleton\nimport QtQuick\nQtObject { property int tag: 42 }\n");

    writeRequest(mcpToolCall(3, QStringLiteral("qmlagent_preview_reload"), {
        { QStringLiteral("timeoutMs"), RequestTimeoutMs },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":3"));
    writeRequest(mcpToolCall(4, QStringLiteral("qmlagent_ui_query"), probeQuery));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":4"));
    writeRequest(mcpRequest(5, QStringLiteral("shutdown")));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":5"));
    writeRequest({
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("method"), QStringLiteral("notifications/exit") },
    });
    if (!client.waitForFinished(ProcessShutdownTimeoutMs)) {
        client.kill();
        client.waitForFinished();
    }

    const QHash<int, QJsonObject> responses = parseMcpResponses(output);
    auto probeTagOf = [](const QJsonObject &response) {
        return response.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("structuredContent")).toObject()
                .value(QStringLiteral("matches")).toArray().at(0).toObject()
                .value(QStringLiteral("properties")).toObject()
                .value(QStringLiteral("probeTag")).toInt(-1);
    };
    const QJsonObject reload = responses.value(3).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(probeTagOf(responses.value(2)), 1);
    QCOMPARE(reload.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(probeTagOf(responses.value(4)), 42);
}

void QmlAgentIntegrationTest::referenceClientClicksOutOfBoundsUnclippedChild()
{
    const QString qmlagentMcp = qmlagentMcpExecutable();
    QVERIFY2(!qmlagentMcp.isEmpty(), "Missing qmlagent-mcp test binary path.");

    QTemporaryDir previewDir;
    QVERIFY2(previewDir.isValid(), qPrintable(previewDir.errorString()));
    const QString qmlFile = QDir(previewDir.path()).filePath(QStringLiteral("Main.qml"));
    QFile file(qmlFile);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Text), qPrintable(file.errorString()));
    // Two grabbers reaching 35px outside their (unclipped) panels. reachGrabber
    // paints on top of a lower list -> Qt delivers there, so it must be
    // clickable. coveredGrabber has a foreign overlay painted on top of it ->
    // genuinely occluded, so it must still report blocked_by_item. (F-028)
    file.write(R"(
import QtQuick

Window {
    width: 400; height: 300; visible: true

    Rectangle {
        objectName: "lowerPanel"; x: 20; y: 40; width: 150; height: 120; color: "#cccccc"
        ListView {
            objectName: "lowerList"; anchors.fill: parent; model: 3
            delegate: Rectangle { width: ListView.view.width; height: 20; color: "#dddddd" }
        }
    }
    Rectangle {
        objectName: "upperPanel"; x: 70; y: 40; width: 100; height: 120; color: "#a0aaccff"
        Rectangle {
            objectName: "reachGrabber"; x: -35; y: 40; width: 35; height: 30; color: "#ff8800"
            MouseArea { objectName: "reachGrabberArea"; anchors.fill: parent }
        }
    }
    Rectangle {
        objectName: "upperPanel2"; x: 300; y: 40; width: 80; height: 120; color: "#a0ccaaff"
        Rectangle {
            objectName: "coveredGrabber"; x: -35; y: 40; width: 35; height: 30; color: "#ff8800"
            MouseArea { objectName: "coveredGrabberArea"; anchors.fill: parent }
        }
    }
    Rectangle {
        objectName: "foreignOverlay"; x: 258; y: 75; width: 40; height: 40; color: "#3300ff00"
        MouseArea { objectName: "foreignArea"; anchors.fill: parent }
    }
}
)");
    file.close();

    QString errorMessage;
    SmokeAppRunner launcher;
    QTemporaryDir launcherWorkingDir;
    QVERIFY2(launcherWorkingDir.isValid(), qPrintable(launcherWorkingDir.errorString()));
    QVERIFY2(launcher.startLauncherPreview(qmlFile, &errorMessage, launcherWorkingDir.path()),
             qPrintable(errorMessage));

    QProcess client;
    client.setProcessChannelMode(QProcess::MergedChannels);
    client.start(qmlagentMcp, { QStringLiteral("--timeout"), QString::number(RequestTimeoutMs) });
    QVERIFY2(client.waitForStarted(), qPrintable(client.errorString()));

    auto writeRequest = [&](const QJsonObject &request) {
        client.write(compactObject(request));
        client.write("\n");
        QVERIFY2(client.waitForBytesWritten(RequestTimeoutMs), qPrintable(client.errorString()));
    };

    writeRequest(mcpRequest(1, QStringLiteral("initialize")));
    writeRequest(mcpToolCall(2, QStringLiteral("qmlagent_input_click"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"reachGrabberArea\"") },
    }));
    QByteArray output = waitForOutput(&client, QByteArrayLiteral("\"id\":2"));
    writeRequest(mcpToolCall(3, QStringLiteral("qmlagent_input_click"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"coveredGrabberArea\"") },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":3"));
    writeRequest(mcpRequest(4, QStringLiteral("shutdown")));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":4"));
    writeRequest({
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("method"), QStringLiteral("notifications/exit") },
    });
    if (!client.waitForFinished(ProcessShutdownTimeoutMs)) {
        client.kill();
        client.waitForFinished();
    }

    const QHash<int, QJsonObject> responses = parseMcpResponses(output);
    const QJsonObject reachResult = responses.value(2).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QVERIFY2(reachResult.value(QStringLiteral("delivered")).toBool(),
             qPrintable(QString::fromUtf8(QJsonDocument(reachResult).toJson(QJsonDocument::Compact))));

    const QJsonObject coveredResult = responses.value(3).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(coveredResult.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(coveredResult.value(QStringLiteral("reason")).toString(),
             QStringLiteral("blocked_by_item"));
}

void QmlAgentIntegrationTest::referenceClientClicksItemLocalPoint()
{
    const QString qmlagentMcp = qmlagentMcpExecutable();
    QVERIFY2(!qmlagentMcp.isEmpty(), "Missing qmlagent-mcp test binary path.");

    QTemporaryDir previewDir;
    QVERIFY2(previewDir.isValid(), qPrintable(previewDir.errorString()));
    const QString qmlFile = QDir(previewDir.path()).filePath(QStringLiteral("Main.qml"));
    QFile file(qmlFile);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Text), qPrintable(file.errorString()));
    // A 300px-wide area recording where the click lands. Center -> ~150; an
    // item-local point:[20,10] must land at ~20, not the center. (F-029)
    file.write(R"(
import QtQuick

Window {
    width: 400; height: 200; visible: true
    Rectangle {
        objectName: "wideArea"; x: 40; y: 60; width: 300; height: 60; color: "#cccccc"
        property real lastX: -1
        MouseArea {
            objectName: "wideAreaMouse"; anchors.fill: parent
            onClicked: (m) => parent.lastX = m.x
        }
    }
}
)");
    file.close();

    QString errorMessage;
    SmokeAppRunner launcher;
    QTemporaryDir launcherWorkingDir;
    QVERIFY2(launcherWorkingDir.isValid(), qPrintable(launcherWorkingDir.errorString()));
    QVERIFY2(launcher.startLauncherPreview(qmlFile, &errorMessage, launcherWorkingDir.path()),
             qPrintable(errorMessage));

    QProcess client;
    client.setProcessChannelMode(QProcess::MergedChannels);
    client.start(qmlagentMcp, { QStringLiteral("--timeout"), QString::number(RequestTimeoutMs) });
    QVERIFY2(client.waitForStarted(), qPrintable(client.errorString()));

    auto writeRequest = [&](const QJsonObject &request) {
        client.write(compactObject(request));
        client.write("\n");
        QVERIFY2(client.waitForBytesWritten(RequestTimeoutMs), qPrintable(client.errorString()));
    };
    const QJsonObject lastXQuery{
        { QStringLiteral("selector"), QStringLiteral("objectName=\"wideArea\"") },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("lastX") } },
        { QStringLiteral("verbosity"), QStringLiteral("full") },
    };
    auto lastXOf = [](const QJsonObject &response) {
        return response.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("structuredContent")).toObject()
                .value(QStringLiteral("matches")).toArray().at(0).toObject()
                .value(QStringLiteral("properties")).toObject()
                .value(QStringLiteral("lastX")).toDouble(-1);
    };

    writeRequest(mcpRequest(1, QStringLiteral("initialize")));
    writeRequest(mcpToolCall(2, QStringLiteral("qmlagent_input_click"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"wideAreaMouse\"") },
    }));
    QByteArray output = waitForOutput(&client, QByteArrayLiteral("\"id\":2"));
    writeRequest(mcpToolCall(3, QStringLiteral("qmlagent_ui_query"), lastXQuery));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":3"));
    writeRequest(mcpToolCall(4, QStringLiteral("qmlagent_input_click"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"wideAreaMouse\"") },
        { QStringLiteral("point"), QJsonArray{ 20, 10 } },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":4"));
    writeRequest(mcpToolCall(5, QStringLiteral("qmlagent_ui_query"), lastXQuery));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":5"));
    writeRequest(mcpRequest(6, QStringLiteral("shutdown")));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":6"));
    writeRequest({
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("method"), QStringLiteral("notifications/exit") },
    });
    if (!client.waitForFinished(ProcessShutdownTimeoutMs)) {
        client.kill();
        client.waitForFinished();
    }

    const QHash<int, QJsonObject> responses = parseMcpResponses(output);
    QCOMPARE(responses.value(2).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(lastXOf(responses.value(3)), 150.0);
    QCOMPARE(responses.value(4).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(lastXOf(responses.value(5)), 20.0);
}

void QmlAgentIntegrationTest::referenceClientMcpPersistentMode()
{
    const QString qmlagentMcp = qmlagentMcpExecutable();
    QVERIFY2(!qmlagentMcp.isEmpty(), "Missing qmlagent-mcp test binary path.");

    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QProcess client;
    client.setProcessChannelMode(QProcess::MergedChannels);
    client.start(qmlagentMcp, {
        QStringLiteral("--timeout"), QString::number(RequestTimeoutMs),
    });
    QVERIFY2(client.waitForStarted(), qPrintable(client.errorString()));

    auto writeRequest = [&](const QJsonObject &request) {
        client.write(compactObject(request));
        client.write("\n");
        QVERIFY2(client.waitForBytesWritten(RequestTimeoutMs), qPrintable(client.errorString()));
    };

    writeRequest(mcpRequest(1, QStringLiteral("initialize")));
    writeRequest(mcpRequest(2, QStringLiteral("tools/list")));
    writeRequest(mcpToolCall(3, QStringLiteral("qmlagent_target_status"), {}));
    writeRequest(mcpToolCall(4, QStringLiteral("qmlagent_ui_query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.content\"") },
    }));
    writeRequest(mcpToolCall(5, QStringLiteral("qmlagent_connect_tcp"), {
        { QStringLiteral("host"), QStringLiteral("127.0.0.1") },
        { QStringLiteral("port"), smoke.port() },
        { QStringLiteral("timeoutMs"), RequestTimeoutMs },
    }));
    writeRequest(mcpToolCall(6, QStringLiteral("qmlagent_ui_query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.content\"") },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("color") } },
    }));
    writeRequest(mcpToolCall(7, QStringLiteral("qmlagent_source_resolve"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.content\"") },
    }));
    writeRequest(mcpToolCall(23, QStringLiteral("qmlagent_runtime_enable_mutation"), {}));
    writeRequest(mcpToolCall(20, QStringLiteral("qmlagent_runtime_set_property"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"runtimeProbe\"") },
        { QStringLiteral("property"), QStringLiteral("label") },
        { QStringLiteral("value"), QStringLiteral("mcp") },
    }));
    writeRequest(mcpToolCall(21, QStringLiteral("qmlagent_runtime_invoke_method"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"runtimeProbe\"") },
        { QStringLiteral("method"), QStringLiteral("setRuntimeCounter") },
        { QStringLiteral("args"), QJsonArray{ 3 } },
    }));
    writeRequest(mcpToolCall(15, QStringLiteral("qmlagent_ui_get_tree"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.dynamic\"") },
    }));
    writeRequest(mcpToolCall(8, QStringLiteral("qmlagent_workflow_key"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("key"), QStringLiteral("m") },
        { QStringLiteral("expectSelector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("expect"), QStringLiteral("text=\"m\"") },
    }));
    writeRequest(mcpToolCall(26, QStringLiteral("qmlagent_workflow_key"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("key"), QStringLiteral("n") },
        { QStringLiteral("expectSelector"), QStringLiteral("objectName=\"smoke.textInput\"") },
        { QStringLiteral("expect"), QStringLiteral("text contains \"mn\"") },
    }));
    writeRequest(mcpToolCall(9, QStringLiteral("qmlagent_log_enable"), {
        { QStringLiteral("replayBuffered"), true },
    }));

    QByteArray output = waitForOutput(&client, QByteArrayLiteral("\"id\":26"));
    output += waitForOutput(&client, QByteArrayLiteral("\"method\":\"Log.entryAdded\""));
    writeRequest(mcpToolCall(22, QStringLiteral("qmlagent_target_status"), {}));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":22"));

    writeRequest(mcpToolCall(10, QStringLiteral("qmlagent_ui_subscribe"), {}));
    writeRequest(mcpToolCall(11, QStringLiteral("qmlagent_workflow_click"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.clickArea\"") },
        { QStringLiteral("expectSelector"), QStringLiteral("objectName=\"smoke.clicked\"") },
        { QStringLiteral("expect"), QStringLiteral("objectName=\"smoke.clicked\"") },
        { QStringLiteral("verbosity"), QStringLiteral("summary") },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":11"));

    writeRequest(mcpToolCall(24, QStringLiteral("qmlagent_workflow_click_and_wait"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.secondClickArea\"") },
        { QStringLiteral("waitSelector"), QStringLiteral("objectName=\"smoke.clicked\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("state"), QStringLiteral("found") },
        } },
        { QStringLiteral("timeoutMs"), 250 },
        { QStringLiteral("verbosity"), QStringLiteral("summary") },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":24"));

    writeRequest(mcpToolCall(25, QStringLiteral("qmlagent_workflow_click_and_wait"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.secondClickArea\"") },
        { QStringLiteral("waitSelector"), QStringLiteral("id=\"indexedDelegateProbe\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("property"), QStringLiteral("selectedIndex") },
            { QStringLiteral("op"), QStringLiteral("=") },
            { QStringLiteral("value"), 123 },
        } },
        { QStringLiteral("timeoutMs"), 25 },
        { QStringLiteral("verbosity"), QStringLiteral("summary") },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":25"));

    writeRequest(mcpToolCall(12, QStringLiteral("qmlagent_input_click"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.secondClickArea\"") },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":12"));
    output += waitForOutput(&client, QByteArrayLiteral("\"method\":\"UI.treeChanged\""));
    writeRequest(mcpToolCall(16, QStringLiteral("qmlagent_input_wheel"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeFlickable\"") },
        { QStringLiteral("deltaY"), -120 },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":16"));
    writeRequest(mcpToolCall(17, QStringLiteral("qmlagent_input_drag"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeDragCommandArea\"") },
        { QStringLiteral("to"), QJsonArray{ 90, 10 } },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":17"));
    writeRequest(mcpToolCall(18, QStringLiteral("qmlagent_input_touch"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeTouchArea\"") },
        { QStringLiteral("type"), QStringLiteral("touchBegin") },
        { QStringLiteral("points"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), 1 },
                { QStringLiteral("state"), QStringLiteral("pressed") },
                { QStringLiteral("point"), QJsonArray{ 15, 20 } },
            },
            QJsonObject{
                { QStringLiteral("id"), 2 },
                { QStringLiteral("state"), QStringLiteral("pressed") },
                { QStringLiteral("point"), QJsonArray{ 55, 20 } },
            },
        } },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":18"));
    writeRequest(mcpToolCall(19, QStringLiteral("qmlagent_input_touch"), {
        { QStringLiteral("selector"), QStringLiteral("id=\"smokeTouchArea\"") },
        { QStringLiteral("type"), QStringLiteral("touchEnd") },
        { QStringLiteral("points"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), 1 },
                { QStringLiteral("state"), QStringLiteral("released") },
                { QStringLiteral("point"), QJsonArray{ 15, 20 } },
            },
            QJsonObject{
                { QStringLiteral("id"), 2 },
                { QStringLiteral("state"), QStringLiteral("released") },
                { QStringLiteral("point"), QJsonArray{ 55, 20 } },
            },
        } },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":19"));
    writeRequest(mcpToolCall(13, QStringLiteral("qmlagent_disconnect"), {}));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":13"));
    writeRequest(mcpRequest(14, QStringLiteral("shutdown")));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":14"));
    writeRequest({
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("method"), QStringLiteral("notifications/exit") },
    });
    if (!client.waitForFinished(ProcessShutdownTimeoutMs)) {
        client.kill();
        client.waitForFinished();
    }

    QJsonArray notifications;
    const QHash<int, QJsonObject> responses = parseMcpResponses(output, &notifications);

    QVERIFY2(responses.contains(1), output.constData());
    QVERIFY2(responses.contains(2), output.constData());
    QVERIFY2(responses.contains(3), output.constData());
    QVERIFY2(responses.contains(4), output.constData());
    QVERIFY2(responses.contains(5), output.constData());
    QVERIFY2(responses.contains(6), output.constData());
    QVERIFY2(responses.contains(7), output.constData());
    QVERIFY2(responses.contains(8), output.constData());
    QVERIFY2(responses.contains(9), output.constData());
    QVERIFY2(responses.contains(10), output.constData());
    QVERIFY2(responses.contains(11), output.constData());
    QVERIFY2(responses.contains(12), output.constData());
    QVERIFY2(responses.contains(13), output.constData());
    QVERIFY2(responses.contains(14), output.constData());
    QVERIFY2(responses.contains(15), output.constData());
    QVERIFY2(responses.contains(16), output.constData());
    QVERIFY2(responses.contains(17), output.constData());
    QVERIFY2(responses.contains(18), output.constData());
    QVERIFY2(responses.contains(19), output.constData());
    QVERIFY2(responses.contains(20), output.constData());
    QVERIFY2(responses.contains(21), output.constData());
    QVERIFY2(responses.contains(22), output.constData());
    QVERIFY2(responses.contains(23), output.constData());
    QVERIFY2(responses.contains(24), output.constData());
    QVERIFY2(responses.contains(25), output.constData());
    QVERIFY2(responses.contains(26), output.constData());

    QCOMPARE(responses.value(1).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("serverInfo")).toObject()
                     .value(QStringLiteral("name")).toString(),
             QStringLiteral("qmlagent"));

    const QJsonArray tools = responses.value(2).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("tools")).toArray();
    bool sawUiQuery = false;
    bool sawUiWaitFor = false;
    bool sawConnect = false;
    bool sawConnectLocalSocket = false;
    bool sawStatus = false;
    bool sawLogEnable = false;
    bool sawInputDrag = false;
    bool sawInputLongPress = false;
    bool sawInputMouse = false;
    bool sawInputTouch = false;
    bool sawInputWheel = false;
    bool sawInputClearText = false;
    bool sawRuntimeEnableMutation = false;
    bool sawRuntimeSetProperty = false;
    bool sawRuntimeInvokeMethod = false;
    bool sawWorkflowClick = false;
    bool sawWorkflowClickAndWait = false;
    bool sawWorkflowLongPressAndWait = false;
    bool sawWorkflowKey = false;
    for (const QJsonValue &toolValue : tools) {
        const QString name = toolValue.toObject().value(QStringLiteral("name")).toString();
        sawUiQuery |= name == QLatin1String("qmlagent_ui_query");
        sawUiWaitFor |= name == QLatin1String("qmlagent_ui_wait_for");
        sawConnect |= name == QLatin1String("qmlagent_connect_tcp");
        sawConnectLocalSocket |= name == QLatin1String("qmlagent_connect_local_socket");
        sawStatus |= name == QLatin1String("qmlagent_target_status");
        sawLogEnable |= name == QLatin1String("qmlagent_log_enable");
        sawInputDrag |= name == QLatin1String("qmlagent_input_drag");
        sawInputLongPress |= name == QLatin1String("qmlagent_input_long_press");
        sawInputMouse |= name == QLatin1String("qmlagent_input_mouse");
        sawInputTouch |= name == QLatin1String("qmlagent_input_touch");
        sawInputWheel |= name == QLatin1String("qmlagent_input_wheel");
        sawInputClearText |= name == QLatin1String("qmlagent_input_clear_text");
        sawRuntimeEnableMutation |= name == QLatin1String("qmlagent_runtime_enable_mutation");
        sawRuntimeSetProperty |= name == QLatin1String("qmlagent_runtime_set_property");
        sawRuntimeInvokeMethod |= name == QLatin1String("qmlagent_runtime_invoke_method");
        sawWorkflowClick |= name == QLatin1String("qmlagent_workflow_click");
        sawWorkflowClickAndWait |= name == QLatin1String("qmlagent_workflow_click_and_wait");
        sawWorkflowLongPressAndWait |= name == QLatin1String("qmlagent_workflow_long_press_and_wait");
        sawWorkflowKey |= name == QLatin1String("qmlagent_workflow_key");
    }
    QVERIFY2(sawUiQuery, output.constData());
    QVERIFY2(sawUiWaitFor, output.constData());
    QVERIFY2(sawConnect, output.constData());
    QVERIFY2(sawConnectLocalSocket, output.constData());
    QVERIFY2(sawStatus, output.constData());
    QVERIFY2(sawLogEnable, output.constData());
    QVERIFY2(sawInputDrag, output.constData());
    QVERIFY2(sawInputLongPress, output.constData());
    QVERIFY2(sawInputMouse, output.constData());
    QVERIFY2(sawInputTouch, output.constData());
    QVERIFY2(sawInputWheel, output.constData());
    QVERIFY2(sawInputClearText, output.constData());
    QVERIFY2(sawRuntimeEnableMutation, output.constData());
    QVERIFY2(sawRuntimeSetProperty, output.constData());
    QVERIFY2(sawRuntimeInvokeMethod, output.constData());
    QVERIFY2(sawWorkflowClick, output.constData());
    QVERIFY2(sawWorkflowClickAndWait, output.constData());
    QVERIFY2(sawWorkflowLongPressAndWait, output.constData());
    QVERIFY2(sawWorkflowKey, output.constData());

    QCOMPARE(responses.value(3).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("connected")).toBool(),
             false);
    const QJsonObject disconnectedStatus = responses.value(3)
            .value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(disconnectedStatus.value(QStringLiteral("attachTools")).toObject()
                     .value(QStringLiteral("tools")).toArray()
                     .at(0).toString(),
             QStringLiteral("qmlagent_connect_tcp"));
    QVERIFY2(disconnectedStatus.value(QStringLiteral("nextStep")).toString()
                     .contains(QStringLiteral("qmlagent_connect_tcp")),
             output.constData());
    QVERIFY2(responses.value(4).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("isError")).toBool(),
             output.constData());
    QCOMPARE(responses.value(5).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("connected")).toBool(),
             true);
    const QJsonObject connectedStatus = responses.value(5).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(connectedStatus.value(QStringLiteral("debugConnectionPolicy")).toObject()
                     .value(QStringLiteral("singleClientPerTarget")).toBool(),
             true);
    QVERIFY2(connectedStatus.value(QStringLiteral("capabilities")).toObject()
                    .contains(QStringLiteral("qtQuick3D")),
             output.constData());
    QVERIFY2(connectedStatus.value(QStringLiteral("targetSessionInfo")).toObject()
                    .value(QStringLiteral("capabilities")).toObject()
                    .contains(QStringLiteral("qtQuick3D")),
             output.constData());

    const QJsonObject queryContent = responses.value(6).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    const QJsonArray matches = queryContent.value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    QCOMPARE(queryContent.value(QStringLiteral("moreAvailable")).toBool(), true);
    QVERIFY2(queryContent.value(QStringLiteral("omittedFields")).toArray()
                     .contains(QStringLiteral("children")),
             output.constData());
    QVERIFY2(!matches.at(0).toObject().contains(QStringLiteral("children")),
             output.constData());
    QCOMPARE(matches.at(0).toObject().value(QStringLiteral("objectName")).toString(),
             QStringLiteral("smoke.content"));
    QCOMPARE(matches.at(0).toObject().value(QStringLiteral("properties")).toObject()
                     .value(QStringLiteral("color")).toString(),
             QStringLiteral("#1f6feb"));

    const QJsonObject sourceContent = responses.value(7).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(sourceContent.value(QStringLiteral("location")).toObject()
                     .value(QStringLiteral("method")).toString(),
             QStringLiteral("qqmldata-direct"));

    QCOMPARE(responses.value(20).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("ok")).toBool(),
             true);
    QCOMPARE(responses.value(20).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("verificationRole")).toString(),
             QStringLiteral("setup-only"));
    QCOMPARE(responses.value(21).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("returnValue")).toInt(-1),
             3);

    const QJsonObject selectedTreeContent = responses.value(15).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QVERIFY2(!selectedTreeContent.value(QStringLiteral("windows")).toArray().isEmpty(),
             output.constData());

    QCOMPARE(responses.value(8).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("kind")).toString(),
             QStringLiteral("key-and-verify"));
    QCOMPARE(responses.value(8).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("passed")).toBool(),
             true);
    QCOMPARE(responses.value(26).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("operator")).toString(),
             QStringLiteral("contains"));
    QCOMPARE(responses.value(26).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("passed")).toBool(),
             true);
    QCOMPARE(responses.value(9).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("enabled")).toBool(),
             true);
    const QJsonObject statusWithLogs = responses.value(22).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QVERIFY2(statusWithLogs.value(QStringLiteral("recentLogEntryCount")).toInt() > 0,
             output.constData());
    QVERIFY2(!statusWithLogs.value(QStringLiteral("recentLogEntries")).toArray().isEmpty(),
             output.constData());
    QCOMPARE(responses.value(23).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("capabilities")).toObject()
                     .value(QStringLiteral("runtimeMutation")).toObject()
                     .value(QStringLiteral("enabled")).toBool(),
             true);
    QCOMPARE(responses.value(10).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("enabled")).toBool(),
             true);
    QCOMPARE(responses.value(11).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("kind")).toString(),
             QStringLiteral("click-and-verify"));
    QCOMPARE(responses.value(11).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("passed")).toBool(),
             true);
    QVERIFY(!responses.value(11).value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("structuredContent")).toObject()
                    .contains(QStringLiteral("before")));
    const QJsonObject clickAndWaitContent = responses.value(24).value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(clickAndWaitContent.value(QStringLiteral("kind")).toString(),
             QStringLiteral("click-and-wait"));
    QCOMPARE(clickAndWaitContent.value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("passed")).toBool(),
             true);
    QCOMPARE(clickAndWaitContent.value(QStringLiteral("waitSelector")).toString(),
             QStringLiteral("objectName=\"smoke.clicked\""));
    QCOMPARE(clickAndWaitContent.value(QStringLiteral("wait")).toObject()
                     .value(QStringLiteral("ok")).toBool(),
             true);
    QCOMPARE(clickAndWaitContent.value(QStringLiteral("wait")).toObject()
                     .value(QStringLiteral("matchCount")).toInt(),
             1);
    QVERIFY2(clickAndWaitContent.value(QStringLiteral("wait")).toObject()
                     .value(QStringLiteral("node")).toObject()
                     .value(QStringLiteral("objectName")).toString()
                     == QStringLiteral("smoke.clicked"),
             output.constData());
    const QJsonObject failedClickAndWaitContent = responses.value(25)
            .value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(failedClickAndWaitContent.value(QStringLiteral("kind")).toString(),
             QStringLiteral("click-and-wait"));
    QCOMPARE(failedClickAndWaitContent.value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("passed")).toBool(),
             false);
    QCOMPARE(failedClickAndWaitContent.value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("timedOut")).toBool(),
             true);
    QCOMPARE(failedClickAndWaitContent.value(QStringLiteral("wait")).toObject()
                     .value(QStringLiteral("timedOut")).toBool(),
             true);
    QVERIFY2(!failedClickAndWaitContent.value(QStringLiteral("wait")).toObject()
                     .value(QStringLiteral("nextHints")).toArray().isEmpty(),
             output.constData());
    QVERIFY2(!failedClickAndWaitContent.value(QStringLiteral("issues")).toArray().isEmpty(),
             output.constData());
    QCOMPARE(responses.value(12).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    const QJsonObject inputClickContent = responses.value(12).value(QStringLiteral("result"))
            .toObject().value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(inputClickContent.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(inputClickContent.value(QStringLiteral("verificationRole")).toString(),
             QStringLiteral("input-delivery-only"));
    QCOMPARE(inputClickContent.value(QStringLiteral("semanticProof")).toBool(true), false);
    QVERIFY(inputClickContent.value(QStringLiteral("settle")).toObject()
                    .contains(QStringLiteral("ok")));
    QCOMPARE(inputClickContent.value(QStringLiteral("settle")).toObject()
                     .value(QStringLiteral("verificationRole")).toString(),
             QStringLiteral("render-loop-settle-only"));
    QCOMPARE(inputClickContent.value(QStringLiteral("settle")).toObject()
                     .value(QStringLiteral("semanticProof")).toBool(true),
             false);
    QCOMPARE(responses.value(16).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    QCOMPARE(responses.value(17).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    QCOMPARE(responses.value(18).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    QCOMPARE(responses.value(19).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("delivered")).toBool(),
             true);
    QCOMPARE(responses.value(13).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("connected")).toBool(),
             false);
    QVERIFY(responses.value(14).value(QStringLiteral("result")).isNull());

    bool sawLogEntry = false;
    int treeChangedCount = 0;
    for (const QJsonValue &notificationValue : notifications) {
        const QJsonObject event = notificationValue.toObject()
                .value(QStringLiteral("params")).toObject()
                .value(QStringLiteral("data")).toObject();
        sawLogEntry |= event.value(QStringLiteral("method")).toString()
                == QLatin1String("Log.entryAdded");
        if (event.value(QStringLiteral("method")).toString()
                == QLatin1String("UI.treeChanged")) {
            ++treeChangedCount;
        }
    }
    QVERIFY2(sawLogEntry, output.constData());
    QVERIFY2(treeChangedCount >= 2, output.constData());
}

void QmlAgentIntegrationTest::referenceClientMcpWorkflowReports()
{
    const QString qmlagentMcp = qmlagentMcpExecutable();
    QVERIFY2(!qmlagentMcp.isEmpty(), "Missing qmlagent-mcp test binary path.");

    auto runWorkflow = [&](const QString &fixtureName, const QString &verbosity = QString()) -> QJsonObject {
        QString errorMessage;
        SmokeAppRunner fixture;
        if (!fixture.startWorkflowFixture(fixtureName, &errorMessage))
            return { { QStringLiteral("error"), errorMessage } };

        QProcess client;
        client.setProcessChannelMode(QProcess::MergedChannels);
        client.start(qmlagentMcp, {
            QStringLiteral("--timeout"), QString::number(RequestTimeoutMs),
        });
        if (!client.waitForStarted())
            return { { QStringLiteral("error"), client.errorString() } };

        auto writeRequest = [&](const QJsonObject &request) {
            client.write(compactObject(request));
            client.write("\n");
            return client.waitForBytesWritten(RequestTimeoutMs);
        };

        if (!writeRequest(mcpRequest(1, QStringLiteral("initialize"))))
            return { { QStringLiteral("error"), client.errorString() } };
        if (!writeRequest(mcpToolCall(2, QStringLiteral("qmlagent_connect_tcp"), {
                { QStringLiteral("host"), QStringLiteral("127.0.0.1") },
                { QStringLiteral("port"), fixture.port() },
                { QStringLiteral("timeoutMs"), RequestTimeoutMs },
            }))) {
            return { { QStringLiteral("error"), client.errorString() } };
        }
        QJsonObject workflowArguments{
                { QStringLiteral("selector"), QStringLiteral("objectName=\"fixture.action\"") },
                { QStringLiteral("expectSelector"), QStringLiteral("objectName=\"fixture.result\"") },
                { QStringLiteral("expect"), QStringLiteral("visible=true") },
        };
        if (!verbosity.isEmpty())
            workflowArguments.insert(QStringLiteral("verbosity"), verbosity);
        if (!writeRequest(mcpToolCall(3, QStringLiteral("qmlagent_workflow_click"),
                                      workflowArguments))) {
            return { { QStringLiteral("error"), client.errorString() } };
        }

        const QByteArray output = waitForOutput(&client, QByteArrayLiteral("\"id\":3"));
        client.terminate();
        if (!client.waitForFinished(ProcessShutdownTimeoutMs)) {
            client.kill();
            client.waitForFinished();
        }

        const QHash<int, QJsonObject> responses = parseMcpResponses(output);
        const QJsonObject connectResponse = responses.value(2);
        if (!connectResponse.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("structuredContent")).toObject()
                    .value(QStringLiteral("connected")).toBool()) {
            return {
                { QStringLiteral("error"), QStringLiteral("missing MCP connect response") },
                { QStringLiteral("output"), QString::fromUtf8(output) },
            };
        }
        const QJsonObject response = responses.value(3);
        if (response.isEmpty()) {
            return {
                { QStringLiteral("error"), QStringLiteral("missing MCP workflow response") },
                { QStringLiteral("output"), QString::fromUtf8(output) },
            };
        }
        return response.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("structuredContent")).toObject();
    };

    const QJsonObject stateChange = runWorkflow(QStringLiteral("input-state-change"));
    QVERIFY2(!stateChange.contains(QStringLiteral("error")),
             qPrintable(QString::fromUtf8(QJsonDocument(stateChange).toJson(QJsonDocument::Compact))));
    QCOMPARE(stateChange.value(QStringLiteral("kind")).toString(), QStringLiteral("click-and-verify"));
    QCOMPARE(stateChange.value(QStringLiteral("input")).toObject()
                     .value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(stateChange.value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("passed")).toBool(), true);
    QVERIFY2(stateChange.value(QStringLiteral("issues")).toArray().isEmpty(),
             qPrintable(QString::fromUtf8(QJsonDocument(stateChange)
                                          .toJson(QJsonDocument::Compact))));

    const QJsonObject stateChangeSummary = runWorkflow(QStringLiteral("input-state-change"),
                                                       QStringLiteral("summary"));
    QVERIFY2(!stateChangeSummary.contains(QStringLiteral("error")),
             qPrintable(QString::fromUtf8(QJsonDocument(stateChangeSummary)
                                          .toJson(QJsonDocument::Compact))));
    QCOMPARE(stateChangeSummary.value(QStringLiteral("input")).toObject()
                     .value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(stateChangeSummary.value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("passed")).toBool(), true);

    const QJsonObject noEffect = runWorkflow(QStringLiteral("input-no-effect"));
    QVERIFY2(!noEffect.contains(QStringLiteral("error")),
             qPrintable(QString::fromUtf8(QJsonDocument(noEffect).toJson(QJsonDocument::Compact))));
    QCOMPARE(noEffect.value(QStringLiteral("input")).toObject()
                     .value(QStringLiteral("delivered")).toBool(), true);
    QCOMPARE(noEffect.value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("passed")).toBool(), false);
    QCOMPARE(noEffect.value(QStringLiteral("issues")).toArray().at(0).toObject()
                     .value(QStringLiteral("id")).toString(),
             QStringLiteral("state.no_change_after_action"));

    const QJsonObject refused = runWorkflow(QStringLiteral("input-refused"));
    QVERIFY2(!refused.contains(QStringLiteral("error")),
             qPrintable(QString::fromUtf8(QJsonDocument(refused).toJson(QJsonDocument::Compact))));
    QCOMPARE(refused.value(QStringLiteral("input")).toObject()
                     .value(QStringLiteral("delivered")).toBool(), false);
    const QJsonArray refusedIssues = refused.value(QStringLiteral("issues")).toArray();
    QVERIFY(!refusedIssues.isEmpty());
    QCOMPARE(refusedIssues.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_clickable"));
    for (const QJsonValue &issue : refusedIssues) {
        QVERIFY2(issue.toObject().value(QStringLiteral("id")).toString()
                         != QLatin1String("state.no_change_after_action"),
                 qPrintable(QString::fromUtf8(QJsonDocument(refused)
                                              .toJson(QJsonDocument::Compact))));
    }
}

void QmlAgentIntegrationTest::referenceClientReportsSingleClientConflict()
{
    const QString qmlagentMcp = qmlagentMcpExecutable();
    QVERIFY2(!qmlagentMcp.isEmpty(), "Missing qmlagent-mcp test binary path.");

    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QProcess mcpClient;
    mcpClient.setProcessChannelMode(QProcess::MergedChannels);
    mcpClient.start(qmlagentMcp, {
        QStringLiteral("--timeout"), QString::number(RequestTimeoutMs),
    });
    QVERIFY2(mcpClient.waitForStarted(), qPrintable(mcpClient.errorString()));

    auto writeRequest = [&](const QJsonObject &request) {
        mcpClient.write(compactObject(request));
        mcpClient.write("\n");
        QVERIFY2(mcpClient.waitForBytesWritten(RequestTimeoutMs),
                 qPrintable(mcpClient.errorString()));
    };

    writeRequest(mcpRequest(1, QStringLiteral("initialize")));
    writeRequest(mcpToolCall(2, QStringLiteral("qmlagent_connect_tcp"), {
        { QStringLiteral("host"), QStringLiteral("127.0.0.1") },
        { QStringLiteral("port"), smoke.port() },
        { QStringLiteral("timeoutMs"), RequestTimeoutMs },
    }));
    QByteArray mcpOutput = waitForOutput(&mcpClient, QByteArrayLiteral("\"id\":2"));
    const QHash<int, QJsonObject> mcpResponses = parseMcpResponses(mcpOutput);
    QVERIFY2(mcpResponses.value(2).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("connected")).toBool(),
             mcpOutput.constData());
    const QJsonObject targetLease = mcpResponses.value(2).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject()
            .value(QStringLiteral("targetLease")).toObject();
    QCOMPARE(targetLease.value(QStringLiteral("owned")).toBool(), true);
    QCOMPARE(targetLease.value(QStringLiteral("mode")).toString(), QStringLiteral("mcp"));

    QProcess secondMcpClient;
    secondMcpClient.setProcessChannelMode(QProcess::MergedChannels);
    secondMcpClient.start(qmlagentMcp, {
        QStringLiteral("--timeout"), QStringLiteral("1000"),
    });
    QVERIFY2(secondMcpClient.waitForStarted(), qPrintable(secondMcpClient.errorString()));
    secondMcpClient.write(compactObject(mcpRequest(1, QStringLiteral("initialize"))));
    secondMcpClient.write("\n");
    secondMcpClient.write(compactObject(mcpToolCall(2, QStringLiteral("qmlagent_connect_tcp"), {
        { QStringLiteral("host"), QStringLiteral("127.0.0.1") },
        { QStringLiteral("port"), smoke.port() },
        { QStringLiteral("timeoutMs"), 1000 },
    })));
    secondMcpClient.write("\n");
    QVERIFY2(secondMcpClient.waitForBytesWritten(RequestTimeoutMs),
             qPrintable(secondMcpClient.errorString()));
    const QByteArray conflictOutput = waitForOutput(&secondMcpClient, QByteArrayLiteral("\"id\":2"));
    secondMcpClient.terminate();
    if (!secondMcpClient.waitForFinished(ProcessShutdownTimeoutMs)) {
        secondMcpClient.kill();
        secondMcpClient.waitForFinished();
    }
    QVERIFY2(conflictOutput.contains("already owned"), conflictOutput.constData());
    QVERIFY2(conflictOutput.contains("Only one qmlagent-mcp gateway"),
             conflictOutput.constData());

    writeRequest(mcpToolCall(3, QStringLiteral("qmlagent_disconnect"), {}));
    mcpOutput += waitForOutput(&mcpClient, QByteArrayLiteral("\"id\":3"));
    writeRequest(mcpRequest(4, QStringLiteral("shutdown")));
    mcpOutput += waitForOutput(&mcpClient, QByteArrayLiteral("\"id\":4"));
    writeRequest({
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("method"), QStringLiteral("notifications/exit") },
    });
    if (!mcpClient.waitForFinished(ProcessShutdownTimeoutMs)) {
        mcpClient.kill();
        mcpClient.waitForFinished();
    }
}

void QmlAgentIntegrationTest::referenceClientMcpRefusesExistingLocalSocketPath()
{
    const QString qmlagentMcp = qmlagentMcpExecutable();
    QVERIFY2(!qmlagentMcp.isEmpty(), "Missing qmlagent-mcp test binary path.");

    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), qPrintable(dir.errorString()));
    const QString path = dir.filePath(QStringLiteral("not-a-socket"));
    QFile existing(path);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("keep"), qint64(4));
    existing.close();

    QProcess client;
    client.setProcessChannelMode(QProcess::MergedChannels);
    client.start(qmlagentMcp, {
        QStringLiteral("--timeout"), QString::number(250),
    });
    QVERIFY2(client.waitForStarted(), qPrintable(client.errorString()));

    auto writeRequest = [&](const QJsonObject &request) {
        client.write(compactObject(request));
        client.write("\n");
        QVERIFY2(client.waitForBytesWritten(RequestTimeoutMs), qPrintable(client.errorString()));
    };

    writeRequest(mcpRequest(1, QStringLiteral("initialize")));
    writeRequest(mcpToolCall(2, QStringLiteral("qmlagent_connect_local_socket"), {
        { QStringLiteral("path"), path },
        { QStringLiteral("timeoutMs"), 250 },
    }));
    QByteArray output = waitForOutput(&client, QByteArrayLiteral("\"id\":2"));
    writeRequest(mcpRequest(3, QStringLiteral("shutdown")));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":3"));
    writeRequest({
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("method"), QStringLiteral("notifications/exit") },
    });
    if (!client.waitForFinished(ProcessShutdownTimeoutMs)) {
        client.kill();
        client.waitForFinished();
    }

    const QHash<int, QJsonObject> responses = parseMcpResponses(output);
    QVERIFY2(responses.contains(2), output.constData());
    const QJsonObject connectResult = responses.value(2).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(connectResult.value(QStringLiteral("connected")).toBool(), false);
    QVERIFY2(connectResult.value(QStringLiteral("lastError")).toString()
                     .contains(QStringLiteral("Refusing to listen on existing local socket path")),
             output.constData());

    QFile kept(path);
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), QByteArrayLiteral("keep"));
}

void QmlAgentIntegrationTest::referenceClientMcpConnectsLocalSocket()
{
    const QString qmlagentMcp = qmlagentMcpExecutable();
    QVERIFY2(!qmlagentMcp.isEmpty(), "Missing qmlagent-mcp test binary path.");

    const QString socketName = QStringLiteral("qmlagent-mcp-local-%1")
            .arg(QUuid::createUuid().toString(QUuid::Id128));

    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.startLocalSocket(socketName, &errorMessage), qPrintable(errorMessage));

    QProcess client;
    client.setProcessChannelMode(QProcess::MergedChannels);
    client.start(qmlagentMcp, {
        QStringLiteral("--timeout"), QString::number(RequestTimeoutMs),
    });
    QVERIFY2(client.waitForStarted(), qPrintable(client.errorString()));

    auto writeRequest = [&](const QJsonObject &request) {
        client.write(compactObject(request));
        client.write("\n");
        QVERIFY2(client.waitForBytesWritten(RequestTimeoutMs), qPrintable(client.errorString()));
    };

    writeRequest(mcpRequest(1, QStringLiteral("initialize")));
    writeRequest(mcpToolCall(2, QStringLiteral("qmlagent_connect_local_socket"), {
        { QStringLiteral("path"), socketName },
        { QStringLiteral("timeoutMs"), RequestTimeoutMs },
    }));
    QByteArray output = waitForOutput(&client, QByteArrayLiteral("\"id\":2"));
    writeRequest(mcpToolCall(3, QStringLiteral("qmlagent_ui_query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.content\"") },
    }));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":3"));
    writeRequest(mcpToolCall(4, QStringLiteral("qmlagent_disconnect"), {}));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":4"));
    writeRequest(mcpRequest(5, QStringLiteral("shutdown")));
    output += waitForOutput(&client, QByteArrayLiteral("\"id\":5"));
    writeRequest({
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("method"), QStringLiteral("notifications/exit") },
    });
    if (!client.waitForFinished(ProcessShutdownTimeoutMs)) {
        client.kill();
        client.waitForFinished();
    }

    const QHash<int, QJsonObject> responses = parseMcpResponses(output);
    QVERIFY2(responses.contains(2), output.constData());
    QVERIFY2(responses.contains(3), output.constData());
    QVERIFY2(responses.contains(4), output.constData());
    QVERIFY2(responses.contains(5), output.constData());

    const QJsonObject connectResult = responses.value(2).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    QCOMPARE(connectResult.value(QStringLiteral("connected")).toBool(), true);
    QCOMPARE(connectResult.value(QStringLiteral("localSocket")).toString(), socketName);

    const QJsonObject queryResult = responses.value(3).value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("structuredContent")).toObject();
    const QJsonArray matches = queryResult.value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.at(0).toObject().value(QStringLiteral("objectName")).toString(),
             QStringLiteral("smoke.content"));

    QCOMPARE(responses.value(4).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("connected")).toBool(),
             false);
    QVERIFY(responses.value(5).value(QStringLiteral("result")).isNull());
}

void QmlAgentIntegrationTest::guiDispatchReportsTruthfulOutcomes()
{
    // smoke.slowHandlerButton blocks the GUI thread for 8s on click — past
    // the input dispatch budget (~5s) plus the grace window (1s in the test
    // environment). The dispatch contract under test: the timeout response
    // must say the outcome is unknown (never imply the click did not happen),
    // requests during the stuck window must be refused as busy instead of
    // interleaving, and once the handler finishes the click's effect must be
    // observable.
    QString errorMessage;
    SmokeAppRunner smoke;
    QVERIFY2(smoke.start(&errorMessage), qPrintable(errorMessage));

    QQmlDebugConnection connection;
    QmlAgentDebugClient client(&connection);
    QVERIFY2(connectToQmlAgent(smoke.port(), &connection, &client, &errorMessage),
             qPrintable(errorMessage));

    QElapsedTimer sinceClick;
    sinceClick.start();
    const auto slowClickResponse = invoke(&client, QStringLiteral("Input.clickNode"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.slowHandlerButton\"") },
    }, 1, &errorMessage, 8000);
    QVERIFY2(slowClickResponse.has_value(), qPrintable(errorMessage));
    const QJsonObject slowClickResult =
            slowClickResponse->value(QStringLiteral("result")).toObject();
    QCOMPARE(slowClickResult.value(QStringLiteral("timedOut")).toBool(false), true);
    QCOMPARE(slowClickResult.value(QStringLiteral("outcome")).toString(),
             QStringLiteral("unknown"));
    QCOMPARE(slowClickResult.value(QStringLiteral("diagnostics")).toArray().at(0).toObject()
                     .value(QStringLiteral("id")).toString(),
             QStringLiteral("session.gui_thread_timeout"));

    // The handler is still holding the GUI thread: further GUI-thread work
    // must be refused, not queued behind it.
    const auto busyResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.content\"") },
    }, 2, &errorMessage);
    QVERIFY2(busyResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(busyResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("diagnostics")).toArray().at(0).toObject()
                     .value(QStringLiteral("id")).toString(),
             QStringLiteral("session.gui_thread_busy"));

    // Wait out the handler, then prove the "timed out" click actually landed:
    // the outcome-unknown wording exists precisely because effects can apply
    // after the timeout was reported.
    const qint64 handlerRemainingMs = 8000 - sinceClick.elapsed();
    if (handlerRemainingMs > 0)
        QTest::qWait(int(handlerRemainingMs) + 500);
    const auto ranResponse = invoke(&client, QStringLiteral("UI.query"), {
        { QStringLiteral("selector"), QStringLiteral("objectName=\"smoke.slowHandlerRan\"") },
    }, 3, &errorMessage);
    QVERIFY2(ranResponse.has_value(), qPrintable(errorMessage));
    QCOMPARE(ranResponse->value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("matches")).toArray().size(),
             1);
}

QTEST_GUILESS_MAIN(QmlAgentIntegrationTest)

#include "tst_qmlagentintegration.moc"
