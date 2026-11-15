"""A catalogue classified in the headset (issue #279): the VOTable the
desktop opens keeps every original column and row of the catalogue, adds the
flag column and the vr_* columns, and appends the sources found by eye."""
from __future__ import annotations

import math

import numpy as np
import pandas as pd
import pytest
from astropy.io import fits
from astropy.table import Table

from app import catalogue_classified as ccl


def _frame():
    return pd.DataFrame({"name": ["SoFiA A", "SoFiA B", "SoFiA C"], "id": [1, 2, 3],
                         "ra": [150.1, 150.2, 150.3], "dec": [2.1, 2.2, 2.3],
                         "f_sum": [1.5, 2.5, np.nan]})


def test_a_session_catalogue_keeps_its_columns_and_rows_and_gets_the_flags():
    sources = [
        {"origin": "catalogue", "row": 2, "label": "SoFiA C", "flag": "spurious", "note": "RFI",
         "x": 5.0, "y": 6.0, "z": None, "velocity_kms": None},
        {"origin": "catalogue", "row": 0, "label": "SoFiA A", "flag": "real", "note": "",
         "x": 1.0, "y": 2.0, "z": 3.5, "velocity_kms": 1234.5},
        {"origin": "user", "label": "VR-1", "lon": 150.15, "lat": 2.15, "x": 10.0, "y": 11.0,
         "z": 12.0, "velocity_kms": 1300.0, "flag": "doubtful", "note": "faint"},
    ]
    t = ccl.classified_table(sources, "vr_flag", _frame(), {"system": "sky", "ra": "ra", "dec": "dec"},
                             "name", "icrs")
    assert t.colnames == ["name", "id", "ra", "dec", "f_sum", "vr_flag", "vr_note", "vr_origin",
                          "vr_x", "vr_y", "vr_z", "vr_velocity_kms"]
    assert len(t) == 4
    assert list(t["vr_flag"]) == ["real", "", "spurious", "doubtful"]
    assert list(t["vr_origin"]) == ["catalogue"] * 3 + ["user"]
    assert list(t["vr_note"]) == ["", "", "RFI", "faint"]
    # The original values unchanged; the found source fills its name and sky columns only.
    assert list(t["id"][:3]) == [1, 2, 3] and bool(t["id"].mask[3])
    assert t["name"][3] == "VR-1" and t["ra"][3] == pytest.approx(150.15) and t["dec"][3] == pytest.approx(2.15)
    assert bool(t["f_sum"].mask[3])
    assert t["vr_velocity_kms"][0] == pytest.approx(1234.5) and math.isnan(t["vr_velocity_kms"][1])
    assert math.isnan(t["vr_z"][2]) and t["vr_z"][0] == pytest.approx(3.5)


def test_a_found_source_in_another_frame_leaves_the_sky_columns_empty():
    sources = [{"origin": "user", "label": "VR-1", "lon": 30.0, "lat": 0.5, "flag": ""}]
    t = ccl.classified_table(sources, "vr_flag", _frame(), {"system": "sky", "ra": "ra", "dec": "dec"},
                             "name", "galactic")
    assert bool(t["ra"].mask[3]) and bool(t["dec"].mask[3]) and t["name"][3] == "VR-1"


def test_a_found_source_given_in_icrs_over_a_galactic_catalogue():
    df = pd.DataFrame({"name": ["G1"], "glon": [30.0], "glat": [0.5]})
    sources = [{"origin": "user", "label": "VR-1", "lon": 280.0, "lat": -2.0, "frame": "icrs"},
               {"origin": "user", "label": "VR-2", "lon": 30.2, "lat": 0.4, "frame": "galactic"}]
    t = ccl.classified_table(sources, "vr_flag", df, {"system": "galactic", "glon": "glon", "glat": "glat"},
                             "name", "galactic")
    assert bool(t["glon"].mask[1]) and t["glon"][2] == pytest.approx(30.2)


def test_an_overlay_without_a_table_writes_its_sources():
    sources = [{"origin": "overlay", "label": "G030.1+0.5", "lon": 30.1, "lat": 0.5, "flag": "real", "note": ""},
               {"origin": "user", "label": "VR-1", "lon": 30.2, "lat": 0.4, "flag": "", "note": ""}]
    t = ccl.classified_table(sources, "class", None, None, None, "galactic")
    assert t.colnames[:3] == ["name", "glon", "glat"] and "class" in t.colnames
    assert list(t["vr_origin"]) == ["overlay", "user"] and list(t["class"]) == ["real", ""]


def test_rows_out_of_the_catalogue_and_clashing_names_are_refused():
    with pytest.raises(ValueError):
        ccl.classified_table([{"origin": "catalogue", "row": 9}], "vr_flag", _frame(), {}, None)
    with pytest.raises(ValueError):
        ccl.classified_table([], "vr_note", _frame(), {}, None)
    with pytest.raises(ValueError):
        ccl.classified_table([], "f_sum", _frame(), {}, None)
    # Without a catalogue the table's own columns are name, ra/dec or glon/glat.
    for name in ("name", "ra", "DEC", "glon", "glat"):
        with pytest.raises(ValueError):
            ccl.classified_table([{"origin": "overlay", "label": "a", "flag": "x"}], name, None, None, None, "icrs")


def test_the_votable_reads_back(tmp_path):
    t = ccl.classified_table([{"origin": "catalogue", "row": 1, "flag": "real", "note": "blended"}], "vr_flag",
                             _frame(), {"system": "sky", "ra": "ra", "dec": "dec"}, "name", "icrs")
    path = tmp_path / "c.xml"
    ccl.write_votable(t, str(path), {"cube": "x.fits", "flag_set": "real, doubtful, spurious"})
    back = Table.read(path, format="votable")
    assert list(back["vr_flag"]) == ["", "real", ""] and list(back["vr_note"]) == ["", "blended", ""]
    # Over a reserved (empty) name, in one step, and no temporary file left.
    reserved = tmp_path / "r.xml"
    reserved.touch()
    ccl.write_votable(t, str(reserved), {})
    assert len(Table.read(reserved, format="votable")) == 3
    assert sorted(p.name for p in tmp_path.iterdir()) == ["c.xml", "r.xml"]
    assert list(back["name"]) == ["SoFiA A", "SoFiA B", "SoFiA C"]


def _cube(tmp_path):
    h = fits.Header()
    for k, v in dict(CTYPE1="RA---SIN", CRPIX1=16, CRVAL1=150.0, CDELT1=-0.002, CUNIT1="deg",
                     CTYPE2="DEC--SIN", CRPIX2=16, CRVAL2=2.0, CDELT2=0.002, CUNIT2="deg",
                     CTYPE3="VRAD", CRPIX3=1.0, CRVAL3=1.0e6, CDELT3=5000.0, CUNIT3="m/s").items():
        h[k] = v
    p = tmp_path / "cube.fits"
    fits.PrimaryHDU(np.zeros((8, 32, 32), np.float32), header=h).writeto(p)
    return str(p)


async def test_the_route_writes_the_votable_in_the_exports(client, auth_headers, tmp_path):
    cube = (await client.post("/v1/datasets/open", json={"path": _cube(tmp_path)}, headers=auth_headers)).json()
    csv = tmp_path / "cat.csv"
    _frame().to_csv(csv, index=False)
    cat = (await client.post("/v1/catalogue/open", json={"path": str(csv), "format": "csv"},
                             headers=auth_headers)).json()
    body = {"dataset_id": cube["dataset_id"], "catalogue_id": cat["dataset_id"], "source_frame": "icrs",
            "flag_set": ["real", "doubtful", "spurious"],
            "sources": [{"origin": "catalogue", "row": 1, "flag": "doubtful", "note": "edge of cube"},
                        {"origin": "user", "label": "VR-1", "lon": 150.0, "lat": 2.0, "x": 15, "y": 15, "z": 4,
                         "velocity_kms": 1015.0, "flag": "real"}]}
    r = await client.post("/v1/catalogue/classified_votable", json=body, headers=auth_headers)
    j = r.json()
    assert r.status_code == 200 and j["valid"], j
    assert j["n_rows"] == 4 and j["n_flagged"] == 2 and j["n_user"] == 1
    assert j["workspace_filename"].endswith(".xml") and "vr_flag" in j["columns"]
    back = Table.read(j["output_path"], format="votable")
    assert list(back["vr_flag"]) == ["", "doubtful", "", "real"]
    # A row that is not in the catalogue: 422, nothing written.
    body["sources"] = [{"origin": "catalogue", "row": 50, "flag": "real"}]
    r = await client.post("/v1/catalogue/classified_votable", json=body, headers=auth_headers)
    assert r.status_code == 422
    # The hub takes it as a product of the headset.
    from app.headset_hub import HeadsetHub   # noqa: F401 - the module the route uses
    from app.routers.headset import PRODUCT_KINDS
    assert "classification" in PRODUCT_KINDS


async def test_catalogue_pixels_says_the_frame_of_the_sources(client, auth_headers, tmp_path):
    cube = (await client.post("/v1/datasets/open", json={"path": _cube(tmp_path)}, headers=auth_headers)).json()
    csv = tmp_path / "cat.csv"
    _frame().to_csv(csv, index=False)
    cat = (await client.post("/v1/catalogue/open", json={"path": str(csv), "format": "csv"},
                             headers=auth_headers)).json()
    r = await client.post("/v1/cube/catalogue_pixels", json={"dataset_id": cube["dataset_id"],
                          "catalogue_id": cat["dataset_id"]}, headers=auth_headers)
    j = r.json()
    assert j["source_frame"] == "icrs" and all("row" in s for s in j["sources"])
