"""Canonical names of the ``X-*`` HTTP response headers the backend emits.

Binary endpoints (cube slices, subvolumes, moment/line-width maps, velocity
grids, extracted spectra) carry all of their *metadata* — including scientific
units — in response headers, because the body is a raw binary blob. The desktop
client reads those headers back by name. A silent rename or drop of one of these
names therefore breaks the client with no error (the value simply reads as 0 /
""), which is exactly the failure class that the past router-refactor incident
caused.

This module is the single source of truth for those names on the Python side.
``backend/tests/test_contract.py`` asserts that (a) every ``X-*`` literal that
appears in the router sources is declared here, and (b) this set matches the
committed manifest ``backend/tests/contract/visivo_headers.json`` that the C++
contract test mirrors. Add a header here (and to the manifest) whenever you emit
a new one.
"""
from __future__ import annotations

# ── Cube / subvolume geometry + scalar metadata ─────────────────────────────
X_WIDTH = "X-Visivo-Width"
X_HEIGHT = "X-Visivo-Height"
X_DEPTH = "X-Visivo-Depth"
X_SCALAR_TYPE = "X-Visivo-Scalar-Type"
X_BYTE_ORDER = "X-Visivo-Byte-Order"
X_COMPONENTS = "X-Visivo-Components"
X_RANGE_MIN = "X-Visivo-Range-Min"
X_RANGE_MAX = "X-Visivo-Range-Max"
X_SPACING = "X-Visivo-Spacing"
X_ORIGIN = "X-Visivo-Origin"
X_BUNIT = "X-Visivo-Bunit"

# ── Spectral-axis metadata ──────────────────────────────────────────────────
X_SPECTRAL_AXIS_TYPE = "X-Visivo-Spectral-Axis-Type"
X_SPECTRAL_AXIS_UNIT = "X-Visivo-Spectral-Axis-Unit"
X_SPECTRAL_MODE = "X-Visivo-Spectral-Mode"

# ── Slice / preview speed (velocity) range + downsampling ───────────────────
X_SPEED_MIN = "X-Visivo-Speed-Min"
X_SPEED_MAX = "X-Visivo-Speed-Max"
X_FULL_SPEED_MIN = "X-Visivo-Full-Speed-Min"
X_FULL_SPEED_MAX = "X-Visivo-Full-Speed-Max"
X_UNIT = "X-Visivo-Unit"
X_FRAME = "X-Visivo-Frame"
X_DOWNSAMPLE_FACTOR = "X-Visivo-Downsample-Factor"
X_FULL_SHAPE = "X-Visivo-Full-Shape"
X_FROM_CACHE = "X-Visivo-From-Cache"

# ── Spectrum / profile ──────────────────────────────────────────────────────
X_PEAK = "X-Visivo-Peak"

# ── Velocity field / basins ─────────────────────────────────────────────────
X_DIV_MIN = "X-Visivo-Div-Min"
X_DIV_MAX = "X-Visivo-Div-Max"
X_N_BASINS = "X-Visivo-N-Basins"
X_BBOX = "X-Visivo-Bbox"
X_SELECTED_COUNT = "X-Visivo-Selected-Count"
X_ROI_X0 = "X-Visivo-Roi-X0"
X_ROI_Y0 = "X-Visivo-Roi-Y0"
X_ROI_Z0 = "X-Visivo-Roi-Z0"
X_CALIBRATED = "X-Visivo-Calibrated"
X_DENSITY_FACTOR = "X-Visivo-Density-Factor"
X_WARNINGS = "X-Visivo-Warnings"

# ── Line-width / moment map units (out-of-band because body is binary) ──────
X_FWHM_UNIT = "X-Fwhm-Unit"
X_EW_UNIT = "X-Ew-Unit"
X_MAP_UNIT = "X-Map-Unit"

# ── Cross-cutting / infrastructure ──────────────────────────────────────────
X_TOKEN = "X-Visivo-Token"
X_SESSION = "X-Visivo-Session"
X_REQUEST_ID = "X-Request-ID"
X_SODA_APPLIED = "X-Soda-Applied"
X_TRUNCATED = "X-Truncated"

# Every declared name. Kept as a frozenset so the contract test can diff it
# against both the router sources and the shared JSON manifest.
KNOWN_RESPONSE_HEADERS = frozenset(
    v
    for k, v in dict(globals()).items()
    if k.startswith("X_") and isinstance(v, str)
)
