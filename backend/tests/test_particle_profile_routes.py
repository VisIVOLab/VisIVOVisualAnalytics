"""The profile and phase-diagram routes and tasks on a Gadget snapshot read
out of core and on the same particles loaded as a VBT table (issue #426)."""
from __future__ import annotations

import asyncio
import threading

import numpy as np
import pytest

BOX = 1000.0
N = 20_000


def _halo(seed=21):
    """An NFW halo (c = 8, r200 = 100) centred on the box face x = 0."""
    from scipy.special import lambertw
    rng = np.random.default_rng(seed)
    c = 8.0
    mu = rng.uniform(0, 1, N) * (np.log(1 + c) - c / (1 + c))
    x = -1.0 / np.real(lambertw(-np.exp(-mu - 1.0))) - 1.0
    cos_t = rng.uniform(-1, 1, N)
    phi = rng.uniform(0, 2 * np.pi, N)
    sin_t = np.sqrt(1 - cos_t ** 2)
    r = 100.0 / c * x
    pos = np.column_stack([r * sin_t * np.cos(phi), r * sin_t * np.sin(phi), r * cos_t])
    pos += np.array([0.0, 500.0, 500.0])
    pos = np.mod(pos, BOX)
    vel = rng.normal(0, 50.0, (N, 3))
    mass = rng.uniform(1.0, 2.0, N)
    return pos, vel, mass


def _gadget(tmp_path, pos, vel, mass):
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "halo.hdf5"
    with h5py.File(path, "w") as h:
        hd = h.create_group("Header")
        hd.attrs["NumPart_ThisFile"] = np.array([0, N, 0, 0, 0, 0], dtype=np.int32)
        hd.attrs["NumPart_Total"] = np.array([0, N, 0, 0, 0, 0], dtype=np.uint32)
        hd.attrs["MassTable"] = np.zeros(6)
        hd.attrs["NumFilesPerSnapshot"] = 1
        hd.attrs["BoxSize"] = BOX
        hd.attrs["Time"] = 1.0
        hd.attrs["Redshift"] = 0.0
        hd.attrs["HubbleParam"] = 0.7
        hd.attrs["Omega0"] = 0.3
        hd.attrs["OmegaLambda"] = 0.7
        h.create_group("Parameters").attrs["ComovingIntegrationOn"] = 1
        g = h.create_group("PartType1")
        g.create_dataset("Coordinates", data=pos.astype(np.float32))
        g.create_dataset("Velocities", data=vel.astype(np.float32))
        g.create_dataset("Masses", data=mass.astype(np.float32))
        g.create_dataset("ParticleIDs", data=np.arange(N, dtype=np.uint32))
    return str(path)


def _vbt(tmp_path, pos, vel, mass):
    data = np.vstack([pos.T, vel.T, mass[None, :]]).astype("<f4")
    binary = tmp_path / "halo.bin"
    data.tofile(binary)
    (tmp_path / "halo.bin.head").write_text(
        f"float\n7\n{N}\nlittle\nx\ny\nz\nvx\nvy\nvz\nmass\n")
    return str(tmp_path / "halo.bin.head")


async def _open(client, headers, path, fmt):
    body = (await client.post("/v1/catalogue/open", json={"path": path, "format": fmt},
                              headers=headers)).json()
    assert body["valid"], body
    return body["dataset_id"]


async def _wait(client, headers, task_id):
    for _ in range(600):
        status = (await client.get(f"/v1/tasks/{task_id}", headers=headers)).json()
        if status["status"] not in ("pending", "running"):
            return status
        await asyncio.sleep(0.02)
    raise AssertionError("task did not finish")


@pytest.mark.asyncio
async def test_profile_out_of_core_equals_in_memory_and_wraps(client, auth_headers, tmp_path,
                                                                monkeypatch):
    from app.routers import catalogue as cat
    monkeypatch.setattr(cat, "PARTICLE_CHUNK_ROWS", 3_001)
    monkeypatch.setattr(cat, "DIAGNOSTIC_CHUNK_ROWS", 4_999)
    pos, vel, mass = _halo()
    gadget = await _open(client, auth_headers, _gadget(tmp_path, pos, vel, mass), "gadget")
    vbt = await _open(client, auth_headers, _vbt(tmp_path, pos, vel, mass), "vbt")
    request = {"columns": ["radial_velocity"], "bins": 12,
               "centre": {"mode": "shrinking_sphere"}}
    created = (await client.post("/v1/tasks/catalogue_profile",
                                 json={"dataset_id": gadget, **request},
                                 headers=auth_headers)).json()
    status = await _wait(client, auth_headers, created["task_id"])
    assert status["status"] == "completed" and status["operation"] == "catalogue_profile"
    out_of_core = status["result"]
    assert out_of_core["valid"], out_of_core["error"]
    in_memory = (await client.post("/v1/catalogue/profile",
                                   json={"dataset_id": vbt, "period": BOX, **request},
                                   headers=auth_headers)).json()
    assert in_memory["valid"], in_memory["error"]

    found = np.asarray(out_of_core["centre"]["centre"])
    delta = found - np.array([0.0, 500.0, 500.0])
    delta -= BOX * np.round(delta / BOX)
    assert np.linalg.norm(delta) <= 1.0                  # 1 % of r200 = 100
    np.testing.assert_allclose(out_of_core["centre"]["centre"], in_memory["centre"]["centre"],
                               atol=1e-6 * BOX)
    assert out_of_core["profile"]["count"] == in_memory["profile"]["count"]
    np.testing.assert_allclose(out_of_core["profile"]["mass"], in_memory["profile"]["mass"],
                               rtol=1e-9)
    assert sum(out_of_core["profile"]["count"]) + out_of_core["profile"]["inside_r_min"][
        "count"] + out_of_core["profile"]["outside_r_max"]["count"] == N
    assert out_of_core["profile"]["r_max"] <= BOX / 2
    assert out_of_core["units"]["radius"] and out_of_core["provenance"]["selection"] == [
        "complete dataset"]
    assert "minimum-image" in out_of_core["provenance"]["frame"]
    assert out_of_core["units"]["physical"]["radius"]["unit"] == "kpc"

    unwrapped = (await client.post("/v1/catalogue/profile",
                                   json={"dataset_id": vbt, "period": 0, **request},
                                   headers=auth_headers)).json()
    far = np.asarray(unwrapped["centre"]["centre"]) - np.array([0.0, 500.0, 500.0])
    far -= BOX * np.round(far / BOX)
    assert np.linalg.norm(far) > 1.0


@pytest.mark.asyncio
async def test_phase_diagram_task_closes_the_mass_of_a_selection(client, auth_headers,
                                                                  tmp_path):
    pos, vel, mass = _halo(seed=22)
    vbt = await _open(client, auth_headers, _vbt(tmp_path, pos, vel, mass), "vbt")
    selection = [{"op": "inside", "sphere": {"fields": ["x", "y", "z"],
                                             "center": [0.0, 500.0, 500.0], "radius": 60.0,
                                             "period": BOX}}]
    body = {"dataset_id": vbt, "filters": selection, "weight_field": "mass", "period": BOX,
            "x": {"field": "radius", "scale": "log", "bins": 24},
            "y": {"field": "radial_velocity", "bins": 20},
            "centre": {"mode": "point", "point": [0.0, 500.0, 500.0]}}
    created = (await client.post("/v1/tasks/catalogue_phase_diagram", json=body,
                                 headers=auth_headers)).json()
    status = await _wait(client, auth_headers, created["task_id"])
    result = status["result"]
    assert result["valid"], result["error"]
    d = pos - np.array([0.0, 500.0, 500.0])
    d -= BOX * np.round(d / BOX)
    inside = np.sum(d * d, axis=1) <= 3600.0
    expected = float(mass.astype(np.float32).astype(np.float64)[inside].sum())
    diagram = result["diagram"]
    assert diagram["selected_rows"] == int(inside.sum())
    assert diagram["selected_weight"] == pytest.approx(expected, rel=1e-9)
    assert diagram["binned_weight"] + diagram["outside_weight"] + diagram["dropped_weight"] \
        == pytest.approx(expected, rel=1e-12)
    assert result["centre"]["centre_velocity_from"]["particles"] >= 1000
    assert result["provenance"]["selection"][0].startswith("sphere: inside")


@pytest.mark.asyncio
async def test_a_profile_task_can_be_cancelled(client, auth_headers, tmp_path, monkeypatch):
    from app.routers import particle_profiles as routes
    pos, vel, mass = _halo(seed=23)
    vbt = await _open(client, auth_headers, _vbt(tmp_path, pos, vel, mass), "vbt")
    gate, release = threading.Event(), threading.Event()
    real = routes._selection_chunks

    def slow(entry, filters, columns):
        chunks = real(entry, filters, columns)

        def gated():
            for item in chunks():
                gate.set()
                release.wait(5)
                yield item
        return gated
    monkeypatch.setattr(routes, "_selection_chunks", slow)
    created = (await client.post("/v1/tasks/catalogue_profile",
                                 json={"dataset_id": vbt, "period": BOX},
                                 headers=auth_headers)).json()
    await asyncio.to_thread(gate.wait, 5)
    cancelled = await client.delete(f"/v1/tasks/{created['task_id']}", headers=auth_headers)
    assert cancelled.status_code == 200
    release.set()
    status = (await client.get(f"/v1/tasks/{created['task_id']}", headers=auth_headers)).json()
    assert status["status"] == "cancelled"


@pytest.mark.asyncio
async def test_refusals_are_explained(client, auth_headers, tmp_path):
    pos, vel, mass = _halo(seed=24)
    vbt = await _open(client, auth_headers, _vbt(tmp_path, pos, vel, mass), "vbt")
    bad = (await client.post("/v1/catalogue/profile",
                             json={"dataset_id": vbt, "columns": ["nope"]},
                             headers=auth_headers)).json()
    assert not bad["valid"] and "Unknown field" in bad["error"]
    bad = (await client.post("/v1/catalogue/phase_diagram",
                             json={"dataset_id": vbt, "x": {"field": "radius"},
                                   "y": {"field": "mass"}}, headers=auth_headers)).json()
    assert not bad["valid"] and "centre" in bad["error"]
    bad = (await client.post("/v1/catalogue/profile",
                             json={"dataset_id": vbt, "centre": {"mode": "point"}},
                             headers=auth_headers)).json()
    assert not bad["valid"] and "three finite" in bad["error"]


@pytest.mark.asyncio
async def test_a_target_row_is_a_centre(client, auth_headers, tmp_path):
    pos, vel, mass = _halo(seed=25)
    vbt = await _open(client, auth_headers, _vbt(tmp_path, pos, vel, mass), "vbt")
    targets = np.array([[10.0, 20.0, 30.0], [1.0, 500.0, 499.0]], dtype="<f4").T.copy()
    (tmp_path / "targets.bin").write_bytes(targets.tobytes())
    (tmp_path / "targets.bin.head").write_text("float\n3\n2\nlittle\nx\ny\nz\n")
    target = await _open(client, auth_headers, str(tmp_path / "targets.bin.head"), "vbt")
    out = (await client.post("/v1/catalogue/profile", json={
        "dataset_id": vbt, "period": BOX, "bins": 8,
        "centre": {"mode": "target", "target_dataset_id": target, "target_row": 1}},
        headers=auth_headers)).json()
    assert out["valid"], out["error"]
    assert out["centre"]["centre"] == pytest.approx([1.0, 500.0, 499.0])
    assert out["centre"]["target_row"] == 1


@pytest.mark.asyncio
async def test_radius_axis_with_a_centre_velocity_and_a_centre_outside_a_box(
        client, auth_headers, tmp_path):
    pos, vel, mass = _halo(seed=26)
    vbt = await _open(client, auth_headers, _vbt(tmp_path, pos, vel, mass), "vbt")
    out = (await client.post("/v1/catalogue/phase_diagram", json={
        "dataset_id": vbt, "period": BOX, "weight_field": "mass",
        "x": {"field": "radius", "scale": "log", "bins": 8}, "y": {"field": "mass", "bins": 8},
        "centre": {"mode": "point", "point": [0.0, 500.0, 500.0], "velocity": [0, 0, 0]}},
        headers=auth_headers)).json()
    assert out["valid"], out["error"]
    box = [{"op": "inside", "box": {"fields": ["x", "y", "z"], "min": [10, 450, 450],
                                    "max": [60, 550, 550], "period": 0}}]
    prof = (await client.post("/v1/catalogue/profile", json={
        "dataset_id": vbt, "period": 0, "filters": box, "bins": 6,
        "centre": {"mode": "point", "point": [100.0, 500.0, 500.0]}},
        headers=auth_headers)).json()
    assert prof["valid"], prof["error"]
    assert prof["profile"]["complete_radius"] == 0.0
    assert not any(prof["profile"]["complete"])
    bad = (await client.post("/v1/catalogue/profile", json={
        "dataset_id": vbt, "centre": {"mode": "point", "point": [0, 500, 500],
                                      "velocity": [0, 0]}}, headers=auth_headers)).json()
    assert not bad["valid"] and "three finite" in bad["error"]


@pytest.mark.asyncio
async def test_a_cancelled_task_gives_its_slot_back(client, auth_headers, tmp_path, monkeypatch):
    from app.routers import particle_profiles as routes
    pos, vel, mass = _halo(seed=27)
    vbt = await _open(client, auth_headers, _vbt(tmp_path, pos, vel, mass), "vbt")
    gate, release = threading.Event(), threading.Event()
    real = routes._selection_chunks

    def slow(entry, filters, columns):
        chunks = real(entry, filters, columns)

        def gated():
            for item in chunks():
                gate.set()
                release.wait(5)
                yield item
        return gated
    monkeypatch.setattr(routes, "_selection_chunks", slow)
    first = (await client.post("/v1/tasks/catalogue_profile",
                               json={"dataset_id": vbt, "period": BOX},
                               headers=auth_headers)).json()
    await asyncio.to_thread(gate.wait, 5)
    await client.delete(f"/v1/tasks/{first['task_id']}", headers=auth_headers)
    release.set()
    monkeypatch.setattr(routes, "_selection_chunks", real)
    for _ in range(100):
        again = await client.post("/v1/tasks/catalogue_profile",
                                  json={"dataset_id": vbt, "period": BOX, "bins": 6},
                                  headers=auth_headers)
        if again.status_code == 200:
            break
        await asyncio.sleep(0.05)
    assert again.status_code == 200
    status = await _wait(client, auth_headers, again.json()["task_id"])
    assert status["status"] == "completed" and status["result"]["valid"]
