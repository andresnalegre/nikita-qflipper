import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15

import Theme 1.0

// Gate for computer_write/computer_mkdir/computer_move/computer_copy/computer_delete: NIKITA
// cannot touch a file on this computer through one of these until a person
// sees what it wants to do and presses Allow. Sibling of
// ComputerRunConfirmDialog.qml (same openWith*/callback pattern) -- kept as a
// separate component because computer_run's dialog is command-text specific,
// while this one is generic across five different verbs with different
// summaries and an optional content preview.
CustomDialog {
    id: control

    property string kind
    property string summary
    property string detail

    function openWithAction(onDecidedFunc, actionKind, actionSummary, actionDetail) {
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

        control.kind = actionKind;
        control.summary = actionSummary;
        control.detail = actionDetail;
        control.title = qsTr("Allow on this computer?");
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
                text: qsTr("Nikita wants to do this on your computer:")
            }

            Rectangle {
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(summaryLabel.implicitHeight + 16, 80)
                color: "#1a1008"
                radius: 5
                border.width: 1
                border.color: Theme.color.mediumorange2

                Flickable {
                    anchors.fill: parent
                    anchors.margins: 8
                    contentWidth: width
                    contentHeight: summaryLabel.implicitHeight
                    clip: true

                    Text {
                        id: summaryLabel
                        width: parent.width
                        wrapMode: Text.Wrap
                        color: Theme.color.lightorange2
                        font.family: "Share Tech Mono"
                        font.pixelSize: 12
                        text: control.summary
                    }
                }
            }

            Rectangle {
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(detailLabel.implicitHeight + 16, 160)
                visible: control.detail.length > 0
                color: "#1a1008"
                radius: 5
                border.width: 1
                border.color: Theme.color.mediumorange2

                Flickable {
                    anchors.fill: parent
                    anchors.margins: 8
                    contentWidth: width
                    contentHeight: detailLabel.implicitHeight
                    clip: true

                    Text {
                        id: detailLabel
                        width: parent.width
                        wrapMode: Text.Wrap
                        color: Theme.color.lightorange2
                        font.family: "Share Tech Mono"
                        font.pixelSize: 12
                        text: control.detail
                    }
                }
            }

            CheckBox {
                id: alwaysCheck
                Layout.leftMargin: 16
                text: qsTr("Always allow host_%1 without asking").arg(control.kind)
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
                    text: qsTr("Allow")
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    onClicked: control.accepted()
                }
            }
        }
    }
}
