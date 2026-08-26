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
    # Executor: (dataset_path, params) -> result dict. ``dataset_path`` may be
    # None for tools that don't operate on the loaded dataset (e.g. cosmology).
    executor: Callable[[str | None, dict[str, Any]], dict[str, Any]] = field(repr=False)

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
    ]
}


def tool_specs() -> list[dict[str, Any]]:
    return [t.spec() for t in TOOLS.values()]
