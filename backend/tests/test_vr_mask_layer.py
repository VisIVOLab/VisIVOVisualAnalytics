"""A label mask over a cube in the headset (issue #282): the mask's sources as
sorted voxel indices on the cube's grid, refused when the grid differs, and
the refined mask written back with the same grid and IDs."""
from __future__ import annotations

import struct

import numpy as np
import pytest
from astropy.io import fits

from app import mask_layer as ml


def _header(nz=6, ny=8, nx=10, cdelt3=5000.0):
    h = fits.Header()
    for k, v in dict(CTYPE1="RA---SIN", CRPIX1=5, CRVAL1=150.0, CDELT1=-0.002, CUNIT1="deg",
                     CTYPE2="DEC--SIN", CRPIX2=4, CRVAL2=2.0, CDELT2=0.002, CUNIT2="deg",
                     CTYPE3="VRAD", CRPIX3=1.0, CRVAL3=1.0e6, CDELT3=cdelt3, CUNIT3="m/s").items():
        h[k] = v
    return h


def _files(tmp_path, four_d_cube=True, cdelt3=5000.0):
    nz, ny, nx = 6, 8, 10
    labels = np.zeros((nz, ny, nx), np.int32)
    labels[1:3, 2:4, 3:6] = 5          # 2 x 2 x 3 = 12 voxels
    labels[4, 6, 8] = 2
    cube = np.random.default_rng(1).normal(size=(nz, ny, nx)).astype(np.float32)
    ch = _header()
    cpath, mpath = tmp_path / "cube.fits", tmp_path / "cube_mask.fits"
    if four_d_cube:          # a degenerate Stokes axis, as HCG 44's cube has
        ch["CTYPE4"], ch["CRPIX4"], ch["CRVAL4"], ch["CDELT4"] = "STOKES", 1.0, 1.0, 1.0
        fits.PrimaryHDU(cube[None], header=ch).writeto(cpath)
    else:
        fits.PrimaryHDU(cube, header=ch).writeto(cpath)
    fits.PrimaryHDU(labels, header=_header(cdelt3=cdelt3)).writeto(mpath)
    return str(cpath), str(mpath), labels


def test_the_layer_lists_each_source_sorted_on_the_full_grid(tmp_path):
    c, m, labels = _files(tmp_path)
    layer = ml.read_layer(c, m)
    assert layer["shape"] == [10, 8, 6]
    assert [s["id"] for s in layer["sources"]] == [2, 5]
    assert layer["n_voxels"] == 13 and layer["max_id"] == 5
    s5 = layer["sources"][1]
    idx = layer["indices"][s5["offset"]:s5["offset"] + s5["count"]]
    assert np.all(np.diff(idx.astype(np.int64)) > 0)
    truth = np.flatnonzero(labels.reshape(-1) == 5)
    assert np.array_equal(idx, truth)
    assert s5["bbox"] == [3, 5, 2, 3, 1, 2]
    assert s5["centroid"] == pytest.approx([4.0, 2.5, 1.5])
    # The wire: JSON length, JSON padded to 4, then the indices.
    body = ml.pack_layer(layer, {"mask_name": "cube_mask.fits"})
    (n,) = struct.unpack_from("<I", body, 0)
    assert (4 + n) % 4 == 0
    assert np.array_equal(np.frombuffer(body, "<u4", offset=4 + n), layer["indices"])


def test_a_mask_on_another_grid_is_refused(tmp_path):
    c, m, _ = _files(tmp_path, cdelt3=8000.0)
    assert "CDELT3" in ml.grid_problem(c, m)
    with pytest.raises(ValueError, match="not on the cube's grid"):
        ml.read_layer(c, m)
    other = tmp_path / "small.fits"
    fits.PrimaryHDU(np.zeros((6, 8, 9), np.int32), header=_header()).writeto(other)
    assert "pixels" in ml.grid_problem(c, str(other))


def test_a_fractional_mask_is_refused(tmp_path):
    c, _, _ = _files(tmp_path)
    frac = tmp_path / "frac.fits"
    fits.PrimaryHDU(np.full((6, 8, 10), 0.5, np.float32), header=_header()).writeto(frac)
    with pytest.raises(ValueError, match="fractional"):
        ml.read_layer(c, str(frac))


def test_the_saved_mask_keeps_the_grid_and_the_ids(tmp_path):
    c, m, labels = _files(tmp_path)
    nx, ny = 10, 8
    lin = lambda x, y, z: (z * ny + y) * nx + x   # noqa: E731
    five = np.flatnonzero(labels.reshape(-1) == 5)
    grown = np.sort(np.concatenate([five, [lin(6, 2, 1), lin(6, 3, 1)]]))
    new = np.array([lin(0, 0, 0), lin(1, 0, 0)])
    out = tmp_path / "out.fits"
    r = ml.write_layer(c, m, str(out), {5: grown, 6: new}, [2], ["test"])
    got = fits.getdata(out)
    assert got.shape == labels.shape and got.dtype.kind == "i"
    assert np.count_nonzero(got == 5) == 14 and np.count_nonzero(got == 6) == 2
    assert np.count_nonzero(got == 2) == 0
    assert r["n_sources"] == 2 and r["max_id"] == 6
    h = fits.getheader(out)
    assert h["CDELT3"] == 5000.0 and "test" in str(h["HISTORY"])
    # A voxel of another source is never taken.
    with pytest.raises(ValueError, match="other sources"):
        ml.write_layer(c, m, str(out), {6: np.array([lin(8, 6, 4)])}, [], [])
    with pytest.raises(ValueError, match="outside"):
        ml.write_layer(c, m, str(out), {6: np.array([10 ** 6])}, [], [])


def _save_body(head, arrays):
    text = __import__("json").dumps(head).encode()
    pad = (4 - (4 + len(text)) % 4) % 4
    return struct.pack("<I", len(text)) + text + b" " * pad + b"".join(np.asarray(a, "<u4").tobytes() for a in arrays)


def test_the_save_body_is_read_strictly():
    head, edits = ml.parse_edits(_save_body({"edits": [{"id": 3, "count": 2}, {"id": 9, "count": 1}]}, [[1, 2], [7]]))
    assert list(edits[3]) == [1, 2] and list(edits[9]) == [7]
    with pytest.raises(ValueError):
        ml.parse_edits(_save_body({"edits": [{"id": 3, "count": 3}]}, [[1, 2]]))
    with pytest.raises(ValueError):
        ml.parse_edits(_save_body({"edits": [{"id": 3, "count": 1}]}, [[1, 2]]))
    with pytest.raises(ValueError):
        ml.parse_edits(_save_body({"edits": [{"id": 0, "count": 0}]}, []))


async def test_the_routes(client, auth_headers, tmp_path):
    c, m, labels = _files(tmp_path)
    cube = (await client.post("/v1/datasets/open", json={"path": c}, headers=auth_headers)).json()
    r = await client.post("/v1/cube/mask_layer_bin", json={"dataset_id": cube["dataset_id"], "mask_path": m},
                          headers=auth_headers)
    assert r.status_code == 200
    body = r.content
    (n,) = struct.unpack_from("<I", body, 0)
    head = __import__("json").loads(body[4:4 + n])
    assert head["mask_name"] == "cube_mask.fits" and [s["id"] for s in head["sources"]] == [2, 5]
    # Saved: source 5 keeps one voxel, a new source 6.
    five = np.flatnonzero(labels.reshape(-1) == 5)
    save = _save_body({"dataset_id": cube["dataset_id"], "mask_path": m, "removed": [],
                       "edits": [{"id": 5, "count": 1, "new": False}, {"id": 6, "count": 1, "new": True}]},
                      [five[:1], [0]])
    r = await client.post("/v1/cube/mask_layer_save", content=save, headers=auth_headers)
    j = r.json()
    assert r.status_code == 200 and j["valid"], j
    assert j["edited"] == [5, 6] and j["n_sources"] == 3 and j["workspace_filename"].endswith(".fits")
    got = fits.getdata(j["output_path"])
    assert np.count_nonzero(got == 5) == 1 and got.reshape(-1)[0] == 6
    assert "source 6 added" in str(fits.getheader(j["output_path"])["HISTORY"])
    # Another grid: 422.
    bad = tmp_path / "bad.fits"
    fits.PrimaryHDU(np.zeros((6, 8, 9), np.int32), header=_header()).writeto(bad)
    r = await client.post("/v1/cube/mask_layer_bin", json={"dataset_id": cube["dataset_id"], "mask_path": str(bad)},
                          headers=auth_headers)
    assert r.status_code == 422 and "grid" in r.json()["detail"]
    # Nothing edited: 422.
    r = await client.post("/v1/cube/mask_layer_save", content=_save_body(
        {"dataset_id": cube["dataset_id"], "mask_path": m, "edits": [], "removed": []}, []), headers=auth_headers)
    assert r.status_code == 422


async def test_the_hub_takes_a_mask_command_and_a_mask_product(client, auth_headers):
    from app.routers.headset import PRODUCT_KINDS, HeadsetCommandRequest
    assert "mask" in PRODUCT_KINDS
    assert HeadsetCommandRequest(type="mask", dataset_id="ds1", path="/d/m.fits").type == "mask"
