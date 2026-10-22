import QtQuick

// RAG-панель. Ребёнок overlayContent: наследует его opacity/visible,
// поэтому клавиатурная interactivity (exclusive только при active чате)
// работает в поле поиска без отдельного переключения.
//
// Сам по себе Item на весь экран, но MouseArea нет -> пустые области
// не перехватывают мышь, чат под панелью кликабелен.
Item {
    id: root

    property bool expanded: false

    // акценты RAG: фиолетовый основной + розовый вторичный (flyingColor окна)
    readonly property color cAcc:   "#a78bfa"
    readonly property color cAcc2:  "#f472b6"
    readonly property color cOk:    "#4ade80"
    readonly property color cWarn:  "#facc15"
    readonly property color cBad:   "#f87171"
    readonly property color cFg:    Qt.rgba(0.92, 0.93, 0.96, 0.92)
    readonly property color cDim:   Qt.rgba(0.92, 0.93, 0.96, 0.45)
    readonly property color cDim2:  Qt.rgba(0.92, 0.93, 0.96, 0.28)
    readonly property color cPane:  Qt.rgba(0.055, 0.058, 0.09, 0.82)
    readonly property color cEdge:  Qt.rgba(0.65, 0.55, 0.98, 0.55)
    readonly property color cEdge2: Qt.rgba(0.65, 0.55, 0.98, 0.22)

    // ── FAB справа снизу ────────────────────────────────────────────────
    Item {
        id: fabZone
        visible: !editor.visible
        width: 56
        height: 56
        anchors.right: parent.right
        anchors.rightMargin: 22
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 22

        // пульс-кольцо при занятости (llama-server поднялся / считается)
        Rectangle {
            anchors.centerIn: parent
            width: 56
            height: 56
            radius: 28
            color: "transparent"
            border.width: 1.5
            border.color: root.cAcc
            visible: rag.busy
            opacity: 0

            SequentialAnimation on scale {
                running: rag.busy
                loops: Animation.Infinite
                PauseAnimation { duration: 0 }
                NumberAnimation { from: 1.0; to: 1.55; duration: 900; easing.type: Easing.OutQuad }
            }
            SequentialAnimation on opacity {
                running: rag.busy
                loops: Animation.Infinite
                NumberAnimation { from: 0.55; to: 0.0; duration: 900; easing.type: Easing.OutQuad }
            }
        }

        Rectangle {
            id: fab
            anchors.fill: parent
            radius: 28
            color: root.expanded ? Qt.rgba(0.42, 0.34, 0.78, 0.92)
                                 : Qt.rgba(0.055, 0.058, 0.09, 0.86)
            border.width: 1
            border.color: root.expanded ? root.cAcc : root.cEdge2

            Behavior on color { ColorAnimation { duration: 160 } }
            Behavior on border.color { ColorAnimation { duration: 160 } }
            Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

            scale: fabMa.containsMouse ? 1.07 : 1.0

            // акцент-полоска как у карточек чата, но дугой-индикатором занятости
            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: 9
                width: 3
                radius: 2
                color: Qt.rgba(root.cAcc.r, root.cAcc.g, root.cAcc.b,
                               rag.working ? 0.95 : 0.55)
                Behavior on color { ColorAnimation { duration: 140 } }

                SequentialAnimation on opacity {
                    running: rag.working
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.35; duration: 420 }
                    NumberAnimation { to: 1.0;  duration: 420 }
                }
            }

            Text {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: 2
                text: "RAG"
                color: root.cFg
                font.pixelSize: 13
                font.bold: true
                font.letterSpacing: 0.5
            }

            MouseArea {
                id: fabMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.expanded = !root.expanded
            }
        }
    }

    // ── браузер ─────────────────────────────────────────────────────────
    RagBrowser {
        id: browser
        visible: root.expanded
        opacity: visible ? 1.0 : 0.0
        scale: visible ? 1.0 : 0.94
        transformOrigin: Item.BottomRight

        Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

        width: 380
        height: Math.min(520, root.height - 150)

        anchors.right: editor.visible ? editor.left : parent.right
        anchors.rightMargin: 12
        anchors.bottom: fabZone.top
        anchors.bottomMargin: 12
    }

    // ── редактор: правый док ────────────────────────────────────────────
    RagEditor {
        id: editor
        visible: rag.hasSelection
        opacity: visible ? 1.0 : 0.0
        scale: visible ? 1.0 : 0.97
        transformOrigin: Item.Right

        Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

        width: Math.min(460, root.width * 0.34)
        anchors.right: parent.right
        anchors.rightMargin: 22
        anchors.top: parent.top
        anchors.topMargin: 22
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 22
    }
}
