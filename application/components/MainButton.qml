import QtQuick 2.15
import QtQuick.Controls 2.15

import Theme 1.0

Button {
    id: control

    enum Accent {
        Default,
        Green,
        Blue
    }

    property int accent: MainButton.Default

    width: 280
    height: 56

    radius: 7
    borderWidth: 3

    font.family: "Born2bSportyV2"
    font.pixelSize: 48

    states: [
        State {
            when: accent === MainButton.Green

            PropertyChanges {
                foregroundColor.normal: Theme.color.lightgreen
                foregroundColor.hover: Theme.color.lightgreen
                foregroundColor.down: Theme.color.darkgreen
            }

            PropertyChanges {
                backgroundColor.normal: Theme.color.mediumgreen2
                backgroundColor.hover: Theme.color.mediumgreen1
                backgroundColor.down: Theme.color.lightgreen
            }

            PropertyChanges {
                strokeColor.normal: Theme.color.lightgreen
                strokeColor.hover: Theme.color.lightgreen
                strokeColor.down: Theme.color.lightgreen
            }
        },

        State {
            when: accent === MainButton.Blue

            PropertyChanges {
                foregroundColor.normal: Theme.color.lightblue
                foregroundColor.hover: Theme.color.lightblue
                foregroundColor.down: Theme.color.darkblue1
            }

            PropertyChanges {
                backgroundColor.normal: Theme.color.darkblue2
                backgroundColor.hover: Theme.color.darkblue1
                backgroundColor.down: Theme.color.lightblue
            }

            PropertyChanges {
                strokeColor.normal: Theme.color.lightblue
                strokeColor.hover: Theme.color.lightblue
                strokeColor.down: Theme.color.lightblue
            }
        }
    ]
}
