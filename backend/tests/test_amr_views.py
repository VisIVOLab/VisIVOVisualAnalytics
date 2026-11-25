"""Views of an AMR hierarchy (issue #550): the gas cells as points, a uniform
cube at a level, the refinement-level map of a slice, the finest level of a
box.

The cube is checked against a brute-force resampling written from the
definition (a voxel takes the leaf that covers it, or the volume-weighted mean
of the finer leaves inside it) on synthetic hierarchies of two and three
levels and on a RAMSES output written as RAMSES writes one; the mass is
checked to be conserved at every level, and the cells as points to give the
same mass; the real output_00185 is checked when it is at hand."""
from __future__ import annotations

import math
import os
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import ramses_output as ro
from app.amr_cells import AmrCells
from app.amr_source import AmrHierarchy, LeafBlock
from app.compute import amr_views as av

from . import _ramses_writer as W

REAL = Path("/Users/fvitello/Downloads/DemoData/Ramses/output_00185")


# ── A hierarchy in memory, and the definition written out ────────────────────

class MemoryAmr(AmrHierarchy):
    """Leaves given as ``(level, centre, {field: value})``; the units are
    invented (100 length units per box, 2 per code density)."""
    code = "memory"

    def __init__(self, leaves, fields=("density",), extent=(1.0, 1.0, 1.0)):
        self.leaves, self._fields, self._extent = list(leaves), tuple(fields), extent

    extent = property(lambda self: self._extent)
    fields = property(lambda self: self._fields)
    levels = property(lambda self: tuple(sorted(self.leaf_counts())))

    def leaf_counts(self):
        out = {}
        for lv, _, _ in self.leaves:
            out[lv] = out.get(lv, 0) + 1
        return out

    def leaf_blocks(self, fields=(), levels=None, lo=None, hi=None, max_block=1 << 20):
        bad = [f for f in fields if f not in self._fields]
        if bad:
            raise ValueError(f"Unknown field: {bad[0]!r}")
        for lv in sorted(self.leaf_counts()):
            if levels is not None and lv not in levels:
                continue
            rows = [r for r in self.leaves if r[0] == lv]
            centre = np.array([r[1] for r in rows], dtype=np.float64)
            keep = ro._inside(centre, lo, hi)
            if not keep.any():
                continue
            vals = {f: np.array([r[2][f] for r in rows])[keep] for f in fields}
            yield LeafBlock(lv, 2.0 ** -lv, centre[keep], vals)

    def scales(self):
        return {"x": {"factor": 100.0, "unit": "kpc"}, "y": {"factor": 100.0, "unit": "kpc"},
                "z": {"factor": 100.0, "unit": "kpc"}, "dx": {"factor": 100.0, "unit": "kpc"},
                "density": {"factor": 2.0, "unit": "g/cm^3"},
                "mass": {"factor": 2.0e6, "unit": "Msun"}}

    def describe(self):
        return {"code": "memory", "scale_factor": 0.5}


def _mesh(refine, base=1):
    """A mesh of ``2**base`` cells a side, the cells ``(level, index)`` of
    ``refine`` split into eight, to any depth; leaf values are a function of
    where the cell is, so every leaf is distinct."""
    leaves = []

    def add(level, idx):
        if (level, idx) in refine:
            for o in range(8):
                add(level + 1, tuple(2 * idx[d] + ((o >> d) & 1) for d in range(3)))
            return
        dx = 2.0 ** -level
        c = tuple((idx[d] + 0.5) * dx for d in range(3))
        leaves.append((level, c, {"density": 1.0 + 10 * c[0] + 100 * c[1] + 1000 * c[2] + level}))

    n = 2 ** base
    for i in range(n):
        for j in range(n):
            for k in range(n):
                add(base, (i, j, k))
    return leaves


#: Three levels: the cell (0,0,0) of level 1 is split, and in it the cell
#: (0,0,0) and (1,1,1) of level 2.
LEAVES = _mesh({(1, (0, 0, 0)), (2, (0, 0, 0)), (2, (1, 1, 1))})


def brute(leaves, level, name="density", mode="mean"):
    """The definition, voxel by voxel: ``(n, n, n)`` array [z, y, x] over the
    unit box at ``level``."""
    n = 2 ** level
    dxl = 1.0 / n
    out = np.full((n, n, n), np.nan)
    for k in range(n):
        for j in range(n):
            for i in range(n):
                lo = np.array([i, j, k]) * dxl
                hi = lo + dxl
                covering, inside = None, []
                for lv, c, vals in leaves:
                    h = 0.5 * 2.0 ** -lv
                    c = np.asarray(c)
                    if lv <= level and (c - h <= lo + 1e-12).all() and (c + h >= hi - 1e-12).all():
                        covering = (lv, vals)
                    elif lv > level and (c >= lo).all() and (c <= hi).all():
                        inside.append((lv, vals))
                if covering is not None:
                    out[k, j, i] = covering[0] if mode == "max" else covering[1][name]
                elif inside:
                    if mode == "max":
                        out[k, j, i] = max(lv for lv, _ in inside)
                    else:
                        out[k, j, i] = sum(v[name] * (2.0 ** -lv) ** 3
                                           for lv, v in inside) / dxl ** 3
    return out


def run_cube(h, field, level, **kw):
    plan = av.plan_uniform(h, field, level, **kw)
    slabs = []
    av.resample(h, plan, slabs.append)
    return plan, np.concatenate(slabs, axis=0)


# ── The cube ─────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("level", [1, 2, 3])
def test_a_cube_is_the_definition_at_every_level(level):
    h = MemoryAmr(LEAVES)
    plan, cube = run_cube(h, "density", level)
    assert cube.shape == (2 ** level,) * 3 and plan.shape == (2 ** level,) * 3
    # Code units: the cube holds the code density, the physical factor is the run's.
    np.testing.assert_allclose(cube, brute(LEAVES, level), rtol=1e-6)


def test_a_cube_coarser_than_the_mesh_averages_and_below_the_root_too():
    h = MemoryAmr(LEAVES)
    _, cube = run_cube(h, "density", 0)       # one voxel: the volume-weighted mean
    total = sum(v["density"] * (2.0 ** -lv) ** 3 for lv, _, v in LEAVES)
    assert cube.shape == (1, 1, 1) and float(cube[0, 0, 0]) == pytest.approx(total, rel=1e-6)


@pytest.mark.parametrize("level", [0, 1, 2, 3])
def test_the_mass_is_conserved_at_every_level(level):
    h = MemoryAmr(LEAVES)
    plan, mass = run_cube(h, "mass", level)
    want = sum(v["density"] * (2.0 ** -lv) ** 3 for lv, _, v in LEAVES)
    assert float(mass.astype(np.float64).sum()) == pytest.approx(want, rel=1e-6)
    # A voxel's mass is its density times its volume (code units; the mass factor is
    # the run's, applied by the units table).
    _, dens = run_cube(h, "density", level)
    np.testing.assert_allclose(mass, dens * plan.dx ** 3, rtol=1e-5)


def test_the_level_field_is_the_finest_leaf_in_a_voxel():
    h = MemoryAmr(LEAVES)
    for level in (1, 2, 3):
        _, cube = run_cube(h, "level", level)
        np.testing.assert_array_equal(cube, brute(LEAVES, level, mode="max"))
    _, one = run_cube(h, "level", 0)
    assert float(one[0, 0, 0]) == 3.0


def test_a_region_is_snapped_outward_to_the_lattice_and_matches_the_whole_cube():
    h = MemoryAmr(LEAVES)
    _, whole = run_cube(h, "density", 3)
    plan, part = run_cube(h, "density", 3, lo=[0.1, 0.0, 0.3], hi=[0.55, 0.3, 0.9])
    # 0.1 -> voxel 0 (of 0.125), 0.55 -> 5 (to 0.625); y: 0 .. 0.3 -> 3; z: 0.3 -> 2 .. 0.9 -> 8.
    assert plan.i0 == (0, 0, 2) and plan.shape == (5, 3, 6)
    np.testing.assert_array_equal(part, whole[2:8, 0:3, 0:5])
    # The same in physical units (100 per box), counted from the corner.
    plan2 = av.plan_uniform(h, "density", 3, lo=[10, 0, 30], hi=[55, 30, 90], unit="physical")
    assert (plan2.i0, plan2.shape) == (plan.i0, plan.shape)


def test_slabs_and_filled_blocks_give_the_same_cube(monkeypatch):
    h = MemoryAmr(LEAVES)
    _, one = run_cube(h, "density", 3)
    monkeypatch.setattr(av, "SLAB_VOXELS", 64)         # 8x8 planes: a plane per slab or so
    plan = av.plan_uniform(h, "density", 3)
    assert plan.slab_planes == 1 and plan.passes == 8
    _, slabs = run_cube(h, "density", 3)
    np.testing.assert_array_equal(slabs, one)
    # A coarse leaf covering more than _VECTOR_FILL voxels a side is filled one by one.
    monkeypatch.setattr(av, "_VECTOR_FILL", 1)
    _, per_leaf = run_cube(h, "density", 3)
    np.testing.assert_array_equal(per_leaf, one)


def test_a_hole_in_the_mesh_is_nan():
    h = MemoryAmr([l for l in LEAVES if l[1][0] > 0.6 or l[0] > 1])    # the x > 0.5 half only, partly
    _, cube = run_cube(h, "density", 2)
    assert np.isnan(cube).any() and np.isfinite(cube).any()
    np.testing.assert_allclose(cube, brute(h.leaves, 2), rtol=1e-6)


# ── Weighting: velocities and scalars are mass-weighted ─────────────────────

#: Level 1 with the cell (0,0,0) split once: a dense, slow fine cell sits beside
#: light, fast ones, so a volume mean and a mass-weighted mean differ a lot.
def _gas(refine):
    out = []
    for lv, c, v in _mesh(refine):
        rho = 1.0 + 10 * c[0] + 100 * c[1] + 1000 * c[2] + lv
        out.append((lv, c, {"density": rho, "vx": 3.0 - 7.0 * c[2] * (1 + lv) - c[0],
                            "metallicity": 0.02 * (1 + c[1])}))
    return out


GAS = _gas({(1, (0, 0, 0)), (2, (0, 0, 0))})
GAS_FIELDS = ("density", "vx", "metallicity")


def brute_density_weighted(leaves, level, name):
    """The mass-weighted mean over a voxel's finer leaves, a coarser leaf's own value."""
    n = 2 ** level
    dxl = 1.0 / n
    out = np.full((n, n, n), np.nan)
    for k in range(n):
        for j in range(n):
            for i in range(n):
                lo = np.array([i, j, k]) * dxl
                num = den = 0.0
                for lv, c, vals in leaves:
                    c, half = np.asarray(c), 0.5 * 2.0 ** -lv
                    if lv <= level and (c - half <= lo + 1e-12).all() and (c + half >= lo + dxl - 1e-12).all():
                        num, den = vals[name], 1.0
                        break
                    if lv > level and (c >= lo).all() and (c <= lo + dxl).all():
                        m = vals["density"] * (2.0 ** -lv) ** 3
                        num, den = num + m * vals[name], den + m
                out[k, j, i] = num / den if den else np.nan
    return out


def test_the_weighting_of_each_field():
    h = MemoryAmr(GAS, GAS_FIELDS)
    assert [av.weighting(h, f) for f in ("density", "mass", "vx", "metallicity", "level")] == [
        "volume", "volume", "density", "density", "finest"]
    assert av.weighting(MemoryAmr(LEAVES), "density") == "volume"
    for f in ("vy", "vz", "velocity_x", "scalar_2", "scalar_17"):
        assert av.weighting(MemoryAmr(GAS, ("density", f)), f) == "density"
    # A pressure is volume-weighted; with no density there is nothing to weigh by.
    assert av.weighting(MemoryAmr(GAS, ("density", "pressure")), "pressure") == "volume"
    assert av.weighting(MemoryAmr(GAS, ("vx",)), "vx") == "volume"


@pytest.mark.parametrize("field", ["vx", "metallicity"])
@pytest.mark.parametrize("level", [0, 1, 2, 3])
def test_a_velocity_is_the_mass_weighted_mean_of_the_finer_leaves(field, level):
    h = MemoryAmr(GAS, GAS_FIELDS)
    plan, cube = run_cube(h, field, level)
    assert plan.weighting == "density"
    np.testing.assert_allclose(cube, brute_density_weighted(GAS, level, field), rtol=1e-6)


def test_the_dense_fine_cell_sets_the_velocity_not_the_light_coarse_one():
    # One voxel at level 0: a mass-weighted vx is the momentum over the mass.
    h = MemoryAmr(GAS, GAS_FIELDS)
    _, cube = run_cube(h, "vx", 0)
    mass = sum(v["density"] * 8.0 ** -lv for lv, _, v in GAS)
    mom = sum(v["density"] * v["vx"] * 8.0 ** -lv for lv, _, v in GAS)
    assert float(cube[0, 0, 0]) == pytest.approx(mom / mass, rel=1e-6)
    vol = sum(v["vx"] * 8.0 ** -lv for lv, _, v in GAS)
    assert abs(float(cube[0, 0, 0]) - vol) > 1e-3            # not the volume mean


@pytest.mark.parametrize("level", [0, 1, 2, 3])
def test_the_momentum_of_the_cube_is_that_of_the_cells(level):
    h = MemoryAmr(GAS, GAS_FIELDS)
    plan, vx = run_cube(h, "vx", level)
    _, rho = run_cube(h, "density", level)
    cells = sum(v["density"] * v["vx"] * 8.0 ** -lv for lv, _, v in GAS)
    scale = sum(v["density"] * abs(v["vx"]) * 8.0 ** -lv for lv, _, v in GAS)
    cube = float((vx.astype(np.float64) * rho).sum()) * plan.dx ** 3
    assert abs(cube - cells) / scale < 1e-6


def test_a_weighted_cube_is_the_same_in_slabs_and_a_massless_voxel_is_nan(monkeypatch):
    h = MemoryAmr(GAS, GAS_FIELDS)
    _, one = run_cube(h, "vx", 3)
    monkeypatch.setattr(av, "SLAB_VOXELS", 64)
    monkeypatch.setattr(av, "_VECTOR_FILL", 1)
    _, slabs = run_cube(h, "vx", 3)
    np.testing.assert_array_equal(slabs, one)
    empty = MemoryAmr([(lv, c, {**v, "density": 0.0 if lv == 3 else v["density"]})
                       for lv, c, v in GAS], GAS_FIELDS)
    _, cube = run_cube(empty, "vx", 2)
    assert np.isnan(cube[0, 0, 0]) and np.isfinite(cube[1:, 1:, 1:]).all()


def test_the_header_and_plan_state_the_weighting():
    h = MemoryAmr(GAS, GAS_FIELDS)
    plan, answer = av.plan_view(h, "vx", 2)
    assert answer["weighting"] == "density" and plan.slab_bytes == plan.slab_planes * 16 * 17
    hdr = av.build_header(h, plan, "x")
    assert hdr["AMRWGT"] == "density" and hdr["AMRMODE"] == "mean"
    assert hdr["BUNIT"] == "code velocity" and hdr["AMRUNIT"] == "code units"
    assert av.build_header(h, av.plan_uniform(h, "mass", 2), "x")["AMRWGT"] == "volume"
    assert av.build_header(h, av.plan_uniform(h, "level", 2), "x")["AMRWGT"] == "finest"


# ── The plan ─────────────────────────────────────────────────────────────────

def test_the_plan_states_the_cost_and_the_limit(monkeypatch):
    h = MemoryAmr(LEAVES)
    plan, answer = av.plan_view(h, "density", None, max_voxels=100)
    assert plan.level == 2 and answer["voxels"] == 64 and answer["fits"]      # 8^3 = 512 > 100
    plan, answer = av.plan_view(h, "density", 3, max_voxels=100)
    assert not answer["fits"] and answer["max_level_within_limit"] == 2
    assert "level 2 fits" in answer["reason"]
    assert answer["bytes_estimate"] == 512 * 4 and answer["passes"] == 1
    assert answer["voxel_size"] == 0.125 and answer["voxel_size_physical"] == 12.5
    assert answer["length_unit"] == "kpc" and answer["value_unit"] == "g/cm^3"
    assert answer["fields"] == ["density", "mass", "level"] and answer["levels"] == [1, 2, 3]
    with pytest.raises(av.AmrViewError, match="above|outside"):
        av.plan_uniform(h, "density", 4)
    with pytest.raises(av.AmrViewError, match="no field"):
        av.plan_uniform(h, "temperature", 2)
    with pytest.raises(av.AmrViewError, match="empty"):
        av.plan_uniform(h, "density", 2, lo=[0.2, 0.2, 0.2], hi=[0.2, 0.9, 0.9])
    monkeypatch.setattr(av, "HARD_MAX_VOXELS", 100)
    with pytest.raises(av.AmrViewError, match="more than"):
        av.plan_uniform(h, "density", 3)


def test_the_finest_level_in_a_box_counts_a_coarse_cell_that_reaches_in():
    h = MemoryAmr(LEAVES)
    # Level 3 cells all lie in [0, 0.25]^3.
    assert h.finest_level_in() == 3
    assert h.finest_level_in([0.0] * 3, [0.2] * 3) == 3
    assert h.finest_level_in([0.3] * 3, [0.9] * 3) == 3       # the split (1,1,1) level-2 cell
    assert h.finest_level_in([0.3, 0.0, 0.0], [0.4, 0.1, 0.1]) == 2   # a level-2 leaf only
    assert h.finest_level_in([0.55] * 3, [0.9] * 3) == 1      # only level-1 cells there
    assert h.finest_level_in([0.9, 0.0, 0.0], [0.95, 0.1, 0.1]) == 1
    # A box inside one coarse cell sees that cell although no centre lies in it.
    assert h.finest_level_in([0.7, 0.7, 0.7], [0.71, 0.71, 0.71]) == 1


def test_a_region_without_a_level_refines_at_the_finest_it_holds():
    h = MemoryAmr(LEAVES)
    plan, answer = av.plan_view(h, "density", None, [0.6, 0.6, 0.6], [0.9, 0.9, 0.9])
    assert answer["finest_level_in_region"] == 1 and plan.level == 1
    plan, answer = av.plan_view(h, "density", None, [0.0] * 3, [0.2] * 3)
    assert answer["finest_level_in_region"] == 3 and plan.level == 3
    assert plan.shape == (2, 2, 2)


def test_the_header_records_how_the_cube_was_made():
    h = MemoryAmr(LEAVES)
    plan = av.plan_uniform(h, "density", 2, lo=[0.25, 0.0, 0.0], hi=[0.75, 0.5, 0.5])
    hdr = av.build_header(h, plan, "somewhere/output_00001")
    assert (hdr["NAXIS1"], hdr["NAXIS2"], hdr["NAXIS3"]) == plan.shape == (2, 2, 2)
    # Code units: the axes are box units, the values the code density.
    assert hdr["CUNIT1"] == "box" and hdr["BUNIT"] == "code density"
    assert hdr["CDELT1"] == pytest.approx(0.25) and hdr["CRVAL1"] == pytest.approx(0.375)
    assert hdr["CRVAL2"] == pytest.approx(0.125)
    assert hdr["AMRUNIT"] == "g/cm^3" and hdr["AMRWGT"] == "volume" and hdr["AMRMODE"] == "mean"
    assert (hdr["AMRLEV"], hdr["AMRFLD"], hdr["AMRCODE"]) == (2, "density", "memory")
    assert (hdr["AMRLO1"], hdr["AMRHI1"]) == (0.25, 0.75) and hdr["AMRFCT"] == 2.0
    assert hdr["AMRSRC"] == "somewhere/output_00001" and hdr["AEXP"] == 0.5
    mass = av.build_header(h, av.plan_uniform(h, "mass", 2), "x")
    assert mass["BUNIT"] == "code density x box volume" and mass["AMRUNIT"] == "Msun"
    assert mass["AMRFCT"] == pytest.approx(2.0e6)


# ── The level map ────────────────────────────────────────────────────────────

def test_a_level_map_tiles_the_plane_and_agrees_with_the_hierarchy():
    h = MemoryAmr(LEAVES)
    # Every plane through a cell's middle: the rectangles tile the box face.
    for axis in "xyz":
        for c in (0.05, 0.2, 0.3, 0.6):
            m = av.level_map(h, axis, c)
            assert m["area"] == pytest.approx(1.0) and m["window_area"] == 1.0
            assert sum(m["counts"].values()) == m["cells"]
            for level, rects in m["levels"].items():
                side = 2.0 ** -int(level)
                for u0, v0, u1, v1 in rects:
                    assert (u1 - u0) == pytest.approx(side) and (v1 - v0) == pytest.approx(side)
    # Sweeping the planes of a level gives every leaf of it exactly once.
    for level, n in h.leaf_counts().items():
        dx = 2.0 ** -level
        total = sum(av.level_map(h, "z", (i + 0.5) * dx)["counts"].get(str(level), 0)
                    for i in range(2 ** level))
        assert total == n, level


def test_a_plane_on_a_cell_boundary_belongs_to_the_upper_cell_and_a_window_clips_the_count():
    h = MemoryAmr(LEAVES)
    on_edge = av.level_map(h, "z", 0.5)
    assert on_edge["counts"] == {"1": 4}                  # the upper layer of level-1 cells
    m = av.level_map(h, "z", 0.1, lo=[0.0, 0.0, 0.0], hi=[0.3, 0.3, 0.3])
    assert m["window_area"] == pytest.approx(0.09) and m["area"] == pytest.approx(0.09)
    phys = av.level_map(h, "z", 10.0, unit="physical", lo=[0, 0, 0], hi=[30, 30, 30],
                        region_unit="physical")
    assert phys["levels"]["3"][0] == pytest.approx([0.0, 0.0, 12.5, 12.5])
    assert phys["to_physical"] == 100.0 and phys["plane_axes"] == ["x", "y"]
    cut = av.level_map(h, "z", 0.1, max_rects=3)
    assert cut["truncated"] and sum(len(r) for r in cut["levels"].values()) == 3
    assert min(map(int, cut["levels"])) == 1              # the coarsest are kept
    with pytest.raises(av.AmrViewError, match="outside"):
        av.level_map(h, "z", 1.5)
    with pytest.raises(av.AmrViewError, match="x, y or z"):
        av.level_map(h, "w", 0.5)


# ── Cells as points ──────────────────────────────────────────────────────────

def test_cells_as_points_are_the_leaves(tmp_path):
    directory, exp = W.write_output(tmp_path, particles=None)
    out = ro.open_output(directory)
    cells = out.cells()
    assert isinstance(cells, AmrCells) and cells.num_rows == 22
    assert cells.names == ["x", "y", "z", "dx", "level", "mass", "density", "vx", "vy", "vz",
                           "pressure", "metallicity"]
    got = {(int(cells.take("level", r)),
            tuple(float(cells.take(c, r)) for c in "xyz")):
           [float(cells.take(f, r)) for f in ("density", "vx", "vy", "vz", "pressure",
                                               "metallicity")]
           for r in range(cells.num_rows)}
    assert got == {(lv, c): v for lv, c, v in exp.leaves}
    rows = np.arange(cells.num_rows)
    np.testing.assert_array_equal(cells.take("dx", rows), 2.0 ** -cells.take("level", rows))
    np.testing.assert_allclose(cells.take("mass", rows),
                               cells.take("density", rows) * cells.take("dx", rows) ** 3)
    assert cells.column_dtype("level") == np.int16 and cells.column_dtype("x") == np.float64
    assert cells.periods() == {"x": 1.0, "y": 1.0, "z": 1.0}
    assert cells.size_column == "dx" and cells.describe()["size_column"] == "dx"
    with pytest.raises(IndexError):
        cells.take("x", [22])
    with pytest.raises(KeyError):
        cells.take("temperature", [0])


def test_the_cells_open_through_the_particle_seam(tmp_path):
    from app import physical_units
    from app.classify import classify_path
    from app.point_table import (component_format, has_component, open_point_table,
                                 particle_catalogue_metadata, particle_components,
                                 particle_dataset_path, particle_rows, resolve_component)
    directory, _ = W.write_output(tmp_path, particles="descriptor")
    ref = ro.ramses_ref(directory, "cells")
    assert ro.split_ref(ref) == (str(directory), "cells") and component_format(ref) == "ramses"
    assert [c["name"] for c in particle_components(ro.ramses_ref(directory, "dm"))] == [
        "dark matter", "stars", "sinks", "gas cells"]
    assert particle_dataset_path(ref) == (ref, "ramses")
    assert has_component(str(directory), "::ramses-family=cells")
    table = open_point_table(ref)
    assert table.num_rows == 22 and table.axes == ("x", "y", "z") and "dx" in table.columns
    rows = particle_rows(ref, [0, 5], ["x", "mass", "density"])
    assert list(rows.columns) == ["x", "mass", "density"]
    meta = particle_catalogue_metadata(ref)
    assert meta["format"] == "ramses" and meta["num_rows"] == 22
    assert {f["name"] for f in meta["fields"]} >= {"dx", "level", "mass", "density"}
    assert meta["component"]["size_column"] == "dx"
    # Physical units through physical_units.scales(): a length is boxlen * unit_l / kpc.
    s = physical_units.scales(ref)
    expect_x = W.BOXLEN * W.UNIT_L / 3.085678e21
    assert s["x"]["unit"] == "kpc" and s["x"]["factor"] == pytest.approx(expect_x)
    assert s["dx"]["factor"] == pytest.approx(expect_x)
    assert s["density"]["unit"] == "g/cm^3" and s["density"]["factor"] == pytest.approx(W.UNIT_D)
    # A cell's mass is density * dx^3 in box units: the box length cubed is in its factor.
    assert s["mass"]["unit"] == "Msun" and s["mass"]["factor"] == pytest.approx(
        W.UNIT_D * (W.BOXLEN * W.UNIT_L) ** 3 / 1.989e33)
    # The classifier offers the cells next to the families; a mesh-only output opens as cells.
    assert classify_path(directory)["components"][-1]["path"] == ref
    only, _ = W.write_output(tmp_path / "only", particles=None)
    r = classify_path(only)
    assert (r["kind"], r["opener"], r["open_path"]) == ("particles", "catalogue",
                                                         ro.ramses_ref(only, "cells"))
    assert [c["name"] for c in r["components"]] == ["gas cells"]
    # A mesh with no hydro files has no cells.
    for f in only.glob("hydro_*"):
        f.unlink()
    assert classify_path(only)["opener"] == "unsupported"
    assert not has_component(str(only), "::ramses-family=cells")
    with pytest.raises(ValueError):
        resolve_component(ro.ramses_ref(only, "cells"))
    # A named family a mesh-only output lacks is refused, not answered with the cells.
    meshonly, _ = W.write_output(tmp_path / "m2", particles=None)
    with pytest.raises(ValueError, match="no 'stars' particles"):
        particle_dataset_path(ro.ramses_ref(meshonly, "stars"))
    assert av.build_header(MemoryAmr(LEAVES), av.plan_uniform(MemoryAmr(LEAVES), "density", 1),
                           "/Users/Jos\u00e9/run")["AMRSRC"] == "/Users/Jos?/run"


def test_a_rewritten_mesh_file_is_not_served_from_the_cache(tmp_path):
    from app.product_cache import file_identity
    directory, _ = W.write_output(tmp_path, particles=None)
    ref = ro.ramses_ref(directory, "cells")
    before = file_identity(ref)
    assert before
    hydro = directory / "hydro_00001.out00001"
    hydro.write_bytes(hydro.read_bytes())
    os.utime(hydro, ns=(1, 1))
    assert file_identity(ref) != before


def test_the_cells_total_mass_matches_the_cube(tmp_path):
    directory, exp = W.write_output(tmp_path, particles=None)
    out = ro.open_output(directory)
    h, cells = out.amr(), out.cells()
    from app import physical_units
    mass_points = float(np.sum(cells.column("mass"))) * physical_units.scales(
        ro.ramses_ref(directory, "cells"))["mass"]["factor"]
    factor = physical_units.scales(ro.ramses_ref(directory, "cells"))["mass"]["factor"]
    for level in (1, 2):
        _, cube = run_cube(h, "mass", level)
        assert float(cube.astype(np.float64).sum()) * factor == pytest.approx(mass_points,
                                                                               rel=1e-6)
    # The cube is the definition on the real reader's leaves too.
    leaves = [(lv, c, {"density": v[0]}) for lv, c, v in exp.leaves]
    _, dens = run_cube(h, "density", 2)
    np.testing.assert_allclose(dens, brute(leaves, 2), rtol=1e-5)


# ── The level map under a cap ────────────────────────────────────────────────

def _flat(m):
    return [(int(lv), r) for lv in sorted(m["levels"], key=int) for r in m["levels"][lv]]


@pytest.mark.parametrize("cap", [0, 1, 3, 4, 9, 10_000])
def test_a_capped_level_map_keeps_the_coarsest_rects_and_the_exact_counts(cap, monkeypatch):
    leaves = _mesh({(1, (0, 0, 0)), (2, (0, 0, 0)), (2, (1, 1, 1)), (1, (1, 0, 0)),
                    (2, (2, 0, 0)), (2, (3, 1, 0))}, base=1)
    h = MemoryAmr(leaves)
    full = av.level_map(h, "z", 0.1, max_rects=10 ** 9)
    # Whatever the cap, nothing above it is ever held.
    held = []
    real = av._drop_finest

    def watch(by_level, excess):
        out = real(by_level, excess)
        held.append(sum(c.shape[0] for chunks in by_level.values() for c in chunks))
        return out

    monkeypatch.setattr(av, "_drop_finest", watch)
    capped = av.level_map(h, "z", 0.1, max_rects=cap)
    assert max(held) <= cap
    assert capped["counts"] == full["counts"] and capped["cells"] == full["cells"]
    assert _flat(capped) == _flat(full)[:cap]
    assert capped["truncated"] == (cap < full["cells"]) and capped["area"] == full["area"]
    assert set(capped["levels"]) == set(full["levels"])


def test_a_capped_level_map_reads_blocks_of_any_order(monkeypatch):
    """A block of a coarser level after a finer one still wins its place."""
    h = MemoryAmr(_mesh({(1, (0, 0, 0))}))
    real = MemoryAmr.leaf_blocks

    def reversed_blocks(self, *a, **k):
        yield from sorted(real(self, *a, **k), key=lambda b: -b.level)

    monkeypatch.setattr(MemoryAmr, "leaf_blocks", reversed_blocks)
    capped = av.level_map(h, "z", 0.1, max_rects=2)
    assert capped["truncated"] and sum(capped["counts"].values()) > 2
    assert len(capped["levels"]["1"]) == 2 and capped["levels"]["2"] == []


# ── Units: the cube is in code units and the units table converts ───────────

def test_an_amr_cube_has_the_units_of_its_run(tmp_path):
    from app import physical_units as pu
    directory, _ = W.write_output(tmp_path / "run", particles=None)
    h = ro.open_output(directory).amr()
    table = h.scales()
    for field in ("density", "pressure", "vx", "mass", "level"):
        made = av.worker_amr_uniform(str(directory), field, 2, None, None, "box", 1 << 20,
                                     str(tmp_path / "out"))
        cube = made["outputs"][0]["path"]
        assert cube.endswith(".amr-uniform.fits")
        got = pu.scales(cube)
        want = {"x": table["x"], "y": table["y"], "z": table["z"]}
        if field != "level":
            want["value"] = table[field]
        assert set(got) == set(want), field
        for column, entry in want.items():
            assert got[column]["factor"] == pytest.approx(entry["factor"]), (field, column)
            assert got[column]["unit"] == entry["unit"]
        # Stored values are code values: the factor is the run's, not baked in.
        with fits.open(cube) as hdul:
            assert hdul[0].header["AMRFCT"] == pytest.approx(
                table.get(field, {"factor": 1.0})["factor"])
            assert hdul[0].header["CUNIT1"] == "box"
    # A typed override on the cube replaces the run's factor for that column only.
    cube = av.worker_amr_uniform(str(directory), "density", 1, None, None, "box", 1 << 20,
                                 str(tmp_path / "out2"))["outputs"][0]["path"]
    pu.set_user(cube, columns={"value": (7.0, "my unit")}, by="test")
    typed = pu.scales(cube)
    assert typed["value"] == {"factor": 7.0, "unit": "my unit", "source": "user"}
    assert typed["x"]["factor"] == pytest.approx(table["x"]["factor"])
    pu.set_user(cube, clear=True, by="test")
    assert pu.scales(cube)["value"]["factor"] == pytest.approx(table["density"]["factor"])


def test_a_cube_whose_run_is_gone_or_a_plain_fits_has_no_amr_units(tmp_path):
    import shutil
    from app import physical_units as pu
    directory, _ = W.write_output(tmp_path / "run", particles=None)
    cube = av.worker_amr_uniform(str(directory), "density", 1, None, None, "box", 1 << 20,
                                 str(tmp_path / "out"))["outputs"][0]["path"]
    shutil.rmtree(directory)
    assert pu.scales(cube) == {}
    other = tmp_path / "plain.fits"
    fits.PrimaryHDU(np.zeros((2, 2, 2), dtype=np.float32)).writeto(other)
    assert pu.scales(other) == {}


# ── Drawing the cells ────────────────────────────────────────────────────────

def test_the_cell_size_is_the_radius_of_a_cell_and_a_named_field_wins(tmp_path):
    from app.particle_raytrace import cell_radius_factor, gaussian_width
    from app.routers.particle_render import ParticleRenderRequest, _prepare
    directory, _ = W.write_output(tmp_path, particles=None)
    ref = ro.ramses_ref(directory, "cells")
    entry = {"kind": "catalogue", "format": "ramses", "path": ref, "paths": [str(directory)]}
    cam = [{"position": [0.5, 0.5, 4.0], "focal_point": [0.5, 0.5, 0.5], "view_up": [0, 1, 0],
            "view_angle": 30.0}]

    def prepared(**kw):
        return _prepare(entry, ParticleRenderRequest(dataset_id="x", cameras=cam, **kw))

    p = prepared()
    cells = ro.open_output(directory).cells()
    dx = cells.take("dx", np.arange(cells.num_rows)).astype(np.float32)
    # One kernel of the rms width side/2 per axis: h = side / sqrt(2) for the Gaussian.
    np.testing.assert_allclose(p["radius"] * p["frame"][1], dx * (2 ** -0.5), rtol=1e-4)
    assert cell_radius_factor("gauss") == pytest.approx(math.sqrt(0.5))
    assert cell_radius_factor("m4") == pytest.approx(math.sqrt(0.5) / gaussian_width("m4"))
    # A named radius field replaces it; the neighbour estimate is used for no cell.
    q = prepared(radius_field="level")
    assert not np.allclose(q["radius"], p["radius"])
    # A table with no declared size is drawn as before (a particle family).
    d2, _ = W.write_output(tmp_path / "p", particles="descriptor")
    e2 = {"kind": "catalogue", "format": "ramses", "path": ro.ramses_ref(d2, "dm"),
          "paths": [str(d2)]}
    assert getattr(ro.resolve(ro.ramses_ref(d2, "dm")), "size_column", None) is None
    assert _prepare(e2, ParticleRenderRequest(dataset_id="x", cameras=cam, neighbours=2))["radius"].size


def test_a_uniform_field_stays_flat_when_the_cells_are_drawn():
    """The kernel's rms of half a cell: Gaussians one side apart sum to a sheet
    whose ripple is 1.4 %, against 39 % at the cube's own rms."""
    side = 1.0
    x = np.linspace(0, side, 101)
    for sigma, ripple in ((0.5, 0.0145), (side / math.sqrt(12), 0.386)):
        total = sum(np.exp(-0.5 * ((x - n * side) / sigma) ** 2) for n in range(-8, 9))
        assert (total.max() - total.min()) / (total.max() + total.min()) == pytest.approx(
            ripple, rel=0.06)


# ── Routes ───────────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_the_routes_plan_run_and_map(client, auth_headers, tmp_path):
    directory, exp = W.write_output(tmp_path / "run", particles=None)
    ref = ro.ramses_ref(directory, "cells")
    plan = (await client.post("/v1/simulations/amr/plan", json={"path": str(directory),
                                                                  "field": "density"},
                              headers=auth_headers)).json()
    assert plan["valid"] and plan["code"] == "ramses"
    p = plan["plan"]
    assert p["level"] == 2 and p["shape"] == [4, 4, 4] and p["fits"] and p["voxels"] == 64
    assert p["length_unit"] == "kpc" and p["finest_level_in_region"] == 2
    box = (await client.post("/v1/simulations/amr/plan", json={
        "path": ref, "field": "pressure", "lo": [0.6, 0.6, 0.6], "hi": [0.9, 0.9, 0.9]},
        headers=auth_headers)).json()["plan"]
    assert box["finest_level_in_region"] == 2 and box["level"] == 2   # the level-2 grid at (0.75,)^3
    far = (await client.post("/v1/simulations/amr/plan", json={
        "path": ref, "lo": [0.5, 0.0, 0.0], "hi": [1.0, 0.2, 0.2]}, headers=auth_headers)).json()
    assert far["plan"]["finest_level_in_region"] == 1 and far["plan"]["level"] == 1
    made = await client.post("/v1/simulations/amr/uniform", json={
        "path": str(directory), "field": "density", "level": 2}, headers=auth_headers)
    assert made.status_code == 200, made.text
    out = made.json()["outputs"][0]
    assert out["shape"] == [4, 4, 4] and made.json()["finest_level"] == 2
    with fits.open(out["path"]) as hdul:
        data = hdul[0].data
        header = hdul[0].header
    assert data.shape == (4, 4, 4) and header["AMRFLD"] == "density" and header["AMRLEV"] == 2
    assert header["BUNIT"] == "code density" and header["CUNIT1"] == "box"
    assert header["AMRUNIT"] == "g/cm^3" and out["path"].endswith(".amr-uniform.fits")
    leaves = [(lv, c, {"density": v[0]}) for lv, c, v in exp.leaves]
    np.testing.assert_allclose(data, brute(leaves, 2), rtol=1e-5)
    # A region without a level: that box at the finest level it holds.
    refined = await client.post("/v1/simulations/amr/uniform", json={
        "path": ref, "field": "density", "lo": [0.6, 0.6, 0.6], "hi": [0.9, 0.9, 0.9]},
        headers=auth_headers)
    assert refined.json()["outputs"][0]["shape"] == [2, 2, 2], refined.text
    # The limit is refused with the plan's reason.
    big = await client.post("/v1/simulations/amr/uniform", json={
        "path": ref, "field": "density", "level": 2, "max_voxels": 10}, headers=auth_headers)
    assert big.status_code == 422 and "allowed" in big.json()["detail"]
    assert (await client.post("/v1/simulations/amr/uniform", json={
        "path": ref, "field": "nope", "level": 2}, headers=auth_headers)).status_code == 422
    lm = (await client.post("/v1/simulations/amr/level-map", json={
        "path": ref, "axis": "z", "coordinate": 0.1}, headers=auth_headers)).json()
    assert lm["valid"] and lm["area"] == pytest.approx(1.0) and lm["plane_axes"] == ["x", "y"]
    assert lm["counts"] == {"1": 3, "2": 4} or lm["cells"] == sum(lm["counts"].values())
    nope = await client.post("/v1/simulations/amr/level-map", json={
        "path": str(tmp_path / "nowhere"), "axis": "z", "coordinate": 0.1}, headers=auth_headers)
    assert nope.status_code == 422


# ── The real output ──────────────────────────────────────────────────────────

needs_real = pytest.mark.skipif(not REAL.is_dir(), reason="the real RAMSES output is not here")


@needs_real
def test_the_real_output_cells_cube_and_level_map_agree():
    out = ro.open_output(REAL)
    h = out.amr()
    counts = h.leaf_counts()
    # The mass of the cells (density * dx^3) against the cubes at levels 7 and 8.
    mass_cells = sum(float(np.sum(b.fields["density"].astype(np.float64)) * b.dx ** 3)
                     for b in h.leaf_blocks(["density"]))
    for level in (7, 8):
        plan = av.plan_uniform(h, "density", level, max_voxels=1 << 27)
        total = 0.0

        def sink(slab):
            nonlocal total
            total += float(slab.astype(np.float64).sum())

        av.resample(h, plan, sink)
        # density * voxel volume, in box units and code density.
        assert total * plan.dx ** 3 == pytest.approx(mass_cells, rel=1e-6), level
    # The level map's cells of each level over a plane are consistent with the counts.
    m = av.level_map(h, "z", 0.5)
    assert m["area"] == pytest.approx(1.0, rel=1e-9)
    assert set(map(int, m["counts"])) <= set(counts) and all(
        n <= counts[int(lv)] for lv, n in m["counts"].items())
    assert h.finest_level_in() == max(lv for lv, n in counts.items() if n)


@needs_real
def test_the_real_output_conserves_momentum_and_level_map_caps_are_exact():
    h = ro.open_output(REAL).amr()
    momentum = scale = 0.0
    for b in h.leaf_blocks(["density", "vx"]):
        m = b.fields["density"].astype(np.float64) * b.dx ** 3
        momentum += float((m * b.fields["vx"]).sum())
        scale += float((m * np.abs(b.fields["vx"])).sum())
    cubes = {}
    for field in ("density", "vx"):
        plan = av.plan_uniform(h, field, 7, max_voxels=1 << 27)
        slabs = []
        av.resample(h, plan, slabs.append)
        cubes[field] = np.concatenate(slabs)
    assert plan.weighting == "density"
    cube = float((cubes["density"].astype(np.float64) * cubes["vx"]).sum()) * plan.dx ** 3
    # Sum of m v over the cells against the cube's rho v voxel-volume sum: float32 storage only.
    assert abs(cube - momentum) / scale < 1e-6
    # A tiny cap keeps the coarsest rectangles, and the counts stay those of the slice.
    full = av.level_map(h, "z", 0.5)
    for cap in (1, 5000):
        small = av.level_map(h, "z", 0.5, max_rects=cap)
        assert small["counts"] == full["counts"] and small["truncated"]
        assert _flat(small) == _flat(full)[:cap]
