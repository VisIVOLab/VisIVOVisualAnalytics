"""``/v1/datasets/open_skava`` — open a SKAVA dataset in place on the node.

The desktop routes a SKAVA dataset to the backend on the node that holds it
and hands over that node's ``file://`` access URL. These tests pin what the
backend does with it: open it in place, refuse other schemes and other hosts,
and keep to ``VISIVO_DATA_ROOT`` when an operator sets one.
"""
from __future__ import annotations

import os

import pytest

from app.skava_client import SkavaResolveError, access_url_to_local_path


# ── Resolver ────────────────────────────────────────────────────────────────


def test_file_url_resolves_to_path(synthetic_fits_cube):
    assert access_url_to_local_path(f"file://{synthetic_fits_cube}") == str(synthetic_fits_cube.resolve())


def test_localhost_file_url_is_local(synthetic_fits_cube):
    assert access_url_to_local_path(f"file://localhost{synthetic_fits_cube}") == str(synthetic_fits_cube.resolve())


@pytest.mark.parametrize("url", ["", "http://node/x.fits", "https://node/x.fits", "file://other-host/x.fits"])
def test_non_local_urls_are_refused(url):
    with pytest.raises(SkavaResolveError):
        access_url_to_local_path(url)


def test_data_root_jail(monkeypatch, synthetic_fits_cube, tmp_path):
    # Inside the jail: accepted. Outside, including via "..": refused.
    monkeypatch.setenv("VISIVO_DATA_ROOT", str(synthetic_fits_cube.parent))
    assert access_url_to_local_path(f"file://{synthetic_fits_cube}")
    monkeypatch.setenv("VISIVO_DATA_ROOT", str(tmp_path / "jail"))
    (tmp_path / "jail").mkdir()
    with pytest.raises(SkavaResolveError):
        access_url_to_local_path(f"file://{synthetic_fits_cube}")
    with pytest.raises(SkavaResolveError):
        access_url_to_local_path(f"file://{tmp_path / 'jail'}/../{synthetic_fits_cube.name}")


def test_data_roots_also_jail(monkeypatch, synthetic_fits_cube, tmp_path):
    # VISIVO_DATA_ROOTS, the name the rest of the backend uses, confines SKAVA
    # access URLs too, so a node declares its data once.
    monkeypatch.delenv("VISIVO_DATA_ROOT", raising=False)
    (tmp_path / "jail").mkdir()
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path / "jail"))
    with pytest.raises(SkavaResolveError):
        access_url_to_local_path(f"file://{synthetic_fits_cube}")
    monkeypatch.setenv("VISIVO_DATA_ROOTS",
                       os.pathsep.join([str(tmp_path / "jail"), str(synthetic_fits_cube.parent)]))
    assert access_url_to_local_path(f"file://{synthetic_fits_cube}")


# ── Route ───────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_open_skava_opens_in_place(client, auth_headers, synthetic_fits_cube):
    resp = await client.post(
        "/v1/datasets/open_skava",
        headers=auth_headers,
        json={"obs_id": "demo-cube", "access_url": f"file://{synthetic_fits_cube}"},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"], body
    assert body["kind"] == "cube"
    assert body["dataset_id"]


@pytest.mark.asyncio
async def test_open_skava_refuses_http_with_a_reason(client, auth_headers):
    resp = await client.post(
        "/v1/datasets/open_skava",
        headers=auth_headers,
        json={"obs_id": "x", "access_url": "http://node:8001/v1/exports/download?filename=a.fits"},
    )
    body = resp.json()
    assert body["valid"] is False
    assert "open_url" in body["error"]          # tells the caller where http belongs


@pytest.mark.asyncio
async def test_open_skava_requires_obs_id(client, auth_headers, synthetic_fits_cube):
    resp = await client.post(
        "/v1/datasets/open_skava",
        headers=auth_headers,
        json={"obs_id": " ", "access_url": f"file://{synthetic_fits_cube}"},
    )
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_open_skava_requires_auth(client, synthetic_fits_cube):
    resp = await client.post(
        "/v1/datasets/open_skava",
        json={"obs_id": "x", "access_url": f"file://{synthetic_fits_cube}"},
    )
    assert resp.status_code in (401, 403)
