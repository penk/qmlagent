// Copyright (C) 2026 Penk Chen <penkia@gmail.com>
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QmlAgentSmoke.Testing

Window {
    id: root
    property bool clicked: false
    property bool secondClicked: false
    property int selectedDelegateIndex: -1
    property bool subscribedDynamicAdded: false
    property bool genericBlockedClicked: false
    property bool labelledControlClicked: false
    property bool slowHandlerRan: false
    property bool grabberDragActivated: false
    property bool movingButtonClicked: false
    property bool longPressed: false
    property bool secondaryWindowClicked: false
    property int bindingBase: 70

    width: 320
    height: 240
    visible: true
    title: "QmlAgent smoke"

    Window {
        id: secondaryWindow
        width: 180
        height: 120
        visible: true
        title: "QmlAgent secondary smoke"

        Rectangle {
            property int keyPressCount: 0

            objectName: root.secondaryWindowClicked ? "smoke.secondaryWindowClicked"
                                                    : "smoke.secondaryWindowContent"
            anchors.fill: parent
            color: root.secondaryWindowClicked ? "#1a7f37" : "#0969da"
            focus: true

            Keys.onPressed: function(event) {
                ++keyPressCount
                event.accepted = true
            }

            MouseArea {
                objectName: "smoke.secondaryWindowClickArea"
                anchors.fill: parent
                onClicked: root.secondaryWindowClicked = true
            }
        }
    }

    Rectangle {
        id: content
        property int keyPressCount: 0

        objectName: root.clicked ? "smoke.clicked" : "smoke.content"
        width: 100
        height: 40
        color: root.clicked ? "#2f7d32" : "#1f6feb"
        focus: true

        Keys.onPressed: function(event) {
            ++keyPressCount
            event.accepted = true
        }

        MouseArea {
            objectName: "smoke.clickArea"
            anchors.fill: parent
            onClicked: root.clicked = true
        }
    }

    Rectangle {
        id: secondContent
        objectName: root.secondClicked ? "smoke.secondClicked" : "smoke.secondContent"
        x: 130
        width: 100
        height: 40
        color: root.secondClicked ? "#bf3989" : "#8250df"

        MouseArea {
            objectName: "smoke.secondClickArea"
            anchors.fill: parent
            onClicked: root.secondClicked = true
        }
    }

    Rectangle {
        id: hiddenProbe
        objectName: "smoke.hiddenProbe"
        visible: false
        width: 20
        height: 20
    }

    Rectangle {
        id: scaledProbe
        objectName: "smoke.scaledProbe"
        x: 260
        width: 100
        height: 40
        scale: 0.5
        transformOrigin: Item.TopLeft
        color: "#d29922"
    }

    Loader {
        objectName: "smoke.loader"
        y: 60
        sourceComponent: Component {
            Rectangle {
                objectName: "smoke.loaded"
                width: 80
                height: 30
                color: "#8a63d2"
            }
        }
    }

    Item {
        id: dynamicHost
        objectName: "smoke.dynamicHost"
        y: 95

        Component {
            id: dynamicComponent
            Rectangle {
                objectName: "smoke.dynamic"
                width: 80
                height: 30
                color: "#0969da"
            }
        }

        Component.onCompleted: dynamicComponent.createObject(dynamicHost)

        Component {
            id: subscribedDynamicComponent
            Rectangle {
                objectName: "smoke.subscribedDynamic"
                x: 96
                width: 64
                height: 24
                color: "#54aeff"

                MouseArea {
                    objectName: "smoke.subscribedDynamicClickArea"
                    anchors.fill: parent
                    onClicked: parent.objectName = "smoke.subscribedDynamicClicked"
                }
            }
        }
    }

    Rectangle {
        objectName: "smoke.addSubscribedDynamic"
        x: 180
        y: 94
        width: 34
        height: 28
        color: root.subscribedDynamicAdded ? "#1a7f37" : "#6e7781"

        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (!root.subscribedDynamicAdded) {
                    subscribedDynamicComponent.createObject(dynamicHost)
                    root.subscribedDynamicAdded = true
                }
            }
        }
    }

    Repeater {
        model: 1
        delegate: Rectangle {
            objectName: "smoke.delegate"
            y: 110
            width: 80
            height: 30
            color: "#d29922"
        }
    }

    Repeater {
        model: 3
        delegate: Rectangle {
            objectName: "smoke.repeatedDelegate"
            x: 230 + index * 14
            y: 110
            width: 10
            height: 10
            color: "#1a7f37"
        }
    }

    Repeater {
        model: [
            { "label": "Alpha" },
            { "label": "Beta" }
        ]

        delegate: Rectangle {
            id: indexedDelegateRow
            required property int index
            required property var modelData

            x: 8 + index * 74
            y: 48
            width: 66
            height: 24
            color: root.selectedDelegateIndex === index ? "#1a7f37" : "#d0d7de"

            Text {
                anchors.centerIn: parent
                text: modelData.label
                color: "#24292f"
            }

            MouseArea {
                id: indexedDelegateTap
                anchors.fill: parent
                onClicked: root.selectedDelegateIndex = index
            }
        }
    }

    Repeater {
        model: [
            { "label": "Gamma" },
            { "label": "Delta" }
        ]

        delegate: Text {
            id: indexedTextContainer
            required property int index
            required property var modelData

            text: modelData.label
            x: 8 + index * 74
            y: 150
            width: 66
            height: 18
            color: "#24292f"

            Text {
                anchors.centerIn: parent
                text: indexedTextContainer.text
                color: "#57606a"
            }
        }
    }

    Text {
        id: indexedDelegateProbe
        property int selectedIndex: root.selectedDelegateIndex
        x: 160
        y: 50
        text: selectedIndex < 0 ? "" : String(selectedIndex)
        color: "#24292f"
    }

    Rectangle {
        id: runtimeProbe
        property int counter: 0
        property string label: "idle"

        function setRuntimeCounter(value) {
            counter = value
            label = "method-" + String(value)
            return counter
        }

        x: 230
        y: 48
        width: 42
        height: 24
        color: label === "idle" ? "#8ecae6" : "#219ebc"
    }

    Rectangle {
        id: bindingProbe
        x: root.bindingBase + 3
        y: 80
        width: 28
        height: 18
        color: "#6f42c1"
    }

    Rectangle {
        id: literalProbe
        x: 14
        y: 102
        width: 24
        height: 16
        color: "#fb8500"

        Rectangle {
            id: nestedLiteralProbe
            x: 2
            y: 777
            width: 6
            height: 6
            color: "#023047"
        }
    }

    Rectangle {
        id: slowHandlerButton
        objectName: root.slowHandlerRan ? "smoke.slowHandlerRan"
                                        : "smoke.slowHandlerButton"
        x: 264
        y: 84
        width: 48
        height: 20
        color: root.slowHandlerRan ? "#1a7f37" : "#9a6700"

        MouseArea {
            anchors.fill: parent
            // Deliberately blocks the GUI thread well past the input dispatch
            // budget to exercise the timeout/grace/busy dispatch contract.
            onClicked: {
                var start = Date.now()
                while (Date.now() - start < 8000) {}
                root.slowHandlerRan = true
            }
        }
    }

    Rectangle {
        id: labelledControl
        objectName: root.labelledControlClicked ? "smoke.labelledControlClicked"
                                                : "smoke.labelledControl"
        x: 190
        y: 84
        width: 70
        height: 20
        color: root.labelledControlClicked ? "#1a7f37" : "#6e7781"

        Text {
            anchors.centerIn: parent
            text: "Press me"
            color: "#ffffff"
        }

        MouseArea {
            anchors.fill: parent
            onClicked: root.labelledControlClicked = true
        }
    }

    Rectangle {
        id: genericBlockedTarget
        objectName: "smoke.genericBlockedTarget"
        x: 278
        y: 48
        width: 32
        height: 24
        color: root.genericBlockedClicked ? "#1a7f37" : "#cf222e"

        MouseArea {
            anchors.fill: parent
            onClicked: root.genericBlockedClicked = true
        }
    }

    Rectangle {
        objectName: "smoke.genericBlocker"
        x: 270
        y: 42
        z: 50
        width: 46
        height: 36
        color: "#8c959f"
        opacity: 0.85

        MouseArea {
            objectName: "smoke.genericBlockerMouseArea"
            anchors.fill: parent
        }
    }

    Repeater {
        model: 2
        delegate: Rectangle {
            id: indexedDuplicateRow
            x: 8 + index * 18
            y: 78
            width: 12
            height: 12
            color: "#fb8500"
        }
    }

    TextInput {
        objectName: "smoke.textInput"
        x: 100
        y: 150
        width: 80
        height: 28
    }

    TextInput {
        objectName: "smoke.readOnlyTextInput"
        x: 190
        y: 150
        width: 100
        height: 28
        text: "locked"
        readOnly: true
    }

    TextInput {
        id: smokeNoClickFocusTextInput
        objectName: "smoke.noClickFocusTextInput"
        x: 100
        y: 184
        width: 80
        height: 28

        TapHandler {
            acceptedButtons: Qt.LeftButton
            onTapped: root.forceActiveFocus()
        }
    }

    Item {
        id: smokeTouchProbe
        x: 8
        y: 132
        width: 72
        height: 38
        property int pressedCount: 0
        property real span: 0
        property bool released: false

        Rectangle {
            anchors.fill: parent
            color: smokeTouchProbe.released ? "#57606a" : "#ddf4ff"
            border.color: "#0969da"
        }

        MultiPointTouchArea {
            id: smokeTouchArea
            anchors.fill: parent
            minimumTouchPoints: 1
            maximumTouchPoints: 2
            touchPoints: [
                TouchPoint { id: smokeTouchPoint1 },
                TouchPoint { id: smokeTouchPoint2 }
            ]
            onPressed: function(points) {
                smokeTouchProbe.released = false
                smokeTouchProbe.pressedCount = points.length
                smokeTouchProbe.span = Math.abs(smokeTouchPoint2.x - smokeTouchPoint1.x)
            }
            onUpdated: function(points) {
                smokeTouchProbe.pressedCount = points.length
                smokeTouchProbe.span = Math.abs(smokeTouchPoint2.x - smokeTouchPoint1.x)
            }
            onReleased: function(points) {
                smokeTouchProbe.released = true
                smokeTouchProbe.pressedCount = points.length
                smokeTouchProbe.span = Math.abs(smokeTouchPoint2.x - smokeTouchPoint1.x)
            }
        }
    }

    Flickable {
        id: smokeFlickable
        objectName: "smoke.flickable"
        x: 8
        y: 182
        width: 112
        height: 46
        contentWidth: width
        contentHeight: 180
        clip: true

        Rectangle {
            width: smokeFlickable.width
            height: smokeFlickable.contentHeight
            color: "#f6f8fa"

            Repeater {
                model: 6
                delegate: Text {
                    x: 6
                    y: index * 28 + 6
                    text: "Row " + (index + 1)
                    color: "#24292f"
                }
            }
        }
    }

    Rectangle {
        id: smokeDragTarget
        objectName: "smoke.dragTarget"
        x: 140
        y: 190
        width: 24
        height: 24
        color: "#0969da"

        MouseArea {
            id: smokeDragArea
            anchors.fill: parent
            drag.target: parent
            drag.axis: Drag.XAxis
            drag.minimumX: 140
            drag.maximumX: 260
        }
    }

    Rectangle {
        id: smokeDragCommandTarget
        objectName: "smoke.dragCommandTarget"
        x: 170
        y: 216
        width: 20
        height: 20
        color: "#1a7f37"

        MouseArea {
            id: smokeDragCommandArea
            anchors.fill: parent
            drag.target: parent
            drag.axis: Drag.XAxis
            drag.minimumX: 170
            drag.maximumX: 270
        }
    }

    // A small DragHandler-driven grabber whose drag lane crosses an
    // interactive obstacle. Dragging the 24px grabber rightward travels over
    // smoke.dragObstacle, which the old path-wide actionability preflight
    // rejected as blocked_by_item (F-024). The press point is on the grabber
    // and clear, so the drag must deliver and the DragHandler must move it.
    Rectangle {
        id: smokeDragTrack
        objectName: "smoke.dragTrack"
        x: 90
        y: 130
        width: 112
        height: 26
        color: "#d0d7de"

        Rectangle {
            id: smokeDragObstacle
            objectName: "smoke.dragObstacleRect"
            x: 40
            width: 72
            height: 26
            color: "#ffd8a8"
            MouseArea {
                objectName: "smoke.dragObstacle"
                anchors.fill: parent
            }
        }

        Rectangle {
            id: smokeGrabber
            objectName: "smoke.grabber"
            z: 1
            width: 24
            height: 26
            color: smokeGrabberHandler.active ? "#1a7f37" : "#8250df"

            DragHandler {
                id: smokeGrabberHandler
                xAxis.enabled: true
                xAxis.minimum: 0
                xAxis.maximum: smokeDragTrack.width - smokeGrabber.width
                yAxis.enabled: false
                onActiveChanged: if (active) root.grabberDragActivated = true
            }
        }
    }

    // A button that jumps out from under the click point when clicked. The
    // post-dispatch recheck must not report the now-vacated point as
    // blocked_by_item; it must flag targetMovedSinceDispatch and recheck the
    // target where it actually is. (F-025)
    Rectangle {
        id: smokeMovingButton
        objectName: root.movingButtonClicked ? "smoke.movingButtonClicked"
                                             : "smoke.movingButton"
        x: root.movingButtonClicked ? 250 : 90
        y: 158
        width: 40
        height: 18
        color: "#0969da"
        MouseArea {
            anchors.fill: parent
            onClicked: root.movingButtonClicked = true
        }
    }

    Item {
        objectName: "smoke.clipParent"
        x: 180
        width: 40
        height: 40
        clip: true

        Rectangle {
            objectName: "smoke.childOverflow"
            x: 30
            width: 40
            height: 20
            color: "#cf222e"
        }
    }

    Item {
        objectName: "smoke.spacer"
        y: 150
        width: 1
        height: 90
    }

    Rectangle {
        objectName: "smoke.offscreenAfterSpacer"
        y: 250
        width: 80
        height: 30
        color: "#6e7781"
    }

    Rectangle {
        objectName: "smoke.centerOffscreen"
        x: 10000
        y: 10000
        width: 24
        height: 24
        color: "#cf222e"
    }

    Rectangle {
        x: -20
        y: 180
        width: 60
        height: 60
        color: "#bf8700"

        MouseArea {
            objectName: "smoke.partiallyVisiblePressArea"
            anchors.fill: parent
        }
    }

    Rectangle {
        objectName: root.longPressed ? "smoke.longPressed" : "smoke.longPressTarget"
        x: 70
        y: 180
        width: 80
        height: 40
        color: root.longPressed ? "#2f7d32" : "#8250df"

        MouseArea {
            objectName: "smoke.longPressArea"
            anchors.fill: parent
            onPressAndHold: root.longPressed = true
        }
    }

    ValueTypeItem {
        id: valueTypeItem
        x: 300
        y: 210
        width: 1
        height: 1
    }

    Component.onCompleted: console.warn("QmlAgent smoke warning")
}
