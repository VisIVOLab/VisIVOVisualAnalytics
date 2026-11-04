"""Ray-traced volume rendering (issue #106): the model of
app/volume_raytrace.py."""
from __future__ import annotations

import numpy as np
import pytest

pytest.importorskip("numba")

from app.particle_raytrace import Camera, project  # noqa: E402
from app.volume_raytrace import (colour_index, emission_from, projected_maps,  # noqa: E402
                                 render)

WHITE = np.ones((1, 3), np.float32)
BOX = (0.0, 10.0, 0.0, 10.0, 0.0, 10.0)
# Looking down -z at the box's middle, without perspective: 16 world units
# across 64 pixels.
TOP = Camera((5.0, 5.0, 30.0), (5.0, 5.0, 5.0), (0.0, 1.0, 0.0), 30.0, True, 8.0)


def test_a_uniform_medium_gives_its_column():
    light = render(np.ones((11, 11, 11), np.float32), None, WHITE, TOP, 64, 64, box=BOX)
    # The middle crosses the whole box (10 units); outside it, nothing.
    assert light[32, 32, 0] == pytest.approx(10.0, rel=1e-4)
    assert light[1, 1, 0] == 0.0
    assert np.allclose(light[..., 0], light[..., 2])


def test_a_bright_voxel_lands_where_the_camera_projects_it():
    grid = np.zeros((11, 11, 11), np.float32)
    grid[4, 3, 7] = 50.0           # z, y, x: the point (7, 3, 4)
    cam = Camera((14.0, -6.0, 20.0), (5.0, 5.0, 5.0), (0.1, 1.0, 0.2), 35.0)
    light = render(grid, None, WHITE, cam, 160, 120, box=BOX)
    sx, sy, _, _ = project(np.array([[7.0, 3.0, 4.0]], np.float32), np.ones(1), cam, 160, 120)
    y, x = np.unravel_index(np.argmax(light[..., 0]), light.shape[:2])
    assert abs(x + 0.5 - sx[0]) <= 1.0 and abs(y + 0.5 - sy[0]) <= 1.0


def test_a_coarse_grid_over_the_same_box_draws_the_same_view():
    # A linear field is sampled exactly at any resolution: the grid fills the
    # box whatever its number of points.
    def field(n):
        x = np.linspace(0.0, 10.0, n, dtype=np.float32)
        return np.broadcast_to(0.1 + x / 10.0, (n, n, n)).astype(np.float32)

    cam = Camera((25.0, 18.0, 30.0), (5.0, 5.0, 5.0), (0.0, 1.0, 0.0), 30.0)
    fine = render(field(41), None, WHITE, cam, 48, 40, box=BOX, step=0.05)
    coarse = render(field(6), None, WHITE, cam, 48, 40, box=BOX, step=0.05)
    assert np.allclose(fine, coarse, rtol=1e-3, atol=1e-4)


def test_the_colour_comes_from_the_second_field():
    colour = np.zeros((11, 11, 11), np.float32)
    colour[:, :, 6:] = 1.0                     # x > 5: the table's last colour
    lut = np.array([[1, 0, 0], [0, 0, 1]], np.float32)
    light = render(np.ones((11, 11, 11), np.float32), colour, lut, TOP, 64, 64, box=BOX)
    left, right = light[32, 20], light[32, 44]
    assert left[0] > 0 and left[2] == 0
    assert right[2] > 0 and right[0] == 0


def test_absorption_dims_what_lies_behind():
    colour = np.zeros((11, 11, 11), np.float32)
    colour[:5] = 1.0                           # far half (low z): blue; near: red
    lut = np.array([[1, 0, 0], [0, 0, 1]], np.float32)
    e = np.ones((11, 11, 11), np.float32)
    clear = render(e, colour, lut, TOP, 64, 64, box=BOX)[32, 32]
    thick = render(e, colour, lut, TOP, 64, 64, box=BOX, absorption=2.0)[32, 32]
    assert clear[0] == pytest.approx(clear[2], rel=0.25)
    assert thick[2] < 0.5 * thick[0]
    assert thick[0] < clear[0]                 # it dims its own light too


def test_a_projected_map_sums_the_column_and_averages_the_value():
    z = np.arange(11, dtype=np.float32)
    value = np.broadcast_to(z[:, None, None], (11, 11, 11)).astype(np.float32)
    maps = projected_maps(np.ones((11, 11, 11), np.float32), value, TOP, 64, 64, box=BOX)
    assert maps[32, 32, 0] == pytest.approx(10.0, rel=1e-4)
    assert maps[32, 32, 1] == pytest.approx(5.0, rel=1e-3)
    assert np.isnan(maps[1, 1, 1])
    # A slab of 4 around the focal plane (z = 5): a column of 4, its mean 5.
    slab = projected_maps(np.ones((11, 11, 11), np.float32), value, TOP, 64, 64, box=BOX,
                          slab=4.0)
    assert slab[32, 32, 0] == pytest.approx(4.0, rel=1e-3)
    assert slab[32, 32, 1] == pytest.approx(5.0, rel=1e-3)


def test_a_projected_map_has_no_perspective():
    cam = Camera((5.0, 5.0, 30.0), (5.0, 5.0, 5.0), (0.0, 1.0, 0.0), 30.0)
    maps = projected_maps(np.ones((11, 11, 11), np.float32), None, cam, 64, 64, box=BOX)
    lit = maps[..., 0] > 0
    # Seen at the focal plane, every column is the box's depth.
    assert np.allclose(maps[..., 0][lit], 10.0, rtol=1e-3)


def test_emission_and_colour_index_from_quantities():
    logs = np.array([[[0.0, 1.0, 2.0, np.nan]]], np.float32)
    e = emission_from(logs, log10=True)
    assert e[0, 0, 3] == 0.0 and e[0, 0, 2] == pytest.approx(1.0, rel=0.05)
    assert e[0, 0, 1] == pytest.approx(0.1 * e[0, 0, 2], rel=1e-3)
    # A log luminosity (erg/s): 10 to it does not fit a float, the light does.
    lum = emission_from(np.array([[[42.0, 43.0, 44.0]]], np.float32), log10=True)
    assert np.isfinite(lum).all() and lum[0, 0, 2] == pytest.approx(1.0, rel=0.05)
    assert lum[0, 0, 0] == pytest.approx(0.01 * lum[0, 0, 2], rel=1e-3)
    # A floor: noise at and under it dark; one below 0 never lights negatives.
    noise = np.array([[[-3.0, -0.5, 0.5, 2.0, 5.0]]], np.float32)
    assert np.count_nonzero(emission_from(noise, floor=1.0)) == 2
    assert np.count_nonzero(emission_from(noise, floor=-1.0)) == 3
    # Of logarithms: on the logarithms, exactly; one beyond them all, dark.
    logs = np.linspace(-10, 10, 201, dtype=np.float32)
    for v in logs[::10]:
        assert np.all(emission_from(logs, log10=True, floor=float(v))[logs <= v] == 0)
    assert not emission_from(logs, log10=True, floor=400.0).any()
    sq = emission_from(np.array([[[1.0, 2.0, 4.0]]], np.float32), power=2.0)
    assert sq[0, 0, 1] / sq[0, 0, 0] == pytest.approx(4.0, rel=1e-4)
    idx, lo, hi = colour_index(np.array([[[1e4, 1e6, 1e8, 0.0]]], np.float32), lo=1e4, hi=1e8,
                               log=True)
    assert (lo, hi) == (1e4, 1e8)
    assert np.allclose(idx[0, 0], [0.0, 0.5, 1.0, 0.0], atol=1e-6)
    # Its range open: where the light is, not an empty background's.
    t = np.concatenate([np.full(1000, 1e-12), np.logspace(4, 8, 1000)]).astype(np.float32)
    lit = np.arange(t.size) >= 1000
    _, lo, hi = colour_index(t[None, None], log=True, lit=lit[None, None])
    assert 1e4 <= lo < 1.2e4 and 0.98e8 < hi <= 1e8
    idx, _, _ = colour_index(np.array([[[0.0, 5.0, 10.0]]], np.float32), lo=0.0, hi=5.0)
    assert np.allclose(idx[0, 0], [0.0, 1.0, 1.0])


_CONCURRENT = """
import sys, threading
if sys.platform != "win32":
    # An abort is the failure: a plain exit code, no crash report or dialog.
    import ctypes, signal
    libc = ctypes.CDLL(None)
    libc.signal.restype = ctypes.c_void_p
    libc.signal.argtypes = [ctypes.c_int, ctypes.c_void_p]
    libc.signal(signal.SIGABRT, ctypes.cast(libc._exit, ctypes.c_void_p))
import numpy as np
from app.particle_raytrace import Camera, render as particles
from app.volume_raytrace import render as volume
cam = Camera((60, 40, 80), (16, 16, 16), (0, 1, 0), 30.0)
grid = np.random.default_rng(0).random((48, 48, 48), dtype=np.float32)
pos = np.random.default_rng(1).uniform(0, 32, (5000, 3)).astype(np.float32)
white = np.ones((1, 3), np.float32)
def go(i):
    for _ in range(8):
        if i % 2:
            volume(grid, None, white, cam, 160, 120, box=(0, 47, 0, 47, 0, 47))
        else:
            particles(pos, np.ones(5000), np.ones((5000, 3), np.float32),
                      np.ones(5000, np.float32), cam, 160, 120)
threads = [threading.Thread(target=go, args=(i,)) for i in range(4)]
[t.start() for t in threads]
[t.join() for t in threads]
print("ok")
"""


def test_parallel_kernels_from_several_threads_do_not_abort_the_process():
    # Numba's workqueue layer aborts the process when two parallel kernels
    # run at once (a render task, a GPU view's grid, a preview): they take
    # turns (app/numba_parallel.py).
    import subprocess
    import sys
    from pathlib import Path
    done = subprocess.run([sys.executable, "-c", _CONCURRENT], capture_output=True, text=True,
                          cwd=Path(__file__).resolve().parents[1], timeout=300,
                          env={**__import__("os").environ, "VISIVO_TOKEN": "x"})
    assert done.returncode == 0 and "ok" in done.stdout, done.stderr[-2000:]
