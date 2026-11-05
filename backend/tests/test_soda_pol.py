"""SODA POL: polarization states mapped to the STOKES axis, through /soda/sync,
/soda/async and the DataLink descriptor of /soda/links."""
from __future__ import annotations

import asyncio
import io
import xml.etree.ElementTree as ET

import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS

from app.soda_cutout import (
    CutoutError,
    CutoutNoOverlap,
    describe,
    make_cutout,
    parse_pol,
    stokes_states,
)

UWS = "{http://www.ivoa.net/xml/UWS/v1.0}"
VOT = "{http://www.ivoa.net/xml/VOTable/v1.3}"
POS = "CIRCLE 10.0 -30.0 0.002"


def _cube(path, *, crval=1.0, cdelt=1.0, crpix=1.0, nstokes=4, cd=False):
    """A (stokes, freq, y, x) cube whose plane k holds the value of its FITS Stokes code."""
    codes = crval + (np.arange(nstokes) + 1 - crpix) * cdelt
    data = np.empty((nstokes, 5, 8, 8), np.float32)
    data[:] = codes[:, None, None, None]
    hdu = fits.PrimaryHDU(data)
    for key, value in (("CTYPE1", "RA---SIN"), ("CTYPE2", "DEC--SIN"), ("CTYPE3", "FREQ"),
                       ("CTYPE4", "STOKES"), ("CRVAL1", 10.0), ("CRVAL2", -30.0), ("CRVAL3", 1.4e9),
                       ("CRVAL4", crval), ("CDELT1", -1e-3), ("CDELT2", 1e-3), ("CDELT3", 1e6),
                       ("CDELT4", cdelt), ("CRPIX1", 4.0), ("CRPIX2", 4.0), ("CRPIX3", 1.0), ("CRPIX4", crpix),
                       ("CUNIT3", "Hz"), ("MJD-OBS", 60000.0), ("EXPTIME", 3600.0)):
        hdu.header[key] = value
    if cd:
        for key in ("CDELT1", "CDELT2", "CDELT3", "CDELT4"):
            n = key[-1]
            hdu.header[f"CD{n}_{n}"] = hdu.header.pop(key)
    hdu.writeto(path)
    return path


def _planes(fits_bytes):
    with fits.open(io.BytesIO(fits_bytes)) as hdul:
        data, header = hdul[0].data, hdul[0].header.copy()
    return [float(v) for v in data[:, 0, 0, 0]], stokes_states(header), data.shape


# ── parsing and mapping ─────────────────────────────────────────────────────


def test_parse_pol_vocabulary_order_and_repeats():
    assert parse_pol(["Q", "u", "Q"]) == ("Q", "U")
    assert parse_pol("I V") == ("I", "V")          # the stored form round-trips
    assert parse_pol([]) is None and parse_pol(None) is None
    assert parse_pol(["POLI", "POLA", "YX"]) == ("POLI", "POLA", "YX")
    with pytest.raises(CutoutError, match="not a polarization state"):
        parse_pol(["Z"])


@pytest.mark.parametrize("crval, cdelt, crpix, expected", [
    (1.0, 1.0, 1.0, ["I", "Q", "U", "V"]),
    (-1.0, -1.0, 1.0, ["RR", "LL", "RL", "LR"]),
    (-5.0, -1.0, 1.0, ["XX", "YY", "XY", "YX"]),
    (-8.0, 1.0, 1.0, ["YX", "XY", "YY", "XX"]),
    (2.0, 1.0, 2.0, ["I", "Q", "U", "V"]),            # reference pixel elsewhere
])
def test_stokes_codes_follow_the_wcs(tmp_path, crval, cdelt, crpix, expected):
    path = _cube(tmp_path / "c.fits", crval=crval, cdelt=cdelt, crpix=crpix)
    assert stokes_states(fits.getheader(path)) == expected


def test_a_stokes_only_dataset(tmp_path):
    # One axis, the STOKES one: the WCS answers with an array, not a tuple.
    path = tmp_path / "stokes1d.fits"
    hdu = fits.PrimaryHDU(np.array([1.0, 2.0, 3.0, 4.0], np.float32))
    for key, value in (("CTYPE1", "STOKES"), ("CRVAL1", 1.0), ("CDELT1", 1.0), ("CRPIX1", 1.0)):
        hdu.header[key] = value
    hdu.writeto(path)
    assert stokes_states(fits.getheader(path)) == ["I", "Q", "U", "V"]
    with fits.open(io.BytesIO(make_cutout(str(path), pol=["I", "V"]).fits_bytes)) as hdul:
        assert list(hdul[0].data) == [1.0, 4.0]


def test_codes_without_a_fits_meaning_are_unknown(tmp_path):
    path = _cube(tmp_path / "c.fits", crval=4.0, nstokes=3)        # 4, 5, 6
    assert stokes_states(fits.getheader(path)) == ["V", None, None]


# ── make_cutout ─────────────────────────────────────────────────────────────


@pytest.mark.parametrize("pol, values, states", [
    (["Q"], [2.0], ["Q"]),
    (["U", "Q"], [2.0, 3.0], ["Q", "U"]),             # planes come in the file's order
    (["I", "V"], [1.0, 4.0], ["I", "V"]),             # non-contiguous, evenly spaced: a step
    (["I", "U"], [1.0, 3.0], ["I", "U"]),
    (["V", "Q", "I", "U"], [1.0, 2.0, 3.0, 4.0], ["I", "Q", "U", "V"]),
])
def test_pol_selects_the_planes(tmp_path, pol, values, states):
    path = _cube(tmp_path / "iquv.fits")
    result = make_cutout(str(path), pol=pol)
    got_values, got_states, _ = _planes(result.fits_bytes)
    assert got_values == values and got_states == states
    assert result.applied["pol"] is True


def test_uneven_states_come_back_one_hdu_each(tmp_path):
    # I, Q and V of IQUV cannot share a linear STOKES axis without U: SODA
    # returns only the requested states, so each is its own image HDU.
    path = _cube(tmp_path / "iquv.fits")
    result = make_cutout(str(path), pol=["V", "I", "Q"], pos=POS)
    with fits.open(io.BytesIO(result.fits_bytes)) as hdul:
        assert [h.name for h in hdul] == ["STOKES_I", "STOKES_Q", "STOKES_V"]
        assert isinstance(hdul[1], fits.ImageHDU)
        for hdu, code in zip(hdul, (1.0, 2.0, 4.0)):
            assert hdu.data.shape[0] == 1 and float(hdu.data[0, 0, 0, 0]) == code
            assert stokes_states(hdu.header) == [{1.0: "I", 2.0: "Q", 4.0: "V"}[code]]
            assert hdu.data.shape[2] < 8                      # POS applied to every plane
    assert result.applied["hdus"] == 3


def test_an_implicit_stokes_axis_is_one_state(tmp_path):
    # WCSAXES = 4 on a 3-D array: the STOKES axis has no pixels, one plane.
    path = tmp_path / "implicit.fits"
    hdu = fits.PrimaryHDU(np.ones((5, 8, 8), np.float32))
    for key, value in (("WCSAXES", 4), ("CTYPE1", "RA---SIN"), ("CTYPE2", "DEC--SIN"), ("CTYPE3", "FREQ"),
                       ("CTYPE4", "STOKES"), ("CRVAL1", 10.0), ("CRVAL2", -30.0), ("CRVAL3", 1.4e9),
                       ("CRVAL4", 3.0), ("CDELT1", -1e-3), ("CDELT2", 1e-3), ("CDELT3", 1e6),
                       ("CDELT4", 1.0), ("CRPIX1", 4.0), ("CRPIX2", 4.0), ("CRPIX3", 1.0), ("CRPIX4", 1.0)):
        hdu.header[key] = value
    hdu.writeto(path)
    assert stokes_states(fits.getheader(path)) == ["U"]
    with fits.open(io.BytesIO(make_cutout(str(path), pol=["U", "Q"]).fits_bytes)) as hdul:
        assert hdul[0].data.shape == (5, 8, 8)
    with pytest.raises(CutoutNoOverlap, match="has U"):
        make_cutout(str(path), pol=["I"])
    assert describe(str(path)).pol == ["U"]


def test_pol_on_a_cd_matrix_axis(tmp_path):
    path = _cube(tmp_path / "cd.fits", cd=True)
    values, states, _ = _planes(make_cutout(str(path), pol=["I", "V"]).fits_bytes)
    assert values == [1.0, 4.0] and states == ["I", "V"]


def test_pol_on_a_descending_correlation_axis(tmp_path):
    path = _cube(tmp_path / "lin.fits", crval=-5.0, cdelt=-1.0)       # XX YY XY YX
    values, states, _ = _planes(make_cutout(str(path), pol=["YX", "XX"]).fits_bytes)
    assert values == [-5.0, -8.0] and states == ["XX", "YX"]


def test_pol_with_pos_band_and_time(tmp_path):
    path = _cube(tmp_path / "iquv.fits")
    result = make_cutout(str(path), pos=POS, band="0.21390 0.21402", time="59999 60001", pol=["Q", "U"])
    values, states, shape = _planes(result.fits_bytes)
    assert states == ["Q", "U"] and values == [2.0, 3.0]
    assert shape[0] == 2 and shape[1] < 5 and shape[2] < 8 and shape[3] < 8
    assert all(result.applied[k] for k in ("pos", "band", "time", "pol"))
    # The celestial WCS still points at the same sky.
    with fits.open(io.BytesIO(result.fits_bytes)) as hdul:
        cut = WCS(hdul[0].header).celestial
    full = WCS(fits.getheader(path)).celestial
    sky = cut.pixel_to_world(0, 0)
    x, y = full.world_to_pixel(sky)
    assert abs(x - round(float(x))) < 1e-6 and abs(y - round(float(y))) < 1e-6


def test_a_missing_state_selects_nothing(tmp_path):
    path = _cube(tmp_path / "iquv.fits")
    for pol in (["RR"], ["POLI"], ["POLA", "XX"]):
        with pytest.raises(CutoutNoOverlap, match="I Q U V"):
            make_cutout(str(path), pol=pol)


def test_a_single_stokes_file_is_one_state(tmp_path):
    # MFImage releases keep one Stokes per file: a degenerate axis with CRVAL4 = 2 is Q.
    path = _cube(tmp_path / "q.fits", crval=2.0, nstokes=1)
    values, states, _ = _planes(make_cutout(str(path), pol=["Q", "U"]).fits_bytes)
    assert values == [2.0] and states == ["Q"]
    with pytest.raises(CutoutNoOverlap, match="has Q"):
        make_cutout(str(path), pol=["U"])


def test_without_a_stokes_axis_the_data_are_total_intensity(synthetic_fits_cube):
    whole = make_cutout(str(synthetic_fits_cube), pol=["I"])
    with fits.open(io.BytesIO(whole.fits_bytes)) as hdul:
        assert hdul[0].data.shape == (32, 64, 64)
    assert whole.applied["pol"] is True
    with pytest.raises(CutoutNoOverlap, match="no STOKES axis"):
        make_cutout(str(synthetic_fits_cube), pol=["Q", "U"])


def test_describe_lists_what_the_dataset_has(tmp_path, synthetic_fits_cube, synthetic_fits_image):
    axes = describe(str(_cube(tmp_path / "lin.fits", crval=-5.0, cdelt=-1.0)))
    assert axes.pol == ["XX", "YY", "XY", "YX"] and axes.celestial
    assert axes.band_m is not None and axes.band_m[0] < 0.2141 < axes.band_m[1]
    assert axes.time_mjd == (60000.0, 60000.0 + 3600.0 / 86400.0)
    plain = describe(str(synthetic_fits_cube))
    assert plain.pol == ["I"] and plain.band_m is not None and plain.time_mjd is None
    image = describe(str(synthetic_fits_image))
    assert image.band_m is None and image.pol == ["I"]


def test_describe_reads_headers_only(tmp_path, monkeypatch):
    # A tile-compressed cube is described without decompressing its pixels.
    source = _cube(tmp_path / "iquv.fits")
    packed = tmp_path / "iquv_packed.fits"
    with fits.open(source) as hdul:
        fits.HDUList([fits.PrimaryHDU(), fits.CompImageHDU(hdul[0].data, hdul[0].header)]).writeto(packed)
    touched = []
    original = fits.CompImageHDU.data
    monkeypatch.setattr(fits.CompImageHDU, "data", property(lambda self: touched.append(1) or original.fget(self)))
    assert describe(str(packed)).pol == ["I", "Q", "U", "V"]
    assert not touched


# ── routes ──────────────────────────────────────────────────────────────────


def _id(path):
    return f"file://{path}"


@pytest.mark.asyncio
async def test_sync_pol_is_multi_valued(client, auth_headers, tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    path = _cube(tmp_path / "iquv.fits")
    resp = await client.get(f"/soda/sync?ID={_id(path)}&POL=I&POL=V&POS={POS}", headers=auth_headers)
    assert resp.status_code == 200, resp.text
    assert set(resp.headers["x-soda-applied"].split(",")) == {"pos", "pol"}
    values, states, _ = _planes(resp.content)
    assert values == [1.0, 4.0] and states == ["I", "V"]
    # POL alone is a constraint: the answer is a cutout, not the file as stored.
    alone = await client.post("/soda/sync", data={"ID": _id(path), "pol": "Q"}, headers=auth_headers)
    assert alone.status_code == 200 and _planes(alone.content)[1] == ["Q"]


@pytest.mark.asyncio
@pytest.mark.parametrize("pol, status, start", [
    ("RR", 204, ""), ("POLI", 204, ""), ("Z", 400, "UsageError: POL 'Z'"),
])
async def test_sync_pol_empty_and_wrong(client, auth_headers, tmp_path, monkeypatch, pol, status, start):
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    path = _cube(tmp_path / "iquv.fits")
    resp = await client.get("/soda/sync", params={"ID": _id(path), "POL": pol}, headers=auth_headers)
    assert resp.status_code == status and resp.text.startswith(start)


@pytest.mark.asyncio
async def test_sync_pol_on_a_total_intensity_cube(client, auth_headers, synthetic_fits_cube):
    ok = await client.get("/soda/sync", params={"ID": _id(synthetic_fits_cube), "POL": "I"},
                          headers=auth_headers)
    assert ok.status_code == 200 and "pol" in ok.headers["x-soda-applied"]
    empty = await client.get("/soda/sync", params={"ID": _id(synthetic_fits_cube), "POL": "V"},
                             headers=auth_headers)
    assert empty.status_code == 204


@pytest.mark.asyncio
async def test_pol_is_not_an_operation_parameter_of_moment(client, auth_headers, synthetic_fits_cube):
    resp = await client.post("/soda/async", headers=auth_headers, data={
        "ID": _id(synthetic_fits_cube), "OPERATION": "MOMENT", "MOMENT": "0", "POL": "I"})
    assert resp.status_code == 400 and "POL" in resp.text


@pytest.mark.asyncio
async def test_async_pol_job(client, auth_headers, tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    path = _cube(tmp_path / "iquv.fits")
    created = await client.post("/soda/async", headers=auth_headers, data={
        "ID": _id(path), "POL": ["U", "Q"], "BAND": "0.2139 0.2140", "PHASE": "RUN"})
    assert created.status_code == 303, created.text
    job = created.headers["location"]
    for _ in range(200):
        phase = (await client.get(job + "/phase", headers=auth_headers)).text
        if phase in ("COMPLETED", "ERROR"):
            break
        await asyncio.sleep(0.05)
    assert phase == "COMPLETED"
    doc = ET.fromstring((await client.get(job, headers=auth_headers)).text)
    pols = [p.text for p in doc.iter(f"{UWS}parameter") if p.get("id") == "POL"]
    assert pols == ["U", "Q"]
    result = await client.get(job + "/results/result", headers=auth_headers)
    values, states, _ = _planes(result.content)
    assert states == ["Q", "U"] and values == [2.0, 3.0]
    as_json = (await client.get(job, headers={**auth_headers, "Accept": "application/json"})).json()
    assert "pol" in as_json["applied"].split(",")

    empty = await client.post("/soda/async", headers=auth_headers,
                              data={"ID": _id(path), "POL": "LL", "PHASE": "RUN"})
    job = empty.headers["location"]
    for _ in range(200):
        if (await client.get(job + "/phase", headers=auth_headers)).text == "COMPLETED":
            break
        await asyncio.sleep(0.05)
    text = (await client.get(job + "/results/result", headers=auth_headers)).text
    assert text.startswith("No data") and "I Q U V" in text


# ── DataLink descriptor ─────────────────────────────────────────────────────


def _descriptors(body: str) -> dict[str, ET.Element]:
    root = ET.fromstring(body)
    return {r.get("ID"): r for r in root.iter(f"{VOT}RESOURCE") if r.get("utype") == "adhoc:service"}


def _input(resource: ET.Element, name: str) -> ET.Element | None:
    group = resource.find(f"{VOT}GROUP[@name='inputParams']")
    return next((p for p in group.findall(f"{VOT}PARAM") if p.get("name") == name), None)


@pytest.mark.asyncio
async def test_links_advertise_the_states_the_dataset_has(client, auth_headers, tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    full = _cube(tmp_path / "rrll.fits", crval=-1.0, cdelt=-1.0, nstokes=2)
    resp = await client.get("/soda/links", params={"ID": _id(full)}, headers=auth_headers)
    assert resp.status_code == 200 and "content=datalink" in resp.headers["content-type"]
    descriptors = _descriptors(resp.text)
    assert set(descriptors) == {"soda-sync-0", "soda-async-0"}
    pol = _input(descriptors["soda-sync-0"], "POL")
    assert pol.get("ucd") == "meta.code;phys.polarization"
    assert [o.get("value") for o in pol.iter(f"{VOT}OPTION")] == ["RR", "LL"]
    band = _input(descriptors["soda-sync-0"], "BAND")
    assert float(band.find(f"{VOT}VALUES/{VOT}MIN").get("value")) < 0.2141
    assert _input(descriptors["soda-sync-0"], "ID").get("ref") == "primaryID"
    standard = {p.get("name"): p.get("value") for p in descriptors["soda-async-0"].findall(f"{VOT}PARAM")}
    assert standard["standardID"] == "ivo://ivoa.net/std/SODA#async-1.0"
    assert standard["accessURL"].endswith("/soda/async")


@pytest.mark.asyncio
async def test_links_rows_and_faults(client, auth_headers, tmp_path, monkeypatch, synthetic_fits_image):
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    good = _cube(tmp_path / "iquv.fits")
    table = tmp_path / "table.fits"
    fits.HDUList([fits.PrimaryHDU(), fits.BinTableHDU.from_columns(
        [fits.Column(name="a", format="E", array=np.ones(3))])]).writeto(table)
    resp = await client.post("/soda/links", headers=auth_headers, data={"ID": [
        _id(good), _id(tmp_path / "missing.fits"), _id(synthetic_fits_image), _id(table)]})
    assert resp.status_code == 200, resp.text
    rows = [[td.text or "" for td in tr] for tr in ET.fromstring(resp.text).iter(f"{VOT}TR")]
    by_id: dict[str, list[list[str]]] = {}
    for row in rows:
        by_id.setdefault(row[0], []).append(row)
    assert sorted(r[5] for r in by_id[_id(good)]) == ["#cutout", "#cutout", "#this"]
    this = next(r for r in by_id[_id(good)] if r[5] == "#this")
    assert "/soda/sync?ID=file%3A%2F%2F" in this[1] and int(this[7]) == good.stat().st_size
    assert by_id[_id(tmp_path / "missing.fits")][0][3].startswith("NotFoundFault")
    assert by_id[_id(synthetic_fits_image)][0][3].startswith("UsageFault")   # outside the roots
    assert by_id[_id(tmp_path / "missing.fits")][0][5] == "#this"            # DataLink 1.1 §3.2.4
    assert [r[5] for r in by_id[_id(table)]] == ["#this"]                      # nothing to cut
    descriptors = _descriptors(resp.text)
    assert set(descriptors) == {"soda-sync-0", "soda-async-0"}
    texts = [d.find(f"{VOT}DESCRIPTION").text for d in descriptors.values()]
    assert len(set(texts)) == len(texts)


@pytest.mark.asyncio
async def test_links_need_an_id_and_the_token(client, auth_headers):
    none = await client.get("/soda/links", headers=auth_headers)        # no ID: an empty table
    assert none.status_code == 200 and not list(ET.fromstring(none.text).iter(f"{VOT}TR"))
    wrong = await client.get("/soda/links", params={"FOO": "1"}, headers=auth_headers)
    assert wrong.status_code == 400 and "votable" in wrong.headers["content-type"]
    doc = ET.fromstring(wrong.text)
    status = next(doc.iter(f"{VOT}INFO"))
    assert status.get("name") == "QUERY_STATUS" and status.get("value") == "ERROR"
    assert status.text.startswith("UsageFault: parameters not understood")
    assert doc.find(f"{VOT}RESOURCE/{VOT}TABLE") is not None       # the (empty) links table
    assert (await client.get("/soda/links", params={"ID": "file:///x"})).status_code == 401
    caps = await client.get("/soda/capabilities")
    assert "ivo://ivoa.net/std/DataLink#links-1.1" in caps.text


@pytest.mark.asyncio
async def test_pyvo_builds_a_pol_cutout_from_the_descriptor(client, auth_headers, tmp_path, monkeypatch):
    adhoc = pytest.importorskip("pyvo.dal.adhoc")
    from astropy.io.votable import parse

    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    path = _cube(tmp_path / "iquv.fits")
    resp = await client.get("/soda/links", params={"ID": _id(path)}, headers=auth_headers)
    links = adhoc.DatalinkResults(parse(io.BytesIO(resp.content)))
    proc = links.get_first_proc()
    assert proc.service_def == "soda-sync-0"
    pol = next(p for p in proc.input_params if p.name == "POL")
    assert list(pol.values.options[i][1] for i in range(len(pol.values.options))) == ["I", "Q", "U", "V"]
    query = adhoc.SodaQuery.from_resource(proc, links.get_adhocservice_by_id(proc.service_def))
    query.pol.add("I")
    query.pol.add("V")
    assert query["ID"] == _id(path) and query["POL"] == ["I", "V"]
    # The query pyvo would send, replayed on the in-process app.
    sent = await client.get("/soda/sync", params=[("ID", query["ID"])] + [("POL", s) for s in query["POL"]],
                            headers=auth_headers)
    assert sent.status_code == 200 and _planes(sent.content)[1] == ["I", "V"]
