"""Whole-cube statistics in bounded memory (issue #333): an exact pass read
slab by slab with positional reads (FITS, scaled FITS, VBT fields, PLUTO
variables); above the full-resolution threshold the pyramid's extremes and
mean with a sample, or a sample alone, within a stated tolerance of the exact
numbers; and the peak resident memory of the route's worker on a cube larger
than its bound, measured in a subprocess."""
from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import textwrap
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import cube_stats as cs
from app.brick_pyramid import build_pyramid, default_pyramid_path

BACKEND = Path(__file__).resolve().parents[1]


@pytest.fixture(autouse=True)
def _pyramid_dir(tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))


@pytest.fixture
def large(monkeypatch):
    """Every cube is above the full-resolution threshold (a preview_only one)."""
    monkeypatch.setenv("VISIVO_FULL_RES_THRESHOLD_BYTES", "1")


def _field(shape=(40, 48, 64), seed=0, blanks=False) -> np.ndarray:
    """A noise field on a smooth gradient with one bright source."""
    rng = np.random.default_rng(seed)
    z, y, x = np.indices(shape, dtype=np.float64)
    a = 10.0 + 0.05 * z + 0.02 * x + rng.normal(0.0, 1.0, shape)
    a[10:20, 10:16, 30:40] += 8.0
    if blanks:
        a[:, :6, :9] = np.nan
        a[5, 20, 33] = np.nan
    return a.astype(np.float32)


def _reference(values: np.ndarray) -> dict:
    v = values.astype(np.float64).ravel()
    f = v[np.isfinite(v)]
    med = float(np.median(f))
    return {"valid_count": int(f.size), "blanked_count": int(v.size - f.size),
            "min": float(f.min()), "max": float(f.max()), "mean": float(f.mean()),
            "std": float(f.std()), "rms": float(np.sqrt(np.mean(f * f))),
            "sum": float(f.sum()), "median": med,
            "mad_sigma": 1.4826 * float(np.median(np.abs(f - med)))}


def _assert_exact(got: dict, ref: dict, median_exact: bool = True) -> None:
    assert got["source"] == "exact"
    assert got["valid_count"] == ref["valid_count"]
    assert got["blanked_count"] == ref["blanked_count"]
    for k in ("min", "max", "mean", "std", "rms", "sum"):
        assert got[k] == pytest.approx(ref[k], rel=1e-9, abs=1e-9), k
    # The median is of every finite voxel of a cube within SAMPLE_TARGET,
    # else of a strided sample of them.
    for k in ("median", "mad_sigma"):
        assert got[k] == pytest.approx(ref[k], rel=1e-9 if median_exact else 0.01), k
    assert got["median_exact"] is median_exact
    assert ("median" in got["exact_fields"]) is median_exact


def _write_pluto():
    gen = BACKEND / "scripts" / "generate_demo_data.py"
    spec = importlib.util.spec_from_file_location("generate_demo_data", gen)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.write_pluto


def _write_vbt(tmp_path: Path, fields: list[np.ndarray]) -> Path:
    nz, ny, nx = fields[0].shape
    data = tmp_path / "v.bin"
    with open(data, "wb") as fh:
        for f in fields:
            f.astype(">f4").tofile(fh)
    (tmp_path / "v.bin.head").write_text("\n".join(
        ["float", str(len(fields)), f"{nx * ny * nz} {nx} {ny} {nz} 1 1 1", "big",
         *[f"F{i}" for i in range(len(fields))]]) + "\n")
    return data


# ── The exact pass ──────────────────────────────────────────────────────────

def test_exact_pass_of_a_float_cube_with_blanks(tmp_path):
    data = _field(blanks=True)
    path = tmp_path / "c.fits"
    fits.PrimaryHDU(data).writeto(path)
    _assert_exact(cs.cube_statistics(str(path)), _reference(data))


def test_exact_pass_reads_in_slabs_and_bands(tmp_path, monkeypatch):
    """Slabs of several planes, and bands of rows when a plane alone is
    larger than the budget, give the same numbers."""
    data = _field(blanks=True)
    path = tmp_path / "c.fits"
    fits.PrimaryHDU(data).writeto(path)
    ref = _reference(data)
    for budget in (48 * 64 * 8 * 3, 64 * 8 * 5):
        monkeypatch.setattr(cs, "SLAB_BYTES", budget)
        _assert_exact(cs.cube_statistics(str(path)), ref)


@pytest.mark.parametrize("kind", ["bscale", "uint16", "blank"])
def test_exact_pass_of_scaled_fits(tmp_path, kind):
    """BSCALE/BZERO/BLANK as astropy applies them (``fits.open`` without a
    memory map is the reference: astropy refuses to map scaled data)."""
    rng = np.random.default_rng(3)
    path = tmp_path / f"{kind}.fits"
    if kind == "uint16":
        fits.PrimaryHDU(rng.integers(0, 65535, (6, 20, 30)).astype(np.uint16)).writeto(path)
    else:
        raw = rng.integers(-3000, 3000, (6, 20, 30)).astype(np.int16)
        hdu = fits.PrimaryHDU(raw)
        hdu.header["BSCALE"] = 0.25
        hdu.header["BZERO"] = 100.0
        if kind == "blank":
            raw[2, 3:7, 4:9] = -32768
            hdu.header["BLANK"] = -32768
        hdu.writeto(path)
    with fits.open(path) as hdul:
        ref = _reference(np.asarray(hdul[0].data, dtype=np.float64))
    got = cs.cube_statistics(str(path))
    _assert_exact(got, ref)
    if kind == "blank":
        assert got["blanked_count"] == 20


def test_exact_pass_of_an_image_and_a_four_axis_cube(tmp_path):
    img = _field((1, 30, 40))[0]
    p2 = tmp_path / "img.fits"
    fits.PrimaryHDU(img).writeto(p2)
    _assert_exact(cs.cube_statistics(str(p2)), _reference(img))
    cube = _field((8, 30, 40))
    p4 = tmp_path / "c4.fits"
    fits.PrimaryHDU(cube[np.newaxis]).writeto(p4)
    _assert_exact(cs.cube_statistics(str(p4)), _reference(cube))


def test_exact_pass_of_a_vbt_field(tmp_path):
    from app.volume_source import vbt_field_ref
    fields = [_field(seed=1), _field(seed=2, blanks=True)]
    ref = vbt_field_ref(_write_vbt(tmp_path, fields), 1)
    _assert_exact(cs.cube_statistics(ref), _reference(fields[1]))


def test_exact_pass_of_a_pluto_variable(tmp_path):
    ps = pytest.importorskip("app.pluto_source")
    rho = np.abs(_field(seed=4)).astype(np.float64)
    first = _write_pluto()(tmp_path, {"rho": rho}, mode="multiple_files",
                           precision="flt", endian="little")
    ref = ps.pluto_ref(first, "rho")
    _assert_exact(cs.cube_statistics(ref), _reference(rho.astype(np.float32)))


# ── Above the threshold: sample, pyramid ────────────────────────────────────

def test_the_sample_is_bounded_and_spread():
    shape = (2048, 2048, 2048)
    bands = cs.sample_bands(shape, 4)
    voxels = sum((yb - ya) * shape[2] for _, ya, yb in bands)
    assert 0.9 * cs.SAMPLE_TARGET <= voxels <= 1.2 * cs.SAMPLE_TARGET
    zs = sorted({z for z, _, _ in bands})
    assert len(zs) > 30 and zs[0] < 2048 // 20 and zs[-1] > 2048 - 2048 // 20
    ys = {ya for _, ya, _ in bands}
    assert len(ys) > 200                       # not the same rows in every plane
    for z, ya, yb in bands:
        assert 0 <= z < 2048 and 0 <= ya < yb <= 2048
    # A narrow cube is read in bands of rows, not a row at a time.
    narrow = cs.sample_bands((4000, 4000, 16), 4, 100_000)
    assert all(yb - ya >= cs.BAND_BYTES // 64 or yb == 4000 for _, ya, yb in narrow)
    # A cube no larger than the target is read whole.
    assert cs.sample_bands((4, 5, 6), 4) == [(z, 0, 5) for z in range(4)]


#: Stated tolerances of the sampled numbers on these noise-dominated fields:
#: mean, median and rms within 1 %, std and MAD within 3 %, counts within 1 %.
def _assert_close_to(got: dict, ref: dict) -> None:
    for k in ("mean", "median", "rms"):
        assert got[k] == pytest.approx(ref[k], rel=0.01), k
    for k in ("std", "mad_sigma"):
        assert got[k] == pytest.approx(ref[k], rel=0.03), k
    assert got["valid_count"] == pytest.approx(ref["valid_count"], rel=0.01)
    assert got["sum"] == pytest.approx(ref["sum"], rel=0.02)
    assert ref["min"] <= got["min"] <= got["max"] <= ref["max"]
    assert got["median_exact"] is False and got["exact_fields"][0] == "total_count"


def test_a_large_cube_is_sampled_not_read_whole(tmp_path, large, monkeypatch):
    data = _field((60, 200, 256), blanks=True)
    path = tmp_path / "c.fits"
    fits.PrimaryHDU(data).writeto(path)
    monkeypatch.setattr(cs, "SAMPLE_TARGET", 400_000)    # an eighth of the cube
    read = []
    real = cs.open_reader

    def counting(*a, **k):
        r = real(*a, **k)
        inner = r.read

        def read_block(*b):
            block = inner(*b)
            read.append(block.size)
            return block
        r.read = read_block
        return r
    monkeypatch.setattr(cs, "open_reader", counting)
    got = cs.cube_statistics(str(path))
    assert got["source"] == "sampled" and got["exact_fields"] == ["total_count"]
    assert sum(read) == got["sample_count"] <= 0.15 * data.size
    _assert_close_to(got, _reference(data))
    # Asked for, the exact pass.
    _assert_exact(cs.cube_statistics(str(path), exact=True), _reference(data), median_exact=False)


@pytest.mark.parametrize("blanks", [False, True])
def test_a_large_cube_with_a_pyramid_takes_its_extremes_and_mean(tmp_path, large, monkeypatch,
                                                                 blanks):
    data = _field((64, 96, 128), blanks=blanks)
    path = tmp_path / "c.fits"
    fits.PrimaryHDU(data).writeto(path)
    build_pyramid(path, default_pyramid_path(path, 0), brick=16)
    monkeypatch.setattr(cs, "SAMPLE_TARGET", 100_000)
    monkeypatch.setattr(cs, "PYRAMID_MEAN_VOXELS", 16 * 24 * 32)    # level 2
    got = cs.cube_statistics(str(path))
    ref = _reference(data)
    assert got["source"] == "pyramid" and got["pyramid_level"] == 2
    assert got["exact_fields"] == ["total_count", "min", "max"]
    assert got["min"] == ref["min"] and got["max"] == ref["max"]
    # Without blanks the level's weighted mean is the cube's (float32 levels);
    # with them, a level voxel next to blank ones weighs as a full block.
    assert got["mean"] == pytest.approx(ref["mean"], rel=1e-6 if not blanks else 2e-3)
    _assert_close_to(got, ref)


def test_a_large_vbt_field_and_pluto_variable_are_sampled(tmp_path, large, monkeypatch):
    from app.volume_source import vbt_field_ref
    ps = pytest.importorskip("app.pluto_source")
    monkeypatch.setattr(cs, "SAMPLE_TARGET", 60_000)
    fields = [_field((40, 96, 128), seed=1), _field((40, 96, 128), seed=2, blanks=True)]
    (tmp_path / "vbt").mkdir()
    ref = vbt_field_ref(_write_vbt(tmp_path / "vbt", fields), 1)
    got = cs.cube_statistics(ref)
    assert got["source"] == "sampled" and got["sample_count"] < fields[1].size / 4
    _assert_close_to(got, _reference(fields[1]))
    rho = np.abs(_field((40, 96, 128), seed=4)).astype(np.float64)
    (tmp_path / "pluto").mkdir()
    first = _write_pluto()(tmp_path / "pluto", {"rho": rho}, mode="multiple_files",
                           precision="flt", endian="little")
    got = cs.cube_statistics(ps.pluto_ref(first, "rho"))
    assert got["source"] == "sampled"
    _assert_close_to(got, _reference(rho.astype(np.float32)))


async def test_the_route_says_where_its_numbers_come_from(client, auth_headers, tmp_path,
                                                          monkeypatch):
    data = _field(blanks=True)
    path = tmp_path / "route.fits"
    fits.PrimaryHDU(data).writeto(path)
    opened = (await client.post("/v1/datasets/open", json={"path": str(path)},
                                headers=auth_headers)).json()
    hdr = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    url = f"/v1/analysis/cube-stats/{opened['dataset_id']}"
    small = (await client.get(url, headers=hdr)).json()
    assert small["source"] == "exact" and small["cached"] is False
    # The exact answer also serves an explicit request for exact numbers.
    again = (await client.get(url + "?exact=true", headers=hdr)).json()
    assert again["cached"] is True and again["source"] == "exact"


# ── Resident memory ─────────────────────────────────────────────────────────

_PROBE = textwrap.dedent("""
    import json, os, resource, sys, tempfile
    sys.path.insert(0, sys.argv[1])
    import numpy as np
    from astropy.io import fits
    from app import cube_stats as cs
    # Imports and a first small read of each kind before the baseline.
    small = os.path.join(tempfile.mkdtemp(), "small.fits")
    fits.PrimaryHDU(np.ones((4, 8, 8), np.float32)).writeto(small)
    cs.cube_statistics(small, exact=True)
    cs.sampled_statistics(cs.open_reader(small, 0))
    from app.preview_pyramid import ready_pyramid
    ready_pyramid(small)
    def peak():
        r = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return r if sys.platform == "darwin" else r * 1024
    base = peak()
    out = cs.cube_statistics(sys.argv[2], exact=sys.argv[3] == "exact")
    print(json.dumps({"grown": peak() - base, "source": out["source"],
                      "valid": out["valid_count"], "max": out["max"]}))
""")


@pytest.fixture(scope="module")
def big_cube(tmp_path_factory):
    """A 1.5 GB float32 FITS cube, sparse (all zeros but a few planes), so it
    costs no disk and is quick to read."""
    path = tmp_path_factory.mktemp("big") / "big.fits"
    shape = (384, 1024, 1024)
    header = fits.PrimaryHDU(np.zeros((1, 1, 1), np.float32)).header
    for i, n in enumerate(reversed(shape), start=1):
        header[f"NAXIS{i}"] = n
    raw = header.tostring().encode("ascii")
    nbytes = int(np.prod(shape)) * 4
    with open(path, "wb") as fh:
        fh.write(raw)
        fh.truncate(len(raw) + nbytes + (-nbytes) % 2880)
        fh.seek(len(raw) + 7 * 1024 * 1024 * 4)                # plane 7: a source
        fh.write(np.full(1024 * 1024, 3.5, ">f4").tobytes())
    return path


@pytest.mark.parametrize("mode, bound_mb", [("exact", 400), ("auto", 250)])
def test_peak_resident_memory_stays_bounded(big_cube, mode, bound_mb):
    """The process's resident peak grows by far less than the 1.5 GB cube: an
    exact pass holds one slab (a memory-mapped pass grew by the whole file),
    and a sample of a preview_only cube reads only the sample."""
    env = dict(os.environ, VISIVO_TOKEN="x", VISIVO_FULL_RES_THRESHOLD_BYTES=str(1 << 30))
    run = subprocess.run([sys.executable, "-c", _PROBE, str(BACKEND), str(big_cube), mode],
                         capture_output=True, text=True, env=env, timeout=600, check=True)
    out = json.loads(run.stdout.strip().splitlines()[-1])
    assert out["source"] == ("exact" if mode == "exact" else "sampled")
    assert out["grown"] < bound_mb * 1024 * 1024, out
    if mode == "exact":
        assert out["valid"] == 384 * 1024 * 1024 and out["max"] == 3.5


# ── Compressed files, no pread, pyramid edge cases, the route's cache ──────

def _scaled_hdu(shape=(10, 40, 50), seed=5) -> fits.PrimaryHDU:
    raw = np.random.default_rng(seed).integers(-3000, 3000, shape).astype(np.int16)
    raw[3, 2:6, 4:9] = -32768
    hdu = fits.PrimaryHDU(raw)
    hdu.header["BSCALE"] = 0.25
    hdu.header["BZERO"] = 100.0
    hdu.header["BLANK"] = -32768
    return hdu


@pytest.mark.parametrize("suffix", [".fits.gz", ".fits.bz2"])
def test_a_file_compressed_as_a_whole_is_streamed(tmp_path, suffix, monkeypatch):
    """Exact through the decompressing stream (scaled data included), above
    the threshold too: reaching a sample would decompress nearly all of it,
    so the exact pass costs the same, in the same bounded memory."""
    path = tmp_path / f"c{suffix}"
    _scaled_hdu().writeto(path)
    with fits.open(path) as hdul:
        data = np.asarray(hdul[0].data, dtype=np.float64)
    ref = _reference(data)
    _assert_exact(cs.cube_statistics(str(path)), ref)
    monkeypatch.setenv("VISIVO_FULL_RES_THRESHOLD_BYTES", "1")
    _assert_exact(cs.cube_statistics(str(path)), ref)


def test_a_stream_that_ends_early_is_an_unusable_input():
    import io
    fill = cs._seek_fill(io.BytesIO(b"0123456789"))
    view = memoryview(bytearray(8))
    fill(view, 2)
    assert bytes(view) == b"23456789"
    with pytest.raises(ValueError):
        fill(memoryview(bytearray(8)), 5)


def test_a_tile_compressed_image_is_read_tile_by_tile(tmp_path):
    data = _field((6, 40, 50), blanks=True)
    path = tmp_path / "tiles.fits"
    fits.HDUList([fits.PrimaryHDU(), fits.CompImageHDU(data, compression_type="GZIP_2",
                                                       tile_shape=(1, 20, 25))]).writeto(path)
    with fits.open(path) as hdul:
        ref = _reference(np.asarray(hdul[1].data, dtype=np.float64))
    _assert_exact(cs.cube_statistics(str(path)), ref)


def test_without_pread_the_file_is_read_by_seek_and_read(tmp_path, monkeypatch):
    path = tmp_path / "s.fits"
    _scaled_hdu().writeto(path)
    with fits.open(path) as hdul:
        ref = _reference(np.asarray(hdul[0].data, dtype=np.float64))
    monkeypatch.delattr(os, "pread")
    # The byte reader still serves (not the memory-mapped fallback).
    reader = cs._byte_reader(str(path), 0)
    assert reader is not None
    reader.close()
    _assert_exact(cs.cube_statistics(str(path)), ref)


def test_the_pyramid_mean_of_an_odd_cube_is_close(tmp_path, large, monkeypatch):
    """Partial blocks at the ends of the axes weigh slightly off (the levels
    are unweighted means): the stated tolerance is 1e-3 of the mean."""
    shape = (67, 97, 131)
    z, y, x = np.indices(shape, dtype=np.float64)
    data = (10.0 + z + 0.5 * x).astype(np.float32)
    path = tmp_path / "odd.fits"
    fits.PrimaryHDU(data).writeto(path)
    build_pyramid(path, default_pyramid_path(path, 0), brick=16)
    monkeypatch.setattr(cs, "PYRAMID_MEAN_VOXELS", 17 * 25 * 33)    # level 2
    got = cs.cube_statistics(str(path))
    assert got["source"] == "pyramid" and got["pyramid_level"] == 2
    assert got["mean"] == pytest.approx(float(data.astype(np.float64).mean()), rel=1e-3)
    assert got["min"] == float(data.min()) and got["max"] == float(data.max())


def test_double_data_pyramid_extremes_are_not_called_exact(tmp_path, large):
    data = _field((32, 48, 64)).astype(np.float64) + 1e-9
    path = tmp_path / "d.fits"
    fits.PrimaryHDU(data).writeto(path)
    build_pyramid(path, default_pyramid_path(path, 0), brick=16)
    got = cs.cube_statistics(str(path))
    assert got["source"] == "pyramid" and got["exact_fields"] == ["total_count"]


def test_an_unusable_pyramid_falls_back_to_the_sample(tmp_path, large, monkeypatch):
    data = _field((32, 48, 64))
    path = tmp_path / "c.fits"
    fits.PrimaryHDU(data).writeto(path)
    build_pyramid(path, default_pyramid_path(path, 0), brick=16)

    def gone(*a, **k):
        raise OSError("the pyramid was evicted")
    monkeypatch.setattr(cs, "pyramid_summary", gone)
    assert cs.cube_statistics(str(path))["source"] == "sampled"


async def test_the_route_does_not_keep_a_sampled_answer(client, auth_headers, tmp_path,
                                                        monkeypatch):
    """A preview_only cube is sampled until its pyramid is ready, then the
    next request takes the pyramid's extremes and mean."""
    data = _field((32, 48, 64))
    path = tmp_path / "late.fits"
    fits.PrimaryHDU(data).writeto(path)
    monkeypatch.setenv("VISIVO_FULL_RES_THRESHOLD_BYTES", "1")
    opened = (await client.post("/v1/datasets/open", json={"path": str(path)},
                                headers=auth_headers)).json()
    hdr = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    url = f"/v1/analysis/cube-stats/{opened['dataset_id']}"
    first = (await client.get(url, headers=hdr)).json()
    assert first["source"] == "sampled"
    build_pyramid(path, default_pyramid_path(path, 0), brick=16)
    second = (await client.get(url, headers=hdr)).json()
    assert second["source"] == "pyramid" and second["cached"] is False
    assert second["max"] == float(data.max())
    third = (await client.get(url, headers=hdr)).json()
    assert third["cached"] is True
    exact = (await client.get(url + "?exact=true", headers=hdr)).json()
    assert exact["source"] == "exact" and exact["valid_count"] == data.size
