"""Issue #64 validation: SoFiA-2 parameters suggested by the app vs hand-tuned,
SoFiA defaults and published/independent reference masks.

Protocol: issue64 spec §3 (rev. 2026-11-05, criteria frozen before the runs).
Paper-shows-the-app: this script only DRIVES the app.

  * the suggestion comes from ``POST /v1/sofia2/suggest/{id}`` of a backend
    started from the worktree (the route the SoFiA-2 dialog calls);
  * every SoFiA-2 run goes through ``POST /v1/tasks/sourcefind`` (the app's
    finder plugin writes the .par; SoFiA-2 2.7.0 = sofiapipeline/sofia2 via
    VISIVO_SOFIA_BIN);
  * flux and w20 on every mask (references included) come from
    ``app.compute.kinematic_fit.source_measures`` with the cube's velocity axis
    (``cube_velocity_axis``) and beam area exactly as the kinematic-fit route
    loads them (``_load_selection``).

Stages (each cached in validation/out/sofia_suggest/):
  run      open the cubes, ask for suggestions, run every arm (needs the backend)
  measure  per-source matching, Dice, flux, w20, completeness, inversion test
  report   the per-criterion pass/fail tables (results .md)

  VISIVO_URL=http://127.0.0.1:18764 VISIVO_TOKEN_FILE=... \
      python validation/sofia_suggest.py run|measure|report [--cube hcg44 ...]
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import sys
import time
import urllib.request
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
OUT = HERE / "out" / "sofia_suggest"
REF = Path(os.environ.get("VISIVO_SOFIA_REF_DIR",
                          "/Users/fvitello/Documents/GitHub/VisIVO-next/validation/sofia_out"))
DEMO = Path(os.environ.get("VISIVO_DEMO_DIR", str(Path.home() / "Downloads" / "DemoData")))

CUBES = {
    "hcg44": {
        "path": DEMO / "hgc44_3axes.fits",
        "hand": HERE / "sofia_hgc44.par",
        "refs": {"R_sof": REF / "sofia_hgc44_mask.fits",
                 "R_pub": REF / "serra2013_hcg44_mask.fits"},
        # rows A-E: SoFiA-2 reference ids ↔ Serra+2013 published ids (spec §3.2)
        "rows": {"A": (5, 13), "B": (7, 1), "C": (12, 8), "D": (14, 3), "E": (8, 7)},
        "arms": ["S", "H", "D", "Sdil", "S_inv", "H_inv", "D_inv", "S_roff", "H_roff"],
    },
    "wallaby": {
        "path": DEMO / "WALLABY_J100903-290239_Hydra_TR2_cube.fits",
        "hand": HERE / "sofia_wallaby.par",
        "refs": {"R_sof": REF / "sofia_wallaby_mask.fits"},
        "disc_ref_id": 1,               # NGC 3137 in R_sof (153 227 voxels)
        # release catalogue (WALLABY PDR1, CASDA AS102 sourcefind_cat_v01, id 21):
        "release": {"n_pix": 299562, "f_sum_jy_hz": 673807.506087966, "freq_hz": 1.41515804801412e9,
                    "w20_hz": 1245500.23265825, "flux_jykms_radio": 142.2},
        "arms": ["S", "H", "D", "Sdil", "S45", "S_inv", "H_inv", "D_inv", "S45_inv", "H_roff"],
    },
    "apertif": {
        "path": DEMO / "3D_Ape-DR1_200426041_AP_B021_HI_image_cube0.fits",
        "hand": None,
        "refs": {},
        "arms": ["S", "D", "S_inv", "D_inv", "S_roff"],
    },
}


# ── backend client ─────────────────────────────────────────────────────────────

def _token() -> str:
    tok = os.environ.get("VISIVO_TOKEN")
    if tok:
        return tok
    return Path(os.environ["VISIVO_TOKEN_FILE"]).read_text().strip()


def call(method: str, route: str, body: dict | None = None, timeout: float = 3600) -> dict:
    url = os.environ.get("VISIVO_URL", "http://127.0.0.1:18764") + route
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"X-Visivo-Token": _token(),
                                          "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def hand_params(par: Path) -> dict[str, str]:
    """The hand-tuned .par's detection keys (input/output are set by the app)."""
    out = {}
    for line in par.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if "=" not in line:
            continue
        k, v = (s.strip() for s in line.split("=", 1))
        if k.split(".")[0] in ("input", "output"):
            continue
        out[k] = v
    return out


def arm_params(arm: str, sug_cat: dict, sug_flux: dict, hand: dict | None) -> dict[str, str]:
    base, _, mod = arm.partition("_")
    if base == "S":
        p = dict(sug_cat["params"])
    elif base == "Sdil":
        p = dict(sug_flux["params"])
    elif base == "S45":
        p = dict(sug_cat["params"]); p["scfind.threshold"] = "4.5"; p["reliability.enable"] = "false"
    elif base == "H":
        p = dict(hand)
    elif base == "D":
        p = {}
    else:
        raise ValueError(arm)
    if mod == "inv":    # inversion test: inverted cube, reliability off, same filter, no dilation
        p["input.invert"] = "true"; p["reliability.enable"] = "false"; p["dilation.enable"] = "false"
    elif mod == "roff":  # like-for-like positive count for the inversion purity
        p["reliability.enable"] = "false"
    return p


def run_stage(cubes: list[str]) -> None:
    for name in cubes:
        cfg = CUBES[name]
        d = OUT / name
        d.mkdir(parents=True, exist_ok=True)
        opened = call("POST", "/v1/datasets/open", {"path": str(cfg["path"])})
        assert opened["valid"], opened
        did = opened["dataset_id"]
        sug = {}
        for goal in ("catalogue", "flux"):
            f = d / f"suggest_{goal}.json"
            t0 = time.perf_counter()
            r = call("POST", f"/v1/sofia2/suggest/{did}", {"goal": goal})
            r["_elapsed_s"] = time.perf_counter() - t0
            f.write_text(json.dumps(r, indent=1))
            sug[goal] = r
        hand = hand_params(cfg["hand"]) if cfg["hand"] else None
        for arm in cfg["arms"]:
            ad = d / arm
            if (ad / "result.json").exists():
                print(f"[{name}/{arm}] cached"); continue
            params = arm_params(arm, sug["catalogue"], sug["flux"], hand)
            t0 = time.perf_counter()
            task = call("POST", "/v1/tasks/sourcefind", {"dataset_id": did, "params": params})
            tid = task["task_id"]
            while True:
                st = call("GET", f"/v1/tasks/{tid}")
                if st["status"] not in ("pending", "running"):
                    break
                time.sleep(5)
            el = time.perf_counter() - t0
            if st["status"] != "completed":
                print(f"[{name}/{arm}] {st['status']}: {st['error']}")
                ad.mkdir(exist_ok=True)
                (ad / "failed.json").write_text(json.dumps({"params": params, "status": st}, indent=1))
                continue
            res = st["result"]
            ad.mkdir(exist_ok=True)
            run_dir = Path(res["log_path"]).parent
            for fn in ("sofia.par", "sofia.log", "sofia_out_cat.xml"):
                if (run_dir / fn).exists():
                    shutil.copy(run_dir / fn, ad / fn)
            res = {k: v for k, v in res.items() if k != "sources"}
            res.update(params=params, elapsed_s=el, task_id=tid)
            (ad / "result.json").write_text(json.dumps(res, indent=1))
            print(f"[{name}/{arm}] {res['n_sources']} sources, {el:.0f} s, {res['summary']}")


# ── measurement (the app's own functions) ──────────────────────────────────────

def _app():
    sys.path.insert(0, str(HERE.parent / "backend"))
    from app.compute import kinematic_fit as kf  # noqa: PLC0415
    from app.compute._common import beam_area_pixels, is_per_beam_bunit  # noqa: PLC0415
    from app.fits_dataset import ScientificFitsDataset  # noqa: PLC0415
    return kf, beam_area_pixels, is_per_beam_bunit, ScientificFitsDataset


class Cube:
    """The cube as the kinematic-fit route loads it: (z, y, x) float32, its
    velocity axis and per-channel widths, beam area in pixels."""

    def __init__(self, path: Path):
        kf, beam_area_pixels, is_per_beam_bunit, SFD = _app()
        self.kf = kf
        ds = SFD(str(path))
        hdul, raw = ds.open_memmap()
        self.data = np.asarray(ds.squeeze_to_3d(raw), dtype=np.float32)
        hdul.close()
        nz = self.data.shape[0]
        self.axis = kf.cube_velocity_axis(ds, nz)
        b = ds.beam
        self.beam_px = (beam_area_pixels(b.major, b.minor, ds.spatial_pixel_scale_arcsec(),
                                         ds.spatial_pixel_area_arcsec2())
                        if is_per_beam_bunit(ds.bunit) and b.major and b.minor else None)
        self.sigma = 1.0   # only the velocity-field cut uses it; flux/w20 do not

    def measure(self, mask: np.ndarray) -> dict:
        idx = np.nonzero(mask)
        if idx[0].size == 0:
            return {"flux": float("nan"), "w20": float("nan"), "n_pix": 0}
        sl = tuple(slice(int(i.min()), int(i.max()) + 1) for i in idx)
        zs = sl[0]
        m = self.kf.source_measures(self.data[sl], mask[sl], self.axis["vel"][zs], self.axis["dvk"][zs],
                                    self.beam_px, self.sigma)
        return {"flux": m["flux"], "w20": m["w20"], "n_pix": int(idx[0].size),
                "z_span": int(idx[0].max() - idx[0].min() + 1)}


def load_labels(path: Path) -> np.ndarray:
    from astropy.io import fits  # noqa: PLC0415
    with fits.open(path, memmap=True) as h:
        a = np.asarray(h[0].data)
    while a.ndim > 3:
        a = a[0]
    return a.astype(np.int32)


def overlaps(a: np.ndarray, b: np.ndarray) -> dict[tuple[int, int], int]:
    sel = (a > 0) & (b > 0)
    av, bv = a[sel].astype(np.int64), b[sel].astype(np.int64)
    key = av * (int(b.max()) + 1) + bv
    u, c = np.unique(key, return_counts=True)
    nb = int(b.max()) + 1
    return {(int(k // nb), int(k % nb)): int(n) for k, n in zip(u, c)}


def sizes(a: np.ndarray) -> dict[int, int]:
    u, c = np.unique(a[a > 0], return_counts=True)
    return dict(zip(u.tolist(), c.tolist()))


def matches(ov: dict, na: dict, nb: dict) -> dict[int, list[int]]:
    """For each b id, the a ids matched to it (overlap ≥ 50 % of the smaller)."""
    out: dict[int, list[int]] = {}
    for (ia, ib), n in ov.items():
        if n >= 0.5 * min(na[ia], nb[ib]):
            out.setdefault(ib, []).append(ia)
    return out


def dice(a: np.ndarray, b: np.ndarray) -> float:
    s = int(a.sum()) + int(b.sum())
    return 2.0 * int((a & b).sum()) / s if s else float("nan")


def catalogue(arm_dir: Path) -> dict[int, dict]:
    sys.path.insert(0, str(HERE.parent / "backend"))
    from app.compute.sofia import _parse_votable_rows  # noqa: PLC0415
    p = arm_dir / "sofia_out_cat.xml"
    return {int(r["id"]): r for r in _parse_votable_rows(p)} if p.exists() else {}


def arm_mask(name: str, arm: str) -> np.ndarray | None:
    r = OUT / name / arm / "result.json"
    if not r.exists():
        return None
    mp = json.loads(r.read_text())["mask_path"]
    return load_labels(Path(mp)) if mp else None


def measure_hcg44(cube: Cube) -> dict:
    cfg = CUBES["hcg44"]
    R_sof, R_pub = (load_labels(cfg["refs"][k]) for k in ("R_sof", "R_pub"))
    n_sof, n_pub = sizes(R_sof), sizes(R_pub)
    out: dict = {"rows": {}, "arms": {}}
    # reference rows, measured with the same function
    for row, (i_sof, i_pub) in cfg["rows"].items():
        ms, mp = R_sof == i_sof, R_pub == i_pub
        out["rows"][row] = {"R_sof": cube.measure(ms), "R_pub": cube.measure(mp), "dice_ref": dice(ms, mp)}
    H = arm_mask("hcg44", "H")
    cat_H = catalogue(OUT / "hcg44" / "H")
    for arm in ("S", "H", "D", "Sdil"):
        A = arm_mask("hcg44", arm)
        if A is None:
            continue
        nA = sizes(A)
        cat = catalogue(OUT / "hcg44" / arm)
        m_sof = matches(overlaps(A, R_sof), nA, n_sof)
        rows = {}
        for row, (i_sof, i_pub) in cfg["rows"].items():
            ids = m_sof.get(i_sof, [])
            u = np.isin(A, ids) if ids else np.zeros(A.shape, bool)
            rows[row] = {"ids": ids, "dice_pub": dice(u, R_pub == i_pub), "dice_sof": dice(u, R_sof == i_sof),
                         **cube.measure(u)}
        # completeness vs H (H sources with snr >= 5) and reliability vs H ∪ R_pub
        nH = sizes(H)
        m_H = matches(overlaps(A, H), nA, nH)          # H id -> A ids
        h5 = [i for i, r in cat_H.items() if (r.get("snr") or 0) >= 5]
        compl = sum(1 for i in h5 if m_H.get(i)) / len(h5) if h5 else float("nan")
        # supplementary (not a criterion): H sources whose match is an A source
        # that also matches another H source, i.e. found only merged
        owners: dict[int, set] = {}
        for ih, ids in m_H.items():
            for ia in ids:
                owners.setdefault(ia, set()).add(ih)
        merged_H = sorted(i for i in h5 if m_H.get(i) and all(len(owners[a]) > 1 for a in m_H[i]))
        in_H = {ia for ids in m_H.values() for ia in ids}
        in_pub = {ia for ids in matches(overlaps(A, R_pub), nA, n_pub).values() for ia in ids}
        unmatched = sorted(set(nA) - in_H - in_pub) if arm != "H" else sorted(set(nA) - in_pub)
        out["arms"][arm] = {
            "n_sources": len(nA), "rows": rows,
            "completeness_vs_H_snr5": compl, "n_H_snr5": len(h5),
            "missed_H_snr5": [i for i in h5 if not m_H.get(i)],
            "merged_H_snr5": merged_H,
            "reliability_vs_H_or_pub": 1 - len(unmatched) / len(nA) if nA else float("nan"),
            "unmatched": [{"id": i, "n_pix": nA[i], "snr_max": cat.get(i, {}).get("snr_max"),
                           "snr": cat.get(i, {}).get("snr"), "rel": cat.get(i, {}).get("rel")}
                          for i in unmatched],
        }
    # "unmatched H" on the same footing as S: H sources matched neither in S nor in R_pub
    S = arm_mask("hcg44", "S")
    if S is not None and H is not None:
        nS = sizes(S); nH = sizes(H)
        in_S = {ih for ids in matches(overlaps(H, S), nH, nS).values() for ih in ids}
        in_pub = {ih for ids in matches(overlaps(H, R_pub), nH, n_pub).values() for ih in ids}
        out["unmatched_H_vs_S_or_pub"] = sorted(set(nH) - in_S - in_pub)
        out["unmatched_S_vs_H_or_pub"] = [u["id"] for u in out["arms"]["S"]["unmatched"]]
    out["inversion"] = inversion("hcg44", {"S": "S_roff", "H": "H_roff", "D": "D"})
    return out


def inversion(name: str, positive_arm: dict[str, str]) -> dict:
    """purity = 1 − N_inv / N_pos, N_pos from the same parameters with reliability
    off (like for like: the inverted run has reliability off). Also against the
    catalogue each arm actually delivers."""
    out = {}
    for arm, pos in positive_arm.items():
        inv, p, deliv = (OUT / name / x / "result.json" for x in (f"{arm}_inv", pos, arm))
        failed = OUT / name / f"{arm}_inv" / "failed.json"
        if not p.exists():
            continue
        if inv.exists():
            n_inv = json.loads(inv.read_text())["n_sources"]
        elif failed.exists() and ("No sources left after filtering" in failed.read_text()
                                  or "failed (exit 8)" in failed.read_text()):
            # SoFiA-2 exits 8 when nothing survives the filter; the app's task
            # reports that as a failure (finders/sofia2.run). For this test it
            # IS the measurement: zero inverted detections. Exit code 8 is
            # SoFiA-2's "No sources left after filtering" (the app keeps only
            # the first 500 characters of stderr, which can be all warnings).
            n_inv = 0
        else:
            continue
        n_pos = json.loads(p.read_text())["n_sources"]
        n_del = json.loads(deliv.read_text())["n_sources"] if deliv.exists() else None
        out[arm] = {"n_inv": n_inv, "n_pos_reloff": n_pos, "purity": 1 - n_inv / n_pos if n_pos else float("nan"),
                    "n_delivered": n_del,
                    "purity_vs_delivered": (1 - n_inv / n_del) if n_del else float("nan")}
    return out


def measure_wallaby(cube: Cube) -> dict:
    cfg = CUBES["wallaby"]
    R = load_labels(cfg["refs"]["R_sof"])
    nR = sizes(R)
    disc = cfg["disc_ref_id"]
    rel = cfg["release"]
    c = 299792.458
    out = {"R_sof_disc": cube.measure(R == disc), "R_sof_n": len(nR), "release": dict(rel)}
    # release w20 (Hz) as optical rest-frame-free km/s at the source frequency, both conventions
    out["release"]["w20_kms_c_dnu_over_nu"] = c * rel["w20_hz"] / rel["freq_hz"]
    out["release"]["flux_jykms_optical"] = rel["f_sum_jy_hz"] * c * 1.420405752e9 / rel["freq_hz"] ** 2
    faint = sorted((i for i in nR if i != disc), key=lambda i: -nR[i])
    out["arms"] = {}
    for arm in ("S", "H", "D", "Sdil", "S45"):
        A = arm_mask("wallaby", arm)
        if A is None:
            continue
        nA = sizes(A)
        m = matches(overlaps(A, R), nA, nR)
        ids = m.get(disc, [])
        u = np.isin(A, ids) if ids else np.zeros(A.shape, bool)
        per_ref = {}
        for i in nR:
            jj = m.get(i, [])
            uu = np.isin(A, jj) if jj else np.zeros(A.shape, bool)
            shared = bool(jj) and all(any(a in m.get(k, []) for k in nR if k != i) for a in jj)
            per_ref[i] = {"ref_n_pix": nR[i], "matched_ids": jj, "dice": dice(uu, R == i) if jj else 0.0,
                          "only_merged": shared}
        in_R = {ia for jj in m.values() for ia in jj}
        cat = catalogue(OUT / "wallaby" / arm)
        out["arms"][arm] = {"n_sources": len(nA), "disc_ids": ids, "disc_dice": dice(u, R == disc),
                            "disc": cube.measure(u), "per_ref": per_ref,
                            "completeness_vs_R_sof": sum(1 for i in nR if m.get(i)) / len(nR),
                            "unmatched": [{"id": i, "n_pix": nA[i], "snr_max": cat.get(i, {}).get("snr_max"),
                                           "rel": cat.get(i, {}).get("rel")} for i in sorted(set(nA) - in_R)]}
    out["inversion"] = inversion("wallaby", {"S": "S", "H": "H_roff", "D": "D", "S45": "S45"})
    out["ref_ids_by_size"] = [disc] + faint
    return out


def measure_apertif() -> dict:
    out = {"inversion": inversion("apertif", {"S": "S_roff", "D": "D"})}
    for arm in ("S", "D"):
        cat = catalogue(OUT / "apertif" / arm)
        top = sorted(cat.values(), key=lambda r: -(r.get("snr") or 0))[:10]
        out[f"top10_{arm}"] = [{k: r.get(k) for k in ("id", "x", "y", "z", "n_pix", "snr", "snr_max", "rel",
                                                       "ra", "dec", "w20")} for r in top]
    return out


def apertif_edge_noise() -> dict:
    """Does the noise rise at the field edge (PB-corrected cube)? Negative-side σ
    (1.4826·MAD of x<0 about 0, × 1/√(1-2/π)…: use the suggester's own estimator)."""
    sys.path.insert(0, str(HERE.parent / "backend"))
    from astropy.io import fits  # noqa: PLC0415
    with fits.open(CUBES["apertif"]["path"], memmap=True) as h:
        d = h[0].data
        ny, nx = d.shape[1:]
        chans = np.linspace(50, d.shape[0] - 50, 24).astype(int)
        sub = np.asarray(d[chans], dtype=np.float32)
    yy, xx = np.mgrid[:ny, :nx]
    r = np.hypot(yy - ny / 2, xx - nx / 2) / (nx / 2)
    rings = []
    for lo, hi in ((0, .2), (.2, .4), (.4, .6), (.6, .8), (.8, 1.0), (1.0, 1.42)):
        sel = (r >= lo) & (r < hi)
        v = sub[:, sel].ravel()
        v = v[np.isfinite(v)]
        neg = v[v < 0]
        # negative side about zero: σ = sqrt(mean(x²)) of x<0 (Gaussian half), plus MAD of the mirrored sample
        mirrored = np.concatenate([neg, -neg])
        sig = 1.4826 * np.median(np.abs(mirrored)) if mirrored.size else float("nan")
        rings.append({"r_over_halfwidth": [lo, hi], "n": int(v.size), "sigma_neg": float(sig),
                      "f_nan": float(1 - v.size / max(1, sub[:, sel].size))})
    return {"channels": chans.tolist(), "rings": rings}


def measure_stage(cubes: list[str]) -> None:
    for name in cubes:
        t0 = time.perf_counter()
        if name == "hcg44":
            res = measure_hcg44(Cube(CUBES[name]["path"]))
        elif name == "wallaby":
            res = measure_wallaby(Cube(CUBES[name]["path"]))
        else:
            res = measure_apertif()
            res["edge_noise"] = apertif_edge_noise()
        res["_elapsed_s"] = time.perf_counter() - t0
        (OUT / name / "measures.json").write_text(json.dumps(res, indent=1, default=float))
        print(f"[{name}] measured in {res['_elapsed_s']:.0f} s")


# ── criteria (spec §3.2, frozen 2026-11-05 before the runs) ───────────────────

def criteria() -> dict:
    h = json.loads((OUT / "hcg44" / "measures.json").read_text())
    w = json.loads((OUT / "wallaby" / "measures.json").read_text())
    dv_h = 8.243965186790092          # app axis, hgc44_3axes.fits (VELO-HEL as given)
    dv_w = 3.9372236636181697         # app axis, CASDA cubelet (optical cz from FREQ)
    res: dict = {"hcg44": {}, "wallaby": {}}
    for arm in ("S", "D", "H", "Sdil"):
        a = h["arms"].get(arm)
        if a is None:
            continue
        rows = {}
        for r, x in a["rows"].items():
            ref = h["rows"][r]
            fs, fp = ref["R_sof"]["flux"], ref["R_pub"]["flux"]
            ws, wp = ref["R_sof"]["w20"], ref["R_pub"]["w20"]
            rows[r] = {
                "dice_pub": x["dice_pub"], "dice_ref": ref["dice_ref"], "dice_sof": x["dice_sof"],
                "c1": x["dice_pub"] >= ref["dice_ref"] - 0.05 and x["dice_sof"] >= 0.50,
                "flux": x["flux"], "flux_lo": 0.9 * fs, "flux_hi": 1.1 * fp,
                "c2": 0.9 * fs <= x["flux"] <= 1.1 * fp,
                "w20": x["w20"], "dw_sof": x["w20"] - ws, "dw_pub": x["w20"] - wp,
                "c3": min(abs(x["w20"] - ws), abs(x["w20"] - wp)) <= 2 * dv_h + 1e-6,
                "ids": x["ids"]}
        n1 = sum(r["c1"] for r in rows.values()); n2 = sum(r["c2"] for r in rows.values())
        n3 = sum(r["c3"] for r in rows.values())
        res["hcg44"][arm] = {"rows": rows, "n_c1": n1, "n_c2": n2, "n_c3": n3,
                             "c1": n1 >= 4, "c2": n2 >= 4, "c3": n3 == 5,
                             "completeness": a["completeness_vs_H_snr5"], "c4": a["completeness_vs_H_snr5"] >= 0.9,
                             "reliability": a["reliability_vs_H_or_pub"], "n_unmatched": len(a["unmatched"]),
                             "purity": h["inversion"].get(arm, {}).get("purity")}
    inv = h["inversion"]
    s = res["hcg44"]["S"]
    s["c5"] = (inv["S"]["purity"] >= inv["H"]["purity"]
               or len(h["unmatched_S_vs_H_or_pub"]) <= len(h["unmatched_H_vs_S_or_pub"]))
    s["c5_detail"] = {"purity_S": inv["S"]["purity"], "purity_H": inv["H"]["purity"],
                      "unmatched_S": h["unmatched_S_vs_H_or_pub"], "unmatched_H": h["unmatched_H_vs_S_or_pub"]}
    s["pass"] = all(s[k] for k in ("c1", "c2", "c3", "c4", "c5"))
    rel = w["release"]; Fs = w["R_sof_disc"]["flux"]; Ws = w["R_sof_disc"]["w20"]
    for arm, a in w["arms"].items():
        F, W = a["disc"]["flux"], a["disc"]["w20"]
        res["wallaby"][arm] = {
            "dice": a["disc_dice"], "c_dice": a["disc_dice"] >= 0.8,
            "flux": F, "flux_vs_release": F / rel["flux_jykms_radio"] - 1, "flux_vs_Rsof": F / Fs - 1,
            "flux_vs_release_optical": F / rel["flux_jykms_optical"] - 1,
            "c_flux": abs(F / rel["flux_jykms_radio"] - 1) <= 0.10 and abs(F / Fs - 1) <= 0.10,
            "w20": W, "dw_Rsof": W - Ws, "dw_release": W - rel["w20_kms_c_dnu_over_nu"],
            "c_w20": abs(W - Ws) <= 2 * dv_w + 1e-6,
            "n_pix": a["disc"]["n_pix"],
            "faint": {i: (a["per_ref"][str(i)]["dice"], bool(a["per_ref"][str(i)]["matched_ids"]),
                          a["per_ref"][str(i)]["only_merged"]) for i in w["ref_ids_by_size"][1:]},
            "purity": w["inversion"].get(arm, {}).get("purity")}
        r = res["wallaby"][arm]; r["pass"] = r["c_dice"] and r["c_flux"] and r["c_w20"]
    res["global_pass"] = res["hcg44"]["S"]["pass"] and res["wallaby"]["S"]["pass"]
    (OUT / "criteria.json").write_text(json.dumps(res, indent=1, default=float))
    return res


def criteria_stage(_cubes) -> None:
    r = criteria()
    s = r["hcg44"]["S"]
    print("HCG44 S:", {k: s[k] for k in ("n_c1", "n_c2", "n_c3", "c1", "c2", "c3", "c4", "c5", "pass")})
    print("WALLABY S:", {k: r["wallaby"]["S"][k] for k in ("c_dice", "c_flux", "c_w20", "pass")})
    print("global:", r["global_pass"])


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("stage", choices=["run", "measure", "criteria"])
    ap.add_argument("--cube", action="append", choices=list(CUBES))
    a = ap.parse_args()
    cubes = a.cube or list(CUBES)
    OUT.mkdir(parents=True, exist_ok=True)
    {"run": run_stage, "measure": measure_stage, "criteria": criteria_stage}[a.stage](cubes)


if __name__ == "__main__":
    main()
