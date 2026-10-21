"""Tests for the F2 tilted-ring kinematic model (app/kinematic_model.py)."""
import struct

import numpy as np
import pytest

from app.kinematic_model import tilted_ring_model


def test_tilted_ring_shape_peak_and_disk_mask():
    spectral = np.linspace(-100.0, 100.0, 32)
    model = tilted_ring_model(64, 64, spectral, x0=32, y0=32, pa_deg=0.0,
                              incl_deg=60.0, vsys=0.0, vrot=100.0,
                              rmax_pix=20.0, sigma=10.0)
    assert model.shape == (32, 64, 64)          # (nchan, ny, nx)
    assert model.dtype == np.float32
    # Some channel lands near each pixel's vlos → near-unit peak.
    assert model.max() > 0.5
    # A corner pixel is well outside the disk → no emission in any channel.
    assert model[:, 0, 0].sum() == 0.0
    # The disk centre emits.
    assert model[:, 32, 32].max() > 0.5


def test_tilted_ring_velocity_gradient_sign():
    """With PA=0 the major axis runs along +y; the +y side must peak at a higher
    velocity channel than the -y side (a rotating-disk velocity gradient)."""
    spectral = np.linspace(-150.0, 150.0, 64)   # increasing velocity axis
    model = tilted_ring_model(64, 64, spectral, x0=32, y0=32, pa_deg=0.0,
                              incl_deg=60.0, vsys=0.0, vrot=100.0,
                              rmax_pix=25.0, sigma=8.0)
    idx_plus = int(np.argmax(model[:, 42, 32]))   # 10 px above centre
    idx_minus = int(np.argmax(model[:, 22, 32]))  # 10 px below centre
    assert idx_plus > idx_minus


def test_tilted_ring_face_on_is_flat():
    """Face-on (incl=0) → no line-of-sight rotation; every disk pixel peaks at
    the same channel (the one nearest vsys)."""
    spectral = np.linspace(-100.0, 100.0, 41)   # vsys=0 falls on channel 20
    model = tilted_ring_model(48, 48, spectral, x0=24, y0=24, pa_deg=30.0,
                              incl_deg=0.0, vsys=0.0, vrot=120.0,
                              rmax_pix=15.0, sigma=10.0)
    for (yy, xx) in [(24, 24), (24, 30), (30, 24), (20, 27)]:
        assert int(np.argmax(model[:, yy, xx])) == 20


@pytest.mark.asyncio
async def test_kinematic_model_endpoint(client, opened_cube):
    # The synthetic cube has a FREQ axis (Hz) + RESTFRQ, so the backend converts
    # it to optical velocity (cz, km/s), as the kinematic fit does; vsys/vrot/sigma are km/s.
    resp = await client.post(
        "/v1/cube/kinematic_model_bin",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "x0": 32.0, "y0": 32.0,
            "pa_deg": 0.0, "incl_deg": 60.0,
            "vsys": 0.0, "vrot": 5.0, "rmax_pix": 20.0, "sigma": 3.0,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200, resp.text
    w = int(resp.headers["X-Visivo-Width"])
    h = int(resp.headers["X-Visivo-Height"])
    d = int(resp.headers["X-Visivo-Depth"])
    assert (w, h, d) == (64, 64, 32)             # matches the synthetic cube
    assert resp.headers["X-Visivo-Spectral-Mode"] == "velocity"
    assert resp.headers["X-Visivo-Spectral-Axis-Unit"] == "km/s"
    body = resp.content
    assert len(body) == w * h * d * 4            # little-endian float32
    peak = float(resp.headers["X-Visivo-Peak"])
    assert peak > 0.0
    # First voxel decodes as a finite float32.
    (v0,) = struct.unpack("<f", body[:4])
    assert np.isfinite(v0)


@pytest.mark.asyncio
async def test_kinematic_model_rejects_bad_params(client, opened_cube):
    """Non-finite / out-of-range parameters are rejected at the API boundary."""
    for bad in (
        {"sigma": 0.0},          # must be > 0
        {"rmax_pix": -5.0},      # must be > 0
        {"incl_deg": 120.0},     # must be 0..90
    ):
        payload = {
            "dataset_id": opened_cube["dataset_id"],
            "x0": 32.0, "y0": 32.0, "pa_deg": 0.0, "incl_deg": 60.0,
            "vsys": 0.0, "vrot": 5.0, "rmax_pix": 20.0, "sigma": 3.0,
        }
        payload.update(bad)
        resp = await client.post("/v1/cube/kinematic_model_bin", json=payload,
                                 headers=opened_cube["headers"])
        assert resp.status_code == 422, (bad, resp.status_code)


@pytest.mark.asyncio
async def test_kinematic_model_size_guard(client, opened_cube, monkeypatch):
    """An oversized model (relative to the voxel cap) returns HTTP 413."""
    monkeypatch.setenv("VISIVO_KINEMATIC_MAX_VOXELS", "1000")  # 64×64×32 ≫ 1000
    resp = await client.post(
        "/v1/cube/kinematic_model_bin",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "x0": 32.0, "y0": 32.0, "pa_deg": 0.0, "incl_deg": 60.0,
            "vsys": 0.0, "vrot": 5.0, "rmax_pix": 20.0, "sigma": 3.0,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 413, resp.text


# ── Per-ring (warped) model: app.kinematic_model.tilted_ring_model_profile ────

def test_profile_constant_equals_scalar():
    """A constant per-ring profile must reduce EXACTLY to the scalar model."""
    from app.kinematic_model import tilted_ring_model_profile
    spectral = np.linspace(-150.0, 150.0, 48)
    scalar = tilted_ring_model(64, 64, spectral, x0=32, y0=30, pa_deg=25.0,
                               incl_deg=60.0, vsys=10.0, vrot=120.0,
                               rmax_pix=22.0, sigma=9.0)
    profile = tilted_ring_model_profile(
        64, 64, spectral, x0=32, y0=30,
        ring_r=[0.0, 22.0], ring_pa_deg=[25.0, 25.0], ring_incl_deg=[60.0, 60.0],
        ring_vrot=[120.0, 120.0], vsys=10.0, sigma=9.0)
    assert profile.shape == scalar.shape
    assert np.allclose(profile, scalar, atol=1e-5)


def test_profile_rising_rotation_curve():
    """A rising Vrot(r) → an outer pixel peaks further from vsys than an inner
    one on the SAME (approaching/receding) side of the major axis."""
    from app.kinematic_model import tilted_ring_model_profile
    spectral = np.linspace(-200.0, 200.0, 81)   # vsys=0 at channel 40
    model = tilted_ring_model_profile(
        64, 64, spectral, x0=32, y0=32,
        ring_r=[2.0, 8.0, 24.0], ring_pa_deg=[0.0, 0.0, 0.0],
        ring_incl_deg=[60.0, 60.0, 60.0], ring_vrot=[20.0, 60.0, 160.0],
        vsys=0.0, sigma=7.0)
    # PA=0 → major axis along +y. Inner pixel (5 px up) vs outer (22 px up).
    inner_idx = int(np.argmax(model[:, 37, 32]))
    outer_idx = int(np.argmax(model[:, 54, 32]))
    centre_idx = int(np.argmax(model[:, 32, 32]))   # vsys channel
    assert outer_idx > inner_idx > centre_idx        # velocity grows with radius


def test_profile_unsorted_rings_are_sorted():
    """Rings given out of radius order must be handled (sorted internally)."""
    from app.kinematic_model import tilted_ring_model_profile
    spectral = np.linspace(-150.0, 150.0, 40)
    a = tilted_ring_model_profile(
        48, 48, spectral, x0=24, y0=24,
        ring_r=[24.0, 4.0, 12.0], ring_pa_deg=[0.0, 0.0, 0.0],
        ring_incl_deg=[55.0, 55.0, 55.0], ring_vrot=[150.0, 30.0, 90.0],
        vsys=0.0, sigma=8.0)
    b = tilted_ring_model_profile(
        48, 48, spectral, x0=24, y0=24,
        ring_r=[4.0, 12.0, 24.0], ring_pa_deg=[0.0, 0.0, 0.0],
        ring_incl_deg=[55.0, 55.0, 55.0], ring_vrot=[30.0, 90.0, 150.0],
        vsys=0.0, sigma=8.0)
    assert np.array_equal(a, b)


def test_profile_warp_runs_and_masks_disk():
    """A warped disk (incl and PA varying with radius) produces a valid cube and
    still blanks pixels outside the outermost ring."""
    from app.kinematic_model import tilted_ring_model_profile
    spectral = np.linspace(-200.0, 200.0, 48)
    model = tilted_ring_model_profile(
        80, 80, spectral, x0=40, y0=40,
        ring_r=[3.0, 15.0, 30.0], ring_pa_deg=[10.0, 25.0, 60.0],
        ring_incl_deg=[30.0, 50.0, 75.0], ring_vrot=[40.0, 120.0, 140.0],
        vsys=5.0, sigma=8.0)
    assert model.shape == (48, 80, 80)
    assert model.dtype == np.float32
    assert model.max() > 0.5
    assert model[:, 0, 0].sum() == 0.0     # corner outside the outer ring
    assert model[:, 40, 40].max() > 0.5    # centre emits


def test_profile_bad_arrays_raise():
    from app.kinematic_model import tilted_ring_model_profile
    spectral = np.linspace(-100.0, 100.0, 16)
    with pytest.raises(ValueError):
        tilted_ring_model_profile(32, 32, spectral, x0=16, y0=16,
                                  ring_r=[0.0, 10.0], ring_pa_deg=[0.0],
                                  ring_incl_deg=[60.0, 60.0], ring_vrot=[100.0, 100.0],
                                  vsys=0.0, sigma=5.0)


@pytest.mark.asyncio
async def test_kinematic_model_endpoint_per_ring(client, opened_cube):
    """The endpoint accepts a per-ring profile and returns a valid model cube."""
    resp = await client.post(
        "/v1/cube/kinematic_model_bin",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "x0": 32.0, "y0": 32.0, "vsys": 0.0, "sigma": 3.0,
            "rings": [
                {"r_pix": 4.0,  "pa_deg": 0.0, "incl_deg": 60.0, "vrot": 2.0},
                {"r_pix": 12.0, "pa_deg": 0.0, "incl_deg": 60.0, "vrot": 5.0},
                {"r_pix": 22.0, "pa_deg": 0.0, "incl_deg": 65.0, "vrot": 6.0},
            ],
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200, resp.text
    w = int(resp.headers["X-Visivo-Width"])
    h = int(resp.headers["X-Visivo-Height"])
    d = int(resp.headers["X-Visivo-Depth"])
    assert (w, h, d) == (64, 64, 32)
    assert len(resp.content) == w * h * d * 4
    assert float(resp.headers["X-Visivo-Peak"]) > 0.0


@pytest.mark.asyncio
async def test_kinematic_model_scalar_requires_params(client, opened_cube):
    """A scalar request (no rings) missing pa/incl/vrot/rmax must be rejected."""
    resp = await client.post(
        "/v1/cube/kinematic_model_bin",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "x0": 32.0, "y0": 32.0, "vsys": 0.0, "sigma": 3.0,
            # no pa_deg/incl_deg/vrot/rmax_pix and no rings → invalid
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 422, resp.text
