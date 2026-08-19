NAME = qFlipper

equals(QT_MAJOR_VERSION, 6): QT += core5compat

unix:!macx {
    DEFINES += USB_BACKEND_LIBUSB
    CONFIG += link_pkgconfig
    PKGCONFIG += libusb-1.0 zlib

    isEmpty(PREFIX): PREFIX = /usr

} else:win32 {
    CONFIG -= debug_and_release
    DEFINES += USB_BACKEND_WIN32
    INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtZlib

    !win32-g++: LIBS +=  -lSetupApi -lWinusb -lUser32
    else: LIBS += -lsetupapi -lwinusb

} else:macx {
    DEFINES += USB_BACKEND_LIBUSB
    PKG_CONFIG = /opt/homebrew/bin/pkg-config
    CONFIG += link_pkgconfig
    PKGCONFIG += libusb-1.0 zlib

} else {
    error("Unsupported OS or compiler")
}

# A GIT_VERSION env var (e.g. `GIT_VERSION=2.0.0 ./package_mac.sh`) wins over
# the local tag -- lets a release build be pinned to the version being
# published without depending on this checkout's tag/commit matching it.
GIT_VERSION = $$(GIT_VERSION)
isEmpty(GIT_VERSION) {
    GIT_VERSION = $$system("git describe --tags --abbrev=0","lines", HAS_VERSION)
    !equals(HAS_VERSION, 0) {
        GIT_VERSION = 2.0.0
    }
}
# Release tags are named "V.X.Y.Z" on GitHub; strip the leading "V." so
# APP_VERSION is plain digits. VersionInfo::toNumericValue() (used to compare
# against the latest GitHub release) can't parse anything else.
GIT_VERSION = $$replace(GIT_VERSION, "^[^0-9]*", "")

GIT_COMMIT = $$system("git rev-parse --short=8 HEAD","lines", HAS_COMMIT)
!equals(HAS_COMMIT, 0) {
    GIT_COMMIT = unknown
}

GIT_TIMESTAMP = $$system("git log -1 --pretty=format:%ct","lines", HAS_TIMESTAMP)
!equals(HAS_TIMESTAMP, 0) {
    GIT_TIMESTAMP = 0
}

DEFINES += APP_NAME=\\\"$$NAME\\\" \
           APP_VERSION=\\\"$$GIT_VERSION\\\" \
           APP_COMMIT=\\\"$$GIT_COMMIT\\\" \
           APP_TIMESTAMP=$$GIT_TIMESTAMP \
           PB_ENABLE_MALLOC

# This is a fork: qFlipper's built-in *app* self-updater used to point at the
# OFFICIAL qFlipper server, which would have let users "update" straight into
# vanilla qFlipper, wiping Nikita. It now points at this fork's own GitHub
# releases instead (see ApplicationUpdateRegistry::check()), so the feature is
# back on. Flipper *firmware* updates are a separate registry and were never
# affected.

