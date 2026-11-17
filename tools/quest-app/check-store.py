#!/usr/bin/env python3
"""Checks a built VisIVO VR APK against the Meta Horizon Store's technical
requirements (issue #386) before it is uploaded.

Usage: check-store.py <apk> [--build-tools DIR] [--report FILE]

DIR is an Android SDK build-tools directory with aapt2 and apksigner
(default: $ANDROID_HOME/build-tools/34.0.0). The manifest is read with
`aapt2 dump xmltree`, the signature with `apksigner verify -v`; nothing is
installed or uploaded. Exit status 0 when every check passes, 1 otherwise;
--report writes the results as JSON.

The rules, as Meta documents them (2026-11; docs/technical/deployment/meta-store.md):
- Application manifest requirements,
  https://developers.meta.com/horizon/resources/publish-mobile-manifest/ :
  minSdkVersion 29-34; targetSdkVersion 32-34 for an immersive app and 34
  for apps created since 2026-03-01; compileSdkVersion >= targetSdkVersion;
  installLocation auto; com.oculus.supportedDevices; head tracking required
  (immersive); not debuggable; the launcher activity has MAIN and
  excludeFromRecents; immersive activities have com.oculus.intent.category.VR.
- VRC.Quest.Packaging.4 (a network security configuration in the manifest),
  .../vrc-quest-packaging-4/ ; Packaging.2 (APK Signature Scheme v2), .../vrc-quest-packaging-2/ ;
  Packaging.5 (APK under 1 GB), .../vrc-quest-packaging-5/ ; Packaging.6
  (64-bit), .../vrc-quest-packaging-6/ .
And this app's own: exactly the permissions its privacy policy and data-use
answers describe (web/vr-loader/privacy.html), the Horizon OS SDK level of a
hybrid app, and the filter that keeps the desktop's USB launch working.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
ANDROID = "http://schemas.android.com/apk/res/android:"
HORIZON_SDK_ELEMENTS = ("http://schemas.horizonos/sdk:uses-horizonos-sdk",
                        "http://schemas.meta.com/metavr-sdk:uses-metavr-sdk")
DEVICES = {"quest2", "questpro", "quest3", "quest3s", "vrglasses"}
# What web/vr-loader/privacy.html and the data-use answers describe: a new
# permission means updating both before the upload.
PERMISSIONS = {"android.permission.CAMERA", "horizonos.permission.HEADSET_CAMERA",
               "com.oculus.permission.HAND_TRACKING"}
MIN_SDK_RANGE = (29, 34)
TARGET_SDK_RANGE = (32, 34)          # immersive apps
TARGET_SDK_NEW_APPS = 34             # apps created since 2026-03-01
MAX_APK_BYTES = 1_000_000_000        # "under 1 GB"


@dataclass
class Element:
    name: str
    attrs: dict[str, str] = field(default_factory=dict)
    children: list["Element"] = field(default_factory=list)

    def all(self, name: str) -> list["Element"]:
        """Every descendant (and self) called name."""
        out = [self] if self.name == name else []
        for c in self.children:
            out.extend(c.all(name))
        return out

    def get(self, attr: str, default: str | None = None) -> str | None:
        return self.attrs.get(attr, default)


_ATTR = re.compile(r'^A: (?P<name>.+?)(?:\(0x[0-9a-f]+\))?=(?P<value>.*)$')


def _value(raw: str) -> str:
    raw = raw.strip()
    if raw.startswith('"'):
        end = raw.find('" (Raw: ')
        return raw[1:end] if end >= 0 else raw[1:].rstrip('"')
    return raw.split(" (Raw: ", 1)[0]


def parse_xmltree(text: str) -> Element:
    """`aapt2 dump xmltree --file AndroidManifest.xml` as a tree. Attribute
    names keep `android:` for the Android namespace and their full URI
    otherwise; values are aapt2's (true/false, decimal integers, @0x... for
    references, the string for strings)."""
    root = Element("#document")
    stack: list[tuple[int, Element]] = [(-1, root)]
    for line in text.splitlines():
        stripped = line.lstrip(" ")
        indent = len(line) - len(stripped)
        if stripped.startswith("E: "):
            name = stripped[3:].split(" (line=", 1)[0]
            while stack[-1][0] >= indent:
                stack.pop()
            el = Element(name)
            stack[-1][1].children.append(el)
            stack.append((indent, el))
        elif stripped.startswith("A: "):
            m = _ATTR.match(stripped)
            if not m:
                continue
            while stack[-1][0] >= indent:
                stack.pop()
            name = m.group("name")
            if name.startswith(ANDROID):
                name = "android:" + name[len(ANDROID):]
            stack[-1][1].attrs[name] = _value(m.group("value"))
    return root


def parse_signing(text: str) -> dict[str, bool]:
    """`apksigner verify -v`: which schemes verified."""
    out = {}
    for m in re.finditer(r"^Verified using (v[0-9.]+) scheme[^:]*: (true|false)$", text, re.M):
        out[m.group(1)] = m.group(2) == "true"
    return out


def _int(v: str | None) -> int | None:
    try:
        return int(v, 0) if v is not None else None
    except ValueError:
        return None


def _filters(activity: Element) -> list[tuple[set[str], set[str], list[Element]]]:
    out = []
    for f in activity.all("intent-filter"):
        actions = {a.get("android:name") for a in f.all("action")}
        cats = {c.get("android:name") for c in f.all("category")}
        out.append((actions, cats, f.all("data")))
    return out


def check(manifest: Element, signing: dict[str, bool], zip_names: list[str], size: int,
          app: dict) -> list[tuple[bool, str]]:
    """[(passed, what)] for every rule above."""
    res: list[tuple[bool, str]] = []

    def rule(ok: bool, what: str) -> None:
        res.append((bool(ok), what))

    man = next(iter(manifest.all("manifest")), Element("manifest"))
    package = man.get("package")
    rule(package == app["packages"]["release"],
         f"package {package} is the Store build's {app['packages']['release']}"
         + (" (this is the development build)" if package == app["packages"].get("development") else ""))
    vc = _int(man.get("android:versionCode"))
    rule(vc == int(app["versionCode"]), f"versionCode {vc} is app.json's {app['versionCode']} "
         "(every upload needs a versionCode higher than any uploaded before)")

    sdk = next(iter(man.all("uses-sdk")), Element("uses-sdk"))
    min_sdk, target = _int(sdk.get("android:minSdkVersion")), _int(sdk.get("android:targetSdkVersion"))
    compile_sdk = _int(man.get("android:compileSdkVersion"))
    rule(min_sdk is not None and MIN_SDK_RANGE[0] <= min_sdk <= MIN_SDK_RANGE[1],
         f"minSdkVersion {min_sdk} in {MIN_SDK_RANGE[0]}-{MIN_SDK_RANGE[1]}")
    rule(target == TARGET_SDK_NEW_APPS and TARGET_SDK_RANGE[0] <= target <= TARGET_SDK_RANGE[1],
         f"targetSdkVersion {target} is {TARGET_SDK_NEW_APPS} (apps created since 2026-03-01)")
    rule(compile_sdk is None or (target is not None and compile_sdk >= target),
         f"compileSdkVersion {compile_sdk} >= targetSdkVersion {target}")
    rule(man.get("android:installLocation") in ("0", "auto"),
         f"installLocation auto (found {man.get('android:installLocation')})")

    application = next(iter(man.all("application")), Element("application"))
    rule(application.get("android:debuggable") not in ("true", "-1"), "not debuggable")
    rule(application.get("android:networkSecurityConfig") is not None,
         "a network security configuration (VRC.Quest.Packaging.4)")
    devices = [m.get("android:value") for m in application.children
               if m.name == "meta-data" and m.get("android:name") == "com.oculus.supportedDevices"]
    names = set(devices[0].split("|")) if len(devices) == 1 and devices[0] else set()
    rule(len(devices) == 1 and names and names <= DEVICES and devices[0] == app.get("supportedDevices"),
         f"com.oculus.supportedDevices {devices[0] if devices else None} (app.json {app.get('supportedDevices')})")

    head = [f for f in man.all("uses-feature") if f.get("android:name") == "android.hardware.vr.headtracking"]
    rule(len(head) == 1 and head[0].get("android:required") == "true" and head[0].get("android:version") == "1",
         "android.hardware.vr.headtracking required, version 1 (an immersive app)")

    hz = [e for n in HORIZON_SDK_ELEMENTS for e in man.all(n)]
    hz_min = _int(next((v for e in hz for k, v in e.attrs.items() if k.endswith(":minSdkVersion")), None))
    rule(hz_min is not None and hz_min >= 69, f"Horizon OS SDK declared, minimum {hz_min} >= 69 (hybrid app)")

    perms = {p.get("android:name") for p in man.all("uses-permission")}
    rule(perms == PERMISSIONS, "permissions are exactly " + ", ".join(sorted(PERMISSIONS))
         + (f" (extra: {', '.join(sorted(perms - PERMISSIONS))}: update privacy.html and the data-use answers)"
            if perms - PERMISSIONS else "")
         + (f" (missing: {', '.join(sorted(PERMISSIONS - perms))})" if PERMISSIONS - perms else ""))

    activities = application.all("activity")
    launchers = [a for a in activities
                 if any("android.intent.action.MAIN" in acts and "android.intent.category.LAUNCHER" in cats
                        for acts, cats, _ in _filters(a))]
    rule(len(launchers) == 1 and launchers[0].get("android:excludeFromRecents") == "true",
         f"one launcher activity, with excludeFromRecents ({[a.get('android:name') for a in launchers]})")
    vr = [a for a in activities if any("com.oculus.intent.category.VR" in cats for _, cats, _ in _filters(a))]
    rule(bool(vr) and all(a.get("android:screenOrientation") in ("0", "landscape") for a in vr),
         "an immersive activity (com.oculus.intent.category.VR), landscape")
    # Every activity of a Horizon OS app is a panel or immersive.
    mode_less = [a.get("android:name") for a in activities
                 if any("android.intent.action.MAIN" in acts for acts, _, _ in _filters(a))
                 and not any(cats & {"com.oculus.intent.category.VR", "com.oculus.intent.category.2D"}
                             for _, cats, _ in _filters(a))]
    rule(not mode_less, f"every MAIN activity is 2D or VR ({mode_less or 'all'})")

    launcher = [a for a in activities if str(a.get("android:name", "")).endswith(".LauncherActivity")]
    usb = launcher and any(
        "android.intent.action.VIEW" in acts and "android.intent.category.BROWSABLE" not in cats
        and {d.get("android:host") for d in data} >= {"localhost", "127.0.0.1"}
        and "http" in {d.get("android:scheme") for d in data}
        for acts, cats, data in _filters(launcher[0]))
    rule(bool(usb), "LauncherActivity matches the desktop's USB launch (VIEW http://localhost, not BROWSABLE)")

    schemes = ", ".join(k for k, v in sorted(signing.items()) if v) or "none"
    rule(signing.get("v2", False), f"APK Signature Scheme v2 (VRC.Quest.Packaging.2; verified: {schemes})")
    libs = {n.split("/")[1] for n in zip_names if n.startswith("lib/") and n.count("/") >= 2}
    rule(not libs or "arm64-v8a" in libs,
         f"64-bit: {'no native code' if not libs else 'ABIs ' + ', '.join(sorted(libs))} (VRC.Quest.Packaging.6)")
    rule(0 < size < MAX_APK_BYTES, f"size {size / 1e6:.2f} MB under 1 GB (VRC.Quest.Packaging.5)")
    return res


def _run(cmd: list[str]) -> str:
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        raise SystemExit(f"{Path(cmd[0]).name} failed: {(p.stderr or p.stdout).strip()[:400]}")
    return p.stdout


def main(argv: list[str]) -> int:
    args = argv[1:]
    bt = os.path.join(os.environ.get("ANDROID_HOME", ""), "build-tools", "34.0.0")
    report = None
    apk = None
    while args:
        a = args.pop(0)
        if a == "--build-tools" and args:
            bt = args.pop(0)
        elif a == "--report" and args:
            report = args.pop(0)
        elif apk is None and not a.startswith("--"):
            apk = a
        else:
            print(__doc__, file=sys.stderr)
            return 2
    if apk is None or not Path(apk).is_file():
        print(__doc__, file=sys.stderr)
        return 2
    app = json.loads((HERE / "app.json").read_text(encoding="utf-8"))
    tree = parse_xmltree(_run([os.path.join(bt, "aapt2"), "dump", "xmltree", "--file", "AndroidManifest.xml", apk]))
    signing = parse_signing(_run([os.path.join(bt, "apksigner"), "verify", "-v", apk]))
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
    results = check(tree, signing, names, Path(apk).stat().st_size, app)
    for ok, what in results:
        print(("ok    " if ok else "FAIL  ") + what)
    failed = sum(1 for ok, _ in results if not ok)
    print(f"{len(results) - failed}/{len(results)} Meta Horizon Store checks passed")
    if report:
        Path(report).write_text(json.dumps({"apk": Path(apk).name, "passed": failed == 0,
                                            "checks": [{"ok": ok, "what": w} for ok, w in results]},
                                           indent=2) + "\n", encoding="utf-8")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
