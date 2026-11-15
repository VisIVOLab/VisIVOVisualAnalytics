"""A label mask over a cube, for the headset's mask layers (issue #282, epic #278).

A source finder (SoFiA-2 and others) or the desktop writes a mask cube: an
integer per voxel, 0 for none, else the source's ID. The headset shows it over
the cube (its outline, only the masked emission, the residuals, each source's
label at its centroid), refines one source with the Kinematic Lasso, and
saves it back. Two things happen here:

* ``read_layer``: the mask's voxels as per-source lists of linear indices of
  the cube's full grid (x fastest: ``(z * ny + y) * nx + x``), sorted, with
  each source's voxel count, bounding box and geometric centroid. Only the
  non-zero voxels travel (HCG 44's SoFiA-2 mask: 42 171 of 77 million). The
  mask must be on the cube's grid: the same non-degenerate shape, and the same
  WCS on the axes they share (CRVAL, CDELT, CRPIX, CTYPE).
* ``write_layer``: the original mask with the edited sources replaced (each
  edited source's voxels are cleared, then set to its new list) and the
  removed ones cleared, written as FITS with the original header (same grid,
  same IDs; new sources get the IDs the page gave them, above the original
  maximum), and HISTORY cards that say what was done.

The page draws the mask on its reduced grid (static/vr/js/mask-layer.js).
"""
from __future__ import annotations

import json
import math
import struct
from typing import Any

import numpy as np

#: The most voxels a layer may carry to the headset (64 MiB of indices).
MAX_LAYER_VOXELS = 16 * 1024 * 1024
#: The most sources (IDs) a layer may have, and the largest ID (int32).
MAX_SOURCES = 65535
MAX_ID = 2 ** 31 - 1
#: Header keys compared between the cube and the mask, per axis.
_AXIS_KEYS = ("CTYPE", "CRVAL", "CDELT", "CRPIX")


def _open(path: str):
    from .fits_dataset import ScientificFitsDataset   # noqa: PLC0415
    return ScientificFitsDataset(path)


def grid_problem(cube_path: str, mask_path: str) -> str:
    """'' when the mask is on the cube's grid, else what differs."""
    cube, mask = _open(cube_path), _open(mask_path)
    ch, cr = cube.header, cube.client_axis_to_fits_axis
    mh, mr = mask.header, mask.client_axis_to_fits_axis
    for axis in (1, 2, 3):
        fc, fm = cr.get(axis), mr.get(axis)
        if (fc is None) != (fm is None):
            return f"the cube and the mask do not have the same axes (viewer axis {axis})"
        if fc is None:
            continue
        nc, nm = int(cube.axis_sizes[fc - 1]), int(mask.axis_sizes[fm - 1])
        if nc != nm:
            return f"axis {axis} has {nc} pixels in the cube and {nm} in the mask"
        for key in _AXIS_KEYS:
            a, b = ch.get(f"{key}{fc}"), mh.get(f"{key}{fm}")
            if a is None and b is None:
                continue
            if key == "CTYPE":
                if str(a or "").strip().upper() != str(b or "").strip().upper():
                    return f"{key}{fc} is {a!r} in the cube and {key}{fm} {b!r} in the mask"
                continue
            try:
                fa, fb = float(a), float(b)
            except (TypeError, ValueError):
                return f"{key}{fc} cannot be compared ({a!r} / {b!r})"
            if not math.isclose(fa, fb, rel_tol=1e-9, abs_tol=1e-12 * max(1.0, abs(fa))):
                return f"{key}{fc} is {fa!r} in the cube and {key}{fm} {fb!r} in the mask"
    return ""


def _labels(path: str):
    """The mask's data as (nz, ny, nx), memory-mapped where FITS allows, and its dataset."""
    ds = _open(path)
    hdul, raw = ds.open_memmap()
    data = ds.squeeze_to_3d(raw)
    if data.ndim != 3:
        hdul.close()
        raise ValueError("The mask is not a cube.")
    if not np.issubdtype(data.dtype, np.integer):
        # A float mask (a desktop mask saved as 0/1 floats): read as integers,
        # refused when it holds fractions.
        probe = np.asarray(data[:: max(1, data.shape[0] // 8)])
        finite = probe[np.isfinite(probe)]
        if finite.size and not np.all(finite == np.round(finite)):
            hdul.close()
            raise ValueError("The mask holds fractional values: it is not a label mask.")
    return hdul, data, ds


def read_layer(cube_path: str, mask_path: str, max_voxels: int = MAX_LAYER_VOXELS) -> dict[str, Any]:
    """The mask as per-source sorted index lists (see the module's docstring):
    {shape [nx, ny, nz], sources: [{id, count, offset, bbox [x0, x1, y0, y1,
    z0, z1], centroid [x, y, z]}], max_id, n_voxels, indices (uint32)}. Read
    one channel at a time."""
    problem = grid_problem(cube_path, mask_path)
    if problem:
        raise ValueError(f"The mask is not on the cube's grid: {problem}.")
    hdul, data, _ = _labels(mask_path)
    try:
        nz, ny, nx = data.shape
        if nx * ny * nz >= 2 ** 32:
            raise ValueError("The cube has more voxels than a 32-bit index addresses.")
        idx_parts, id_parts = [], []
        total = 0
        for z in range(nz):
            plane = np.asarray(data[z])
            if not np.issubdtype(plane.dtype, np.integer):
                plane = np.nan_to_num(plane, nan=0.0).astype(np.int64)
            flat = np.flatnonzero(plane)
            if not flat.size:
                continue
            total += flat.size
            if total > max_voxels:
                raise ValueError(f"The mask has more than {max_voxels} voxels in its sources: "
                                 "too many for the headset.")
            ids = plane.reshape(-1)[flat].astype(np.int64)
            if ids.min() < 0:
                raise ValueError("The mask holds negative values: it is not a label mask.")
            if ids.max() > MAX_ID:
                raise ValueError(f"The mask holds an ID above {MAX_ID}: IDs are 32-bit signed integers.")
            idx_parts.append(flat.astype(np.uint64) + np.uint64(z * ny * nx))
            id_parts.append(ids)
    finally:
        hdul.close()
    if idx_parts:
        idx = np.concatenate(idx_parts).astype(np.uint32)
        ids = np.concatenate(id_parts)
    else:
        idx = np.zeros(0, np.uint32)
        ids = np.zeros(0, np.int64)
    order = np.lexsort((idx, ids))           # by ID, then by index
    idx, ids = idx[order], ids[order]
    uniq, starts, counts = np.unique(ids, return_index=True, return_counts=True)
    if uniq.size > MAX_SOURCES:
        raise ValueError(f"The mask has {uniq.size} sources: more than {MAX_SOURCES}.")
    sources = []
    for sid, s0, n in zip(uniq.tolist(), starts.tolist(), counts.tolist()):
        part = idx[s0:s0 + n].astype(np.int64)
        x, rest = part % nx, part // nx
        y, z = rest % ny, rest // ny
        sources.append({"id": int(sid), "count": int(n), "offset": int(s0),
                        "bbox": [int(x.min()), int(x.max()), int(y.min()), int(y.max()), int(z.min()), int(z.max())],
                        "centroid": [float(x.mean()), float(y.mean()), float(z.mean())]})
    return {"shape": [int(nx), int(ny), int(nz)], "sources": sources,
            "max_id": int(uniq.max()) if uniq.size else 0, "n_voxels": int(idx.size), "indices": idx}


def pack_layer(layer: dict[str, Any], meta: dict[str, Any]) -> bytes:
    """The wire: uint32 little-endian JSON length, the JSON (padded with
    spaces to a multiple of 4 bytes), then the indices as uint32 little-endian."""
    head = {k: v for k, v in layer.items() if k != "indices"}
    head.update(meta)
    text = json.dumps(head, separators=(",", ":")).encode("utf-8")
    text += b" " * ((4 - len(text) % 4) % 4)
    return struct.pack("<I", len(text)) + text + layer["indices"].astype("<u4").tobytes()


def parse_edits(body: bytes) -> tuple[dict[str, Any], dict[int, np.ndarray]]:
    """A save's body (the page's mask-layer.js saveBody): uint32 JSON length,
    the JSON {edits: [{id, count}], removed, ...}, padding to 4, then each
    edited source's sorted indices in the order of ``edits``."""
    if len(body) < 4:
        raise ValueError("The body is too short.")
    (n,) = struct.unpack_from("<I", body, 0)
    if n > len(body) - 4 or n > 1_000_000:
        raise ValueError("The body's header length is wrong.")
    head = json.loads(body[4:4 + n].decode("utf-8"))
    pos = 4 + n
    pos += (4 - pos % 4) % 4
    data = np.frombuffer(body, dtype="<u4", offset=pos) if pos < len(body) else np.zeros(0, "<u4")
    edits: dict[int, np.ndarray] = {}
    k = 0
    for e in head.get("edits", []):
        sid, count = int(e["id"]), int(e["count"])
        if sid <= 0 or sid > MAX_ID or count < 0:
            raise ValueError(f"Bad edit {e!r}.")
        if sid in edits:
            raise ValueError(f"Source {sid} is edited twice.")
        if k + count > data.size:
            raise ValueError("The body holds fewer indices than the edits say.")
        edits[sid] = data[k:k + count].astype(np.int64)
        k += count
    if k != data.size:
        raise ValueError("The body holds more indices than the edits say.")
    if len(edits) > MAX_SOURCES:
        raise ValueError(f"More than {MAX_SOURCES} sources edited.")
    removed = head.get("removed", [])
    if not isinstance(removed, list) or len(removed) > MAX_SOURCES:
        raise ValueError(f"'removed' must be a list of at most {MAX_SOURCES} IDs.")
    head["removed"] = [int(i) for i in removed]
    if any(i <= 0 or i > MAX_ID for i in head["removed"]):
        raise ValueError(f"'removed' holds an ID outside 1..{MAX_ID}.")
    return head, edits


def _allocate(path: str, header, shape: tuple[int, ...]) -> int:
    """An int32 FITS file of ``shape`` at ``path``, its data zero, written
    without holding the data in memory: the header, then the file extended to
    the padded data size (astropy's recipe for large files). Returns where
    the data starts (the header's length)."""
    from astropy.io import fits   # noqa: PLC0415
    header = header.copy()
    # The structural keys are the HDU's own (in their FITS order), then sized to `shape`.
    for key in ["BITPIX", "NAXIS"] + [f"NAXIS{i}" for i in range(1, 10)]:
        header.remove(key, ignore_missing=True)
    hdu = fits.PrimaryHDU(data=np.zeros((1,) * len(shape), dtype=np.int32), header=header)
    h = hdu.header
    for i, n in enumerate(reversed(shape), start=1):
        h[f"NAXIS{i}"] = int(n)
    h.tofile(path, overwrite=True)
    nbytes = int(np.prod(shape)) * 4
    start = len(h.tostring())
    with open(path, "rb+") as f:
        f.seek(start + ((nbytes + 2879) // 2880) * 2880 - 1)
        f.write(b"\0")
    return start


def write_layer(cube_path: str, mask_path: str, out_path: str, edits: dict[int, np.ndarray],
                removed: list[int], history: list[str], slab_bytes: int = 16 * 1024 * 1024) -> dict[str, Any]:
    """The edited mask written to ``out_path`` (see the module's docstring),
    the name reserved for it: built in a hidden temporary file beside it and
    moved over the reservation in one step. The original is read one slab of
    channels at a time and written out sequentially (big-endian int32, the
    FITS order), the edited and removed sources cleared in the same pass
    (np.isin); then each edited source is set to its voxels through a
    memory map of the output, which touches only their pages. Indices outside
    the grid are refused; a voxel of another source claimed by an edit is
    refused too (the page never steals: Add takes free voxels)."""
    import os   # noqa: PLC0415
    import uuid   # noqa: PLC0415
    from pathlib import Path   # noqa: PLC0415
    from astropy.io import fits   # noqa: PLC0415
    problem = grid_problem(cube_path, mask_path)
    if problem:
        raise ValueError(f"The mask is not on the cube's grid: {problem}.")
    target = Path(out_path)
    tmp = target.with_name(f".{target.name}.{uuid.uuid4().hex}.tmp")
    hdul, data, ds = _labels(mask_path)
    try:
        nz, ny, nx = data.shape
        nvox = nx * ny * nz
        for sid, idx in edits.items():
            if idx.size and (idx.min() < 0 or idx.max() >= nvox):
                raise ValueError(f"Source {sid} has voxels outside the cube.")
        hdu = hdul[ds.hdu_index]
        orig_shape, header = tuple(hdu.data.shape), hdu.header.copy()
        # Written as the primary HDU of its own file, in plain int32.
        for key in ("BSCALE", "BZERO", "BLANK", "XTENSION", "PCOUNT", "GCOUNT", "EXTNAME", "SIMPLE", "EXTEND"):
            header.remove(key, ignore_missing=True)
        for line in history:
            header.add_history(line[:70])
        if int(np.prod(orig_shape)) != nvox:
            raise ValueError(f"The mask's data {orig_shape} is not {(nz, ny, nx)} voxels.")
        start = _allocate(str(tmp), header, orig_shape)
        clear = np.array(sorted(set(edits) | set(removed)), dtype=np.int64)
        step = max(1, slab_bytes // max(1, ny * nx * 8))
        ids: set[int] = set()
        n_voxels = 0
        with open(tmp, "rb+") as f:
            f.seek(start)
            for z0 in range(0, nz, step):
                slab = np.asarray(data[z0:z0 + step])
                if not np.issubdtype(slab.dtype, np.integer):
                    slab = np.nan_to_num(slab, nan=0.0)
                slab = slab.astype(np.int64, copy=False)
                if clear.size:
                    slab = np.where(np.isin(slab, clear), 0, slab)
                kept = slab[slab > 0]
                n_voxels += int(kept.size)
                ids.update(np.unique(kept).tolist())
                f.write(slab.astype(">i4").tobytes())
        if any(idx.size for idx in edits.values()):
            out = np.memmap(tmp, dtype=">i4", mode="r+", offset=start, shape=(nvox,))
            try:
                for sid, idx in edits.items():
                    if not idx.size:
                        continue
                    if np.any(out[idx] != 0):
                        raise ValueError(f"Source {sid} claims voxels of other sources.")
                    out[idx] = sid
                    n_voxels += int(np.unique(idx).size)
                    ids.add(int(sid))
                out.flush()
            finally:
                del out
        os.replace(tmp, target)
    finally:
        hdul.close()
        tmp.unlink(missing_ok=True)
    return {"output_path": out_path, "n_sources": len(ids), "max_id": max(ids) if ids else 0, "n_voxels": n_voxels}
