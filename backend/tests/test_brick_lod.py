"""Choosing the blocks of a brick pyramid a view draws (#70)."""
from __future__ import annotations

import numpy as np
import pytest

from app.brick_lod import Camera, Node, PyramidLayout, choose_nodes, node_box, projected_voxel_px
from app.brick_pyramid import level_shapes

BRICK, NODE = 16, 32


def _layout(shape=(100, 120, 140), maxima=False, fill=1.0):
    shapes = level_shapes(shape, BRICK)
    stats = None
    if maxima:
        stats = [np.full(tuple(-(-n // BRICK) for n in s), fill, dtype=np.float32) for s in shapes]
    return PyramidLayout(BRICK, shapes, stats)


def _covered(layout, nodes):
    """Level-0 voxels covered by the chosen nodes, counting overlaps."""
    nz, ny, nx = layout.shapes[0]
    count = np.zeros((nz, ny, nx), dtype=np.int8)
    for n in nodes:
        lo, hi = node_box(layout, NODE, n)
        x0, y0, z0 = (int(v) for v in lo)
        x1, y1, z1 = (int(v) for v in hi)
        count[z0:z1, y0:y1, x0:x1] += 1
    return count


def _near(layout):
    nz, ny, nx = layout.shapes[0]
    return Camera(position=(nx / 2, ny / 2, -5.0), viewport_height_px=4000)


def test_with_room_and_a_close_camera_everything_is_full_resolution():
    layout = _layout()
    nodes = choose_nodes(layout, _near(layout), budget_voxels=10 ** 9, node_size=NODE)
    assert all(n.level == 0 for n in nodes)
    assert (_covered(layout, nodes) == 1).all()            # every voxel once, no overlap


def test_the_budget_is_kept_and_the_cover_stays_complete():
    layout = _layout()
    budget = 120_000
    nodes = choose_nodes(layout, _near(layout), budget_voxels=budget, node_size=NODE)
    from app.brick_lod import _node_voxels
    assert sum(_node_voxels(layout, NODE, n) for n in nodes) <= budget
    assert (_covered(layout, nodes) == 1).all()
    assert len({n.level for n in nodes}) > 1                # mixed resolutions


def test_a_far_camera_needs_only_coarse_blocks():
    layout = _layout()
    far = Camera(position=(70.0, 60.0, -1e6), viewport_height_px=1000)
    nodes = choose_nodes(layout, far, budget_voxels=10 ** 9, node_size=NODE)
    assert min(n.level for n in nodes) == len(layout.shapes) - 1


def test_nearer_blocks_are_finer():
    layout = _layout((64, 64, 512))
    # Looking along +x on a small viewport: the far end needs no full resolution.
    eye = Camera(position=(-10.0, 32.0, 32.0), viewport_height_px=100)
    nodes = choose_nodes(layout, eye, budget_voxels=10 ** 9, node_size=NODE, target_px=1.0)
    near = [n for n in nodes if node_box(layout, NODE, n)[0][0] < 64]
    far = [n for n in nodes if node_box(layout, NODE, n)[0][0] >= 384]
    assert max(n.level for n in near) < min(n.level for n in far)


def test_parallel_projection_uses_one_scale():
    layout = _layout()
    cam = Camera(position=(0.0, 0.0, -100.0), viewport_height_px=1000, parallel_scale=500.0)
    assert projected_voxel_px(layout, NODE, Node(0, 0, 0, 0), cam) == pytest.approx(1.0)
    nodes = choose_nodes(layout, cam, budget_voxels=10 ** 9, node_size=NODE, target_px=1.0)
    assert {n.level for n in nodes} == {0}


def test_blocks_below_the_threshold_or_blank_are_left_out():
    layout = _layout(maxima=True, fill=0.5)
    nodes = choose_nodes(layout, _near(layout), budget_voxels=10 ** 9, node_size=NODE, threshold=1.0)
    assert nodes == []
    blank = _layout(maxima=True, fill=np.nan)
    assert choose_nodes(blank, _near(blank), budget_voxels=10 ** 9, node_size=NODE) == []
    bright = _layout(maxima=True, fill=2.0)
    assert choose_nodes(bright, _near(bright), budget_voxels=10 ** 9, node_size=NODE, threshold=1.0)


def test_blocks_outside_the_frustum_are_culled():
    layout = _layout()
    # One plane keeping only x >= 64: x - 64 >= 0.
    cam = Camera(position=(70.0, 60.0, -5.0), viewport_height_px=4000, planes=[(1.0, 0.0, 0.0, -64.0)])
    nodes = choose_nodes(layout, cam, budget_voxels=10 ** 9, node_size=NODE)
    # A block that reaches the plane is kept; none lies wholly on the wrong side.
    assert nodes and all(node_box(layout, NODE, n)[1][0] >= 64 for n in nodes)
    assert all(node_box(layout, NODE, n)[0][0] >= 32 for n in nodes)


def test_a_node_count_limit_is_kept():
    layout = _layout()
    nodes = choose_nodes(layout, _near(layout), budget_voxels=10 ** 9, node_size=NODE, max_nodes=10)
    assert 0 < len(nodes) <= 10
    assert (_covered(layout, nodes) == 1).all()


def test_the_node_must_be_whole_bricks():
    layout = _layout()
    with pytest.raises(ValueError):
        choose_nodes(layout, _near(layout), budget_voxels=10 ** 9, node_size=24)


def test_a_single_bright_voxel_keeps_its_block(tmp_path):
    """Coarse levels are averages: a lone voxel of 100 is 1.6 at the top level.
    Pruning uses the level-0 maxima beneath each block, so it is not lost."""
    from astropy.io import fits

    from app.brick_pyramid import BrickPyramid, build_pyramid

    data = np.zeros((32, 32, 32), dtype=np.float32)
    data[10, 10, 10] = 100.0
    fits.PrimaryHDU(data).writeto(tmp_path / "spike.fits")
    with BrickPyramid(build_pyramid(tmp_path / "spike.fits", tmp_path / "p.h5", brick=8)) as p:
        assert p.stats(p.levels - 1)[1].max() < 10                 # the average hides it
        layout = PyramidLayout.of(p)
    nodes = choose_nodes(layout, Camera((16.0, 16.0, -5.0), 1000), budget_voxels=10 ** 6,
                         node_size=8, threshold=10.0)
    assert nodes
    covered = [n for n in nodes if all(lo <= c < hi for c, lo, hi in
                                       zip((10, 10, 10), *node_box(layout, 8, n)))]
    assert covered and covered[0].level == 0                       # refined down to the voxel


def test_a_budget_below_the_coarsest_cover_is_refused():
    layout = _layout()
    with pytest.raises(ValueError, match="coarsest cover"):
        choose_nodes(layout, _near(layout), budget_voxels=1, node_size=NODE)


def test_off_axis_blocks_are_measured_at_their_depth():
    """Perspective scales with depth along the view, not with distance: a block
    to the side at depth 128 under a 90-degree field on 1000 px shows a level-1
    voxel (2 units) over 2 / (2 * 128 / 1000) = 7.81 px."""
    layout = PyramidLayout(16, level_shapes((256, 256, 256), 16))
    cam = Camera((0.0, 0.0, 0.0), 1000, view_angle=90.0, direction=(0.0, 0.0, 1.0))
    assert projected_voxel_px(layout, NODE, Node(1, 2, 0, 2), cam) == pytest.approx(7.8125)


def test_a_bright_voxel_across_a_face_keeps_the_neighbouring_block(tmp_path):
    """A drawn block interpolates up to its faces from its ghost samples: the
    block next to a bright voxel shows part of it and must not be pruned."""
    from astropy.io import fits

    from app.brick_pyramid import BrickPyramid, build_pyramid

    data = np.zeros((32, 32, 32), dtype=np.float32)
    data[20, 20, 16] = 100.0                                   # first voxel of the x=16.. node
    fits.PrimaryHDU(data).writeto(tmp_path / "edge.fits")
    with BrickPyramid(build_pyramid(tmp_path / "edge.fits", tmp_path / "p.h5", brick=8)) as p:
        layout = PyramidLayout.of(p)
    camera = Camera((16.0, 16.0, -50.0), 4000, direction=(0.0, 0.0, 1.0))
    nodes = choose_nodes(layout, camera, budget_voxels=10 ** 6, node_size=16,
                         threshold=9.0, halo=1)
    assert Node(0, 0, 1, 1) in nodes and Node(0, 1, 1, 1) in nodes
    # Without the halo only the voxel's own block is kept.
    nodes = choose_nodes(layout, camera, budget_voxels=10 ** 6, node_size=16, threshold=9.0)
    assert Node(0, 0, 1, 1) not in nodes and Node(0, 1, 1, 1) in nodes


def test_culling_uses_the_extent_of_the_voxels():
    """Voxel 16 spans [15.5, 16.5]: a frustum ending at x = 15.75 still sees it."""
    layout = PyramidLayout(8, level_shapes((32, 32, 32), 8), None)
    camera = Camera((16.0, 16.0, -50.0), 4000, direction=(0.0, 0.0, 1.0),
                    planes=[(-1.0, 0.0, 0.0, 15.75)])
    nodes = choose_nodes(layout, camera, budget_voxels=10 ** 6, node_size=16)
    assert any(n.level == 0 and n.ix == 1 for n in nodes)
