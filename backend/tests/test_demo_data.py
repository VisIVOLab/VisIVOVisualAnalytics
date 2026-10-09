"""Regression tests for the demo-data generator (backend/scripts/generate_demo_data.py).

Locks in the "one dataset per viewer feature" harness documented in
docs/demo-datasets.md: every file must be produced and load through the same
backend paths the desktop uses, and the HI cube must match the kinematic model
at the parameters the doc tells reviewers to enter.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

_BACKEND = Path(__file__).resolve().parents[1]
_GEN = _BACKEND / "scripts" / "generate_demo_data.py"


def _load_generator():
    spec = importlib.util.spec_from_file_location("generate_demo_data", _GEN)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def demo_dir(tmp_path_factory) -> Path:
    """Generate the full demo set once into a temp dir via the real entrypoint."""
    out = tmp_path_factory.mktemp("visivo_demo")
    gen = _load_generator()
    argv = sys.argv
    sys.argv = ["generate_demo_data", str(out)]
    try:
        assert gen.main() == 0
    finally:
        sys.argv = argv
    return out


def test_manifest(demo_dir: Path):
    expected = [
        "catalogue_sky.csv", "catalogue_cartesian.csv", "catalogue.speck",
        "velocity_field.fits", "hi_cube.fits", "image_2d.fits",
        "combo/velocity.fits", "combo/galaxies.csv",
        "tbl_snapshot/snap_demo_1_1_1.tbl",
    ]
    for rel in expected:
        assert (demo_dir / rel).exists(), f"missing demo file: {rel}"


def test_catalogue_systems_detected(demo_dir: Path):
    from app.dependencies import (_catalogue_load_dataframe,
                                  _catalogue_metadata_from_dataframe)

    def system_of(name: str, fmt: str):
        df = _catalogue_load_dataframe(demo_dir / name, fmt)
        md = _catalogue_metadata_from_dataframe(df, fmt)
        return md["coordinate_fields"]["system"], df

    s, _ = system_of("catalogue_sky.csv", "csv")
    assert s == "sky"

    s, df = system_of("catalogue_cartesian.csv", "csv")
    assert s == "cartesian"
    assert {"x", "y", "z", "vx", "vy", "vz"} <= set(df.columns)

    s, _ = system_of("catalogue.speck", "speck")
    assert s == "cartesian"

    s, _ = system_of("tbl_snapshot/snap_demo_1_1_1.tbl", "ipac")
    assert s == "cartesian"


def test_tbl_siblings_have_identical_columns(demo_dir: Path):
    # Multi-file concat requires identical columns across siblings.
    from app.dependencies import _catalogue_load_dataframe

    tbls = sorted((demo_dir / "tbl_snapshot").glob("*.tbl"))
    assert len(tbls) == 3
    cols = [list(_catalogue_load_dataframe(p, "ipac").columns) for p in tbls]
    assert all(c == cols[0] for c in cols)


def test_velocity_fields_load(demo_dir: Path):
    from app.velocity_field import load_velocity_field

    for name in ("velocity_field.fits", "combo/velocity.fits"):
        vf = load_velocity_field(demo_dir / name, box_mpc=200.0)
        assert vf.vx.shape == (64, 64, 64)
        speed = np.sqrt(vf.vx ** 2 + vf.vy ** 2 + vf.vz ** 2)
        assert speed.max() > 0.0


def test_hi_cube_wcs_and_shape(demo_dir: Path):
    with fits.open(demo_dir / "hi_cube.fits") as hdul:
        hdr = hdul[0].header
        assert hdul[0].data.shape == (64, 64, 64)
    assert hdr["CTYPE3"] == "VRAD"
    assert hdr["RESTFRQ"] > 0.0
    assert "BMAJ" in hdr


def test_hi_cube_matches_kinematic_model(demo_dir: Path):
    """The demo cube's velocity field must match tilted_ring_model at the params
    docs/demo-datasets.md tells the reviewer to enter, so the overlay lines up."""
    from app.kinematic_model import tilted_ring_model

    with fits.open(demo_dir / "hi_cube.fits") as hdul:
        cube = hdul[0].data.astype(np.float64)
        hdr = hdul[0].header
    nchan, ny, nx = cube.shape
    # Derive the spectral axis from the FITS WCS (not hard-coded), so a drift in
    # CRVAL3/CDELT3/CUNIT3 — which would break the real app overlay even if the
    # data still matched — is caught here too. Convert to km/s for the model.
    pix = np.arange(nchan) + 1.0  # FITS 1-based pixel index
    axis = hdr["CRVAL3"] + (pix - hdr["CRPIX3"]) * hdr["CDELT3"]
    cunit3 = str(hdr.get("CUNIT3", "m/s")).strip().lower()
    assert hdr["CTYPE3"] == "VRAD", "demo cube must have a radio-velocity axis"
    spectral = axis / 1000.0 if cunit3 in ("m/s", "m s-1", "ms-1") else axis  # → km/s

    model = np.asarray(
        tilted_ring_model(nx, ny, spectral, x0=32.0, y0=32.0, pa_deg=30.0,
                          incl_deg=60.0, vsys=0.0, vrot=150.0, rmax_pix=26.0,
                          sigma=18.0),
        dtype=np.float64)

    def moment1(c):
        m0 = c.sum(0)
        return np.where(m0 > 0.05,
                        (c * spectral[:, None, None]).sum(0) / np.maximum(m0, 1e-6),
                        np.nan)

    a, b = moment1(cube), moment1(model)
    both = np.isfinite(a) & np.isfinite(b)
    assert both.sum() > 500  # a real disk overlap region
    # Median velocity-field difference within ~one channel + injected noise.
    assert np.nanmedian(np.abs(a[both] - b[both])) < 20.0


def test_image_2d_wcs(demo_dir: Path):
    with fits.open(demo_dir / "image_2d.fits") as hdul:
        hdr = hdul[0].header
        data = hdul[0].data
    assert data.shape == (256, 256)
    assert hdr["CTYPE1"].startswith("RA")
    assert hdr["BUNIT"] == "Jy/beam"
    assert float(np.nanmax(data)) > 0.4  # the injected sources
