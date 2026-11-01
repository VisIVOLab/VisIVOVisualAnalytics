"""The default 3-D threshold from the cube's noise (issue #93): about
NOISE_VOXELS noise voxels of the whole cube lie above it."""
from __future__ import annotations

import numpy as np
import pytest
from scipy.stats import norm

from app.noise_threshold import (NOISE_VOXELS, _MAX_TAIL_EXCESS, default_threshold,
                                 noise_threshold)

N = 2_000_000


def _noise(rng, n=N, scale=1.0):
    return (scale * rng.standard_normal(n)).astype(np.float32)


def test_gaussian_noise_leaves_about_the_chosen_number_of_voxels_above():
    rng = np.random.default_rng(1)
    cube = _noise(rng)
    t = noise_threshold(cube, cube.size)
    assert t == pytest.approx(norm.isf(NOISE_VOXELS / N), rel=0.05)
    assert 50 <= np.count_nonzero(cube > t) <= 170


def test_the_cut_rises_with_the_size_of_the_cube():
    # The same noise, as a sample of a cube 1000 times larger: k grows from
    # about 4.1 to about 5.5 rather than staying at a fixed nσ.
    sample = _noise(np.random.default_rng(2))
    small = noise_threshold(sample, N)
    large = noise_threshold(sample, 1000 * N)
    assert large == pytest.approx(norm.isf(NOISE_VOXELS / (1000 * N)), rel=0.08)
    assert large > small + 1.0


def test_heavy_tailed_noise_raises_the_cut_by_itself():
    rng = np.random.default_rng(3)
    # Tails about as heavy as the APERTIF cube's (k raised ×1.4 there).
    cube = (rng.standard_t(10, N) / np.sqrt(10 / 8)).astype(np.float32)   # unit variance
    t = noise_threshold(cube, cube.size)
    gaussian = np.median(cube) + norm.isf(NOISE_VOXELS / N) * 1.4826 * np.median(
        np.median(cube) - cube[cube < np.median(cube)])
    assert np.count_nonzero(cube > gaussian) > 3 * NOISE_VOXELS
    assert 30 <= np.count_nonzero(cube > t) <= 2.5 * NOISE_VOXELS


def test_emission_does_not_move_the_cut():
    rng = np.random.default_rng(4)
    noise = _noise(rng)
    cube = noise.copy()
    cube[: N // 50] += rng.uniform(2, 30, N // 50).astype(np.float32)   # 2 % of voxels
    assert noise_threshold(cube, N) == pytest.approx(noise_threshold(noise, N), rel=0.03)


def test_negative_artefacts_are_capped():
    rng = np.random.default_rng(5)
    cube = _noise(rng)
    cube[: N // 200] = -50.0                       # absorption, sidelobes: 0.5 %
    t = noise_threshold(cube, N)
    assert t <= _MAX_TAIL_EXCESS * norm.isf(NOISE_VOXELS / N) * 1.02


def test_blanks_read_as_zero_are_left_out():
    rng = np.random.default_rng(6)
    noise = _noise(rng, scale=3.0)
    cube = np.concatenate([noise, np.zeros(N, dtype=np.float32)])
    assert noise_threshold(cube, N) == pytest.approx(noise_threshold(noise, N), rel=0.02)


@pytest.mark.parametrize("values", [
    np.full(N, 7.0, dtype=np.float32),                                         # constant
    np.random.default_rng(7).lognormal(0.0, 1.0, N).astype(np.float32),       # skewed, positive
    np.full(500, 1.0, dtype=np.float32),                                       # too few values
])
def test_data_that_is_not_noise_keeps_the_percentile(values):
    assert noise_threshold(values, values.size) is None
    assert default_threshold(values, values.size) == pytest.approx(np.percentile(values, 99.0))


def test_nan_is_ignored():
    rng = np.random.default_rng(8)
    cube = _noise(rng)
    with_nan = cube.copy()
    with_nan[::3] = np.nan
    assert noise_threshold(with_nan, N) == pytest.approx(noise_threshold(cube, N), rel=0.03)


@pytest.mark.asyncio
async def test_the_preview_carries_the_default_threshold(client, opened_cube):
    resp = await client.post("/v1/cube/preview",
                             json={"dataset_id": opened_cube["dataset_id"], "downsample": 1},
                             headers=opened_cube["headers"])
    body = resp.json()
    assert body["valid"], body["error"]
    assert isinstance(body["default_threshold"], float)


@pytest.mark.parametrize("sigma", [1.0, 3.0])
def test_integer_noise_keeps_its_zeros(sigma):
    # Integer counts: 0 is the commonest noise value, not blanking.
    rng = np.random.default_rng(9)
    cube = np.round(sigma * rng.standard_normal(N)).astype(np.float32)
    t = noise_threshold(cube, N)
    assert t is not None
    # A cut on integers lands on a whole count; some noise voxels pass it.
    assert 0 < np.count_nonzero(cube > t) <= 10 * NOISE_VOXELS


def test_a_small_cube_lets_through_at_most_a_tiny_fraction():
    rng = np.random.default_rng(10)
    cube = _noise(rng, n=20_000)
    t = noise_threshold(cube, cube.size)
    assert t == pytest.approx(norm.isf(1e-4), rel=0.15)          # not isf(100/20000) = 2.6σ


def test_more_samples_than_voxels_do_not_break_the_rule():
    rng = np.random.default_rng(11)
    t = noise_threshold(_noise(rng, n=5000), 150)
    assert t is not None and np.isfinite(t)
