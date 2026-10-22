import QtQuick

Rectangle {
    id: b

    property color cAcc:  "#a78bfa"
    property color cAcc2: "#f472b6"
    property color cFg:   Qt.rgba(0.92, 0.93, 0.96, 0.92)
    property color cDim:  Qt.rgba(0.92, 0.93, 0.96, 0.45)
    property color cDim2: Qt.rgba(0.92, 0.93, 0.96, 0.28)

    property real phase: 0
    property var motes: []
    property string pending: ""

    color: Qt.rgba(0.055, 0.058, 0.09, 0.82)
    border.width: 1
    border.color: Qt.rgba(0.65, 0.55, 0.98, rag.working ? 0.62 : 0.30)
    radius: 18
    clip: true

    Behavior on border.color { ColorAnimation { duration: 160 } }

    // ── живой фон: волна по левой кромке + дрейфующие точки ─────────────
    Canvas {
        id: bg
        anchors.fill: parent
        renderStrategy: Canvas.Threaded
        antialiasing: true
        opacity: 0.9

        function dot(ctx, x, y, r, col, a) {
            if (a <= 0.004 || r <= 0) return
            ctx.beginPath()
            ctx.arc(x, y, r, 0, Math.PI * 2)
            ctx.fillStyle = Qt.rgba(col.r, col.g, col.b, a)
            ctx.fill()
        }

        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            // волна вдоль левой внутренней кромки
            var n = 46
            var amp = b.phase % 1 < 0.5 ? 5.0 : 5.0
            for (var i = 0; i < n; i++) {
                var t = i / (n - 1)
                var y = 14 + t * (height - 28)
                var off = Math.sin(b.phase * 1.7 + t * 9.0) * 5.0
                          + Math.sin(b.phase * 0.9 + t * 3.1) * 2.5
                var x = 11 + off
                var fade = Math.sin(t * Math.PI)            // затухание к краям
                var pulse = 1 + Math.sin(b.phase * 3 + t * 6) * 0.35
                var a = (0.35 + 0.45 * Math.sin(b.phase * 2 + t * 5)) * fade
                var col = (i % 4 === 0) ? cAcc2 : cAcc
                dot(ctx, x, y, 1.6 * pulse * 2.4, col, a * 0.10)
                dot(ctx, x, y, 1.6 * pulse, col, a * 0.7)
            }

            // дрейфующие точки по площади
            for (var j = 0; j < b.motes.length; j++) {
                var p = b.motes[j]
                var tw = 0.4 + 0.6 * Math.sin(b.phase * 3 + p.ph)
                dot(ctx, p.x, p.y, p.sz * 2.2, cAcc, tw * 0.07)
                dot(ctx, p.x, p.y, p.sz, (j % 3 === 0) ? cAcc2 : cAcc, tw * 0.5)
            }
        }
    }

    Timer {
        interval: 16
        running: b.visible
        repeat: true
        onTriggered: {
            b.phase += rag.working ? 0.045 : 0.018
            var w = b.width, h = b.height
            for (var i = 0; i < b.motes.length; i++) {
                var p = b.motes[i]
                p.x += p.vx; p.y += p.vy
                if (p.x < 8 || p.x > w - 8) p.vx = -p.vx
                if (p.y < 8 || p.y > h - 8) p.vy = -p.vy
                var sp = Math.sqrt(p.vx * p.vx + p.vy * p.vy)
                if (sp > 0.9) { p.vx = p.vx / sp * 0.9; p.vy = p.vy / sp * 0.9 }
            }
            bg.requestPaint()
        }
    }

    function initMotes() {
        motes = []
        for (var i = 0; i < 12; i++) {
            motes.push({
                x: Math.random() * width,
                y: Math.random() * height,
                vx: (Math.random() - 0.5) * 0.7,
                vy: (Math.random() - 0.5) * 0.7,
                sz: 0.9 + Math.random() * 1.4,
                ph: Math.random() * Math.PI * 2
            })
        }
    }

    Component.onCompleted: initMotes()
    onWidthChanged: initMotes()
    onHeightChanged: initMotes()

    // ── акцент-полоска слева, как у карточек чата ───────────────────────
    Rectangle {
        anchors.left: parent.left
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
        anchors.leftMargin: 20
        anchors.rightMargin: 12
        anchors.topMargin: 14
        anchors.bottomMargin: 12
        spacing: 8

        // ── строка поиска ──
        Row {
            width: parent.width
            spacing: 6

            Rectangle {
                width: parent.width - 138
                height: 32
                radius: 10
                color: Qt.rgba(0.02, 0.02, 0.04, 0.55)
                border.width: 1
                border.color: qs.activeFocus ? Qt.rgba(0.65, 0.55, 0.98, 0.9) : Qt.rgba(0.65, 0.55, 0.98, 0.22)
                Behavior on border.color { ColorAnimation { duration: 140 } }

                TextInput {
                    id: qs
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 8
                    verticalAlignment: TextInput.AlignVCenter
                    color: cFg
                    font.pixelSize: 13
                    font.family: "Sans Serif"
                    clip: true
                    selectByMouse: true
                    text: b.pending
                    onTextChanged: b.pending = text
                    onAccepted: rag.search(text)
                    Keys.onEscapePressed: { b.pending = ""; rag.clearSearch() }
                }

                Text {
                    visible: qs.text.length === 0
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    text: "поиск по базе…"
                    color: Qt.rgba(0.29, 0.29, 0.34, 1.0)
                    font.pixelSize: 13
                    font.family: "Sans Serif"
                }
            }

            Btn {
                width: 60; height: 32; label: "Найти"
                accent: true
                enabled: !rag.working && qs.text.trim().length > 0
                onClicked: rag.search(qs.text)
            }
            Btn {
                width: 72; height: 32; label: "Сброс"
                enabled: rag.mode === "search"
                onClicked: { b.pending = ""; rag.clearSearch() }
            }
        }

        // ── indeterminate-полоса под поиском, пока считает ──────────────
        Item {
            width: parent.width
            height: rag.working ? 2 : 0
            clip: true
            visible: height > 0
            Behavior on height { NumberAnimation { duration: 120 } }

            Rectangle {
                width: 64
                height: 2
                radius: 1
                color: Qt.rgba(cAcc.r, cAcc.g, cAcc.b, 0.9)
                x: -64
                NumberAnimation on x {
                    running: rag.working && b.visible
                    from: -64; to: b.width
                    duration: 950
                    loops: Animation.Infinite
                    easing.type: Easing.Linear
                }
            }
        }

        // ── заголовок секции + «печатает» ──
        Row {
            width: parent.width
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: rag.mode === "search"
                      ? ("релевантность · " + rag.query)
                      : ("документы · " + rag.totalDocs)
                color: cDim
                font.pixelSize: 11
                font.family: "Sans Serif"
                elide: Text.ElideRight
                width: parent.width - 60
            }

            // три точки, каскадный пульс при занятости
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                visible: rag.working
                Repeater {
                    model: 3
                    Rectangle {
                        width: 5; height: 5; radius: 2.5
                        color: cAcc
                        SequentialAnimation on opacity {
                            running: rag.working && b.visible
                            loops: Animation.Infinite
                            PauseAnimation { duration: index * 180 }
                            NumberAnimation { to: 0.2; duration: 300; easing.type: Easing.OutQuad }
                            NumberAnimation { to: 1.0; duration: 300; easing.type: Easing.InQuad }
                        }
                    }
                }
            }
        }

        // ── список ──
        Item {
            width: parent.width
            height: parent.height - 132
            clip: true

            ListView {
                id: lv
                anchors.fill: parent
                anchors.rightMargin: 6
                model: rag.items
                spacing: 6
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                visible: count > 0

                Text {
                    anchors.centerIn: parent
                    visible: lv.count === 0 && !rag.busy
                    text: rag.mode === "search" ? "ничего не найдено" : "база пуста"
                    color: cDim2
                    font.pixelSize: 13
                    font.family: "Sans Serif"
                }

                delegate: Rectangle {
                    id: card
                    required property var modelData
                    required property int index

                    property real appearKey: modelData.docId
                    property bool sel: rag.selected.docId !== undefined
                                       && rag.selected.docId === modelData.docId

                    width: lv.width
                    height: col.implicitHeight + 18
                    radius: 12
                    color: ma.containsMouse ? Qt.rgba(0.10, 0.10, 0.16, 0.9)
                                            : Qt.rgba(0.045, 0.048, 0.075, 0.72)
                    border.width: 1
                    border.color: card.sel ? Qt.rgba(0.65, 0.55, 0.98, 0.85)
                                           : Qt.rgba(0.65, 0.55, 0.98, 0.10)
                    opacity: 0

                    Behavior on color { ColorAnimation { duration: 120 } }
                    Behavior on border.color { ColorAnimation { duration: 140 } }

                    SequentialAnimation {
                        id: appear
                        PauseAnimation { duration: Math.min(220, card.index * 24) }
                        NumberAnimation { target: card; property: "opacity"; to: 1.0;
                                          duration: 200; easing.type: Easing.OutCubic }
                    }

                    onAppearKeyChanged: { card.opacity = 0; appear.restart() }
                    Component.onCompleted: { card.opacity = 0; appear.restart() }

                    Column {
                        id: col
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 10
                        spacing: 4

                        Row {
                            width: parent.width
                            spacing: 8

                            Text {
                                width: parent.width - (modelData.score >= 0 ? 74 : 0)
                                text: modelData.title
                                color: cFg
                                font.pixelSize: 13
                                font.bold: true
                                font.family: "Sans Serif"
                                elide: Text.ElideRight
                            }
                            Text {
                                visible: modelData.score >= 0
                                width: 66
                                horizontalAlignment: Text.AlignRight
                                text: modelData.score.toFixed(3)
                                color: cAcc
                                font.pixelSize: 12
                                font.family: "Monospace"
                            }
                        }

                        Row {
                            spacing: 8
                            Text { text: modelData.type; color: cDim; font.pixelSize: 10; font.family: "Sans Serif" }
                            Text {
                                visible: modelData.chunks > 0
                                text: "чанков " + modelData.chunks
                                color: cDim2; font.pixelSize: 10; font.family: "Sans Serif"
                            }
                            Repeater {
                                model: modelData.tags
                                Text { text: "#" + modelData; color: Qt.rgba(0.49, 0.83, 0.99, 0.8); font.pixelSize: 10; font.family: "Sans Serif" }
                            }
                        }

                        Text {
                            visible: modelData.snippet.length > 0
                            width: parent.width
                            text: modelData.snippet
                            color: cDim
                            font.pixelSize: 11
                            font.family: "Sans Serif"
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        id: ma
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: rag.openDoc(modelData.docId)
                    }
                }
            }

            // самодельный скролл-индикатор (без QtQuick.Controls)
            Rectangle {
                visible: lv.contentHeight > lv.height
                width: 3
                radius: 1.5
                color: Qt.rgba(0.65, 0.55, 0.98, 0.45)
                anchors.right: parent.right
                height: Math.max(24, lv.height * (lv.height / Math.max(1, lv.contentHeight)))
                y: (lv.height - height) * (lv.contentY / Math.max(1, lv.contentHeight - lv.height))
            }
        }

        // ── пагинация ──
        Row {
            width: parent.width
            spacing: 6
            visible: rag.mode === "browse"

            Btn {
                width: 74; height: 28; label: "← назад"
                enabled: rag.page > 0 && !rag.busy
                onClicked: rag.prevPage()
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: (rag.page + 1) + " / " + rag.pageCount
                color: cFg; font.pixelSize: 12; font.family: "Sans Serif"
            }
            Btn {
                width: 74; height: 28; label: "вперёд →"
                enabled: rag.page + 1 < rag.pageCount && !rag.busy
                onClicked: rag.nextPage()
            }

            Item { width: 1; height: 1 }

            Btn {
                anchors.right: parent.right
                width: 86; height: 28; label: "+ новый"
                accent: true
                onClicked: rag.newDoc()
            }
        }

        // ── статус ──
        Column {
            width: parent.width
            spacing: 2

            Text {
                visible: rag.error.length > 0
                width: parent.width
                text: rag.error
                color: "#f87171"
                font.pixelSize: 11
                font.family: "Sans Serif"
                wrapMode: Text.Wrap
            }
            Text {
                visible: !rag.ready
                width: parent.width
                text: "RAG не готов: проверь WATCHER_LLAMA_SERVER и путь к GGUF"
                color: "#facc15"
                font.pixelSize: 11
                font.family: "Sans Serif"
                wrapMode: Text.Wrap
            }
            Text {
                visible: rag.ready
                width: parent.width
                text: "доков " + rag.stats.documents + " · чанков " + rag.stats.chunks +
                      " · dim " + rag.stats.dimensions + " · " + rag.stats.model +
                      " · pooling " + rag.stats.pooling
                color: cDim2
                font.pixelSize: 10
                font.family: "Sans Serif"
                wrapMode: Text.Wrap
            }
        }
    }

    // общая кнопка-прямоугольник в стиле карточек (inline component, Qt6)
    component Btn: Rectangle {
        id: btn
        property string label: ""
        property bool accent: false
        signal clicked

        radius: 10
        color: !enabled ? Qt.rgba(0.04, 0.04, 0.06, 0.6)
             : accent ? Qt.rgba(0.42, 0.34, 0.78, bm.containsMouse ? 1.0 : 0.85)
             : Qt.rgba(0.10, 0.10, 0.16, bm.containsMouse ? 0.95 : 0.7)
        border.width: 1
        border.color: accent ? Qt.rgba(0.65, 0.55, 0.98, 0.9) : Qt.rgba(0.65, 0.55, 0.98, 0.18)
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
