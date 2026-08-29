import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15

import Theme 1.0

// A file already sits at the path a save_file wants to write on the Flipper.
// Instead of the firmware silently overwriting it, the user decides: Replace it,
// Rename the new one, or Cancel. Sibling of ComputerActionConfirmDialog -- same
// CustomDialog/openWith* pattern, but three outcomes instead of allow/deny, and
// a name field for the Rename path.
CustomDialog {
    id: control

    property string path
    property string preview
    // Just the filename, for the rename field's starting point and label.
    readonly property string baseName: {
        var p = control.path;
        var i = p.lastIndexOf("/");
        return i >= 0 ? p.substring(i + 1) : p;
    }

    // onDecided(action, newName): action is "replace" | "rename" | "cancel".
    function openWithConflict(onDecided, conflictPath, conflictPreview) {
        function finish(action, newName) {
            control.accepted.disconnect(onAccepted);
            control.rejected.disconnect(onRejected);
            onDecided(action, newName);
        }
        function onAccepted() { /* unused: buttons call decide() directly */ }
        function onRejected() { finish("cancel", ""); }

        control.path = conflictPath;
        control.preview = conflictPreview;
        control.title = qsTr("File already exists");
        nameField.text = control.baseName;
        control._finish = finish;

        control.rejected.connect(onRejected);
        control.accepted.connect(onAccepted);
        control.open();
        widgetContents.forceActiveFocus();
    }

    // Set by openWithConflict; the buttons call it.
    property var _finish: null
    function decide(action, newName) {
        if (control._finish) {
            var f = control._finish;
            control._finish = null;
            control.rejected.disconnect();  // avoid the reject path double-firing on close
            f(action, newName);
        }
        control.close();
    }

    contentWidget: Item {
        id: widgetContents
        implicitWidth: 460
        implicitHeight: layout.implicitHeight
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape) { control.decide("cancel", ""); }
        }

        ColumnLayout {
            id: layout
            width: parent.implicitWidth
            spacing: 10

            Text {
                Layout.topMargin: 20
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.color.lightorange2
                font.family: "Share Tech Mono"
                font.pixelSize: 13
                text: qsTr("There is already a file at:")
            }

            Rectangle {
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(pathLabel.implicitHeight + 16, 60)
                color: "#1a1008"
                radius: 5
                border.width: 1
                border.color: Theme.color.mediumorange2
                Text {
                    id: pathLabel
                    anchors.fill: parent
                    anchors.margins: 8
                    wrapMode: Text.WrapAnywhere
                    color: Theme.color.lightorange2
                    font.family: "Share Tech Mono"
                    font.pixelSize: 12
                    text: control.path
                }
            }

            Text {
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.color.mediumorange1
                font.family: "Share Tech Mono"
                font.pixelSize: 11
                text: qsTr("Replace it, or save the new one under a different name?")
            }

            // Rename field: the name Rename will use. Editing it does not save;
            // the Rename button does.
            Rectangle {
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: "#1a1008"
                radius: 5
                border.width: 1
                border.color: nameField.activeFocus ? Theme.color.lightorange2
                                                    : Theme.color.mediumorange2
                TextInput {
                    id: nameField
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    color: Theme.color.lightorange2
                    font.family: "Share Tech Mono"
                    font.pixelSize: 12
                    selectByMouse: true
                    onAccepted: control.decide("rename", nameField.text)
                }
            }

            RowLayout {
                spacing: 16
                Layout.margins: 20
                Layout.fillWidth: true
                Layout.preferredHeight: 42

                SmallButton {
                    radius: 7
                    text: qsTr("Cancel")
                    highlighted: true
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    onClicked: control.decide("cancel", "")
                }
                SmallButton {
                    radius: 7
                    text: qsTr("Rename")
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    enabled: nameField.text.trim().length > 0
                             && nameField.text.trim() !== control.baseName
                    onClicked: control.decide("rename", nameField.text)
                }
                SmallButton {
                    radius: 7
                    text: qsTr("Replace")
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    onClicked: control.decide("replace", "")
                }
            }
        }
    }
}
