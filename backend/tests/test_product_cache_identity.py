"""The product cache is keyed by the file's version, not only its path.

A product computed for a file must not be served once the file has been
replaced or rewritten: before, the key was path + operation + parameters, so a
moment of a cube re-saved with values 7x larger came back from the cache with
the old values and cache_hit=True (and the moment FITS export wrote them).
"""
from __future__ import annotations

import asyncio
import base64
import os
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits


def _write_cube(path: Path, scale: float) -> None:
    data = np.ones((4, 3, 3), dtype=np.float32) * np.float32(scale)
    hdu = fits.PrimaryHDU(data)
    hdu.header["CTYPE3"] = "VRAD"
    hdu.header["CRVAL3"] = 0.0
    hdu.header["CRPIX3"] = 1.0
    hdu.header["CDELT3"] = 1000.0
    hdu.header["CUNIT3"] = "m/s"
    hdu.header["BUNIT"] = "Jy/beam"
    hdu.writeto(str(path), overwrite=True)


def _moment0(path: Path) -> tuple[np.ndarray, bool]:
    from app.dependencies import _moment_product_payload
    from app.schemas import MomentProductRequest

    request = MomentProductRequest(dataset_id="d", moment_order=0, channel_start=0,
                                   channel_end=3, mask_enabled=False, threshold_value=0.0)
    payload, hit = asyncio.run(_moment_product_payload({"path": str(path)}, request))
    return np.frombuffer(base64.b64decode(payload["data_base64"]), dtype=np.float32), hit


def test_moment_of_replaced_file_is_recomputed(tmp_path):
    cube = tmp_path / "cube.fits"
    _write_cube(cube, 1.0)
    first, hit = _moment0(cube)
    assert not hit
    again, hit = _moment0(cube)
    assert hit
    np.testing.assert_array_equal(again, first)

    # Replaced by a new file (new inode), as a re-reduction saved over it is.
    fresh = tmp_path / "fresh.fits"
    _write_cube(fresh, 7.0)
    os.replace(fresh, cube)
    replaced, hit = _moment0(cube)
    assert not hit
    np.testing.assert_allclose(replaced, 7.0 * first)

    # Rewritten in place, same size.
    _write_cube(cube, 2.0)
    rewritten, hit = _moment0(cube)
    assert not hit
    np.testing.assert_allclose(rewritten, 2.0 * first)


def test_key_follows_an_in_place_rewrite_that_restores_mtime(tmp_path):
    from app.product_cache import ProductCache

    path = tmp_path / "data.fits"
    _write_cube(path, 1.0)
    before = os.stat(path)
    key, param_hash = ProductCache.make_key(str(path), "moment", {"order": 0})
    assert ProductCache.make_key(str(path), "moment", {"order": 0}) == (key, param_hash)
    # One data byte changed through the same inode, mtime then put back.
    with open(path, "r+b") as fh:
        fh.seek(-1, os.SEEK_END)
        fh.write(b"\x01")
    os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
    assert os.stat(path).st_ino == before.st_ino
    assert os.stat(path).st_size == before.st_size
    new_key, new_hash = ProductCache.make_key(str(path), "moment", {"order": 0})
    assert new_hash == param_hash        # the parameters did not change
    assert new_key != key                # the file did (ctime)


def test_vbt_header_is_part_of_the_version(tmp_path):
    from app.product_cache import file_identity
    from app.volume_source import vbt_field_ref

    data = tmp_path / "vol.bin"
    data.write_bytes(b"\0" * 64)
    head = tmp_path / "vol.bin.head"
    head.write_text("float\n")
    ref = vbt_field_ref(data, 0)
    first = file_identity(ref)
    assert first and first == file_identity(data)
    head.write_text("double\n")
    assert file_identity(ref) != first


def test_prepared_particles_follow_a_rewritten_vbt_binary(tmp_path, monkeypatch):
    """/v1/vbt/open registers ``head_path`` and ``bin_path`` (no ``path``): a
    binary rewritten under the same dataset id is prepared again."""
    from collections import OrderedDict

    from app.product_cache import dataset_identity
    from app.routers import particle_render

    data = tmp_path / "points.bin"
    head = tmp_path / "points.bin.head"
    data.write_bytes(b"old data")
    head.write_text("float\n")
    entry = {"kind": "vbt", "head_path": str(head), "bin_path": str(data)}

    class Req:
        def model_dump(self, **_kwargs):
            return {"dataset_id": "vbt_demo"}

    monkeypatch.setattr(particle_render, "_PREPARED", OrderedDict())
    monkeypatch.setattr(particle_render, "_prepare", lambda e, r: {"data": data.read_bytes()})
    first = dataset_identity(entry)
    assert particle_render._prepared(entry, Req(), "t")["data"] == b"old data"
    data.write_bytes(b"new data!")
    assert dataset_identity(entry) != first
    assert particle_render._prepared(entry, Req(), "t")["data"] == b"new data!"


def test_missing_path_keys_without_identity(tmp_path):
    from app.product_cache import ProductCache, file_identity

    missing = str(tmp_path / "nope.fits")
    assert file_identity(missing) == ""
    assert ProductCache.make_key(missing, "pv", {})[0] == ProductCache.make_key(missing, "pv", {})[0]


def test_overlay_csv_reread_after_rewrite(tmp_path):
    from app import dependencies

    csv = tmp_path / "cat.csv"
    csv.write_text("ra,dec\n10.0,20.0\n")
    dependencies._load_overlay_csv.cache_clear()
    first = dependencies._load_overlay_csv(str(csv), dependencies.file_identity(str(csv)))
    assert len(first) == 1
    csv.write_text("ra,dec\n10.0,20.0\n10.1,20.1\n")
    again = dependencies._load_overlay_csv(str(csv), dependencies.file_identity(str(csv)))
    assert len(again) == 2


@pytest.mark.asyncio
async def test_cube_stats_follow_the_file(client, auth_headers, tmp_path):
    cube = tmp_path / "stats.fits"
    _write_cube(cube, 1.0)
    resp = await client.post("/v1/datasets/open", json={"path": str(cube)}, headers=auth_headers)
    opened = resp.json()
    assert opened["valid"], opened["error"]
    headers = {**auth_headers, "X-Visivo-Session": opened["session_id"]}
    url = f"/v1/analysis/cube-stats/{opened['dataset_id']}"
    first = (await client.get(url, headers=headers)).json()
    assert first["max"] == pytest.approx(1.0)
    assert (await client.get(url, headers=headers)).json()["cached"] is True
    _write_cube(cube, 5.0)
    second = (await client.get(url, headers=headers)).json()
    assert second["cached"] is False
    assert second["max"] == pytest.approx(5.0)
