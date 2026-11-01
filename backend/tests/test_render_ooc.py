"""Out-of-core rendering from the brick pyramid (#70): the block set a view
gets, how it is loaded and shown. No GPU: real VTK objects, nothing drawn."""
from __future__ import annotations

import numpy as np
import pytest
from astropy.io import fits

vtk = pytest.importorskip("vtk")

from app.brick_lod import node_box  # noqa: E402
from app.render_session import _ooc  # noqa: E402


@pytest.fixture
def cube(tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    # Small numbers so that a small cube exercises several levels and blocks.
    monkeypatch.setattr(_ooc, "OOC_NODE", 16)
    monkeypatch.setattr(_ooc, "OOC_BUDGET_BYTES", 4 * 40_000)
    monkeypatch.setattr(_ooc, "OOC_MAX_BLOCKS", 64)
    monkeypatch.setattr(_ooc, "_PROXY_VOXELS", 20 ** 3)
    rng = np.random.default_rng(1)
    data = rng.normal(0.0, 1.0, (40, 48, 56)).astype(np.float32)
    data[0:8, 0:8, 0:8] = np.nan
    path = tmp_path / "cube.fits"
    fits.PrimaryHDU(data).writeto(path)
    return path, data


def _session_objects(path):
    from app.brick_pyramid import build_pyramid, default_pyramid_path

    build_pyramid(path, default_pyramid_path(path), brick=8)   # small bricks for the test
    ooc = _ooc.OutOfCoreCube(str(path), 0, "test")
    prop = vtk.vtkVolumeProperty()
    blocks = _ooc.BlockVolumes(prop)
    ooc.blocks = blocks
    renderer = vtk.vtkRenderer()
    blocks.attach(renderer)
    cam = renderer.GetActiveCamera()
    nz, ny, nx = ooc.shape
    cam.SetFocalPoint(nx / 2, ny / 2, nz / 2)
    cam.SetPosition(nx / 2, ny / 2, nz / 2 - 150)
    return ooc, blocks, renderer


def _settle(ooc, renderer, threshold=None):
    for _ in range(100):
        if not ooc.update(renderer, (400, 300), threshold, interactive=False):
            return
    raise AssertionError("the block set never finished loading")


def test_the_view_gets_a_complete_non_overlapping_block_set(cube):
    path, data = cube
    ooc, blocks, renderer = _session_objects(path)
    _settle(ooc, renderer)
    nodes = blocks.nodes
    assert nodes and renderer.GetVolumes().GetNumberOfItems() == len(nodes)
    count = np.zeros(data.shape, dtype=np.int8)
    for n in nodes:
        lo, hi = node_box(ooc.layout, _ooc.OOC_NODE, n)
        x0, y0, z0 = (int(v) for v in lo)
        x1, y1, z1 = (int(v) for v in hi)
        count[z0:z1, y0:y1, x0:x1] += 1
    # Every voxel once, except the blank corner, which has no block.
    assert count.max() == 1
    assert (count[8:, 8:, 8:] == 1).all()
    # The budget holds for what is uploaded, ghost samples included.
    assert sum(ooc._cache[n][0].nbytes for n in nodes) <= _ooc.OOC_BUDGET_BYTES
    ooc.close()


def test_a_block_is_placed_in_level_zero_voxels_with_a_ghost_sample(cube):
    path, data = cube
    ooc, blocks, renderer = _session_objects(path)
    _settle(ooc, renderer)
    for node in blocks.nodes:
        array, origin, spacing, _ = ooc._cache[node]
        lo, _ = node_box(ooc.layout, _ooc.OOC_NODE, node)
        assert spacing == 2 ** node.level
        assert origin == tuple(float(v) - spacing + (spacing - 1) / 2 for v in lo)
        # Blank voxels are the cube's minimum, never NaN.
        assert np.isfinite(array).all()
    ooc.close()


def test_a_block_holds_its_region_with_edge_repeated_ghosts(cube):
    """The values: the level's region, one sample more on every side, the edge
    repeated where the cube ends, blank voxels at the cube's minimum."""
    path, data = cube
    ooc, blocks, renderer = _session_objects(path)
    _settle(ooc, renderer)
    blocks.SetBlendModeToMaximumIntensity()
    _settle(ooc, renderer)
    for key in list(ooc._cache):
        array = ooc._cache[key][0]
        level, regions, _, _ = ooc._region(key)
        full = ooc.pyramid._h5[f"level{level}/data"][...]
        full = np.where(np.isfinite(full), full, np.float32(ooc.vmin))
        expected = np.pad(full, 1, mode="edge")[tuple(slice(r.start, r.stop + 2) for r in regions)]
        assert array.dtype == np.float32 and np.array_equal(array, expected)
    ooc.close()


def _mixed_levels(path):
    ooc, blocks, renderer = _session_objects(path)
    cam = renderer.GetActiveCamera()
    nz, ny, nx = ooc.shape
    cam.SetPosition(nx / 2, ny / 2, -25)          # close to one face: fine there, coarse behind
    cam.SetViewAngle(60)
    _settle(ooc, renderer)
    assert len({n.level for n in blocks.nodes}) > 1
    return ooc, blocks, renderer


def test_blocks_of_different_levels_meet_without_gaps_or_overlaps(cube):
    """What VTK draws: the cropping planes of the mappers tile the cube's sample
    range, and every block's samples reach its planes."""
    path, data = cube
    ooc, blocks, renderer = _mixed_levels(path)
    boxes = []
    for key, (image, mapper, _) in blocks._blocks.items():
        array, origin, spacing, crop = ooc._cache[key]
        assert mapper.GetCropping() == 1
        assert np.allclose(mapper.GetCroppingRegionPlanes(),
                           _ooc._vtk_crop_planes(crop, origin, spacing, image.GetDimensions()))
        b = image.GetBounds()
        for axis in range(3):
            assert b[2 * axis] <= crop[2 * axis] and crop[2 * axis + 1] <= b[2 * axis + 1]
        boxes.append(np.array(crop).reshape(3, 2))
    nz, ny, nx = data.shape
    rng = np.random.default_rng(2)
    points = rng.uniform(0, 1, (4000, 3)) * [nx - 1, ny - 1, nz - 1]
    inside = np.zeros(len(points), dtype=int)
    for box in boxes:
        inside += np.all((points > box[:, 0]) & (points < box[:, 1]), axis=1)
    assert inside.max() == 1
    # Outside the blank corner (no block), every point is drawn by one block.
    away = np.any(points > 8.0, axis=1)
    assert (inside[away] == 1).all()
    ooc.close()


def test_crop_planes_undo_the_mappers_cell_extent():
    """The positions measured on the GPU (VTK 9.3.1 and 9.6.2): with n samples
    from o at spacing s, a plane p is drawn at o - s/2 + (p - o) n / (n - 1)."""
    def drawn(p, o, s, n):
        return o - s / 2 + (p - o) * n / (n - 1)

    assert drawn(31.5, 0.0, 1.0, 33) == pytest.approx(31.98, abs=0.02)   # measured 31.97
    assert drawn(32.0, 31.0, 1.0, 33) == pytest.approx(31.53, abs=0.02)  # measured 31.55
    assert drawn(31.0, 0.5, 2.0, 18) == pytest.approx(31.79, abs=0.02)   # measured 31.79
    crop = (0.0, 31.5, 2.0, 30.0, 31.5, 63.0)
    origin, spacing, dims = (0.0, 1.0, 31.0), 1.0, (33, 31, 33)
    planes = _ooc._vtk_crop_planes(crop, origin, spacing, dims)
    for axis in range(3):
        o, n = origin[axis], dims[axis]
        assert drawn(planes[2 * axis], o, spacing, n) == pytest.approx(crop[2 * axis], abs=1e-5)
        assert drawn(planes[2 * axis + 1], o, spacing, n) == pytest.approx(
            crop[2 * axis + 1] - _ooc._FACE_EPS, abs=1e-5)


def test_shared_faces_stay_apart_in_float32_at_any_coordinate():
    """VTK keeps the planes as float32: neighbours must not overlap there,
    even where 1e-3 is below float32's resolution."""
    def drawn(p, o, s, n):
        return o - s / 2 + (float(np.float32(p)) - o) * n / (n - 1)

    for face in (31.5, 4095.5, 65535.5, 1_048_575.5):
        for s in (1.0, 4.0):
            n = 258
            o_a, o_b = face - (n - 1.5) * s + (s - 1) / 2, face - s / 2 + (s - 1) / 2
            a = _ooc._vtk_crop_planes((o_a, face) * 3, (o_a,) * 3, s, (n,) * 3)
            b = _ooc._vtk_crop_planes((face, o_b + 10 * s) * 3, (o_b,) * 3, s, (n,) * 3)
            assert drawn(a[1], o_a, s, n) < face <= drawn(b[0], o_b, s, n)


def _drawn_back_to_front(ooc, blocks, renderer):
    """No block is drawn before one that hides part of it: for each pair, if
    they are separated along only one axis the eye must not be on the side of
    the earlier one."""
    eye = renderer.GetActiveCamera().GetPosition()
    by_volume = {id(v): np.array(ooc._cache[k][3]).reshape(3, 2)
                 for k, (_, _, v) in blocks._blocks.items()}
    volumes = renderer.GetVolumes()
    volumes.InitTraversal()
    order = [by_volume[id(volumes.GetNextVolume())] for _ in range(volumes.GetNumberOfItems())]
    for i, a in enumerate(order):
        for b in order[i + 1:]:
            sides = []
            for axis in range(3):
                if a[axis, 1] <= b[axis, 0]:
                    sides.append(eye[axis] < a[axis, 1])   # eye on a's side: a in front
                elif b[axis, 1] <= a[axis, 0]:
                    sides.append(eye[axis] > a[axis, 0])
            assert sides and not all(sides), "a nearer block is drawn first"


def test_dragging_keeps_the_block_set_and_reorders_it(cube):
    path, _ = cube
    ooc, blocks, renderer = _mixed_levels(path)
    before = blocks.nodes
    cam = renderer.GetActiveCamera()
    nz, ny, nx = ooc.shape
    cam.SetPosition(nx / 2, ny / 2, nz + 25)     # round to the other side
    assert ooc.update(renderer, (400, 300), None, interactive=True) is False
    assert blocks.nodes == before
    _drawn_back_to_front(ooc, blocks, renderer)
    ooc.close()


def test_mip_and_minip_draw_one_whole_level(cube):
    path, data = cube
    ooc, blocks, renderer = _session_objects(path)
    for mode in (blocks.SetBlendModeToMaximumIntensity, blocks.SetBlendModeToMinimumIntensity):
        mode()
        _settle(ooc, renderer)
        (key,) = blocks.nodes
        assert isinstance(key, _ooc.WholeLevel)
        array, _, _, crop = ooc._cache[key]
        assert array.nbytes <= _ooc.OOC_BUDGET_BYTES
        nz, ny, nx = data.shape
        assert crop == (0.0, nx - 1.0, 0.0, ny - 1.0, 0.0, nz - 1.0)
    blocks.SetBlendModeToComposite()
    _settle(ooc, renderer)
    assert all(not isinstance(k, _ooc.WholeLevel) for k in blocks.nodes)
    ooc.close()


def test_pruning_starts_where_the_opacity_is_zero():
    from app.render_session._common import _build_opacity_tf, _opacity_floor

    floor = _opacity_floor(0.0, 100.0, 50.0)
    tf = _build_opacity_tf(0.0, 100.0, 50.0)
    assert tf.GetValue(floor) == 0.0 and tf.GetValue(floor + 0.1) > 0.0
    assert _opacity_floor(5.0, 5.0, 5.0) is None


def test_settings_reach_every_block_present_and_future(cube):
    path, _ = cube
    ooc, blocks, renderer = _session_objects(path)
    blocks.SetAutoAdjustSampleDistances(0)
    blocks.SetImageSampleDistance(3.0)
    blocks.SetBlendModeToMaximumIntensity()
    blocks.SetVisibility(0)
    _settle(ooc, renderer)
    for _, mapper, volume in blocks._blocks.values():
        assert mapper.GetAutoAdjustSampleDistances() == 0
        assert mapper.GetImageSampleDistance() == 3.0
        assert mapper.GetBlendMode() == mapper.MAXIMUM_INTENSITY_BLEND
        assert volume.GetVisibility() == 0
    blocks.SetVisibility(1)
    assert all(v.GetVisibility() == 1 for _, _, v in blocks._blocks.values())
    ooc.close()


def test_blocks_are_drawn_back_to_front(cube):
    path, _ = cube
    ooc, blocks, renderer = _mixed_levels(path)
    _drawn_back_to_front(ooc, blocks, renderer)
    ooc.close()


def test_the_cutting_plane_reads_full_resolution_from_the_file(cube):
    path, data = cube
    ooc, _, _ = _session_objects(path)
    assert np.allclose(ooc.plane(20), data[20], equal_nan=True)
    ooc.close()


def test_the_proxy_spans_the_cube(cube):
    path, data = cube
    ooc, _, _ = _session_objects(path)
    proxy, origin, spacing = ooc.proxy()
    assert proxy.size <= _ooc._PROXY_VOXELS and np.isfinite(proxy).all()
    nz, ny, nx = data.shape
    pz, py, px = proxy.shape
    # The proxy's last voxel centre lies within one of its voxels of the cube's end.
    assert abs(origin[0] + (px - 1) * spacing - (nx - 1)) <= spacing
    assert abs(origin[2] + (pz - 1) * spacing - (nz - 1)) <= spacing
    ooc.close()


def test_small_cubes_stay_in_core(monkeypatch):
    monkeypatch.setattr(_ooc, "OOC_BYTES", 6 * 1024 ** 3)
    assert not _ooc.wants_out_of_core(2 * 1024 ** 3)
    assert _ooc.wants_out_of_core(7 * 1024 ** 3)


def test_mip_with_a_budget_no_level_fits_draws_nothing(cube, monkeypatch):
    path, _ = cube
    ooc, blocks, renderer = _session_objects(path)
    monkeypatch.setattr(_ooc, "OOC_BUDGET_BYTES", 4)
    blocks.SetBlendModeToMaximumIntensity()
    assert ooc.update(renderer, (400, 300), None, interactive=False) is False
    assert not blocks.nodes
    ooc.close()


def test_percentiles_come_from_full_resolution_values(cube):
    """Noise averaged in the proxy has a narrower spread; the sample of
    level-0 bricks has the cube's."""
    path, data = cube
    ooc, _, _ = _session_objects(path)
    sample = ooc.sample(voxels=8 * 8 ** 3)
    assert sample.size == 8 * 8 ** 3 and np.isfinite(sample).all()
    true_p99 = np.nanpercentile(data, 99.0)
    proxy, _, _ = ooc.proxy()
    assert abs(np.percentile(sample, 99.0) - true_p99) < abs(np.percentile(proxy, 99.0) - true_p99)
    assert abs(np.percentile(sample, 99.0) - true_p99) < 0.25
    ooc.close()


def test_reordering_keeps_the_blocks_in_the_renderer(cube):
    """The order changes inside the prop collection: the blocks are never
    removed from the renderer (that would release their GPU textures)."""
    path, _ = cube
    ooc, blocks, renderer = _mixed_levels(path)
    removed = []

    class Spy:
        def __getattr__(self, name):
            return getattr(renderer, name)

        def RemoveVolume(self, volume):
            removed.append(volume)
            renderer.RemoveVolume(volume)

    blocks._renderer = Spy()
    nz, ny, nx = ooc.shape
    renderer.GetActiveCamera().SetPosition(nx / 2, ny / 2, nz + 25)
    ooc.update(renderer, (400, 300), None, interactive=True)
    assert not removed
    _drawn_back_to_front(ooc, blocks, renderer)
    ooc.close()


def test_a_removed_block_releases_its_data():
    """VTK reads the cached array without a copy; once the block is gone,
    nothing may keep that array alive."""
    import gc
    import weakref

    from app.brick_lod import Node

    blocks = _ooc.BlockVolumes(vtk.vtkVolumeProperty())
    blocks.attach(vtk.vtkRenderer())
    refs = []
    for i in range(5):
        array = np.ones((8, 8, 8), dtype=np.float32)
        refs.append(weakref.ref(array))
        blocks.replace({Node(0, i, 0, 0): (array, (0.0, 0.0, 0.0), 1.0, (0, 7, 0, 7, 0, 7))})
        del array
    gc.collect()
    assert refs[-1]() is not None                    # the shown block's data is alive
    blocks.clear()
    gc.collect()
    assert all(r() is None for r in refs)
