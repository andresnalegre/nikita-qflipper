pragma Singleton

import QtQuick 2.15

QtObject {
    // Fixed palette. Values were pulled out of the runtime color editor when it
    // was dropped, so the UI looks exactly as it did. (transparent stays literal.)
    readonly property var color: QtObject {
        readonly property color transparent: Qt.rgba(0, 0, 0, 0)

        readonly property color lightorange1: "#fd8cff"
        readonly property color lightorange2: "#fd8cff"
        readonly property color lightorange3: "#ac5fae"
        readonly property color darkorange1: "#3d223d"
        readonly property color darkorange2: "#3a203b"
        readonly property color mediumorange1: "#a159a2"
        readonly property color mediumorange2: "#6c3c6d"
        readonly property color mediumorange3: "#583159"
        readonly property color mediumorange4: "#8a4c8b"
        readonly property color mediumorange5: "#915092"

        readonly property color lightgreen: "#2ed832"
        readonly property color mediumgreen1: "#285b12"
        readonly property color mediumgreen2: "#203812"
        readonly property color darkgreen: "#0c160c"

        readonly property color lightblue: "#be69bf"
        readonly property color mediumblue: "#532e53"
        readonly property color darkblue1: "#492849"
        readonly property color darkblue2: "#3e223e"

        readonly property color lightred1: "#d174d3"
        readonly property color lightred2: "#cf73d1"
        readonly property color lightred3: "#945295"
        readonly property color lightred4: "#ae60af"
        readonly property color mediumred1: "#7b447c"
        readonly property color mediumred2: "#583058"
        readonly property color darkred1: "#3b203b"
        readonly property color darkred2: "#2a172a"
    }

    readonly property var timing: QtObject {
        readonly property int toolTipDelay: 500
    }
}
