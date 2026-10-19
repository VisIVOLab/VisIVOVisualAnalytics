"""PV diagram geometry and spectral-axis regressions.

The "emission in the middle of the cube lands near the end of the spectral axis"
report (Serra): the client mapped rows to an ascending y range while the rows
were in channel order, so a cube with CDELT3 < 0 came out mirrored. The backend
now returns the channel coordinates themselves, channel 0 first; these tests pin
that contract, the uniform arc-length sampling, NaN handling, the refusal of
transposed cubes and the FITS export.
"""
from __future__ import annotations

import base64

import numpy as np
import pytest
from astropy.io import fits

from app.compute import compute_pv, worker_pv, worker_pv_save_fits


def _cube_header(nx: int, ny: int, nz: int, cdelt3: float, crval3: float) -> fits.Header:
    h = fits.Header()
    h["CTYPE1"], h["CUNIT1"], h["CRPIX1"], h["CRVAL1"], h["CDELT1"] = "RA---SIN", "deg", nx / 2, 150.0, -1.0 / 3600
    h["CTYPE2"], h["CUNIT2"], h["CRPIX2"], h["CRVAL2"], h["CDELT2"] = "DEC--SIN", "deg", ny / 2, 2.0, 1.0 / 3600
    h["CTYPE3"], h["CUNIT3"], h["CRPIX3"], h["CRVAL3"], h["CDELT3"] = "VRAD", "km/s", 1.0, crval3, cdelt3
    h["BUNIT"] = "Jy/beam"
    h["BMAJ"], h["BMIN"], h["BPA"] = 3.0 / 3600, 2.0 / 3600, 10.0
    h["SPECSYS"] = "BARYCENT"
    h["RESTFRQ"] = 1.420405752e9
    return h


def _write(tmp_path, data: np.ndarray, header: fits.Header, name: str = "cube.fits") -> str:
    path = tmp_path / name
    fits.PrimaryHDU(data=data.astype(np.float32), header=header).writeto(path)
    return str(path)


@pytest.fixture
def descending_cube(tmp_path):
    """40 channels, velocity DEcreasing (CDELT3 < 0); one bright source in
    channel 30 at (x=12, y=10) — off the cube's spectral centre on purpose."""
    nx, ny, nz = 32, 20, 40
    data = np.zeros((nz, ny, nx), dtype=np.float32)
    data[30, 10, 12] = 100.0
    return _write(tmp_path, data, _cube_header(nx, ny, nz, cdelt3=-2.0, crval3=500.0))


def _coords(result: dict) -> np.ndarray:
    return np.frombuffer(base64.b64decode(result["spectral_coordinates_base64"]), dtype="<f8")


def test_spectral_coordinates_follow_channel_order_on_descending_axis(descending_cube):
    r = worker_pv(descending_cube, [[2, 10], [25, 10]], 1)
    coords = _coords(r)
    assert coords.size == r["depth"] == 40
    # Channel 0 first, decreasing: CRVAL3 at channel 0, CDELT3 per channel.
    assert coords[0] == pytest.approx(500.0)
    assert coords[30] == pytest.approx(440.0)
    assert r["spectral_crval"] == pytest.approx(500.0)
    assert r["spectral_cdelt"] == pytest.approx(-2.0)


def test_emission_sits_in_its_own_channel_row(descending_cube):
    r = compute_pv(descending_cube, [[2, 10], [25, 10]], 1)
    pv = r["pv"]
    z, s = np.unravel_index(np.nanargmax(pv), pv.shape)
    assert z == 30                     # NOT mirrored to channel 9
    assert r["positions"][s] == pytest.approx(10.0)   # x=12 is 10 px from x=2
    assert r["spectral_coordinates"][z] == pytest.approx(440.0)


def test_uniform_arc_length_sampling_on_a_diagonal(descending_cube):
    # A 3-4-5 diagonal: 6 samples, 1 px apart, exactly linear offsets.
    r = compute_pv(descending_cube, [[2.0, 2.0], [5.0, 6.0]], 1)
    np.testing.assert_allclose(r["positions"], np.arange(6.0))
    assert r["total_length"] == pytest.approx(5.0)


def test_fractional_vertices_are_interpolated(tmp_path):
    nx, ny, nz = 10, 6, 3
    data = np.zeros((nz, ny, nx), dtype=np.float32)
    data[:, 3, :] = np.arange(nx, dtype=np.float32)       # value = x along row 3
    path = _write(tmp_path, data, _cube_header(nx, ny, nz, 1.0, 0.0))
    r = compute_pv(path, [[1.5, 3.0], [4.5, 3.0]], 1)
    np.testing.assert_allclose(r["pv"][0], [1.5, 2.5, 3.5, 4.5])


def test_blank_neighbour_does_not_blank_the_sample(tmp_path):
    nx, ny, nz = 8, 5, 2
    data = np.ones((nz, ny, nx), dtype=np.float32)
    data[:, 2, 5] = np.nan
    path = _write(tmp_path, data, _cube_header(nx, ny, nz, 1.0, 0.0))
    # Samples at x = 1, 2, 3, 4 on row 2: x=4 has the NaN at x=5 as a zero-weight
    # neighbour, and must keep its value.
    r = compute_pv(path, [[1.0, 2.0], [4.0, 2.0]], 1)
    assert np.all(np.isfinite(r["pv"]))


def test_transposed_cube_is_refused(tmp_path):
    nx, ny, nz = 8, 6, 5
    h = _cube_header(nx, ny, nz, 1.0, 0.0)
    # Spectral axis first: (NAXIS1 = VRAD, NAXIS2 = RA, NAXIS3 = DEC).
    t = fits.Header()
    for key_src, key_dst in ((3, 1), (1, 2), (2, 3)):
        for k in ("CTYPE", "CUNIT", "CRPIX", "CRVAL", "CDELT"):
            t[f"{k}{key_dst}"] = h[f"{k}{key_src}"]
    data = np.zeros((ny, nx, nz), dtype=np.float32)
    path = _write(tmp_path, data, t)
    with pytest.raises(ValueError, match="spectral axis"):
        compute_pv(path, [[0, 0], [3, 3]], 1)


def test_fits_export_has_wcs_and_matches_the_diagram(descending_cube, tmp_path):
    out = tmp_path / "pv.fits"
    meta = worker_pv_save_fits(descending_cube, str(out), [[2, 10], [25, 10]], 3)
    ref = compute_pv(descending_cube, [[2, 10], [25, 10]], 3)
    with fits.open(out) as hdul:
        h, data = hdul[0].header, hdul[0].data
    assert data.shape == ref["pv"].shape == (40, meta["width"])
    np.testing.assert_allclose(data, ref["pv"], equal_nan=True)
    assert h["CTYPE1"] == "OFFSET" and h["CUNIT1"] == "arcsec"
    assert h["CDELT1"] == pytest.approx(1.0, rel=1e-6)          # 1 px = 1 arcsec here
    assert h["CRPIX1"] == 1.0 and h["CRVAL1"] == 0.0
    assert h["CTYPE2"] == "VRAD" and h["CUNIT2"] == "km/s"
    assert (h["CRPIX2"], h["CRVAL2"], h["CDELT2"]) == (1.0, pytest.approx(500.0), pytest.approx(-2.0))
    assert h["BUNIT"] == "Jy/beam"
    assert h["BMAJ"] == pytest.approx(3.0 / 3600)
    assert h["SPECSYS"] == "BARYCENT"
    assert h["PVWIDTH"] == 3
    history = "\n".join(str(x) for x in h["HISTORY"])
    assert "PV vertex 0: x=2.000 y=10.000" in history and "lon=" in history


def test_fits_export_never_overwrites(descending_cube, tmp_path):
    out = tmp_path / "pv.fits"
    out.write_bytes(b"keep me")
    with pytest.raises(OSError):
        worker_pv_save_fits(descending_cube, str(out), [[2, 10], [25, 10]], 1)
    assert out.read_bytes() == b"keep me"


@pytest.mark.asyncio
async def test_pv_fits_route(client, auth_headers, descending_cube):
    opened = (await client.post("/v1/datasets/open", json={"path": descending_cube},
                                headers=auth_headers)).json()
    assert opened["valid"], opened["error"]
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    resp = await client.post("/v1/products/pv_fits", json={
        "dataset_id": opened["dataset_id"], "vertices": [[2.5, 10], [25, 10.25]],
        "width_pixels": 1, "output_basename": "serra_pv.fits"}, headers=headers)
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["workspace_filename"].startswith("serra_pv")
    assert body["height"] == 40
    pv = (await client.post("/v1/cube/pv", json={
        "dataset_id": opened["dataset_id"], "vertices": [[2.5, 10], [25, 10.25]],
        "width_pixels": 1}, headers=headers)).json()
    assert pv["valid"], pv["error"]
    assert pv["spectral_cdelt"] == pytest.approx(-2.0)
    assert _coords(pv).size == 40


def _spectral_world(header: fits.Header, axis: int, n: int) -> np.ndarray:
    """World values (SI) of pixels 0..n-1 along FITS ``axis`` of ``header``."""
    from astropy.wcs import WCS
    w = WCS(header)
    pix = [np.zeros(n) for _ in range(w.pixel_n_dim)]
    pix[axis - 1] = np.arange(n, dtype=float)
    return np.asarray(w.pixel_to_world_values(*pix)[axis - 1], dtype=float)


@pytest.mark.parametrize("variant", ["vrad", "freq_pc", "vopt_f2w"])
def test_fits_spectral_wcs_matches_the_cube_on_every_channel(tmp_path, variant):
    nx, ny, nz = 12, 10, 25
    h = _cube_header(nx, ny, nz, -2.0, 500.0)
    if variant == "freq_pc":
        h["CTYPE3"], h["CUNIT3"], h["CRPIX3"], h["CRVAL3"], h["CDELT3"] = "FREQ", "Hz", 13.0, 1.4e9, 1.0
        h["PC3_3"] = 18_500.0            # the channel width lives in the PC matrix
    elif variant == "vopt_f2w":
        # Optical velocity sampled linearly in frequency: NON-linear in velocity.
        h["CTYPE3"], h["CUNIT3"], h["CRPIX3"], h["CRVAL3"], h["CDELT3"] = "VOPT-F2W", "m/s", 1.0, 3.0e7, -3.0e6
    data = np.ones((nz, ny, nx), dtype=np.float32)
    path = _write(tmp_path, data, h)
    out = tmp_path / "pv.fits"
    worker_pv_save_fits(path, str(out), [[1, 5], [10, 5]], 1)
    with fits.open(out) as hdul:
        exported = _spectral_world(hdul[0].header, 2, nz)
    source = _spectral_world(h, 3, nz)
    np.testing.assert_allclose(exported, source, rtol=1e-9, atol=1e-6)


def test_offset_axis_is_angular_on_non_square_pixels(tmp_path):
    nx, ny, nz = 30, 30, 2
    h = _cube_header(nx, ny, nz, 1.0, 0.0)
    h["CDELT1"], h["CDELT2"] = -1.0 / 3600, 3.0 / 3600      # 1" × 3" pixels
    path = _write(tmp_path, np.ones((nz, ny, nx), dtype=np.float32), h)
    horizontal = compute_pv(path, [[2, 5], [12, 5]], 1)      # 10 px × 1"  = 10"
    vertical = compute_pv(path, [[5, 2], [5, 12]], 1)        # 10 px × 3"  = 30"
    assert horizontal["pixel_scale_arcsec"] == pytest.approx(1.0, rel=1e-3)
    assert horizontal["positions"][-1] * horizontal["pixel_scale_arcsec"] == pytest.approx(10.0, rel=1e-3)
    assert vertical["positions"][-1] * vertical["pixel_scale_arcsec"] == pytest.approx(30.0, rel=1e-3)


def test_cache_key_and_computation_share_canonical_vertices(descending_cube):
    from app.compute.pv_iso import canonical_pv_vertices
    a = [[2.0, 10.0], [4.9996, 10.0]]
    b = [[2.0, 10.0], [5.0004, 10.0]]
    assert canonical_pv_vertices(a) == canonical_pv_vertices(b)
    # Same key ⇒ same diagram, whichever path computed it first.
    assert compute_pv(descending_cube, a, 1)["pv"].shape == compute_pv(descending_cube, b, 1)["pv"].shape


def test_offset_axis_is_angular_on_a_skewed_grid(tmp_path):
    from astropy.wcs import WCS
    nx, ny, nz = 40, 40, 2
    h = _cube_header(nx, ny, nz, 1.0, 0.0)
    h["CDELT1"], h["CDELT2"] = -1.0 / 3600, 1.0 / 3600
    h["PC1_1"], h["PC1_2"], h["PC2_1"], h["PC2_2"] = 1.0, 0.6, 0.0, 1.0      # sheared axes
    path = _write(tmp_path, np.ones((nz, ny, nx), dtype=np.float32), h)
    r = compute_pv(path, [[10, 10], [10, 25]], 1)
    # True angular length of the cut from the WCS itself.
    w = WCS(h).celestial
    a, b = w.pixel_to_world(10, 10), w.pixel_to_world(10, 25)
    true_arcsec = a.separation(b).arcsec
    labelled = r["positions"][-1] * r["pixel_scale_arcsec"]
    assert labelled == pytest.approx(true_arcsec, abs=r["pixel_scale_arcsec"])
    # The client draws the band with the same matrix.
    m = np.asarray(worker_pv(path, [[10, 10], [10, 25]], 3)["sky_matrix_arcsec"]).reshape(2, 2)
    np.testing.assert_allclose(m, r["sky_matrix_arcsec"])
    assert abs(m[0, 1]) > 0.1                                             # the shear is there


def test_bilinear_reads_only_the_neighbours(tmp_path):
    """The slab handed to _bilinear_mean is indexed lazily, never converted whole."""
    from app.compute.pv_iso import _bilinear_mean

    class Lazy:
        def __init__(self, arr):
            self.arr, self.shape = arr, arr.shape

        def __getitem__(self, key):
            if isinstance(key, slice):
                return Lazy(self.arr[key])
            out = self.arr[key]
            assert out.size <= 64, "a whole plane was materialised"
            return out

        def __array__(self, *a, **k):
            raise AssertionError("the slab was converted to an array")

    arr = np.arange(3 * 50 * 60, dtype=np.float32).reshape(3, 50, 60)
    out = _bilinear_mean(Lazy(arr), np.array([1.5, 2.0]), np.array([3.0, 3.5]), 2, 1, 60, 50)
    assert out.shape == (3, 2) and np.all(np.isfinite(out))
