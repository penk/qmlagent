// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "qqmlagentinput_p.h"
#include "qqmlagentdiagnostics_p.h"
#include "qqmlagentlogcollector_p.h"
#include "qqmlagentrender_p.h"
#include "qqmlagentruntime_p.h"
#include "qqmlagentuitree_p.h"

#include <private/qqmldebugservice_p.h>

#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qlogging.h>
#include <QtCore/qobject.h>
#include <QtCore/qscopedpointer.h>
#include <QtCore/qurl.h>
#include <QtCore/qtimer.h>
#include <QtQuick/qquickitem.h>
#include <QtQuick/qquickwindow.h>
#include <QtQuick/private/qquickflickable_p.h>
#include <QtQml/qqmlcomponent.h>
#include <QtQml/qqmlengine.h>
#include <QtTest/qtest.h>

QT_USE_NAMESPACE

class tst_QQmlAgentInput : public QObject
{
    Q_OBJECT

private slots:
    void clickNodeFailureReasons_data();
    void clickNodeFailureReasons();
    void dispatchKeyEventRejectsInvalidKey();
    void dispatchMouseEventRejectsInvalidType();
    void dispatchTouchEventRejectsInvalidPoints();
    void dragNodeRejectsMissingTarget();
    void typeTextRejectsEmptyText();
    void wheelRejectsMissingDelta();
    void diagnosticsPromoteOnlyRepairRelevantLogs();
    void waitForObservesInvisibleProperty();
    void waitForPrefersUniqueVisibleMatch();
    void dispatchBudgetsCoverLongRunningRequests();
    void selectorStabilityReflectsTreeUniqueness();
    void queryManyAlignsResultsAndAppliesDefaults();
    void windowObjectIsAddressable();
#ifdef QMLAGENT_HAS_QUICK3D
    void quick3DReachabilityIsAutomatic();
    void quick3DView3DIncludesSceneChildrenAutomatically();
#endif
    void scrollIntoViewAdjustsAncestorFlickable();
    void dismissPopupReportsNoVisiblePopup();
};

static QJsonObject clickNode(int nodeId)
{
    return QQmlAgentInput::clickNode({ { QStringLiteral("nodeId"), nodeId } });
}

void tst_QQmlAgentInput::clickNodeFailureReasons_data()
{
    QTest::addColumn<QString>("caseName");
    QTest::addColumn<QString>("expectedReason");

    QTest::newRow("node_not_found") << QStringLiteral("missing")
                                    << QStringLiteral("node_not_found");
    QTest::newRow("not_qquickitem") << QStringLiteral("object")
                                    << QStringLiteral("not_qquickitem");
    QTest::newRow("not_visible") << QStringLiteral("invisibleItem")
                                 << QStringLiteral("not_visible");
    QTest::newRow("disabled") << QStringLiteral("disabledItem")
                              << QStringLiteral("disabled");
    QTest::newRow("zero_size") << QStringLiteral("zeroSizeItem")
                               << QStringLiteral("zero_size");
    QTest::newRow("center_outside_viewport") << QStringLiteral("centerOutsideViewportItem")
                                             << QStringLiteral("center_outside_viewport");
    QTest::newRow("unknown_window") << QStringLiteral("windowlessItem")
                                    << QStringLiteral("unknown_window");
}

void tst_QQmlAgentInput::clickNodeFailureReasons()
{
    QFETCH(QString, caseName);
    QFETCH(QString, expectedReason);

    int nodeId = -1;
    std::unique_ptr<QQuickWindow> window;
    std::unique_ptr<QObject> object;

    if (caseName == QLatin1String("object")) {
        object = std::make_unique<QObject>();
        nodeId = QQmlDebugService::idForObject(object.get());
    } else if (caseName == QLatin1String("invisibleItem")) {
        window = std::make_unique<QQuickWindow>();
        window->resize(100, 100);
        auto item = std::make_unique<QQuickItem>();
        item->setParentItem(window->contentItem());
        item->setWidth(10);
        item->setHeight(10);
        item->setVisible(false);
        nodeId = QQmlDebugService::idForObject(item.get());
        object = std::move(item);
    } else if (caseName == QLatin1String("disabledItem")) {
        window = std::make_unique<QQuickWindow>();
        window->resize(100, 100);
        auto item = std::make_unique<QQuickItem>();
        item->setParentItem(window->contentItem());
        item->setWidth(10);
        item->setHeight(10);
        item->setVisible(true);
        item->setEnabled(false);
        nodeId = QQmlDebugService::idForObject(item.get());
        object = std::move(item);
    } else if (caseName == QLatin1String("zeroSizeItem")) {
        window = std::make_unique<QQuickWindow>();
        window->resize(100, 100);
        auto item = std::make_unique<QQuickItem>();
        item->setParentItem(window->contentItem());
        item->setVisible(true);
        nodeId = QQmlDebugService::idForObject(item.get());
        object = std::move(item);
    } else if (caseName == QLatin1String("centerOutsideViewportItem")) {
        window = std::make_unique<QQuickWindow>();
        window->resize(100, 100);
        auto item = std::make_unique<QQuickItem>();
        item->setParentItem(window->contentItem());
        item->setY(200);
        item->setWidth(10);
        item->setHeight(10);
        item->setVisible(true);
        nodeId = QQmlDebugService::idForObject(item.get());
        object = std::move(item);
    } else if (caseName == QLatin1String("windowlessItem")) {
        auto item = std::make_unique<QQuickItem>();
        item->setWidth(10);
        item->setHeight(10);
        item->setVisible(true);
        nodeId = QQmlDebugService::idForObject(item.get());
        object = std::move(item);
    }

    const QJsonObject result = clickNode(nodeId);
    QCOMPARE(result.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("reason")).toString(), expectedReason);

    const QJsonArray diagnostics = result.value(QStringLiteral("diagnostics")).toArray();
    QCOMPARE(diagnostics.size(), 1);
    const QJsonObject diagnostic = diagnostics.at(0).toObject();
    QCOMPARE(diagnostic.value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_clickable"));
    QVERIFY2(!diagnostic.value(QStringLiteral("message")).toString().isEmpty(),
             qPrintable(QString::fromUtf8(QJsonDocument(diagnostic)
                                          .toJson(QJsonDocument::Compact))));
    if (expectedReason == QLatin1String("center_outside_viewport")) {
        const QJsonArray nextHints = result.value(QStringLiteral("nextHints")).toArray();
        QVERIFY2(!nextHints.isEmpty(),
                 qPrintable(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))));
        QCOMPARE(nextHints.at(0).toObject().value(QStringLiteral("method")).toString(),
                 QStringLiteral("Input.scrollIntoView"));
        QCOMPARE(nextHints.at(0).toObject().value(QStringLiteral("tool")).toString(),
                 QStringLiteral("qmlagent_input_scroll_into_view"));
        QVERIFY(nextHints.at(0).toObject().value(QStringLiteral("cli")).toString()
                        .contains(QStringLiteral("qmlagentctl scroll-into-view")));
    }
}

void tst_QQmlAgentInput::dispatchKeyEventRejectsInvalidKey()
{
    const QJsonObject result = QQmlAgentInput::dispatchKeyEvent({});
    QCOMPARE(result.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("invalid_key"));

    const QJsonArray diagnostics = result.value(QStringLiteral("diagnostics")).toArray();
    QCOMPARE(diagnostics.size(), 1);
    QCOMPARE(diagnostics.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_deliverable"));
}

void tst_QQmlAgentInput::dispatchMouseEventRejectsInvalidType()
{
    const QJsonObject result = QQmlAgentInput::dispatchMouseEvent(
            { { QStringLiteral("nodeId"), 1 } });
    QCOMPARE(result.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("invalid_type"));

    const QJsonArray diagnostics = result.value(QStringLiteral("diagnostics")).toArray();
    QCOMPARE(diagnostics.size(), 1);
    QCOMPARE(diagnostics.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_mouse_dispatchable"));
}

void tst_QQmlAgentInput::dispatchTouchEventRejectsInvalidPoints()
{
    const QJsonObject result = QQmlAgentInput::dispatchTouchEvent({
        { QStringLiteral("nodeId"), 1 },
        { QStringLiteral("type"), QStringLiteral("touchBegin") },
    });
    QCOMPARE(result.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("invalid_points"));

    const QJsonArray diagnostics = result.value(QStringLiteral("diagnostics")).toArray();
    QCOMPARE(diagnostics.size(), 1);
    QCOMPARE(diagnostics.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_touch_dispatchable"));
}

void tst_QQmlAgentInput::dragNodeRejectsMissingTarget()
{
    const QJsonObject result = QQmlAgentInput::dragNode({ { QStringLiteral("nodeId"), 1 } });
    QCOMPARE(result.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("invalid_points"));

    const QJsonArray diagnostics = result.value(QStringLiteral("diagnostics")).toArray();
    QCOMPARE(diagnostics.size(), 1);
    QCOMPARE(diagnostics.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_draggable"));
}

void tst_QQmlAgentInput::typeTextRejectsEmptyText()
{
    const QJsonObject result = QQmlAgentInput::typeText({});
    QCOMPARE(result.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("invalid_text"));

    const QJsonArray diagnostics = result.value(QStringLiteral("diagnostics")).toArray();
    QCOMPARE(diagnostics.size(), 1);
    QCOMPARE(diagnostics.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_deliverable"));
}

void tst_QQmlAgentInput::wheelRejectsMissingDelta()
{
    const QJsonObject result = QQmlAgentInput::wheel({ { QStringLiteral("nodeId"), 1 } });
    QCOMPARE(result.value(QStringLiteral("delivered")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("reason")).toString(), QStringLiteral("invalid_delta"));

    const QJsonArray diagnostics = result.value(QStringLiteral("diagnostics")).toArray();
    QCOMPARE(diagnostics.size(), 1);
    QCOMPARE(diagnostics.at(0).toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("input.not_wheelable"));
}

void tst_QQmlAgentInput::diagnosticsPromoteOnlyRepairRelevantLogs()
{
    QQmlAgentLogCollector logs;

    QTest::ignoreMessage(QtWarningMsg, "Populating font family aliases took 42 ms");
    QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, "qt.qpa.fonts")
            .warning("Populating font family aliases took 42 ms");
    QTest::ignoreMessage(QtWarningMsg, "ReferenceError: missingName is not defined");
    QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, "qml")
            .warning("ReferenceError: missingName is not defined");

    const QJsonObject result = QQmlAgentDiagnostics::analyzeTree({}, &logs);
    const QJsonArray issues = result.value(QStringLiteral("issues")).toArray();
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues.at(0).toObject().value(QStringLiteral("message")).toString(),
             QStringLiteral("ReferenceError: missingName is not defined"));

    const QJsonObject summary = result.value(QStringLiteral("summary")).toObject();
    QCOMPARE(summary.value(QStringLiteral("logEntryCount")).toInt(), 2);
    QCOMPARE(summary.value(QStringLiteral("promotedLogIssueCount")).toInt(), 1);
    QCOMPARE(summary.value(QStringLiteral("ignoredLogEntryCount")).toInt(), 1);
    QCOMPARE(summary.value(QStringLiteral("issueCount")).toInt(), issues.size());
}

void tst_QQmlAgentInput::waitForObservesInvisibleProperty()
{
    QQuickWindow window;
    window.resize(200, 200);
    QQuickItem item;
    item.setParentItem(window.contentItem());
    item.setObjectName(QStringLiteral("waitInvisibleTarget"));
    item.setWidth(20);
    item.setHeight(20);
    item.setVisible(true);

    QTimer::singleShot(100, &item, [&item]() { item.setVisible(false); });

    const QJsonObject result = QQmlAgentUiTree::waitFor({
        { QStringLiteral("selector"), QStringLiteral("objectName=\"waitInvisibleTarget\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("property"), QStringLiteral("visible") },
            { QStringLiteral("op"), QStringLiteral("=") },
            { QStringLiteral("value"), false },
        } },
        { QStringLiteral("timeoutMs"), 3000 },
    });

    QVERIFY2(result.value(QStringLiteral("ok")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))));
    QCOMPARE(result.value(QStringLiteral("reason")).toString(),
             QStringLiteral("predicate_satisfied"));
    QCOMPARE(result.value(QStringLiteral("actual")).toBool(true), false);
}

void tst_QQmlAgentInput::waitForPrefersUniqueVisibleMatch()
{
    QQuickWindow window;
    window.resize(200, 200);

    QQuickItem visibleItem;
    visibleItem.setParentItem(window.contentItem());
    visibleItem.setObjectName(QStringLiteral("waitDuplicateTarget"));
    visibleItem.setWidth(40);
    visibleItem.setHeight(20);
    visibleItem.setVisible(true);

    QQuickItem invisibleItem;
    invisibleItem.setParentItem(window.contentItem());
    invisibleItem.setObjectName(QStringLiteral("waitDuplicateTarget"));
    invisibleItem.setWidth(1);
    invisibleItem.setHeight(1);
    invisibleItem.setVisible(false);

    const QJsonObject result = QQmlAgentUiTree::waitFor({
        { QStringLiteral("selector"), QStringLiteral("objectName=\"waitDuplicateTarget\"") },
        { QStringLiteral("until"), QJsonObject{
            { QStringLiteral("property"), QStringLiteral("width") },
            { QStringLiteral("op"), QStringLiteral(">") },
            { QStringLiteral("value"), 10 },
        } },
        { QStringLiteral("timeoutMs"), 500 },
    });

    QVERIFY2(result.value(QStringLiteral("ok")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))));
    QCOMPARE(result.value(QStringLiteral("reason")).toString(),
             QStringLiteral("predicate_satisfied"));
}

void tst_QQmlAgentInput::dispatchBudgetsCoverLongRunningRequests()
{
    // UI.waitFor budget follows the requested timeout, top-level or in until.
    QCOMPARE(QQmlAgentUiTree::waitForBudgetMs({ { QStringLiteral("timeoutMs"), 20000 } }), 20000);
    QCOMPARE(QQmlAgentUiTree::waitForBudgetMs({
        { QStringLiteral("until"), QJsonObject{ { QStringLiteral("timeoutMs"), 8000 } } },
    }), 8000);
    QCOMPARE(QQmlAgentUiTree::waitForBudgetMs({ { QStringLiteral("timeoutMs"), 90000 } }), 30000);

    // Long press budget covers hold plus settle.
    QVERIFY(QQmlAgentInput::dispatchBudgetMs(QStringLiteral("Input.longPressNode"),
                                             { { QStringLiteral("holdMs"), 8000 } }) >= 8000);

    // Settle budgets are honored and clamped.
    const QJsonObject longSettle{
        { QStringLiteral("settle"), QJsonObject{ { QStringLiteral("timeoutMs"), 12000 } } },
    };
    QVERIFY(QQmlAgentInput::dispatchBudgetMs(QStringLiteral("Input.clickNode"), longSettle) >= 12000);
    QVERIFY(QQmlAgentRuntime::dispatchBudgetMs(longSettle) >= 12000);
    const QJsonObject hugeSettle{
        { QStringLiteral("settle"), QJsonObject{ { QStringLiteral("timeoutMs"), 600000 } } },
    };
    QVERIFY(QQmlAgentInput::dispatchBudgetMs(QStringLiteral("Input.clickNode"), hugeSettle) <= 60000);
}

static QString objectNameSelectorStability(const QJsonObject &tree, const QString &objectName)
{
    QList<QJsonObject> stack;
    const QJsonArray windows = tree.value(QStringLiteral("windows")).toArray();
    for (const QJsonValue &window : windows)
        stack.append(window.toObject().value(QStringLiteral("root")).toObject());
    while (!stack.isEmpty()) {
        const QJsonObject node = stack.takeLast();
        const QJsonArray children = node.value(QStringLiteral("children")).toArray();
        for (const QJsonValue &child : children)
            stack.append(child.toObject());
        if (node.value(QStringLiteral("objectName")).toString() != objectName)
            continue;
        const QJsonArray selectors = node.value(QStringLiteral("selectors")).toArray();
        for (const QJsonValue &selectorValue : selectors) {
            const QJsonObject selector = selectorValue.toObject();
            if (selector.value(QStringLiteral("kind")).toString()
                    == QLatin1String("objectName")) {
                return selector.value(QStringLiteral("stability")).toString();
            }
        }
    }
    return {};
}

static QJsonObject nodeByObjectName(const QJsonObject &tree, const QString &objectName)
{
    QList<QJsonObject> stack;
    const QJsonArray windows = tree.value(QStringLiteral("windows")).toArray();
    for (const QJsonValue &window : windows) {
        const QJsonObject windowObject = window.toObject();
        stack.append(windowObject.value(QStringLiteral("root")).toObject());
        stack.append(windowObject.value(QStringLiteral("window")).toObject());
    }
    while (!stack.isEmpty()) {
        const QJsonObject node = stack.takeLast();
        if (node.value(QStringLiteral("objectName")).toString() == objectName)
            return node;
        const QJsonArray children = node.value(QStringLiteral("children")).toArray();
        for (const QJsonValue &child : children)
            stack.append(child.toObject());
    }
    return {};
}

void tst_QQmlAgentInput::selectorStabilityReflectsTreeUniqueness()
{
    QQuickWindow window;
    window.resize(200, 200);

    QQuickItem first;
    first.setParentItem(window.contentItem());
    first.setObjectName(QStringLiteral("duplicateName"));
    QQuickItem second;
    second.setParentItem(window.contentItem());
    second.setObjectName(QStringLiteral("duplicateName"));
    QQuickItem third;
    third.setParentItem(window.contentItem());
    third.setObjectName(QStringLiteral("uniqueName"));

    const QJsonObject tree = QQmlAgentUiTree::getTree({
        { QStringLiteral("includeInvisible"), true },
    });

    QCOMPARE(objectNameSelectorStability(tree, QStringLiteral("duplicateName")),
             QStringLiteral("medium"));
    QCOMPARE(objectNameSelectorStability(tree, QStringLiteral("uniqueName")),
             QStringLiteral("high"));
}

void tst_QQmlAgentInput::queryManyAlignsResultsAndAppliesDefaults()
{
    QQuickWindow window;
    window.resize(200, 200);
    QQuickItem first;
    first.setParentItem(window.contentItem());
    first.setObjectName(QStringLiteral("batchFirst"));
    first.setWidth(11);
    QQuickItem second;
    second.setParentItem(window.contentItem());
    second.setObjectName(QStringLiteral("batchSecond"));
    second.setWidth(22);

    const QJsonObject batch = QQmlAgentUiTree::queryMany({
        { QStringLiteral("queries"), QJsonArray{
            QJsonObject{ { QStringLiteral("selector"), QStringLiteral("objectName=\"batchFirst\"") } },
            QJsonObject{ { QStringLiteral("selector"), QStringLiteral("objectName=\"batchSecond\"") } },
        } },
        { QStringLiteral("defaults"), QJsonObject{
            { QStringLiteral("includeInvisible"), true },
            { QStringLiteral("properties"), QJsonArray{ QStringLiteral("width") } },
        } },
    });

    QCOMPARE(batch.value(QStringLiteral("resultCount")).toInt(), 2);
    const QJsonArray results = batch.value(QStringLiteral("results")).toArray();
    QCOMPARE(results.size(), 2);
    const QJsonObject firstMatch = results.at(0).toObject()
            .value(QStringLiteral("matches")).toArray().at(0).toObject();
    const QJsonObject secondMatch = results.at(1).toObject()
            .value(QStringLiteral("matches")).toArray().at(0).toObject();
    QCOMPARE(firstMatch.value(QStringLiteral("objectName")).toString(),
             QStringLiteral("batchFirst"));
    QCOMPARE(secondMatch.value(QStringLiteral("objectName")).toString(),
             QStringLiteral("batchSecond"));
    QCOMPARE(firstMatch.value(QStringLiteral("properties")).toObject()
                     .value(QStringLiteral("width")).toDouble(), 11.0);
    QCOMPARE(secondMatch.value(QStringLiteral("properties")).toObject()
                     .value(QStringLiteral("width")).toDouble(), 22.0);

    const QJsonObject empty = QQmlAgentUiTree::queryMany({});
    QCOMPARE(empty.value(QStringLiteral("diagnostics")).toArray().at(0).toObject()
                     .value(QStringLiteral("id")).toString(),
             QStringLiteral("batch.queries_required"));

    QJsonArray tooMany;
    for (int i = 0; i < 51; ++i)
        tooMany.append(QJsonObject{ { QStringLiteral("selector"), QStringLiteral("nodeId=1") } });
    const QJsonObject overflow = QQmlAgentUiTree::queryMany({
        { QStringLiteral("queries"), tooMany },
    });
    QCOMPARE(overflow.value(QStringLiteral("diagnostics")).toArray().at(0).toObject()
                     .value(QStringLiteral("id")).toString(),
             QStringLiteral("batch.too_many_queries"));
}

void tst_QQmlAgentInput::windowObjectIsAddressable()
{
    QQuickWindow window;
    window.setObjectName(QStringLiteral("evalMainWindow"));
    window.setTitle(QStringLiteral("Eval Window"));
    window.resize(200, 200);

    const QJsonObject tree = QQmlAgentUiTree::getTree({});
    const QJsonArray windows = tree.value(QStringLiteral("windows")).toArray();
    QVERIFY(!windows.isEmpty());
    bool found = false;
    for (const QJsonValue &entry : windows) {
        const QJsonObject windowNode = entry.toObject()
                .value(QStringLiteral("window")).toObject();
        if (windowNode.value(QStringLiteral("objectName")).toString()
                == QLatin1String("evalMainWindow")) {
            found = true;
            QCOMPARE(windowNode.value(QStringLiteral("kind")).toString(),
                     QStringLiteral("QObject"));
        }
    }
    QVERIFY2(found, "window object node missing from UI.getTree");

    const QJsonObject queried = QQmlAgentUiTree::query({
        { QStringLiteral("selector"), QStringLiteral("objectName=\"evalMainWindow\"") },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("title") } },
    });
    const QJsonArray matches = queried.value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.at(0).toObject().value(QStringLiteral("properties")).toObject()
                     .value(QStringLiteral("title")).toString(),
             QStringLiteral("Eval Window"));
}

#ifdef QMLAGENT_HAS_QUICK3D
static bool jsonArrayContainsString(const QJsonArray &array, const QString &needle)
{
    for (const QJsonValue &value : array) {
        if (value.toString() == needle)
            return true;
    }
    return false;
}

static bool selectorArrayContainsTypeValue(const QJsonArray &selectors, const QString &value)
{
    for (const QJsonValue &selectorValue : selectors) {
        const QJsonObject selector = selectorValue.toObject();
        if (selector.value(QStringLiteral("kind")).toString() == QLatin1String("type")
                && selector.value(QStringLiteral("value")).toString() == value) {
            return true;
        }
    }
    return false;
}

void tst_QQmlAgentInput::quick3DReachabilityIsAutomatic()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQuick
import QtQuick.Window
import QtQml.Models
import QtQuick3D

Window {
    width: 320
    height: 240
    visible: false

    QtObject {
        id: transformSeed
        property vector3d cubeScale: Qt.vector3d(1.8, 1.8, 1.8)
    }

    ListModel {
        id: frameModel
        ListElement { xOffset: 0 }
        ListElement { xOffset: 50 }
    }

    View3D {
        id: view
        objectName: "quick3d.probe.view"
        anchors.fill: parent
        camera: camera
        environment: SceneEnvironment {
            id: sceneEnvironment
            backgroundMode: SceneEnvironment.Color
            clearColor: "#101010"
        }

        PerspectiveCamera {
            id: camera
            z: 600
        }

        DirectionalLight {
            id: keyLight
            brightness: 1.5
        }

        Model {
            id: cube
            objectName: "quick3d.probe.cube"
            source: "#Cube"
            scale: transformSeed.cubeScale
            materials: PrincipledMaterial {
                id: cubeMaterial
                baseColor: "#41cd52"
                baseColorMap: Texture {
                    id: cubeTexture
                    sourceItem: Rectangle {
                        id: textureSource
                        width: 8
                        height: 8
                        color: "#41cd52"
                    }
                }
            }
        }

        Repeater3D {
            id: frameRepeater
            objectName: "quick3d.probe.repeater"
            model: frameModel
            delegate: Model {
                source: "#Cube"
                x: xOffset
            }
        }
    }
}
)",
                      QUrl(QStringLiteral("file:///tmp/Quick3DReachability.qml")));

    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QVERIFY(qobject_cast<QQuickWindow *>(root.data()));

    const QJsonObject queried = QQmlAgentUiTree::query({
        { QStringLiteral("selector"), QStringLiteral("objectName=\"quick3d.probe.cube\"") },
        { QStringLiteral("includeInvisible"), true },
        { QStringLiteral("includeSource"), true },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("nodeId"),
            QStringLiteral("kind"),
            QStringLiteral("sceneKind"),
            QStringLiteral("type"),
            QStringLiteral("typeAliases"),
            QStringLiteral("qmlId"),
            QStringLiteral("objectName"),
            QStringLiteral("sourceLocation"),
            QStringLiteral("selectors"),
            QStringLiteral("properties"),
            QStringLiteral("render3D"),
        } },
        { QStringLiteral("properties"), QJsonArray{
            QStringLiteral("source"),
            QStringLiteral("scale"),
            QStringLiteral("sceneTransform"),
            QStringLiteral("materials"),
        } },
    });
    const QJsonArray matches = queried.value(QStringLiteral("matches")).toArray();
    QCOMPARE(matches.size(), 1);
    const QJsonObject model = matches.at(0).toObject();
    QCOMPARE(model.value(QStringLiteral("kind")).toString(), QStringLiteral("QQuick3DObject"));
    QCOMPARE(model.value(QStringLiteral("sceneKind")).toString(), QStringLiteral("QtQuick3D"));
    QCOMPARE(model.value(QStringLiteral("type")).toString(), QStringLiteral("Model"));
    const QJsonArray typeAliases = model.value(QStringLiteral("typeAliases")).toArray();
    QVERIFY(!jsonArrayContainsString(typeAliases, QStringLiteral("3DModel")));
    const QJsonArray selectors = model.value(QStringLiteral("selectors")).toArray();
    QVERIFY(selectorArrayContainsTypeValue(selectors, QStringLiteral("Model")));
    QVERIFY(!selectorArrayContainsTypeValue(selectors, QStringLiteral("3DModel")));
    QCOMPARE(model.value(QStringLiteral("qmlId")).toString(), QStringLiteral("cube"));
    QCOMPARE(model.value(QStringLiteral("objectName")).toString(),
             QStringLiteral("quick3d.probe.cube"));
    const QJsonObject render3D = model.value(QStringLiteral("render3D")).toObject();
    QVERIFY2(!render3D.value(QStringLiteral("available")).toBool(),
             qPrintable(QJsonDocument(render3D).toJson(QJsonDocument::Compact)));
    QCOMPARE(render3D.value(QStringLiteral("reason")).toString(),
             QStringLiteral("model_bounds_unavailable"));
    QVERIFY2(!render3D.contains(QStringLiteral("worldBounds")),
             qPrintable(QJsonDocument(render3D).toJson(QJsonDocument::Compact)));
    QVERIFY2(!render3D.value(QStringLiteral("projection")).toObject()
                    .value(QStringLiteral("available")).toBool(),
             qPrintable(QJsonDocument(render3D).toJson(QJsonDocument::Compact)));
    QCOMPARE(render3D.value(QStringLiteral("projection")).toObject()
                     .value(QStringLiteral("reason")).toString(),
             QStringLiteral("model_bounds_unavailable"));
    QVERIFY2(!render3D.contains(QStringLiteral("inFrustum")),
             qPrintable(QJsonDocument(render3D).toJson(QJsonDocument::Compact)));
    QCOMPARE(render3D.value(QStringLiteral("distanceFromCamera")).toObject()
                     .value(QStringLiteral("reason")).toString(),
             QStringLiteral("model_bounds_unavailable"));

    const QJsonObject pick = QQmlAgentRender::pick3D({
        { QStringLiteral("selector"), QStringLiteral("objectName=\"quick3d.probe.view\"") },
        { QStringLiteral("x"), 10 },
        { QStringLiteral("y"), 10 },
    });
    QVERIFY2(pick.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(pick).toJson(QJsonDocument::Compact)));
    QCOMPARE(pick.value(QStringLiteral("coordinateSpace")).toString(),
             QStringLiteral("view3d-local-logical-pixels"));
    QCOMPARE(pick.value(QStringLiteral("hit")).toBool(), false);
    QCOMPARE(pick.value(QStringLiteral("pick")).toObject()
                     .value(QStringLiteral("hitType")).toObject()
                     .value(QStringLiteral("name")).toString(),
             QStringLiteral("Null"));
    QCOMPARE(pick.value(QStringLiteral("point")).toObject()
                     .value(QStringLiteral("x")).toInt(),
             10);
    QCOMPARE(model.value(QStringLiteral("properties")).toObject()
                     .value(QStringLiteral("source")).toString(),
             QStringLiteral("#Cube"));
    const QJsonObject modelProperties = model.value(QStringLiteral("properties")).toObject();
    const QJsonObject scale = modelProperties.value(QStringLiteral("scale")).toObject();
    QVERIFY(qAbs(scale.value(QStringLiteral("x")).toDouble() - 1.8) < 0.001);
    QVERIFY(qAbs(scale.value(QStringLiteral("y")).toDouble() - 1.8) < 0.001);
    QVERIFY(qAbs(scale.value(QStringLiteral("z")).toDouble() - 1.8) < 0.001);
    const QJsonArray sceneTransform = modelProperties.value(QStringLiteral("sceneTransform")).toArray();
    QCOMPARE(sceneTransform.size(), 4);
    QCOMPARE(sceneTransform.at(0).toArray().size(), 4);
    const QJsonObject materials = modelProperties.value(QStringLiteral("materials")).toObject();
    QCOMPARE(materials.value(QStringLiteral("kind")).toString(), QStringLiteral("QObjectListRef"));
    QCOMPARE(materials.value(QStringLiteral("count")).toInt(), 1);
    const QJsonObject materialRef = materials.value(QStringLiteral("items")).toArray().at(0).toObject();
    QCOMPARE(materialRef.value(QStringLiteral("qmlId")).toString(), QStringLiteral("cubeMaterial"));
    QVERIFY2(materialRef.value(QStringLiteral("selector")).toString()
                    .contains(QStringLiteral("cubeMaterial")),
             qPrintable(QJsonDocument(materialRef).toJson(QJsonDocument::Compact)));

    const QJsonObject sourceLocation = model.value(QStringLiteral("sourceLocation")).toObject();
    QCOMPARE(sourceLocation.value(QStringLiteral("method")).toString(),
             QStringLiteral("qqmldata-direct"));
    QVERIFY(sourceLocation.value(QStringLiteral("file")).toString()
                    .endsWith(QStringLiteral("Quick3DReachability.qml")));
    QVERIFY(sourceLocation.value(QStringLiteral("line")).toInt() > 0);

    const QJsonObject viewQuery = QQmlAgentUiTree::query({
        { QStringLiteral("selector"), QStringLiteral("objectName=\"quick3d.probe.view\"") },
        { QStringLiteral("includeInvisible"), true },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("type"),
            QStringLiteral("properties"),
        } },
        { QStringLiteral("properties"), QJsonArray{
            QStringLiteral("camera"),
            QStringLiteral("environment"),
        } },
    });
    const QJsonObject viewProperties = viewQuery.value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject();
    QCOMPARE(viewProperties.value(QStringLiteral("camera")).toObject()
                     .value(QStringLiteral("qmlId")).toString(),
             QStringLiteral("camera"));
    QCOMPARE(viewProperties.value(QStringLiteral("environment")).toObject()
                     .value(QStringLiteral("qmlId")).toString(),
             QStringLiteral("sceneEnvironment"));

    const QJsonObject materialQuery = QQmlAgentUiTree::query({
        { QStringLiteral("selector"), QStringLiteral("id=\"cubeMaterial\"") },
        { QStringLiteral("includeInvisible"), true },
        { QStringLiteral("fields"), QJsonArray{ QStringLiteral("properties") } },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("baseColorMap") } },
    });
    const QJsonObject textureRef = materialQuery.value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("baseColorMap")).toObject();
    QCOMPARE(textureRef.value(QStringLiteral("qmlId")).toString(), QStringLiteral("cubeTexture"));

    const QJsonObject textureQuery = QQmlAgentUiTree::query({
        { QStringLiteral("selector"), QStringLiteral("id=\"cubeTexture\"") },
        { QStringLiteral("includeInvisible"), true },
        { QStringLiteral("fields"), QJsonArray{ QStringLiteral("properties") } },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("sourceItem") } },
    });
    const QJsonObject sourceItemRef = textureQuery.value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("sourceItem")).toObject();
    QCOMPARE(sourceItemRef.value(QStringLiteral("qmlId")).toString(), QStringLiteral("textureSource"));

    const QJsonObject repeaterQuery = QQmlAgentUiTree::query({
        { QStringLiteral("selector"), QStringLiteral("objectName=\"quick3d.probe.repeater\"") },
        { QStringLiteral("includeInvisible"), true },
        { QStringLiteral("fields"), QJsonArray{ QStringLiteral("properties") } },
        { QStringLiteral("properties"), QJsonArray{ QStringLiteral("model") } },
    });
    const QJsonObject repeaterModelRef = repeaterQuery.value(QStringLiteral("matches")).toArray()
            .at(0).toObject().value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("model")).toObject();
    QCOMPARE(repeaterModelRef.value(QStringLiteral("qmlId")).toString(), QStringLiteral("frameModel"));

    const QJsonObject binding = QQmlAgentDiagnostics::analyzeBinding({
        { QStringLiteral("selector"), QStringLiteral("id=\"cube\"") },
        { QStringLiteral("property"), QStringLiteral("scale") },
    });
    QVERIFY2(binding.value(QStringLiteral("ok")).toBool(),
             qPrintable(QJsonDocument(binding).toJson(QJsonDocument::Compact)));
    QVERIFY(qAbs(binding.value(QStringLiteral("value")).toObject()
                         .value(QStringLiteral("x")).toDouble() - 1.8) < 0.001);
    const QJsonArray dependencies = binding.value(QStringLiteral("provenance")).toObject()
            .value(QStringLiteral("dependencies")).toArray();
    QVERIFY2(!dependencies.isEmpty(), qPrintable(QJsonDocument(binding).toJson(QJsonDocument::Compact)));
    QVERIFY(qAbs(dependencies.at(0).toObject().value(QStringLiteral("value")).toObject()
                         .value(QStringLiteral("x")).toDouble() - 1.8) < 0.001);

    const QJsonObject camera = QQmlAgentUiTree::query({
        { QStringLiteral("selector"), QStringLiteral("id=\"camera\"") },
        { QStringLiteral("includeInvisible"), true },
        { QStringLiteral("fields"), QJsonArray{ QStringLiteral("type"), QStringLiteral("kind") } },
    });
    QCOMPARE(camera.value(QStringLiteral("matches")).toArray().size(), 1);
    QCOMPARE(camera.value(QStringLiteral("matches")).toArray().at(0).toObject()
                     .value(QStringLiteral("type")).toString(),
             QStringLiteral("PerspectiveCamera"));
}

void tst_QQmlAgentInput::quick3DView3DIncludesSceneChildrenAutomatically()
{
    QQmlEngine engine;
    QQmlComponent sceneComponent(&engine);
    sceneComponent.setData(R"(
import QtQuick
import QtQuick.Window
import QtQuick3D

Window {
    width: 320
    height: 240
    visible: false

    View3D {
        id: viewWithScene
        objectName: "quick3d.view.withScene"
        anchors.fill: parent

        Model {
            id: cube
            objectName: "quick3d.nudge.cube"
            source: "#Cube"
        }
    }
}
)",
                           QUrl(QStringLiteral("file:///tmp/Quick3DNudge.qml")));
    QScopedPointer<QObject> sceneRoot(sceneComponent.create());
    QVERIFY2(sceneRoot, qPrintable(sceneComponent.errorString()));

    const QJsonObject sceneTree = QQmlAgentUiTree::getTree({
        { QStringLiteral("depth"), -1 },
        { QStringLiteral("includeInvisible"), true },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("objectName"),
            QStringLiteral("kind"),
            QStringLiteral("sceneKind"),
            QStringLiteral("type"),
            QStringLiteral("children"),
        } },
    });
    const QJsonObject viewNode = nodeByObjectName(sceneTree,
                                                  QStringLiteral("quick3d.view.withScene"));
    QVERIFY(!viewNode.isEmpty());
    QCOMPARE(viewNode.value(QStringLiteral("type")).toString(), QStringLiteral("View3D"));
    QCOMPARE(viewNode.value(QStringLiteral("sceneKind")).toString(), QStringLiteral("QtQuick3D"));
    const QJsonObject cubeNode = nodeByObjectName(sceneTree, QStringLiteral("quick3d.nudge.cube"));
    QVERIFY(!cubeNode.isEmpty());
    QCOMPARE(cubeNode.value(QStringLiteral("kind")).toString(), QStringLiteral("QQuick3DObject"));
    QCOMPARE(cubeNode.value(QStringLiteral("sceneKind")).toString(), QStringLiteral("QtQuick3D"));

    QQmlComponent emptyComponent(&engine);
    emptyComponent.setData(R"(
import QtQuick
import QtQuick.Window
import QtQuick3D

Window {
    width: 320
    height: 240
    visible: false

    View3D {
        objectName: "quick3d.view.empty"
        anchors.fill: parent
    }
}
)",
                           QUrl(QStringLiteral("file:///tmp/Quick3DNoNudge.qml")));
    QScopedPointer<QObject> emptyRoot(emptyComponent.create());
    QVERIFY2(emptyRoot, qPrintable(emptyComponent.errorString()));

    const QJsonObject emptyTree = QQmlAgentUiTree::getTree({
        { QStringLiteral("depth"), -1 },
        { QStringLiteral("includeInvisible"), true },
        { QStringLiteral("fields"), QJsonArray{
            QStringLiteral("objectName"),
            QStringLiteral("kind"),
            QStringLiteral("sceneKind"),
            QStringLiteral("type"),
            QStringLiteral("children"),
        } },
    });
    const QJsonObject emptyNode = nodeByObjectName(emptyTree,
                                                   QStringLiteral("quick3d.view.empty"));
    QVERIFY(!emptyNode.isEmpty());
    QCOMPARE(emptyNode.value(QStringLiteral("type")).toString(), QStringLiteral("View3D"));
    QCOMPARE(emptyNode.value(QStringLiteral("sceneKind")).toString(), QStringLiteral("QtQuick3D"));
}
#endif

void tst_QQmlAgentInput::scrollIntoViewAdjustsAncestorFlickable()
{
    QQuickWindow window;
    window.resize(200, 200);
    QQuickFlickable flickable;
    flickable.setParentItem(window.contentItem());
    flickable.setWidth(200);
    flickable.setHeight(200);
    flickable.setContentHeight(1000);
    flickable.setContentWidth(200);

    QQuickItem target;
    target.setParentItem(flickable.contentItem());
    target.setObjectName(QStringLiteral("deepItem"));
    target.setY(900);
    target.setWidth(100);
    target.setHeight(40);
    const int nodeId = QQmlDebugService::idForObject(&target);

    const QJsonObject result = QQmlAgentInput::scrollIntoView(
            { { QStringLiteral("nodeId"), nodeId } });
    QVERIFY2(result.value(QStringLiteral("delivered")).toBool(false),
             qPrintable(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))));
    QCOMPARE(result.value(QStringLiteral("scrolled")).toBool(false), true);
    QVERIFY(flickable.contentY() > 0);
    // The item's window-relative box must now intersect the viewport.
    const QPointF inWindow = target.mapToScene(QPointF(0, 0));
    QVERIFY2(inWindow.y() >= 0 && inWindow.y() < 200,
             qPrintable(QStringLiteral("y=%1").arg(inWindow.y())));

    // Items with no scrollable ancestor report that honestly.
    QQuickItem flat;
    flat.setParentItem(window.contentItem());
    flat.setWidth(10);
    flat.setHeight(10);
    const QJsonObject noScroll = QQmlAgentInput::scrollIntoView(
            { { QStringLiteral("nodeId"), QQmlDebugService::idForObject(&flat) } });
    QCOMPARE(noScroll.value(QStringLiteral("scrolled")).toBool(true), false);
    QCOMPARE(noScroll.value(QStringLiteral("reason")).toString(),
             QStringLiteral("no_scrollable_ancestor"));
}

void tst_QQmlAgentInput::dismissPopupReportsNoVisiblePopup()
{
    // With no popup in the scene the route reports that honestly rather than
    // claiming a dismissal. (Closing behaviour is covered by live corpus
    // verification, since a working QQuickPopup needs an ApplicationWindow
    // overlay this fixture does not build.)
    QQuickWindow window;
    window.resize(100, 100);
    const QJsonObject result = QQmlAgentInput::dismissPopup({});
    QCOMPARE(result.value(QStringLiteral("dismissed")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("reason")).toString(),
             QStringLiteral("no_visible_popup"));
    QCOMPARE(result.value(QStringLiteral("popupCount")).toInt(-1), 0);
}

QTEST_MAIN(tst_QQmlAgentInput)

#include "tst_qmlagentinput.moc"
