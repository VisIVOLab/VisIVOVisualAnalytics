"""Tests for the file classifier behind the Data Hub's single *Open…*.

The point of the classifier is that the user no longer has to know whether the
FITS in their hand is an image, a cube or a table before choosing a menu entry.
So these tests are mostly about telling those three apart from the bytes, and
about saying "ask" rather than guessing when the file genuinely is ambiguous.
"""
from __future__ import annotations

import gzip

import numpy as np
import pytest
from astropy.io import fits

from app.classify import classify_path


# ── FITS ─────────────────────────────────────────────────────────────────────

def test_two_dimensional_fits_is_an_image(tmp_path):
    path = tmp_path / "map.fits"
    fits.PrimaryHDU(np.zeros((32, 48), dtype=np.float32)).writeto(path)
    verdict = classify_path(path)
    assert (verdict["kind"], verdict["opener"]) == ("image", "dataset")
    assert verdict["confident"]


def test_three_dimensional_fits_is_a_cube(tmp_path):
    path = tmp_path / "cube.fits"
    fits.PrimaryHDU(np.zeros((8, 16, 16), dtype=np.float32)).writeto(path)
    verdict = classify_path(path)
    assert (verdict["kind"], verdict["opener"]) == ("cube", "dataset")


def test_degenerate_axes_do_not_make_a_cube(tmp_path):
    """A map with a length-1 Stokes axis is still a map.

    Radio images routinely carry NAXIS=4 with two axes of length one; counting
    axes instead of *significant* axes would send every one of them to the cube
    viewer.
    """
    path = tmp_path / "stokes.fits"
    fits.PrimaryHDU(np.zeros((1, 1, 32, 32), dtype=np.float32)).writeto(path)
    assert classify_path(path)["kind"] == "image"


def test_time_frequency_cube_is_a_dynamic_spectrum(tmp_path):
    path = tmp_path / "dynspec.fits"
    hdu = fits.PrimaryHDU(np.zeros((64, 128), dtype=np.float32))
    hdu.header["CTYPE1"] = "TIME"
    hdu.header["CTYPE2"] = "FREQ"
    hdu.writeto(path)
    verdict = classify_path(path)
    assert (verdict["kind"], verdict["opener"]) == ("dynspec", "dataset")


def test_fits_table_is_ambiguous_and_asks(tmp_path):
    """A FITS table can be a 3-D catalogue or an overlay — only the user knows."""
    path = tmp_path / "sources.fits"
    column = fits.Column(name="GLON", format="E", array=np.array([1.0, 2.0]))
    fits.HDUList([fits.PrimaryHDU(), fits.BinTableHDU.from_columns([column])]).writeto(path)
    verdict = classify_path(path)
    assert verdict["kind"] == "fits-table"
    assert verdict["opener"] == "ask"
    # Ambiguous is not the same as unrecognised: we know what it is.
    assert verdict["confident"]


def test_fits_with_neither_image_nor_table_is_unknown(tmp_path):
    path = tmp_path / "empty.fits"
    fits.HDUList([fits.PrimaryHDU()]).writeto(path)
    verdict = classify_path(path)
    assert verdict["kind"] == "unknown"
    assert verdict["opener"] == "ask"


def test_truncated_fits_still_routes_to_the_dataset_opener(tmp_path):
    """A header we cannot parse is a fact to report, not a crash."""
    path = tmp_path / "broken.fits"
    path.write_bytes(b"SIMPLE  =                    T" + b" " * 50)
    verdict = classify_path(path)
    assert verdict["opener"] == "dataset"
    assert not verdict["confident"]


def test_a_compressed_file_is_refused_rather_than_routed(tmp_path):
    """No open route accepts a `.gz`, so classifying it as an image would only
    move the failure one dialog later."""
    path = tmp_path / "map.fits.gz"
    buffer = tmp_path / "plain.fits"
    fits.PrimaryHDU(np.zeros((8, 8), dtype=np.float32)).writeto(buffer)
    path.write_bytes(gzip.compress(buffer.read_bytes()))
    verdict = classify_path(path)
    assert verdict["opener"] == "unsupported"
    assert "decompress" in verdict["detail"].lower()


# ── VBT ──────────────────────────────────────────────────────────────────────

def test_vbt_header_goes_to_the_vbt_opener(tmp_path):
    path = tmp_path / "points.bin.head"
    path.write_text("float\n3\n100\nX Y Z\n")
    verdict = classify_path(path)
    assert (verdict["kind"], verdict["opener"]) == ("vbt", "vbt")


def test_vbt_binary_opens_through_its_sidecar(tmp_path):
    """The VBT route parses the header, not the binary, so the header is the
    path the client must hand it."""
    binary = tmp_path / "points.bin"
    binary.write_bytes(b"\x00" * 64)
    header = tmp_path / "points.bin.head"
    header.write_text("float\n3\n100\nX Y Z\n")
    verdict = classify_path(binary)
    assert verdict["opener"] == "vbt"
    assert verdict["open_path"] == str(header)


def test_a_header_opens_as_itself(tmp_path):
    path = tmp_path / "points.bin.head"
    path.write_text("float\n3\n100\nX Y Z\n")
    assert classify_path(path)["open_path"] == ""


def test_bare_binary_without_a_sidecar_is_not_claimed_as_vbt(tmp_path):
    binary = tmp_path / "orphan.bin"
    binary.write_bytes(b"\x00" * 64)
    verdict = classify_path(binary)
    assert verdict["opener"] == "ask"


# ── Tables and text ──────────────────────────────────────────────────────────

def test_csv_is_a_catalogue(tmp_path):
    path = tmp_path / "sources.csv"
    path.write_text("glon,glat,flux\n10.0,0.1,1.2\n10.5,0.2,3.4\n")
    verdict = classify_path(path)
    assert (verdict["kind"], verdict["opener"]) == ("catalogue", "catalogue")


def test_tab_separated_text_is_a_catalogue(tmp_path):
    path = tmp_path / "sources.tsv"
    path.write_text("glon\tglat\n10.0\t0.1\n10.5\t0.2\n")
    assert classify_path(path)["opener"] == "catalogue"


def test_votable_is_a_catalogue(tmp_path):
    path = tmp_path / "result.xml"
    path.write_text('<?xml version="1.0"?>\n<VOTABLE version="1.3"><RESOURCE/></VOTABLE>\n')
    assert classify_path(path)["opener"] == "catalogue"


def test_ipac_table_is_a_catalogue(tmp_path):
    path = tmp_path / "table.tbl"
    path.write_text("|glon|glat|\n|double|double|\n 10.0 0.1 \n")
    assert classify_path(path)["opener"] == "catalogue"


def test_speck_is_a_catalogue(tmp_path):
    path = tmp_path / "points.speck"
    path.write_text("datavar 0 colour\n1 2 3 0.5\n")
    assert classify_path(path)["opener"] == "catalogue"


def test_prose_is_not_mistaken_for_a_table(tmp_path):
    """One comma per sentence is not a column separator."""
    path = tmp_path / "notes.txt"
    path.write_text("This is a note about the observation, taken in June.\n"
                    "It mentions nothing in particular.\n")
    verdict = classify_path(path)
    assert verdict["opener"] == "ask"


def test_a_single_delimited_line_is_not_a_table(tmp_path):
    path = tmp_path / "one.txt"
    path.write_text("a,b,c\n")
    assert classify_path(path)["opener"] == "ask"


def test_comment_lines_do_not_break_the_column_count(tmp_path):
    path = tmp_path / "commented.csv"
    path.write_text("# produced by the pipeline\nglon,glat\n1.0,2.0\n3.0,4.0\n")
    assert classify_path(path)["opener"] == "catalogue"


# ── Edge cases ───────────────────────────────────────────────────────────────

def test_hdf5_goes_to_the_dataset_opener(tmp_path):
    path = tmp_path / "beam.h5"
    path.write_bytes(b"\x89HDF\r\n\x1a\n" + b"\x00" * 64)
    verdict = classify_path(path)
    assert (verdict["kind"], verdict["opener"]) == ("dynspec", "dataset")


def test_empty_file_is_reported_as_empty(tmp_path):
    path = tmp_path / "nothing.fits"
    path.write_bytes(b"")
    verdict = classify_path(path)
    assert verdict["opener"] == "ask"
    assert "empty" in verdict["detail"].lower()


def test_directory_is_not_a_file_to_open(tmp_path):
    verdict = classify_path(tmp_path)
    assert verdict["kind"] == "directory"


def test_binary_noise_is_unknown(tmp_path):
    path = tmp_path / "noise.dat"
    path.write_bytes(bytes(range(256)) * 4)
    verdict = classify_path(path)
    assert verdict["kind"] == "unknown"
    assert verdict["opener"] == "ask"


# ── The route ────────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_classify_route_answers_for_a_real_file(client, auth_headers, tmp_path):
    path = tmp_path / "map.fits"
    fits.PrimaryHDU(np.zeros((16, 16), dtype=np.float32)).writeto(path)
    response = await client.post("/v1/files/classify", json={"path": str(path)},
                                 headers=auth_headers)
    assert response.status_code == 200
    body = response.json()
    assert body["valid"] is True
    assert body["opener"] == "dataset"
    assert body["path"] == str(path)


@pytest.mark.asyncio
async def test_classify_route_hides_whether_a_path_exists(client, auth_headers, tmp_path):
    response = await client.post("/v1/files/classify",
                                 json={"path": str(tmp_path / "absent.fits")},
                                 headers=auth_headers)
    body = response.json()
    assert body["valid"] is False
    assert body["error"] == "File not found."


@pytest.mark.asyncio
async def test_classify_route_requires_a_token(client, tmp_path):
    response = await client.post("/v1/files/classify", json={"path": str(tmp_path)})
    assert response.status_code in (401, 403)
