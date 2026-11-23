"""Projected maps drawn with the SPH kernel integrated along the line of
sight (issue #477): the tabulated F(R) against an independent quadrature,
one particle's mass and profile from 0.3 to 30 pixels, a Plummer sphere's
surface density, and how a request picks its kernel."""
from __future__ import annotations

import io
import math
import sys
import warnings
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("numba")

from scipy.integrate import quad  # noqa: E402

from app.particle_raytrace import Camera, projected_kernel, projected_maps  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_projected_units import _plummer, _surface_density, _top  # noqa: E402

SPH = ("m4", "wendland_c4")
KERNELS = ("gauss", *SPH)


def _w(kernel: str, q: float) -> float:
    """The 3-D kernel of support 1, as the issue states it."""
    if q >= 1.0:
        return 0.0
    if kernel == "m4":
        return 8.0 / math.pi * (1 - 6 * q * q + 6 * q ** 3 if q < 0.5 else 2 * (1 - q) ** 3)
    return 495.0 / (32.0 * math.pi) * (1 - q) ** 6 * (1 + 6 * q + 35.0 * q * q / 3.0)


def _column(kernel: str, u: float) -> float:
    """H² F(R) at u = R / H, by adaptive quadrature along the line of sight."""
    if u >= 1.0:
        return 0.0
    top = math.sqrt(1.0 - u * u)
    knee = [math.sqrt(0.25 - u * u)] if kernel == "m4" and u < 0.5 else None
    return 2.0 * quad(lambda z: _w(kernel, math.hypot(u, z)), 0.0, top, points=knee,
                      epsabs=1e-12, epsrel=1e-10)[0]


@pytest.mark.parametrize("kernel,peak,sigma", [("m4", 6.0 / math.pi, 0.274),
                                               ("wendland_c4", 2.918, 0.226)])
def test_the_tabulated_column_is_the_kernel_along_the_line_of_sight(kernel, peak, sigma):
    table = projected_kernel(kernel)
    s = np.linspace(0.0, 1.0, table.size)
    assert table[0] == pytest.approx(peak, rel=1e-3)
    for u in (0.0, 0.1, 0.3, 0.49, 0.5, 0.51, 0.7, 0.9, 0.97):
        assert np.interp(u * u, s, table) == pytest.approx(_column(kernel, u), rel=1e-5, abs=1e-9)
    r = np.linspace(0.0, 1.0, 400_001)
    f = np.interp(r * r, s, table)
    assert np.trapezoid(2 * math.pi * r * f, r) == pytest.approx(1.0, abs=1e-6)
    # The projected profile's sigma (per axis): sqrt(<R²> / 2).
    assert math.sqrt(np.trapezoid(math.pi * r ** 3 * f, r)) == pytest.approx(sigma, abs=1e-3)


def _one(kernel: str, h_px: float, offset, size: int = 128, mass: float = 3.0):
    cam = _top(size / 2.0)   # one table unit a pixel
    pos = np.array([[offset[0], offset[1], 0.0]], np.float32)
    return projected_maps(pos, np.array([h_px]), np.array([mass], np.float32), None, cam,
                          size, size, kernel=kernel)[..., 0]


@pytest.mark.parametrize("kernel", SPH)
@pytest.mark.parametrize("h_px", [0.1, 0.3, 0.5, 0.8, 1.0, 1.7, 2.0, 3.0, 5.0, 7.9, 8.0, 15.0,
                                  30.0])
def test_one_particle_holds_its_mass_at_any_size(kernel, h_px):
    rng = np.random.default_rng(int(h_px * 100))
    for offset in [(0.0, 0.0), (0.5, 0.5), *rng.uniform(-0.5, 0.5, (6, 2))]:
        held = _one(kernel, h_px, offset).sum(dtype=np.float64)
        assert held == pytest.approx(3.0, rel=1e-3), offset


@pytest.mark.parametrize("kernel", SPH)
@pytest.mark.parametrize("h_px", [8.0, 12.5, 30.0])
def test_one_particle_draws_its_projected_kernel(kernel, h_px):
    """Σ at each pixel within 0.95 H (beyond, F is near 0) is F(R) times the
    mass within 1 %, against the quadrature rather than the table."""
    size, offset = 128, (0.31, -0.17)
    sigma = _one(kernel, h_px, offset, size)
    centres = np.arange(size) + 0.5 - size / 2.0
    dx, dy = np.meshgrid(centres - offset[0], -centres - offset[1])
    u = np.hypot(dx, dy) / h_px
    inner = u < 0.95
    truth = np.array([3.0 * _column(kernel, v) / h_px ** 2 for v in u[inner]])
    assert np.abs(sigma[inner] / truth - 1.0).max() < 0.01
    assert not sigma[u >= 1.0].any()


def test_a_particle_off_the_edge_keeps_only_what_falls_in_view():
    """A particle centred on the image's edge: half its mass is in view."""
    held = _one("m4", 4.0, (64.0, 0.0)).sum(dtype=np.float64)
    assert held == pytest.approx(1.5, rel=1e-3)


@pytest.mark.parametrize("kernel", ["gauss", *SPH])
def test_a_plummer_sphere_keeps_its_analytic_surface_density(kernel):
    """Σ(R) = M a² / (π (a² + R²)²) within 3 % from 0.2 a to 3 a, as #393's
    test, for every kernel."""
    rng = np.random.default_rng(1)
    n, a, total, h = 1_000_000, 1.0, 1.0e12, 0.05
    pos = _plummer(n, a, rng)
    cam = _top(4.0 * a)
    maps = projected_maps(pos, np.full(n, h), np.ones(n, np.float32), None, cam, 800, 800,
                          kernel=kernel)
    sigma, _ = _surface_density(maps, cam, 800, total / n)
    centres = (np.arange(800) + 0.5) * 0.01 - 4.0
    r = np.hypot(*np.meshgrid(centres, centres))
    edges = np.array([0.2, 0.3, 0.45, 0.65, 1.0, 1.4, 2.0, 3.0]) * a
    within = total * edges ** 2 / (a ** 2 + edges ** 2)
    for lo, hi, m_lo, m_hi in zip(edges[:-1], edges[1:], within[:-1], within[1:]):
        ring = (r >= lo) & (r < hi)
        truth = (m_hi - m_lo) / (math.pi * (hi ** 2 - lo ** 2))
        assert sigma[ring].mean() == pytest.approx(truth, rel=0.03), (lo, hi)


@pytest.mark.parametrize("kernel", SPH)
def test_many_particles_of_every_size_hold_their_mass(kernel):
    rng = np.random.default_rng(2)
    n = 72_000
    pos = _plummer(n, 1.0, rng, rmax=2.0)
    mass = rng.lognormal(0.0, 0.5, n).astype(np.float32)
    radius = rng.uniform(0.002, 0.4, n)
    for size in (64, 512):
        cam = _top(2.0 + radius.max() + 0.1)
        maps = projected_maps(pos, radius, mass, None, cam, size, size, kernel=kernel)
        assert maps[..., 0].sum(dtype=np.float64) == pytest.approx(
            float(mass.sum(dtype=np.float64)), rel=1e-4)


@pytest.mark.parametrize("kernel", SPH)
@pytest.mark.parametrize("h_px", [2.0, 3.0, 5.0, 7.5])
def test_a_small_particle_puts_in_each_pixel_its_integral_over_it(kernel, h_px):
    """Between 2 and 8 pixels a pixel holds F integrated over its area (here
    by 40 × 40 sub-pixels) within 1.5 % where F is not near 0."""
    size, offset = 32, (0.0, 0.0)
    sigma = _one(kernel, h_px, offset, size, mass=1.0)
    table = projected_kernel(kernel)
    s = np.linspace(0.0, 1.0, table.size)
    fine = (np.arange(size * 40) + 0.5) / 40.0 - size / 2.0
    dx, dy = np.meshgrid(fine, -fine)
    q2 = (dx * dx + dy * dy) / h_px ** 2
    f = np.where(q2 < 1.0, np.interp(np.minimum(q2, 1.0), s, table), 0.0) / h_px ** 2
    truth = f.reshape(size, 40, size, 40).mean(axis=(1, 3))
    truth /= truth.sum()
    strong = truth > 0.2 * truth.max()
    assert np.abs(sigma[strong] / truth[strong] - 1.0).max() < 0.015


@pytest.mark.parametrize("kernel", SPH)
def test_the_mean_is_weighted_by_the_mass_the_kernel_puts_in_a_pixel(kernel):
    """Two overlapping particles at 10 and 50: each pixel's mean is weighted
    by the mass each one puts there, as each draws alone."""
    cam = _top(32.0)
    pos = np.array([[0.0, 0.0, 0.0], [4.3, -2.1, 1.0]], np.float32)
    h = np.array([9.0, 6.5])
    mass = np.array([1.0, 3.0], np.float32)
    t = np.array([10.0, 50.0], np.float32)
    both = projected_maps(pos, h, mass, t, cam, 64, 64, kernel=kernel)
    one = [projected_maps(pos[i:i + 1], h[i:i + 1], mass[i:i + 1], None, cam, 64, 64,
                          kernel=kernel)[..., 0] for i in range(2)]
    overlap = (one[0] > 0) & (one[1] > 0)
    assert overlap.sum() > 50
    a, b = one[0][overlap], one[1][overlap]
    assert np.allclose(both[..., 1][overlap], (10.0 * a + 50.0 * b) / (a + b), rtol=1e-5)
    assert np.allclose(both[..., 0], one[0] + one[1], rtol=1e-5, atol=1e-9)
    # A NaN is no value: left out of the mean, its mass still in the density.
    nan = projected_maps(pos, h, mass, np.array([np.nan, 50.0], np.float32), cam, 64, 64,
                         kernel=kernel)
    assert np.allclose(nan[..., 1][overlap], 50.0, rtol=1e-6)
    assert np.isnan(nan[..., 1][(one[0] > 0) & (one[1] == 0)]).all()


@pytest.mark.parametrize("kernel", KERNELS)
@pytest.mark.parametrize("h", [1e-40, 1e-30, 0.0, 1e19, 1e25, 1e38, np.inf, np.nan])
def test_an_extreme_support_never_reads_outside_its_arrays(kernel, h):
    """A support of a subnormal, 0, beyond int64 in pixels, infinite or NaN,
    among ordinary particles on tile and pixel edges of an image that is no
    multiple of the tile: the ordinary ones keep their mass (a float beyond
    int64 once crashed the process in a pixel loop's int())."""
    cam = _top(35.0)   # 70 × 45 pixels, one unit a pixel
    edges = np.array([[-35.0 + 32.0, 22.5 - 32.0, 0.0], [-35.0 + 64.0, 0.0, 0.0],
                      [0.0, 0.0, 0.0], [-3.0, 22.5 - 1.0, 0.0], [10.25, -7.75, 0.0]],
                     np.float32)
    pos = np.vstack([edges, [[1.5, 2.5, 0.0]]]).astype(np.float32)
    radius = np.array([3.0, 4.0, 40.0, 2.0, 0.6, h])
    maps = projected_maps(pos, radius, np.ones(len(pos), np.float32), None, cam, 70, 45,
                          kernel=kernel)
    alone = projected_maps(edges, radius[:-1], np.ones(len(edges), np.float32), None, cam, 70,
                           45, kernel=kernel)
    if kernel == "gauss" and 0.0 < h < 1e-20:
        # The Gaussian's per-particle light, (median h / h)², overflows
        # float32 there: NaN pixels, though nothing is read out of bounds.
        return
    assert np.isfinite(maps[..., 0]).all()
    extra = maps[..., 0].sum(dtype=np.float64) - alone[..., 0].sum(dtype=np.float64)
    if kernel == "gauss" or not np.isfinite(h) or h > 1e3:
        assert -1e-4 <= extra <= 1.0 + 1e-4
    else:
        assert extra == pytest.approx(1.0, abs=1e-4)


@pytest.mark.parametrize("kernel", KERNELS)
def test_positions_that_are_not_finite_are_left_out(kernel):
    pos = np.array([[np.nan, 0, 0], [np.inf, 0, 0], [0, -np.inf, 0], [1, 1, 0]], np.float32)
    maps = projected_maps(pos, np.full(4, 2.0), np.ones(4, np.float32), None, _top(16.0), 32,
                          32, kernel=kernel)
    assert np.isfinite(maps[..., 0]).all()
    assert maps[..., 0].sum(dtype=np.float64) == pytest.approx(1.0, rel=2e-3)


@pytest.mark.parametrize("kernel", SPH)
def test_extreme_supports_neither_crash_nor_lose_the_others(kernel):
    """An H of 1e25 pixels spreads its mass over the whole sky (about none
    in view); an H of 0, or 1e-30, puts it all in its pixel."""
    cam = _top(32.0)
    pos = np.array([[0.2, 0.3, 0.0], [5.0, 5.0, 0.0], [-7.5, 3.3, 0.0], [9.0, -9.0, 0.0]],
                   np.float32)
    h = np.array([1e25, 0.0, 1e-30, 3.0])
    maps = projected_maps(pos, h, np.ones(4, np.float32), None, cam, 64, 64, kernel=kernel)
    assert np.isfinite(maps[..., 0]).all()
    assert maps[..., 0].sum(dtype=np.float64) == pytest.approx(3.0, rel=1e-5)
    assert maps[32 - 5, 32 + 5, 0] == pytest.approx(1.0)


@pytest.mark.parametrize("kernel", SPH)
def test_millions_on_one_pixel_keep_their_mass(kernel):
    n = 2_000_000
    pos = np.zeros((n, 3), np.float32) + np.float32(0.3)
    maps = projected_maps(pos, np.full(n, 1.5), np.ones(n, np.float32), None, _top(8.0), 16,
                          16, kernel=kernel)
    assert maps[..., 0].sum(dtype=np.float64) == pytest.approx(n, rel=1e-5)


def test_an_unknown_kernel_is_refused():
    with pytest.raises(ValueError):
        projected_maps(np.zeros((1, 3), np.float32), np.ones(1), np.ones(1, np.float32), None,
                       _top(1.0), 16, 16, kernel="quintic")
    with pytest.raises(ValueError):
        projected_kernel("gauss")


# ── How a request picks its kernel ───────────────────────────────────────────

def _gas(tmp_path):
    from test_gadget_snapshot import _snapshot

    from app import gadget_snapshot as gs
    files, _ = _snapshot(tmp_path)
    return {"kind": "catalogue", "format": "gadget", "path": f"{files[0]}{gs.REF_SEP}0"}


_CAM = {"position": (500, 500, 2000), "focal_point": (500, 500, 500), "view_up": (0, 1, 0),
        "parallel": True, "parallel_scale": 300.0}


def test_a_smoothing_length_column_is_drawn_with_the_cubic_spline(tmp_path, monkeypatch):
    from astropy.io import fits

    from app import gadget_snapshot as gs
    from app.projected_units import projected_fits
    from app.routers import particle_render as router
    monkeypatch.delenv(gs.UNITS_ENV, raising=False)
    entry = _gas(tmp_path)
    req = lambda **kw: router.ParticleRenderRequest(  # noqa: E731
        dataset_id="g", mode="projected", width=64, height=64, cameras=[_CAM], **kw)
    hsml = router._prepare(entry, req(radius_field="hsml"))
    assert hsml["kernel"] == "m4" and hsml["map_units"]["kernel"] == "m4"
    assert router._light_headers(hsml)[router.X_PROJECTED_KERNEL] == "m4"
    # Read as the support H, unchanged; the Gaussian takes the same column as its h.
    plain = router._prepare(entry, req(radius_field="hsml", kernel="gauss"))
    assert plain["kernel"] == "gauss" and np.array_equal(plain["radius"], hsml["radius"])
    c4 = router._prepare(entry, req(radius_field="hsml", kernel="wendland_c4"))
    assert c4["map_units"]["kernel"] == "wendland_c4"
    # No smoothing length: the Gaussian, as before, unless a kernel is asked
    # for; its support then reaches the neighbours-th particle, twice the h.
    estimated = router._prepare(entry, req())
    assert estimated["kernel"] == "gauss"
    spline = router._prepare(entry, req(kernel="m4"))
    assert np.allclose(spline["radius"], 2.0 * estimated["radius"], rtol=1e-6)
    # A 3-D render keeps the Gaussian whatever is asked.
    light = req(radius_field="hsml", kernel="m4").model_copy(update={"mode": "light"})
    assert router._projected_kernel(light) == "gauss"
    assert router._particle_key_fields(light)["kernel"] == "gauss"
    _, maps, _ = router._frame(hsml, req(), req().cameras[0], 64, 64, req().tone, None)
    assert np.isfinite(maps[..., 0]).all() and maps[..., 0].sum() > 0
    with warnings.catch_warnings():
        warnings.simplefilter("error", fits.verify.VerifyWarning)
        body = projected_fits(maps, c4["map_units"], req().cameras[0], 64, 64, None)
        body = projected_fits(maps, hsml["map_units"], req().cameras[0], 64, 64, None)
    with fits.open(io.BytesIO(body)) as hdul:
        assert hdul[0].header["KERNEL"] == "M4" and hdul["MEAN"].header["KERNEL"] == "M4"
        assert hdul[0].header["HSMLDEF"] == "support"
        assert hdul[0].header["HSMLSRC"] == "column hsml"
    assert spline["map_units"]["smoothing"] == "distance to neighbour 32"


def test_the_gpu_rows_carry_the_gaussian_of_the_kernels_width(tmp_path, monkeypatch):
    """The GPU view draws Gaussians: an SPH map's rows are as wide as the
    kernel's projected second moment (σ = 0.274 H for M4), sampled or not."""
    import asyncio

    from app import gadget_snapshot as gs
    from app.particle_raytrace import gaussian_width
    from app.routers import particle_render as router
    monkeypatch.delenv(gs.UNITS_ENV, raising=False)
    monkeypatch.setattr(router, "_PREPARED", router.OrderedDict())
    assert gaussian_width("m4") == pytest.approx(0.27386 * math.sqrt(2.0), rel=1e-4)
    assert gaussian_width("wendland_c4") == pytest.approx(0.22646 * math.sqrt(2.0), rel=1e-4)
    assert gaussian_width("gauss") == 1.0

    class _Session:
        datasets = {"g": _gas(tmp_path)}

    for limit in (50_000_000, 1000):
        req = router.ParticleDataRequest(dataset_id="g", mode="projected", radius_field="hsml",
                                         max_particles=limit)
        reply = asyncio.run(router.particle_render_particles(req, token="tok",
                                                             session=_Session()))
        assert reply.headers["X-Visivo-Projected-Kernel"] == "m4"
        rows = np.frombuffer(reply.body, dtype="<f4").reshape(-1, 6)
        prepared = router._prepare(_Session.datasets["g"], req)
        step = int(reply.headers["X-Visivo-Particle-Step"])
        assert np.allclose(rows[:, 3], prepared["radius"][::step] * gaussian_width("m4"),
                           rtol=1e-6)
