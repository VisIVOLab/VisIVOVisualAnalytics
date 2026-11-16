"""Isosurfaces of a cube's field (issue #370): app/isosurface.py and
/v1/isosurface/*. An analytic field whose level sets are spheres (a Gaussian
density) checks the surfaces' area and enclosed volume against 4πR² and
4πR³/3, on a linear and on a log scale, after decimation, across slab seams,
at a coarser level of detail read every other voxel and from a brick pyramid;
then the task (progress, cache reuse), the wire mesh, the exports' world
coordinates, a PLUTO variable and a sibling field."""
from __future__ import annotations

import asyncio
import importlib.util
import json
import math
import struct
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import isosurface as ISO

N = 48                  # the cube's side (voxels)
SIGMA = 8.0             # the Gaussian's width (voxels)
PEAK = 1.0e-3
R = 15.0                # the sphere checked (voxels)
LEVEL = PEAK * math.exp(-R * R / (2 * SIGMA * SIGMA))
CENTRE = (N - 1) / 2.0

_GEN = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"


def _gauss(shape=(N, N, N), centre=None, sigma=SIGMA) -> np.ndarray:
    c = centre if centre is not None else [(n - 1) / 2.0 for n in shape]
    k, j, i = np.meshgrid(*(np.arange(n, dtype=np.float64) for n in shape), indexing="ij")
    r2 = (i - c[2]) ** 2 + (j - c[1]) ** 2 + (k - c[0]) ** 2
    return (PEAK * np.exp(-r2 / (2 * sigma * sigma))).astype(np.float32)


@pytest.fixture()
def sphere_cube(tmp_path) -> str:
    path = tmp_path / "sphere.fits"
    fits.PrimaryHDU(_gauss()).writeto(path)
    return str(path)


@pytest.fixture(autouse=True)
def _fresh_cache():
    ISO.CACHE.clear()
    yield
    ISO.CACHE.clear()


def _voxels(mesh: ISO.Mesh) -> np.ndarray:
    return mesh.points.astype(np.float64) * np.array(mesh.meta["full_shape"], float) - 0.5


def _errors(points_vox: np.ndarray, triangles: np.ndarray, radius: float = R) -> tuple[float, float]:
    area = ISO.surface_area(points_vox, triangles)
    volume = ISO.enclosed_volume(points_vox, triangles)
    return area / (4 * math.pi * radius ** 2) - 1.0, volume / (4.0 / 3.0 * math.pi * radius ** 3) - 1.0


def _closed(triangles: np.ndarray) -> bool:
    """Every edge in exactly two triangles, once in each direction."""
    t = triangles.astype(np.int64)
    directed = np.concatenate([t[:, [0, 1]], t[:, [1, 2]], t[:, [2, 0]]])
    fwd = {tuple(e) for e in directed.tolist()}
    return len(fwd) == len(directed) and all((b, a) in fwd for a, b in fwd)


# ── The extraction ──────────────────────────────────────────────────────────


@pytest.mark.parametrize("scale", ["linear", "log"])
def test_sphere_area_and_volume(sphere_cube, scale):
    (mesh,) = ISO.extract(sphere_cube, [LEVEL], scale=scale, max_triangles=ISO.MAX_TRIANGLES)
    assert mesh.meta["lod"] == 0 and mesh.meta["source"] == "cube" and mesh.meta["scale"] == scale
    assert mesh.raw_triangles == len(mesh.triangles)      # under the budget: not decimated
    da, dv = _errors(_voxels(mesh), mesh.triangles)
    assert abs(da) < 0.01 and abs(dv) < 0.01, (da, dv)
    assert _closed(mesh.triangles)
    # Centred where the field peaks; the normals are unit and point outwards
    # or inwards consistently (the field's gradient side).
    centre = _voxels(mesh).mean(axis=0)
    assert np.allclose(centre, CENTRE, atol=0.05)
    radial = _voxels(mesh) - CENTRE
    radial /= np.linalg.norm(radial, axis=1)[:, None]
    dots = np.einsum("ij,ij->i", radial, mesh.normals.astype(np.float64))
    assert np.allclose(np.linalg.norm(mesh.normals, axis=1), 1.0, atol=1e-3)
    assert (dots > 0.95).all() or (dots < -0.95).all()


def test_log_level_is_contoured_in_log_space(sphere_cube):
    (mesh,) = ISO.extract(sphere_cube, [LEVEL], scale="log")
    assert mesh.meta["contour_value"] == pytest.approx(math.log10(LEVEL))
    with pytest.raises(ValueError, match="no logarithm"):
        ISO.extract(sphere_cube, [0.0], scale="log")
    with pytest.raises(ValueError, match="no logarithm"):
        ISO.extract(sphere_cube, [-1e-5], scale="log")


def test_auto_scale_is_the_reduced_cubes_rule(tmp_path, sphere_cube):
    # A FITS cube with no axis types counts as a simulation volume (its
    # values span six decades); one on the sky stays linear under auto.
    assert ISO.resolve_scale(sphere_cube, "auto", None) == "log"
    sky = tmp_path / "sky.fits"
    hdu = fits.PrimaryHDU(_gauss())
    hdu.header.update(CTYPE1="RA---SIN", CTYPE2="DEC--SIN", CTYPE3="VRAD")
    hdu.writeto(sky)
    assert ISO.resolve_scale(str(sky), "auto", None) == "linear"
    assert ISO.resolve_scale(str(sky), "log", None) == "log"
    # A simulation volume (a VBT's X, Y, Z) over more than three decades: log.
    data = tmp_path / "sim.bin"
    (_gauss() + 1e-12).astype("<f4").tofile(data)
    (tmp_path / "sim.bin.head").write_text(f"float\n1\n{N ** 3} {N} {N} {N} 1 1 1\nlittle\nrho\n")
    assert ISO.resolve_scale(f"{data}::vbt-field=0", "auto", None) == "log"


def test_decimated_to_the_budget(sphere_cube):
    (full,) = ISO.extract(sphere_cube, [LEVEL], scale="linear", max_triangles=ISO.MAX_TRIANGLES)
    for budget in (4000, 1000):
        (mesh,) = ISO.extract(sphere_cube, [LEVEL], scale="linear", max_triangles=budget)
        assert mesh.raw_triangles == len(full.triangles) > budget >= len(mesh.triangles) > 0.9 * budget
        da, dv = _errors(_voxels(mesh), mesh.triangles)
        assert abs(da) < 0.02 and abs(dv) < 0.02, (budget, da, dv)


def test_clustered_then_decimated(sphere_cube, monkeypatch):
    # Far above its budget a surface is clustered first, then decimated.
    called = []
    import vtk
    real = vtk.vtkQuadricClustering

    def spy():
        called.append(1)
        return real()
    monkeypatch.setattr(vtk, "vtkQuadricClustering", spy)
    (mesh,) = ISO.extract(sphere_cube, [LEVEL], scale="linear", max_triangles=ISO.MIN_TRIANGLES)
    assert called and len(mesh.triangles) <= ISO.MIN_TRIANGLES
    da, dv = _errors(_voxels(mesh), mesh.triangles)
    assert abs(da) < 0.03 and abs(dv) < 0.03, (da, dv)


def test_slab_seams_are_merged(sphere_cube, monkeypatch):
    (whole,) = ISO.extract(sphere_cube, [LEVEL], scale="linear", max_triangles=ISO.MAX_TRIANGLES)
    # Slabs of five planes: ten of them, nine seams through the sphere.
    monkeypatch.setattr(ISO, "SLAB_BYTES", 4 * N * N * 5)
    assert len(ISO.slab_ranges(N, 4 * N * N, ISO.SLAB_BYTES)) > 9
    (slabbed,) = ISO.extract(sphere_cube, [LEVEL], scale="linear", max_triangles=ISO.MAX_TRIANGLES)
    assert len(slabbed.triangles) == len(whole.triangles)
    assert len(slabbed.points) == len(whole.points)       # the shared planes' vertices merged
    assert _closed(slabbed.triangles)
    assert ISO.surface_area(_voxels(slabbed), slabbed.triangles) == pytest.approx(
        ISO.surface_area(_voxels(whole), whole.triangles), rel=1e-6)


def test_slab_ranges_cover_every_cell():
    for depth in (2, 3, 7, 64, 65):
        for planes in (2, 3, 5, 100):
            r = ISO.slab_ranges(depth, 1, planes)
            assert r[0][0] == 0 and r[-1][1] == depth
            assert all(b[0] == a[1] - 1 for a, b in zip(r, r[1:]))   # one shared plane
            assert all(z1 - z0 >= 2 and z1 - z0 <= planes for z0, z1 in r)


def test_progress_rises_to_one_and_cancel(sphere_cube, monkeypatch):
    monkeypatch.setattr(ISO, "SLAB_BYTES", 4 * N * N * 8)
    seen = []
    ISO.extract(sphere_cube, [LEVEL, LEVEL * 2], scale="linear", progress=lambda f, s: seen.append(f))
    assert len(seen) > 5 and seen == sorted(seen) and seen[-1] == pytest.approx(1.0)
    with pytest.raises(ISO.IsosurfaceCancelled):
        ISO.extract(sphere_cube, [LEVEL], should_stop=lambda: True)


def test_coarser_level_of_detail(tmp_path):
    # 96³ is more than 50³ voxels: level 1 (48³), read every other voxel (no
    # pyramid); the sphere twice as large in cube voxels as the others.
    path = tmp_path / "big.fits"
    fits.PrimaryHDU(_gauss((96, 96, 96), sigma=2 * SIGMA)).writeto(path)
    (mesh,) = ISO.extract(str(path), [LEVEL], scale="log", max_voxels=50 ** 3)
    assert mesh.meta["lod"] == 1 and mesh.meta["source"] == "strided" and mesh.meta["grid"] == [48, 48, 48]
    assert mesh.meta["factor"] == 2 and mesh.meta["voxel_offset"] == 0.0
    da, dv = _errors(_voxels(mesh), mesh.triangles, 2 * R)
    assert abs(da) < 0.02 and abs(dv) < 0.02, (da, dv)
    # Read every other voxel from 0, the coarse grid's centre is the cube's
    # voxel 94 / 2 = 47 (not 47.5): the sphere is half a voxel off, as a
    # strided sample is (a pyramid level's block means are not).
    assert np.allclose(_voxels(mesh).mean(axis=0), 47.5, atol=0.6)
    with pytest.raises(ValueError, match="not one of the cube's levels"):
        ISO.extract(str(path), [LEVEL], lod=9)


def test_pyramid_level(tmp_path):
    from app.brick_pyramid import build_pyramid, default_pyramid_path
    from app.preview_pyramid import ready_pyramid
    path = tmp_path / "big.fits"
    fits.PrimaryHDU(_gauss((64, 64, 64))).writeto(path)
    build_pyramid(path, default_pyramid_path(str(path)), brick=16)
    pyramid = ready_pyramid(str(path))
    assert pyramid
    (mesh,) = ISO.extract(str(path), [LEVEL], scale="log", lod=1, pyramid_path=pyramid)
    # A block mean of 2³ voxels stands for their centre, half a voxel on.
    assert mesh.meta["source"] == "pyramid" and mesh.meta["voxel_offset"] == 0.5
    pts = _voxels(mesh)
    assert np.allclose(pts.mean(axis=0), 31.5, atol=0.1)
    # The block mean of a Gaussian is a slightly wider one: the sphere is
    # within a few percent of the full-resolution one.
    (ref,) = ISO.extract(str(path), [LEVEL], scale="log", lod=0)
    a = ISO.surface_area(pts, mesh.triangles)
    assert a == pytest.approx(ISO.surface_area(_voxels(ref), ref.triangles), rel=0.04)


def test_level_outside_the_values_is_empty(sphere_cube):
    (mesh,) = ISO.extract(sphere_cube, [PEAK * 10], scale="linear")
    assert len(mesh.triangles) == 0 and len(mesh.points) == 0
    raw = ISO.encode_mesh(mesh)
    assert len(raw) == ISO.HEADER_BYTES
    p, n, t = ISO.decode_mesh(raw)
    assert p.shape == (0, 3) and t.shape == (0, 3)


# ── The wire mesh and the exports ───────────────────────────────────────────


def test_wire_round_trip(sphere_cube):
    (mesh,) = ISO.extract(sphere_cube, [LEVEL], scale="linear", max_triangles=2000)
    raw = ISO.encode_mesh(mesh)
    n, m = len(mesh.points), len(mesh.triangles)
    assert raw[:4] == b"VISO" and struct.unpack_from("<III", raw, 4) == (1, n, m)
    assert len(raw) == ISO.HEADER_BYTES + 6 * n + (-(6 * n)) % 4 + 4 * n + 12 * m
    p, nrm, t = ISO.decode_mesh(raw)
    span = mesh.points.max(axis=0) - mesh.points.min(axis=0)
    assert np.abs(p - mesh.points).max() <= span.max() / 65535 + 1e-7
    assert np.abs(nrm - mesh.normals).max() <= 0.5 / 127 + 1e-6
    assert (t == mesh.triangles).all()


def _read_ply(raw: bytes):
    end = raw.index(b"end_header\n") + len(b"end_header\n")
    head = raw[:end].decode("ascii")
    nv = int(head.split("element vertex ")[1].split()[0])
    nf = int(head.split("element face ")[1].split()[0])
    verts = np.frombuffer(raw, "<f4", nv * 6, end).reshape(nv, 6)
    faces = np.frombuffer(raw, [("n", "u1"), ("i", "<i4", (3,))], nf, end + nv * 24)
    return head, verts, faces


def _vbt(tmp_path, spacing=(2.0, 2.0, 2.0)) -> Path:
    data = tmp_path / "sim.bin"
    with open(data, "wb") as fh:
        (_gauss() + 1e-12).astype("<f4").tofile(fh)
        (-_gauss()).astype("<f4").tofile(fh)
    (tmp_path / "sim.bin.head").write_text(
        f"float\n2\n{N ** 3} {N} {N} {N} {spacing[0]} {spacing[1]} {spacing[2]}\nlittle\nrho\nneg\n")
    return data


def test_ply_and_glb_in_world_coordinates(tmp_path):
    ref = f"{_vbt(tmp_path, (2.0, 3.0, 0.5))}::vbt-field=0"
    (mesh,) = ISO.extract(ref, [LEVEL], scale="log", max_triangles=3000)
    axes = ISO.world_axes(ref)
    assert [a["cdelt"] for a in axes] == [2.0, 3.0, 0.5]
    head, verts, faces = _read_ply(ISO.to_ply(mesh, axes))
    assert "comment coordinates world" in head
    assert (faces["n"] == 3).all() and (faces["i"] == mesh.triangles).all()
    # The VBT's axes start at 0 at the first voxel: world = voxel * spacing.
    centre = verts[:, :3].astype(np.float64).mean(axis=0)
    assert np.allclose(centre, CENTRE * np.array([2.0, 3.0, 0.5]), atol=0.2)
    # An ellipsoid of semi-axes R * spacing, its normals unit.
    assert np.allclose(np.ptp(verts[:, :3], axis=0), 2 * R * np.array([2.0, 3.0, 0.5]), rtol=0.03)
    assert np.allclose(np.linalg.norm(verts[:, 3:], axis=1), 1.0, atol=1e-3)
    glb = ISO.to_glb(mesh, axes, (1.0, 0.5, 0.0, 1.0))
    magic, version, total = struct.unpack_from("<4sII", glb, 0)
    assert (magic, version, total) == (b"glTF", 2, len(glb))
    jlen, jtype = struct.unpack_from("<I4s", glb, 12)
    gltf = json.loads(glb[20:20 + jlen])
    assert jtype == b"JSON" and gltf["accessors"][2]["count"] == 3 * len(mesh.triangles)
    assert gltf["accessors"][0]["count"] == len(mesh.points)
    assert gltf["meshes"][0]["extras"]["coordinates"] == "world"
    # A FITS cube with sky or spectral axes stays in voxels.
    sky = tmp_path / "sky.fits"
    hdu = fits.PrimaryHDU(_gauss())
    hdu.header.update(CTYPE1="RA---SIN", CTYPE2="DEC--SIN", CTYPE3="VRAD")
    hdu.writeto(sky)
    assert ISO.world_axes(str(sky)) is None
    head, _v, _f = _read_ply(ISO.to_ply(mesh, None))
    assert "comment coordinates voxel" in head


# ── The routes ──────────────────────────────────────────────────────────────


async def _open(client, auth_headers, path):
    r = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=auth_headers)
    assert r.status_code == 200, r.text
    body = r.json()
    return body["dataset_id"], {**auth_headers, "X-Visivo-Session": body["session_id"]}


async def _finish(client, headers, task_id, timeout=60.0):
    progress = []
    for _ in range(int(timeout / 0.05)):
        r = await client.get(f"/v1/tasks/{task_id}", headers=headers)
        assert r.status_code == 200, r.text
        body = r.json()
        progress.append(body["progress"])
        if body["status"] not in ("pending", "running"):
            return body, progress
        await asyncio.sleep(0.05)
    raise AssertionError("the isosurface task never ended")


async def test_route_task_cache_and_mesh(client, auth_headers, sphere_cube):
    dataset, headers = await _open(client, auth_headers, sphere_cube)
    body = {"dataset_id": dataset, "levels": [LEVEL], "scale": "log", "max_triangles": 5000}
    r = await client.post("/v1/isosurface/extract", json=body, headers=headers)
    assert r.status_code == 200, r.text
    created = r.json()
    assert created["status"] == "running" and not created["cache_hit"]
    task, progress = await _finish(client, headers, created["task_id"])
    assert task["status"] == "completed", task
    assert progress == sorted(progress) and progress[-1] == 1.0
    res = task["result"]
    assert res["scale"] == "log" and res["cache_hits"] == 0 and len(res["meshes"]) == 1
    m = res["meshes"][0]
    assert m["triangles"] <= 5000 < m["raw_triangles"] and m["lod"] == 0
    raw = await client.get(f"/v1/isosurface/mesh/{m['mesh_id']}", headers=headers)
    assert raw.status_code == 200 and raw.headers["content-type"] == "application/octet-stream"
    assert len(raw.content) == m["bytes"]
    p, _n, t = ISO.decode_mesh(raw.content)
    da, dv = _errors(p.astype(np.float64) * N - 0.5, t)
    assert abs(da) < 0.02 and abs(dv) < 0.02

    # The same level again: completed at once from the cache, the same mesh.
    r = await client.post("/v1/isosurface/extract", json=body, headers=headers)
    again = r.json()
    assert again["status"] == "completed" and again["cache_hit"]
    task2, _ = await _finish(client, headers, again["task_id"])
    assert task2["result"]["meshes"][0]["mesh_id"] == m["mesh_id"] and task2["result"]["cache_hits"] == 1
    # One more level: only that one is made; the order is the request's.
    body2 = {**body, "levels": [LEVEL * 3, LEVEL]}
    r = await client.post("/v1/isosurface/extract", json=body2, headers=headers)
    task3, _ = await _finish(client, headers, r.json()["task_id"])
    res3 = task3["result"]
    assert res3["cache_hits"] == 1 and res3["meshes"][1]["mesh_id"] == m["mesh_id"]
    assert res3["meshes"][0]["level"] == pytest.approx(LEVEL * 3)
    # Another budget, scale or level of detail is another surface.
    for change in ({"max_triangles": 4000}, {"scale": "linear"}, {"lod": 0}):
        r = await client.post("/v1/isosurface/extract", json={**body, **change}, headers=headers)
        t4, _ = await _finish(client, headers, r.json()["task_id"])
        assert t4["status"] == "completed", (change, t4)
        assert t4["result"]["meshes"][0]["mesh_id"] != m["mesh_id"], change

    # Exports: PLY sent back, glTF saved in the workspace's exports.
    ply = await client.get(f"/v1/isosurface/mesh/{m['mesh_id']}/export", headers=headers)
    assert ply.status_code == 200 and ply.content.startswith(b"ply\n")
    assert 'filename="sphere.fits_iso_' in ply.headers["content-disposition"]
    saved = await client.get(f"/v1/isosurface/mesh/{m['mesh_id']}/export",
                             params={"format": "glb", "save": "true", "color": "#ff8800"}, headers=headers)
    out = saved.json()
    assert out["valid"] and Path(out["path"]).read_bytes()[:4] == b"glTF"
    assert Path(out["path"]).parent.name == "isosurfaces"
    assert (await client.get("/v1/isosurface/mesh/" + "0" * 24, headers=headers)).status_code == 404
    assert (await client.get("/v1/isosurface/mesh/../x", headers=headers)).status_code == 404


async def test_route_refusals(client, auth_headers, sphere_cube):
    dataset, headers = await _open(client, auth_headers, sphere_cube)
    url = "/v1/isosurface/extract"
    r = await client.post(url, json={"dataset_id": dataset, "levels": [-1.0], "scale": "log"}, headers=headers)
    assert r.status_code == 422 and "logarithm" in r.text
    r = await client.post(url, json={"dataset_id": dataset, "levels": []}, headers=headers)
    assert r.status_code == 422
    r = await client.post(url, json={"dataset_id": dataset, "levels": [1e-4, 1e-4]}, headers=headers)
    assert r.status_code == 422
    r = await client.post(url, json={"dataset_id": dataset, "levels": [1e-4], "max_triangles": 10}, headers=headers)
    assert r.status_code == 422
    r = await client.post(url, json={"dataset_id": dataset, "levels": [1e-4], "field": "T"}, headers=headers)
    assert r.status_code == 422 and "No field" in r.text
    r = await client.post(url, json={"dataset_id": dataset, "levels": [1e-4]})
    assert r.status_code in (401, 403)


async def test_route_pluto_variable_and_sibling_field(client, auth_headers, tmp_path):
    spec = importlib.util.spec_from_file_location("generate_demo_data", _GEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    shape = (32, 32, 32)
    # Squared: about five decades, so auto gives a log scale.
    rho = _gauss(shape).astype(np.float64) ** 2 + 1e-12
    temp = 1e4 * _gauss(shape, centre=(15.5, 15.5, 10.0)).astype(np.float64) + 1.0
    first = mod.write_pluto(tmp_path / "p", {"rho": rho, "T": temp}, mode="multiple_files",
                            precision="flt", endian="little")
    dataset, headers = await _open(client, auth_headers, first)
    r = await client.post("/v1/isosurface/extract", json={"dataset_id": dataset, "levels": [LEVEL ** 2]}, headers=headers)
    task, _ = await _finish(client, headers, r.json()["task_id"])
    assert task["status"] == "completed", task
    m = task["result"]["meshes"][0]
    assert task["result"]["scale"] == "log" and m["full_shape"] == [32, 32, 32]
    raw = (await client.get(f"/v1/isosurface/mesh/{m['mesh_id']}", headers=headers)).content
    p, _n, t = ISO.decode_mesh(raw)
    assert np.allclose((p.astype(np.float64) * 32 - 0.5).mean(axis=0), 15.5, atol=0.1)
    # The temperature beside it, by name: its sphere sits elsewhere.
    r = await client.post("/v1/isosurface/extract",
                          json={"dataset_id": dataset, "field": "T", "levels": [1e4 * PEAK * math.exp(-0.5 * (6 / SIGMA) ** 2) + 1.0]},
                          headers=headers)
    task, _ = await _finish(client, headers, r.json()["task_id"])
    assert task["status"] == "completed", task
    raw = (await client.get(f"/v1/isosurface/mesh/{task['result']['meshes'][0]['mesh_id']}", headers=headers)).content
    p, _n, t = ISO.decode_mesh(raw)
    assert np.allclose((p.astype(np.float64) * 32 - 0.5).mean(axis=0), [10.0, 15.5, 15.5], atol=0.2)
    # PLY of a PLUTO variable: world coordinates on [-1, 1].
    ply = await client.get(f"/v1/isosurface/mesh/{m['mesh_id']}/export", headers=headers)
    head, verts, _f = _read_ply(ply.content)
    assert "comment coordinates world" in head
    assert np.allclose(verts[:, :3].mean(axis=0), 0.0, atol=0.01)
