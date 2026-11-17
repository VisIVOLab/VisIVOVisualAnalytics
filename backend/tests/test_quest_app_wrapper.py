"""The VisIVO VR headset app's wrapper (issue #383): the patches applied to
Bubblewrap's Android project (a QR-code scanner panel, the launcher's
addresses), the app's identity in app.json, the Digital Asset Links
statement the loader's host publishes, and PairCode's own checks (Java, run
when a JDK is there).

The project is a stand-in with the template's lines the patches look for;
the real build is tools/quest-app/build.sh."""
from __future__ import annotations

import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / "tools" / "quest-app"

pytestmark = pytest.mark.skipif(not (QUEST / "patch-project.py").is_file(), reason="not a source checkout")


def _load(name: str, file: str):
    spec = importlib.util.spec_from_file_location(name, QUEST / file)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod          # dataclasses look their module up there
    spec.loader.exec_module(mod)
    return mod


MANIFEST = '''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="it.example.vr">
    <uses-permission android:name="com.oculus.permission.HAND_TRACKING" />

    <uses-feature
        android:name="android.hardware.vr.headtracking"
        android:required="false"
        android:version="1" />

    <application
        android:name="Application"
        android:theme="@android:style/Theme.Translucent.NoTitleBar">

        <activity android:name="LauncherActivity"
            android:alwaysRetainTaskState="true"
            android:exported="true">
            <meta-data android:name="horizonos.pwa.APP_MODE" android:value="@string/horizonOSAppMode" />

            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />

                    <category android:name="com.oculus.intent.category.VR" />


            </intent-filter>

            <intent-filter android:autoVerify="true">
                <action android:name="android.intent.action.VIEW"/>
                <category android:name="android.intent.category.DEFAULT" />
                <category android:name="android.intent.category.BROWSABLE"/>
                <data android:scheme="https"
                    android:host="@string/hostName"
                />
            </intent-filter>
        </activity>

        <activity android:name="com.meta.androidbrowserhelper.trusted.FocusActivity" />
    </application>
</manifest>
'''

LAUNCHER = '''package it.example.vr;

import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;

public class LauncherActivity
        extends com.meta.androidbrowserhelper.trusted.LauncherActivity {

    @Override
    protected Uri getLaunchingUrl() {
        // Get the original launch Url.
        Uri uri = super.getLaunchingUrl();
        return uri;
    }
}
'''

STRINGS = '''<resources>
  <string name="assetStatements">
    [{
        \\"relation\\": [\\"delegate_permission/common.handle_all_urls\\"],
        \\"target\\": {
            \\"namespace\\": \\"web\\",
            \\"site\\": \\"https://vr.example.org\\"
        }
    }]
  </string>
</resources>
'''

APP_GRADLE = '''android {

        compileSdkVersion 32

    defaultConfig {
        minSdkVersion 32

            targetSdkVersion 32

        def launchUrl = "https://" + twaManifest.hostName + twaManifest.launchUrl
    }
}
dependencies {
    implementation fileTree(include: ['*.jar'], dir: 'libs')
        implementation 'com.meta.androidbrowserhelper:androidbrowserhelper:2.5.0'
}
'''


def _project(tmp_path: Path) -> Path:
    proj = tmp_path / "proj"
    java = proj / "app" / "src" / "main" / "java" / "it" / "example" / "vr"
    java.mkdir(parents=True)
    (java / "LauncherActivity.java").write_text(LAUNCHER, encoding="utf-8")
    res = proj / "app" / "src" / "main" / "res" / "values"
    res.mkdir(parents=True)
    (res / "strings.xml").write_text(STRINGS, encoding="utf-8")
    (proj / "app" / "src" / "main" / "AndroidManifest.xml").write_text(MANIFEST, encoding="utf-8")
    (proj / "app" / "build.gradle").write_text(APP_GRADLE, encoding="utf-8")
    (proj / "build.gradle").write_text("repositories { jcenter() }\n", encoding="utf-8")
    return proj


def _patch(proj: Path, origin: str) -> None:
    mod = _load("quest_patch_project", "patch-project.py")
    assert mod.main(["patch-project.py", str(proj), origin]) == 0


def test_the_icon_opens_the_scanner_and_the_launcher_stays_immersive(tmp_path: Path) -> None:
    proj = _project(tmp_path)
    _patch(proj, "https://vr.example.org")
    m = (proj / "app" / "src" / "main" / "AndroidManifest.xml").read_text(encoding="utf-8")
    # One launcher icon: the scanner, a 2-D panel with its own task.
    assert m.count("android.intent.category.LAUNCHER") == 1
    scan = m[m.index('<activity android:name="ScanActivity"'):m.index("FocusActivity")]
    assert "android.intent.category.LAUNCHER" in scan and "com.oculus.intent.category.2D" in scan
    assert 'android:taskAffinity="${applicationId}.scan"' in scan
    # The launcher keeps its immersive category, now without the icon.
    launcher = m[m.index('<activity android:name="LauncherActivity"'):m.index('<activity android:name="ScanActivity"')]
    assert "com.oculus.intent.category.VR" in launcher and "android.intent.category.LAUNCHER" not in launcher
    assert "android.intent.category.DEFAULT" in launcher.split("<intent-filter>")[1]
    # The camera, and the hybrid app's SDK level.
    for line in ('android.permission.CAMERA', 'horizonos.permission.HEADSET_CAMERA',
                 'xmlns:horizonos="http://schemas.horizonos/sdk"', 'horizonos:minSdkVersion="69"'):
        assert line in m, line
    # The verified link: the desktop's QR code (https://<loader>/#pair=…) opens the app.
    assert '<data android:scheme="https"\n                    android:host="vr.example.org"\n' in m
    assert 'android:autoVerify="true"' in m


def test_the_launcher_opens_only_backend_pages_and_the_loader_with_a_code(tmp_path: Path) -> None:
    proj = _project(tmp_path)
    _patch(proj, "https://vr.example.org")
    java = proj / "app" / "src" / "main" / "java" / "it" / "example" / "vr"
    src = (java / "LauncherActivity.java").read_text(encoding="utf-8")
    assert "super.getLaunchingUrl()" not in src.split("// Not super.getLaunchingUrl()")[0]
    assert "protected boolean shouldLaunchImmediately()" in src
    assert "PairCode.isBackendPage(url)" in src and "PairCode.loaderLaunchFor(url" in src
    assert 'putExtra("extra_launch_in_home_pending_intent"' in src
    assert "if (fromScanner) openScanner();" in src
    assert "import android.app.PendingIntent;" in src and "import android.content.Intent;" in src
    # The scanner's sources, in the app's package; ZXing's decoder; mavenCentral.
    for name in ("ScanActivity.java", "PairCode.java"):
        text = (java / name).read_text(encoding="utf-8")
        assert text.startswith("package it.example.vr;"), name
        assert "__PACKAGE__" not in text
    assert "implementation 'com.google.zxing:core:3.5.3'" in (proj / "app" / "build.gradle").read_text(encoding="utf-8")
    assert "mavenCentral()" in (proj / "build.gradle").read_text(encoding="utf-8")


def test_a_development_loader_on_localhost(tmp_path: Path) -> None:
    proj = _project(tmp_path)
    _patch(proj, "http://localhost:8790")
    m = (proj / "app" / "src" / "main" / "AndroidManifest.xml").read_text(encoding="utf-8")
    assert '<data android:scheme="http"\n                    android:host="localhost"\n                    android:port="8790"' in m
    assert 'def launchUrl = "http://"' in (proj / "app" / "build.gradle").read_text(encoding="utf-8")
    assert '\\"site\\": \\"http://localhost:8790\\"' in (proj / "app" / "src" / "main" / "res" / "values" / "strings.xml").read_text(encoding="utf-8")


def test_the_meta_horizon_store_manifest_rules(tmp_path: Path) -> None:
    """Issue #386: what the Store's manifest rules ask for, from app.json."""
    app = json.loads((QUEST / "app.json").read_text(encoding="utf-8"))
    proj = _project(tmp_path)
    _patch(proj, "https://vr.example.org")
    gradle = (proj / "app" / "build.gradle").read_text(encoding="utf-8")
    assert f"compileSdkVersion {app['targetSdkVersion']}\n" in gradle
    assert f"targetSdkVersion {app['targetSdkVersion']}\n" in gradle
    assert app["targetSdkVersion"] == 34 and 29 <= app["minSdkVersion"] <= 34
    m = (proj / "app" / "src" / "main" / "AndroidManifest.xml").read_text(encoding="utf-8")
    assert 'android:installLocation="auto"' in m
    assert ('android:name="android.hardware.vr.headtracking"\n        android:required="true"') in m
    assert f'<meta-data android:name="com.oculus.supportedDevices" android:value="{app["supportedDevices"]}" />' in m
    assert 'android:networkSecurityConfig="@xml/network_security_config"' in m
    nsc = (proj / "app" / "src" / "main" / "res" / "xml" / "network_security_config.xml").read_text(encoding="utf-8")
    assert 'cleartextTrafficPermitted="false"' in nsc
    # The desktop's USB launch has a filter of its own in LauncherActivity,
    # not BROWSABLE (no web link opens the app), before the verified link.
    launcher = m[m.index('<activity android:name="LauncherActivity"'):m.index('<activity android:name="ScanActivity"')]
    usb = launcher[launcher.index("The desktop's USB launch"):launcher.index('<intent-filter android:autoVerify="true">')]
    usb = usb[usb.index("<intent-filter>"):]
    for line in ('android.intent.action.VIEW', 'android:scheme="http"', 'android:host="localhost"',
                 'android:host="127.0.0.1"'):
        assert line in usb, line
    assert "BROWSABLE" not in usb


def test_supported_devices_must_be_metas_names(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    mod = _load("quest_patch_project_devices", "patch-project.py")
    app = json.loads((QUEST / "app.json").read_text(encoding="utf-8"))
    bad = tmp_path / "app.json"
    bad.write_text(json.dumps({**app, "supportedDevices": "quest3,quest3s"}), encoding="utf-8")
    monkeypatch.setattr(mod, "APP", bad)
    with pytest.raises(SystemExit, match="supportedDevices"):
        mod.store_settings()


def _xmltree(min_sdk: int = 32, target: int = 34, install: str | None = "0", devices: str | None = "quest3|quest3s",
             head_required: str = "true", nsc: bool = True, usb: bool = True, extra_perm: str | None = None,
             version_code: int | None = None) -> str:
    """A manifest as `aapt2 dump xmltree` prints it (the shape of the real APK's)."""
    a = "http://schemas.android.com/apk/res/android:"
    vc = version_code if version_code is not None else json.loads((QUEST / "app.json").read_text())["versionCode"]
    lines = ["N: android=http://schemas.android.com/apk/res/android (line=2)",
             "  N: horizonos=http://schemas.horizonos/sdk (line=2)",
             "    E: manifest (line=2)",
             f"      A: {a}versionCode(0x0101021b)={vc}",
             f"      A: {a}compileSdkVersion(0x01010572)={target}"]
    if install is not None:
        lines.append(f"      A: {a}installLocation(0x010102b7)={install}")
    lines += ['      A: package="it.inaf.oact.visivo.vr" (Raw: "it.inaf.oact.visivo.vr")',
              "        E: uses-sdk (line=31)",
              f"          A: {a}minSdkVersion(0x0101020c)={min_sdk}",
              f"          A: {a}targetSdkVersion(0x01010270)={target}"]
    for perm in ["com.oculus.permission.HAND_TRACKING", "android.permission.CAMERA",
                 "horizonos.permission.HEADSET_CAMERA"] + ([extra_perm] if extra_perm else []):
        lines += ["        E: uses-permission (line=35)", f'          A: {a}name(0x01010003)="{perm}" (Raw: "{perm}")']
    lines += ["        E: uses-feature (line=37)",
              f'          A: {a}name(0x01010003)="android.hardware.vr.headtracking" (Raw: "android.hardware.vr.headtracking")',
              f"          A: {a}required(0x0101028e)={head_required}",
              f"          A: {a}version(0x01010519)=1",
              "        E: http://schemas.horizonos/sdk:uses-horizonos-sdk (line=53)",
              "          A: http://schemas.horizonos/sdk:minSdkVersion=69",
              "        E: application (line=67)"]
    if nsc:
        lines.append(f"          A: {a}networkSecurityConfig(0x01010527)=@0x7f0d0001")
    if devices is not None:
        lines += ["            E: meta-data (line=80)",
                  f'              A: {a}name(0x01010003)="com.oculus.supportedDevices" (Raw: "com.oculus.supportedDevices")',
                  f'              A: {a}value(0x01010024)="{devices}" (Raw: "{devices}")']
    lines += ["            E: activity (line=92)",
              f'              A: {a}name(0x01010003)="it.inaf.oact.visivo.vr.LauncherActivity" (Raw: "x")',
              f"              A: {a}screenOrientation(0x0101001e)=0",
              "                E: intent-filter (line=144)",
              "                    E: action (line=145)",
              f'                      A: {a}name(0x01010003)="android.intent.action.MAIN" (Raw: "x")',
              "                    E: category (line=148)",
              f'                      A: {a}name(0x01010003)="com.oculus.intent.category.VR" (Raw: "x")']
    if usb:
        lines += ["                E: intent-filter (line=150)",
                  "                    E: action (line=151)",
                  f'                      A: {a}name(0x01010003)="android.intent.action.VIEW" (Raw: "x")',
                  "                    E: data (line=156)",
                  f'                      A: {a}scheme(0x01010027)="http" (Raw: "http")',
                  "                    E: data (line=157)",
                  f'                      A: {a}host(0x01010028)="localhost" (Raw: "localhost")',
                  "                    E: data (line=158)",
                  f'                      A: {a}host(0x01010028)="127.0.0.1" (Raw: "127.0.0.1")']
    lines += ["            E: activity (line=166)",
              f'              A: {a}name(0x01010003)="it.inaf.oact.visivo.vr.ScanActivity" (Raw: "x")',
              f"              A: {a}excludeFromRecents(0x01010017)=true",
              "                E: intent-filter (line=179)",
              "                    E: action (line=180)",
              f'                      A: {a}name(0x01010003)="android.intent.action.MAIN" (Raw: "x")',
              "                    E: category (line=183)",
              f'                      A: {a}name(0x01010003)="android.intent.category.LAUNCHER" (Raw: "x")',
              "                    E: category (line=184)",
              f'                      A: {a}name(0x01010003)="com.oculus.intent.category.2D" (Raw: "x")']
    return "\n".join(lines) + "\n"


def _store_failures(xml: str, signing: str = "Verified using v2 scheme (APK Signature Scheme v2): true\n",
                    names: tuple[str, ...] = ("classes.dex",), size: int = 700_000) -> list[str]:
    mod = _load("quest_check_store", "check-store.py")
    app = json.loads((QUEST / "app.json").read_text(encoding="utf-8"))
    res = mod.check(mod.parse_xmltree(xml), mod.parse_signing(signing), list(names), size, app)
    assert len(res) >= 19
    return [what for ok, what in res if not ok]


def test_the_store_check_passes_a_conforming_apk() -> None:
    assert _store_failures(_xmltree()) == []


def test_the_store_check_names_what_the_store_would_refuse() -> None:
    # The 1.1.0 APK (versionCode 3): Bubblewrap's defaults for Quest.
    fails = _store_failures(_xmltree(min_sdk=23, target=32, install=None, devices=None, head_required="false",
                                     nsc=False, usb=False, version_code=3))
    for what in ("versionCode 3", "minSdkVersion 23", "targetSdkVersion 32", "installLocation",
                 "network security", "supportedDevices", "headtracking", "USB launch"):
        assert any(what in f for f in fails), (what, fails)
    assert len(fails) == 8
    assert any("extra: android.permission.INTERNET" in f for f in _store_failures(_xmltree(extra_perm="android.permission.INTERNET")))
    assert any("supportedDevices" in f for f in _store_failures(_xmltree(devices="quest3|pico4")))
    assert any("Signature Scheme v2" in f for f in _store_failures(
        _xmltree(), signing="Verified using v1 scheme (JAR signing): true\nVerified using v2 scheme (APK Signature Scheme v2): false\n"))
    assert any("64-bit" in f for f in _store_failures(_xmltree(), names=("classes.dex", "lib/armeabi-v7a/libx.so")))
    assert not _store_failures(_xmltree(), names=("lib/arm64-v8a/libx.so",))
    assert any("1 GB" in f for f in _store_failures(_xmltree(), size=1_200_000_000))


def test_the_store_images_have_metas_sizes(tmp_path: Path) -> None:
    """Issue #386: make-store-assets.py writes every image the dashboard asks
    for in Meta's size and pixel format, and fits headset pictures to 16:9."""
    pil = pytest.importorskip("PIL.Image")
    pytest.importorskip("numpy")
    mod = _load("quest_make_store_assets", "make-store-assets.py")
    assert mod.main(["make-store-assets.py", str(tmp_path / "store")]) == 0
    want = {"hero-3000x900.png": ((3000, 900), "RGB"), "cover-landscape-2560x1440.png": ((2560, 1440), "RGB"),
            "cover-square-1440x1440.png": ((1440, 1440), "RGB"), "cover-portrait-1008x1440.png": ((1008, 1440), "RGB"),
            "mini-landscape-1080x360.png": ((1080, 360), "RGB"), "icon-512x512.png": ((512, 512), "RGB")}
    for name, (size, mode) in want.items():
        with pil.open(tmp_path / "store" / name) as im:
            assert (im.size, im.mode) == (size, mode), name
    with pil.open(tmp_path / "store" / "logo-transparent.png") as im:
        assert im.mode == "RGBA" and im.height <= 1440 and im.width <= 9000
        assert im.getchannel("A").getextrema()[0] == 0          # transparent around the logo
    with pil.open(tmp_path / "store" / "icon-512x512.png") as im:
        assert im.getpixel((0, 0)) == (255, 255, 255)           # a solid square, square corners
    # Headset pictures: centre-cropped to 16:9, 2560x1440; too small ones refused.
    assert mod.screenshot(pil.new("RGB", (1920, 1080))).size == (2560, 1440)
    assert mod.screenshot(pil.new("RGBA", (1440, 1440))).mode == "RGB"
    with pytest.raises(ValueError, match="too small"):
        mod.screenshot(pil.new("RGB", (1024, 1024)))
    shots = tmp_path / "shots"
    shots.mkdir()
    pil.new("RGB", (2000, 2000), (10, 20, 30)).save(shots / "a.png")
    assert mod.main(["make-store-assets.py", "--screenshots", str(shots), str(tmp_path / "out")]) == 0
    with pil.open(tmp_path / "out" / "screenshot-1-2560x1440.png") as im:
        assert im.size == (2560, 1440) and im.getpixel((5, 5)) == (10, 20, 30)


def test_a_changed_template_fails_loudly(tmp_path: Path) -> None:
    proj = _project(tmp_path)
    manifest = proj / "app" / "src" / "main" / "AndroidManifest.xml"
    manifest.write_text(MANIFEST.replace('<category android:name="android.intent.category.LAUNCHER" />', ""), encoding="utf-8")
    with pytest.raises(SystemExit, match="launcher intent filter"):
        _patch(proj, "https://vr.example.org")


def test_the_app_opens_the_hosted_loader_and_has_a_new_version() -> None:
    app = json.loads((QUEST / "app.json").read_text(encoding="utf-8"))
    assert app["loaderOrigin"].startswith("https://")
    assert "launch=immersive" in app["startPath"]
    # 1.2.0 shipped versionCode 2, the QR scanner is 3; the Store's manifest
    # rules (#386) are 4: the desktop offers Update to those headsets.
    assert int(app["versionCode"]) >= 4
    cfg = json.loads((ROOT / "backend" / "app" / "vr_app.json").read_text(encoding="utf-8"))
    assert app["packages"] == cfg["packages"]


def test_the_loader_hosts_statement_lists_the_release_app() -> None:
    """web/vr-loader/.well-known/assetlinks.json is what the loader's host
    serves: generated from vr_app.json, so a new release key cannot be left
    out (the headset's browser would close the app on that host)."""
    mod = _load("quest_make_assetlinks", "make-assetlinks.py")
    cfg = json.loads((ROOT / "backend" / "app" / "vr_app.json").read_text(encoding="utf-8"))
    committed = ROOT / "web" / "vr-loader" / ".well-known" / "assetlinks.json"
    assert committed.read_text(encoding="utf-8") == mod.text(cfg), "run tools/quest-app/make-assetlinks.py"
    st = json.loads(committed.read_text(encoding="utf-8"))
    assert st[0]["target"]["package_name"] == "it.inaf.oact.visivo.vr"
    assert st[0]["relation"] == ["delegate_permission/common.handle_all_urls"]
    assert all(len(f) == 95 for f in st[0]["target"]["sha256_cert_fingerprints"])


@pytest.mark.skipif(shutil.which("javac") is None or shutil.which("java") is None, reason="no JDK")
def test_pair_code_on_a_jvm(tmp_path: Path) -> None:
    src = tmp_path / "src" / "vt"
    src.mkdir(parents=True)
    for name in ("PairCode.java", "PairCodeTest.java"):
        text = (QUEST / "android" / name).read_text(encoding="utf-8")
        (src / name).write_text(text.replace("package __PACKAGE__;", "package vt;", 1), encoding="utf-8")
    out = tmp_path / "out"
    try:
        subprocess.run(["javac", "-d", str(out), *map(str, sorted(src.glob("*.java")))],
                       check=True, capture_output=True, text=True, timeout=120)
    except subprocess.CalledProcessError as err:
        if "Unable to locate a Java Runtime" in (err.stderr or ""):
            pytest.skip("no JDK behind javac")
        raise
    run = subprocess.run(["java", "-cp", str(out), "vt.PairCodeTest"], capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "checks passed" in run.stdout
