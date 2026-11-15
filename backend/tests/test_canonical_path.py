"""Caches keyed on a source path are keyed on the name the file system
stores (issue #325): on a case-insensitive file system the same file named in
another case gets the same brick pyramid and the same decompressed copy."""

from __future__ import annotations

import gzip
import os
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import brick_pyramid, compressed_fits
from app.canonical_path import canonical_path


def _case_insensitive(directory: Path) -> bool:
    probe = directory / "CaseProbe"
    probe.mkdir()
    try:
        return (directory / "caseprobe").exists()
    finally:
        probe.rmdir()


@pytest.fixture
def mixed_case_dir(tmp_path: Path) -> Path:
    if not _case_insensitive(tmp_path):
        pytest.skip("the temporary directory's file system is case sensitive")
    d = tmp_path / "STEP62"
    d.mkdir()
    return d


def _write_cube(path: Path) -> None:
    data = np.arange(4 * 5 * 6, dtype=np.float32).reshape(4, 5, 6)
    fits.PrimaryHDU(data).writeto(path)


def test_canonical_path_spells_the_stored_name(mixed_case_dir: Path) -> None:
    cube = mixed_case_dir / "Cube.fits"
    _write_cube(cube)
    other = Path(str(cube).replace("STEP62", "step62").replace("Cube.fits", "cube.FITS"))
    assert other.exists()
    assert canonical_path(other) == canonical_path(cube)
    assert canonical_path(other).name == "Cube.fits"
    assert canonical_path(other).parent.name == "STEP62"


def test_a_missing_path_comes_back_resolved(tmp_path: Path) -> None:
    missing = tmp_path / "nope" / "x.fits"
    assert canonical_path(missing) == missing.resolve()


def test_the_pyramid_name_does_not_depend_on_the_case(mixed_case_dir: Path) -> None:
    cube = mixed_case_dir / "rho.fits"
    _write_cube(cube)
    lower = str(cube).replace("STEP62", "step62")
    assert brick_pyramid.SourceSignature.of(lower, 0) == brick_pyramid.SourceSignature.of(str(cube), 0)
    assert brick_pyramid.default_pyramid_path(lower) == brick_pyramid.default_pyramid_path(str(cube))


def test_the_decompressed_copy_does_not_depend_on_the_case(mixed_case_dir: Path, tmp_path: Path,
                                                            monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("VISIVO_DECOMPRESS_CACHE_DIR", str(tmp_path / "decompressed"))
    raw = tmp_path / "plain.fits"
    _write_cube(raw)
    gz = mixed_case_dir / "Cube.fits.gz"
    with open(raw, "rb") as src, gzip.open(gz, "wb") as dst:
        dst.write(src.read())
    first = compressed_fits.materialize(gz)
    second = compressed_fits.materialize(str(gz).replace("STEP62", "step62"))
    assert first is not None and second is not None
    assert Path(first.path) == Path(second.path)
    assert second.cache_hit
