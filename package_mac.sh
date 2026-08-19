#!/bin/bash
#
# package_mac.sh - build a self-contained, signed, notarized DMG.
#
#   ./package_mac.sh              full run: build, bundle, sign, notarize, DMG
#   ./package_mac.sh --no-build   skip build_mac.sh, package what is there
#   ./package_mac.sh --no-notary  bundle and sign ad-hoc, skip Apple entirely
#
# Needs: a Developer ID in the keychain, a notarytool profile (see NOTARY_PROFILE),
# and dmgbuild (pip3 install --break-system-packages dmgbuild).
#
# Why every step is here, in short: macdeployqt does not work against Homebrew's
# qt_universal (it is a static build), so the bundle is assembled by hand. Each
# phase below fixes something that silently breaks the app if skipped.

set -o pipefail

PROJECT="qFlipper"
BUILD_DIR="build_mac"
STAGE="$HOME/Library/Caches/nikita-pack"
SIGN_ID="${MAC_OS_SIGNING_KEY_ID:-Developer ID Application: Andres Nicolas Alegre (Y76PU2RL9K)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-nikita}"

# Homebrew formulae that ship the Qt frameworks this app links against.
QT_PREFIXES="qtbase qtdeclarative qtsvg qtserialport qt5compat qtshadertools"

# Non-Qt libraries Qt itself pulls in.
LIB_PREFIXES="icu4c@78 pcre2 zstd brotli glib double-conversion libb2 openssl@3 gettext"

NO_BUILD=0
NO_NOTARY=0
for arg in "$@"; do
    case "$arg" in
        --no-build)  NO_BUILD=1 ;;
        --no-notary) NO_NOTARY=1 ;;
        *) echo "unknown option: $arg"; exit 1 ;;
    esac
done

say()  { printf '\n\033[95;1m==> %s\033[0m\n' "$1"; }
ok()   { printf '\033[92m    %s\033[0m\n' "$1"; }
die()  { printf '\033[91;1m!!! %s\033[0m\n' "$1"; exit 1; }

cd "$(dirname "$0")" || die "cannot cd to script directory"

# ---------------------------------------------------------------- build

if [ "$NO_BUILD" -eq 0 ]; then
    say "Building"
    ./build_mac.sh > /tmp/package_build.log 2>&1 || {
        tail -20 /tmp/package_build.log
        die "build failed, full log in /tmp/package_build.log"
    }
    ok "built"
fi

APP="$BUILD_DIR/$PROJECT.app"
[ -d "$APP" ] || die "$APP not found, run without --no-build"
FW="$APP/Contents/Frameworks"
PI="$APP/Contents/PlugIns"
RES="$APP/Contents/Resources"

# ------------------------------------------------------- copy Qt payload

say "Bundling Qt frameworks, plugins and QML modules"

mkdir -p "$FW" "$PI" "$RES"

# cp -a, never cp -R: -R flattens the Versions/Current symlink every framework
# depends on, and dyld then cannot find the binary inside it.
for pfx in $QT_PREFIXES; do
    D="/opt/homebrew/opt/$pfx/lib"
    [ -d "$D" ] || continue
    for fw in "$D"/*.framework; do
        [ -d "$fw" ] && cp -a "$fw" "$FW/"
    done
done

for pfx in $QT_PREFIXES; do
    P="/opt/homebrew/opt/$pfx/share/qt/plugins"
    [ -d "$P" ] && cp -a "$P"/* "$PI/" 2>/dev/null
done

# Static libraries and their metadata ride along in the plugin and QML folders.
# They do nothing at runtime, and codesign fails on them, which takes the whole
# bundle down with it.
find "$PI" -type d -name "objects-*" -exec rm -rf {} + 2>/dev/null
find "$PI" \( -name "*.a" -o -name "*.prl" -o -name "*.o" \) -delete 2>/dev/null

[ -d /opt/homebrew/opt/qtdeclarative/share/qt/qml ] && \
    cp -a /opt/homebrew/opt/qtdeclarative/share/qt/qml "$RES/" 2>/dev/null

# Same for the QML tree, which is copied after the sweep above.
find "$RES/qml" -type d -name "objects-*" -exec rm -rf {} + 2>/dev/null
find "$RES/qml" \( -name "*.a" -o -name "*.prl" -o -name "*.o" \) -delete 2>/dev/null

for pfx in $LIB_PREFIXES; do
    D="/opt/homebrew/opt/$pfx/lib"
    [ -d "$D" ] || continue
    for lib in "$D"/*.dylib; do
        [ -f "$lib" ] && cp -a "$lib" "$FW/" 2>/dev/null
    done
done

# Homebrew ships these read-only, and codesign cannot clear extended attributes
# from a file it may not write to.
chmod -R u+w "$APP"
ok "$(ls "$FW" | grep -c framework) frameworks, $(ls "$FW"/*.dylib 2>/dev/null | wc -l | tr -d ' ') dylibs"

# ------------------------------------------------- rewrite install names

say "Rewriting library paths to @rpath"

rewrite_one() {
    local b="$1"
    [ -f "$b" ] || return 0
    file "$b" 2>/dev/null | grep -q 'Mach-O' || return 0
    case "$b" in
        *.dylib) install_name_tool -id "@rpath/$(basename "$b")" "$b" 2>/dev/null ;;
    esac
    otool -L "$b" 2>/dev/null | grep -E '/opt/homebrew|/usr/local' | awk '{print $1}' | \
    while read -r old; do
        # A framework keeps its Foo.framework/Versions/A/Foo suffix; a loose
        # dylib is referenced by bare filename. Getting this backwards is what
        # makes dyld report a library as missing that is sitting right there.
        case "$old" in
            *.framework/*) new="@rpath/$(echo "$old" | sed 's|.*/lib/||')" ;;
            *)             new="@rpath/$(basename "$old")" ;;
        esac
        install_name_tool -change "$old" "$new" "$b" 2>/dev/null
    done
}

for f in "$FW"/*.dylib; do rewrite_one "$f"; done
for fwdir in "$FW"/*.framework; do
    name=$(basename "$fwdir" .framework)
    rewrite_one "$fwdir/Versions/A/$name"
done
find "$PI" -name '*.dylib' 2>/dev/null | while read -r p; do rewrite_one "$p"; done
find "$RES/qml" -name '*.dylib' 2>/dev/null | while read -r p; do rewrite_one "$p"; done
rewrite_one "$APP/Contents/MacOS/$PROJECT"
rewrite_one "$APP/Contents/MacOS/$PROJECT-cli"

ok "rewritten"

# --------------------------------------------- resolve transitive deps

say "Resolving transitive dependencies"

# Each library dragged in has dependencies of its own (harfbuzz wants graphite2,
# and so on). Loop until a pass adds nothing.
for round in 1 2 3 4 5 6; do
    missing=$(
        for f in "$FW"/*.dylib "$FW"/*.framework/Versions/A/*; do
            [ -f "$f" ] || continue
            otool -L "$f" 2>/dev/null | grep '@rpath/lib' | awk '{print $1}' | sed 's|@rpath/||'
        done | sort -u | while read -r d; do
            [ -f "$FW/$d" ] || echo "$d"
        done
    )

    [ -z "$missing" ] && { ok "round $round: complete"; break; }

    echo "$missing" | while read -r dep; do
        [ -n "$dep" ] || continue
        src=$(find /opt/homebrew/opt/*/lib -maxdepth 1 -name "$dep" 2>/dev/null | head -1)
        [ -z "$src" ] && src=$(find /opt/homebrew/lib -maxdepth 1 -name "$dep" 2>/dev/null | head -1)
        [ -z "$src" ] && { echo "    ? not found: $dep"; continue; }
        # Homebrew's lib dir is mostly symlinks; copy what they point at.
        real=$(python3 -c "import os,sys;print(os.path.realpath(sys.argv[1]))" "$src")
        cp -a "$real" "$FW/$dep" 2>/dev/null && echo "    + $dep"
    done

    chmod -R u+w "$APP"
    for f in "$FW"/*.dylib; do rewrite_one "$f"; done
done

still=$(for f in "$FW"/*.dylib; do
    otool -L "$f" 2>/dev/null | grep '@rpath/lib' | awk '{print $1}' | sed 's|@rpath/||'
done | sort -u | while read -r d; do [ -f "$FW/$d" ] || echo "$d"; done)
[ -n "$still" ] && die "unresolved after 6 rounds: $still"

ext=$(otool -L "$APP/Contents/MacOS/$PROJECT" | grep -c homebrew)
[ "$ext" -ne 0 ] && die "$ext external references remain in the main binary"
ok "self-contained"

# --------------------------------------------------------- stage & sign

say "Staging and signing"

# Something in the build tree keeps putting com.apple.FinderInfo back on the
# bundle directory between the xattr call and codesign, and codesign refuses to
# sign a bundle carrying it. ditto --noextattr hands over a clean copy and the
# cache directory leaves it alone.
rm -rf "$STAGE"
mkdir -p "$STAGE"
/usr/bin/ditto --noextattr --norsrc "$APP" "$STAGE/$PROJECT.app"
chmod -R u+w "$STAGE/$PROJECT.app"

cd "$STAGE" || die "cannot cd to stage"
SAPP="$PROJECT.app"

# Stale signatures survive install_name_tool as invalid pages, and the kernel
# kills the process at launch with CODESIGNING rather than anything readable.
find "$SAPP" -type f \( -name '*.dylib' -o -name '*.so' \) \
    -exec codesign --remove-signature {} \; 2>/dev/null
codesign --remove-signature "$SAPP/Contents/MacOS/$PROJECT" 2>/dev/null
codesign --remove-signature "$SAPP/Contents/MacOS/$PROJECT-cli" 2>/dev/null

if [ "$NO_NOTARY" -eq 1 ]; then
    SIGN_ARGS=(--force -s -)
else
    # --timestamp and --options=runtime are both required for notarization;
    # without them Apple accepts the upload and rejects it minutes later.
    SIGN_ARGS=(--force --timestamp --options=runtime -s "$SIGN_ID")
fi

# Inside out. Note this covers Resources/qml too: every QML module ships its own
# plugin dylib, and one unsigned file there fails the whole submission.
find "$SAPP" -type f \( -name '*.dylib' -o -name '*.so' \) \
    -exec codesign "${SIGN_ARGS[@]}" {} \; 2>/dev/null
find "$SAPP/Contents/Frameworks" -maxdepth 1 -name '*.framework' \
    -exec codesign "${SIGN_ARGS[@]}" {} \; 2>/dev/null
codesign "${SIGN_ARGS[@]}" "$SAPP/Contents/MacOS/$PROJECT-cli" 2>/dev/null
codesign "${SIGN_ARGS[@]}" "$SAPP/Contents/MacOS/$PROJECT" || die "signing the executable failed"
codesign "${SIGN_ARGS[@]}" "$SAPP" || die "signing the bundle failed"

codesign --verify --deep --strict "$SAPP" || die "signature does not verify"
ok "signed and verified"

# ------------------------------------------------------------- notarize

if [ "$NO_NOTARY" -eq 0 ]; then
    say "Notarizing the app (this waits on Apple, several minutes)"
    rm -f "$PROJECT.zip"
    /usr/bin/ditto -c -k --keepParent "$SAPP" "$PROJECT.zip"
    xcrun notarytool submit --keychain-profile "$NOTARY_PROFILE" --wait "$PROJECT.zip" \
        || die "notarization failed; xcrun notarytool log <id> --keychain-profile $NOTARY_PROFILE"
    # Stapling writes the ticket into the bundle. Without it the first launch on
    # an offline machine still asks Apple and shows the unidentified-developer wall.
    xcrun stapler staple "$SAPP" || die "stapling the app failed"
    rm -f "$PROJECT.zip"
    ok "notarized and stapled"
fi

# ------------------------------------------------------------------ DMG

say "Building the DMG"

cd - > /dev/null || exit 1
cd "$BUILD_DIR" || die "cannot cd to $BUILD_DIR"

command -v dmgbuild > /dev/null 2>&1 || die "dmgbuild not installed (pip3 install --break-system-packages dmgbuild)"

VOLNAME="nikita-qflipper2.zero"
rm -f "$PROJECT.dmg"
dmgbuild -s "../installer-assets/macos/dmgbuild-config.py" \
    -D "app=$STAGE/$PROJECT.app" \
    "$VOLNAME" "$PROJECT.dmg" || die "dmgbuild failed"

if [ "$NO_NOTARY" -eq 0 ]; then
    # The DMG is a new file carrying none of the app's signature. Left unsigned
    # it is the disk image Gatekeeper rejects, however clean the app inside is.
    codesign --force --timestamp -s "$SIGN_ID" "$PROJECT.dmg" || die "signing the DMG failed"
    xcrun notarytool submit --keychain-profile "$NOTARY_PROFILE" --wait "$PROJECT.dmg" \
        || die "DMG notarization failed"
    xcrun stapler staple "$PROJECT.dmg" || die "stapling the DMG failed"

    say "Gatekeeper verdict"
    spctl --assess --type open --context context:primary-signature -vv "$PROJECT.dmg"
fi

# Put the signed bundle back so what you run day to day is what ships.
rm -rf "$PROJECT.app"
/usr/bin/ditto "$STAGE/$PROJECT.app" "$PROJECT.app"

say "Done"
ok "$BUILD_DIR/$PROJECT.dmg   ($(du -h "$PROJECT.dmg" | cut -f1))"
ok "$BUILD_DIR/$PROJECT.app"
echo
echo "Before publishing, prove it runs without Homebrew's Qt:"
echo "  sudo mv /opt/homebrew/opt/qtbase /opt/homebrew/opt/qtbase.off"
echo "  $BUILD_DIR/$PROJECT.app/Contents/MacOS/$PROJECT"
echo "  sudo mv /opt/homebrew/opt/qtbase.off /opt/homebrew/opt/qtbase"