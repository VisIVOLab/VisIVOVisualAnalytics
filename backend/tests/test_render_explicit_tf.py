"""A remote view drawn with the client's own transfer functions (issue #83):
``set_transfer_functions`` carries them point by point, so that a viewer whose
scale is not linear (a VBT density field on a log scale) shows remotely what
it shows locally."""
from __future__ import annotations

import pytest

from app.render_session import RenderSession
from app.render_session._common import (
    SessionViewState,
    _explicit_opacity_floor,
    _parse_tf_points,
)

COLOR = [[1e-6, 0.0, 0.0, 0.1], [1e-4, 0.9, 0.5, 0.1], [1e-9, 0.0, 0.0, 0.0]]
OPACITY = [[1e-9, 0.0], [1e-6, 0.0], [1e-5, 0.02], [1e-4, 0.4]]


def _session():
    s = object.__new__(RenderSession)
    s.session_id, s.view = "t", SessionViewState()
    s._vmin, s._vmax = 0.0, 1.0
    s.rebuilt, s.blends, s.idr = 0, [], 0

    def rebuild():
        s.rebuilt += 1
    s._rebuild_volume_tfs_on_thread = rebuild
    s._apply_blend_mode_on_thread = s.blends.append
    s.request_idr = lambda: setattr(s, "idr", s.idr + 1)
    return s


def test_points_are_checked_and_sorted():
    assert _parse_tf_points(COLOR, 4)[0] == (1e-9, 0.0, 0.0, 0.0)
    for bad in ([], [[1.0, 0.5]], [[float("nan"), 0, 0, 0]], "x", [[0, 0, 0, 0]] * 5000):
        with pytest.raises(ValueError):
            _parse_tf_points(bad, 4)


def test_the_opacity_floor_is_where_the_opacity_leaves_zero():
    assert _explicit_opacity_floor(sorted(OPACITY)) == 1e-6
    assert _explicit_opacity_floor([(0.0, 0.1), (1.0, 1.0)]) is None


def test_the_command_replaces_the_ramp_until_a_lut_or_threshold():
    s = _session()
    s._apply_command_on_thread({"type": "set_transfer_functions", "color": COLOR,
                                "opacity": OPACITY, "blend_mode": 1})
    color, opacity = s.view.explicit_tf
    assert color[0][0] == 1e-9 and len(opacity) == 4
    assert s.view.blend_mode == 1 and s.blends == [1]
    assert s.rebuilt == 1 and s.idr == 1
    s._apply_command_on_thread({"type": "set_threshold", "value": 0.3})
    assert s.view.explicit_tf is None
    s._apply_command_on_thread({"type": "set_transfer_functions", "color": COLOR,
                                "opacity": OPACITY})
    s._apply_command_on_thread({"type": "set_lut", "name": "Viridis"})
    assert s.view.explicit_tf is None


def test_a_malformed_command_changes_nothing():
    s = _session()
    s._apply_command_on_thread({"type": "set_transfer_functions", "color": COLOR,
                                "opacity": [[0.0, "x"]]})
    assert s.view.explicit_tf is None and s.rebuilt == 0


def test_the_functions_are_built_from_the_points():
    pytest.importorskip("vtk")
    from app.render_session._common import _build_explicit_tfs

    ctf, otf = _build_explicit_tfs(_parse_tf_points(COLOR, 4), _parse_tf_points(OPACITY, 2))
    assert ctf.GetSize() == 3 and otf.GetSize() == 4
    assert otf.GetValue(1e-6) == pytest.approx(0.0)
    assert otf.GetValue(1e-4) == pytest.approx(0.4)
    assert ctf.GetColor(1e-4) == pytest.approx((0.9, 0.5, 0.1))
