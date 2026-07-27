// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qqmlagentprotocol_p.h"
#include "qmlagentmcpprotocol.h"

#include <QtCore/qfile.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qset.h>
#include <QtTest/qtest.h>

class tst_QQmlAgentProtocol : public QObject
{
    Q_OBJECT

private slots:
    void parsesValidRequest();
    void rejectsMalformedJsonAsParseError();
    void rejectsOversizedRequest();
    void rejectsNonObjectJsonAsInvalidRequest();
    void rejectsInvalidParams();
    void formatsResponse();
    void formatsError();
    void formatsEvent();
    void mcpToolResultDoesNotDuplicateStructuredPayload();
    void mcpToolSchemasExposeAgentFirstContracts();
    void agentFacingSurfaceDoesNotAdvertiseRetiredNames();
};

static QJsonObject objectFromJson(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object();
}

void tst_QQmlAgentProtocol::parsesValidRequest()
{
    const auto request = QQmlAgentProtocol::parseRequest(
            R"({"jsonrpc":"2.0","id":7,"method":"UI.getTree","params":{"depth":1}})");

    QVERIFY(request.valid);
    QCOMPARE(request.id.toInt(), 7);
    QCOMPARE(request.method, QStringLiteral("UI.getTree"));
    QCOMPARE(request.params.value(QStringLiteral("depth")).toInt(), 1);
}

void tst_QQmlAgentProtocol::rejectsMalformedJsonAsParseError()
{
    const auto request = QQmlAgentProtocol::parseRequest("{");

    QVERIFY(!request.valid);
    QCOMPARE(request.errorCode, -32700);
    QCOMPARE(request.errorMessage, QStringLiteral("Parse error"));
}

void tst_QQmlAgentProtocol::rejectsOversizedRequest()
{
    const QByteArray payload(QQmlAgentProtocol::MaxInboundMessageBytes + 1, ' ');
    const auto request = QQmlAgentProtocol::parseRequest(payload);

    QVERIFY(!request.valid);
    QCOMPARE(request.errorCode, -32000);
    QCOMPARE(request.errorMessage, QStringLiteral("QmlAgent request payload is too large"));
    QCOMPARE(request.errorData.value(QStringLiteral("maxBytes")).toInt(),
             QQmlAgentProtocol::MaxInboundMessageBytes);
}

void tst_QQmlAgentProtocol::rejectsNonObjectJsonAsInvalidRequest()
{
    const auto request = QQmlAgentProtocol::parseRequest("[]");

    QVERIFY(!request.valid);
    QCOMPARE(request.errorCode, -32600);
    QCOMPARE(request.errorMessage, QStringLiteral("Invalid request"));
}

void tst_QQmlAgentProtocol::rejectsInvalidParams()
{
    const auto request = QQmlAgentProtocol::parseRequest(
            R"({"jsonrpc":"2.0","id":8,"method":"UI.getTree","params":[]})");

    QVERIFY(!request.valid);
    QCOMPARE(request.id.toInt(), 8);
    QCOMPARE(request.errorCode, -32602);
    QCOMPARE(request.errorMessage, QStringLiteral("Invalid params"));
}

void tst_QQmlAgentProtocol::formatsResponse()
{
    const QJsonObject response = objectFromJson(QQmlAgentProtocol::response(
            9, { { QStringLiteral("ok"), true } }));

    QCOMPARE(response.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
    QCOMPARE(response.value(QStringLiteral("id")).toInt(), 9);
    QVERIFY(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("ok")).toBool());
}

void tst_QQmlAgentProtocol::formatsError()
{
    const QJsonObject response = objectFromJson(QQmlAgentProtocol::error(
            10, -32602, QStringLiteral("Invalid params"),
            { { QStringLiteral("field"), QStringLiteral("nodeId") } }));
    const QJsonObject error = response.value(QStringLiteral("error")).toObject();

    QCOMPARE(response.value(QStringLiteral("id")).toInt(), 10);
    QCOMPARE(error.value(QStringLiteral("code")).toInt(), -32602);
    QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("Invalid params"));
    QCOMPARE(error.value(QStringLiteral("data")).toObject().value(QStringLiteral("field")).toString(),
             QStringLiteral("nodeId"));
}

void tst_QQmlAgentProtocol::formatsEvent()
{
    const QJsonObject event = objectFromJson(QQmlAgentProtocol::event(
            QStringLiteral("Log.entryAdded"), { { QStringLiteral("count"), 1 } }));

    QCOMPARE(event.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
    QCOMPARE(event.value(QStringLiteral("method")).toString(), QStringLiteral("Log.entryAdded"));
    QVERIFY(!event.contains(QStringLiteral("id")));
    QCOMPARE(event.value(QStringLiteral("params")).toObject().value(QStringLiteral("count")).toInt(), 1);
}

static QJsonObject mcpToolByName(const QString &name)
{
    const QJsonArray tools = QmlAgentMcp::toolList();
    for (const QJsonValue &toolValue : tools) {
        const QJsonObject tool = toolValue.toObject();
        if (tool.value(QStringLiteral("name")).toString() == name)
            return tool;
    }
    return {};
}

static QSet<QString> mcpToolNames()
{
    QSet<QString> names;
    const QJsonArray tools = QmlAgentMcp::toolList();
    for (const QJsonValue &toolValue : tools)
        names.insert(toolValue.toObject().value(QStringLiteral("name")).toString());
    return names;
}

static QString sourceFileContents(const QString &relativePath)
{
    QFile file(QString::fromUtf8(QMLAGENT_SOURCE_DIR) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

void tst_QQmlAgentProtocol::mcpToolResultDoesNotDuplicateStructuredPayload()
{
    const QJsonObject payload{
        { QStringLiteral("ok"), true },
        { QStringLiteral("largeEvidence"), QStringLiteral("this must stay out of text content") },
    };
    const QJsonObject result = QmlAgentMcp::toolResult(payload);

    QCOMPARE(result.value(QStringLiteral("structuredContent")).toObject(), payload);
    const QString text = result.value(QStringLiteral("content")).toArray().first().toObject()
            .value(QStringLiteral("text")).toString();
    QVERIFY(text.contains(QStringLiteral("structuredContent")));
    QVERIFY(!text.contains(QStringLiteral("largeEvidence")));
}

void tst_QQmlAgentProtocol::mcpToolSchemasExposeAgentFirstContracts()
{
    const QJsonArray tools = QmlAgentMcp::toolList();
    QVERIFY(!tools.isEmpty());

    QSet<QString> names;
    for (const QJsonValue &toolValue : tools) {
        const QJsonObject tool = toolValue.toObject();
        const QString name = tool.value(QStringLiteral("name")).toString();
        QVERIFY2(!name.isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument(tool).toJson())));
        QVERIFY2(!names.contains(name), qPrintable(name));
        names.insert(name);
        QCOMPARE(tool.value(QStringLiteral("inputSchema")).toObject()
                         .value(QStringLiteral("additionalProperties")).toBool(true),
                 false);
    }

    const QJsonObject screenshot = mcpToolByName(QStringLiteral("qmlagent_render_capture_screenshot"));
    QVERIFY(!screenshot.isEmpty());
    QVERIFY2(screenshot.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("Fallback visual evidence")),
             qPrintable(QJsonDocument(screenshot).toJson(QJsonDocument::Compact)));
    const QJsonObject screenshotProperties = screenshot.value(QStringLiteral("inputSchema"))
            .toObject().value(QStringLiteral("properties")).toObject();
    QVERIFY(screenshotProperties.contains(QStringLiteral("includeData")));
    QVERIFY(screenshotProperties.contains(QStringLiteral("scale")));
    QVERIFY(screenshotProperties.contains(QStringLiteral("region")));

    const QJsonObject workflowKey = mcpToolByName(QStringLiteral("qmlagent_workflow_key"));
    QVERIFY(!workflowKey.isEmpty());
    QVERIFY2(workflowKey.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("startsWith")),
             qPrintable(QJsonDocument(workflowKey).toJson(QJsonDocument::Compact)));
    QVERIFY2(workflowKey.value(QStringLiteral("inputSchema")).toObject()
                    .value(QStringLiteral("properties")).toObject()
                    .value(QStringLiteral("expect")).toObject()
                    .value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("contains")),
             qPrintable(QJsonDocument(workflowKey).toJson(QJsonDocument::Compact)));

    const QJsonObject uiWait = mcpToolByName(QStringLiteral("qmlagent_ui_wait_for"));
    QVERIFY(!uiWait.isEmpty());
    const QJsonArray waitOperators = uiWait.value(QStringLiteral("inputSchema")).toObject()
            .value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("until")).toObject()
            .value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("op")).toObject()
            .value(QStringLiteral("enum")).toArray();
    QVERIFY(waitOperators.contains(QStringLiteral("contains")));
    QVERIFY(waitOperators.contains(QStringLiteral("startsWith")));
    QVERIFY(waitOperators.contains(QStringLiteral("endsWith")));

    const QJsonObject inputClick = mcpToolByName(QStringLiteral("qmlagent_input_click"));
    QVERIFY(!inputClick.isEmpty());
    QVERIFY2(inputClick.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("input delivery only")),
             qPrintable(QJsonDocument(inputClick).toJson(QJsonDocument::Compact)));
    QVERIFY2(inputClick.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("settle.timedOut")),
             qPrintable(QJsonDocument(inputClick).toJson(QJsonDocument::Compact)));
    QVERIFY(inputClick.value(QStringLiteral("inputSchema")).toObject()
                    .value(QStringLiteral("properties")).toObject()
                    .contains(QStringLiteral("settle")));

    // target_status is the entry point; per-tool steering lives in the
    // in-band agentToolGuide payload, not in this description.
    const QJsonObject targetStatus = mcpToolByName(QStringLiteral("qmlagent_target_status"));
    QVERIFY(!targetStatus.isEmpty());
    const QString targetStatusDescription = targetStatus.value(QStringLiteral("description")).toString();
    QVERIFY2(targetStatusDescription.contains(QStringLiteral("Start here")),
             qPrintable(targetStatusDescription));
    QVERIFY2(targetStatusDescription.contains(QStringLiteral("launcherGateway.sessions")),
             qPrintable(targetStatusDescription));

    const QJsonObject queryMany = mcpToolByName(QStringLiteral("qmlagent_ui_query_many"));
    QVERIFY(!queryMany.isEmpty());
    QVERIFY2(queryMany.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("Batch verification reads")),
             qPrintable(QJsonDocument(queryMany).toJson(QJsonDocument::Compact)));
    const QJsonObject queryManyItemProperties = queryMany.value(QStringLiteral("inputSchema"))
            .toObject().value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("queries")).toObject()
            .value(QStringLiteral("items")).toObject()
            .value(QStringLiteral("properties")).toObject();
    const QString oldQuick3DFlag = QStringLiteral("include") + QStringLiteral("3D");
    QVERIFY(!queryManyItemProperties.contains(oldQuick3DFlag));
    const QJsonObject getTree = mcpToolByName(QStringLiteral("qmlagent_ui_get_tree"));
    QVERIFY(!getTree.isEmpty());
    const QJsonObject getTreeProperties = getTree.value(QStringLiteral("inputSchema"))
            .toObject().value(QStringLiteral("properties")).toObject();
    QVERIFY(!getTreeProperties.contains(oldQuick3DFlag));
    QVERIFY2(getTree.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("QtQuick3D")),
             qPrintable(QJsonDocument(getTree).toJson(QJsonDocument::Compact)));
    QVERIFY2(getTree.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("not QQuickItem click targets")),
             qPrintable(QJsonDocument(getTree).toJson(QJsonDocument::Compact)));
    QVERIFY2(getTree.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("render3D")),
             qPrintable(QJsonDocument(getTree).toJson(QJsonDocument::Compact)));
    const QJsonObject query = mcpToolByName(QStringLiteral("qmlagent_ui_query"));
    QVERIFY(!query.isEmpty());
    QVERIFY(!query.value(QStringLiteral("inputSchema")).toObject()
                    .value(QStringLiteral("properties")).toObject()
                    .contains(oldQuick3DFlag));
    QVERIFY2(query.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("QtQuick3D")),
             qPrintable(QJsonDocument(query).toJson(QJsonDocument::Compact)));
    QVERIFY2(query.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("projected screen bounds")),
             qPrintable(QJsonDocument(query).toJson(QJsonDocument::Compact)));

    const QJsonObject scrollIntoView =
            mcpToolByName(QStringLiteral("qmlagent_input_scroll_into_view"));
    QVERIFY(!scrollIntoView.isEmpty());
    const QString scrollDescription = scrollIntoView.value(QStringLiteral("description")).toString();
    QVERIFY2(scrollDescription.contains(QStringLiteral("center_outside_viewport")),
             qPrintable(QJsonDocument(scrollIntoView).toJson(QJsonDocument::Compact)));

    const QJsonObject workflowClickAndWait =
            mcpToolByName(QStringLiteral("qmlagent_workflow_click_and_wait"));
    QVERIFY(!workflowClickAndWait.isEmpty());
    QVERIFY2(workflowClickAndWait.value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("Agent-first compressed workflow")),
             qPrintable(QJsonDocument(workflowClickAndWait).toJson(QJsonDocument::Compact)));

    const QJsonObject diagnosticsTree = mcpToolByName(QStringLiteral("qmlagent_diagnostics_analyze_tree"));
    QVERIFY(!diagnosticsTree.isEmpty());
    const QJsonObject diagnosticsProperties = diagnosticsTree.value(QStringLiteral("inputSchema"))
            .toObject().value(QStringLiteral("properties")).toObject();
    QCOMPARE(diagnosticsProperties.value(QStringLiteral("verbosity")).toObject()
                     .value(QStringLiteral("default")).toString(),
             QStringLiteral("summary"));
    QVERIFY(diagnosticsProperties.value(QStringLiteral("maxIssues")).isObject());
}

void tst_QQmlAgentProtocol::agentFacingSurfaceDoesNotAdvertiseRetiredNames()
{
    const QSet<QString> toolNames = mcpToolNames();
    QVERIFY(toolNames.contains(QStringLiteral("qmlagent_input_type_text")));
    QVERIFY(toolNames.contains(QStringLiteral("qmlagent_input_clear_text")));
    QVERIFY(!toolNames.contains(QStringLiteral("qmlagent_input_replace_text")));

    const QString readme = sourceFileContents(QStringLiteral("README.md"));
    const QString skill = sourceFileContents(QStringLiteral("skills/qmlagent-runtime/SKILL.md"));
    const QString cli = sourceFileContents(QStringLiteral("tools/qmlagent/main.cpp"));
    QVERIFY2(!readme.isEmpty(), "README.md must be readable for surface drift checks.");
    QVERIFY2(!skill.isEmpty(), "qmlagent-runtime skill must be readable for surface drift checks.");
    QVERIFY2(!cli.isEmpty(), "qmlagentctl source must be readable for surface drift checks.");

    const QStringList agentFacingFiles{ readme, skill, cli };
    for (const QString &contents : agentFacingFiles) {
        QVERIFY2(!contents.contains(QStringLiteral("replace-text")),
                 qPrintable(contents.mid(qMax(0, contents.indexOf(QStringLiteral("replace-text")) - 80), 160)));
        QVERIFY2(!contents.contains(QStringLiteral("input_replace_text")),
                 qPrintable(contents.mid(qMax(0, contents.indexOf(QStringLiteral("input_replace_text")) - 80), 160)));
    }

    QVERIFY(readme.contains(QStringLiteral("clear-text")));
    QVERIFY(readme.contains(QStringLiteral("type 'id=\"urlField\"' --text")));
    QVERIFY(readme.contains(QStringLiteral("qmlagent_ui_query_many")));
    QVERIFY(readme.contains(QStringLiteral("qmlagent_input_scroll_into_view")));
    QVERIFY(readme.contains(QStringLiteral("qmlagentctl\" methods")));
    QVERIFY(readme.contains(QStringLiteral("query-many --params")));
    QVERIFY(readme.contains(QStringLiteral("scroll-into-view 'id=\"saveButton\"'")));
    QVERIFY(skill.contains(QStringLiteral("qmlagent_input_clear_text")));
    QVERIFY(skill.contains(QStringLiteral("qmlagent_input_type_text")));
    QVERIFY(skill.contains(QStringLiteral("qmlagent_ui_query_many")));
    QVERIFY(skill.contains(QStringLiteral("qmlagent_input_scroll_into_view")));
    QVERIFY(skill.contains(QStringLiteral("qmlagentctl\" methods")));
    QVERIFY(cli.contains(QStringLiteral("qmlagentctl methods")));
    QVERIFY(cli.contains(QStringLiteral("qmlagentctl capabilities")));
    QVERIFY(cli.contains(QStringLiteral("qmlagentctl type <selector> --text value")));
    QVERIFY(cli.contains(QStringLiteral("qmlagentctl clear-text <selector>")));
    QVERIFY(cli.contains(QStringLiteral("qmlagentctl query-many --params")));
    QVERIFY(cli.contains(QStringLiteral("qmlagentctl scroll-into-view <selector>")));
    QVERIFY(cli.contains(QStringLiteral("UI.queryMany")));
    QVERIFY(cli.contains(QStringLiteral("Input.scrollIntoView")));
    QVERIFY(cli.contains(QStringLiteral("center_outside_viewport")));
}

QTEST_GUILESS_MAIN(tst_QQmlAgentProtocol)

#include "tst_qmlagentprotocol.moc"
