from __future__ import annotations

from typing import Any, Literal
from pydantic import BaseModel, Field

class ErrorDetail(BaseModel):
    error: str
    detail: str
    request_id: str = ""


class FileEntry(BaseModel):
    name: str
    path: str
    type: str
    size: int = 0
    modified_time: str = ""
    is_fits: bool = False


class FilesListResponse(BaseModel):
    valid: bool
    error: str
    current_path: str = ""
    entries: list[FileEntry]


class FileHeaderRequest(BaseModel):
    path: str


class FileHeaderResponse(BaseModel):
    valid: bool
    error: str
    cards: list[str]


class FileClassifyRequest(BaseModel):
    path: str


class ParticleComponentInfo(BaseModel):
    """One selectable component of a particle snapshot (issue #31): a
    particle type, opened by its own path (a reference)."""
    name: str
    path: str
    num_rows: int = 0


class FileClassifyResponse(BaseModel):
    """What one file is, so a single *Open…* can decide where it goes."""
    valid: bool
    error: str = ""
    path: str = ""
    #: image | cube | dynspec | fits-table | catalogue | vbt | pluto | compressed |
    #: directory | unknown
    kind: str = ""
    #: dataset | catalogue | vbt | ask | unsupported — the route that opens it
    opener: str = ""
    #: The path to hand the opener when it differs from `path` — a VBT chosen by
    #: its `.bin` opens by its `.head` sidecar. Empty means "use `path`".
    open_path: str = ""
    #: False when the client should ask the user instead of dispatching.
    confident: bool = False
    #: One sentence for the user, in the words of what was actually found.
    detail: str = ""
    #: A particle snapshot's components (its particle types with particles),
    #: each opened by its own path; ``open_path`` is the first. Empty for a
    #: file of one component.
    components: list[ParticleComponentInfo] = []


#: A VBT's byte order chosen against its header (issue #165): "little" or
#: "big"; None keeps what this backend already reads the file in (the header's
#: order, or an earlier choice).
VbtByteOrder = Literal["little", "big"] | None


class VbtByteOrderWarning(BaseModel):
    """A VBT whose values look like the other byte order than its header
    declares (app/vbt_byte_order.py). ``confidence`` is "overwhelming" or
    "strong"; ``evidence`` the scores of both orders and, per field, its
    first value read either way."""
    declared: str
    suggested: str
    confidence: str
    evidence: dict[str, Any] = {}


class OpenDatasetRequest(BaseModel):
    path: str
    # A VBT volume field only: read the file in this byte order (issue #165).
    byte_order: VbtByteOrder = None


class DecompressRequest(BaseModel):
    """A compressed FITS (``.fits.gz``, ``.fz``) to decompress into the
    workspace cache before opening it (issue #57)."""
    path: str


class DatasetField(BaseModel):
    """A field of a multi-field source, opened as a dataset of its own."""
    name: str
    path: str      # what /v1/datasets/open takes to open it


class OpenDatasetResponse(BaseModel):
    valid: bool
    error: str
    dataset_id: str = ""
    session_id: str = ""           # R3: client must echo as X-Visivo-Session
    kind: str = ""
    active_axes: int = 0
    width: int = 0
    height: int = 0
    depth: int = 0
    degenerate_axes_summary: str = ""
    wcs_status: str = "ok"
    wcs_warning_message: str = ""
    wcs_sanitized_axes: list[int] = []
    spectral_axis_type: str = ""
    spectral_axis_unit: str = ""
    rest_freq_hz: float | None = None   # header RESTFRQ/RESTFREQ, Hz (None = absent)
    spacing: list[float] = [1.0, 1.0, 1.0]
    origin: list[float] = [0.0, 0.0, 0.0]
    bunit: str = ""
    ctype: list[str] = ["", "", ""]
    cunit: list[str] = ["", "", ""]
    crval: list[float] = [0.0, 0.0, 0.0]
    crpix: list[float] = [1.0, 1.0, 1.0]
    cdelt: list[float] = [1.0, 1.0, 1.0]
    beam_major: float | None = None
    beam_minor: float | None = None
    beam_pa: float | None = None
    beam_per_channel: bool = False   # a BEAMS table: one beam per channel (issue #401)
    # "header", "user" (typed with /v1/datasets/beam, issue #416), "none", or
    # "not_applicable" for data without celestial axes (VBT fields, raw
    # simulation fields), where no beam can be set.
    beam_source: str = ""
    # The values' unit when BUNIT is missing (TEMPSCAL/BTYPE/LABEL); equals
    # bunit when it is present, "" when nothing says. The note says where it
    # came from.
    intensity_unit: str = ""
    intensity_unit_note: str = ""
    # Size-aware loading policy. Clients use ``recommended_load_mode``
    # to decide whether to auto-load full resolution or default to
    # preview-only. Threshold is configurable via
    # VISIVO_FULL_RES_THRESHOLD_BYTES (default 4 GiB, or a quarter of the
    # backend's memory when that is less).
    file_size_bytes: int = 0
    full_res_threshold_bytes: int = 4 * 1024 ** 3
    recommended_load_mode: str = "auto_full"  # "auto_full" | "preview_only"
    # A VisIVO Binary Table volume opens one field as a cube (issue #83): the
    # field shown, and every field of the file with the path that opens it.
    field_name: str = ""
    fields: list[DatasetField] = []
    # A VBT volume (issue #165): the byte order its values are read in, the
    # one its header declares, and a warning when they look like the other.
    byte_order: str = ""
    byte_order_declared: str = ""
    byte_order_warning: VbtByteOrderWarning | None = None
    # True iff this backend can host server-side rendering for the
    # opened cube (EGL build + GPU detected). Backend-agnostic: any
    # client connecting to any backend (SKAVA-discovered, manually
    # added Remote Backend, localhost) reads this single flag to
    # decide whether to offer the "Use remote rendering" mode.
    server_side_rendering_available: bool = False


class MomentProductRequest(BaseModel):
    dataset_id: str
    moment_order: int
    channel_start: int = 0
    channel_end: int = 0
    mask_enabled: bool = False
    threshold_value: float = 0.0
    # When True (and mask_enabled), the backend derives the mask threshold from
    # the data itself (median + 3·σ via MAD) instead of using threshold_value.
    threshold_auto: bool = False
    # R-03: for moment order 2 the default output is the velocity dispersion σ
    # (km/s), matching CASA/CARTA/SoFiA. Set True to return the raw variance σ²
    # (spectral_unit²) instead. Ignored for other orders.
    moment2_variance: bool = False


class MomentProductResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    spectral_axis_type: str = ""
    spectral_axis_unit: str = ""
    moment_unit: str = ""
    bunit: str = ""
    # Echo of the mask threshold actually applied (resolved value when
    # threshold_auto was requested); null when no mask was applied.
    threshold_used: float | None = None
    data_base64: str = ""


class CubePreviewRequest(BaseModel):
    dataset_id: str
    downsample: int = 1
    #: ``auto``: block means from the cube's brick pyramid when one is complete
    #: and current (issue #320; opening a cube never starts a build), else
    #: every ``downsample``-th voxel. ``strided``: every ``downsample``-th
    #: voxel whatever exists.
    source: Literal["auto", "strided"] = "auto"


class CubePreviewResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    depth: int = 0
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    # The cube's default 3-D threshold, from its noise (issue #93); null for
    # a preview with no finite value.
    default_threshold: float | None = None
    # Empty string = uncompressed raw float32 (legacy default).
    # "qt-zlib"  = zlib bytes with 4-byte big-endian length prefix
    #              (Qt's qUncompress convention). Required for the
    #              compressed preview path; without it pydantic drops
    #              the field, the client falls back to "uncompressed",
    #              decodes zlib bytes as raw float32 → grey slice.
    compression: str = ""
    data_base64: str = ""
    # Where the preview came from (issue #320): "strided" (every
    # factors-th voxel of the cube) or "pyramid" (each voxel the mean of its
    # factors-sized cell, from brick-pyramid level ``level`` of ``levels``,
    # whose voxels are level_factors cube voxels per axis). Either way the
    # shape is ceil(n / factor) per axis; voxel i stands for cube voxel
    # i * factor + voxel_offset (for "pyramid" the centre of a whole cell;
    # the last cell of an axis may be shorter).
    source: str = ""
    level: int | None = None
    levels: int | None = None
    level_factors: list[int] | None = None
    factors: list[int] | None = None
    voxel_offset: float = 0.0


class CubeRegionLevelRequest(BaseModel):
    """A box of one level of detail of a cube (issue #321, app/region_level.py)."""
    dataset_id: str
    #: 0 is the cube itself; level ``l`` the cube halved ``l`` times per axis.
    level: int = Field(ge=0, le=62)
    #: The box, inclusive, in voxels of the level.
    x0: int
    x1: int
    y0: int
    y1: int
    z0: int
    z1: int
    #: Samples of the neighbours added on every side (the edge repeated
    #: outside the cube).
    ghost: Literal[0, 1] = 0
    #: ``auto``: a level >= 1 from the cube's brick pyramid when one is
    #: complete and current, else a strided read (never a build);
    #: ``pyramid``: from the pyramid only (409 without one); ``strided``:
    #: every ``2^level``-th voxel of the cube. Level 0 is always the cube.
    source: Literal["auto", "pyramid", "strided"] = "auto"


class PyramidStatusRequest(BaseModel):
    dataset_id: str
    #: Start the build when the pyramid is missing, stale or failed long
    #: enough ago; ``false`` only reports.
    build: bool = False


class PyramidStatusResponse(BaseModel):
    valid: bool = True
    #: ``ready``, ``building``, ``missing``, ``failed`` or ``unsupported``
    #: (a cube no pyramid holds: a Stokes cube, a cube with an axis of one
    #: plane).
    state: str
    #: 0..1 while building; 1 when ready.
    progress: float = 0.0
    #: The pyramid's levels, brick and per-level (z, y, x) shapes when ready,
    #: else those a default build would have: the levels region_level_bin
    #: answers (strided without a pyramid).
    levels: int = 0
    brick: int = 0
    shapes: list[list[int]] = Field(default_factory=list)
    #: The pyramid file's size when ready, else about what a build writes.
    bytes_estimate: int = 0
    #: Why the last build failed, or why a pyramid found ready could not be
    #: opened.
    error: str = ""
    #: True while building (the 202 answers' convention).
    preparing: bool = False


class CubeSliceRequest(BaseModel):
    dataset_id: str
    axis: str
    index: int
    #: The longest side of the answer, in samples: the plane (or the region
    #: below) is read every ``stride`` pixels so that neither side exceeds it.
    #: 0 is the whole resolution, what clients before issue #381 get: a
    #: 7233 x 7233 channel is 209 MB of float32 at full resolution.
    max_longest_side: int = Field(0, ge=0)
    #: The region to read, inclusive source pixels, all four or none: the part
    #: of the plane a zoomed view shows. Clamped to the plane.
    x0: int | None = None
    x1: int | None = None
    y0: int | None = None
    y1: int | None = None


class CubeSliceResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    compression: str = ""
    data_base64: str = ""
    #: Where the samples sit on the plane: sample (i, j) is source pixel
    #: (x0 + i * stride, y0 + j * stride) of a full_width x full_height plane.
    full_width: int = 0
    full_height: int = 0
    x0: int = 0
    y0: int = 0
    stride: int = 1


class CubeSubvolumeRequest(BaseModel):
    dataset_id: str
    x0: int
    x1: int
    y0: int
    y1: int
    z0: int
    z1: int


class CubeSubvolumeResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    depth: int = 0
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    spectral_axis_type: str = ""
    spectral_axis_unit: str = ""
    bunit: str = ""
    # Same compression negotiation as CubePreviewResponse — see comment
    # there. Required for the compressed-subvolume path; absent → silent
    # zlib-as-float32 corruption client-side.
    compression: str = ""
    data_base64: str = ""


class CubePvRequest(BaseModel):
    dataset_id: str
    # 0-based cube pixel (x, y); fractional values are honoured (an edited path
    # is not snapped to the grid).
    vertices: list[list[float]]
    width_pixels: int = 1


class CubePvResponse(BaseModel):
    valid: bool
    error: str
    num_samples: int = 0
    depth: int = 0
    scalar_type: str = ""
    compression: str = ""
    positions_base64: str = ""
    positions_arcsec_base64: str = ""
    pixel_scale_arcsec_per_pixel: float = 0.0
    spatial_unit: str = "pixel"
    data_base64: str = ""
    computed_on: str = ""
    width_pixels: int = 1
    vertex_count: int = 0
    total_length: float = 0.0
    valid_samples: int = 0
    spectral_axis_type: str = ""
    spectral_axis_unit: str = ""
    # Channel coordinates (float64 little-endian, base64), channel 0 first, in
    # spectral_axis_unit; and the linear axis they define (CDELT may be < 0).
    spectral_coordinates_base64: str = ""
    spectral_crval: float = 0.0
    spectral_cdelt: float = 0.0
    step_pixels: float = 1.0
    sky_matrix_arcsec: list[float] = []
    bunit: str = ""
    beam_major: float | None = None
    beam_minor: float | None = None
    beam_pa: float | None = None


class TaskCreateResponse(BaseModel):
    valid: bool
    error: str
    task_id: str = ""
    status: str = ""
    cache_hit: bool = False


class TaskStatusResponse(BaseModel):
    valid: bool
    error: str
    task_id: str = ""
    operation: str = ""
    status: str = ""
    created_at: str = ""
    updated_at: str = ""
    progress: float = 0.0
    cache_hit: bool = False
    result: dict[str, Any] | None = None


class ImageFullRequest(BaseModel):
    dataset_id: str


class ImagePreviewRequest(BaseModel):
    dataset_id: str
    max_longest_side: int = 1536


class ImageTileRequest(BaseModel):
    dataset_id: str
    # Pyramid level: 0 = full resolution, L decimates by 2**L.
    level: int = 0
    tile_x: int = 0
    tile_y: int = 0
    tile_size: int = 256


class ImageTileResponse(BaseModel):
    valid: bool
    error: str = ""
    width: int = 0            # actual tile pixel dims (≤ tile_size at edges)
    height: int = 0
    full_width: int = 0
    full_height: int = 0
    level: int = 0
    num_levels: int = 1
    tile_x: int = 0
    tile_y: int = 0
    tile_size: int = 256
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    # Issue #85. True when the tile comes from the image's averaged pyramid:
    # pixel i of level L stands for source pixels i*2**L .. (i+1)*2**L - 1
    # (centred at i*2**L + (2**L - 1)/2), the mean of the 2x2 means of the
    # level below; false when sampled from the file (pixel i is source pixel
    # i*2**L).
    averaged: bool = False
    # The image's pyramid: "none" (small image, no pyramid), "building",
    # "ready" or "failed"; and the fraction of its build done.
    pyramid: str = "none"
    pyramid_progress: float = 0.0
    compression: str = ""
    data_base64: str = ""


class ImageFullResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    full_width: int = 0
    full_height: int = 0
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    is_preview: bool = False
    preview_scale_factor: float = 1.0
    compression: str = ""
    data_base64: str = ""


class CubeNoiseRequest(BaseModel):
    dataset_id: str
    x0: int
    x1: int
    y0: int
    y1: int
    channel_start: int = 0
    channel_end: int = 0


class CubeNoiseResponse(BaseModel):
    valid: bool
    error: str
    channel_start: int = 0
    channel_end: int = 0
    num_channels: int = 0
    mad: list[float] = []
    sigma: list[float] = []
    region: dict = {}
    # Fraction (0..1) of finite pixels in the selected region that were
    # exactly 0. A high value typically means the region overlaps off-source
    # blanked area and the noise estimate may be unreliable.
    blank_fraction: float = 0.0


class CatalogueOpenRequest(BaseModel):
    path: str
    format: str = "csv"
    # Optional: open SEVERAL catalogue files of the same format as ONE catalogue
    # (e.g. the 27 spatial subtables of a cosmological snapshot). When non-empty
    # these are concatenated; `path` is then a fallback label only.
    paths: list[str] = []
    # A VBT point table only: read it in this byte order (issue #165).
    byte_order: VbtByteOrder = None


class CatalogueFieldInfo(BaseModel):
    name: str
    type: str


class CatalogueCoordinateFields(BaseModel):
    ra: str = ""
    dec: str = ""
    #: Galactic longitude/latitude columns, when the catalogue carries l/b
    #: instead of RA/Dec — every VLKB table does. Reported as themselves, with
    #: `system` naming the frame, rather than copied into ra/dec: l/b relabelled
    #: as RA/Dec places a source up to 90 degrees from where it is.
    glon: str = ""
    glat: str = ""
    x: str = ""
    y: str = ""
    z: str = ""
    #: "sky" (RA/DEC) · "galactic" (l/b) · "cartesian" (X/Y/Z, e.g. Mpc)
    system: str = "sky"


class CatalogueOpenResponse(BaseModel):
    valid: bool
    error: str
    dataset_id: str = ""
    session_id: str = ""
    num_rows: int = 0
    fields: list[CatalogueFieldInfo] = []
    coordinate_fields: CatalogueCoordinateFields = CatalogueCoordinateFields()
    # The points can be drawn on this backend (/v1/render, issue #83): a VBT
    # point table, on a backend that renders server-side.
    server_side_rendering_available: bool = False
    # A VBT point table (issue #165): as for OpenDatasetResponse.
    byte_order: str = ""
    byte_order_declared: str = ""
    byte_order_warning: VbtByteOrderWarning | None = None
    # The format read ("vbt", "gadget", "csv", ...): a particle snapshot is
    # recognised by its content, whatever format was asked for (issue #31).
    format: str = ""
    # A simulation's particles read in place (a VBT of points, a snapshot's
    # particle type): cartesian, drawn ray-traced by default.
    particles: bool = False
    # The path the dataset is known by: for a snapshot, its particle type's
    # reference ("<first file>::gadget-type=<N>").
    path: str = ""
    # A snapshot's particle types with particles, each opened by its own path.
    components: list[ParticleComponentInfo] = []
    # A snapshot's header (time, redshift, box, cosmology, unit system, blocks)
    # and this particle type's columns, units and physical conversions.
    snapshot: dict[str, Any] = {}
    component: dict[str, Any] = {}


class CatalogueSubsetRequest(BaseModel):
    dataset_id: str
    max_rows: int = 100000


class CatalogueSubsetResponse(BaseModel):
    valid: bool
    error: str
    rows: list[dict[str, Any]] = []


class TabularBox(BaseModel):
    fields: list[str]
    min: list[float]
    max: list[float]
    # The period of the three coordinates (a periodic simulation box), so the
    # box wraps round it; None: the dataset's own when it states one (a
    # cosmological Gadget snapshot's BoxSize), 0: no wrapping.
    period: float | None = None


class TabularSphere(BaseModel):
    fields: list[str]
    center: list[float]
    radius: float
    # As TabularBox.period: distances by the nearest periodic image.
    period: float | None = None


class TabularFilter(BaseModel):
    # A scalar predicate has field/op/value. A reproducible spatial predicate
    # has box or sphere plus op=inside|outside and an optional user-facing name.
    field: str = ""
    op: str = ""
    value: str = ""
    name: str = ""
    box: TabularBox | None = None
    sphere: TabularSphere | None = None


class TabularSortSpec(BaseModel):
    field: str
    direction: str = "asc"   # "asc" or "desc"


class TabularQueryRequest(BaseModel):
    dataset_id: str
    limit: int = 100000
    offset: int = 0
    # Empty means every column.  Large particle tables can request only the
    # coordinates needed for first paint, then hydrate all attributes in a
    # background query over the same deterministic sample.
    fields: list[str] = []
    filters: list[TabularFilter] = []
    sort: TabularSortSpec | None = None
    # When more rows match than ``limit``: an even sample of them (one in every
    # ``step`` of the response, from the ``phase``-th) instead of a page.
    even_sample: bool = False
    # Reproducible randomized stratified pages; phases are disjoint and together
    # cover all matches. Overrides even_sample; offset is unused while sampled.
    random_sample: bool = False
    phase: int = 0


class TabularQueryResponse(BaseModel):
    valid: bool
    error: str
    total_rows: int = 0
    returned_rows: int = 0
    step: int = 1   # number of sample phases (legacy even or randomized)
    # catalogue: row-oriented dicts
    rows: list[dict[str, Any]] = []
    # catalogue: the 0-based position of each returned row in the table as it
    # was opened, before any filter, sort or sample. SAMP names rows by it
    # (table.highlight.row), so a client that loaded a subset can map them.
    row_index: list[int] = []
    # VBT point: column-oriented base64 float64-LE
    num_rows: int = 0
    fields: list[str] = []
    columns: dict[str, str] = {}


class CatalogueDiagnosticsRequest(BaseModel):
    dataset_id: str
    field: str
    weight_field: str = ""
    bins: int = Field(default=64, ge=4, le=512)
    filters: list[TabularFilter] = []
    range_min: float | None = None
    range_max: float | None = None


class CatalogueDiagnosticsResponse(BaseModel):
    valid: bool
    error: str
    field: str = ""
    field_unit: str = ""
    weight_field: str = ""
    weight_unit: str = ""
    selected_rows: int = 0
    finite_rows: int = 0
    minimum: float = 0.0
    maximum: float = 0.0
    mean: float = 0.0
    stddev: float = 0.0
    weighted_mean: float | None = None
    weight_sum: float | None = None
    bin_edges: list[float] = []
    counts: list[float] = []
    weighting: str = "unweighted"
    selection: list[str] = []


class ProfileCentre(BaseModel):
    """Where a radial profile or a radius axis is measured from (issue #426).

    ``point``: the three coordinates in ``point``. ``target``: the position
    columns of row ``target_row`` of another open catalogue (a SubFind group
    or subhalo; same coordinate units as the particles). ``shrinking_sphere``:
    the mass-weighted shrinking-sphere centre of the selection (Power et al.
    2003), from ``start`` (default: the selection's periodic centre of mass)
    and ``initial_radius`` (default: its largest distance from the start),
    each sphere ``shrink_factor`` times the last, stopping before fewer than
    min(``min_particles``, max(100, 1 % of the selection)) particles remain.
    ``velocity``: the centre velocity radial velocities are taken against;
    by default the mass-weighted mean velocity of the particles within a
    tenth of the outer radius (at least 1000 of them)."""
    mode: Literal["point", "target", "shrinking_sphere"] = "shrinking_sphere"
    point: list[float] | None = None
    target_dataset_id: str = ""
    target_row: int | None = None
    start: list[float] | None = None
    initial_radius: float | None = None
    shrink_factor: float = Field(default=0.975, ge=0.5, lt=1.0)
    min_particles: int = Field(default=1000, ge=10)
    velocity: list[float] | None = None


class ParticleProfileRequest(BaseModel):
    dataset_id: str
    filters: list[TabularFilter] = []
    position_fields: list[str] = ["x", "y", "z"]
    #: None: vx, vy, vz when the table has them; []: no velocities.
    velocity_fields: list[str] | None = None
    #: None: "mass" when the table has it; "": every particle weighs 1.
    mass_field: str | None = None
    #: Numeric columns profiled per shell, and "radial_velocity".
    columns: list[str] = []
    centre: ProfileCentre = ProfileCentre()
    r_min: float | None = None
    r_max: float | None = None
    bins: int = Field(default=32, ge=4, le=256)
    #: Polar axis of the polar and azimuthal velocity components; None: the
    #: angular momentum of the particles the centre velocity is taken from.
    axis: list[float] | None = None
    #: None: the dataset's own box (a periodic Gadget snapshot); 0: no
    #: wrapping; a length: that period on the three position fields.
    period: float | None = None
    #: "spherical": shells in the distance from the centre. "cylindrical":
    #: annuli in the distance R from the axis through the centre (r_min,
    #: r_max; the whole selection along the axis, or the slab z_min..z_max
    #: when both are given). "axial": slabs in the signed height z along the
    #: axis (z_min..z_max, default the selection's extent either way), over
    #: every R or only R < r_max. The axis is ``axis``, else the angular
    #: momentum of the central particles, else the table's z.
    geometry: Literal["spherical", "cylindrical", "axial"] = "spherical"
    z_min: float | None = None
    z_max: float | None = None


class ParticleProfileResponse(BaseModel):
    valid: bool
    error: str
    profile: dict[str, Any] = {}
    centre: dict[str, Any] = {}
    units: dict[str, Any] = {}
    provenance: dict[str, Any] = {}


class ParticleTotalsRequest(BaseModel):
    """Physical totals of the selected particles (issue #426)."""
    dataset_id: str
    filters: list[TabularFilter] = []
    #: Only used to read the row count when no other column is needed.
    position_fields: list[str] = ["x", "y", "z"]
    #: None: vx, vy, vz when the table has them; []: none.
    velocity_fields: list[str] | None = None
    #: None: "mass" when the table has it; "": none (no mass-weighted total).
    mass_field: str | None = None
    #: Specific internal energy: None: "u" when the table has it; "": none.
    energy_field: str | None = None


class ParticleTotalsResponse(BaseModel):
    valid: bool
    error: str
    totals: dict[str, Any] = {}
    provenance: dict[str, Any] = {}


class PhaseAxis(BaseModel):
    #: A numeric column, or "radius" / "radial_velocity" (needs a centre).
    field: str
    scale: Literal["linear", "log"] = "linear"
    bins: int = Field(default=64, ge=4, le=512)
    min: float | None = None
    max: float | None = None


class PhaseDiagramRequest(BaseModel):
    dataset_id: str
    filters: list[TabularFilter] = []
    x: PhaseAxis
    y: PhaseAxis
    #: "": every row weighs 1 (a count histogram).
    weight_field: str = ""
    position_fields: list[str] = ["x", "y", "z"]
    velocity_fields: list[str] | None = None
    mass_field: str | None = None
    centre: ProfileCentre | None = None
    period: float | None = None


class PhaseDiagramResponse(BaseModel):
    valid: bool
    error: str
    diagram: dict[str, Any] = {}
    centre: dict[str, Any] = {}
    units: dict[str, Any] = {}
    provenance: dict[str, Any] = {}


class VbtOpenRequest(BaseModel):
    path: str
    format: str = ""
    byte_order: VbtByteOrder = None   # issue #165, as for /v1/catalogue/open


class VbtFieldInfo(BaseModel):
    name: str
    type: str  # "float32" or "float64"


class VbtOpenResponse(BaseModel):
    valid: bool
    error: str
    dataset_id: str = ""
    session_id: str = ""
    kind: str = ""          # "point" or "volume"
    fields: list[VbtFieldInfo] = []
    num_rows: int = 0
    scalar_type: str = ""
    dimensions: list[int] = []    # [nx, ny, nz] for volume
    spacing: list[float] = []     # [sx, sy, sz] for volume
    byte_order: str = ""          # issue #165, as for /v1/catalogue/open
    byte_order_declared: str = ""
    byte_order_warning: VbtByteOrderWarning | None = None


class VbtSubsetRequest(BaseModel):
    dataset_id: str
    max_rows: int = 0        # 0 → all rows
    field_name: str = ""     # volume: which field to return; empty → first field


class VbtSubsetResponse(BaseModel):
    valid: bool
    error: str
    kind: str = ""
    num_rows: int = 0
    fields: list[str] = []
    # point data: {field_name: base64-encoded float64-LE column}
    columns: dict[str, str] = {}
    # volume data: single-field block
    field_name: str = ""
    block: str = ""         # base64-encoded float32-LE voxel block
    dimensions: list[int] = []
    spacing: list[float] = []


class IsosurfaceProductRequest(BaseModel):
    dataset_id: str
    threshold: float


class IsosurfaceProductResponse(BaseModel):
    valid: bool
    error: str
    compression: str = ""
    points_base64: str = ""
    polys_base64: str = ""
    num_points: int = 0
    num_polys: int = 0


class CosmologyDistanceRequest(BaseModel):
    z: float
    model: str = "Planck18"


class CosmologyDistanceResponse(BaseModel):
    valid: bool
    error: str
    # None (JSON null) when no real value was computed (e.g. valid=False).
    # 0.0 is a VALID redshift/distance, so it must NOT double as a sentinel.
    z: float | None = None
    model: str = ""
    comoving_Mpc: float | None = None
    luminosity_Mpc: float | None = None
    # Proper transverse scale at z (kpc per arcsecond, from D_A).
    scale_kpc_per_arcsec: float | None = None
    # DEPRECATED alias of scale_kpc_per_arcsec (#129): the old name, which is
    # not a proper motion. Same value; to be removed after one release.
    proper_motion_kpc_per_arcsec: float | None = Field(
        default=None, deprecated=True,
        description="Deprecated alias of scale_kpc_per_arcsec; removed after one release.")


class CosmologyBatchRequest(BaseModel):
    redshifts: list[float]
    model: str = "Planck18"


class CosmologyBatchResponse(BaseModel):
    valid: bool
    error: str
    model: str = ""
    # None entries mark invalid/uncomputable redshifts (valid z=0.0 stays 0.0).
    distances_Mpc: list[float | None] = []


class HiPSOpenRequest(BaseModel):
    url: str


class HiPSSurveyResponse(BaseModel):
    valid: bool
    error: str = ""
    survey_id: str = ""
    session_id: str = ""
    base_url: str = ""
    title: str = ""
    frame: str = "equatorial"   # "equatorial" | "galactic" | "ecliptic"
    max_order: int = 9
    min_order: int = 0
    tile_width: int = 512
    tile_formats: list[str] = []  # e.g. ["png", "jpeg"]


class HiPSTileInfo(BaseModel):
    pix: int
    order: int
    ra_center: float    # degrees
    dec_center: float   # degrees
    corners_ra: list[float]   # 4 values, degrees
    corners_dec: list[float]  # 4 values, degrees


class HiPSViewRequest(BaseModel):
    survey_id: str
    ra_center: float
    dec_center: float
    fov_deg: float
    order: int
    # Angular radius that actually covers the viewport. A field of view quoted
    # across the width says nothing about a tall window or a curved
    # projection; 0 falls back to deriving it from fov_deg.
    radius_deg: float = 0.0


class HiPSViewResponse(BaseModel):
    valid: bool
    error: str = ""
    tiles: list[HiPSTileInfo] = []


class TargetResolveRequest(BaseModel):
    name: str


class TargetResolveResponse(BaseModel):
    valid: bool
    error: str = ""
    ra_deg: float = 0.0
    dec_deg: float = 0.0
    resolved_name: str = ""
    source: str = ""


class CatalogueOverlayRequest(BaseModel):
    catalogue_path: str      # backend-side absolute path to a CSV file
    ra_center: float
    dec_center: float
    fov_deg: float
    # Angular radius that actually covers the viewport, corners included. The
    # client knows its aspect ratio and projection and can measure it; without
    # it the field is guessed from fov_deg alone and the corners are cut off.
    radius_deg: float = 0.0  # 0 = derive it from fov_deg
    ra_field: str = ""       # auto-detected if empty
    dec_field: str = ""
    max_sources: int = 5000  # spatial sub-sample cap


class HiPSRegistryEntry(BaseModel):
    id: str = ""
    title: str = ""
    url: str = ""
    category: str = ""
    regime: str = ""
    frame: str = ""
    tile_formats: list[str] = []
    kind: str = ""          # image | catalog | cube | spectral-cube | unknown
    max_order: int = 0


class HiPSRegistryResponse(BaseModel):
    valid: bool
    error: str = ""
    surveys: list[HiPSRegistryEntry] = []


class CatalogueOverlaySource(BaseModel):
    ra: float
    dec: float


class CatalogueOverlayResponse(BaseModel):
    valid: bool
    error: str = ""
    sources: list[CatalogueOverlaySource] = []
    total_in_view: int = 0
    truncated: bool = False



# ── SoFiA-2 suggested parameters (issue #64) ────────────────────────────────

class Sofia2SuggestRequest(BaseModel):
    """``data_type`` and ``line`` override what the suggester deduces: send
    them when the previous answer was ``ambiguous``. (The "goal" field of the
    first draft is gone with the flux goal; like any unknown field it is
    ignored, so a client that still sends it is not refused.)"""
    data_type: Literal["emission", "absorption", "continuum"] | None = None
    line: Literal["hi", "galactic"] | None = None


class Sofia2Choice(BaseModel):
    """A deduction the user may have to confirm: ask when ``ambiguous``."""
    value: str
    deduced: bool
    ambiguous: bool
    reason: str
    options: list[str]
    confidence: str = ""        # data type only: high | medium | low | user


class Sofia2SuggestedParameter(BaseModel):
    key: str                    # SoFiA-2 name, e.g. "scfind.kernelsXY"
    value: str                  # as written in the .par, e.g. "0, 5, 10", "true"
    reason: str
    sofia_default: str | None   # SoFiA-2 2.7 built-in default; None when it has none
    changed: bool               # value differs from that default


class Sofia2SuggestWarning(BaseModel):
    code: str                   # memory | no_beam | no_channel_width | reliability_caveat | zero_blanks | blank_footprint | channel_offsets
    message: str


class Sofia2SuggestResponse(BaseModel):
    valid: bool
    error: str = ""
    dataset_id: str = ""
    sofia_version: str = ""
    data_type: Sofia2Choice | None = None
    line: Sofia2Choice | None = None
    parameters: list[Sofia2SuggestedParameter] = []
    params: dict[str, str] = {}     # the same, ready for POST /v1/tasks/sourcefind
    sofia_defaults: dict[str, str] = {}  # SoFiA-2 2.7's default of every key a run accepts ("" when none)
    measurements: dict[str, Any] = {}
    warnings: list[Sofia2SuggestWarning] = []
