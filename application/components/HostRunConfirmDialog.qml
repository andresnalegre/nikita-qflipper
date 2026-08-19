import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15

import Theme 1.0

// Gate for host_run: NIKITA cannot execute a shell command on this computer
// until a person sees the literal command here and presses Run. Modeled on
// ConfirmationDialog.qml (same openWithMessage-style callback pattern), with
// the command text and the "always allow" checkbox this action needs.
CustomDialog {
    id: control

    property string command
    property string cwd

    function openWithCommand(onDecidedFunc, cmd, workDir) {
        // Function declarations (hoisted), not const function expressions: see
        // ConfirmationDialog.qml's openWithMessage for why.
        function onDialogRejected() {
            control.rejected.disconnect(onDialogRejected);
            control.accepted.disconnect(onDialogAccepted);
            onDecidedFunc(false, false);
        }
        function onDialogAccepted() {
            control.rejected.disconnect(onDialogRejected);
            control.accepted.disconnect(onDialogAccepted);
            onDecidedFunc(true, alwaysCheck.checked);
        }

        control.command = cmd;
        control.cwd = workDir;
        control.title = qsTr("Run on this computer?");
        alwaysCheck.checked = false;

        control.rejected.connect(onDialogRejected);
        control.accepted.connect(onDialogAccepted);
        control.open();
        widgetContents.forceActiveFocus();
    }

    contentWidget: Item {
        id: widgetContents
        implicitWidth: 460
        implicitHeight: layout.implicitHeight
        KeyNavigation.tab: contentWidget
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Escape) { control.rejected(); }
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
                text: qsTr("Nikita wants to run this on your computer, in %1:").arg(control.cwd)
            }

            Rectangle {
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(cmdLabel.implicitHeight + 16, 160)
                color: "#1a1008"
                radius: 5
                border.width: 1
                border.color: Theme.color.mediumorange2

                Flickable {
                    anchors.fill: parent
                    anchors.margins: 8
                    contentWidth: width
                    contentHeight: cmdLabel.implicitHeight
                    clip: true

                    Text {
                        id: cmdLabel
                        width: parent.width
                        wrapMode: Text.Wrap
                        color: Theme.color.lightorange2
                        font.family: "Share Tech Mono"
                        font.pixelSize: 12
                        text: control.command
                    }
                }
            }

            CheckBox {
                id: alwaysCheck
                Layout.leftMargin: 16
                text: qsTr("Always allow this exact command")
                contentItem: Text {
                    text: alwaysCheck.text
                    leftPadding: alwaysCheck.indicator.width + 6
                    color: Theme.color.lightorange2
                    font.family: "Share Tech Mono"
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                }
            }

            RowLayout {
                spacing: 30
                Layout.margins: 20
                Layout.fillWidth: true
                Layout.preferredHeight: 42
                layoutDirection: Qt.platform.os === "osx" ? Qt.RightToLeft : Qt.LeftToRight

                SmallButton {
                    radius: 7
                    text: qsTr("Cancel")
                    highlighted: true
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    onClicked: control.rejected()
                }

                SmallButton {
                    radius: 7
                    text: qsTr("Run")
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    onClicked: control.accepted()
                }
            }
        }
    }
}
