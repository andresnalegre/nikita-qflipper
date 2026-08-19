import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import QtQuick.Controls.impl 2.15

import Misc 1.0
import Theme 1.0
import QFlipper 1.0

Item {
    id: delegate

    required property int index

    required property string fileName
    required property string filePath
    required property int fileType
    required property int fileSize
    property bool editFlag

    readonly property bool isDirectory: !fileType
    readonly property bool isNewDirectory: Backend.fileManager.newDirectoryIndex === index
    readonly property bool isHovered: iconMouseArea.containsMouse || labelMouseArea.containsMouse
    readonly property bool isCurrent: GridView.isCurrentItem

    property var fileManager                              // set by the FileManager root
    readonly property bool multiSelected: fileManager ? fileManager.isSelected(index) : false

    property color selectionColor: Color.transparent(Theme.color.darkorange1,
        (delegate.multiSelected || delegate.isCurrent) ? 1 : delegate.isHovered ? 0.5 : 0)

    property ConfirmationDialog confirmationDialog

    width: 120
    height: 86

    // True when a point (in this delegate's coordinates) actually lands on the
    // icon or the label. The cell is 120x86 but the visual item only occupies
    // the middle of it, so GridView.indexAt() answers yes for a click in the
    // padding, which is why clicking the gap between icons selected one instead
    // of clearing the selection.
    function hitsContent(px, py) {
        var p = delegate.mapToItem(iconMouseArea, px, py);
        if(p.x >= 0 && p.y >= 0 && p.x < iconMouseArea.width && p.y < iconMouseArea.height) { return true; }
        p = delegate.mapToItem(labelMouseArea, px, py);
        return (p.x >= 0 && p.y >= 0 && p.x < labelMouseArea.width && p.y < labelMouseArea.height);
    }

    onIsNewDirectoryChanged: {
        if(isNewDirectory) {
            beginEdit();
        }
    }

    Behavior on selectionColor {
        ColorAnimation {
            duration: 150
            easing.type: Easing.OutQuad
        }
    }

    ColumnLayout {
        spacing: 3
        anchors.fill: parent

        Rectangle {
            Layout.topMargin: 10
            Layout.alignment: Qt.AlignHCenter

            radius: 2
            color: delegate.selectionColor
            Layout.preferredWidth: icon.width + 12
            Layout.preferredHeight: icon.height + 12

            MouseArea {
                id: iconMouseArea
                hoverEnabled: true
                anchors.fill: parent

                acceptedButtons: Qt.LeftButton | Qt.RightButton

                onClicked: function(mouse) {delegate.itemClick(mouse);}
                onDoubleClicked: function(mouse) {delegate.doubleClick(mouse);}
            }

            IconImage {
                id: icon

                x: 6
                y: 6

                layer.enabled: true

                sourceSize: Qt.size(28, 28)
                color: Theme.color.lightorange2

                source: {
                    if(delegate.filePath === "/ext") {
                        return "qrc:/assets/gfx/symbolic/mimetypes/sdcard.svg";
                    } else if(delegate.filePath === "/int") {
                        return "qrc:/assets/gfx/symbolic/mimetypes/internal.svg";
                    } else if(delegate.isDirectory) {
                        return "qrc:/assets/gfx/symbolic/mimetypes/folder.svg";
                    }

                    const extension = delegate.fileName.substring(delegate.fileName.lastIndexOf('.') + 1);

                    if(extension in FileTypes.icons) {
                        return FileTypes.icons[extension];
                    } else {
                        return FileTypes.icons["default"];
                    }
                }
            }
        }

        Item {
            Layout.alignment: Qt.AlignHCenter

            implicitWidth: delegate.width - 8
            implicitHeight: nameLabel.height

            Rectangle {
                anchors.fill: nameLabel
                color: delegate.selectionColor
            }
            MouseArea {
                id: labelMouseArea
                hoverEnabled: true
                anchors.fill: parent

                acceptedButtons: Qt.LeftButton | Qt.RightButton

                onClicked: function(mouse) {delegate.itemClick(mouse);}
                onDoubleClicked: function(mouse) {delegate.doubleClick(mouse);}
            }

            Text {
                id: nameLabel

                visible: !editBox.visible
                anchors.horizontalCenter: parent.horizontalCenter

                font.pixelSize: 16
                font.family: "Share Tech"

                color: Theme.color.lightorange2
                horizontalAlignment: Text.AlignHCenter

                maximumLineCount: 2
                elide: Text.ElideRight
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText

                width: Math.min(implicitWidth, delegate.width - 8)

                text: {
                    if(delegate.filePath === "/ext") {
                        return "SD Card";
                    } else if(delegate.filePath === "/int") {
                        return "Internal Flash";
                    } else {
                        return delegate.fileName;
                    }
                }
            }

            Rectangle {
                id: editBox

                readonly property int padding: 2

                visible: false

                y: -padding
                width: parent.width
                height: nameEdit.height + padding * 2

                color: "black"
                border.color: Theme.color.lightorange2
                border.width: 1

                TextInput {
                    id: nameEdit

                    y: editBox.padding

                    width: parent.width
                    font: nameLabel.font

                    color: Theme.color.lightorange2
                    selectionColor: color
                    selectedTextColor: "black"

                    wrapMode: Text.WrapAnywhere
                    horizontalAlignment: Text.AlignHCenter

                    selectByMouse: true

                    onEditingFinished: delegate.commitEdit();

                    validator: RegularExpressionValidator {
                        regularExpression: /[\x20-\x7E]+/ //Printable ASCII characters
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }

    Menu {
        id: storageMenu

        MenuItem { action: uploadHereAction }
        MenuItem { action: downloadAction }
    }

    Menu {
        id: fileMenu

        MenuItem { action: downloadAction }
        MenuItem { action: renameAction }
        MenuItem { action: removeAction }
    }

    Menu {
        id: dirMenu

        MenuItem { action: uploadHereAction }
        MenuItem { action: downloadAction }
        MenuItem { action: renameAction }
        MenuItem { action: removeAction }
    }

    Action {
        id: uploadHereAction
        text: qsTr("Upload here...")
        icon.source: "qrc:/assets/gfx/symbolic/filemgr/action-upload.svg"

        onTriggered: {
            SystemFileDialog.accepted.connect(function() {
                const doUpload = function() {
                    Backend.fileManager.uploadTo(delegate.fileName, SystemFileDialog.fileUrls);
                };

                if(Backend.fileManager.isTooLarge(SystemFileDialog.fileUrls)) {
                    const isMultiple = SystemFileDialog.fileUrls.length > 1;
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
            });

            SystemFileDialog.beginOpenFiles(SystemFileDialog.LastLocation, [ "All files (*)" ]);
        }
    }

    Action {
        id: downloadAction
        text: qsTr("Download...")
        icon.source: "qrc:/assets/gfx/symbolic/filemgr/action-download.svg"

        onTriggered: beginDownload();
    }

    Action {
        id: renameAction
        text: qsTr("Rename")
        icon.source: "qrc:/assets/gfx/symbolic/filemgr/action-rename.svg"
        onTriggered: delegate.beginEdit();
    }

    Action {
        id: removeAction
        text: qsTr("Delete...")
        icon.source: "qrc:/assets/gfx/symbolic/filemgr/action-remove.svg"

        onTriggered: beginDelete();
    }

    // Reached only when the selection overlay in FileManager lets an event
    // through, which it does for right clicks on an item. The left button path
    // is kept so the delegate still behaves on its own if that overlay ever
    // stops covering the grid.
    function itemClick(mouse) {
        delegate.GridView.view.currentIndex = delegate.index
        forceActiveFocus(Qt.MouseFocusReason);

        if(mouse.button === Qt.LeftButton) {
            if(delegate.fileManager) {
                if(mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) {
                    delegate.fileManager.toggleSel(delegate.index);
                } else {
                    delegate.fileManager.selectOnly(delegate.index);
                }
            }
            return;
        }

        // Right click on an item that isn't in the selection makes it the
        // selection, so the menu acts on what the user just pointed at.
        if(delegate.fileManager && !delegate.fileManager.isSelected(delegate.index)) {
            delegate.fileManager.selectOnly(delegate.index);
        }

        if(delegate.filePath === "/ext" || delegate.filePath === "/int") {
            storageMenu.popup();
        } else if(delegate.isDirectory) {
            dirMenu.popup();
        } else {
            fileMenu.popup();
        }
    }

    function doubleClick(mouse) {
        if(mouse.button === Qt.RightButton) {
            return;
        }
        if(delegate.isDirectory) {
            Backend.fileManager.cd(delegate.fileName);
        } else {
            Nikita.openFileForEdit(delegate.filePath);
        }
    }

    function beginEdit() {
        editFlag = true;
        nameEdit.text = nameLabel.text;
        nameEdit.selectAll();
        nameEdit.forceActiveFocus(Qt.MouseFocusReason);

        editBox.visible = true;
    }

    function commitEdit() {
        if(!editBox.visible) return;

        const oldName = delegate.fileName;
        const newName = nameEdit.text;

        if(delegate.isNewDirectory) {
            Backend.fileManager.commitMkDir(newName);
        } else if(oldName !== newName) {
            Backend.fileManager.rename(oldName, newName);
        }

        nameLabel.text = newName;
        editBox.visible = false;
    }

    function beginDelete() {
        var fm = delegate.fileManager;
        // Deleting acts on the whole selection when this item is part of it,
        // and on this item alone otherwise.
        var sel = (fm && fm.selectedList.length > 1 && fm.isSelected(delegate.index))
                  ? fm.selectedList.slice() : [delegate.index];

        const doRemove = function() {
            if(sel.length === 1 && sel[0] === delegate.index) {
                Backend.fileManager.remove(delegate.fileName, delegate.isDirectory);
            } else {
                for(var i = 0; i < sel.length; ++i) {
                    var nm = Backend.fileManager.fileNameAt(sel[i]);
                    if(nm.length > 0) {
                        Backend.fileManager.remove(nm, Backend.fileManager.isDirectoryAt(sel[i]));
                    }
                }
                if(fm) { fm.clearSel(); }
            }
        };

        const title = (sel.length > 1)
            ? "%1 %2 items?".arg(qsTr("Delete")).arg(sel.length)
            : "%1 \"%2\"?".arg(qsTr("Delete")).arg(delegate.fileName);

        const msgObj = {
            title: title,
            message: qsTr("This action cannot be undone."),
            suggestedRole: ConfirmationDialog.RejectRole,
            customText: qsTr("Delete")
        };

        confirmationDialog.openWithMessage(doRemove, msgObj);
    }

    function beginDownload() {
        SystemFileDialog.accepted.connect(function() {
            Backend.fileManager.download(delegate.fileName, SystemFileDialog.fileUrls[0], delegate.isDirectory);
        });

        if(delegate.isDirectory) {
            SystemFileDialog.beginSaveDir(SystemFileDialog.LastLocation);
        } else {
            SystemFileDialog.beginSaveFile(SystemFileDialog.LastLocation, [ "All files (*)" ], delegate.fileName);
        }
    }

    Keys.onPressed: function(event) {
        if(editBox.visible) {
            event.accepted = false;
            return;
        }

        switch(event.key) {
        case Qt.Key_Delete:
            if(Backend.fileManager.isRoot) {
                event.accepted = false;
            } else if(event.modifiers & Qt.ShiftModifier) {
                Backend.fileManager.remove(delegate.fileName, delegate.isDirectory);
                event.accepted = true;
            } else {
                beginDelete();
                event.accepted = true;
            }
            return;

        case Qt.Key_Return:
            if(delegate.isDirectory && !editFlag) {
                Backend.fileManager.cd(delegate.fileName);
                event.accepted = true;
            } else {
                editFlag = false;
                event.accepted = false;
            }
            return;

        case Qt.Key_E:
            if(Backend.fileManager.isRoot) {
                event.accepted = false;
            } else if(event.modifiers & Qt.ControlModifier) {
                beginEdit();
                event.accepted = true;
            } else {
                event.accepted = false;
            }
            return;

        case Qt.Key_D:
            if(event.modifiers & Qt.ControlModifier) {
                beginDownload();
                event.accepted = true;
            } else {
                event.accepted = false;
            }
            return;

        default:
            event.accepted = false;
        }
    }

    Connections {
        target: confirmationDialog
        function onVisibleChanged() {
            if (!confirmationDialog.visible) {
                fileView.forceActiveFocus();
            }
        }
    }

}
