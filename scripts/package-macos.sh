#!/usr/bin/env bash
#
# Build, bundle, sign and package VisIVOVisualAnalytics.app into a DMG.
#
#   scripts/package-macos.sh [build-dir] [staging-dir]
#
# Defaults: build-release, dist-mac. The DMG lands in ~/Downloads unless
# VISIVO_DMG_OUT says otherwise.
#
# ── Why this script exists ──────────────────────────────────────────────────
#
# Four things about packaging this app are not discoverable from the CMake
# files, and each one has cost a working day at least once. They are all
# ordering or verification rules, so they live here rather than in a comment
# nobody reads:
#
#   1. The Python runtime must be OUT of the bundle while `cmake --install`
#      runs, and go back in AFTERWARDS.
#
#      fixup_bundle walks every Mach-O under the .app, and the bundled
#      interpreter declares `@executable_path/../lib/libpython3.12.dylib`.
#      That `@executable_path` is the *interpreter's* directory, not the app's,
#      so otool cannot resolve it from where fixup_bundle stands and the whole
#      install aborts with "otool -l failed: 1". Nothing in the error names
#      Python.
#
#   2. Therefore the bundle must be re-signed by hand after the runtime goes
#      back. The install's own codesign pass ran before the runtime existed,
#      and a signature does not cover files that appear later.
#
#   3. A dangling symlink anywhere under the .app makes codesign fail with
#      "No such file or directory" and no path. Ask before signing, not after.
#      This bit us when a spare copy of the runtime was kept in a scratch
#      directory that got cleaned: the regular files went, the symlinks stayed,
#      and `bin/python3` pointed at nothing.
#
#   4. hdiutil returns before the DMG is fully written. A package copied while
#      it is still growing is silently corrupt. Wait for the size to hold
#      steady, then verify by MOUNTING the image — never by checking the
#      staging tree, which is not what anybody will download.
#
# ── The Python runtime ──────────────────────────────────────────────────────
#
# It is ~1 GB (CPython 3.12 plus the backend's dependencies: astropy, numpy,
# healpy, dask, …) and is NOT built here and NOT in the repository. Point the
# script at one with VISIVO_PYTHON_RUNTIME, or leave a copy at
# ~/.visivo-build/keep-python and it will be found. Keep that copy somewhere
# durable — not in a temporary directory (see 3 above).
#
# To make one from scratch: install a relocatable CPython 3.12 (python.org
# framework build or `uv python install`), then
#   <runtime>/bin/python3 -m pip install -r backend/requirements.txt
# and hand the resulting prefix to this script.

set -euo pipefail

BUILD_DIR="${1:-build-release}"
STAGING_DIR="${2:-dist-mac}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

APP_NAME="VisIVOVisualAnalytics"
APP="$STAGING_DIR/$APP_NAME.app"
RUNTIME_DEST="$APP/Contents/Resources/python"
DMG_OUT="${VISIVO_DMG_OUT:-$HOME/Downloads/$APP_NAME-macos-arm64.dmg}"
SIGN_ID="${VISIVO_CODESIGN_ID:--}"
HOLDING_AREA="${VISIVO_PYTHON_HOLD:-$HOME/.visivo-build/python-staging}"

step()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die()   { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "this script packages a macOS bundle"

# ── Locate the Python runtime ───────────────────────────────────────────────

RUNTIME_SRC=""
if [ -n "${VISIVO_PYTHON_RUNTIME:-}" ]; then
    RUNTIME_SRC="$VISIVO_PYTHON_RUNTIME"
    [ -x "$RUNTIME_SRC/bin/python3" ] \
        || die "VISIVO_PYTHON_RUNTIME=$RUNTIME_SRC has no bin/python3"
elif [ -x "$RUNTIME_DEST/bin/python3" ]; then
    # Already staged from a previous run: it gets moved aside and back.
    RUNTIME_SRC="$RUNTIME_DEST"
elif [ -x "$HOME/.visivo-build/keep-python/bin/python3" ]; then
    RUNTIME_SRC="$HOME/.visivo-build/keep-python"
else
    die "no Python runtime found. Set VISIVO_PYTHON_RUNTIME=<prefix>, or keep
       one at ~/.visivo-build/keep-python. See the header of this script for
       how to build one."
fi
step "Python runtime: $RUNTIME_SRC ($("$RUNTIME_SRC/bin/python3" -V 2>&1))"

# Rule 3, applied to the source before anything expensive happens.
DANGLING="$(find "$RUNTIME_SRC" -type l ! -exec test -e {} \; -print 2>/dev/null | head -20)"
[ -z "$DANGLING" ] || die "the runtime has dangling symlinks — codesign will
       fail later with an unhelpful message. Restore it from a working copy.
$DANGLING"

# ── Build ───────────────────────────────────────────────────────────────────

step "Building ($BUILD_DIR)"
[ -d "$BUILD_DIR" ] || die "$BUILD_DIR does not exist; configure it first"
cmake --build "$BUILD_DIR" --parallel

# ── Install, with the runtime held outside the bundle (rule 1) ──────────────

if [ "$RUNTIME_SRC" = "$RUNTIME_DEST" ]; then
    step "Moving the staged runtime out of the bundle for the install"
    rm -rf "$HOLDING_AREA"
    mkdir -p "$(dirname "$HOLDING_AREA")"
    mv "$RUNTIME_DEST" "$HOLDING_AREA"
    RUNTIME_SRC="$HOLDING_AREA"
    RESTORE_BY_MOVE=1
else
    rm -rf "$RUNTIME_DEST"
    RESTORE_BY_MOVE=0
fi

step "Installing into $STAGING_DIR (fixup_bundle, plugin repair, first signature)"
cmake --install "$BUILD_DIR" --prefix "$REPO_ROOT/$STAGING_DIR"
[ -d "$APP" ] || die "$APP was not produced"

# ── Put the runtime back, then re-sign (rules 1 and 2) ──────────────────────

step "Restoring the Python runtime into the bundle"
if [ "$RESTORE_BY_MOVE" = "1" ]; then
    mv "$RUNTIME_SRC" "$RUNTIME_DEST"
else
    ditto "$RUNTIME_SRC" "$RUNTIME_DEST"
fi

# Byte-compiled caches from any test run are unsigned clutter inside a signed
# bundle; they also differ between builds for no reason.
find "$APP/Contents/Resources/backend" -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null || true

step "Checking for dangling symlinks before signing (rule 3)"
DANGLING="$(find "$APP" -type l ! -exec test -e {} \; -print 2>/dev/null | head -20)"
[ -z "$DANGLING" ] || die "dangling symlinks inside the bundle:
$DANGLING"

step "Re-signing the whole bundle (identity: $SIGN_ID)"
codesign --force --deep --sign "$SIGN_ID" --timestamp=none "$APP"
codesign --verify --deep --strict "$APP" || die "signature does not verify"

# ── Verify the bundle is self-contained ─────────────────────────────────────

step "Checking the executable's rpaths and dependencies"
otool -l "$APP/Contents/MacOS/$APP_NAME" | awk '/LC_RPATH/{f=1} f&&/path /{print "    rpath: "$2; f=0}'
EXTERNAL="$(otool -L "$APP/Contents/MacOS/$APP_NAME" | tail -n +2 \
            | grep -v '@executable_path\|/usr/lib/\|/System/' || true)"
[ -z "$EXTERNAL" ] || die "the executable depends on files outside the bundle:
$EXTERNAL"
echo "    no dependency outside the bundle"

# ── Package, and wait for it (rule 4) ───────────────────────────────────────

step "Creating $DMG_OUT"
rm -f "$DMG_OUT"
hdiutil create -volname "VisIVO Visual Analytics" -srcfolder "$APP" \
        -ov -format UDZO "$DMG_OUT" >/dev/null

printf '    waiting for the image to stop growing'
previous=0
while :; do
    sleep 4
    current="$(stat -f%z "$DMG_OUT")"
    printf '.'
    [ "$current" = "$previous" ] && [ "$current" -gt 0 ] && break
    previous="$current"
done
printf ' %s\n' "$(du -h "$DMG_OUT" | cut -f1)"

# ── Verify what will actually be downloaded (rule 4) ────────────────────────

step "Verifying the mounted image"
MOUNT_OUT="$(hdiutil attach -nobrowse -readonly "$DMG_OUT")"
MOUNT_POINT="$(echo "$MOUNT_OUT" | grep -o '/Volumes/.*' | tail -1)"
[ -n "$MOUNT_POINT" ] || die "could not mount $DMG_OUT"
trap 'hdiutil detach "$MOUNT_POINT" >/dev/null 2>&1 || true' EXIT

MOUNTED_APP="$MOUNT_POINT/$APP_NAME.app"
codesign --verify --deep --strict "$MOUNTED_APP" \
    || die "the signature does not verify from the mounted image"
echo "    signature valid"

"$MOUNTED_APP/Contents/Resources/python/bin/python3" -c 'import astropy, numpy, fastapi' \
    || die "the bundled interpreter cannot import the backend's dependencies"
echo "    bundled interpreter imports astropy, numpy, fastapi"

[ -f "$MOUNTED_APP/Contents/Resources/backend/app/main.py" ] \
    || die "the backend is missing from the bundle"
echo "    backend present"

step "Done: $DMG_OUT"
ls -lh "$DMG_OUT"
