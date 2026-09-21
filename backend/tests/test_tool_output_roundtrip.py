"""What a tool produces has to be openable by the next step.

Every tool here is tested on its own elsewhere: the filter smooths correctly,
the header patch writes the keywords, the cone search parses the VOTable. What
none of those tests ask is the question the user actually asks next — *can I
open the thing it just made?*

That question has a specific shape in this application: a tool writes a file
into the workspace, the client hands the path to ``/v1/files/classify``, and the
answer decides which viewer receives it. A tool whose output is classified as
"unknown", or as a catalogue when it is an image, is broken in a way no unit
test of the tool itself can see — and it was found by hand (the smoothed cube
opening in a new window) rather than by anything in this suite.

So these tests chain the routes the way the client does.
"""
from __future__ import annotations

import csv

import numpy as np
import pytest
from astropy.io import fits


def _image(path, shape=(32, 48), **cards):
    header = fits.Header()
    header["CRPIX1"], header["CRPIX2"] = 16.0, 16.0
    header["CRVAL1"], header["CRVAL2"] = 10.0, -5.0
    header["CDELT1"], header["CDELT2"] = -0.001, 0.001
    header["CTYPE1"], header["CTYPE2"] = "RA---TAN", "DEC--TAN"
    for key, value in cards.items():
        header[key] = value
    data = np.random.default_rng(7).normal(size=shape).astype(np.float32)
    fits.PrimaryHDU(data=data, header=header).writeto(path)
    return path


def _cube(path, shape=(6, 24, 32)):
    header = fits.Header()
    header["CRPIX1"], header["CRPIX2"], header["CRPIX3"] = 16.0, 12.0, 1.0
    header["CRVAL1"], header["CRVAL2"], header["CRVAL3"] = 10.0, -5.0, 1.4e9
    header["CDELT1"], header["CDELT2"], header["CDELT3"] = -0.001, 0.001, 1e6
    header["CTYPE1"], header["CTYPE2"], header["CTYPE3"] = "RA---TAN", "DEC--TAN", "FREQ"
    header["BUNIT"] = "Jy/beam"
    data = np.random.default_rng(11).normal(size=shape).astype(np.float32)
    fits.PrimaryHDU(data=data, header=header).writeto(path)
    return path


async def _classify(client, auth_headers, path):
    response = await client.post("/v1/files/classify", json={"path": path},
                                 headers=auth_headers)
    body = response.json()
    assert body["valid"] is True, body
    return body


# ── Smooth / Regrid ──────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_a_smoothed_image_is_classified_as_an_image(client, auth_headers, tmp_path):
    source = _image(tmp_path / "map.fits")
    filtered = (await client.post("/v1/image/filter",
                                  json={"path": str(source), "smooth_fwhm_pixels": 2.0},
                                  headers=auth_headers)).json()
    assert filtered["valid"] is True

    verdict = await _classify(client, auth_headers, filtered["output_path"])
    assert (verdict["kind"], verdict["opener"]) == ("image", "dataset")


@pytest.mark.asyncio
async def test_a_smoothed_cube_is_still_a_cube(client, auth_headers, tmp_path):
    """Found by hand first: the smoothed cube has to open in the cube viewer,
    which means the classifier must still see three significant axes in it."""
    source = _cube(tmp_path / "cube.fits")
    filtered = (await client.post("/v1/image/filter",
                                  json={"path": str(source), "smooth_fwhm_pixels": 2.0},
                                  headers=auth_headers)).json()
    assert filtered["valid"] is True
    assert filtered["depth"] == 6

    verdict = await _classify(client, auth_headers, filtered["output_path"])
    assert (verdict["kind"], verdict["opener"]) == ("cube", "dataset")


@pytest.mark.asyncio
async def test_a_regridded_cube_survives_the_round_trip(client, auth_headers, tmp_path):
    source = _cube(tmp_path / "cube.fits")
    filtered = (await client.post("/v1/image/filter",
                                  json={"path": str(source), "regrid_factor": 2},
                                  headers=auth_headers)).json()
    assert filtered["valid"] is True

    verdict = await _classify(client, auth_headers, filtered["output_path"])
    assert verdict["kind"] == "cube"
    # …and the dataset route really opens it, which is the step after the classify.
    opened = (await client.post("/v1/datasets/open",
                                json={"path": filtered["output_path"]},
                                headers=auth_headers)).json()
    assert opened["valid"] is True, opened.get("error")
    assert opened["kind"] == "cube"
    assert (opened["width"], opened["height"]) == (16, 12)


@pytest.mark.asyncio
async def test_a_regrid_that_squeezes_a_degenerate_axis_opens_as_an_image(
        client, auth_headers, tmp_path):
    """A Stokes axis of length 1 is dropped by the filter; the result must be
    an IMAGE, and its WCS must still be celestial once the axis keywords have
    been renumbered."""
    header = fits.Header()
    header["WCSAXES"] = 4
    header["CTYPE1"], header["CTYPE2"] = "RA---TAN", "DEC--TAN"
    header["CTYPE3"], header["CTYPE4"] = "FREQ", "STOKES"
    header["CRPIX1"], header["CRPIX2"], header["CRPIX3"], header["CRPIX4"] = 16.0, 16.0, 1.0, 1.0
    header["CRVAL1"], header["CRVAL2"], header["CRVAL3"], header["CRVAL4"] = 10.0, -5.0, 1.4e9, 1.0
    header["CDELT1"], header["CDELT2"], header["CDELT3"], header["CDELT4"] = (
        -0.001, 0.001, 1e6, 1.0)
    source = tmp_path / "stokes.fits"
    fits.PrimaryHDU(data=np.zeros((1, 1, 32, 32), dtype=np.float32),
                    header=header).writeto(source)

    filtered = (await client.post("/v1/image/filter",
                                  json={"path": str(source), "smooth_fwhm_pixels": 1.5},
                                  headers=auth_headers)).json()
    assert filtered["valid"] is True

    verdict = await _classify(client, auth_headers, filtered["output_path"])
    assert (verdict["kind"], verdict["opener"]) == ("image", "dataset")
    opened = (await client.post("/v1/datasets/open",
                                json={"path": filtered["output_path"]},
                                headers=auth_headers)).json()
    assert opened["valid"] is True, opened.get("error")
    assert opened["wcs_status"] in ("ok", "sanitized")


# ── Header editing ───────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_a_repaired_header_opens_where_it_could_not_before(
        client, auth_headers, tmp_path):
    """The whole point of the editor: a file that would not open, opened."""
    header = fits.Header()
    header["CRPIX1"], header["CRPIX2"] = 16.0, 16.0
    header["CRVAL1"], header["CRVAL2"] = 10.0, -5.0
    header["CDELT1"], header["CDELT2"] = -0.001, 0.001
    source = tmp_path / "broken.fits"
    fits.PrimaryHDU(data=np.zeros((32, 32), dtype=np.float32), header=header).writeto(source)

    before = (await client.post("/v1/datasets/open", json={"path": str(source)},
                                headers=auth_headers)).json()
    assert before["valid"] is True
    # It "opens", but with no CTYPE there is no sky under it: the axes are
    # pixels wearing a WCS, which is exactly the state the editor exists for.
    assert before["ctype"][0] == ""


    patched = (await client.post(
        "/v1/files/header/patch",
        json={"path": str(source),
              "cards": [{"key": "CTYPE1", "value": "RA---TAN"},
                        {"key": "CTYPE2", "value": "DEC--TAN"}]},
        headers=auth_headers)).json()
    assert patched["wcs_celestial"] is True

    verdict = await _classify(client, auth_headers, patched["output_path"])
    assert verdict["opener"] == "dataset"
    after = (await client.post("/v1/datasets/open", json={"path": patched["output_path"]},
                               headers=auth_headers)).json()
    assert after["valid"] is True
    assert after["ctype"][0] == "RA---TAN"
    assert after["ctype"][1] == "DEC--TAN"


# ── Searches that end in a catalogue ─────────────────────────────────────────

@pytest.mark.asyncio
async def test_a_cone_search_result_opens_as_a_catalogue(
        client, auth_headers, tmp_path, monkeypatch):
    """The legacy's dead button, made real: the rows are saved and the saved
    file has to be something the catalogue viewer accepts."""
    votable = """<?xml version="1.0"?>
    <VOTABLE version="1.3"><RESOURCE type="results"><TABLE>
    <FIELD name="_RAJ2000" datatype="double"/><FIELD name="_DEJ2000" datatype="double"/>
    <FIELD name="Jmag" datatype="float"/>
    <DATA><TABLEDATA>
    <TR><TD>83.8221</TD><TD>-5.3911</TD><TD>12.3</TD></TR>
    <TR><TD>83.8300</TD><TD>-5.3800</TD><TD>13.1</TD></TR>
    </TABLEDATA></DATA></TABLE></RESOURCE></VOTABLE>""".encode()

    class _Response:
        def read(self):
            return votable

        def __enter__(self):
            return self

        def __exit__(self, *args):
            return False

    monkeypatch.setattr("app.safe_fetch.safe_urlopen", lambda *a, **k: _Response())
    result = (await client.post(
        "/v1/resolve/cone_search",
        json={"url": "https://example.org/scs", "ra": 83.82, "dec": -5.39, "radius": 0.05},
        headers=auth_headers)).json()
    assert result["valid"] is True
    assert result["position_columns"]["ra"] == "_RAJ2000"

    verdict = await _classify(client, auth_headers, result["output_path"])
    assert (verdict["kind"], verdict["opener"]) == ("catalogue", "catalogue")
    opened = (await client.post("/v1/catalogue/open",
                                json={"path": result["output_path"], "format": "csv"},
                                headers=auth_headers)).json()
    assert opened["valid"] is True, opened.get("error")
    assert {field["name"] for field in opened["fields"]} >= {"_RAJ2000", "_DEJ2000", "Jmag"}


@pytest.mark.asyncio
async def test_a_vlkb_query_result_opens_as_a_catalogue(
        client, auth_headers, monkeypatch):
    # The contour column contains the separator, so a real service quotes it —
    # and the classifier has to parse the fields rather than count commas.
    payload = (b"idbubble,glon_cen,glat_cen,contour\n"
               b'1,10.5,0.1,"10.4_10.6,0.0_0.2"\n'
               b'2,10.6,0.2,"10.5_10.7,0.1_0.3"\n')

    class _Response:
        def read(self):
            return payload

        def __enter__(self):
            return self

        def __exit__(self, *args):
            return False

    monkeypatch.setenv("VISIVO_VLKB_ALLOWED_HOSTS", "vlkb.example.org")
    monkeypatch.setattr("app.safe_fetch.safe_urlopen", lambda *a, **k: _Response())
    result = (await client.post(
        "/v1/vlkb/tap",
        json={"preset": "bubbles", "glon_min": 10.0, "glon_max": 11.0,
              "glat_min": -1.0, "glat_max": 1.0,
              "service_url": "https://vlkb.example.org/vlkb"},
        headers=auth_headers)).json()
    assert result["valid"] is True
    assert result["total_rows"] == 2

    verdict = await _classify(client, auth_headers, result["output_path"])
    assert verdict["opener"] == "catalogue"
    opened = (await client.post("/v1/catalogue/open",
                                json={"path": result["output_path"], "format": "csv"},
                                headers=auth_headers)).json()
    assert opened["valid"] is True, opened.get("error")


# ── The history sees the chain ───────────────────────────────────────────────

@pytest.mark.asyncio
async def test_a_chain_of_tools_shows_up_as_a_chain_of_jobs(client, auth_headers, tmp_path):
    """What the Data Hub's "Last 5 jobs" is for: the work, in the order it ran,
    without the browsing in between."""
    source = _image(tmp_path / "map.fits")
    await client.post("/v1/datasets/open", json={"path": str(source)}, headers=auth_headers)
    filtered = (await client.post("/v1/image/filter",
                                  json={"path": str(source), "smooth_fwhm_pixels": 2.0},
                                  headers=auth_headers)).json()
    await client.post("/v1/files/classify", json={"path": filtered["output_path"]},
                      headers=auth_headers)
    await client.post("/v1/datasets/open", json={"path": filtered["output_path"]},
                      headers=auth_headers)

    recent = (await client.get("/v1/health", headers=auth_headers)).json()["recent_tasks"]
    operations = [entry["operation"] for entry in recent]
    # Newest first, and the registry is process-wide (other tests have run), so
    # what is asserted is the order of THESE three, not their absolute position.
    assert operations[:3] == ["datasets/open", "image/filter", "datasets/open"]
    # The browsing in between is deliberately not a job.
    assert "files/classify" not in operations[:3]
    assert all(entry["status"] == "done" for entry in recent[:3])


@pytest.mark.asyncio
async def test_a_galactic_catalogue_opens_and_keeps_its_frame(client, auth_headers, tmp_path):
    """Every VLKB table is in Galactic coordinates. Refusing them ended the
    chain one step from the end — and copying l/b into the ra/dec fields would
    have been worse: it puts a source up to 90 degrees from where it is."""
    path = tmp_path / "bubbles.csv"
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["idbubble", "glon_cen", "glat_cen", "contour"])
        writer.writerow(["1", "10.5", "0.1", "10.4_10.6,0.0_0.2"])
        writer.writerow(["2", "10.6", "0.2", "10.5_10.7,0.1_0.3"])

    opened = (await client.post("/v1/catalogue/open",
                                json={"path": str(path), "format": "csv"},
                                headers=auth_headers)).json()
    assert opened["valid"] is True, opened.get("error")
    coords = opened["coordinate_fields"]
    assert coords["system"] == "galactic"
    assert (coords["glon"], coords["glat"]) == ("glon_cen", "glat_cen")
    # Not smuggled in as celestial coordinates.
    assert coords["ra"] == "" and coords["dec"] == ""


@pytest.mark.asyncio
async def test_the_band_merged_column_names_are_recognised(client, auth_headers, tmp_path):
    """sed_view_final calls them glonft/glatft."""
    path = tmp_path / "sources.csv"
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["designation", "glonft", "glatft", "flux250"])
        writer.writerow(["HIGALBM010", "10.5", "0.1", "12.3"])
        writer.writerow(["HIGALBM011", "10.6", "0.2", "8.1"])

    opened = (await client.post("/v1/catalogue/open",
                                json={"path": str(path), "format": "csv"},
                                headers=auth_headers)).json()
    assert opened["valid"] is True, opened.get("error")
    assert opened["coordinate_fields"]["system"] == "galactic"


@pytest.mark.asyncio
async def test_a_catalogue_with_no_position_at_all_is_still_refused(
        client, auth_headers, tmp_path):
    """Accepting Galactic must not turn into accepting anything: a table with no
    position is not something a viewer can place."""
    path = tmp_path / "nopos.csv"
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["name", "flux", "note"])
        writer.writerow(["a", "1.0", "x"])
        writer.writerow(["b", "2.0", "y"])

    opened = (await client.post("/v1/catalogue/open",
                                json={"path": str(path), "format": "csv"},
                                headers=auth_headers)).json()
    assert opened["valid"] is False
    assert "Galactic" in opened["error"]
