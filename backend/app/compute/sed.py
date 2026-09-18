"""SED fitting — modified-blackbody (greybody) grid fits for compact sources.

Ported from the ViaLacteaVisualAnalytics ``sedfitgrid_engine_thin/thick``
engines (D. Elia 2012-2015, Python port S. Mordini 2021), which the legacy
desktop client drove through ``QProcess`` + ``eval()`` on positional argv
strings. The physics is unchanged; everything around it is not:

* the grid is evaluated by NumPy broadcasting instead of 4-deep Python loops;
* the refinement loop keeps the *global* best model, so a refinement step that
  lands on a worse grid can never overwrite a better solution, and it re-centres
  or expands the search rather than pegging silently at the range the user typed;
* uncertainties come from a fine per-parameter Δχ² = 1 profile over the user's
  full range, not from the width of the zoomed-in final grid (which measures the
  zoom, not the confidence interval);
* upper limits veto models that overshoot them instead of being folded into a
  sign test that indexed a NumPy array with ``list.index``.

Units follow the legacy engines: wavelength µm, flux Jy, distance pc, opacity
cm² g⁻¹ at a reference wavelength in µm, mass M⊙, size arcsec.
"""
from __future__ import annotations

from collections.abc import Callable, Sequence
from dataclasses import dataclass, field

import numpy as np

# Physical constants, in the cgs flavours the original engines used.
_PLANCK_C1 = 3.7417749e-5     # 2πhc² [erg cm² s⁻¹]
_PLANCK_C2 = 1.4387687        # hc/k  [cm K]
_PC_CM = 3.08567758128e18     # parsec in cm
_MSUN_G = 1.98892e33          # solar mass in g
_LSUN_W = 3.827e26            # solar luminosity in W
_ARCSEC_PER_RAD = 206265.0
_JY_CONV = 1.0 / 2.99792458e-13   # F_λ[erg/cm²/s/Å]·λ_µm²·_JY_CONV → Jy

# Model curve sampling, as in the legacy engines (5…2000 µm, 1 µm steps).
_MODEL_LAMBDA = np.arange(5.0, 2000.0, 1.0)

# Herschel/PACS colour corrections as a function of dust temperature. Values
# from the legacy engines (PACS calibration assumes νF_ν = const, so a cold
# greybody needs correcting band by band).
_CC_TEMP = np.array([250.0, 100, 50, 40, 30, 20, 19, 18, 17, 16, 15, 14, 13,
                     12, 11, 10, 9, 8, 7, 6, 5])
_CC_70 = np.array([1.005, 0.989, 0.982, 0.992, 1.034, 1.224, 1.269, 1.325, 1.396,
                   1.488, 1.607, 1.768, 1.992, 2.317, 2.816, 3.645, 5.175, 8.497,
                   17.815, 58.391, 456.837])
_CC_100 = np.array([1.023, 1.007, 0.985, 0.980, 0.982, 1.036, 1.051, 1.069, 1.093,
                    1.123, 1.162, 1.213, 1.282, 1.377, 1.512, 1.711, 2.024, 2.554,
                    3.552, 5.774, 12.259])
_CC_160 = np.array([1.062, 1.042, 1.010, 0.995, 0.976, 0.963, 0.964, 0.967, 0.972,
                    0.979, 0.990, 1.005, 1.028, 1.061, 1.110, 1.184, 1.300, 1.491,
                    1.833, 2.528, 4.278])
_COLOUR_CORRECTIONS = {70.0: _CC_70, 100.0: _CC_100, 160.0: _CC_160}
_CC_BAND_TOL = 0.5   # µm: how close a band has to be to 70/100/160 to be corrected

_MAX_ITERATIONS = 10
_REFINE_HALF_WIDTH = 3        # cells kept either side of the optimum
_MULTI_START_SEEDS = 8        # separated minima the refinement starts from
_MULTI_START_CELL_BUDGET = 200_000   # above this, refine fewer seeds
_RESOLUTION_TOLERANCE = 1e-4  # relative axis width at which the zoom is done
_MAX_STEPS_PER_AXIS = 2000    # a single axis beyond this is a typo, not a request
_MAX_GRID_CELLS = 2_000_000   # model rows; ×n_bands×8 B is the bank's footprint
_DELTA_CHI2 = 1.0             # 1σ on one parameter
_PROFILE_SCAN_STEPS = 16      # outward samples used to bracket the Δχ² crossing
_PROFILE_NUISANCE_STEPS = 24  # per-axis sampling of the re-minimised parameters
_PROFILE_NUISANCE_BUDGET = 20_000   # models per profile step
_PROFILE_BISECTIONS = 32      # bisection steps that then locate it
_MIN_POSITIVE = 1e-9


class SedFitError(ValueError):
    """Invalid SED fit input (bad ranges, too few points, an oversized grid…)."""


# ── model evaluation ──────────────────────────────────────────────────────────

def planck_lambda(wavelength_um, temperature: float) -> np.ndarray:
    """Blackbody flux πI_λ in erg cm⁻² s⁻¹ Å⁻¹ (IDL ``planck.pro`` convention).

    Returns 0 where hc/λkT overflows, matching the legacy guard.
    """
    wave_cm = np.asarray(wavelength_um, dtype=float) * 1e-4
    temperature = float(temperature)
    if not np.isfinite(temperature) or temperature <= 0:
        raise SedFitError("temperature must be a finite positive number of kelvin")
    with np.errstate(over="ignore", divide="ignore", invalid="ignore"):
        x = _PLANCK_C2 / (wave_cm * temperature)
        # expm1 overflows around x ≈ 709; those bands carry no flux anyway.
        safe = np.isfinite(x) & (x < np.log(np.finfo(float).max))
        denom = wave_cm**5 * np.expm1(np.where(safe, x, 0.0))
        out = np.where(safe & (denom > 0),
                       _PLANCK_C1 / np.where(denom > 0, denom, 1.0) * 1e-8, 0.0)
    return out


def _jy_factor(wavelength_um) -> np.ndarray:
    """F_λ[erg/cm²/s/Å] → F_ν[Jy] conversion factor at each wavelength."""
    return _JY_CONV * np.asarray(wavelength_um, dtype=float) ** 2


def greybody_thin(wavelength_um, mass_msun: float, temperature: float, beta: float,
                  distance_pc: float, kappa_ref: float, lambda_ref_um: float) -> np.ndarray:
    """Optically thin modified blackbody, in Jy.

    F_ν = M κ_ref (λ_ref/λ)^β / d² · B_λ(λ,T)/π · (λ²/c)
    """
    lam = np.asarray(wavelength_um, dtype=float)
    dist_cm = float(distance_pc) * _PC_CM
    return (float(mass_msun) * _MSUN_G * float(kappa_ref) / dist_cm**2
            * planck_lambda(lam, temperature) / np.pi
            * _jy_factor(lam) * (float(lambda_ref_um) / lam) ** float(beta))


def greybody_thick(wavelength_um, size_arcsec: float, temperature: float, beta: float,
                   lambda0_um: float) -> np.ndarray:
    """Optically thick modified blackbody, in Jy.

    F_ν = Ω · B_λ(λ,T)/π · (λ²/c) · (1 − e^{−τ}),  τ = (λ₀/λ)^β,
    with Ω the solid angle of a circular source of angular diameter ``size_arcsec``.
    """
    lam = np.asarray(wavelength_um, dtype=float)
    omega = solid_angle_sr(size_arcsec)
    tau = (float(lambda0_um) / lam) ** float(beta)
    return (omega * planck_lambda(lam, temperature) / np.pi
            * _jy_factor(lam) * (-np.expm1(-tau)))


def solid_angle_sr(size_arcsec: float) -> float:
    """Solid angle (sr) of a circular source of angular diameter ``size_arcsec``."""
    size = float(size_arcsec)
    if not np.isfinite(size) or size <= 0:
        raise SedFitError("source size must be a finite positive angle in arcsec")
    return (size / _ARCSEC_PER_RAD) ** 2 * np.pi / 4.0


def thick_mass_msun(size_arcsec: float, beta: float, lambda0_um: float,
                    distance_pc: float, kappa_ref: float, lambda_ref_um: float) -> float:
    """Mass implied by an optically thick solution (legacy ``mass=`` line)."""
    dist_cm = float(distance_pc) * _PC_CM
    omega = solid_angle_sr(size_arcsec)
    return (dist_cm**2 * omega / float(kappa_ref)
            * (float(lambda0_um) / float(lambda_ref_um)) ** float(beta) / _MSUN_G)


def bolometric_luminosity(wavelength_um, flux_jy, distance_pc: float) -> float:
    """L_bol in L⊙ by trapezoidal integration of the SED, resampled log-log.

    Straight lines in log-log are what an SED looks like between points, so the
    interpolation is done there and integrated back in linear space (legacy
    ``lbol.py``). This integrates the *sampled* range only (5–2000 µm), so it is
    a lower bound on the true bolometric luminosity.
    """
    lam = np.asarray(wavelength_um, dtype=float)
    flux = np.asarray(flux_jy, dtype=float)
    good = np.isfinite(lam) & np.isfinite(flux) & (lam > 0) & (flux > 0)
    lam, flux = lam[good], flux[good]
    if lam.size < 2:
        return 0.0
    order = np.argsort(lam)
    lam, flux = lam[order], flux[order]
    f_lambda = 1.0e-15 * flux * 0.2997 / lam**2      # Jy → W cm⁻² µm⁻¹
    log_lam = np.log10(lam)
    grid = np.linspace(log_lam[0], log_lam[-1], 1000)
    f_fine = 10.0 ** np.interp(grid, log_lam, np.log10(f_lambda))
    lam_fine = 10.0**grid
    trapezoid = getattr(np, "trapezoid", None) or np.trapz
    dist_cm = float(distance_pc) * _PC_CM
    return float(trapezoid(f_fine, lam_fine)) * 4.0 * np.pi * dist_cm**2 / _LSUN_W


# ── grid axes ─────────────────────────────────────────────────────────────────

@dataclass
class Range:
    """A ``[min, max, steps]`` fit range, as the legacy engines took them."""

    minimum: float
    maximum: float
    steps: int
    log_like: bool = False    # mass is stepped uniformly in M^0.2, not in M
    positive: bool = False    # the parameter has no meaning at or below zero

    @classmethod
    def parse(cls, value, name: str, *, log_like: bool = False,
              positive: bool = False) -> "Range":
        try:
            lo, hi, n = float(value[0]), float(value[1]), int(value[2])
        except (TypeError, IndexError, ValueError, KeyError) as exc:
            raise SedFitError(f"{name} must be [min, max, steps]") from exc
        if not np.isfinite(lo) or not np.isfinite(hi):
            raise SedFitError(f"{name} bounds must be finite")
        if n < 1:
            raise SedFitError(f"{name} needs at least 1 step")
        if n > _MAX_STEPS_PER_AXIS:
            raise SedFitError(f"{name} asks for {n} steps (limit {_MAX_STEPS_PER_AXIS})")
        if hi < lo:
            lo, hi = hi, lo
        if (positive or log_like) and lo <= 0:
            raise SedFitError(f"{name} minimum must be > 0")
        return cls(lo, hi, n, log_like, positive or log_like)

    @property
    def fixed(self) -> bool:
        return self.steps <= 1 or self.maximum == self.minimum

    @property
    def cells(self) -> int:
        """Number of grid points, without building the array."""
        return 1 if self.fixed else int(self.steps)

    def values(self) -> np.ndarray:
        if self.fixed:
            # One step means "hold this parameter fixed". The legacy engines
            # disagreed with themselves about which end that was (bmax for beta,
            # both ends for mass), so pin it to the minimum, always.
            return np.array([self.minimum], dtype=float)
        if self.log_like:
            # Uniform in M^0.2: dense at low masses, where the SED changes fast.
            return np.linspace(self.minimum**0.2, self.maximum**0.2, self.steps) ** 5
        return np.linspace(self.minimum, self.maximum, self.steps)


def _refine(values: np.ndarray, best_index: int, rng: Range) -> Range:
    """Shrink a range around the optimum, or push the boundary out if it sits there."""
    if rng.fixed or values.size < 2:
        return rng
    n = values.size
    lo = float(values[max(0, best_index - _REFINE_HALF_WIDTH)])
    hi = float(values[min(n - 1, best_index + _REFINE_HALF_WIDTH)])
    span = float(values[-1] - values[0])
    if best_index == 0:
        # Optimum against the lower wall: extend downwards rather than pretend
        # the wall is the answer.
        lo = float(values[0]) - 0.5 * span
    if best_index == n - 1:
        hi = float(values[-1]) + 0.5 * span
    if rng.positive:
        # A negative temperature, size or λ₀ is not a colder/smaller solution,
        # it is a different formula; expansion must stay inside the domain.
        lo = max(lo, min(_MIN_POSITIVE, float(values[0]) * 0.5))
        hi = max(hi, lo * (1.0 + 1e-9))
    if hi <= lo:
        hi = lo + max(abs(lo) * 1e-4, _MIN_POSITIVE)
    return Range(lo, hi, rng.steps, rng.log_like, rng.positive)


def _colour_correction(wavelength_um: np.ndarray, temperatures: np.ndarray) -> np.ndarray:
    """Per-(temperature, band) multiplicative colour correction, shape (nT, nλ)."""
    corr = np.ones((temperatures.size, wavelength_um.size), dtype=float)
    for band, table in _COLOUR_CORRECTIONS.items():
        hit = np.abs(wavelength_um - band) <= _CC_BAND_TOL
        if not np.any(hit):
            continue
        # The tables span 5…250 K; clamp outside instead of extrapolating, and
        # feed np.interp ascending abscissae (_CC_TEMP descends).
        t_clamped = np.clip(temperatures, _CC_TEMP.min(), _CC_TEMP.max())
        corr[:, hit] = np.interp(t_clamped, _CC_TEMP[::-1], table[::-1])[:, None]
    return corr


# ── χ² machinery ──────────────────────────────────────────────────────────────

def _chi_square(models: np.ndarray, flux: np.ndarray, errors: np.ndarray | None,
                fit_mask: np.ndarray, upper_limits: np.ndarray) -> np.ndarray:
    """χ² of each model row against the measured SED.

    ``models`` is (n_models, n_bands). Bands outside ``fit_mask`` (zero flux, or
    flagged as upper limits) do not contribute. A model brighter than any upper
    limit is rejected outright, as is any model that is not finite.
    """
    used = models[:, fit_mask]
    obs = flux[fit_mask]
    if errors is not None:
        err = errors[fit_mask]
        safe = err > 0
        chi = np.zeros_like(used)
        if np.any(safe):
            chi[:, safe] = ((used[:, safe] - obs[safe]) / err[safe]) ** 2
        if np.any(~safe):
            # No usable error on this band: fall back to the Poisson-like form
            # the legacy engine used when no error bars were given at all.
            chi[:, ~safe] = (used[:, ~safe] - obs[~safe]) ** 2 / (used[:, ~safe] + 1e-7)
    else:
        chi = (used - obs) ** 2 / (used + 1e-7)

    total = np.where(np.all(np.isfinite(used), axis=1), chi.sum(axis=1), np.inf)
    if np.any(upper_limits):
        over = models[:, upper_limits] > flux[upper_limits]
        total = np.where(np.any(over, axis=1), np.inf, total)
    return total


def _fit_mask(flux: np.ndarray, upper_limits: np.ndarray) -> np.ndarray:
    return (flux != 0) & np.isfinite(flux) & ~upper_limits


def _validate_sed(wavelength_um, flux_jy, errors_jy, upper_limits):
    """Sort and sanity-check a SED. Returns (λ, F, σ, UL, order)."""
    lam = np.asarray(wavelength_um, dtype=float).ravel()
    flux = np.asarray(flux_jy, dtype=float).ravel()
    if lam.size != flux.size:
        raise SedFitError("wavelengths and fluxes must be arrays of the same length")
    if lam.size < 2:
        raise SedFitError("a SED fit needs at least 2 photometric points")
    if np.any(~np.isfinite(lam)) or np.any(lam <= 0):
        raise SedFitError("wavelengths must be finite and positive")

    errors = None
    if errors_jy is not None and len(errors_jy):
        errors = np.asarray(errors_jy, dtype=float).ravel()
        if errors.size != lam.size:
            raise SedFitError("errors must have one entry per band")
        errors = np.where(np.isfinite(errors) & (errors > 0), errors, 0.0)
        if not np.any(errors > 0):
            errors = None

    if upper_limits is not None and len(upper_limits):
        ul = np.asarray(upper_limits).astype(bool).ravel()
        if ul.size != lam.size:
            raise SedFitError("upper_limits must have one entry per band")
        if np.any(ul & ~np.isfinite(flux)):
            raise SedFitError("an upper limit must carry a finite flux value")
    else:
        ul = np.zeros(lam.size, dtype=bool)

    if int(np.count_nonzero(_fit_mask(flux, ul))) < 2:
        raise SedFitError(
            "a SED fit needs at least 2 detections (zero fluxes and upper limits do not count)"
            if np.any(ul) else "no usable band left: every point is zero or not finite")

    order = np.argsort(lam)
    return (lam[order], flux[order],
            (errors[order] if errors is not None else None), ul[order], order)


def _guard_grid_size(ranges: Sequence[Range]) -> None:
    """Refuse an oversized grid *before* any array is allocated."""
    cells = 1
    for rng in ranges:
        cells *= rng.cells
        if cells > _MAX_GRID_CELLS:
            raise SedFitError(
                f"the requested grid has more than {_MAX_GRID_CELLS:,} models; "
                "reduce the number of steps")


# ── generic grid search ───────────────────────────────────────────────────────

BankBuilder = Callable[[Sequence[np.ndarray]], np.ndarray]


@dataclass
class _Best:
    values: list[float]
    chi2: float
    row: np.ndarray
    indices: tuple[int, ...]
    grids: list[np.ndarray]


def _evaluate_grid(ranges: list[Range], build_bank: BankBuilder, flux: np.ndarray,
                   errors: np.ndarray | None, mask: np.ndarray, ul: np.ndarray):
    """One pass over the grid: the axis values, the model rows and their χ²."""
    grids = [r.values() for r in ranges]
    bank = build_bank(grids)
    rows = bank.reshape(-1, bank.shape[-1])
    chi2 = _chi_square(rows, flux, errors, mask, ul)
    return grids, bank.shape[:-1], rows, chi2


def _seed_indices(chi2: np.ndarray, shape: tuple[int, ...], count: int) -> list[tuple[int, ...]]:
    """The ``count`` best grid cells that are not in each other's zoom window.

    Refining from the single best cell is a greedy zoom: with a coarse grid over
    four correlated parameters the global optimum routinely sits in a different
    basin from the coarse minimum, and the refinement then converges neatly onto
    the wrong answer. Seeding the refinement from a few *separated* minima costs
    a small multiple of the same work and finds it.
    """
    finite = np.isfinite(chi2)
    if not np.any(finite):
        return []
    order = np.argsort(np.where(finite, chi2, np.inf))
    seeds: list[tuple[int, ...]] = []
    for flat in order:
        if not finite[flat]:
            break
        candidate = tuple(int(i) for i in np.unravel_index(int(flat), shape))
        far_enough = all(
            any(abs(candidate[axis] - seed[axis]) > _REFINE_HALF_WIDTH
                for axis in range(len(shape)))
            for seed in seeds)
        if far_enough:
            seeds.append(candidate)
        if len(seeds) >= count:
            break
    return seeds


def _refine_from(seed: tuple[int, ...], ranges: list[Range], grids: list[np.ndarray],
                 rows: np.ndarray, chi2: np.ndarray, shape: tuple[int, ...],
                 build_bank: BankBuilder, flux: np.ndarray, errors: np.ndarray | None,
                 mask: np.ndarray, ul: np.ndarray) -> tuple[_Best, int, bool]:
    """Zoom in from one starting cell until the χ² stops improving."""
    flat = int(np.ravel_multi_index(seed, shape))
    best = _Best([float(g[i]) for g, i in zip(grids, seed)],
                 float(chi2[flat]), rows[flat].copy(), seed, grids)
    current = [_refine(g, i, r) for g, i, r in zip(grids, seed, ranges)]

    iterations = 1
    converged = False
    stalled = 0
    while iterations < _MAX_ITERATIONS:
        iterations += 1
        grids, shape, rows, chi2 = _evaluate_grid(current, build_bank, flux, errors, mask, ul)
        if not np.any(np.isfinite(chi2)):
            break

        flat = int(np.argmin(np.where(np.isfinite(chi2), chi2, np.inf)))
        indices = tuple(int(i) for i in np.unravel_index(flat, shape))
        chi2_here = float(chi2[flat])

        previous = best.chi2
        improved = chi2_here < previous
        if improved:
            best = _Best([float(g[i]) for g, i in zip(grids, indices)],
                         chi2_here, rows[flat].copy(), indices, grids)

        # Convergence is about RESOLUTION, not about how much the last step
        # improved χ². The legacy engines stopped as soon as a step bought less
        # than 1% of χ², which halts the zoom while the grid is still coarse:
        # on a four-parameter thick fit that left the optimum a whole grid cell
        # away, with a χ² of 6 where the true model scores 0. Each step narrows
        # every axis by about half, so ten steps resolve it a thousandfold —
        # and a step that finds nothing better twice running means the zoom has
        # nothing left to find.
        if improved:
            stalled = 0
            if best.chi2 <= 0.0:
                converged = True
                break
        else:
            stalled += 1
            if stalled >= 2:
                converged = True
                break

        centre = indices if improved else tuple(
            int(np.argmin(np.abs(g - v))) for g, v in zip(grids, best.values))
        current = [_refine(g, i, r) for g, i, r in zip(grids, centre, current)]
        if _fully_resolved(current):
            converged = True
            break

    return best, iterations, converged


def _fully_resolved(ranges: list[Range]) -> bool:
    """True once every free axis spans a negligible fraction of its own value."""
    for rng in ranges:
        if rng.fixed:
            continue
        scale = max(abs(rng.minimum), abs(rng.maximum), _MIN_POSITIVE)
        if (rng.maximum - rng.minimum) / scale > _RESOLUTION_TOLERANCE:
            return False
    return True


def _grid_search(ranges: list[Range], build_bank: BankBuilder, flux: np.ndarray,
                 errors: np.ndarray | None, mask: np.ndarray, ul: np.ndarray
                 ) -> tuple[_Best, int, bool]:
    """Refine the grid around its χ² minima and return the best model found.

    The best model ever seen is kept across iterations, so a refinement that
    lands on a shifted grid can lower the resolution but never the quality of
    the reported solution, and the refinement starts from several separated
    minima of the first grid rather than only its lowest cell.
    """
    grids, shape, rows, chi2 = _evaluate_grid(ranges, build_bank, flux, errors, mask, ul)
    if not np.any(np.isfinite(chi2)):
        raise SedFitError(
            "no model in the grid produces a finite SED that satisfies the "
            "upper limits; widen the ranges or drop the limits")

    # Every seed costs another refinement over a grid this size. On a coarse
    # four-parameter fit the extra seeds are what finds the optimum at all
    # (three seeds stop at χ² ≈ 6 where eight reach ≈ 1), so only a grid large
    # enough for the cost to matter gets fewer.
    budget = _MULTI_START_SEEDS if chi2.size <= _MULTI_START_CELL_BUDGET else 3
    seeds = _seed_indices(chi2, shape, budget)
    best: _Best | None = None
    total_iterations = 0
    converged = False
    for seed in seeds:
        candidate, iterations, seed_converged = _refine_from(
            seed, ranges, grids, rows, chi2, shape, build_bank, flux, errors, mask, ul)
        total_iterations = max(total_iterations, iterations)
        if best is None or candidate.chi2 < best.chi2:
            best = candidate
            converged = seed_converged

    assert best is not None
    return best, total_iterations, converged


def _nuisance_grids(axis: int, ranges: list[Range], best_values: list[float]) -> list[np.ndarray]:
    """Sampling of the parameters being profiled *over*, for one profiled axis.

    Two properties matter. It must be denser than the user's own step count, or
    the re-minimisation is cruder than the fit it is describing. And it must
    contain the best-fit value of every nuisance parameter, or the profile at
    the optimum can come out *above* the optimum's own χ² — which makes the
    threshold search start outside its own bracket and report an interval of
    zero. The refined optimum is by construction not on the user's grid, so it
    is added explicitly.
    """
    free = [i for i, rng in enumerate(ranges) if i != axis and not rng.fixed]
    per_axis = _PROFILE_NUISANCE_STEPS
    if free:
        # Keep the product bounded: this grid is evaluated once per profile step.
        while per_axis > 2 and per_axis ** len(free) > _PROFILE_NUISANCE_BUDGET:
            per_axis -= 1

    grids: list[np.ndarray] = []
    for i, rng in enumerate(ranges):
        if i == axis:
            grids.append(np.array([best_values[i]], dtype=float))
        elif rng.fixed:
            grids.append(np.array([rng.minimum], dtype=float))
        else:
            dense = Range(rng.minimum, rng.maximum, max(rng.steps, per_axis),
                          rng.log_like, rng.positive).values()
            grids.append(np.unique(np.append(dense, best_values[i])))
    return grids


def _profile_chi2(axis: int, value: float, nuisance: list[np.ndarray],
                  build_bank: BankBuilder, flux: np.ndarray, errors: np.ndarray | None,
                  mask: np.ndarray, ul: np.ndarray) -> float:
    """χ² profiled at ``axis = value``: the *other* parameters re-minimised.

    Holding the others at the best fit instead would give a conditional slice,
    which is narrower than the confidence interval whenever the parameters are
    correlated — and mass, temperature and β are strongly correlated in a
    greybody.
    """
    grids = list(nuisance)
    grids[axis] = np.array([value], dtype=float)
    bank = build_bank(grids)
    chi2 = _chi_square(bank.reshape(-1, bank.shape[-1]), flux, errors, mask, ul)
    finite = np.isfinite(chi2)
    return float(np.min(chi2[finite])) if np.any(finite) else float("inf")


def _threshold_crossing(axis: int, best_value: float, limit: float, threshold: float,
                        nuisance: list[np.ndarray], build_bank: BankBuilder,
                        flux: np.ndarray, errors: np.ndarray | None, mask: np.ndarray,
                        ul: np.ndarray) -> tuple[float, bool]:
    """Where the profile χ² first reaches ``threshold`` between best and ``limit``.

    Returns (value, open_ended); open_ended is True when the threshold is never
    reached on this side, so the interval is only a lower bound.

    Sampling the axis on a fixed grid is not enough: an interval narrower than
    one sample comes back as zero, which is how a 0.1 % photometric error used
    to produce a mass "known exactly". The crossing is instead bracketed by an
    outward scan from the best fit and then bisected, so the resolution follows
    the interval rather than the range.

    The scan takes the first sample above the threshold, so what it returns is
    the boundary of the region *containing the best fit*. A profile that rises
    above the threshold and falls back between two samples is missed, which
    widens the reported interval; it cannot narrow it.
    """
    if limit == best_value:
        # This side was never explored — the optimum sits on it. Saying the
        # interval ends here would claim a crossing that was never found.
        return best_value, True

    inside = best_value
    span = limit - best_value
    outside = None
    for step in range(1, _PROFILE_SCAN_STEPS + 1):
        candidate = best_value + span * (step / _PROFILE_SCAN_STEPS)
        if _profile_chi2(axis, candidate, nuisance, build_bank, flux, errors, mask,
                         ul) > threshold:
            outside = candidate
            break
        inside = candidate
    if outside is None:
        return limit, True

    for _ in range(_PROFILE_BISECTIONS):
        middle = 0.5 * (inside + outside)
        if middle == inside or middle == outside:
            break
        if _profile_chi2(axis, middle, nuisance, build_bank, flux, errors, mask,
                         ul) > threshold:
            outside = middle
        else:
            inside = middle
    return inside, False


def _profile_bounds(ranges: list[Range], build_bank: BankBuilder, best_values: list[float],
                    flux: np.ndarray, errors: np.ndarray | None, mask: np.ndarray,
                    ul: np.ndarray, chi2_best: float
                    ) -> tuple[list[tuple[float, float]], list[bool]]:
    """Δχ² = 1 interval per parameter, by profiling over the user's full range.

    Not read off the refined grid: that grid is a few cells wide around the
    optimum by construction, so its width measures the zoom, not the confidence
    interval. Each axis is instead scanned outwards from the best fit until the
    profile χ² crosses χ²_min + 1, with the other parameters re-minimised at
    every step.

    Returns the bounds and, per axis, whether the crossing was never reached on
    at least one side (so the interval is only a lower bound).
    """
    bounds: list[tuple[float, float]] = []
    open_ended: list[bool] = []

    for axis, rng in enumerate(ranges):
        value = best_values[axis]
        if rng.fixed:
            bounds.append((value, value))
            open_ended.append(False)
            continue

        nuisance = _nuisance_grids(axis, ranges, best_values)
        # Anchor the threshold on the profile's OWN value at the optimum. The
        # nuisance sampling is finite, so this can sit a hair above the fit's
        # χ²; taking the difference from it keeps the search inside its bracket
        # instead of starting outside it and returning a zero-width interval.
        at_best = _profile_chi2(axis, value, nuisance, build_bank, flux, errors, mask, ul)
        threshold = max(chi2_best, at_best) + _DELTA_CHI2

        # The refinement is allowed to escape the user's range; profile at least
        # as far as the solution it found.
        low_limit = min(rng.minimum, value)
        high_limit = max(rng.maximum, value)
        low, low_open = _threshold_crossing(axis, value, low_limit, threshold, nuisance,
                                            build_bank, flux, errors, mask, ul)
        high, high_open = _threshold_crossing(axis, value, high_limit, threshold, nuisance,
                                              build_bank, flux, errors, mask, ul)
        bounds.append((min(low, value), max(high, value)))
        open_ended.append(low_open or high_open)

    return bounds, open_ended


@dataclass
class SedFitResult:
    model: str
    parameters: dict[str, float]
    uncertainties: dict[str, float]
    parameter_bounds: dict[str, tuple[float, float]] = field(default_factory=dict)
    unbounded: list[str] = field(default_factory=list)
    chi2: float = 0.0
    chi2_reduced: float | None = 0.0
    dof: int = 0
    iterations: int = 0
    converged: bool = False
    at_boundary: list[str] = field(default_factory=list)
    model_wavelength_um: list[float] = field(default_factory=list)
    model_flux_jy: list[float] = field(default_factory=list)
    band_wavelength_um: list[float] = field(default_factory=list)
    fitted_flux_jy: list[float] = field(default_factory=list)
    luminosity_lsun: float = 0.0
    bands_used: int = 0
    bands_upper_limit: int = 0
    warnings: list[str] = field(default_factory=list)

    def as_dict(self) -> dict:
        return {
            "model": self.model,
            "parameters": self.parameters,
            "uncertainties": self.uncertainties,
            "parameter_bounds": {k: list(v) for k, v in self.parameter_bounds.items()},
            "unbounded": self.unbounded,
            "chi2": self.chi2,
            "chi2_reduced": self.chi2_reduced,
            "dof": self.dof,
            "iterations": self.iterations,
            "converged": self.converged,
            "at_boundary": self.at_boundary,
            "model_wavelength_um": self.model_wavelength_um,
            "model_flux_jy": self.model_flux_jy,
            "band_wavelength_um": self.band_wavelength_um,
            "fitted_flux_jy": self.fitted_flux_jy,
            "luminosity_lsun": self.luminosity_lsun,
            "bands_used": self.bands_used,
            "bands_upper_limit": self.bands_upper_limit,
            "warnings": self.warnings,
        }


# ── optically thin fit ────────────────────────────────────────────────────────

def fit_thin(wavelength_um, flux_jy, *, distance_pc: float, kappa_ref: float,
             lambda_ref_um: float, mass_range, temp_range, beta_range,
             errors_jy=None, upper_limits=None,
             colour_correction: bool = True) -> SedFitResult:
    """Fit an optically thin greybody (mass, temperature, β) by grid refinement."""
    lam, flux, errors, ul, order = _validate_sed(wavelength_um, flux_jy, errors_jy, upper_limits)
    _require_positive(distance_pc, "distance", "a positive number of parsecs")
    _require_positive(kappa_ref, "reference opacity", "positive")
    _require_positive(lambda_ref_um, "reference wavelength", "positive")

    ranges = [Range.parse(mass_range, "mass_range", log_like=True),
              Range.parse(temp_range, "temp_range", positive=True),
              Range.parse(beta_range, "beta_range")]
    _guard_grid_size(ranges)

    mask = _fit_mask(flux, ul)
    dof, dof_warning = _degrees_of_freedom(mask, ranges)
    dist_cm = float(distance_pc) * _PC_CM
    lam_factor = _jy_factor(lam)

    def build(grids: Sequence[np.ndarray]) -> np.ndarray:
        masses, temps, betas = grids
        planck_grid = np.stack([planck_lambda(lam, t) for t in temps]) / np.pi * lam_factor
        if colour_correction:
            planck_grid = planck_grid * _colour_correction(lam, temps)
        opacity = (lambda_ref_um / lam)[None, :] ** betas[:, None]      # (nβ, nλ)
        scale = masses * _MSUN_G * kappa_ref / dist_cm**2               # (nM,)
        return (scale[:, None, None, None]                              # (nM,nT,nβ,nλ)
                * planck_grid[None, :, None, :]
                * opacity[None, None, :, :])

    best, iterations, converged = _grid_search(ranges, build, flux, errors, mask, ul)
    bounds, open_ended = _profile_bounds(ranges, build, best.values, flux, errors,
                                         mask, ul, best.chi2)

    mass, temperature, beta = best.values
    curve = greybody_thin(_MODEL_LAMBDA, mass, temperature, beta,
                          distance_pc, kappa_ref, lambda_ref_um)
    keep = curve > 1e-8
    lum = bolometric_luminosity(_MODEL_LAMBDA[keep], curve[keep], distance_pc) if np.any(keep) else 0.0

    names = ["mass_msun", "temperature_k", "beta"]
    result = _assemble(
        model="thin", names=names, values=best.values, bounds=bounds, open_ended=open_ended,
        best=best, ranges=ranges, iterations=iterations, converged=converged,
        dof=dof, dof_warning=dof_warning, errors=errors, lam=lam, order=order,
        mask=mask, ul=ul, curve=curve, lum=lum)
    result.parameters["luminosity_lsun"] = lum
    if lum > 0 and mass > 0:
        result.parameters["l_over_m"] = lum / mass
    return result


# ── optically thick fit ───────────────────────────────────────────────────────

def fit_thick(wavelength_um, flux_jy, *, distance_pc: float, kappa_ref: float,
              lambda_ref_um: float, size_arcsec: float, temp_range, beta_range,
              lambda0_range, scale_range, errors_jy=None, upper_limits=None,
              colour_correction: bool = True) -> SedFitResult:
    """Fit an optically thick greybody (T, β, λ₀, angular size) by grid refinement.

    ``size_arcsec`` is the measured source size; ``scale_range`` multiplies it,
    so the fitted size is ``scale × size_arcsec``. Mass is *derived* from the
    solid angle, λ₀ and β rather than fitted, and its uncertainty is taken over
    the models actually inside the Δχ² ≤ 1 region — not over the corners of the
    marginal bounds, which need not be admissible models at all.
    """
    lam, flux, errors, ul, order = _validate_sed(wavelength_um, flux_jy, errors_jy, upper_limits)
    _require_positive(size_arcsec, "source size", "a positive angle in arcsec")
    _require_positive(distance_pc, "distance", "a positive number of parsecs")
    _require_positive(kappa_ref, "reference opacity", "positive")
    _require_positive(lambda_ref_um, "reference wavelength", "positive")

    ranges = [Range.parse(temp_range, "temp_range", positive=True),
              Range.parse(beta_range, "beta_range"),
              Range.parse(lambda0_range, "lambda0_range", positive=True),
              Range.parse(scale_range, "scale_range", positive=True)]
    _guard_grid_size(ranges)

    mask = _fit_mask(flux, ul)
    dof, dof_warning = _degrees_of_freedom(mask, ranges)
    lam_factor = _jy_factor(lam)

    def build(grids: Sequence[np.ndarray]) -> np.ndarray:
        temps, betas, lambda0s, scales = grids
        planck_grid = np.stack([planck_lambda(lam, t) for t in temps]) / np.pi * lam_factor
        if colour_correction:
            planck_grid = planck_grid * _colour_correction(lam, temps)
        tau = (lambda0s[:, None] / lam[None, :]) ** betas[:, None, None]   # (nβ,nλ0,nλ)
        emission = -np.expm1(-tau)
        omega = np.array([solid_angle_sr(s * float(size_arcsec)) for s in scales])
        return (planck_grid[:, None, None, None, :]        # (nT,nβ,nλ0,nS,nλ)
                * emission[None, :, :, None, :]
                * omega[None, None, None, :, None])

    best, iterations, converged = _grid_search(ranges, build, flux, errors, mask, ul)
    bounds, open_ended = _profile_bounds(ranges, build, best.values, flux, errors,
                                         mask, ul, best.chi2)

    temperature, beta, lambda0, scale = best.values
    size = scale * float(size_arcsec)
    curve = greybody_thick(_MODEL_LAMBDA, size, temperature, beta, lambda0)
    keep = curve > 1e-8
    lum = bolometric_luminosity(_MODEL_LAMBDA[keep], curve[keep], distance_pc) if np.any(keep) else 0.0
    mass = thick_mass_msun(size, beta, lambda0, distance_pc, kappa_ref, lambda_ref_um)

    names = ["temperature_k", "beta", "lambda0_um", "scale"]
    result = _assemble(
        model="thick", names=names, values=best.values, bounds=bounds, open_ended=open_ended,
        best=best, ranges=ranges, iterations=iterations, converged=converged,
        dof=dof, dof_warning=dof_warning, errors=errors, lam=lam, order=order,
        mask=mask, ul=ul, curve=curve, lum=lum)
    result.parameters["size_arcsec"] = size
    result.parameters["mass_msun"] = mass
    result.parameters["luminosity_lsun"] = lum
    if lum > 0 and mass > 0:
        result.parameters["l_over_m"] = lum / mass

    size_lo, size_hi = (b * float(size_arcsec) for b in bounds[3])
    result.parameter_bounds["size_arcsec"] = (size_lo, size_hi)
    result.uncertainties["size_arcsec"] = max(abs(size - size_lo), abs(size_hi - size))
    mass_lo, mass_hi = _thick_mass_bounds(bounds, float(size_arcsec), distance_pc,
                                         kappa_ref, lambda_ref_um)
    result.parameter_bounds["mass_msun"] = (mass_lo, mass_hi)
    result.uncertainties["mass_msun"] = max(abs(mass - mass_lo), abs(mass_hi - mass))
    result.warnings.append(
        "mass is derived from the fitted size, λ₀ and β, and its range is taken over the "
        "corners of their separate intervals; it is an indication of the spread, not a "
        "Δχ² = 1 interval")
    return result


def _thick_mass_bounds(bounds: list[tuple[float, float]], size_arcsec: float,
                       distance_pc: float, kappa_ref: float,
                       lambda_ref_um: float) -> tuple[float, float]:
    """Derived-mass range over the corners of the (β, λ₀, scale) intervals.

    Mass is not fitted in the thick model, it is computed from the solid angle,
    λ₀ and β, so this is the mass at the corners of three *separately profiled*
    intervals. Those corners are not guaranteed to be admissible models, and the
    three parameters are correlated, so treat this as an indication of the
    spread rather than a Δχ² = 1 interval on the mass.
    """
    (_temp, (b_lo, b_hi), (l_lo, l_hi), (s_lo, s_hi)) = bounds
    masses = [thick_mass_msun(s * size_arcsec, b, l, distance_pc, kappa_ref, lambda_ref_um)
              for s in (s_lo, s_hi) for b in (b_lo, b_hi) for l in (l_lo, l_hi)]
    return float(min(masses)), float(max(masses))


# ── shared helpers ────────────────────────────────────────────────────────────

def _require_positive(value, label: str, requirement: str) -> None:
    if not np.isfinite(value) or float(value) <= 0:
        raise SedFitError(f"{label} must be {requirement}")


def _degrees_of_freedom(mask: np.ndarray, ranges: Sequence[Range]) -> tuple[int, str]:
    """N_detections − N_free_parameters, and a warning when that is not positive.

    The legacy engines counted a parameter as free only when it had ≥5 grid
    steps, which made the reduced χ² depend on the sampling rather than on the
    model; a genuinely varying 4-step axis counted as zero.

    A non-positive result is returned as it is, not clamped to 1: an
    under-determined fit has no reduced χ², and inventing one lets it win a
    thin-versus-thick comparison on a number that means nothing.
    """
    n_points = int(np.count_nonzero(mask))
    n_free = sum(0 if r.fixed else 1 for r in ranges)
    dof = n_points - n_free
    if dof <= 0:
        return dof, (f"{n_points} detections for {n_free} free parameters: the fit is not "
                     "over-determined, so there is no reduced χ² and the uncertainties are "
                     "indicative only")
    return dof, ""


def _assemble(*, model: str, names: list[str], values: list[float],
              bounds: list[tuple[float, float]], open_ended: list[bool],
              best: _Best, ranges: list[Range], iterations: int, converged: bool,
              dof: int, dof_warning: str, errors: np.ndarray | None,
              lam: np.ndarray, order: np.ndarray, mask: np.ndarray, ul: np.ndarray,
              curve: np.ndarray, lum: float) -> SedFitResult:
    at_boundary = [n for n, (lo, hi), rng, v in zip(names, bounds, ranges, values)
                   if not rng.fixed and (v <= lo or v >= hi) and (lo != hi)]
    # Report the fitted band fluxes in the order the caller gave the bands.
    inverse = np.empty_like(order)
    inverse[order] = np.arange(order.size)
    fitted = np.asarray(best.row, dtype=float)[inverse]

    result = SedFitResult(
        model=model,
        parameters=dict(zip(names, values)),
        uncertainties={n: max(abs(v - lo), abs(hi - v))
                       for n, (lo, hi), v in zip(names, bounds, values)},
        parameter_bounds=dict(zip(names, bounds)),
        unbounded=[n for n, flag in zip(names, open_ended) if flag],
        chi2=best.chi2,
        chi2_reduced=(best.chi2 / dof) if dof > 0 else None,
        dof=dof,
        iterations=iterations,
        converged=converged,
        at_boundary=at_boundary,
        model_wavelength_um=_MODEL_LAMBDA.tolist(),
        model_flux_jy=curve.tolist(),
        band_wavelength_um=lam[inverse].tolist(),
        fitted_flux_jy=fitted.tolist(),
        luminosity_lsun=lum,
        bands_used=int(np.count_nonzero(mask)),
        bands_upper_limit=int(np.count_nonzero(ul)),
    )
    if result.unbounded:
        result.warnings.append(
            "the Δχ² ≤ 1 region reaches the edge of the searched range for "
            + ", ".join(result.unbounded)
            + "; the quoted uncertainty is a lower bound")
    if not converged:
        result.warnings.append(
            f"grid refinement stopped after {iterations} iterations without the χ² "
            "settling; treat the uncertainties as indicative")
    if errors is None:
        result.warnings.append(
            "no flux errors were supplied, so χ² is not a statistical quantity and "
            "the Δχ² ≤ 1 interval has no confidence-level meaning")
    if dof_warning:
        result.warnings.append(dof_warning)
    return result


def worker_sed_fit(model: str, wavelength_um, flux_jy, params: dict) -> dict:
    """Process-pool entry point: run one SED fit and return a JSON-ready dict."""
    kind = (model or "thin").strip().lower()
    common = dict(
        errors_jy=params.get("errors_jy"),
        upper_limits=params.get("upper_limits"),
        distance_pc=float(params.get("distance_pc", 1000.0)),
        kappa_ref=float(params.get("kappa_ref", 0.1)),
        lambda_ref_um=float(params.get("lambda_ref_um", 300.0)),
        colour_correction=bool(params.get("colour_correction", True)),
    )
    if kind == "thin":
        return fit_thin(
            wavelength_um, flux_jy,
            mass_range=params.get("mass_range", [1.0, 5000.0, 30]),
            temp_range=params.get("temp_range", [5.0, 50.0, 30]),
            beta_range=params.get("beta_range", [2.0, 2.0, 1]),
            **common).as_dict()
    if kind == "thick":
        return fit_thick(
            wavelength_um, flux_jy,
            size_arcsec=float(params.get("size_arcsec", 0.0)),
            temp_range=params.get("temp_range", [5.0, 50.0, 30]),
            beta_range=params.get("beta_range", [2.0, 2.0, 1]),
            lambda0_range=params.get("lambda0_range", [10.0, 300.0, 30]),
            scale_range=params.get("scale_range", [1.0, 1.0, 1]),
            **common).as_dict()
    raise SedFitError(f"unknown SED model '{model}' (expected 'thin' or 'thick')")
