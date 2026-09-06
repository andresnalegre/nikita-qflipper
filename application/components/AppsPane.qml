import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import QtQuick.Effects

import Theme 1.0
import QFlipper 1.0

// The app catalog, as a tab page rather than a panel over everything: it lives
// in the same frame as the file manager, so switching between "browse the card"
// and "browse the catalog" is the same gesture as any other tab.
//
// Downloads a .fap and hands it to the file manager, which already knows how to
// put a file on the card -- nothing here re-implements the upload.
Item {
    id: control

    implicitWidth: 742
    implicitHeight: 344

    // Shared with the rest of the home screen so the app catalog asks its
    // questions in the same dialog everything else uses.
    property ConfirmationDialog confirmationDialog

    Connections {
        target: Apps
        // The Flipper will not open a second app over a running one. Rather
        // than reporting that as a failure, offer the only thing the user
        // could want next.
        function onLaunchNeedsAppClosed(index, name) {
            if(!control.confirmationDialog) { return; }
            control.confirmationDialog.openWithMessage(
                function() { Apps.launchClosingCurrent(index); },
                {
                    title: qsTr("An app is running"),
                    message: qsTr("The Flipper already has an app open. Close it and open %1?").arg(name),
                    customText: qsTr("Close and open")
                });
        }
    }

    // The catalog still tracks "open" -- it uses it as the cue to fetch a list
    // it has not got yet -- so the tab page's visibility is what sets it now.
    Binding {
        target: Apps
        property: "open"
        value: control.visible
    }

    Binding {
        target: Apps
        property: "deviceTarget"
        value: (Backend.deviceState && Backend.deviceState.info && Backend.deviceState.info.hardware)
               ? Backend.deviceState.info.hardware.target : ""
    }

    Binding {
        target: Apps
        property: "deviceApi"
        value: (Backend.deviceState && Backend.deviceState.info && Backend.deviceState.info.firmware)
               ? Backend.deviceState.info.firmware.api : ""
    }

    Connections {
        target: Apps
        function onReadyToInstall(localFile, remoteDir) {
            // The file manager takes local file URLs, and creates the
            // directory if the category folder is not there yet.
            Backend.fileManager.uploadTo(remoteDir, [ "file://" + localFile ]);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        spacing: 0

        // Two cards per row: at this panel width a single column left most of
        // the card empty, and 400-odd apps is a lot of scrolling to do one at a
        // time. A GridView rather than a Flow so the rows stay virtualised.
        GridView {
            id: appList
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            cellWidth: Math.floor(width / 2)
            cellHeight: 100
            boundsBehavior: Flickable.StopAtBounds
            model: Apps.apps

            // Where the reader left the view. The model is a plain list that
            // gets rebuilt whole on every change -- an install included -- and
            // a rebuilt model resets the view to the top, so installing an app
            // near the bottom threw you back to the first row. The position is
            // put back instead.
            //
            // keepY 0 doubles as the first-layout case: the filter tiles ride
            // in the header, so its height is only known once the Flow has laid
            // out, and a header that grows after the fact leaves the view
            // already scrolled past it.
            property real keepY: 0

            onMovementEnded: keepY = contentY
            onFlickEnded: keepY = contentY

            function restoreY() {
                if(keepY <= 0) { positionViewAtBeginning(); }
                else { contentY = keepY; }
            }

            Component.onCompleted: restoreY()
            onCountChanged: restoreY()

            header: Item {
                width: GridView.view.width
                height: modeRow.height + (catFlow.visible ? catFlow.height + 12 : 0) + 10
                onHeightChanged: if(!appList.movingVertically) appList.restoreY()

                // Sits above the categories because it cuts across them: a
                // different question from "which kind of app", and the two
                // combine rather than replace each other. "Installed" is read
                // off the card, so it stays dimmed until that walk has finished
                // rather than showing a count it cannot back up.
                Row {
                    id: modeRow
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 6

                    Repeater {
                        model: [
                            { label: "All Apps",  mode: 0 },
                            { label: "Installed", mode: 1 }
                        ]

                        delegate: Rectangle {
                            readonly property bool current: Apps.installFilter === modelData.mode
                            readonly property bool ready: modelData.mode === 0 || Apps.installedKnown

                            implicitWidth: modeText.implicitWidth + 22
                            height: 26
                            radius: 13
                            color: current ? Theme.color.mediumorange2
                                 : modeMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.08)
                                 : Qt.rgba(1, 1, 1, 0.03)
                            Behavior on color { ColorAnimation { duration: 110 } }

                            Text {
                                id: modeText
                                anchors.centerIn: parent
                                text: modelData.mode === 0
                                      ? modelData.label
                                      : modelData.label + (parent.ready ? "  " + Apps.installedCount : "")
                                color: current ? "#ffffff"
                                     : parent.ready ? Qt.rgba(1, 1, 1, 0.70) : Qt.rgba(1, 1, 1, 0.35)
                                font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                            }

                            MouseArea {
                                id: modeMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    // A new filter is a new list: that one goes
                                    // back to the top on purpose.
                                    appList.keepY = 0;
                                    Apps.setInstallFilter(modelData.mode);
                                    // "All Apps" is the one that means all of
                                    // them: it drops the category too, so there
                                    // is always one click back to the whole
                                    // catalogue however it was narrowed.
                                    if(modelData.mode === 0) { Apps.selectCategory(""); }
                                }
                            }
                        }
                    }
                }

                // Filter tiles, one per category that actually has apps.
                // Clicking the one already showing clears it, so they double as
                // their own "show everything".
                Flow {
                    id: catFlow
                    // A Flow given the full width lays its tiles out from the
                    // left however few there are, so centring it did nothing:
                    // the item was centred, its content was not. Sized to the
                    // tiles it will actually fit on a row instead, which is
                    // what makes the block sit under the pills.
                    readonly property int tileW: 118
                    readonly property int gap: 6
                    readonly property int perRow:
                        Math.max(1, Math.floor((parent.width + gap) / (tileW + gap)))

                    anchors.top: modeRow.bottom
                    anchors.topMargin: 10
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(Apps.categories.length, perRow) * (tileW + gap) - gap
                    spacing: gap
                    visible: Apps.categories.length > 0

                    Repeater {
                        model: Apps.categories

                        delegate: Rectangle {
                            width: 118; height: 54
                            radius: 8
                            color: modelData.selected ? Qt.rgba(1, 1, 1, 0.10)
                                 : catMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.06)
                                 : Qt.rgba(1, 1, 1, 0.02)
                            border.width: 1
                            border.color: modelData.selected ? Theme.color.lightorange2
                                        : catMouse.containsMouse ? Theme.color.mediumorange2
                                        : Qt.rgba(1, 1, 1, 0.10)
                            Behavior on color { ColorAnimation { duration: 110 } }
                            Behavior on border.color { ColorAnimation { duration: 110 } }

                            // The catalog ships its own category icons: 24x24
                            // SVGs filled black, recoloured white here the same
                            // way the app icons are, and for the same reason.
                            Image {
                                id: catIconSrc
                                x: 9; y: 8
                                width: 18; height: 18
                                source: modelData.icon
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                cache: true
                                visible: false
                            }

                            MultiEffect {
                                anchors.fill: catIconSrc
                                source: catIconSrc
                                brightness: 1.0
                                opacity: modelData.selected ? 1.0 : 0.75
                            }

                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 9
                                y: 9
                                text: modelData.count
                                color: Qt.rgba(1, 1, 1, 0.55)
                                font.family: "Share Tech Mono"; font.pixelSize: 11
                            }

                            Text {
                                x: 9; y: 32
                                width: parent.width - 18
                                text: modelData.name
                                color: modelData.selected ? "#ffffff" : Qt.rgba(1, 1, 1, 0.70)
                                font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                                elide: Text.ElideRight
                            }

                            MouseArea {
                                id: catMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    appList.keepY = 0;
                                    Apps.selectCategory(modelData.name);
                                }
                            }
                        }
                    }
                }
            }


            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: appCard
                // The cell is the grid's; the card insets itself inside it,
                // which is what puts the gap between neighbours in both
                // directions now that there are two per row.
                width: GridView.view.cellWidth - 6
                height: GridView.view.cellHeight - 6
                radius: 8

                // Rows lift on hover, the way the app list does on the
                // phone, so the pointer always has something under it.
                color: cardMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.05)
                                               : Qt.rgba(1, 1, 1, 0.02)
                border.width: 1
                border.color: cardMouse.containsMouse ? Theme.color.mediumorange2
                                                      : Qt.rgba(1, 1, 1, 0.10)
                Behavior on color { ColorAnimation { duration: 110 } }
                Behavior on border.color { ColorAnimation { duration: 110 } }

                MouseArea {
                    id: cardMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }

                RowLayout {
                    id: appRow
                    anchors.fill: parent
                    anchors.margins: 9
                    spacing: 11

                    // The catalog's icons are 10x10 pixel art. Scaled
                    // up with smoothing off so they stay crisp squares
                    // instead of turning to mush.
                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 40; implicitHeight: 40
                        radius: 6
                        color: Qt.rgba(1, 1, 1, 0.06)
                        border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.10)

                        // The catalog's icons are pure black on
                        // transparent -- invisible against this panel.
                        // Recoloured white, keeping the hard pixel
                        // edges (smooth: false) so the 10x10 art stays
                        // crisp when scaled up.
                        Image {
                            id: appIconSrc
                            anchors.centerIn: parent
                            width: 30; height: 30
                            source: modelData.icon
                            smooth: false
                            mipmap: false
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: true
                            visible: false
                        }

                        // brightness, not colorization: colorization
                        // keeps the source's luminance, and these are
                        // pure black, so tinting left them black. Full
                        // brightness lifts the RGB to white and leaves
                        // the alpha -- the actual artwork -- untouched.
                        MultiEffect {
                            anchors.fill: appIconSrc
                            source: appIconSrc
                            brightness: 1.0
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 3

                        Text {
                            Layout.fillWidth: true
                            text: modelData.name
                            color: "#ffffff"
                            font.family: "Share Tech Mono"; font.pixelSize: 14; font.bold: true
                            elide: Text.ElideRight
                        }

                        Text {
                            Layout.fillWidth: true
                            text: modelData.description
                            color: Qt.rgba(1, 1, 1, 0.55)
                            font.family: "Share Tech Mono"; font.pixelSize: 11
                            elide: Text.ElideRight
                            visible: !!modelData.description
                        }

                        RowLayout {
                            spacing: 6

                            Rectangle {   // category chip
                                visible: !!modelData.category
                                implicitWidth: catText.implicitWidth + 12
                                implicitHeight: 16
                                radius: 8
                                color: Qt.rgba(1, 1, 1, 0.06)
                                Text {
                                    id: catText
                                    anchors.centerIn: parent
                                    text: modelData.category
                                    color: Qt.rgba(1, 1, 1, 0.55)
                                    font.family: "Share Tech Mono"; font.pixelSize: 10
                                }
                            }

                            Text {
                                text: "v" + modelData.version
                                color: Qt.rgba(1, 1, 1, 0.55)
                                font.family: "Share Tech Mono"; font.pixelSize: 10
                            }

                            Text {
                                visible: modelData.downloads > 0
                                text: "↓ " + modelData.downloads
                                color: Qt.rgba(1, 1, 1, 0.55)
                                font.family: "Share Tech Mono"; font.pixelSize: 10
                            }
                        }
                    }

                    // In the Installed view the row offers both things you
                    // can do to an app that is already there: open it, and get
                    // rid of it. Stacked rather than side by side -- the row is
                    // only 78px of button wide, and OPEN sitting above UNINSTALL
                    // also puts the harmless one under the pointer first.
                    ColumnLayout {
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 5

                        readonly property bool onCard: !!modelData.installed
                        readonly property bool installedView: Apps.installFilter === 1

                        Rectangle {
                            id: instButton
                            readonly property bool opens: parent.onCard && parent.installedView
                            // In the full list an app already on the card has
                            // nothing left to offer, so the button goes quiet
                            // rather than inviting a second install of the same
                            // thing.
                            readonly property bool acts: opens || !parent.onCard

                            Layout.preferredWidth: 88
                            Layout.preferredHeight: 26
                            radius: 6
                            opacity: acts ? 1.0 : 0.45
                            color: acts && instAppMouse.containsMouse && !Apps.busy
                                   ? Qt.rgba(1, 1, 1, 0.10) : "transparent"
                            border.width: 1
                            border.color: (Apps.busy || !acts) ? Qt.rgba(1,1,1,0.15)
                                                               : Theme.color.mediumorange2
                            Behavior on color { ColorAnimation { duration: 110 } }

                            Text {
                                anchors.centerIn: parent
                                text: instButton.opens ? "OPEN"
                                    : instButton.parent.onCard ? "INSTALLED" : "INSTALL"
                                color: Apps.busy ? Qt.rgba(1, 1, 1, 0.55) : "#ffffff"
                                font.family: "Share Tech Mono"; font.pixelSize: 12; font.bold: true
                            }

                            MouseArea {
                                id: instAppMouse
                                anchors.fill: parent
                                enabled: instButton.acts
                                hoverEnabled: instButton.acts
                                cursorShape: (Apps.busy || !instButton.acts)
                                             ? Qt.ArrowCursor : Qt.PointingHandCursor
                                onClicked: {
                                    if(Apps.busy) { return; }
                                    if(instButton.opens) { Apps.launch(modelData.index); }
                                    else { Apps.install(modelData.index); }
                                }
                            }
                        }

                        // Deleting from the card is not something to do by
                        // accident, so it asks -- the same dialog the rest of
                        // the home screen uses -- and it only ever appears
                        // where it makes sense.
                        Rectangle {
                            visible: parent.onCard && parent.installedView
                            Layout.preferredWidth: 88
                            Layout.preferredHeight: 26
                            radius: 6
                            color: uninstMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.10)
                                                             : "transparent"
                            border.width: 1
                            border.color: Qt.rgba(1, 1, 1, 0.25)
                            Behavior on color { ColorAnimation { duration: 110 } }

                            Text {
                                anchors.centerIn: parent
                                text: "UNINSTALL"
                                color: Qt.rgba(1, 1, 1, 0.70)
                                font.family: "Share Tech Mono"; font.pixelSize: 11; font.bold: true
                            }

                            MouseArea {
                                id: uninstMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if(!control.confirmationDialog) {
                                        Apps.uninstall(modelData.index);
                                        return;
                                    }
                                    const i = modelData.index;
                                    const n = modelData.name;
                                    control.confirmationDialog.openWithMessage(
                                        function() { Apps.uninstall(i); },
                                        {
                                            title: qsTr("Remove %1?").arg(n),
                                            message: qsTr("Deletes the app from the Flipper's SD card."),
                                            customText: qsTr("Remove")
                                        });
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
