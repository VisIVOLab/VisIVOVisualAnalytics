"""Issue #64, held-out cube 2: SKA SDC2 'ldev' (Hartley+2023, MNRAS 523, 1967).

RUN 2026-11-05/06 on PLEIADI-GPU (job 1074, run_sdc2_pleiadi.sbatch): SDC2 PASS
under §7.4. Results, caveats and the pre-run scorer-adapter fixes (skiprows,
w20 convention) are in validation/out/sofia_suggest/RESULTS.md, "SDC2 ldev:
result"; outputs in validation/out/sofia_suggest/sdc2/.

Validates the REVISED rules of spec §7 (rev. 2026-11-05b: R2 spatial kernels,
C2 wide-band contsub, no dilation) on data not used to build them; criteria
P1-P4 are frozen in spec §7.4.

Drives the app on a PLEIADI compute node: the backend (worktree code, with R2/C2
implemented) answers POST /v1/sofia2/suggest on the WHOLE cube (one strided
pass), then each arm runs through POST /v1/tasks/sourcefind on 4 sub-cubes given
as ``input.region`` (the full cube needs ~2.3 x 44 GB). Arms (§7.4): S2 (the
route's suggestion; the driver refuses to run if it does not match §7), S1 (old
kernelsXY/contsub rules), H (validation/sofia_hgc44.par detection keys), D
(SoFiA defaults), S2nocont (only if C2 alone turns contsub on), S2_inv, D_inv. The SoFiA-2 catalogues are converted to the SDC2 submission format
and scored with the challenge's own scorer (``ska_sdc.Sdc2Scorer``, PyPI
``ska-sdc``) against the truth catalogue cut to the same sub-cube. No score is
computed by re-implementation (spec §4).

  python sofia_suggest_sdc2.py run    --cube CUBE --truth TRUTH --out OUT
  python sofia_suggest_sdc2.py score  --cube CUBE --truth TRUTH --out OUT

Environment: VISIVO_URL, VISIVO_TOKEN_FILE (as validation/sofia_suggest.py).
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sofia_suggest import call, hand_params  # noqa: E402  (same client as the local campaign)

C = 299792.458
HI = 1.420405752e9

# Sub-cubes (x0, x1, y0, y1, z0, z1; 0-based inclusive, SoFiA input.region).
# Fixed BEFORE looking at the truth catalogue: four 400x400 px quadrant-centred
# tiles, each over the full band split in two halves (low/high frequency: the
# noise rises 26.7 -> 31.5 uJy/beam with frequency, so both regimes are tested).
# 400*400*3334*4 B = 2.1 GB per region -> ~5 GB SoFiA memory.
REGIONS = {
    "q1_lo": (143, 542, 143, 542, 0, 3333),
    "q2_hi": (743, 1142, 143, 542, 3334, 6667),
    "q3_lo": (143, 542, 743, 1142, 0, 3333),
    "q4_hi": (743, 1142, 743, 1142, 3334, 6667),
}


# ── rules frozen in spec §7 (rev. 2026-11-05b) ────────────────────────────────

def rnd(x: float) -> int:
    """Round half up (§7.1), not Python's banker's rounding."""
    return int(math.floor(x + 0.5))


def r2_kernels_xy(b_px: float | None) -> str:
    """§7.1 R2."""
    if not b_px:
        return "0, 3, 6"
    k_max = rnd(1.12 * b_px)
    if k_max < 2:
        return "0, 3"
    k_mid = rnd(k_max / 2)
    ks = [0] + ([k_mid] if 2 <= k_mid < k_max else []) + [k_max]
    return ", ".join(str(k) for k in ks)


def old_kernels_xy(b_px: float | None, n_res: float | None) -> str:
    """The §2.1 rule as run in the §3 campaign (frozen diff d4bdafe; Python round)."""
    if not b_px:
        return "0, 3, 6"
    xy = [0, int(round(b_px)), int(round(2 * b_px))] + ([int(round(3 * b_px))] if (n_res or 0) >= 1e7 else [])
    return ", ".join(str(k) for k in xy)


W_MAX = {"hi": 120.0, "galactic": 20.0}


def c2_contsub(m: dict, line: str) -> bool:
    """§7.2 C2 OR the old 1 % test."""
    band = m.get("band_kms") or 0.0
    return band >= 20 * W_MAX.get(line, 120.0) or (m.get("cont_fraction") or 0.0) > 0.01


def arms_from(sug: dict, hand: dict) -> dict[str, dict]:
    """S2 must be what the route answers (the app implements R2/C2); S1, H, D
    and the controls are built from it as §7.4 says."""
    m, line = sug["measurements"], sug["line"]["value"]
    s2 = dict(sug["params"])
    want_xy = r2_kernels_xy(m.get("b_px"))
    want_cs = c2_contsub(m, line)
    got_cs = str(s2.get("contsub.enable", "false")).lower() == "true"
    if s2.get("scfind.kernelsXY", "").replace(" ", "") != want_xy.replace(" ", "") or got_cs != want_cs:
        raise SystemExit(f"The app's suggestion does not implement spec §7 (kernelsXY "
                         f"{s2.get('scfind.kernelsXY')!r} vs {want_xy!r}; contsub {got_cs} vs {want_cs}): "
                         "update backend/app/compute/sofia_suggest.py before running.")
    for k in [k for k in s2 if k.startswith("dilation.")]:
        s2.pop(k)
    s1 = dict(s2)
    s1["scfind.kernelsXY"] = old_kernels_xy(m.get("b_px"), m.get("n_res"))
    old_cs = (m.get("cont_fraction") or 0.0) > 0.01
    if not old_cs:
        s1["contsub.enable"] = "false"
        for k in [k for k in s1 if k.startswith("contsub.") and k != "contsub.enable"]:
            s1.pop(k)
    arms = {"S2": s2, "S1": s1, "H": dict(hand), "D": {}}
    if got_cs and not old_cs:
        arms["S2nocont"] = {**s2, "contsub.enable": "false"}
    inv = {"input.invert": "true", "reliability.enable": "false"}
    arms["S2_inv"] = {**s2, **inv}
    arms["D_inv"] = dict(inv)
    return arms


def run(cube: str, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    opened = call("POST", "/v1/datasets/open", {"path": cube})
    assert opened["valid"], opened
    did = opened["dataset_id"]
    f = out / "suggest_catalogue.json"
    if not f.exists():
        t0 = time.perf_counter()
        r = call("POST", f"/v1/sofia2/suggest/{did}", {"goal": "catalogue"}, timeout=7200)
        r["_elapsed_s"] = time.perf_counter() - t0
        f.write_text(json.dumps(r, indent=1))
    sug = json.loads(f.read_text())
    hand = hand_params(Path(__file__).resolve().parents[1] / "sofia_hgc44.par")
    arms = arms_from(sug, hand)
    (out / "arms.json").write_text(json.dumps(arms, indent=1))
    for reg, box in REGIONS.items():
        for arm, base in arms.items():
            ad = out / reg / arm
            if (ad / "result.json").exists():
                continue
            params = dict(base)
            params["input.region"] = ", ".join(str(v) for v in box)
            params["parameter.enable"] = "true"
            params["parameter.physical"] = "true"
            task = call("POST", "/v1/tasks/sourcefind", {"dataset_id": did, "params": params})
            tid = task["task_id"]
            t0 = time.perf_counter()
            while True:
                st = call("GET", f"/v1/tasks/{tid}")
                if st["status"] not in ("pending", "running"):
                    break
                time.sleep(30)
            ad.mkdir(parents=True, exist_ok=True)
            res = st.get("result") or {}
            res.pop("sources", None)
            # SoFiA-2 exit 8 = "No sources left after filtering": zero detections
            # (the app reports it as a failed task; see RESULTS.md finding 1).
            if st["status"] == "failed" and "(exit 8)" in (st.get("error") or ""):
                res.update(n_sources=0, zero_by_exit8=True)
            res.update(params=params, status=st["status"], error=st.get("error"),
                       elapsed_s=time.perf_counter() - t0)
            (ad / "result.json").write_text(json.dumps(res, indent=1))
            print(f"[{reg}/{arm}] {st['status']} {res.get('n_sources')} {res.get('summary')}", flush=True)


def to_submission(cat_xml: Path, header, dst: Path) -> int:
    """SoFiA-2 catalogue -> SDC2 submission columns
    id ra dec hi_size line_flux_integral central_freq pa i w20.
    hi_size: 2 x ell_maj (px, 2-sigma ellipse) x pixel scale (arcsec) -- an
    approximation of the HI diameter (spec: to check against Hartley+2023 §2);
    i: from the axis ratio with intrinsic q0 = 0.2;
    w20: c * f0 * w20[Hz] / freq**2 (f0 = HI rest frequency), the inverse of
    the scorer's own conversion (ska_sdc ScaleAndCalculateLargestSize:
    spectral_size[Hz] = w20 / (c * f0 / freq**2)). The plain c * w20[Hz] / freq
    is (1 + z) smaller in that convention, and the scorer takes the
    submission's w20 as the radius of its candidate search.
    These conversions affect the per-source score, not the match count much;
    report n_match / n_false / n_det alongside the score."""
    from astropy.io.votable import parse_single_table  # noqa: PLC0415
    t = parse_single_table(str(cat_xml)).to_table()
    pix = abs(float(header["CDELT1"])) * 3600.0
    rows = []
    for k, r in enumerate(t):
        q = min(1.0, float(r["ell_min"]) / float(r["ell_maj"])) if r["ell_maj"] > 0 else 1.0
        q0 = 0.2
        inc = math.degrees(math.acos(math.sqrt(max(0.0, (q * q - q0 * q0) / (1 - q0 * q0)))))
        freq = float(r["freq"]) if "freq" in t.colnames else float("nan")
        rows.append(f"{k} {float(r['ra']):.6f} {float(r['dec']):.6f} {2 * float(r['ell_maj']) * pix:.3f} "
                    f"{float(r['f_sum']):.6g} {freq:.1f} {float(r['kin_pa']) % 360:.2f} {inc:.2f} "
                    f"{C * HI * float(r['w20']) / freq ** 2:.2f}")
    dst.write_text("id ra dec hi_size line_flux_integral central_freq pa i w20\n" + "\n".join(rows) + "\n")
    return len(rows)


def score(cube: str, truth: Path, out: Path) -> None:
    from astropy.io import fits  # noqa: PLC0415
    from astropy.wcs import WCS  # noqa: PLC0415
    import numpy as np  # noqa: PLC0415
    from ska_sdc import Sdc2Scorer  # noqa: PLC0415  (the challenge's scorer, not a re-implementation)

    hdr = fits.getheader(cube)
    w = WCS(hdr)
    tt = np.genfromtxt(truth, names=True, dtype=None, encoding=None)
    summary = {}
    for reg, (x0, x1, y0, y1, z0, z1) in REGIONS.items():
        # truth cut to the sub-cube (pixel box from the cube's own WCS)
        px, py, pz = w.all_world2pix(tt["ra"], tt["dec"], tt["central_freq"], 0)
        inside = (px >= x0) & (px <= x1) & (py >= y0) & (py <= y1) & (pz >= z0) & (pz <= z1)
        tdst = out / reg / "truth_region.txt"
        tdst.parent.mkdir(parents=True, exist_ok=True)
        np.savetxt(tdst, tt[inside], fmt="%s", header=" ".join(tt.dtype.names), comments="")
        arm_names = list(json.loads((out / "arms.json").read_text()))
        for arm in [a for a in arm_names if a.endswith("_inv")]:
            # Inversion arms are reported (spec §7.4), not scored: N_inv per region.
            res = json.loads((out / reg / arm / "result.json").read_text())
            summary[f"{reg}/{arm}"] = {"status": res.get("status"), "n_det": res.get("n_sources")}
        for arm in [a for a in arm_names if not a.endswith("_inv")]:
            res = json.loads((out / reg / arm / "result.json").read_text())
            if res.get("status") != "completed" and not res.get("zero_by_exit8"):
                summary[f"{reg}/{arm}"] = {"status": res.get("status")}
                continue
            if res.get("zero_by_exit8") or not res.get("catalog_path"):
                # SoFiA-2 exit 8 or an empty catalogue: no detection, nothing to match.
                summary[f"{reg}/{arm}"] = {"n_truth": int(inside.sum()), "n_det": 0, "n_match": 0,
                                           "n_false": 0, "score": 0.0, "completeness": 0.0,
                                           "reliability": 0.0, "empty": True}
                print(reg, arm, summary[f"{reg}/{arm}"], flush=True)
                continue
            sub = out / reg / arm / "submission.txt"
            n = to_submission(Path(res["catalog_path"]), hdr, sub)
            # Both files carry their header on line 0: from_txt() reads the
            # first line it does not skip as the column names (ska-sdc 2.0.0).
            s = Sdc2Scorer.from_txt(str(sub), str(tdst), sub_skiprows=0, truth_skiprows=0).run()
            summary[f"{reg}/{arm}"] = {"n_truth": int(inside.sum()), "n_det": n, "n_match": int(s.n_match),
                                       "n_false": int(s.n_false), "score": float(s.value),
                                       "completeness": s.n_match / max(1, int(inside.sum())),
                                       "reliability": s.n_match / max(1, n)}
            print(reg, arm, summary[f"{reg}/{arm}"], flush=True)
    (out / "sdc2_scores.json").write_text(json.dumps(summary, indent=1))
    crit = evaluate(summary)
    (out / "sdc2_criteria.json").write_text(json.dumps(crit, indent=1))
    print(json.dumps(crit, indent=1))


def evaluate(summary: dict) -> dict:
    """Spec §7.4 P1-P4, frozen 2026-11-05b. A region is scored if n_truth >= 20."""
    tests = {"P1": ("D", 0.0, 0.05), "P2": ("H", 0.05, 0.05), "P3": ("S1", 0.05, 0.05),
             "P4": ("S2nocont", 0.05, 0.02)}
    out: dict = {}
    for name, (ref, tol_c, tol_r) in tests.items():
        per = {}
        for reg in REGIONS:
            a, b = summary.get(f"{reg}/S2"), summary.get(f"{reg}/{ref}")
            if not a or not b or "completeness" not in a or "completeness" not in b or a["n_truth"] < 20:
                continue
            per[reg] = (a["completeness"] >= b["completeness"] - tol_c
                        and a["reliability"] >= b["reliability"] - tol_r)
        if name == "P4" and not per:
            out[name] = "not run (C2 has no separate effect on SDC2)"
            continue
        out[name] = {"per_region": per, "n_scored": len(per),
                     "result": ("inconclusive" if len(per) < 3 else
                                "pass" if sum(per.values()) >= 3 else "fail")}
    res = [v["result"] for v in out.values() if isinstance(v, dict)]
    out["SDC2"] = "inconclusive" if "inconclusive" in res else ("pass" if all(r == "pass" for r in res) else "fail")
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("stage", choices=["run", "score"])
    ap.add_argument("--cube", required=True)
    ap.add_argument("--truth", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    if a.stage == "run":
        run(a.cube, a.out)
    else:
        score(a.cube, a.truth, a.out)


if __name__ == "__main__":
    main()
