import QtQuick

Rectangle {
    id: ed

    property color cAcc:  "#a78bfa"
    property color cAcc2: "#f472b6"
    property color cFg:   Qt.rgba(0.92, 0.93, 0.96, 0.92)
    property color cDim:  Qt.rgba(0.92, 0.93, 0.96, 0.45)
    property color cDim2: Qt.rgba(0.92, 0.93, 0.96, 0.28)

    property bool creating: rag.editorMode === "create"
    property bool confirmDelete: false
    property bool loading: false
    property bool dirty: false
    property string fType: "docs"
    property real phase: 0

    color: Qt.rgba(0.055, 0.058, 0.09, 0.84)
    border.width: 1
    border.color: Qt.rgba(0.65, 0.55, 0.98, rag.working ? 0.62 : 0.30)
    radius: 18
    clip: true

    Behavior on border.color { ColorAnimation { duration: 160 } }

    // волна по правой кромке (зеркало браузера), без частиц — легче
    Canvas {
        anchors.fill: parent
        renderStrategy: Canvas.Threaded
        antialiasing: true
        opacity: 0.85
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            var n = 44
            for (var i = 0; i < n; i++) {
                var t = i / (n - 1)
                var y = 14 + t * (height - 28)
                var off = Math.sin(ed.phase * 1.5 + t * 8.0) * 5.0
                          + Math.sin(ed.phase * 0.8 + t * 2.7) * 2.5
                var x = width - 11 - off
                var fade = Math.sin(t * Math.PI)
                var pulse = 1 + Math.sin(ed.phase * 3 + t * 6) * 0.3
                var a = (0.3 + 0.4 * Math.sin(ed.phase * 2 + t * 5)) * fade
                var col = (i % 5 === 0) ? cAcc2 : cAcc
                ctx.beginPath(); ctx.arc(x, y, 1.5 * pulse * 2.3, 0, Math.PI * 2)
                ctx.fillStyle = Qt.rgba(col.r, col.g, col.b, a * 0.09); ctx.fill()
                ctx.beginPath(); ctx.arc(x, y, 1.5 * pulse, 0, Math.PI * 2)
                ctx.fillStyle = Qt.rgba(col.r, col.g, col.b, a * 0.6); ctx.fill()
            }
        }
        Timer {
            interval: 16
            running: ed.visible
            repeat: true
            onTriggered: { ed.phase += rag.working ? 0.04 : 0.016; parent.requestPaint() }
        }
    }

    function loadFrom(sel) {
        loading = true
        ti.text = sel.title !== undefined ? sel.title : ""
        tg.text = (sel.tags !== undefined ? sel.tags : []).join(", ")
        ta.text = sel.text !== undefined ? sel.text : ""
        ed.fType = sel.type !== undefined ? sel.type : "docs"
        loading = false
        ed.dirty = false
        ed.confirmDelete = false
    }

    Connections {
        target: rag
        function onSelectionChanged() {
            if (rag.editorMode === "none") { ed.confirmDelete = false; return }
            if (rag.editorMode === "create") {
                ed.loadFrom({})
                ti.text = ""; tg.text = ""; ta.text = ""
                ed.fType = "docs"
            } else {
                ed.loadFrom(rag.selected)
            }
            ti.forceActiveFocus()
        }
    }

    Component.onCompleted: if (rag.hasSelection) ed.loadFrom(rag.selected)

    // акцент-полоска справа (зеркально), дышит при занятости
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 9
        width: 3
        radius: 2
        color: Qt.rgba(cAcc.r, cAcc.g, cAcc.b, 0.85)
        SequentialAnimation on opacity {
            running: rag.working
            loops: Animation.Infinite
            NumberAnimation { to: 0.4; duration: 420 }
            NumberAnimation { to: 1.0; duration: 420 }
        }
    }

    Column {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 20
        anchors.topMargin: 14
        anchors.bottomMargin: 12
        spacing: 8

        // ── шапка ──
        Row {
            width: parent.width
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: ed.creating ? "новый документ" : ("документ #" + rag.selected.docId)
                color: cFg; font.pixelSize: 13; font.bold: true; font.family: "Sans Serif"
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: !ed.creating
                text: "· " + (rag.selected.type || "")
                color: cDim; font.pixelSize: 11; font.family: "Sans Serif"
            }

            Item { width: 1; height: 1 }

            Btn {
                anchors.verticalCenter: parent.verticalCenter
                width: 66; height: 26; label: "закрыть"
                onClicked: rag.closeDoc()
            }
        }

        // ── название ─
        Column {
            width: parent.width
            spacing: 3
            Text { text: "название"; color: cDim; font.pixelSize: 10; font.family: "Sans Serif" }
            Rectangle {
                width: parent.width; height: 30; radius: 10
                color: Qt.rgba(0.02, 0.02, 0.04, 0.55)
                border.width: 1
                border.color: ti.activeFocus ? Qt.rgba(0.65, 0.55, 0.98, 0.9) : Qt.rgba(0.65, 0.55, 0.98, 0.18)
                Behavior on border.color { ColorAnimation { duration: 140 } }
                TextInput {
                    id: ti
                    anchors.fill: parent
                    anchors.leftMargin: 10; anchors.rightMargin: 8
                    verticalAlignment: TextInput.AlignVCenter
                    color: cFg; font.pixelSize: 13; font.family: "Sans Serif"
                    clip: true; selectByMouse: true
                    onTextChanged: if (!ed.loading) { ed.dirty = true }
                }
            }
        }

        // ── тип (чипы; в режиме правки read-only) ──
        Row {
            width: parent.width
            spacing: 6
            Text { anchors.verticalCenter: parent.verticalCenter; text: "тип"; color: cDim; font.pixelSize: 10; font.family: "Sans Serif" }
            Repeater {
                model: ["docs", "notes", "code", "chat_history", "web_clip", "fact"]
                delegate: Rectangle {
                    required property string modelData
                    width: lb.implicitWidth + 16; height: 24; radius: 12
                    color: ed.fType === modelData ? Qt.rgba(0.42, 0.34, 0.78, 0.9) : Qt.rgba(0.04, 0.04, 0.06, 0.6)
                    border.width: 1
                    border.color: ed.fType === modelData ? Qt.rgba(0.65, 0.55, 0.98, 0.9) : Qt.rgba(0.65, 0.55, 0.98, 0.14)
                    visible: ed.creating || modelData === ed.fType
                    Text { id: lb; anchors.centerIn: parent; text: modelData; color: cFg; font.pixelSize: 11; font.family: "Sans Serif" }
                    MouseArea {
                        anchors.fill: parent
                        enabled: ed.creating
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { ed.fType = modelData; ed.dirty = true }
                    }
                }
            }
        }

        // ── теги ──
        Column {
            width: parent.width
            spacing: 3
            Text { text: "теги (через запятую)"; color: cDim; font.pixelSize: 10; font.family: "Sans Serif" }
            Rectangle {
                width: parent.width; height: 30; radius: 10
                color: Qt.rgba(0.02, 0.02, 0.04, 0.55)
                border.width: 1
                border.color: tg.activeFocus ? Qt.rgba(0.65, 0.55, 0.98, 0.9) : Qt.rgba(0.65, 0.55, 0.98, 0.18)
                Behavior on border.color { ColorAnimation { duration: 140 } }
                TextInput {
                    id: tg
                    anchors.fill: parent
                    anchors.leftMargin: 10; anchors.rightMargin: 8
                    verticalAlignment: TextInput.AlignVCenter
                    color: Qt.rgba(0.49, 0.83, 0.99, 0.95); font.pixelSize: 12; font.family: "Sans Serif"
                    clip: true; selectByMouse: true
                    onTextChanged: if (!ed.loading) { ed.dirty = true }
                }
            }
        }

        // ── текст ─
        Row {
            width: parent.width
            spacing: 8
            Text { text: "текст"; color: cDim; font.pixelSize: 10; font.family: "Sans Serif" }
            Text { text: ta.text.length + " симв."; color: cDim2; font.pixelSize: 10; font.family: "Sans Serif" }
            Text {
                visible: !ed.creating && rag.selected.truncated === true
                text: "⚠ doc-вектор усечён"
                color: "#facc15"; font.pixelSize: 10; font.family: "Sans Serif"
            }
        }

        Rectangle {
            width: parent.width
            height: parent.height - 226
            radius: 12
            color: Qt.rgba(0.02, 0.02, 0.04, 0.55)
            border.width: 1
            border.color: ta.activeFocus ? Qt.rgba(0.65, 0.55, 0.98, 0.5) : Qt.rgba(0.65, 0.55, 0.98, 0.14)
            Behavior on border.color { ColorAnimation { duration: 140 } }
            clip: true

            Flickable {
                anchors.fill: parent
                anchors.margins: 10
                contentHeight: ta.paintedHeight
                boundsBehavior: Flickable.StopAtBounds
                clip: true

                TextEdit {
                    id: ta
                    width: parent.width
                    color: cFg
                    font.pixelSize: 13
                    font.family: "Monospace"
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    persistentSelection: true
                    onTextChanged: if (!ed.loading) { ed.dirty = true }
                }
            }

            Rectangle {
                visible: ta.paintedHeight > height - 20
                width: 3; radius: 1.5
                color: Qt.rgba(0.65, 0.55, 0.98, 0.4)
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                height: Math.max(24, (parent.height - 20) * ((parent.height - 20) / Math.max(1, ta.paintedHeight)))
                y: 10 + (parent.height - 20 - height) * (0)
            }
        }

        // ── низ ──
        Row {
            width: parent.width
            spacing: 6

            Btn {
                width: 104; height: 30
                label: rag.working ? "считаю…" : (ed.dirty ? "сохранить" : "сохранено")
                accent: true
                enabled: ed.dirty && !rag.working
                onClicked: rag.saveDoc(ti.text, ed.fType, tg.text, ta.text)
            }

            Btn {
                visible: !ed.creating
                width: ed.confirmDelete ? 128 : 80
                height: 30
                label: ed.confirmDelete ? "удалить точно?" : "удалить"
                danger: true
                enabled: !rag.working
                onClicked: {
                    if (!ed.confirmDelete) { ed.confirmDelete = true; return }
                    ed.confirmDelete = false
                    rag.deleteDoc(rag.selected.docId)
                }
            }

            Item { width: 1; height: 1 }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: !ed.creating
                text: "обновлён " + (rag.selected.updatedAt || "").slice(0, 19).replace("T", " ")
                color: cDim2; font.pixelSize: 10; font.family: "Sans Serif"
            }
        }

        Text {
            visible: ed.creating
            width: parent.width
            text: "создание посчитает эмбеддинги (~2 с: поднимется llama-server)"
            color: cDim2; font.pixelSize: 10; font.family: "Sans Serif"
            wrapMode: Text.Wrap
        }
    }

    component Btn: Rectangle {
        id: btn
        property string label: ""
        property bool accent: false
        property bool danger: false
        signal clicked

        radius: 10
        color: !enabled ? Qt.rgba(0.04, 0.04, 0.06, 0.6)
             : accent ? Qt.rgba(0.42, 0.34, 0.78, bm.containsMouse ? 1.0 : 0.85)
             : danger ? Qt.rgba(0.55, 0.18, 0.18, bm.containsMouse ? 0.95 : 0.7)
             : Qt.rgba(0.10, 0.10, 0.16, bm.containsMouse ? 0.95 : 0.7)
        border.width: 1
        border.color: accent ? Qt.rgba(0.65, 0.55, 0.98, 0.9)
                    : danger ? Qt.rgba(0.97, 0.44, 0.44, 0.8)
                    : Qt.rgba(0.65, 0.55, 0.98, 0.18)
        opacity: enabled ? 1.0 : 0.45
        Behavior on color { ColorAnimation { duration: 120 } }

        Text {
            anchors.centerIn: parent
            text: btn.label
            color: Qt.rgba(0.92, 0.93, 0.96, 0.92)
            font.pixelSize: 12
            font.family: "Sans Serif"
        }
        MouseArea {
            id: bm
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.enabled
            cursorShape: btn.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: btn.clicked()
        }
    }
}
