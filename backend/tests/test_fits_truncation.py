"""A FITS file shorter than its headers declare is refused by name (issue #118).

Small synthetic files cut at the places that matter: inside the data, inside a
header, inside the last padding block (still readable, so not refused), in an
extension, in a tile-compressed HDU. Then the routes that open FITS by path,
which must all give the same message instead of "buffer is too small".
"""
from __future__ import annotations

import gzip
import hashlib

import warnings

import numpy as np
import pytest
from astropy.io import fits

from app.fits_dataset import ScientificFitsDataset
from app.fits_truncation import (
    BLOCK,
    FitsTruncatedError,
    _data_size,
    check_not_truncated,
)

NZ, NY, NX = 10, 20, 30            # float32: one plane is 2400 bytes
PLANE = 4 * NY * NX
CUBE = np.arange(NZ * NY * NX, dtype=np.float32).reshape(NZ, NY, NX)


def _cut(src, n: int, dst):
    dst.write_bytes(src.read_bytes()[:n])
    return dst


def _reads(path) -> float:
    """Open as the backend does and read every pixel: what "readable" means."""
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")   # astropy's "File may have been truncated"
        ds = ScientificFitsDataset(path)
        with fits.open(path, memmap=True) as hdul:
            return float(np.nansum(hdul[ds.hdu_index].data))


def _refusal(path, hdu=None) -> str:
    with pytest.raises(FitsTruncatedError) as info:
        check_not_truncated(path, hdu)
    return str(info.value)


@pytest.fixture()
def cube(tmp_path):
    p = tmp_path / "cube.fits"
    fits.PrimaryHDU(CUBE).writeto(p)
    assert p.stat().st_size == BLOCK + 9 * BLOCK   # one header block, 24000 B of data padded
    return p


# ── the check itself ─────────────────────────────────────────────────────────

def test_complete_file_passes(cube):
    check_not_truncated(cube)
    assert _reads(cube) == float(CUBE.sum())


def test_cut_mid_data_names_file_sizes_and_planes(cube, tmp_path):
    cut = _cut(cube, BLOCK + 6 * PLANE + 7, tmp_path / "cut.fits")
    assert _refusal(cut) == ("The file cut.fits is truncated: 17.3 kB on disk, "
                             "28.8 kB declared (6 of 10 planes present).")
    # The message ScientificFitsDataset raises, where it used to open and then
    # fail at the first read with "buffer is too small for requested array".
    with pytest.raises(FitsTruncatedError, match="6 of 10 planes present"):
        ScientificFitsDataset(cut)


def test_cut_mid_header(cube, tmp_path):
    cut = _cut(cube, 1000, tmp_path / "hdr.fits")
    assert _refusal(cut) == "The file hdr.fits is truncated: 1.0 kB on disk, ending inside the header of HDU 0."


@pytest.mark.parametrize("missing", [10, BLOCK - NZ * PLANE % BLOCK])
def test_cut_inside_the_last_padding_block_still_opens(cube, tmp_path, missing):
    """All the data is there; astropy reads it, so it is not refused."""
    cut = _cut(cube, cube.stat().st_size - missing, tmp_path / "pad.fits")
    check_not_truncated(cut)
    assert _reads(cut) == float(CUBE.sum())


def test_one_byte_of_data_missing_is_refused(cube, tmp_path):
    cut = _cut(cube, BLOCK + NZ * PLANE - 1, tmp_path / "short.fits")
    assert "(9 of 10 planes present)" in _refusal(cut)


def test_trailing_bytes_are_not_an_error(cube, tmp_path):
    longer = tmp_path / "longer.fits"
    longer.write_bytes(cube.read_bytes() + b"\0" * 1234)
    check_not_truncated(longer)
    assert _reads(longer) == float(CUBE.sum())


def test_extension_cube(tmp_path):
    p = tmp_path / "ext.fits"
    stat = fits.ImageHDU(np.ones((NZ, NY, NX), np.float32), name="STAT_EXT")
    fits.HDUList([fits.PrimaryHDU(), fits.ImageHDU(CUBE, name="DATA_EXT"), stat]).writeto(p)
    data_offset = 2 * BLOCK
    cut = _cut(p, data_offset + 3 * PLANE + 100, tmp_path / "ext_cut.fits")
    assert _refusal(cut) == ("The file ext_cut.fits is truncated: 13.1 kB on disk, "
                             "31.7 kB declared (3 of 10 planes present).")
    header_cut = _cut(p, BLOCK + 500, tmp_path / "ext_hdr.fits")
    assert _refusal(header_cut) == ("The file ext_hdr.fits is truncated: 3.4 kB on disk, "
                                    "ending inside the header of HDU 1 (after HDU 0).")
    # Cut in the variance cube after the one being read: the cube is whole and
    # the open path never reads STAT_EXT, so the file opens.
    after = _cut(p, p.stat().st_size - 5000, tmp_path / "ext_after.fits")
    check_not_truncated(after)
    assert _reads(after) == float(CUBE.sum())
    # Asked for that HDU, it is refused, by name.
    assert "(3 of 10 planes present)" not in _refusal(after, hdu=2)


def test_cut_in_an_hdu_before_the_image(tmp_path):
    p = tmp_path / "before.fits"
    first = fits.ImageHDU(np.zeros(3000, np.float32), name="SPECTRUM")   # 1-D: not the image
    fits.HDUList([fits.PrimaryHDU(), first, fits.ImageHDU(CUBE, name="DATA")]).writeto(p)
    cut = _cut(p, 2 * BLOCK + 5000, tmp_path / "before_cut.fits")
    assert _refusal(cut) == ("The file before_cut.fits is truncated: 10.8 kB on disk, "
                             "20.2 kB declared (the data of HDU 1 named SPECTRUM is incomplete).")


def test_random_groups_primary_is_not_the_image(tmp_path):
    """astropy's GroupsHDU is not an image, so image_hdu_index reads the
    extension after it: that is the HDU the check must reach."""
    p = tmp_path / "groups.fits"
    groups = fits.GroupsHDU(fits.GroupData(np.ones((2, 3, 4), np.float32), parnames=["p"],
                                           pardata=[np.ones(2)], bitpix=-32))
    fits.HDUList([groups, fits.ImageHDU(CUBE)]).writeto(p)
    with fits.open(p) as hdul:
        data_offset = hdul[1].fileinfo()["datLoc"]
    check_not_truncated(p)
    cut = _cut(p, data_offset + 2 * PLANE + 10, tmp_path / "groups_cut.fits")
    assert _refusal(cut).endswith("(2 of 10 planes present).")


def test_without_an_image_hdu_the_primary_is_read(tmp_path):
    """No HDU has two axes longer than one: HDU 0 is read, as image_hdu_index
    falls back to it. A cut table after a whole primary is not read."""
    table = fits.BinTableHDU.from_columns([fits.Column("data", "J", array=np.arange(10000))])
    p = tmp_path / "spectrum.fits"
    fits.HDUList([fits.PrimaryHDU(np.ones((1, 20), np.float32)), table]).writeto(p)
    cut = _cut(p, p.stat().st_size - 10000, tmp_path / "spectrum_cut.fits")
    check_not_truncated(cut)
    assert ScientificFitsDataset(cut).hdu_index == 0
    # An empty primary leaves nothing else to read: the cut is reported.
    q = tmp_path / "empty.fits"
    fits.HDUList([fits.PrimaryHDU(), table]).writeto(q)
    cut = _cut(q, q.stat().st_size - 10000, tmp_path / "empty_cut.fits")
    assert _refusal(cut).endswith("(the data of HDU 1 is incomplete).")
    header_cut = _cut(q, BLOCK + 100, tmp_path / "empty_hdr.fits")
    assert "ending inside the header of HDU 1" in _refusal(header_cut)


def test_tile_compressed_cube(tmp_path):
    """The declared size is the BINTABLE's (rows plus heap), not the image's."""
    p = tmp_path / "comp.fits"
    noisy = CUBE + np.random.default_rng(0).normal(size=CUBE.shape).astype(np.float32)
    fits.HDUList([fits.PrimaryHDU(), fits.CompImageHDU(noisy, name="COMP")]).writeto(p)
    size = p.stat().st_size
    assert size < BLOCK + NZ * PLANE   # compressed: the image's own size would call it complete
    cut = _cut(p, size - 3000, tmp_path / "comp_cut.fits")
    message = _refusal(cut)
    assert message.startswith("The file comp_cut.fits is truncated: ")
    assert message.endswith("(the compressed data of HDU 1 named COMP is incomplete).")
    check_not_truncated(p)
    assert _reads(p) == pytest.approx(float(noisy.sum()), rel=1e-3)


@pytest.mark.parametrize("bitpix_dtype", [np.uint8, np.int16, np.int32, np.int64, np.float32, np.float64])
def test_every_bitpix(tmp_path, bitpix_dtype):
    p = tmp_path / "b.fits"
    fits.PrimaryHDU(np.ones((4, 6, 8), dtype=bitpix_dtype)).writeto(p)
    plane = np.dtype(bitpix_dtype).itemsize * 6 * 8
    cut = _cut(p, BLOCK + 2 * plane + 1, tmp_path / "b_cut.fits")
    assert "(2 of 4 planes present)" in _refusal(cut)


def test_degenerate_axes_count_planes_over_every_higher_axis(tmp_path):
    p = tmp_path / "c4.fits"
    fits.PrimaryHDU(CUBE.reshape(NZ, 1, NY, NX)).writeto(p)   # NAXIS3 = 1 (Stokes), NAXIS4 = 10
    cut = _cut(p, BLOCK + 3 * PLANE + 1, tmp_path / "c4_cut.fits")
    assert "(3 of 10 planes present)" in _refusal(cut)


def test_image_counts_rows(tmp_path):
    p = tmp_path / "img.fits"
    fits.PrimaryHDU(np.ones((NY, NX), np.int16)).writeto(p)
    cut = _cut(p, BLOCK + 7 * 2 * NX + 1, tmp_path / "img_cut.fits")
    assert _refusal(cut).endswith("(7 of 20 rows present).")


def test_table_with_heap_is_sized_with_pcount(tmp_path):
    p = tmp_path / "vla.fits"
    col = fits.Column("v", "PJ()", array=np.array([np.arange(500), np.arange(700)], dtype=object))
    fits.HDUList([fits.PrimaryHDU(), fits.BinTableHDU.from_columns([col])]).writeto(p)
    with fits.open(p) as hdul:
        pcount = hdul[1].header["PCOUNT"]
    assert pcount >= 4 * 1200
    cut = _cut(p, 2 * BLOCK + 16 + 100, tmp_path / "vla_cut.fits")   # rows there, heap cut
    assert _refusal(cut, hdu=1).endswith("(the data of HDU 1 is incomplete).")


def test_data_size_follows_the_standard():
    # |BITPIX|/8 * GCOUNT * (PCOUNT + NAXIS1 * ... * NAXISn)
    assert _data_size({"BITPIX": -32, "NAXIS": 3, "NAXIS1": 30, "NAXIS2": 20, "NAXIS3": 10}) == 24000
    assert _data_size({"BITPIX": 8, "NAXIS": 0}) == 0
    assert _data_size({"BITPIX": 16, "NAXIS": 2, "NAXIS1": 5, "NAXIS2": 0}) == 0
    # Random groups: NAXIS1 = 0 marks them and is left out of the product.
    assert _data_size({"BITPIX": -32, "NAXIS": 3, "NAXIS1": 0, "NAXIS2": 3, "NAXIS3": 4,
                       "GROUPS": True, "PCOUNT": 5, "GCOUNT": 7}) == 4 * 7 * (5 + 12)
    # A binary table with a heap.
    assert _data_size({"XTENSION": "BINTABLE", "BITPIX": 8, "NAXIS": 2, "NAXIS1": 16,
                       "NAXIS2": 2, "PCOUNT": 4800, "GCOUNT": 1}) == 32 + 4800


def test_wallaby_scale_message(tmp_path, monkeypatch):
    """The cube that prompted #118, from its header alone: no terabyte is written."""
    header = fits.Header()
    header["SIMPLE"], header["BITPIX"], header["NAXIS"] = True, -32, 4
    header["NAXIS1"], header["NAXIS2"], header["NAXIS3"], header["NAXIS4"] = 5560, 5560, 7776, 1
    p = tmp_path / "Eridanus_full_image_V3.fits"
    p.write_bytes(header.tostring().encode("ascii"))
    plane = 4 * 5560 * 5560
    on_disk = BLOCK + 5644 * plane + plane // 4
    monkeypatch.setattr("app.fits_truncation._file_size", lambda q: on_disk)
    assert _refusal(p) == ("The file Eridanus_full_image_V3.fits is truncated: 697.9 GB on disk, "
                           "961.5 GB declared (5644 of 7776 planes present).")


def test_a_few_bytes_short_of_a_large_file_gives_exact_counts(tmp_path, monkeypatch):
    header = fits.Header()
    header["SIMPLE"], header["BITPIX"], header["NAXIS"] = True, 8, 2
    header["NAXIS1"], header["NAXIS2"] = 1_000_000, 1_000_000
    p = tmp_path / "big.fits"
    p.write_bytes(header.tostring().encode("ascii"))
    on_disk = BLOCK + 10**12 - 3
    monkeypatch.setattr("app.fits_truncation._file_size", lambda q: on_disk)
    # 1.0 TB against 1.0 TB would say nothing: the counts are given in bytes.
    assert "1,000,000,002,877 bytes on disk, 1,000,000,005,120 bytes declared" in _refusal(p)


def test_gzip_is_not_size_checked(cube, tmp_path):
    """The header declares the decompressed size: nothing to compare the
    compressed file with, so it is left to the decompressor."""
    gz = tmp_path / "cube.fits.gz"
    gz.write_bytes(gzip.compress(cube.read_bytes()))
    check_not_truncated(gz)
    assert _reads(gz) == float(CUBE.sum())
    half = _cut(gz, gz.stat().st_size // 2, tmp_path / "half.fits.gz")
    check_not_truncated(half)   # not judged here


def test_not_fits_and_missing_paths_pass_unchecked(tmp_path):
    other = tmp_path / "x.h5"
    other.write_bytes(b"\x89HDF\r\n\x1a\n" + b"\0" * 100)
    check_not_truncated(other)
    check_not_truncated(tmp_path / "absent.fits")
    check_not_truncated(tmp_path)


# ── the routes ───────────────────────────────────────────────────────────────

MESSAGE = "The file {name} is truncated: 17.3 kB on disk, 28.8 kB declared (6 of 10 planes present)."


@pytest.fixture()
def truncated(cube, tmp_path):
    return _cut(cube, BLOCK + 6 * PLANE + 7, tmp_path / "truncated.fits")


@pytest.mark.asyncio
async def test_open_route_refuses_with_422_and_the_message(client, auth_headers, truncated):
    resp = await client.post("/v1/datasets/open", json={"path": str(truncated)}, headers=auth_headers)
    assert resp.status_code == 422, resp.text
    assert resp.json() == {"detail": MESSAGE.format(name="truncated.fits")}


@pytest.mark.asyncio
@pytest.mark.filterwarnings("ignore:File may have been truncated")
async def test_open_route_still_opens_a_file_cut_in_its_padding(client, auth_headers, cube, tmp_path):
    cut = _cut(cube, cube.stat().st_size - 100, tmp_path / "padcut.fits")
    resp = await client.post("/v1/datasets/open", json={"path": str(cut)}, headers=auth_headers)
    assert resp.status_code == 200 and resp.json()["valid"], resp.text


@pytest.mark.asyncio
async def test_open_skava_refuses_the_same_way(client, auth_headers, truncated):
    resp = await client.post("/v1/datasets/open_skava", headers=auth_headers,
                             json={"obs_id": "eridanus", "access_url": f"file://{truncated}"})
    assert resp.status_code == 422
    assert resp.json()["detail"] == MESSAGE.format(name="truncated.fits")


@pytest.mark.asyncio
async def test_open_url_refuses_and_drops_the_cached_copy(client, auth_headers, truncated):
    from app.dependencies import get_exports_dir  # noqa: PLC0415

    url = "https://node.example.org/data/eridanus.fits"
    cache = get_exports_dir() / "skava_cache"
    cache.mkdir(parents=True, exist_ok=True)
    cached = cache / f"eridanus__{hashlib.sha256(url.encode()).hexdigest()[:16]}.fits"
    cached.write_bytes(truncated.read_bytes())   # a short download already in the cache
    resp = await client.post("/v1/datasets/open_url", headers=auth_headers,
                             json={"url": url, "obs_id": "eridanus"})
    assert resp.status_code == 422
    assert resp.json()["detail"].startswith(MESSAGE.format(name=cached.name))
    assert "downloads it again" in resp.json()["detail"]
    assert not cached.exists()


@pytest.mark.asyncio
async def test_cutout_route_refuses(client, auth_headers, truncated):
    resp = await client.post("/v1/datasets/cutout", headers=auth_headers,
                             json={"obs_id": "x", "access_url": f"file://{truncated}"})
    assert resp.status_code == 422
    assert resp.json()["detail"] == MESSAGE.format(name="truncated.fits")


@pytest.mark.asyncio
async def test_soda_refuses_operations_but_serves_the_file_as_stored(client, auth_headers, truncated):
    resp = await client.get("/soda/sync", headers=auth_headers,
                            params={"ID": f"file://{truncated}", "POS": "CIRCLE 0 0 0.1"})
    assert resp.status_code == 422
    assert resp.text == "Error: " + MESSAGE.format(name="truncated.fits")
    whole = await client.get("/soda/sync", headers=auth_headers, params={"ID": f"file://{truncated}"})
    assert whole.status_code == 200 and whole.content == truncated.read_bytes()


@pytest.mark.asyncio
async def test_filter_route_refuses(client, auth_headers, truncated):
    resp = await client.post("/v1/image/filter", headers=auth_headers,
                             json={"path": str(truncated), "smooth_fwhm_pixels": 2.0})
    body = resp.json()
    assert body["valid"] is False
    assert body["error"] == MESSAGE.format(name="truncated.fits")


@pytest.mark.asyncio
async def test_velocity_open_refuses(client, auth_headers, tmp_path):
    p = tmp_path / "vel.fits"
    fits.PrimaryHDU(np.zeros((3, 4, 5, 6), np.float32)).writeto(p)
    cut = _cut(p, BLOCK + 2 * 4 * 5 * 6 + 1, tmp_path / "vel_cut.fits")   # 2 of 3x4 planes
    resp = await client.post("/v1/velocity/open", headers=auth_headers,
                             json={"path": str(cut), "box_mpc": 100.0})
    body = resp.json()
    assert body["valid"] is False
    assert body["error"].endswith("(2 of 12 planes present).")
