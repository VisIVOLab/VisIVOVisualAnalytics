from __future__ import annotations

import base64
import concurrent.futures as _cf  # noqa: F401
import logging  # noqa: F401
import os as _os  # noqa: F401
import shutil  # noqa: F401
import subprocess  # noqa: F401
import warnings  # noqa: F401
import zlib  # noqa: F401
from pathlib import Path  # noqa: F401
from typing import Any  # noqa: F401

import numpy as np

from ..fits_dataset import ScientificFitsDataset  # noqa: F401
from ._common import (  # noqa: F401
    logger,
    _qt_zlib_compress,
    _b64f32,
    _b64i32,
    _b64f32_compressed,
    _b64i32_compressed,
    _finite_range,
    _integrated_spacing,
)


def _region_mask(region: dict[str, Any], shape: tuple[int, int]) -> np.ndarray:
    """Build a boolean mask (ny, nx) for a canonical *image-frame* region.

    Pixel coordinates are 0-based (numpy convention). Sizes/radii are in pixels.
    ``angle_deg`` rotates ellipse/box counter-clockwise. The geometry is
    ``app.region_stats.Region``'s, the one the statistics use.
    """
    from ..region_stats import Region  # noqa: PLC0415

    ny, nx = shape
    yy, xx = np.mgrid[0:ny, 0:nx]
    return Region(region).mask(xx, yy)


def worker_region_stats(
    path: str,
    region: dict[str, Any],
    channel: int | None = None,
) -> dict[str, Any]:
    """Pixel statistics inside a 2-D region (image-frame, 0-based pixels), on the
    file at full resolution, read in blocks (``app.region_stats``, issue #95).

    Returns counts and mean/rms/sum/min/max/median/mad_sigma over the finite
    pixels in the region, plus ``flux_jy`` when the data is Jy/beam and a beam
    is defined (integrated intensity = sum / beam-area-in-pixels).
    """
    from ..brick_pyramid import source_shape  # noqa: PLC0415
    from ..region_stats import Region, region_statistics  # noqa: PLC0415

    if region.get("frame", "image") != "image":
        raise ValueError(
            "Region must be in image (pixel) frame; convert with to_frame='image' first.")
    dataset = ScientificFitsDataset(path)
    nz = source_shape(path, dataset.hdu_index)[0]
    ch = 0 if channel is None else max(0, min(int(channel), nz - 1))
    stats = region_statistics(path, Region(region), dataset.hdu_index, ch)
    if stats["total_count"] == 0:
        raise ValueError("No pixels inside the region.")
    if stats["valid_count"] == 0:
        # All blank: an answer, with nothing to average.
        return {"shape": region["shape"], "total_count": stats["total_count"], "valid_count": 0,
                "blanked_count": stats["blank_count"], "min": None, "max": None, "mean": None,
                "rms": None, "sum": 0.0, "median": None, "mad_sigma": None,
                "bunit": dataset.bunit, "flux_jy": None, "channel": ch}

    result: dict[str, Any] = {
        "shape": region["shape"],
        "total_count": stats["total_count"],
        "valid_count": stats["valid_count"],
        "blanked_count": stats["blank_count"],
        "min": stats["min"],
        "max": stats["max"],
        "mean": stats["mean"],
        "rms": stats["stddev"],
        "sum": stats["sum"],
        "median": stats["median"],
        "mad_sigma": stats["mad_sigma"],
        "bunit": dataset.bunit,
        "flux_jy": None,
        "channel": ch,
    }

    # Integrated flux for Jy/beam data when the beam is known. Uses the EXACT
    # pixel solid angle (correct for non-square/skewed pixels), via the shared
    # beam_area_pixels helper. The beam is the SELECTED channel's: a CASA
    # multi-beam cube carries one beam per plane in a BEAMS table, and the
    # primary-header beam (when present at all) is only a representative one, so
    # using it would scale the flux by the ratio of the two beam areas.
    from ._common import beam_area_pixels, is_per_beam_bunit  # noqa: PLC0415
    beam = dataset.beam_for_channel(ch)
    bmaj, bmin = beam.major, beam.minor
    if is_per_beam_bunit(dataset.bunit) and bmaj and bmin:
        beam_area_px = beam_area_pixels(bmaj, bmin, dataset.spatial_pixel_scale_arcsec(),
                                        dataset.spatial_pixel_area_arcsec2())
        if beam_area_px and beam_area_px > 0:
            result["flux_jy"] = float(stats["sum"] / beam_area_px)
            result["beam_area_pix"] = float(beam_area_px)
    return result


def worker_cube_stats(path: str) -> dict[str, Any]:
    """Full-resolution whole-cube pixel statistics, computed OUT-OF-CORE.

    The client only holds a decimated *preview* of a remote cube, so its local
    3-D stats are approximate and coarse. This streams the FULL-res cube in
    z-slab chunks from a memmap, accumulating count / sum / sum-of-squares /
    min / max over the finite voxels in a single pass (bounded memory), plus a
    strided subsample for a near-exact median + MAD. Returns min, max, mean,
    median, std, rms, sum and the finite / blank counts.

    ``median_exact`` is True only when every finite voxel was in the median
    sample (small cubes); for large cubes the median is from a ~4 M uniform
    strided sample, which is statistically indistinguishable from exact.
    """
    import math  # noqa: PLC0415

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        cube = ScientificFitsDataset.squeeze_to_3d(np.asarray(raw))
        if cube.ndim == 2:
            cube = cube[np.newaxis, ...]
        if cube.ndim != 3:
            raise ValueError(f"Unsupported cube dimensionality: {cube.ndim}")
        nz, ny, nx = (int(cube.shape[0]), int(cube.shape[1]), int(cube.shape[2]))
        total_px = nz * ny * nx

        # z-chunk sized to ~64 MB of float64 per read → bounded peak memory.
        plane_bytes = max(1, ny * nx * 8)
        chunk_z = max(1, (64 * 1024 * 1024) // plane_bytes)
        # Strided subsample for the median: ~4 M values across the whole cube.
        sample_target = 4_000_000
        stride = max(1, total_px // sample_target)

        count = 0
        vsum = 0.0
        vsumsq = 0.0
        vmin = math.inf
        vmax = -math.inf
        # Chan's parallel (mean, M2) accumulator for a numerically-stable variance — robust to a
        # large pedestal + small spread, unlike sumsq/N - mean^2.
        agg_mean = 0.0
        agg_m2 = 0.0
        sample_parts: list[np.ndarray] = []
        finite_offset = 0  # running index into the concatenated FINITE voxels

        for z0 in range(0, nz, chunk_z):
            z1 = min(z0 + chunk_z, nz)
            flat = np.asarray(cube[z0:z1], dtype=np.float64).reshape(-1)
            finite = flat[np.isfinite(flat)]
            nb = int(finite.size)
            if nb:
                vsum += float(finite.sum())
                vsumsq += float(np.square(finite).sum())
                vmin = min(vmin, float(finite.min()))
                vmax = max(vmax, float(finite.max()))
                # Combine this chunk into the running (mean, M2) — Chan et al.
                mean_b = float(finite.mean())
                m2_b = float(((finite - mean_b) ** 2).sum())
                delta = mean_b - agg_mean
                new_count = count + nb
                agg_mean += delta * nb / new_count
                agg_m2 += m2_b + delta * delta * count * nb / new_count
                count = new_count
                # Strided median sample over the FINITE voxels (not the raw grid), so blank
                # masks can't alias or starve the sample; phase carried over finite count.
                if stride == 1:
                    sub = finite
                else:
                    sub = finite[((-finite_offset) % stride)::stride]
                if sub.size:
                    sample_parts.append(np.asarray(sub, dtype=np.float64))
                finite_offset += nb

        if count == 0:
            raise ValueError("Cube has no finite voxels.")

        mean = agg_mean
        rms = math.sqrt(max(0.0, vsumsq / count))
        std = math.sqrt(max(0.0, agg_m2 / count))
        if sample_parts:
            sample = np.concatenate(sample_parts)
            median = float(np.median(sample))
            mad = float(np.median(np.abs(sample - median)))
        else:
            median = float("nan")
            mad = float("nan")

        return {
            "total_count": total_px,
            "valid_count": int(count),
            "blanked_count": int(total_px - count),
            "min": float(vmin),
            "max": float(vmax),
            "mean": float(mean),
            "median": median,
            "std": float(std),
            "rms": float(rms),
            "mad_sigma": float(1.4826 * mad) if math.isfinite(mad) else float("nan"),
            "sum": float(vsum),
            "bunit": dataset.bunit,
            "median_exact": bool(stride == 1),
        }
    finally:
        hdul.close()


# ── A-03: WCS reprojection ────────────────────────────────────────────────────


# Source keys that describe what the pixel values ARE (unit, quantity, beam) and
# where they came from. Reprojection resamples the pixel grid but keeps the
# surface-brightness unit and the beam (BMAJ/BMIN are angles in degrees, BPA is
# measured from north: none of them depends on the projection), so without them
# a Jy/beam image would come out with no unit and no beam, and flux, beam-area
# and SoFiA-style tools downstream would silently refuse or guess.
_REPROJECT_CARRIED_KEYS: tuple[str, ...] = (
    "BUNIT", "BTYPE", "BMAJ", "BMIN", "BPA",
    "OBJECT", "TELESCOP", "INSTRUME", "OBSERVER", "ORIGIN",
)


def _carry_flux_keys(out_header, src_header, beam) -> None:
    """Copy the source's unit/beam/provenance keys onto a reprojected header.

    Only keys the target header does not already define are written, so the
    target WCS (and anything the caller set explicitly) always wins; source WCS
    keywords are never copied. ``beam`` is the beam of the plane that was
    reprojected (the per-channel one when the cube has a BEAMS table, else the
    primary-header one), so the output carries the beam its pixels were
    actually observed with; the BEAMS table itself is not copied, since the
    output has a single plane.
    """
    beam_keys = ("BMAJ", "BMIN", "BPA")
    values: dict[str, Any] = {}
    for key in _REPROJECT_CARRIED_KEYS:
        if key in src_header and key not in beam_keys:
            values[key] = src_header[key]
    # The beam is one object: if the target names any part of it, the target
    # owns the whole beam (mixing its BMAJ with the source's BMIN would invent a
    # beam nobody observed with).
    target_has_beam = any(k in out_header for k in beam_keys)
    if not target_has_beam and beam is not None and beam.major and beam.minor:
        values["BMAJ"] = float(beam.major)
        values["BMIN"] = float(beam.minor)
        values["BPA"] = float(beam.pa) if beam.pa is not None else 0.0
    for key, value in values.items():
        if key in out_header:
            continue
        try:
            out_header[key] = value
        except Exception:  # noqa: BLE001 - an odd card must not fail the reprojection
            pass


def _celestial_plane_index(header, ndim: int) -> tuple:
    """NumPy index selecting the first sky plane of an ``ndim`` array.

    The two celestial axes (from the header's WCS) are kept whole and every
    other axis is fixed at 0. Without a celestial pair, the last two NumPy axes
    (FITS axes 1 and 2) are taken as the plane, the FITS convention.
    """
    from astropy.wcs import WCS as _WCS  # noqa: PLC0415

    keep: set[int] = set()
    try:
        w = _WCS(header, relax=True, naxis=ndim)
        lng, lat = int(w.wcs.lng), int(w.wcs.lat)
        if lng >= 0 and lat >= 0:
            # FITS axis k (0-based) is NumPy axis ndim - 1 - k.
            keep = {ndim - 1 - lng, ndim - 1 - lat}
    except Exception:  # noqa: BLE001 - an unreadable WCS falls back to FITS order
        keep = set()
    if len(keep) != 2:
        keep = {ndim - 2, ndim - 1}
    return tuple(slice(None) if ax in keep else 0 for ax in range(ndim))


def worker_reproject(
    path: str,
    output_path: str,
    target_header_dict: dict[str, Any],
    target_shape: list[int],
    order: int,
) -> dict[str, Any]:
    """Reproject a 2-D FITS image to a new WCS using the ``reproject`` package.

    ``order`` = 1 → bilinear interpolation (reproject_interp)
    ``order`` = 3 → exact flux-conserving reproject (reproject_exact)

    The reprojected array is written to ``output_path`` as a new FITS file and
    registered back in the session registry by the caller.
    """
    try:
        from reproject import reproject_interp, reproject_exact  # noqa: PLC0415
    except ImportError as exc:
        raise ImportError(
            "reproject is required for WCS reprojection. "
            "Install with: pip install reproject"
        ) from exc

    from astropy.io import fits as _fits  # noqa: PLC0415
    from astropy.wcs import WCS as _WCS   # noqa: PLC0415

    dataset = ScientificFitsDataset(path)
    hdul, raw = dataset.open_memmap()
    try:
        # Always ONE 2-D plane: the first channel (first Stokes plane of a 4-D
        # cube). reproject would otherwise treat leading axes as a stack of
        # images, and the single beam written below would then be wrong for
        # every plane but the first. Index 0 is taken on the non-celestial axes
        # only (never squeezed), so a single-row or single-column image keeps
        # its two spatial axes and a cube whose spectral axis is not the
        # slowest one (FREQ as FITS axis 1) still yields its sky plane. The
        # celestial axes keep their header order, which is the order of the
        # `.celestial` WCS handed to reproject below.
        image = raw[_celestial_plane_index(dataset.header, raw.ndim)]
        if image.ndim != 2:
            raise ValueError(f"Reprojection needs a 2-D plane, got shape {image.shape}.")
        image = np.array(image, dtype=np.float32)
        src_header = dataset.header.copy()
    finally:
        hdul.close()

    target_header = _fits.Header()
    for k, v in target_header_dict.items():
        try:
            target_header[k] = v
        except Exception:
            pass
    target_wcs = _WCS(target_header, relax=True)
    target_shape_tuple = (int(target_shape[0]), int(target_shape[1]))

    if order == 3:
        result_arr, footprint = reproject_exact(
            (image, _WCS(src_header, relax=True).celestial),
            target_wcs.celestial,
            shape_out=target_shape_tuple,
        )
    else:
        result_arr, footprint = reproject_interp(
            (image, _WCS(src_header, relax=True).celestial),
            target_wcs.celestial,
            shape_out=target_shape_tuple,
            order=order,
        )

    result_arr = result_arr.astype(np.float32)
    out_header = target_header.copy()
    out_header["NAXIS"] = 2
    out_header["NAXIS1"] = target_shape_tuple[1]
    out_header["NAXIS2"] = target_shape_tuple[0]
    # Channel 0 is the plane reprojected; a 2-D image's BEAMS table (if any)
    # also indexes its single plane as CHAN 0.
    _carry_flux_keys(out_header, src_header, dataset.beam_for_channel(0))
    out_hdu = _fits.PrimaryHDU(data=result_arr, header=out_header)
    out_hdu.writeto(output_path, overwrite=True)

    range_min, range_max = _finite_range(result_arr)
    return {
        "width": int(target_shape_tuple[1]),
        "height": int(target_shape_tuple[0]),
        "scalar_type": "float32",
        "range_min": float(range_min),
        "range_max": float(range_max),
        "output_path": output_path,
        "order": order,
    }


# ── A-04: External catalogue cross-match ─────────────────────────────────────


def _sources_from_table(table, centre_coord, ra_cands, dec_cands, name_cands, catalogue_id):
    """Extract source dicts from an astroquery result table robustly.

    Column names drift between astroquery/astropy versions (e.g. SIMBAD went
    from ``RA``/``RA_d`` to lowercase ``ra``/``dec`` in degrees around 0.4.9).
    Resolve columns case-insensitively, accept both decimal-degree and
    sexagesimal string values, and skip any row that fails to parse instead of
    aborting the whole query.
    """
    from astropy.coordinates import SkyCoord  # noqa: PLC0415
    import astropy.units as _u                # noqa: PLC0415

    if table is None or len(table) == 0:
        return []

    colmap = {str(c).lower(): c for c in table.colnames}

    def _pick(cands):
        for cand in cands:
            hit = colmap.get(cand.lower())
            if hit is not None:
                return hit
        return None

    ra_col = _pick(ra_cands)
    dec_col = _pick(dec_cands)
    if ra_col is None or dec_col is None:
        raise ValueError(
            f"{catalogue_id} result missing RA/Dec columns (got {list(table.colnames)})."
        )
    name_col = _pick(name_cands)

    out: list[dict[str, Any]] = []
    for row in table:
        try:
            ra_v, dec_v = row[ra_col], row[dec_col]
            try:
                ra_s, dec_s = float(ra_v), float(dec_v)
            except (TypeError, ValueError):
                # Sexagesimal strings (RA in hours, Dec in degrees).
                sc = SkyCoord(str(ra_v), str(dec_v), unit=(_u.hourangle, _u.deg))
                ra_s, dec_s = float(sc.ra.deg), float(sc.dec.deg)
            if not (np.isfinite(ra_s) and np.isfinite(dec_s)):
                continue
            sep = float(
                centre_coord.separation(SkyCoord(ra_s, dec_s, unit="deg")).arcsec
            )
            name = ""
            if name_col is not None:
                name_val = row[name_col]
                if name_val is not None and not (
                    hasattr(name_val, "mask") and bool(getattr(name_val, "mask"))
                ):
                    name = str(name_val)
            out.append({
                "ra_deg": ra_s,
                "dec_deg": dec_s,
                "name": name,
                "catalogue_id": catalogue_id,
                "separation_arcsec": sep,
            })
        except Exception:  # noqa: BLE001 — one bad row must not sink the query
            continue
    return out


def worker_crossmatch(
    path: str,
    radius_arcsec: float,
    catalogue: str,
    max_sources: int = 500,
) -> dict[str, Any]:
    """Query an external astronomical catalogue using astroquery.

    Returns sources within the bounding box of the FITS image footprint,
    sorted by angular separation from the image centre.

    Supported catalogue identifiers:
      "simbad"  → astroquery.simbad.Simbad
      "2MASS"   → Vizier II/246 (2MASS PSC)
      "NVSS"    → Vizier VIII/65 (NVSS)
      "FIRST"   → Vizier VIII/92 (FIRST)
    """
    VIZIER_CATALOG_IDS = {
        "2MASS": "II/246",
        "NVSS":  "VIII/65",
        "FIRST": "VIII/92",
    }

    dataset = ScientificFitsDataset(path)
    if dataset.wcs is None:
        raise ValueError("WCS not available, cannot compute image footprint.")
    celestial = dataset.wcs.celestial
    if celestial is None:
        raise ValueError("No celestial WCS axes in this dataset.")

    # Image-centre sky coords
    ny = int(dataset.header.get("NAXIS2", dataset.header.get("NAXIS1", 1)))
    nx = int(dataset.header.get("NAXIS1", 1))
    centre_pix = celestial.all_pix2world([[nx / 2.0, ny / 2.0]], 0)[0]
    ra_cen, dec_cen = float(centre_pix[0]), float(centre_pix[1])

    # Search radius = half-diagonal of image (degrees) + extra margin
    pixel_scale_deg = dataset.spatial_pixel_scale_arcsec() / 3600.0
    search_radius_deg = max(
        float(radius_arcsec) / 3600.0,
        0.5 * pixel_scale_deg * np.sqrt(nx ** 2 + ny ** 2),
    )

    cat_lower = catalogue.lower()
    sources: list[dict[str, Any]] = []
    truncated = False

    # Upstream row cap: query far more rows than the final display cap so the
    # spread-preserving sub-sample below has the full footprint population to draw
    # from. Both SIMBAD and Vizier return rows sorted by distance from the centre,
    # so a tight upstream limit (e.g. max_sources+1) would itself bias the result
    # to the image centre and defeat the uniform sub-sampling. Bounded so ultra-dense
    # fields don't trigger an unbounded download.
    upstream_row_cap = min(max(20 * max_sources, 20000), 50000)

    if cat_lower == "simbad":
        try:
            from astroquery.simbad import Simbad  # noqa: PLC0415
        except ImportError as exc:
            raise ImportError(
                "astroquery is required for SIMBAD cross-matching. "
                "Install with: pip install astroquery"
            ) from exc
        from astropy.coordinates import SkyCoord  # noqa: PLC0415
        import astropy.units as _u                # noqa: PLC0415

        simbad = Simbad()
        simbad.ROW_LIMIT = upstream_row_cap
        # ra/dec (decimal degrees) are default outputs in recent astroquery;
        # older versions need them requested explicitly. Best-effort either way.
        for _fld in ("ra", "dec", "ra(d)", "dec(d)"):
            try:
                simbad.add_votable_fields(_fld)
            except Exception:  # noqa: BLE001
                pass
        result_table = simbad.query_region(
            SkyCoord(ra_cen, dec_cen, unit="deg"),
            radius=search_radius_deg * _u.deg,
        )
        centre_coord = SkyCoord(ra_cen, dec_cen, unit="deg")
        sources = _sources_from_table(
            result_table,
            centre_coord,
            ra_cands=("ra_d", "ra", "raj2000"),
            dec_cands=("dec_d", "dec", "dej2000"),
            name_cands=("main_id",),
            catalogue_id="SIMBAD",
        )
    else:
        # Vizier
        vizier_cat = VIZIER_CATALOG_IDS.get(catalogue.upper())
        if vizier_cat is None:
            raise ValueError(
                f"Unknown catalogue '{catalogue}'. "
                f"Supported: simbad, 2MASS, NVSS, FIRST."
            )
        try:
            from astroquery.vizier import Vizier  # noqa: PLC0415
        except ImportError as exc:
            raise ImportError(
                "astroquery is required for Vizier cross-matching. "
                "Install with: pip install astroquery"
            ) from exc
        from astropy.coordinates import SkyCoord  # noqa: PLC0415
        import astropy.units as _u                # noqa: PLC0415

        v = Vizier(columns=["*"], row_limit=upstream_row_cap)
        result_list = v.query_region(
            SkyCoord(ra_cen, dec_cen, unit="deg"),
            radius=search_radius_deg * _u.deg,
            catalog=vizier_cat,
        )
        if result_list and len(result_list) > 0:
            table = result_list[0]
            centre_coord = SkyCoord(ra_cen, dec_cen, unit="deg")
            sources = _sources_from_table(
                table,
                centre_coord,
                ra_cands=("raj2000", "_raj2000", "ra"),
                dec_cands=("dej2000", "_dej2000", "de", "dec"),
                name_cands=("_2mass", "2mass", "nvss", "first", "name"),
                catalogue_id=catalogue.upper(),
            )

    # Crop to the *valid* image footprint. The cone query radius is the image
    # half-diagonal (the circle circumscribing the rectangular grid), so it returns
    # sources outside the rectangle AND sources in the blank/NaN border of a
    # non-rectangular footprint (e.g. a rounded radio mosaic). Project each source to
    # pixel coordinates with the image WCS, drop those outside the grid, then drop
    # those landing on a non-finite (unobserved) pixel so the overlay only shows
    # sources on real data. Done before sub-sampling so the retained max_sources all
    # land on the image.
    if sources:
        hdul = None
        try:
            radec = np.array([[s["ra_deg"], s["dec_deg"]] for s in sources], dtype=float)
            pix = np.asarray(celestial.all_world2pix(radec, 0), dtype=float)
            xi = np.round(pix[:, 0]).astype(int)
            yi = np.round(pix[:, 1]).astype(int)
            in_box = (xi >= 0) & (xi <= nx - 1) & (yi >= 0) & (yi <= ny - 1)
            keep = in_box.copy()
            # Refine the bbox crop to the *valid* (finite) pixels. If reading the
            # image data fails, fall back to the bbox crop rather than dropping the
            # crop entirely.
            try:
                if in_box.any():
                    hdul, data = dataset.open_memmap()
                    img2d = ScientificFitsDataset.squeeze_to_3d(data)
                    while img2d.ndim > 2:
                        img2d = img2d[0]
                    vals = np.asarray(img2d[yi[in_box], xi[in_box]], dtype=float)
                    keep[in_box] = np.isfinite(vals)
            except Exception as exc:  # noqa: BLE001 — keep the bbox crop on data-read failure
                logger.warning("[crossmatch] valid-pixel refine skipped, using bbox: %s", exc)
                keep = in_box
            sources = [s for s, k in zip(sources, keep) if k]
        except Exception as exc:  # noqa: BLE001 — a projection failure must not sink the query
            logger.warning("[crossmatch] footprint crop skipped (projection failed): %s", exc)
        finally:
            if hdul is not None:
                hdul.close()

    # Sort by separation (nearest first, useful for the table / "closest match").
    sources.sort(key=lambda s: s["separation_arcsec"])
    if len(sources) > max_sources:
        # Do NOT keep only the nearest-to-centre sources: in a dense field that
        # collapses the overlay into a tiny central disk instead of covering the
        # image footprint. Uniformly sub-sample across the separation-sorted list
        # so the retained sources preserve the spatial (radial) distribution and
        # spread across the whole field. The nearest source is always kept.
        step = len(sources) / max_sources
        sources = [sources[int(i * step)] for i in range(max_sources)]
        truncated = True

    return {
        "n_sources": len(sources),
        "truncated": truncated,
        "centre_ra_deg": ra_cen,
        "centre_dec_deg": dec_cen,
        "search_radius_deg": search_radius_deg,
        "sources": sources,
    }


# ── P-01: Stokes plane extraction ────────────────────────────────────────────
