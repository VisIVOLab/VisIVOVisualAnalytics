"""The standard HI measurements of a selection (issue #280): the definitions on
synthetic cubes, and SoFiA-2's catalogue reproduced from its own masks on
HCG 44 and WALLABY (skipped when the cubes or SoFiA-2's output are absent).

The real-data test sends each catalogued source's own mask through
``/v1/cube/mask_stats_bin`` and compares with the catalogue row. SoFiA-2 was
run with ``parameter.physical = false``, so its catalogue is in native units
(Jy/beam sums, channels, pixels); the test converts it as SoFiA-2 itself does
with ``physical = true`` (flux x |channel width| / beam area, err x |channel
width| / sqrt(beam area), widths x |channel width|). Tolerances and the
convention differences they absorb:

* sums, peak, centroid, widths and the moment-0 ellipse: equal to the
  catalogue's precision (same voxels, same formulae);
* the local noise: within 10 % — SoFiA-2 leaves out every catalogued source's
  voxels in the frame around a source, the route only the selection's (it
  knows no other source); with SoFiA-2's rms supplied as ``sigma`` the flux
  uncertainty and peak S/N are exact;
* RA/Dec within 0.1 arcsec: the route answers in ICRS, SoFiA-2 in the cube's
  own frame (FK5 J2000 here, about 0.03 arcsec away).
"""
from __future__ import annotations

import math
import os
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits
from httpx import AsyncClient

from app.compute.hi_measures import (channel_rho, correlation_factor, flux_uncertainty, line_widths,
                                     m0_ellipse, mad_about_zero, sky_position_angle, spectral_frame)

REPO = Path(__file__).resolve().parents[2]
SOFIA_OUT = Path(os.environ.get("VISIVO_SOFIA_OUT", REPO / "validation" / "sofia_out"))
DEMO = Path("/Users/fvitello/downloads/DemoData")
# The cubes SoFiA-2 was run on (validation/sofia_*.par: /data/hgc44_3axes.fits,
# /data/WALLABY.fits): the WALLABY catalogue's positions are in that file's
# header, which differs from the CASDA cubelet's.
CUBES = {
    "hgc44": Path(os.environ.get("VISIVO_VALIDATION_CUBE", DEMO / "hgc44_3axes.fits")),
    "wallaby": Path(os.environ.get("VISIVO_VALIDATION_WALLABY_SOFIA_CUBE", DEMO / "WALLABY.fits")),
}


# ── Definitions ──────────────────────────────────────────────────────────────

def test_line_widths_interpolate_from_the_outside_in() -> None:
    """SoFiA-2's rule: the first channel at or above the level from each end,
    the crossing interpolated with the channel outside it."""
    s = np.array([0.0, 1.0, 4.0, 10.0, 6.0, 2.0, 0.0, 3.0, 0.0])   # peak 10; a 3 beyond a dip
    v = 1000.0 + 5.0 * np.arange(s.size)
    w = line_widths(s, v)
    # 20 %: level 2. Low side: channel 1 (1) -> 2 (4): crossing 2 - (4-2)/(4-1) = 1.333.
    # High side: the last channel >= 2 is 7 (3); 7 + (3-2)/(3-0) = 7.333.
    assert w["w20"] == pytest.approx(5.0 * (7.0 + 1 / 3 - (2.0 - 2 / 3)))
    assert w["vsys"] == pytest.approx(1000.0 + 5.0 * 0.5 * ((2.0 - 2 / 3) + (7.0 + 1 / 3)))
    # 50 %: level 5. 3 - (10-5)/(10-4) and 4 + (6-5)/(6-2).
    assert w["w50"] == pytest.approx(5.0 * ((4.0 + 0.25) - (3.0 - 5 / 6)))
    # A decreasing velocity axis gives the same widths; blank channels are zero.
    assert line_widths(s, v[::-1])["w20"] == pytest.approx(w["w20"])
    s2 = s.copy()
    s2[0] = np.nan
    assert line_widths(s2, v)["w20"] == pytest.approx(w["w20"])
    assert math.isnan(line_widths(np.zeros(4), v[:4])["w20"])


def test_m0_ellipse_is_sofias_semi_axes() -> None:
    """A uniform ellipse of semi-axes 36 and 15 at PA 30 deg (from +y towards
    -x) gives 2 sqrt(lambda) = its semi-axes (to the pixelisation)."""
    yy, xx = np.mgrid[0:81, 0:81].astype(float)
    pa = math.radians(30.0)
    # Major axis along (-sin pa, cos pa): from +y towards -x.
    u = -(xx - 40) * math.sin(pa) + (yy - 40) * math.cos(pa)
    w = (xx - 40) * math.cos(pa) + (yy - 40) * math.sin(pa)
    m0 = ((u / 36.0) ** 2 + (w / 15.0) ** 2 <= 1.0).astype(float)
    m0[0, 0] = -3.0                                   # a negative pixel is left out
    e = m0_ellipse(m0, (40.0, 40.0))
    assert e["major_px"] == pytest.approx(36.0, rel=0.01)
    assert e["minor_px"] == pytest.approx(15.0, rel=0.01)
    assert e["pa_grid_deg"] == pytest.approx(30.0, abs=0.3)
    assert e["note"] == ""
    assert m0_ellipse(np.zeros((3, 3)), (1.0, 1.0)) is None


def test_m0_ellipse_refuses_degenerate_footprints() -> None:
    """A one-pixel-wide line has no size; a round footprint no position angle."""
    line = np.zeros((9, 9))
    line[4, 1:8] = 1.0
    e = m0_ellipse(line, (4.0, 4.0))
    assert e["major_px"] is None and e["minor_px"] is None and e["pa_grid_deg"] is None
    assert "narrower than a pixel" in e["note"]
    yy, xx = np.mgrid[0:41, 0:41]
    disc = (((xx - 20) ** 2 + (yy - 20) ** 2) <= 15 ** 2).astype(float)
    e = m0_ellipse(disc, (20.0, 20.0))
    assert e["major_px"] == pytest.approx(15.0, rel=0.02) and e["pa_grid_deg"] is None
    assert "round" in e["note"]


def test_line_widths_flag_a_line_cut_at_the_edge() -> None:
    v = np.arange(6) * 5.0
    assert not line_widths(np.array([0.0, 2.0, 10.0, 8.0, 1.0, 0.0]), v)["edge20"]
    w = line_widths(np.array([6.0, 9.0, 10.0, 4.0, 1.0, 0.0]), v)
    assert w["edge20"] and w["edge50"]


def test_sky_position_angle_follows_a_tilted_north() -> None:
    """On a grid rotated by 20 deg, the grid's +y is not north: the major axis
    along +y lies at PA 20 (or 160) east of north, as the WCS says."""
    from astropy.wcs import WCS
    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crval = [150.0, 20.0]
    w.wcs.crpix = [50.0, 50.0]
    rot = math.radians(20.0)
    s = 1.0 / 3600
    w.wcs.cd = [[-s * math.cos(rot), s * math.sin(rot)], [s * math.sin(rot), s * math.cos(rot)]]

    def to_sky(x, y):
        return tuple(float(v) for v in w.all_pix2world([[x, y]], 0)[0])

    pa = sky_position_angle(to_sky, 49.0, 49.0, math.radians(90.0))
    # The +y pixel direction: computed independently from two sky points.
    ra0, dec0 = to_sky(49.0, 49.0)
    ra1, dec1 = to_sky(49.0, 50.0)
    want = math.degrees(math.atan2((ra1 - ra0) * math.cos(math.radians(dec0)), dec1 - dec0)) % 180.0
    assert pa == pytest.approx(want, abs=0.01)
    assert min(abs(pa - 20.0), abs(pa - 160.0)) < 0.1
    # An untilted grid (north up, east left): the grid PA.
    w.wcs.cd = [[-s, 0.0], [0.0, s]]
    assert sky_position_angle(to_sky, 49.0, 49.0, math.radians(120.0)) == pytest.approx(30.0, abs=0.01)
    assert sky_position_angle(lambda x, y: None, 1.0, 1.0, 0.0) is None


def test_channel_correlation_from_noise_pairs() -> None:
    """Hanning-smoothed noise: rho1 = 2/3, rho2 = 1/6, rho3 = 0, measured from
    lag differences; the factor on a 20-channel sum is the variance's."""
    rng = np.random.default_rng(5)
    white = rng.normal(0.0, 1.0, (400_000 + 2,))
    noise = 0.25 * white[:-2] + 0.5 * white[1:-1] + 0.25 * white[2:]
    sigma = mad_about_zero(noise)
    rho = channel_rho(sigma, [noise[j:] - noise[:-j] for j in (1, 2, 3)])
    assert rho[0] == pytest.approx(2 / 3, abs=0.01)
    assert rho[1] == pytest.approx(1 / 6, abs=0.01)
    assert rho[2] == pytest.approx(0.0, abs=0.01)
    f = correlation_factor(rho, 20)
    sums = np.add.reduceat(noise[: 20 * 20_000], np.arange(0, 20 * 20_000, 20))
    assert f == pytest.approx(float(np.std(sums)) / (sigma * math.sqrt(20)), rel=0.03)
    assert correlation_factor([None, None, None], 20) == 1.0
    assert correlation_factor(rho, 1) == 1.0
    assert channel_rho(1.0, [np.ones(10)]) == [None], "too few pairs"


def test_spectral_frame_names() -> None:
    assert spectral_frame("BARYCENT", "VOPT") == ("barycentric", "SPECSYS BARYCENT")
    assert spectral_frame("", "VELO-HEL")[0] == "barycentric (heliocentric)"
    assert spectral_frame(None, "FELO-LSR")[0] == "LSRK"
    assert spectral_frame(None, "FREQ-OBS")[0] == "topocentric"
    assert spectral_frame("TOPOCENT", "FREQ")[0] == "topocentric"
    assert spectral_frame(None, "FREQ") == ("", "")
    assert spectral_frame(None, "VELOCITY") == ("", "")


def test_velocity_convention_labels() -> None:
    from app.compute.kinematic_fit import velocity_convention_label
    from app.compute.sofia_suggest import velocity_convention
    assert velocity_convention("VELOCITY")[1].endswith("convention not stated: taken as optical")
    assert "AIPS" not in velocity_convention("VELOCITY")[1]
    assert velocity_convention("VELO")[0] == "relativistic"
    assert velocity_convention_label("Hz", "FREQ", {})[1] == "optical (cz), from frequency and RESTFRQ"
    assert velocity_convention_label("m/s", "VELO-HEL", {"VELREF": 258}) == \
        ("radio", "radio (AIPS VELO-HEL, VELREF=258)")


def test_noise_frame_plan_stays_within_its_budget() -> None:
    from app.compute.mask_stats import noise_frame_plan
    s, planes = noise_frame_plan((60, 50, 50), budget=1_000_000)
    assert s == 1 and planes == list(range(60))
    s, planes = noise_frame_plan((300, 2000, 2000), budget=4_000_000)
    per_plane = (-(-2000 // s)) ** 2
    assert per_plane * len(planes) <= 4_000_000 and planes == list(range(300))
    # More planes than the budget allows at one pixel each: runs of 4 consecutive planes.
    s, planes = noise_frame_plan((100_000, 40, 40), budget=200_000)
    per_plane = (-(-40 // s)) ** 2
    assert per_plane * len(planes) <= 200_000
    assert len(planes) >= 8 and planes[:4] == [planes[0] + i for i in range(4)]
    assert planes[-1] > 90_000, "the runs are spread over the frame"


def test_meta_sigma_survives_huge_numbers() -> None:
    from app.routers.share import meta_sigma
    assert meta_sigma({"grow": {"sigma": 10 ** 400}}) == 0.0
    assert meta_sigma({"grow": {"sigma": 0.002}}) == 0.002
    assert meta_sigma({"grow": {"sigma": True}}) == 0.0
    assert meta_sigma({}) == 0.0


def test_flux_uncertainty_and_noise() -> None:
    # Uniform axis: sigma sqrt(N) dv / sqrt(Omega).
    assert flux_uncertainty(0.002, np.array([10, 20, 30]), np.full(3, 4.0), np.full(3, 25.0)) == \
        pytest.approx(0.002 * math.sqrt(60) * 4.0 / 5.0)
    rng = np.random.default_rng(1)
    noise = rng.normal(0.0, 2.0, 400_000)
    assert mad_about_zero(np.concatenate([noise, [np.nan, np.inf]])) == pytest.approx(2.0, rel=0.01)
    assert mad_about_zero(np.array([np.nan])) is None


def _disc_cube(path: Path) -> tuple[np.ndarray, dict]:
    """A rotating disc in noise: 40 x 48 x 48 (z, y, x), VOPT axis 5 km/s
    channels from 900 km/s, a 6 arcsec beam on 2 arcsec pixels."""
    rng = np.random.default_rng(7)
    nz, ny, nx = 40, 48, 48
    data = rng.normal(0.0, 0.001, (nz, ny, nx)).astype(np.float32)
    zz, yy, xx = np.mgrid[0:nz, 0:ny, 0:nx]
    vz = 20.0 + 6.0 * (xx - 24) / 10.0                  # a velocity gradient along x
    disc = np.exp(-(((xx - 24) / 9.0) ** 2 + ((yy - 22) / 4.0) ** 2))
    data += (0.02 * disc * np.exp(-0.5 * ((zz - vz) / 2.0) ** 2)).astype(np.float32)
    h = fits.Header()
    h.update({"CTYPE1": "RA---SIN", "CTYPE2": "DEC--SIN", "CTYPE3": "VOPT",
              "CRVAL1": 150.0, "CRVAL2": -30.0, "CRVAL3": 900e3, "CRPIX1": 25.0, "CRPIX2": 25.0,
              "CRPIX3": 1.0, "CDELT1": -2 / 3600, "CDELT2": 2 / 3600, "CDELT3": 5000.0,
              "CUNIT1": "deg", "CUNIT2": "deg", "CUNIT3": "m/s", "BUNIT": "Jy/beam",
              "BMAJ": 6 / 3600, "BMIN": 6 / 3600, "BPA": 0.0, "SPECSYS": "BARYCENT",
              "RADESYS": "ICRS"})
    fits.PrimaryHDU(data, header=h).writeto(path, overwrite=True)
    return data, h


async def _open(client: AsyncClient, auth_headers: dict, path: Path) -> dict:
    r = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"], body["error"]
    return {"dataset_id": body["dataset_id"],
            "headers": {**auth_headers, "X-Visivo-Session": body["session_id"],
                        "Content-Type": "application/octet-stream"}}


async def _stats(client, ds, mask: np.ndarray, origin_xyz, **params) -> dict:
    d, h, w = mask.shape
    r = await client.post(
        "/v1/cube/mask_stats_bin",
        params={"dataset_id": ds["dataset_id"], "origin_x": origin_xyz[0], "origin_y": origin_xyz[1],
                "origin_z": origin_xyz[2], "width": w, "height": h, "depth": d, **params},
        content=mask.astype("<f4").tobytes(), headers=ds["headers"])
    assert r.status_code == 200, r.text
    return r.json()


@pytest.mark.asyncio
async def test_hi_measurements_of_a_disc(client, auth_headers, tmp_path) -> None:
    path = tmp_path / "disc.fits"
    data, h = _disc_cube(path)
    ds = await _open(client, auth_headers, path)
    full = data > 0.004                                   # the disc, a 4 sigma cut
    zs, ys, xs = (np.nonzero(full.any(a))[0] for a in ((1, 2), (0, 2), (0, 1)))
    box = full[zs[0]:zs[-1] + 1, ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1]
    s = await _stats(client, ds, box, (xs[0], ys[0], zs[0]))
    hi = s["hi"]
    vel = 900.0 + 5.0 * np.arange(data.shape[0])
    beam_px = math.pi / (4 * math.log(2)) * 9.0
    sel = full & np.isfinite(data)
    # Flux and its uncertainty from the local noise (the input noise is 1 mJy/beam).
    assert hi["flux_jy_kms"] == pytest.approx(float(data[sel].sum()) * 5.0 / beam_px, rel=1e-5)
    assert hi["noise_sigma"] == pytest.approx(0.001, rel=0.05)
    assert hi["noise_source"].startswith("local") and hi["noise_voxels"] > 10_000
    assert hi["flux_err_indep_jy_kms"] == pytest.approx(
        hi["noise_sigma"] * math.sqrt(sel.sum()) * 5.0 / math.sqrt(beam_px), rel=1e-6)
    # White noise: the measured channel correlation is small, the factor near 1
    # (the disc's faint wings outside the 4 sigma cut correlate a little).
    assert len(hi["channel_rho"]) == 3 and abs(hi["channel_rho"][0]) < 0.1
    assert hi["flux_err_factor"] == pytest.approx(1.0, abs=0.1)
    assert hi["flux_err_jy_kms"] == pytest.approx(hi["flux_err_indep_jy_kms"] * hi["flux_err_factor"])
    assert hi["channel_correlation"].startswith("channel")
    assert hi["snr_integrated"] == pytest.approx(hi["flux_jy_kms"] / hi["flux_err_jy_kms"])
    assert hi["peak"] == pytest.approx(float(data[sel].max()), rel=1e-6)
    assert hi["peak_snr"] == pytest.approx(hi["peak"] / hi["noise_sigma"])
    assert hi["peak_unit"] == "Jy/beam" and hi["noise_unit"] == "Jy/beam"
    # Widths from the integrated spectrum, the systemic velocity at its W20 midpoint.
    spec = np.where(sel, data, 0.0).sum((1, 2)) / beam_px
    w = line_widths(spec, vel)
    assert hi["w20_kms"] == pytest.approx(w["w20"], rel=1e-6)
    assert hi["w50_kms"] == pytest.approx(w["w50"], rel=1e-6)
    assert hi["vsys_kms"] == pytest.approx(1000.0, abs=2.0)        # channel 20 of the model
    assert hi["velocity_convention"].startswith("optical") and hi["velocity_convention_name"] == "optical"
    assert hi["velocity_frame"] == "barycentric" and hi["velocity_frame_source"] == "SPECSYS BARYCENT"
    # The flux-weighted centroid of the positive selected voxels.
    pos = np.where(sel & (data > 0), data, 0.0).astype(np.float64)
    zz, yy, xx = np.indices(data.shape)
    assert hi["centroid_x"] == pytest.approx((pos * xx).sum() / pos.sum(), rel=1e-9)
    assert hi["centroid_y"] == pytest.approx((pos * yy).sum() / pos.sum(), rel=1e-9)
    assert hi["centroid_z"] == pytest.approx((pos * zz).sum() / pos.sum(), rel=1e-9)
    assert hi["centroid_velocity_kms"] == pytest.approx((pos.sum((1, 2)) * vel).sum() / pos.sum(), rel=1e-9)
    from astropy.wcs import WCS
    ra, dec = WCS(h).celestial.all_pix2world([[hi["centroid_x"], hi["centroid_y"]]], 0)[0]
    assert hi["centroid_frame"] == "ICRS"
    assert hi["centroid_ra_deg"] == pytest.approx(ra, abs=1e-9)
    assert hi["centroid_dec_deg"] == pytest.approx(dec, abs=1e-9)
    # Elongated along x (east-west): PA 90 deg, on the grid and on the sky (north up).
    assert hi["pa_grid_deg"] == pytest.approx(90.0, abs=3.0)
    assert hi["pa_deg"] == pytest.approx(hi["pa_grid_deg"], abs=0.05)
    assert hi["major_axis_px"] > 2 * hi["minor_axis_px"]
    assert hi["major_axis_arcsec"] == pytest.approx(2.0 * hi["major_axis_px"])
    assert hi["beam_major_arcsec"] == pytest.approx(6.0)

    # The session's noise, supplied: used as it is.
    s2 = await _stats(client, ds, box, (xs[0], ys[0], zs[0]), sigma=0.002)
    assert s2["hi"]["noise_sigma"] == 0.002 and s2["hi"]["noise_source"].startswith("supplied")
    assert s2["hi"]["flux_err_jy_kms"] == pytest.approx(
        0.002 * math.sqrt(sel.sum()) * 5.0 / math.sqrt(beam_px), rel=1e-6)
    assert s2["hi"]["noise_voxels"] == 0
    assert s2["hi"]["channel_rho"] is None and s2["hi"]["flux_err_factor"] is None
    assert s2["hi"]["channel_correlation"] == "channel correlation not measured"


def test_corrected_error_is_the_scatter_of_noise_fluxes(tmp_path) -> None:
    """Pure noise, Hanning-correlated along the channels and independent on
    the sky (a beam of one pixel): the corrected uncertainty of a box's flux
    is the scatter of the fluxes of many boxes; the independent one is
    about 1.6 times too small."""
    from app.compute.mask_stats import worker_cube_mask_stats
    rng = np.random.default_rng(11)
    nz, ny, nx = 62, 96, 96
    white = rng.normal(0.0, 1.0, (nz + 2, ny, nx)).astype(np.float32)
    data = 0.25 * white[:-2] + 0.5 * white[1:-1] + 0.25 * white[2:]
    b = math.sqrt(4 * math.log(2) / math.pi) / 3600          # beam area = 1 pixel (1 arcsec)
    h = fits.Header()
    h.update({"CTYPE1": "RA---SIN", "CTYPE2": "DEC--SIN", "CTYPE3": "VOPT", "CRVAL1": 150.0,
              "CRVAL2": 0.0, "CRVAL3": 0.0, "CRPIX1": 48, "CRPIX2": 48, "CRPIX3": 1,
              "CDELT1": -1 / 3600, "CDELT2": 1 / 3600, "CDELT3": 1000.0, "CUNIT1": "deg",
              "CUNIT2": "deg", "CUNIT3": "m/s", "BUNIT": "Jy/beam", "BMAJ": b, "BMIN": b,
              "SPECSYS": "BARYCENT"})
    path = tmp_path / "corr.fits"
    fits.PrimaryHDU(data, header=h).writeto(path)
    fluxes, errs, indep, rho1s = [], [], [], []
    for y in range(4, 92, 8):
        for x in range(4, 92, 8):
            s = worker_cube_mask_stats(str(path), None, x, y, 21, 4, 4, 20)["hi"]
            fluxes.append(s["flux_jy_kms"])
            errs.append(s["flux_err_jy_kms"])
            indep.append(s["flux_err_indep_jy_kms"])
            rho1s.append(s["channel_rho"][0])
    scatter = float(np.std(fluxes))
    assert float(np.median(rho1s)) == pytest.approx(2 / 3, abs=0.03)
    assert float(np.median(errs)) == pytest.approx(scatter, rel=0.15)      # 121 boxes: ±7 %
    assert float(np.median(errs)) / float(np.median(indep)) == pytest.approx(1.60, abs=0.08)


@pytest.mark.asyncio
async def test_hi_measurements_of_a_box_and_without_a_beam(client, auth_headers, tmp_path) -> None:
    path = tmp_path / "disc.fits"
    data, h = _disc_cube(path)
    ds = await _open(client, auth_headers, path)
    q = {"dataset_id": ds["dataset_id"], "origin_x": 10, "origin_y": 14, "origin_z": 8,
         "width": 28, "height": 16, "depth": 26, "box": "true"}
    r = await client.post("/v1/cube/mask_stats_bin", params=q, content=b"", headers=ds["headers"])
    assert r.status_code == 200, r.text
    hi = r.json()["hi"]
    # The box's frame is the noise: every voxel of the box is the selection.
    assert hi["noise_sigma"] == pytest.approx(0.001, rel=0.05)
    assert hi["w20_kms"] > 0 and hi["flux_err_jy_kms"] > 0
    assert not any("lower limit" in n for n in hi["notes"])
    # A box that cuts the line: the widths are lower limits, and the answer says so.
    r = await client.post("/v1/cube/mask_stats_bin", params={**q, "origin_z": 17, "depth": 6},
                          content=b"", headers=ds["headers"])
    assert "width is a lower limit: the line reaches the selection's edge" in r.json()["hi"]["notes"]
    # Kelvin: no flux in Jy, so no flux uncertainty; the rest is measured.
    h2 = h.copy()
    h2["BUNIT"] = "K"
    h2["SPECSYS"] = "TOPOCENT"
    p2 = tmp_path / "kelvin.fits"
    fits.PrimaryHDU(data, header=h2).writeto(p2)
    ds2 = await _open(client, auth_headers, p2)
    r = await client.post("/v1/cube/mask_stats_bin", params={**q, "dataset_id": ds2["dataset_id"]},
                          content=b"", headers=ds2["headers"])
    hi = r.json()["hi"]
    assert hi["flux_jy_kms"] is None and hi["flux_err_jy_kms"] is None
    assert hi["peak_unit"] == "K" and hi["peak_snr"] > 5
    assert hi["w20_kms"] > 0 and hi["centroid_ra_deg"] is not None and hi["major_axis_px"] > 0
    assert hi["velocity_frame"] == "topocentric"
    assert "topocentric velocities: not corrected for the Earth's motion" in hi["notes"]


# ── SoFiA-2's catalogue from its own masks ───────────────────────────────────

def _catalogue(name: str):
    from astropy.io.votable import parse_single_table
    cube, mask, cat = CUBES[name], SOFIA_OUT / f"sofia_{name}_mask.fits", SOFIA_OUT / f"sofia_{name}_cat.xml"
    missing = [str(p) for p in (cube, mask, cat) if not p.is_file()]
    if missing:
        pytest.skip(f"validation data absent: {', '.join(missing)} (set VISIVO_SOFIA_OUT, "
                    "VISIVO_VALIDATION_CUBE, VISIVO_VALIDATION_WALLABY_SOFIA_CUBE)")
    return cube, fits.getdata(mask), parse_single_table(str(cat)).to_table()


@pytest.mark.asyncio
@pytest.mark.parametrize("name", ["hgc44", "wallaby"])
async def test_sofia2_masks_give_sofia2_catalogue(client, auth_headers, name) -> None:
    from app.compute.kinematic_fit import cube_velocity_axis
    from app.fits_dataset import ScientificFitsDataset
    from app.compute._common import beam_area_pixels

    cube, labels, cat = _catalogue(name)
    ds = await _open(client, auth_headers, cube)
    dset = ScientificFitsDataset(str(cube))
    vel = cube_velocity_axis(dset, labels.shape[0])["vel"]
    idx = np.arange(vel.size, dtype=np.float64)
    dv = abs(float(vel[1] - vel[0]))                      # VELO-HEL / VOPT: uniform channels
    omega = beam_area_pixels(dset.beam.major, dset.beam.minor, dset.spatial_pixel_scale_arcsec(),
                             dset.spatial_pixel_area_arcsec2())
    vcol = next(c for c in cat.colnames if c.startswith("v_") and not c.endswith("_peak"))

    def v_at(z):
        return float(np.interp(z, idx, vel))

    worst = {"rms": 0.0, "radec_arcsec": 0.0}
    factors: list[float] = []
    rho1s: list[float] = []
    for row in cat:
        sid = int(row["id"])
        x0, x1, y0, y1, z0, z1 = (int(row[k]) for k in ("x_min", "x_max", "y_min", "y_max", "z_min", "z_max"))
        mask = labels[z0:z1 + 1, y0:y1 + 1, x0:x1 + 1] == sid
        s = await _stats(client, ds, mask, (x0, y0, z0))
        hi = s["hi"]
        assert s["valid_voxels"] == int(row["n_pix"]), sid
        assert s["sum"] == pytest.approx(float(row["f_sum"]), rel=1e-5), sid
        assert hi["peak"] == pytest.approx(float(row["f_max"]), rel=1e-5), sid
        # SoFiA-2's physical conversion of f_sum.
        assert hi["flux_jy_kms"] == pytest.approx(float(row["f_sum"]) * dv / omega, rel=1e-4), sid
        # Local noise: the convention difference (other sources in the frame).
        rel = abs(hi["noise_sigma"] / float(row["rms"]) - 1.0)
        worst["rms"] = max(worst["rms"], rel)
        assert rel < 0.10, (sid, hi["noise_sigma"], float(row["rms"]))
        # Widths and systemic velocity: SoFiA-2's crossings in channels, on our axis.
        for key, zc, w in (("w20_kms", "z_w20", "w20"), ("w50_kms", "z_w50", "w50")):
            lo, up = float(row[zc]) - float(row[w]) / 2, float(row[zc]) + float(row[w]) / 2
            assert hi[key] == pytest.approx(abs(v_at(up) - v_at(lo)), abs=0.01), (sid, key)
        assert hi["vsys_kms"] == pytest.approx(v_at(float(row["z_w20"])), abs=0.01), sid
        # Flux-weighted centroid (positive voxels), in pixels, on the sky and in velocity.
        for k in ("x", "y", "z"):
            assert hi[f"centroid_{k}"] == pytest.approx(float(row[k]), abs=1e-4), (sid, k)
        assert hi["centroid_velocity_kms"] == pytest.approx(float(row[vcol]) / 1000.0, abs=0.01), sid
        dra = (hi["centroid_ra_deg"] - float(row["ra"])) * math.cos(math.radians(float(row["dec"])))
        sep = 3600.0 * math.hypot(dra, hi["centroid_dec_deg"] - float(row["dec"]))
        worst["radec_arcsec"] = max(worst["radec_arcsec"], sep)
        assert sep < 0.1, (sid, sep)
        # The moment-0 ellipse: SoFiA-2's ell_maj / ell_min / ell_pa.
        assert hi["major_axis_px"] == pytest.approx(float(row["ell_maj"]), rel=1e-4), sid
        assert hi["minor_axis_px"] == pytest.approx(float(row["ell_min"]), rel=1e-4), sid
        dpa = (hi["pa_grid_deg"] - float(row["ell_pa"])) % 180.0
        assert min(dpa, 180.0 - dpa) < 0.05, (sid, hi["pa_grid_deg"], float(row["ell_pa"]))
        # East of north: the grid PA turned by the WCS north's tilt at the source (< 3 deg here).
        dsky = (hi["pa_deg"] - hi["pa_grid_deg"]) % 180.0
        worst["north_tilt"] = max(worst.get("north_tilt", 0.0), min(dsky, 180.0 - dsky))
        assert min(dsky, 180.0 - dsky) < 3.0, sid
        # The correlated-channel correction (local noise): never below the independent error.
        assert hi["flux_err_jy_kms"] >= hi["flux_err_indep_jy_kms"] * 0.95, sid
        factors.append(hi["flux_err_factor"])
        rho1s.append(hi["channel_rho"][0])
        # With SoFiA-2's rms as the session's noise: its err_f_sum (physical) exactly.
        s2 = await _stats(client, ds, mask, (x0, y0, z0), sigma=float(row["rms"]))
        assert s2["hi"]["flux_err_indep_jy_kms"] == pytest.approx(
            float(row["err_f_sum"]) * dv / math.sqrt(omega), rel=1e-4), sid
        assert s2["hi"]["flux_err_jy_kms"] == s2["hi"]["flux_err_indep_jy_kms"]
        assert s2["hi"]["peak_snr"] == pytest.approx(float(row["f_max"]) / float(row["rms"]), rel=1e-5), sid
    print(f"{name}: {len(cat)} sources; worst local-noise difference {100 * worst['rms']:.1f} %, "
          f"worst position {worst['radec_arcsec']:.3f} arcsec, north tilt up to "
          f"{worst['north_tilt']:.2f} deg; rho1 median {np.median(rho1s):.2f}, "
          f"error factor {min(factors):.2f}-{max(factors):.2f}")
