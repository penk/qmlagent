// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qmlagentmcpoutput.h"

#include <QtCore/qjsonarray.h>
#include <QtCore/qjsonvalue.h>

static QJsonObject summarizedNode(const QJsonObject &node)
{
    QJsonObject summary;
    for (const QString &field : {
             QStringLiteral("nodeId"),
             QStringLiteral("qmlId"),
             QStringLiteral("objectName"),
             QStringLiteral("kind"),
             QStringLiteral("renderKind"),
             QStringLiteral("type"),
             QStringLiteral("text"),
             QStringLiteral("visible"),
             QStringLiteral("enabled"),
             QStringLiteral("bbox"),
             QStringLiteral("insideViewport"),
             QStringLiteral("viewport"),
             QStringLiteral("sourceLocation"),
             QStringLiteral("properties"),
             QStringLiteral("frameworkInternal"),
         }) {
        if (node.contains(field))
            summary.insert(field, node.value(field));
    }
    return summary;
}

static QJsonObject summarizedWaitResult(const QJsonObject &wait)
{
    QJsonObject summary;
    for (const QString &field : {
             QStringLiteral("ok"),
             QStringLiteral("timedOut"),
             QStringLiteral("reason"),
             QStringLiteral("selector"),
             QStringLiteral("until"),
             QStringLiteral("property"),
             QStringLiteral("actual"),
             QStringLiteral("elapsedMs"),
             QStringLiteral("timeoutMs"),
             QStringLiteral("attempts"),
             QStringLiteral("framesObserved"),
             QStringLiteral("matchCount"),
             QStringLiteral("diagnostics"),
             QStringLiteral("nextHints"),
         }) {
        if (wait.contains(field))
            summary.insert(field, wait.value(field));
    }
    if (wait.contains(QStringLiteral("node"))) {
        summary.insert(QStringLiteral("node"),
                       summarizedNode(wait.value(QStringLiteral("node")).toObject()));
        summary.insert(QStringLiteral("moreAvailable"), true);
        summary.insert(QStringLiteral("omittedFields"), QJsonArray{
            QStringLiteral("full wait node fields not listed in summary"),
        });
    }
    return summary;
}

QJsonObject summarizedQueryResult(const QJsonObject &result)
{
    QJsonArray matches;
    const QJsonArray originalMatches = result.value(QStringLiteral("matches")).toArray();
    for (const QJsonValue &match : originalMatches)
        matches.append(summarizedNode(match.toObject()));

    QJsonObject summary{
        { QStringLiteral("matchCount"), originalMatches.size() },
        { QStringLiteral("matches"), matches },
        { QStringLiteral("moreAvailable"), true },
        { QStringLiteral("omittedFields"), QJsonArray{
            QStringLiteral("children"),
            QStringLiteral("selectors"),
            QStringLiteral("full node fields not listed in summary"),
        } },
        { QStringLiteral("nextHints"), QJsonArray{
            QJsonObject{
                { QStringLiteral("tool"), QStringLiteral("qmlagent_ui_query") },
                { QStringLiteral("arguments"), QJsonObject{
                    { QStringLiteral("verbosity"), QStringLiteral("full") },
                } },
                { QStringLiteral("reason"), QStringLiteral("Retrieve full node evidence for the same selector.") },
            },
        } },
    };
    for (const QString &field : {
             QStringLiteral("truncated"),
             QStringLiteral("diagnostics"),
         }) {
        if (result.contains(field))
            summary.insert(field, result.value(field));
    }
    return summary;
}

QJsonObject summarizedQueryManyResult(const QJsonObject &result)
{
    if (!result.contains(QStringLiteral("results")))
        return result;

    QJsonArray summarized;
    const QJsonArray results = result.value(QStringLiteral("results")).toArray();
    for (const QJsonValue &entry : results)
        summarized.append(summarizedQueryResult(entry.toObject()));

    QJsonObject summary{
        { QStringLiteral("results"), summarized },
        { QStringLiteral("resultCount"), summarized.size() },
    };
    if (result.contains(QStringLiteral("diagnostics")))
        summary.insert(QStringLiteral("diagnostics"), result.value(QStringLiteral("diagnostics")));
    return summary;
}

QJsonObject summarizedWorkflowReport(const QJsonObject &report)
{
    QJsonObject inputResult = report.value(QStringLiteral("input")).toObject();
    if (inputResult.contains(QStringLiteral("result")))
        inputResult = inputResult.value(QStringLiteral("result")).toObject();
    QJsonObject input{
        { QStringLiteral("ok"), inputResult.value(QStringLiteral("ok")).toBool() },
        { QStringLiteral("delivered"), inputResult.value(QStringLiteral("delivered")).toBool() },
        { QStringLiteral("reason"), inputResult.value(QStringLiteral("reason")).toString() },
    };
    for (const QString &field : {
             QStringLiteral("verificationRole"),
             QStringLiteral("semanticProof"),
             QStringLiteral("nextHints"),
         }) {
        if (inputResult.contains(field))
            input.insert(field, inputResult.value(field));
    }
    if (inputResult.contains(QStringLiteral("settle")))
        input.insert(QStringLiteral("settle"), inputResult.value(QStringLiteral("settle")));
    if (inputResult.contains(QStringLiteral("diagnostics")))
        input.insert(QStringLiteral("diagnostics"), inputResult.value(QStringLiteral("diagnostics")));
    if (inputResult.contains(QStringLiteral("node")))
        input.insert(QStringLiteral("node"),
                     summarizedNode(inputResult.value(QStringLiteral("node")).toObject()));

    QJsonObject summary{
        { QStringLiteral("kind"), report.value(QStringLiteral("kind")) },
        { QStringLiteral("targetSelector"), report.value(QStringLiteral("targetSelector")) },
        { QStringLiteral("expectedSelector"), report.value(QStringLiteral("expectedSelector")) },
        { QStringLiteral("waitSelector"), report.value(QStringLiteral("waitSelector")) },
        { QStringLiteral("expectation"), report.value(QStringLiteral("expectation")) },
        { QStringLiteral("until"), report.value(QStringLiteral("until")) },
        { QStringLiteral("input"), input },
        { QStringLiteral("verification"), report.value(QStringLiteral("verification")) },
        { QStringLiteral("eventCount"), report.value(QStringLiteral("events")).toArray().size() },
        { QStringLiteral("issues"), report.value(QStringLiteral("issues")) },
        { QStringLiteral("moreAvailable"), true },
        { QStringLiteral("omittedFields"), QJsonArray{
            QStringLiteral("target"),
            QStringLiteral("before"),
            QStringLiteral("after"),
            QStringLiteral("wait"),
            QStringLiteral("events"),
            QStringLiteral("full input node"),
        } },
        { QStringLiteral("nextHints"), QJsonArray{
            QJsonObject{
                { QStringLiteral("tool"), QStringLiteral("same workflow tool") },
                { QStringLiteral("arguments"), QJsonObject{
                    { QStringLiteral("verbosity"), QStringLiteral("full") },
                } },
                { QStringLiteral("reason"), QStringLiteral("Retrieve full target/before/after/event evidence.") },
            },
            QJsonObject{
                { QStringLiteral("tool"), QStringLiteral("qmlagent_ui_query") },
                { QStringLiteral("arguments"), QJsonObject{
                    { QStringLiteral("selector"), report.contains(QStringLiteral("waitSelector"))
                            ? report.value(QStringLiteral("waitSelector"))
                            : report.value(QStringLiteral("expectedSelector")) },
                    { QStringLiteral("verbosity"), QStringLiteral("full") },
                } },
                { QStringLiteral("reason"), QStringLiteral("Drill into the expected/wait post-action node.") },
            },
        } },
    };
    if (report.contains(QStringLiteral("key")))
        summary.insert(QStringLiteral("key"), report.value(QStringLiteral("key")));
    if (report.contains(QStringLiteral("gateway")))
        summary.insert(QStringLiteral("gateway"), report.value(QStringLiteral("gateway")));
    if (report.contains(QStringLiteral("eventStream")))
        summary.insert(QStringLiteral("eventStream"), report.value(QStringLiteral("eventStream")));
    if (report.contains(QStringLiteral("wait")))
        summary.insert(QStringLiteral("wait"),
                       summarizedWaitResult(report.value(QStringLiteral("wait")).toObject()));
    return summary;
}
