"""A cube whose primary HDU is empty and whose data sit in an image extension.

ESO phase-3 and APEX products are laid out this way: PRIMARY with NAXIS = 0 and
the observation's descriptive cards, DATA_EXT with the cube and its WCS,
STAT_EXT with the variance. Reading ``hdul[0]`` on such a file gave an image of
shape ``()``: the classifier called it "unknown" and the opener found no data.
"""
from __future__ import annotations

import base64

import numpy as np
import pytest
from astropy.io import fits
from httpx import AsyncClient

from app.classify import classify_path
from app.fits_dataset import ScientificFitsDataset, image_hdu_index

NZ, NY, NX = 12, 10, 8


def _cube_header(h: fits.Header) -> None:
    h["CTYPE1"], h["CTYPE2"], h["CTYPE3"] = "GLON-GLS", "GLAT-GLS", "VRAD"
    h["CRVAL1"], h["CRVAL2"], h["CRVAL3"] = 10.0, 0.0, -5000.0
    h["CRPIX1"], h["CRPIX2"], h["CRPIX3"] = 4.0, 5.0, 1.0
    h["CDELT1"], h["CDELT2"], h["CDELT3"] = -0.0025, 0.0025, 250.0
    h["CUNIT1"], h["CUNIT2"], h["CUNIT3"] = "deg", "deg", "m/s"
    h["BUNIT"] = "K"
    h["BMAJ"], h["BMIN"], h["BPA"] = 0.005, 0.005, 0.0


@pytest.fixture(scope="module")
def extension_cube(tmp_path_factory):
    rng = np.random.default_rng(3)
    data = (rng.standard_normal((NZ, NY, NX)) * 0.1).astype(np.float32)
    data[4:8, 3:7, 2:6] += 4.0
    primary = fits.PrimaryHDU()
    primary.header["OBJECT"] = "G10"
    primary.header["INSTRUME"] = "APEXHET"
    cube = fits.ImageHDU(data, name="DATA_EXT")
    _cube_header(cube.header)
    stat = fits.ImageHDU(np.full((NZ, NY, NX), 0.01, dtype=np.float32), name="STAT_EXT")
    _cube_header(stat.header)
    p = tmp_path_factory.mktemp("multi") / "apex_like.fits"
    fits.HDUList([primary, cube, stat]).writeto(p)
    return p


def test_image_hdu_index_finds_the_extension(extension_cube, synthetic_fits_cube, tmp_path):
    with fits.open(extension_cube) as hdul:
        assert image_hdu_index(hdul) == 1          # DATA_EXT, not STAT_EXT
    with fits.open(synthetic_fits_cube) as hdul:
        assert image_hdu_index(hdul) == 0          # an ordinary cube is untouched
    table_only = tmp_path / "table.fits"
    fits.HDUList([fits.PrimaryHDU(),
                  fits.BinTableHDU.from_columns([fits.Column("x", "E", array=np.zeros(3))])]
                 ).writeto(table_only)
    with fits.open(table_only) as hdul:
        assert image_hdu_index(hdul) == 0          # nothing qualifies: callers fail as before


def test_classifier_sees_the_cube(extension_cube):
    r = classify_path(extension_cube)
    assert r["kind"] == "cube", r
    assert r["opener"] == "dataset"
    assert f"{NX} × {NY} × {NZ}" in r["detail"]


def test_dataset_reads_the_extension(extension_cube):
    ds = ScientificFitsDataset(extension_cube)
    assert ds.hdu_index == 1
    assert ds.shape == (NZ, NY, NX)
    assert ds.naxis == 3 and ds.active_axes == [1, 2, 3]
    assert ds.header["CTYPE3"] == "VRAD" and ds.bunit == "K"
    # The primary's descriptive cards travel with the image header…
    assert ds.header["OBJECT"] == "G10" and ds.header["INSTRUME"] == "APEXHET"
    hdul, data = ds.open_memmap()
    try:
        assert data.shape == (NZ, NY, NX)
        assert float(np.nanmax(data)) > 3.0
    finally:
        hdul.close()


def test_single_hdu_dataset_unchanged(synthetic_fits_cube):
    ds = ScientificFitsDataset(synthetic_fits_cube)
    assert ds.hdu_index == 0 and ds.shape == (32, 64, 64)


@pytest.mark.asyncio
async def test_open_header_and_moment_through_the_api(
    client: AsyncClient, auth_headers: dict, extension_cube
) -> None:
    resp = await client.post("/v1/datasets/open", json={"path": str(extension_cube)},
                             headers=auth_headers)
    assert resp.status_code == 200
    opened = resp.json()
    assert opened["valid"], opened["error"]
    assert opened["kind"] == "cube"
    headers = dict(auth_headers)
    headers["X-Visivo-Session"] = opened["session_id"]

    # The header viewer shows the observation's cards AND the image's own.
    resp = await client.post("/v1/files/header", json={"path": str(extension_cube)},
                             headers=auth_headers)
    cards = " ".join(resp.json()["cards"])
    assert "INSTRUME" in cards and "EXTNAME = 'DATA_EXT'" in cards and "CTYPE3" in cards

    # A worker product: moment 0 over the line channels, on the extension's data.
    resp = await client.post(
        "/v1/products/moment",
        json={"dataset_id": opened["dataset_id"], "moment_order": 0,
              "channel_start": 3, "channel_end": 9, "mask_enabled": False,
              "threshold_value": 0.0},
        headers=headers)
    assert resp.status_code == 200
    m0 = resp.json()
    assert m0["valid"], m0["error"]
    assert (m0["width"], m0["height"]) == (NX, NY)
    arr = np.frombuffer(base64.b64decode(m0["data_base64"]), dtype=np.float32)
    assert arr.shape == (NX * NY,) and np.nanmax(arr) > 0.0
    assert m0["bunit"] == "K" and m0["spectral_axis_type"] == "VRAD"


# ── Review follow-ups: every reader picks the same HDU, and only once ─────────

def test_rest_frequency_spellings_are_one_field(tmp_path):
    """A primary RESTFRQ must not land beside the extension's own RESTFREQ."""
    primary = fits.PrimaryHDU()
    primary.header["RESTFRQ"] = 1.0e9
    ext = fits.ImageHDU(np.zeros((3, 4, 5), dtype=np.float32))
    ext.header["RESTFREQ"] = 2.0e9
    p = tmp_path / "aliases.fits"
    fits.HDUList([primary, ext]).writeto(p)
    ds = ScientificFitsDataset(p)
    assert "RESTFRQ" not in ds.header and ds.header["RESTFREQ"] == 2.0e9
    assert ds._rest_freq_hz() == 2.0e9


def test_one_d_primary_then_cube_is_classified_as_the_cube(tmp_path):
    p = tmp_path / "spectrum_then_cube.fits"
    fits.HDUList([fits.PrimaryHDU(np.zeros(8, dtype=np.float32)),
                  fits.ImageHDU(np.zeros((3, 4, 5), dtype=np.float32))]).writeto(p)
    assert classify_path(p)["kind"] == "cube"
    assert ScientificFitsDataset(p).hdu_index == 1


def test_velocity_field_in_an_extension_loads(tmp_path):
    from app.velocity_field import load_multicomponent_fits, velocity_subgrid_worker
    p = tmp_path / "flow.fits"
    comps = np.zeros((3, 4, 5, 6), dtype=np.float32)
    comps[0] = 1.0
    fits.HDUList([fits.PrimaryHDU(), fits.ImageHDU(comps)]).writeto(p)
    assert classify_path(p)["kind"] == "velocity-field"
    field = load_multicomponent_fits(str(p))
    assert field.vx.shape == (4, 5, 6)
    sub = velocity_subgrid_worker(str(p), 1, 3, 0, 2, 0, 4)
    assert sub["nx"] == 2 and sub["ny"] == 2 and sub["nz"] == 4


def test_dynspec_in_an_extension_opens(tmp_path):
    from app.dynspec_tools import _open_dynspec
    ext = fits.ImageHDU(np.ones((16, 32), dtype=np.float32))
    ext.header["CTYPE1"], ext.header["CTYPE2"] = "TIME", "FREQ"
    ext.header["CDELT1"], ext.header["CDELT2"] = 0.001, 1.0e6
    p = tmp_path / "dynspec.fits"
    fits.HDUList([fits.PrimaryHDU(), ext]).writeto(p)
    assert classify_path(p)["kind"] == "dynspec"
    data, header = _open_dynspec(str(p))
    assert data.shape == (16, 32) and header["CTYPE2"] == "FREQ"


@pytest.mark.asyncio
async def test_header_listing_has_every_keyword_once(client: AsyncClient, auth_headers: dict,
                                                     extension_cube) -> None:
    resp = await client.post("/v1/files/header", json={"path": str(extension_cube)},
                             headers=auth_headers)
    cards = resp.json()["cards"]
    keys = [c.split("=", 1)[0].strip() for c in cards if "=" in c]
    assert len(keys) == len(set(keys)), "the header editor refuses a keyword that appears twice"
    assert "INSTRUME" in keys and "CTYPE3" in keys and "EXTNAME" in keys


def test_header_patch_edits_the_image_hdu(extension_cube, tmp_path):
    from app.fits_header_edit import worker_patch_header
    out = tmp_path / "patched.fits"
    result = worker_patch_header(str(extension_cube), str(out),
                                 cards=[{"key": "BUNIT", "value": "Jy/beam"}])
    assert result["hdu_index"] == 1
    with fits.open(out) as hdul:
        assert hdul[1].header["BUNIT"] == "Jy/beam"      # the image HDU's own card
        assert "BUNIT" not in hdul[0].header            # the primary is left alone
        assert hdul[0].header["OBJECT"] == "G10"
        assert hdul[2].header["EXTNAME"] == "STAT_EXT"  # extensions travel with the copy


def test_header_patch_edits_inherited_cards_where_they_live(extension_cube, tmp_path):
    """The listing merges the primary's descriptive cards into the image header;
    an edit of one of those has to reach the primary, or it would be a no-op."""
    from app.fits_header_edit import worker_patch_header
    out = tmp_path / "patched2.fits"
    result = worker_patch_header(str(extension_cube), str(out),
                                 cards=[{"key": "INSTRUME", "value": "FLASH"},
                                        {"key": "NEWKEY", "value": 1}],
                                 remove=["OBJECT"])
    assert result["hdu_index"] == 1
    with fits.open(out) as hdul:
        assert hdul[0].header["INSTRUME"] == "FLASH"      # primary-owned: changed there
        assert "OBJECT" not in hdul[0].header             # primary-owned: removed there
        assert hdul[1].header["NEWKEY"] == 1              # new: the image HDU
        assert "INSTRUME" not in hdul[1].header
    assert "OBJECT" not in ScientificFitsDataset(out).header


def test_velocity_density_streams_from_the_extension(tmp_path):
    from app.velocity_field import _density_streaming
    p = tmp_path / "flow2.fits"
    comps = np.zeros((3, 4, 5, 6), dtype=np.float32)
    comps[0] = np.arange(6, dtype=np.float32)[None, None, :]   # vx grows along x
    fits.HDUList([fits.PrimaryHDU(), fits.ImageHDU(comps)]).writeto(p)
    out = _density_streaming(str(p), None)
    assert out is not None, "an extension-hosted field must take the streaming path"
