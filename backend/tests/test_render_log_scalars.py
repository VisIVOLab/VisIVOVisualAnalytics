"""A log-scaled cube drawn in log10 by the render session (issue #328).

The VR page sends the transfer functions of a log-scaled cube (a density over
seven decades) with its points in log10 and ``scale: "log"``. Drawn in
values, VTK samples them into lookup textures linear in value: a range over
seven decades needs millions of texels, VTK falls back to 16384, and the
faint decades share one (a third of a density's halo was lost in the
measurement on #328). The session now draws such a cube in log10."""
from __future__ import annotations

import numpy as np
import pytest

from app.render_session import RenderSession
from app.render_session._common import SessionViewState, log_floor, to_log10

COLOR = [[-9.0, 0.0, 0.0, 0.1], [-5.0, 0.9, 0.5, 0.1]]
OPACITY = [[-9.0001, 0.0], [-9.0, 0.02], [-5.0, 0.4]]


def test_log10_with_a_floor_for_values_without_a_logarithm():
    v = np.array([1e-12, 1e-6, 0.0, -3.0, np.nan, np.inf, 10.0], dtype=np.float32)
    floor = log_floor(v)
    assert floor == pytest.approx(-13.0, abs=1e-5)
    out = to_log10(v, floor)
    assert out.dtype == np.float32 and out.shape == v.shape
    np.testing.assert_allclose(out[[0, 1, 6]], [-12.0, -6.0, 1.0], atol=1e-5)
    assert (out[2:6] == np.float32(floor)).all()
    assert v[1] == np.float32(1e-6), "the values are not changed"
    assert log_floor(np.array([0.0, -1.0, np.nan])) == 0.0


class _Image:
    def __init__(self):
        self.scalars, self.modified = None, 0
    def GetPointData(self):  # noqa: N802 - VTK's interface
        return self
    def SetScalars(self, a):  # noqa: N802
        self.scalars = a
    def Modified(self):  # noqa: N802
        self.modified += 1


class _Blocks:
    def __init__(self):
        self.log_floor, self.cleared = None, 0
    def clear(self):
        self.cleared += 1


def _session(ooc=False):
    s = object.__new__(RenderSession)
    s.session_id, s.view = "t", SessionViewState()
    s._vmin, s._vmax = 0.0, 1.0
    s._rebuild_volume_tfs_on_thread = lambda: None
    s._apply_blend_mode_on_thread = lambda m: None
    s.request_idr = lambda: None
    s.dirty = 0
    s.mark_dirty = lambda: setattr(s, "dirty", s.dirty + 1)
    s._cube_flat_view = np.array([1e-12, 1e-8, 1e-5, 0.0], dtype=np.float32)
    s._vtk_intensity_array = "linear-array"
    s._image_data = _Image()
    s._ooc = None
    if ooc:
        class _Ooc:
            blocks = _Blocks()
            def sample(self):
                return np.array([1e-11, 1e-6], dtype=np.float32)
        s._ooc = _Ooc()
    return s


def test_a_log_command_draws_log10_and_a_linear_one_the_values_again():
    pytest.importorskip("vtk")
    s = _session()
    s._apply_command_on_thread({"type": "set_transfer_functions", "color": COLOR, "opacity": OPACITY, "scale": "log"})
    assert s.view.log_scalars is True
    np.testing.assert_allclose(s._log_flat[:3], [-12.0, -8.0, -5.0], atol=1e-5)
    assert s._log_flat[3] == pytest.approx(-13.0, abs=1e-5)
    assert s._image_data.scalars is s._log_array and s.dirty == 1
    # The same scale again: nothing made anew.
    arr = s._log_array
    s._apply_command_on_thread({"type": "set_transfer_functions", "color": COLOR, "opacity": OPACITY, "scale": "log"})
    assert s._log_array is arr
    # A linear look, a LUT or a threshold: the values' array again.
    for cmd in ({"type": "set_transfer_functions", "color": [[0.0, 0, 0, 0], [1.0, 1, 1, 1]], "opacity": [[0.0, 0.0], [1.0, 1.0]]},
                {"type": "set_lut", "name": "Viridis"}, {"type": "set_threshold", "value": 0.3}):
        s._apply_command_on_thread({"type": "set_transfer_functions", "color": COLOR, "opacity": OPACITY, "scale": "log"})
        assert s.view.log_scalars
        s._apply_command_on_thread(cmd)
        assert s.view.log_scalars is False and s._image_data.scalars == "linear-array" and s._log_flat is None


def test_out_of_core_blocks_are_made_again_in_log10():
    pytest.importorskip("vtk")
    s = _session(ooc=True)
    s._apply_command_on_thread({"type": "set_transfer_functions", "color": COLOR, "opacity": OPACITY, "scale": "log"})
    blocks = s._ooc.blocks
    assert blocks.log_floor == pytest.approx(-12.0, abs=1e-5) and blocks.cleared == 1
    s._apply_command_on_thread({"type": "set_lut", "name": "Viridis"})
    assert blocks.log_floor is None and blocks.cleared == 2


def test_a_block_is_coded_in_log10_and_the_cache_keeps_its_values():
    pytest.importorskip("vtk")
    from app.render_session._ooc import BlockVolumes
    import vtk

    b = BlockVolumes(vtk.vtkVolumeProperty())
    b.log_floor = -13.0
    values = np.array([[[1e-12, 1e-6], [0.0, 1.0]]], dtype=np.float32)
    b.replace({"k": (values, (0.0, 0.0, 0.0), 1.0, (0, 1, 0, 1, 0, 0))})
    np.testing.assert_allclose(b._arrays["k"], [-12.0, -6.0, -13.0, 0.0], atol=1e-5)
    assert values[0, 0, 1] == np.float32(1e-6)


def test_a_reset_draws_the_values_again():
    pytest.importorskip("vtk")
    s = _session()
    s._nz, s._default_threshold = 4, None
    for name in ("_apply_interactive_mode_on_thread", "_apply_crop_on_thread", "_rebuild_slice_lut_on_thread",
                 "_apply_camera_on_thread", "_apply_scene_on_thread", "_apply_visibility_on_thread"):
        setattr(s, name, lambda *a, **k: None)
    s._apply_command_on_thread({"type": "set_transfer_functions", "color": COLOR, "opacity": OPACITY, "scale": "log"})
    assert s._image_data.scalars is s._log_array
    try:
        s._apply_command_on_thread({"type": "reset"})
    except AttributeError as exc:            # a stand-in this test does not provide, after the log switch
        assert "log" not in str(exc), exc
    assert s._image_data.scalars == "linear-array" and not s.view.log_scalars


def test_a_cube_streamed_in_is_made_again_in_log10_when_complete():
    pytest.importorskip("vtk")
    s = _session()
    s._apply_command_on_thread({"type": "set_transfer_functions", "color": COLOR, "opacity": OPACITY, "scale": "log"})
    first = s._log_array
    s._cube_flat_view[3] = np.float32(1e-3)           # a chunk arrived after the copy was made
    s._relog_after_load = True
    if s._relog_after_load:                            # what _poll_cube_chunks does after its events
        s._relog_after_load = False
        s._set_log_scalars_on_thread(False)
        s._set_log_scalars_on_thread(True)
    assert s._log_array is not first and s._log_flat[3] == pytest.approx(-3.0, abs=1e-5)


def test_the_log_floor_needs_no_copy_of_the_cube():
    v = np.array([[3.0, 0.0], [np.nan, 1e-4]], dtype=np.float32)
    assert log_floor(v) == pytest.approx(-5.0, abs=1e-5)
