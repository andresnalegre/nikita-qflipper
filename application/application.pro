QT += quick serialport widgets quickcontrols2 svg network

# BLE connection -- Windows, Linux, macOS. Qt Bluetooth uses the WinRT
# backend on Windows, BlueZ/D-Bus on Linux (the Linux CI Qt now includes
# qtconnectivity; see docker/Dockerfile), and CoreBluetooth on macOS via
# Homebrew's qtconnectivity formula. Gated behind HZUI_BLE.
#   blespike     -- scan/connect test panel (Phase 1 proof + Phase 3 device connect)
#   bletransport -- the FlipperTransport impl the real session runs over
# macOS additionally needs NSBluetoothAlwaysUsageDescription in the bundle's
# Info.plist (see application/Info.plist.app) -- CoreBluetooth silently
# refuses to scan/connect without it, with no error dialog to explain why.
win32|linux|macx {
    QT += bluetooth
    DEFINES += HZUI_BLE
    SOURCES += blespike.cpp bletransport.cpp
    HEADERS += blespike.h bletransport.h
}

include(../qflipper_common.pri)

TARGET = $$NAME
DESTDIR = $$OUT_PWD/..

CONFIG += c++11

SOURCES += \
        application.cpp \
        applicationupdater.cpp \
        applicationupdateregistry.cpp \
        nikitabackend.cpp \
        main.cpp \
        qtsingleapplication/qtlocalpeer.cpp \
        qtsingleapplication/qtlockedfile.cpp \
        qtsingleapplication/qtlockedfile_unix.cpp \
        qtsingleapplication/qtlockedfile_win.cpp \
        qtsingleapplication/qtsingleapplication.cpp \
        qtsingleapplication/qtsinglecoreapplication.cpp \
        screencanvas.cpp \
        systemfiledialog.cpp

RESOURCES += qml.qrc

TRANSLATIONS += \
    translations/en_US.ts

CONFIG += lrelease
CONFIG += embed_translations

QML_IMPORT_PATH += $$PWD/imports

unix:!macx {
    QTPLUGIN += qxdgdesktopportal
    QTPLUGIN.platforms += qxcb qwayland-egl qwayland-generic
}

win32:!win32-g++ {
    PRE_TARGETDEPS += \
        $$OUT_PWD/../backend/backend.lib \
        $$OUT_PWD/../dfu/dfu.lib

} else:unix|win32-g++ {
    PRE_TARGETDEPS += \
        $$OUT_PWD/../backend/libbackend.a \
        $$OUT_PWD/../dfu/libdfu.a

    contains(CONFIG, static): PRE_TARGETDEPS += \
        $$OUT_PWD/../plugins/libflipperproto0.a \
        $$OUT_PWD/../3rdparty/lib3rdparty.a
}

unix|win32 {
    LIBS += \
        -L$$OUT_PWD/../backend/ -lbackend \
        -L$$OUT_PWD/../dfu/ -ldfu

    contains(CONFIG, static): LIBS += \
        -L$$OUT_PWD/../plugins/ -lflipperproto0 \
        -L$$OUT_PWD/../3rdparty/ -l3rdparty
}

win32 {
    equals(HAS_VERSION, 0) {
        RC_SUFFIX = -rc

        contains(GIT_VERSION, .*$${RC_SUFFIX}.*) {
            # Remove -rc suffix as it isn't allowed in Windows manifest
            TOKENS = $$split(GIT_VERSION, -)
            VERSION = $$first(TOKENS)
        } else {
            VERSION = $$GIT_VERSION
        }

        # Strip a leading "v"/"V" (e.g. tag "V1.1.0") so the numeric Windows
        # FILEVERSION resource stays valid; rc.exe rejects non-numeric versions.
        VERSION = $$replace(VERSION, "^[vV]", "")

    } else: VERSION = 0.0.0
}

macx: ICON = assets/icons/$${NAME}.icns
else:win32: RC_ICONS = assets/icons/$${NAME}.ico

# Qt's own template plus NSBluetoothAlwaysUsageDescription (see the file for
# why) -- needed once BLE is enabled on macOS below.
macx: QMAKE_INFO_PLIST = Info.plist.app

INCLUDEPATH += \
    $$PWD/../dfu \
    $$PWD/../backend

DEPENDPATH += \
    $$PWD/../dfu \
    $$PWD/../backend

HEADERS += \
    application.h \
    applicationupdater.h \
    applicationupdateregistry.h \
    nikitabackend.h \
    qtsingleapplication/qtlocalpeer.h \
    qtsingleapplication/qtlockedfile.h \
    qtsingleapplication/qtsingleapplication.h \
    qtsingleapplication/qtsinglecoreapplication.h \
    screencanvas.h \
    systemfiledialog.h

DISTFILES +=

unix:!macx {
    target.path = $$PREFIX/bin

    desktopfiles.files = $$PWD/../installer-assets/appimage/$${TARGET}.desktop
    desktopfiles.path = $$PREFIX/share/applications

    iconfiles.files = $$PWD/assets/icons/$${TARGET}.png
    iconfiles.path = $$PREFIX/share/icons/hicolor/512x512/apps

    udevfiles.files = $$PWD/../installer-assets/udev/42-flipperzero.rules
    udevfiles.path = $$PREFIX/lib/udev/rules.d

    INSTALLS += target desktopfiles iconfiles udevfiles

} else:win32 {
    target.path = $$DESTDIR/$$NAME
    INSTALLS += target
}