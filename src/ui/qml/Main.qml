import QtQuick
import QtQuick.Window

Window {
    id: overlayWindow

    width: Screen.width
    height: Screen.height
    x: 0
    y: 0

    visible: false
    color: "transparent"

    property bool active: false
    property bool fading: false

    readonly property real chatWidth: Math.min(Screen.width * 0.58, 920)

    // ═══════════════════════════════════════════
    // ФОНОВАЯ АНИМАЦИЯ
    // ═══════════════════════════════════════════
    property int sinePointCount: 120
    property int flyingPointCount: 40

    property real idleSineAmplitude: 6
    property real thinkingSineAmplitude: 17

    property real idleBaseSpeed: 0.008
    property real thinkingBaseSpeed: 0.026

    property real edgeOffset: 14
    property real hueSpeed: 0.0006

    property real sineAlpha: 0.65
    property real flyingAlpha: 0.45
    property real keyAlpha: 0.55


    property real currentSineAmplitude: agentThinking ? thinkingSineAmplitude : idleSineAmplitude
    property real currentBaseSpeed: agentThinking ? thinkingBaseSpeed : idleBaseSpeed

    Behavior on currentSineAmplitude {
        NumberAnimation {
            duration: 160
            easing.type: Easing.OutCubic
        }
    }

    Behavior on currentBaseSpeed {
        NumberAnimation {
            duration: 160
            easing.type: Easing.OutCubic
        }
    }

    // ═══════════════════════════════════════════
    // КЛАВИАТУРА ДЛЯ ФОНТАНЧИКОВ
    // ═══════════════════════════════════════════
    property real keyboardLeftRatio: 0.088
    property real keyboardWidthRatio: 0.70
    property real homeRowShift: 0.33
    property real bottomRowShift: 0.67
    property real keyboardSpawnY: height - 6

    // ═══════════════════════════════════════════
    // ЦВЕТА
    // ═══════════════════════════════════════════
    property color dotColor: "#60a5fa"
    property color flyingColor: "#f472b6"
    property real hueShift: 0

    property color userAccent: "#93c5fd"
    property color reasoningAccent: "#9ca3af"
    property color assistantAccent: "#4ade80"
    property color toolAccent: "#f87171"

    // ═══════════════════════════════════════════
    // СОСТОЯНИЕ
    // ═══════════════════════════════════════════
    property var sinePoints: []
    property var flyingPoints: []
    property var keyPoints: []
    property var chatParticles: []

    property real time: 0

    property bool stickToBottom: true
    property bool agentThinking: chatEngine.thinking

    property bool copiedToastVisible: false

    function showCopiedToast() {
        copiedToastVisible = true
        copiedToastTimer.restart()
    }

    function clearOtherCardSelections(exceptCard) {
        for (var i = 0; i < chatColumn.children.length; i++) {
            var cardItem = chatColumn.children[i]

            if (!cardItem) {
                continue
            }

            if (exceptCard && cardItem === exceptCard) {
                continue
            }

            if (typeof cardItem.clearSelection === "function") {
                cardItem.clearSelection()
            }
        }
    }

    function copyCurrentSelection() {
        if (!active) {
            return
        }

        if (inputField.selectedText.length > 0) {
            chatEngine.copyToClipboard(inputField.selectedText)
            showCopiedToast()
            return
        }

        for (var i = 0; i < chatColumn.children.length; i++) {
            var item = chatColumn.children[i]

            if (!item) {
                continue
            }

            if (item.visible === false) {
                continue
            }

            if (typeof item.getSelectionText !== "function") {
                continue
            }

            var selected = item.getSelectionText()

            if (selected && selected.length > 0) {
                chatEngine.copyToClipboard(selected)
                showCopiedToast()
                return
            }
        }
    }

    function forceCloseChatUi() {
        fading = false
        active = false
        visible = false

        overlayContent.opacity = 0
        inputField.text = ""

        keyPoints = []
        chatParticles = []
    }

    // ═══════════════════════════════════════════
    // МАППИНГ СКАН-КОДОВ
    // Только буквы и пробел.
    // ═══════════════════════════════════════════
    property var scanCodeMap: ({
        // Верхний буквенный ряд: Q W E R T Y U I O P
        "16": {row: "top", idx: 0},
        "17": {row: "top", idx: 1},
        "18": {row: "top", idx: 2},
        "19": {row: "top", idx: 3},
        "20": {row: "top", idx: 4},
        "21": {row: "top", idx: 5},
        "22": {row: "top", idx: 6},
        "23": {row: "top", idx: 7},
        "24": {row: "top", idx: 8},
        "25": {row: "top", idx: 9},

        // Средний буквенный ряд: A S D F G H J K L
        "30": {row: "home", idx: 0},
        "31": {row: "home", idx: 1},
        "32": {row: "home", idx: 2},
        "33": {row: "home", idx: 3},
        "34": {row: "home", idx: 4},
        "35": {row: "home", idx: 5},
        "36": {row: "home", idx: 6},
        "37": {row: "home", idx: 7},
        "38": {row: "home", idx: 8},

        // Нижний буквенный ряд: Z X C V B N M
        "44": {row: "bottom", idx: 0},
        "45": {row: "bottom", idx: 1},
        "46": {row: "bottom", idx: 2},
        "47": {row: "bottom", idx: 3},
        "48": {row: "bottom", idx: 4},
        "49": {row: "bottom", idx: 5},
        "50": {row: "bottom", idx: 6},

        // Пробел
        "57": {row: "space", idx: 0}
    })

    Timer {
        id: copiedToastTimer

        interval: 900
        repeat: false

        onTriggered: overlayWindow.copiedToastVisible = false
    }

    Shortcut {
        sequences: [StandardKey.Copy]
        context: Qt.ApplicationShortcut

        onActivated: overlayWindow.copyCurrentSelection()
    }

    Shortcut {
        sequences: ["Esc"]
        context: Qt.ApplicationShortcut

        onActivated: overlayWindow.closeChatUi()
    }

    // ═══════════════════════════════════════════
    // ИНИЦИАЛИЗАЦИЯ ФОНА
    // ═══════════════════════════════════════════
    Component.onCompleted: initPoints()
    onWidthChanged: initPoints()
    onHeightChanged: initPoints()

    function initPoints() {
        sinePoints = []
        flyingPoints = []

        var w = width
        var h = height
        var perimeter = 2 * (w + h)

        for (var i = 0; i < sinePointCount; i++) {
            var pos = getPerimeterPoint((i / sinePointCount) * perimeter, w, h)

            sinePoints.push({
                baseX: pos.x + pos.nx * edgeOffset,
                baseY: pos.y + pos.ny * edgeOffset,
                nx: pos.nx,
                ny: pos.ny,
                phase: Math.random() * Math.PI * 2,
                speed: 0.5 + Math.random() * 1.5,
                size: 2 + Math.random() * 3
            })
        }

        for (var j = 0; j < flyingPointCount; j++) {
            flyingPoints.push({
                x: Math.random() * w,
                y: Math.random() * h,
                vx: (Math.random() - 0.5) * 2,
                vy: (Math.random() - 0.5) * 2,
                size: 1.5 + Math.random() * 2.5,
                phase: Math.random() * Math.PI * 2
            })
        }
    }

    function getPerimeterPoint(dist, w, h) {
        if (dist < w) {
            return { x: dist, y: 0, nx: 0, ny: 1 }
        }
        if (dist < w + h) {
            return { x: w, y: dist - w, nx: -1, ny: 0 }
        }
        if (dist < 2 * w + h) {
            return { x: w - (dist - w - h), y: h, nx: 0, ny: -1 }
        }
        return { x: 0, y: h - (dist - 2 * w - h), nx: 1, ny: 0 }
    }

    // ═══════════════════════════════════════════
    // НОРМАЛИЗАЦИЯ ТЕКСТА
    // Убирает лишние \n, \r, пустые строки.
    // ═══════════════════════════════════════════
    function escapeHtml(value) {
        return String(value)
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;");
    }

    function inlineMarkdown(value) {
        var s = escapeHtml(value);

        // Код в обратных кавычках
        s = s.replace(/`([^`]+)`/g, "<font face=\"monospace\" color=\"#93c5fd\">$1</font>");

        // Жирный
        s = s.replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>");
        s = s.replace(/__([^_]+)__/g, "<b>$1</b>");

        // Курсив
        s = s.replace(/\*([^*]+)\*/g, "<i>$1</i>");
        s = s.replace(/(^|[^a-zA-Z0-9])_([^_]+)_/g, "$1<i>$2</i>");

        // Зачёркнутый
        s = s.replace(/~~([^~]+)~~/g, "<strike>$1</strike>");

        // Ссылки
        s = s.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, "<a href=\"$2\">$1</a>");

        return s;
    }

    function renderMarkdown(raw) {
        var text = normalizeChatText(raw);

        if (!text) {
            return "";
        }

        var lines = text.split("\n");
        var out = [];

        var inCode = false;
        var codeLines = [];

        var listTag = "";
        var paragraph = [];

        function flushParagraph() {
            if (paragraph.length > 0) {
                var parts = [];

                for (var i = 0; i < paragraph.length; i++) {
                    parts.push(inlineMarkdown(paragraph[i]));
                }

                out.push("<p>" + parts.join("<br>") + "</p>");
                paragraph = [];
            }
        }

        function closeList() {
            if (listTag !== "") {
                out.push("</" + listTag + ">");
                listTag = "";
            }
        }

        function openList(tag) {
            if (listTag !== tag) {
                closeList();
                out.push("<" + tag + ">");
                listTag = tag;
            }
        }

        for (var i = 0; i < lines.length; i++) {
            var line = lines[i];
            var trimmed = line.trim();

            // Блоки кода ```
            if (trimmed.indexOf("```") === 0) {
                if (!inCode) {
                    flushParagraph();
                    closeList();
                    inCode = true;
                    codeLines = [];
                } else {
                    var codeHtml = escapeHtml(codeLines.join("\n")).replace(/\n/g, "<br>");
                    out.push(
                        "<pre><font face=\"monospace\" color=\"#d4d4dc\">" +
                        codeHtml +
                        "</font></pre>"
                    );
                    inCode = false;
                }

                continue;
            }

            if (inCode) {
                codeLines.push(line);
                continue;
            }

            // Пустая строка
            if (trimmed === "") {
                flushParagraph();
                closeList();
                continue;
            }

            // Заголовки #, ##, ###
            var heading = /^(#{1,6})\s+(.*)$/.exec(trimmed);

            if (heading) {
                flushParagraph();
                closeList();

                var level = heading[1].length;

                out.push(
                    "<h" + level + ">" +
                    inlineMarkdown(heading[2]) +
                    "</h" + level + ">"
                );

                continue;
            }

            // Горизонтальная линия
            if (/^(-{3,}|\*{3,}|_{3,})$/.test(trimmed)) {
                flushParagraph();
                closeList();
                out.push("<hr>");
                continue;
            }

            // Цитата
            if (trimmed.charAt(0) === ">") {
                flushParagraph();
                closeList();

                var quoteText = trimmed.replace(/^>\s?/, "");

                out.push(
                    "<blockquote>" +
                    inlineMarkdown(quoteText) +
                    "</blockquote>"
                );

                continue;
            }

            // Маркированный список
            var ul = /^[-*+]\s+(.*)$/.exec(trimmed);

            if (ul) {
                flushParagraph();
                openList("ul");
                out.push("<li>" + inlineMarkdown(ul[1]) + "</li>");
                continue;
            }

            // Нумерованный список
            var ol = /^\d+[.)]\s+(.*)$/.exec(trimmed);

            if (ol) {
                flushParagraph();
                openList("ol");
                out.push("<li>" + inlineMarkdown(ol[1]) + "</li>");
                continue;
            }

            // Обычный абзац
            paragraph.push(trimmed);
        }

        // Если код не закрылся
        if (inCode) {
            var codeHtml = escapeHtml(codeLines.join("\n")).replace(/\n/g, "<br>");

            out.push(
                "<pre><font face=\"monospace\" color=\"#d4d4dc\">" +
                codeHtml +
                "</font></pre>"
            );
        }

        flushParagraph();
        closeList();

        return out.join("");
    }

    function normalizeChatText(value) {
        if (!value) {
            return ""
        }

        var s = String(value)
        var out = ""
        var i = 0

        while (i < s.length) {
            var c = s.charAt(i)

            if (c === "\\" && i + 1 < s.length) {
                var n = s.charAt(i + 1)

                if (n === "n" || n === "r") {
                    out += "\n"
                    i += 2
                    continue
                }

                if (n === "t") {
                    out += " "
                    i += 2
                    continue
                }
            }

            if (c === "\r") {
                out += "\n"
                i += 1
                continue
            }

            if (c === "\n") {
                if (out.length === 0 || out.charAt(out.length - 1) !== "\n") {
                    out += "\n"
                }

                i += 1
                continue
            }

            out += c
            i += 1
        }

        while (out.length > 0) {
            var first = out.charAt(0)

            if (first === "\n" || first === " " || first === "\t") {
                out = out.substring(1)
            } else {
                break
            }
        }

        while (out.length > 0) {
            var last = out.charAt(out.length - 1)

            if (last === "\n" || last === " " || last === "\t") {
                out = out.substring(0, out.length - 1)
            } else {
                break
            }
        }

        return out
    }

    // ═══════════════════════════════════════════
    // ФОНТАНЧИКИ ОТ КЛАВИШ
    // ═══════════════════════════════════════════
    function keyX(index, rowShift) {
        var kbLeft = width * keyboardLeftRatio
        var kbWidth = width * keyboardWidthRatio
        var keyWidth = kbWidth / 12
        return kbLeft + (index + 0.5 + rowShift) * keyWidth
    }

    function createKeyBurstByScanCode(scanCode) {
        if (!active) {
            return
        }

        var info = scanCodeMap[String(scanCode)]
        if (!info) {
            return
        }

        var x = -1

        switch (info.row) {
            case "top":
                x = keyX(info.idx, 0)
                break

            case "home":
                x = keyX(info.idx, homeRowShift)
                break

            case "bottom":
                x = keyX(info.idx, bottomRowShift)
                break

            case "space":
                x = width * (keyboardLeftRatio + keyboardWidthRatio * 0.5)
                break
        }

        if (x < 0) {
            return
        }

        var count = 4 + Math.floor(Math.random() * 4)

        for (var i = 0; i < count; i++) {
            keyPoints.push({
                x: x + (Math.random() - 0.5) * 10,
                y: keyboardSpawnY,
                vx: (Math.random() - 0.5) * 1.0,
                vy: -(1.5 + Math.random() * 2.2),
                size: 1.8 + Math.random() * 1.5,
                life: 1.0,
                decay: 0.010 + Math.random() * 0.006
            })
        }

        if (keyPoints.length > 150) {
            keyPoints.splice(0, keyPoints.length - 150)
        }
    }

    // ═══════════════════════════════════════════
    // СТИЛИ РОЛЕЙ ИЗ kinds.json
    // ═══════════════════════════════════════════

    function defaultStyle(role) {
        if (role === "system") {
            return {
                accent: "#60a5fa",
                title: "System",
                showTitle: true,
                hidden: true,
                markdown: true,
                selectable: true,
                fontPixelSize: 14
            }
        }

        if (role === "user") {
            return {
                accent: userAccent,
                title: "",
                showTitle: false,
                hidden: false,
                markdown: true,
                selectable: true,
                fontPixelSize: 14
            }
        }

        if (role === "reasoning") {
            return {
                accent: reasoningAccent,
                title: "",
                showTitle: false,
                hidden: false,
                markdown: true,
                selectable: true,
                fontPixelSize: 14
            }
        }

        if (role === "assistant") {
            return {
                accent: assistantAccent,
                title: "",
                showTitle: false,
                hidden: false,
                markdown: true,
                selectable: true,
                fontPixelSize: 14
            }
        }

        if (role === "tool") {
            return {
                accent: toolAccent,
                title: "Tool",
                showTitle: true,
                hidden: false,
                markdown: true,
                selectable: true,
                fontPixelSize: 14
            }
        }

        if (role === "question") {
            return {
                accent: "#facc15",
                title: "Уточнение",
                showTitle: true,
                hidden: false,
                markdown: true,
                selectable: true,
                fontPixelSize: 14
            }
        }

        return {
            accent: assistantAccent,
            title: "",
            showTitle: false,
            hidden: false,
            markdown: true,
            selectable: true,
            fontPixelSize: 14
        }
    }

    function styleForRole(role) {
        var def = defaultStyle(role)

        if (typeof kindConfig === "undefined" || !kindConfig) {
            return def
        }

        var cfg = kindConfig[role]

        if (!cfg) {
            return def
        }

        return {
            accent: cfg.accent !== undefined ? cfg.accent : def.accent,
            title: cfg.title !== undefined ? cfg.title : def.title,
            showTitle: cfg.showTitle !== undefined ? cfg.showTitle : def.showTitle,
            hidden: cfg.hidden !== undefined ? cfg.hidden : def.hidden,
            markdown: cfg.markdown !== undefined ? cfg.markdown : def.markdown,
            selectable: cfg.selectable !== undefined ? cfg.selectable : def.selectable,
            fontPixelSize: cfg.fontPixelSize !== undefined ? Number(cfg.fontPixelSize) : def.fontPixelSize
        }
    }

    function updateChatScroll(force) {
        if (!chatScroller) {
            return
        }

        if (force || stickToBottom) {
            Qt.callLater(function () {
                chatScroller.contentY = Math.max(
                    0,
                    chatScroller.contentHeight - chatScroller.height
                )
            })
        }
    }

    // ═══════════════════════════════════════════
    // ОТКРЫТИЕ / ЗАКРЫТИЕ ЧАТ-UI
    // ═══════════════════════════════════════════
    function openChatUi() {
        if (fading) {
            return
        }

        fading = true
        active = true
        visible = true

        overlayContent.opacity = 0

        stickToBottom = true

        showAnimation.start()
        raise()
        requestActivate()

        Qt.callLater(function () {
            updateChatScroll(true)
            inputField.forceActiveFocus()
        })
    }

    function closeChatUi() {
        if (fading) {
            return
        }

        fading = true

        // Скрытие UI не отменяет генерацию.
        // FLM-соединение и ChatEngine продолжают работать в фоне.
        hideAnimation.start()
    }

    function toggleChatUi() {
        if (active) {
            closeChatUi()
        } else {
            openChatUi()
        }
    }

    // ═══════════════════════════════════════════
    // АНИМАЦИЯ ФОНА И ЧАСТИЦ
    // ═══════════════════════════════════════════
    Timer {
        interval: 16
        running: overlayWindow.active
        repeat: true

        onTriggered: {
            overlayWindow.time += overlayWindow.currentBaseSpeed
            overlayWindow.hueShift = (overlayWindow.hueShift + overlayWindow.hueSpeed) % 1.0

            overlayWindow.dotColor = Qt.hsla((0.6 + overlayWindow.hueShift) % 1.0, 0.75, 0.65, 1.0)
            overlayWindow.flyingColor = Qt.hsla((0.9 + overlayWindow.hueShift) % 1.0, 0.8, 0.65, 1.0)

            overlayWindow.updateFlyingPoints()
            overlayWindow.updateTransient(overlayWindow.keyPoints, 0.015)
            overlayWindow.updateTransient(overlayWindow.chatParticles, 0.004)

            canvas.requestPaint()
        }
    }

    function updateTransient(arr, gravity) {
        for (var i = arr.length - 1; i >= 0; i--) {
            var p = arr[i]

            p.x += p.vx
            p.y += p.vy
            p.vy += gravity
            p.life -= p.decay

            if (p.life <= 0 || p.y < -40) {
                arr.splice(i, 1)
            }
        }
    }

    function updateFlyingPoints() {
        var w = width
        var h = height

        for (var i = 0; i < flyingPoints.length; i++) {
            var p = flyingPoints[i]

            p.x += p.vx + Math.sin(time * 2 + p.phase) * 0.3
            p.y += p.vy + Math.cos(time * 2 + p.phase) * 0.3

            if (p.x < -20 || p.x > w + 20) {
                p.vx = -p.vx
            }

            if (p.y < -20 || p.y > h + 20) {
                p.vy = -p.vy
            }

            var speed = Math.sqrt(p.vx * p.vx + p.vy * p.vy)
            if (speed > 2.5) {
                p.vx = (p.vx / speed) * 2.5
                p.vy = (p.vy / speed) * 2.5
            }

            if (Math.random() < 0.003) {
                p.vx = (Math.random() - 0.5) * 2
                p.vy = (Math.random() - 0.5) * 2
            }
        }
    }

    Component {
        id: cardComponent

        Rectangle {
            id: card

            property string role: model.role
            property string content: model.content
            property var style: overlayWindow.styleForRole(role)

            property color accentColor: {
                if (!style || style.accent === undefined || style.accent === null) {
                    return overlayWindow.assistantAccent
                }

                return style.accent
            }

            function clearSelection() {
                cardText.select(0, 0)
            }

            function getSelectionText() {
                return cardText.selectedText
            }

            onActiveFocusChanged: {
                if (activeFocus) {
                    overlayWindow.clearOtherCardSelections(card)
                }
            }

            visible: style && !style.hidden

            width: overlayWindow.chatWidth
            height: visible ? Math.max(54, cardText.height + 26) : 0

            radius: 18

            color: Qt.rgba(
                0.055,
                0.058,
                0.09,
                0.78
            )

            border.width: 1
            border.color: Qt.rgba(
                accentColor.r,
                accentColor.g,
                accentColor.b,
                0.55
            )

            opacity: 1
            scale: 1

            Rectangle {
                id: accentBar

                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: 9

                width: 3
                radius: 2

                color: Qt.rgba(
                    accentColor.r,
                    accentColor.g,
                    accentColor.b,
                    0.85
                )
            }

            TextEdit {
                id: cardText

                anchors.left: accentBar.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.top: parent.top
                anchors.topMargin: 13

                height: contentHeight

                readOnly: true
                selectByMouse: style.selectable
                persistentSelection: style.selectable
                activeFocusOnPress: style.selectable
                cursorVisible: false

                textFormat: style.markdown
                    ? TextEdit.RichText
                    : TextEdit.PlainText

                text: {
                    var body = content

                    if (style.showTitle && style.title.length > 0) {
                        body = style.title + "\n" + body
                    }

                    return style.markdown
                        ? overlayWindow.renderMarkdown(body)
                        : body
                }

                color: Qt.rgba(
                    0.92,
                    0.93,
                    0.96,
                    0.92
                )

                wrapMode: TextEdit.Wrap
                font.pixelSize: style.fontPixelSize

                selectionColor: Qt.rgba(
                    accentColor.r,
                    accentColor.g,
                    accentColor.b,
                    0.35
                )
            }
        }
    }

    // ═══════════════════════════════════════════
    // ОСНОВНОЙ КОНТЕНТ
    // ═══════════════════════════════════════════
    Rectangle {
        id: overlayContent

        anchors.fill: parent
        color: "transparent"
        opacity: 0

        Canvas {
            id: canvas

            anchors.fill: parent
            renderStrategy: Canvas.Threaded
            antialiasing: true

            function dot(ctx, x, y, size, color, alpha) {
                if (alpha <= 0.005) {
                    return
                }

                ctx.beginPath()
                ctx.arc(x, y, size, 0, Math.PI * 2)
                ctx.fillStyle = Qt.rgba(color.r, color.g, color.b, alpha)
                ctx.fill()
            }

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)

                // Синусоида по краям
                for (var i = 0; i < overlayWindow.sinePoints.length; i++) {
                    var sp = overlayWindow.sinePoints[i]

                    var off = Math.sin(overlayWindow.time * sp.speed + sp.phase) * overlayWindow.currentSineAmplitude
                    var x = sp.baseX + sp.nx * off
                    var y = sp.baseY + sp.ny * off

                    var pulse = 1 + Math.sin(overlayWindow.time * 3 + sp.phase) * 0.3
                    var size = sp.size * pulse
                    var a = (0.5 + 0.5 * Math.sin(overlayWindow.time * 2 + sp.phase)) * overlayWindow.sineAlpha

                    dot(ctx, x, y, size * 2.5, overlayWindow.dotColor, a * 0.12)
                    dot(ctx, x, y, size, overlayWindow.dotColor, a * 0.85)
                }

                // Летающие точки
                for (var j = 0; j < overlayWindow.flyingPoints.length; j++) {
                    var fp = overlayWindow.flyingPoints[j]
                    var fa = (0.5 + 0.5 * Math.sin(overlayWindow.time * 4 + fp.phase)) * overlayWindow.flyingAlpha

                    dot(ctx, fp.x, fp.y, fp.size * 2, overlayWindow.flyingColor, fa * 0.15)
                    dot(ctx, fp.x, fp.y, fp.size, overlayWindow.flyingColor, fa)
                }

                // Фонтанчики от клавиш
                for (var m = 0; m < overlayWindow.keyPoints.length; m++) {
                    var kp = overlayWindow.keyPoints[m]

                    dot(ctx, kp.x, kp.y, kp.size * kp.life,
                        overlayWindow.dotColor, kp.life * overlayWindow.keyAlpha)

                    dot(ctx, kp.x, kp.y, kp.size * kp.life * 2,
                        overlayWindow.dotColor, kp.life * overlayWindow.keyAlpha * 0.12)
                }

                // Частицы от распавшихся чат-карточек
                for (var t = 0; t < overlayWindow.chatParticles.length; t++) {
                    var tp = overlayWindow.chatParticles[t]

                    dot(ctx, tp.x, tp.y, tp.size * tp.life,
                        tp.color, tp.life * 0.55)
                }
            }
        }

        // ═══════════════════════════════════════
        // КАРТОЧКИ ЧАТА
        // UI рендерит напрямую chatEngine.m_messages
        // ═══════════════════════════════════════
        Item {
            id: chatLayer

            anchors.fill: parent
            anchors.bottomMargin: 96

            Flickable {
                id: chatScroller

                anchors.fill: parent

                contentWidth: width
                contentHeight: chatContent.height

                boundsBehavior: Flickable.StopAtBounds
                interactive: true

                onContentYChanged: {
                    overlayWindow.stickToBottom =
                        contentY >= contentHeight - height - 30
                }

                onContentHeightChanged: {
                    overlayWindow.updateChatScroll(false)
                }

                Item {
                    id: chatContent

                    width: chatScroller.width
                    height: Math.max(chatScroller.height, chatColumn.height + 20)

                    Column {
                        id: chatColumn

                        anchors.bottom: parent.bottom
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottomMargin: 10

                        width: overlayWindow.chatWidth
                        spacing: 10

                        Repeater {
                            model: chatEngine
                            delegate: cardComponent
                        }
                    }
                }
            }
        }

        // ═══════════════════════════════════════
        // ПОЛЕ ВВОДА — самый нижний прямоугольник
        // ═══════════════════════════════════════
        Rectangle {
            id: inputPanel

            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottomMargin: 18

            width: overlayWindow.chatWidth
            height: 58

            radius: 18

            color: Qt.rgba(0.055, 0.058, 0.09, 0.88)

            border.width: 1
            border.color: chatEngine.thinking
                ? Qt.rgba(0.98, 0.72, 0.36, 0.85)
                : inputField.activeFocus
                    ? Qt.rgba(0.36, 0.65, 0.98, 0.90)
                    : Qt.rgba(0.36, 0.65, 0.98, 0.45)

            Behavior on border.color {
                ColorAnimation {
                    duration: 140
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton

                onClicked: {
                    inputField.forceActiveFocus()
                }
            }

            Rectangle {
                id: inputAccentBar

                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: 9

                width: 3
                radius: 2

                color: "#60a5fa"
            }

            TextInput {
                id: inputField

                anchors.left: inputAccentBar.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter

                color: "#e2e2e8"
                font.pixelSize: 15
                font.family: "Sans Serif"

                clip: true
                selectByMouse: true
                activeFocusOnPress: true
                activeFocusOnTab: true

                onActiveFocusChanged: {
                    if (activeFocus) {
                        overlayWindow.clearOtherCardSelections(null)
                    }
                }

                onAccepted: {
                    if (chatEngine.thinking) {
                        inputField.forceActiveFocus()
                        return
                    }

                    var text = inputField.text.trim()

                    if (text.length > 0) {
                        chatEngine.sendMessage(text)
                        inputField.text = ""
                    }

                    inputField.forceActiveFocus()
                }

                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) {
                        overlayWindow.closeChatUi()
                        event.accepted = true
                    }
                }
            }

            Text {
                anchors.left: inputAccentBar.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter

                visible: inputField.text.length === 0

                text: chatEngine.thinking
                    ? "Агент отвечает..."
                    : "Напишите сообщение агенту..."
                color: "#4a4a56"
                font.pixelSize: 15
                font.family: "Sans Serif"
            }
        }
        Rectangle {
            id: copiedToast

            anchors.bottom: inputPanel.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottomMargin: 10

            width: copiedToastText.implicitWidth + 24
            height: 30

            radius: 15

            color: Qt.rgba(0.055, 0.058, 0.09, 0.92)

            border.width: 1
            border.color: Qt.rgba(0.36, 0.65, 0.98, 0.55)

            visible: overlayWindow.copiedToastVisible
            opacity: visible ? 1.0 : 0.0

            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }

            Text {
                id: copiedToastText

                anchors.centerIn: parent

                text: "Скопировано"
                color: "#e2e2e8"
                font.pixelSize: 13
            }
        }

        RagPanel {
            anchors.fill: parent
        }
    }

    // ═══════════════════════════════════════════
    // КОМПОНЕНТ КАРТОЧКИ СООБЩЕНИЯ
    // ═══════════════════════════════════════════
    // ═══════════════════════════════════════════
    // ПЛАВНОЕ ПОЯВЛЕНИЕ / ИСЧЕЗНОВЕНИЕ
    // ═══════════════════════════════════════════
    SequentialAnimation {
        id: showAnimation

        NumberAnimation {
            target: overlayContent
            property: "opacity"
            to: 1.0
            duration: 220
            easing.type: Easing.OutCubic
        }

        ScriptAction {
            script: overlayWindow.fading = false
        }
    }

    SequentialAnimation {
        id: hideAnimation

        NumberAnimation {
            target: overlayContent
            property: "opacity"
            to: 0.0
            duration: 220
            easing.type: Easing.InCubic
        }

        ScriptAction {
            script: {
                overlayWindow.active = false
                overlayWindow.visible = false
                overlayWindow.fading = false

                inputField.text = ""

                overlayWindow.keyPoints = []
                overlayWindow.chatParticles = []
            }
        }
    }
}
