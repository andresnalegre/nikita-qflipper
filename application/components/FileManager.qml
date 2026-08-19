import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import QtQuick.Controls.impl 2.15

import Theme 1.0
import QFlipper 1.0

Item {
    id: control

    implicitWidth: 742
    implicitHeight: 344

    property MessageDialog messageDialog
    property ConfirmationDialog confirmationDialog

    // ---- multi selection (drag band, Cmd+click, Cmd+A) ----
    property var selectedList: []                       // selected row indices
    function isSelected(i) { return selectedList.indexOf(i) >= 0 }
    // Clearing the selection has to drop the current item too. The delegate
    // paints at full opacity for either multiSelected or isCurrent, so emptying
    // selectedList on its own left the last clicked file looking selected even
    // though nothing was.
    function clearSel() { selectedList = []; fileView.currentIndex = -1 }
    function selectOnly(i) { selectedList = [i] }
    function toggleSel(i) {
        var a = selectedList.slice();
        var p = a.indexOf(i);
        if(p >= 0) { a.splice(p, 1); } else { a.push(i); }
        selectedList = a;
    }
    function selectAll() {
        var a = [];
        for(var i = 0; i < fileView.count; ++i) { a.push(i); }
        selectedList = a;
    }
    function selectRange(rect) {                          // rect in fileView content coords
        var cols = Math.max(1, Math.floor(fileView.width / fileView.cellWidth));
        var a = [];
        for(var i = 0; i < fileView.count; ++i) {
            var cx = (i % cols) * fileView.cellWidth;
            var cy = Math.floor(i / cols) * fileView.cellHeight;
            // intersect the cell box with the band
            if(cx < rect.x + rect.w && cx + fileView.cellWidth > rect.x &&
               cy < rect.y + rect.h && cy + fileView.cellHeight > rect.y) {
                a.push(i);
            }
        }
        selectedList = a;
    }

    onVisibleChanged: {
        if(Backend.backendState === ApplicationBackend.Ready) {
            Backend.screenStreamer.isEnabled = !visible
        }
        if(visible) {
            fileView.forceActiveFocus();
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 7
        anchors.rightMargin: 7

        RowLayout {
            spacing: 7
            Layout.fillWidth: true

            Button {
                id: backButton
                action: backAction

                implicitWidth: 30
                implicitHeight: 30

                padding: 4
            }

            Button {
                id: fwdButton
                action: forwardAction

                implicitWidth: 30
                implicitHeight: 30

                padding: 5
            }

            Rectangle {
                Layout.preferredWidth: 500
                Layout.preferredHeight: 30

                color: "black"
                radius: 6

                border.width: 2
                border.color: Theme.color.mediumorange1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 7

                    spacing: 5

                    IconImage {
                        Layout.alignment: Qt.AlignVCenter

                        color: Theme.color.lightorange2
                        sourceSize: Qt.size(16, 16)
                        source: {
                            const path = Backend.fileManager.currentPath

                            if(path.startsWith("/ext")) {
                                return "qrc:/assets/gfx/symbolic/filemgr/location-sdcard.svg"
                            } else if(path.startsWith("/int")) {
                                return "qrc:/assets/gfx/symbolic/filemgr/location-internal.svg"
                            } else {
                                return "";
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter

                        font.pixelSize: 18
                        font.family: "Share Tech"

                        color: Theme.color.lightorange2
                        elide: Text.ElideMiddle

                        text: {
                            const path = Backend.fileManager.currentPath

                            if(path.startsWith("/ext")) {
                                return path.replace("/ext", "SD Card") + "/";
                            } else if(path.startsWith("/int")) {
                                return path.replace("/int", "Internal Flash") + "/";
                            } else {
                                return path;
                            }
                        }

                        onTextChanged: {
                            // A new folder starts with nothing selected. Setting
                            // currentIndex to 0 here used to leave the first item
                            // painted as selected before the user touched anything.
                            control.clearSel();
                        }

                    }
                }
            }

            Button {
                id: refreshButton
                action: refreshAction

                implicitWidth: 30
                implicitHeight: 30

                padding: 2
            }

            Switch {
                id: hiddenFileSwitch
                text: qsTr("Hidden files")
                checked: Preferences.showHiddenFiles
                onCheckedChanged: {
                    Preferences.showHiddenFiles = checked;
                    Backend.fileManager.refresh();
                }
            }
        }

        ScrollView {
            id: scrollView

            Layout.fillWidth: true
            Layout.fillHeight: true

            ScrollBar.vertical.anchors.right: scrollView.right
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            padding: 0
            clip: true

            background: Item {}

            GridView {
                id: fileView
                cellWidth: 120
                cellHeight: 86

                boundsBehavior: Flickable.StopAtBounds

                // Selection overlay: sits above the delegates and owns click,
                // drag and double click. Right clicks on an item are passed
                // through so the delegate can open its own menu.
                MouseArea {
                    id: bandArea
                    z: 100
                    anchors.fill: parent
                    hoverEnabled: false
                    preventStealing: true   // keep the drag from the Flickable so the band works (wheel still scrolls)
                    acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.BackButton | Qt.ForwardButton

                    property real startX: 0
                    property real startY: 0
                    property bool banding: false
                    property bool didBand: false

                    function idxAt(mx, my) {
                        var cx = mx + fileView.contentX;
                        var cy = my + fileView.contentY;
                        var idx = fileView.indexAt(cx, cy);
                        if(idx < 0) { return -1; }
                        // indexAt answers for the whole cell. Once the grid is
                        // full there is no truly empty space left to click, so
                        // the padding around an icon has to count as empty,
                        // otherwise the selection can never be cleared.
                        var item = fileView.itemAtIndex(idx);
                        if(item && item.hitsContent) {
                            var p = fileView.contentItem.mapToItem(item, cx, cy);
                            if(!item.hitsContent(p.x, p.y)) { return -1; }
                        }
                        return idx;
                    }

                    onPressed: function(mouse) {
                        forceActiveFocus(Qt.MouseFocusReason);
                        var idx = idxAt(mouse.x, mouse.y);

                        if(mouse.button === Qt.RightButton) {
                            if(idx >= 0) { mouse.accepted = false; return; }   // let the delegate open its menu
                            return;                                            // empty right click, handled onClicked
                        }
                        if(mouse.button !== Qt.LeftButton) { return; }

                        // Just record the start. A rubber band only begins once the
                        // mouse actually moves (below), so a drag can start on top
                        // of any item, folders included, not only on empty space.
                        bandArea.startX = mouse.x; bandArea.startY = mouse.y;
                        bandArea.didBand = false;
                        bandArea.banding = false;
                    }

                    onPositionChanged: function(mouse) {
                        if(!(mouse.buttons & Qt.LeftButton)) { return; }
                        var dx = mouse.x - bandArea.startX;
                        var dy = mouse.y - bandArea.startY;

                        if(!bandArea.banding) {
                            if(Math.abs(dx) > 4 || Math.abs(dy) > 4) {
                                bandArea.banding = true;
                                bandArea.didBand = true;
                                if(!(mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier))) { control.clearSel(); }
                            } else {
                                return;   // not enough movement yet, still a click
                            }
                        }

                        var x = Math.min(mouse.x, bandArea.startX);
                        var y = Math.min(mouse.y, bandArea.startY);
                        var w = Math.abs(dx);
                        var h = Math.abs(dy);
                        band.x = x; band.y = y; band.width = w; band.height = h;
                        control.selectRange({ x: x + fileView.contentX, y: y + fileView.contentY, w: w, h: h });
                    }

                    onReleased: function(mouse) { bandArea.banding = false; }

                    onClicked: function(mouse) {
                        if(mouse.button === Qt.BackButton && Backend.fileManager.canGoBack) { Backend.fileManager.historyBack(); return; }
                        if(mouse.button === Qt.ForwardButton && Backend.fileManager.canGoForward) { Backend.fileManager.historyForward(); return; }

                        var idx = idxAt(mouse.x, mouse.y);

                        if(mouse.button === Qt.RightButton) {
                            if(idx < 0 && !Backend.fileManager.isRoot) { emptyMenu.popup(); }
                            return;
                        }
                        if(mouse.button !== Qt.LeftButton) { return; }
                        if(bandArea.didBand) { return; }   // a drag just happened, keep that selection

                        if(idx < 0) {
                            control.clearSel();            // click on empty space clears selection
                        } else if(mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) {
                            control.toggleSel(idx);
                            fileView.currentIndex = idx;
                        } else {
                            control.selectOnly(idx);
                            fileView.currentIndex = idx;
                        }
                    }

                    onDoubleClicked: function(mouse) {
                        if(mouse.button !== Qt.LeftButton) { return; }
                        var idx = idxAt(mouse.x, mouse.y);
                        if(idx < 0) { return; }
                        if(Backend.fileManager.isDirectoryAt(idx)) {
                            var name = Backend.fileManager.fileNameAt(idx);
                            if(name.length > 0) { Backend.fileManager.cd(name); }
                        } else {
                            // The model already knows the absolute path, so it
                            // is read from there rather than rebuilt from the
                            // current path and the name.
                            var full = Backend.fileManager.filePathAt(idx);
                            if(full.length > 0) { Nikita.openFileForEdit(full); }
                        }
                    }
                }

                // rubber band selection rectangle (viewport coords)
                Rectangle {
                    id: band
                    z: 99
                    visible: bandArea.banding && (width > 2 || height > 2)
                    color: Color.transparent(Theme.color.lightorange2, 0.12)
                    border.width: 1
                    border.color: Theme.color.mediumorange2
                }

                model: Backend.fileManager
                delegate: FileManagerDelegate {
                    confirmationDialog: control.confirmationDialog
                    fileManager: control
                }
            }
        }
    }

    DropArea {
        enabled: !Backend.fileManager.isRoot
        anchors.fill: parent
        onDropped: function(drop) {
            if(drop.source || !drop.hasUrls || drop.proposedAction !== Qt.CopyAction) {
                const msgObj = {
                    title: qsTr("Error"),
                    message: qsTr("Operation is not supported"),
                    customText: qsTr("Close")
                };

                messageDialog.openWithMessage(null, msgObj);

            } else {
                control.uploadUrls(drop.urls);
                drop.accept()
            }
        }
    }

    Menu {
        id: emptyMenu

        MenuItem { action: uploadHereAction }
        MenuItem { action: newFileAction }
        MenuItem { action: newDirAction }
    }

    Action {
        id: forwardAction
        enabled: Backend.fileManager.canGoForward

        icon.width: 8
        icon.height: 14
        icon.source: "qrc:/assets/gfx/symbolic/arrow-forward-small.svg"

        onTriggered: Backend.fileManager.historyForward()
    }

    Action {
        id: backAction
        enabled: Backend.fileManager.canGoBack

        icon.width: 8
        icon.height: 14
        icon.source: "qrc:/assets/gfx/symbolic/arrow-back-small.svg"

        onTriggered: Backend.fileManager.historyBack()
    }

    Action {
        id: refreshAction

        icon.width: 16
        icon.height: 16
        icon.source: "qrc:/assets/gfx/symbolic/refresh-small.svg"

        onTriggered: Backend.fileManager.refresh()
    }

    Action {
        id: uploadHereAction
        text: qsTr("Upload here...")
        icon.source: "qrc:/assets/gfx/symbolic/filemgr/action-upload.svg"

        onTriggered: beginUpload();
    }

    Action {
        id: newFileAction
        text: qsTr("New File")
        icon.source: "qrc:/assets/gfx/symbolic/filemgr/action-new.svg"
        onTriggered: newFilePanel.show();
    }

    Action {
        id: newDirAction
        text: qsTr("New Folder")
        onTriggered: Backend.fileManager.beginMkDir();
        icon.source: "qrc:/assets/gfx/symbolic/filemgr/action-new.svg"
    }

    Component.onCompleted: {
        Backend.fileManager.currentPathChanged.connect(function() {
            // Covers every way of arriving somewhere: cd, back, forward, refresh
            // and the jump to root. selectedList was never cleared on navigation
            // either, so indices left over from the previous folder came back
            // highlighted on rows that just happened to share the same numbers.
            control.clearSel();
        });
    }

    function uploadUrls(urls) {
        const doUpload = function() {
            Backend.fileManager.upload(urls);
        };

        if(Backend.fileManager.isTooLarge(urls)) {
            const isMultiple = urls.length > 1;
            const msgObj = {
                title: qsTr("Warning"),
                message: qsTr("Selected %1 too large.\nUpload anyway?").arg(isMultiple ? qsTr("files are") : qsTr("file is")),
                suggestedRole: ConfirmationDialog.RejectRole,
                customText: qsTr("Upload")
            };

            confirmationDialog.openWithMessage(doUpload, msgObj);

        } else {
            doUpload();
        }
    }

    function beginUpload() {
        SystemFileDialog.accepted.connect(function() {
            control.uploadUrls(SystemFileDialog.fileUrls);
        });
        SystemFileDialog.beginOpenFiles(SystemFileDialog.LastLocation, [ "All files (*)" ]);
    }

    Keys.onPressed: function(event) {
        switch(event.key) {
        case Qt.Key_A:
            if(event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) {
                control.selectAll();
                event.accepted = true;
            } else {
                event.accepted = false;
            }
            return;

        case Qt.Key_Escape:
            // The panel takes Escape first, so it can be dismissed without
            // reaching for the mouse.
            if(newFilePanel.visible) {
                newFilePanel.hide();
            } else {
                control.clearSel();
            }
            event.accepted = true;
            return;

        case Qt.Key_Backspace:
            if(Backend.fileManager.canGoBack) {
                Backend.fileManager.historyBack();
            }
            event.accepted = true;
            return;

        case Qt.Key_L:
            if(Backend.fileManager.isRoot) {
                event.accepted = false;
            } else if(event.modifiers & Qt.ControlModifier) {
                beginUpload();
                event.accepted = true;
            } else {
                event.accepted = false;
            }
            return;

        case Qt.Key_N:
            if(Backend.fileManager.isRoot) {
                event.accepted = false;
            } else if(event.modifiers & Qt.ControlModifier) {
                Backend.fileManager.beginMkDir();
                event.accepted = true;
            } else {
                event.accepted = false;
            }
            return;

        case Qt.Key_G:
            if(event.modifiers & Qt.ControlModifier) {
                Backend.fileManager.refresh()
                event.accepted = true;
            } else {
                event.accepted = false;
            }
            return;

        default:
            event.accepted = false;
        }
    }

    // ---- new file panel (create a file with any extension, right in the app) ----
    Rectangle {
        id: newFilePanel
        visible: false
        z: 210
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 52
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        height: 46
        color: "#0b0410"
        radius: 6
        border.width: 1
        border.color: Theme.color.mediumorange2

        // Set while a create is in flight, so the refresh below only fires for
        // the file this panel just wrote and not for every save in the app.
        property string awaitingPath: ""

        function show() {
            newFileField.text = "";
            visible = true;
            newFileField.forceActiveFocus();
        }
        function hide() {
            visible = false;
            fileView.forceActiveFocus();
        }
        function createFile() {
            var name = newFileField.text.trim();
            if(name.length === 0) { return; }
            var base = Backend.fileManager.currentPath;
            var full = (base.charAt(base.length - 1) === "/") ? (base + name) : (base + "/" + name);
            Nikita.writeFile(full, "");          // create empty file on the Flipper
            newFilePanel.awaitingPath = full;
            newFilePanel.hide();
        }

        Connections {
            target: Nikita
            function onFileSaved(path) {
                if(newFilePanel.awaitingPath === path) {
                    // Just show the new file, don't open it: the user asked for
                    // a file, not for the editor.
                    Backend.fileManager.refresh();
                    newFilePanel.awaitingPath = "";
                }
            }
        }

        Keys.onEscapePressed: newFilePanel.hide()

        RowLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 8

            Text {
                text: "New file:"
                color: Theme.color.lightorange2
                font.family: "Share Tech Mono"; font.pixelSize: 13
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                color: "#000000"; radius: 4
                border.width: 1; border.color: Theme.color.mediumorange3
                TextInput {
                    id: newFileField
                    anchors.fill: parent
                    anchors.leftMargin: 8; anchors.rightMargin: 8
                    verticalAlignment: TextInput.AlignVCenter
                    color: Theme.color.lightorange2
                    font.family: "Share Tech Mono"; font.pixelSize: 13
                    clip: true
                    onAccepted: newFilePanel.createFile()
                    Keys.onEscapePressed: newFilePanel.hide()
                    Text {
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        visible: newFileField.text.length === 0
                        text: "name.txt / name.nfc / name.sub / name.ir"
                        color: Theme.color.mediumorange1
                        font: newFileField.font
                    }
                }
            }
            Text {
                text: "cancel"
                color: nfCancel.containsMouse ? Theme.color.lightorange2 : Theme.color.mediumorange1
                font.family: "Share Tech Mono"; font.pixelSize: 12
                MouseArea { id: nfCancel; anchors.fill: parent; anchors.margins: -6; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: newFilePanel.hide() }
            }
            Text {
                text: "create"
                color: nfCreate.containsMouse ? Theme.color.lightgreen : Theme.color.lightorange2
                font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                MouseArea { id: nfCreate; anchors.fill: parent; anchors.margins: -6; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: newFilePanel.createFile() }
            }
        }
    }

}
