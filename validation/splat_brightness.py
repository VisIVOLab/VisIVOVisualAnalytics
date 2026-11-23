#!/usr/bin/env python
"""Gaussian splat brightness against the number of particles (issue #457).

One truncated Plummer sphere (scale a = 1, cut at 10 a) is drawn from 1e5,
1e6, 3.66e6, 1e7 and 3e7 of its particles, each subset the first N rows of
the same independent draw, so every subset samples the same density field.
Splats carry a share of the light that falls as 1/N while there are more of
them, so the picture should not change with N. Two things are measured on
each picture, seen face on over +-4 a:

  - the mean light over the picture, against the nominal light the splat
    rule promises (N x per-splat amplitude x the kernel's integral, for the
    particles in view) and against the smallest table (the "mean ratio");
  - the projected profile: the light at R = 3 a (1 % of the central surface
    density of a Plummer sphere) over the light at the centre, against the
    same ratio of the truncated sphere's analytic surface density.

Paths, all through the app's own code:

Eight markers at the corners of the cut cube pin the box the splat rules size
the splats by: the outermost of N draws reaches further out as N grows, and
the rules' light grows with the box's two shorter sides squared, which is not
what this script measures.

  - ``vtk``: the backend's point render session
    (app/render_session/points.py, the path the desktop's Remote view and a
    comparison pane stream), Gaussian mode, read back as 8-bit RGB. The
    centre is read at intensity 1 and R = 3 a at intensity 100 (an 8-bit
    picture cannot hold both), each divided by its intensity (the gain is
    linear in the intensity below the opacity cap, which no N here reaches);
    the mean ratio
    is taken at intensity 1, where nothing reaches white (a few 8-bit
    levels on average: the readback's rounding puts mean / nominal near
    1.06, the same for every N); ``vtk-i35`` is
    the mean at the standard intensity 35, whose core is clipped at white;
  - ``vr-<rule>``: the VR page's splat rules (app/point_splat.py
    splat_style + splat_light, the light the backend's refined stereo frames
    and, in expectation, the page sum), as linear light before the tone
    curve, at the standard intensity 35. ``page`` is the page's own picture
    of a sample of at most --page-rows rows (its splats grown to cover the
    table); ``all`` draws every row.

Pass (issue #457): mean ratio 1 +- 5 % across N, profile within 10 %.

Run:  VISIVO_TOKEN=x backend/.venv/bin/python validation/splat_brightness.py \
          [--workdir DIR] [--sizes 1e5,1e6,3.66e6,1e7,3e7] [--paths vtk,vr]
Writes out/splat_brightness.{md,csv}.
"""
from __future__ import annotations

import argparse
import asyncio
import csv
import math
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "backend"))
os.environ.setdefault("VISIVO_TOKEN", "x")

A = 1.0
R_CUT = 10.0
VIEW = 4.0
PIXELS = 1000
SIZE = 1.0
R_PROFILE = 3.0
CORE = 0.1
RING = 0.1
KERNEL_SHARE = math.erf(3.0 / math.sqrt(2.0)) ** 2


def plummer(n: int, seed: int = 457) -> np.ndarray:
    """``n`` positions (float32, n x 3) of a Plummer sphere of scale A cut at
    R_CUT: radii by inverting M(<r) = r^3 / (r^2 + a^2)^1.5 below M(<R_CUT),
    isotropic directions."""
    rng = np.random.default_rng(seed)
    out = np.empty((n, 3), dtype=np.float32)
    m_cut = R_CUT ** 3 / (R_CUT ** 2 + A ** 2) ** 1.5
    step = 1 << 22
    for i in range(0, n, step):
        k = min(step, n - i)
        m = rng.uniform(0.0, m_cut, k)
        r = A / np.sqrt(m ** (-2.0 / 3.0) - 1.0)
        cos_t = rng.uniform(-1.0, 1.0, k)
        phi = rng.uniform(0.0, 2.0 * math.pi, k)
        sin_t = np.sqrt(1.0 - cos_t ** 2)
        out[i:i + k, 0] = r * sin_t * np.cos(phi)
        out[i:i + k, 1] = r * sin_t * np.sin(phi)
        out[i:i + k, 2] = r * cos_t
    corners = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)],
                       dtype=np.float32) * np.float32(R_CUT)
    out[:8] = corners
    return out


def surface_density(radius: np.ndarray) -> np.ndarray:
    """The truncated sphere's surface density (arbitrary units) at projected
    ``radius``: rho ~ (1 + r^2)^-2.5 integrated along the line of sight
    inside R_CUT."""
    out = np.zeros_like(radius, dtype=float)
    for i, big_r in enumerate(np.ravel(radius)):
        if big_r >= R_CUT:
            continue
        zt = math.sqrt(R_CUT ** 2 - big_r ** 2)
        z = np.linspace(0.0, zt, 4001)
        rho = (1.0 + (big_r ** 2 + z ** 2) / A ** 2) ** -2.5
        out.flat[i] = 2.0 * np.trapezoid(rho, z)
    return out


def ring_mean(func, r0: float, r1: float) -> float:
    """Area-weighted mean of ``func`` over the annulus r0..r1."""
    r = np.linspace(r0, r1, 201)
    return float(np.trapezoid(func(r) * r, r) / np.trapezoid(r, r))


def analytic_profile() -> float:
    """Sigma(R_PROFILE ring) / Sigma(core disc) of the truncated sphere."""
    return (ring_mean(surface_density, R_PROFILE - RING / 2, R_PROFILE + RING / 2)
            / ring_mean(surface_density, 0.0, CORE))


def radii_grid() -> np.ndarray:
    """Projected radius (data units) of each pixel centre of the face-on view."""
    c = (np.arange(PIXELS) + 0.5) / PIXELS * 2.0 * VIEW - VIEW
    return np.hypot(*np.meshgrid(c, c))


def measured_profile(core_light: np.ndarray, ring_light: np.ndarray) -> float:
    rr = radii_grid()
    core = core_light[rr < CORE].mean()
    ring = ring_light[np.abs(rr - R_PROFILE) < RING / 2].mean()
    return float(ring / core)


def splat_geometry(xyz: np.ndarray) -> tuple[float, float]:
    """(spacing, depth) the splat rules derive from the box the points fill."""
    lo, hi = xyz.min(axis=0).astype(float), xyz.max(axis=0).astype(float)
    dims = hi - lo
    longest = max(dims.max(), 1e-6)
    volume = float(np.prod(np.maximum(dims, longest * 1e-3)))
    spacing = math.cbrt(volume / len(xyz))
    return spacing, max(longest / spacing, 1.0)


def nominal_mean(xyz: np.ndarray, amplitude: float, sigma: float) -> float:
    """The mean light per pixel the rule promises: every particle in view
    adds amplitude x its kernel's integral (2 pi sigma^2 out to 3 sigma, in
    pixels)."""
    in_view = np.count_nonzero((np.abs(xyz[:, 0]) < VIEW) & (np.abs(xyz[:, 1]) < VIEW))
    sigma_px = sigma * PIXELS / (2.0 * VIEW)
    return in_view * amplitude * 2.0 * math.pi * sigma_px ** 2 * KERNEL_SHARE / PIXELS ** 2


def nominal_amplitude(depth: float, intensity: float) -> float:
    """1.5 / depth at the standard intensity 35: the brightness the rule
    is normalised to, before any quantisation or floor."""
    return min(1.5 * intensity / (35.0 * depth), 0.25)


def write_vbt(path: Path, xyz: np.ndarray) -> str:
    binary = path.with_suffix(".bin")
    with open(binary, "wb") as fh:
        for c in range(3):
            np.ascontiguousarray(xyz[:, c]).astype("<f4").tofile(fh)
    head = Path(str(binary) + ".head")
    head.write_text(f"float\n3\n{len(xyz)}\nlittle\nX\nY\nZ\n")
    return str(head)


def vtk_light(head: str, intensities: list[int]) -> dict[int, np.ndarray]:
    """The point session's Gaussian picture at each intensity, as light in
    units of the splat colour (red channel over the background, / 0.8)."""
    from app.render_session.points import _BACKGROUNDS, PointRenderSession
    loop = asyncio.new_event_loop()
    s = PointRenderSession("v" * 32, head)
    out = {}
    try:
        loop.run_until_complete(s.initialize())
        for cmd in ({"type": "set_show_outline", "visible": False},
                    {"type": "set_show_axes", "visible": False},
                    {"type": "set_show_colorbar", "visible": False},
                    {"type": "set_background", "name": "Dark"},
                    {"type": "resize", "width": PIXELS, "height": PIXELS},
                    {"type": "set_camera", "position": [0, 0, 100], "focal_point": [0, 0, 0],
                     "view_up": [0, 1, 0], "parallel": True, "parallel_scale": VIEW}):
            loop.run_until_complete(s.apply_command(cmd))
        bg = round(_BACKGROUNDS["Dark"][0] * 255.0)
        for intensity in intensities:
            loop.run_until_complete(s.apply_command(
                {"type": "set_point_style", "mode": "gaussian", "size": SIZE,
                 "intensity": intensity}))
            rgba = loop.run_until_complete(s.render_rgb_frame())[0]
            red = rgba[..., 0].astype(float)
            out[intensity] = ((red - bg) / 255.0 / 0.8, bool((red >= 255).any()))
    finally:
        loop.run_until_complete(s.close())
        loop.close()
    return out


def vr_light(xyz: np.ndarray, total: int, sample: int, rule: str, intensity: float):
    """The page's splats of the first ``sample`` rows (sized for a table of
    ``total``) as linear light, one colour (white): (light, amplitude,
    sample radius)."""
    from app.point_splat import splat_light, splat_style
    lo, hi = xyz[:total].min(axis=0), xyz[:total].max(axis=0)
    bounds = [float(lo[0]), float(hi[0]), float(lo[1]), float(hi[1]), float(lo[2]), float(hi[2])]
    _, amp, radius = splat_style(bounds, total, sample, SIZE, intensity, rule)
    distance = 1000.0
    to_eye = np.eye(4)
    to_eye[2, 3] = -distance
    near, far = distance - 20.0, distance + 20.0
    proj = np.zeros((4, 4))
    proj[0, 0] = proj[1, 1] = distance / VIEW
    proj[2, 2] = -(far + near) / (far - near)
    proj[2, 3] = -2.0 * far * near / (far - near)
    proj[3, 2] = -1.0
    lut = np.ones((1, 3), dtype=np.float32)
    light = splat_light(xyz[:sample], None, lut, to_eye, proj, PIXELS, PIXELS, radius, amp)
    return light[..., 0].astype(np.float64), amp, radius


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--workdir", default=None, help="where the VBT tables go (default: a temp dir)")
    ap.add_argument("--sizes", default="1e5,1e6,3.66e6,1e7,3e7")
    ap.add_argument("--paths", default="vtk,vr")
    ap.add_argument("--rules", default="v2-8bit,v2-f16,v2-f32")
    ap.add_argument("--page-rows", type=float, default=1e6)
    ap.add_argument("--label", default="", help="tag for the output files (e.g. 'before')")
    args = ap.parse_args()
    sizes = [int(float(s)) for s in args.sizes.split(",")]
    paths = args.paths.split(",")
    xyz = plummer(max(sizes))
    analytic = analytic_profile()
    rows = []
    tmp = None
    workdir = Path(args.workdir) if args.workdir else Path(tmp := tempfile.mkdtemp(prefix="splat457-"))
    workdir.mkdir(parents=True, exist_ok=True)
    try:
        for n in sizes:
            sub = xyz[:n]
            spacing, depth = splat_geometry(sub)
            sigma = spacing * SIZE / 8.0
            if "vtk" in paths:
                head = write_vbt(workdir / f"plummer_{n}", sub)
                lights = vtk_light(head, [1, 100, 35])
                (lo, lo_clip), (hi, _), (std, std_clip) = lights[1], lights[100], lights[35]
                profile = measured_profile(lo, hi / 100.0)
                for path, light, clip, intensity in (("vtk", lo, lo_clip, 1),
                                                     ("vtk-i35", std, std_clip, 35)):
                    nominal = nominal_mean(sub, nominal_amplitude(depth, intensity), sigma)
                    rows.append({"path": path, "n": n, "mean": light.mean(), "nominal": nominal,
                                 "profile": profile, "clipped": clip})
                for f in workdir.glob(f"plummer_{n}.bin*"):
                    f.unlink()
            if "vr" in paths:
                for rule in args.rules.split(","):
                    for mode, sample in (("all", n), ("page", min(n, int(args.page_rows)))):
                        light, amp, radius = vr_light(xyz, n, sample, rule, 35.0)
                        nominal = nominal_mean(sub, nominal_amplitude(depth, 35.0), sigma)
                        rows.append({"path": f"vr-{rule}-{mode}", "n": n, "mean": light.mean(),
                                     "nominal": nominal, "profile": measured_profile(light, light),
                                     "clipped": False})
            print(f"N = {n:>10,d} done", flush=True)
    finally:
        if tmp:
            for f in Path(tmp).glob("*"):
                f.unlink()
            os.rmdir(tmp)
    base = {}
    for r in rows:
        base.setdefault(r["path"], r["mean"])
        r["ratio"] = r["mean"] / base[r["path"]]
        r["to_nominal"] = r["mean"] / r["nominal"]
        r["profile_err"] = r["profile"] / analytic - 1.0
        r["pass"] = abs(r["ratio"] - 1) <= 0.05 and abs(r["profile_err"]) <= 0.10
    out = HERE / "out"
    out.mkdir(exist_ok=True)
    tag = f"_{args.label}" if args.label else ""
    keys = ["path", "n", "mean", "nominal", "to_nominal", "ratio", "profile", "profile_err",
            "clipped", "pass"]
    with open(out / f"splat_brightness{tag}.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow({k: r[k] for k in keys})
    lines = [f"# Splat brightness against N (issue #457){' - ' + args.label if args.label else ''}",
             "",
             f"Plummer sphere a = {A}, cut at {R_CUT} a, viewed face on over +-{VIEW} a at "
             f"{PIXELS} px, splat size {SIZE}. Analytic Sigma(3a)/Sigma(0) of the cut sphere: "
             f"{analytic:.5f}.",
             "",
             "| path | N | mean / nominal | mean ratio to first N | profile / analytic - 1 | pass |",
             "|---|---:|---:|---:|---:|:---:|"]
    for r in rows:
        lines.append(f"| {r['path']} | {r['n']:,} | {r['to_nominal']:.3f} | {r['ratio']:.3f} | "
                     f"{r['profile_err']:+.3f} | {'yes' if r['pass'] else 'no'} |")
    (out / f"splat_brightness{tag}.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
