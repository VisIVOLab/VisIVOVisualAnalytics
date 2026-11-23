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
X_DOWNSAMPLE_FACTORS = "X-Visivo-Downsample-Factors"  # per-axis x,y,z (velocity LOD)
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
X_TRIM_BOX = "X-Visivo-Trim-Box"
# Shared selections (issue #188, routers/share.py).
X_SELECTION_SEQ = "X-Visivo-Selection-Seq"
X_SELECTION_KIND = "X-Visivo-Selection-Kind"
X_ROI_X0 = "X-Visivo-Roi-X0"
X_ROI_Y0 = "X-Visivo-Roi-Y0"
X_ROI_Z0 = "X-Visivo-Roi-Z0"
X_CALIBRATED = "X-Visivo-Calibrated"
X_DENSITY_FACTOR = "X-Visivo-Density-Factor"
X_WARNINGS = "X-Visivo-Warnings"

# ── Ray-traced particle preview ─────────────────────────────────────────────
X_EXPOSURE = "X-Visivo-Exposure"
X_RENDER_MS = "X-Visivo-Render-Ms"
X_PARTICLE_COUNT = "X-Visivo-Particle-Count"
X_PARTICLE_FIELDS = "X-Visivo-Particle-Fields"
X_PARTICLE_STEP = "X-Visivo-Particle-Step"
# The rows' frame: table = origin + scale * row position (issue #208).
X_PARTICLE_ORIGIN = "X-Visivo-Particle-Origin"
X_PARTICLE_SCALE = "X-Visivo-Particle-Scale"
# Rows left out for their intensity (not finite, not positive, or a light that
# is 0 in float32) and rows whose light was capped (issue #414).
X_PARTICLE_DROPPED = "X-Visivo-Particle-Dropped"
X_PARTICLE_CLIPPED = "X-Visivo-Particle-Clipped"
# A projected map's units: its surface density and the mean it is coloured
# by ("relative" when the table states none) (issue #393).
X_PROJECTED_UNIT = "X-Visivo-Map-Unit"
X_PROJECTED_VALUE_UNIT = "X-Visivo-Map-Value-Unit"

# ── Ray-traced volume grid (issue #106) ─────────────────────────────────────
X_VOLUME_SHAPE = "X-Visivo-Volume-Shape"
X_VOLUME_BOX = "X-Visivo-Volume-Box"
X_COLOUR_RANGE = "X-Visivo-Colour-Range"
X_COLOUR_TABLE = "X-Visivo-Colour-Table"

# ── Reduced cube for a headset (issue #167) ─────────────────────────────────
X_BIN_FACTORS = "X-Visivo-Bin-Factors"
X_BIN_MODE = "X-Visivo-Bin-Mode"
X_THRESHOLD = "X-Visivo-Threshold"
# Where it was read from: "cube" or "pyramid", and the pyramid level (part of #166).
X_SOURCE = "X-Visivo-Source"
X_PYRAMID_LEVEL = "X-Visivo-Pyramid-Level"
# Level voxels next to blank ones, and how many were recomputed from level 0.
X_BORDER_VOXELS = "X-Visivo-Border-Voxels"
# The revision of the file a reduced cube was read from (reduced_info's file_revision).
X_FILE_REVISION = "X-Visivo-File-Revision"
# Its value scale, "linear" or "log" (issue #317); on a log scale the log10
# range the codes span ("min,max") and the voxels at or below zero made blank.
X_SCALE = "X-Visivo-Scale"
X_LOG_RANGE = "X-Visivo-Log-Range"
# Several fields of one source reduced on one grid (issue #357): the channels
# a voxel holds, and each field's name, scale, range and threshold (JSON).
X_CHANNELS = "X-Visivo-Channels"
X_FIELDS = "X-Visivo-Fields"
X_NONPOSITIVE_VOXELS = "X-Visivo-Nonpositive-Voxels"

# ── Levels of detail of a large cube (issue #321) ──────────────────────────
# The level a box is of; the level index (x,y,z) of its sample 0, ghost
# included; the cube voxels from a level voxel's first cube voxel to its
# centre ((2^l - 1) / 2 for a pyramid level, 0 for the cube or a strided read).
X_LEVEL = "X-Visivo-Level"
X_LEVEL_ORIGIN = "X-Visivo-Level-Origin"
X_VOXEL_OFFSET = "X-Visivo-Voxel-Offset"

# ── Line-width / moment map units (out-of-band because body is binary) ──────
X_FWHM_UNIT = "X-Fwhm-Unit"
X_EW_UNIT = "X-Ew-Unit"
X_MAP_UNIT = "X-Map-Unit"

# ── The VR page's layers (issue #190): point samples, velocity lines ───────
X_COUNT = "X-Visivo-Count"
X_TOTAL_ROWS = "X-Visivo-Total-Rows"
X_STEP = "X-Visivo-Step"
X_BOUNDS = "X-Visivo-Bounds"
X_AXES = "X-Visivo-Axes"
X_COLOR_FIELD = "X-Visivo-Color-Field"
X_COLOR_RANGE = "X-Visivo-Color-Range"
X_COLOR_LOG = "X-Visivo-Color-Log"
X_DATA_RANGE = "X-Visivo-Data-Range"
# Each sampled row's size (issue #259: a smoothing length), its column and range.
X_SIZE_FIELD = "X-Visivo-Size-Field"
X_SIZE_RANGE = "X-Visivo-Size-Range"
X_SIZE_NOTE = "X-Visivo-Size-Note"
# A sky catalogue's placement (issues #260, #265): "frame,distance_mode,cosmology",
# and the sources left out for want of a distance, or placed from cz < 1000 km/s.
X_SKY = "X-Visivo-Sky"
X_UNPLACED = "X-Visivo-Unplaced"
X_UNCERTAIN = "X-Visivo-Uncertain"
# The rows a point sample was drawn from (the unplaced ones left out): the
# sample's step and the page's splat size count these, not X-Visivo-Total-Rows.
X_PLACED_ROWS = "X-Visivo-Placed-Rows"
X_LINE_POINTS = "X-Visivo-Line-Points"
X_LINES = "X-Visivo-Lines"
X_ARROWS = "X-Visivo-Arrows"
X_GLYPH_CELL = "X-Visivo-Glyph-Cell"
X_SEEDS = "X-Visivo-Seeds"
X_SEED = "X-Visivo-Seed"
X_BOX_MPC = "X-Visivo-Box-Mpc"

# ── The headset's view on the desktop (issue #281): a spectator frame ──────
X_FRAME_SEQ = "X-Visivo-Frame-Seq"
X_FRAME_WIDTH = "X-Visivo-Frame-Width"
X_FRAME_HEIGHT = "X-Visivo-Frame-Height"
X_FRAME_EYE = "X-Visivo-Frame-Eye"
X_FRAME_AGE_MS = "X-Visivo-Frame-Age-Ms"
X_FRAME_COST_MS = "X-Visivo-Frame-Cost-Ms"
X_INSTANCE = "X-Visivo-Instance"

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
