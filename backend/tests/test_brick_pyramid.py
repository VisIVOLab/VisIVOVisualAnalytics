"""The brick pyramid of a cube (#69): what each level holds, how bricks are
read, and that building it out of core changes nothing but the memory used."""
from __future__ import annotations

import os
import time

import numpy as np
import pytest
from astropy.io import fits

from app.brick_pyramid import (
    BrickPyramid,
    StalePyramid,
    build_pyramid,
    level_shapes,
)


def _cube(shape=(37, 45, 70), seed=3) -> np.ndarray:
    """Odd sizes on every axis, so every level has partial edge bricks, and a
    blank region, so NaN handling is exercised."""
    rng = np.random.default_rng(seed)
    data = rng.normal(0.0, 1.0, shape).astype(np.float32)
    data[5:20, 10:30, 40:70] = np.nan
    return data


def _write(tmp_path, data, name="cube.fits"):
    path = tmp_path / name
    fits.PrimaryHDU(data).writeto(path)
    return path


def _reference_halve(a: np.ndarray) -> np.ndarray:
    """Mean of each 2x2x2 block over its finite voxels, written plainly."""
    nz, ny, nx = ((n + 1) // 2 for n in a.shape)
    out = np.full((nz, ny, nx), np.nan, dtype=np.float32)
    for z in range(nz):
        for y in range(ny):
            for x in range(nx):
                cell = a[2 * z:2 * z + 2, 2 * y:2 * y + 2, 2 * x:2 * x + 2]
                vals = cell[np.isfinite(cell)]
                if vals.size:
                    out[z, y, x] = vals.astype(np.float64).mean()
    return out


def test_levels_halve_until_one_brick():
    assert level_shapes((37, 45, 70), 16) == [(37, 45, 70), (19, 23, 35), (10, 12, 18), (5, 6, 9)]
    assert level_shapes((10, 10, 10), 16) == [(10, 10, 10)]


def test_level0_is_the_cube_and_each_level_halves_the_one_below(tmp_path):
    data = _cube()
    src = _write(tmp_path, data)
    out = build_pyramid(src, tmp_path / "p.h5", brick=16)
    with BrickPyramid(out, source=src) as p:
        assert p.levels == 4
        level0 = p._h5["level0/data"][...]
        assert np.array_equal(np.isnan(level0), np.isnan(data))
        assert np.allclose(level0, data, equal_nan=True)
        below = level0
        for level in range(1, p.levels):
            here = p._h5[f"level{level}/data"][...]
            assert np.allclose(here, _reference_halve(below), equal_nan=True, rtol=1e-6, atol=1e-6)
            below = here


def test_brick_statistics_match_the_bricks(tmp_path):
    data = _cube()
    src = _write(tmp_path, data)
    with BrickPyramid(build_pyramid(src, tmp_path / "p.h5", brick=16)) as p:
        lo, hi = p.stats(0)
        assert lo.shape == p.grid(0) == (3, 3, 5)
        for bz in range(3):
            for by in range(3):
                for bx in range(5):
                    tile = data[16 * bz:16 * bz + 16, 16 * by:16 * by + 16, 16 * bx:16 * bx + 16]
                    if np.isfinite(tile).any():
                        assert lo[bz, by, bx] == np.nanmin(tile)
                        assert hi[bz, by, bx] == np.nanmax(tile)
                    else:
                        assert np.isnan(lo[bz, by, bx]) and np.isnan(hi[bz, by, bx])


def test_a_fully_blank_brick_has_no_statistics(tmp_path):
    data = np.ones((32, 32, 32), dtype=np.float32)
    data[:16, :16, :16] = np.nan
    src = _write(tmp_path, data)
    with BrickPyramid(build_pyramid(src, tmp_path / "p.h5", brick=16)) as p:
        lo, hi = p.stats(0)
        assert np.isnan(lo[0, 0, 0]) and np.isnan(hi[0, 0, 0])
        assert lo[1, 1, 1] == hi[1, 1, 1] == 1.0
        # The blank octant averages to NaN, not to zero, one level up.
        assert np.isnan(p._h5["level1/data"][0, 0, 0])


def test_a_brick_comes_with_its_neighbours_and_nan_outside_the_cube(tmp_path):
    data = _cube()
    src = _write(tmp_path, data)
    with BrickPyramid(build_pyramid(src, tmp_path / "p.h5", brick=16)) as p:
        inner = p.read_brick(0, 1, 1, 1, ghost=2)
        assert inner.shape == (20, 20, 20)
        assert np.allclose(inner, data[14:34, 14:34, 14:34], equal_nan=True)
        # The corner brick: the ghost before it is outside the cube.
        corner = p.read_brick(0, 0, 0, 0, ghost=1)
        assert np.isnan(corner[0]).all() and np.isnan(corner[:, 0]).all()
        assert np.allclose(corner[1:, 1:, 1:], data[0:17, 0:17, 0:17], equal_nan=True)
        # The last brick along x is partial (70 = 4*16 + 6): beyond the cube is NaN.
        edge = p.read_brick(0, 0, 0, 4, ghost=0)
        assert np.allclose(edge[:, :, :6], data[0:16, 0:16, 64:70], equal_nan=True)
        assert np.isnan(edge[:, :, 6:]).all()
        with pytest.raises(IndexError):
            p.read_brick(0, 3, 0, 0)


def test_small_blocks_give_the_same_pyramid_as_large_ones(tmp_path):
    """Out of core is only a matter of block size: a build that reads the cube a
    brick at a time writes the same file content as one that reads it whole."""
    data = _cube()
    src = _write(tmp_path, data)
    small = build_pyramid(src, tmp_path / "small.h5", brick=16, max_block_bytes=8 * 16 ** 3 * 4)
    large = build_pyramid(src, tmp_path / "large.h5", brick=16, max_block_bytes=1 << 30)
    with BrickPyramid(small) as a, BrickPyramid(large) as b:
        for level in range(a.levels):
            for name in ("data", "min", "max"):
                assert np.array_equal(a._h5[f"level{level}/{name}"][...],
                                      b._h5[f"level{level}/{name}"][...], equal_nan=True)


def test_a_changed_source_makes_the_pyramid_stale(tmp_path):
    data = _cube()
    src = _write(tmp_path, data)
    out = build_pyramid(src, tmp_path / "p.h5", brick=16)
    BrickPyramid(out, source=src).close()
    later = time.time() + 10
    os.utime(src, (later, later))
    with pytest.raises(StalePyramid):
        BrickPyramid(out, source=src)


def test_an_abandoned_build_leaves_nothing(tmp_path):
    src = _write(tmp_path, _cube())
    calls = []

    def stop():
        calls.append(1)
        if len(calls) > 3:
            raise RuntimeError("stopped")

    with pytest.raises(RuntimeError):
        build_pyramid(src, tmp_path / "p.h5", brick=16, max_block_bytes=8 * 16 ** 3 * 4,
                      should_stop=stop)
    assert not (tmp_path / "p.h5").exists()
    assert not list(tmp_path.glob("*.part"))


def test_the_brick_must_be_a_power_of_two(tmp_path):
    src = _write(tmp_path, _cube())
    with pytest.raises(ValueError):
        build_pyramid(src, tmp_path / "p.h5", brick=48)


def test_a_file_that_is_not_a_pyramid_is_refused(tmp_path):
    import h5py

    path = tmp_path / "other.h5"
    with h5py.File(path, "w") as h5:
        h5.attrs["format"] = "something-else"
    with pytest.raises(ValueError):
        BrickPyramid(path)


def test_the_default_location_is_in_the_workspace(tmp_path, monkeypatch):
    from app.brick_pyramid import default_pyramid_path

    src = _write(tmp_path, _cube())
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    first = default_pyramid_path(src)
    assert first.parent == tmp_path / "pyr" and first.name.endswith(".pyr.h5")
    later = time.time() + 10
    os.utime(src, (later, later))
    assert default_pyramid_path(src) != first          # a changed file gets a new pyramid


def _level0(path):
    with BrickPyramid(path) as p:
        return p._h5["level0/data"][...]


def test_scaled_integer_data_is_scaled_and_blank_is_nan(tmp_path):
    """astropy will not memory-map scaled data: the builder maps it raw and
    applies BLANK, BSCALE and BZERO itself, block by block."""
    raw = np.arange(10 * 12 * 14, dtype=np.int16).reshape(10, 12, 14) - 800
    raw[2, 3, 4] = -32768
    hdu = fits.PrimaryHDU(raw)
    hdu.header["BSCALE"], hdu.header["BZERO"], hdu.header["BLANK"] = 0.5, 10.0, -32768
    src = tmp_path / "scaled.fits"
    hdu.writeto(src)
    with fits.open(src, memmap=False) as h:
        expected = h[0].data.astype(np.float32)          # astropy's own scaling
    got = _level0(build_pyramid(src, tmp_path / "p.h5", brick=8))
    assert np.isnan(got[2, 3, 4]) and np.isnan(expected[2, 3, 4])
    assert np.allclose(got, expected, equal_nan=True)

    unsigned = tmp_path / "unsigned.fits"
    fits.PrimaryHDU(np.arange(6 * 7 * 9, dtype=np.uint16).reshape(6, 7, 9) + 60000).writeto(unsigned)
    with fits.open(unsigned, memmap=False) as h:
        expected = h[0].data.astype(np.float32)
    assert np.array_equal(_level0(build_pyramid(unsigned, tmp_path / "u.h5", brick=8)), expected)


def test_a_compressed_cube_is_read_tile_by_tile(tmp_path):
    data = _cube((9, 20, 30))
    src = tmp_path / "comp.fits"
    fits.HDUList([fits.PrimaryHDU(), fits.CompImageHDU(data, compression_type="GZIP_1")]).writeto(src)
    with fits.open(src) as h:
        expected = h[1].data.astype(np.float32)       # float tiles are quantised: what astropy reads
    got = _level0(build_pyramid(src, tmp_path / "p.h5", hdu=1, brick=8))
    assert np.array_equal(got, expected, equal_nan=True)


def test_degenerate_axes_are_dropped(tmp_path):
    data = _cube((9, 20, 30))
    src = tmp_path / "stokes.fits"
    fits.PrimaryHDU(data[np.newaxis]).writeto(src)             # (1, z, y, x)
    assert np.allclose(_level0(build_pyramid(src, tmp_path / "p.h5", brick=8)), data, equal_nan=True)


def test_the_pyramid_is_never_written_over_its_source(tmp_path):
    src = _write(tmp_path, _cube())
    before = src.read_bytes()
    with pytest.raises(ValueError):
        build_pyramid(src, src, brick=16)
    assert src.read_bytes() == before


def test_a_different_file_with_the_same_size_and_time_is_not_the_source(tmp_path):
    a = _write(tmp_path, _cube(seed=1), "a.fits")
    b = _write(tmp_path, _cube(seed=2), "b.fits")
    st = a.stat()
    os.utime(b, ns=(st.st_atime_ns, st.st_mtime_ns))
    out = build_pyramid(a, tmp_path / "p.h5", brick=16)
    with pytest.raises(StalePyramid):
        BrickPyramid(out, source=b)


def test_a_budget_below_one_brick_is_refused(tmp_path):
    src = _write(tmp_path, _cube())
    with pytest.raises(ValueError):
        build_pyramid(src, tmp_path / "p.h5", brick=16, max_block_bytes=1024)


def _astropy_level0(src, hdu):
    with fits.open(src, memmap=False) as h:
        return h[hdu].data.astype(np.float32)


@pytest.mark.parametrize("kind", ["uint16", "scaled-int16"])
def test_compressed_integer_data_is_scaled_too(tmp_path, kind):
    if kind == "uint16":
        data = np.arange(8 ** 3, dtype=np.uint16).reshape(8, 8, 8) + 60000
        item = fits.CompImageHDU(data, compression_type="GZIP_1")
    else:
        data = np.arange(8 ** 3, dtype=np.int16).reshape(8, 8, 8)
        item = fits.CompImageHDU(data, compression_type="GZIP_1")
        item.header["BSCALE"], item.header["BZERO"], item.header["BLANK"] = 0.5, 10.0, 0
    src = tmp_path / f"{kind}.fits"
    fits.HDUList([fits.PrimaryHDU(), item]).writeto(src)
    got = _level0(build_pyramid(src, tmp_path / "p.h5", hdu=1, brick=8))
    if kind == "uint16":
        assert np.array_equal(got, _astropy_level0(src, 1))
    else:
        # BLANK marks undefined integer pixels (FITS standard); astropy does not
        # apply it to tile-compressed images, the pyramid does.
        expected = data.astype(np.float64) * 0.5 + 10.0
        expected[data == 0] = np.nan
        assert np.array_equal(got, expected.astype(np.float32), equal_nan=True)


def test_unsigned_32_bit_values_survive(tmp_path):
    """Stored as value - 2**31: converting to float32 before adding BZERO would
    merge neighbouring values."""
    data = np.arange(8 ** 3, dtype=np.uint32).reshape(8, 8, 8)
    src = tmp_path / "u32.fits"
    fits.PrimaryHDU(data).writeto(src)
    got = _level0(build_pyramid(src, tmp_path / "p.h5", brick=8))
    assert np.array_equal(got, data.astype(np.float32))


def test_a_whole_file_compressed_fits_is_refused(tmp_path):
    src = tmp_path / "cube.fits.gz"
    fits.PrimaryHDU(_cube((8, 8, 8))).writeto(src)
    with pytest.raises(ValueError, match="decompress"):
        build_pyramid(src, tmp_path / "p.h5", brick=8)


def test_compression_tiles_larger_than_the_budget_are_refused(tmp_path):
    data = np.ones((64, 64, 64), dtype=np.float32)
    src = tmp_path / "one-tile.fits"
    fits.HDUList([fits.PrimaryHDU(),
                  fits.CompImageHDU(data, tile_shape=data.shape, compression_type="GZIP_1")]).writeto(src)
    with pytest.raises(ValueError, match="tiles"):
        build_pyramid(src, tmp_path / "p.h5", hdu=1, brick=8, max_block_bytes=8 * 8 ** 3 * 4)


def test_unsigned_64_bit_values_survive(tmp_path):
    data = np.arange(8 ** 3, dtype=np.uint64).reshape(8, 8, 8)
    # Large values too: rounded once, as a direct uint64 to float32 conversion does.
    data.flat[:4] = [2 ** 63 + 2 ** 39 - 1, 2 ** 63 + 2 ** 39, 2 ** 63 + 2 ** 39 + 1, 2 ** 64 - 1]
    src = tmp_path / "u64.fits"
    fits.PrimaryHDU(data).writeto(src)
    got = _level0(build_pyramid(src, tmp_path / "p.h5", brick=8))
    assert np.array_equal(got, data.astype(np.float32))


def test_a_zip_archive_is_refused_before_it_is_read(tmp_path):
    import io
    import zipfile

    buf = io.BytesIO()
    fits.PrimaryHDU(_cube((8, 8, 8))).writeto(buf)
    src = tmp_path / "cube.zip"
    with zipfile.ZipFile(src, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("cube.fits", buf.getvalue())
    with pytest.raises(ValueError, match="decompress"):
        build_pyramid(src, tmp_path / "p.h5", brick=8)


def test_a_compressed_block_decompresses_one_tile_at_a_time(tmp_path):
    """Long, thin tiles: a block crosses many of them, and reading the block as
    one section would decompress all of them into one buffer."""
    import tracemalloc

    from app.brick_pyramid import _SourceCube

    data = np.arange(64 ** 3, dtype=np.float32).reshape(64, 64, 64)
    src = tmp_path / "thin-tiles.fits"
    fits.HDUList([fits.PrimaryHDU(),
                  fits.CompImageHDU(data, tile_shape=(64, 64, 1), compression_type="GZIP_1",
                                    quantize_level=0.0)]).writeto(src)
    cube = _SourceCube(str(src), 1, 64 * 64 * 8)
    try:
        tracemalloc.start()
        block = cube.block(0, 8, 0, 16, 0, 64)
        _, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
    finally:
        cube.close()
    assert np.array_equal(block, _astropy_level0(src, 1)[0:8, 0:16, 0:64])
    assert peak < data.nbytes / 4, peak            # never the whole cube


def test_the_command_builds_the_pyramid_the_viewer_looks_for(tmp_path, monkeypatch):
    """A cube in an extension: without --hdu the command picks the HDU the
    render session opens, so the session finds the pyramid built beforehand."""
    from app.brick_pyramid import default_pyramid_path, main

    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    src = tmp_path / "ext.fits"
    fits.HDUList([fits.PrimaryHDU(), fits.ImageHDU(_cube((16, 16, 16)))]).writeto(src)
    assert main(["build", str(src), "--brick", "8"]) == 0
    with BrickPyramid(default_pyramid_path(src, 1), source=src) as p:
        assert p.source.hdu == 1 and p.shape(0) == (16, 16, 16)


def test_the_command_refuses_a_zipped_cube_without_reading_it(tmp_path, monkeypatch):
    import io
    import zipfile

    from app.brick_pyramid import main

    buf = io.BytesIO()
    fits.PrimaryHDU(_cube((16, 16, 16))).writeto(buf)
    src = tmp_path / "cube.zip"
    with zipfile.ZipFile(src, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("cube.fits", buf.getvalue())
    reads = []
    monkeypatch.setattr(zipfile.ZipFile, "read", lambda *a, **k: reads.append(a) or b"")
    with pytest.raises(ValueError, match="compressed as a whole"):
        main(["build", str(src), str(tmp_path / "p.h5"), "--brick", "8"])
    assert not reads
