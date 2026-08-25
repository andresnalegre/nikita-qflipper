#!/bin/bash

# shellcheck disable=SC2207

set -exuo pipefail;

PROJECT="qFlipper";
BUILD_DIRECTORY="build_mac";

if [ -d ".git" ]; then
    git submodule update --init;
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script needs to be run under MacOS";
    exit 1;
fi

if [[ "$(uname -m)" == "arm64" ]]; then
    eval "$(/opt/homebrew/bin/brew shellenv)";
else
    eval "$(/usr/local/Homebrew/bin/brew shellenv)";
fi

# Build for THIS Mac's architecture only. The upstream script forced a universal
# (x86_64 + arm64) build, but on Apple Silicon the project-compiled deps (nanopb
# -> flipperproto0) have no x86_64 slice, so the linker dies with
# "symbol(s) not found for architecture x86_64". Native single-arch just works.
# Override with e.g.  BUILD_ARCHS="x86_64 arm64" ./build_mac.sh  if you really
# want a universal binary (and have universal deps).
BUILD_ARCHS="${BUILD_ARCHS:-$(uname -m)}";

if ! brew --version; then
    echo "Brew isn't installed!";
    exit 1;
fi

if ! brew --prefix libusb_universal; then
    echo "Please install libusb_universal first!";
    printf "\tbrew install flipperdevices/homebrew-flipper/libusb_universal\n";
    exit 1;
fi

if ! brew --prefix qt_universal; then
    echo "Please install qt_universal first!";
    printf "\tbrew install flipperdevices/homebrew-flipper/qt_universal\n";
    exit 1;
fi

rm -rf "$BUILD_DIRECTORY";
mkdir "$BUILD_DIRECTORY";

cd "$BUILD_DIRECTORY";

qmake \
    -spec macx-clang \
    CONFIG+="release qtquickcompiler" \
    -o Makefile \
    ../$PROJECT.pro \
    QMAKE_APPLE_DEVICE_ARCHS="$BUILD_ARCHS";

make qmake_all;
# NOTE: output is intentionally NOT sent to /dev/null so compile errors are
# visible. (The upstream script hid it, which turns any build failure into a
# silent "the .app doesn't exist".)
make "-j$(sysctl -n hw.ncpu)";
make install;

# Bundle the Qt frameworks and QML plugins INTO the .app. Without this the app
# links to Qt at /opt/homebrew (the build machine's Homebrew), which is absent
# on any other Mac AND, under the hardened runtime, is refused even here because
# the Homebrew Qt is signed by a different Team ID than the app -- dyld aborts
# at launch with "Library not loaded ... different Team IDs". macdeployqt copies
# the frameworks in and rewrites the load paths to @executable_path/../Frameworks.
# -qmldir lets it discover which QML modules the UI imports so their plugins come
# too. Its own trailing ad-hoc codesign is expected to fail on a synced folder
# (detritus xattrs); the real signing happens below, so that failure is ignored.
MACDEPLOYQT="$(brew --prefix qt)/bin/macdeployqt";
[ -x "$MACDEPLOYQT" ] || MACDEPLOYQT="$(command -v macdeployqt)";
"$MACDEPLOYQT" "$PROJECT.app" -qmldir="$PWD/../application" -no-strip || true;
# The virtual-keyboard input-context plugin references QtVirtualKeyboard, which
# is not part of this build, so it is dropped -- left in, it is dead weight and
# a signing/notarization snag. The app uses the normal text input.
rm -rf "$PROJECT.app/Contents/PlugIns/platforminputcontexts";

# bundle libusb
mkdir -p "$PROJECT.app/Contents/Frameworks";
cp "$(brew --prefix libusb_universal)/lib/libusb-1.0.0.dylib" "$PROJECT.app/Contents/Frameworks";
# Homebrew ships it read-only, and codesign cannot clear extended attributes
# from a file it may not write to.
chmod u+w "$PROJECT.app/Contents/Frameworks/libusb-1.0.0.dylib";

relink_framework()
{
    local FILE;
    local LIB;
    local REL_PATH;
    FILE="$1";
    LIB="$2";
    REL_PATH="$3";
    PATHS=( $(otool -L "$FILE" | grep "$LIB" | awk '{print $1}' ) );
    for CUR in "${PATHS[@]}"; do
        install_name_tool -change "$CUR" "$REL_PATH" "$FILE";
    done
}

relink_framework \
    "$PROJECT.app/Contents/Frameworks/libusb-1.0.0.dylib" \
    "libusb-1.0.0.dylib" \
    "@loader_path/libusb-1.0.0.dylib";
relink_framework \
    "$PROJECT.app/Contents/MacOS/qFlipper" \
    "libusb-1.0.0.dylib" \
    "@loader_path/../Frameworks/libusb-1.0.0.dylib";
relink_framework \
    "$PROJECT.app/Contents/MacOS/qFlipper-cli" \
    "libusb-1.0.0.dylib" \
    "@loader_path/../Frameworks/libusb-1.0.0.dylib";
# Sign, notarize and staple. Off by default so a dev build stays fast:
#   RELEASE=1 ./build_mac.sh
# Needs a Developer ID in the keychain and a notarytool profile named "nikita"
# (xcrun notarytool store-credentials "nikita" --apple-id ... --team-id ...).
STAGE="$HOME/Library/Caches/nikita-sign";

if [ -n "${RELEASE:-}" ]; then
    SIGN_ID="${MAC_OS_SIGNING_KEY_ID:-Developer ID Application: Andres Nicolas Alegre (Y76PU2RL9K)}";
    PROFILE="${NOTARY_PROFILE:-nikita}";

    # Signing happens in a staging folder, not here. Something in this tree puts
    # com.apple.FinderInfo back on the bundle directory between the xattr call
    # and codesign, and codesign refuses to sign a bundle carrying it. ditto
    # --noextattr hands over a clean copy, and the cache directory leaves it be.
    rm -rf "$STAGE";
    mkdir -p "$STAGE";
    /usr/bin/ditto --noextattr --norsrc "$PROJECT.app" "$STAGE/$PROJECT.app";

    # Sign bottom-up: every bundled framework and dylib first, then the
    # executables, then the app itself. `--deep` alone is unreliable across the
    # ~48 Qt frameworks macdeployqt brings in -- it silently leaves some nested
    # code unsigned and notarization rejects the lot. Signing each nested Mach-O
    # explicitly, deepest first, is the sequence Apple actually wants.
    # --timestamp is required for notarization (without it the submission is
    # accepted, then rejected minutes later with no useful message).
    while IFS= read -r fw; do
        codesign --force --options=runtime --timestamp -s "$SIGN_ID" "$fw";
    done < <(find "$STAGE/$PROJECT.app/Contents/Frameworks" -maxdepth 1 -name "*.framework" -type d);
    while IFS= read -r dl; do
        codesign --force --options=runtime --timestamp -s "$SIGN_ID" "$dl";
    done < <(find "$STAGE/$PROJECT.app" -name "*.dylib");
    codesign --force --options=runtime --timestamp -s "$SIGN_ID" \
        "$STAGE/$PROJECT.app/Contents/MacOS/$PROJECT-cli";
    codesign --force --deep --options=runtime --timestamp \
        -s "$SIGN_ID" -v "$STAGE/$PROJECT.app";
    codesign --verify --deep --strict "$STAGE/$PROJECT.app";

    /usr/bin/ditto -c -k --keepParent "$STAGE/$PROJECT.app" "$STAGE/$PROJECT.zip";
    xcrun notarytool submit --keychain-profile "$PROFILE" --wait "$STAGE/$PROJECT.zip";

    # Staple the ticket into the bundle. Notarizing alone still leaves the first
    # launch asking Apple over the network, so an offline machine sees the
    # "unidentified developer" wall on an app that is perfectly notarized.
    xcrun stapler staple "$STAGE/$PROJECT.app";
    rm -f "$STAGE/$PROJECT.zip";

    # Put the signed bundle back, so what you run day to day is what ships.
    rm -rf "$PROJECT.app";
    /usr/bin/ditto "$STAGE/$PROJECT.app" "$PROJECT.app";
fi

# Optional, so a missing dmgbuild never fails the build.
if command -v dmgbuild >/dev/null 2>&1; then
    rm -f "$PROJECT.dmg";

    # On a release run the DMG is packed from the staged copy: nothing that was
    # cleaned off it can come back before it goes in.
    DMG_SRC="$PROJECT.app";
    if [ -n "${RELEASE:-}" ]; then DMG_SRC="$STAGE/$PROJECT.app"; fi

    dmgbuild \
        -s "../installer-assets/macos/dmgbuild-config.py" \
        -D "app=$DMG_SRC" \
        "nikita-qflipper2.zero" \
        "$PROJECT.dmg";

    # The DMG is a new file and carries none of the .app's signature. Left
    # unsigned it is the disk image Gatekeeper complains about, even though the
    # app inside it is fine.
    if [ -n "${RELEASE:-}" ]; then
        codesign --force --timestamp -s "$SIGN_ID" -v "$PROJECT.dmg";
        xcrun notarytool submit --keychain-profile "$PROFILE" --wait "$PROJECT.dmg";
        xcrun stapler staple "$PROJECT.dmg";
        echo "--- Gatekeeper verdict ---";
        spctl --assess --type open --context context:primary-signature -vv "$PROJECT.dmg" || true;
        rm -rf "$STAGE";
    fi

    echo "Built $BUILD_DIRECTORY/$PROJECT.dmg";
else
    echo "dmgbuild not found, skipping DMG. Your app is ready:";
fi
echo "  $BUILD_DIRECTORY/$PROJECT.app   (run:  open $BUILD_DIRECTORY/$PROJECT.app )";
