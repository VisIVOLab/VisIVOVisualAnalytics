"""validation/vr_measure_report.py on a fixture record (issue #224). The
fixture (tests/vr/measure_fixture.jsonl) was built with the page's own
recorder; measure.test.mjs checks that its frame figures are FrameStats'.
The pooled figures here are worked out by hand from its raw values."""
from __future__ import annotations

import csv
import importlib.util
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).resolve().parent / "vr" / "measure_fixture.jsonl"


def _report():
    spec = importlib.util.spec_from_file_location("vr_measure_report", REPO / "validation" / "vr_measure_report.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_definitions_are_the_pages() -> None:
    R = _report()
    # FrameStats.stats: sorted[floor(q (n - 1))]; Rolling.quantile: sorted[min(n - 1, floor(q n))].
    assert R.frame_stats([1, 2, 3, 4])["p50"] == 2 and R.frame_stats([1, 2, 3, 4])["p95"] == 3
    assert R.rolling_quantile([1, 2, 3, 4], 0.5) == 3 and R.rolling_quantile([1, 2, 3, 4], 0.95) == 4
    assert R.frame_stats([0, 1000, 5]) == {"n": 1, "mean": 5, "p50": 5, "p95": 5}     # FrameStats keeps 0 < v < 1000
    assert R.missed_pct([10, 21, 22], 72) == pytest.approx(200 / 3)
    assert R.missed_pct([10, 21], 0) is None


def test_the_fixture_gives_the_tables_worked_out_by_hand(tmp_path) -> None:
    R = _report()
    assert R.main([str(FIXTURE), "--out", str(tmp_path)]) == 0
    rows = {(r["condition"], r["phase"]): r for r in csv.DictReader(open(tmp_path / "per_phase.csv"))}
    # The lead-in is left out; local/still pools 3 samples, 24 intervals.
    assert set(rows) == {("local", "still"), ("jpeg", "still"), ("session:local", "-")}
    loc = rows[("local", "still")]
    assert loc["samples"] == "3" and loc["frames"] == "24"
    assert float(loc["frame_mean_ms"]) == pytest.approx(363.7 / 24, abs=0.005)
    assert float(loc["frame_p50_ms"]) == 13.9 and float(loc["frame_p95_ms"]) == 14.0
    assert float(loc["missed_pct"]) == pytest.approx(100 * 2 / 24, abs=0.005)      # 30 and 28 ms > 1.5 × 13.89
    assert float(loc["fps"]) == pytest.approx(1000 / (363.7 / 24), abs=0.005)
    assert loc["held_equal"] == "yes"
    jp = rows[("jpeg", "still")]
    assert jp["pose_frame_n"] == "9" and float(jp["pose_frame_p50_ms"]) == 110 and float(jp["pose_frame_p95_ms"]) == 200
    assert jp["stop_first_n"] == "2" and float(jp["stop_first_p50_ms"]) == 170
    assert jp["stop_full_n"] == "1" and float(jp["stop_full_p50_ms"]) == 210
    assert float(jp["via_jpeg_pct"]) == 100
    assert jp["held_equal"] == "no: look"            # the fixture's last jpeg sample has another look
    md = (tmp_path / "report.md").read_text()
    assert "Loads: 1 (failed 0), p50 2400 ms" in md and "Pairing link: down 1, up 1" in md
    assert (tmp_path / "frame_time.png").stat().st_size > 5000


def test_a_gap_told_between_samples_marks_the_phase_not_held() -> None:
    R = _report()
    p = {"condition": "webrtc", "phase": "still", "part": "measure"}
    rec = {"id": "x", "samples": [{"k": "s", "proto": p, "iv": [14] * 72, "frames": {"n": 72, "target_hz": 72},
                                   "refine": {"via": "webrtc"}}],
           "events": [{"k": "e", "name": "condition-degraded", "proto": p, "data": p}]}
    assert R.summarise(rec)[0]["held_equal"] == "no: transport"
    rec["events"] = []
    assert R.summarise(rec)[0]["held_equal"] == "yes"
