"""Tests for the server-side SODA cutout (compute-next-to-data).

Unit-level tests drive ``app.soda_cutout.make_cutout`` directly against
the shared synthetic FITS fixtures; one integration test exercises the
``/v1/datasets/cutout`` route end-to-end through the ASGI client.
"""
from __future__ import annotations

import io

import astropy.units as u
import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS

from app.soda_cutout import (
    Band,
    Circle,
    CutoutError,
    Range,
    make_cutout,
    parse_band,
    parse_pos,
)


# ── Parameter parsing ───────────────────────────────────────────────────────


def test_parse_pos_circle():
    pos = parse_pos("CIRCLE 83.82 -5.39 0.01")
    assert isinstance(pos, Circle)
    assert (pos.ra, pos.dec, pos.radius) == (83.82, -5.39, 0.01)


def test_parse_pos_range_and_polygon_bbox():
    rng = parse_pos("RANGE 10 12 -6 -4")
    assert isinstance(rng, Range)
    poly = parse_pos("POLYGON 10 -6 12 -6 12 -4 10 -4")
    assert isinstance(poly, Range)
    assert (poly.ra1, poly.ra2, poly.dec1, poly.dec2) == (10.0, 12.0, -6.0, -4.0)


def test_parse_pos_rejects_unknown_shape():
    with pytest.raises(CutoutError):
        parse_pos("BOX 1 2 3")


def test_parse_band_orders_and_validates():
    assert parse_band("1.4e-6 1.3e-6") == Band(1.3e-6, 1.4e-6)
    with pytest.raises(CutoutError):
        parse_band("1.3e-6")


# ── Spatial cutout on a 2D image ────────────────────────────────────────────


def test_circle_cutout_shrinks_image(synthetic_fits_image):
    # Centre on the image reference pixel; radius ≈ 5 px worth of CDELT.
    cdelt = 2.777e-4
    res = make_cutout(
        str(synthetic_fits_image),
        pos=f"CIRCLE 83.82208 -5.39111 {5 * cdelt}",
    )
    assert res.applied["pos"] is True
    h, w = res.applied["out_shape"]
    assert 0 < h < 64 and 0 < w < 64

    # The returned bytes are a valid FITS with the smaller shape.
    with fits.open(io.BytesIO(res.fits_bytes)) as hdul:
        assert hdul[0].data.shape == (h, w)


def test_band_on_image_without_spectral_axis_errors(synthetic_fits_image):
    with pytest.raises(CutoutError):
        make_cutout(str(synthetic_fits_image), band="1.3e-6 1.4e-6")


def test_non_overlapping_pos_errors(synthetic_fits_image):
    with pytest.raises(CutoutError):
        make_cutout(str(synthetic_fits_image), pos="CIRCLE 10 80 0.001")


# ── Spectral + spatial cutout on a cube ─────────────────────────────────────


def _channel_wavelengths_m(cube_path: str, channels) -> list[float]:
    spec = WCS(fits.getheader(str(cube_path))).spectral
    world = spec.pixel_to_world(np.asarray(channels, dtype=float))
    return list(u.Quantity(world).to(u.m, equivalencies=u.spectral()).value)


def test_band_cutout_selects_channels(synthetic_fits_cube):
    wl = _channel_wavelengths_m(synthetic_fits_cube, [8, 12])
    res = make_cutout(
        str(synthetic_fits_cube),
        band=f"{min(wl)} {max(wl)}",
    )
    assert res.applied["band"] is True
    depth = res.applied["out_shape"][0]  # FREQ is NAXIS3 → numpy axis 0
    assert 1 <= depth < 32


def test_pos_and_band_combined_on_cube(synthetic_fits_cube):
    wl = _channel_wavelengths_m(synthetic_fits_cube, [10, 16])
    cdelt = 2.777e-4
    res = make_cutout(
        str(synthetic_fits_cube),
        pos=f"CIRCLE 83.82208 -5.39111 {6 * cdelt}",
        band=f"{min(wl)} {max(wl)}",
    )
    assert res.applied["pos"] is True and res.applied["band"] is True
    depth, height, width = res.applied["out_shape"]
    assert depth < 32 and height < 64 and width < 64


def test_no_params_returns_full_file(synthetic_fits_image):
    res = make_cutout(str(synthetic_fits_image))
    assert res.applied["pos"] is False and res.applied["band"] is False
    assert res.applied["out_shape"] == [64, 64]


# ── Endpoint integration ────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_cutout_endpoint_streams_fits(client, auth_headers, synthetic_fits_image):
    cdelt = 2.777e-4
    resp = await client.post(
        "/v1/datasets/cutout",
        headers=auth_headers,
        json={
            "obs_id": "demo-1",
            "access_url": f"file://{synthetic_fits_image}",
            "pos": f"CIRCLE 83.82208 -5.39111 {5 * cdelt}",
        },
    )
    assert resp.status_code == 200, resp.text
    assert resp.headers["content-type"].startswith("application/fits")
    assert "pos" in resp.headers.get("x-soda-applied", "")
    with fits.open(io.BytesIO(resp.content)) as hdul:
        assert hdul[0].data.shape[0] < 64


@pytest.mark.asyncio
async def test_cutout_endpoint_save_to_workspace_returns_path(client, auth_headers, synthetic_fits_image):
    cdelt = 2.777e-4
    resp = await client.post(
        "/v1/datasets/cutout",
        headers=auth_headers,
        json={
            "obs_id": "demo-2",
            "access_url": f"file://{synthetic_fits_image}",
            "pos": f"CIRCLE 83.82208 -5.39111 {5 * cdelt}",
            "save_to_workspace": True,
        },
    )
    assert resp.status_code == 200, resp.text
    assert resp.headers["content-type"].startswith("application/json")
    body = resp.json()
    assert body["valid"] is True
    assert body["path"].endswith("_cutout.fits")
    assert "pos" in body["applied"]
    # The returned path is a real FITS the backend can open.
    with fits.open(body["path"]) as hdul:
        assert hdul[0].data.shape[0] < 64


@pytest.mark.asyncio
async def test_cutout_endpoint_rejects_http_scheme(client, auth_headers):
    resp = await client.post(
        "/v1/datasets/cutout",
        headers=auth_headers,
        json={"obs_id": "x", "access_url": "http://example.org/f.fits", "pos": "CIRCLE 1 2 0.1"},
    )
    assert resp.status_code == 400
