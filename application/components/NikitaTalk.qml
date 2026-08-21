import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

import Theme 1.0
import QFlipper 1.0

Rectangle {
    id: root

    // ---- cross-message text selection -----------------------------------
    // Each message is its own TextEdit, and selectByMouse only ever works
    // inside one of them, which is why dragging could never get past a message
    // boundary. So the drag is driven from here instead: the anchor and the
    // head are (message index, character offset) pairs, and applySelection()
    // pushes the resulting range down onto whichever delegates currently exist.
    property int selAnchorIdx: -1
    property int selAnchorPos: 0
    property int selHeadIdx: -1
    property int selHeadPos: 0

    function bodyAt(i) {
        var it = listView.itemAtIndex(i);
        return (it && it.bodyEdit) ? it.bodyEdit : null;
    }
    // Normalised low/high ends, so dragging upwards behaves like dragging down.
    function selRange() {
        var a = root.selAnchorIdx, ap = root.selAnchorPos;
        var b = root.selHeadIdx,   bp = root.selHeadPos;
        if (a < 0 || b < 0) { return null; }
        if (b < a || (b === a && bp < ap)) { return { lo: b, loPos: bp, hi: a, hiPos: ap }; }
        return { lo: a, loPos: ap, hi: b, hiPos: bp };
    }
    function applySelection() {
        var r = root.selRange();
        for (var i = 0; i < chatModel.count; i++) {
            var e = root.bodyAt(i);
            if (!e) { continue; }                       // scrolled out and recycled
            if (!r || i < r.lo || i > r.hi)   { e.deselect(); }
            else if (i === r.lo && i === r.hi) { e.select(r.loPos, r.hiPos); }
            else if (i === r.lo)               { e.select(r.loPos, e.length); }
            else if (i === r.hi)               { e.select(0, r.hiPos); }
            else                               { e.selectAll(); }
        }
    }
    // ListView.spacing leaves a few pixels between delegates where indexAt()
    // reports -1. Dragging through that gap used to jump the selection to the
    // end of the conversation, so probe outwards before giving up.
    function msgIndexAt(cx, cy) {
        var idx = listView.indexAt(cx, cy);
        if (idx >= 0) { return idx; }
        for (var d = 2; d <= 16; d += 2) {
            idx = listView.indexAt(cx, cy - d);
            if (idx >= 0) { return idx; }
            idx = listView.indexAt(cx, cy + d);
            if (idx >= 0) { return idx; }
        }
        // Genuinely past an end of the content.
        if (cy < 0) { return 0; }
        if (cy > listView.contentHeight) { return chatModel.count - 1; }
        return -1;
    }

    // Double-click selects the sentence around the click, not just the word:
    // scan outward from the click position for ". " / "!" / "?" / a line
    // break (kept together so "..." or "?!" don't each start a new one), then
    // trim the leading space the previous terminator left behind. Operates on
    // getText() rather than the raw model text so positions line up with
    // positionAt()/selectionStart even when markdown syntax has been resolved
    // away by the delegate.
    function sentenceBoundsAt(edit, pos) {
        var text = edit.getText(0, edit.length);
        var start = pos;
        while (start > 0 && !/[.!?\n]/.test(text.charAt(start - 1))) { start--; }
        while (start < text.length && /\s/.test(text.charAt(start))) { start++; }
        var end = pos;
        while (end < text.length && !/[.!?\n]/.test(text.charAt(end))) { end++; }
        if (end < text.length) { end++; }   // include the terminator itself
        return { start: Math.min(start, end), end: end };
    }

    // The role label highlights only when the selection actually reaches the
    // start of that message: a drag beginning mid-sentence should not light
    // up the speaker's name above it.
    function indexInSelection(i) {
        var r = root.selRange();
        if (r === null || i < r.lo || i > r.hi) { return false; }
        return (i > r.lo) || (r.loPos === 0);
    }
    function clearSelection() {
        root.selAnchorIdx = -1;
        root.selHeadIdx = -1;
        root.applySelection();
    }
    function selectAllMessages() {
        if (chatModel.count === 0) { return; }
        root.selAnchorIdx = 0;
        root.selAnchorPos = 0;
        root.selHeadIdx = chatModel.count - 1;
        var last = root.bodyAt(chatModel.count - 1);
        root.selHeadPos = last ? last.length : chatModel.get(chatModel.count - 1).text.length;
        root.applySelection();
    }
    // Build the text for the clipboard. Visible delegates give the rendered
    // text (markdown already resolved); ones that were recycled fall back to
    // the raw message, which for a code block is arguably the more useful form.
    function selectedText() {
        var r = root.selRange();
        if (!r) { return ""; }
        var parts = [];
        for (var i = r.lo; i <= r.hi; i++) {
            var e = root.bodyAt(i);
            var raw = chatModel.get(i).text;
            var who = (chatModel.get(i).role === "nikita" ? root.aiName : "you") + ": ";
            if (e) {
                var a = (i === r.lo) ? r.loPos : 0;
                var b = (i === r.hi) ? r.hiPos : e.length;
                parts.push(who + e.getText(a, b));
            } else {
                parts.push(who + raw);
            }
        }
        return parts.join("\n");
    }
    function copySelection() {
        var t = root.selectedText();
        if (t.length === 0) { t = root.conversationText(); }
        Cli.copyToClipboard(t);
    }
    function copyMessage(i) {
        if (i < 0 || i >= chatModel.count) { return; }
        Cli.copyToClipboard(chatModel.get(i).text);
    }
    function conversationText() {
        var all = [];
        for (var i = 0; i < chatModel.count; i++) {
            all.push((chatModel.get(i).role === "nikita" ? root.aiName : "you") + ": " + chatModel.get(i).text);
        }
        return all.join("\n\n");
    }
    function copyConversation() { Cli.copyToClipboard(root.conversationText()); }

    // Nothing ever called copySelection() or selectAllMessages(): the drag could
    // build a selection but there was no way to get it out, which is the whole
    // reason copying here felt broken next to the CLI. Guarded on the input not
    // having focus, so Cmd+C/Cmd+A still mean what they should while typing.
    Shortcut {
        sequences: [StandardKey.Copy]
        // Not while the CLI owns the screen, or the log panel is slid up over
        // it: both have their own selection, and a Shortcut fires regardless
        // of what is actually on top -- this one used to win the race and
        // swallow Cmd+C meant for the log view, which read as "copy does
        // nothing" over there since the chat's own selection wasn't visible.
        enabled: root.visible && !input.activeFocus && !Cli.open && !App.logsOpen
        onActivated: root.copySelection()
    }
    Shortcut {
        sequences: [StandardKey.SelectAll]
        enabled: root.visible && !Cli.open && !App.logsOpen
        onActivated: root.selectAllMessages()
    }

    // Right-click menu on a message: the discoverable half of the same thing.
    Menu {
        id: msgMenu
        property int msgIndex: -1
        MenuItem {
            text: "Copy selection"
            enabled: root.selRange() !== null
            onTriggered: root.copySelection()
        }
        MenuItem {
            text: "Copy this message"
            onTriggered: root.copyMessage(msgMenu.msgIndex)
        }
        MenuItem {
            text: "Copy whole conversation"
            onTriggered: root.copyConversation()
        }
        MenuItem {
            text: "Select all"
            onTriggered: root.selectAllMessages()
        }
    }

    // Dragging a selection past the top or bottom edge scrolls the log, the way
    // every other text view does. Without it a selection could never reach
    // anything that was off-screen when the drag started.
    property real dragScroll: 0
    Timer {
        interval: 16; repeat: true; running: root.dragScroll !== 0
        onTriggered: {
            var maxY = Math.max(0, listView.contentHeight - listView.height);
            listView.contentY = Math.max(0, Math.min(maxY, listView.contentY + root.dragScroll));
        }
    }

    // Header label. "no model" read like the name of a model; this says what it
    // actually means, and every layer of the chromatic-aberration stack reads
    // the same property so they can never drift apart.
    readonly property bool hasModel: Nikita.modelName.length > 0
    readonly property string modelLabel: root.hasModel ? Nikita.modelName : "no model selected"

    // ---- view state / geometry ------------------------------------------
    // "normal" = docked in the corner, "max" = big read view, "min" = collapsed
    property string viewState: "normal"

    // memory.txt may have been edited in the file manager while this panel was
    // hidden. Re-read the card's copy whenever the chat comes back into view,
    // so the conversation starts from what the file actually says.
    onVisibleChanged: if (visible) { Nikita.reloadMemory(); }

    readonly property int dockX: 24
    readonly property int dockY: 272
    readonly property int dockW: 384
    readonly property int dockH: 146
    readonly property int minH: 44
    property int streamIdx: -1   // index of the message currently being streamed
    property bool showScreen: false   // mirror the live Flipper screen in-panel
    // The assistant is Nikita. The Flipper has its own name, set on the device,
    // and it is shown in the header. Deriving one from the other meant a device
    // with no name left the assistant without one too.
    readonly property string aiName: "Nikita"

    x: viewState === "max" ? 14 : dockX
    y: viewState === "max" ? 78 : dockY
    width:  viewState === "max" ? ((parent ? parent.width  : 804) - 28) : dockW
    height: viewState === "max" ? ((parent ? parent.height : 394) - 92)
            : (viewState === "min" ? minH : dockH)

    Behavior on width  { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    Behavior on height { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    Behavior on y      { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

    radius: 9
    color: "#0b0410"
    border.width: 2
    border.color: Theme.color.lightorange2

    // ---- helpers --------------------------------------------------------
    function deviceContext() {
        var ds = Backend.deviceState;
        if(!ds || !ds.info) {
            return "No Flipper is currently connected.";
        }
        var i = ds.info;
        var lines = [];
        lines.push("Name: " + i.name);
        if(ds.isRecoveryMode) {
            lines.push("Mode: Update & Recovery (DFU)");
        } else {
            var fw = i.firmware.version;
            if(i.firmware.commit && i.firmware.commit.length) { fw += " (commit " + i.firmware.commit + ")"; }
            lines.push("Firmware: " + fw);
            lines.push("SD card: " + (i.storage.isExternalPresent ? (i.storage.externalFree + "% free") : "not present"));
            lines.push("Databases: " + (i.storage.isAssetsInstalled ? "installed" : "missing"));
            lines.push("Radio firmware: " + (i.radioVersion.length ? i.radioVersion : "corrupted"));
        }
        lines.push("Hardware: " + i.hardware.version + "." + i.hardware.target + i.hardware.body + i.hardware.connect);
        return lines.join("\n");
    }

    function appendMessage(role, text) {
        chatModel.append({ "role": role, "text": text });
        listView.positionViewAtEnd();
    }

    function clearChat() {
        chatModel.clear();
        Nikita.clearHistory();
    }

    function sendCurrent() {
        var t = input.text.trim();
        if(t.length === 0 || Nikita.thinking) {
            return;
        }
        appendMessage("you", t);
        Nikita.send(t, deviceContext());
        input.text = "";
    }


    ListModel { id: chatModel }

    // parent: root centered these on NikitaTalk's own narrow strip instead of
    // the screen -- a CustomDialog is a Popup sized to fill and center in
    // whatever it's parented to, and root is the chat panel, not the window.
    // Same fix modelManager already uses below: root.parent is the overlay
    // NikitaTalk sits inside, which fills mainContent -- the actual content
    // area of the window -- so centering there reads as centered on screen.
    HostRunConfirmDialog {
        id: hostRunConfirmDialog
        radius: root.radius
        parent: root.parent ? root.parent : root
    }

    HostActionConfirmDialog {
        id: hostActionConfirmDialog
        radius: root.radius
        parent: root.parent ? root.parent : root
    }

    Connections {
        target: Nikita
        // host_run is waiting on screen: nothing runs on this computer until
        // the command is answered here.
        function onHostRunConfirmRequested(command, cwd) {
            hostRunConfirmDialog.openWithCommand(function(allow, always) {
                Nikita.answerHostRunConfirm(allow, always);
            }, command, cwd);
        }
        // Same idea for host_write/host_mkdir/host_move/host_copy/host_delete:
        // nothing touches a file on this computer until it's answered here.
        function onHostActionConfirmRequested(kind, summary, detail) {
            hostActionConfirmDialog.openWithAction(function(allow, always) {
                Nikita.answerHostActionConfirm(allow, always);
            }, kind, summary, detail);
        }
        // live typing: grow one bubble as tokens arrive
        function onPartialReceived(text) {
            if(root.streamIdx < 0) {
                chatModel.append({ "role": "nikita", "text": text });
                root.streamIdx = chatModel.count - 1;
            } else {
                chatModel.setProperty(root.streamIdx, "text", text);
            }
            listView.positionViewAtEnd();
        }
        function onReplyReceived(text) {
            if(root.streamIdx >= 0) {
                chatModel.setProperty(root.streamIdx, "text", text);  // finalize
                root.streamIdx = -1;
            } else {
                root.appendMessage("nikita", text);
            }
            listView.positionViewAtEnd();
        }
        function onErrorOccurred(text) {
            if(root.streamIdx >= 0) {
                chatModel.setProperty(root.streamIdx, "text", text);
                root.streamIdx = -1;
            } else {
                root.appendMessage("nikita", text);
            }
        }
        // A model was picked (or the first one finished installing).
        function onModelChanged() { root.greetIfReady(); }
        // Feedback from the manual save panel (model-free save straight to SD).
        function onScriptSaved(path) {
            root.appendMessage("nikita", "✅ Salvo em " + path);
            listView.positionViewAtEnd();
        }
        function onScriptSaveError(message) {
            root.appendMessage("nikita", "⚠️ Couldn't save: " + message);
            listView.positionViewAtEnd();
        }
    }


    // The greeting is the assistant speaking, so it waits until there is an
    // assistant to speak. With no model selected there is nobody behind it, and
    // an empty chat that opens with "how can I help you today?" only to reject
    // the first message is worse than saying nothing. The empty state over the
    // log explains the situation instead, and the greeting lands the moment a
    // model is picked.
    function greetIfReady() {
        if (!root.hasModel) { return; }
        if (chatModel.count > 0) { return; }
        root.appendMessage("nikita", "Hey, how can I help you today?");
    }

    Component.onCompleted: {
        // Kept ahead of reloadMemory() for the original reason: anything that
        // throws in there used to take the greeting down with it.
        greetIfReady();
        Nikita.reloadMemory();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 9
        spacing: 5

        // ---- header: name + status + window buttons ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 5

            // ---- cyberpunk model badge (status dot + glitch text + cursor) ----
            Item {
                id: modelBadge
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: dot.width + 8 + modelFg.implicitWidth + 10
                implicitHeight: Math.max(modelFg.implicitHeight, 8) + 2

                QtObject { id: glitch; property real off: 0 }
                Timer {
                    interval: 110; running: true; repeat: true
                    onTriggered: glitch.off = (Math.random() < 0.16) ? (Math.random() * 2.4) : 0
                }

                // Pulsing status dot, read left to right like a traffic light:
                //   red:    no model selected, nothing can be sent
                //   yellow: working on an answer
                //   green:  ready for the next message
                Rectangle {
                    id: dot
                    width: 6; height: 6; radius: 3
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    color: !root.hasModel   ? "#ff3b30"
                         : Nikita.thinking   ? "#ffd400"
                                            : "#39ff14"
                    Behavior on color { ColorAnimation { duration: 150 } }
                    SequentialAnimation on opacity {
                        loops: Animation.Infinite; running: true
                        NumberAnimation { from: 1.0; to: 0.2; duration: 850; easing.type: Easing.InOutSine }
                        NumberAnimation { from: 0.2; to: 1.0; duration: 850; easing.type: Easing.InOutSine }
                    }
                }

                // chromatic-aberration layers (magenta + cyan behind, bright on top)
                Text {
                    text: root.modelLabel
                    color: "#ff2fb0"; opacity: 0.7
                    anchors.left: dot.right; anchors.leftMargin: 8 + glitch.off
                    anchors.verticalCenter: parent.verticalCenter
                    font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                }
                Text {
                    text: root.modelLabel
                    color: "#00e5ff"; opacity: 0.7
                    anchors.left: dot.right; anchors.leftMargin: 8 - glitch.off
                    anchors.verticalCenter: parent.verticalCenter
                    font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                }
                Text {
                    id: modelFg
                    text: root.modelLabel
                    color: "#eaffea"
                    anchors.left: dot.right; anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                }
            }
            Item { Layout.fillWidth: true }

            // Model manager (gear icon)
            Rectangle {
                // Needs an id because the Canvas below reads gearHot. An
                // unqualified name in a binding resolves against the object
                // itself, then the root of this file, and it does NOT walk up
                // through parent objects. So `gearHot` inside the Canvas was a
                // ReferenceError, the tint binding died, and `tint` sat at the
                // default-constructed colour: black. Hence the invisible gear.
                id: gearBtn
                Layout.preferredWidth: 26
                Layout.preferredHeight: 20
                Layout.alignment: Qt.AlignVCenter
                radius: 3
                // One source of truth for both the box and the icon colour: the
                // panel covers this button while it is open, so hover there is
                // never real.
                readonly property bool gearHot: gearMouse.containsMouse && !modelManager.open
                color: gearBtn.gearHot ? Qt.rgba(1, 1, 1, 0.14) : "transparent"

                // Drawn rather than typed. "\u2699" has no glyph in Share Tech
                // Mono (nor in the app's other faces), so Qt was substituting a
                // system symbol font for that one character, which is why the
                // gear came out smaller than its neighbours and sitting on a
                // different baseline. A Canvas has no font to fall back to: it
                // scales cleanly and takes the theme colour directly.
                Canvas {
                    id: gearIcon
                    width: 15; height: 15
                    anchors.centerIn: parent
                    // Constant. The hover feedback is the box behind it; the
                    // icon itself stays the theme pink.
                    property color tint: Theme.color.lightorange2
                    onTintChanged: requestPaint()
                    Component.onCompleted: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d");
                        // reset() restores the context state but does NOT erase
                        // what is already on the canvas, and a Canvas keeps its
                        // pixels between paints. So the bright hover gear was
                        // still sitting underneath when the normal one was drawn
                        // back over it, and its antialiased edges showed through
                        // as a white fringe that never went away: the gear
                        // looked permanently stuck on.
                        ctx.reset();
                        ctx.clearRect(0, 0, width, height);
                        ctx.translate(width / 2, height / 2);
                        ctx.fillStyle = gearIcon.tint;

                        var teeth = 8;
                        var rOuter = width * 0.50;
                        var rBody  = width * 0.34;
                        var toothW = width * 0.20;

                        for (var i = 0; i < teeth; i++) {
                            ctx.save();
                            ctx.rotate(i * Math.PI / teeth * 2);
                            ctx.fillRect(-toothW / 2, -rOuter, toothW, rOuter - rBody + 1);
                            ctx.restore();
                        }
                        ctx.beginPath();
                        ctx.arc(0, 0, rBody, 0, Math.PI * 2);
                        ctx.fill();

                        // Punch the hub out so it reads as a gear, not a blob.
                        ctx.globalCompositeOperation = "destination-out";
                        ctx.beginPath();
                        ctx.arc(0, 0, width * 0.14, 0, Math.PI * 2);
                        ctx.fill();
                    }
                }
                MouseArea {
                    id: gearMouse
                    anchors.fill: parent
                    // Switched off while the panel is up. Opening it puts a
                    // full-area MouseArea over this button, so the exit event
                    // never arrived and containsMouse stayed true forever --
                    // the gear kept its hover box and its bright hover colour
                    // even after the pointer had gone. Dropping hoverEnabled
                    // resets containsMouse to false.
                    hoverEnabled: !modelManager.visible
                    cursorShape: Qt.PointingHandCursor
                    onClicked: modelManager.openManager()
                }

            }

            // Clear conversation
            Rectangle {
                Layout.preferredWidth: 42
                Layout.preferredHeight: 20
                Layout.alignment: Qt.AlignVCenter
                radius: 3
                color: clearMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.14) : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: "clear"
                    color: Theme.color.lightorange2
                    font.family: "Share Tech Mono"; font.pixelSize: 11
                }
                MouseArea {
                    id: clearMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.clearChat()
                }

            }

            // Minimize / restore (collapse to title bar)
            Rectangle {
                Layout.preferredWidth: 26
                Layout.preferredHeight: 20
                Layout.alignment: Qt.AlignVCenter
                radius: 3
                color: minMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.14) : "transparent"

                Rectangle {
                    width: 11; height: 2; radius: 1
                    color: Theme.color.lightorange2
                    anchors.centerIn: parent
                }
                MouseArea {
                    id: minMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.viewState = (root.viewState === "min") ? "normal" : "min"
                }
            }

            // Maximize / restore (big read view)
            Rectangle {
                Layout.preferredWidth: 26
                Layout.preferredHeight: 20
                Layout.alignment: Qt.AlignVCenter
                radius: 3
                color: maxMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.14) : "transparent"

                Rectangle {
                    width: root.viewState === "max" ? 8 : 11
                    height: width
                    color: "transparent"
                    border.width: 2
                    border.color: Theme.color.lightorange2
                    anchors.centerIn: parent
                    Behavior on width { NumberAnimation { duration: 120 } }
                }
                MouseArea {
                    id: maxMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.viewState = (root.viewState === "max") ? "normal" : "max"
                }
            }
        }

        // ---- live Flipper screen mirror (Phase A: tap qFlipper's stream) ----
        Rectangle {
            visible: root.showScreen && Backend.screenStreamer && Backend.screenStreamer.isEnabled
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? (root.viewState === "max" ? 180 : 72) : 0
            color: "#0b0410"
            radius: 5
            border.width: 1
            border.color: Theme.color.mediumorange2
            ScreenCanvas {
                anchors.fill: parent
                anchors.margins: 4
                frame: Backend.screenStreamer.screenFrame
                foregroundColor: Theme.color.lightorange2
                backgroundColor: "#0b0410"
            }
        }

        // ---- message log (hidden when minimized) ----
        ListView {
            id: listView
            visible: root.viewState !== "min"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: chatModel
            spacing: 6
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            // Shown while the model is generating; the only real wait in the UI.
            footer: Item {
                width: ListView.view ? ListView.view.width : 0
                height: Nikita.thinking ? 20 : 0
                visible: Nikita.thinking
                Row {
                    spacing: 0
                    Text {
                        text: root.aiName + " is thinking"
                        color: Theme.color.mediumorange4
                        font.family: "Share Tech Mono"
                        font.pixelSize: 12
                    }
                    Text {
                        id: thinkDots
                        color: Theme.color.mediumorange4
                        font.family: "Share Tech Mono"
                        font.pixelSize: 12
                        property int step: 0
                        text: ["   ", ".  ", ".. ", "..."][step]
                    }
                    Timer {
                        running: Nikita.thinking
                        interval: 380; repeat: true
                        onTriggered: thinkDots.step = (thinkDots.step + 1) % 4
                    }
                }
            }

            delegate: Column {
                id: msgCol
                width: ListView.view.width
                spacing: 1

                // Lets root.applySelection() reach this message's text.
                property alias bodyEdit: bodyText

                TextEdit { id: clip; visible: false }  // hidden clipboard helper

                HoverHandler { id: msgHover }

                Row {
                    spacing: 8
                    Rectangle {
                        width: roleLabel.implicitWidth + 4
                        height: roleLabel.implicitHeight
                        color: root.indexInSelection(index) ? roleLabel.selectionColor : "transparent"
                        Text {
                            id: roleLabel
                            x: 2
                            readonly property color selectionColor: Theme.color.mediumorange2
                            text: model.role === "nikita" ? root.aiName : "you"
                            color: model.role === "nikita" ? Theme.color.lightorange2 : Theme.color.mediumorange1
                            font.family: "Share Tech Mono"
                            font.pixelSize: 11
                        }
                    }
                    // One click for the whole message, since that is what people
                    // want most of the time and a keyboard shortcut nobody can
                    // see is not an interface.
                    // msgCopyBtn, not copyBtn: the save-script panel further down
                    // this same delegate already owns that id, and two of them in
                    // one component scope is a hard QML error: the whole file
                    // fails to load, which takes MainWindow and the app with it.
                    Text {
                        id: msgCopyBtn
                        property bool done: false
                        visible: msgHover.hovered || done
                        text: done ? "copied" : "copy"
                        color: done ? "#39ff14"
                                    : (msgCopyMouse.containsMouse ? Theme.color.lightorange2 : Theme.color.mediumorange4)
                        font.family: "Share Tech Mono"
                        font.pixelSize: 10
                        anchors.verticalCenter: parent.verticalCenter
                        Timer { id: msgCopiedReset; interval: 1200; onTriggered: msgCopyBtn.done = false }
                        MouseArea {
                            id: msgCopyMouse
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.copyMessage(index);
                                msgCopyBtn.done = true;
                                msgCopiedReset.restart();
                            }
                        }
                    }
                }
                TextEdit {
                    id: bodyText
                    width: parent.width
                    text: model.text
                    readOnly: true
                    // Off on purpose: the drag below spans messages, and the
                    // built-in one would fight it inside this single message.
                    selectByMouse: false
                    persistentSelection: true
                    wrapMode: TextEdit.Wrap
                    textFormat: model.role === "nikita" ? TextEdit.MarkdownText : TextEdit.PlainText
                    color: "white"
                    // Spelled out rather than left to the style: the default
                    // highlight is nearly invisible on this background, so a
                    // selection that was working still looked like nothing had
                    // happened. Same blue the role label uses.
                    selectionColor: Theme.color.mediumorange2
                    selectedTextColor: "white"
                    font.family: "Share Tech Mono"
                    font.pixelSize: 13
                    onLinkActivated: function(link) { Qt.openUrlExternally(link) }

                    // Covers the message text and nothing else, so the save
                    // panel below keeps its own mouse handling untouched.
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        cursorShape: Qt.IBeamCursor
                        // The list is a Flickable: without this it steals the
                        // drag halfway through and treats it as a scroll, which
                        // is what made the selection break up mid-gesture.
                        preventStealing: true

                        onPressed: function(mouse) {
                            if (mouse.button === Qt.RightButton) {
                                // Keep an existing selection: right-clicking
                                // inside one in order to copy it should not be
                                // the thing that throws it away.
                                if (!root.indexInSelection(index)) { root.clearSelection(); }
                                msgMenu.msgIndex = index;
                                msgMenu.popup();
                                return;
                            }
                            root.selAnchorIdx = index;
                            root.selAnchorPos = bodyText.positionAt(mouse.x, mouse.y);
                            root.selHeadIdx = index;
                            root.selHeadPos = root.selAnchorPos;
                            root.applySelection();
                        }
                        onReleased: root.dragScroll = 0
                        onCanceled: root.dragScroll = 0
                        onPositionChanged: function(mouse) {
                            if (!pressed) { return; }
                            // Past an edge of the viewport -> scroll towards it,
                            // faster the further out the pointer goes.
                            var vp = mapToItem(listView, mouse.x, mouse.y);
                            if (vp.y < 0)                        { root.dragScroll = Math.max(-30, vp.y / 2); }
                            else if (vp.y > listView.height)     { root.dragScroll = Math.min(30, (vp.y - listView.height) / 2); }
                            else                                 { root.dragScroll = 0; }
                            // Which message is under the cursor now? Map into the
                            // list's content item so the answer stays right while
                            // the view scrolls.
                            var pt = mapToItem(listView.contentItem, mouse.x, mouse.y);
                            var idx = root.msgIndexAt(pt.x, pt.y);
                            if (idx < 0) { return; }
                            var e = root.bodyAt(idx);
                            if (!e) { return; }
                            var local = mapToItem(e, mouse.x, mouse.y);
                            root.selHeadIdx = idx;
                            root.selHeadPos = e.positionAt(local.x, local.y);
                            root.applySelection();
                        }
                        onDoubleClicked: function(mouse) {
                            var pos = bodyText.positionAt(mouse.x, mouse.y);
                            var sel = root.sentenceBoundsAt(bodyText, pos);
                            root.selAnchorIdx = index;
                            root.selHeadIdx = index;
                            bodyText.select(sel.start, sel.end);
                            root.selAnchorPos = sel.start;
                            root.selHeadPos = sel.end;
                        }
                    }
                }

                // ---- Manual save panel (model-free) --------------------------
                // Shown for Nikita replies that contain a code block. YOU pick the
                // folder + name and hit save; the app writes straight to the SD.
                // The AI drafts, you save, with no fumbled tool calls.
                Rectangle {
                    visible: model.role === "nikita" && model.text.indexOf("```") >= 0
                    width: parent.width
                    implicitHeight: panelCol.implicitHeight + 16
                    color: "#0A0010"
                    border.color: Theme.color.mediumorange2
                    border.width: 1
                    radius: 3

                    Column {
                        id: panelCol
                        x: 8; y: 8
                        width: parent.width - 16
                        spacing: 6

                        // top bar: copy, and nothing else.
                        //
                        // The folder picker, the filename field and the save
                        // button used to live here, from before Nikita could
                        // write to the card himself. He has save_file now, and
                        // the CLI panel has cp and edit, so the manual path was
                        // a third way to do the same thing -- one that appeared
                        // on EVERY fenced block, asking which Flipper folder to
                        // put a shell one-liner in.
                        Item {
                            width: parent.width
                            height: 26

                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: "⧉ copy"
                                color: copyBtn.containsMouse ? Theme.color.lightorange2 : Theme.color.mediumorange1
                                font.family: "Share Tech Mono"; font.pixelSize: 12
                                MouseArea {
                                    id: copyBtn; anchors.fill: parent; anchors.margins: -4
                                    hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: { clip.text = scriptArea.text; clip.selectAll(); clip.copy(); }
                                }
                            }
                        }

                        // the script itself, editable, terminal-styled
                        Rectangle {
                            width: parent.width
                            implicitHeight: Math.min(scriptArea.implicitHeight + 12, 260)
                            color: "#000000"
                            border.color: Theme.color.mediumorange1
                            border.width: 1
                            radius: 2
                            Flickable {
                                anchors.fill: parent
                                anchors.margins: 6
                                contentHeight: scriptArea.implicitHeight
                                clip: true
                                TextArea {
                                    id: scriptArea
                                    width: parent.width
                                    text: Nikita.extractScript(model.text)
                                    wrapMode: TextArea.NoWrap
                                    color: Theme.color.lightgreen
                                    selectByMouse: true
                                    font.family: "Share Tech Mono"
                                    font.pixelSize: 12
                                    background: null
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---- input row (hidden when minimized) ----
        RowLayout {
            visible: root.viewState !== "min"
            Layout.fillWidth: true
            spacing: 6

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                radius: 6
                color: "black"
                border.width: 1
                border.color: Theme.color.mediumorange2

                TextInput {
                    id: input
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: "white"
                    selectionColor: Theme.color.lightorange2
                    font.family: "Share Tech Mono"
                    font.pixelSize: 13
                    enabled: !Nikita.thinking && root.hasModel
                    onAccepted: root.sendCurrent()

                    Text {
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        visible: input.text.length === 0 && !input.activeFocus
                        text: root.hasModel ? ("Talk to " + root.aiName + "…")
                                            : "Select a model to start…"
                        color: Theme.color.mediumorange1
                        font: input.font
                    }
                }
            }

            Button {
                // Becomes the interrupt control the instant a turn starts --
                // same idea as Claude Code's own stop-mid-response button.
                text: Nikita.thinking ? "Stop" : "Send"
                enabled: Nikita.thinking || (root.hasModel && input.text.length > 0)
                onClicked: Nikita.thinking ? Nikita.stopThinking() : root.sendCurrent()
            }
        }
    }

    // Empty state for the log. Parented to listView rather than declared inside
    // it: a visual child declared in a ListView is reparented to the scrolling
    // contentItem, which is zero-height when there are no rows to give it one.
    //
    // One line, not three. The heading repeated what the header already says a
    // few pixels above it, and the "open model manager" link repeated the gear
    // sitting right next to that; in the docked size all of it collided with
    // the input box.
    Text {
        parent: listView
        anchors.centerIn: parent
        width: Math.max(120, listView.width - 32)
        visible: chatModel.count === 0 && root.viewState !== "min"
        text: root.hasModel ? "No messages yet."
                            : "Pick or install a model to start talking."
        color: Theme.color.mediumorange1
        font.family: "Share Tech Mono"; font.pixelSize: 11
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }

    // ---- Model manager panel (gear icon) ----------------------------------
    // Not a Popup any more. A modal Popup brought a style-drawn dim with it and
    // read as something layered on top of the app; the CLI panel next door is
    // just an Item filling the same content box, and that is what the rest of
    // this UI looks like. So this is the same shape: parented to the overlay
    // NikitaTalk itself sits in (which fills mainContent, exactly the area the
    // CLI covers), toggled by `open`, no dim behind it.
    Item {
        id: modelManager
        parent: root.parent ? root.parent : root
        anchors.fill: parent
        z: 200

        property bool open: false
        visible: opacity > 0
        enabled: visible
        // Needed for Escape to reach Keys.onEscapePressed below; dropped again
        // on close so the chat input can take the keyboard back.
        focus: open
        opacity: open ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.InOutQuad } }

        // Model list is only pulled fresh when the panel opens, not polled --
        // it changes only in response to actions this same panel drives.
        function openManager() {
            catalogModel.refresh();
            Nikita.detectOllama();
            modelManager.open = true;
        }
        function close() { modelManager.open = false; }

        // Swallows anything aimed at the screen behind, the way cliOverlay does.
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onWheel: function(wheel) { wheel.accepted = true }
        }

        Keys.onEscapePressed: modelManager.close()


        // Backing list model for the catalog. A plain ListModel refreshed from
        // Nikita.modelCatalog(): simplest option here since the catalog is
        // small (a handful of curated entries), not something worth a C++
        // QAbstractListModel for.
        ListModel { id: catalogModel
            function refresh() {
                // Update rows in place rather than clear()+append(): clearing
                // drops the list to zero items for a frame, which snaps
                // catalogView's scroll position back to the top on every
                // refresh, annoying if you'd scrolled down and an install
                // finishes, or Ollama comes online, mid-scroll.
                var rows = Nikita.modelCatalog();
                for (var i = 0; i < rows.length; i++) {
                    if (i < count) { set(i, rows[i]); } else { append(rows[i]); }
                }
                while (count > rows.length) { remove(count - 1); }
            }
        }

        // modelOpKind/Name/Status/Progress all share one NOTIFY (modelOpChanged),
        // which fires on every chunk of `ollama pull` output, many times a
        // second while a download is running. The busy row's progress bar and
        // status text already bind straight to Nikita.modelOpProgress/modelOpStatus
        // below, so they don't need a list rebuild to update. Only rebuild when
        // a row actually needs to flip into/out of "busy", i.e. when the op
        // kind itself changes; otherwise every tick was tearing down and
        // recreating all the delegates, which is what caused the flicker/glitch
        // during a pull.
        QtObject {
            id: modelOpTracker
            property string kind: ""
            // What the result line at the bottom shows. A plain binding to
            // Nikita.modelOpStatus left "Ollama installed." (or any other
            // finished-op message) sitting there indefinitely -- nothing
            // ever set it back to empty, since the backend property is only
            // ever overwritten by the NEXT operation, which might be
            // minutes or never. Fading it out on a timer instead means it
            // read as a toast, not a stuck label.
            property string fadingStatus: ""
        }

        Timer {
            id: fadingStatusTimer
            interval: 4000
            onTriggered: modelOpTracker.fadingStatus = ""
        }

        Connections {
            target: Nikita
            function onModelOpChanged() {
                if (Nikita.modelOpKind !== modelOpTracker.kind) {
                    const wasRunning = modelOpTracker.kind !== "";
                    modelOpTracker.kind = Nikita.modelOpKind;
                    catalogModel.refresh();
                    // Just finished (kind went back to ""): show the result,
                    // then fade it.
                    if (wasRunning && Nikita.modelOpKind === "" && Nikita.modelOpStatus.length > 0) {
                        modelOpTracker.fadingStatus = Nikita.modelOpStatus;
                        fadingStatusTimer.restart();
                    }
                }
            }
            function onModelInstallFinished()     { catalogModel.refresh(); }
            function onModelUninstallFinished()   { catalogModel.refresh(); }
            function onOllamaInstallFinished()    { catalogModel.refresh(); }
            function onOllamaInstalledChanged()   { catalogModel.refresh(); }
            function onModelChanged()             { if (modelManager.visible) { catalogModel.refresh(); } }
        }

        // Same panel chrome as the CLI: filled card on the content box, one
        // border, title in the header row, close on the right.
        Rectangle {
            anchors.fill: parent
            color: "#0b0410"
            radius: 8
            border.width: 2
            border.color: Theme.color.mediumorange2
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: "CHOOSE A MODEL"
                    color: Theme.color.lightorange2
                    font.family: "Share Tech Mono"; font.pixelSize: 20; font.bold: true
                    Layout.fillWidth: true
                }
                Text {
                    text: "\u2715"   // ✕
                    color: closeMouse.containsMouse ? Theme.color.lightorange2 : Theme.color.mediumorange4
                    font.family: "Share Tech Mono"; font.pixelSize: 18
                    MouseArea {
                        id: closeMouse
                        anchors.fill: parent
                        anchors.margins: -6
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: modelManager.close()
                    }
                }
            }

            // ---- "Ollama itself isn't installed" banner ----
            Rectangle {
                visible: !Nikita.ollamaInstalled
                Layout.fillWidth: true
                Layout.preferredHeight: ollamaBannerCol.implicitHeight + 16
                radius: 5
                color: "#2a0a0a"
                border.width: 1
                border.color: "#ff5a5a"

                ColumnLayout {
                    id: ollamaBannerCol
                    x: 10; y: 8
                    width: parent.width - 20
                    spacing: 6

                    Text {
                        text: "Ollama isn't installed on this machine."
                        color: "#ffb3b3"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        font.family: "Share Tech Mono"; font.pixelSize: 11
                    }
                }
            }

            // ---- optional extra brain: real Claude, on the user's own account ----
            // Not part of catalogModel/catalogView on purpose: that list is built
            // from NIKITA_CATALOG (pull/remove Ollama models), and Claude is
            // neither -- it's detected (installed + logged in), never installed
            // or removed from here.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: claudeRowCol.implicitHeight + 16
                radius: 5
                color: (Nikita.modelName === "Claude") ? "#1a0d24" : "#120818"
                border.width: 1
                border.color: Nikita.claudeAvailable
                    ? ((Nikita.modelName === "Claude") ? Theme.color.lightorange2 : Theme.color.mediumorange2)
                    : "#5a3a3a"

                ColumnLayout {
                    id: claudeRowCol
                    x: 10; y: 8
                    width: parent.width - 20
                    spacing: 4

                    // No polling loop tied to panel visibility to gate against
                    // (this Rectangle exists whenever the manager does) -- cheap
                    // enough (one quick subprocess) that ticking while not
                    // available and stopping the moment it is costs nothing
                    // worth guarding further.
                    Timer {
                        interval: 4000
                        running: !Nikita.claudeAvailable
                        repeat: true
                        onTriggered: Nikita.detectClaude()
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Text {
                            // Fixed -- never swaps for account text, an error, or
                            // anything else. This is the row's name, full stop.
                            text: "Claude"
                            color: "#eaffea"
                            font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                            Layout.fillWidth: true
                        }
                        Text {
                            visible: Nikita.claudeAvailable
                            text: Nikita.claudeAccountLabel
                            color: "#39ff14"
                            font.family: "Share Tech Mono"; font.pixelSize: 10
                        }
                        Rectangle {
                            visible: Nikita.claudeAvailable && Nikita.modelName !== "Claude"
                            Layout.preferredWidth: claudeEquipLabel.implicitWidth + 14
                            Layout.preferredHeight: 16
                            radius: 3
                            color: claudeEquipMouse.containsMouse ? Theme.color.lightorange2 : "transparent"
                            border.width: 1
                            border.color: Theme.color.lightorange2
                            Text {
                                id: claudeEquipLabel
                                anchors.centerIn: parent
                                text: "EQUIP"
                                color: claudeEquipMouse.containsMouse ? "#0b0410" : Theme.color.lightorange2
                                font.family: "Share Tech Mono"; font.pixelSize: 10; font.bold: true
                            }
                            MouseArea {
                                id: claudeEquipMouse
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Nikita.setModel("Claude")
                            }
                        }
                    }

                    Text {
                        // Fixed -- same wording whether signed in or not. What
                        // sign in/out below does is self-explanatory; this line
                        // doesn't need to narrate the account state on top of it.
                        text: "Runs on your Claude subscription plan."
                        color: Theme.color.mediumorange1
                        font.family: "Share Tech Mono"; font.pixelSize: 10
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    // Status pill + sign in/out, bottom row -- same spot, same
                    // pill styling (color(0f3d1f)/border/radius all copied
                    // verbatim) and the same visual language for the action
                    // link, as installed/not-installed + install/uninstall in
                    // the catalog rows below.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            Layout.preferredWidth: claudeConnPill.implicitWidth + 12
                            Layout.preferredHeight: 18
                            radius: 3
                            color: Nikita.claudeAvailable ? "#0f3d1f" : "transparent"
                            border.width: Nikita.claudeAvailable ? 0 : 1
                            border.color: Theme.color.mediumorange2
                            Text {
                                id: claudeConnPill
                                anchors.centerIn: parent
                                text: Nikita.claudeAvailable ? "connected" : "not connected"
                                color: Nikita.claudeAvailable ? "#39ff14" : Theme.color.mediumorange1
                                font.family: "Share Tech Mono"; font.pixelSize: 10
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            visible: !Nikita.claudeAvailable
                            // Two different clicks behind one label would be
                            // fine if they both did something -- but "sign
                            // in" on a machine with no `claude` CLI used to
                            // just do nothing, which read as broken rather
                            // than as "go install it first".
                            text: Nikita.claudeInstalled ? "sign in" : "install"
                            color: claudeSignInMouse.containsMouse ? Theme.color.lightgreen : Theme.color.lightorange2
                            font.family: "Share Tech Mono"; font.pixelSize: 11; font.bold: true
                            MouseArea {
                                id: claudeSignInMouse
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Nikita.claudeSignIn()
                            }
                        }
                        Text {
                            visible: Nikita.claudeAvailable
                            text: "sign out"
                            color: claudeSignOutMouse.containsMouse ? "#ff6a6a" : Theme.color.mediumorange1
                            font.family: "Share Tech Mono"; font.pixelSize: 11
                            MouseArea {
                                id: claudeSignOutMouse
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Nikita.claudeSignOut()
                            }
                        }
                    }
                }
            }

            // ---- Ollama missing: one big themed button instead of a list of
            // models that can't be installed anyway. Laypeople land here with
            // no idea what Ollama even is, so this has to be the one obvious
            // thing to click, not a link that assumes they know to look for it.
            Item {
                visible: !Nikita.ollamaInstalled
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 12

                    // The same big pill button "UP TO DATE"/"Install"/"Update"
                    // use elsewhere (HomeOverlay's updateButton) -- this is the
                    // one call-to-action on the whole screen, so it reads as
                    // one of THIS app's buttons, not a generic Qt one dropped in.
                    MainButton {
                        Layout.alignment: Qt.AlignHCenter
                        // Wider than the 280 default, at the same full-size
                        // font ("UP TO DATE" uses) -- "Install Ollama" is a
                        // longer string, not a reason to shrink the text.
                        width: 440
                        height: 72
                        enabled: Nikita.modelOpKind !== "ollama"
                        text: Nikita.modelOpKind === "ollama" ? qsTr("Installing…") : qsTr("Install Ollama")
                        onClicked: Nikita.installOllama()
                    }

                    Text {
                        visible: Nikita.modelOpKind === "ollama"
                        Layout.alignment: Qt.AlignHCenter
                        Layout.maximumWidth: 320
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: Nikita.modelOpStatus
                        color: Theme.color.mediumorange1
                        font.family: "Share Tech Mono"; font.pixelSize: 11
                    }

                    // Same "cancel" link/behavior as a model row mid-download --
                    // the button turning into "Installing..." shouldn't also
                    // mean committed-to-waiting; kill()ing partway through a
                    // curl/hdiutil/cp sequence leaves nothing worse behind than
                    // a partial temp download, which cleanup() already removes.
                    Text {
                        visible: Nikita.modelOpKind === "ollama"
                        Layout.alignment: Qt.AlignHCenter
                        text: "cancel"
                        color: cancelOllamaInstallMouse.containsMouse ? "#ff6a6a" : Theme.color.mediumorange1
                        font.family: "Share Tech Mono"; font.pixelSize: 11; font.bold: true
                        MouseArea {
                            id: cancelOllamaInstallMouse
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Nikita.cancelModelOp()
                        }
                    }
                }
            }

            // ---- catalog list ----
            ListView {
                id: catalogView
                visible: Nikita.ollamaInstalled
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: catalogModel
                spacing: 6

                delegate: Rectangle {
                    width: catalogView.width
                    height: rowCol.implicitHeight + 16
                    radius: 5
                    color: model.active ? "#1a0d24" : "#120818"
                    border.width: 1
                    border.color: model.active ? Theme.color.lightorange2 : Theme.color.mediumorange2

                    ColumnLayout {
                        id: rowCol
                        x: 10; y: 8
                        width: parent.width - 20
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Text {
                                text: model.label + "  ·  " + model.size
                                color: "#eaffea"
                                font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                                Layout.fillWidth: true
                            }
                            Text {
                                visible: model.active
                                text: "active"
                                color: "#39ff14"
                                font.family: "Share Tech Mono"; font.pixelSize: 10
                            }
                            Rectangle {
                                visible: model.installed && !model.active
                                Layout.preferredWidth: equipLabel.implicitWidth + 14
                                Layout.preferredHeight: 16
                                radius: 3
                                color: equipMouse.containsMouse ? Theme.color.lightorange2 : "transparent"
                                border.width: 1
                                border.color: Theme.color.lightorange2
                                Text {
                                    id: equipLabel
                                    anchors.centerIn: parent
                                    text: "EQUIP"
                                    color: equipMouse.containsMouse ? "#0b0410" : Theme.color.lightorange2
                                    font.family: "Share Tech Mono"; font.pixelSize: 10; font.bold: true
                                }
                                MouseArea {
                                    id: equipMouse
                                    anchors.fill: parent
                                    anchors.margins: -4
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    enabled: Nikita.modelOpKind === ""
                                    onClicked: Nikita.setModel(model.tag)
                                }
                            }
                        }

                        Text {
                            text: model.blurb
                            color: Theme.color.mediumorange1
                            font.family: "Share Tech Mono"; font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        // Busy row: progress bar + status line instead of the buttons.
                        ColumnLayout {
                            visible: model.busy
                            Layout.fillWidth: true
                            spacing: 4

                            // Deliberately NOT a ProgressBar. The DefaultAmber
                            // style's ProgressBar is built for the full-screen
                            // firmware update: its contentItem centres a 48px
                            // "HaxrCorp 4089" percentage label, and that label is
                            // `Math.round(control.value) + "%"`, i.e. it assumes
                            // value runs 0..100, while ProgressBar's default range
                            // (and modelOpProgress) is 0..1. Two consequences, both
                            // visible in the screenshots: the control's implicit
                            // height collapses to zero, so the 48px label escapes
                            // and floats unclipped over the row as that pixelated
                            // blob; and Math.round(0.63) is 1, so it reads "0%"
                            // until the download passes halfway and then "1%" for
                            // the entire rest of it. That is the stuck 1%.
                            Item {
                                id: barTrack
                                Layout.fillWidth: true
                                Layout.preferredHeight: 6
                                clip: true

                                Rectangle {
                                    anchors.fill: parent
                                    radius: 3
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Theme.color.mediumorange2
                                }

                                // Determinate fill.
                                Rectangle {
                                    visible: Nikita.modelOpProgress >= 0
                                    x: 1; y: 1
                                    height: parent.height - 2
                                    radius: 2
                                    color: Theme.color.lightorange2
                                    width: Math.max(0, (barTrack.width - 2) * Math.min(1, Nikita.modelOpProgress))
                                    Behavior on width { NumberAnimation { duration: 150 } }
                                }

                                // Indeterminate sweep, for the stages that report
                                // no percentage ("pulling manifest", "verifying").
                                Rectangle {
                                    id: barSweep
                                    visible: Nikita.modelOpProgress < 0
                                    y: 1
                                    height: parent.height - 2
                                    width: Math.max(12, barTrack.width * 0.25)
                                    radius: 2
                                    color: Theme.color.mediumorange1
                                    SequentialAnimation on x {
                                        running: barSweep.visible && modelManager.visible
                                        loops: Animation.Infinite
                                        NumberAnimation {
                                            from: 1; to: Math.max(1, barTrack.width - barSweep.width - 1)
                                            duration: 850; easing.type: Easing.InOutQuad
                                        }
                                        NumberAnimation {
                                            from: Math.max(1, barTrack.width - barSweep.width - 1); to: 1
                                            duration: 850; easing.type: Easing.InOutQuad
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text {
                                    text: Nikita.modelOpStatus
                                    color: Theme.color.mediumorange1
                                    font.family: "Share Tech Mono"; font.pixelSize: 9
                                    elide: Text.ElideRight
                                    maximumLineCount: 1
                                    Layout.fillWidth: true
                                }
                                Text {
                                    visible: Nikita.modelOpProgress >= 0
                                    text: Math.round(Nikita.modelOpProgress * 100) + "%"
                                    color: Theme.color.lightorange2
                                    font.family: "Share Tech Mono"; font.pixelSize: 9; font.bold: true
                                }
                                Text {
                                    text: "cancel"
                                    color: cancelOpMouse.containsMouse ? "#ff6a6a" : Theme.color.mediumorange1
                                    font.family: "Share Tech Mono"; font.pixelSize: 9; font.bold: true
                                    MouseArea {
                                        id: cancelOpMouse
                                        anchors.fill: parent
                                        anchors.margins: -4
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: Nikita.cancelModelOp()
                                    }
                                }
                            }
                        }

                        // Idle row: status pill + action button.
                        RowLayout {
                            visible: !model.busy
                            Layout.fillWidth: true
                            spacing: 8

                            Rectangle {
                                Layout.preferredWidth: installedPill.implicitWidth + 12
                                Layout.preferredHeight: 18
                                radius: 3
                                color: model.installed ? "#0f3d1f" : "transparent"
                                border.width: model.installed ? 0 : 1
                                border.color: Theme.color.mediumorange2
                                Text {
                                    id: installedPill
                                    anchors.centerIn: parent
                                    text: model.installed ? "installed" : "not installed"
                                    color: model.installed ? "#39ff14" : Theme.color.mediumorange1
                                    font.family: "Share Tech Mono"; font.pixelSize: 10
                                }
                            }

                            Item { Layout.fillWidth: true }

                            Text {
                                visible: !model.installed
                                text: "install"
                                color: installMouse.containsMouse ? Theme.color.lightgreen : Theme.color.lightorange2
                                font.family: "Share Tech Mono"; font.pixelSize: 11; font.bold: true
                                MouseArea {
                                    id: installMouse
                                    anchors.fill: parent
                                    anchors.margins: -4
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    enabled: Nikita.modelOpKind === ""
                                    onClicked: Nikita.installModel(model.tag)
                                }
                            }
                            Text {
                                visible: model.installed
                                text: "uninstall"
                                color: uninstallMouse.containsMouse ? "#ff6a6a" : Theme.color.mediumorange1
                                font.family: "Share Tech Mono"; font.pixelSize: 11
                                MouseArea {
                                    id: uninstallMouse
                                    anchors.fill: parent
                                    anchors.margins: -4
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    enabled: Nikita.modelOpKind === ""
                                    onClicked: Nikita.uninstallModel(model.tag)
                                }
                            }
                        }
                    }
                }
            }

            // Result line for the op that just finished. Until now the only
            // places modelOpStatus was shown were bound to an op being *in
            // flight* (modelOpKind === "ollama", or a row's busy state), so a
            // fast failure (start, fail, finish, kind back to "") left no
            // trace anywhere in the UI and read as "the button does nothing".
            Text {
                visible: modelOpTracker.fadingStatus.length > 0
                text: modelOpTracker.fadingStatus
                color: Theme.color.mediumorange1
                font.family: "Share Tech Mono"; font.pixelSize: 10
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }
    }
}
