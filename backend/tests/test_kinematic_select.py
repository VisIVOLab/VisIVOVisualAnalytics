

def test_lasso_levels_replay_select():
    """The slider's level field thresholded at a budget is the single-budget grow,
    voxel for voxel — what lets a recipe replay a GUI selection through
    /v1/cube/kinematic_select_bin from the recorded seed and parameters."""
    import numpy as np
    from app.compute.selection import cube_kinematic_levels, cube_kinematic_select
    rng = np.random.default_rng(5)
    data = (rng.normal(0.0, 1.0, (24, 40, 40))).astype(np.float32)
    zz, yy, xx = np.mgrid[0:24, 0:40, 0:40]
    blob = np.exp(-(((xx - 20) ** 2 + (yy - 18) ** 2) / 40.0 + ((zz - 12) ** 2) / 12.0))
    data += (12.0 * blob).astype(np.float32)
    seed = (12, 18, 20)
    for reach in (4.0, 8.0, 16.0):
        sel = cube_kinematic_select(data, seed, reach=reach, lambda_v=1.0, nsigma=2.5)
        out = cube_kinematic_levels(data, [seed], [], reach_max=64.0, lambda_v=1.0, nsigma=2.5)
        oz, oy, ox = out["origin"]
        add = out["add_levels"]
        m = np.zeros_like(sel)
        m[oz:oz + add.shape[0], oy:oy + add.shape[1], ox:ox + add.shape[2]] = \
            (add <= reach) & (add != out["unreachable"])
        assert sel.sum() > 50
        assert np.array_equal(m, sel), f"reach {reach}: {int((m ^ sel).sum())} voxels differ"


def test_lasso_replay_passes_the_level_field_budget(monkeypatch):
    """When the node cap trims the graph, a single-budget grow and the slider's
    level field differ unless the grow is given the same shell budget — which
    is why the recipe records reach_max and the replay passes it."""
    import numpy as np
    from app.compute import selection
    from app.compute.selection import cube_kinematic_levels, cube_kinematic_select
    monkeypatch.setattr(selection, "_MAX_SELECT_NODES", 6000)
    data = np.zeros((32, 64, 64), dtype=np.float32)
    data[16, 32, 15:50] = 20.0
    seed = (16, 32, 32)
    out = cube_kinematic_levels(data, [seed], [], reach_max=64.0, sigma=1.0)
    oz, oy, ox = out["origin"]
    add = out["add_levels"]
    levels = np.zeros(data.shape, dtype=bool)
    levels[oz:oz + add.shape[0], oy:oy + add.shape[1], ox:ox + add.shape[2]] = \
        (add <= 4) & (add != out["unreachable"])
    replay = cube_kinematic_select(data, seed, reach=4.0, sigma=1.0, margin_reach=64.0)
    assert np.array_equal(replay, levels)
