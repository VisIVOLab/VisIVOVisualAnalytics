"""A 3-component vector field is told apart from a cube and from a Stokes cube."""
import numpy as np
from astropy.io import fits

from app.classify import classify_path


def _write(path, shape, ctype4=None):
    h = fits.Header()
    if ctype4:
        h["CTYPE4"] = ctype4
    fits.PrimaryHDU(np.zeros(shape, dtype=np.float32), header=h).writeto(path)


def test_three_components_on_a_grid_is_a_velocity_field(tmp_path):
    p = tmp_path / "flow.fits"
    _write(p, (3, 8, 8, 8))
    v = classify_path(p)
    assert (v["kind"], v["opener"]) == ("velocity-field", "velocity")


def test_a_stokes_axis_of_three_is_not(tmp_path):
    p = tmp_path / "iqu.fits"
    _write(p, (3, 8, 8, 8), ctype4="STOKES")
    assert classify_path(p)["opener"] == "dataset"


def test_a_cube_is_still_a_cube(tmp_path):
    p = tmp_path / "cube.fits"
    _write(p, (8, 8, 8))
    assert classify_path(p)["kind"] == "cube"


def test_a_time_axis_of_three_is_not(tmp_path):
    p = tmp_path / "t.fits"
    _write(p, (3, 8, 8, 8), ctype4="TIME")
    assert classify_path(p)["opener"] != "velocity"


def test_a_spectral_grid_axis_is_not(tmp_path):
    p = tmp_path / "e.fits"
    h = fits.Header()
    h["CTYPE3"], h["CUNIT3"] = "ENER", "eV"
    fits.PrimaryHDU(np.zeros((3, 8, 8, 8), dtype=np.float32), header=h).writeto(p)
    assert classify_path(p)["opener"] != "velocity"
