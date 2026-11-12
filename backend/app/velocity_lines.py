"""Streamlines and arrows of a velocity field, traced on the backend (issue
#190, VR epic #166).

The desktop's velocity viewer (src/gui/vtkWindowVectorField.cpp) fetches the
display grid (/v1/velocity/grid_bin) and traces it with VTK; the VR page
asks for the result instead, traced here with the same filters and settings
on the same grid, so a headset draws lines (a few MB) rather than integrating
a 128^3 field itself:

* the grid is exactly the one /v1/velocity/grid_bin serves (the same worker,
  its spacing and origin as that route reports them), so a change of the
  display grid's geometry there is followed here;
* streamlines: seeds from ``vtkPointSource`` (a ball at the grid's centre of
  radius 0.3 x its longest side), Runge-Kutta 4-5, both directions, at most
  2 x the longest side and 2000 steps (makeStreamActor);
* arrows: at most ``max_arrows`` grid points drawn by ``vtkMaskPoints``
  (uniform spatial sampling), each the vector scaled by
  ``3 x cell / full-resolution speed max`` (makeGlyphActor);
* colour: the speed |v| over the full-resolution speed range.

The seeds and the arrows' points are random in VTK; here their random
sequence starts from ``seed``, so the same request gives the same lines.

Wire format (``encode``), little-endian: the streamline points (n x 3
float32), their speeds (n float32), the start of each line in those points
(lines + 1 uint32), then the arrows' positions (m x 3 float32) and vectors
(m x 3 float32). The counts are in the headers.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

#: The desktop's defaults (vtkWindowVectorField setupUi).
DEFAULT_SEEDS = 200
DEFAULT_ARROWS = 50_000
MAX_SEEDS = 5000
MAX_ARROWS = 500_000


@dataclass
class FlowLines:
    points: np.ndarray        # (n, 3) float32
    speed: np.ndarray         # (n,) float32
    offsets: np.ndarray       # (lines + 1,) uint32
    arrow_pos: np.ndarray     # (m, 3) float32
    arrow_vec: np.ndarray     # (m, 3) float32
    grid: dict                # the display grid's geometry and ranges

    def encode(self) -> bytes:
        return b"".join(np.ascontiguousarray(a, dtype=t).tobytes() for a, t in (
            (self.points, "<f4"), (self.speed, "<f4"), (self.offsets, "<u4"),
            (self.arrow_pos, "<f4"), (self.arrow_vec, "<f4")))


def grid_image(payload: dict):
    """The display grid as the desktop builds it (buildImageData): the
    vectors and their speed on a vtkImageData placed by ``spacing``/``origin``."""
    import vtk   # noqa: PLC0415
    from vtk.util import numpy_support   # noqa: PLC0415
    nx, ny, nz = int(payload["nx"]), int(payload["ny"]), int(payload["nz"])
    vec = np.frombuffer(payload["raw_bytes"], dtype="<f4").reshape(-1, 3).astype(np.float32)
    img = vtk.vtkImageData()
    img.SetDimensions(nx, ny, nz)
    img.SetSpacing(*[float(s) for s in payload["spacing"]])
    img.SetOrigin(*[float(o) for o in payload["origin"]])
    arr = numpy_support.numpy_to_vtk(vec, deep=True, array_type=vtk.VTK_FLOAT)
    arr.SetName("velocity")
    speed = numpy_support.numpy_to_vtk(np.sqrt((vec.astype(np.float64) ** 2).sum(axis=1))
                                       .astype(np.float32), deep=True,
                                       array_type=vtk.VTK_FLOAT)
    speed.SetName("speed")
    img.GetPointData().SetVectors(arr)
    img.GetPointData().SetScalars(speed)
    return img


def _sequence(seed: int):
    import vtk   # noqa: PLC0415
    seq = vtk.vtkMinimalStandardRandomSequence()
    seq.SetSeed(int(seed))
    return seq


def streamlines(img, n_seeds: int = DEFAULT_SEEDS, seed: int = 1):
    """The desktop's streamlines of ``img`` (makeStreamActor, without the tube):
    ``(points, speed, offsets)``."""
    import vtk   # noqa: PLC0415
    from vtk.util.numpy_support import vtk_to_numpy   # noqa: PLC0415
    b = img.GetBounds()
    ext = max(b[1] - b[0], b[3] - b[2], b[5] - b[4])
    seeds = vtk.vtkPointSource()
    seeds.SetCenter(0.5 * (b[0] + b[1]), 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5]))
    seeds.SetRadius(0.3 * ext)
    seeds.SetNumberOfPoints(int(n_seeds))
    seeds.SetRandomSequence(_sequence(seed))
    tracer = vtk.vtkStreamTracer()
    tracer.SetInputData(img)
    tracer.SetSourceConnection(seeds.GetOutputPort())
    tracer.SetIntegratorTypeToRungeKutta45()
    tracer.SetIntegrationDirectionToBoth()
    tracer.SetMaximumPropagation(2.0 * ext)
    tracer.SetMaximumNumberOfSteps(2000)
    tracer.SetComputeVorticity(False)
    tracer.Update()
    out = tracer.GetOutput()
    n = out.GetNumberOfPoints()
    if n == 0 or out.GetNumberOfCells() == 0:
        return (np.zeros((0, 3), np.float32), np.zeros(0, np.float32),
                np.zeros(1, np.uint32))
    pts = vtk_to_numpy(out.GetPoints().GetData()).astype(np.float32)
    sp = out.GetPointData().GetArray("speed")
    speed = (vtk_to_numpy(sp).astype(np.float32) if sp is not None
             else np.zeros(n, np.float32))
    lines = out.GetLines()
    offsets = vtk_to_numpy(lines.GetOffsetsArray()).astype(np.int64)
    conn = vtk_to_numpy(lines.GetConnectivityArray()).astype(np.int64)
    # The lines' points in line order (VTK's tracer writes them so already;
    # gathered through the connectivity so it never depends on it).
    return pts[conn], speed[conn], offsets.astype(np.uint32)


def arrows(img, max_arrows: int = DEFAULT_ARROWS, seed: int = 1):
    """The desktop's arrow points (makeGlyphActor's vtkMaskPoints):
    ``(positions, vectors)``."""
    import vtk   # noqa: PLC0415
    from vtk.util.numpy_support import vtk_to_numpy   # noqa: PLC0415
    mask = vtk.vtkMaskPoints()
    mask.SetInputData(img)
    mask.RandomModeOn()
    mask.SetRandomModeType(1)
    mask.SetRandomSeed(int(seed))
    mask.SetMaximumNumberOfPoints(int(max_arrows))
    mask.Update()
    out = mask.GetOutput()
    if out.GetNumberOfPoints() == 0:
        return np.zeros((0, 3), np.float32), np.zeros((0, 3), np.float32)
    pos = vtk_to_numpy(out.GetPoints().GetData()).astype(np.float32)
    vec = vtk_to_numpy(out.GetPointData().GetArray("velocity")).astype(np.float32)
    return pos, vec


def flow_lines_worker(path: str, box_mpc: float | None, max_dim: int = 128,
                      n_seeds: int = DEFAULT_SEEDS, max_arrows: int = DEFAULT_ARROWS,
                      seed: int = 1, want_lines: bool = True, want_arrows: bool = True
                      ) -> dict:
    """Picklable worker: the display grid, its streamlines and its arrows."""
    from .velocity_field import velocity_grid_worker   # noqa: PLC0415
    payload = velocity_grid_worker(path, box_mpc, max_dim)
    img = grid_image(payload)
    if want_lines:
        pts, speed, offsets = streamlines(img, max(1, min(int(n_seeds), MAX_SEEDS)), seed)
    else:
        pts, speed, offsets = (np.zeros((0, 3), np.float32), np.zeros(0, np.float32),
                               np.zeros(1, np.uint32))
    if want_arrows:
        apos, avec = arrows(img, max(1, min(int(max_arrows), MAX_ARROWS)), seed)
    else:
        apos, avec = np.zeros((0, 3), np.float32), np.zeros((0, 3), np.float32)
    spacing = [float(s) for s in payload["spacing"]]
    grid = {
        "shape": [int(payload["nx"]), int(payload["ny"]), int(payload["nz"])],
        "full_shape": list(payload.get("full_shape", [])),      # nz, ny, nx
        "downsample_factor": payload.get("downsample_factor", 1),
        "downsample_factors": payload.get("downsample_factors"),
        "spacing": spacing, "origin": [float(o) for o in payload["origin"]],
        "bounds": list(img.GetBounds()),
        "cell": float(min(spacing)),
        "speed_min": float(payload.get("full_speed_min", payload["speed_min"])),
        "speed_max": float(payload.get("full_speed_max", payload["speed_max"])),
        "unit": str(payload.get("unit", "")), "frame": str(payload.get("frame", "")),
        "n_seeds": int(n_seeds) if want_lines else 0, "seed": int(seed),
        "box_mpc": float(box_mpc) if box_mpc else 0.0,
    }
    lines = FlowLines(pts, speed, offsets, apos, avec, grid)
    return {"bytes": lines.encode(), "points": len(pts), "lines": max(0, len(offsets) - 1),
            "arrows": len(apos), "grid": grid}


def decode(result: dict):
    """The arrays of a worker's result (the page's linesArrays, for tests)."""
    buf, o = result["bytes"], 0
    out = []
    for dtype, count in (("<f4", 3 * result["points"]), ("<f4", result["points"]),
                         ("<u4", result["lines"] + 1), ("<f4", 3 * result["arrows"]),
                         ("<f4", 3 * result["arrows"])):
        a = np.frombuffer(buf, dtype=dtype, count=count, offset=o)
        o += a.nbytes
        out.append(a)
    pts, speed, offsets, apos, avec = out
    return pts.reshape(-1, 3), speed, offsets, apos.reshape(-1, 3), avec.reshape(-1, 3)


def headers(result: dict) -> dict[str, str]:
    g = result["grid"]
    h = {
        "X-Visivo-Line-Points": str(result["points"]),
        "X-Visivo-Lines": str(result["lines"]),
        "X-Visivo-Arrows": str(result["arrows"]),
        "X-Visivo-Width": str(g["shape"][0]), "X-Visivo-Height": str(g["shape"][1]),
        "X-Visivo-Depth": str(g["shape"][2]),
        "X-Visivo-Full-Shape": ",".join(str(int(s)) for s in g["full_shape"]),
        "X-Visivo-Downsample-Factor": str(g["downsample_factor"]),
        "X-Visivo-Spacing": ",".join(repr(v) for v in g["spacing"]),
        "X-Visivo-Origin": ",".join(repr(v) for v in g["origin"]),
        "X-Visivo-Bounds": ",".join(repr(float(v)) for v in g["bounds"]),
        "X-Visivo-Glyph-Cell": repr(g["cell"]),
        "X-Visivo-Full-Speed-Min": repr(g["speed_min"]),
        "X-Visivo-Full-Speed-Max": repr(g["speed_max"]),
        "X-Visivo-Seeds": str(g["n_seeds"]), "X-Visivo-Seed": str(g["seed"]),
        # The box the lines were traced with (Mpc); 0: grid units.
        "X-Visivo-Box-Mpc": repr(g.get("box_mpc", 0.0)),
        "X-Visivo-Unit": g["unit"].encode("latin-1", "replace").decode("latin-1"),
        "X-Visivo-Frame": g["frame"].encode("latin-1", "replace").decode("latin-1"),
        "X-Visivo-Byte-Order": "little",
    }
    if g.get("downsample_factors"):
        h["X-Visivo-Downsample-Factors"] = ",".join(str(int(f)) for f in g["downsample_factors"])
    return h
