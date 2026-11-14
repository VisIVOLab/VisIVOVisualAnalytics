#!/usr/bin/env python3
"""Adjusts the Android project Meta's Bubblewrap generated for the VisIVO VR
headset app (issues #254, #255).

Usage: patch-project.py <project_dir> <origin>

1. The root build.gradle's jcenter() becomes mavenCentral() (Meta's
   packaging guide; jcenter is read-only).
2. The launch URL and the asset statement use the origin's own scheme: the
   template hardcodes https://, and the app opens http://localhost:<port>
   through adb reverse (a secure context with no certificate).
3. The VIEW intent filter names the host and the port separately: an
   Android intent filter cannot take "host:port".
4. LauncherActivity.getLaunchingUrl():
   - never calls the library's own, which writes the launch URL to logcat
     (the desktop's URL carries a one-time ticket in its fragment);
   - opens only a VR page of a backend on the headset's own localhost (any
     port, any path prefix): the activity is exported, so another app could
     start it with any address;
   - remembers the last such page (without its fragment) so that a launch
     from the app's icon opens the backend the desktop last used, not only
     the default port.
   LauncherActivity.onResume() finishes the activity when it comes back on
   top (the browser's activity above it ended with its VR session): it is
   translucent, so the library's own finish() in onRestart() never runs and
   an invisible activity would stay over the room.
Each patch fails loudly when the template no longer has what it replaces.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import urlsplit


def sub(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise SystemExit(f"{path}: '{old[:70]}' not found: the Bubblewrap template changed")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


LAUNCHING_URL = '''    @Override
    protected Uri getLaunchingUrl() {
        // Not super.getLaunchingUrl(): it writes the launch URL to logcat, and
        // the desktop's launch URL carries a one-time ticket (issue #255).
        Uri uri = getIntent() != null ? getIntent().getData() : null;
        SharedPreferences prefs = getSharedPreferences("visivo-vr", MODE_PRIVATE);
        if (uri != null && isBackendPage(uri)) {
            // An icon launch opens the same backend later (never the fragment:
            // the ticket is single use).
            prefs.edit().putString("last_page", uri.buildUpon().fragment(null).build().toString()).apply();
            return uri;
        }
        String last = prefs.getString("last_page", null);
        if (last != null) {
            Uri kept = Uri.parse(last);
            if (isBackendPage(kept)) return kept;
        }
        return Uri.parse(getString(R.string.launchUrl));
    }

    private boolean resumedOnce;

    @Override
    protected void onResume() {
        super.onResume();
        // Back on top: the browser's activity above it is gone (its VR session
        // ended), so this one goes too and no transparent activity is left
        // over the room. Translucent, it is never stopped, so the library's
        // own finish() in onRestart() never runs (Quest 3, issue #255).
        if (resumedOnce) finish();
        resumedOnce = true;
    }

    /** A VR page of a backend on this headset's localhost (adb reverse), at
     *  any port and under any path prefix: the only pages this app opens. */
    static boolean isBackendPage(Uri u) {
        String scheme = u.getScheme(), host = u.getHost(), path = u.getPath();
        return ("http".equals(scheme) || "https".equals(scheme))
                && ("localhost".equals(host) || "127.0.0.1".equals(host))
                && path != null && (path.endsWith("/vr/") || path.endsWith("/vr/index.html"));
    }
'''


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

    # 3.
    port_attr = f'\n                    android:port="{port}"' if port else ""
    sub(proj / "app" / "src" / "main" / "AndroidManifest.xml",
        '<data android:scheme="https"\n                    android:host="@string/hostName"',
        f'<data android:scheme="{scheme}"\n                    android:host="{host}"{port_attr}')

    # 4.
    launcher = next(proj.glob("app/src/main/java/**/LauncherActivity.java"), None)
    if launcher is None:
        raise SystemExit("LauncherActivity.java not found: the Bubblewrap template changed")
    src = launcher.read_text(encoding="utf-8")
    method = re.compile(r"    @Override\n    protected Uri getLaunchingUrl\(\) \{\n.*?return uri;\n    \}\n", re.S)
    if not method.search(src) or "super.getLaunchingUrl()" not in src:
        raise SystemExit("LauncherActivity.getLaunchingUrl not found: the Bubblewrap template changed")
    src = method.sub(lambda _m: LAUNCHING_URL, src, count=1)
    src = src.replace("import android.net.Uri;", "import android.content.SharedPreferences;\nimport android.net.Uri;", 1)
    if "super.getLaunchingUrl()" in src.split("// Not super.getLaunchingUrl()")[0] or "SharedPreferences;" not in src:
        raise SystemExit("LauncherActivity not patched")
    launcher.write_text(src, encoding="utf-8")
    print(f"patched {proj.name} for {origin}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
