import QtQuick 2.15
import QtQuick.Controls.impl 2.15

import Theme 1.0
import QFlipper 1.0

Item {
    id: overlay
    visible: opacity > 0

    // The BLE scan/connect panel itself lives in MainWindow (it needs to
    // outlive this screen, since a successful connection is what makes this
    // screen go away). Asking for it by signal rather than reaching for an
    // id in another file keeps this component reusable on its own.
    signal bleRequested()

    Behavior on opacity {
        PropertyAnimation {
            easing.type: Easing.InOutQuad
            duration: 150
        }
    }

    Image {
        id: usbPlug

        // to the right of the centered Flipper, at USB-port height -- shifted
        // in step with the device's own -100->-150 offset in MainWindow.qml
        // (200-(-100) == 150-(-150) == 300) so it stays glued to the port.
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.horizontalCenterOffset: 150
        y: 132

        source: "qrc:/assets/gfx/images/typec.svg"
        sourceSize: Qt.size(159, 37)
    }

    // Cable not required: a Flipper reachable over Bluetooth needs a way in
    // from right here, not just from the header button that only exists
    // once a device (any device) is already connected -- which was the
    // whole chicken-and-egg problem with BLE-only up to now. Placed right
    // next to the USB glyph, at the same height, so the two read as the
    // two ways in rather than the cable being the only one illustrated.
    Item {
        id: bleRow
        visible: Nikita.hasBle
        anchors.left: usbPlug.right
        anchors.leftMargin: 24
        anchors.verticalCenter: usbPlug.verticalCenter
        width: bleOrLabel.width + 14 + bleIcon.width
        height: Math.max(bleOrLabel.height, bleIcon.height)

        TextLabel {
            id: bleOrLabel
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter

            color: Theme.color.lightorange3
            text: qsTr("OR")

            font.family: "Born2bSportyV2"
            font.pixelSize: 24
        }

        // White rather than the theme pink: everything else on this screen
        // is illustration, and the one thing here you can actually click
        // needs to visibly not be part of that illustration.
        IconImage {
            id: bleIcon
            anchors.left: bleOrLabel.right
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter

            source: "qrc:/assets/gfx/images/bluetooth.svg"
            sourceSize: Qt.size(44, 44)
            color: bleMouse.containsMouse ? Theme.color.lightorange1 : "#ffffff"
            opacity: bleMouse.containsMouse ? 1.0 : 0.9
        }

        MouseArea {
            id: bleMouse
            anchors.left: bleIcon.left
            anchors.right: bleIcon.right
            anchors.top: bleIcon.top
            anchors.bottom: bleIcon.bottom
            anchors.margins: -10
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: overlay.bleRequested()
        }
    }

    TextLabel {
        id: connectMsg
        anchors.horizontalCenter: parent.horizontalCenter
        y: 294

        color: Theme.color.lightorange2
        text: qsTr("Connect your Flipper")

        font.family: "Born2bSportyV2"
        font.pixelSize: 54
    }

    Image {
        id: spinner

        anchors.rightMargin: 31
        anchors.bottomMargin: 21

        anchors.right: overlay.right
        anchors.bottom: overlay.bottom

        source: "qrc:/assets/gfx/images/spinner.svg"
        sourceSize: Qt.size(24, 24)

        opacity: Backend.isQueryInProgress

        PropertyAnimation {
            target: spinner
            duration: 1500
            loops: Animation.Infinite
            property: "rotation"
            running: Backend.isQueryInProgress
            from: 0
            to: 360
        }

        Behavior on opacity {
            PropertyAnimation {
                easing.type: Easing.InOutQuad
                duration: 150
            }
        }
    }
}
