import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15

import Theme 1.0
import QFlipper 1.0


Item {
    id: container

    property alias backupAction: backupAction
    property alias restoreAction: restoreAction
    property alias formatAction: formatAction
    property alias rebootAction: rebootAction
    property alias clearImportAction: clearImportAction
    property alias selfUpdateAction: selfUpdateAction

    implicitWidth: 318
    implicitHeight: control.implicitHeight + verticalPadding * 2

    readonly property int horizontalPadding: Math.floor((container.implicitWidth - control.implicitWidth) / 2)
    readonly property int verticalPadding: 10

    ColumnLayout {
        id: control
        spacing: 10

        x: horizontalPadding
        y: verticalPadding

        TransparentLabel {
            color: Theme.color.lightorange2
            text: qsTr("Firmware update channel")
        }

        ComboBox {
            id: channelComboBox

            // The stock model only lists official channels, which don't match
            // what a Flipper on Unleashed or Momentum is actually running.
            // Follow the installed firmware when there is one.
            readonly property bool onFork: Firmware.installedChannels.length > 0 &&
                                           Firmware.installedName !== "Official"

            enabled: onFork
                     ? Firmware.installedChannels.length > 1
                     : (Backend.firmwareUpdateState !== Backend.Unknown &&
                        Backend.firmwareUpdateState !== Backend.Checking &&
                        Backend.firmwareUpdateState !== Backend.ErrorOccured)

            // ChannelDelegate reads both model shapes, so one delegate covers
            // either case.
            delegate: ChannelDelegate {}

            model: onFork ? Firmware.installedChannels : Backend.firmwareUpdateModel
            textRole: onFork ? "" : "name"

            Layout.fillWidth: true

            currentIndex: onFork
                          ? Firmware.installedChannels.indexOf(Firmware.installedChannel)
                          : (Backend.firmwareUpdateState !== Backend.Unknown ? find(Preferences.updateChannel) : -1)

            onActivated: function(index) {
                if(onFork) { Firmware.setInstalledChannel(Firmware.installedChannels[index]); }
                else       { Preferences.updateChannel = textAt(index); }
            }

            ToolTip {
                visible: parent.hovered
                // The two lists look alike, so say which one is showing.
                text: channelComboBox.onFork
                      ? qsTr("Switch between the channels published by the installed firmware.")
                      : qsTr("Change the firmware update channel")
                implicitWidth: 250
            }
        }

        TransparentLabel {
            color: Theme.color.lightorange2
            text: qsTr("Backup & Restore")
        }

        GridLayout {
            columns: 2
            rowSpacing: control.spacing
            columnSpacing: control.spacing

            Layout.fillWidth: true

            SmallButton {
                action: backupAction
                Layout.fillWidth: true

                icon.source: "qrc:/assets/gfx/symbolic/backup-symbolic.svg"
                icon.width: 18
                icon.height: 20

                ToolTip {
                    visible: parent.hovered
                    text: qsTr("Save the contents of Flipper's internal storage to this computer's disk.")
                    implicitWidth: 250
                }
            }

            SmallButton {
                action: restoreAction
                Layout.fillWidth: true

                icon.source: "qrc:/assets/gfx/symbolic/restore-symbolic.svg"
                icon.width: 18
                icon.height: 20

                ToolTip {
                    visible: parent.hovered
                    text: qsTr("Download the contents of a backup directory to Flipper's internal storage.")
                    implicitWidth: 250
                }
            }

            // Red like the destructive buttons in DeveloperActions. This is the
            // only one here that throws away user data.
            SmallButtonRed {
                action: formatAction
                Layout.fillWidth: true

                icon.source: "qrc:/assets/gfx/symbolic/trashcan.svg"
                icon.width: 18
                icon.height: 20

                ToolTip {
                    visible: parent.hovered
                    text: qsTr("Format the SD card. WARNING! Everything stored on it will be lost!")
                    implicitWidth: 250
                }
            }

            SmallButton {
                action: rebootAction
                Layout.fillWidth: true

                icon.source: "qrc:/assets/gfx/symbolic/update-symbolic.svg"
                icon.width: 16
                icon.height: 16

                ToolTip {
                    visible: parent.hovered
                    text: rebootAction.enabled
                          ? qsTr("Restart the Flipper.")
                          : qsTr("Connect a Flipper first.")
                    implicitWidth: 250
                }
            }
        }

        // Picking another firmware replaces the staged one, so there is only
        // ever a single slot. This empties it. Stays visible and greys out when
        // nothing is staged, since hiding it made it hard to find.
        Button {
            Layout.fillWidth: true
            action: clearImportAction
            // No icon, otherwise the label sits off centre.

            ToolTip {
                visible: parent.hovered
                text: Firmware.hasSelection
                      ? qsTr("Discard the firmware file staged for install.")
                      : qsTr("No firmware file is staged right now.")
                implicitWidth: 250
            }
        }

        TransparentLabel {
            color: Theme.color.lightorange2
            text: qsTr("Application update")
        }

        Button {
            action: selfUpdateAction
            Layout.fillWidth: true

            icon.source: "qrc:/assets/gfx/symbolic/update-symbolic.svg"
            icon.width: 16
            icon.height: 16

            ToolTip {
                visible: parent.hovered
                text: qsTr("This build has no release feed yet, so there is nothing to check against.")
                implicitWidth: 250
            }
        }

        Action {
            id: backupAction
            text: qsTr("Backup")
            enabled: Backend.deviceState && !Backend.deviceState.isRecoveryMode
        }

        Action {
            id: restoreAction
            text: qsTr("Restore")
            enabled: Backend.deviceState && !Backend.deviceState.isRecoveryMode
        }

        Action {
            id: formatAction
            text: qsTr("Format")
            enabled: Backend.deviceState && !Backend.deviceState.isRecoveryMode
        }

        Action {
            id: rebootAction
            text: qsTr("Reboot")
            // Restarting needs nothing but a live connection.
            enabled: Backend.deviceState && !Backend.deviceState.isRecoveryMode
        }

        Action {
            id: clearImportAction
            text: qsTr("Clear")
            enabled: Firmware.hasSelection && !Firmware.busy
            onTriggered: Firmware.clearSelection()
        }

        Action {
            id: selfUpdateAction
            // Off for now. There is no release feed, and
            // DISABLE_APPLICATION_UPDATES in qflipper_common.pri forces
            // Preferences.checkAppUpdates to false anyway. To turn it on: drop
            // that define, point applicationupdateregistry at the releases, then
            // restore the two bindings below.
            text: qsTr("Check app updates")
            enabled: false
            // text: App.updateStatus === App.Checking ? qsTr("Checking...") :
            //       App.updateStatus === App.NoUpdates && checkTimer.running ? qsTr("No updates") : qsTr("Check app updates")
            // enabled: Preferences.checkAppUpdates && App.updateStatus !== App.Checking && !checkTimer.running
            onTriggered: App.checkForUpdates()
        }

        // Only used by the commented bindings above.
        Timer {
            id: checkTimer
            interval: 1000

            Component.onCompleted: {
                App.updateStatusChanged.connect(function() {
                    if(App.updateStatus === App.NoUpdates) {
                        start();
                    }
                });
            }
        }
    }
}
