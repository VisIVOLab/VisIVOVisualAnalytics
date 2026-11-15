"""Golden values for tests/test_cube_lod_geometry.cpp, from the backend's code.

The client's level-of-detail geometry (src/vtk/CubeLodGeometry.h) ports two
backend functions; this script records what the originals return so the port
is checked against them, not against a second reading of the same formula:

- brick_pyramid.level_shapes: the shapes of the pyramid's levels;
- render_session._ooc._vtk_crop_planes: the cropping planes of a block,
  float32 rounding included.

Run from the repository root with the backend's environment:

    backend/.venv/bin/python tests/data/cube_lod_golden.py > tests/data/cube_lod_golden.json
"""
import json
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "backend"))

from app.brick_pyramid import level_shapes  # noqa: E402
from app.render_session._ooc import _vtk_crop_planes  # noqa: E402


def shapes_cases():
    out = []
    for shape, brick in [((1, 1, 1), 256), ((7, 13, 1000), 256), ((257, 255, 513), 256),
                         ((2048, 2048, 2048), 256), ((3, 1025, 9), 128), ((100, 200, 300), 64),
                         ((1, 4097, 2), 256), ((999, 777, 555), 32)]:
        # level_shapes takes (z, y, x); the client works in (x, y, z).
        zyx = tuple(reversed(shape))
        out.append({"shape": list(shape), "brick": brick,
                    "levels": [list(reversed(s)) for s in level_shapes(zyx, brick)]})
    return out


def crop_cases():
    rng = random.Random(321)
    out = []
    fixed = [
        # A level-0 block of 11 samples owning [0, 10].
        ((0.0, 10.0, 0.0, 10.0, 0.0, 10.0), (0.0, 0.0, 0.0), 1.0, (11, 11, 11)),
        # One sample along an axis: the plane is the origin.
        ((2.0, 2.0, -0.5, 3.5, 0.0, 1.0), (2.0, -1.0, 0.0), 1.0, (1, 6, 3)),
        # A level-3 block with its voxel offset, in the preview frame of stride 8.
        ((15.9375, 47.9375, 0.0, 31.9375, 7.9375, 39.9375), (15.0, -1.0, 7.0), 1.0, (34, 34, 34)),
        # Large coordinates, where 1e-3 is below float32's resolution.
        ((1.0e6, 1.0e6 + 256.0, 4.0e6, 4.0e6 + 512.0, 2.0e7, 2.0e7 + 64.0),
         (1.0e6 - 1.0, 4.0e6 - 2.0, 2.0e7 - 1.0), 1.0, (258, 516, 66)),
    ]
    for crop, origin, spacing, dims in fixed:
        out.append({"crop": list(crop), "origin": list(origin), "spacing": spacing,
                    "dims": list(dims), "planes": list(_vtk_crop_planes(crop, origin, spacing, dims))})
    for _ in range(200):
        spacing = rng.choice([1.0, 0.5, 0.25, 0.125, 2.0, 4.0, 1.0 / 3.0, 2.0 / 3.0, 0.0625])
        dims = [rng.choice([1, 2, 3, rng.randint(4, 600)]) for _ in range(3)]
        scale = rng.choice([1.0, 100.0, 1.0e4, 3.0e6])
        origin = [rng.uniform(-scale, scale) for _ in range(3)]
        crop = []
        for axis in range(3):
            extent = (dims[axis] - 1) * spacing
            a = origin[axis] + rng.uniform(0.0, max(extent, 0.0))
            b = rng.uniform(a, origin[axis] + max(extent, 0.0)) if extent > 0 else a
            crop += [a, b]
        out.append({"crop": crop, "origin": origin, "spacing": spacing, "dims": dims,
                    "planes": list(_vtk_crop_planes(tuple(crop), tuple(origin), spacing, tuple(dims)))})
    return out


# One case per line: diffs stay readable when the backend changes.
for i, (key, cases) in enumerate((("level_shapes", shapes_cases()), ("crop_planes", crop_cases()))):
    sys.stdout.write(("{" if i == 0 else ",\n") + json.dumps(key) + ": [\n")
    sys.stdout.write(",\n".join(json.dumps(c, separators=(",", ":")) for c in cases))
    sys.stdout.write("\n]")
sys.stdout.write("}\n")
