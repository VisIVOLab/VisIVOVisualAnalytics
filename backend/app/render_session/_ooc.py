"""Out-of-core volume rendering of a cube from its brick pyramid (issue #70).

A cube larger than ``VISIVO_RENDER_OUT_OF_CORE_BYTES`` is not loaded: the
session builds (or reuses) its brick pyramid (:mod:`app.brick_pyramid`) and
draws, for the current view, the blocks :func:`app.brick_lod.choose_nodes`
picks: fine where the view needs detail, coarse elsewhere, within
``VISIVO_RENDER_OOC_BUDGET_BYTES`` of GPU data and ``VISIVO_RENDER_OOC_MAX_BLOCKS``
blocks (each block is one ray-casting pass, about 5 ms per frame on a V100).

Each block is a ``vtkGPUVolumeRayCastMapper`` + ``vtkVolume`` sharing the
session's volume property; :class:`BlockVolumes` stands in for the single
mapper and volume of the in-core path, so the code that sets sampling, blend
mode and visibility does not change. Each block carries one ghost sample on
every side and its mapper is cropped to the region the block owns, so blocks of
different levels meet on the same plane, without gaps or overlaps. They are put
in back-to-front order (an octree traversal) before every frame, which
composites non-overlapping boxes correctly in Composite mode.

MIP and MinIP take an extremum along the whole ray, which separate passes
cannot combine (a near block's maximum would hide a larger one behind it), so in
those modes the view is one volume: the finest level that fits the budget.

The block set is chosen again when the view has changed and the user is not
dragging; the new blocks are read in the background of the frames (a time
budget per frame) and replace the old set only when all have arrived, so the
picture never shows holes or overlaps.
"""
from __future__ import annotations

import logging
import os
import time
from collections import OrderedDict
from dataclasses import dataclass

import numpy as np

from ..brick_lod import Camera, Node, PyramidLayout, choose_nodes, node_box, node_region

logger = logging.getLogger("visivo.render")

OOC_BYTES = int(os.environ.get("VISIVO_RENDER_OUT_OF_CORE_BYTES", str(6 * 1024 ** 3)))
OOC_BUDGET_BYTES = int(os.environ.get("VISIVO_RENDER_OOC_BUDGET_BYTES", str(2 * 1024 ** 3)))
OOC_MAX_BLOCKS = int(os.environ.get("VISIVO_RENDER_OOC_MAX_BLOCKS", "12"))
OOC_NODE = int(os.environ.get("VISIVO_RENDER_OOC_NODE", "256"))
# The coarse copy of the cube that outline, isosurface and percentiles use.
_PROXY_VOXELS = 256 ** 3
# Blocks own half-open boxes: the high face drawn this far (level-0 voxels) inside.
_FACE_EPS = 1e-3
# Time spent reading blocks per frame while a new set is loading.
_LOAD_SECONDS_PER_FRAME = 0.15


def wants_out_of_core(cube_bytes: int) -> bool:
    return cube_bytes > OOC_BYTES


@dataclass(frozen=True)
class WholeLevel:
    """A whole pyramid level as one block (MIP and MinIP)."""
    level: int


def _vtk_crop_planes(crop, origin, spacing: float, dims) -> tuple[float, ...]:
    """The cropping planes that make ``vtkGPUVolumeRayCastMapper`` draw exactly
    ``crop`` (x0, x1, y0, y1, z0, z1, world units).

    The mapper takes a plane p as a fraction of the samples' extent,
    (p - o) / ((n - 1) s), but draws it on the cells' extent, which is one
    sample longer and starts half a sample earlier: it draws up to
    d = o - s/2 + (p - o) n / (n - 1). Measured with VTK 9.3.1 (EGL, V100) and
    9.6.2; given ``crop`` itself, adjacent blocks overlapped by a sample and
    their faces showed as bright lines. The mapper includes both faces, so a
    ray lying exactly on a shared face would be drawn by both blocks: the
    boxes are half-open, the high plane at least ``_FACE_EPS`` inside. VTK keeps
    the planes as float32, so each is rounded there, the low one up and the
    high one strictly down (at large coordinates ``_FACE_EPS`` is below
    float32's resolution)."""
    def plane(d, o, n):
        return o + (d - o + spacing / 2) * (n - 1) / n if n > 1 else o

    planes = []
    for axis, n in enumerate(dims):
        o = origin[axis]
        lo = plane(crop[2 * axis], o, n)
        hi = plane(crop[2 * axis + 1] - _FACE_EPS, o, n)
        lo32 = np.float32(lo)
        if float(lo32) < lo:            # in float64: NumPy would compare in float32
            lo32 = np.nextafter(lo32, np.float32(np.inf))
        hi32 = np.float32(hi)
        if float(hi32) >= hi:
            hi32 = np.nextafter(hi32, np.float32(-np.inf))
        planes += [float(lo32), float(hi32)]
    return tuple(planes)


class BlockVolumes:
    """Several volume blocks behind the interface of one mapper and one volume.

    The session's code calls the methods it calls on a ``vtkGPUVolumeRayCastMapper``
    (sample distances, jittering, blend mode) and on a ``vtkVolume``
    (visibility); they are remembered and applied to every block, present and
    future."""

    def __init__(self, prop) -> None:
        self._prop = prop
        self._renderer = None
        self._blocks: dict = {}                   # key -> (image, mapper, volume)
        self._arrays: dict = {}                   # key -> the array VTK reads, kept alive
        self._order: list = []                    # keys in the order last drawn
        self._s = {"auto": 1, "image_sd": 1.0, "sample_d": 1.0, "jitter": 1,
                   "blend": "composite", "visible": 1}

    # -- the mapper's interface ------------------------------------------
    def SetAutoAdjustSampleDistances(self, v):
        self._set("auto", int(v))

    def GetAutoAdjustSampleDistances(self):
        return self._s["auto"]

    def SetImageSampleDistance(self, v):
        self._set("image_sd", float(v))

    def GetImageSampleDistance(self):
        return self._s["image_sd"]

    def SetSampleDistance(self, v):
        self._set("sample_d", float(v))

    def GetSampleDistance(self):
        return self._s["sample_d"]

    def SetUseJittering(self, v):
        self._set("jitter", int(v))

    def SetBlendModeToMaximumIntensity(self):
        self._set("blend", "max")

    def SetBlendModeToMinimumIntensity(self):
        self._set("blend", "min")

    def SetBlendModeToComposite(self):
        self._set("blend", "composite")

    # -- the volume's interface ------------------------------------------
    def SetVisibility(self, v):
        self._set("visible", int(v))

    def GetVisibility(self):
        return self._s["visible"]

    # -- blocks -----------------------------------------------------------
    @property
    def blend(self) -> str:
        return self._s["blend"]

    def attach(self, renderer) -> None:
        self._renderer = renderer
        for _, _, volume in self._blocks.values():
            renderer.AddVolume(volume)

    @property
    def nodes(self) -> set:
        return set(self._blocks)

    def replace(self, blocks: dict) -> None:
        """Show exactly ``blocks`` (key -> (array z,y,x, origin x,y,z, spacing,
        crop box x0,x1,y0,y1,z0,z1)); blocks already shown are kept as they are."""
        import vtk  # noqa: PLC0415
        from vtk.util.numpy_support import numpy_to_vtk  # noqa: PLC0415

        self._order = []
        for key in [k for k in self._blocks if k not in blocks]:
            image, _, volume = self._blocks.pop(key)
            # Detached explicitly: otherwise VTK's wrapper can keep the shared
            # array alive after the block is gone (seen with VTK 9.6.2).
            image.GetPointData().SetScalars(None)
            self._arrays.pop(key, None)
            if self._renderer is not None:
                self._renderer.RemoveVolume(volume)
        for key, (array, origin, spacing, crop) in blocks.items():
            if key in self._blocks:
                continue
            nz, ny, nx = array.shape
            image = vtk.vtkImageData()
            image.SetDimensions(nx, ny, nz)
            image.SetSpacing(spacing, spacing, spacing)
            image.SetOrigin(*origin)
            # Shared with the cache, not copied: the block's data is never
            # changed after it is read, and a copy would double what a block
            # costs in memory (a whole level, in MIP).
            flat = np.ascontiguousarray(array, dtype=np.float32).ravel()
            self._arrays[key] = flat
            scalars = numpy_to_vtk(flat, deep=False, array_type=vtk.VTK_FLOAT)
            scalars.SetName("intensity")
            image.GetPointData().SetScalars(scalars)
            mapper = vtk.vtkGPUVolumeRayCastMapper()
            mapper.SetInputData(image)
            # Draw only the region the block owns; the ghost samples outside
            # it are there for interpolation up to its faces.
            mapper.SetCropping(1)
            mapper.SetCroppingRegionPlanes(*_vtk_crop_planes(crop, origin, spacing, (nx, ny, nz)))
            mapper.SetCroppingRegionFlagsToSubVolume()
            volume = vtk.vtkVolume()
            volume.SetMapper(mapper)
            volume.SetProperty(self._prop)
            self._blocks[key] = (image, mapper, volume)
            self._apply(mapper, volume)
            if self._renderer is not None:
                self._renderer.AddVolume(volume)

    def draw_in_order(self, keys: list) -> None:
        """Draw the blocks in ``keys`` order (the first is drawn first).

        The renderer's prop collection is reordered directly:
        ``RemoveVolume`` would release each block's GPU textures, so every
        frame would upload them again."""
        if self._renderer is None or len(self._blocks) < 2 or keys == self._order:
            return
        props = self._renderer.GetViewProps()
        for key in keys:
            volume = self._blocks[key][2]
            props.RemoveItem(volume)
            props.AddItem(volume)
        self._order = list(keys)

    def clear(self) -> None:
        self.replace({})

    def _set(self, key, value) -> None:
        self._s[key] = value
        for _, mapper, volume in self._blocks.values():
            self._apply(mapper, volume)

    def _apply(self, mapper, volume) -> None:
        s = self._s
        mapper.SetAutoAdjustSampleDistances(s["auto"])
        if not s["auto"]:
            mapper.SetImageSampleDistance(s["image_sd"])
            mapper.SetSampleDistance(s["sample_d"])
        mapper.SetUseJittering(s["jitter"])
        {"max": mapper.SetBlendModeToMaximumIntensity,
         "min": mapper.SetBlendModeToMinimumIntensity,
         "composite": mapper.SetBlendModeToComposite}[s["blend"]]()
        volume.SetVisibility(s["visible"])


class OutOfCoreCube:
    """The pyramid of the session's cube, the coarse proxy, and the blocks
    of the current view."""

    def __init__(self, dataset_path: str, hdu: int, session_id: str) -> None:
        from ..brick_pyramid import BrickPyramid, StalePyramid, build_pyramid, default_pyramid_path  # noqa: PLC0415

        self.dataset_path, self.hdu, self.session_id = dataset_path, hdu, session_id
        path = default_pyramid_path(dataset_path, hdu)
        pyramid = None
        if path.exists():
            try:
                pyramid = BrickPyramid(path, source=dataset_path)
            except (StalePyramid, ValueError, OSError) as exc:
                logger.warning("[render-session %s] pyramid %s unusable (%s): rebuilding",
                               session_id, path, exc)
        if pyramid is None:
            t0 = time.time()
            logger.info("[render-session %s] building the brick pyramid of %s",
                        session_id, dataset_path)
            build_pyramid(dataset_path, path, hdu=hdu)
            logger.info("[render-session %s] pyramid built in %.0f s", session_id, time.time() - t0)
            pyramid = BrickPyramid(path, source=dataset_path)
        self.pyramid = pyramid
        self.layout = PyramidLayout.of(pyramid)
        self.shape = pyramid.shape(0)                         # (z, y, x)
        lo, hi = pyramid.stats(0)
        finite_lo, finite_hi = lo[np.isfinite(lo)], hi[np.isfinite(hi)]
        self.vmin = float(finite_lo.min()) if finite_lo.size else 0.0
        self.vmax = float(finite_hi.max()) if finite_hi.size else 1.0
        self.blocks: BlockVolumes | None = None
        self._cache: OrderedDict = OrderedDict()   # key -> (array, origin, spacing, crop)
        self._cache_bytes = 0
        self._view_key = None
        self._target: list = []
        self._source = None

    # -- the coarse proxy ---------------------------------------------------
    def proxy(self) -> tuple[np.ndarray, tuple[float, float, float], float]:
        """The finest level of at most ``_PROXY_VOXELS`` voxels, with blank
        voxels set to the cube's minimum (as the in-core path does), its origin
        and spacing in level-0 voxels."""
        level = next(level for level in range(self.pyramid.levels)
                     if int(np.prod(self.pyramid.shape(level))) <= _PROXY_VOXELS)
        data = self.pyramid._h5[f"level{level}/data"][...]
        data = np.where(np.isfinite(data), data, np.float32(self.vmin)).astype(np.float32)
        spacing = float(2 ** level)
        offset = (spacing - 1.0) / 2.0
        return data, (offset, offset, offset), spacing

    # -- a sample of the full-resolution values ----------------------------
    def sample(self, voxels: int = 8_000_000) -> np.ndarray:
        """Finite level-0 values from whole bricks drawn at random among those
        that hold data, for the percentiles (threshold, slice range): the
        proxy's averages have a narrower spread than the cube's voxels."""
        brick = self.layout.brick
        peaks = self.layout.brick_max[0]
        cells = np.argwhere(np.isfinite(peaks))
        if not len(cells):
            return np.array([self.vmin], dtype=np.float32)
        count = min(len(cells), max(1, voxels // brick ** 3))
        rng = np.random.default_rng(0)
        chosen = cells[rng.choice(len(cells), size=count, replace=False)]
        data = self.pyramid._h5["level0/data"]
        parts = []
        for bz, by, bx in sorted(map(tuple, chosen)):
            block = data[bz * brick:(bz + 1) * brick, by * brick:(by + 1) * brick,
                         bx * brick:(bx + 1) * brick]
            parts.append(block[np.isfinite(block)].astype(np.float32, copy=False))
        out = np.concatenate(parts)
        return out if out.size else np.array([self.vmin], dtype=np.float32)

    # -- the full-resolution plane for the cutting plane ---------------------
    def plane(self, z: int) -> np.ndarray:
        """Channel ``z`` at full resolution, read from the FITS file (a plane is
        contiguous there; from the pyramid it would cost a whole row of
        bricks)."""
        if self._source is None:
            from ..brick_pyramid import _SourceCube  # noqa: PLC0415
            self._source = _SourceCube(self.dataset_path, self.hdu, 1 << 62)
        _, ny, nx = self.shape
        return self._source.block(z, z + 1, 0, ny, 0, nx)[0]

    # -- choosing and loading blocks ------------------------------------------
    def update(self, renderer, window_size, threshold, interactive: bool) -> bool:
        """Choose the blocks for the current view (not while dragging) and read
        some of the missing ones; True when more remain to read, so the caller
        renders again. ``threshold``: blocks whose values all lie below it are
        left out (Composite only)."""
        if self.blocks is None:
            return False
        cam = renderer.GetActiveCamera()
        if interactive:
            # The set stays while dragging; the drawing order follows the eye.
            self._sort(cam.GetPosition())
            return False
        w, h = (int(window_size[0]) or 1, int(window_size[1]) or 1)
        blend = self.blocks.blend
        key = (tuple(round(v, 3) for v in cam.GetPosition()),
               tuple(round(v, 3) for v in cam.GetFocalPoint()),
               round(cam.GetViewAngle(), 3), int(cam.GetParallelProjection()),
               round(cam.GetParallelScale(), 3), w, h, threshold, blend)
        if key != self._view_key:
            self._view_key = key
            budget = OOC_BUDGET_BYTES // 4
            if blend != "composite":
                try:
                    self._target = [self._whole_level(budget)]
                except ValueError as exc:
                    logger.warning("[render-session %s] %s", self.session_id, exc)
                    return False
            else:
                planes = [0.0] * 24
                cam.GetFrustumPlanes(w / h, planes)
                camera = Camera(
                    position=tuple(cam.GetPosition()), viewport_height_px=h,
                    view_angle=cam.GetViewAngle(),
                    parallel_scale=cam.GetParallelScale() if cam.GetParallelProjection() else None,
                    planes=[tuple(planes[i:i + 4]) for i in range(0, 24, 4)],
                    direction=tuple(cam.GetDirectionOfProjection()))
                try:
                    # halo=1: each block is uploaded with a ghost sample per side.
                    self._target = choose_nodes(self.layout, camera, budget_voxels=budget,
                                                node_size=OOC_NODE, threshold=threshold,
                                                max_nodes=OOC_MAX_BLOCKS, halo=1)
                except ValueError as exc:
                    logger.warning("[render-session %s] %s", self.session_id, exc)
                    return False
        missing = [k for k in self._target if k not in self._cache]
        deadline = time.perf_counter() + _LOAD_SECONDS_PER_FRAME
        for k in missing:
            if time.perf_counter() > deadline:
                break
            self._load(k)
        if any(k not in self._cache for k in self._target):
            self._sort(cam.GetPosition())
            return True
        if set(self._target) != self.blocks.nodes:
            self.blocks.replace({k: self._cache[k] for k in self._target})
        self._sort(cam.GetPosition())
        return False

    def _whole_level(self, budget_voxels: int) -> WholeLevel:
        """The finest level that fits the budget as one block, ghosts included."""
        for level, shape in enumerate(self.layout.shapes):
            if int(np.prod([n + 2 for n in shape])) <= budget_voxels:
                return WholeLevel(level)
        raise ValueError(f"no level of the cube fits the budget ({budget_voxels} voxels) "
                         "as one block")

    def _sort(self, eye) -> None:
        """Back to front by an octree traversal: from the top level down, the
        siblings farthest from the eye's cell come first along x, then y, then
        z. For boxes of a hierarchical partition this is a correct visibility
        order (distance between centres is not, for boxes of different sizes)."""
        keys = [k for k in self.blocks.nodes if isinstance(k, Node)]
        if len(keys) < 2:
            return
        top = len(self.layout.shapes) - 1
        ex, ey, ez = (float(v) for v in eye)

        def path(node: Node):
            out = []
            for level in range(top, node.level - 1, -1):
                shift = level - node.level
                cell = OOC_NODE * 2 ** level          # level-0 voxels per cell
                # Cell k spans [k*cell - 0.5, (k+1)*cell - 0.5) in world units.
                out.append(tuple(-abs((i >> shift) - np.floor((e + 0.5) / cell))
                                 for i, e in ((node.ix, ex), (node.iy, ey), (node.iz, ez))))
            return out

        self.blocks.draw_in_order(sorted(keys, key=path))

    def _region(self, key):
        """(level, (z, y, x) slices at that level, level-0 lower and upper
        corners (x, y, z))."""
        if isinstance(key, Node):
            lo, hi = node_box(self.layout, OOC_NODE, key)
            return key.level, node_region(self.layout, OOC_NODE, key), lo, hi
        nz, ny, nx = self.layout.shapes[key.level]
        z0, y0, x0 = self.shape
        return (key.level, (slice(0, nz), slice(0, ny), slice(0, nx)),
                np.zeros(3), np.array([x0, y0, z0], dtype=float))

    def _load(self, key) -> None:
        """Read a block with one ghost sample on every side (repeating the edge
        at the cube's border) and the crop box of the region it owns: voxel i
        spans [i - 0.5, i + 0.5] in level-0 units, clipped to the cube's
        sample range [0, n - 1] as in the in-core path. Blank voxels are set to
        the cube's minimum, as in the in-core path."""
        level, (zs, ys, xs), lo, hi = self._region(key)
        dims = self.layout.shapes[level]
        lo_i = [max(0, r.start - 1) for r in (zs, ys, xs)]
        hi_i = [min(n, r.stop + 1) for r, n in zip((zs, ys, xs), dims)]
        # Read straight into the padded block (one copy of it in memory, which
        # matters for a whole level), then repeat the edge where there was no
        # neighbour to read, axis by axis as np.pad(mode="edge") does.
        regions = (zs, ys, xs)
        data = np.empty([r.stop - r.start + 2 for r in regions], dtype=np.float32)
        inner = tuple(slice(a - (r.start - 1), b - (r.start - 1)) for r, a, b in zip(regions, lo_i, hi_i))
        self.pyramid._h5[f"level{level}/data"].read_direct(
            data, np.s_[lo_i[0]:hi_i[0], lo_i[1]:hi_i[1], lo_i[2]:hi_i[2]], inner)
        for axis, (r, a, b) in enumerate(zip(regions, lo_i, hi_i)):
            view = np.moveaxis(data, axis, 0)
            if a == r.start:
                view[0] = view[1]
            if b == r.stop:
                view[-1] = view[-2]
        np.copyto(data, np.float32(self.vmin), where=~np.isfinite(data))
        spacing = float(2 ** level)
        offset = (spacing - 1.0) / 2.0
        origin = tuple((r.start - 1) * spacing + offset for r in (xs, ys, zs))
        n0 = (self.shape[2], self.shape[1], self.shape[0])
        crop = []
        for a, b, n in zip(lo, hi, n0):
            crop += [max(float(a) - 0.5, 0.0), min(float(b) - 0.5, float(n - 1))]
        self._cache[key] = (data, origin, spacing, tuple(crop))
        self._cache_bytes += data.nbytes
        # Bounded by twice the budget: the shown set and the one loading
        # (each within the budget, ghosts included).
        while self._cache_bytes > 2 * OOC_BUDGET_BYTES and len(self._cache) > 1:
            old, (array, *_rest) = next(iter(self._cache.items()))
            if old in self._target or (self.blocks and old in self.blocks.nodes):
                self._cache.move_to_end(old)
                if all(k in self._target or (self.blocks and k in self.blocks.nodes) for k in self._cache):
                    break
                continue
            self._cache.pop(old)
            self._cache_bytes -= array.nbytes

    def close(self) -> None:
        if self.blocks is not None:
            self.blocks.clear()
        if self._source is not None:
            self._source.close()
        self.pyramid.close()
