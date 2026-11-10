"""The file browser's preview of any file (issue #159 item 3).

Every kind is told apart from the bytes, and every answer stays inside its
bounds: the file browser asks on each click, possibly through a remote backend.
"""
from __future__ import annotations

import bz2
import gzip
import os
import zipfile

import numpy as np
import pytest
from astropy.io import fits

from app.file_preview import (HDF5_MAX_OBJECTS, MAX_BYTES_CAP, MAX_LINES_CAP, clamp_bounds,
                              hex_dump, preview_path)


def _csv(path, rows=200):
    path.write_text("id,ra,dec,flux,z\n" + "".join(
        f"{i},{i * 0.5},{-i * 0.25},{i * 1.5},0.0{i % 10}\n" for i in range(rows)))
    return path


# ── Text ─────────────────────────────────────────────────────────────────────

def test_csv_names_separator_and_columns_and_is_cut_at_max_lines(tmp_path):
    out = preview_path(_csv(tmp_path / "cat.csv"), max_lines=10)
    assert out["kind"] == "text"
    assert out["format"] == "Comma-separated table, 5 columns"
    assert out["text"].splitlines()[0] == "id,ra,dec,flux,z"
    assert len(out["text"].splitlines()) == 10
    assert out["truncated"] is True


def test_tsv_is_sniffed_from_content_not_from_the_name(tmp_path):
    path = tmp_path / "table.dat"
    path.write_text("a\tb\tc\n1\t2\t3\n4\t5\t6\n")
    out = preview_path(path)
    assert out["format"] == "Tab-separated table, 3 columns"
    assert out["truncated"] is False
    assert out["text"] == "a\tb\tc\n1\t2\t3\n4\t5\t6"


def test_byte_bound_reads_only_the_start_and_drops_the_half_line(tmp_path):
    path = _csv(tmp_path / "cat.csv", rows=5000)
    out = preview_path(path, max_bytes=100, max_lines=400)
    assert len(out["text"].encode()) <= 100
    assert out["truncated"] is True
    # The last line shown is a whole row, not the half the byte bound cut.
    assert len(out["text"].splitlines()[-1].split(",")) == 5


def test_latin1_text_is_text(tmp_path):
    path = tmp_path / "notes.txt"
    path.write_bytes("Osservazioni: velocità e °C\nseconda riga\n".encode("latin-1"))
    out = preview_path(path)
    assert out["kind"] == "text"
    assert out["format"] == "Text (Latin-1)"
    assert "velocità" in out["text"]


def test_utf8_cut_inside_a_character_is_still_utf8(tmp_path):
    path = tmp_path / "u.txt"
    path.write_text("à" * 100 + "\n", encoding="utf-8")   # 2 bytes each
    out = preview_path(path, max_bytes=7)
    assert out["kind"] == "text" and out["format"] == "Text"
    assert out["truncated"] is True


def test_votable_xml_json_speck_labels(tmp_path):
    (tmp_path / "a.vot").write_text('<?xml version="1.0"?>\n<VOTABLE version="1.4">\n</VOTABLE>\n')
    (tmp_path / "b.xml").write_text("<root>\n</root>\n")
    (tmp_path / "c.json").write_text('{"a": 1}\n')
    (tmp_path / "d.speck").write_text("datavar 0 lum\n1 2 3 4\n")
    labels = {name: preview_path(tmp_path / name)["format"]
              for name in ("a.vot", "b.xml", "c.json", "d.speck")}
    assert labels == {"a.vot": "VOTable", "b.xml": "XML", "c.json": "JSON",
                      "d.speck": "speck point catalogue"}


def test_empty_file(tmp_path):
    path = tmp_path / "empty.txt"
    path.write_bytes(b"")
    out = preview_path(path)
    assert (out["kind"], out["format"], out["text"], out["size_bytes"]) == \
        ("text", "Empty file", "", 0)


# ── Binary ───────────────────────────────────────────────────────────────────

def test_binary_gets_a_bounded_hex_dump(tmp_path):
    path = tmp_path / "blob.dat"
    path.write_bytes(bytes(range(256)) * 40)
    out = preview_path(path)
    assert out["kind"] == "binary" and out["format"] == "Binary data"
    lines = out["text"].splitlines()
    assert len(lines) == 16                      # 256 bytes, 16 per line
    assert lines[4].startswith("00000040  40 41 42")
    assert out["truncated"] is True
    assert out["size_bytes"] == 256 * 40


def test_hex_dump_ascii_column():
    assert hex_dump(b"AB\x00") == [f"00000000  41 42 00{' ' * 39}  |AB.|"]


# ── VBT ──────────────────────────────────────────────────────────────────────

def _vbt(tmp_path, name="volume"):
    data = tmp_path / f"{name}.bin"
    np.zeros(4 * 4 * 4 * 2, dtype="<f4").tofile(data)
    head = tmp_path / f"{name}.bin.head"
    head.write_text("float\n2\n64 4 4 4 1 1 1\nlittle\ndensity\ntemperature\n")
    return data, head


def test_bin_shows_its_head_and_head_shows_itself(tmp_path):
    data, head = _vbt(tmp_path)
    from_bin = preview_path(data)
    from_head = preview_path(head)
    for out in (from_bin, from_head):
        assert out["kind"] == "vbt"
        assert out["format"] == "VisIVO Binary Table, volume 4×4×4, 2 fields"
        assert out["text"].splitlines()[-1] == "temperature"
    assert from_bin["size_bytes"] == data.stat().st_size


def test_points_vbt_with_plain_head_sibling(tmp_path):
    data = tmp_path / "points.bin"
    data.write_bytes(b"\x00" * 24)
    (tmp_path / "points.head").write_text("float\n3\n2\nlittle\nX\nY\nZ\n")
    out = preview_path(data)
    assert (out["kind"], out["format"]) == ("vbt", "VisIVO Binary Table, 2 points, 3 fields")


def test_head_that_is_not_vbt_is_text(tmp_path):
    """A SCAMP ``.head`` is a text FITS header, not a VBT."""
    path = tmp_path / "image.head"
    path.write_text("CRVAL1  =  10.0\nCRVAL2  =  20.0\nEND\n")
    assert preview_path(path)["kind"] == "text"


def test_bin_sibling_refused_by_allow_is_not_read(tmp_path):
    data, _ = _vbt(tmp_path)
    out = preview_path(data, allow=lambda p: False)
    assert out["kind"] == "binary"


# ── HDF5 ─────────────────────────────────────────────────────────────────────

def test_hdf5_tree_with_shapes_and_types(tmp_path):
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "beam.h5"
    with h5py.File(path, "w") as f:
        f.create_dataset("SUB_ARRAY_POINTING_000/BEAM_000/STOKES_0",
                         shape=(100, 16), dtype="f4")
        f.create_dataset("meta", data=3)
    out = preview_path(path)
    assert out["kind"] == "hdf5"
    assert out["format"] == "HDF5, 2 groups, 2 datasets"
    assert "    STOKES_0  100×16  float32" in out["text"].splitlines()
    assert "meta  scalar  int64" in out["text"]
    assert out["truncated"] is False


def test_hdf5_walk_stops_at_the_object_cap(tmp_path):
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "many.h5"
    with h5py.File(path, "w") as f:
        for i in range(HDF5_MAX_OBJECTS + 50):
            f.create_dataset(f"d{i:04d}", shape=(1,), dtype="u1")
    out = preview_path(path, max_lines=MAX_LINES_CAP)
    assert len(out["text"].splitlines()) == HDF5_MAX_OBJECTS
    assert out["truncated"] is True
    assert out["format"] == f"HDF5, 0+ groups, {HDF5_MAX_OBJECTS}+ datasets"


# ── FITS ─────────────────────────────────────────────────────────────────────

def _cube(path):
    hdu = fits.PrimaryHDU(np.zeros((6, 5, 7), dtype=np.float32))
    hdu.header["OBJECT"] = "NGC 3137"
    hdu.writeto(path)


def test_fits_cube_summary_and_cards(tmp_path):
    path = tmp_path / "cube.fits"
    _cube(path)
    out = preview_path(path, max_lines=MAX_LINES_CAP)
    assert out["kind"] == "fits"
    assert out["format"] == "FITS image 7×5×6"
    lines = out["text"].splitlines()
    assert lines[0] == "HDU 0  PRIMARY  image 7×5×6 (BITPIX -32)"
    assert lines[1] == ""
    assert lines[2].startswith("SIMPLE  =")
    assert any(line.startswith("OBJECT  = 'NGC 3137'") for line in lines)


def test_fits_gz_header_off_the_stream(tmp_path):
    plain = tmp_path / "cube.fits"
    _cube(plain)
    path = tmp_path / "cube.fits.gz"
    path.write_bytes(gzip.compress(plain.read_bytes()))
    out = preview_path(path)
    assert (out["kind"], out["format"]) == ("fits", "FITS image 7×5×6 (gzip)")


def test_fits_tile_compressed(tmp_path):
    path = tmp_path / "map.fits.fz"
    fits.HDUList([fits.PrimaryHDU(),
                  fits.CompImageHDU(np.zeros((8, 9), dtype=np.float32))]).writeto(path)
    out = preview_path(path)
    assert (out["kind"], out["format"]) == ("fits", "FITS image 9×8 (tile-compressed)")


def test_fits_table(tmp_path):
    path = tmp_path / "sources.fits"
    table = fits.BinTableHDU.from_columns([
        fits.Column(name="RA", format="D", array=np.arange(4.0)),
        fits.Column(name="DEC", format="D", array=np.arange(4.0))])
    fits.HDUList([fits.PrimaryHDU(), table]).writeto(path)
    out = preview_path(path)
    assert out["format"] == "FITS table, 2 columns, 4 rows"
    assert "HDU 1  -  table, 2 columns, 4 rows" in out["text"]


# ── Compressed ───────────────────────────────────────────────────────────────

def test_gzip_csv_shows_decompressed_lines(tmp_path):
    path = tmp_path / "cat.csv.gz"
    path.write_bytes(gzip.compress(_csv(tmp_path / "cat.csv").read_bytes()))
    out = preview_path(path, max_lines=3)
    assert out["kind"] == "compressed"
    assert out["format"] == "Comma-separated table, 5 columns (gzip-compressed)"
    assert out["text"].splitlines() == ["id,ra,dec,flux,z", "0,0.0,0.0,0.0,0.00",
                                        "1,0.5,-0.25,1.5,0.01"]
    assert out["truncated"] is True


def test_bzip2_binary_is_named_not_dumped(tmp_path):
    path = tmp_path / "blob.bz2"
    path.write_bytes(bz2.compress(bytes(range(256)) * 4))
    out = preview_path(path)
    assert (out["kind"], out["format"]) == ("compressed", "bzip2-compressed data")


def test_zip_lists_members_or_shows_the_only_one(tmp_path):
    many = tmp_path / "many.zip"
    with zipfile.ZipFile(many, "w") as z:
        z.writestr("a.txt", "hello\n")
        z.writestr("b.bin", b"\x00" * 10)
    out = preview_path(many)
    assert out["format"] == "ZIP archive, 2 files"
    assert out["text"].splitlines() == ["a.txt  6 bytes", "b.bin  10 bytes"]
    one = tmp_path / "one.zip"
    with zipfile.ZipFile(one, "w") as z:
        z.writestr("t.tsv", "a\tb\n1\t2\n")
    out = preview_path(one)
    assert out["format"] == "Tab-separated table, 2 columns (zip: t.tsv)"


def test_damaged_gzip_is_reported_not_raised(tmp_path):
    path = tmp_path / "bad.gz"
    path.write_bytes(b"\x1f\x8b\x08\x00garbage")
    out = preview_path(path)
    assert out["kind"] == "compressed"
    assert "could not be read" in out["text"]


# ── Directory, special files, bounds ─────────────────────────────────────────

def test_directory_lists_folders_first(tmp_path):
    (tmp_path / "zeta.fits").write_bytes(b"")
    (tmp_path / "Alpha").mkdir()
    (tmp_path / ".hidden").write_text("x")
    out = preview_path(tmp_path, hide_hidden=True)
    assert (out["kind"], out["format"], out["size_bytes"]) == ("directory", "Directory, 2 entries", 0)
    assert out["text"].splitlines() == ["Alpha/", "zeta.fits"]


@pytest.mark.skipif(not hasattr(os, "mkfifo"), reason="no FIFOs here")
def test_fifo_is_not_opened(tmp_path):
    path = tmp_path / "pipe"
    os.mkfifo(path)
    assert preview_path(path)["format"] == "Special file (not read)"


def test_bounds_are_capped():
    assert clamp_bounds(10 ** 9, 10 ** 9) == (MAX_BYTES_CAP, MAX_LINES_CAP)
    assert clamp_bounds(0, -3) == (1, 1)


def test_long_single_line_is_cut_to_the_byte_bound(tmp_path):
    path = tmp_path / "one.txt"
    path.write_text("x" * 5000)
    out = preview_path(path, max_bytes=64)
    assert out["text"] == "x" * 64 and out["truncated"] is True


# ── The route ────────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_route_answers_the_contract(client, auth_headers, tmp_path):
    path = _csv(tmp_path / "cat.csv")
    response = await client.post("/v1/files/preview",
                                 json={"path": str(path), "max_lines": 2},
                                 headers=auth_headers)
    assert response.status_code == 200
    body = response.json()
    assert set(body) == {"path", "size_bytes", "kind", "format", "text", "truncated"}
    assert body["path"] == str(path)
    assert body["size_bytes"] == path.stat().st_size
    assert body["kind"] == "text" and body["truncated"] is True
    assert len(body["text"].splitlines()) == 2


@pytest.mark.asyncio
async def test_route_caps_rather_than_refuses_large_bounds(client, auth_headers, tmp_path):
    path = _csv(tmp_path / "cat.csv", rows=5000)
    response = await client.post("/v1/files/preview",
                                 json={"path": str(path), "max_bytes": 10 ** 7,
                                       "max_lines": 10 ** 6},
                                 headers=auth_headers)
    body = response.json()
    assert response.status_code == 200
    assert len(body["text"].splitlines()) == MAX_LINES_CAP
    assert len(body["text"].encode()) <= MAX_BYTES_CAP


@pytest.mark.asyncio
@pytest.mark.parametrize("payload", [{"max_bytes": 0}, {"max_lines": -1},
                                     {"max_bytes": "many"}, {"path": ""}])
async def test_route_rejects_bad_arguments(client, auth_headers, tmp_path, payload):
    body = {"path": str(_csv(tmp_path / "cat.csv")), **payload}
    response = await client.post("/v1/files/preview", json=body, headers=auth_headers)
    assert response.status_code == 422


@pytest.mark.asyncio
async def test_route_missing_is_404(client, auth_headers, tmp_path):
    response = await client.post("/v1/files/preview", json={"path": str(tmp_path / "no.csv")},
                                 headers=auth_headers)
    assert response.status_code == 404


@pytest.mark.asyncio
async def test_route_refuses_outside_the_roots_existing_or_not(client, auth_headers, tmp_path,
                                                                monkeypatch):
    root = tmp_path / "root"
    root.mkdir()
    outside = _csv(tmp_path / "secret.csv")
    (root / "link.csv").symlink_to(outside)
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(root))
    for target in (outside, tmp_path / "absent.csv", root / "link.csv"):
        response = await client.post("/v1/files/preview", json={"path": str(target)},
                                     headers=auth_headers)
        assert response.status_code == 403, target


@pytest.mark.asyncio
async def test_route_does_not_follow_a_head_symlinked_out_of_the_roots(
        client, auth_headers, tmp_path, monkeypatch):
    root = tmp_path / "root"
    root.mkdir()
    data, head = _vbt(tmp_path)
    (root / "volume.bin").write_bytes(data.read_bytes())
    (root / "volume.bin.head").symlink_to(head)
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(root))
    response = await client.post("/v1/files/preview", json={"path": str(root / "volume.bin")},
                                 headers=auth_headers)
    assert response.status_code == 200
    assert response.json()["kind"] == "binary"


@pytest.mark.asyncio
async def test_route_requires_a_token(client, tmp_path):
    response = await client.post("/v1/files/preview", json={"path": str(tmp_path)})
    assert response.status_code in (401, 403)


# ── Review fixes: bounded gzip headers, null bytes, the sniffed head ─────────

def _gzip_fits_without_end(path, megabytes=40):
    """A gzipped stream that starts like FITS and never has an END card."""
    card = b"SIMPLE  =                    T".ljust(80)
    filler = b"COMMENT".ljust(80)
    with gzip.open(path, "wb", compresslevel=1) as out:
        out.write(card)
        chunk = filler * (2880 // 80 * 100)
        for _ in range(megabytes * 1024 * 1024 // len(chunk) + 1):
            out.write(chunk)
    return path


def test_gzip_header_without_end_is_refused_without_reading_it_whole(tmp_path):
    from app.compressed_fits import _MAX_HEADER_BLOCKS, gzip_image_headers
    path = _gzip_fits_without_end(tmp_path / "bomb.fits.gz")
    calls = []
    real_open = gzip.open

    class Counting:
        def __init__(self, *args, **kwargs):
            self._s = real_open(*args, **kwargs)

        def read(self, n=-1):
            calls.append(n)
            return self._s.read(n)

        def __getattr__(self, name):
            return getattr(self._s, name)

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            self._s.close()

    import app.compressed_fits as cf
    original = cf.gzip.open
    cf.gzip.open = Counting
    try:
        assert gzip_image_headers(path) is None
    finally:
        cf.gzip.open = original
    assert len(calls) <= _MAX_HEADER_BLOCKS + 1
    out = preview_path(path, 16384, 60)
    assert out["kind"] == "fits" and "could not be read" in out["text"]


def test_gzip_preview_does_not_decompress_a_large_table_to_reach_the_image(tmp_path):
    from app import file_preview
    from app.compressed_fits import gzip_image_headers
    table = fits.BinTableHDU.from_columns([fits.Column(name="v", format="D",
                                                       array=np.zeros(200_000))])
    image = fits.ImageHDU(np.zeros((4, 5, 6), dtype=np.float32))
    plain = tmp_path / "t.fits"
    fits.HDUList([fits.PrimaryHDU(), table, image]).writeto(plain)
    path = tmp_path / "t.fits.gz"
    with open(plain, "rb") as src, gzip.open(path, "wb") as out:
        out.write(src.read())
    assert len(gzip_image_headers(path)) == 3                       # unbounded: reaches the image
    assert len(gzip_image_headers(path, max_skip_bytes=1024)) == 2  # bounded: stops at the table
    original = file_preview.GZIP_MAX_SKIP_BYTES
    file_preview.GZIP_MAX_SKIP_BYTES = 1024
    try:
        out = preview_path(path, 16384, 60)
    finally:
        file_preview.GZIP_MAX_SKIP_BYTES = original
    assert out["kind"] == "fits" and "HDU 1" in out["text"] and "HDU 2" not in out["text"]


def test_a_table_is_described_from_the_same_head_the_classifier_reads(tmp_path):
    from app.text_table import SNIFF_BYTES, describe_table
    wide = ",".join(f"column_with_a_long_name_{i}" for i in range(400))  # one line > 8 KB
    assert len(wide) > SNIFF_BYTES
    text = wide + "\n" + ",".join("1" for _ in range(400)) + "\n"
    assert describe_table(text) is None   # the classifier cannot judge it either


@pytest.mark.asyncio
async def test_route_refuses_a_null_byte_with_422(client, auth_headers, tmp_path):
    response = await client.post("/v1/files/preview",
                                 json={"path": str(tmp_path / "a\x00b.csv")},
                                 headers=auth_headers)
    assert response.status_code == 422
