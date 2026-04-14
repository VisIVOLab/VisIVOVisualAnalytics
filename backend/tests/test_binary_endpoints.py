"""
test_binary_endpoints.py – Tests for the binary transport endpoints.

Covers:
  POST /v1/moment/result/binary
  POST /v1/pv/result/binary

Wire format (12-byte header, big-endian uint32 × 3, followed by float32 LE body):
  [0:4]  rows (height)
  [4:8]  cols (width)
  [8:12] flags (0 = plain float32)
  [12:]  float32 row-major pixel values, NaN for masked pixels

Run with:
    cd backend
    pytest tests/test_binary_endpoints.py -v
"""

from __future__ import annotations

import struct

import numpy as np
import pytest
from httpx import AsyncClient


# ── Helpers ───────────────────────────────────────────────────────────────────


def _parse_header(raw: bytes) -> tuple[int, int, int]:
    """Unpack the 12-byte binary header → (rows, cols, flags)."""
    assert len(raw) >= 12, f"Response too short: {len(raw)} bytes"
    return struct.unpack(">III", raw[:12])


def _body_floats(raw: bytes, rows: int, cols: int) -> np.ndarray:
    """Extract the float32 body and reshape to (rows, cols)."""
    body = raw[12:]
    assert len(body) == rows * cols * 4, (
        f"Body length mismatch: expected {rows * cols * 4}, got {len(body)}"
    )
    return np.frombuffer(body, dtype="<f4").reshape(rows, cols)


# ── Moment binary endpoint ────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_moment_binary_header(
    client: AsyncClient,
    opened_cube: dict,
) -> None:
    """Correct rows, cols, flags in the first 12 bytes of the moment binary response."""
    resp = await client.post(
        "/v1/moment/result/binary",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "moment_order": 0,
            "channel_start": 8,
            "channel_end": 23,
            "mask_enabled": False,
            "threshold_value": 0.0,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200, resp.text
    assert resp.headers["content-type"] == "application/octet-stream"
    rows, cols, flags = _parse_header(resp.content)
    # The synthetic cube is 32 channels × 64 × 64 (nz=32, ny=64, nx=64).
    # The moment map shape must be (ny, nx) = (64, 64).
    assert rows == 64
    assert cols == 64
    assert flags == 0, f"Expected flags=0 (plain float32), got {flags}"


@pytest.mark.asyncio
async def test_moment_binary_length(
    client: AsyncClient,
    opened_cube: dict,
) -> None:
    """Total response length == rows * cols * 4 + 12."""
    resp = await client.post(
        "/v1/moment/result/binary",
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
    assert resp.status_code == 200
    rows, cols, _flags = _parse_header(resp.content)
    expected_len = rows * cols * 4 + 12
    assert len(resp.content) == expected_len, (
        f"Expected {expected_len} bytes, got {len(resp.content)}"
    )
    # Sanity: no NaN-only map for a cube with real emission.
    arr = _body_floats(resp.content, rows, cols)
    assert np.isfinite(arr).any(), "Moment map should contain at least some finite values"


@pytest.mark.asyncio
async def test_moment_binary_cache_hit(
    client: AsyncClient,
    opened_cube: dict,
) -> None:
    """Second identical request hits the product cache (X-Visivo-From-Cache: true)."""
    payload = {
        "dataset_id": opened_cube["dataset_id"],
        "moment_order": 6,
        "channel_start": 5,
        "channel_end": 20,
        "mask_enabled": False,
        "threshold_value": 0.0,
    }
    # First request — cold cache.
    r1 = await client.post(
        "/v1/moment/result/binary", json=payload, headers=opened_cube["headers"]
    )
    assert r1.status_code == 200
    assert r1.headers.get("x-visivo-from-cache", "").lower() == "false"

    # Second request — should hit cache.
    r2 = await client.post(
        "/v1/moment/result/binary", json=payload, headers=opened_cube["headers"]
    )
    assert r2.status_code == 200
    assert r2.headers.get("x-visivo-from-cache", "").lower() == "true"

    # Pixel values must be identical between the two responses.
    assert r1.content == r2.content


# ── PV binary endpoint ────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_pv_binary_shape(
    client: AsyncClient,
    opened_cube: dict,
) -> None:
    """rows == n_spectral_channels, cols == n_path_pixels for the PV diagram."""
    # A simple 2-vertex diagonal cut across the 64×64 cube.
    resp = await client.post(
        "/v1/pv/result/binary",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "vertices": [[10, 10], [53, 53]],
            "width_pixels": 1,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200, resp.text
    assert resp.headers["content-type"] == "application/octet-stream"

    rows, cols, flags = _parse_header(resp.content)

    # rows must equal the cube spectral depth (32 channels in the conftest fixture).
    assert rows == 32, f"Expected rows==32 (spectral depth), got {rows}"
    # cols must be positive (the number of sampled path pixels).
    assert cols > 0, "PV diagram must have at least one path pixel"
    assert flags == 0

    expected_len = rows * cols * 4 + 12
    assert len(resp.content) == expected_len

    arr = _body_floats(resp.content, rows, cols)
    assert np.isfinite(arr).any(), "PV diagram should contain some finite values"
