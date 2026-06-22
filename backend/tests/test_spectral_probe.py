"""Spectral probe / aperture endpoints — node-side spectrum extraction so the
desktop profiler scales to large/remote cubes."""
import pytest


@pytest.mark.asyncio
async def test_spectral_probe_pixel(client, opened_cube):
    resp = await client.post(
        "/v1/spectral/probe",
        headers=opened_cube["headers"],
        json={"dataset_id": opened_cube["dataset_id"], "x": 32, "y": 32},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"]
    assert len(body["values"]) == len(body["spectral_axis"]) == 32
    assert body["spectral_unit"]            # FREQ → Hz (or m), not empty
    assert body["spectral_type"]


@pytest.mark.asyncio
async def test_spectral_probe_out_of_range(client, opened_cube):
    resp = await client.post(
        "/v1/spectral/probe",
        headers=opened_cube["headers"],
        json={"dataset_id": opened_cube["dataset_id"], "x": 9999, "y": 0},
    )
    assert resp.status_code == 400


@pytest.mark.asyncio
async def test_spectral_aperture(client, opened_cube):
    resp = await client.post(
        "/v1/spectral/aperture",
        headers=opened_cube["headers"],
        json={"dataset_id": opened_cube["dataset_id"], "x": 32, "y": 32, "radius": 3},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"]
    assert body["n_pix"] > 1
    assert len(body["values"]) == 32


@pytest.mark.asyncio
async def test_spectral_probe_requires_cube(client, opened_image):
    resp = await client.post(
        "/v1/spectral/probe",
        headers=opened_image["headers"],
        json={"dataset_id": opened_image["dataset_id"], "x": 1, "y": 1},
    )
    assert resp.status_code == 422
