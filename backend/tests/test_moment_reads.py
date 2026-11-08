"""Faster out-of-core moments: explicit reads (issue #116), the thread count
from the node (issue #117), and Export Moment as FITS on the moment
product's path (issue #119).

The rule throughout: the maps do not change. A block read by explicit reads
equals the same slice of the memory-mapped data value for value, whatever the
file stores (big-endian floats, scaled integers, BLANK, unsigned integers, a
VBT field), and the maps computed from it are bit-identical.
"""
from __future__ import annotations

import base64
import gzip
import time

import numpy as np
import pytest
from astropy.io import fits
from httpx import AsyncClient


def _decode(b64: str, shape) -> np.ndarray:
    return np.frombuffer(base64.b64decode(b64), dtype=np.float32).reshape(shape)


def _spectral(h) -> None:
    h["CTYPE1"], h["CTYPE2"], h["CTYPE3"] = "RA---SIN", "DEC--SIN", "FREQ"
    h["CRVAL3"], h["CRPIX3"], h["CDELT3"], h["CUNIT3"] = 1.4e9, 1.0, 1.0e5, "Hz"


def _cube(nz=12, ny=10, nx=14, seed=3) -> np.ndarray:
    rng = np.random.default_rng(seed)
    data = rng.normal(0.0, 1.0, (nz, ny, nx)).astype(np.float32)
    data[nz // 2 - 2:nz // 2 + 2, 2:7, 3:9] += 6.0
    data[1, 0, 0] = np.nan
    return data


def _fresh() -> None:
    """Forget the opened files: each test sees its own."""
    from app.compute import moment as m

    m._RAW_CUBES.clear()
    for fh, hdul, _ in list(m._MAPPED_CUBES.values()):
        hdul.close()
        fh.close()
    m._MAPPED_CUBES.clear()


def _hdus(tmp_path):
    """FITS files of every layout the memory-mapped path reads, each with the
    3-D array astropy gives for it (squeezed as the backend squeezes)."""
    from app.fits_dataset import ScientificFitsDataset

    base = _cube()
    ints = np.round(np.nan_to_num(base) * 1000).astype(np.int16)
    cases = {}

    def add(name, hdul, index=0):
        p = tmp_path / f"{name}.fits"
        hdul.writeto(p, overwrite=True)
        # astropy's values; without memmap=True, which astropy 8 refuses
        # for a scaled image (the memory-mapped path cannot read those).
        with fits.open(p) as h:
            want = np.array(ScientificFitsDataset.squeeze_to_3d(h[index].data))
        cases[name] = (p, index, want)

    add("f32", fits.HDUList([fits.PrimaryHDU(base)]))
    add("f64", fits.HDUList([fits.PrimaryHDU(base.astype(np.float64))]))
    blank_float = fits.PrimaryHDU(base)
    blank_float.header["BLANK"] = -32768        # ignored on float data, as by astropy
    add("f32_blank_ignored", fits.HDUList([blank_float]))
    add("i32_plain", fits.HDUList([fits.PrimaryHDU(ints.astype(np.int32))]))
    scaled = fits.PrimaryHDU(ints)
    scaled.header["BSCALE"], scaled.header["BZERO"] = 0.001, 0.25
    add("i16_scaled", fits.HDUList([scaled]))
    blank = ints.copy()
    blank[3, 2, 2] = -32768
    with_blank = fits.PrimaryHDU(blank)
    with_blank.header["BSCALE"], with_blank.header["BLANK"] = 0.001, -32768
    add("i16_blank", fits.HDUList([with_blank]))
    add("u16", fits.HDUList([fits.PrimaryHDU((ints.astype(np.int32) + 40000).astype(np.uint16))]))
    u8 = fits.PrimaryHDU((ints % 200).astype(np.uint8))
    u8.header["BSCALE"], u8.header["BZERO"] = 0.5, -3.0
    add("u8_scaled", fits.HDUList([u8]))
    add("stokes_degenerate", fits.HDUList([fits.PrimaryHDU(base[None])]))
    add("stokes_two", fits.HDUList([fits.PrimaryHDU(np.stack([base, base * 2]))]))
    add("extension", fits.HDUList([fits.PrimaryHDU(), fits.ImageHDU(base, name="SKY")]), 1)
    for _name, (p, _i, _w) in cases.items():
        with fits.open(p, mode="update") as h:
            _spectral(h[_i].header)
    return cases


def test_explicit_reads_equal_the_memory_map_for_every_layout(tmp_path) -> None:
    """Every key dask asks for (whole planes, bands of rows, a part of the
    columns) gives the values of the memory-mapped array, in its type."""
    from app.compute import moment as m

    for name, (p, index, want) in _hdus(tmp_path).items():
        _fresh()
        src = m._FitsCubeSource(str(p), index, 2, (8,) + want.shape[1:], want.dtype)
        assert m._raw_cube(src.path, index, src.signature) is not None, name
        for key in [(slice(0, 3), slice(None), slice(None)),
                    (slice(3, 8), slice(2, 7), slice(0, want.shape[2])),
                    (slice(5, 6), slice(9, 10), slice(4, 11)),
                    (slice(0, 0), slice(0, 0), slice(0, 0))]:
            got = src[key]
            ref = want[2:10][key]
            assert got.dtype == ref.dtype.newbyteorder("="), name
            assert got.dtype.isnative, name
            np.testing.assert_array_equal(got, ref, err_msg=f"{name} {key}")


@pytest.mark.parametrize("endian", ["little", "big"])
def test_explicit_reads_of_a_vbt_field(tmp_path, endian) -> None:
    from app.compute import moment as m
    from app.volume_source import REF_SEP

    fields = [_cube(seed=1), _cube(seed=2)]
    code = ("<" if endian == "little" else ">") + "f4"
    data = tmp_path / "v.bin"
    with open(data, "wb") as fh:
        for f in fields:
            f.astype(code).tofile(fh)
    nz, ny, nx = fields[0].shape
    (tmp_path / "v.bin.head").write_text(
        f"float\n2\n{nx * ny * nz} {nx} {ny} {nz} 1 1 1\n{endian}\nA\nB\n")
    _fresh()
    ref = f"{data}{REF_SEP}1"
    src = m._FitsCubeSource(ref, 1, 0, fields[1].shape, np.dtype(code))
    assert m._raw_cube(ref, 1, src.signature) is not None
    np.testing.assert_array_equal(src[0:nz, 3:6, :], fields[1][:, 3:6, :])


def test_a_compressed_file_is_read_through_the_memory_map(tmp_path) -> None:
    """A gzip file cannot be read at offsets: the source falls back."""
    from app.compute import moment as m

    data = _cube()
    plain = tmp_path / "c.fits"
    fits.PrimaryHDU(data).writeto(plain)
    gz = tmp_path / "c.fits.gz"
    gz.write_bytes(gzip.compress(plain.read_bytes()))
    _fresh()
    src = m._FitsCubeSource(str(gz), 0, 0, data.shape, data.dtype)
    assert m._raw_cube(str(gz), 0, src.signature) is None
    np.testing.assert_array_equal(src[0:4, :, :], data[0:4])


def test_explicit_reads_refuse_a_changed_file(tmp_path) -> None:
    import os

    from app.compute import moment as m

    p = tmp_path / "c.fits"
    fits.PrimaryHDU(np.zeros((4, 3, 3), np.float32)).writeto(p)
    _fresh()
    src = m._FitsCubeSource(str(p), 0, 0, (4, 3, 3), np.dtype(">f4"))
    assert m._raw_cube(str(p), 0, src.signature) is not None
    time.sleep(0.01)
    fits.PrimaryHDU(np.ones((4, 3, 3), np.float32)).writeto(tmp_path / "n.fits")
    os.replace(tmp_path / "n.fits", p)
    with pytest.raises(RuntimeError, match="changed"):
        src[2:4]


def test_a_truncated_file_is_left_to_the_memory_map(tmp_path) -> None:
    from app.compute import moment as m

    p = tmp_path / "c.fits"
    fits.PrimaryHDU(_cube()).writeto(p)
    with open(p, "r+b") as fh:
        fh.truncate(p.stat().st_size - 3000)
    _fresh()
    st = p.stat()
    assert m._raw_cube(str(p), 0, (st.st_size, st.st_mtime_ns)) is None


@pytest.mark.parametrize("order", [0, 1, 2, 6])
@pytest.mark.parametrize("budget", [1 << 30, 3 * 14 * 4, 1])
def test_dask_maps_are_bit_identical_to_the_memory_map(tmp_path, monkeypatch, order, budget) -> None:
    """Whole planes, bands of rows and one channel per block: the maps from
    explicit reads equal, bit for bit, the maps from the memory map."""
    from app.compute import moment as m

    monkeypatch.setattr(m, "_MOMENT_CHUNK_BYTES", budget)
    real = m._raw_cube
    explicit = {"on": True}
    monkeypatch.setattr(m, "_raw_cube", lambda *a: real(*a) if explicit["on"] else None)
    cases = _hdus(tmp_path)
    for name in ("f32", "i32_plain", "extension"):
        p = cases[name][0]
        _fresh()
        explicit["on"] = True
        fast = m._moment_map_dask(str(p), order, 1, 10, 0.5, True, 64, None)
        _fresh()
        explicit["on"] = False
        slow = m._moment_map_dask(str(p), order, 1, 10, 0.5, True, 64, None)
        shape = (fast["height"], fast["width"])
        a, b = _decode(fast["data_base64"], shape), _decode(slow["data_base64"], shape)
        assert a.tobytes() == b.tobytes(), f"{name} M{order}"


@pytest.mark.parametrize("order", [0, 1, 2])
def test_streamed_maps_with_read_ahead_are_bit_identical(tmp_path, monkeypatch, order) -> None:
    """The streamer with explicit reads and read-ahead equals the streamer
    slicing the memory map in its own thread, bit for bit."""
    from app.compute import moment as m
    from app.compute import worker_moment

    p = _hdus(tmp_path)["stokes_degenerate"][0]
    monkeypatch.setattr(m, "_moment_stream_bytes", lambda: 0)          # always stream
    monkeypatch.setattr(m, "_MOMENT_CHUNK_BYTES", 3 * 10 * 14 * 4)     # 3-channel slabs
    _fresh()
    fast = worker_moment(str(p), order, 0, 11, True, 0.5)
    monkeypatch.setattr(m, "_cube_reader", lambda *a: None)
    monkeypatch.setattr(m, "_MOMENT_READ_AHEAD", 0)
    slow = worker_moment(str(p), order, 0, 11, True, 0.5)
    assert fast["data_base64"] == slow["data_base64"]


def test_read_ahead_reads_nothing_after_an_abort(tmp_path, monkeypatch) -> None:
    """Reads run ahead, one at a time: a read that starts after the stop file
    appeared is refused, so a cancelled task reads at most one block more."""
    from app.compute import moment as m
    from app.compute.abort import ComputationAborted

    cube = np.ones((16, 4, 4), np.float32)
    stop = tmp_path / "stop"
    reads = []

    def reader(za, zb):
        reads.append(za)
        if len(reads) == 2:
            stop.touch()
        return cube[za:zb]

    monkeypatch.setattr(m, "_MOMENT_CHUNK_BYTES", 2 * 16 * 4)   # two channels a slab: 8 slabs
    monkeypatch.setattr(m, "_MOMENT_READ_AHEAD", 3)
    with pytest.raises(ComputationAborted):
        m._moment_map_streaming(cube, 0, 15, 0, np.arange(16.0), False, 0.0,
                                str(stop), reader)
    assert reads == [0, 2], reads


def test_thread_choice_explicit_override(monkeypatch) -> None:
    from app.compute import moment as m

    monkeypatch.setattr(m, "_MOMENT_THREADS", 7)
    assert m._moment_threads_choice() == (7, "VISIVO_MOMENT_THREADS")


def test_thread_choice_follows_cpus_and_memory(monkeypatch) -> None:
    """The granted CPUs shared among the heavy slots, then the memory budget
    at eight blocks per thread, then the ceiling."""
    from app import node_status
    from app.compute import moment as m

    monkeypatch.setattr(m, "_MOMENT_THREADS", 0)
    monkeypatch.setattr(m, "_MOMENT_CHUNK_BYTES", 64 * 1024 ** 2)
    monkeypatch.setenv("VISIVO_HEAVY_SLOTS", "1")
    monkeypatch.setattr(node_status, "granted_memory_bytes", lambda: None)
    budget = {"value": 1 << 40}
    monkeypatch.setattr(m, "_moment_memory_budget", lambda: (budget["value"], "test"))

    monkeypatch.setattr(node_status, "granted_cpus", lambda: 8.0)
    assert m._moment_threads_choice()[0] == 8                      # CPUs bind
    monkeypatch.setattr(node_status, "granted_cpus", lambda: 128.0)
    assert m._moment_threads_choice()[0] == m._MOMENT_THREADS_MAX  # the ceiling
    budget["value"] = 16 * 10 ** 9                                 # the #3 cap
    n, why = m._moment_threads_choice()
    assert 12 <= n <= 18 and "memory" in why                       # 16 fitted, 32 did not
    # A 6082² plane (the 450 GB cube) leaves less for the threads.
    assert m._moment_threads_choice(6082 * 6082)[0] < n
    budget["value"] = 1 << 20
    assert m._moment_threads_choice()[0] == 1                      # never fewer than one
    monkeypatch.setenv("VISIVO_HEAVY_SLOTS", "4")
    budget["value"] = 1 << 40
    monkeypatch.setattr(node_status, "granted_cpus", lambda: 18.0)
    assert m._moment_threads_choice()[0] == 4                      # 18 CPUs over 4 slots


def test_thread_choice_without_a_known_budget(monkeypatch) -> None:
    from app import node_status
    from app.compute import moment as m

    monkeypatch.setattr(m, "_MOMENT_THREADS", 0)
    monkeypatch.setenv("VISIVO_HEAVY_SLOTS", "1")
    monkeypatch.setattr(node_status, "granted_cpus", lambda: 128.0)
    monkeypatch.setattr(m, "_moment_memory_budget", lambda: (None, "memory unknown"))
    n, why = m._moment_threads_choice()
    assert n == m._MOMENT_THREADS_MAX and "memory unknown" in why


def test_no_headroom_is_a_budget(monkeypatch) -> None:
    """Zero bytes available limits the threads; it is not 'unknown'."""
    import resource

    from app import node_status
    from app.compute import moment as m

    monkeypatch.setenv("VISIVO_HEAVY_SLOTS", "1")
    monkeypatch.setattr(resource, "getrlimit", lambda what: (resource.RLIM_INFINITY,) * 2)
    monkeypatch.setattr(node_status, "granted_memory_bytes", lambda: 64 * 2 ** 30)
    monkeypatch.setattr(node_status, "available_memory_bytes", lambda: 0)
    assert m._moment_memory_budget()[0] == 0
    monkeypatch.setattr(m, "_MOMENT_THREADS", 0)
    monkeypatch.setattr(node_status, "granted_cpus", lambda: 64.0)
    assert m._moment_threads_choice()[0] == 1


def test_memory_budget_takes_the_tightest_limit(monkeypatch) -> None:
    import resource

    from app import node_status
    from app.compute import moment as m

    monkeypatch.setenv("VISIVO_HEAVY_SLOTS", "2")
    monkeypatch.setattr(resource, "getrlimit", lambda what: (16 * 10 ** 9, resource.RLIM_INFINITY))
    monkeypatch.setattr(node_status, "granted_memory_bytes", lambda: 64 * 2 ** 30)
    monkeypatch.setattr(node_status, "available_memory_bytes", lambda: 300 * 10 ** 9)
    assert m._moment_memory_budget() == (16 * 10 ** 9, "RLIMIT_DATA")
    monkeypatch.setattr(resource, "getrlimit", lambda what: (resource.RLIM_INFINITY,) * 2)
    value, why = m._moment_memory_budget()
    assert value == 32 * 2 ** 30 and why.startswith("SLURM grant")
    monkeypatch.setattr(node_status, "granted_memory_bytes", lambda: None)
    monkeypatch.setattr(node_status, "available_memory_bytes", lambda: None)
    monkeypatch.setattr(node_status, "memory_limit_bytes", lambda: 64 * 10 ** 9)
    assert m._moment_memory_budget() == (32 * 10 ** 9, "host RAM / 2 heavy slots")


# ── Export Moment as FITS on the moment product's path (issue #119) ──────────

@pytest.mark.asyncio
@pytest.mark.parametrize("dask", [False, True])
@pytest.mark.parametrize("order", [0, 1, 2])
async def test_moment_fits_equals_the_moment_product_bit_for_bit(
        client: AsyncClient, opened_cube: dict, monkeypatch, dask, order) -> None:
    """The FITS export and /v1/products/moment, for the same request, hold the
    same map bit for bit; each is computed afresh (the cache is emptied in
    between), on the in-process path and on the chunked dask path."""
    from app import dependencies
    from app.product_cache import PRODUCT_CACHE

    if dask:
        monkeypatch.setattr(dependencies, "_DASK_MODE", "local")
    else:
        monkeypatch.setattr(dependencies, "_DASK_MODE", "off")
    body = {"dataset_id": opened_cube["dataset_id"], "moment_order": order,
            "channel_start": 2, "channel_end": 13, "mask_enabled": True,
            "threshold_value": 0.05}
    monkeypatch.setattr(PRODUCT_CACHE, "get", lambda key: None)        # computed afresh
    exported = (await client.post("/v1/products/moment_fits",
                                  json={**body, "output_basename": f"bit_{order}_{dask}.fits"},
                                  headers=opened_cube["headers"])).json()
    assert exported["valid"], exported["error"]
    product = (await client.post("/v1/products/moment", json=body,
                                 headers=opened_cube["headers"])).json()
    assert product["valid"], product["error"]
    shape = (product["height"], product["width"])
    with fits.open(exported["output_path"]) as hdul:
        written = np.asarray(hdul[0].data, dtype=np.float32)
    assert written.tobytes() == _decode(product["data_base64"], shape).tobytes()
    assert exported["moment_unit"] == product["moment_unit"]
