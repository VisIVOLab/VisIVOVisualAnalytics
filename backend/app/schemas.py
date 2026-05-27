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
    spacing: list[float] = [1.0, 1.0, 1.0]
    origin: list[float] = [0.0, 0.0, 0.0]
    ctype: list[str] = ["", "", ""]
    cunit: list[str] = ["", "", ""]
    crval: list[float] = [0.0, 0.0, 0.0]
    crpix: list[float] = [1.0, 1.0, 1.0]
    cdelt: list[float] = [1.0, 1.0, 1.0]
    beam_major: float | None = None
    beam_minor: float | None = None
    beam_pa: float | None = None


class MomentProductRequest(BaseModel):
    dataset_id: str
    moment_order: int
    channel_start: int = 0
    channel_end: int = 0
    mask_enabled: bool = False
    threshold_value: float = 0.0


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


class CatalogueFieldInfo(BaseModel):
    name: str
    type: str


class CatalogueCoordinateFields(BaseModel):
    ra: str = ""
    dec: str = ""


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
    z: float = 0.0
    model: str = ""
    comoving_Mpc: float = 0.0
    luminosity_Mpc: float = 0.0
    proper_motion_kpc_per_arcsec: float = 0.0


class CosmologyBatchRequest(BaseModel):
    redshifts: list[float]
    model: str = "Planck18"


class CosmologyBatchResponse(BaseModel):
    valid: bool
    error: str
    model: str = ""
    distances_Mpc: list[float] = []


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
    ra_field: str = ""       # auto-detected if empty
    dec_field: str = ""
    max_sources: int = 5000  # spatial sub-sample cap


class CatalogueOverlaySource(BaseModel):
    ra: float
    dec: float


class CatalogueOverlayResponse(BaseModel):
    valid: bool
    error: str = ""
    sources: list[CatalogueOverlaySource] = []
    total_in_view: int = 0
    truncated: bool = False

