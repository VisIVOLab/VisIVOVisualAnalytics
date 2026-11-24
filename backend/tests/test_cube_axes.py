"""Which three axes of a FITS array are its cube (issue #506): one rule, in
fits_dataset.cube_axes, that the dataset, the brick pyramid, the render session
and the volume source all follow. A CASA export (RA, DEC, STOKES = 1, FREQ) is
numpy (FREQ, 1, Y, X): the pyramid and the render session used to read it as
one plane."""
from __future__ import annotations

import h5py
import numpy as np
import pytest
from astropy.io import fits

from app.brick_pyramid import (BrickPyramid, SourceSignature, StalePyramid, _SourceCube,
                               build_pyramid, source_shape)
from app.fits_dataset import (ScientificFitsDataset, cube_axes, cube_index, cube_layout,
                              cube_shape)
from app.volume_source import volume_cube_shape, volume_data, volume_shape

# numpy shape -> CTYPE of the FITS axes 1, 2, ... and the cube's shape
CASES = {
    "casa_freq_last": ((7, 1, 12, 14), ("RA---SIN", "DEC--SIN", "STOKES", "FREQ"), (7, 12, 14)),
    "freq_before_stokes": ((1, 7, 12, 14), ("RA---SIN", "DEC--SIN", "FREQ", "STOKES"), (7, 12, 14)),
    "two_stokes_one_channel": ((2, 1, 12, 14), ("RA---SIN", "DEC--SIN", "FREQ", "STOKES"), (2, 12, 14)),
    "plain_3d": ((7, 12, 14), ("RA---SIN", "DEC--SIN", "FREQ"), (7, 12, 14)),
    "image_4d": ((1, 1, 12, 14), ("RA---SIN", "DEC--SIN", "FREQ", "STOKES"), (1, 12, 14)),
}


def _write(tmp_path, name, shape, ctype, compressed=False):
    data = np.arange(int(np.prod(shape)), dtype=np.float32).reshape(shape)
    header = fits.Header()
    for i, c in enumerate(ctype, start=1):
        header[f"CTYPE{i}"] = c
        header[f"CRPIX{i}"] = 1.0
        header[f"CRVAL{i}"] = 1.0
        header[f"CDELT{i}"] = 1.0
    path = tmp_path / f"{name}.fits"
    if compressed:
        tile = (1,) * (len(shape) - 2) + (shape[-2], shape[-1])
        fits.HDUList([fits.PrimaryHDU(), fits.CompImageHDU(data, header=header, tile_shape=tile)]
                     ).writeto(path)
    else:
        fits.PrimaryHDU(data, header=header).writeto(path)
    return path, data


def _expected(data):
    """What the dataset reports: np.squeeze, as squeeze_to_3d always did."""
    out = np.squeeze(data)
    return out[np.newaxis] if out.ndim == 2 else out


@pytest.mark.parametrize("name", list(CASES))
@pytest.mark.parametrize("compressed", [False, True])
def test_every_reader_sees_the_datasets_cube(tmp_path, name, compressed):
    shape, ctype, cube = CASES[name]
    path, data = _write(tmp_path, name, shape, ctype, compressed)
    hdu = 1 if compressed else 0
    want = _expected(data)
    assert want.shape == cube
    probe = (min(1, cube[0] - 1), 3, 5)

    # the dataset
    ds = ScientificFitsDataset(str(path))
    assert ds.hdu_index == hdu
    hdul, raw = ds.open_memmap()
    try:
        got = ScientificFitsDataset.squeeze_to_3d(raw)
        got = got[np.newaxis] if got.ndim == 2 else got
        assert got.shape == cube and got[probe] == want[probe]
    finally:
        hdul.close()

    # the pyramid's source and its header-only shape
    src = _SourceCube(str(path), hdu, 1 << 28)
    try:
        assert src.shape == cube
        np.testing.assert_array_equal(
            src.block(0, cube[0], 2, 7, 1, 9), want[:, 2:7, 1:9])
    finally:
        src.close()
    assert source_shape(path, hdu) == cube

    # the volume source and the render session's shape
    full, index = volume_shape(path, None)
    assert index == hdu
    assert volume_cube_shape(path, full, index) == cube
    with volume_data(str(path), hdu) as vol:
        assert vol.shape == cube and vol[probe] == want[probe]


@pytest.mark.parametrize("name", ["casa_freq_last", "freq_before_stokes", "two_stokes_one_channel",
                                  "plain_3d"])
def test_a_pyramid_built_from_the_file_agrees_with_the_dataset(tmp_path, name):
    shape, ctype, cube = CASES[name]
    path, data = _write(tmp_path, name, shape, ctype)
    want = _expected(data)
    out = build_pyramid(path, tmp_path / "p.h5", hdu=0, brick=8)
    with BrickPyramid(out, source=path) as pyr:
        assert pyr.shape(0) == cube
    with h5py.File(out) as h:
        level0 = h["level0/data"][...]
    assert level0.shape == cube
    np.testing.assert_array_equal(level0, want)


def test_the_axes_rule():
    assert cube_axes((7, 1, 12, 14)) == (0, 2, 3)
    assert cube_axes((1, 7, 12, 14)) == (1, 2, 3)
    assert cube_axes((12, 14)) == (0, 1)
    assert cube_axes((3, 4, 7, 12, 14)) == (2, 3, 4)   # the first plane of the others
    assert cube_axes((3, 1, 7, 12, 14)) == (2, 3, 4)
    assert cube_index((7, 1, 12, 14)) == (slice(None), 0, slice(None), slice(None))
    assert cube_shape((7, 1, 12, 14)) == (7, 12, 14)


def test_extra_active_axes_keep_their_first_plane(tmp_path):
    data = np.arange(3 * 4 * 5 * 6 * 7, dtype=np.float32).reshape(3, 4, 5, 6, 7)
    path = tmp_path / "five.fits"
    fits.PrimaryHDU(data).writeto(path)
    src = _SourceCube(str(path), 0, 1 << 28)
    try:
        assert src.shape == (5, 6, 7)
        np.testing.assert_array_equal(src.block(0, 5, 0, 6, 0, 7), data[0, 0])
    finally:
        src.close()
    with volume_data(str(path)) as vol:
        np.testing.assert_array_equal(vol, data[0, 0])
    np.testing.assert_array_equal(ScientificFitsDataset.squeeze_to_3d(data), data[0, 0])


def test_the_view_is_lazy_on_a_memory_map(tmp_path):
    path, _ = _write(tmp_path, "lazy", *CASES["casa_freq_last"][:2])
    with fits.open(path, memmap=True) as hdul:
        data = hdul[0].data
        view = data[cube_index(data.shape)]
        assert view.shape == (7, 12, 14) and np.shares_memory(view, data)


def test_a_pyramid_made_with_the_old_rule_is_not_reused(tmp_path):
    """A (FREQ, 1, Y, X) file's pyramid has another name and, built as before,
    another layout: StalePyramid, so it is rebuilt. A plain cube keeps its."""
    path, _ = _write(tmp_path, "casa", *CASES["casa_freq_last"][:2])
    assert cube_layout((7, 1, 12, 14)) == "axes=0,2,3"
    assert SourceSignature.of(path, 0).layout == "axes=0,2,3"
    out = build_pyramid(path, tmp_path / "p.h5", hdu=0, brick=8)
    with h5py.File(out, "r+") as h:
        h.attrs["source_layout"] = ""            # as a pyramid of the old rule
    with pytest.raises(StalePyramid):
        BrickPyramid(out, source=path)

    plain, _ = _write(tmp_path, "plain", *CASES["plain_3d"][:2])
    assert SourceSignature.of(plain, 0).layout == ""
    # cubes read the same by both rules keep their layout "" (and pyramids)
    for shape in [(7, 12, 14), (12, 14), (1, 12, 14)]:
        assert cube_layout(shape) == ""
    assert cube_layout((12, 14, 1)) != ""


def test_a_whole_file_compressed_signature_does_not_open_it(tmp_path):
    bogus = tmp_path / "x.fits.gz"
    bogus.write_bytes(b"not a fits file")
    assert SourceSignature.of(bogus, 0).layout == ""
