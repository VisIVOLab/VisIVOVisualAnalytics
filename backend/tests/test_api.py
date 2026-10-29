"""
test_api.py – pytest-asyncio test suite for the VisIVO /v1/ API.

Covers:
  - Authentication (401 on missing/wrong token)
  - Health endpoint
  - File listing and header extraction
  - Dataset open (image and cube)
  - Session isolation (dataset from one session not visible in another)
  - Cube slice, subvolume, preview, moment, PV diagram
  - Image full and preview
  - Error handling for invalid inputs

Run with:
    cd backend
    pytest tests/ -v --asyncio-mode=auto
"""

from __future__ import annotations

import asyncio
import base64
import struct

import numpy as np
import pytest
import pytest_asyncio
from httpx import AsyncClient


# ── Helpers ───────────────────────────────────────────────────────────────────


def _decode_f32(b64: str) -> np.ndarray:
    """Decode a plain base64 float32 array."""
    return np.frombuffer(base64.b64decode(b64), dtype=np.float32)


def _decode_qt_zlib(b64: str) -> np.ndarray:
    """Decode a qt-zlib compressed float32 payload."""
    import zlib
    raw = base64.b64decode(b64)
    expected_len = struct.unpack(">I", raw[:4])[0]
    decompressed = zlib.decompress(raw[4:])
    assert len(decompressed) == expected_len
    return np.frombuffer(decompressed, dtype=np.float32)


# ── Auth tests ────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_no_token_returns_401(client: AsyncClient) -> None:
    resp = await client.get("/v1/health")
    assert resp.status_code == 401


@pytest.mark.asyncio
async def test_wrong_token_returns_401(client: AsyncClient) -> None:
    resp = await client.get("/v1/health", headers={"X-Visivo-Token": "wrong-token"})
    assert resp.status_code == 401


# ── Health ────────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_health_ok(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.get("/v1/health", headers=auth_headers)
    assert resp.status_code == 200
    data = resp.json()
    assert data["ok"] is True
    assert "workers" in data
    assert "active_sessions" in data
    assert "product_cache_entries" in data
    assert "product_cache_capacity" in data
    assert "task_registry_entries" in data
    assert "task_ttl_enabled" in data
    assert "task_ttl_seconds" in data
    assert "X-Request-ID" in resp.headers  # R8: request-id header present


# ── File operations ───────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_list_files_home(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.get("/v1/files/list", headers=auth_headers)
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert isinstance(data["entries"], list)


@pytest.mark.asyncio
async def test_list_files_invalid_path(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.get(
        "/v1/files/list",
        params={"path": "/nonexistent_path_xyz_12345"},
        headers=auth_headers,
    )
    assert resp.status_code == 200
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_file_header_fits(
    client: AsyncClient, auth_headers: dict, synthetic_fits_image
) -> None:
    resp = await client.post(
        "/v1/files/header",
        json={"path": str(synthetic_fits_image)},
        headers=auth_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    cards_joined = " ".join(data["cards"])
    assert "NAXIS" in cards_joined
    assert "CTYPE1" in cards_joined


@pytest.mark.asyncio
async def test_file_header_nonexistent(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.post(
        "/v1/files/header",
        json={"path": "/does/not/exist.fits"},
        headers=auth_headers,
    )
    assert resp.json()["valid"] is False


# ── Dataset open ──────────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_open_image(opened_image: dict) -> None:
    assert opened_image["dataset_id"].startswith("ds_")
    assert opened_image["session_id"] != ""


@pytest.mark.asyncio
async def test_open_cube(opened_cube: dict) -> None:
    assert opened_cube["dataset_id"].startswith("ds_")


@pytest.mark.asyncio
async def test_open_image_metadata(
    client: AsyncClient, auth_headers: dict, synthetic_fits_image
) -> None:
    resp = await client.post(
        "/v1/datasets/open",
        json={"path": str(synthetic_fits_image)},
        headers=auth_headers,
    )
    data = resp.json()
    assert data["kind"] == "image"
    assert data["width"] == 64
    assert data["height"] == 64
    assert data["depth"] == 1
    assert data["active_axes"] == 2
    assert data["ctype"][0] == "RA---SIN"
    assert data["wcs_status"] == "ok"
    assert data["wcs_warning_message"] == ""


@pytest.mark.asyncio
async def test_open_cube_metadata(
    client: AsyncClient, auth_headers: dict, synthetic_fits_cube
) -> None:
    resp = await client.post(
        "/v1/datasets/open",
        json={"path": str(synthetic_fits_cube)},
        headers=auth_headers,
    )
    data = resp.json()
    assert data["kind"] == "cube"
    assert data["width"] == 64
    assert data["height"] == 64
    assert data["depth"] == 32
    assert data["active_axes"] == 3
    assert data["wcs_status"] == "ok"


@pytest.mark.asyncio
async def test_open_nonexistent_file(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.post(
        "/v1/datasets/open",
        json={"path": "/no/such/file.fits"},
        headers=auth_headers,
    )
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_open_malformed_celestial_unit_fits(
    client: AsyncClient, auth_headers: dict, malformed_unit_fits_image
) -> None:
    resp = await client.post(
        "/v1/datasets/open",
        json={"path": str(malformed_unit_fits_image)},
        headers=auth_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert data["kind"] == "image"
    assert data["wcs_status"] == "sanitized"
    assert "compatibility" in data["wcs_warning_message"]
    assert 1 in data["wcs_sanitized_axes"]


# ── Session isolation ─────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_session_isolation(
    client: AsyncClient,
    auth_headers: dict,
    synthetic_fits_cube,
) -> None:
    """A dataset opened in session A must NOT be accessible from session B."""
    import uuid

    # ── Session A: open a cube, get its dataset_id and session_id ────────────
    resp_a = await client.post(
        "/v1/datasets/open",
        json={"path": str(synthetic_fits_cube)},
        headers=auth_headers,  # no session → anonymous session A
    )
    data_a = resp_a.json()
    assert data_a["valid"]
    dataset_id_a = data_a["dataset_id"]
    session_id_a = data_a["session_id"]

    # ── Session B: supply a brand-new random session_id ───────────────────────
    # The registry will create a fresh session because this UUID is unknown.
    session_id_b = str(uuid.uuid4())
    headers_b = {**auth_headers, "X-Visivo-Session": session_id_b}

    # Try to access session A's dataset from session B – must be 404.
    resp = await client.post(
        "/v1/cube/slice",
        json={"dataset_id": dataset_id_a, "axis": "z", "index": 0},
        headers=headers_b,
    )
    assert resp.status_code == 404, (
        f"Expected 404 for cross-session access, got {resp.status_code}: {resp.text}"
    )


# ── Cube endpoints ────────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_cube_slice_z(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/slice",
        json={"dataset_id": opened_cube["dataset_id"], "axis": "z", "index": 0},
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert data["width"] == 64
    assert data["height"] == 64
    assert data["compression"] == "qt-zlib"
    # Decode and verify shape.
    arr = _decode_qt_zlib(data["data_base64"])
    assert arr.shape == (64 * 64,)


@pytest.mark.asyncio
async def test_cube_slice_out_of_range(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/slice",
        json={"dataset_id": opened_cube["dataset_id"], "axis": "z", "index": 9999},
        headers=opened_cube["headers"],
    )
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_cube_subvolume(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/subvolume",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "x0": 10, "x1": 20, "y0": 10, "y1": 20, "z0": 5, "z1": 10,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert data["width"] == 11
    assert data["height"] == 11
    assert data["depth"] == 6


@pytest.mark.asyncio
async def test_cube_mask_registers_new_dataset(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/mask",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "x0": 10, "x1": 20, "y0": 10, "y1": 20, "z0": 5, "z1": 10,
            "mode": "blank_outside",
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["valid"] is True
    assert data["new_dataset_id"].startswith("ds_")
    # Masking keeps full cube dimensions (unlike subvolume which crops).
    assert (data["width"], data["height"], data["depth"]) == (64, 64, 32)


@pytest.mark.asyncio
async def test_cube_mask_bad_mode_422(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/mask",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "x0": 0, "x1": 5, "y0": 0, "y1": 5, "z0": 0, "z1": 5, "mode": "nope",
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 422, resp.text


@pytest.mark.asyncio
async def test_cube_kinematic_select_bin(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/kinematic_select_bin",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "seed_z": 16, "seed_y": 32, "seed_x": 32,
            "reach": 8.0, "lambda_v": 1.0, "roi_radius": 20,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200, resp.text
    # Payload is the ROI (bounding box of the selection), not the full cube.
    rw = int(resp.headers["X-Visivo-Width"])
    rh = int(resp.headers["X-Visivo-Height"])
    rd = int(resp.headers["X-Visivo-Depth"])
    assert len(resp.content) == rw * rh * rd * 4          # float32 ROI mask
    assert resp.headers["X-Visivo-Full-Shape"] == "64,64,32"
    assert len(resp.headers["X-Visivo-Origin"].split(",")) == 3
    count = int(resp.headers["X-Visivo-Selected-Count"])
    assert count >= 1                                     # at least the seed
    assert len(resp.headers["X-Visivo-Bbox"].split(",")) == 6
    assert resp.headers["X-Visivo-Trim-Box"] == "0"       # nothing cut on a small cube


@pytest.mark.asyncio
async def test_cube_kinematic_select_bin_seed_outside_is_422(
    client: AsyncClient, opened_cube: dict
) -> None:
    # A seed outside the cube is bad user input, not a server error → 422, not 500.
    resp = await client.post(
        "/v1/cube/kinematic_select_bin",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "seed_z": 999, "seed_y": 32, "seed_x": 32,
            "reach": 8.0, "lambda_v": 1.0, "roi_radius": 20,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 422, resp.text


@pytest.mark.asyncio
async def test_cube_kinematic_select_bin_multi_seed(
    client: AsyncClient, opened_cube: dict
) -> None:
    # ⇧-click add-seed + ⌥-click sub-seed round-trip through the API.
    resp = await client.post(
        "/v1/cube/kinematic_select_bin",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "seed_z": 16, "seed_y": 32, "seed_x": 32,
            "add_seeds": [[16, 30, 30]],
            "sub_seeds": [[16, 40, 40]],
            "reach": 8.0, "lambda_v": 1.0, "roi_radius": 20,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200, resp.text
    assert int(resp.headers["X-Visivo-Selected-Count"]) >= 1


@pytest.mark.asyncio
async def test_cube_mask_selection_registers_new_dataset(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/mask_selection",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "seed_z": 16, "seed_y": 32, "seed_x": 32,
            "reach": 8.0, "lambda_v": 1.0, "roi_radius": 20,
            "mode": "blank_outside",
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["valid"] is True
    assert data["new_dataset_id"].startswith("ds_")
    assert (data["width"], data["height"], data["depth"]) == (64, 64, 32)  # full dims


@pytest.mark.asyncio
async def test_cube_mask_apply_bin_explicit_mask(client: AsyncClient, opened_cube: dict) -> None:
    # Apply an explicit ROI mask (float32 0/1, x-fastest) — no re-grow from seeds.
    import numpy as np
    rw, rh, rd = 5, 4, 3
    m = np.zeros((rd, rh, rw), dtype="<f4")
    m[1, 1:3, 1:3] = 1.0                       # a few selected voxels
    resp = await client.post(
        "/v1/cube/mask_apply_bin",
        params={
            "dataset_id": opened_cube["dataset_id"],
            "origin_x": 20, "origin_y": 24, "origin_z": 8,
            "width": rw, "height": rh, "depth": rd,
            "mode": "blank_outside",
            "recipe": "seed_zyx=9,25,21; reach=8; lambda_v=1.00; nsigma=2.5; smooth_zyx=2,6,6",
        },
        content=m.tobytes(),
        headers={**opened_cube["headers"], "Content-Type": "application/octet-stream"},
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["valid"] is True
    assert data["new_dataset_id"].startswith("ds_")
    # How the mask was made travels with the file: one HISTORY card per parameter.
    from astropy.io import fits as _f
    history = [str(c) for c in _f.getheader(data["output_path"])["HISTORY"]]
    for item in ("Lasso seed_zyx=9,25,21", "Lasso reach=8", "Lasso nsigma=2.5", "Lasso smooth_zyx=2,6,6"):
        assert item in history, history
    assert (data["width"], data["height"], data["depth"]) == (64, 64, 32)  # full dims kept

    # The EXACT mask must have been applied verbatim (not re-grown): for blank_outside
    # ONLY the selected ROI voxels stay finite, everything else is NaN.
    from astropy.io import fits
    out = np.squeeze(fits.getdata(data["output_path"]))
    assert out.shape == (32, 64, 64)                       # (z, y, x)
    finite = np.isfinite(out)
    assert int(finite.sum()) == int((m > 0.5).sum())       # == selected-voxel count
    sub = finite[8:8 + rd, 24:24 + rh, 20:20 + rw]         # the ROI region (oz,oy,ox)
    assert np.array_equal(sub, (m > 0.5))                  # finite exactly where the mask is


@pytest.mark.asyncio
async def test_cube_mask_apply_bin_bad_size_422(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/mask_apply_bin",
        params={
            "dataset_id": opened_cube["dataset_id"],
            "origin_x": 0, "origin_y": 0, "origin_z": 0,
            "width": 4, "height": 4, "depth": 4,   # expects 256 bytes
            "mode": "blank_inside",
        },
        content=b"\x00\x00\x00\x00",              # 4 bytes → mismatch
        headers={**opened_cube["headers"], "Content-Type": "application/octet-stream"},
    )
    assert resp.status_code == 422, resp.text


@pytest.mark.asyncio
async def test_cube_preview(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/preview",
        json={"dataset_id": opened_cube["dataset_id"], "downsample": 2},
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert data["depth"] == 16   # 32 // 2
    assert data["width"] == 32   # 64 // 2


@pytest.mark.asyncio
@pytest.mark.parametrize("order", [0, 1, 2, 6, 8, 10])
async def test_moment_map_orders(
    client: AsyncClient, opened_cube: dict, order: int
) -> None:
    resp = await client.post(
        "/v1/products/moment",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "moment_order": order,
            "channel_start": 10,
            "channel_end": 20,
            "mask_enabled": False,
            "threshold_value": 0.0,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True, f"Moment {order} failed: {data['error']}"
    assert data["width"] == 64
    assert data["height"] == 64
    arr = _decode_f32(data["data_base64"])
    assert arr.shape == (64 * 64,)
    assert np.isfinite(arr).any()
    # Scientific metadata must be present and non-empty.
    assert data.get("bunit") == "Jy/beam", f"bunit missing or wrong for M{order}"
    assert data.get("spectral_axis_type") == "FREQ"
    assert data.get("spectral_axis_unit") == "Hz"
    assert data.get("moment_unit") != "", f"moment_unit empty for M{order}"


@pytest.mark.asyncio
async def test_moment_with_mask(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/products/moment",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "moment_order": 0,
            "channel_start": 0,
            "channel_end": 31,
            "mask_enabled": True,
            "threshold_value": 3.0,
        },
        headers=opened_cube["headers"],
    )
    data = resp.json()
    assert data["valid"] is True
    arr = _decode_f32(data["data_base64"])
    # With mask only the bright emission-line region should contribute.
    assert np.nansum(arr) > 0


@pytest.mark.asyncio
async def test_cube_pv(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/pv",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "vertices": [[10, 32], [54, 32]],
            "width_pixels": 3,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert data["depth"] == 32
    assert data["valid_samples"] > 0
    assert data["compression"] == "qt-zlib"
    assert "spectral_axis_type" in data


@pytest.mark.asyncio
async def test_moment_task_endpoint(client: AsyncClient, opened_cube: dict) -> None:
    create_resp = await client.post(
        "/v1/tasks/moment",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "moment_order": 0,
            "channel_start": 4,
            "channel_end": 12,
            "mask_enabled": False,
            "threshold_value": 0.0,
        },
        headers=opened_cube["headers"],
    )
    assert create_resp.status_code == 200
    task = create_resp.json()
    assert task["valid"] is True
    assert task["task_id"].startswith("task_")

    for _ in range(20):
        status_resp = await client.get(f"/v1/tasks/{task['task_id']}", headers=opened_cube["headers"])
        assert status_resp.status_code == 200
        status_data = status_resp.json()
        if status_data["status"] == "completed":
            assert status_data["result"]["valid"] is True
            assert "data_base64" in status_data["result"]
            break
        await asyncio.sleep(0.05)
    else:
        pytest.fail("moment task did not complete in time")


@pytest.mark.asyncio
async def test_pv_task_endpoint(client: AsyncClient, opened_cube: dict) -> None:
    create_resp = await client.post(
        "/v1/tasks/pv",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "vertices": [[10, 32], [54, 32]],
            "width_pixels": 3,
        },
        headers=opened_cube["headers"],
    )
    assert create_resp.status_code == 200
    task = create_resp.json()
    assert task["valid"] is True
    assert task["task_id"].startswith("task_")

    for _ in range(20):
        status_resp = await client.get(f"/v1/tasks/{task['task_id']}", headers=opened_cube["headers"])
        assert status_resp.status_code == 200
        status_data = status_resp.json()
        if status_data["status"] == "completed":
            assert status_data["result"]["valid"] is True
            assert "data_base64" in status_data["result"]
            assert status_data["result"]["valid_samples"] > 0
            break
        await asyncio.sleep(0.05)
    else:
        pytest.fail("pv task did not complete in time")


@pytest.mark.asyncio
async def test_pv_task_cache_hit(client: AsyncClient, opened_cube: dict) -> None:
    payload = {
        "dataset_id": opened_cube["dataset_id"],
        "vertices": [[10, 32], [54, 32]],
        "width_pixels": 3,
    }
    first_resp = await client.post("/v1/tasks/pv", json=payload, headers=opened_cube["headers"])
    assert first_resp.status_code == 200
    first_task = first_resp.json()

    for _ in range(20):
        status_resp = await client.get(f"/v1/tasks/{first_task['task_id']}", headers=opened_cube["headers"])
        status_data = status_resp.json()
        if status_data["status"] == "completed":
            break
        await asyncio.sleep(0.05)
    else:
        pytest.fail("first pv task did not complete in time")

    second_resp = await client.post("/v1/tasks/pv", json=payload, headers=opened_cube["headers"])
    assert second_resp.status_code == 200
    second_task = second_resp.json()

    for _ in range(20):
        status_resp = await client.get(f"/v1/tasks/{second_task['task_id']}", headers=opened_cube["headers"])
        status_data = status_resp.json()
        if status_data["status"] == "completed":
            assert status_data["cache_hit"] is True
            assert status_data["result"]["valid"] is True
            break
        await asyncio.sleep(0.05)
    else:
        pytest.fail("second pv task did not complete in time")


@pytest.mark.asyncio
async def test_cube_pv_too_few_vertices(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/pv",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "vertices": [[10, 10]],
            "width_pixels": 1,
        },
        headers=opened_cube["headers"],
    )
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_isosurface_product(client: AsyncClient, opened_cube: dict) -> None:
    pytest.importorskip("vtk")
    resp = await client.post(
        "/v1/products/isosurface",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "threshold": 1.0,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True, data["error"]
    assert data["num_points"] > 0
    assert data["num_polys"] > 0
    assert data["points_base64"] != ""
    assert data["polys_base64"] != ""


# ── Image endpoints ───────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_image_full(client: AsyncClient, opened_image: dict) -> None:
    resp = await client.post(
        "/v1/image/full",
        json={"dataset_id": opened_image["dataset_id"]},
        headers=opened_image["headers"],
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert data["width"] == 64
    assert data["height"] == 64
    assert data["is_preview"] is False
    assert data["compression"] == "qt-zlib"
    arr = _decode_qt_zlib(data["data_base64"])
    assert arr.shape == (64 * 64,)


@pytest.mark.asyncio
async def test_image_preview_downsampled(client: AsyncClient, opened_image: dict) -> None:
    resp = await client.post(
        "/v1/image/preview",
        json={"dataset_id": opened_image["dataset_id"], "max_longest_side": 32},
        headers=opened_image["headers"],
    )
    data = resp.json()
    assert data["valid"] is True
    assert data["is_preview"] is True
    assert max(data["width"], data["height"]) <= 32


@pytest.mark.asyncio
async def test_image_full_wrong_kind(client: AsyncClient, opened_cube: dict) -> None:
    """Passing a cube dataset_id to /image/full must fail gracefully."""
    resp = await client.post(
        "/v1/image/full",
        json={"dataset_id": opened_cube["dataset_id"]},
        headers=opened_cube["headers"],
    )
    data = resp.json()
    assert data["valid"] is False


# ── Unknown dataset_id ────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_unknown_dataset_id(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.post(
        "/v1/cube/slice",
        json={"dataset_id": "ds_doesnotexist", "axis": "z", "index": 0},
        headers=auth_headers,
    )
    assert resp.status_code == 404



# ── /v1/cube/noise tests ──────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_cube_noise_basic(client: AsyncClient, opened_cube: dict) -> None:
    """Noise endpoint should return per-channel MAD and sigma."""
    resp = await client.post(
        "/v1/cube/noise",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "x0": 0, "x1": 10, "y0": 0, "y1": 10,
            "channel_start": 0, "channel_end": 31,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True, data["error"]
    assert data["num_channels"] == 32
    assert len(data["mad"]) == 32
    assert len(data["sigma"]) == 32
    # All sigma should be positive finite (noise region has no NaN-only channels).
    assert all(s > 0 for s in data["sigma"] if s == s), "All sigma should be positive"


@pytest.mark.asyncio
async def test_cube_noise_sigma_relation(client: AsyncClient, opened_cube: dict) -> None:
    """sigma must equal MAD * 1.4826 within float precision."""
    resp = await client.post(
        "/v1/cube/noise",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "x0": 0, "x1": 8, "y0": 0, "y1": 8,
            "channel_start": 0, "channel_end": 10,
        },
        headers=opened_cube["headers"],
    )
    data = resp.json()
    for mad, sigma in zip(data["mad"], data["sigma"]):
        if mad == mad and sigma == sigma and mad > 0:  # skip NaN
            assert abs(sigma / mad - 1.4826) < 1e-4


@pytest.mark.asyncio
async def test_cube_noise_wrong_kind(client: AsyncClient, opened_image: dict) -> None:
    """Noise endpoint on an image dataset must fail."""
    resp = await client.post(
        "/v1/cube/noise",
        json={
            "dataset_id": opened_image["dataset_id"],
            "x0": 0, "x1": 10, "y0": 0, "y1": 10,
            "channel_start": 0, "channel_end": 5,
        },
        headers=opened_image["headers"],
    )
    # Should be 422 (wrong kind) or valid=False.
    assert resp.status_code in (422, 200)
    if resp.status_code == 200:
        assert resp.json()["valid"] is False


# ── PV physical coordinates ───────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_cube_pv_has_arcsec_positions(client: AsyncClient, opened_cube: dict) -> None:
    """PV response must include physical arcsecond positions."""
    resp = await client.post(
        "/v1/cube/pv",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "vertices": [[10, 32], [54, 32]],
            "width_pixels": 1,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert data.get("spatial_unit") == "arcsec"
    assert data.get("pixel_scale_arcsec_per_pixel", 0) > 0
    assert data.get("positions_arcsec_base64", "") != ""
    # Decode and check monotonicity.
    import zlib
    raw = base64.b64decode(data["positions_arcsec_base64"])
    n_expected = struct.unpack(">I", raw[:4])[0]
    arcsec = np.frombuffer(zlib.decompress(raw[4:])[:n_expected], dtype=np.float32)
    assert len(arcsec) == data["num_samples"]
    assert np.all(np.diff(arcsec) >= 0), "Arcsec positions should be non-decreasing"


# ── Concurrency limit ─────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_concurrent_task_limit_returns_429(
    client: AsyncClient, opened_cube: dict
) -> None:
    """Submitting more than VISIVO_MAX_CONCURRENT_TASKS requests for the same session
    must yield HTTP 429 for the overflow request."""
    headers = opened_cube["headers"]
    payload = {
        "dataset_id": opened_cube["dataset_id"],
        "moment_order": 0,
        "channel_start": 0,
        "channel_end": 0,
    }

    # Occupy all task slots by directly manipulating the session counter.
    from app.sessions import REGISTRY, _MAX_CONCURRENT_TASKS

    session_id = opened_cube["session_id"]
    session = REGISTRY.get(session_id)
    assert session is not None, "Session must exist after opened_cube fixture"

    with session._task_lock:
        session._active_tasks = _MAX_CONCURRENT_TASKS

    try:
        resp = await client.post("/v1/products/moment", json=payload, headers=headers)
        assert resp.status_code == 429, (
            f"Expected 429 when session is at capacity, got {resp.status_code}"
        )
    finally:
        # Always restore so subsequent tests are not affected.
        with session._task_lock:
            session._active_tasks = 0


# ── Cosmology ─────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_cosmology_planck18_z1(client: AsyncClient, auth_headers: dict) -> None:
    """Planck18 cosmology at z=1 should return sensible Mpc distances."""
    resp = await client.post(
        "/v1/cosmology/distance",
        json={"z": 1.0, "model": "Planck18"},
        headers=auth_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert data["model"] == "Planck18"
    assert data["z"] == pytest.approx(1.0)
    # Planck18: D_C(z=1) ≈ 3303 Mpc, D_L(z=1) ≈ 6607 Mpc
    assert 3000 < data["comoving_Mpc"] < 3700, "Comoving distance at z=1 out of expected range"
    assert 6000 < data["luminosity_Mpc"] < 7200, "Luminosity distance at z=1 out of expected range"
    assert data["proper_motion_kpc_per_arcsec"] > 0


@pytest.mark.asyncio
async def test_cosmology_z0_returns_zero_distances(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.post(
        "/v1/cosmology/distance",
        json={"z": 0.0},
        headers=auth_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert data["comoving_Mpc"] == pytest.approx(0.0, abs=1e-3)
    assert data["luminosity_Mpc"] == pytest.approx(0.0, abs=1e-3)


@pytest.mark.asyncio
async def test_cosmology_negative_z_returns_error(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.post(
        "/v1/cosmology/distance",
        json={"z": -0.5},
        headers=auth_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is False
    assert "z" in data["error"].lower() or "redshift" in data["error"].lower()


@pytest.mark.asyncio
async def test_cosmology_unknown_model_returns_error(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.post(
        "/v1/cosmology/distance",
        json={"z": 1.0, "model": "NonExistentModel99"},
        headers=auth_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is False
    assert "NonExistentModel99" in data["error"]


@pytest.mark.asyncio
async def test_cosmology_luminosity_gt_comoving(client: AsyncClient, auth_headers: dict) -> None:
    """D_L = D_C * (1+z) > D_C for z > 0."""
    resp = await client.post(
        "/v1/cosmology/distance",
        json={"z": 0.5},
        headers=auth_headers,
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"] is True
    assert data["luminosity_Mpc"] > data["comoving_Mpc"]


# Next-server test: task registry TTL eviction

def test_task_registry_ttl_eviction() -> None:
    from app.tasks import TaskRegistry

    registry = TaskRegistry(ttl_seconds=1)
    task = registry.create("pv")
    record = registry.get(task.task_id)
    assert record is not None
    record.last_touched_monotonic -= 5.0
    assert registry.get(task.task_id) is None


# ── Task cancellation (DELETE /v1/tasks/{id}) ─────────────────────────────────


@pytest.mark.asyncio
async def test_cancel_running_task(client: AsyncClient, opened_cube: dict) -> None:
    """A running task must be cancellable via DELETE and reflect status='cancelled'."""
    create_resp = await client.post(
        "/v1/tasks/moment",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "moment_order": 0,
            "channel_start": 0,
            "channel_end": 31,
            "mask_enabled": False,
            "threshold_value": 0.0,
        },
        headers=opened_cube["headers"],
    )
    assert create_resp.status_code == 200
    task_id = create_resp.json()["task_id"]

    del_resp = await client.delete(f"/v1/tasks/{task_id}", headers=opened_cube["headers"])
    assert del_resp.status_code == 200
    assert del_resp.json()["cancelled"] is True
    assert del_resp.json()["task_id"] == task_id

    # After cancellation the status must be 'cancelled' (or 'completed' if it
    # already finished before the cancel propagated — both are acceptable).
    status_resp = await client.get(f"/v1/tasks/{task_id}", headers=opened_cube["headers"])
    assert status_resp.status_code == 200
    final_status = status_resp.json()["status"]
    assert final_status in ("cancelled", "completed"), f"Unexpected status: {final_status}"


@pytest.mark.asyncio
async def test_cancel_nonexistent_task_returns_404(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.delete("/v1/tasks/task_doesnotexist_abc", headers=auth_headers)
    assert resp.status_code == 404


@pytest.mark.asyncio
async def test_cancel_completed_task_returns_409(client: AsyncClient, opened_cube: dict) -> None:
    """Cancelling a task that has already completed must return 409 Conflict."""
    create_resp = await client.post(
        "/v1/tasks/moment",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "moment_order": 0,
            "channel_start": 0,
            "channel_end": 4,
        },
        headers=opened_cube["headers"],
    )
    assert create_resp.status_code == 200
    task_id = create_resp.json()["task_id"]

    # Wait for completion. The first moment task in a test run pays the
    # ProcessPoolExecutor cold-start cost (fork + import numpy/astropy in the
    # subprocess), which can exceed a couple of seconds — poll generously so the
    # task reliably reaches its terminal state before we assert on the 409.
    status = ""
    for _ in range(150):
        s = await client.get(f"/v1/tasks/{task_id}", headers=opened_cube["headers"])
        status = s.json()["status"]
        if status in ("completed", "failed", "cancelled"):
            break
        await asyncio.sleep(0.1)
    assert status == "completed", f"Task did not complete in time (status={status})"

    del_resp = await client.delete(f"/v1/tasks/{task_id}", headers=opened_cube["headers"])
    assert del_resp.status_code == 409


# ── Isosurface via PRODUCT_CACHE ──────────────────────────────────────────────


@pytest.mark.asyncio
async def test_isosurface_cache_hit(client: AsyncClient, opened_cube: dict) -> None:
    """Two identical isosurface requests must produce a cache hit on the second call."""
    if not _vtk_available():
        pytest.skip("VTK not available on this server")

    payload = {"dataset_id": opened_cube["dataset_id"], "threshold": 1.0}
    resp1 = await client.post("/v1/products/isosurface", json=payload, headers=opened_cube["headers"])
    assert resp1.status_code == 200
    assert resp1.json()["valid"] is True

    resp2 = await client.post("/v1/products/isosurface", json=payload, headers=opened_cube["headers"])
    assert resp2.status_code == 200
    assert resp2.json()["valid"] is True
    # Both responses must carry identical geometry data.
    assert resp1.json()["num_points"] == resp2.json()["num_points"]
    assert resp1.json()["num_polys"] == resp2.json()["num_polys"]


def _vtk_available() -> bool:
    try:
        import vtk  # noqa: F401
        return True
    except ImportError:
        return False


# ── Image tiling (out-of-core 2-D pyramid) ────────────────────────────────────


@pytest.mark.asyncio
async def test_image_tile_endpoint(client: AsyncClient, opened_image: dict) -> None:
    resp = await client.post(
        "/v1/image/tile",
        json={"dataset_id": opened_image["dataset_id"],
              "level": 0, "tile_x": 0, "tile_y": 0, "tile_size": 32},
        headers=opened_image["headers"],
    )
    assert resp.status_code == 200
    data = resp.json()
    assert data["valid"], data.get("error")
    assert (data["full_width"], data["full_height"]) == (64, 64)
    assert (data["width"], data["height"]) == (32, 32)
    assert data["level"] == 0
    assert data["num_levels"] >= 2      # 64px / 32px tile → 2 levels
    assert data["data_base64"]


@pytest.mark.asyncio
async def test_image_tile_out_of_range_returns_422(
    client: AsyncClient, opened_image: dict
) -> None:
    resp = await client.post(
        "/v1/image/tile",
        json={"dataset_id": opened_image["dataset_id"],
              "level": 0, "tile_x": 99, "tile_y": 0, "tile_size": 32},
        headers=opened_image["headers"],
    )
    assert resp.status_code == 422


@pytest.mark.asyncio
async def test_image_tile_on_cube_returns_422(
    client: AsyncClient, opened_cube: dict
) -> None:
    resp = await client.post(
        "/v1/image/tile",
        json={"dataset_id": opened_cube["dataset_id"], "level": 0,
              "tile_x": 0, "tile_y": 0, "tile_size": 32},
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 422


@pytest.mark.asyncio
async def test_cube_mask_apply_bin_long_recipe_survives_whole(client: AsyncClient,
                                                              opened_cube: dict) -> None:
    """A selection with many extra seeds carries them as coordinates: the route
    must accept the long recipe and the HISTORY must keep every seed, not the
    first 72 characters of the first 24 items."""
    import numpy as np
    from astropy.io import fits as _f
    rw, rh, rd = 5, 4, 3
    m = np.zeros((rd, rh, rw), dtype="<f4")
    m[1, 1:3, 1:3] = 1.0
    seeds = "|".join(f"({100 + i},{200 + i},{300 + i})" for i in range(80))
    recipe = ("seed_zyx=9,25,21; reach=8; lambda_v=1.00; nsigma=2.5; "
              "sigma=0.0019689999999999999 (noise estimate); smooth_zyx=0,0,0; reach_max=64; "
              f"add_seeds={seeds}; sub_seeds=none; hand_edited=no")
    assert len(recipe) > 1000
    resp = await client.post(
        "/v1/cube/mask_apply_bin",
        params={"dataset_id": opened_cube["dataset_id"], "origin_x": 20, "origin_y": 24,
                "origin_z": 8, "width": rw, "height": rh, "depth": rd,
                "mode": "blank_outside", "recipe": recipe},
        content=m.tobytes(),
        headers={**opened_cube["headers"], "Content-Type": "application/octet-stream"},
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["valid"] is True, data
    history = "\n".join(str(c) for c in _f.getheader(data["output_path"])["HISTORY"])
    joined = history.replace("\n", "")
    for i in (0, 40, 79):
        assert f"({100 + i},{200 + i},{300 + i})" in joined
    assert "Lasso hand_edited=no" in history and "Lasso reach_max=64" in history
    assert "sigma=0.0019689999999999999" in joined


def test_task_registry_never_evicts_live_work() -> None:
    """A record whose work is still going outlives the retention clock; one
    whose cancelled computation is still running too."""
    import asyncio

    from app.tasks import TaskRegistry

    loop = asyncio.new_event_loop()
    try:
        registry = TaskRegistry(ttl_seconds=1)
        running = registry.create("moment", owner_token="t")
        registry.update(running.task_id, status="running", asyncio_handle=loop.create_future())
        abandoned = registry.create("soda-moment", owner_token="t")
        work = loop.create_future()
        registry.update(abandoned.task_id, status="cancelled", work_handle=work)
        finished = registry.create("pv", owner_token="t")
        registry.update(finished.task_id, status="completed")
        for record in (running, abandoned, finished):
            record.last_touched_monotonic -= 10
        snapshot = registry.snapshot()
        assert snapshot["running_count"] == 2          # the running one and the abandoned worker
        assert registry.peek(running.task_id) is not None
        assert registry.peek(abandoned.task_id) is not None
        assert registry.peek(finished.task_id) is None  # ended: evicted on time
        work.set_result(None)
        abandoned.last_touched_monotonic -= 10
        registry.stats()
        assert registry.peek(abandoned.task_id) is None  # its work over, it goes too
    finally:
        loop.close()


@pytest_asyncio.fixture()
async def heavy_sandbox(monkeypatch):
    """A registry and a heavy semaphore of this test's own, so that work it
    holds back never leaks into other tests; the held pool tasks are always
    released and awaited at the end, whatever the test did."""
    from app import dependencies
    from app.tasks import TaskRegistry

    registry = TaskRegistry()
    monkeypatch.setattr(dependencies, "TASKS", registry)
    monkeypatch.setattr(dependencies, "_HEAVY_SEM", asyncio.Semaphore(2))
    monkeypatch.setattr(dependencies, "_HEAVY_TIMEOUT_S", 0.01)
    gate, started, done = asyncio.Event(), asyncio.Event(), asyncio.Event()
    held_tasks: list[asyncio.Task] = []

    async def held(fn, *args):
        held_tasks.append(asyncio.current_task())
        started.set()
        try:
            await gate.wait()
            return "late"
        finally:
            done.set()

    monkeypatch.setattr(dependencies, "_run", held)
    try:
        yield registry, gate, started, done
    finally:
        gate.set()
        await asyncio.gather(*held_tasks, return_exceptions=True)


@pytest.mark.asyncio
async def test_heavy_timeout_keeps_the_work_counted(heavy_sandbox) -> None:
    """A heavy call that times out answers 504, but the pool task behind it
    runs on: the node counts it as running until it ends."""
    from fastapi import HTTPException

    from app import dependencies

    registry, gate, _started, done = heavy_sandbox
    with pytest.raises(HTTPException) as failure:
        await dependencies._run_heavy(lambda: None)
    assert failure.value.status_code == 504
    assert registry.snapshot()["running_count"] == 1
    gate.set()
    await asyncio.wait_for(done.wait(), 1)
    await asyncio.sleep(0)
    assert registry.snapshot()["running_count"] == 0


@pytest.mark.asyncio
async def test_cancelled_heavy_call_keeps_the_work_counted(heavy_sandbox, monkeypatch) -> None:
    """Cancelling the caller (a cancelled task) does not stop the pool task:
    it is counted until it ends."""
    from app import dependencies

    registry, gate, started, done = heavy_sandbox
    monkeypatch.setattr(dependencies, "_HEAVY_TIMEOUT_S", 10.0)
    caller = asyncio.create_task(dependencies._run_heavy(lambda: None))
    await asyncio.wait_for(started.wait(), 1)
    caller.cancel()
    with pytest.raises(asyncio.CancelledError):
        await caller
    assert registry.snapshot()["running_count"] == 1
    gate.set()
    await asyncio.wait_for(done.wait(), 1)
    await asyncio.sleep(0)
    assert registry.snapshot()["running_count"] == 0


def test_forgetting_a_record_counts_its_work_once() -> None:
    """A deleted job's runner and the work it shields are one activity."""
    from app.tasks import TaskRegistry

    loop = asyncio.new_event_loop()
    try:
        registry = TaskRegistry()
        record = registry.create("soda-moment", owner_token="t")
        runner, work = loop.create_future(), loop.create_future()
        registry.update(record.task_id, status="cancelled", asyncio_handle=runner, work_handle=work)
        registry.forget(record.task_id)
        assert registry.snapshot()["running_count"] == 1
        runner.set_result(None)
        assert registry.snapshot()["running_count"] == 1
        work.set_result(None)
        assert registry.snapshot()["running_count"] == 0
    finally:
        loop.close()
