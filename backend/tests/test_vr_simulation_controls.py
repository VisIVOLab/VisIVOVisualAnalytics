"""The headset's simulation controls (issue #259): a snapshot series beside a
table's file, each particle's size from a column (a smoothing length), a box
selection as a filter (inside or outside), and the column units for the
colour bar."""
from __future__ import annotations

import math

import numpy as np
import pytest

from app import point_sample as ps
from app import snapshot_series as ss
from app.point_table import open_point_table


def _vbt(folder, name, n=4000, seed=5):
    rng = np.random.default_rng(seed)
    x, y, z = rng.uniform(-10, 10, n), rng.uniform(0, 5, n), rng.uniform(100, 102, n)
    h = np.exp(rng.normal(-1, 0.7, n))           # a smoothing length
    h[3] = 0.0                                    # not positive: the median is taken
    h[4] = np.nan
    data = np.stack([x, y, z, h]).astype("<f4")
    data.tofile(folder / f"{name}.bin")
    (folder / f"{name}.bin.head").write_text(f"float\n4\n{n}\nlittle\nX\nY\nZ\nhsml\n")
    return str(folder / f"{name}.bin.head"), data


# ── Snapshot series ─────────────────────────────────────────────────────────

def test_series_is_the_names_that_differ_only_in_their_number():
    names = ["snap_010.bin.head", "snap_002.bin.head", "snap_1000.bin.head", "snap_010.bin",
             "snapx_003.bin.head", "snap_004.txt", "other.bin.head", "snap_007.bin.head.bak"]
    got = ss.members("snap_010.bin.head", names)
    assert [n for _, n in got] == ["snap_002.bin.head", "snap_010.bin.head", "snap_1000.bin.head"]
    assert [k for k, _ in got] == [2, 10, 1000]
    # An extension's digits never count (snap.hdf5 has no number of its own).
    assert ss.members("snap.hdf5", ["snap.hdf5", "snap.hdf4"]) == [(0, "snap.hdf5")]
    # The last digit run of the stem: run2_out_0042.csv steps 0042, not 2.
    got = ss.members("run2_out_0042.csv", ["run2_out_0041.csv", "run3_out_0042.csv", "run2_out_0043.csv"])
    assert [n for _, n in got] == ["run2_out_0041.csv", "run2_out_0042.csv", "run2_out_0043.csv"]


def test_snapshot_series_lists_the_folder(tmp_path):
    for k in (0, 1, 2, 5):
        _vbt(tmp_path, f"snap_{k:03d}", n=10)
    s = ss.snapshot_series(str(tmp_path / "snap_002.bin.head"))
    assert s["count"] == 4 and s["numbers"] == [0, 1, 2, 5] and s["index"] == 2
    assert s["paths"][3].endswith("snap_005.bin.head")
    one = ss.snapshot_series(str(tmp_path / "missing" / "a.bin.head"))
    assert one["count"] == 1 and one["index"] == 0


# ── Sizes and box filters in the sample ─────────────────────────────────────

def test_sizes_are_log_codes_over_the_positive_range(tmp_path):
    head, data = _vbt(tmp_path, "sized")
    t = open_point_table(head)
    s = ps.sample_points(t, 10_000, colour_field="hsml", size_field="hsml")   # every row: the range is the column's
    assert s.size is not None and len(s.size) == len(s.positions)
    h = data[3].astype(np.float64)
    ok = np.isfinite(h) & (h > 0)
    lo, hi = s.size_range
    assert lo == pytest.approx(h[ok].min(), rel=1e-6) and hi == pytest.approx(h[ok].max(), rel=1e-6)
    body = s.encode()
    assert len(body) == len(s.positions) * 10            # 6 position + 2 colour + 2 size bytes
    # Decoded sizes are the column's within the 16-bit log step.
    decoded = 10 ** (math.log10(lo) + s.size.astype(np.float64) / 65535 * (math.log10(hi) - math.log10(lo)))
    assert np.all(decoded >= lo * 0.9999) and np.all(decoded <= hi * 1.0001)
    h_ = ps.headers(s)
    assert h_["X-Visivo-Size-Field"] == "hsml" and len(h_["X-Visivo-Size-Range"].split(",")) == 2
    with pytest.raises(ValueError):
        ps.sample_points(t, 10, size_field="nope")
    with pytest.raises(ValueError):
        ps.size_codes(np.array([0.0, -1.0, np.nan]))


def test_a_size_column_without_positive_values_gives_uniform_splats_and_a_note(tmp_path):
    rng = np.random.default_rng(2)
    n = 300
    data = np.stack([rng.uniform(0, 1, n), rng.uniform(0, 1, n), rng.uniform(0, 1, n), np.zeros(n)]).astype("<f4")
    data.tofile(tmp_path / "flat.bin")
    (tmp_path / "flat.bin.head").write_text(f"float\n4\n{n}\nlittle\nX\nY\nZ\nzero\n")
    s = ps.sample_points(open_point_table(str(tmp_path / "flat.bin.head")), 1000, size_field="zero")
    assert s.size is None and s.size_field is None and "no positive value" in s.size_note
    h = ps.headers(s)
    assert "X-Visivo-Size-Field" not in h and "no positive value" in h["X-Visivo-Size-Note"]
    assert len(s.encode()) == n * 6


def test_a_long_series_keeps_the_nearest_and_the_allowed(tmp_path, monkeypatch):
    for k in range(0, 40):
        (tmp_path / f"s_{k:03d}.csv").write_text("x\n")
    monkeypatch.setattr(ss, "MAX_MEMBERS", 5)
    s = ss.snapshot_series(str(tmp_path / "s_020.csv"))
    assert s["numbers"] == [18, 19, 20, 21, 22] and s["index"] == 2
    s = ss.snapshot_series(str(tmp_path / "s_001.csv"))
    assert s["numbers"] == [0, 1, 2, 3, 4] and s["index"] == 1
    # A member the data roots refuse (a link outside them) is left out; the open one stays.
    s = ss.snapshot_series(str(tmp_path / "s_020.csv"), allowed=lambda p: not p.endswith(("s_021.csv", "s_020.csv")))
    assert 21 not in s["numbers"] and 20 in s["numbers"]


def test_a_size_codes_round_trip():
    v = np.array([0.01, 0.1, 1.0, 10.0, -3.0])
    codes, (lo, hi) = ps.size_codes(v)
    assert (lo, hi) == (0.01, 10.0)
    assert codes[0] == 0 and codes[3] == 65535
    back = 10 ** (math.log10(lo) + codes / 65535 * (math.log10(hi) - math.log10(lo)))
    np.testing.assert_allclose(back[:4], v[:4], rtol=2e-4)
    assert back[4] == pytest.approx(np.median([0.01, 0.1, 1.0, 10.0]), rel=2e-4)


def test_box_filters_keep_the_inside_or_the_outside(tmp_path):
    head, data = _vbt(tmp_path, "boxed")
    t = open_point_table(head)
    box = {"fields": ["X", "Y", "Z"], "min": [-5, 1, 100.5], "max": [5, 4, 101.5]}
    x, y, z = data[0], data[1], data[2]
    inside = (x >= -5) & (x <= 5) & (y >= 1) & (y <= 4) & (z >= 100.5) & (z <= 101.5)
    s_in = ps.sample_points(t, 10_000, filters=[{"box": box, "op": "inside"}])
    s_out = ps.sample_points(t, 10_000, filters=[{"box": box, "op": "outside"}])
    assert len(s_in.positions) == int(inside.sum())
    assert len(s_out.positions) == int((~inside).sum())
    with pytest.raises(ValueError):
        ps.sample_points(t, 10, filters=[{"box": box, "op": "around"}])
    with pytest.raises(ValueError):
        ps.sample_points(t, 10, filters=[{"box": {"fields": ["Q"], "min": [0], "max": [1]}}])


# ── Routes ──────────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_series_sizes_and_units_routes(client, auth_headers, tmp_path):
    for k in (1, 2, 3):
        _vbt(tmp_path, f"run_{k:02d}", n=500, seed=k)
    head = str(tmp_path / "run_02.bin.head")
    did = (await client.post("/v1/vbt/open", json={"path": head}, headers=auth_headers)).json()["dataset_id"]
    s = (await client.post("/v1/points/series", json={"dataset_id": did}, headers=auth_headers)).json()
    assert s["count"] == 3 and s["index"] == 1 and s["paths"][2].endswith("run_03.bin.head")
    r = await client.post("/v1/points/sample_bin", json={"dataset_id": did, "max_points": 300,
                          "color_field": "hsml", "size_field": "hsml"}, headers=auth_headers)
    assert r.status_code == 200 and len(r.content) == 300 * 10
    assert r.headers["x-visivo-size-field"] == "hsml"
    r = await client.post("/v1/points/sample_bin", json={"dataset_id": did, "max_points": 300,
                          "filters": [{"box": {"fields": ["X", "Y", "Z"], "min": [0, 0, 100], "max": [10, 5, 102]},
                                       "op": "outside"}]}, headers=auth_headers)
    assert r.status_code == 200 and int(r.headers["x-visivo-count"]) < 300
    info = (await client.post("/v1/points/info", json={"dataset_id": did}, headers=auth_headers)).json()
    assert info["units"] == {}
    # A catalogue's declared units (IPAC header) reach the page.
    tbl = tmp_path / "halos.tbl"
    tbl.write_text("|x    |y    |z    |mass    |\n|double|double|double|double |\n"
                   "|Mpc  |Mpc  |Mpc  |Msun    |\n 1.0   2.0   3.0   1e12\n 2.0   3.0   4.0   2e12\n")
    cat = (await client.post("/v1/catalogue/open", json={"path": str(tbl), "format": "ipac"},
                             headers=auth_headers)).json()
    assert cat["valid"], cat
    info = (await client.post("/v1/points/info", json={"dataset_id": cat["dataset_id"]}, headers=auth_headers)).json()
    assert info.get("valid"), info
    assert info["units"].get("mass") == "Msun"


@pytest.mark.asyncio
async def test_series_leaves_out_links_outside_the_data_roots(client, auth_headers, tmp_path, monkeypatch):
    root, outside = tmp_path / "root", tmp_path / "outside"
    root.mkdir()
    outside.mkdir()
    for k in (1, 2):
        _vbt(root, f"snap_{k:03d}", n=50, seed=k)
    _vbt(outside, "snap_003", n=50, seed=3)
    (root / "snap_003.bin.head").symlink_to(outside / "snap_003.bin.head")
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(root))
    did = (await client.post("/v1/vbt/open", json={"path": str(root / "snap_001.bin.head")},
                             headers=auth_headers)).json()["dataset_id"]
    s = (await client.post("/v1/points/series", json={"dataset_id": did}, headers=auth_headers)).json()
    assert s["numbers"] == [1, 2], s

