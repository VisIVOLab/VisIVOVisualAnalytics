#!/usr/bin/env python3
"""Adjusts the Android project Meta's Bubblewrap generated for the VisIVO VR
headset app (issues #254, #255, #383).

Usage: patch-project.py <project_dir> <origin>

<origin> is the loader's origin (app.json loaderOrigin): what the app opens
after a QR code is read. The desktop's USB launch names its own backend
page on localhost in the intent.

1. The root build.gradle's jcenter() becomes mavenCentral() (Meta's
   packaging guide; jcenter is read-only).
2. The launch URL and the asset statement use the origin's own scheme: the
   template hardcodes https://, and a development loader may be
   http://localhost:<port> through adb reverse.
3. The VIEW intent filter names the host and the port separately: an
   Android intent filter cannot take "host:port".
4. LauncherActivity (the immersive part, a Trusted Web Activity) opens:
   - a VR page of a backend on the headset's own localhost (any port, any
     path prefix): the desktop's USB launch, whose URL carries a one-time
     ticket, so it never calls the library's getLaunchingUrl(), which
     writes the launch URL to logcat;
   - the loader with a pairing code (#pair=): the scanner's launch, or the
     desktop's QR code (https://<loader>/#pair=<code>) read by the
     headset's own QR scanner and handed to the app as a verified link
     (the VIEW filter keeps autoVerify); either opens as the app's start
     URL with that code;
   and nothing else: launched without one of those (from the app's icon
   when Horizon OS uses the LAUNCHER entry, or by another app: the activity
   is exported) it opens the scanner in Home instead and goes.
   When it comes back on top (the browser's activity above it ended with
   its VR session) it goes: after a USB launch the app quits, as before;
   after the scanner's launch it opens the scanner again (in Home), so
   leaving VR returns to the scanner. It is translucent, so the library's
   own finish() in onRestart() never runs.
5. The manifest: ScanActivity, a 2-D panel (category
   com.oculus.intent.category.2D), reads the desktop's QR code with the
   headset's passthrough camera (android.permission.CAMERA and
   horizonos.permission.HEADSET_CAMERA, asked for at its first start) or
   takes the code typed. LauncherActivity stays the launcher activity (MAIN,
   LAUNCHER and com.oculus.intent.category.VR): the Meta Horizon Store
   refuses an immersive web app whose launcher activity lacks the VR
   category (issue #386), and it gets excludeFromRecents (a launch activity
   must have it). ScanActivity is what Home opens when the user selects the
   app there (com.oculus.intent.category.VR_HOME_LAUNCHER, the last category
   of its filter, app.json panelHomeLauncher), so no immersive activity
   starts just to hand over to the panel; where Horizon OS uses the LAUNCHER
   entry instead, LauncherActivity hands over (4.). The app declares Horizon
   OS SDK 69 (hybrid apps: a panel and an immersive activity in one APK).
6. ScanActivity.java and PairCode.java (android/, next to this script) go
   into the app's package; ZXing's core (the QR decoder, Apache-2.0) is a
   dependency.
7. The Meta Horizon Store's manifest rules (issue #386; app.json, checked on
   the built APK by check-store.py): compileSdk and targetSdk 34 (the
   template's Quest branch has 32; apps created since 2026-03-01 must target
   34), installLocation auto, com.oculus.supportedDevices, head tracking
   required (an immersive app), a network security configuration (no clear
   text: the app itself has no network permission, the browser loads the
   pages). An explicit filter matches the desktop's USB launch (VIEW on
   http(s)://localhost or 127.0.0.1): an app targeting Android 13 or later
   may refuse an explicit intent from another app, adb's shell included,
   that matches none of the activity's filters. It has no BROWSABLE, so no
   web page link opens the app.
Each patch fails loudly when the template no longer has what it replaces.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from urllib.parse import urlsplit

HERE = Path(__file__).resolve().parent
APP = HERE / "app.json"
ZXING = "com.google.zxing:core:3.5.3"


def sub(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise SystemExit(f"{path}: '{old[:70]}' not found: the Bubblewrap template changed")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


LAUNCHER_METHODS = '''    /** What this launch opens (shouldLaunchImmediately decides). */
    private Uri chosen;
    /** Opened by the scanner: leaving VR opens the scanner again. */
    private boolean fromScanner;

    @Override
    protected boolean shouldLaunchImmediately() {
        // The address of the icon launches of app versions before #383, no longer used.
        getSharedPreferences("visivo-vr", MODE_PRIVATE).edit().remove("last_page").apply();
        Uri uri = getIntent() != null ? getIntent().getData() : null;
        String url = uri != null ? uri.toString() : null;
        if (url != null && PairCode.isBackendPage(url)) {
            chosen = uri;                  // the desktop's USB launch (#255)
            return true;
        }
        // The loader with a code: the scanner's launch, or the desktop's QR
        // code read by the headset's own scanner and handed over as a
        // verified link (#383). Opened as the app's own start URL with that
        // code, so the loader pairs and enters VR by itself.
        String loader = url != null ? PairCode.loaderLaunchFor(url, getString(R.string.launchUrl)) : null;
        if (loader != null) {
            chosen = Uri.parse(loader);
            fromScanner = true;
            return true;
        }
        // Anything else: the scanner, never an address another app chose.
        openScanner();
        finish();
        return false;
    }

    @Override
    protected Uri getLaunchingUrl() {
        // Not super.getLaunchingUrl(): it writes the launch URL to logcat, and
        // the desktop's launch URL carries a one-time ticket (issue #255).
        return chosen != null ? chosen : Uri.parse(getString(R.string.launchUrl));
    }

    private boolean resumedOnce;

    @Override
    protected void onResume() {
        super.onResume();
        // Back on top: the browser's activity above it is gone (its VR session
        // ended), so this one goes too and no transparent activity is left
        // over the room (Quest 3, issue #255). Opened by the scanner, the
        // scanner comes back (issue #383).
        if (resumedOnce) {
            if (fromScanner) openScanner();
            finish();
        }
        resumedOnce = true;
    }

    /** The scanner, a 2-D panel, in Home (Horizon OS hybrid apps: an
     *  immersive activity opens a panel through Home). */
    private void openScanner() {
        Intent panel = new Intent(this, ScanActivity.class);
        panel.setAction(Intent.ACTION_MAIN);
        panel.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            PendingIntent pending = PendingIntent.getActivity(this, 0, panel,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            Intent home = new Intent(Intent.ACTION_MAIN);
            home.addCategory(Intent.CATEGORY_HOME);
            home.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            home.putExtra("extra_launch_in_home_pending_intent", pending);
            startActivity(home);
        } catch (RuntimeException e) {
            startActivity(panel);
        }
    }
'''

SCAN_ACTIVITY = '''        <!-- The scanner (issues #383, #386): reads the desktop's QR code with the
             headset's passthrough camera, then opens LauncherActivity. What Home
             opens when the app is selected there (VR_HOME_LAUNCHER, the filter's last
             category); LauncherActivity is the launcher activity the Store asks for. -->
        <activity android:name="ScanActivity"
            android:label="@string/launcherName"
            android:exported="true"
            android:theme="@android:style/Theme.DeviceDefault.NoActionBar"
            android:screenOrientation="landscape"
            android:taskAffinity="${applicationId}.scan"
            android:excludeFromRecents="true"
            android:configChanges="screenSize|screenLayout|orientation|keyboardHidden|keyboard|navigation|uiMode">
            <layout android:defaultWidth="1024dp" android:defaultHeight="600dp" />
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.DEFAULT" />
                <category android:name="com.oculus.intent.category.2D" />
__HOME_LAUNCHER__            </intent-filter>
        </activity>

'''
HOME_LAUNCHER = '                <category android:name="com.oculus.intent.category.VR_HOME_LAUNCHER" />\n'

MANIFEST_HEAD = '''    <!-- The scanner (issue #383): the passthrough camera reads the desktop's QR code. -->
    <uses-permission android:name="android.permission.CAMERA" />
    <uses-permission android:name="horizonos.permission.HEADSET_CAMERA" />
    <uses-feature android:name="android.hardware.camera" android:required="false" />
    <!-- A 2-D panel and an immersive activity in one app (hybrid, Horizon OS v69+). -->
    <horizonos:uses-horizonos-sdk horizonos:minSdkVersion="69" horizonos:targetSdkVersion="69" />

'''


NETWORK_SECURITY_CONFIG = '''<?xml version="1.0" encoding="utf-8"?>
<!-- VisIVO VR (issue #386): the app itself opens no connection (it has no
     network permission; the headset's browser loads the pages), so clear
     text is refused everywhere. -->
<network-security-config>
    <base-config cleartextTrafficPermitted="false" />
</network-security-config>
'''

USB_LAUNCH_FILTER = '''            <!-- The desktop's USB launch (issue #255): am start -a VIEW -n <app>/.LauncherActivity
                 -d http://localhost:<port>/.../vr/... An app targeting Android 13+ may
                 refuse an explicit intent from another app (adb's shell) that matches none
                 of the activity's filters (issue #386). No BROWSABLE: no web link opens it. -->
            <intent-filter>
                <action android:name="android.intent.action.VIEW" />
                <category android:name="android.intent.category.DEFAULT" />
                <data android:scheme="http" />
                <data android:scheme="https" />
                <data android:host="localhost" />
                <data android:host="127.0.0.1" />
            </intent-filter>

'''

SUPPORTED_DEVICES = re.compile(r"^(quest2|questpro|quest3|quest3s|vrglasses)(\|(quest2|questpro|quest3|quest3s|vrglasses))*$")


def store_settings() -> tuple[int, str]:
    """targetSdkVersion and com.oculus.supportedDevices from app.json."""
    app = json.loads(APP.read_text(encoding="utf-8"))
    target = int(app["targetSdkVersion"])
    devices = str(app["supportedDevices"])
    if not SUPPORTED_DEVICES.match(devices):
        raise SystemExit(f"app.json supportedDevices: {devices!r} is not a |-list of Meta's device names")
    return target, devices


def patch_store(proj: Path, manifest: Path, target: int, devices: str) -> None:
    gradle = proj / "app" / "build.gradle"
    text = gradle.read_text(encoding="utf-8")
    for key in ("compileSdkVersion", "targetSdkVersion"):
        found = re.findall(rf"^\s*{key} \d+\s*$", text, re.M)
        if len(found) != 1:
            raise SystemExit(f"{gradle}: expected one '{key} <n>' line, found {len(found)}: the Bubblewrap template changed")
        text = re.sub(rf"^(\s*){key} \d+(\s*)$", rf"\g<1>{key} {target}\g<2>", text, count=1, flags=re.M)
    gradle.write_text(text, encoding="utf-8")

    sub(manifest, '<manifest xmlns:android="http://schemas.android.com/apk/res/android"\n',
        '<manifest xmlns:android="http://schemas.android.com/apk/res/android"\n'
        '    android:installLocation="auto"\n')
    sub(manifest, 'android:name="android.hardware.vr.headtracking"\n        android:required="false"',
        'android:name="android.hardware.vr.headtracking"\n        android:required="true"')
    sub(manifest, '    <application\n        android:name="Application"\n',
        '    <application\n        android:name="Application"\n'
        '        android:networkSecurityConfig="@xml/network_security_config"\n')
    sub(manifest, '        <activity android:name="ScanActivity"',
        '        <!-- The headsets the Store offers the app to (app.json supportedDevices). -->\n'
        f'        <meta-data android:name="com.oculus.supportedDevices" android:value="{devices}" />\n\n'
        '        <activity android:name="ScanActivity"')
    sub(manifest, '            <intent-filter android:autoVerify="true">',
        USB_LAUNCH_FILTER + '            <intent-filter android:autoVerify="true">')
    xml = proj / "app" / "src" / "main" / "res" / "xml"
    xml.mkdir(parents=True, exist_ok=True)
    (xml / "network_security_config.xml").write_text(NETWORK_SECURITY_CONFIG, encoding="utf-8")


def patch_launcher(proj: Path) -> Path:
    launcher = next(proj.glob("app/src/main/java/**/LauncherActivity.java"), None)
    if launcher is None:
        raise SystemExit("LauncherActivity.java not found: the Bubblewrap template changed")
    src = launcher.read_text(encoding="utf-8")
    method = re.compile(r"    @Override\n    protected Uri getLaunchingUrl\(\) \{\n.*?return uri;\n    \}\n", re.S)
    if not method.search(src) or "super.getLaunchingUrl()" not in src:
        raise SystemExit("LauncherActivity.getLaunchingUrl not found: the Bubblewrap template changed")
    src = method.sub(lambda _m: LAUNCHER_METHODS, src, count=1)
    if "import android.net.Uri;" not in src:
        raise SystemExit("LauncherActivity's imports changed: the Bubblewrap template changed")
    src = src.replace("import android.net.Uri;",
                      "import android.app.PendingIntent;\nimport android.content.Intent;\nimport android.net.Uri;", 1)
    if "super.getLaunchingUrl()" in src.split("// Not super.getLaunchingUrl()")[0] or "PendingIntent;" not in src:
        raise SystemExit("LauncherActivity not patched")
    launcher.write_text(src, encoding="utf-8")
    return launcher


def panel_home_launcher() -> bool:
    """app.json panelHomeLauncher: Home opens the scanner panel directly."""
    value = json.loads(APP.read_text(encoding="utf-8")).get("panelHomeLauncher", True)
    if not isinstance(value, bool):
        raise SystemExit(f"app.json panelHomeLauncher: {value!r} is not true or false")
    return value


def patch_manifest(manifest: Path, scheme: str, host: str, port: int | None, home_launcher: bool = True) -> None:
    # 3.
    port_attr = f'\n                    android:port="{port}"' if port else ""
    sub(manifest, '<data android:scheme="https"\n                    android:host="@string/hostName"',
        f'<data android:scheme="{scheme}"\n                    android:host="{host}"{port_attr}')
    # 5.
    sub(manifest, '<manifest xmlns:android="http://schemas.android.com/apk/res/android"',
        '<manifest xmlns:android="http://schemas.android.com/apk/res/android"\n'
        '    xmlns:horizonos="http://schemas.horizonos/sdk"')
    sub(manifest, "    <application\n", MANIFEST_HEAD + "    <application\n")
    text = manifest.read_text(encoding="utf-8")
    launcher_filter = re.compile(
        r'(<activity android:name="LauncherActivity".*?<intent-filter>\s*'
        r'<action android:name="android\.intent\.action\.MAIN" />\s*)'
        r'<category android:name="android\.intent\.category\.LAUNCHER" />'
        r'(\s*<category android:name="com\.oculus\.intent\.category\.VR" />)', re.S)
    # LauncherActivity's filter (MAIN, LAUNCHER, VR) stays as it is: the Store
    # wants the VR category on the launcher activity of an immersive web app.
    if not launcher_filter.search(text):
        raise SystemExit("LauncherActivity's launcher intent filter (MAIN, LAUNCHER, VR) not found: "
                         "the Bubblewrap template changed")
    start = '        <activity android:name="LauncherActivity"\n'
    if text.count(start) != 1:
        raise SystemExit("LauncherActivity's element not found: the Bubblewrap template changed")
    text = text.replace(start, start + '            android:excludeFromRecents="true"\n', 1)
    anchor = '        <activity android:name="com.meta.androidbrowserhelper.trusted.FocusActivity" />'
    if anchor not in text:
        raise SystemExit("FocusActivity not found in the manifest: the Bubblewrap template changed")
    scan = SCAN_ACTIVITY.replace("__HOME_LAUNCHER__", HOME_LAUNCHER if home_launcher else "", 1)
    text = text.replace(anchor, scan + anchor, 1)
    if text.count("android.intent.category.LAUNCHER") != 1:
        raise SystemExit("the manifest must have exactly one launcher activity (LauncherActivity)")
    manifest.write_text(text, encoding="utf-8")


def add_sources(launcher: Path) -> None:
    m = re.search(r"^package ([A-Za-z0-9_.]+);", launcher.read_text(encoding="utf-8"), re.M)
    if not m:
        raise SystemExit("LauncherActivity has no package line")
    for name in ("ScanActivity.java", "PairCode.java"):
        text = (HERE / "android" / name).read_text(encoding="utf-8")
        if "package __PACKAGE__;" not in text:
            raise SystemExit(f"android/{name} has no 'package __PACKAGE__;' line")
        (launcher.parent / name).write_text(text.replace("package __PACKAGE__;", f"package {m.group(1)};", 1),
                                            encoding="utf-8")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    proj = Path(argv[1])
    origin = argv[2].rstrip("/")
    u = urlsplit(origin)
    scheme, host, port = u.scheme, u.hostname, u.port
    if scheme not in ("http", "https") or not host:
        raise SystemExit(f"not an http(s) origin: {origin}")

    # 1.
    sub(proj / "build.gradle", "jcenter()", "mavenCentral()")

    # 2.
    sub(proj / "app" / "build.gradle", 'def launchUrl = "https://" + twaManifest.hostName',
        f'def launchUrl = "{scheme}://" + twaManifest.hostName')
    strings = proj / "app" / "src" / "main" / "res" / "values" / "strings.xml"
    text = strings.read_text(encoding="utf-8")
    new = re.sub(r'\\"site\\": \\"https://[^\\]*\\"', f'\\\\"site\\\\": \\\\"{origin}\\\\"', text)
    if f'\\"site\\": \\"{origin}\\"' not in new:
        raise SystemExit("asset statement not found: the Bubblewrap template changed")
    strings.write_text(new, encoding="utf-8")

    # 3. and 5.
    manifest = proj / "app" / "src" / "main" / "AndroidManifest.xml"
    patch_manifest(manifest, scheme, host, port, panel_home_launcher())

    # 7. (after 5.: the supportedDevices entry goes before ScanActivity)
    patch_store(proj, manifest, *store_settings())

    # 4.
    launcher = patch_launcher(proj)

    # 6.
    add_sources(launcher)
    sub(proj / "app" / "build.gradle", "implementation 'com.meta.androidbrowserhelper:androidbrowserhelper:2.5.0'",
        f"implementation 'com.meta.androidbrowserhelper:androidbrowserhelper:2.5.0'\n    implementation '{ZXING}'")
    print(f"patched {proj.name} for {origin}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
