#!/usr/bin/env python3
"""Writes the Digital Asset Links statement the pairing loader's host must
publish at /.well-known/assetlinks.json (issue #383), from
backend/app/vr_app.json: the release app's package and certificates.

Usage: make-assetlinks.py [<out>]   (default web/vr-loader/.well-known/assetlinks.json)

The headset's browser fetches it at every launch of the VisIVO VR app's
immersive part on the loader's origin and closes the app when the app is not
listed; Android also reads it to verify the app's link to that host (the
desktop's QR code opening the app). The committed copy is checked against
vr_app.json by backend/tests/test_quest_app_wrapper.py, so a new release
key cannot be forgotten here.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "backend" / "app" / "vr_app.json"
DEFAULT_OUT = ROOT / "web" / "vr-loader" / ".well-known" / "assetlinks.json"


def statements(config: dict) -> list[dict]:
    fingerprints = [str(f).upper() for f in config.get("release_sha256", [])]
    if not fingerprints:
        raise SystemExit("vr_app.json lists no release_sha256")
    return [{
        "relation": ["delegate_permission/common.handle_all_urls"],
        "target": {"namespace": "android_app", "package_name": config["packages"]["release"],
                   "sha256_cert_fingerprints": fingerprints},
    }]


def text(config: dict) -> str:
    return json.dumps(statements(config), indent=2) + "\n"


def main(argv: list[str]) -> int:
    out = Path(argv[1]) if len(argv) > 1 else DEFAULT_OUT
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text(json.loads(CONFIG.read_text(encoding="utf-8"))), encoding="utf-8")
    print(f"{out}: {', '.join(json.loads(CONFIG.read_text(encoding='utf-8'))['release_sha256'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
