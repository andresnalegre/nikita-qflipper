import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

import Theme 1.0
import Primitives 1.0

ItemDelegate {
    id: control

    // Two model shapes reach this delegate: the stock C++ channel model, which
    // exposes "name" and "description" as roles, and a plain string list for a
    // community firmware's own channels. Reading "name" against the string list
    // silently yields undefined -- which is why those rows rendered as bare
    // colour bars with no text.
    text: (typeof name !== "undefined") ? name
        : (modelData && modelData.name !== undefined) ? modelData.name
        : (modelData !== undefined ? String(modelData) : "")

    readonly property string blurb: (typeof description !== "undefined") ? description
                                  : (modelData && modelData.description !== undefined) ? modelData.description
                                  : ""

    readonly property bool last: {
        const m = ListView.view.model;
        const n = !m ? 0
                : (m.count !== undefined) ? m.count      // QAbstractListModel
                : (m.length !== undefined) ? m.length    // string list / JS array
                : 0;
        return index === n - 1;
    }

    highlighted: control.highlightedIndex === index
    hoverEnabled: control.hoverEnabled

    width: ListView.view.width

    contentItem: RowLayout {
        spacing: 10

        Rectangle {
            Layout.preferredWidth: 4
            Layout.fillHeight: true
            Layout.topMargin: 2
            Layout.bottomMargin: 2

            // Stock channels spell them out; forks abbreviate. Same meaning,
            // so the same colour.
            color: (text === "development" || text === "dev") ? "orangered" :
                   (text === "release-candidate" || text === "rc") ? "blueviolet" : "limegreen"
        }

        TextLabel {
            text: control.text
            color: control.down ? Theme.color.darkorange1 : control.hovered ? Theme.color.lightorange1 : Theme.color.lightorange2
            Layout.fillWidth: true
        }
    }

    background: AdvancedRectangle {
        x: 2
        width: parent.width - 4
        color: control.down ? Theme.color.lightorange2 : control.hovered ? Theme.color.mediumorange2 : Theme.color.darkorange1
        bottomRadius: control.last ? 5 : 0

        Behavior on color {
            ColorAnimation {
                duration: 150
                easing.type: Easing.OutQuad
            }
        }
    }

    ToolTip {
        delay: 300
        visible: parent.hovered && control.blurb.length > 0
        text: control.blurb
        implicitWidth: 250
    }
}
