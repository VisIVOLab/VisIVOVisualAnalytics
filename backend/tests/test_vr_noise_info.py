"""The headset's noise σ and the cube's spectral information (issue #258):
reduced_info's ``noise_sigma`` is the desktop's Estimate Noise σ (the median
over channels of 1.4826 x MAD about each channel's median, over the whole
field), and ``rest_frequency_hz`` / ``beam`` come from the header."""
from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits
from httpx import AsyncClient

from app.noise_threshold import channel_noise_sigma, flat_noise_sigma, plane_mad

HI_HZ = 1420405751.768


def _hi_cube(path: Path, sigma: float = 0.002, *, beam: bool = True, rest: bool = True,
             shape=(40, 48, 56), seed: int = 7) -> np.ndarray:
    """An HI-like cube: Gaussian noise of ``sigma`` with a bright compact
    source covering a few per cent of the voxels; RA/Dec SIN, FREQ."""
    rng = np.random.default_rng(seed)
    data = rng.normal(0.0, sigma, size=shape).astype(np.float32)
    nz, ny, nx = shape
    zz, yy, xx = np.mgrid[0:nz, 0:ny, 0:nx]
    src = 40 * sigma * np.exp(-(((xx - nx / 2) / 3.0) ** 2 + ((yy - ny / 2) / 3.0) ** 2
                                 + ((zz - nz / 2) / 4.0) ** 2) / 2)
    data += src.astype(np.float32)
    h = fits.Header()
    h["CTYPE1"], h["CRVAL1"], h["CRPIX1"], h["CDELT1"], h["CUNIT1"] = "RA---SIN", 150.0, nx / 2, -4.0 / 3600, "deg"
    h["CTYPE2"], h["CRVAL2"], h["CRPIX2"], h["CDELT2"], h["CUNIT2"] = "DEC--SIN", 10.0, ny / 2, 4.0 / 3600, "deg"
    h["CTYPE3"], h["CRVAL3"], h["CRPIX3"], h["CDELT3"], h["CUNIT3"] = "FREQ", 1.41e9, 1, -24414.0625, "Hz"
    h["BUNIT"] = "Jy/beam"
    h["SPECSYS"] = "BARYCENT"
    if rest:
        h["RESTFRQ"] = HI_HZ
    if beam:
        h["BMAJ"], h["BMIN"], h["BPA"] = 15.0 / 3600, 12.0 / 3600, 33.0
    fits.PrimaryHDU(data, header=h).writeto(path)
    return data


async def _open(client: AsyncClient, auth_headers: dict, path: Path) -> dict:
    r = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"], body["error"]
    return {"dataset_id": body["dataset_id"],
            "headers": {**auth_headers, "X-Visivo-Session": body["session_id"]}}


# ── The shared rule ─────────────────────────────────────────────────────────

def test_plane_mad_keeps_zero_noise_and_drops_zero_blanking():
    rng = np.random.default_rng(1)
    noise = rng.normal(0, 1, 10_000).astype(np.float32)
    assert math.isclose(1.4826 * plane_mad(noise), 1.0, rel_tol=0.05)
    # Zeros as a minority stay (noise centred on zero).
    some = noise.copy()
    some[:1000] = 0.0
    assert plane_mad(some) < plane_mad(noise)
    # Zeros as blanking (more than half the finite values) are left out.
    blank = np.concatenate([np.zeros(30_000, np.float32), noise])
    assert plane_mad(blank) == pytest.approx(plane_mad(noise), rel=1e-12)
    assert math.isnan(plane_mad(np.array([np.nan, 1.0])))


def test_channel_noise_sigma_is_the_median_of_the_channels():
    rng = np.random.default_rng(3)
    s = np.stack([rng.normal(0, k, (64, 64)) for k in (1.0, 2.0, 3.0, 4.0, 5.0)]).astype(np.float32)
    per = [1.4826 * plane_mad(s[k]) for k in range(5)]
    assert channel_noise_sigma(s) == pytest.approx(float(np.median(per)))
    s[0] = np.nan                               # a blank channel has no measure
    assert channel_noise_sigma(s) == pytest.approx(float(np.median(per[1:])))
    assert channel_noise_sigma(np.full((3, 4, 4), np.nan)) is None
    assert channel_noise_sigma(np.zeros((3, 4, 4))) is None
    assert flat_noise_sigma(rng.normal(0, 2.0, 100_000)) == pytest.approx(2.0, rel=0.03)


# ── reduced_info ────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_reduced_info_sigma_is_the_desktops_estimate_noise(client, auth_headers, tmp_path) -> None:
    """On a cube small enough to be read whole, reduced_info's σ is exactly the
    median σ /v1/cube/noise gives for the desktop dialog's default region (the
    whole field, every channel), and within a few per cent of the true noise."""
    path = tmp_path / "hi_cube.fits"
    data = _hi_cube(path, sigma=0.002)
    ds = await _open(client, auth_headers, path)
    info = (await client.post("/v1/cube/reduced_info", json={"dataset_id": ds["dataset_id"]},
                              headers=ds["headers"])).json()
    nz, ny, nx = data.shape
    noise = (await client.post("/v1/cube/noise", json={
        "dataset_id": ds["dataset_id"], "x0": 0, "x1": nx - 1, "y0": 0, "y1": ny - 1,
        "channel_start": 0, "channel_end": nz - 1}, headers=ds["headers"])).json()
    assert noise["valid"], noise["error"]
    desktop = float(np.median([s for s in noise["sigma"] if s is not None and math.isfinite(s)]))
    assert info["noise_sigma"] == pytest.approx(desktop, rel=1e-6)
    assert info["noise_sigma"] == pytest.approx(0.002, rel=0.05)
    assert "Estimate Noise" in info["noise_sigma_method"]
    # The default threshold is a few σ above the noise (#93's rule).
    assert 3.0 < info["default_threshold"] / info["noise_sigma"] < 7.0
    # The info line's keywords.
    assert info["rest_frequency_hz"] == pytest.approx(HI_HZ)
    assert info["rest_line"] == "HI"
    b = info["beam"]
    assert b["major_arcsec"] == pytest.approx(15.0) and b["minor_arcsec"] == pytest.approx(12.0)
    assert b["pa_deg"] == pytest.approx(33.0) and b["per_channel"] is False
    # Channel width in km/s (optical, from FREQ and RESTFRQ) is already in the axes.
    assert info["axes"]["z"]["channel_width_kms"] == pytest.approx(
        299792.458 * HI_HZ * 24414.0625 / 1.41e9 ** 2, rel=0.01)


@pytest.mark.asyncio
async def test_reduced_info_without_beam_or_rest_frequency(client, auth_headers, tmp_path) -> None:
    path = tmp_path / "plain_cube.fits"
    _hi_cube(path, beam=False, rest=False, shape=(16, 20, 24))
    ds = await _open(client, auth_headers, path)
    info = (await client.post("/v1/cube/reduced_info", json={"dataset_id": ds["dataset_id"]},
                              headers=ds["headers"])).json()
    assert info["beam"] is None
    assert info["rest_frequency_hz"] is None and info["rest_line"] == ""
    assert info["noise_sigma"] is not None


@pytest.mark.asyncio
async def test_reduced_info_sigma_is_null_for_constant_data(client, auth_headers, tmp_path) -> None:
    path = tmp_path / "flat.fits"
    fits.PrimaryHDU(np.ones((8, 10, 12), np.float32)).writeto(path)
    ds = await _open(client, auth_headers, path)
    info = (await client.post("/v1/cube/reduced_info", json={"dataset_id": ds["dataset_id"]},
                              headers=ds["headers"])).json()
    assert info["noise_sigma"] is None and info["noise_sigma_method"] == ""
