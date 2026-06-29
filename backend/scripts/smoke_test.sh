#!/usr/bin/env bash
# Backend smoke test — run from anywhere; uses the backend .venv.
# Checks: full pytest suite, app boots + mounts all routers, and the
# astropy.samp deprecation is gone (pyvo.samp is the active SAMP path).
set -uo pipefail

BACKEND_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$BACKEND_DIR/.venv/bin/python"
cd "$BACKEND_DIR"

echo "================ 1. pytest suite ================"
"$PY" -m pytest -q
PYTEST=$?
echo "pytest exit: $PYTEST"

echo ""
echo "================ 2. app boot + routers ================"
"$PY" - <<'PY'
import importlib
app = importlib.import_module("app.main").app
from fastapi.testclient import TestClient
c = TestClient(app)
r = c.get("/v1/openapi.json")
paths = r.json().get("paths", {}) if r.status_code == 200 else {}
print(f"GET /v1/openapi.json -> {r.status_code} | endpoint paths: {len(paths)}")
assert r.status_code == 200 and len(paths) > 50, "router mount/boot FAILED"
print("BOOT OK")
PY
BOOT=$?

echo ""
echo "================ 3. SAMP migration (no astropy.samp deprecation) ================"
"$PY" - <<'PY'
import warnings
with warnings.catch_warnings():
    warnings.simplefilter("error")          # any DeprecationWarning -> exception
    import app.samp_bridge as s
print("SAMP available:", s._SAMP_AVAILABLE)
# confirm the active client comes from pyvo, not the deprecated astropy.samp
mod = s._SAMPIntegratedClient.__module__
print("SAMP client module:", mod)
assert mod.startswith("pyvo"), f"expected pyvo.samp, got {mod}"
print("SAMP MIGRATION OK")
PY
SAMP=$?

echo ""
echo "================ SUMMARY ================"
echo "pytest:        $([ $PYTEST -eq 0 ] && echo PASS || echo FAIL)"
echo "boot/routers:  $([ $BOOT  -eq 0 ] && echo PASS || echo FAIL)"
echo "samp/pyvo:     $([ $SAMP  -eq 0 ] && echo PASS || echo FAIL)"
[ $PYTEST -eq 0 ] && [ $BOOT -eq 0 ] && [ $SAMP -eq 0 ] && echo "ALL GREEN" || echo "SOME CHECKS FAILED"
