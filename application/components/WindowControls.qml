import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

import Theme 1.0

RowLayout {
    id: control

    enum Style {
        Windows,
        MacOS,
        Linux
    }

    signal minimizeRequested
    signal closeRequested

    property bool closeEnabled: true

    readonly property int style: {
        // TODO: additional Linux style?
        if(Qt.platform.os === "osx") {
            return WindowControls.Style.MacOS
        } else {
            return WindowControls.Style.Windows
        }
    }

    layoutDirection: {
        switch(style) {
        case WindowControls.Style.MacOS:
            return Qt.RightToLeft;

        case WindowControls.Style.Windows: default:
            return Qt.LeftToRight;
        }
    }

    spacing: {
        switch(style) {
        case WindowControls.Style.MacOS:
           return 8;

        case WindowControls.Style.Windows: default:
           return 12;
        }
    }

    DragHandler {
        target: null
        onActiveChanged: if(active) control.Window.window.startSystemMove();
    }

    Item {
        id: spacer
        Layout.fillWidth: true
    }

    // Both buttons are drawn rather than loaded from SVG, so they follow the
    // palette like everything else on screen. The glyphs are plain rectangles
    // for the same reason: a font that lacks the character would render the
    // cross at a different weight than the minus beside it.
    Rectangle {
        id: minimizeButton
        Layout.preferredWidth: 22
        Layout.preferredHeight: 20
        radius: 4
        border.width: 1
        border.color: Theme.color.mediumorange2
        color: minMouse.pressed        ? Theme.color.mediumorange1
             : minMouse.containsMouse  ? Theme.color.mediumorange2
             : Theme.color.transparent

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
            onClicked: minimizeRequested()
        }
    }

    Rectangle {
        id: closeButton
        enabled: control.closeEnabled
        opacity: enabled ? 1 : 0.35
        Layout.preferredWidth: 22
        Layout.preferredHeight: 20
        radius: 4
        border.width: 1
        border.color: Theme.color.mediumorange2
        color: closeMouse.pressed       ? Theme.color.mediumorange1
             : closeMouse.containsMouse ? Theme.color.mediumorange2
             : Theme.color.transparent

        // Two bars crossed, same size and weight as the minimize one.
        Rectangle {
            width: 11; height: 2; radius: 1
            color: Theme.color.lightorange2
            anchors.centerIn: parent
            rotation: 45
        }
        Rectangle {
            width: 11; height: 2; radius: 1
            color: Theme.color.lightorange2
            anchors.centerIn: parent
            rotation: -45
        }

        MouseArea {
            id: closeMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: closeRequested()
        }
    }
}
