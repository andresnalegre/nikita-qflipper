import QtQuick 2.15
import QtQuick.Window 2.15

import QFlipper 1.0

import "components"

Window {
    id: root
    visible: true
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint
    title: Qt.application.displayName

    width: mainWindow.baseWidth + mainWindow.shadowSize * 2
    height: mainWindow.baseHeight + mainWindow.shadowSize * 2

    minimumWidth: mainWindow.baseWidth + mainWindow.shadowSize * 2
    minimumHeight: mainWindow.baseHeight + mainWindow.shadowSize * 2

    maximumWidth: minimumWidth
    maximumHeight: minimumHeight

    color: "transparent"

    MainWindow {
        id: mainWindow

        onExpandStarted: {
            root.maximumHeight = baseHeight + logHeight + shadowSize * 2;
            root.height = root.maximumHeight;
        }

        onExpandFinished: {
            root.minimumHeight = root.maximumHeight;
        }

        onCollapseStarted: {
            root.minimumHeight = baseHeight + shadowSize * 2;
        }

        onCollapseFinished: {
            root.height = root.minimumHeight;
            root.maximumHeight = root.minimumHeight;
        }
    }

    // Bring Nikita's window back when the tray icon / menu asks for it.
    function showFromBackground() {
        root.show();
        root.raise();
        root.requestActivate();
    }

    // The close button: with a background presence, Nikita lives on (hide to
    // tray) instead of quitting; without one, close still means quit.
    function onClose() {
        if (App.backgroundAlive) {
            App.notifyHidden();
            root.hide();
        } else {
            Qt.quit();
        }
    }

    Component.onCompleted: {
        App.messageReceived.connect(root.alert);
        mainWindow.controls.minimizeRequested.connect(root.showMinimized);
        mainWindow.controls.closeRequested.connect(root.onClose);
        App.showWindowRequested.connect(root.showFromBackground);
    }
}
