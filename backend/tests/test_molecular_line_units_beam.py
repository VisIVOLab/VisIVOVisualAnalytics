"""Molecular-line cubes without BUNIT or BMAJ (issue #416).

CHIMPS cubes from JCMT/ACSIS carry ``TEMPSCAL = 'TA*'`` and a Starlink LABEL
but no BUNIT and no beam. Their moment-0 map was labelled and saved as km/s,
and nothing per beam could be computed. The intensity unit now falls back to
TEMPSCAL/BTYPE/LABEL, an unknown one reads "(unknown) × km/s" and is never
written as BUNIT, and a beam can be typed for the file.
"""
from __future__ import annotations

import os

import numpy as np
import pytest
from astropy.io import fits

from app import beam_override_store
from app.compute.moment import worker_moment, write_moment_fits
from app.fits_dataset import ScientificFitsDataset, intensity_unit_from_header

_STARLINK_LABEL = "T%s60+%v30+A%^50+%<20+*%+   corrected antenna temperature"


def _cube(path, **cards):
    rng = np.random.default_rng(1)
    data = rng.normal(0.0, 0.1, (16, 8, 8)).astype(np.float32)
    data[6:9, 3:5, 3:5] += 1.0
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"], h["CTYPE3"] = "GLON-TAN", "GLAT-TAN", "VRAD"
    h["CRVAL1"], h["CRVAL2"], h["CRVAL3"] = 40.0, 0.3, 80.0
    h["CRPIX1"], h["CRPIX2"], h["CRPIX3"] = 4.0, 4.0, 1.0
    h["CDELT1"], h["CDELT2"], h["CDELT3"] = -0.002, 0.002, 0.5
    h["CUNIT1"], h["CUNIT2"], h["CUNIT3"] = "deg", "deg", "km/s"
    for key, value in cards.items():
        h[key] = value
    fits.PrimaryHDU(data, header=h).writeto(path)
    return str(path)


@pytest.fixture(autouse=True)
def _no_typed_beams():
    beam_override_store.clear()
    yield
    beam_override_store.clear()


@pytest.mark.parametrize("cards, unit", [
    ({"BUNIT": "Jy/beam", "TEMPSCAL": "TA*"}, "Jy/beam"),
    ({"TEMPSCAL": "TA*"}, "K"),
    ({"BTYPE": "Brightness Temperature"}, "K"),
    ({"BTYPE": "Tb"}, "K"),
    ({"BTYPE": "Tmb"}, "K"),
    ({"TEMPSCAL": "TMB"}, "K"),
    ({"TEMPSCAL": "NONE"}, ""),
    ({"BTYPE": "Surface brightness"}, ""),
    ({"LABEL": _STARLINK_LABEL}, "K"),
    ({"BTYPE": "Intensity"}, ""),
    ({}, ""),
])
def test_intensity_unit_fallback(cards, unit):
    h = fits.Header()
    for key, value in cards.items():
        h[key] = value
    got, note = intensity_unit_from_header(h)
    assert got == unit
    assert (note == "") == ("BUNIT" in cards)


def test_tempscal_cube_moment0_is_k_km_s(tmp_path):
    path = _cube(tmp_path / "c18o.fits", TEMPSCAL="TA*", LABEL=_STARLINK_LABEL)
    result = worker_moment(path, 0, 0, 15, False, 0.0)
    assert result["moment_unit"] == "K km/s"
    assert result["bunit"] == "K"
    assert "TA*" in result["intensity_unit_note"]
    out = tmp_path / "m0.fits"
    write_moment_fits(path, str(out), result, 0, 0, 15, False, 0.0)
    header = fits.getheader(out)
    assert header["BUNIT"] == "K km/s"
    assert header["TEMPSCAL"] == "TA*"
    assert "TA*" in " ".join(str(c) for c in header["COMMENT"])


def test_unknown_unit_moment0_is_never_saved_as_km_s(tmp_path):
    path = _cube(tmp_path / "bare.fits")
    result = worker_moment(path, 0, 0, 15, False, 0.0)
    assert result["moment_unit"] == "(unknown) × km/s"
    out = tmp_path / "m0.fits"
    write_moment_fits(path, str(out), result, 0, 0, 15, False, 0.0)
    header = fits.getheader(out)
    assert "BUNIT" not in header
    assert "unknown" in " ".join(str(c) for c in header["COMMENT"])
    assert worker_moment(path, 1, 0, 15, False, 0.0)["moment_unit"] == "km/s"


def test_typed_beam_is_seen_by_a_fresh_dataset(tmp_path):
    path = _cube(tmp_path / "c18o.fits", TEMPSCAL="TA*")
    assert ScientificFitsDataset(path).beam_source == "none"
    beam_override_store.set_beam(path, beam_override_store.validate(15.0, None, None))
    ds = ScientificFitsDataset(path)
    assert ds.beam_source == "user"
    assert ds.beam.major == pytest.approx(15.0 / 3600.0)
    assert ds.beam.minor == pytest.approx(15.0 / 3600.0)
    assert ds.beam.pa == 0.0
    os.utime(path, ns=(1, 1))
    assert ScientificFitsDataset(path).beam_source == "none"


def test_header_beam_wins_over_a_typed_one(tmp_path):
    path = _cube(tmp_path / "beam.fits", BMAJ=0.01, BMIN=0.005, BPA=30.0)
    beam_override_store.set_beam(path, (0.1, 0.1, 0.0))
    ds = ScientificFitsDataset(path)
    assert ds.beam_source == "header"
    assert ds.beam.major == pytest.approx(0.01)


@pytest.mark.parametrize("major, minor, pa, match", [
    (-1.0, None, None, "positive"),
    (10.0, 20.0, None, "minor axis"),
    (1e6, None, None, "arcseconds"),
    (15.0, None, float("inf"), "position angle"),
])
def test_beam_validation(major, minor, pa, match):
    with pytest.raises(ValueError, match=match):
        beam_override_store.validate(major, minor, pa)


async def _open(client, headers, path):
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"], body["error"]
    return body


@pytest.mark.asyncio
async def test_beam_route_sets_reports_and_clears(client, auth_headers, tmp_path):
    path = _cube(tmp_path / "c18o.fits", TEMPSCAL="TA*")
    opened = await _open(client, auth_headers, path)
    assert opened["intensity_unit"] == "K" and opened["bunit"] == ""
    assert opened["beam_source"] == "none" and opened["beam_major"] is None
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    resp = await client.post("/v1/datasets/beam", headers=headers, json={
        "dataset_id": opened["dataset_id"], "major_arcsec": 15.0, "minor_arcsec": 14.0,
        "pa_deg": 20.0})
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["beam_source"] == "user"
    assert body["beam_major"] == pytest.approx(15.0 / 3600.0)
    assert body["beam_minor"] == pytest.approx(14.0 / 3600.0)
    assert body["beam_pa"] == pytest.approx(20.0)
    reopened = await _open(client, headers, path)
    assert reopened["beam_source"] == "user"
    assert reopened["beam_major"] == pytest.approx(15.0 / 3600.0)
    resp = await client.post("/v1/datasets/beam", headers=headers, json={
        "dataset_id": opened["dataset_id"], "clear": True})
    body = resp.json()
    assert body["valid"] and body["beam_source"] == "none" and body["beam_major"] is None


@pytest.mark.asyncio
async def test_beam_route_refuses_a_header_beam_and_bad_input(client, auth_headers, tmp_path):
    path = _cube(tmp_path / "beam.fits", BMAJ=0.01, BMIN=0.005)
    opened = await _open(client, auth_headers, path)
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    body = (await client.post("/v1/datasets/beam", headers=headers, json={
        "dataset_id": opened["dataset_id"], "major_arcsec": 15.0})).json()
    assert not body["valid"] and "already has a beam" in body["error"]
    assert body["beam_source"] == "header"
    bare = await _open(client, headers, _cube(tmp_path / "bare.fits"))
    body = (await client.post("/v1/datasets/beam", headers=headers, json={
        "dataset_id": bare["dataset_id"], "major_arcsec": 15.0, "minor_arcsec": 30.0})).json()
    assert not body["valid"] and "minor axis" in body["error"]
    body = (await client.post("/v1/datasets/beam", headers=headers, json={
        "dataset_id": bare["dataset_id"]})).json()
    assert not body["valid"] and "major_arcsec" in body["error"]


def test_partial_header_beam_can_be_completed(tmp_path):
    path = _cube(tmp_path / "half.fits", BMAJ=0.004)
    ds = ScientificFitsDataset(path)
    assert ds.beam_source == "none" and ds.beam_header_partial
    beam_override_store.set_beam(path, beam_override_store.validate(15.0, None, None))
    assert ScientificFitsDataset(path).beam_source == "user"


@pytest.mark.asyncio
async def test_beam_route_notes_a_replaced_partial_beam(client, auth_headers, tmp_path):
    path = _cube(tmp_path / "half.fits", BMAJ=0.004)
    opened = await _open(client, auth_headers, path)
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    body = (await client.post("/v1/datasets/beam", headers=headers, json={
        "dataset_id": opened["dataset_id"], "major_arcsec": 15.0})).json()
    assert body["valid"] and body["beam_source"] == "user"
    assert "incomplete beam" in body["note"]


@pytest.mark.asyncio
async def test_map_math_route_refuses_disjoint_maps(client, auth_headers, tmp_path):
    from app.dependencies import get_exports_dir
    h = fits.Header()
    h["CTYPE1"], h["CTYPE2"] = "RA---TAN", "DEC--TAN"
    h["CRVAL1"], h["CRVAL2"], h["CDELT1"], h["CDELT2"] = 150.0, 2.0, -1 / 3600, 1 / 3600
    h["CRPIX1"], h["CRPIX2"] = 8.0, 8.0
    a = tmp_path / "a.fits"
    fits.PrimaryHDU(np.ones((16, 16), np.float32), header=h).writeto(a)
    h["CRPIX1"] = 8.0 + 500.0
    b = tmp_path / "b.fits"
    fits.PrimaryHDU(np.ones((16, 16), np.float32), header=h).writeto(b)
    oa = await _open(client, auth_headers, a)
    headers = {**auth_headers, "X-Visivo-Session": oa["session_id"]}
    ob = await _open(client, headers, b)
    before = set(get_exports_dir().iterdir())
    body = (await client.post("/v1/products/map_math", headers=headers, json={
        "dataset_id": oa["dataset_id"], "op": "add", "dataset_id_b": ob["dataset_id"]})).json()
    assert not body["valid"] and "do not overlap" in body["error"]
    assert set(get_exports_dir().iterdir()) == before
