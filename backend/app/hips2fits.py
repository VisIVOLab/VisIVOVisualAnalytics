"""hips2fits — cut a FITS (or a picture) out of any HiPS survey.

CDS runs a service that reprojects a HiPS onto a WCS of your choosing and hands
back a FITS. The legacy client had a form for it (``HiPS2FITSForm``) that sent
whatever was in the fields, empty ones included, and let the service refuse:
a blank FoV or a width of 100000 became a 400 or a very long wait, reported as
"Error" with whatever description came back.

What is different here:

* the request is **checked before it is sent** — every parameter the service
  requires is required here too, with limits that say what they are;
* the output is **written into the workspace**, so it is a file the rest of the
  application can open, rather than a download in a temp directory;
* the survey list is not hard-coded into a form. The legacy shipped fifty
  entries as a QStringList inside the dialog; :func:`known_surveys` keeps them
  in one place on the backend, where they can be corrected without a release.

The endpoint is a fixed public service. It is still validated as a fetch target
(``assert_safe_fetch_url``), because it is overridable by environment for sites
that mirror it.
"""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

#: The public service. ``VISIVO_HIPS2FITS_URL`` overrides it for a mirror.
DEFAULT_ENDPOINT = "https://alasky.cds.unistra.fr/hips-image-services/hips2fits"

#: Projections the service accepts, as three-letter FITS codes.
PROJECTIONS = ("TAN", "SIN", "AIT", "MOL", "ARC", "CAR", "MER", "STG", "ZEA")

COORDSYS = ("icrs", "galactic")
FORMATS = ("fits", "jpg", "png")
STRETCHES = ("power", "linear", "sqrt", "log", "asinh")

#: A pixel budget rather than a per-side limit: 40000×1 is as absurd as
#: 10000×10000, and the service will spend a long time on either before saying
#: so. 30 Mpx is a 5000×6000 mosaic — larger than anything a viewer wants.
MAX_PIXELS = 30_000_000
MAX_SIDE = 20_000


def known_surveys() -> list[str]:
    """A starting list of HiPS ids, as the legacy's form carried.

    Not exhaustive and not authoritative — the service accepts any id from the
    CDS registry, and the client lets one be typed.
    """
    return [
        "CDS/C/HI4PI/HI",
        "CDS/P/2MASS/color",
        "CDS/P/ACT_Planck/DR5/f220",
        "CDS/P/DES-DR2/ColorIRG",
        "CDS/P/DSS2/color",
        "CDS/P/Fermi/3",
        "CDS/P/Finkbeiner",
        "CDS/P/GALEXGR6_7/FUV",
        "CDS/P/GALEXGR6_7/NUV",
        "CDS/P/HGPS/Flux",
        "CDS/P/IRIS/color",
        "CDS/P/MeerKAT/Galactic-Centre-1284MHz-StokesI",
        "CDS/P/NVSS",
        "CDS/P/PLANCK/R3/HFI/color",
        "CDS/P/PLANCK/R3/HFI100",
        "CDS/P/PLANCK/R3/LFI30",
        "CDS/P/PanSTARRS/DR1/color-i-r-g",
        "CDS/P/PanSTARRS/DR1/g",
        "CDS/P/PanSTARRS/DR1/i",
        "CDS/P/PanSTARRS/DR1/r",
        "CDS/P/PanSTARRS/DR1/z",
        "CDS/P/unWISE/color-W2-W1W2-W1",
        "CSIRO/P/RACS/low/I",
        "CSIRO/P/RACS/mid/I",
        "ESAVO/P/AKARI/N60",
        "ESAVO/P/AKARI/WideL",
        "ESAVO/P/HERSCHEL/SPIRE-250",
        "ESAVO/P/HERSCHEL/SPIRE-350",
        "ESAVO/P/HERSCHEL/SPIRE-500",
        "astron.nl/P/lotss_dr2_high",
        "astron.nl/P/tgssadr",
        "ov-gso/P/CGPS/VGPS",
        "ov-gso/P/GLEAM/072-103",
        "ov-gso/P/GLEAM/170-231",
        "ov-gso/P/GLIMPSE/irac1",
        "ov-gso/P/GLIMPSE/irac4",
    ]


def endpoint() -> str:
    return os.environ.get("VISIVO_HIPS2FITS_URL", "").strip() or DEFAULT_ENDPOINT


def _finite(name: str, value: Any) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be a number.") from exc
    if not math.isfinite(number):
        raise ValueError(f"{name} must be a finite number.")
    return number


def build_query(hips: str, width: int, height: int, ra: float, dec: float, fov: float,
                projection: str = "TAN", coordsys: str = "icrs", rotation_angle: float = 0.0,
                image_format: str = "fits", min_cut: float = 0.5, max_cut: float = 99.5,
                stretch: str = "linear") -> dict[str, str]:
    """The query string for one cutout, validated.

    Raises ``ValueError`` with the reason, so the client can show it before a
    round trip rather than after one.
    """
    survey = str(hips or "").strip()
    if not survey:
        raise ValueError("Choose a HiPS survey.")

    width, height = int(width or 0), int(height or 0)
    if width < 1 or height < 1:
        raise ValueError("Width and height must be at least 1 pixel.")
    if width > MAX_SIDE or height > MAX_SIDE:
        raise ValueError(f"Width and height are limited to {MAX_SIDE} pixels each.")
    if width * height > MAX_PIXELS:
        raise ValueError(
            f"{width}×{height} is {width * height / 1e6:.0f} Mpx; the limit is "
            f"{MAX_PIXELS // 1_000_000} Mpx.")

    ra_value, dec_value = _finite("RA", ra), _finite("Dec", dec)
    if not -90.0 <= dec_value <= 90.0:
        raise ValueError("Dec must be between −90 and +90 degrees.")
    ra_value %= 360.0

    fov_value = _finite("FoV", fov)
    if fov_value <= 0 or fov_value > 180.0:
        raise ValueError("The field of view must be greater than 0 and at most 180 degrees.")

    projection_code = str(projection or "TAN").strip().upper()[:3]
    if projection_code not in PROJECTIONS:
        raise ValueError(f"Unknown projection '{projection}'. "
                         f"Use one of: {', '.join(PROJECTIONS)}.")

    frame = str(coordsys or "icrs").strip().lower()
    if frame not in COORDSYS:
        raise ValueError(f"Unknown coordinate system '{coordsys}'. Use icrs or galactic.")

    fmt = str(image_format or "fits").strip().lower()
    if fmt not in FORMATS:
        raise ValueError(f"Unknown format '{image_format}'. Use fits, jpg or png.")

    params = {
        "hips": survey,
        "width": str(width),
        "height": str(height),
        "ra": f"{ra_value:.8f}",
        "dec": f"{dec_value:.8f}",
        "fov": f"{fov_value:.8f}",
        "projection": projection_code,
        "coordsys": frame,
        "rotation_angle": f"{_finite('Rotation angle', rotation_angle):.4f}",
        "format": fmt,
    }

    if fmt != "fits":
        # Cuts and stretch are how the service turns numbers into pixels; they
        # mean nothing for a FITS, which carries the numbers themselves.
        low = _finite("Minimum cut", min_cut)
        high = _finite("Maximum cut", max_cut)
        if not 0.0 <= low < high <= 100.0:
            raise ValueError("The cuts are percentiles: 0 ≤ min < max ≤ 100.")
        curve = str(stretch or "linear").strip().lower()
        if curve not in STRETCHES:
            raise ValueError(f"Unknown stretch '{stretch}'. "
                             f"Use one of: {', '.join(STRETCHES)}.")
        params["min_cut"] = f"{low}%"
        params["max_cut"] = f"{high}%"
        params["stretch"] = curve

    return params


def query_url(params: dict[str, str]) -> str:
    return f"{endpoint()}?{urlencode(params)}"


def default_basename(hips: str, image_format: str) -> str:
    """A filename that says where the pixels came from."""
    stem = str(hips or "hips").strip().strip("/").replace("/", "_").replace(" ", "_")
    return f"hips2fits_{stem}.{str(image_format or 'fits').lower()}"


def service_error(payload: bytes, status: int | str = "") -> str:
    """The service's own explanation, when it sent one.

    It answers a bad request with a JSON body carrying ``description``; the
    legacy read exactly that field, and it is worth keeping — the messages are
    good ("Survey xyz not found").
    """
    try:
        description = json.loads(payload.decode("utf-8", errors="replace")).get("description")
    except (ValueError, AttributeError):
        description = None
    if description:
        return str(description)
    text = payload.decode("utf-8", errors="replace").strip()
    if text and len(text) < 400 and "<" not in text[:1]:
        return text
    return f"The hips2fits service refused the request ({status})." if status \
        else "The hips2fits service refused the request."


def looks_like_fits(payload: bytes) -> bool:
    return payload[:6] == b"SIMPLE"


def fetch_cutout(params: dict[str, str], output_path: str, timeout: float = 120.0) -> dict[str, Any]:
    """Run the query and write the answer to ``output_path``.

    The service answers 200 with the image or an error body; a FITS that does
    not start with ``SIMPLE`` is not a FITS, and saying so here is better than
    writing an HTML error page under a ``.fits`` name — which is what the legacy
    did, since it saved whatever came back.
    """
    import urllib.error  # noqa: PLC0415

    from .safe_fetch import safe_urlopen  # noqa: PLC0415

    url = query_url(params)
    try:
        # The endpoint is overridable by environment, and a redirect can lead
        # anywhere, so both the URL and every hop are checked.
        response = safe_urlopen(
            url, allowlist_env="VISIVO_HIPS2FITS_ALLOWED_HOSTS", timeout=timeout,
            headers={"User-Agent": "VisIVO-Backend/1.0 (hips2fits)"})
        with response:
            payload = response.read()
    except urllib.error.HTTPError as exc:
        raise ValueError(service_error(exc.read() or b"", exc.code)) from exc
    except urllib.error.URLError as exc:
        raise ValueError(f"The hips2fits service could not be reached: {exc.reason}") from exc

    if not payload:
        raise ValueError("The hips2fits service returned an empty response.")
    if params.get("format") == "fits" and not looks_like_fits(payload):
        raise ValueError(service_error(payload))

    destination = Path(output_path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(payload)
    return {
        "output_path": str(destination),
        "bytes": len(payload),
        "is_fits": bool(params.get("format") == "fits"),
        "url": url,
    }
