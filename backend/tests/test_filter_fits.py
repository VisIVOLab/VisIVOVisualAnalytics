"""Tests for smoothing and regridding — the legacy's *Filter FITS*.

Most of these are about the three things the legacy got wrong and that a user
only discovers later, in the photometry: blank pixels eating their neighbours,
a beam that no longer describes the map, and flux lost by block-averaging a
per-pixel unit.
"""
from __future__ import annotations

import numpy as np
import pytest
from astropy.io import fits

from app.compute.imutils import (
    beam_flux_scale,
    combine_for_unit,
    drop_axes,
    is_per_beam,
    fwhm_to_sigma,
    pixel_scale_degrees,
    regrid_header,
    regrid_plane,
    smooth_plane,
    widen_beam,
    worker_filter_fits,
)


def _write(path, data, **cards):
    header = fits.Header()
    header["CRPIX1"], header["CRPIX2"] = 16.0, 16.0
    header["CRVAL1"], header["CRVAL2"] = 10.0, -5.0
    header["CDELT1"], header["CDELT2"] = -0.001, 0.001
    header["CTYPE1"], header["CTYPE2"] = "RA---TAN", "DEC--TAN"
    for key, value in cards.items():
        header[key] = value
    fits.PrimaryHDU(data=np.asarray(data, dtype=np.float32), header=header).writeto(path)
    return path


# ── Smoothing ────────────────────────────────────────────────────────────────

def test_a_point_source_is_spread_but_its_flux_is_kept():
    plane = np.zeros((41, 41))
    plane[20, 20] = 1.0
    out = smooth_plane(plane, sigma_pixels=2.0)
    assert out[20, 20] < 1.0            # spread…
    assert out[20, 20] == out.max()     # …around where it was
    assert np.isclose(out.sum(), 1.0, rtol=1e-3)   # and conserved


def test_a_blank_pixel_does_not_eat_its_neighbourhood():
    """The legacy's filter turned one NaN into a NaN hole 4 sigma wide."""
    plane = np.ones((31, 31))
    plane[15, 15] = np.nan
    out = smooth_plane(plane, sigma_pixels=2.0)
    assert np.isnan(out[15, 15])                    # still blank
    assert np.isfinite(out[14, 15]) and np.isfinite(out[15, 14])
    # A flat field stays flat: the blank neither drags its neighbours down…
    neighbourhood = out[10:21, 10:21]
    assert np.allclose(neighbourhood[np.isfinite(neighbourhood)], 1.0, atol=1e-6)


def test_an_all_blank_plane_stays_blank():
    out = smooth_plane(np.full((8, 8), np.nan), sigma_pixels=1.5)
    assert np.isnan(out).all()


def test_zero_sigma_is_a_copy():
    plane = np.arange(16.0).reshape(4, 4)
    out = smooth_plane(plane, sigma_pixels=0.0)
    assert np.array_equal(out, plane)
    out[0, 0] = -1.0
    assert plane[0, 0] == 0.0    # a copy, not a view


def test_the_fwhm_to_sigma_factor():
    assert fwhm_to_sigma(2.3548200450309493) == pytest.approx(1.0, rel=1e-9)


# ── Regridding ───────────────────────────────────────────────────────────────

def test_block_mean_shrinks_and_averages():
    plane = np.array([[1.0, 3.0], [5.0, 7.0]])
    assert regrid_plane(plane, 2) == pytest.approx(np.array([[4.0]]))


def test_a_partial_block_at_the_edge_is_dropped():
    """5 pixels with a factor of 2 is two blocks and a leftover column."""
    plane = np.ones((5, 5))
    assert regrid_plane(plane, 2).shape == (2, 2)


def test_a_factor_that_leaves_nothing_is_refused():
    with pytest.raises(ValueError, match="leaves nothing"):
        regrid_plane(np.ones((3, 3)), 4)


def test_blanks_do_not_poison_a_block():
    plane = np.array([[1.0, np.nan], [3.0, 5.0]])
    assert regrid_plane(plane, 2) == pytest.approx(np.array([[3.0]]))


def test_an_entirely_blank_block_stays_blank():
    assert np.isnan(regrid_plane(np.full((2, 2), np.nan), 2)).all()


def test_a_per_pixel_unit_keeps_its_flux():
    """Averaging a Jy/pixel map loses a factor of f²: the flux is IN the pixel."""
    plane = np.ones((4, 4))
    assert regrid_plane(plane, 2, combine="sum") == pytest.approx(np.full((2, 2), 4.0))
    assert regrid_plane(plane, 2, combine="mean") == pytest.approx(np.full((2, 2), 1.0))


@pytest.mark.parametrize("unit,expected", [
    ("Jy/pixel", "sum"),
    ("Jy/pix", "sum"),
    ("Jy per pixel", "sum"),
    ("Jy/beam", "mean"),
    ("MJy/sr", "mean"),
    ("K", "mean"),
    ("", "mean"),
])
def test_the_unit_decides_how_a_block_is_combined(unit, expected):
    assert combine_for_unit(unit) == expected


def test_the_reference_pixel_moves_to_the_block_centre():
    """CRPIX/f alone puts the map half a block off — four pixels at factor 8."""
    header = fits.Header({"CRPIX1": 16.5, "CRPIX2": 16.5, "CDELT1": -0.001, "CDELT2": 0.001})
    regrid_header(header, 2)
    assert header["CRPIX1"] == pytest.approx(8.5)
    assert header["CDELT1"] == pytest.approx(-0.002)
    assert header["CDELT2"] == pytest.approx(0.002)


def test_a_cd_matrix_is_scaled_too():
    header = fits.Header({"CRPIX1": 8.5, "CRPIX2": 8.5,
                          "CD1_1": -0.001, "CD1_2": 0.0, "CD2_1": 0.0, "CD2_2": 0.001})
    regrid_header(header, 4)
    assert header["CD1_1"] == pytest.approx(-0.004)
    assert header["CD2_2"] == pytest.approx(0.004)


def test_a_pc_matrix_is_not_scaled_with_cdelt():
    """PC is the dimensionless rotation that MULTIPLIES CDELT. Scaling both
    applies the factor twice: 0.001°/px at factor 2 became 0.004, not 0.002."""
    from astropy.wcs import WCS

    header = fits.Header({
        "WCSAXES": 2, "CTYPE1": "RA---TAN", "CTYPE2": "DEC--TAN",
        "CRPIX1": 8.5, "CRPIX2": 8.5, "CRVAL1": 10.0, "CRVAL2": -5.0,
        "CDELT1": -0.001, "CDELT2": 0.001,
        "PC1_1": 1.0, "PC1_2": 0.0, "PC2_1": 0.0, "PC2_2": 1.0,
    })
    regrid_header(header, 2)
    assert header["PC1_1"] == pytest.approx(1.0)
    scale = np.abs(WCS(header).pixel_scale_matrix)
    assert scale.max() == pytest.approx(0.002)


def test_a_rotated_pc_matrix_keeps_its_rotation():
    header = fits.Header({"CDELT1": -0.001, "CDELT2": 0.001,
                          "PC1_1": 0.0, "PC1_2": 1.0, "PC2_1": -1.0, "PC2_2": 0.0})
    regrid_header(header, 4)
    assert header["PC1_2"] == pytest.approx(1.0)
    assert header["PC2_1"] == pytest.approx(-1.0)


def test_a_spectral_third_axis_is_left_alone():
    header = fits.Header({"CRPIX1": 8.5, "CRPIX2": 8.5, "CRPIX3": 1.0,
                          "CDELT1": -0.001, "CDELT2": 0.001, "CDELT3": 1000.0})
    regrid_header(header, 2)
    assert header["CRPIX3"] == 1.0
    assert header["CDELT3"] == 1000.0


# ── Squeezed axes ────────────────────────────────────────────────────────────

def test_a_squeezed_stokes_axis_takes_its_wcs_keywords_with_it():
    """A 2-D image that still carries CTYPE3/CTYPE4 and WCSAXES=4 describes a
    four-axis WCS over a plane, and astropy duly builds one."""
    header = fits.Header({
        "WCSAXES": 4,
        "CTYPE1": "RA---TAN", "CRPIX1": 8.0, "CDELT1": -0.001, "CRVAL1": 10.0,
        "CTYPE2": "DEC--TAN", "CRPIX2": 8.0, "CDELT2": 0.001, "CRVAL2": -5.0,
        "CTYPE3": "FREQ", "CRPIX3": 1.0, "CDELT3": 1e6, "CRVAL3": 1.4e9,
        "CTYPE4": "STOKES", "CRPIX4": 1.0, "CDELT4": 1.0, "CRVAL4": 1.0,
    })
    drop_axes(header, [1, 2])
    assert header["WCSAXES"] == 2
    assert "CTYPE3" not in header and "CTYPE4" not in header
    assert header["CTYPE1"] == "RA---TAN" and header["CTYPE2"] == "DEC--TAN"


def test_renumbering_does_not_overwrite_an_axis_that_has_not_moved_yet():
    """Keeping axes 2 and 3 means 2→1 and 3→2; doing it in place in ascending
    order writes over 2 before it has been read."""
    header = fits.Header({
        "WCSAXES": 3,
        "CTYPE1": "STOKES", "CRVAL1": 1.0,
        "CTYPE2": "RA---TAN", "CRVAL2": 10.0,
        "CTYPE3": "DEC--TAN", "CRVAL3": -5.0,
    })
    drop_axes(header, [2, 3])
    assert header["CTYPE1"] == "RA---TAN"
    assert header["CTYPE2"] == "DEC--TAN"
    assert "CTYPE3" not in header


def test_a_cd_matrix_is_renumbered_with_its_axes():
    header = fits.Header({
        "WCSAXES": 3,
        "CD1_1": -0.001, "CD2_2": 0.001, "CD3_3": 1.0, "CD1_2": 0.0002,
    })
    drop_axes(header, [1, 2])
    assert header["CD1_1"] == pytest.approx(-0.001)
    assert header["CD1_2"] == pytest.approx(0.0002)
    assert "CD3_3" not in header


def test_nothing_is_dropped_when_every_axis_survives():
    header = fits.Header({"WCSAXES": 2, "CTYPE1": "RA---TAN", "CTYPE2": "DEC--TAN"})
    drop_axes(header, [1, 2])
    assert header["CTYPE1"] == "RA---TAN" and header["CTYPE2"] == "DEC--TAN"


def test_the_written_file_has_a_two_axis_wcs(tmp_path):
    """End to end: the degenerate-axis case, through the worker."""
    from astropy.wcs import WCS

    data = np.zeros((1, 1, 16, 16), dtype=np.float32)
    header = fits.Header({
        "WCSAXES": 4,
        "CTYPE1": "RA---TAN", "CRPIX1": 8.0, "CDELT1": -0.001, "CRVAL1": 10.0,
        "CTYPE2": "DEC--TAN", "CRPIX2": 8.0, "CDELT2": 0.001, "CRVAL2": -5.0,
        "CTYPE3": "FREQ", "CRPIX3": 1.0, "CDELT3": 1e6, "CRVAL3": 1.4e9,
        "CTYPE4": "STOKES", "CRPIX4": 1.0, "CDELT4": 1.0, "CRVAL4": 1.0,
    })
    source = tmp_path / "stokes.fits"
    fits.PrimaryHDU(data=data, header=header).writeto(source)
    out = tmp_path / "stokes_out.fits"
    worker_filter_fits(str(source), str(out), 1.5, 1)
    with fits.open(out) as hdul:
        wcs = WCS(hdul[0].header)
        assert wcs.naxis == 2
        assert wcs.has_celestial


# ── The beam ─────────────────────────────────────────────────────────────────

def test_the_beam_grows_in_quadrature():
    header = fits.Header({"BMAJ": 0.003, "BMIN": 0.004})
    widen_beam(header, 0.004)
    assert header["BMAJ"] == pytest.approx(0.005)      # 3-4-5
    assert header["BMIN"] == pytest.approx(np.hypot(0.004, 0.004))


def test_a_map_without_a_beam_gains_none():
    header = fits.Header({"CRPIX1": 1.0})
    widen_beam(header, 0.004)
    assert "BMAJ" not in header


def test_the_pixel_scale_comes_from_cdelt_or_the_cd_matrix():
    assert pixel_scale_degrees(fits.Header({"CDELT1": -0.002, "CDELT2": 0.002})) \
        == pytest.approx(0.002)
    assert pixel_scale_degrees(fits.Header({"CD1_1": -0.002, "CD2_2": 0.002})) \
        == pytest.approx(0.002)
    assert pixel_scale_degrees(fits.Header()) == 0.0


def test_the_pixel_scale_of_a_rotated_cd_matrix_is_not_on_its_diagonal():
    """A 90°-rotated field has zeros on the diagonal. Reading those reports a
    pixel scale of 0, which silently skips the beam widening and tells the user
    the kernel was 0 arcseconds wide."""
    rotated = fits.Header({"CD1_1": 0.0, "CD1_2": -0.002, "CD2_1": 0.002, "CD2_2": 0.0})
    assert pixel_scale_degrees(rotated) == pytest.approx(0.002)


def test_a_degenerate_axis_takes_its_projection_parameters_with_it():
    """PV2_1 belongs to axis 2; left behind after a squeeze it describes a
    different projection."""
    header = fits.Header({
        "WCSAXES": 3,
        "CTYPE1": "STOKES", "CTYPE2": "RA---SIN", "CTYPE3": "DEC--SIN",
        "PV2_1": 0.5, "PV3_1": 0.25,
    })
    drop_axes(header, [2, 3])
    assert header["PV1_1"] == pytest.approx(0.5)
    assert header["PV2_1"] == pytest.approx(0.25)
    assert "PV3_1" not in header


def test_axes_are_renumbered_even_when_wcsaxes_is_smaller_than_naxis():
    """A header may describe fewer WCS axes than the data has; comparing the
    kept count to WCSAXES alone returns early and leaves the stale keywords."""
    header = fits.Header({
        "NAXIS": 3, "WCSAXES": 2,
        "CTYPE1": "STOKES", "CTYPE2": "RA---TAN", "CTYPE3": "DEC--TAN",
    })
    drop_axes(header, [2, 3])
    assert header["CTYPE1"] == "RA---TAN"
    assert header["CTYPE2"] == "DEC--TAN"
    assert "CTYPE3" not in header


# ── Flux, after the beam grows ───────────────────────────────────────────────

@pytest.mark.parametrize("unit,expected", [
    ("Jy/beam", True), ("mJy/beam", True), ("Jy / beam", True),
    ("Jy/pixel", False), ("MJy/sr", False), ("K", False), ("", False),
])
def test_per_beam_units_are_recognised(unit, expected):
    assert is_per_beam(unit) is expected


def test_the_pixels_are_scaled_with_the_beam():
    """Convolving preserves the SUM of the pixels; in Jy/beam the integrated
    flux is that sum divided by the beam area, so a wider beam means the same
    numbers describe less flux. A source smoothed with a kernel equal to its own
    beam doubles the beam area — and would appear to lose half its flux."""
    assert beam_flux_scale("Jy/beam", 0.001, 0.001, 0.002, 0.002) == pytest.approx(4.0)
    assert beam_flux_scale("Jy/beam", 0.001, 0.001,
                           0.001 * np.sqrt(2), 0.001 * np.sqrt(2)) == pytest.approx(2.0)


@pytest.mark.parametrize("unit", ["Jy/pixel", "MJy/sr", "K", ""])
def test_other_units_are_not_scaled(unit):
    """Per-pixel values keep their sum; surface brightness is per solid angle
    and the convolution does not change it."""
    assert beam_flux_scale(unit, 0.001, 0.001, 0.002, 0.002) == 1.0


def test_a_map_without_a_beam_is_not_scaled():
    assert beam_flux_scale("Jy/beam", None, None, None, None) == 1.0


def test_smoothing_a_jy_per_beam_map_conserves_the_integrated_flux(tmp_path):
    """End to end, the measurement that matters: sum / beam-area before and
    after."""
    data = np.zeros((64, 64))
    data[32, 32] = 1.0
    source = _write(tmp_path / "in.fits", data, BUNIT="Jy/beam",
                    BMAJ=0.002, BMIN=0.002, BPA=0.0)
    out = tmp_path / "out.fits"
    result = worker_filter_fits(str(source), str(out), 4.0, 1)

    with fits.open(source) as hdul:
        before = np.nansum(hdul[0].data) / (hdul[0].header["BMAJ"] * hdul[0].header["BMIN"])
    with fits.open(out) as hdul:
        after = np.nansum(hdul[0].data) / (hdul[0].header["BMAJ"] * hdul[0].header["BMIN"])
    assert after == pytest.approx(before, rel=1e-3)
    assert result["flux_scale"] > 1.0


def test_smoothing_a_jy_per_pixel_map_conserves_the_sum(tmp_path):
    data = np.zeros((64, 64))
    data[32, 32] = 1.0
    source = _write(tmp_path / "in.fits", data, BUNIT="Jy/pixel")
    out = tmp_path / "out.fits"
    result = worker_filter_fits(str(source), str(out), 4.0, 1)
    with fits.open(out) as hdul:
        assert np.nansum(hdul[0].data) == pytest.approx(1.0, rel=1e-3)
    assert result["flux_scale"] == 1.0


# ── End to end ───────────────────────────────────────────────────────────────

def test_both_steps_together(tmp_path):
    data = np.zeros((32, 32))
    data[16, 16] = 10.0
    source = _write(tmp_path / "in.fits", data, BUNIT="Jy/beam", BMAJ=0.002, BMIN=0.002)
    out = tmp_path / "out.fits"
    result = worker_filter_fits(str(source), str(out), 2.0, 2)

    assert (result["width"], result["height"]) == (16, 16)
    assert result["combine"] == "mean"
    with fits.open(out) as hdul:
        header = hdul[0].header
        assert header["CDELT2"] == pytest.approx(0.002)
        assert header["BMAJ"] > 0.002                    # widened
        assert header["NAXIS1"] == 16
        history = " ".join(str(card) for card in header["HISTORY"])
        assert "smooth" in history.lower() and "regrid" in history.lower()


def test_a_cube_is_filtered_plane_by_plane(tmp_path):
    data = np.zeros((4, 16, 16))
    data[:, 8, 8] = 1.0
    source = _write(tmp_path / "cube.fits", data)
    out = tmp_path / "cube_out.fits"
    result = worker_filter_fits(str(source), str(out), 1.5, 2)
    assert (result["depth"], result["height"], result["width"]) == (4, 8, 8)
    with fits.open(out) as hdul:
        assert hdul[0].data.shape == (4, 8, 8)


def test_the_spectral_axis_is_not_smoothed(tmp_path):
    """A line in one channel must not appear in its neighbours."""
    data = np.zeros((5, 16, 16))
    data[2] = 1.0
    source = _write(tmp_path / "line.fits", data)
    out = tmp_path / "line_out.fits"
    worker_filter_fits(str(source), str(out), 2.0, 1)
    with fits.open(out) as hdul:
        assert hdul[0].data[1].max() == pytest.approx(0.0)
        assert hdul[0].data[2].max() > 0.0


def test_doing_nothing_is_refused(tmp_path):
    source = _write(tmp_path / "in.fits", np.ones((8, 8)))
    with pytest.raises(ValueError, match="Nothing to do"):
        worker_filter_fits(str(source), str(tmp_path / "out.fits"), 0.0, 1)


def test_an_absurd_regrid_factor_is_refused(tmp_path):
    source = _write(tmp_path / "in.fits", np.ones((8, 8)))
    with pytest.raises(ValueError, match="above 64"):
        worker_filter_fits(str(source), str(tmp_path / "out.fits"), 0.0, 100)


def test_a_table_only_fits_is_refused(tmp_path):
    path = tmp_path / "table.fits"
    column = fits.Column(name="X", format="E", array=np.array([1.0]))
    fits.HDUList([fits.PrimaryHDU(), fits.BinTableHDU.from_columns([column])]).writeto(path)
    with pytest.raises(ValueError, match="no image data"):
        worker_filter_fits(str(path), str(tmp_path / "out.fits"), 1.0, 1)


def test_a_degenerate_stokes_axis_does_not_make_it_four_dimensional(tmp_path):
    data = np.zeros((1, 1, 16, 16))
    source = _write(tmp_path / "stokes.fits", data)
    out = tmp_path / "stokes_out.fits"
    result = worker_filter_fits(str(source), str(out), 1.0, 1)
    assert (result["width"], result["height"], result["depth"]) == (16, 16, 1)


@pytest.mark.asyncio
async def test_the_route_writes_into_the_workspace(client, auth_headers, tmp_path):
    data = np.zeros((16, 16))
    data[8, 8] = 1.0
    source = _write(tmp_path / "map.fits", data)
    response = await client.post("/v1/image/filter",
                                 json={"path": str(source), "smooth_fwhm_pixels": 2.0},
                                 headers=auth_headers)
    body = response.json()
    assert body["valid"] is True
    assert body["workspace_filename"] == "map_smooth.fits"
    assert body["smooth_fwhm_arcsec"] == pytest.approx(2.0 * 0.001 * 3600.0)


@pytest.mark.asyncio
async def test_the_route_hides_whether_a_path_exists(client, auth_headers, tmp_path):
    response = await client.post("/v1/image/filter",
                                 json={"path": str(tmp_path / "absent.fits"),
                                       "regrid_factor": 2},
                                 headers=auth_headers)
    body = response.json()
    assert body["valid"] is False
    assert body["error"] == "File not found."
    # Nothing was computed, so nothing is reported.
    assert body["output_path"] == "" and body["workspace_filename"] == ""
    assert (body["width"], body["height"]) == (0, 0)
