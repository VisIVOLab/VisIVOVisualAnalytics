"""Compressed FITS open like their uncompressed versions (issue #57).

A ``.fits.gz`` and a tile-compressed (fpack) file are decompressed once into
the workspace cache (app/compressed_fits.py), and every reader reads that
copy. These tests hold the copy to the original pixel for pixel, the API's
answers to the uncompressed file's byte for byte, and the cache to its
promises: one decompression per source version, a clear refusal for a damaged
file, room made without touching an open dataset, nothing left by a
cancelled or failed decompression.
"""
from __future__ import annotations

import asyncio
import contextlib
import gzip
import os
import shutil
import threading
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits
from httpx import AsyncClient

from app import compressed_fits as cf
from app.fits_truncation import FitsTruncatedError


@pytest.fixture(autouse=True)
def _cache(tmp_path, monkeypatch):
    """A cache of its own for every test, and no session holding anything."""
    directory = tmp_path / "decompressed"
    monkeypatch.setenv("VISIVO_DECOMPRESS_CACHE_DIR", str(directory))
    monkeypatch.delenv("VISIVO_DECOMPRESS_CACHE_MAX_BYTES", raising=False)
    return directory


def _cube(seed: int = 7, shape=(6, 10, 12)) -> np.ndarray:
    data = np.random.default_rng(seed).normal(size=shape).astype(np.float32)
    data[0, 0, 0] = np.nan
    return data


def _wcs_header(header: fits.Header) -> fits.Header:
    for k, v in (("CTYPE1", "RA---SIN"), ("CTYPE2", "DEC--SIN"), ("CRVAL1", 83.8),
                 ("CRVAL2", -5.4), ("CDELT1", -1e-3), ("CDELT2", 1e-3), ("CRPIX1", 6.0),
                 ("CRPIX2", 5.0), ("CUNIT1", "deg"), ("CUNIT2", "deg"), ("BUNIT", "Jy/beam")):
        header[k] = v
    if header.get("NAXIS", 0) >= 3 or "ZNAXIS3" in header:
        for k, v in (("CTYPE3", "VRAD"), ("CRVAL3", 1000.0), ("CDELT3", 5000.0),
                     ("CRPIX3", 1.0), ("CUNIT3", "m/s")):
            header[k] = v
    return header


def _plain(path: Path, data: np.ndarray) -> Path:
    hdu = fits.PrimaryHDU(data)
    _wcs_header(hdu.header)
    hdu.writeto(path)
    return path


def _gzipped(path: Path, plain: Path) -> Path:
    path.write_bytes(gzip.compress(plain.read_bytes(), compresslevel=1))
    return path


def _fpacked(path: Path, data: np.ndarray, compression: str = "RICE_1", **kwargs) -> Path:
    comp = fits.CompImageHDU(data, compression_type=compression, **kwargs)
    _wcs_header(comp.header)
    fits.HDUList([fits.PrimaryHDU(), comp]).writeto(path)
    return path


def _read(path: Path, hdu: int | None = None) -> np.ndarray:
    from app.fits_dataset import image_hdu_index
    with fits.open(path, memmap=False) as hdul:
        return np.array(hdul[image_hdu_index(hdul) if hdu is None else hdu].data)


# ── The decompressed copy ────────────────────────────────────────────────────


@pytest.mark.parametrize("data", [_cube(), _cube()[0]], ids=["cube", "image"])
def test_gzip_copy_is_the_original(tmp_path, data, _cache):
    plain = _plain(tmp_path / "x.fits", data)
    source = _gzipped(tmp_path / "x.fits.gz", plain)
    made = cf.materialize(source)
    assert made is not None and made.compression == "gzip" and not made.cache_hit
    assert made.path.parent.parent == _cache
    assert made.path.name == "x.fits"            # products are named as from x.fits
    assert made.path.read_bytes() == plain.read_bytes()
    assert cf.source_of(made.path) == str(source)


def test_an_uncompressed_file_is_left_alone(tmp_path):
    plain = _plain(tmp_path / "x.fits", _cube())
    assert cf.materialize(plain) is None
    assert cf.readable_path(plain) == plain
    assert cf.source_of(plain) is None


@pytest.mark.parametrize("data", [_cube(), _cube()[0]], ids=["cube", "image"])
def test_rice_float_copy_is_astropys_decompression(tmp_path, data):
    """RICE quantises floats (lossy): the copy holds exactly what astropy
    decompresses, which is the original within the quantisation step."""
    source = _fpacked(tmp_path / "x.fits.fz", data)
    made = cf.materialize(source)
    assert made.compression == "tile"
    with fits.open(source) as hdul:
        expected = np.array(hdul[1].data)
    got = _read(made.path)
    np.testing.assert_array_equal(got, expected)
    finite = np.isfinite(data)
    assert np.array_equal(np.isnan(got), ~finite)            # NaN survives
    assert np.max(np.abs(got[finite] - data[finite])) < 0.2  # q=16 on unit noise
    with fits.open(made.path) as hdul:
        hdul.verify("exception")
        assert hdul[1].header["CTYPE1"] == "RA---SIN"


@pytest.mark.parametrize("compression", ["GZIP_1", "GZIP_2"])
def test_gzip_tile_floats_are_lossless(tmp_path, compression):
    """Without quantisation (quantize_level=0, fpack -q0) floats are lossless."""
    data = _cube()
    made = cf.materialize(_fpacked(tmp_path / "x.fz", data, compression, quantize_level=0.0))
    np.testing.assert_array_equal(_read(made.path), data)
    assert made.path.name == "x.fits"


def test_rice_integers_keep_bzero_exactly(tmp_path):
    """uint16 is stored as int16 with BZERO = 32768: the copy keeps the stored
    integers and the scaling, so the values are exact."""
    data = np.random.default_rng(3).integers(0, 65535, size=(5, 9, 11)).astype(np.uint16)
    made = cf.materialize(_fpacked(tmp_path / "u.fz", data))
    with fits.open(made.path, memmap=False) as hdul:
        assert hdul[1].header["BITPIX"] == 16 and hdul[1].header["BZERO"] == 32768
        np.testing.assert_array_equal(hdul[1].data, data)


def test_multi_extension_keeps_every_hdu(tmp_path):
    """Empty primary, compressed cube, plain variance cube, table: the image
    HDU is chosen as for the uncompressed file, the rest is copied as is."""
    data = _cube()
    comp = fits.CompImageHDU(data, name="DATA_EXT")
    _wcs_header(comp.header)
    variance = fits.ImageHDU(np.ones_like(data), name="STAT_EXT")
    table = fits.BinTableHDU.from_columns([fits.Column("x", "D", array=np.arange(4.0))],
                                          name="CAT")
    source = tmp_path / "eso.fits"     # tile-compressed under a plain .fits name
    fits.HDUList([fits.PrimaryHDU(), comp, variance, table]).writeto(source)
    assert cf.compression_of(source) == "tile"
    made = cf.materialize(source)
    with fits.open(made.path) as hdul:
        assert [h.name for h in hdul] == ["PRIMARY", "DATA_EXT", "STAT_EXT", "CAT"]
        assert isinstance(hdul[1], fits.ImageHDU) and not isinstance(hdul[1], fits.CompImageHDU)
        np.testing.assert_array_equal(hdul[2].data, np.ones_like(data))
        np.testing.assert_array_equal(hdul[3].data["x"], np.arange(4.0))
    with fits.open(source) as hdul:
        np.testing.assert_array_equal(_read(made.path), hdul[1].data)


def test_an_image_that_was_the_primary_becomes_the_primary_again(tmp_path):
    """fpack marks a compressed primary with ZSIMPLE = T; funpack restores it
    as the primary, and so does the cache (readers that use HDU 0 see it)."""
    data = _cube()
    source = _fpacked(tmp_path / "p.fz", data, "GZIP_2", quantize_level=0.0)
    with fits.open(source, mode="update", disable_image_compression=True) as hdul:
        hdul[1].header["ZSIMPLE"] = True
    made = cf.materialize(source)
    with fits.open(made.path) as hdul:
        assert len(hdul) == 1 and hdul[0].header["SIMPLE"] is True
        np.testing.assert_array_equal(hdul[0].data, data)
        hdul.verify("exception")


def test_a_gzipped_fpack_file_is_expanded_too(tmp_path):
    data = _cube()
    fz = _fpacked(tmp_path / "x.fits.fz", data, "GZIP_2", quantize_level=0.0)
    made = cf.materialize(_gzipped(tmp_path / "x.fits.fz.gz", fz))
    assert made.path.name == "x.fits"
    np.testing.assert_array_equal(_read(made.path), data)


def test_blocks_cover_the_array_in_order():
    shape = (3, 5, 7)
    seen = np.zeros(shape, dtype=int)
    order = []
    for index in cf._blocks(shape, 4, 1):
        seen[index] += 1
        order.append(index)
    assert (seen == 1).all()
    # A big array is split into blocks of whole tiles along the outer axis.
    blocks = list(cf._blocks((4000, 2000), 4, 16))
    assert len(blocks) > 1 and all((b[0].stop - b[0].start) % 16 == 0 for b in blocks[:-1])


# ── Damaged files ────────────────────────────────────────────────────────────


def test_a_truncated_gzip_is_named_as_such(tmp_path, _cache):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    source.write_bytes(source.read_bytes()[:-100])
    with pytest.raises(cf.CompressedFitsError, match="x.fits.gz is truncated"):
        cf.materialize(source)
    assert not any(p.is_dir() for p in _cache.iterdir())   # nothing cached


def test_a_gzip_of_a_truncated_fits_is_refused_by_its_own_name(tmp_path):
    plain = _plain(tmp_path / "x.fits", _cube())
    short = tmp_path / "short.fits"
    short.write_bytes(plain.read_bytes()[:4000])
    source = _gzipped(tmp_path / "short.fits.gz", short)
    with pytest.raises(FitsTruncatedError, match="short.fits.gz"):
        cf.materialize(source)


def test_a_corrupt_fz_is_named_as_damaged(tmp_path, _cache):
    source = _fpacked(tmp_path / "bad.fz", _cube())
    with fits.open(source) as hdul:
        start = hdul.fileinfo(1)["datLoc"]
    raw = bytearray(source.read_bytes())
    raw[start:start + 16] = b"\x7f" * 16        # the tile descriptors
    source.write_bytes(bytes(raw))
    with pytest.raises(cf.CompressedFitsError, match="bad.fz cannot be decompressed"):
        cf.materialize(source)
    assert not any(p.is_dir() for p in _cache.iterdir())


def test_a_gzip_that_is_not_fits(tmp_path):
    source = tmp_path / "t.fits.gz"
    source.write_bytes(gzip.compress(b"a,b\n1,2\n" * 50))
    with pytest.raises(cf.CompressedFitsError, match="not a FITS file"):
        cf.materialize(source)


# ── The cache ────────────────────────────────────────────────────────────────


def test_second_open_hits_the_cache(tmp_path, monkeypatch):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    first = cf.materialize(source)
    calls = []
    monkeypatch.setattr(cf, "_gunzip", lambda *a, **k: calls.append(a))
    second = cf.materialize(source)
    assert second.cache_hit and second.path == first.path and not calls


def test_a_changed_source_is_decompressed_again(tmp_path):
    plain = _plain(tmp_path / "x.fits", _cube(seed=1))
    source = _gzipped(tmp_path / "x.fits.gz", plain)
    first = cf.materialize(source)
    _plain(tmp_path / "y.fits", _cube(seed=2))
    _gzipped(source, tmp_path / "y.fits")
    os.utime(source, ns=(source.stat().st_atime_ns, source.stat().st_mtime_ns + 10**9))
    second = cf.materialize(source)
    assert not second.cache_hit and second.path != first.path
    np.testing.assert_array_equal(_read(second.path), _cube(seed=2))


def test_concurrent_opens_decompress_once(tmp_path, monkeypatch):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    real, calls = cf._gunzip, []
    gate = threading.Event()

    def slow(*args, **kwargs):
        calls.append(1)
        gate.wait(5)
        return real(*args, **kwargs)

    monkeypatch.setattr(cf, "_gunzip", slow)
    results = []
    threads = [threading.Thread(target=lambda: results.append(cf.materialize(source)))
               for _ in range(3)]
    for t in threads:
        t.start()
    gate.set()
    for t in threads:
        t.join(10)
    assert len(calls) == 1 and len(results) == 3
    assert len({r.path for r in results}) == 1
    assert sorted(r.cache_hit for r in results) == [False, True, True]


def test_eviction_makes_room_but_spares_open_datasets(tmp_path, monkeypatch):
    a = _gzipped(tmp_path / "a.fits.gz", _plain(tmp_path / "a.fits", _cube(seed=1)))
    b = _gzipped(tmp_path / "b.fits.gz", _plain(tmp_path / "b.fits", _cube(seed=2)))
    c = _gzipped(tmp_path / "c.fits.gz", _plain(tmp_path / "c.fits", _cube(seed=3)))
    one = (tmp_path / "a.fits").stat().st_size
    monkeypatch.setenv("VISIVO_DECOMPRESS_CACHE_MAX_BYTES", str(int(one * 1.5) + 2000))
    monkeypatch.setattr(cf, "_EVICT_GRACE_SECONDS", 0.0)
    made_a = cf.materialize(a)
    made_b = cf.materialize(b)                  # a is evicted to make room
    assert not made_a.path.exists() and made_b.path.exists()
    # b is held by a session: c cannot evict it, and says why.
    monkeypatch.setattr(cf, "in_use_provider", lambda: {str(made_b.path)})
    with pytest.raises(cf.CompressedFitsError, match="VISIVO_DECOMPRESS_CACHE_MAX_BYTES"):
        cf.materialize(c)
    assert made_b.path.exists()


def test_a_recently_used_entry_is_not_evicted(tmp_path, monkeypatch):
    """Between a decompression and the session registering the dataset the
    copy is held by nobody; the grace period keeps it."""
    a = _gzipped(tmp_path / "a.fits.gz", _plain(tmp_path / "a.fits", _cube(seed=1)))
    b = _gzipped(tmp_path / "b.fits.gz", _plain(tmp_path / "b.fits", _cube(seed=2)))
    one = (tmp_path / "a.fits").stat().st_size
    monkeypatch.setenv("VISIVO_DECOMPRESS_CACHE_MAX_BYTES", str(int(one * 1.5) + 2000))
    made_a = cf.materialize(a)
    with pytest.raises(cf.CompressedFitsError, match="used in the last"):
        cf.materialize(b)
    assert made_a.path.exists()


def test_concurrent_decompressions_share_the_limit(tmp_path, monkeypatch):
    """Two decompressions admitted at once must not both count on the same
    free room: the second sees the first one's reservation."""
    files = [_gzipped(tmp_path / f"{n}.fits.gz", _plain(tmp_path / f"{n}.fits", _cube(seed=n)))
             for n in range(2)]
    one = (tmp_path / "0.fits").stat().st_size
    monkeypatch.setenv("VISIVO_DECOMPRESS_CACHE_MAX_BYTES", str(int(one * 1.5) + 2000))
    real, entered = cf._gunzip, threading.Barrier(2, timeout=2)

    def held(*args, **kwargs):
        with contextlib.suppress(threading.BrokenBarrierError):
            entered.wait()      # both writing at once, if both were admitted
        return real(*args, **kwargs)

    monkeypatch.setattr(cf, "_gunzip", held)
    outcomes = []

    def run(path):
        try:
            outcomes.append(cf.materialize(path))
        except cf.CompressedFitsError as exc:
            outcomes.append(exc)

    threads = [threading.Thread(target=run, args=(f,)) for f in files]
    for t in threads:
        t.start()
    for t in threads:
        t.join(10)
    made = [o for o in outcomes if isinstance(o, cf.Materialized)]
    assert len(made) == 1 and len(outcomes) == 2


def test_a_stop_after_the_last_chunk_publishes_nothing(tmp_path, _cache):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    stop = []
    with pytest.raises(cf.DecompressionCancelled):
        cf.materialize(source, progress=lambda f: stop.append(f >= 1.0),
                       should_stop=lambda: any(stop))
    assert not any(p.is_dir() for p in _cache.iterdir())


@pytest.mark.parametrize("links", [True, False], ids=["hard-links", "no-hard-links"])
def test_publishing_never_replaces_a_published_copy(tmp_path, monkeypatch, links):
    """Without a file lock (Windows) two processes may publish the same entry:
    the second keeps the first one's copy, which may be open already."""
    import errno
    monkeypatch.setattr(cf, "_HAVE_FILE_LOCK", False)
    if not links:
        def no_link(*_a):
            raise OSError(errno.EOPNOTSUPP, "no hard links")
        monkeypatch.setattr(cf.os, "link", no_link)
        if os.name != "nt":
            # POSIX rename replaces; emulate the Windows refusal.
            def rename(src, dst):
                if Path(dst).exists():
                    raise FileExistsError(dst)
                os.replace(src, dst)
            monkeypatch.setattr(cf.os, "rename", rename)
    target, partial = tmp_path / "t.fits", tmp_path / "t.partial"
    target.write_bytes(b"first")
    partial.write_bytes(b"second")
    cf._publish(partial, target)
    assert target.read_bytes() == b"first" and not partial.exists()
    # And a first publication does move the copy into place.
    fresh = tmp_path / "u.partial"
    fresh.write_bytes(b"x")
    cf._publish(fresh, tmp_path / "u.fits")
    assert (tmp_path / "u.fits").read_bytes() == b"x" and not fresh.exists()


def test_no_limit_means_no_eviction(tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_DECOMPRESS_CACHE_MAX_BYTES", "0")
    paths = [cf.materialize(_gzipped(tmp_path / f"{n}.fits.gz",
                                     _plain(tmp_path / f"{n}.fits", _cube(seed=n)))).path
             for n in range(3)]
    assert all(p.exists() for p in paths)


def test_not_enough_disk_is_refused_up_front(tmp_path, monkeypatch, _cache):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    monkeypatch.setattr(cf.shutil, "disk_usage",
                        lambda _p: shutil._ntuple_diskusage(10**12, 10**12 - 10, 10))
    with pytest.raises(cf.CompressedFitsError, match="VISIVO_DECOMPRESS_CACHE_DIR"):
        cf.materialize(source)
    assert not any(p.is_dir() for p in _cache.iterdir())


def test_a_cancelled_decompression_leaves_nothing(tmp_path, _cache):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    with pytest.raises(cf.DecompressionCancelled):
        cf.materialize(source, should_stop=lambda: True)
    assert not any(p.is_dir() for p in _cache.iterdir())
    assert not cf.materialize(source).cache_hit      # and the next open starts afresh


def test_progress_reaches_one(tmp_path):
    seen = []
    cf.materialize(_fpacked(tmp_path / "x.fz", _cube()), progress=seen.append)
    assert seen and seen[-1] == 1.0 and seen == sorted(seen)


# ── Through the API ──────────────────────────────────────────────────────────


async def _open(client: AsyncClient, headers: dict, path: Path) -> dict:
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=headers)
    assert resp.status_code == 200, resp.text
    return resp.json()


async def _answers(client: AsyncClient, auth_headers: dict, path: Path) -> dict:
    """What the viewers ask of a cube: preview, a slice, a moment, a spectrum."""
    opened = await _open(client, auth_headers, path)
    assert opened["valid"], opened["error"]
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    ds = opened["dataset_id"]
    out = {"kind": opened["kind"], "shape": (opened["width"], opened["height"], opened["depth"]),
           "ctype": opened["ctype"], "bunit": opened["bunit"], "dataset_id": ds,
           "headers": headers}
    for name, route, body in (
            ("preview", "/v1/cube/preview", {"downsample": 1}),
            ("slice", "/v1/cube/slice", {"axis": "z", "index": 2}),
            ("moment", "/v1/products/moment",
             {"moment_order": 0, "channel_start": 1, "channel_end": 4,
              "mask_enabled": False, "threshold_value": 0.0}),
            ("spectrum", "/v1/spectral/probe", {"x": 5, "y": 4})):
        resp = await client.post(route, json={"dataset_id": ds, **body}, headers=headers)
        assert resp.status_code == 200, (name, resp.text)
        answer = resp.json()
        if isinstance(answer, dict):
            for volatile in ("dataset_id", "elapsed_ms", "cache_hit", "duration_ms"):
                answer.pop(volatile, None)
        out[name] = answer
    return out


@pytest.mark.asyncio
@pytest.mark.parametrize("compress", ["gzip", "GZIP_2"])
async def test_compressed_cube_answers_as_the_original(
        client: AsyncClient, auth_headers: dict, tmp_path, compress):
    data = _cube(shape=(8, 10, 12))
    plain = _plain(tmp_path / "cube.fits", data)
    source = (_gzipped(tmp_path / "cube.fits.gz", plain) if compress == "gzip"
              else _fpacked(tmp_path / "cube.fits.fz", data, compress, quantize_level=0.0))
    expected = await _answers(client, auth_headers, plain)
    got = await _answers(client, auth_headers, source)
    for key in ("kind", "shape", "ctype", "bunit", "preview", "slice", "moment", "spectrum"):
        assert got[key] == expected[key], key
    # The session keeps the file the user opened; the readers read the copy.
    from app.sessions import REGISTRY
    session = REGISTRY.peek(got["headers"]["X-Visivo-Session"])
    entry = session.datasets[got["dataset_id"]]
    assert entry["source_path"] == str(source)
    assert cf.source_of(entry["path"]) == str(source)
    resp = await client.get("/v1/reproducibility/provenance", headers=got["headers"])
    entity = resp.json()["prov"]["entity"][f"visivo:{got['dataset_id']}"]
    assert entity["visivo:path"] == str(source)


@pytest.mark.asyncio
async def test_rice_image_opens(client: AsyncClient, auth_headers: dict, tmp_path):
    data = _cube()[0]
    source = _fpacked(tmp_path / "map.fz", data)
    opened = await _open(client, auth_headers, source)
    assert opened["valid"], opened["error"]
    assert (opened["kind"], opened["width"], opened["height"]) == ("image", 12, 10)
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    resp = await client.post("/v1/image/preview",
                             json={"dataset_id": opened["dataset_id"]}, headers=headers)
    assert resp.json()["valid"]


@pytest.mark.asyncio
async def test_truncated_gzip_is_refused_with_its_reason(
        client: AsyncClient, auth_headers: dict, tmp_path):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    source.write_bytes(source.read_bytes()[:-100])
    opened = await _open(client, auth_headers, source)
    assert not opened["valid"]
    assert "truncated" in opened["error"] and "Empty or corrupt" not in opened["error"]


@pytest.mark.asyncio
async def test_a_deleted_copy_is_made_again(client: AsyncClient, auth_headers: dict, tmp_path):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    got = await _answers(client, auth_headers, source)
    from app.sessions import REGISTRY
    entry = REGISTRY.peek(got["headers"]["X-Visivo-Session"]).datasets[got["dataset_id"]]
    shutil.rmtree(Path(entry["path"]).parent)
    body = {"dataset_id": got["dataset_id"], "axis": "z", "index": 2}
    # The event loop does not decompress: the request is told to come back,
    # and the copy is made again in the background, at the same path.
    resp = await client.post("/v1/cube/slice", json=body, headers=got["headers"])
    assert resp.status_code == 503 and resp.headers["Retry-After"]
    assert "decompressed again" in resp.json()["detail"]
    for _ in range(250):
        if Path(entry["path"]).exists():
            break
        await asyncio.sleep(0.02)
    resp = await client.post("/v1/cube/slice", json=body, headers=got["headers"])
    assert resp.status_code == 200
    assert resp.json()["data_base64"] == got["slice"]["data_base64"]


def test_a_lost_copy_follows_a_touched_source(tmp_path):
    """Off the event loop the copy is made again inline; a source whose time
    changed meanwhile has a new cache key, and the dataset follows it."""
    from app.dependencies import _ensure_decompressed, _fits_metadata
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    _kind, geometry = _fits_metadata(source)
    entry = {"path": geometry["resolved_path"], "source_path": str(source)}
    shutil.rmtree(Path(entry["path"]).parent)
    old = source.stat()
    os.utime(source, ns=(old.st_atime_ns, old.st_mtime_ns + 10**9))
    _ensure_decompressed(entry)
    assert Path(entry["path"]).exists() and entry["path"] != geometry["resolved_path"]
    np.testing.assert_array_equal(_read(Path(entry["path"])), _cube())


@pytest.mark.asyncio
async def test_decompress_task_then_open_hits_the_cache(
        client: AsyncClient, auth_headers: dict, tmp_path):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    resp = await client.post("/v1/tasks/decompress", json={"path": str(source)},
                             headers=auth_headers)
    assert resp.status_code == 200 and resp.json()["valid"]
    task_id = resp.json()["task_id"]
    for _ in range(200):
        status = (await client.get(f"/v1/tasks/{task_id}", headers=auth_headers)).json()
        if status["status"] not in ("pending", "running"):
            break
        await asyncio.sleep(0.02)
    assert status["status"] == "completed", status
    result = status["result"]
    assert result["compression"] == "gzip" and not result["cache_hit"]
    assert result["decompressed_bytes"] == (tmp_path / "x.fits").stat().st_size
    made = cf.materialize(source)
    assert made.cache_hit


@pytest.mark.asyncio
async def test_decompress_task_refuses_a_missing_file(
        client: AsyncClient, auth_headers: dict, tmp_path):
    resp = await client.post("/v1/tasks/decompress",
                             json={"path": str(tmp_path / "nope.fits.gz")}, headers=auth_headers)
    assert resp.status_code == 404


@pytest.mark.asyncio
async def test_decompress_task_can_be_cancelled(
        client: AsyncClient, auth_headers: dict, tmp_path, monkeypatch, _cache):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    started, release = threading.Event(), threading.Event()
    real = cf._gunzip

    def held(src, target, estimate, progress, should_stop):
        started.set()
        release.wait(5)
        return real(src, target, estimate, progress, should_stop)

    monkeypatch.setattr(cf, "_gunzip", held)
    resp = await client.post("/v1/tasks/decompress", json={"path": str(source)},
                             headers=auth_headers)
    task_id = resp.json()["task_id"]
    await asyncio.to_thread(started.wait, 5)
    resp = await client.delete(f"/v1/tasks/{task_id}", headers=auth_headers)
    assert resp.status_code == 200
    release.set()
    for _ in range(200):
        if not any(p.is_dir() for p in _cache.iterdir()):
            break
        await asyncio.sleep(0.02)
    status = (await client.get(f"/v1/tasks/{task_id}", headers=auth_headers)).json()
    assert status["status"] == "cancelled"
    assert not any(p.is_dir() for p in _cache.iterdir())
    # The Data Hub's "Last 5 jobs" lists it once its work has ended.
    from app.tasks import TASKS
    for _ in range(200):
        recent = TASKS.snapshot()["recent_tasks"]
        if any(r["operation"] == "decompress" and r["status"] == "cancelled" for r in recent):
            break
        await asyncio.sleep(0.02)
    else:
        raise AssertionError(f"cancelled decompression not in recent tasks: {recent}")


@pytest.mark.asyncio
async def test_compressed_names_are_listed_as_fits(
        client: AsyncClient, auth_headers: dict, tmp_path):
    _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    _fpacked(tmp_path / "y.fz", _cube())
    resp = await client.get("/v1/files/list", params={"path": str(tmp_path)}, headers=auth_headers)
    flags = {e["name"]: e["is_fits"] for e in resp.json()["entries"]}
    assert flags["x.fits.gz"] and flags["y.fz"] and flags["x.fits"]


@pytest.mark.asyncio
async def test_a_cached_file_is_answered_completed_at_once(
        client: AsyncClient, auth_headers: dict, tmp_path):
    """A reopen must cost what a plain open does: the create response itself
    says the copy is there, so the client follows no task."""
    plain = _plain(tmp_path / "x.fits", _cube())
    source = _gzipped(tmp_path / "x.fits.gz", plain)
    cf.materialize(source)
    resp = await client.post("/v1/tasks/decompress", json={"path": str(source)},
                             headers=auth_headers)
    body = resp.json()
    assert body["valid"] and body["status"] == "completed" and body["cache_hit"]
    status = (await client.get(f"/v1/tasks/{body['task_id']}", headers=auth_headers)).json()
    assert status["status"] == "completed" and status["result"]["cache_hit"]
    assert status["result"]["decompressed_bytes"] == plain.stat().st_size
    # Not compressed at all: completed at once too, and no cache hit.
    resp = await client.post("/v1/tasks/decompress", json={"path": str(plain)},
                             headers=auth_headers)
    body = resp.json()
    assert body["status"] == "completed" and not body["cache_hit"]


@pytest.mark.asyncio
async def test_the_header_preview_of_a_gzip_is_the_plain_files(
        client: AsyncClient, auth_headers: dict, tmp_path, monkeypatch):
    """The file browser's header preview waits for this route: on a .fits.gz
    it reads the headers off the stream (astropy decompressed all of it,
    4.5 s on a 2 GB cube), and answers what it answers for the plain file."""
    layouts = _hdul_layouts(tmp_path)
    for name in ("primary cube", "empty primary"):
        plain = layouts[name]
        source = _gzipped(tmp_path / f"{plain.name}.gz", plain)
        got, expected = [
            (await client.post("/v1/files/header", json={"path": str(p)},
                               headers=auth_headers)).json()
            for p in (source, plain)]
        assert got["valid"] and got["cards"] == expected["cards"], name

    # Not through astropy: a gzip stream read whole would fail this.
    from app.routers import files as files_router
    calls: list[str] = []
    real = files_router.gzip_image_headers
    monkeypatch.setattr(files_router, "gzip_image_headers",
                        lambda p: calls.append(str(p)) or real(p))
    source = tmp_path / "a.fits.gz"
    resp = await client.post("/v1/files/header", json={"path": str(source)}, headers=auth_headers)
    assert resp.json()["valid"] and calls == [str(source)]


def test_cached_copy_never_decompresses(tmp_path, _cache):
    source = _gzipped(tmp_path / "x.fits.gz", _plain(tmp_path / "x.fits", _cube()))
    assert cf.cached_copy(source) is None
    assert not (_cache.exists() and any(p.is_dir() for p in _cache.iterdir()))
    made = cf.materialize(source)
    hit = cf.cached_copy(source)
    assert hit is not None and hit.cache_hit and hit.path == made.path
    assert cf.cached_copy(tmp_path / "x.fits") is None


def _hdul_layouts(tmp_path: Path) -> dict[str, Path]:
    """Plain FITS of the layouts the image HDU is chosen among."""
    cube = _cube()
    out = {"primary cube": _plain(tmp_path / "a.fits", cube)}
    path = tmp_path / "b.fits"     # ESO phase-3: empty primary, cube in DATA_EXT
    fits.HDUList([fits.PrimaryHDU(), fits.ImageHDU(cube, name="DATA_EXT"),
                  fits.ImageHDU(cube, name="STAT_EXT")]).writeto(path)
    out["empty primary"] = path
    path = tmp_path / "c.fits"     # a 1-D primary, then a table, then the image
    table = fits.BinTableHDU.from_columns([fits.Column("a", "E", array=np.arange(5.0))])
    fits.HDUList([fits.PrimaryHDU(np.arange(4.0)), table, fits.ImageHDU(cube[0])]).writeto(path)
    out["after a table"] = path
    path = tmp_path / "d.fits"     # no image at all: a catalogue
    fits.HDUList([fits.PrimaryHDU(), table]).writeto(path)
    out["table only"] = path
    return out


def test_gzip_headers_pick_the_image_astropy_picks(tmp_path):
    """Read off the gzip stream, without astropy decompressing the whole of it:
    the same image HDU, the same header, as astropy on the plain file."""
    from app.fits_dataset import image_hdu_index
    for name, plain in _hdul_layouts(tmp_path).items():
        source = _gzipped(tmp_path / f"{plain.name}.gz", plain)
        hdus = cf.gzip_image_headers(source)
        assert hdus is not None, name
        with fits.open(plain) as hdul:
            index = image_hdu_index(hdul)
            assert image_hdu_index(hdus) == index, name
            assert hdus[index].header == hdul[index].header, name
            if not cf._has_two_long_axes(hdul[index].header):
                assert len(hdus) == len(hdul), name   # no image: read to the end


def test_gzip_headers_decline_what_they_cannot_read(tmp_path):
    fpacked = _fpacked(tmp_path / "x.fits.fz", _cube())
    assert cf.gzip_image_headers(_gzipped(tmp_path / "x.fits.fz.gz", fpacked)) is None
    (tmp_path / "t.csv.gz").write_bytes(gzip.compress(b"a,b\n1,2\n"))
    assert cf.gzip_image_headers(tmp_path / "t.csv.gz") is None
    cut = _gzipped(tmp_path / "y.fits.gz", _plain(tmp_path / "y.fits", _cube()))
    cut.write_bytes(cut.read_bytes()[:40])
    assert cf.gzip_image_headers(cut) is None


def test_the_gzip_estimate_does_not_decompress_the_stream(tmp_path, monkeypatch):
    """The size estimate reads the trailer and the headers, never the data:
    through astropy it cost a whole decompression of a 2 GB cube (4.6 s)."""
    plain = _plain(tmp_path / "x.fits", _cube(shape=(40, 64, 64)))
    source = _gzipped(tmp_path / "x.fits.gz", plain)
    read: list[int] = []
    real_open = gzip.open

    def counting_open(*args, **kwargs):
        stream = real_open(*args, **kwargs)
        real_read = stream.read

        def tracked(n=-1):
            data = real_read(n)
            read.append(len(data))
            return data

        stream.read = tracked
        return stream

    monkeypatch.setattr(gzip, "open", counting_open)
    assert cf._estimate(source, "gzip") == plain.stat().st_size
    assert 0 < sum(read) < 3 * 2880
