"""Image segmentation as a finder plugin, for 2-D images (photutils).

Pixels above ``median + nsigma * sigma`` (sigma-clipped statistics of the
image) that form connected groups of at least ``npixels`` are sources. The
label image is the mask; the catalogue lists each source's centroid (pixel
and, when the image has a celestial WCS, sky), area, flux and peak.

The segmentation runs in a child Python process (``python -m`` this module),
so that a cancelled or timed-out task stops it like any other finder.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any

import numpy as np

from ._process import ChildTimeout, run_child

PARAMETERS = {
    "nsigma": "detection threshold above the clipped median, in sigma (default 3.0)",
    "npixels": "minimum connected pixels of a source (default 5)",
    "connectivity": "4 or 8 (default 8)",
}

# Exit code of the child when the request, not the service, is at fault.
_USAGE_EXIT = 3
# Memory per byte of image: the data and the background, convolved and
# segmented copies photutils keeps, as float64 for a float32 input; plus
# the interpreter with astropy and photutils loaded.
MEMORY_FACTOR = 8.0
MEMORY_FLOOR_BYTES = 512 << 20
_BACKEND_ROOT = Path(__file__).resolve().parents[3]


def info():
    from . import FinderInfo

    return FinderInfo(
        name="photutils",
        kinds=("image",),
        description="Threshold segmentation of a 2-D image (photutils).",
        parameters=PARAMETERS,
        available=True,
        memory_factor=MEMORY_FACTOR,
        memory_floor_bytes=MEMORY_FLOOR_BYTES,
    )


def _number(params: dict[str, Any], key: str, default: float, lo: float, hi: float) -> float:
    from . import FinderError

    raw = params.get(key, default)
    try:
        value = float(raw)
    except (TypeError, ValueError) as exc:
        raise FinderError(f"{key} must be a number, got {raw!r}") from exc
    if not lo <= value <= hi:
        raise FinderError(f"{key} must be between {lo:g} and {hi:g}")
    return value


def segment(path: str, out_dir: Path, params: dict[str, Any]) -> dict[str, Any]:
    """The segmentation itself; runs in the child process."""
    from astropy.io import fits
    from astropy.stats import sigma_clipped_stats
    from astropy.table import Table
    from astropy.wcs import WCS

    from . import FinderError

    nsigma = _number(params, "nsigma", 3.0, 0.5, 100.0)
    npixels = int(_number(params, "npixels", 5, 1, 1_000_000))
    connectivity = int(_number(params, "connectivity", 8, 4, 8))
    if connectivity not in (4, 8):
        raise FinderError("connectivity must be 4 or 8")
    unknown = set(params) - set(PARAMETERS)
    if unknown:
        raise FinderError(f"unknown parameters: {', '.join(sorted(unknown))}")

    with fits.open(path, memmap=True) as hdul:
        hdu = next((h for h in hdul if h.data is not None), None)
        if hdu is None:
            raise FinderError("the file has no image data")
        raw = np.asarray(hdu.data, dtype=np.float32)
        header = hdu.header.copy()
    data = np.squeeze(raw)
    if data.ndim != 2:
        raise FinderError(f"photutils segments 2-D images; this dataset has {data.ndim} axes")

    # The mask's WCS is the image's without the degenerate axes the squeeze
    # removed (numpy axis i is FITS/WCS axis ndim-1-i).
    try:
        wcs = WCS(header)
        for axis in sorted((raw.ndim - 1 - i for i, n in enumerate(raw.shape) if n == 1), reverse=True):
            wcs = wcs.dropaxis(axis)
        # relax: distortion terms (SIP) go with the mask, as they do with the catalogue.
        mask_header = wcs.to_header(relax=True) if wcs.naxis == 2 else fits.Header()
        celestial = wcs.celestial if wcs.naxis == 2 and wcs.has_celestial else None
    except Exception:  # noqa: BLE001 — an image without a usable WCS is still segmentable
        wcs, mask_header, celestial = None, fits.Header(), None

    finite = np.isfinite(data)
    if not finite.any():
        raise FinderError("the image has no finite pixels")
    _, median, std = sigma_clipped_stats(data, mask=~finite, sigma=3.0)
    if not np.isfinite(std) or std <= 0:
        raise FinderError("the image has no measurable noise")
    filled = np.where(finite, data, median)
    threshold = median + nsigma * std

    from photutils.segmentation import SourceCatalog, detect_sources

    segm = detect_sources(filled, threshold, npixels=npixels, connectivity=connectivity)

    labels = segm.data.astype(np.int32) if segm is not None else np.zeros(data.shape, np.int32)
    mask_header["BUNIT"] = "label"
    mask_header["HISTORY"] = f"photutils segmentation: nsigma={nsigma:g} npixels={npixels} connectivity={connectivity}"
    mask_path = out_dir / "photutils_mask.fits"
    fits.PrimaryHDU(labels, header=mask_header).writeto(mask_path, overwrite=True)

    if segm is None:
        table = Table(names=("label", "x", "y", "area", "flux", "peak"),
                      dtype=(int, float, float, float, float, float))
    else:
        cat = SourceCatalog(filled, segm, wcs=celestial)
        table = Table()
        table["label"] = np.asarray(cat.labels, dtype=int)
        table["x"] = np.asarray(cat.xcentroid, dtype=float)
        table["y"] = np.asarray(cat.ycentroid, dtype=float)
        if celestial is not None and cat.sky_centroid is not None:
            sky = cat.sky_centroid.icrs
            table["ra"] = np.asarray(sky.ra.deg, dtype=float)
            table["dec"] = np.asarray(sky.dec.deg, dtype=float)
        table["area"] = np.asarray(cat.area.value, dtype=float)
        table["flux"] = np.asarray(cat.segment_flux, dtype=float)
        table["peak"] = np.asarray(cat.max_value, dtype=float)
    catalog_path = out_dir / "photutils_cat.xml"
    table.write(catalog_path, format="votable", overwrite=True)

    rows = [{name: (int(row[name]) if name == "label" else float(row[name])) for name in table.colnames}
            for row in table]
    return {
        "n_sources": len(rows),
        "sources": rows,
        "mask_path": str(mask_path),
        "catalog_path": str(catalog_path),
        "threshold": float(threshold),
        "noise": float(std),
    }


async def run(path: str, out_dir: Path, params: dict[str, Any], timeout: float) -> dict[str, Any]:
    from . import FinderError

    env = dict(os.environ)
    env["PYTHONPATH"] = str(_BACKEND_ROOT) + (os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
    args = [sys.executable, "-m", "app.compute.finders.photutils_finder", path, str(out_dir), json.dumps(params)]
    try:
        code, out, err = await run_child(args, _BACKEND_ROOT, timeout, out_dir / "photutils.log", env=env)
    except ChildTimeout as exc:
        raise FinderError(f"photutils {exc}") from exc
    if code == _USAGE_EXIT:
        raise FinderError(err.strip().splitlines()[-1] if err.strip() else "photutils refused the request")
    if code != 0:
        raise FinderError(f"photutils failed (exit {code}): {err[-500:] or out[-500:]}")
    try:
        return json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError) as exc:
        raise FinderError("photutils produced no result") from exc


def main(argv: list[str]) -> int:
    from . import FinderError

    path, out_dir, params = argv[0], Path(argv[1]), json.loads(argv[2])
    try:
        result = segment(path, out_dir, params)
    except FinderError as exc:
        print(str(exc), file=sys.stderr)
        return _USAGE_EXIT
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
