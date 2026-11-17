#!/usr/bin/env bash
# Builds the VisIVO VR headset app (issues #255, #383): a QR-code scanner and
# the pairing loader (or, launched by the desktop over USB, the backend's VR
# page) packaged as an immersive Horizon OS app with Meta's Bubblewrap (pinned
# in package.json), the patches of patch-project.py, Gradle, zipalign and
# apksigner. See README.md.
#
# Usage: tools/quest-app/build.sh [--release] [--out DIR] [--loader ORIGIN]
#
#   (default)  the development app (package it.inaf.oact.visivo.vr.dev),
#              signed with this computer's own key, made at the first build
#              in $VISIVO_QUEST_APP_DIR (default ~/.visivo/quest-app). Its
#              fingerprint goes to dev-sha256.txt there, where a backend on
#              this computer finds it for /.well-known/assetlinks.json.
#   --release  the release app (it.inaf.oact.visivo.vr), signed with the
#              project's key: QUEST_APP_KEYSTORE (the keystore file),
#              QUEST_APP_KEYSTORE_PASS_FILE (a file holding its password),
#              QUEST_APP_KEY_ALIAS (default visivo-vr). CI passes them from
#              its secrets; they never enter the repository.
#   --loader   the pairing loader the scanner opens, instead of app.json's
#              loaderOrigin: a development loader, e.g. http://localhost:8790
#              through adb reverse (it must serve /.well-known/assetlinks.json
#              for this build's certificate). Not with --release.
#
# Needs: JAVA_HOME (JDK 17), ANDROID_HOME (an Android SDK; build-tools 34.0.0
# and platform 34 are installed with its sdkmanager when missing), node and
# npm, python3. Writes DIR (default tools/quest-app/out): visivo-vr[-dev].apk,
# visivo-vr[-dev].json (package, version, certificate and APK digests) and
# assetlinks[-dev].json (the statement for the backend).
# Passwords are handed to the tools as files, never on a command line.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
MODE=dev
OUT="$HERE/out"
LOADER=""
while [ $# -gt 0 ]; do
  case "$1" in
    --release) MODE=release ;;
    --out) OUT="$2"; shift ;;
    --loader) LOADER="${2%/}"; shift ;;
    -h|--help) sed -n '2,31p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

die() { echo "quest-app: $*" >&2; exit 1; }
if [ -n "$LOADER" ]; then
  [ "$MODE" = dev ] || die "--loader is for development builds: a release opens app.json's loaderOrigin"
  python3 -c 'import sys; from urllib.parse import urlsplit; u = urlsplit(sys.argv[1]); sys.exit(0 if u.scheme in ("http", "https") and u.hostname and u.path in ("", "/") and not u.query and not u.fragment else 1)' "$LOADER" \
    || die "--loader takes an origin (scheme://host[:port]), not $LOADER"
fi
[ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/java" ] || die "set JAVA_HOME to a JDK 17"
"$JAVA_HOME/bin/java" -version 2>&1 | grep -q 'version "17' || die "JAVA_HOME is not a JDK 17 (Bubblewrap's Gradle build needs 17)"
[ -n "${ANDROID_HOME:-}" ] && [ -d "$ANDROID_HOME" ] || die "set ANDROID_HOME to an Android SDK"
command -v node >/dev/null || die "node is needed (Bubblewrap)"
command -v python3 >/dev/null || die "python3 is needed"

BUILD_TOOLS=34.0.0
PLATFORM=android-34
need=()
[ -x "$ANDROID_HOME/build-tools/$BUILD_TOOLS/apksigner" ] || need+=("build-tools;$BUILD_TOOLS")
[ -d "$ANDROID_HOME/platforms/$PLATFORM" ] || need+=("platforms;$PLATFORM")
if [ ${#need[@]} -gt 0 ]; then
  SDKM="$(ls "$ANDROID_HOME"/cmdline-tools/*/bin/sdkmanager 2>/dev/null | head -1 || true)"
  [ -n "$SDKM" ] || die "missing ${need[*]} and no sdkmanager under $ANDROID_HOME/cmdline-tools"
  echo "installing ${need[*]}"
  yes | "$SDKM" --sdk_root="$ANDROID_HOME" --licenses >/dev/null 2>&1 || true
  "$SDKM" --sdk_root="$ANDROID_HOME" "${need[@]}" >/dev/null
fi
BT="$ANDROID_HOME/build-tools/$BUILD_TOOLS"

# ── The signing key ──────────────────────────────────────────────────────────
KEYTOOL="$JAVA_HOME/bin/keytool"
if [ "$MODE" = release ]; then
  KS="${QUEST_APP_KEYSTORE:-}"
  PASS="${QUEST_APP_KEYSTORE_PASS_FILE:-}"
  ALIAS="${QUEST_APP_KEY_ALIAS:-visivo-vr}"
  [ -n "$KS" ] && [ -f "$KS" ] || die "--release needs QUEST_APP_KEYSTORE (the keystore file)"
  [ -n "$PASS" ] && [ -f "$PASS" ] || die "--release needs QUEST_APP_KEYSTORE_PASS_FILE (a file with its password)"
  NAME=visivo-vr
  LINKS=assetlinks.json
else
  KEYDIR="${VISIVO_QUEST_APP_DIR:-$HOME/.visivo/quest-app}"
  mkdir -p "$KEYDIR"
  chmod 700 "$KEYDIR"
  KS="$KEYDIR/dev.keystore"
  PASS="$KEYDIR/dev.pass"
  ALIAS=visivo-vr-dev
  if [ ! -s "$PASS" ]; then
    (umask 077; head -c 24 /dev/urandom | base64 | tr -d '/+=\n' > "$PASS")
  fi
  if [ ! -f "$KS" ]; then
    echo "making this computer's development key: $KS"
    "$KEYTOOL" -genkeypair -keystore "$KS" -storetype PKCS12 -alias "$ALIAS" -keyalg RSA -keysize 3072 \
      -validity 9125 -dname "CN=VisIVO VR development key, O=VisIVO Visual Analytics" \
      -storepass:file "$PASS" -keypass:file "$PASS" >/dev/null 2>&1 || die "keytool could not make the key"
    chmod 600 "$KS"
  fi
  NAME=visivo-vr-dev
  LINKS=assetlinks-dev.json
fi
CERT_SHA256="$("$KEYTOOL" -list -v -keystore "$KS" -alias "$ALIAS" -storepass:file "$PASS" 2>/dev/null \
  | sed -n 's/^.*SHA256: *\([0-9A-F:]*\).*$/\1/p' | head -1)"
[ ${#CERT_SHA256} -eq 95 ] || die "could not read the key's certificate (alias $ALIAS)"
if [ "$MODE" = dev ]; then
  printf '%s\n' "$CERT_SHA256" > "$KEYDIR/dev-sha256.txt"
fi

# ── Bubblewrap, pinned ───────────────────────────────────────────────────────
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$OUT/work-$MODE"
rm -rf "$WORK"
mkdir -p "$WORK/home/.bubblewrap" "$WORK/proj"
(cd "$HERE" && npm ci --no-audit --no-fund --loglevel=error >/dev/null)
BW="$HERE/node_modules/@meta-quest/bubblewrap-cli/bin/bubblewrap.js"
[ -f "$BW" ] || die "Bubblewrap not installed under $HERE/node_modules"
# Bubblewrap appends Contents/Home to the JDK on macOS.
JDK_PATH="$JAVA_HOME"
case "$(uname -s)" in Darwin) JDK_PATH="${JAVA_HOME%/Contents/Home}" ;; esac
python3 -c 'import json,sys; print(json.dumps({"jdkPath": sys.argv[1], "androidSdkPath": sys.argv[2]}))' \
  "$JDK_PATH" "$ANDROID_HOME" > "$WORK/home/.bubblewrap/config.json"

# Bubblewrap fetches the icons over HTTP: a server on loopback for the build,
# serving the plain icon's directory (the maskable one must sit beside it).
ICON="$REPO/$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["icon"])' "$HERE/app.json")"
MASKABLE="$REPO/$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["maskableIcon"])' "$HERE/app.json")"
[ -f "$ICON" ] || die "icon not found: $ICON"
[ -f "$MASKABLE" ] || die "maskable icon not found: $MASKABLE"
[ "$(cd "$(dirname "$ICON")" && pwd)" = "$(cd "$(dirname "$MASKABLE")" && pwd)" ] \
  || die "app.json's icon and maskableIcon must be in the same directory"
ICON_PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
python3 -m http.server "$ICON_PORT" --bind 127.0.0.1 --directory "$(dirname "$ICON")" >/dev/null 2>&1 &
ICON_PID=$!
trap 'kill "$ICON_PID" 2>/dev/null || true' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do
  python3 -c 'import sys,urllib.request; urllib.request.urlopen(sys.argv[1], timeout=1)' \
    "http://127.0.0.1:$ICON_PORT/$(basename "$ICON")" 2>/dev/null && break
  sleep 0.3
done

ORIGIN="${LOADER:-$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["loaderOrigin"])' "$HERE/app.json")}"
python3 -I "$HERE/make-twa.py" "$WORK/proj" "$MODE" "http://127.0.0.1:$ICON_PORT/$(basename "$ICON")" \
  "http://127.0.0.1:$ICON_PORT/$(basename "$MASKABLE")" "$KS" "$ALIAS" "$ORIGIN"
(cd "$WORK/proj" && HOME="$WORK/home" node "$BW" update --skipVersionUpgrade --manifest="$WORK/proj/twa-manifest.json" \
  < /dev/null > "$WORK/bubblewrap.log" 2>&1) || { tail -30 "$WORK/bubblewrap.log"; die "bubblewrap failed"; }
python3 -I "$HERE/patch-project.py" "$WORK/proj" "$ORIGIN"

# ── Gradle, then align and sign ──────────────────────────────────────────────
(cd "$WORK/proj" && ./gradlew --no-daemon -q assembleRelease > "$WORK/gradle.log" 2>&1) \
  || { tail -40 "$WORK/gradle.log"; die "the Gradle build failed"; }
UNSIGNED="$WORK/proj/app/build/outputs/apk/release/app-release-unsigned.apk"
[ -f "$UNSIGNED" ] || die "no APK from Gradle"
"$BT/zipalign" -f -p 4 "$UNSIGNED" "$WORK/aligned.apk"
"$BT/apksigner" sign --ks "$KS" --ks-key-alias "$ALIAS" --ks-pass "file:$PASS" \
  --out "$OUT/$NAME.apk" "$WORK/aligned.apk"
rm -f "$OUT/$NAME.apk.idsig"
SIGNED_SHA256="$("$BT/apksigner" verify --print-certs "$OUT/$NAME.apk" | sed -n 's/^Signer #1 certificate SHA-256 digest: *//p' | head -1)"
python3 -I - "$HERE/app.json" "$MODE" "$OUT/$NAME.apk" "$CERT_SHA256" "$SIGNED_SHA256" "$OUT/$NAME.json" "$OUT/$LINKS" "$ORIGIN" <<'PY'
import hashlib, json, sys
app_json, mode, apk, cert, signed, info_out, links_out, origin = sys.argv[1:9]
app = json.load(open(app_json, encoding="utf-8"))
if cert.replace(":", "").lower() != signed.lower():
    sys.exit(f"the APK is signed by {signed}, not by the key {cert}")
package = app["packages"]["development" if mode == "dev" else "release"]
info = {"package": package, "version_code": int(app["versionCode"]), "version_name": app["versionName"],
        "mode": mode, "cert_sha256": cert,
        "apk_sha256": hashlib.sha256(open(apk, "rb").read()).hexdigest(),
        "start_path": app["startPath"], "default_origin": origin.rstrip("/"),
        "loader_url": origin.rstrip("/") + app["startPath"]}
json.dump(info, open(info_out, "w", encoding="utf-8"), indent=2)
statement = [{"relation": ["delegate_permission/common.handle_all_urls"],
              "target": {"namespace": "android_app", "package_name": package, "sha256_cert_fingerprints": [cert]}}]
json.dump(statement, open(links_out, "w", encoding="utf-8"), indent=2)
print(f"{apk}: {package} {info['version_name']} ({info['version_code']}), certificate {cert}")
PY
