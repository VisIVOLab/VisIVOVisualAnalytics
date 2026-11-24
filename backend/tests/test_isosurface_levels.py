"""Isosurfaces at several levels (issue #526): the levels' generation (linear and
log), a field of nested spheres giving one closed surface per level at the
right radius, the single threshold still working, and the product route."""
from __future__ import annotations

import base64
import zlib

import numpy as np
import pytest
from astropy.io import fits
from httpx import AsyncClient

from app.compute import pv_iso
from app.iso_levels import MAX_LEVELS, resolve_levels, spaced_levels
from app.schemas import IsosurfaceProductRequest

N = 64
CENTRE = (N - 1) / 2.0


def _spheres(tmp_path) -> str:
    """value = 100 - r: the level 100 - R is the sphere of radius R."""
    z, y, x = np.mgrid[:N, :N, :N].astype(np.float32)
    r = np.sqrt((x - CENTRE) ** 2 + (y - CENTRE) ** 2 + (z - CENTRE) ** 2)
    path = tmp_path / "spheres.fits"
    fits.PrimaryHDU((100.0 - r).astype(np.float32)).writeto(path)
    return str(path)


def _decode(entry: dict) -> tuple[np.ndarray, np.ndarray]:
    def raw(key: str, dtype) -> np.ndarray:
        buf = base64.b64decode(entry[key])
        if entry["compression"]:
            buf = zlib.decompress(buf[4:])   # Qt's qCompress: 4-byte length prefix
        return np.frombuffer(buf, dtype=dtype)
    pts = raw("points_base64", np.float32).reshape(-1, 3)
    polys = raw("polys_base64", np.int32).reshape(-1, 4)
    assert (polys[:, 0] == 3).all()
    return pts, polys[:, 1:]


def _is_closed(tris: np.ndarray) -> bool:
    """Every edge is shared by exactly two triangles."""
    edges = np.sort(np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]]), axis=1)
    _, counts = np.unique(edges, axis=0, return_counts=True)
    return bool((counts == 2).all())


# ── the levels ──────────────────────────────────────────────────────────────


def test_linear_levels_are_evenly_spaced_and_include_both_ends():
    assert spaced_levels(5, 1.0, 9.0) == pytest.approx([1, 3, 5, 7, 9])


def test_log_levels_are_evenly_spaced_in_their_logarithm():
    got = spaced_levels(4, 1e-3, 1.0, "log")
    assert got == pytest.approx([1e-3, 1e-2, 1e-1, 1.0])
    assert np.allclose(np.diff(np.log10(got)), 1.0)


def test_one_level_is_the_middle_of_the_range():
    assert spaced_levels(1, 2.0, 8.0) == [5.0]
    assert spaced_levels(1, 1.0, 100.0, "log") == pytest.approx([10.0])


def test_a_reversed_range_is_read_the_right_way_round():
    assert spaced_levels(3, 9.0, 1.0) == pytest.approx([1, 5, 9])


def test_log_needs_a_range_above_zero():
    with pytest.raises(ValueError, match="above zero"):
        spaced_levels(3, 0.0, 1.0, "log")
    with pytest.raises(ValueError, match="above zero"):
        spaced_levels(3, -1.0, 1.0, "log")


def test_resolve_prefers_levels_then_n_levels_then_threshold():
    assert resolve_levels(threshold=7.0) == [7.0]
    assert resolve_levels(threshold=7.0, n_levels=2, level_min=1, level_max=3) == [1.0, 3.0]
    assert resolve_levels(threshold=7.0, levels=[5.0, 2.0, 5.0], n_levels=2,
                          level_min=1, level_max=3) == [2.0, 5.0]


def test_resolve_refuses_what_it_cannot_read():
    with pytest.raises(ValueError, match="Give threshold"):
        resolve_levels()
    with pytest.raises(ValueError, match="level_min and level_max"):
        resolve_levels(n_levels=3)
    with pytest.raises(ValueError, match="finite"):
        resolve_levels(levels=[1.0, float("nan")])
    with pytest.raises(ValueError, match="At most"):
        resolve_levels(levels=list(range(1, MAX_LEVELS + 2)))


def test_request_model_keeps_a_lone_threshold_and_rejects_nothing_at_all():
    assert IsosurfaceProductRequest(dataset_id="d", threshold=0.25).resolved_levels() == [0.25]
    with pytest.raises(ValueError):
        IsosurfaceProductRequest(dataset_id="d")
    with pytest.raises(ValueError):
        IsosurfaceProductRequest(dataset_id="d", n_levels=3, level_min=0.0, level_max=1.0,
                                 spacing="log")


# ── nested spheres ──────────────────────────────────────────────────────────


def test_nested_spheres_give_one_closed_surface_per_level_at_its_radius(tmp_path):
    pytest.importorskip("vtk")
    radii = [6.0, 12.0, 18.0, 24.0]
    levels = [100.0 - r for r in radii]
    out = pv_iso.worker_isosurface(_spheres(tmp_path), N, N, N, levels)
    assert out["skipped_levels"] == []
    assert out["stride"] == [1, 1, 1]
    assert [m["value"] for m in out["levels"]] == sorted(levels)   # ascending: outermost first
    assert out["num_polys_total"] == sum(m["num_polys"] for m in out["levels"])
    for mesh in out["levels"]:
        pts, tris = _decode(mesh)
        radius = 100.0 - mesh["value"]
        measured = np.linalg.norm(pts - CENTRE, axis=1)
        assert abs(float(np.median(measured)) - radius) < 0.5
        assert measured.min() > radius - 1.0 and measured.max() < radius + 1.0
        assert _is_closed(tris)
        # One connected shell: Euler characteristic of a sphere.
        assert len(pts) - 3 * len(tris) // 2 + len(tris) == 2


def test_the_first_surface_keys_are_the_lowest_level(tmp_path):
    pytest.importorskip("vtk")
    out = pv_iso.worker_isosurface(_spheres(tmp_path), N, N, N, [90.0, 80.0])
    assert out["num_polys"] == out["levels"][0]["num_polys"]
    assert out["points_base64"] == out["levels"][0]["points_base64"]
    assert out["levels"][0]["value"] == 80.0


def test_a_lone_threshold_still_works_as_a_float_and_gives_one_level(tmp_path):
    pytest.importorskip("vtk")
    path = _spheres(tmp_path)
    single = pv_iso.worker_isosurface(path, N, N, N, 88.0)
    listed = pv_iso.worker_isosurface(path, N, N, N, [88.0])
    assert len(single["levels"]) == 1
    assert single["num_polys"] == listed["num_polys"] > 0
    assert single["points_base64"] == listed["points_base64"]


def test_a_level_above_the_data_has_no_surface_and_is_reported(tmp_path):
    pytest.importorskip("vtk")
    out = pv_iso.worker_isosurface(_spheres(tmp_path), N, N, N, [85.0, 500.0])
    assert out["skipped_levels"] == [500.0]
    assert [m["value"] for m in out["levels"]] == [85.0]


def test_no_level_cutting_the_data_is_an_error(tmp_path):
    pytest.importorskip("vtk")
    with pytest.raises(ValueError, match="Empty isosurface"):
        pv_iso.worker_isosurface(_spheres(tmp_path), N, N, N, [500.0, 600.0])


def test_the_levels_together_stay_within_the_triangle_budget(tmp_path, monkeypatch):
    pytest.importorskip("vtk")
    path = _spheres(tmp_path)
    levels = [94.0, 88.0, 82.0, 76.0]
    full = pv_iso.worker_isosurface(path, N, N, N, levels)
    monkeypatch.setattr(pv_iso, "_ISO_MIN_SAMPLES", 16)
    monkeypatch.setattr(pv_iso, "ISO_MAX_POLYS", full["num_polys_total"] - 1)
    sampled = pv_iso.worker_isosurface(path, N, N, N, levels)
    assert sampled["num_polys_total"] <= full["num_polys_total"] - 1
    assert sampled["stride"] != [1, 1, 1]
    monkeypatch.setattr(pv_iso, "ISO_MAX_POLYS", 10)
    with pytest.raises(ValueError, match="raise the lowest level"):
        pv_iso.worker_isosurface(path, N, N, N, levels)


# ── the route ───────────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_route_answers_one_surface_per_level(client: AsyncClient, opened_cube: dict) -> None:
    pytest.importorskip("vtk")
    body = {"dataset_id": opened_cube["dataset_id"], "n_levels": 3,
            "level_min": 0.5, "level_max": 4.0, "spacing": "log"}
    resp = await client.post("/v1/products/isosurface", json=body, headers=opened_cube["headers"])
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True, data["error"]
    values = [m["value"] for m in data["levels"]] + data["skipped_levels"]
    assert sorted(values) == pytest.approx([0.5, float(np.sqrt(0.5 * 4.0)), 4.0])
    assert data["num_polys"] == data["levels"][0]["num_polys"] > 0
    assert data["num_polys_total"] == sum(m["num_polys"] for m in data["levels"])
    # The same request again is served from the cache, the same surfaces.
    again = (await client.post("/v1/products/isosurface", json=body,
                               headers=opened_cube["headers"])).json()
    assert again["levels"] == data["levels"]


@pytest.mark.asyncio
async def test_route_keeps_the_single_threshold(client: AsyncClient, opened_cube: dict) -> None:
    pytest.importorskip("vtk")
    resp = await client.post("/v1/products/isosurface",
                             json={"dataset_id": opened_cube["dataset_id"], "threshold": 1.0},
                             headers=opened_cube["headers"])
    data = resp.json()
    assert data["valid"] is True, data["error"]
    assert [m["value"] for m in data["levels"]] == [1.0]
    assert data["num_points"] > 0 and data["points_base64"] and data["polys_base64"]


@pytest.mark.asyncio
async def test_route_refuses_a_request_without_levels_or_with_a_bad_log_range(
        client: AsyncClient, opened_cube: dict) -> None:
    for body in ({"dataset_id": opened_cube["dataset_id"]},
                 {"dataset_id": opened_cube["dataset_id"], "n_levels": 3, "level_min": -1.0,
                  "level_max": 2.0, "spacing": "log"},
                 {"dataset_id": opened_cube["dataset_id"], "levels": [float(i) for i in range(1, 12)]}):
        resp = await client.post("/v1/products/isosurface", json=body, headers=opened_cube["headers"])
        assert resp.status_code == 422, body
