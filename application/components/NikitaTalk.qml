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
            // Dragging a selection past the edge moves the view without any
            // Flickable movement signal, so the follow flag has to be updated
            // by hand here or the next incoming token snaps the selection away.
            listView.stickToBottom = (listView.contentY + listView.height) >= (listView.contentHeight - 24);
        }
    }

    // Header label. "no model" read like the name of a model; this says what it
    // actually means, and every layer of the chromatic-aberration stack reads
    // the same property so they can never drift apart.
    // A key that has not been accepted by the API is not a working assistant, so
    // the badge does not get to claim one. It showed the model name from the
    // moment the panel opened -- green dot, model pill, ready-looking -- with
    // no key set at all, and the first message was then refused.
    readonly property bool hasModel: Nikita.apiKeyValid && Nikita.modelName.length > 0
    readonly property string modelLabel: root.hasModel ? Nikita.modelName
                                       : (Nikita.apiKeyStatus === "checking" ? "checking key…"
                                       : (Nikita.apiKeyStatus === "invalid"  ? "key rejected"
                                       : (Nikita.apiKeyPresent ? "key not verified" : "No Model Selected")))

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

    // seq -> index in chatModel, for the tool rows. Never cleared: the ids come
    // from a counter that only goes up, so a stale entry can never collide with
    // a live one, and clearing it would strand rows from an earlier turn.
    property var toolRows: ({})

    // Has the current assistant turn already printed its "Nikita" header? The
    // first assistant-side row of a turn -- whether that is a tool line or the
    // reply text -- carries the label; everything after it in the same turn does
    // not, so a turn that runs three tools and then answers shows ONE "Nikita:"
    // above the lot, the way Claude Code groups a turn. Reset when the user
    // speaks. Without this the tool lines hung under the "you" block with no
    // header, reading as if the user had typed them.
    property bool turnLabelShown: false

    // Records who owns a new row and whether it should carry the name label.
    // Every append goes through here so the rule lives in one place.
    function pushRow(role, text, toolDone, toolFailed) {
        var isUser = (role === "you");
        if (isUser) { root.turnLabelShown = false; }
        var show = isUser ? true : !root.turnLabelShown;
        if (!isUser) { root.turnLabelShown = true; }
        chatModel.append({ "role": role, "text": text,
                           "toolDone": toolDone === true, "toolFailed": toolFailed === true,
                           "toolDetail": "", "toolExpanded": false,
                           "showName": show });
        return chatModel.count - 1;
    }

    function appendMessage(role, text) {
        pushRow(role, text, false, false);
        listView.followEnd();
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

    // Erase, and the "clear" command, throw the conversation away in the
    // backend. This is the panel doing the same to its own copy: without it the
    // bubbles stayed on screen and came straight back the next time the
    // assistant was switched on, so an erase only held until the app restarted.
    Connections {
        target: Nikita
        // A queued message just started its turn -- show it as a "you" bubble
        // now, in step with the turn, so queued questions read like normal ones
        // instead of appearing only in the "queued (N)" strip and then vanishing.
        function onQueuedMessageStarting(text) {
            root.appendMessage("you", text);
        }
        function onHistoryCleared() {
            chatModel.clear();
            root.streamIdx = -1;
            root.turnLabelShown = false;
            root.clearSelection();
            root.greetIfReady();
        }
        // Switched back on after an erase: the log is empty and the greeting
        // never ran, because it only fires once when the panel is created.
        function onAssistantEnabledChanged() {
            if (Nikita.assistantEnabled) {
                root.greetIfReady();
            } else {
                // Switched OFF from the maximised view, the panel kept the
                // maximised geometry -- so the "ENABLE NIKITA" face, which just
                // fills the panel, stretched across the whole window and sat on
                // top of the device screen. It looked like a broken layout and
                // only came right after a restart, because viewState is not
                // persisted. There is nothing to be big for once it is off.
                root.viewState = "normal";
            }
        }
        // And the moment a key is accepted. The greeting only fired when the
        // panel was built, so pasting a key left the log blank until the app was
        // restarted -- the assistant came online without a word.
        function onApiKeyChanged() {
            if (Nikita.apiKeyValid) { root.greetIfReady(); }
        }
    }

    // parent: root centered these on NikitaTalk's own narrow strip instead of
    // the screen -- a CustomDialog is a Popup sized to fill and center in
    // whatever it's parented to, and root is the chat panel, not the window.
    // Same fix modelManager already uses below: root.parent is the overlay
    // NikitaTalk sits inside, which fills mainContent -- the actual content
    // area of the window -- so centering there reads as centered on screen.
    ComputerRunConfirmDialog {
        id: hostRunConfirmDialog
        radius: root.radius
        parent: root.parent ? root.parent : root
    }

    ComputerActionConfirmDialog {
        id: hostActionConfirmDialog
        radius: root.radius
        parent: root.parent ? root.parent : root
    }

    SaveConflictDialog {
        id: saveConflictDialog
        radius: root.radius
        parent: root.parent ? root.parent : root
    }

    Connections {
        target: Nikita
        // computer_run is waiting on screen: nothing runs on this computer until
        // the command is answered here.
        function onHostRunConfirmRequested(command, cwd) {
            hostRunConfirmDialog.openWithCommand(function(allow, always) {
                Nikita.answerHostRunConfirm(allow, always);
            }, command, cwd);
        }
        // Same idea for computer_write/computer_mkdir/computer_move/computer_copy/computer_delete:
        // nothing touches a file on this computer until it's answered here.
        function onHostActionConfirmRequested(kind, summary, detail) {
            hostActionConfirmDialog.openWithAction(function(allow, always) {
                Nikita.answerHostActionConfirm(allow, always);
            }, kind, summary, detail);
        }
        // A Flipper save_file would overwrite an existing file: ask first.
        function onSaveConflictRequested(path, preview) {
            saveConflictDialog.openWithConflict(function(action, newName) {
                Nikita.answerSaveConflict(action, newName);
            }, path, preview);
        }
        // The turn status ticks once a second (turnStatusChanged). While a turn
        // is running and the user is parked at the bottom, ride that tick to keep
        // the live footer -- the pulsing dot, the climbing seconds -- in view.
        // Without this the footer scrolls off during the long "thinking" stretch
        // between tool lines, and a working turn looks stopped.
        function onTurnStatusChanged() {
            if (Nikita.thinking && listView.stickToBottom) {
                Qt.callLater(listView.followEnd);
            }
        }
        // Snap to the bottom the instant a turn starts, so the footer is on
        // screen from the first second rather than only after the first scroll.
        function onThinkingChanged() {
            if (Nikita.thinking) {
                listView.stickToBottom = true;
                Qt.callLater(listView.followEnd);
            }
        }
        // One row per tool call, rewritten in place when it answers.
        //
        // Keyed by the backend's seq rather than by "the last row", because a
        // turn can have several calls in flight in one batch and the one that
        // answers first is not necessarily the one that started last.
        function onToolActivity(seq, text, detail, finished, failed) {
            var idx = root.toolRows[seq];
            if (idx === undefined) {
                // A tool line interrupts the streaming bubble: whatever prose
                // was being typed is finished where it stands, and the model's
                // next words open a new one underneath. That is what makes the
                // transcript read in the order things actually happened.
                root.streamIdx = -1;
                idx = root.pushRow("tool", text, finished, failed);
                root.toolRows[seq] = idx;
            } else {
                chatModel.setProperty(idx, "text", text);
                chatModel.setProperty(idx, "toolDone", finished);
                chatModel.setProperty(idx, "toolFailed", failed);
            }
            // The exact call, kept on the row so it can be expanded to show what
            // actually ran. Set every update so the finished detail (with any
            // late-filled args) wins.
            chatModel.setProperty(idx, "toolDetail", detail);
            listView.followEnd();
        }
        // live typing: grow one bubble as tokens arrive
        function onPartialReceived(text) {
            if(root.streamIdx < 0) {
                root.streamIdx = root.pushRow("nikita", text, false, false);
            } else {
                chatModel.setProperty(root.streamIdx, "text", text);
            }
            listView.followEnd();
        }
        function onReplyReceived(text) {
            if(root.streamIdx >= 0) {
                chatModel.setProperty(root.streamIdx, "text", text);  // finalize
                root.streamIdx = -1;
            } else {
                root.appendMessage("nikita", text);
            }
            listView.followEnd();
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
            listView.followEnd();
        }
        function onScriptSaveError(message) {
            root.appendMessage("nikita", "⚠️ Couldn't save: " + message);
            listView.followEnd();
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

            // ---- master switch, small: turns the whole assistant off --------
            // Sits next to the gear because that is where the panel's own
            // controls live. Off replaces everything below with the big
            // ENABLE NIKITA control.
            Rectangle {
                id: powerSwitch
                Layout.preferredWidth: 30
                Layout.preferredHeight: 16
                Layout.alignment: Qt.AlignVCenter
                radius: 8
                color: "transparent"
                border.width: 1
                border.color: powerMouse.containsMouse ? Theme.color.lightorange2
                                                       : Theme.color.mediumorange2
                Rectangle {
                    // Knob to the right while on, left while off -- the state is
                    // readable without a label, which the header has no room for.
                    width: 10; height: 10; radius: 5
                    y: 2
                    x: Nikita.assistantEnabled ? parent.width - width - 3 : 3
                    // App palette, not green: green was borrowed from the status
                    // dot, and its off state (mediumorange2, #6c3c6d) is so dark
                    // on this background that the knob simply vanished.
                    color: Nikita.assistantEnabled ? Theme.color.lightorange2
                                                   : Theme.color.mediumorange1
                    Behavior on x { NumberAnimation { duration: 120 } }
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                MouseArea {
                    id: powerMouse
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: consent.ask(!Nikita.assistantEnabled)
                }
            }

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

            // Keep the newest line in view as the answer grows -- but only when
            // the user was already at the bottom. Yanking the view down while
            // someone is reading back through the conversation is worse than
            // not following at all.
            property bool stickToBottom: true

            // The one place the view is allowed to jump to the end. Everything
            // that used to call positionViewAtEnd() directly now goes through
            // here, because half of those calls ignored stickToBottom -- so a
            // streaming answer hauled the view back down on every token while
            // the user was trying to scroll up, and the fight between the two
            // read as the list shaking up and down.
            function followEnd() {
                if (!stickToBottom) { return; }
                // positionViewAtEnd is the ONLY correct way to pin a ListView to
                // the bottom -- it respects delegate virtualization. Setting
                // contentY by hand strands the view at a position where the
                // ListView has not created delegates, so messages blank out and
                // flicker back in as you scroll. The debounce timer below keeps
                // this from firing every streamed token, which is what caused
                // the twitch that tempted the hand-rolled version in the first
                // place.
                positionViewAtEnd();
            }

            // Decided from what the USER does, not from every contentY change.
            // contentY moves for layout reasons too -- a delegate growing as
            // text streams in, the footer appearing -- and reading "the user
            // scrolled" out of those was how the flag flickered.
            onMovementStarted: stickToBottom = false
            onMovementEnded: stickToBottom = (contentY + height) >= (contentHeight - 24)
            onFlickEnded: stickToBottom = (contentY + height) >= (contentHeight - 24)

            // Debounced rather than per-change: a streaming answer resizes its
            // delegate on every token, and one correction after the burst is
            // both smoother and cheaper than one per frame.
            Timer {
                id: followTimer
                interval: 40
                onTriggered: listView.followEnd()
            }
            onCountChanged: followTimer.restart()
            onContentHeightChanged: followTimer.restart()

            // ---- live turn status, as the list's footer ------------------
            // Directly under the last message, flowing with the conversation --
            // not pinned to the bottom edge of the panel. It sits where the next
            // line of the transcript would go, which is where the eye already
            // is while an answer streams in. The stickToBottom auto-scroll above
            // keeps it in view on a long turn, so it no longer drifts off the
            // top the way the old footer did.
            //
            // The phrase is whatever the backend is doing this second (writing
            // the file, running the command, wrapping up); the seconds keep
            // moving even when the phrase does not, so a slow turn still reads
            // as working rather than as hung.
            footer: Item {
                width: ListView.view ? ListView.view.width : 0
                height: Nikita.thinking ? 24 : 0
                visible: Nikita.thinking
                Row {
                    x: 0
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 0
                    Text {
                        id: liveDot
                        text: "\u25cf  "
                        color: Theme.color.lightorange2
                        font.family: "Share Tech Mono"; font.pixelSize: 12
                        SequentialAnimation on opacity {
                            running: Nikita.thinking
                            loops: Animation.Infinite
                            NumberAnimation { from: 1.0; to: 0.25; duration: 700 }
                            NumberAnimation { from: 0.25; to: 1.0; duration: 700 }
                        }
                    }
                    // elapsed · tokens · cost · what it is doing
                    Text {
                        text: Nikita.turnElapsedText
                        color: Theme.color.mediumorange2
                        font.family: "Share Tech Mono"; font.pixelSize: 12
                    }
                    Text {
                        visible: Nikita.turnTokensText.length > 0
                        text: " \u00b7 " + Nikita.turnTokensText
                        color: Theme.color.mediumorange2
                        font.family: "Share Tech Mono"; font.pixelSize: 12
                    }
                    Text {
                        visible: Nikita.turnCostText.length > 0
                        text: " \u00b7 " + Nikita.turnCostText
                        color: Theme.color.mediumorange2
                        font.family: "Share Tech Mono"; font.pixelSize: 12
                    }
                    Text {
                        text: " \u00b7 " + (Nikita.turnStatus.length > 0
                                            ? Nikita.turnStatus : "thinking")
                        color: Theme.color.mediumorange4
                        font.family: "Share Tech Mono"; font.pixelSize: 12
                    }
                    Text {
                        id: thinkDots
                        color: Theme.color.mediumorange4
                        font.family: "Share Tech Mono"; font.pixelSize: 12
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

                // The turn's "Nikita" header, when the first thing in the turn
                // is a tool line rather than prose. Without this the tool lines
                // sit directly under the user's message and read as the user's.
                Text {
                    visible: model.role === "tool" && model.showName === true
                    text: root.aiName
                    color: Theme.color.lightorange2
                    font.family: "Share Tech Mono"
                    font.pixelSize: 11
                    bottomPadding: 2
                }

                // ---- a tool call, as its own line ------------------------
                // Not a message: no role label, no bubble, no markdown. One
                // dim line that says what is happening and then what happened,
                // rewritten in place, so the transcript reads in the order the
                // work actually occurred instead of as a summary written after
                // the fact.
                Item {
                    visible: model.role === "tool"
                    width: msgCol.width
                    // Item (not Column) so the MouseArea can anchors.fill it --
                    // a Column refuses anchored children and silently breaks.
                    implicitHeight: toolCol.implicitHeight
                    height: model.role === "tool" ? implicitHeight : 0
                    Column {
                        id: toolCol
                        width: parent.width
                        spacing: 1
                    Row {
                        spacing: 6
                        // Pulses while the call is in flight and stops when it
                        // lands, so a slow tool is visibly alive rather than stuck.
                        Text {
                            text: model.toolFailed ? "\u2717" : (model.toolDone ? "\u00b7" : "\u25cf")
                            color: model.toolFailed ? "#ff6a6a" : Theme.color.mediumorange4
                            font.family: "Share Tech Mono"
                            font.pixelSize: 12
                            SequentialAnimation on opacity {
                                running: model.role === "tool" && !model.toolDone
                                loops: Animation.Infinite
                                NumberAnimation { from: 1.0; to: 0.25; duration: 700 }
                                NumberAnimation { from: 0.25; to: 1.0; duration: 700 }
                            }
                        }
                        Text {
                            text: model.text
                            color: model.toolFailed ? "#ff6a6a" : Theme.color.mediumorange1
                            font.family: "Share Tech Mono"
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            width: Math.min(implicitWidth, msgCol.width - 60)
                        }
                        // The chevron turns to point down when expanded; the
                        // whole row is the click target. Only shown when there
                        // is a detail to reveal.
                        Text {
                            visible: (model.toolDetail || "").length > 0
                            text: model.toolExpanded ? "\u2304" : "\u203a"
                            color: toolRowHover.containsMouse ? Theme.color.lightorange2
                                                             : Theme.color.mediumorange4
                            font.family: "Share Tech Mono"
                            font.pixelSize: 12
                        }
                    }
                    // The exact call, revealed on click. Monospace, dim, indented
                    // under the line it belongs to.
                    Text {
                        visible: model.toolExpanded && (model.toolDetail || "").length > 0
                        text: "    " + (model.toolDetail || "")
                        color: model.toolFailed ? "#ff6a6a" : Theme.color.mediumorange2
                        font.family: "Share Tech Mono"
                        font.pixelSize: 11
                        width: msgCol.width
                        wrapMode: Text.WrapAnywhere
                    }
                    }   // end inner Column
                    MouseArea {
                        id: toolRowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: (model.toolDetail || "").length > 0
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: chatModel.setProperty(index, "toolExpanded", !model.toolExpanded)
                    }
                }

                Row {
                    visible: model.role !== "tool"
                    spacing: 8
                    Rectangle {
                        // Hidden (and zero-width, so the Row skips it) on a row
                        // that continues a turn whose header already showed --
                        // e.g. the reply text after a run of tool lines.
                        visible: model.showName === true
                        width: visible ? roleLabel.implicitWidth + 4 : 0
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
                        // opacity, not visible, and a FIXED width. Toggling
                        // visible on hover added/removed a row child, which
                        // reflowed the delegate and changed the ListView's
                        // contentHeight -- and a contentHeight change nudges the
                        // scroll position, so every hover made the chat twitch.
                        // Kept permanently laid out at a fixed size, it fades in
                        // and out without touching layout, and "copy"->"copied"
                        // (wider) no longer changes the row width either.
                        width: 44
                        horizontalAlignment: Text.AlignLeft
                        opacity: (msgHover.hovered || done) ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 90 } }
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
                    visible: model.role !== "tool"
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

        // ---- queued messages -------------------------------------------------
        // What was typed while the model was busy. Shown so it is obvious the
        // text was kept rather than swallowed, and cancellable in one click.
        RowLayout {
            visible: Nikita.queuedCount > 0 && root.viewState !== "min"
            Layout.fillWidth: true
            spacing: 6
            Text {
                text: "\u21b3 queued (" + Nikita.queuedCount + "): " + Nikita.queuedPreview
                color: Theme.color.mediumorange1
                font.family: "Share Tech Mono"; font.pixelSize: 11
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Text {
                text: "cancel"
                color: qCancelMouse.containsMouse ? Theme.color.lightorange2
                                                  : Theme.color.mediumorange2
                font.family: "Share Tech Mono"; font.pixelSize: 11
                MouseArea {
                    id: qCancelMouse
                    anchors.fill: parent; anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Nikita.clearQueue()
                }
            }
        }

        // ---- feedback strip -------------------------------------------------
        // Appears after an action, never before: by the time this is on screen
        // the turn has closed and the lesson is already filed. Answering adds a
        // second opinion; ignoring it, clearing the chat or quitting leaves the
        // automatic verdict standing. Silence is not a verdict.
        ColumnLayout {
            visible: Nikita.canRate && root.viewState !== "min"
            Layout.fillWidth: true
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Text {
                    text: "Did that do what you asked?"
                    color: Theme.color.mediumorange1
                    font.family: "Share Tech Mono"; font.pixelSize: 11
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.preferredWidth: yesTxt.implicitWidth + 20
                    Layout.preferredHeight: 22
                    radius: 3
                    color: yesMouse.containsMouse ? Theme.color.lightorange2 : "transparent"
                    border.width: 1
                    border.color: Theme.color.mediumorange2
                    Text {
                        id: yesTxt
                        anchors.centerIn: parent
                        text: "YES"
                        color: yesMouse.containsMouse ? "#0b0410" : Theme.color.lightorange2
                        font.family: "Share Tech Mono"; font.pixelSize: 11; font.bold: true
                    }
                    MouseArea {
                        id: yesMouse
                        anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { Nikita.rateLastAction(true, note.text); note.text = ""; }
                    }
                }

                Rectangle {
                    Layout.preferredWidth: noTxt.implicitWidth + 20
                    Layout.preferredHeight: 22
                    radius: 3
                    color: noMouse.containsMouse ? Theme.color.lightorange2 : "transparent"
                    border.width: 1
                    border.color: Theme.color.mediumorange2
                    Text {
                        id: noTxt
                        anchors.centerIn: parent
                        text: "NO"
                        color: noMouse.containsMouse ? "#0b0410" : Theme.color.lightorange2
                        font.family: "Share Tech Mono"; font.pixelSize: 11; font.bold: true
                    }
                    MouseArea {
                        id: noMouse
                        anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { Nikita.rateLastAction(false, note.text); note.text = ""; }
                    }
                }
            }

            // Optional, and the most useful half when it is filled in: the tool
            // reported success and the file exists, so only the person who asked
            // can say the request was not met.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                radius: 4
                color: "black"
                border.width: 1
                border.color: Theme.color.mediumorange2
                TextInput {
                    id: note
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: TextInput.AlignVCenter
                    color: "#eaffea"
                    font.family: "Share Tech Mono"; font.pixelSize: 11
                    clip: true
                    maximumLength: 200
                    onAccepted: { Nikita.rateLastAction(false, note.text); note.text = ""; }
                    Text {
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        visible: note.text.length === 0 && !note.activeFocus
                        text: "what went wrong? (optional)"
                        color: Theme.color.mediumorange2
                        font.family: "Share Tech Mono"; font.pixelSize: 11
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
                    // Stays editable while a turn runs. On a model that takes
                    // minutes, locking the field means losing the thought.
                    enabled: root.hasModel
                    onAccepted: {
                        if (Nikita.thinking) {
                            Nikita.queueMessage(input.text);
                            input.text = "";
                        } else {
                            root.sendCurrent();
                        }
                    }

                    Text {
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        visible: input.text.length === 0 && !input.activeFocus
                        // The same invitation whether or not a key is set up.
                        // What is wrong when there isn't one is said by the badge
                        // above and spelled out in SETUP; this box only has room
                        // to name what it is for, and a sentence of instructions
                        // here was clipped mid-word.
                        text: "Talk to " + root.aiName + "…"
                        color: Theme.color.mediumorange1
                        font: input.font
                    }
                }
            }

            // While a turn runs the primary control is STOP, so an interrupt is
            // always one click away. SEND only appears once something is typed:
            // outside a turn it sends normally, and DURING a turn it drops the
            // text into the queue to run after this one. So a running turn shows
            // just STOP until the user starts typing, then STOP + SEND together.
            // Drawn by hand rather than left to the themed Button, which was
            // not reliably showing up mid-turn -- and a STOP you cannot find
            // while something is running is the same as no STOP at all. Same
            // colours as every other control in this panel: it is the ordinary
            // way to end a turn, not an alarm.
            Rectangle {
                visible: Nikita.thinking
                Layout.preferredWidth: stopTxt.implicitWidth + 24
                Layout.preferredHeight: 30
                radius: 6
                color: stopMouse.containsMouse ? Theme.color.mediumorange2 : "transparent"
                border.width: 1
                border.color: Theme.color.mediumorange2
                Text {
                    id: stopTxt
                    anchors.centerIn: parent
                    text: "STOP"
                    color: Theme.color.lightorange2
                    font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                }
                MouseArea {
                    id: stopMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Nikita.stopThinking()
                }
            }
            Button {
                text: "Send"
                // Outside a turn: the normal send button. During a turn: only
                // once there is something to queue.
                visible: !Nikita.thinking || input.text.length > 0
                enabled: root.hasModel && input.text.length > 0
                onClicked: {
                    if (!Nikita.thinking) {
                        root.sendCurrent();
                    } else {
                        Nikita.queueMessage(input.text);
                        input.text = "";
                    }
                }
            }
        }
    }

    // No empty state for the log. It went from three lines to one to none: the
    // heading repeated the header a few pixels above it, the link repeated the
    // gear beside it, and the last survivor -- "No messages yet." -- told
    // someone looking at an empty log the one thing they could already see.
    // With a key in place the greeting arrives on its own and the log is not
    // empty for long.

    // ---- Model manager panel (gear icon) ----------------------------------
    // Not a Popup any more. A modal Popup brought a style-drawn dim with it and
    // read as something layered on top of the app; the CLI panel next door is
    // just an Item filling the same content box, and that is what the rest of
    // this UI looks like. So this is the same shape: parented to the overlay
    // NikitaTalk itself sits in (which fills mainContent, exactly the area the
    // CLI covers), toggled by `open`, no dim behind it.
    // ---- disabled state: the whole panel becomes one control ----------------
    // Not a dimmed overlay on top of a visible chat: when NIKITA is off there
    // is nothing to look at and nothing to read, so the panel shows the single
    // thing that can be done with it. z above the model manager, because a
    // gear panel left open must not survive the assistant being switched off.
    Rectangle {
        id: disabledFace
        anchors.fill: parent
        z: 300
        visible: !Nikita.assistantEnabled
        color: root.color
        radius: root.radius

        Column {
            anchors.centerIn: parent
            spacing: 18

            // The big switch. Same shape as the small one in the header, scaled
            // up -- one visual idea, so nobody has to learn a second control.
            Rectangle {
                id: bigSwitch
                width: 96; height: 44; radius: 22
                anchors.horizontalCenter: parent.horizontalCenter
                color: "transparent"
                border.width: 2
                border.color: bigMouse.containsMouse ? Theme.color.lightorange2
                                                     : Theme.color.mediumorange2

                Rectangle {
                    width: 32; height: 32; radius: 16
                    y: 6
                    x: Nikita.assistantEnabled ? parent.width - width - 6 : 6
                    color: Nikita.assistantEnabled ? Theme.color.lightorange2
                                                   : Theme.color.mediumorange1
                    Behavior on x { NumberAnimation { duration: 160 } }
                    Behavior on color { ColorAnimation { duration: 160 } }
                }

                // A slow pulse on the border, so an off panel still reads as a
                // thing that responds rather than a dead rectangle.
                SequentialAnimation on opacity {
                    running: disabledFace.visible
                    loops: Animation.Infinite
                    NumberAnimation { from: 1.0; to: 0.55; duration: 1100 }
                    NumberAnimation { from: 0.55; to: 1.0; duration: 1100 }
                }

                MouseArea {
                    id: bigMouse
                    anchors.fill: parent
                    anchors.margins: -8
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: consent.ask(true)
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "ENABLE NIKITA"
                color: Theme.color.lightorange2
                font.family: "Share Tech Mono"
                font.pixelSize: 15
                font.bold: true
            }
        }
    }

    // ---- consent gate -------------------------------------------------------
    // Neither direction of this switch is a preference toggle. Turning it on
    // starts something that reads files and keeps notes about the person;
    // turning it off destroys those notes. Both deserve a sentence and a
    // choice, so neither happens by a misclick on a 30x16 control.
    Rectangle {
        id: consent
        anchors.fill: parent
        z: 400
        visible: pending !== 0
        color: Qt.rgba(0, 0, 0, 0.86)

        // 0 = closed, 1 = about to enable, 2 = about to disable
        property int pending: 0
        // Compact by default, expandable: the sheet lives inside a panel that
        // can be 300px tall, so the full text does not fit and the body has to
        // scroll. The square button gives the whole message in one look --
        // scrolling to find out what gets erased is the wrong way to read a
        // consent notice.
        // Expanding the sheet alone did nothing: it already filled the panel, so
        // both branches of its height resolved to the same number. What has to
        // grow is the PANEL -- so this drives the chat's own maximize, and puts
        // it back the way it was when the dialog closes.
        property bool expanded: false
        property string viewBefore: ""
        onExpandedChanged: {
            if (expanded) {
                if (viewBefore === "") { viewBefore = root.viewState; }
                root.viewState = "max";
            } else if (viewBefore !== "") {
                root.viewState = viewBefore;
                viewBefore = "";
            }
        }
        readonly property bool turningOn: pending === 1
        function ask(on) { pending = on ? 1 : 2; expanded = false }
        onPendingChanged: if (pending === 0) { expanded = false }

        MouseArea { anchors.fill: parent; hoverEnabled: true }

        // The sheet is sized to the PANEL, not to its own text: this lives
        // inside the chat panel, which can be as short as ~300px, and a sheet
        // that grows past it puts its own buttons off-screen. So the body
        // scrolls and the buttons are pinned.
        Rectangle {
            id: sheet
            width: consent.expanded ? parent.width - 16
                                    : Math.min(parent.width - 24, 380)
            Behavior on width { NumberAnimation { duration: 130 } }
            // A constant preferred height, clamped to the panel -- NOT derived
            // from the body's contentHeight. That was a binding loop: the sheet
            // sized itself from the body while the body sized itself from the
            // sheet, Qt cut the cycle, and the bullet list came out at zero
            // height. The body scrolls instead.
            height: consent.expanded ? parent.height - 16
                                     : Math.min(parent.height - 16, 300)
            Behavior on height { NumberAnimation { duration: 130 } }
            anchors.centerIn: parent
            radius: 6
            color: "#120818"
            border.width: 1
            border.color: Theme.color.mediumorange2

            // Same square as the chat panel's maximize, same behaviour: filled
            // smaller when already expanded.
            Rectangle {
                id: expandBtn
                width: 26; height: 20; radius: 3
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.top: parent.top
                anchors.topMargin: 9
                color: expandMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.14) : "transparent"
                Rectangle {
                    width: consent.expanded ? 8 : 11
                    height: width
                    color: "transparent"
                    border.width: 2
                    border.color: Theme.color.lightorange2
                    anchors.centerIn: parent
                    Behavior on width { NumberAnimation { duration: 120 } }
                }
                MouseArea {
                    id: expandMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: consent.expanded = !consent.expanded
                }
            }

            Text {
                id: headerTxt
                x: 14; y: 12
                width: parent.width - 28 - expandBtn.width
                text: consent.turningOn ? "ENABLE NIKITA?" : "DISABLE NIKITA?"
                color: Theme.color.lightorange2
                font.family: "Share Tech Mono"; font.pixelSize: 14; font.bold: true
            }

            Flickable {
                id: bodyArea
                x: 14
                anchors.top: headerTxt.bottom
                anchors.topMargin: 8
                anchors.bottom: btnRow.top
                anchors.bottomMargin: 10
                width: parent.width - 28
                contentHeight: bodyCol.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: bodyCol
                    width: bodyArea.width
                    spacing: 5

                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: "#eaffea"
                        font.family: "Share Tech Mono"; font.pixelSize: 11
                        text: consent.turningOn
                            ? "Kimi API handles the task and everything else stays local:"
                            : "Erases everything NIKITA stored about you:"
                    }

                    Item { width: 1; height: 3 }

                    // One Text per bullet. The first version put them all in one
                    // string with hard newlines, which fought WordWrap and broke
                    // lines in the middle of a phrase on a narrow panel.
                    Repeater {
                        model: consent.turningOn
                            ? ["keeps your conversations here",
                               "saves facts you ask it to remember, here and in /ext/nikita",
                               "records which actions worked",
                               "touches files only after you allow it in ACCESS"]
                            : ["conversations and remembered facts",
                               "the actions it learned",
                               "the permissions you granted it",
                               "/ext/nikita on the SD card"]
                        Row {
                            width: bodyCol.width
                            spacing: 6
                            Text {
                                text: "\u00b7"
                                color: Theme.color.mediumorange1
                                font.family: "Share Tech Mono"; font.pixelSize: 11
                            }
                            Text {
                                width: bodyCol.width - 14
                                wrapMode: Text.WordWrap
                                text: modelData
                                color: Theme.color.mediumorange1
                                font.family: "Share Tech Mono"; font.pixelSize: 11
                            }
                        }
                    }

                    Item { width: 1; height: 4 }

                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: Theme.color.mediumorange1
                        font.family: "Share Tech Mono"; font.pixelSize: 11
                        text: consent.turningOn
                            ? "Turning it off later erases all of it."
                            : "Cannot be undone."
                    }
                }
            }

            // Pinned to the bottom of the sheet, so they are reachable no
            // matter how short the panel is.
            Row {
                id: btnRow
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                spacing: 8

                Rectangle {
                    width: cancelTxt.implicitWidth + 22; height: 24; radius: 3
                    color: cancelMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.12) : "transparent"
                    border.width: 1
                    border.color: Theme.color.mediumorange2
                    Text {
                        id: cancelTxt
                        anchors.centerIn: parent
                        text: "CANCEL"
                        color: Theme.color.mediumorange1
                        font.family: "Share Tech Mono"; font.pixelSize: 11; font.bold: true
                    }
                    MouseArea {
                        id: cancelMouse
                        anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: consent.pending = 0
                    }
                }

                Rectangle {
                    width: okTxt.implicitWidth + 22; height: 24; radius: 3
                    color: okMouse.containsMouse ? Theme.color.lightorange2 : "transparent"
                    border.width: 1
                    border.color: Theme.color.lightorange2
                    Text {
                        id: okTxt
                        anchors.centerIn: parent
                        text: consent.turningOn ? "CONFIRM" : "ERASE"
                        color: okMouse.containsMouse ? "#0b0410" : Theme.color.lightorange2
                        font.family: "Share Tech Mono"; font.pixelSize: 11; font.bold: true
                    }
                    MouseArea {
                        id: okMouse
                        anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (consent.turningOn) {
                                Nikita.assistantEnabled = true;
                            } else {
                                // Wipe BEFORE flipping: the panel is gone the
                                // instant it flips, and an error during the
                                // wipe still needs somewhere to show itself.
                                Nikita.wipeAssistantData();
                                Nikita.assistantEnabled = false;
                            }
                            consent.pending = 0;
                        }
                    }
                }
            }
        }
    }

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

        function openManager() {
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
                    text: "SETUP"
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
            // Everything below the title scrolls. The panel is a fixed-height
            // card, and the settings inside it are not: three model options, the
            // key row and then one entry per access filter add up past the
            // bottom edge on any window that isn't tall -- which left the
            // permissions, the part with real consequences, cropped and
            // unreachable. The header stays put so the title and the close
            // button never scroll away from under the pointer.
            Flickable {
                id: setupFlick
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: width
                contentHeight: setupCol.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    // Only when there is something to scroll: a permanent bar on
                    // a panel that happens to fit reads as broken chrome.
                    policy: setupFlick.contentHeight > setupFlick.height
                            ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
                }

                ColumnLayout {
                    id: setupCol
                    // Room for the scroll bar, so the widest rows are not sitting
                    // underneath it.
                    width: setupFlick.width - 12
                    spacing: 10


                    // ---- BRAIN -------------------------------------------------
                    // Which Kimi model answers. k2.6 is the default -- GA, full account
                    // rate limit; k3 is newer with a 1M window but is throttled in
                    // preview, which stalls multi-round tool turns. One tap switches.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: Theme.color.mediumorange2
                        opacity: 0.4
                    }

                    Text {
                        text: "BRAIN"
                        color: Theme.color.lightorange2
                        font.family: "Share Tech Mono"; font.pixelSize: 14; font.bold: true
                    }

                    Repeater {
                        model: Nikita.apiModelChoices
                        Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.preferredHeight: 38
                            radius: 3
                            color: "transparent"
                            border.width: 1
                            border.color: Nikita.apiModel === modelData.id
                                          ? Theme.color.lightorange2
                                          : (brainHover.containsMouse ? Theme.color.mediumorange1
                                                                      : Theme.color.mediumorange2)
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8
                                // A filled dot marks the active model.
                                Text {
                                    text: Nikita.apiModel === modelData.id ? "\u25cf" : "\u25cb"
                                    color: Nikita.apiModel === modelData.id
                                           ? Theme.color.lightorange2 : Theme.color.mediumorange1
                                    font.family: "Share Tech Mono"; font.pixelSize: 12
                                }
                                ColumnLayout {
                                    spacing: 0
                                    Layout.fillWidth: true
                                    Text {
                                        text: modelData.label
                                        color: Nikita.apiModel === modelData.id
                                               ? Theme.color.lightorange2 : Theme.color.mediumorange1
                                        font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                                    }
                                    Text {
                                        text: modelData.note
                                        color: Theme.color.mediumorange1
                                        opacity: 0.7
                                        font.family: "Share Tech Mono"; font.pixelSize: 9
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                            MouseArea {
                                id: brainHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Nikita.apiModel = modelData.id
                            }
                        }
                    }


                    // Two states, and the resting one is what tells you a key is there:
                    // a stored key shows as a fixed row of dots you cannot edit, the
                    // way a saved password looks in any settings panel. Clicking it
                    // opens an EMPTY field to type a replacement into -- the real
                    // characters are never put back on screen, because the backend has
                    // no getter for them at all and this panel could not read them if
                    // it wanted to. The dots are a marker, not the key.
                    ColumnLayout {
                        id: apiKeyBox
                        Layout.fillWidth: true
                        spacing: 6

                        // Typing a new key over a saved one. Reset whenever the stored
                        // key changes underneath, so saving drops straight back to dots.
                        property bool editing: false
                        // The eye. Never sticks: a Timer puts it back, and saving,
                        // clearing or starting to type all close it.
                        property bool revealed: false

                        // Clicking the key opens it for editing WITH the key still in
                        // it, rather than handing back an empty box. Blanking the field
                        // on a click reads as having destroyed something -- and if you
                        // only wanted to look, or to fix one character, an empty field
                        // means fetching the key from wherever you keep it all over
                        // again. The consequence to know about: the dots now count the
                        // real key, so the field's width gives its length away, where
                        // the resting state deliberately showed a fixed 28.
                        function beginEditing() {
                            if (fromEnv) { return; }   // the environment wins; nothing to edit
                            apiKeyField.text = Nikita.revealApiKey();
                            editing = true;
                            apiKeyField.forceActiveFocus();
                            apiKeyField.selectAll();
                        }
                        property bool fromEnv: Nikita.apiKeySource === "environment"
                        onFromEnvChanged: { editing = false; revealed = false; }

                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            // Present is not the same as working, and the difference
                            // is the whole reason this line exists: a saved key that
                            // the API rejects has to say so here rather than let the
                            // panel look ready and fail on the first message.
                            text: {
                                if (!Nikita.apiKeyPresent) { return "No kimi API key Found."; }
                                if (Nikita.apiKeyStatus === "checking") { return "Checking the key with Moonshot…"; }
                                if (Nikita.apiKeyStatus === "invalid") {
                                    return "Key rejected — " + Nikita.apiKeyMessage;
                                }
                                if (Nikita.apiKeyStatus === "offline" && !Nikita.apiKeyValid) {
                                    return "Couldn't reach the API to check this key.";
                                }
                                // Never the model id. Which brain is answering is
                                // the badge's job at the top of the chat, and
                                // naming it here too put a second answer a few
                                // pixels under the picker that sets it.
                                if (apiKeyBox.fromEnv) {
                                    return "Key found in MOONSHOT_API_KEY. The environment wins over anything saved here.";
                                }
                                return Nikita.apiKeyValid ? "Key accepted." : "Not verified yet.";
                            }
                            color: !Nikita.apiKeyPresent || Nikita.apiKeyStatus === "invalid" ? "#ff6a6a"
                                 : Nikita.apiKeyValid ? Theme.color.mediumorange1 : "#ffd400"
                            font.family: "Share Tech Mono"; font.pixelSize: 11
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            // ---- eye: reveal the key ---------------------------
                            // Drawn, not typed, for the same reason as the gear above:
                            // Share Tech Mono has no glyph for an eye, and letting Qt
                            // substitute a system symbol font puts it on a different
                            // baseline at a different size.
                            Item {
                                Layout.preferredWidth: 20
                                Layout.preferredHeight: 20
                                Layout.alignment: Qt.AlignVCenter
                                visible: Nikita.apiKeyPresent && !apiKeyBox.fromEnv
                                Canvas {
                                    id: eyeIcon
                                    anchors.centerIn: parent
                                    width: 18; height: 18
                                    property color tint: eyeMouse.containsMouse || apiKeyBox.revealed
                                                         ? Theme.color.lightorange2
                                                         : Theme.color.mediumorange1
                                    property bool open: apiKeyBox.revealed
                                    onTintChanged: requestPaint()
                                    onOpenChanged: requestPaint()
                                    Component.onCompleted: requestPaint()
                                    onPaint: {
                                        var ctx = getContext("2d");
                                        ctx.reset();
                                        ctx.clearRect(0, 0, width, height);
                                        ctx.strokeStyle = eyeIcon.tint;
                                        ctx.fillStyle = eyeIcon.tint;
                                        ctx.lineWidth = 1.4;
                                        var w = width, h = height, cy = h / 2;
                                        // The almond, as two mirrored quadratic curves.
                                        ctx.beginPath();
                                        ctx.moveTo(w * 0.10, cy);
                                        ctx.quadraticCurveTo(w * 0.50, cy - h * 0.34, w * 0.90, cy);
                                        ctx.quadraticCurveTo(w * 0.50, cy + h * 0.34, w * 0.10, cy);
                                        ctx.stroke();
                                        ctx.beginPath();
                                        ctx.arc(w * 0.50, cy, w * 0.13, 0, Math.PI * 2);
                                        ctx.fill();
                                        // Struck through when hidden, so the two states
                                        // differ in shape and not only in brightness --
                                        // a colour-only difference is no difference at
                                        // all to a lot of people.
                                        if (!eyeIcon.open) {
                                            ctx.beginPath();
                                            ctx.moveTo(w * 0.14, h * 0.82);
                                            ctx.lineTo(w * 0.86, h * 0.18);
                                            ctx.stroke();
                                        }
                                    }
                                }
                                MouseArea {
                                    id: eyeMouse
                                    anchors.fill: parent
                                    anchors.margins: -3
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        apiKeyBox.revealed = !apiKeyBox.revealed;
                                        if (apiKeyBox.revealed) { hideAgain.restart(); }
                                    }
                                }
                            }
                            // Never left showing. Walking away from a revealed key is
                            // the whole risk of having this control at all.
                            Timer {
                                id: hideAgain
                                interval: 15000
                                onTriggered: apiKeyBox.revealed = false
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 30
                                radius: 3
                                color: "transparent"
                                border.width: 1
                                border.color: Nikita.apiKeyPresent && !apiKeyBox.editing
                                              ? Theme.color.mediumorange1
                                              : Theme.color.mediumorange2

                                // RESTING: a key is stored and nothing is being typed.
                                // Masked, unless the eye is open -- and only then is the
                                // real key asked for, one call, at the moment it is
                                // needed. A fixed count of dots otherwise: the key's
                                // real length is nobody's business.
                                Text {
                                    anchors.left: parent.left
                                    anchors.right: apiCopyBtn.left
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    visible: Nikita.apiKeyPresent && !apiKeyBox.editing
                                    elide: Text.ElideRight
                                    text: apiKeyBox.revealed ? Nikita.revealApiKey()
                                                             : "•".repeat(28)
                                    color: Theme.color.lightorange2
                                    font.family: "Share Tech Mono"; font.pixelSize: 12
                                }
                                MouseArea {
                                    anchors.left: parent.left
                                    anchors.right: apiCopyBtn.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    hoverEnabled: true
                                    enabled: Nikita.apiKeyPresent && !apiKeyBox.editing && !apiKeyBox.fromEnv
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: apiKeyBox.beginEditing()
                                }

                                // EDITING, or nothing stored yet.
                                TextInput {
                                    id: apiKeyField
                                    anchors.left: parent.left
                                    anchors.right: apiCopyBtn.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    verticalAlignment: TextInput.AlignVCenter
                                    visible: !Nikita.apiKeyPresent || apiKeyBox.editing
                                    enabled: visible && !apiKeyBox.fromEnv
                                    echoMode: apiKeyBox.revealed ? TextInput.Normal : TextInput.Password
                                    clip: true
                                    color: Theme.color.lightorange2
                                    font.family: "Share Tech Mono"; font.pixelSize: 12
                                    selectByMouse: true
                                    onAccepted: saveApiKey()
                                    Keys.onEscapePressed: {
                                        text = "";
                                        apiKeyBox.editing = false;
                                        apiKeyBox.revealed = false;
                                    }
                                    function saveApiKey() {
                                        if (text.length === 0) {
                                            // Emptying the field and saving IS the
                                            // delete. It is the only way the key goes
                                            // away, which is the point: nothing about
                                            // opening, looking at or clicking the field
                                            // can lose it, and the one gesture that
                                            // removes it is a thing you have to do on
                                            // purpose and then confirm with Save.
                                            //
                                            // Guarded on a key existing, so Save on an
                                            // empty field during first setup stays the
                                            // harmless no-op it was.
                                            if (Nikita.apiKeyPresent) { Nikita.clearApiKey(); }
                                            apiKeyBox.editing = false;
                                            apiKeyBox.revealed = false;
                                            return;
                                        }
                                        Nikita.setApiKey(text);
                                        text = "";
                                        apiKeyBox.editing = false;
                                        apiKeyBox.revealed = false;
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: apiKeyField.text.length === 0
                                        text: Nikita.apiKeyPresent ? "paste a new key to replace the saved one"
                                                                   : "paste your Kimi API key"
                                        color: Theme.color.mediumorange1
                                        opacity: 0.5
                                        font.family: "Share Tech Mono"; font.pixelSize: 12
                                    }
                                }

                                // ---- copy, inside the field's right edge -----------
                                // The key never reaches QML for this: copyApiKeyToClipboard()
                                // reads it and puts it on the clipboard entirely in C++.
                                Item {
                                    id: apiCopyBtn
                                    width: 26; height: parent.height
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: Nikita.apiKeyPresent
                                    Canvas {
                                        id: copyIcon
                                        anchors.centerIn: parent
                                        width: 14; height: 14
                                        property color tint: copiedFlash.running
                                                             ? Theme.color.lightorange2
                                                             : (copyMouse.containsMouse ? Theme.color.lightorange2
                                                                                        : Theme.color.mediumorange1)
                                        onTintChanged: requestPaint()
                                        Component.onCompleted: requestPaint()
                                        onPaint: {
                                            var ctx = getContext("2d");
                                            ctx.reset();
                                            ctx.clearRect(0, 0, width, height);
                                            ctx.strokeStyle = copyIcon.tint;
                                            ctx.lineWidth = 1.3;
                                            var w = width, h = height;
                                            // Two offset sheets: the back one peeking
                                            // out top-right, the front one solid.
                                            ctx.strokeRect(w * 0.30, h * 0.06, w * 0.62, h * 0.62);
                                            ctx.clearRect(w * 0.06, h * 0.30, w * 0.62, h * 0.64);
                                            ctx.strokeRect(w * 0.06, h * 0.30, w * 0.62, h * 0.62);
                                        }
                                    }
                                    // Confirmation, because a copy is invisible: nothing
                                    // else on screen changes, and without some feedback
                                    // there is no way to tell a click that worked from
                                    // one that missed the button. The icon itself is
                                    // the whole acknowledgement -- it brightens and
                                    // pops for a moment. No word beside it: a label
                                    // that appears and vanishes next to a button draws
                                    // more attention than the thing it is confirming.
                                    Timer { id: copiedFlash; interval: 450 }
                                    SequentialAnimation {
                                        id: copyPop
                                        NumberAnimation { target: copyIcon; property: "scale"
                                                          to: 1.35; duration: 90
                                                          easing.type: Easing.OutQuad }
                                        NumberAnimation { target: copyIcon; property: "scale"
                                                          to: 1.0; duration: 160
                                                          easing.type: Easing.OutQuad }
                                    }
                                    MouseArea {
                                        id: copyMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: if (Nikita.copyApiKeyToClipboard()) {
                                            copiedFlash.restart();
                                            copyPop.restart();
                                        }
                                    }
                                }
                            }

                            Text {
                                visible: !apiKeyBox.fromEnv
                                text: "Save"
                                color: saveKeyMouse.containsMouse ? Theme.color.lightorange2
                                                                  : Theme.color.mediumorange1
                                font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                                MouseArea {
                                    id: saveKeyMouse
                                    anchors.fill: parent
                                    anchors.margins: -6
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        // Save on a field nobody has typed into means
                                        // "let me type": the alternative is a button
                                        // that silently does nothing, which reads as
                                        // broken.
                                        if (!apiKeyBox.editing && Nikita.apiKeyPresent) {
                                            apiKeyBox.beginEditing();
                                        } else {
                                            apiKeyField.saveApiKey();
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ---- ACCESS FILTERS ----------------------------------------
                    // What NIKITA is allowed to touch. The two buttons up top are
                    // shortcuts for the same toggles below -- not separate modes, just
                    // all-on/all-off in one click. The actual blocking happens in the
                    // backend (runOneTool), never here: a control that only hides a
                    // tool from the list is decoration, not a gate.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: Theme.color.mediumorange2
                        opacity: 0.4
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "ACCESS"
                            color: Theme.color.lightorange2
                            font.family: "Share Tech Mono"; font.pixelSize: 14; font.bold: true
                        }
                        Text {
                            text: Nikita.filterPreset === 1 ? "full"
                                : Nikita.filterPreset === 0 ? "none" : "custom"
                            // No red here. FORMAT wipes the entire SD card and uses the
                            // standard palette, so red is not this app's language for
                            // "destructive" -- it appears only on uninstall/cancel.
                            color: Nikita.filterPreset === 1 ? "#39ff14" : Theme.color.mediumorange1
                            font.family: "Share Tech Mono"; font.pixelSize: 11
                            Layout.fillWidth: true
                        }

                        // preset: nothing
                        Rectangle {
                            Layout.preferredWidth: noneLabel.implicitWidth + 16
                            Layout.preferredHeight: 20
                            radius: 3
                            color: noneMouse.containsMouse ? Theme.color.lightorange2 : "transparent"
                            border.width: 1
                            border.color: Nikita.filterPreset === 0 ? Theme.color.lightorange2
                                                                    : Theme.color.mediumorange2
                            Text {
                                id: noneLabel
                                anchors.centerIn: parent
                                text: "NO ACCESS"
                                color: noneMouse.containsMouse ? "#0b0410"
                                     : (Nikita.filterPreset === 0 ? Theme.color.lightorange2
                                                                  : Theme.color.mediumorange1)
                                font.family: "Share Tech Mono"; font.pixelSize: 10; font.bold: true
                            }
                            MouseArea {
                                id: noneMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Nikita.setAllFilters(false)
                            }
                        }

                        // preset: everything
                        Rectangle {
                            Layout.preferredWidth: allLabel.implicitWidth + 16
                            Layout.preferredHeight: 20
                            radius: 3
                            color: allMouse.containsMouse ? Theme.color.lightgreen : "transparent"
                            border.width: 1
                            border.color: Nikita.filterPreset === 1 ? "#39ff14" : Theme.color.mediumorange2
                            Text {
                                id: allLabel
                                anchors.centerIn: parent
                                text: "FULL ACCESS"
                                color: allMouse.containsMouse ? "#0b0410"
                                     : (Nikita.filterPreset === 1 ? "#39ff14" : Theme.color.mediumorange1)
                                font.family: "Share Tech Mono"; font.pixelSize: 10; font.bold: true
                            }
                            MouseArea {
                                id: allMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Nikita.setAllFilters(true)
                            }
                        }
                    }

                    // A Repeater, not a ListView: the panel scrolls as one page
                    // now, and a list that scrolls inside a page that scrolls
                    // fights the wheel for whichever one the pointer happens to
                    // be over. Every filter is laid out at full height and the
                    // Flickable above carries the lot.
                    ColumnLayout {
                        id: filterView
                        Layout.fillWidth: true
                        spacing: 4

                        Repeater {
                            model: Nikita.filters

                            delegate: Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: filterCol.implicitHeight + 12
                                radius: 4
                                color: modelData.enabled ? "#120818" : "#0d0610"
                                border.width: 1
                                border.color: modelData.enabled ? Theme.color.mediumorange2 : "#3a2a3a"

                                ColumnLayout {
                                    id: filterCol
                                    x: 10; y: 6
                                    width: parent.width - 20
                                    spacing: 2

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        Text {
                                            text: modelData.label
                                            color: modelData.enabled ? "#eaffea" : Theme.color.mediumorange1
                                            font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                                            Layout.fillWidth: true
                                        }
                                        // Same pill styling as installed/not installed in
                                        // the model list above: one visual language, one click.
                                        Rectangle {
                                            Layout.preferredWidth: 44
                                            Layout.preferredHeight: 18
                                            radius: 3
                                            color: modelData.enabled ? "#0f3d1f" : "transparent"
                                            border.width: modelData.enabled ? 0 : 1
                                            border.color: Theme.color.mediumorange2
                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.enabled ? "ON" : "OFF"
                                                color: modelData.enabled ? "#39ff14" : Theme.color.mediumorange1
                                                font.family: "Share Tech Mono"; font.pixelSize: 10; font.bold: true
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                anchors.margins: -4
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: Nikita.setFilter(modelData.id, !modelData.enabled)
                                            }
                                        }
                                    }

                                    Text {
                                        text: modelData.blurb
                                        color: Theme.color.mediumorange1
                                        font.family: "Share Tech Mono"; font.pixelSize: 10
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }
                                }
                            }
                        }
                    }
                }
            }

        }
    }
}
