"""LTE analysis of CO isotopologue lines (issue #416, item 4).

From ¹³CO and C¹⁸O (and optionally ¹²CO) in the same rotational transition:
the optical depth from the line ratio, the excitation temperature, the LTE
column densities corrected for opacity, and the H₂ column density and mass.
Every function takes and returns plain numbers or numpy arrays in the units its
docstring names (CGS for the physics: K, cm⁻², cm/s), so the same code serves a
region (scalars) and a map (arrays).

Conventions and sources
-----------------------
* T_mb = T_A* / η_mb (the JCMT convention; Rigby et al. 2016, MNRAS 456, 2885
  adopt η_mb = 0.72 for CHIMPS).
* R = W(¹³CO) / W(C¹⁸O) = (1 − e^(−τ₁₃)) / (1 − e^(−τ₁₃/X)), with
  X = X(¹³CO)/X(C¹⁸O) = (¹⁶O/¹⁸O) / (¹²C/¹³C) and τ(C¹⁸O) = τ₁₃/X, both lines
  sharing T_ex, the beam filling and J(T_ex) − J(T_bg). The last is the usual
  approximation: the two frequencies differ by 0.4 %, which moves τ by at most
  0.75 % for T_ex ≥ 10 K in J = 3–2 (R ≈ 4) and 0.15 % in J = 1–0.
* ¹²C/¹³C = 6.21 R_GC + 18.71 (Milam et al. 2005, ApJ 634, 1126) or
  7.5 R_GC + 7.6 (Wilson & Rood 1994, ARA&A 32, 191); ¹⁶O/¹⁸O = 58.8 R_GC + 37.1
  (Wilson & Rood 1994); R_GC in kpc with R₀ = 8.5 kpc.
* T_ex from optically thick ¹²CO: T_ex = T₀ / ln(1 + T₀/(T_mb,peak + J(T_bg))).
* N_tot = 8πν³/(c³ A_ul) · Q(T_ex)/g_u · e^(E_u/kT_ex) / (e^(hν/kT_ex) − 1)
  / (J(T_ex) − J(T_bg)) · ∫T_mb dv · τ/(1 − e^(−τ)), the A_ul form of
  Mangum & Shirley 2015 (PASP 127, 266) eqs. 29–31, with the exact rotational
  partition function Q = Σ (2J+1) e^(−hBJ(J+1)/kT) and g_u = 2J_u + 1.
* N(H₂) = N(C¹⁸O)/X(C¹⁸O) or N(¹³CO)/X(¹³CO), X(¹²CO) = 8.5 × 10⁻⁵
  (Frerking, Langer & Wilson 1982, ApJ 262, 590) divided by the isotopic ratio;
  M = μ_H₂ m_H Σ N(H₂) A_pix with μ_H₂ = 2.8 (Kauffmann et al. 2008, A&A 487,
  993, App. A).
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

H_CGS = 6.62607015e-27       # erg s
K_CGS = 1.380649e-16         # erg / K
C_CGS = 2.99792458e10        # cm / s
M_H_G = 1.67372e-24          # g (1.00794 u)
M_SUN_G = 1.98841e33         # g
PC_CM = 3.0856775814913673e18
T_CMB_K = 2.73
R0_KPC = 8.5
X12CO_DEFAULT = 8.5e-5
MU_H2_DEFAULT = 2.8


@dataclass(frozen=True)
class Line:
    """One rotational transition J_u → J_u − 1 of a linear rotor, from CDMS."""
    species: str
    transition: str
    freq_hz: float
    eu_k: float          # E_u / k
    a_ul: float          # s⁻¹
    j_u: int
    b_hz: float          # rotational constant B
    cdms: str            # CDMS entry

    @property
    def g_u(self) -> int:
        return 2 * self.j_u + 1

    @property
    def t0(self) -> float:
        return H_CGS * self.freq_hz / K_CGS


# CDMS (Müller et al. 2005, J. Mol. Struct. 742, 215), entries c028503 (CO),
# c029501 (¹³CO, version 3, March 2025) and c030502 (C¹⁸O), read 2026-11-23.
# A_ul from CDMS log I(300 K); g_u = 2J_u + 1, the convention of the partition
# function below (CDMS's ¹³CO g_u and Q both carry the ¹³C spin factor 2).
_B_HZ = {"12CO": 57635.968e6, "13CO": 55101.013e6, "C18O": 54891.421e6}
_CDMS = {"12CO": "CDMS 028503", "13CO": "CDMS 029501", "C18O": "CDMS 030502"}
_LINES_RAW = {
    ("13CO", "1-0"): (110201.3543e6, 5.2888, 6.333e-8, 1),
    ("13CO", "3-2"): (330587.9653e6, 31.7320, 2.194e-6, 3),
    ("C18O", "1-0"): (109782.1757e6, 5.2687, 6.266e-8, 1),
    ("C18O", "3-2"): (329330.5523e6, 31.6113, 2.172e-6, 3),
    ("12CO", "1-0"): (115271.2018e6, 5.5321, 7.203e-8, 1),
    ("12CO", "3-2"): (345795.9899e6, 33.1919, 2.497e-6, 3),
}
TRANSITIONS = ("1-0", "3-2")
LINES = {key: Line(key[0], key[1], f, eu, a, ju, _B_HZ[key[0]], _CDMS[key[0]])
         for key, (f, eu, a, ju) in _LINES_RAW.items()}


def line(species: str, transition: str) -> Line:
    try:
        return LINES[(species, transition)]
    except KeyError:
        raise ValueError(f"No constants for {species} {transition}; "
                         f"transitions available: {', '.join(TRANSITIONS)}.") from None


def j_nu(t0: float, t_k):
    """Radiation temperature J_ν(T) = T₀ / (e^(T₀/T) − 1), K."""
    t = np.asarray(t_k, dtype=np.float64)
    with np.errstate(over="ignore", divide="ignore", invalid="ignore"):
        return t0 / np.expm1(t0 / t)


def partition_function(b_hz: float, t_k, j_max: int = 200):
    """Q(T) = Σ_J (2J + 1) e^(−hBJ(J+1)/kT), J = 0 … j_max − 1."""
    t = np.asarray(t_k, dtype=np.float64)
    j = np.arange(j_max, dtype=np.float64)
    e = H_CGS * b_hz * j * (j + 1) / K_CGS
    with np.errstate(over="ignore", divide="ignore", invalid="ignore"):
        terms = (2 * j + 1) * np.exp(-e / t[..., None])
    return np.sum(terms, axis=-1)


# ── the isotopic ratio ───────────────────────────────────────────────────────

CARBON_GRADIENTS = {
    "milam2005": (6.21, 18.71, "Milam et al. 2005, ApJ 634, 1126"),
    "wilson_rood1994": (7.5, 7.6, "Wilson & Rood 1994, ARA&A 32, 191"),
}
OXYGEN_GRADIENT = (58.8, 37.1, "Wilson & Rood 1994, ARA&A 32, 191")


def galactocentric_radius_kpc(l_deg: float, b_deg: float, d_kpc: float,
                              r0_kpc: float = R0_KPC) -> float:
    """R_GC = sqrt(R₀² + d² cos²b − 2 R₀ d cos b cos l)."""
    lr, br = math.radians(l_deg), math.radians(b_deg)
    dp = d_kpc * math.cos(br)
    return math.sqrt(max(r0_kpc ** 2 + dp ** 2 - 2 * r0_kpc * dp * math.cos(lr), 0.0))


def carbon_ratio(r_gc_kpc: float, gradient: str = "milam2005") -> float:
    try:
        a, b, _ = CARBON_GRADIENTS[gradient]
    except KeyError:
        raise ValueError(f"Unknown 12C/13C gradient '{gradient}'; "
                         f"use one of {sorted(CARBON_GRADIENTS)}.") from None
    return a * r_gc_kpc + b


def oxygen_ratio(r_gc_kpc: float) -> float:
    a, b, _ = OXYGEN_GRADIENT
    return a * r_gc_kpc + b


def abundance_ratio(r_gc_kpc: float, gradient: str = "milam2005") -> float:
    """X = X(¹³CO)/X(C¹⁸O) = (¹⁶O/¹⁸O) / (¹²C/¹³C) at R_GC."""
    return oxygen_ratio(r_gc_kpc) / carbon_ratio(r_gc_kpc, gradient)


# ── optical depth from the ratio ─────────────────────────────────────────────

FLAG_OK, FLAG_THIN, FLAG_NO_SOLUTION, FLAG_LOW_SNR = 0, 1, 2, 3
FLAG_MEANING = "0 ok, 1 optically thin (R >= X), 2 no LTE solution (R <= 1), 3 below S/N"
_TAU_MIN, _TAU_MAX = 1e-8, 200.0


def ratio_of_tau(tau13, x: float):
    """R(τ₁₃) = (1 − e^(−τ₁₃)) / (1 − e^(−τ₁₃/X)), → X as τ → 0."""
    t = np.asarray(tau13, dtype=np.float64)
    with np.errstate(divide="ignore", invalid="ignore"):
        r = -np.expm1(-t) / -np.expm1(-t / x)
    return np.where(t > 0, r, x)


def tau_from_ratio(r, x: float):
    """``(τ₁₃, flag)`` solving R = (1 − e^(−τ))/(1 − e^(−τ/X)); R falls
    monotonically from X (τ → 0) to 1 (τ → ∞). R ≥ X gives τ = 0 (thin),
    R ≤ 1 or a non-finite R no solution (NaN). Vectorised bisection in log τ:
    80 halvings of log tau over [1e-8, 200] leave an interval far below double precision."""
    if not (x > 1.0 and math.isfinite(x)):
        raise ValueError(f"The abundance ratio X must be finite and above 1, got {x}.")
    r = np.asarray(r, dtype=np.float64)
    tau = np.full(r.shape, np.nan)
    flag = np.full(r.shape, FLAG_NO_SOLUTION, dtype=np.int16)
    finite = np.isfinite(r)
    thin = finite & (r >= x)
    tau[thin] = 0.0
    flag[thin] = FLAG_THIN
    solve = finite & (r > 1.0) & (r < x)
    if solve.any():
        lo = np.full(int(solve.sum()), math.log(_TAU_MIN))
        hi = np.full_like(lo, math.log(_TAU_MAX))
        target = r[solve]
        for _ in range(80):
            mid = 0.5 * (lo + hi)
            above = ratio_of_tau(np.exp(mid), x) > target
            lo = np.where(above, mid, lo)
            hi = np.where(above, hi, mid)
        tau[solve] = np.exp(0.5 * (lo + hi))
        flag[solve] = FLAG_OK
    return tau, flag


def ratio_error(w13, s13, w18, s18):
    """σ_R = R sqrt((σ₁₃/W₁₃)² + (σ₁₈/W₁₈)²)."""
    w13, s13, w18, s18 = (np.asarray(v, dtype=np.float64) for v in (w13, s13, w18, s18))
    with np.errstate(divide="ignore", invalid="ignore"):
        r = w13 / w18
        return np.abs(r) * np.sqrt((s13 / w13) ** 2 + (s18 / w18) ** 2)


# ── excitation temperature ───────────────────────────────────────────────────

def tex_from_thick_peak(t_peak_mb, ln: Line, t_bg: float = T_CMB_K):
    """T_ex from the peak T_mb of an optically thick line (¹²CO)."""
    t0 = ln.t0
    with np.errstate(divide="ignore", invalid="ignore"):
        return t0 / np.log1p(t0 / (np.asarray(t_peak_mb, dtype=np.float64) + j_nu(t0, t_bg)))


def tex_from_peak_and_tau(t_peak_mb, tau, ln: Line, t_bg: float = T_CMB_K):
    """T_ex from the peak T_mb of a line of known peak optical depth."""
    t0 = ln.t0
    with np.errstate(divide="ignore", invalid="ignore"):
        jj = np.asarray(t_peak_mb, dtype=np.float64) / -np.expm1(-np.asarray(tau, dtype=np.float64)) \
            + j_nu(t0, t_bg)
        return t0 / np.log1p(t0 / jj)


# ── column density ───────────────────────────────────────────────────────────

def thin_column_factor(ln: Line, t_ex, t_bg: float = T_CMB_K):
    """N per unit ∫T_mb dv in the optically thin limit, cm⁻² per K km/s."""
    t = np.asarray(t_ex, dtype=np.float64)
    nu = ln.freq_hz
    with np.errstate(over="ignore", divide="ignore", invalid="ignore"):
        f = (8.0 * math.pi * nu ** 3 / (C_CGS ** 3 * ln.a_ul)
             * partition_function(ln.b_hz, t) / ln.g_u
             * np.exp(ln.eu_k / t)
             / np.expm1(ln.t0 / t)
             / (j_nu(ln.t0, t) - j_nu(ln.t0, t_bg)))
    return f * 1e5


def opacity_correction(tau):
    """τ / (1 − e^(−τ)), 1 at τ = 0."""
    t = np.asarray(tau, dtype=np.float64)
    with np.errstate(divide="ignore", invalid="ignore"):
        c = t / -np.expm1(-t)
    return np.where(t > 0, c, np.where(t == 0, 1.0, np.nan))


def column_density(w_kkms, ln: Line, t_ex, tau, t_bg: float = T_CMB_K):
    """LTE N_tot (cm⁻²) from ∫T_mb dv (K km/s), opacity corrected."""
    return (thin_column_factor(ln, t_ex, t_bg) * np.asarray(w_kkms, dtype=np.float64)
            * opacity_correction(tau))


# ── H₂ ───────────────────────────────────────────────────────────────────────

def isotopologue_abundance(species: str, r_gc_kpc: float | None, gradient: str,
                           x12co: float = X12CO_DEFAULT) -> tuple[float, str]:
    """X(species) relative to H₂ and how it was obtained."""
    if r_gc_kpc is None:
        raise ValueError("The H2 abundance needs R_GC (or a distance to compute it).")
    if species == "13CO":
        ratio = carbon_ratio(r_gc_kpc, gradient)
        return x12co / ratio, f"X(12CO)={x12co:g} / 12C/13C={ratio:.2f} ({CARBON_GRADIENTS[gradient][2]})"
    if species == "C18O":
        ratio = oxygen_ratio(r_gc_kpc)
        return x12co / ratio, f"X(12CO)={x12co:g} / 16O/18O={ratio:.2f} ({OXYGEN_GRADIENT[2]})"
    raise ValueError(f"No H2 abundance for {species}.")


def pixel_area_cm2(d_kpc: float, cdelt1_deg: float, cdelt2_deg: float) -> float:
    """A_pix = d² |Δθ₁ Δθ₂| (small angles), cm²."""
    d_cm = d_kpc * 1e3 * PC_CM
    return d_cm ** 2 * abs(math.radians(cdelt1_deg) * math.radians(cdelt2_deg))


def mass_msun(n_h2_sum_cm2: float, area_pix_cm2: float, mu_h2: float = MU_H2_DEFAULT) -> float:
    """M = μ_H₂ m_H Σ N(H₂) A_pix, in M_sun."""
    return mu_h2 * M_H_G * n_h2_sum_cm2 * area_pix_cm2 / M_SUN_G
