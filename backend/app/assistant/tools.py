"""Tool catalogue for the scientific copilot.

Each tool is a thin, whitelisted wrapper over an EXISTING validated compute
worker. Executors return a compact JSON-able dict (facts + units, never raw
pixel arrays) plus the provenance needed to reproduce them. The copilot may
only quote numbers that came out of one of these calls.
"""
from __future__ import annotations

import base64
from dataclasses import dataclass, field
from typing import Any, Callable

import numpy as np

from ..fits_dataset import ScientificFitsDataset


@dataclass(frozen=True)
class AssistantTool:
    name: str
    description: str
    # JSON Schema (draft-2020) of the parameters, LLM-tool-call compatible.
    parameters: dict[str, Any]
    # Executor: (dataset_path, params) -> result dict, or, when ``wants_context``
    # is set, (dataset_path, params, context) where ``context`` is the resolved
    # session dataset entry (kind/format/path and, for catalogues, the already
    # loaded/concatenated ``dataframe``). ``dataset_path`` may be None for tools
    # that don't operate on the loaded dataset (e.g. cosmology).
    executor: Callable[..., dict[str, Any]] = field(repr=False)
    # When True the engine passes the resolved dataset entry as a third argument,
    # so the tool operates on the in-session data (correct for multi-file /
    # already-transformed datasets) rather than re-reading a single path.
    wants_context: bool = False
    # Action tools do NOT compute a read-only result — they emit a display/analysis
    # DIRECTIVE the desktop app executes on the active viewer. Their executor
    # validates the parameters and returns {"action": <directive>, "status":"queued"};
    # the engine collects the directive into the response's ``actions`` list and
    # feeds the ack back to the model so it can chain further steps.
    is_action: bool = False

    def spec(self) -> dict[str, Any]:
        """Anthropic/OpenAI-style tool spec for a provider to consume."""
        return {
            "name": self.name,
            "description": self.description,
            "input_schema": self.parameters,
        }


# ── Executors ────────────────────────────────────────────────────────────────

def _dataset_metadata(path: str | None, _params: dict[str, Any]) -> dict[str, Any]:
    if not path:
        raise ValueError("dataset_metadata requires a loaded dataset.")
    ds = ScientificFitsDataset(path)
    prov = ds.provenance_context()
    prov["has_per_channel_beam"] = ds.has_per_channel_beam
    try:
        prov["spatial_pixel_scale_arcsec"] = ds.spatial_pixel_scale_arcsec()
    except Exception:  # noqa: BLE001
        prov["spatial_pixel_scale_arcsec"] = None
    return prov


def _cube_statistics(path: str | None, _params: dict[str, Any]) -> dict[str, Any]:
    if not path:
        raise ValueError("cube_statistics requires a loaded dataset.")
    from ..compute import worker_cube_stats  # noqa: PLC0415
    return worker_cube_stats(path)


def _moment_map_summary(path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    if not path:
        raise ValueError("moment_map_summary requires a loaded dataset.")
    from ..compute import worker_moment  # noqa: PLC0415
    order = int(params.get("order", 0))
    if order not in (0, 1, 2):
        raise ValueError("moment_map_summary supports order 0, 1 or 2.")
    # Resolve the channel range against the actual cube depth; default to the
    # FULL spectral depth when the caller gave no (or an empty) range, and report
    # the EFFECTIVE range actually used (worker_moment silently clamps).
    ds = ScientificFitsDataset(path)
    nz = int(ds.axis_sizes[ds.cube_numpy_spectral_fits_axis() - 1]) if ds.axis_sizes else 0
    cs = int(params.get("channel_start", 0))
    ce = int(params.get("channel_end", 0))
    if ce <= cs:                      # unset / empty → whole spectral axis
        cs, ce = 0, max(0, nz - 1)
    cs = max(0, min(cs, max(0, nz - 1)))
    ce = max(cs, min(ce, max(0, nz - 1)))
    mask = bool(params.get("mask_enabled", False))
    thr = float(params.get("threshold_value", 0.0))
    variance = bool(params.get("moment2_variance", False))
    res = worker_moment(path, order, cs, ce, mask, thr, False, variance)
    # Return only summary facts — not the pixel array — for the narrator.
    arr = np.frombuffer(base64.b64decode(res["data_base64"]), dtype=np.float32)
    finite = arr[np.isfinite(arr)]
    return {
        "order": order,
        "channel_start": cs,          # effective (clamped) range actually used
        "channel_end": ce,
        "n_channels_in_cube": nz,
        "moment_unit": res.get("moment_unit"),
        "bunit": res.get("bunit"),
        "spectral_axis_unit": res.get("spectral_axis_unit"),
        "range_min": res.get("range_min"),
        "range_max": res.get("range_max"),
        "finite_pixels": int(finite.size),
        "mean": float(finite.mean()) if finite.size else None,
        "median": float(np.median(finite)) if finite.size else None,
        "note": ("moment 2 is the velocity dispersion σ by default (set "
                 "moment2_variance=true for σ²)") if order == 2 else None,
    }


_COSMO_MODELS = {"Planck18", "Planck15", "Planck13", "WMAP9"}


def _hi_products(path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    if not path:
        raise ValueError("hi_products requires a loaded dataset.")
    from ..compute import worker_hi_products  # noqa: PLC0415
    return worker_hi_products(
        path,
        int(params.get("channel_start", 0)),
        int(params.get("channel_end", 0)),
        params.get("distance_mpc"),
        bool(params.get("mask_enabled", False)),
        float(params.get("threshold_value", 0.0)),
        params.get("rest_freq_hz"),
        bool(params.get("threshold_auto", False)),
    )


def _cosmology_distance(_path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    import astropy.cosmology as _cosmo  # noqa: PLC0415
    import astropy.units as u  # noqa: PLC0415

    z = float(params["redshift"])
    if not np.isfinite(z) or z <= -1.0:
        raise ValueError("redshift must be a finite value > -1.")
    model = str(params.get("model", "Planck18"))
    if model not in _COSMO_MODELS:
        raise ValueError(f"unknown cosmology model {model!r}; use one of {sorted(_COSMO_MODELS)}")
    cosmo = getattr(_cosmo, model)
    return {
        "redshift": z,
        "model": model,
        "comoving_Mpc": float(cosmo.comoving_distance(z).to(u.Mpc).value),
        "luminosity_Mpc": float(cosmo.luminosity_distance(z).to(u.Mpc).value),
        "angular_diameter_Mpc": float(cosmo.angular_diameter_distance(z).to(u.Mpc).value),
        "H0_km_s_Mpc": float(cosmo.H0.value),
    }


# Catalogue-file extension → the format token _catalogue_load_dataframe expects.
_CATALOGUE_SUFFIX_FMT = {
    ".csv": "csv",
    ".vot": "votable",
    ".votable": "votable",
    ".xml": "votable",
    ".fits": "fits",
    ".fit": "fits",
    ".speck": "speck",
    ".tbl": "ipac",
}


def _catalogue_summary(path: str | None, params: dict[str, Any],
                       context: dict[str, Any] | None = None) -> dict[str, Any]:
    """Structure + per-column numeric ranges of the loaded point catalogue.

    Operates on the catalogue the desktop opened (CSV / VOTable / FITS-table /
    Partiview .speck / IPAC .tbl) and returns its columns, types, coordinate
    system and — for numeric columns — min/max/mean/finite-count. These are the
    facts a cosmology copilot needs to reason about a redshift / position
    catalogue. No row data is returned, only aggregate statistics.

    Prefers the DataFrame already cached in the session entry (correct for
    multi-file catalogues the backend concatenated, and immune to the file
    changing after open); only if that is absent does it re-read ``path``,
    inferring the format from the file extension.
    """
    from ..dependencies import (  # noqa: PLC0415
        _catalogue_load_dataframe,
        _catalogue_metadata_from_dataframe,
    )

    frame = None
    fmt = None
    if context is not None:
        frame = context.get("dataframe")
        fmt = context.get("format")
    if frame is None:
        if not path:
            raise ValueError("catalogue_summary requires a loaded catalogue dataset.")
        from pathlib import Path  # noqa: PLC0415

        p = Path(path)
        fmt = _CATALOGUE_SUFFIX_FMT.get(p.suffix.lower())
        if fmt is None:
            raise ValueError(
                f"unsupported catalogue extension {p.suffix!r}; expected one of "
                f"{sorted(set(_CATALOGUE_SUFFIX_FMT))}"
            )
        frame = _catalogue_load_dataframe(p, fmt)
    meta = _catalogue_metadata_from_dataframe(frame, str(fmt or "unknown"))
    max_cols = max(1, int(params.get("max_columns", 40)))
    columns: list[dict[str, Any]] = []
    for info in meta["fields"][:max_cols]:
        name = info["name"]
        entry: dict[str, Any] = {"name": name, "type": info["type"]}
        if info["type"] in ("int", "float"):
            col = frame[name].to_numpy(dtype="float64", na_value=np.nan)
            fin = col[np.isfinite(col)]
            # Always report finite_count so the copilot can tell "numeric but
            # entirely non-finite" (count 0, stats None) from "not computed".
            entry["finite_count"] = int(fin.size)
            entry["min"] = float(fin.min()) if fin.size else None
            entry["max"] = float(fin.max()) if fin.size else None
            entry["mean"] = float(fin.mean()) if fin.size else None
        columns.append(entry)
    return {
        "kind": "catalogue",
        "format": meta["format"],
        "num_rows": meta["num_rows"],
        "num_columns": len(meta["fields"]),
        "coordinate_fields": meta["coordinate_fields"],
        "columns": columns,
        "columns_truncated": len(meta["fields"]) > max_cols,
    }


def _vbt_summary(_path: str | None, params: dict[str, Any],
                 context: dict[str, Any] | None = None) -> dict[str, Any]:
    """Structure + per-field numeric ranges of the loaded VBT point/volume table.

    Reads the memory-mapped VBT binary already open in the session (no re-read of
    disk) and returns row count, the VBT kind (point / volume), and per field
    min/max/mean/finite-count. Row data is never returned, only aggregate stats.
    """
    if not context or context.get("kind") != "vbt":
        raise ValueError("vbt_summary requires a loaded VBT dataset.")
    header = context["header"]
    memmap = context["memmap"]  # shape (field_count, num_rows), read-only
    names = list(header["field_names"])
    max_fields = max(1, int(params.get("max_fields", 40)))
    fields: list[dict[str, Any]] = []
    for i, name in enumerate(names[:max_fields]):
        # Stream the memmapped row in chunks — never materialise a full-length
        # float64 promotion of a huge VBT column (out-of-core, bounded RAM).
        row = memmap[i]
        n = int(row.shape[0])
        count = 0
        total = 0.0
        vmin: float | None = None
        vmax: float | None = None
        step = 1_000_000
        for start in range(0, n, step):
            block = np.asarray(row[start:start + step], dtype="float64")
            block = block[np.isfinite(block)]
            if block.size:
                count += int(block.size)
                total += float(block.sum())
                bmin = float(block.min())
                bmax = float(block.max())
                vmin = bmin if vmin is None else min(vmin, bmin)
                vmax = bmax if vmax is None else max(vmax, bmax)
        fields.append({
            "name": str(name),
            "finite_count": count,
            "min": vmin,
            "max": vmax,
            "mean": (total / count) if count else None,
        })
    return {
        "kind": "vbt",
        "vbt_kind": header.get("kind"),   # "point" or "volume"
        "num_rows": int(header["num_rows"]),
        "num_fields": int(header["field_count"]),
        "scalar_type": header.get("scalar_type"),
        "fields": fields,
        "fields_truncated": len(names) > max_fields,
    }


def _int_param(params: dict[str, Any], key: str, default: int | None = None) -> int:
    """Coerce a tool argument to int, REJECTING non-integral floats.

    The engine does not JSON-schema-validate arguments, so ``channel=7.9`` or
    ``n_bins=3.9`` would otherwise silently run a different measurement.
    """
    if key not in params or params[key] is None:
        if default is None:
            raise ValueError(f"{key} is required.")
        return default
    v = params[key]
    if isinstance(v, bool):
        raise ValueError(f"{key} must be an integer.")
    if isinstance(v, float):
        if not v.is_integer():
            raise ValueError(f"{key} must be an integer, got {v}.")
        return int(v)
    if not isinstance(v, int):
        raise ValueError(f"{key} must be an integer.")
    return v


def _finite_float(params: dict[str, Any], key: str, *, positive: bool = False) -> float:
    raw = params[key]
    if isinstance(raw, bool):   # True/False would coerce to 1.0/0.0 silently
        raise ValueError(f"{key} must be a number.")
    v = float(raw)
    if not np.isfinite(v):
        raise ValueError(f"{key} must be a finite number.")
    if positive and not (v > 0):
        raise ValueError(f"{key} must be > 0.")
    return v


def _noise_estimate(path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    """Per-channel noise (MAD + Gaussian σ) over a box, summarised.

    Defaults to the full spatial extent and full spectral depth; a caller can
    restrict it to an emission-free box / channel range for a cleaner estimate.
    Returns the median (and range) of the per-channel σ and MAD — the numbers a
    detection threshold (e.g. 3–5 σ) should be built on.
    """
    if not path:
        raise ValueError("noise_estimate requires a loaded cube.")
    from ..compute import worker_noise_estimate  # noqa: PLC0415
    ds = ScientificFitsDataset(path)
    # worker_noise_estimate reads squeeze_to_3d(raw) as (depth, height, width) —
    # numpy axis 0 (the channel axis) is the LAST active FITS axis. If the actual
    # spectral axis isn't that one, "per-channel" noise would be measured along a
    # spatial axis, so refuse rather than return a wrong number.
    if ds.active_axes and ds.cube_numpy_spectral_fits_axis() != ds.active_axes[-1]:
        raise ValueError(
            "noise_estimate needs the spectral axis to be the cube's last axis "
            "(e.g. NAXIS3); this cube's spectral axis is elsewhere."
        )
    # Mirror the worker's squeeze exactly (squeezed FITS sizes, reversed by numpy).
    squeezed = [s for s in ds.axis_sizes if s > 1]
    w = squeezed[0] if squeezed else 1
    h = squeezed[1] if len(squeezed) > 1 else 1
    nz = squeezed[2] if len(squeezed) > 2 else 1
    x0 = max(0, _int_param(params, "x0", 0))
    x1 = max(0, _int_param(params, "x1", w - 1))
    y0 = max(0, _int_param(params, "y0", 0))
    y1 = max(0, _int_param(params, "y1", h - 1))
    cs = max(0, _int_param(params, "channel_start", 0))
    ce = max(0, _int_param(params, "channel_end", max(0, nz - 1)))
    res = worker_noise_estimate(path, x0, x1, y0, y1, cs, ce)
    sigma = np.asarray(res.get("sigma", []), dtype="float64")
    mad = np.asarray(res.get("mad", []), dtype="float64")
    sigma = sigma[np.isfinite(sigma)]
    mad = mad[np.isfinite(mad)]
    return {
        "bunit": ds.bunit,
        "channel_start": res.get("channel_start"),
        "channel_end": res.get("channel_end"),
        "num_channels": res.get("num_channels"),
        "region": res.get("region"),
        "blank_fraction": res.get("blank_fraction"),
        "sigma_median": float(np.median(sigma)) if sigma.size else None,
        "sigma_min": float(sigma.min()) if sigma.size else None,
        "sigma_max": float(sigma.max()) if sigma.size else None,
        "mad_median": float(np.median(mad)) if mad.size else None,
        "mad_min": float(mad.min()) if mad.size else None,
        "mad_max": float(mad.max()) if mad.size else None,
        "note": "σ is the per-channel Gaussian noise (MAD-based); use k×σ for a k-sigma threshold.",
    }


def _aperture_photometry(path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    """Background-subtracted aperture flux at a sky position (needs WCS).

    ``ra_deg``/``dec_deg`` and ``aperture_arcsec`` are required; the background
    annulus defaults to [aperture, 2×aperture] when not given. ``channel`` selects
    a cube plane (omit for a 2-D image).
    """
    if not path:
        raise ValueError("aperture_photometry requires a loaded image or cube.")
    from ..compute import worker_aperture_photometry  # noqa: PLC0415
    ra = _finite_float(params, "ra_deg")
    dec = _finite_float(params, "dec_deg")
    ap = _finite_float(params, "aperture_arcsec", positive=True)
    inner = float(params.get("annulus_inner_arcsec", ap))
    outer = float(params.get("annulus_outer_arcsec", 2.0 * ap))
    if not (np.isfinite(inner) and np.isfinite(outer)):
        raise ValueError("annulus radii must be finite.")
    if not (0 < inner < outer):
        raise ValueError("require 0 < annulus_inner_arcsec < annulus_outer_arcsec.")
    channel = None if params.get("channel") is None else max(0, _int_param(params, "channel"))
    return worker_aperture_photometry(path, channel, ra, dec, ap, inner, outer)


def _source_gaussfit(path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    """Fit a 2-D elliptical Gaussian to a source (needs WCS).

    Returns peak, integrated flux (Jy when beam-corrected), fitted and
    beam-deconvolved FWHMs and position angle — a compact source characterisation.
    ``ra_deg``/``dec_deg`` and ``box_arcsec`` (the fit cut-out size) are required;
    ``channel`` selects a cube plane (omit for a 2-D image).
    """
    if not path:
        raise ValueError("source_gaussfit requires a loaded image or cube.")
    from ..compute import worker_gauss_fit_2d  # noqa: PLC0415
    ra = _finite_float(params, "ra_deg")
    dec = _finite_float(params, "dec_deg")
    box = _finite_float(params, "box_arcsec", positive=True)
    channel = None if params.get("channel") is None else max(0, _int_param(params, "channel"))
    return worker_gauss_fit_2d(path, channel, ra, dec, box)


def _angular_power_spectrum(path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    """Relative (un-calibrated) radial power spectrum of a loaded 2-D map.

    Bins the 2-D Fourier power of the map's pixel grid into ``n_bins`` radial
    bins (no HEALPix). This is a RELATIVE spectrum — the amplitude is not a
    calibrated angular C(ℓ); use it to compare structure scales, and quote the
    worker's own caveat, not an absolute C(ℓ).
    """
    if not path:
        raise ValueError("angular_power_spectrum requires a loaded map.")
    from ..compute import worker_angular_power_spectrum  # noqa: PLC0415
    channel = max(0, _int_param(params, "channel", 0))
    n_bins = max(2, _int_param(params, "n_bins", 64))
    return worker_angular_power_spectrum(path, channel, n_bins)


# ── Action executors (emit a viewer directive; the desktop app applies it) ────
# These do NOT compute a result — they validate the parameters and return an
# {"action": <directive>, "status": "queued"} envelope. The directive's ``type``
# routes it, in the app, to the active viewer's copilot-action handler. All are
# display-only and reversible (the app snapshots state before applying, and
# offers Undo), so the copilot can regulate the visualisation autonomously.

def _act(directive: dict[str, Any]) -> dict[str, Any]:
    return {"action": directive, "status": "queued"}


def _act_set_colormap(_path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    name = str(params.get("name", "")).strip()
    if not name:
        raise ValueError("colormap name must be a non-empty string")
    return _act({"type": "set_colormap", "name": name})


def _act_set_threshold(_path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    value = float(params["value"])
    if not np.isfinite(value):
        raise ValueError("threshold must be a finite number")
    return _act({"type": "set_threshold", "value": value})


def _act_set_render_mode(_path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    mode = str(params.get("mode", "")).lower()
    if mode not in ("volume", "isosurface"):
        raise ValueError("mode must be 'volume' or 'isosurface'")
    return _act({"type": "set_render_mode", "mode": mode})


def _act_set_blend_mode(_path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    mode = str(params.get("mode", "")).lower()
    if mode not in ("composite", "mip", "minip"):
        raise ValueError("mode must be 'composite', 'mip', or 'minip'")
    return _act({"type": "set_blend_mode", "mode": mode})


def _act_goto_channel(_path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    index = int(params["index"])
    if index < 0:
        raise ValueError("channel index must be >= 0")
    return _act({"type": "goto_channel", "index": index})


def _act_reset_camera(_path: str | None, _params: dict[str, Any]) -> dict[str, Any]:
    return _act({"type": "reset_camera"})


def _act_compute_moment(_path: str | None, params: dict[str, Any]) -> dict[str, Any]:
    order = int(params.get("order", 0))
    if order not in (0, 1, 2):
        raise ValueError("moment order must be 0, 1, or 2")
    directive: dict[str, Any] = {"type": "compute_moment", "order": order}
    for k in ("channel_start", "channel_end"):
        if k in params:
            directive[k] = int(params[k])
    for k in ("mask_enabled", "threshold_auto"):
        if k in params:
            directive[k] = bool(params[k])
    if "threshold_value" in params:
        directive["threshold_value"] = float(params["threshold_value"])
    return _act(directive)


# ── Registry ─────────────────────────────────────────────────────────────────

TOOLS: dict[str, AssistantTool] = {
    t.name: t for t in [
        AssistantTool(
            name="dataset_metadata",
            description=(
                "Return the loaded cube's metadata: BUNIT, synthesized beam "
                "(BMAJ/BMIN/BPA), whether a per-channel beam table is present, "
                "WCS status/warnings, CTYPE/CUNIT per axis, and the pixel scale. "
                "Call this first to know the units and frame before interpreting "
                "any other result."
            ),
            parameters={"type": "object", "properties": {}, "additionalProperties": False},
            executor=_dataset_metadata,
        ),
        AssistantTool(
            name="cube_statistics",
            description=(
                "Full-resolution, out-of-core pixel statistics of the whole cube "
                "(min, max, mean, std via Chan's algorithm, median, valid/blank "
                "counts). Units are the cube's BUNIT."
            ),
            parameters={"type": "object", "properties": {}, "additionalProperties": False},
            executor=_cube_statistics,
        ),
        AssistantTool(
            name="moment_map_summary",
            description=(
                "Compute a moment map over a channel range and return SUMMARY "
                "facts (unit, value range, mean/median, finite-pixel count) — not "
                "the pixel array. order 0=integrated intensity, 1=velocity field, "
                "2=velocity dispersion σ (set moment2_variance=true for variance σ²)."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "order": {"type": "integer", "enum": [0, 1, 2]},
                    "channel_start": {"type": "integer"},
                    "channel_end": {"type": "integer"},
                    "mask_enabled": {"type": "boolean"},
                    "threshold_value": {"type": "number"},
                    "moment2_variance": {"type": "boolean"},
                },
                "required": ["order"],
                "additionalProperties": False,
            },
            executor=_moment_map_summary,
        ),
        AssistantTool(
            name="hi_products",
            description=(
                "Optically-thin HI column density (N_HI, cm^-2) map summary and, "
                "when distance_mpc is given, the integrated HI mass (M_sun) from "
                "moment-0. Requires a Jy/beam cube with a known beam and a "
                "velocity axis (or rest_freq_hz for a frequency axis)."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "channel_start": {"type": "integer"},
                    "channel_end": {"type": "integer"},
                    "distance_mpc": {"type": "number"},
                    "mask_enabled": {"type": "boolean"},
                    "threshold_value": {"type": "number"},
                    "threshold_auto": {"type": "boolean"},
                    "rest_freq_hz": {"type": "number"},
                },
                "additionalProperties": False,
            },
            executor=_hi_products,
        ),
        AssistantTool(
            name="noise_estimate",
            description=(
                "Per-channel noise of the cube (Gaussian σ and MAD), summarised "
                "as the median/min/max σ over a box and channel range. Defaults to "
                "the whole cube; pass an emission-free x0/x1/y0/y1 box for a cleaner "
                "estimate. Use this to ground a detection threshold (k×σ)."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "x0": {"type": "integer"}, "x1": {"type": "integer"},
                    "y0": {"type": "integer"}, "y1": {"type": "integer"},
                    "channel_start": {"type": "integer"},
                    "channel_end": {"type": "integer"},
                },
                "additionalProperties": False,
            },
            executor=_noise_estimate,
        ),
        AssistantTool(
            name="aperture_photometry",
            description=(
                "Background-subtracted aperture flux at a sky position (RA/DEC in "
                "degrees). Requires WCS. Reports the raw sum with its unit and, for "
                "beam-corrected data, the flux in Jy. Give aperture_arcsec (and "
                "optionally the background annulus); channel selects a cube plane."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "ra_deg": {"type": "number"},
                    "dec_deg": {"type": "number"},
                    "aperture_arcsec": {"type": "number"},
                    "annulus_inner_arcsec": {"type": "number"},
                    "annulus_outer_arcsec": {"type": "number"},
                    "channel": {"type": "integer"},
                },
                "required": ["ra_deg", "dec_deg", "aperture_arcsec"],
                "additionalProperties": False,
            },
            executor=_aperture_photometry,
        ),
        AssistantTool(
            name="source_gaussfit",
            description=(
                "Fit a 2-D elliptical Gaussian to a source at a sky position "
                "(RA/DEC in degrees). Requires WCS. Returns peak, integrated flux "
                "(Jy when beam-corrected), fitted and beam-deconvolved FWHMs and "
                "position angle. Give box_arcsec (fit cut-out size); channel selects "
                "a cube plane."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "ra_deg": {"type": "number"},
                    "dec_deg": {"type": "number"},
                    "box_arcsec": {"type": "number"},
                    "channel": {"type": "integer"},
                },
                "required": ["ra_deg", "dec_deg", "box_arcsec"],
                "additionalProperties": False,
            },
            executor=_source_gaussfit,
        ),
        AssistantTool(
            name="angular_power_spectrum",
            description=(
                "RELATIVE (un-calibrated) radial power spectrum of a loaded 2-D "
                "map (e.g. a cosmological / CMB-like map), binned into n_bins "
                "radial bins from the map's Fourier power. NOT a calibrated "
                "angular C(ℓ) — use it to compare structure scales, and quote the "
                "worker's caveat. channel selects a plane for a cube."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "channel": {"type": "integer"},
                    "n_bins": {"type": "integer"},
                },
                "additionalProperties": False,
            },
            executor=_angular_power_spectrum,
        ),
        AssistantTool(
            name="cosmology_distance",
            description=(
                "Comoving / luminosity / angular-diameter distance for a redshift "
                "under a named cosmology (Planck18/Planck15/Planck13/WMAP9), via "
                "astropy. Distances in Mpc."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "redshift": {"type": "number"},
                    "model": {"type": "string"},
                },
                "required": ["redshift"],
                "additionalProperties": False,
            },
            executor=_cosmology_distance,
        ),
        AssistantTool(
            name="catalogue_summary",
            description=(
                "Structure of the loaded point catalogue (cosmology / VBT point "
                "cloud): number of rows, columns with their types, the detected "
                "coordinate system (RA/DEC or Cartesian X/Y/Z), and per numeric "
                "column min/max/mean/finite-count. Use this to know what a "
                "redshift / position catalogue contains before reasoning about it. "
                "Combine a redshift column's range with cosmology_distance to "
                "convert to distances."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "max_columns": {"type": "integer"},
                },
                "additionalProperties": False,
            },
            executor=_catalogue_summary, wants_context=True,
        ),
        AssistantTool(
            name="vbt_summary",
            description=(
                "Structure of the loaded VBT (Visual Binary Table) point/volume "
                "dataset: number of rows, whether it is a point or volume table, "
                "and per field min/max/mean/finite-count. Use this to know what a "
                "VBT contains before reasoning about it or recolouring by a field."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "max_fields": {"type": "integer"},
                },
                "additionalProperties": False,
            },
            executor=_vbt_summary, wants_context=True,
        ),
        # ── Cube visualisation actions (display-only, reversible) ─────────────
        AssistantTool(
            name="set_colormap",
            description=(
                "ACTION: change the active view's colour map to better reveal "
                "structure (e.g. inferno, viridis, magma, gray, cubehelix). Works "
                "for both the 3-D cube and 2-D image viewers."
            ),
            parameters={
                "type": "object",
                "properties": {"name": {"type": "string"}},
                "required": ["name"], "additionalProperties": False,
            },
            executor=_act_set_colormap, is_action=True,
        ),
        AssistantTool(
            name="set_threshold",
            description=(
                "ACTION: set the rendering / isosurface threshold (in the cube's "
                "BUNIT) to hide noise and emphasise a source. Use cube_statistics "
                "first to pick a sensible value (e.g. a few × RMS)."
            ),
            parameters={
                "type": "object",
                "properties": {"value": {"type": "number"}},
                "required": ["value"], "additionalProperties": False,
            },
            executor=_act_set_threshold, is_action=True,
        ),
        AssistantTool(
            name="set_render_mode",
            description="ACTION: switch the 3-D representation between 'volume' and 'isosurface'.",
            parameters={
                "type": "object",
                "properties": {"mode": {"type": "string", "enum": ["volume", "isosurface"]}},
                "required": ["mode"], "additionalProperties": False,
            },
            executor=_act_set_render_mode, is_action=True,
        ),
        AssistantTool(
            name="set_blend_mode",
            description=(
                "ACTION: set the volume blend mode — 'composite' (opacity ray-cast), "
                "'mip' (maximum intensity, good to spot the brightest source), or "
                "'minip' (minimum intensity)."
            ),
            parameters={
                "type": "object",
                "properties": {"mode": {"type": "string", "enum": ["composite", "mip", "minip"]}},
                "required": ["mode"], "additionalProperties": False,
            },
            executor=_act_set_blend_mode, is_action=True,
        ),
        AssistantTool(
            name="goto_channel",
            description=(
                "ACTION: move the spectral slice to a given 0-based channel index "
                "(e.g. the channel where a source peaks)."
            ),
            parameters={
                "type": "object",
                "properties": {"index": {"type": "integer"}},
                "required": ["index"], "additionalProperties": False,
            },
            executor=_act_goto_channel, is_action=True,
        ),
        AssistantTool(
            name="reset_camera",
            description=(
                "ACTION: reset the active view's camera to a default framing "
                "(frames the whole cube; refits the current image view)."
            ),
            parameters={"type": "object", "properties": {}, "additionalProperties": False},
            executor=_act_reset_camera, is_action=True,
        ),
        AssistantTool(
            name="compute_moment",
            description=(
                "ACTION: compute a moment map AND DISPLAY it as a new product in the "
                "session (added to Session Data / shown in a 2-D pane). order 0 = "
                "integrated intensity, 1 = velocity field, 2 = velocity dispersion σ. "
                "Prefer this over moment_map_summary when the user wants to SEE the "
                "map; use moment_map_summary only to report statistics without "
                "displaying. Omit the channel range for the full cube."
            ),
            parameters={
                "type": "object",
                "properties": {
                    "order": {"type": "integer", "enum": [0, 1, 2]},
                    "channel_start": {"type": "integer"},
                    "channel_end": {"type": "integer"},
                    "mask_enabled": {"type": "boolean"},
                    "threshold_value": {"type": "number"},
                    "threshold_auto": {"type": "boolean"},
                },
                "required": ["order"],
                "additionalProperties": False,
            },
            executor=_act_compute_moment, is_action=True,
        ),
    ]
}


def tool_specs() -> list[dict[str, Any]]:
    return [t.spec() for t in TOOLS.values()]
