import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15

import Theme 1.0
import QFlipper 1.0

AbstractOverlay {
    id: overlay

    // Shared by the title and the bar here, and mirrored on the device widget
    // in MainWindow, so the three keep their spacing while the group centres.
    readonly property int contentShift: 26

    // Backup, restore and format run through this same screen. They are our own
    // operations rather than qFlipper states, so the three fields below take
    // their values from Nikita while one is running and fall back to the
    // firmware flow otherwise.
    readonly property bool transfer: Nikita.transferActive

    TextLabel {
        id: updateLabel
        capitalized: false
        anchors.horizontalCenter: parent.horizontalCenter
        // The whole group sat high: 19px of air above the title against ~72
        // below the status line, in a 430px container. Everything here and the
        // device widget move down by the same 26px so the two margins match.
        y: 19 + overlay.contentShift

        color: Theme.color.lightorange2

        font.family: "Born2bSportyV2"
        font.pixelSize: 48

        text: {
            if(overlay.transfer) { return Nikita.transferTitle; }

            switch(Backend.backendState) {
            case ApplicationBackend.UpdatingDevice:
                return qsTr("Updating your Flipper");
            case ApplicationBackend.RepairingDevice:
                return qsTr("Repairing your Flipper");
            case ApplicationBackend.CreatingBackup:
                return qsTr("Creating Backup");
            case ApplicationBackend.RestoringBackup:
                return qsTr("Restoring Backup");
            case ApplicationBackend.FactoryResetting:
                return qsTr("Performing Factory Reset");
            case ApplicationBackend.InstallingFirmware:
                return qsTr("Installing Firmware");
            case ApplicationBackend.InstallingWirelessStack:
                return qsTr("Installing Wireless Firmware");
            case ApplicationBackend.InstallingFUS:
                return qsTr("Installing FUS Firmware");
            default:
                return text;
            }
        }
    }

    ProgressBar {
        id: progressBar

        width: 280
        height: 56

        from: 0
        to: 100

        // Anchored rather than positioned by arithmetic. The label above and
        // the one below both use anchors.horizontalCenter; Math.round() here
        // snapped to a whole pixel while they centre exactly, so on an odd
        // parent width the bar sat half a pixel off from both of them.
        anchors.horizontalCenter: parent.horizontalCenter
        y: 270 + overlay.contentShift

        value: overlay.transfer ? (Nikita.transferProgress < 0 ? 0 : Nikita.transferProgress * 100)
                                : (deviceState ? deviceState.progress : 0)
        // A negative fraction means "working, but the total isn't known yet" --
        // walking the card, or packing the archive.
        indeterminate: overlay.transfer ? Nikita.transferProgress < 0
                                        : (!deviceState ? true : deviceState.progress < 0)
    }

    TextLabel {
        id: messageLabel
        anchors.top: progressBar.bottom
        anchors.topMargin: 20
        // Centred on the bar, not on the overlay: the two belong together, and
        // tying them to the same reference means they cannot drift apart.
        anchors.horizontalCenter: progressBar.horizontalCenter
        horizontalAlignment: Text.AlignHCenter
        width: parent.width - 80
        elide: Text.ElideMiddle
        text: overlay.transfer ? Nikita.transferNote
            : !deviceState ? text
            : deviceState.isError ? deviceState.errorString : deviceState.statusString
        color: Theme.color.lightorange2
    }

    MouseArea {
        x: 620
        y: 120 + overlay.contentShift

        hoverEnabled: true

        width: layout.implicitWidth
        height: layout.implicitHeight

        opacity: overlay.transfer ? 0 :
                 Backend.backendState === ApplicationBackend.UpdatingDevice ? !!deviceInfo && !deviceInfo.storage.isExternalPresent :
                 Backend.backendState === ApplicationBackend.RepairingDevice ? !!deviceInfo && !deviceState.isRecoveryMode && !deviceInfo.storage.isExternalPresent : 0

        enabled: opacity > 0

        ColumnLayout {
            id: layout

            Image {
                source: "qrc:/assets/gfx/images/warning-sdcard.svg"
                sourceSize: Qt.size(44, 58)
                Layout.alignment: Qt.AlignHCenter
            }

            TextLabel {
                text: qsTr("No SD")
                Layout.alignment: Qt.AlignHCenter
            }
        }

        ToolTip {
            implicitWidth: 250
            text: qsTr("SD Card is not installed. Some functionality will not be available.")
            visible: parent.containsMouse
        }

        Behavior on opacity {
            PropertyAnimation {
                duration: 150
                easing.type: Easing.InOutQuad
            }
        }
    }

}
