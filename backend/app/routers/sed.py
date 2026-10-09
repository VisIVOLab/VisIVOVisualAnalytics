"""SED endpoints — greybody fits and VLKB theoretical-model fits.

Three routes, matching the three things the legacy ViaLactea client could do
with a compact-source SED:

* ``POST /v1/sed/fit`` — optically thin and/or optically thick modified-blackbody
  grid fit, computed here (the legacy client shelled out to ``python
  sedfit_main.py`` with ``eval()``-ed argv strings).
* ``POST /v1/sed/models`` — theoretical pre-computed models from the VLKB
  ``searchd`` service, proxied so the odd positional-string protocol and the
  SSRF guard live on the server.
* ``GET /v1/sed/defaults`` — the default fit ranges, so the desktop client does
  not have to hard-code the same numbers a second time.

Fluxes are Jy and wavelengths µm throughout, as in the Hi-GAL band-merged
catalogue (``compactsources.sed_view_final``) the SEDs are built from.
"""
from __future__ import annotations

import asyncio
import json
import logging
import math
import os
import urllib.error as _urllib_error
import urllib.parse as _urllib_parse
import time as _time
import urllib.request as _urllib_request

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field

from ..auth import verify_token
from ..dependencies import assert_safe_fetch_url

router = APIRouter(tags=["sed"])
logger = logging.getLogger("visivo.backend")
_auth = Depends(verify_token)

# Defaults chosen to match the Hi-GAL clump regime the legacy dialogs opened on.
DEFAULT_MASS_RANGE = [1.0, 5000.0, 30]
DEFAULT_TEMP_RANGE = [5.0, 50.0, 30]
DEFAULT_BETA_RANGE = [2.0, 2.0, 1]
DEFAULT_LAMBDA0_RANGE = [10.0, 300.0, 30]
DEFAULT_SCALE_RANGE = [1.0, 1.0, 1]
DEFAULT_KAPPA_REF = 0.1        # cm² g⁻¹ at 300 µm (Hildebrand 1983 / OH94 tail)
DEFAULT_LAMBDA_REF_UM = 300.0

_MODELS_TIMEOUT_S = float(os.environ.get("VISIVO_SED_MODELS_TIMEOUT", "300"))
_MODELS_MAX_BYTES = int(os.environ.get("VISIVO_SED_MODELS_MAX_BYTES", str(64 * 1024**2)))
_MAX_MODELS_CAP = int(os.environ.get("VISIVO_SED_MAX_MODELS", "2000"))


class SedFitRequest(BaseModel):
    """One SED and the grid to search. ``model`` picks thin, thick, or both."""

    model_config = {"protected_namespaces": ()}

    model: str = Field(default="thin", description="'thin', 'thick' or 'both'")
    wavelengths_um: list[float]
    fluxes_jy: list[float]
    errors_jy: list[float] | None = None
    upper_limits: list[bool] | None = None

    distance_pc: float = 1000.0
    kappa_ref: float = DEFAULT_KAPPA_REF
    lambda_ref_um: float = DEFAULT_LAMBDA_REF_UM
    colour_correction: bool = True

    # thin
    mass_range: list[float] | None = None
    temp_range: list[float] | None = None
    beta_range: list[float] | None = None
    # thick
    size_arcsec: float = 0.0
    lambda0_range: list[float] | None = None
    scale_range: list[float] | None = None

    # Provenance echoed back into the client's Session Data product.
    source_label: str = ""


class SedModelsRequest(BaseModel):
    """A theoretical-model fit against the VLKB ``searchd`` model grid."""

    wavelengths_um: list[float]
    fluxes_jy: list[float]
    errors_jy: list[float] | None = None
    flags: list[int] | None = None
    distance_pc: float = 1000.0
    prefilter: float = 0.0
    weight_mid_ir: float = 1.0
    weight_far_ir: float = 1.0
    weight_submm: float = 1.0
    delta_chi2: float = 1.0
    service_url: str = ""
    max_models: int = Field(default=200, ge=1, le=_MAX_MODELS_CAP)
    source_label: str = ""


@router.get("/v1/sed/defaults", tags=["sed"])
async def sed_defaults(_: None = _auth):
    """Default fit ranges and constants, so client and server agree on them."""
    return {
        "valid": True,
        "error": "",
        "mass_range": DEFAULT_MASS_RANGE,
        "temp_range": DEFAULT_TEMP_RANGE,
        "beta_range": DEFAULT_BETA_RANGE,
        "lambda0_range": DEFAULT_LAMBDA0_RANGE,
        "scale_range": DEFAULT_SCALE_RANGE,
        "kappa_ref": DEFAULT_KAPPA_REF,
        "lambda_ref_um": DEFAULT_LAMBDA_REF_UM,
        "distance_pc": 1000.0,
        "models_service_url": os.environ.get("VISIVO_VLKB_SEDFIT_URL", ""),
    }


@router.post("/v1/sed/fit", tags=["sed"])
async def sed_fit(request: SedFitRequest, _: None = _auth):
    """Fit a modified blackbody to a SED and return parameters + model curve.

    Returns ``{valid, error, fits: [<fit>…], best}``; ``fits`` holds one entry
    per requested model ('both' runs thin and thick), and ``best`` names the one
    with the lower reduced χ². Each fit carries ``parameters``,
    ``uncertainties`` (Δχ²ᵣ ≤ 1), ``chi2``/``chi2_reduced``/``dof``, the sampled
    ``model_wavelength_um``/``model_flux_jy`` curve, ``fitted_flux_jy`` at the
    input bands, ``luminosity_lsun`` and any ``warnings`` (parameter pegged at a
    range boundary, refinement not converged).

    422 on an unusable SED (fewer than two detections, non-positive distance or
    opacity, a grid too large to evaluate).
    """
    from ..compute.sed import SedFitError, worker_sed_fit  # noqa: PLC0415

    kinds = _requested_models(request.model)
    params = {
        "errors_jy": request.errors_jy,
        "upper_limits": request.upper_limits,
        "distance_pc": request.distance_pc,
        "kappa_ref": request.kappa_ref,
        "lambda_ref_um": request.lambda_ref_um,
        "colour_correction": request.colour_correction,
        "mass_range": request.mass_range or DEFAULT_MASS_RANGE,
        "temp_range": request.temp_range or DEFAULT_TEMP_RANGE,
        "beta_range": request.beta_range or DEFAULT_BETA_RANGE,
        "size_arcsec": request.size_arcsec,
        "lambda0_range": request.lambda0_range or DEFAULT_LAMBDA0_RANGE,
        "scale_range": request.scale_range or DEFAULT_SCALE_RANGE,
    }

    fits = []
    for kind in kinds:
        try:
            fit = await asyncio.to_thread(
                worker_sed_fit, kind, request.wavelengths_um, request.fluxes_jy, params)
        except SedFitError as exc:
            # 'both' should not fail wholesale because the thick fit lacks a size.
            if len(kinds) > 1:
                logger.info("[sed] %s fit skipped: %s", kind, exc)
                continue
            raise HTTPException(422, str(exc)) from exc
        except Exception as exc:  # noqa: BLE001
            logger.warning("[sed] %s fit failed: %s", kind, exc)
            raise HTTPException(500, str(exc)) from exc
        fit["source_label"] = request.source_label
        fits.append(fit)

    if not fits:
        raise HTTPException(422, "no SED model could be fitted to these points")

    # An under-determined fit has no reduced χ² (dof ≤ 0). Rank those by plain
    # χ² behind every fit that does have one, rather than letting a missing
    # value win the comparison.
    def _rank(fit: dict) -> tuple[int, float]:
        reduced = fit.get("chi2_reduced")
        if reduced is None:
            return (1, float(fit.get("chi2", float("inf"))))
        return (0, float(reduced))

    best = min(fits, key=_rank)
    return {"valid": True, "error": "", "fits": fits, "best": best["model"]}


@router.post("/v1/sed/models", tags=["sed"])
async def sed_models(request: SedModelsRequest, _: None = _auth):
    """Fit the SED against the VLKB grid of pre-computed theoretical models.

    Proxies the VLKB ``searchd`` service, whose query is a single underscore-
    joined positional string. The client supplies only the photometry and the
    weights; the URL is validated against SSRF (``VISIVO_VLKB_ALLOWED_HOSTS``)
    and defaults to ``VISIVO_VLKB_SEDFIT_URL`` when the client sends none.

    Returns ``{valid, error, columns, models: [{…}], best}`` with the models
    sorted by χ², capped at ``max_models``. Each model carries the physical
    parameters the service returns (``clump_mass``, ``dust_temp``,
    ``bolometric_luminosity``, ``chi2``, …) plus its sampled SED in
    ``model_wavelength_um``/``model_flux_jy``.
    """
    base = (request.service_url or os.environ.get("VISIVO_VLKB_SEDFIT_URL", "")).strip()
    if not base:
        raise HTTPException(
            503, "No VLKB SED-fit service configured: set VISIVO_VLKB_SEDFIT_URL "
                 "or pass service_url.")

    n = len(request.wavelengths_um)
    if n < 2 or len(request.fluxes_jy) != n:
        raise HTTPException(422, "a model fit needs at least 2 bands, and one flux per band")
    errors = request.errors_jy if request.errors_jy and len(request.errors_jy) == n \
        else [0.0] * n
    flags = request.flags if request.flags and len(request.flags) == n else [1] * n

    weights = [request.weight_mid_ir, request.weight_far_ir, request.weight_submm]
    query = "_".join([
        _as_list(request.wavelengths_um),
        _as_list(request.fluxes_jy),
        _as_list(errors),
        _as_list(flags),
        _num(request.distance_pc),
        _num(request.prefilter),
        _as_list(weights),
        "0",
        _as_list(request.wavelengths_um),
        _num(request.delta_chi2),
    ])
    url = f"{base.rstrip('/')}/searchd/?" + _urllib_parse.quote(f"'{query}'")

    try:
        assert_safe_fetch_url(url, allowlist_env="VISIVO_VLKB_ALLOWED_HOSTS", block_private=True)
    except ValueError as exc:
        raise HTTPException(400, str(exc)) from exc

    logger.info("[sed] model fit bands=%d dist=%.1f -> %s", n, request.distance_pc, base)
    try:
        payload = await asyncio.to_thread(_fetch_json, url)
    except _urllib_error.HTTPError as exc:
        raise HTTPException(502, f"VLKB SED-fit service returned HTTP {exc.code}: {exc.reason}") from exc
    except ValueError as exc:
        raise HTTPException(502, str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        logger.warning("[sed] model fit failed: %s", exc)
        raise HTTPException(502, f"VLKB SED-fit service unreachable: {exc}") from exc

    return _normalise_models(payload, max_models=request.max_models,
                             source_label=request.source_label)


# ── helpers ───────────────────────────────────────────────────────────────────

def _requested_models(model: str) -> list[str]:
    kind = (model or "thin").strip().lower()
    if kind == "both":
        return ["thin", "thick"]
    if kind in ("thin", "thick"):
        return [kind]
    raise HTTPException(422, f"unknown SED model '{model}' (expected thin, thick or both)")


def _num(value: float) -> str:
    return f"{float(value):g}"


def _as_list(values) -> str:
    return "[" + ",".join(_num(v) for v in values) + "]"


def _remaining(deadline: float) -> float:
    """Seconds left before ``deadline``; raises once it has passed."""
    left = deadline - _time.monotonic()
    if left <= 0:
        raise ValueError(
            f"VLKB SED-fit service did not finish within {_MODELS_TIMEOUT_S:g} s")
    return left


def _set_socket_timeout(response, timeout: float) -> None:
    """Apply ``timeout`` to the socket behind an ``http.client`` response."""
    try:
        response.fp.raw._sock.settimeout(timeout)   # noqa: SLF001
    except (AttributeError, OSError):
        pass


class _ValidatingRedirectHandler(_urllib_request.HTTPRedirectHandler):
    """Re-run the SSRF check on every redirect target.

    Validating only the URL the client sent is not enough: a public host is free
    to answer 302 with ``http://169.254.169.254/`` or a loopback address, and
    ``urlopen`` follows redirects by default. Each hop is checked against the
    same policy, so a redirect chain cannot walk into the private network.
    """

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D102, ANN001
        assert_safe_fetch_url(newurl, allowlist_env="VISIVO_VLKB_ALLOWED_HOSTS",
                              block_private=True)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _fetch_json(url: str) -> dict:
    """GET ``url`` and parse JSON, under a byte cap and a wall-clock deadline.

    The socket timeout alone bounds each read, not the transfer: a service
    dribbling bytes could hold a worker thread open indefinitely. Reading in
    chunks against a deadline bounds the whole call.
    """
    deadline = _time.monotonic() + _MODELS_TIMEOUT_S
    opener = _urllib_request.build_opener(_ValidatingRedirectHandler)
    req = _urllib_request.Request(url, method="GET")
    chunks: list[bytes] = []
    total = 0
    with opener.open(req, timeout=_remaining(deadline)) as resp:
        while True:
            remaining = _remaining(deadline)
            # Re-arm the socket before every read. Checking the clock only
            # between reads bounds nothing: a service that dribbles a few bytes
            # just inside each socket timeout would hold this worker thread for
            # as long as it liked.
            _set_socket_timeout(resp, remaining)
            # read1() returns after ONE underlying recv; read() would loop
            # inside http.client until the buffer is full, and a service that
            # sends a few bytes just inside each socket timeout could hold this
            # thread far past the deadline without ever coming back here.
            chunk = resp.read1(256 * 1024) if hasattr(resp, "read1") else resp.read(256 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > _MODELS_MAX_BYTES:
                raise ValueError(
                    f"VLKB SED-fit response exceeds {_MODELS_MAX_BYTES} bytes")
            chunks.append(chunk)
    raw = b"".join(chunks)
    try:
        payload = json.loads(raw.decode("utf-8", errors="replace"))
    except json.JSONDecodeError as exc:
        snippet = raw[:300].decode("utf-8", errors="replace")
        raise ValueError(f"VLKB SED-fit service did not return JSON: {snippet}") from exc
    if not isinstance(payload, dict):
        raise ValueError("VLKB SED-fit service returned an unexpected JSON shape")
    return payload


def _normalise_models(payload: dict, *, max_models: int, source_label: str) -> dict:
    """Turn the service's ``{columns, data}`` table into a list of dicts by χ².

    ``wmod``/``fmod`` are the model SED sampling; they are renamed to the same
    ``model_wavelength_um``/``model_flux_jy`` keys the greybody fit uses, so the
    client plots either kind of result through one code path.
    """
    columns = [str(c).strip() for c in payload.get("columns", [])]
    rows = payload.get("data", [])
    if not columns or not isinstance(rows, list):
        raise HTTPException(502, "VLKB SED-fit service returned no model table")

    index = {name: i for i, name in enumerate(columns)}
    models = []
    skipped = 0
    for row in rows:
        if not isinstance(row, list):
            skipped += 1
            continue
        entry = {}
        for name, i in index.items():
            if i < len(row):
                entry[name] = row[i]
        wmod = _float_list(entry.pop("wmod", None))
        fmod = _float_list(entry.pop("fmod", None))
        if wmod is not None and fmod is not None and len(wmod) == len(fmod):
            entry["model_wavelength_um"] = wmod
            entry["model_flux_jy"] = fmod
        # A model with no usable χ² cannot be ranked; drop it rather than
        # sorting on an infinity that then fails to serialise as JSON.
        chi2 = _safe_float(entry.get("chi2"), None)
        if chi2 is None:
            skipped += 1
            continue
        entry["chi2"] = chi2
        # JSON permits Infinity and NaN; our own response must not carry them,
        # or serialising it fails after the work is already done. A row with a
        # usable χ² is worth keeping, so the offending field becomes null.
        models.append({key: _json_safe(value) for key, value in entry.items()})

    models.sort(key=lambda m: m["chi2"])
    truncated = len(models) > max_models
    models = models[:max_models]
    return {
        "valid": True,
        "error": "",
        "columns": [c for c in columns if c not in ("wmod", "fmod")],
        "models": models,
        "best": models[0] if models else None,
        "total_models": len(rows),
        "skipped_models": skipped,
        "truncated": truncated,
        "source_label": source_label,
    }


def _safe_float(value, default):
    """float(value), or ``default`` when it is missing, non-numeric or not finite."""
    try:
        out = float(value)
    except (TypeError, ValueError):
        return default
    return out if math.isfinite(out) else default


def _json_safe(value):
    """Replace non-finite numbers with None, recursively."""
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, list):
        return [_json_safe(item) for item in value]
    if isinstance(value, dict):
        return {key: _json_safe(item) for key, item in value.items()}
    return value


def _float_list(value):
    """A list of finite floats, or None when the service sent something else."""
    if not isinstance(value, list):
        return None
    out = []
    for item in value:
        number = _safe_float(item, None)
        if number is None:
            return None
        out.append(number)
    return out
