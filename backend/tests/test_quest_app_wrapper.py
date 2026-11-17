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
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / "tools" / "quest-app"

pytestmark = pytest.mark.skipif(not (QUEST / "patch-project.py").is_file(), reason="not a source checkout")


def _load(name: str, file: str):
    spec = importlib.util.spec_from_file_location(name, QUEST / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


MANIFEST = '''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="it.example.vr">
    <uses-permission android:name="com.oculus.permission.HAND_TRACKING" />

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
    defaultConfig {
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
    # 1.2.0 shipped versionCode 2: the desktop offers Update to those headsets.
    assert int(app["versionCode"]) >= 3
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
