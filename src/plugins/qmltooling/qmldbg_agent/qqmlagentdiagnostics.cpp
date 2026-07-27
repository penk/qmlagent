// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qqmlagentdiagnostics_p.h"

#include "qqmlagentactionability_p.h"
#include "qqmlagentjsonutils_p.h"
#include "qqmlagentlogcollector_p.h"
#include "qqmlagentsourceresolver_p.h"
#include "qqmlagentuitree_p.h"

#include <QtCore/qjsonarray.h>
#include <QtCore/qobject.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qset.h>
#include <QtCore/qstringlist.h>
#include <QtCore/qvariant.h>
#include <QtCore/qvector.h>
#include <QtGui/qvector3d.h>
#include <QtQuick/qquickitem.h>
#include <QtQuick/qquickwindow.h>
#include <QtQuick/private/qquickrepeater_p.h>
#include <QtQml/qqmllist.h>

#ifdef QMLAGENT_HAS_QUICK3D
#include <QtQuick3D/qquick3dobject.h>
#endif

#include <algorithm>

QT_BEGIN_NAMESPACE

static constexpr qreal LargeSpacerViewportRatio = 0.25;
static constexpr qreal LargeSpacerParentRatio = 0.40;

static QJsonArray rectArray(const QRectF &rect)
{
    return { rect.x(), rect.y(), rect.width(), rect.height() };
}

static QRectF itemBoxInWindow(const QQuickItem *item)
{
    if (!item)
        return {};
    return item->mapRectToScene(QRectF(QPointF(0, 0), QSizeF(item->width(), item->height())));
}

static QJsonObject issue(const QString &id, const QString &severity, double confidence,
                         int nodeId, const QString &message, const QJsonArray &evidence,
                         const QJsonObject &sourceLocation = {})
{
    QJsonObject object{
        { QStringLiteral("id"), id },
        { QStringLiteral("severity"), severity },
        { QStringLiteral("confidence"), confidence },
        { QStringLiteral("message"), message },
        { QStringLiteral("evidence"), evidence },
    };
    if (nodeId >= 0)
        object.insert(QStringLiteral("nodeId"), nodeId);
    if (!sourceLocation.isEmpty())
        object.insert(QStringLiteral("sourceLocation"), sourceLocation);
    return object;
}

static QJsonObject evidenceProfile(const QString &kind, const QString &basis,
                                   const QJsonArray &limitations = {})
{
    QJsonObject object{
        { QStringLiteral("kind"), kind },
        { QStringLiteral("basis"), basis },
    };
    if (!limitations.isEmpty())
        object.insert(QStringLiteral("limitations"), limitations);
    return object;
}

static QJsonObject repairHint(const QString &kind, double confidence, const QString &reason,
                              const QJsonObject &details = {})
{
    QJsonObject hint{
        { QStringLiteral("kind"), kind },
        { QStringLiteral("confidence"), confidence },
        { QStringLiteral("reason"), reason },
    };
    for (auto it = details.constBegin(); it != details.constEnd(); ++it)
        hint.insert(it.key(), it.value());
    return hint;
}

static QJsonObject compactBindingProvenance(QObject *object, const QString &property)
{
    const QJsonObject analysis =
            QQmlAgentSourceResolver::bindingProvenanceForProperty(object, property);
    if (!analysis.value(QStringLiteral("ok")).toBool(false))
        return {};

    const QJsonObject provenance = analysis.value(QStringLiteral("provenance")).toObject();
    if (provenance.isEmpty())
        return {};

    const QString kind = provenance.value(QStringLiteral("kind")).toString();
    const bool hasExpression = provenance.contains(QStringLiteral("expression"));
    const bool hasSourceAssignment = provenance.contains(QStringLiteral("sourceAssignment"));
    if (kind != QLatin1String("binding") && !hasExpression && !hasSourceAssignment)
        return {};

    QJsonObject compact{
        { QStringLiteral("property"), property },
        { QStringLiteral("value"), analysis.value(QStringLiteral("value")) },
        { QStringLiteral("kind"), kind },
        { QStringLiteral("confidence"), provenance.value(QStringLiteral("confidence")) },
    };
    if (provenance.contains(QStringLiteral("bindingKind")))
        compact.insert(QStringLiteral("bindingKind"), provenance.value(QStringLiteral("bindingKind")));
    if (hasExpression)
        compact.insert(QStringLiteral("expression"), provenance.value(QStringLiteral("expression")));
    if (hasSourceAssignment)
        compact.insert(QStringLiteral("sourceAssignment"),
                       provenance.value(QStringLiteral("sourceAssignment")));
    if (provenance.contains(QStringLiteral("sourceLocation")))
        compact.insert(QStringLiteral("sourceLocation"),
                       provenance.value(QStringLiteral("sourceLocation")));
    if (provenance.contains(QStringLiteral("candidateIdentifiers")))
        compact.insert(QStringLiteral("candidateIdentifiers"),
                       provenance.value(QStringLiteral("candidateIdentifiers")));
    if (provenance.contains(QStringLiteral("dependencies")))
        compact.insert(QStringLiteral("dependencies"), provenance.value(QStringLiteral("dependencies")));
    if (provenance.contains(QStringLiteral("dependencySummary")))
        compact.insert(QStringLiteral("dependencySummary"),
                       provenance.value(QStringLiteral("dependencySummary")));
    if (provenance.contains(QStringLiteral("dependencyLimitations")))
        compact.insert(QStringLiteral("dependencyLimitations"),
                       provenance.value(QStringLiteral("dependencyLimitations")));
    if (provenance.contains(QStringLiteral("limitations")))
        compact.insert(QStringLiteral("limitations"), provenance.value(QStringLiteral("limitations")));
    return compact;
}

static void attachBindingProvenance(QJsonObject *issue, QObject *object,
                                    const QStringList &properties)
{
    QJsonArray evidence;
    bool hasBinding = false;
    for (const QString &property : properties) {
        const QJsonObject provenance = compactBindingProvenance(object, property);
        if (!provenance.isEmpty()) {
            hasBinding = hasBinding
                    || provenance.value(QStringLiteral("kind")).toString()
                    == QLatin1String("binding");
            evidence.append(provenance);
        }
    }
    if (!evidence.isEmpty()) {
        issue->insert(QStringLiteral("bindingProvenance"), evidence);

        const QJsonObject primary = evidence.first().toObject();
        const QString primaryExpression =
                primary.value(QStringLiteral("expression")).toString(
                        primary.value(QStringLiteral("sourceAssignment")).toObject()
                                .value(QStringLiteral("expression")).toString());

        QJsonArray hints = issue->value(QStringLiteral("repairHints")).toArray();
        QJsonObject details{
            { QStringLiteral("properties"), properties.join(QLatin1Char(',')) },
            { QStringLiteral("primaryProperty"), primary.value(QStringLiteral("property")) },
            { QStringLiteral("primaryKind"), primary.value(QStringLiteral("kind")) },
            { QStringLiteral("suggestedDirection"),
              hasBinding ? QStringLiteral("patch-computed-property-binding")
                         : QStringLiteral("patch-authored-property-assignment") },
        };
        if (!primaryExpression.isEmpty())
            details.insert(QStringLiteral("primaryExpression"), primaryExpression);
        hints.append(repairHint(
                hasBinding ? QStringLiteral("inspect-binding-provenance")
                           : QStringLiteral("inspect-source-assignment"),
                hasBinding ? 0.80 : 0.70,
                hasBinding
                        ? QStringLiteral("One or more failed runtime properties are computed by QML bindings.")
                        : QStringLiteral("One or more failed runtime properties have readable QML source assignments."),
                details));
        issue->insert(QStringLiteral("repairHints"), hints);
    }
}

static QString objectLabel(const QObject *object)
{
    if (!object)
        return {};
    if (!object->objectName().isEmpty())
        return object->objectName();
    return QString::fromUtf8(object->metaObject()->className());
}

static bool isAnonymousLayoutFiller(const QJsonObject &node, const QQuickItem *item)
{
    if (!item || !item->isVisible() || !item->childItems().isEmpty())
        return false;

    if (item->metaObject() != &QQuickItem::staticMetaObject)
        return false;

    if (!node.value(QStringLiteral("objectName")).toString().isEmpty()
        || !node.value(QStringLiteral("qmlId")).toString().isEmpty()) {
        return false;
    }

    const bool zeroWidth = item->width() <= 0.5;
    const bool zeroHeight = item->height() <= 0.5;
    return zeroWidth != zeroHeight;
}

static bool isTreeDiagnosticInfrastructure(const QJsonObject &node)
{
    const int nodeId = node.value(QStringLiteral("nodeId")).toInt(-1);
    QObject *object = QQmlAgentUiTree::objectForNodeId(nodeId);
    if (!object)
        return false;

    if (qobject_cast<QQuickRepeater *>(object))
        return true;

    return isAnonymousLayoutFiller(node, qobject_cast<QQuickItem *>(object));
}

static bool isQtFrameworkSource(const QString &file)
{
    return file.startsWith(QLatin1String("qrc:/qt-project.org/imports/"))
            || file.startsWith(QLatin1String("qrc:/qt/qml/QtQuick/"))
            || file.contains(QLatin1String("/QtQuick/Controls/"));
}

static bool isFrameworkOwnedIssue(const QJsonObject &node, const QJsonObject &issue)
{
    const QString file = issue.value(QStringLiteral("sourceLocation")).toObject()
            .value(QStringLiteral("file")).toString();
    if (isQtFrameworkSource(file))
        return true;

    if (!file.isEmpty())
        return false;

    const QString nodeFile = node.value(QStringLiteral("sourceLocation")).toObject()
            .value(QStringLiteral("file")).toString();
    if (isQtFrameworkSource(nodeFile))
        return true;

    if (!nodeFile.isEmpty())
        return false;

    // Application-scope analysis should not blame anonymous framework/style
    // implementation items that have no authored source or stable app selector.
    return node.value(QStringLiteral("qmlId")).toString().isEmpty()
            && node.value(QStringLiteral("objectName")).toString().isEmpty();
}

static bool isApplicationSourceLocation(const QJsonObject &sourceLocation)
{
    const QString file = sourceLocation.value(QStringLiteral("file")).toString();
    return !file.isEmpty() && !isQtFrameworkSource(file);
}

static bool isViewportContainer(const QQuickItem *item)
{
    if (!item)
        return false;

    const QString className = QString::fromUtf8(item->metaObject()->className());
    return item->inherits("QQuickFlickable")
            || item->inherits("QQuickListView")
            || item->inherits("QQuickGridView")
            || item->inherits("QQuickTableView")
            || item->inherits("QQuickTreeView")
            || item->inherits("QQuickPathView")
            || className.contains(QLatin1String("Flickable"))
            || className.contains(QLatin1String("ListView"))
            || className.contains(QLatin1String("GridView"))
            || className.contains(QLatin1String("TableView"))
            || className.contains(QLatin1String("TreeView"))
            || className.contains(QLatin1String("PathView"));
}

static bool isDelegateTemplateSourceLocation(const QJsonObject &sourceLocation)
{
    return sourceLocation.value(QStringLiteral("method")).toString()
            == QLatin1String("qqmldata-delegate");
}

static QString frameworkSuppressionReason(const QJsonObject &node, const QJsonObject &issue)
{
    const QString file = issue.value(QStringLiteral("sourceLocation")).toObject()
            .value(QStringLiteral("file")).toString();
    const QString nodeFile = node.value(QStringLiteral("sourceLocation")).toObject()
            .value(QStringLiteral("file")).toString();
    if (isQtFrameworkSource(file) || isQtFrameworkSource(nodeFile))
        return QStringLiteral("framework-owned-issue-outside-application-scope");
    return QStringLiteral("anonymous-node-without-authored-source-outside-application-scope");
}

static QJsonObject suppressedFrameworkIssueSummary(const QJsonObject &node,
                                                   const QJsonObject &issue)
{
    QJsonObject summary{
        { QStringLiteral("id"), issue.value(QStringLiteral("id")) },
        { QStringLiteral("nodeId"), issue.value(QStringLiteral("nodeId")) },
        { QStringLiteral("type"), node.value(QStringLiteral("type")) },
        { QStringLiteral("issueOwner"), QStringLiteral("qt-framework") },
        { QStringLiteral("reason"), frameworkSuppressionReason(node, issue) },
    };
    if (node.contains(QStringLiteral("qmlId")))
        summary.insert(QStringLiteral("qmlId"), node.value(QStringLiteral("qmlId")));
    if (issue.contains(QStringLiteral("sourceLocation")))
        summary.insert(QStringLiteral("sourceLocation"), issue.value(QStringLiteral("sourceLocation")));
    return summary;
}

static QJsonObject compactNodeSnapshot(const QJsonObject &node)
{
    QJsonObject compact;
    const QStringList fields{
        QStringLiteral("nodeId"),
        QStringLiteral("qmlId"),
        QStringLiteral("objectName"),
        QStringLiteral("type"),
        QStringLiteral("text"),
        QStringLiteral("bbox"),
        QStringLiteral("insideViewport"),
        QStringLiteral("sourceLocation"),
    };
    for (const QString &field : fields) {
        if (node.contains(field))
            compact.insert(field, node.value(field));
    }
    return compact;
}

static QJsonObject compactIssueSummary(const QJsonObject &issue)
{
    QJsonObject compact;
    const QStringList fields{
        QStringLiteral("id"),
        QStringLiteral("severity"),
        QStringLiteral("confidence"),
        QStringLiteral("nodeId"),
        QStringLiteral("message"),
        QStringLiteral("sourceLocation"),
        QStringLiteral("bbox"),
        QStringLiteral("viewport"),
        QStringLiteral("property"),
    };
    for (const QString &field : fields) {
        if (issue.contains(field))
            compact.insert(field, issue.value(field));
    }
    if (issue.contains(QStringLiteral("target")))
        compact.insert(QStringLiteral("target"),
                       compactNodeSnapshot(issue.value(QStringLiteral("target")).toObject()));
    return compact;
}

static QJsonArray compactIssueSummaries(const QJsonArray &issues, int maxIssues)
{
    QJsonArray compact;
    const int limit = qBound(0, maxIssues, issues.size());
    for (int i = 0; i < limit; ++i)
        compact.append(compactIssueSummary(issues.at(i).toObject()));
    return compact;
}

static QJsonArray analyzeTreeSummaryOmittedFields()
{
    return {
        QStringLiteral("evidence"),
        QStringLiteral("blameChain"),
        QStringLiteral("repairHints"),
        QStringLiteral("bindingProvenance"),
        QStringLiteral("target.children"),
    };
}

static QJsonObject blameEntry(const QQuickItem *item, const QString &evidence)
{
    QObject *object = const_cast<QQuickItem *>(item);
    return {
        { QStringLiteral("nodeId"), QQmlAgentUiTree::nodeForObject(object, 0, 0, true, false, {})
                .value(QStringLiteral("nodeId")).toInt(-1) },
        { QStringLiteral("type"), QString::fromUtf8(object->metaObject()->className()) },
        { QStringLiteral("sourceLocation"),
          QQmlAgentSourceResolver::sourceLocationForObject(object) },
        { QStringLiteral("evidence"), evidence },
    };
}

static QString bboxEvidence(const QString &label, const QRectF &rect)
{
    return QStringLiteral("%1=[%2,%3,%4,%5]")
            .arg(label)
            .arg(rect.x())
            .arg(rect.y())
            .arg(rect.width())
            .arg(rect.height());
}

static bool actionabilityHasReason(const QJsonArray &reasons, const QString &reasonId,
                                   QJsonObject *matchedReason = nullptr)
{
    for (const QJsonValue &reasonValue : reasons) {
        const QJsonObject reason = reasonValue.toObject();
        if (reason.value(QStringLiteral("id")).toString() != reasonId)
            continue;
        if (matchedReason)
            *matchedReason = reason;
        return true;
    }
    return false;
}

static QQuickItem *nearestAppAuthoredAncestor(QQuickItem *item, bool includeDelegateTemplates)
{
    for (QQuickItem *ancestor = item ? item->parentItem() : nullptr; ancestor;
         ancestor = ancestor->parentItem()) {
        if (isViewportContainer(ancestor))
            return nullptr;
        const QJsonObject source = QQmlAgentSourceResolver::sourceLocationForObject(ancestor);
        if (!isApplicationSourceLocation(source))
            continue;
        if (!includeDelegateTemplates && isDelegateTemplateSourceLocation(source))
            continue;
        return ancestor;
    }
    return nullptr;
}

static QJsonObject overlapIssueForNode(const QString &message, int nodeId,
                                       const QJsonObject &source, const QRectF &bbox,
                                       const QJsonArray &reasons,
                                       const QJsonObject &blockedReason,
                                       const QJsonObject &targetNode = {},
                                       const QJsonArray &extraEvidence = {})
{
    QJsonArray evidence{
        bboxEvidence(QStringLiteral("bbox"), bbox),
    };
    for (const QJsonValue &value : extraEvidence)
        evidence.append(value);
    const QJsonArray reasonEvidence = blockedReason.value(QStringLiteral("evidence")).toArray();
    for (const QJsonValue &value : reasonEvidence)
        evidence.append(value);

    QJsonObject overlapIssue = issue(
            QStringLiteral("layout.overlap"), QStringLiteral("warning"), 0.60,
            nodeId, message, evidence, source);
    overlapIssue.insert(QStringLiteral("bbox"), rectArray(bbox));
    overlapIssue.insert(QStringLiteral("evidenceProfile"),
                        evidenceProfile(QStringLiteral("center-point-hit-test"),
                                        QStringLiteral("paint-order hit test at target center"),
                                        {
                                            QStringLiteral("does not prove full-area occlusion"),
                                            QStringLiteral("does not detect edge-only or transparent blockers"),
                                            QStringLiteral("blame direction is approximate"),
                                        }));
    if (!targetNode.isEmpty())
        overlapIssue.insert(QStringLiteral("target"), targetNode);
    overlapIssue.insert(QStringLiteral("actionability"), QJsonObject{
        { QStringLiteral("ok"), false },
        { QStringLiteral("reasons"), reasons },
        { QStringLiteral("limitations"), QJsonArray{
            QStringLiteral("overlap evidence uses center-point paint-order hit testing"),
            QStringLiteral("intentional overlays and transitions can overlap by design"),
            QStringLiteral("center hit testing identifies the top item at one point; it does not prove full-area occlusion or blame direction"),
            QStringLiteral("use screenshots or QSG visualization only as fallback visual evidence"),
        } },
    });
    overlapIssue.insert(QStringLiteral("patchDirection"),
                        QStringLiteral("Constrain or clip the moving/overlapping content, adjust z/order, or keep inactive pages inside their visible viewport."));
    overlapIssue.insert(QStringLiteral("repairHints"), QJsonArray{
        repairHint(QStringLiteral("clip-moving-content"), 0.75,
                   QStringLiteral("The item is visible outside the region where it is expected to appear."),
                   {
                       { QStringLiteral("suggestedDirection"), QStringLiteral("enable-clipping-or-constrain-content") },
                   }),
        repairHint(QStringLiteral("inspect-z-order"), 0.65,
                   QStringLiteral("Paint-order evidence says another item covers the target center."),
                   {
                       { QStringLiteral("suggestedDirection"), QStringLiteral("adjust-z-or-layout-order") },
                   }),
    });
    return overlapIssue;
}

static bool boolProperty(const QObject *object, const char *name, bool *value)
{
    const int index = object ? object->metaObject()->indexOfProperty(name) : -1;
    if (index < 0)
        return false;

    *value = object->property(name).toBool();
    return true;
}

static QJsonValue propertyValue(const QObject *object, const char *name)
{
    const int index = object ? object->metaObject()->indexOfProperty(name) : -1;
    if (index < 0)
        return QJsonValue();

    const QVariant value = object->property(name);
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
    default:
        return QJsonValue::fromVariant(value);
    }
}

static QQuickItem *disabledAncestor(QQuickItem *item)
{
    for (QQuickItem *ancestor = item ? item->parentItem() : nullptr; ancestor;
         ancestor = ancestor->parentItem()) {
        bool enabled = true;
        if (boolProperty(ancestor, "enabled", &enabled) && !enabled)
            return ancestor;
    }

    return nullptr;
}

static QJsonArray viewportBlameChain(const QQuickItem *item, const QRectF &viewport)
{
    QJsonArray blameChain;
    const QRectF bbox = itemBoxInWindow(item);
    blameChain.append(blameEntry(item, QStringLiteral("%1; viewport=[0,0,%2,%3]")
                                 .arg(bboxEvidence(QStringLiteral("bbox"), bbox))
                                 .arg(viewport.width())
                                 .arg(viewport.height())));

    for (const QQuickItem *ancestor = item->parentItem(); ancestor;
         ancestor = ancestor->parentItem()) {
        const QRectF ancestorBox = itemBoxInWindow(ancestor);
        if (!viewport.contains(ancestorBox)) {
            blameChain.append(blameEntry(ancestor, QStringLiteral("%1 also exceeds viewport")
                                         .arg(bboxEvidence(QStringLiteral("ancestorBbox"),
                                                           ancestorBox))));
        }
    }

    return blameChain;
}

static int editDistance(const QString &left, const QString &right)
{
    if (left.isEmpty())
        return right.size();
    if (right.isEmpty())
        return left.size();

    QVector<int> previous(right.size() + 1);
    QVector<int> current(right.size() + 1);
    for (int i = 0; i <= right.size(); ++i)
        previous[i] = i;

    for (int i = 1; i <= left.size(); ++i) {
        current[0] = i;
        for (int j = 1; j <= right.size(); ++j) {
            const int substitutionCost = left.at(i - 1) == right.at(j - 1) ? 0 : 1;
            current[j] = std::min({
                previous[j] + 1,
                current[j - 1] + 1,
                previous[j - 1] + substitutionCost,
            });
        }
        previous.swap(current);
    }

    return previous[right.size()];
}

static double similarity(const QString &left, const QString &right)
{
    const int length = std::max(left.size(), right.size());
    if (length == 0)
        return 1.0;
    return 1.0 - double(editDistance(left, right)) / double(length);
}

static bool parsePositiveInt(const QString &text, int *value)
{
    bool ok = false;
    const int parsed = text.toInt(&ok);
    if (!ok || parsed <= 0)
        return false;
    *value = parsed;
    return true;
}

static QSet<QString> requestedChecks(const QJsonObject &params)
{
    QSet<QString> checks;
    const QJsonArray values = params.value(QStringLiteral("checks")).toArray();
    for (const QJsonValue &value : values) {
        QString check = value.toString();
        if (check == QLatin1String("layout.minimum_size"))
            check = QStringLiteral("minSize");
        else if (check == QLatin1String("layout.viewport"))
            check = QStringLiteral("insideViewport");
        else if (check == QLatin1String("layout.clipping"))
            check = QStringLiteral("childExceedsParent");
        else if (check == QLatin1String("layout.overlap"))
            check = QStringLiteral("overlap");
        else if (check == QLatin1String("layout.excessive_spacer"))
            check = QStringLiteral("excessiveSpacer");
        else if (check == QLatin1String("input.actionability"))
            check = QStringLiteral("actionable");
        else if (check == QLatin1String("text.elide"))
            check = QStringLiteral("textElided");
        else if (check == QLatin1String("quick3d") || check == QLatin1String("quick3D")
                 || check == QLatin1String("quick3d.scene"))
            check = QStringLiteral("quick3D");
        if (!check.isEmpty())
            checks.insert(check);
    }
    return checks;
}

static bool checkRequested(const QSet<QString> &checks, const QString &check)
{
    return checks.isEmpty() || checks.contains(check);
}

static bool checkExplicitlyRequested(const QSet<QString> &checks, const QString &check)
{
    return checks.contains(check);
}

static bool isIdentifierLikeShortText(const QString &text)
{
    if (text.isEmpty() || text.size() > 32)
        return false;

    static const QRegularExpression ligaturePattern(
            QStringLiteral("^[a-z][a-z0-9_]*$"));
    return ligaturePattern.match(text).hasMatch();
}

static bool hasWeakDefaultTextElideEvidence(const QString &text,
                                            const QJsonValue &contentWidth)
{
    if (!isIdentifierLikeShortText(text))
        return false;
    if (contentWidth.isUndefined())
        return false;

    bool ok = false;
    const double width = contentWidth.toVariant().toDouble(&ok);
    return ok && width <= 0.5;
}

static bool evidenceContains(const QJsonObject &reason, QLatin1String needle)
{
    const QJsonArray evidence = reason.value(QStringLiteral("evidence")).toArray();
    for (const QJsonValue &value : evidence) {
        if (value.toString().contains(needle))
            return true;
    }
    return false;
}

static bool looksLikeIntentionalOverlayBlocker(QQuickItem *item, const QRectF &bbox,
                                               const QJsonObject &blockedReason)
{
    if (!item || !item->window())
        return false;
    if (!evidenceContains(blockedReason, QLatin1String("blockingItem=QQuickMouseArea")))
        return false;

    const QSize windowSize = item->window()->size();
    const QRectF viewport(QPointF(0, 0), QSizeF(windowSize));
    if (viewport.isEmpty())
        return false;

    const qreal targetArea = bbox.width() * bbox.height();
    const qreal viewportArea = viewport.width() * viewport.height();
    return targetArea >= viewportArea * 0.45;
}

static QJsonArray checksArray(const QSet<QString> &checks)
{
    QJsonArray values;
    for (const QString &check : checks)
        values.append(check);
    return values;
}

static QJsonArray ranChecks(const QSet<QString> &checks)
{
    const QJsonArray all{
        QStringLiteral("layout.minimum_size"),
        QStringLiteral("layout.viewport"),
        QStringLiteral("layout.clipping"),
        QStringLiteral("layout.overlap"),
        QStringLiteral("input.actionability"),
        QStringLiteral("text.elide"),
        QStringLiteral("quick3D"),
        QStringLiteral("log.entries"),
    };
    if (checks.isEmpty())
        return all;

    QJsonArray ran;
    if (checks.contains(QStringLiteral("minSize")))
        ran.append(QStringLiteral("layout.minimum_size"));
    if (checks.contains(QStringLiteral("insideViewport")) || checks.contains(QStringLiteral("clickable")))
        ran.append(QStringLiteral("layout.viewport"));
    if (checks.contains(QStringLiteral("childExceedsParent")))
        ran.append(QStringLiteral("layout.clipping"));
    if (checks.contains(QStringLiteral("overlap")))
        ran.append(QStringLiteral("layout.overlap"));
    if (checks.contains(QStringLiteral("actionable")) || checks.contains(QStringLiteral("interactable"))
        || checks.contains(QStringLiteral("clickable"))) {
        ran.append(QStringLiteral("input.actionability"));
    }
    if (checks.contains(QStringLiteral("textElided")))
        ran.append(QStringLiteral("text.elide"));
    if (checks.contains(QStringLiteral("quick3D")))
        ran.append(QStringLiteral("quick3D"));
    if (checks.contains(QStringLiteral("log.entries")))
        ran.append(QStringLiteral("log.entries"));
    return ran;
}

struct ParsedSourceSelector
{
    QString file;
    int line = -1;
    int column = -1;
};

static ParsedSourceSelector parseSourceSelectorValue(const QString &value)
{
    ParsedSourceSelector parsed{ value, -1, -1 };

    const int lastColon = value.lastIndexOf(QLatin1Char(':'));
    if (lastColon < 0)
        return parsed;

    int trailingNumber = -1;
    if (!parsePositiveInt(value.mid(lastColon + 1), &trailingNumber))
        return parsed;

    const QString beforeTrailingNumber = value.left(lastColon);
    const int previousColon = beforeTrailingNumber.lastIndexOf(QLatin1Char(':'));
    if (previousColon >= 0) {
        int line = -1;
        if (parsePositiveInt(beforeTrailingNumber.mid(previousColon + 1), &line)) {
            parsed.file = beforeTrailingNumber.left(previousColon);
            parsed.line = line;
            parsed.column = trailingNumber;
            return parsed;
        }
    }

    parsed.file = beforeTrailingNumber;
    parsed.line = trailingNumber;
    return parsed;
}

static QString sourceLocationSelectorValue(const QJsonObject &location)
{
    const QString file = location.value(QStringLiteral("file")).toString();
    if (file.isEmpty())
        return {};

    const int line = location.value(QStringLiteral("line")).toInt(-1);
    if (line <= 0)
        return file;

    const int column = location.value(QStringLiteral("column")).toInt(-1);
    if (column <= 0)
        return QStringLiteral("%1:%2").arg(file).arg(line);
    return QStringLiteral("%1:%2:%3").arg(file).arg(line).arg(column);
}

static int sourceProximityWeight(const QString &selectorValue, const QJsonObject &candidateLocation)
{
    const QString candidateValue = sourceLocationSelectorValue(candidateLocation);
    if (candidateValue.isEmpty())
        return 0;

    const ParsedSourceSelector selector = parseSourceSelectorValue(selectorValue);
    const ParsedSourceSelector candidate = parseSourceSelectorValue(candidateValue);
    if (selector.file.isEmpty() || candidate.file.isEmpty())
        return 0;

    int score = 0;
    if (selector.file == candidate.file) {
        score += 1000;
    } else if (selector.file.section(QLatin1Char('/'), -1)
               == candidate.file.section(QLatin1Char('/'), -1)) {
        score += 450;
    } else if (candidate.file.endsWith(selector.file) || selector.file.endsWith(candidate.file)) {
        score += 350;
    } else {
        return 0;
    }

    if (selector.line > 0 && candidate.line > 0)
        score += std::max(0, 300 - qAbs(selector.line - candidate.line) * 10);
    if (selector.column > 0 && candidate.column > 0)
        score += std::max(0, 100 - qAbs(selector.column - candidate.column) * 5);

    return score;
}

static int stabilityWeight(const QString &stability)
{
    if (stability == QLatin1String("high"))
        return 300;
    if (stability == QLatin1String("medium"))
        return 150;
    if (stability == QLatin1String("low"))
        return 50;
    return 0;
}

struct CandidateSelector
{
    QJsonObject object;
    int score = 0;
};

static bool isLowValueFrameworkCandidate(const QJsonObject &node, const QString &candidateKind,
                                         const QString &candidateValue)
{
    if (!node.value(QStringLiteral("frameworkInternal")).toBool(false))
        return false;
    if (candidateKind != QLatin1String("id") && candidateKind != QLatin1String("objectName"))
        return false;
    static const QSet<QString> lowValueNames{
        QStringLiteral("background"),
        QStringLiteral("contentItem"),
        QStringLiteral("control"),
        QStringLiteral("indicator"),
        QStringLiteral("handle"),
    };
    return lowValueNames.contains(candidateValue);
}

static bool appendCandidate(QList<CandidateSelector> *candidates, const QJsonObject &node,
                            const QString &selectorKind, const QString &selectorValue,
                            const QString &candidateKind, const QString &candidateValue,
                            const QString &stability)
{
    if (candidateValue.isEmpty())
        return false;
    if (isLowValueFrameworkCandidate(node, candidateKind, candidateValue))
        return false;

    const double textSimilarity = similarity(selectorValue, candidateValue);
    int score = int(textSimilarity * 1000.0) + stabilityWeight(stability);
    if (selectorKind == candidateKind)
        score += 500;
    if (selectorKind == QLatin1String("sourceLocation"))
        score += sourceProximityWeight(selectorValue,
                                       node.value(QStringLiteral("sourceLocation")).toObject());
    if (candidateKind == QLatin1String("id"))
        score += 250;
    else if (candidateKind == QLatin1String("objectName"))
        score += 200;
    else if (candidateKind == QLatin1String("type"))
        score += 100;
    else if (candidateKind == QLatin1String("sourceLocation"))
        score += 100;

    candidates->append({
        QJsonObject{
            { QStringLiteral("selector"), QStringLiteral("%1=\"%2\"").arg(candidateKind, candidateValue) },
            { QStringLiteral("kind"), candidateKind },
            { QStringLiteral("value"), candidateValue },
            { QStringLiteral("stability"), stability },
            { QStringLiteral("nodeId"), node.value(QStringLiteral("nodeId")) },
            { QStringLiteral("score"), score },
        },
        score,
    });
    return true;
}

static void appendSelectorCandidate(QList<CandidateSelector> *candidates,
                                    const QJsonObject &node,
                                    const QString &selectorKind,
                                    const QString &selectorValue,
                                    const QJsonObject &selector)
{
    const QString candidateKind = selector.value(QStringLiteral("kind")).toString();
    const QString candidateValue = selector.value(QStringLiteral("value")).toString();
    const QString stability = selector.value(QStringLiteral("stability")).toString();
    if (candidateKind.isEmpty() || candidateValue.isEmpty())
        return;
    if (candidateKind == QLatin1String("nodeId"))
        return;
    if (candidateKind == QLatin1String("id")
            || candidateKind == QLatin1String("objectName")
            || candidateKind == QLatin1String("type")
            || candidateKind == QLatin1String("text")
            || candidateKind == QLatin1String("visualPath")
            || candidateKind == QLatin1String("sourceLocation")) {
        return;
    }

    if (!appendCandidate(candidates, node, selectorKind, selectorValue, candidateKind,
                         candidateValue, stability.isEmpty() ? QStringLiteral("medium") : stability)) {
        return;
    }
    QJsonObject object = candidates->last().object;
    object.insert(QStringLiteral("selector"), candidateValue);
    if (selector.contains(QStringLiteral("reason")))
        object.insert(QStringLiteral("reason"), selector.value(QStringLiteral("reason")));
    candidates->last().object = object;
}

static void collectCandidateSelectors(const QJsonObject &node, const QString &selectorKind,
                                      const QString &selectorValue,
                                      QList<CandidateSelector> *candidates)
{
    const QString qmlId = node.value(QStringLiteral("qmlId")).toString();
    appendCandidate(candidates, node, selectorKind, selectorValue,
                    QStringLiteral("id"), qmlId, QStringLiteral("high"));

    const QString objectName = node.value(QStringLiteral("objectName")).toString();
    appendCandidate(candidates, node, selectorKind, selectorValue,
                    QStringLiteral("objectName"), objectName, QStringLiteral("high"));

    const QString type = node.value(QStringLiteral("type")).toString();
    appendCandidate(candidates, node, selectorKind, selectorValue,
                    QStringLiteral("type"), type, QStringLiteral("medium"));

    const QString text = node.value(QStringLiteral("text")).toString();
    appendCandidate(candidates, node, selectorKind, selectorValue,
                    QStringLiteral("text"), text, QStringLiteral("low"));

    const QString visualPath = node.value(QStringLiteral("visualPath")).toString();
    appendCandidate(candidates, node, selectorKind, selectorValue,
                    QStringLiteral("visualPath"), visualPath, QStringLiteral("low"));

    const QString sourceLocation = sourceLocationSelectorValue(
            node.value(QStringLiteral("sourceLocation")).toObject());
    appendCandidate(candidates, node, selectorKind, selectorValue,
                    QStringLiteral("sourceLocation"), sourceLocation, QStringLiteral("medium"));

    const QJsonArray selectors = node.value(QStringLiteral("selectors")).toArray();
    for (const QJsonValue &selector : selectors)
        appendSelectorCandidate(candidates, node, selectorKind, selectorValue,
                                selector.toObject());

    const QJsonArray children = node.value(QStringLiteral("children")).toArray();
    for (const QJsonValue &child : children)
        collectCandidateSelectors(child.toObject(), selectorKind, selectorValue, candidates);
}

QJsonObject QQmlAgentDiagnostics::selectorNotFound(const QString &kind, const QString &value,
                                                   const QJsonObject &tree)
{
    QList<CandidateSelector> rankedCandidates;
    const QJsonArray windows = tree.value(QStringLiteral("windows")).toArray();
    for (const QJsonValue &window : windows)
        collectCandidateSelectors(window.toObject().value(QStringLiteral("root")).toObject(),
                                  kind, value, &rankedCandidates);

    std::stable_sort(rankedCandidates.begin(), rankedCandidates.end(),
                     [](const CandidateSelector &left, const CandidateSelector &right) {
        return left.score > right.score;
    });

    QJsonArray candidates;
    QSet<QString> seenCandidateSelectors;
    for (const CandidateSelector &candidate : std::as_const(rankedCandidates)) {
        const QString selector = candidate.object.value(QStringLiteral("selector")).toString();
        if (selector.isEmpty() || seenCandidateSelectors.contains(selector))
            continue;
        seenCandidateSelectors.insert(selector);
        candidates.append(candidate.object);
        if (candidates.size() >= 20)
            break;
    }

    return {
        { QStringLiteral("id"), QStringLiteral("selector.not_found") },
        { QStringLiteral("severity"), QStringLiteral("warning") },
        { QStringLiteral("confidence"), 1.0 },
        { QStringLiteral("message"), QStringLiteral("No nodes matched selector.") },
        { QStringLiteral("selector"), QStringLiteral("%1=\"%2\"").arg(kind, value) },
        { QStringLiteral("candidateSelectors"), candidates },
    };
}

static bool isQuick3DCheckRequested(const QSet<QString> &checks)
{
    return checks.isEmpty() || checks.contains(QStringLiteral("quick3D"));
}

static bool isQuick3DViewportObject(QObject *object)
{
    return object && object->inherits("QQuick3DViewport");
}

static bool isQuick3DModelObject(QObject *object)
{
    return object && object->inherits("QQuick3DModel");
}

static bool isQuick3DLightObject(QObject *object)
{
    if (!object)
        return false;
    const QString className = QString::fromUtf8(object->metaObject()->className());
    return object->inherits("QQuick3DAbstractLight")
            || className.endsWith(QLatin1String("Light"));
}

static bool isQuick3DCameraObject(QObject *object)
{
    if (!object)
        return false;
    const QString className = QString::fromUtf8(object->metaObject()->className());
    return object->inherits("QQuick3DCamera")
            || className.endsWith(QLatin1String("Camera"));
}

static bool isQuick3DTextureObject(QObject *object)
{
    return object && object->inherits("QQuick3DTexture");
}

static QObject *objectPropertyObject(QObject *object, const char *name)
{
    if (!object || object->metaObject()->indexOfProperty(name) < 0)
        return nullptr;
    const QVariant value = object->property(name);
    if (!value.canConvert<QObject *>())
        return nullptr;
    return qvariant_cast<QObject *>(value);
}

static QJsonValue quick3DPropertyEvidence(QObject *object, const char *name)
{
    if (!object || object->metaObject()->indexOfProperty(name) < 0)
        return QJsonValue();
    return QQmlAgentJsonUtils::valueFromVariant(object->property(name));
}

static void appendUniqueObject(QVector<QObject *> *objects, QSet<QObject *> *seen, QObject *object)
{
    if (!object || seen->contains(object))
        return;
    seen->insert(object);
    objects->append(object);
}

static QVector<QObject *> quick3DChildObjects(QObject *object)
{
    QVector<QObject *> children;
    QSet<QObject *> seen;

#ifdef QMLAGENT_HAS_QUICK3D
    if (auto *quick3DObject = qobject_cast<QQuick3DObject *>(object)) {
        const QList<QQuick3DObject *> childItems = quick3DObject->childItems();
        for (QQuick3DObject *child : childItems)
            appendUniqueObject(&children, &seen, child);
    }
#endif

    for (QObject *child : object ? object->children() : QObjectList()) {
        if (child->inherits("QQuick3DObject") || child->inherits("QQuick3DTexture")
            || child->inherits("QQuick3DMaterial")) {
            appendUniqueObject(&children, &seen, child);
        }
    }
    return children;
}

static bool quick3DSceneContains(QObject *object, bool (*predicate)(QObject *),
                                 QSet<QObject *> *visited = nullptr)
{
    QSet<QObject *> localVisited;
    if (!visited)
        visited = &localVisited;
    if (!object || visited->contains(object))
        return false;
    visited->insert(object);
    if (predicate(object))
        return true;

    for (QObject *child : quick3DChildObjects(object)) {
        if (quick3DSceneContains(child, predicate, visited))
            return true;
    }
    return false;
}

static QVector<QObject *> quick3DViewportSceneRoots(QObject *viewport)
{
    QVector<QObject *> roots;
    QSet<QObject *> seen;
    appendUniqueObject(&roots, &seen, objectPropertyObject(viewport, "scene"));
    appendUniqueObject(&roots, &seen, objectPropertyObject(viewport, "importScene"));
    for (QObject *child : quick3DChildObjects(viewport))
        appendUniqueObject(&roots, &seen, child);
    return roots;
}

static bool quick3DViewportHasSceneModels(QObject *viewport)
{
    for (QObject *root : quick3DViewportSceneRoots(viewport)) {
        if (quick3DSceneContains(root, isQuick3DModelObject))
            return true;
    }
    return false;
}

static bool quick3DViewportHasLights(QObject *viewport)
{
    for (QObject *root : quick3DViewportSceneRoots(viewport)) {
        if (quick3DSceneContains(root, isQuick3DLightObject))
            return true;
    }
    return false;
}

static bool quick3DViewportHasCameras(QObject *viewport)
{
    for (QObject *root : quick3DViewportSceneRoots(viewport)) {
        if (quick3DSceneContains(root, isQuick3DCameraObject))
            return true;
    }
    return false;
}

static int quick3DMaterialCount(QObject *model, bool *readable)
{
    *readable = false;
    QQmlListReference reference(model, "materials");
    if (reference.isValid() && reference.isReadable()) {
        *readable = true;
        return int(reference.count());
    }

    const QVariant value = model ? model->property("materials") : QVariant();
    if (value.canConvert<QObjectList>()) {
        *readable = true;
        return qvariant_cast<QObjectList>(value).size();
    }
    return 0;
}

static bool vector3DProperty(QObject *object, const char *name, QVector3D *value)
{
    if (!object || object->metaObject()->indexOfProperty(name) < 0)
        return false;
    const QVariant variant = object->property(name);
    if (!variant.canConvert<QVector3D>())
        return false;
    *value = qvariant_cast<QVector3D>(variant);
    return true;
}

static bool isDegenerateScale(const QVector3D &scale)
{
    static constexpr float DegenerateScaleThreshold = 0.0001f;
    return qAbs(scale.x()) <= DegenerateScaleThreshold
            || qAbs(scale.y()) <= DegenerateScaleThreshold
            || qAbs(scale.z()) <= DegenerateScaleThreshold;
}

static bool textureHasUsableSource(QObject *texture)
{
    const QVariant source = texture ? texture->property("source") : QVariant();
    if (source.metaType().id() == QMetaType::QUrl && !source.toUrl().isEmpty())
        return true;
    if (source.canConvert<QString>() && !source.toString().isEmpty())
        return true;
    return objectPropertyObject(texture, "sourceItem");
}

static void appendQuick3DNodeDiagnostics(QObject *object, int nodeId,
                                         const QJsonObject &source,
                                         const QSet<QString> &checks,
                                         QJsonArray *issues)
{
    if (!isQuick3DCheckRequested(checks) || !object)
        return;

    if (isQuick3DViewportObject(object) && quick3DViewportHasSceneModels(object)) {
        const bool hasActiveCamera = objectPropertyObject(object, "camera");
        const bool hasAuthoredSceneCamera = quick3DViewportHasCameras(object);
        if (!hasActiveCamera && !hasAuthoredSceneCamera) {
            QJsonObject cameraIssue = issue(
                    QStringLiteral("quick3d.view_missing_camera"), QStringLiteral("error"),
                    0.95, nodeId,
                    QStringLiteral("View3D contains model content but no active or authored scene camera was found."),
                    { QStringLiteral("activeCamera=false"),
                      QStringLiteral("authoredSceneCamera=false"),
                      QStringLiteral("sceneContainsModel=true") },
                    source);
            cameraIssue.insert(QStringLiteral("evidenceProfile"),
                               evidenceProfile(QStringLiteral("quick3d-view-properties"),
                                               QStringLiteral("View3D camera property plus scene graph traversal for authored camera nodes"),
                                               {
                                                   QStringLiteral("does not inspect renderer fallback state"),
                                                   QStringLiteral("an active camera may be auto-assigned after a render pass"),
                                                   QStringLiteral("scene content is determined from public Quick3D child traversal and QObject children"),
                                               }));
            cameraIssue.insert(QStringLiteral("repairHints"), QJsonArray{
                repairHint(QStringLiteral("assign-view3d-camera"), 0.90,
                           QStringLiteral("Models in a View3D need a camera to produce meaningful render-space evidence."),
                           {
                               { QStringLiteral("suggestedDirection"), QStringLiteral("set View3D.camera to an authored Camera node") },
                           }),
            });
            issues->append(cameraIssue);
        }

        if (!quick3DViewportHasLights(object)) {
            QJsonObject lightIssue = issue(
                    QStringLiteral("quick3d.scene_missing_light"), QStringLiteral("warning"),
                    0.60, nodeId,
                    QStringLiteral("View3D model scene has no authored light."),
                    { QStringLiteral("sceneContainsModel=true"),
                      QStringLiteral("authoredLightFound=false") },
                    source);
            lightIssue.insert(QStringLiteral("evidenceProfile"),
                              evidenceProfile(QStringLiteral("quick3d-scene-traversal"),
                                              QStringLiteral("Quick3D scene graph traversal looking for light nodes"),
                                              {
                                                  QStringLiteral("environment lighting, emissive materials, or custom shaders can make a scene visible without authored light nodes"),
                                                  QStringLiteral("does not prove final raster darkness"),
                                              }));
            lightIssue.insert(QStringLiteral("repairHints"), QJsonArray{
                repairHint(QStringLiteral("inspect-scene-lighting"), 0.55,
                           QStringLiteral("Missing lights are a common reason model geometry appears black or flat."),
                           {
                               { QStringLiteral("suggestedDirection"), QStringLiteral("add or verify DirectionalLight, PointLight, SpotLight, or environment lighting") },
                           }),
            });
            issues->append(lightIssue);
        }
    }

    if (isQuick3DModelObject(object)) {
        bool readableMaterials = false;
        const int materialCount = quick3DMaterialCount(object, &readableMaterials);
        if (readableMaterials && materialCount == 0) {
            QJsonObject materialIssue = issue(
                    QStringLiteral("quick3d.model_missing_material"), QStringLiteral("warning"),
                    0.75, nodeId,
                    QStringLiteral("Quick3D Model has no assigned materials."),
                    { QStringLiteral("materials.count=0") },
                    source);
            materialIssue.insert(QStringLiteral("evidenceProfile"),
                                 evidenceProfile(QStringLiteral("quick3d-model-properties"),
                                                 QStringLiteral("Model materials list count"),
                                                 {
                                                     QStringLiteral("default materials or custom render paths may still display geometry"),
                                                     QStringLiteral("does not prove final raster invisibility"),
                                                 }));
            materialIssue.insert(QStringLiteral("repairHints"), QJsonArray{
                repairHint(QStringLiteral("assign-model-material"), 0.70,
                           QStringLiteral("Material assignment is usually required for useful Quick3D model rendering."),
                           {
                               { QStringLiteral("suggestedDirection"), QStringLiteral("set Model.materials to an authored material") },
                           }),
            });
            issues->append(materialIssue);
        }

        QVector3D scale;
        if (vector3DProperty(object, "scale", &scale) && isDegenerateScale(scale)) {
            QJsonObject scaleIssue = issue(
                    QStringLiteral("quick3d.model_degenerate_scale"), QStringLiteral("error"),
                    0.95, nodeId,
                    QStringLiteral("Quick3D Model has a zero or near-zero scale axis."),
                    { QStringLiteral("scale.x=%1").arg(scale.x()),
                      QStringLiteral("scale.y=%1").arg(scale.y()),
                      QStringLiteral("scale.z=%1").arg(scale.z()),
                      QStringLiteral("zeroScaleThreshold=0.0001") },
                    source);
            scaleIssue.insert(QStringLiteral("evidenceProfile"),
                              evidenceProfile(QStringLiteral("quick3d-transform-property"),
                                              QStringLiteral("Model scale property"),
                                              {
                                                  QStringLiteral("does not account for parent transforms or custom geometry shaders"),
                                              }));
            attachBindingProvenance(&scaleIssue, object, { QStringLiteral("scale") });
            issues->append(scaleIssue);
        }

        const QJsonObject node = QQmlAgentUiTree::nodeForObject(
                object, 0, 0, true, true, { QStringLiteral("render3D") });
        const QJsonObject render3D = node.value(QStringLiteral("render3D")).toObject();
        if (render3D.value(QStringLiteral("available")).toBool(false)
            && !render3D.value(QStringLiteral("inFrustum")).toBool(true)) {
            QJsonObject frustumIssue = issue(
                    QStringLiteral("quick3d.model_outside_frustum"), QStringLiteral("warning"),
                    0.80, nodeId,
                    QStringLiteral("Quick3D Model bounds are outside the active camera frustum."),
                    { QStringLiteral("render3D.inFrustum=false"),
                      QStringLiteral("render3D.projection.available=true") },
                    source);
            frustumIssue.insert(QStringLiteral("render3D"), render3D);
            frustumIssue.insert(QStringLiteral("evidenceProfile"),
                                evidenceProfile(QStringLiteral("quick3d-projection"),
                                                QStringLiteral("world-space bounds projected through View3D mapFrom3DScene"),
                                                {
                                                    QStringLiteral("geometric evidence only; does not prove raster visibility, occlusion, material opacity, or depth/culling state"),
                                                }));
            issues->append(frustumIssue);
        }
    }

    if (isQuick3DTextureObject(object) && !textureHasUsableSource(object)) {
        QJsonArray evidence{
            QStringLiteral("source empty"),
            QStringLiteral("sourceItem=null"),
        };
        const QJsonValue sourceEvidence = quick3DPropertyEvidence(object, "source");
        if (!sourceEvidence.isUndefined())
            evidence.append(QStringLiteral("source=%1").arg(sourceEvidence.toString()));
        QJsonObject textureIssue = issue(
                QStringLiteral("quick3d.texture_missing_source"), QStringLiteral("warning"),
                0.75, nodeId,
                QStringLiteral("Quick3D Texture has no source URL or sourceItem."),
                evidence, source);
        textureIssue.insert(QStringLiteral("evidenceProfile"),
                            evidenceProfile(QStringLiteral("quick3d-texture-properties"),
                                            QStringLiteral("Texture source and sourceItem properties"),
                                            {
                                                QStringLiteral("procedural or backend-provided texture data may not expose source/sourceItem"),
                                                QStringLiteral("does not prove final raster output is missing"),
                                            }));
        textureIssue.insert(QStringLiteral("repairHints"), QJsonArray{
            repairHint(QStringLiteral("assign-texture-source"), 0.65,
                       QStringLiteral("Texture references are easier to repair when they point at a source URL or sourceItem."),
                       {
                           { QStringLiteral("suggestedDirection"), QStringLiteral("set Texture.source or Texture.sourceItem") },
                       }),
        });
        issues->append(textureIssue);
    }
}

QJsonObject QQmlAgentDiagnostics::analyzeNode(const QJsonObject &params)
{
    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    const int nodeId = ref.nodeId;
    QObject *object = ref.object;
    QJsonArray issues;
    const QSet<QString> checks = requestedChecks(params);
    const bool clickableCheck = checkExplicitlyRequested(checks, QStringLiteral("clickable"));
    const bool actionableCheck = checkExplicitlyRequested(checks, QStringLiteral("actionable"))
            || checkExplicitlyRequested(checks, QStringLiteral("interactable"));

    if (!ref.issues.isEmpty())
        return { { QStringLiteral("issues"), ref.issues } };

    QQuickItem *item = qobject_cast<QQuickItem *>(object);
    if (!object) {
        issues.append(issue(QStringLiteral("input.not_clickable"), QStringLiteral("error"), 1.0, nodeId,
                            QStringLiteral("Node does not exist in this session."),
                            { QStringLiteral("node_not_found") }));
        return { { QStringLiteral("issues"), issues } };
    }

    const QJsonObject source = QQmlAgentSourceResolver::sourceLocationForObject(object);
    if (!item) {
        appendQuick3DNodeDiagnostics(object, nodeId, source, checks, &issues);
        if (actionableCheck) {
            issues.append(issue(QStringLiteral("input.not_actionable"), QStringLiteral("error"),
                                1.0, nodeId, QStringLiteral("Node is not currently actionable."),
                                { QStringLiteral("not_qquickitem") },
                                source));
        }
        if (clickableCheck) {
            issues.append(issue(QStringLiteral("input.not_clickable"), QStringLiteral("error"), 1.0,
                                nodeId, QStringLiteral("Node is not a QQuickItem."),
                                { QStringLiteral("not_qquickitem") },
                                source));
        }
        return {
            { QStringLiteral("node"), ref.node },
            { QStringLiteral("issues"), issues },
        };
    }

    if (actionableCheck) {
        const QJsonArray reasons = QQmlAgentActionability::reasons(object);
        if (!reasons.isEmpty()) {
            QJsonArray evidence;
            for (const QJsonValue &reasonValue : reasons)
                evidence.append(reasonValue.toObject().value(QStringLiteral("id")).toString());
            QJsonObject actionableIssue = issue(
                    QStringLiteral("input.not_actionable"), QStringLiteral("error"), 1.0,
                    nodeId, QStringLiteral("Node is not currently actionable."),
                    evidence, source);
            actionableIssue.insert(QStringLiteral("evidenceProfile"),
                                   evidenceProfile(QStringLiteral("actionability-reasons"),
                                                   QStringLiteral("computed visibility, enabled, viewport, hit-test, and modal-blocking checks"),
                                                   {
                                                       QStringLiteral("not proof that the target has a semantic input handler"),
                                                       QStringLiteral("generic blocker detection uses center-point paint-order evidence"),
                                                       QStringLiteral("modal popup evidence depends on visible Qt Quick overlay state"),
                                                   }));
            actionableIssue.insert(QStringLiteral("actionability"), QJsonObject{
                { QStringLiteral("ok"), false },
                { QStringLiteral("reasons"), reasons },
                { QStringLiteral("limitations"), QJsonArray{
                    QStringLiteral("does not prove the target has an input handler"),
                    QStringLiteral("generic blocker detection uses a center-point paint-order approximation"),
                    QStringLiteral("Qt Quick Controls modal popup blocking is refined through the overlay stack"),
                } },
            });
            issues.append(actionableIssue);
        }
    }

    const bool overlapExplicitlyRequested = checkExplicitlyRequested(checks, QStringLiteral("overlap"));
    if (checks.isEmpty() || overlapExplicitlyRequested) {
        const QJsonArray reasons = QQmlAgentActionability::reasons(object);
        QJsonObject blockedReason;
        const bool hasBlockingEvidence =
                actionabilityHasReason(reasons, QStringLiteral("blocked_by_item"), &blockedReason);
        const bool hasPrimaryVisibilityFailure =
                actionabilityHasReason(reasons, QStringLiteral("opacity_zero"))
                || actionabilityHasReason(reasons, QStringLiteral("not_visible"))
                || actionabilityHasReason(reasons, QStringLiteral("zero_size"));
        if (hasBlockingEvidence && !hasPrimaryVisibilityFailure) {
            const QRectF bbox = itemBoxInWindow(item);
            const bool suppressAsIntentionalOverlay = !overlapExplicitlyRequested
                    && looksLikeIntentionalOverlayBlocker(item, bbox, blockedReason);
            if (!suppressAsIntentionalOverlay && isApplicationSourceLocation(source)
                    && (overlapExplicitlyRequested || !isDelegateTemplateSourceLocation(source))) {
                issues.append(overlapIssueForNode(
                        QStringLiteral("Visible authored item overlaps with another item."),
                        nodeId, source, bbox, reasons, blockedReason, ref.node));
            } else if (!suppressAsIntentionalOverlay) {
                QQuickItem *appAncestor = nearestAppAuthoredAncestor(item, overlapExplicitlyRequested);
                if (appAncestor) {
                    const QRectF ancestorBox = itemBoxInWindow(appAncestor);
                    if (!ancestorBox.contains(bbox)) {
                        const QJsonObject ancestorSource =
                                QQmlAgentSourceResolver::sourceLocationForObject(appAncestor);
                        const QJsonObject ancestorNode = QQmlAgentUiTree::nodeForObject(
                                appAncestor, 0, 0, true, true, {});
                        const int ancestorNodeId =
                                ancestorNode.value(QStringLiteral("nodeId")).toInt(-1);
                        issues.append(overlapIssueForNode(
                                QStringLiteral("Authored item has a descendant that overlaps with another item."),
                                ancestorNodeId, ancestorSource, ancestorBox, reasons, blockedReason,
                                ancestorNode,
                                {
                                    QStringLiteral("descendantNodeId=%1").arg(nodeId),
                                    QStringLiteral("descendantType=%1").arg(
                                            QString::fromUtf8(object->metaObject()->className())),
                                    bboxEvidence(QStringLiteral("descendantBbox"), bbox),
                                    bboxEvidence(QStringLiteral("ancestorBbox"), ancestorBox),
                                    QStringLiteral("descendant outside authored ancestor bounds"),
                                }));
                    }
                }
            }
        }
    }

    const bool runVisibleCheck = checkRequested(checks, QStringLiteral("visible")) || clickableCheck;
    if (runVisibleCheck && !item->isVisible()) {
        QJsonObject invisibleIssue =
                issue(QStringLiteral("layout.invisible_ancestor"), QStringLiteral("error"), 0.90,
                      nodeId, QStringLiteral("Node is not visible."),
                      { QStringLiteral("visible=false") }, source);
        invisibleIssue.insert(QStringLiteral("evidenceProfile"),
                              evidenceProfile(QStringLiteral("quick-item-visibility"),
                                              QStringLiteral("QQuickItem::isVisible() effective visibility"),
                                              {
                                                  QStringLiteral("does not identify every ancestor that contributed to effective visibility"),
                                                  QStringLiteral("visibility can change after bindings or state transitions reevaluate"),
                                              }));
        attachBindingProvenance(&invisibleIssue, object, { QStringLiteral("visible") });
        issues.append(invisibleIssue);
    }

    if ((checks.isEmpty() || checkExplicitlyRequested(checks, QStringLiteral("opacity")))
        && item->opacity() <= 0.01) {
        QJsonObject opacityIssue =
                issue(QStringLiteral("layout.opacity_zero"), QStringLiteral("error"), 0.90,
                      nodeId, QStringLiteral("Node has near-zero opacity."),
                      { QStringLiteral("opacity=%1").arg(item->opacity()) }, source);
        opacityIssue.insert(QStringLiteral("evidenceProfile"),
                            evidenceProfile(QStringLiteral("quick-item-opacity-threshold"),
                                            QStringLiteral("QQuickItem::opacity() <= 0.01"),
                                            {
                                                QStringLiteral("threshold is an interaction heuristic, not proof of visual invisibility in every shader/effect path"),
                                                QStringLiteral("does not include inherited effective opacity from every ancestor"),
                                            }));
        attachBindingProvenance(&opacityIssue, object, { QStringLiteral("opacity") });
        issues.append(opacityIssue);
    }

    bool truncated = false;
    const bool textElidedExplicitlyRequested =
            checkExplicitlyRequested(checks, QStringLiteral("textElided"));
    if ((checks.isEmpty() || textElidedExplicitlyRequested)
        && boolProperty(object, "truncated", &truncated) && truncated) {
        const QJsonValue text = propertyValue(object, "text");
        const QJsonValue contentWidth = propertyValue(object, "contentWidth");
        const bool weakDefaultEvidence = hasWeakDefaultTextElideEvidence(text.toString(),
                                                                         contentWidth);
        if (!weakDefaultEvidence || textElidedExplicitlyRequested) {
            QJsonArray evidence{
                QStringLiteral("truncated=true"),
                QStringLiteral("width=%1").arg(item->width()),
            };
            if (!text.isUndefined())
                evidence.append(QStringLiteral("text=%1").arg(text.toString()));
            if (!contentWidth.isUndefined())
                evidence.append(QStringLiteral("contentWidth=%1").arg(contentWidth.toDouble()));
            if (weakDefaultEvidence)
                evidence.append(QStringLiteral("weakDefaultEvidence=true"));

            QJsonObject textIssue =
                    issue(QStringLiteral("layout.text_elided"), QStringLiteral("warning"), 0.95,
                          nodeId, QStringLiteral("Text is elided or truncated."),
                          evidence, source);
            if (weakDefaultEvidence) {
                textIssue.insert(QStringLiteral("confidence"), 0.55);
                textIssue.insert(QStringLiteral("limitations"), QJsonArray{
                    QStringLiteral("short identifier-like text with non-positive contentWidth can be an icon ligature or style artifact"),
                    QStringLiteral("explicit textElided check requested this low-confidence evidence"),
                });
            }
            attachBindingProvenance(&textIssue, object, { QStringLiteral("width"),
                                                          QStringLiteral("text") });
            issues.append(textIssue);
        }
    }

    bool readOnly = false;
    if (checkExplicitlyRequested(checks, QStringLiteral("editable"))
        && boolProperty(object, "readOnly", &readOnly) && readOnly) {
        issues.append(issue(QStringLiteral("input.not_editable"), QStringLiteral("error"), 1.0,
                            nodeId, QStringLiteral("Text input is read-only."),
                            { QStringLiteral("readOnly=true") }, source));
    }

    const bool runMinSizeCheck = checkRequested(checks, QStringLiteral("minSize")) || clickableCheck;
    if (runMinSizeCheck && (item->width() <= 0.5 || item->height() <= 0.5)) {
        QJsonObject zeroSizeIssue =
                issue(QStringLiteral("layout.zero_size"), QStringLiteral("error"), 0.90, nodeId,
                      QStringLiteral("Node has zero or near-zero size."),
                      { QStringLiteral("width=%1").arg(item->width()),
                        QStringLiteral("height=%1").arg(item->height()),
                        QStringLiteral("implicitWidth=%1").arg(item->implicitWidth()),
                        QStringLiteral("implicitHeight=%1").arg(item->implicitHeight()),
                        QStringLiteral("zeroSizeThresholdLogicalPx=0.5") },
                      source);
        zeroSizeIssue.insert(QStringLiteral("evidenceProfile"),
                             evidenceProfile(QStringLiteral("logical-size-threshold"),
                                             QStringLiteral("width/height logical pixel threshold"),
                                             {
                                                 QStringLiteral("threshold is an actionability heuristic, not a semantic layout proof"),
                                                 QStringLiteral("device pixel ratio and transforms can affect rendered result"),
                                             }));
        attachBindingProvenance(&zeroSizeIssue, object, { QStringLiteral("width"),
                                                          QStringLiteral("height") });
        issues.append(zeroSizeIssue);
    }

    const bool insideViewportExplicitlyRequested =
            checkExplicitlyRequested(checks, QStringLiteral("insideViewport"));
    const bool runInsideViewportCheck = checks.isEmpty() || insideViewportExplicitlyRequested
            || clickableCheck;
    if (runInsideViewportCheck && item->window()) {
        const QRectF bbox = itemBoxInWindow(item);
        const QRectF viewport(QPointF(0, 0), item->window()->size());
        if (!viewport.contains(bbox)) {
            QJsonArray evidence{
                QStringLiteral("bbox=[%1,%2,%3,%4]").arg(bbox.x()).arg(bbox.y()).arg(bbox.width()).arg(bbox.height()),
                QStringLiteral("viewport=[0,0,%1,%2]").arg(viewport.width()).arg(viewport.height()),
                QStringLiteral("coordinateSpace=window logical pixels"),
                QStringLiteral("devicePixelRatio=%1").arg(item->window()->devicePixelRatio()),
            };
            if (bbox.right() > viewport.right())
                evidence.append(QStringLiteral("right=%1 exceeds viewport width=%2").arg(bbox.right()).arg(viewport.right()));
            if (bbox.bottom() > viewport.bottom())
                evidence.append(QStringLiteral("bottom=%1 exceeds viewport height=%2").arg(bbox.bottom()).arg(viewport.bottom()));
            if (bbox.left() < viewport.left())
                evidence.append(QStringLiteral("left=%1 is before viewport left=%2").arg(bbox.left()).arg(viewport.left()));
            if (bbox.top() < viewport.top())
                evidence.append(QStringLiteral("top=%1 is before viewport top=%2").arg(bbox.top()).arg(viewport.top()));

            QJsonObject outside = issue(QStringLiteral("layout.outside_viewport"), QStringLiteral("error"),
                                        0.90, nodeId,
                                        QStringLiteral("Visible item is not fully inside the viewport."),
                                        evidence, source);
            outside.insert(QStringLiteral("bbox"), rectArray(bbox));
            outside.insert(QStringLiteral("viewport"), rectArray(viewport));
            outside.insert(QStringLiteral("evidenceProfile"),
                           evidenceProfile(QStringLiteral("window-logical-geometry"),
                                           QStringLiteral("item scene bbox compared with QQuickWindow logical size"),
                                           {
                                               QStringLiteral("uses logical coordinates, not physical pixels"),
                                               QStringLiteral("does not account for platform window decorations or native clipping outside QQuickWindow"),
                                           }));
            outside.insert(QStringLiteral("blameChain"), viewportBlameChain(item, viewport));
            outside.insert(QStringLiteral("patchDirection"),
                           QStringLiteral("Reduce fixed spacing, wrap content in ScrollView, use Layout.fillHeight, or anchor action buttons separately."));
            outside.insert(QStringLiteral("repairHints"), QJsonArray{
                repairHint(QStringLiteral("reduce-prior-fixed-spacing"), 0.65,
                           QStringLiteral("Target is outside the viewport; inspect preceding fixed-height spacing or layout constraints."),
                           {
                               { QStringLiteral("suggestedDirection"), QStringLiteral("decrease-or-replace-fixed-spacing") },
                               { QStringLiteral("suggestedPattern"), QStringLiteral("Layout.fillHeight or ScrollView") },
                           }),
                repairHint(QStringLiteral("anchor-action-inside-viewport"), 0.55,
                           QStringLiteral("Action controls should remain inside the window viewport."),
                           {
                               { QStringLiteral("suggestedDirection"), QStringLiteral("anchor-or-layout-action-row-inside-viewport") },
                           }),
            });
            attachBindingProvenance(&outside, object, { QStringLiteral("x"),
                                                        QStringLiteral("y"),
                                                        QStringLiteral("width"),
                                                        QStringLiteral("height") });
            issues.append(outside);
        }
    }

    QQuickItem *parentItem = item->parentItem();
    if ((checks.isEmpty() || checkExplicitlyRequested(checks, QStringLiteral("childExceedsParent")))
        && parentItem) {
        const QRectF bbox = itemBoxInWindow(item);
        const QRectF parentBox = itemBoxInWindow(parentItem);
        if (parentItem->clip() && !parentBox.contains(bbox)) {
            QJsonArray evidence{
                QStringLiteral("bbox=[%1,%2,%3,%4]").arg(bbox.x()).arg(bbox.y()).arg(bbox.width()).arg(bbox.height()),
                QStringLiteral("parentBbox=[%1,%2,%3,%4]").arg(parentBox.x()).arg(parentBox.y()).arg(parentBox.width()).arg(parentBox.height()),
                QStringLiteral("parentClip=true"),
            };
            if (bbox.right() > parentBox.right())
                evidence.append(QStringLiteral("right=%1 exceeds parent right=%2").arg(bbox.right()).arg(parentBox.right()));
            if (bbox.bottom() > parentBox.bottom())
                evidence.append(QStringLiteral("bottom=%1 exceeds parent bottom=%2").arg(bbox.bottom()).arg(parentBox.bottom()));
            if (bbox.left() < parentBox.left())
                evidence.append(QStringLiteral("left=%1 is before parent left=%2").arg(bbox.left()).arg(parentBox.left()));
            if (bbox.top() < parentBox.top())
                evidence.append(QStringLiteral("top=%1 is before parent top=%2").arg(bbox.top()).arg(parentBox.top()));

            QJsonObject childExceeds = issue(
                    QStringLiteral("layout.child_exceeds_parent"), QStringLiteral("error"), 0.90,
                    nodeId, QStringLiteral("Child item exceeds a clipping parent item."),
                    evidence, source);
            childExceeds.insert(QStringLiteral("evidenceProfile"),
                                evidenceProfile(QStringLiteral("clipping-parent-geometry"),
                                                QStringLiteral("child scene bbox compared with clipping parent scene bbox"),
                                                {
                                                    QStringLiteral("uses axis-aligned logical geometry"),
                                                    QStringLiteral("does not fully model transforms, shader effects, or nested clip intersections"),
                                                }));
            childExceeds.insert(QStringLiteral("parentNodeId"),
                                QQmlAgentUiTree::nodeForObject(parentItem, 0, 0, true, false, {})
                                        .value(QStringLiteral("nodeId")).toInt(-1));
            childExceeds.insert(QStringLiteral("blameChain"), QJsonArray{
                blameEntry(item, bboxEvidence(QStringLiteral("bbox"), bbox)),
                blameEntry(parentItem, QStringLiteral("%1; clip=true")
                           .arg(bboxEvidence(QStringLiteral("parentBbox"), parentBox))),
            });
            childExceeds.insert(QStringLiteral("patchDirection"),
                                QStringLiteral("Constrain the child inside the clipping parent, resize the parent, or remove clipping if overflow is intentional."));
            childExceeds.insert(QStringLiteral("repairHints"), QJsonArray{
                repairHint(QStringLiteral("constrain-child-to-clipping-parent"), 0.85,
                           QStringLiteral("Child geometry exceeds a parent that has clipping enabled."),
                           {
                               { QStringLiteral("suggestedDirection"), QStringLiteral("resize-child-or-parent") },
                           }),
            });
            attachBindingProvenance(&childExceeds, object, { QStringLiteral("x"),
                                                             QStringLiteral("y"),
                                                             QStringLiteral("width"),
                                                             QStringLiteral("height") });
            issues.append(childExceeds);
        }
    }

    const int enabledIndex = object->metaObject()->indexOfProperty("enabled");
    const bool runEnabledCheck = checkRequested(checks, QStringLiteral("enabled")) || clickableCheck;
    if (runEnabledCheck && enabledIndex >= 0 && !object->property("enabled").toBool()) {
        issues.append(issue(QStringLiteral("input.not_clickable"), QStringLiteral("error"), 1.0, nodeId,
                            QStringLiteral("Node is disabled."),
                            { QStringLiteral("enabled=false") }, source));
    } else if (runEnabledCheck) {
        QQuickItem *ancestor = disabledAncestor(item);
        if (ancestor) {
            QJsonObject ancestorIssue = issue(
                    QStringLiteral("input.not_clickable"), QStringLiteral("error"), 1.0, nodeId,
                    QStringLiteral("Node has a disabled ancestor."),
                    { QStringLiteral("ancestorEnabled=false"),
                      QStringLiteral("ancestor=%1").arg(objectLabel(ancestor)) },
                    source);
            ancestorIssue.insert(QStringLiteral("blameChain"), QJsonArray{
                blameEntry(item, QStringLiteral("target enabled but not effectively clickable")),
                blameEntry(ancestor, QStringLiteral("enabled=false")),
            });
            issues.append(ancestorIssue);
        }
    }

    appendQuick3DNodeDiagnostics(object, nodeId, source, checks, &issues);

    return {
        { QStringLiteral("node"), ref.node },
        { QStringLiteral("issues"), issues },
    };
}

static bool isEmptySpacerCandidate(const QQuickItem *item)
{
    if (!item || !item->isVisible() || !item->childItems().isEmpty())
        return false;

    if (item->metaObject() != &QQuickItem::staticMetaObject)
        return false;

    const qreal height = item->height();
    const qreal parentHeight = item->parentItem() ? item->parentItem()->height() : 0.0;
    const qreal viewportHeight = item->window() ? item->window()->height() : 0.0;
    return height > 0.0
            && ((viewportHeight > 0.0 && height >= viewportHeight * LargeSpacerViewportRatio)
                || (parentHeight > 0.0 && height >= parentHeight * LargeSpacerParentRatio));
}

static void appendExcessiveSpacerHints(const QList<QQuickItem *> &siblings, QJsonArray *issues)
{
    for (int i = 0; i < siblings.size(); ++i) {
        QQuickItem *spacer = siblings.at(i);
        if (!isEmptySpacerCandidate(spacer))
            continue;

        const QRectF spacerBox = itemBoxInWindow(spacer);
        for (int j = i + 1; j < siblings.size(); ++j) {
            QQuickItem *target = siblings.at(j);
            if (!target || !target->isVisible() || !target->window())
                continue;

            const QRectF targetBox = itemBoxInWindow(target);
            const QRectF viewport(QPointF(0, 0), target->window()->size());
            if (viewport.contains(targetBox))
                continue;

            QObject *spacerObject = spacer;
            QObject *targetObject = target;
            const int spacerNodeId = QQmlAgentUiTree::nodeForObject(spacerObject, 0, 0, true, false, {})
                    .value(QStringLiteral("nodeId")).toInt(-1);
            const int targetNodeId = QQmlAgentUiTree::nodeForObject(targetObject, 0, 0, true, false, {})
                    .value(QStringLiteral("nodeId")).toInt(-1);

            QJsonObject spacerIssue = issue(
                    QStringLiteral("layout.excessive_spacer"), QStringLiteral("warning"), 0.75,
                    spacerNodeId,
                    QStringLiteral("Large empty spacer appears before an item that is outside the viewport."),
                    {
                        QStringLiteral("spacerBbox=[%1,%2,%3,%4]").arg(spacerBox.x()).arg(spacerBox.y()).arg(spacerBox.width()).arg(spacerBox.height()),
                        QStringLiteral("target=%1").arg(objectLabel(targetObject)),
                        QStringLiteral("targetBbox=[%1,%2,%3,%4]").arg(targetBox.x()).arg(targetBox.y()).arg(targetBox.width()).arg(targetBox.height()),
                        QStringLiteral("viewport=[0,0,%1,%2]").arg(viewport.width()).arg(viewport.height()),
                    },
                    QQmlAgentSourceResolver::sourceLocationForObject(spacerObject));
            spacerIssue.insert(QStringLiteral("relatedNodeId"), targetNodeId);
            spacerIssue.insert(QStringLiteral("blameChain"), QJsonArray{
                blameEntry(spacer, QStringLiteral("large empty spacer before failed target")),
                blameEntry(target, QStringLiteral("target is outside viewport")),
            });
            spacerIssue.insert(QStringLiteral("patchDirection"),
                               QStringLiteral("Replace fixed spacer height with Layout.fillHeight, reduce preferredHeight, or wrap overflowing content in ScrollView."));
            spacerIssue.insert(QStringLiteral("repairHints"), QJsonArray{
                repairHint(QStringLiteral("reduce-fixed-size"), 0.8,
                           QStringLiteral("Large empty spacer appears before a target that is outside the viewport."),
                           {
                               { QStringLiteral("property"), QStringLiteral("height") },
                               { QStringLiteral("currentValue"), spacer->height() },
                               { QStringLiteral("suggestedDirection"), QStringLiteral("decrease") },
                           }),
                repairHint(QStringLiteral("replace-fixed-spacing"), 0.65,
                           QStringLiteral("Flexible layout or scrolling can keep later controls reachable."),
                           {
                               { QStringLiteral("suggestedPattern"), QStringLiteral("Layout.fillHeight or ScrollView") },
                           }),
            });
            issues->append(spacerIssue);
            break;
        }
    }
}

static constexpr int SuppressedFrameworkIssueDetailLimit = 20;

static void analyzeNodeObject(const QJsonObject &node, QJsonArray *issues,
                              QJsonArray *suppressedFrameworkIssues, int *suppressedFrameworkIssueCount,
                              bool includeFrameworkIssues, const QSet<QString> &checks)
{
    const int nodeId = node.value(QStringLiteral("nodeId")).toInt(-1);
    const bool isInternalRootItem = node.value(QStringLiteral("type")).toString()
            == QLatin1String("QQuickRootItem");
    if (!isInternalRootItem && !isTreeDiagnosticInfrastructure(node)) {
        QJsonObject analyzeParams{ { QStringLiteral("nodeId"), nodeId } };
        const QJsonArray requested = checksArray(checks);
        if (!requested.isEmpty())
            analyzeParams.insert(QStringLiteral("checks"), requested);
        const QJsonArray nodeIssues = QQmlAgentDiagnostics::analyzeNode(analyzeParams)
                .value(QStringLiteral("issues")).toArray();
        for (const QJsonValue &nodeIssue : nodeIssues) {
            const QJsonObject issue = nodeIssue.toObject();
            if (!includeFrameworkIssues && isFrameworkOwnedIssue(node, issue)) {
                ++(*suppressedFrameworkIssueCount);
                if (suppressedFrameworkIssues->size() < SuppressedFrameworkIssueDetailLimit)
                    suppressedFrameworkIssues->append(suppressedFrameworkIssueSummary(node, issue));
                continue;
            }
            issues->append(issue);
        }
    }

    const QJsonArray children = node.value(QStringLiteral("children")).toArray();
    for (const QJsonValue &child : children)
        analyzeNodeObject(child.toObject(), issues, suppressedFrameworkIssues,
                          suppressedFrameworkIssueCount, includeFrameworkIssues, checks);

    QObject *object = QQmlAgentUiTree::objectForNodeId(nodeId);
    QQuickItem *item = qobject_cast<QQuickItem *>(object);
    if (!item)
        return;

    if (checks.isEmpty() || checkExplicitlyRequested(checks, QStringLiteral("excessiveSpacer")))
        appendExcessiveSpacerHints(item->childItems(), issues);
}

static bool isRepairRelevantLogEntry(const QJsonObject &entry)
{
    const QString level = entry.value(QStringLiteral("level")).toString();
    if (level != QLatin1String("warning")
            && level != QLatin1String("error")
            && level != QLatin1String("fatal")) {
        return false;
    }

    const QString category = entry.value(QStringLiteral("category")).toString();
    if (category == QLatin1String("qml"))
        return true;

    const QString message = entry.value(QStringLiteral("text")).toString();
    if (message.contains(QLatin1String("ReferenceError:"))
            || message.contains(QLatin1String("TypeError:"))) {
        return true;
    }

    return false;
}

QJsonObject QQmlAgentDiagnostics::analyzeBinding(const QJsonObject &params)
{
    const QQmlAgentUiTree::NodeRef ref = QQmlAgentUiTree::resolveNodeRef(params);
    if (!ref.issues.isEmpty()) {
        return {
            { QStringLiteral("ok"), false },
            { QStringLiteral("diagnostics"), ref.issues },
        };
    }

    const QString propertyName = params.value(QStringLiteral("property")).toString();
    QJsonObject result = QQmlAgentSourceResolver::bindingProvenanceForProperty(ref.object,
                                                                               propertyName);
    if (!ref.node.isEmpty())
        result.insert(QStringLiteral("node"), ref.node);

    QJsonObject target{
        { QStringLiteral("nodeId"), ref.nodeId },
    };
    const QString selector = params.value(QStringLiteral("selector")).toString();
    if (!selector.isEmpty())
        target.insert(QStringLiteral("selector"), selector);
    result.insert(QStringLiteral("target"), target);
    return result;
}

QJsonObject QQmlAgentDiagnostics::analyzeTree(const QJsonObject &params, QQmlAgentLogCollector *logs)
{
    QJsonArray issues;
    QJsonArray suppressedFrameworkIssues;
    int suppressedFrameworkIssueCount = 0;
    const QString issueScope = params.value(QStringLiteral("issueScope")).toString();
    const bool includeFrameworkIssues =
            params.value(QStringLiteral("includeFrameworkIssues")).toBool(false)
            || issueScope == QLatin1String("all");
    const QSet<QString> checks = requestedChecks(params);
    const QJsonObject tree = QQmlAgentUiTree::getTree({
        { QStringLiteral("depth"), -1 },
        { QStringLiteral("includeInvisible"), params.value(QStringLiteral("includeInvisible")).toBool(false) },
        { QStringLiteral("includeSource"), true },
    });
    const QJsonArray windows = tree.value(QStringLiteral("windows")).toArray();
    for (const QJsonValue &window : windows)
        analyzeNodeObject(window.toObject().value(QStringLiteral("root")).toObject(), &issues,
                          &suppressedFrameworkIssues, &suppressedFrameworkIssueCount,
                          includeFrameworkIssues, checks);

    int logEntryCount = 0;
    int promotedLogIssueCount = 0;
    int ignoredLogEntryCount = 0;
    if (logs && (checks.isEmpty() || checkExplicitlyRequested(checks, QStringLiteral("log.entries")))) {
        const QJsonArray entries = logs->entries({}).value(QStringLiteral("entries")).toArray();
        logEntryCount = entries.size();
        for (const QJsonValue &entryValue : entries) {
            const QJsonObject entry = entryValue.toObject();
            if (!isRepairRelevantLogEntry(entry)) {
                ++ignoredLogEntryCount;
                continue;
            }
            ++promotedLogIssueCount;
            issues.append(QJsonObject{
                { QStringLiteral("id"), QStringLiteral("qml.warning") },
                { QStringLiteral("severity"), entry.value(QStringLiteral("level")).toString(QStringLiteral("warning")) },
                { QStringLiteral("confidence"), 1.0 },
                { QStringLiteral("message"), entry.value(QStringLiteral("text")).toString() },
                { QStringLiteral("sourceLocation"), entry.value(QStringLiteral("sourceLocation")) },
            });
        }
    }

    QJsonObject summary{
        { QStringLiteral("ok"), issues.isEmpty() },
        { QStringLiteral("issueCount"), issues.size() },
        { QStringLiteral("logEntryCount"), logEntryCount },
        { QStringLiteral("promotedLogIssueCount"), promotedLogIssueCount },
        { QStringLiteral("ignoredLogEntryCount"), ignoredLogEntryCount },
        { QStringLiteral("issueScope"),
          includeFrameworkIssues ? QStringLiteral("all") : QStringLiteral("application") },
        { QStringLiteral("suppressedFrameworkIssueCount"), suppressedFrameworkIssueCount },
        { QStringLiteral("suppressedFrameworkIssueDetailsTruncated"),
          suppressedFrameworkIssueCount > suppressedFrameworkIssues.size() },
        { QStringLiteral("suppressedFrameworkIssues"), suppressedFrameworkIssues },
        { QStringLiteral("ran"), ranChecks(checks) },
    };

    if (params.value(QStringLiteral("verbosity")).toString() == QLatin1String("summary")) {
        const int maxIssues = qBound(0, params.value(QStringLiteral("maxIssues")).toInt(20), 100);
        const QJsonArray returnedIssues = compactIssueSummaries(issues, maxIssues);
        summary.insert(QStringLiteral("verbosity"), QStringLiteral("summary"));
        summary.insert(QStringLiteral("returnedIssueCount"), returnedIssues.size());
        summary.insert(QStringLiteral("omittedIssueCount"), issues.size() - returnedIssues.size());
        summary.insert(QStringLiteral("moreAvailable"), issues.size() > returnedIssues.size());
        summary.insert(QStringLiteral("omittedFields"), analyzeTreeSummaryOmittedFields());
        if (issues.size() > returnedIssues.size()) {
            summary.insert(QStringLiteral("nextHints"), QJsonArray{
                QJsonObject{
                    { QStringLiteral("method"), QStringLiteral("Diagnostics.analyzeTree") },
                    { QStringLiteral("params"), QJsonObject{
                        { QStringLiteral("verbosity"), QStringLiteral("evidence") },
                        { QStringLiteral("includeInvisible"),
                          params.value(QStringLiteral("includeInvisible")).toBool(false) },
                    } },
                },
            });
        }
        return {
            { QStringLiteral("issues"), returnedIssues },
            { QStringLiteral("summary"), summary },
        };
    }

    summary.insert(QStringLiteral("verbosity"), QStringLiteral("evidence"));
    return {
        { QStringLiteral("issues"), issues },
        { QStringLiteral("summary"), summary },
    };
}

QT_END_NAMESPACE
