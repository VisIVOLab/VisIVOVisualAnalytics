from __future__ import annotations

from typing import Any
from pydantic import BaseModel

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


class FileClassifyResponse(BaseModel):
    """What one file is, so a single *Open…* can decide where it goes."""
    valid: bool
    error: str = ""
    path: str = ""
    #: image | cube | dynspec | fits-table | catalogue | vbt | directory | unknown
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


class OpenDatasetRequest(BaseModel):
    path: str


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
    # Size-aware loading policy. Clients use ``recommended_load_mode``
    # to decide whether to auto-load full resolution or default to
    # preview-only. Threshold is configurable via
    # VISIVO_FULL_RES_THRESHOLD_BYTES (default 4 GiB).
    file_size_bytes: int = 0
    full_res_threshold_bytes: int = 4 * 1024 ** 3
    recommended_load_mode: str = "auto_full"  # "auto_full" | "preview_only"
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


class CubePreviewResponse(BaseModel):
    valid: bool
    error: str
    width: int = 0
    height: int = 0
    depth: int = 0
    scalar_type: str = ""
    range_min: float = 0.0
    range_max: float = 0.0
    # Empty string = uncompressed raw float32 (legacy default).
    # "qt-zlib"  = zlib bytes with 4-byte big-endian length prefix
    #              (Qt's qUncompress convention). Required for the
    #              compressed preview path; without it pydantic drops
    #              the field, the client falls back to "uncompressed",
    #              decodes zlib bytes as raw float32 → grey slice.
    compression: str = ""
    data_base64: str = ""


class CubeSliceRequest(BaseModel):
    dataset_id: str
    axis: str
    index: int


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
    vertices: list[list[int]]
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


class CatalogueSubsetRequest(BaseModel):
    dataset_id: str
    max_rows: int = 100000


class CatalogueSubsetResponse(BaseModel):
    valid: bool
    error: str
    rows: list[dict[str, Any]] = []


class TabularFilter(BaseModel):
    field: str
    op: str    # "=", "!=", ">", ">=", "<", "<=", "contains", "in"
    value: str


class TabularSortSpec(BaseModel):
    field: str
    direction: str = "asc"   # "asc" or "desc"


class TabularQueryRequest(BaseModel):
    dataset_id: str
    limit: int = 100000
    offset: int = 0
    filters: list[TabularFilter] = []
    sort: TabularSortSpec | None = None


class TabularQueryResponse(BaseModel):
    valid: bool
    error: str
    total_rows: int = 0
    returned_rows: int = 0
    # catalogue: row-oriented dicts
    rows: list[dict[str, Any]] = []
    # VBT point: column-oriented base64 float64-LE
    num_rows: int = 0
    fields: list[str] = []
    columns: dict[str, str] = {}


class VbtOpenRequest(BaseModel):
    path: str
    format: str = ""


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
    proper_motion_kpc_per_arcsec: float | None = None


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

