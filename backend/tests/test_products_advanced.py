"""Advanced 2-D products: map math, mosaic, publication figure, image quality.

Back SKAVA visualization use cases UC 3.2 / 3.3 / 1.3 / 4.2.
"""
import pytest


async def _open(client, headers, path):
    resp = await client.post("/v1/datasets/open", json={"path": str(path)}, headers=headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"], body["error"]
    return body["dataset_id"]


# ── UC 3.2: map math / spectral index ───────────────────────────────────────


@pytest.mark.asyncio
async def test_map_math_ratio(client, opened_image, synthetic_fits_image):
    h = opened_image["headers"]
    b = await _open(client, h, synthetic_fits_image)
    resp = await client.post(
        "/v1/products/map_math", headers=h,
        json={"dataset_id": opened_image["dataset_id"], "op": "ratio", "dataset_id_b": b},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["output_path"].endswith(".fits")
    assert body["info"]["op"] == "ratio"


@pytest.mark.asyncio
async def test_map_math_spectral_index(client, opened_image, synthetic_fits_image):
    h = opened_image["headers"]
    b = await _open(client, h, synthetic_fits_image)
    resp = await client.post(
        "/v1/products/map_math", headers=h,
        json={
            "dataset_id": opened_image["dataset_id"], "op": "spectral_index",
            "dataset_id_b": b, "freq_a": 1.4e9, "freq_b": 3.0e9,
        },
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["valid"], resp.json()["error"]


@pytest.mark.asyncio
async def test_map_math_spectral_index_requires_freqs(client, opened_image, synthetic_fits_image):
    h = opened_image["headers"]
    b = await _open(client, h, synthetic_fits_image)
    resp = await client.post(
        "/v1/products/map_math", headers=h,
        json={"dataset_id": opened_image["dataset_id"], "op": "spectral_index", "dataset_id_b": b},
    )
    # Missing frequencies → graceful failure, not a crash.
    assert resp.status_code == 200
    assert resp.json()["valid"] is False


# ── UC 3.3: mosaic ──────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_mosaic_two_tiles(client, opened_image, synthetic_fits_image):
    h = opened_image["headers"]
    b = await _open(client, h, synthetic_fits_image)
    resp = await client.post(
        "/v1/products/mosaic", headers=h,
        json={"dataset_ids": [opened_image["dataset_id"], b], "weight_mode": "noise"},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["info"]["n_inputs"] == 2
    assert 0.0 < body["info"]["coverage_fraction"] <= 1.0


@pytest.mark.asyncio
async def test_mosaic_requires_two(client, opened_image):
    h = opened_image["headers"]
    resp = await client.post(
        "/v1/products/mosaic", headers=h,
        json={"dataset_ids": [opened_image["dataset_id"]]},
    )
    assert resp.status_code == 422


# ── UC 1.3: publication figure ──────────────────────────────────────────────


@pytest.mark.asyncio
async def test_publication_figure(client, opened_image):
    h = opened_image["headers"]
    resp = await client.post(
        "/v1/products/publication_figure", headers=h,
        json={"dataset_id": opened_image["dataset_id"], "stretch": "log", "title": "M-test"},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"], body["error"]
    assert body["output_path"].endswith(".png")
    assert body["has_wcs"] is True


# ── UC 4.2: image quality ───────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_image_quality_metrics(client, opened_image):
    h = opened_image["headers"]
    resp = await client.post(
        "/v1/products/image_quality", headers=h,
        json={"dataset_id": opened_image["dataset_id"]},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"]
    m = body["metrics"]
    # The synthetic image carries injected NaNs → non-zero blanked fraction.
    assert m["blanked_fraction"] > 0.0
    for key in ("dynamic_range", "robust_std", "striping_metric", "negative_bowl", "flags"):
        assert key in m
