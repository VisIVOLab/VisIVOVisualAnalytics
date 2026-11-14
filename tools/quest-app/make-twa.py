#!/usr/bin/env python3
"""Writes the twa-manifest.json that Meta's Bubblewrap turns into the VisIVO
VR headset app's Android project (issue #255).

Usage: make-twa.py <project_dir> <dev|release> <icon_url> <keystore> <alias>

Everything else comes from app.json next to this script. The origin is
app.json's defaultOrigin: what an icon launch opens. The desktop's launch
names its own backend's address in the intent (any port on localhost), and
the browser verifies that origin's assetlinks.json at every launch.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path
from urllib.parse import urlsplit

HERE = Path(__file__).resolve().parent


def manifest(mode: str, icon_url: str, keystore: str, alias: str, app: dict) -> dict:
    origin = app["defaultOrigin"].rstrip("/")
    u = urlsplit(origin)
    color = app["themeColor"]
    return {
        "packageId": app["packages"][mode],
        # Bubblewrap's host: its template prefixes "https://"; patch-project.py
        # writes the origin's own scheme instead.
        "host": u.netloc,
        "name": app["names"][mode],
        "launcherName": app["names"][mode],
        "display": "standalone",
        "themeColor": color,
        "themeColorDark": color,
        "navigationColor": color,
        "navigationColorDark": color,
        "navigationDividerColor": color,
        "navigationDividerColorDark": color,
        "backgroundColor": color,
        "enableNotifications": False,
        "startUrl": app["startPath"],
        "iconUrl": icon_url,
        "maskableIconUrl": icon_url,
        "splashScreenFadeOutDuration": 300,
        "signingKey": {"path": keystore, "alias": alias},
        "appVersionCode": int(app["versionCode"]),
        "appVersion": str(app["versionName"]),
        "shortcuts": [],
        "generatorApp": "bubblewrap-cli",
        "fallbackType": "customtabs",
        "features": {},
        "alphaDependencies": {"enabled": False},
        "enableSiteSettingsShortcut": False,
        "isChromeOSOnly": False,
        "isMetaQuest": True,
        "horizonOSAppMode": "immersive",
        "fullScopeUrl": f"{origin}{app['scopePath']}",
        "minSdkVersion": 23,
        "orientation": "landscape",
        "fingerprints": [],
        "additionalTrustedOrigins": [],
        "retainedBundles": [],
        "protocolHandlers": [],
        "fileHandlers": [],
        "displayOverride": [],
    }


def main(argv: list[str]) -> int:
    if len(argv) != 6 or argv[2] not in ("dev", "release"):
        print(__doc__, file=sys.stderr)
        return 2
    out_dir, mode, icon_url, keystore, alias = Path(argv[1]), argv[2], argv[3], argv[4], argv[5]
    app = json.loads((HERE / "app.json").read_text(encoding="utf-8"))
    key = "development" if mode == "dev" else "release"
    app = {**app, "packages": {mode: app["packages"][key]}, "names": {mode: app["names"][key]}}
    m = manifest(mode, icon_url, keystore, alias, app)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "twa-manifest.json").write_text(json.dumps(m, indent=2) + "\n", encoding="utf-8")
    print(f"twa-manifest.json: {m['packageId']} {m['appVersion']} ({m['appVersionCode']}), start {m['startUrl']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
