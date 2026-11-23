"""Radial profiles, phase diagrams and a shrinking-sphere centre for the
selected rows of a particle table (issues #426 and #414).

Every function reads the selection through a chunk factory, as the exact
selection diagnostics do (routers/catalogue.py): ``chunks()`` yields
``(columns, done)`` with ``columns`` a dict of float64 arrays for one slice
of the selected rows and ``done`` the fraction of the table read so far. A
factory is called once per pass, so memory stays bounded by one chunk plus
the per-bin accumulators, whatever the selection's size, and an out-of-core
Gadget snapshot and a loaded table give the same answer.

Everything is computed in the table's own units (a Gadget snapshot's code
units: comoving kpc/h and so on when the run is comoving). Counts, masses,
densities, means, quantiles and slopes all commute with the conversion to
physical units, which the caller states beside the result.

Distances use the minimum image on the coordinates with a known period
(app/periodic.py). The shrinking-sphere centre follows Power et al. (2003,
MNRAS 338, 14, section 2.2): nested spheres, each 2.5 % smaller than the last
and centred on the mass-weighted centre of the particles inside the
previous one, stopping before fewer than min(1000, 1 % of the selection)
particles remain. Its start is the periodic centre of mass of Bai & Breen
(2008, J. Graphics Tools 13, 53) on a periodic axis, so it does not depend on
the order of the rows.
"""
from __future__ import annotations

import math
from collections.abc import Callable, Iterable

import numpy as np

from .periodic import separation

Chunks = Callable[[], Iterable[tuple[dict[str, np.ndarray], float]]]

#: Bins of the per-shell histograms a quantile is read from, then refined
#: once within the bin it falls in (resolution ~1e-6 of the shell's range).
QUANTILE_BINS = 1024
QUANTILES = (0.16, 0.5, 0.84)
#: Below this many selected particles no centre is searched.
MIN_SELECTION = 100
#: Particles a shrinking sphere keeps in memory once its nested selection is
#: this small (positions and mass in float64: 32 bytes a particle).
CENTRE_MEMORY_ROWS = 2_000_000
#: The centre velocity is the mass-weighted mean velocity inside a tenth of
#: the profile's outer radius, widened until it holds this many particles.
CENTRE_VELOCITY_PARTICLES = 1000
DERIVED_COLUMNS = ("radius", "radial_velocity")


class ProfileCancelled(Exception):
    """A profile or phase diagram stopped between two chunks."""


class _Passes:
    """Runs the passes of one computation, reporting progress over the
    expected number of passes and stopping between chunks when asked."""

    def __init__(self, chunks: Chunks, stop=None, progress=None, expected: int = 1):
        self.chunks, self.stop, self.progress = chunks, stop, progress
        self.expected, self.done = max(1, expected), 0

    def run(self):
        for columns, fraction in self.chunks():
            if self.stop is not None and self.stop.is_set():
                raise ProfileCancelled("Cancelled by client.")
            yield columns
            if self.progress is not None:
                self.progress(min(0.99, (self.done + float(fraction)) / self.expected))
        if self.stop is not None and self.stop.is_set():
            raise ProfileCancelled("Cancelled by client.")
        self.done += 1
        self.expected = max(self.expected, self.done + 1)


def _weights(columns: dict, mass_field: str, n: int) -> np.ndarray:
    if not mass_field:
        return np.ones(n, dtype=np.float64)
    return np.asarray(columns[mass_field], dtype=np.float64)


def _separations(columns: dict, pos_fields, centre, periods) -> list[np.ndarray]:
    return [separation(columns[f], c, p) for f, c, p in zip(pos_fields, centre, periods)]


def _usable(columns: dict, pos_fields, mass_field: str) -> tuple[np.ndarray, np.ndarray]:
    """Rows with finite positions and a finite non-negative mass, and their
    weights (1 each without a mass field)."""
    n = len(columns[pos_fields[0]])
    weight = _weights(columns, mass_field, n)
    keep = np.isfinite(weight) & (weight >= 0)
    for field in pos_fields:
        keep &= np.isfinite(columns[field])
    return keep, weight


def mass_centre(chunks: Chunks, pos_fields, mass_field: str, periods,
                passes: _Passes | None = None) -> dict:
    """Mass-weighted centre of every usable selected row: on an axis with a
    period L, the angle-averaged centre of Bai & Breen (2008), x_c =
    L/(2 pi) (atan2(-<m sin t>, -<m cos t>) + pi) with t = 2 pi x / L; on any
    other axis the plain weighted mean (about the first chunk's mean, so
    coordinates far from zero keep their digits)."""
    passes = passes or _Passes(chunks)
    n = 0
    weight_sum = 0.0
    sums = np.zeros((3, 2), dtype=np.float64)
    origin: list[float | None] = [None, None, None]
    for columns in passes.run():
        keep, weight = _usable(columns, pos_fields, mass_field)
        if not keep.any():
            continue
        w = weight[keep]
        n += int(keep.sum())
        weight_sum += float(w.sum())
        for axis, (field, period) in enumerate(zip(pos_fields, periods)):
            x = np.asarray(columns[field], dtype=np.float64)[keep]
            if period:
                theta = 2.0 * math.pi * x / period
                sums[axis, 0] += float(np.dot(w, np.cos(theta)))
                sums[axis, 1] += float(np.dot(w, np.sin(theta)))
            else:
                if origin[axis] is None:
                    origin[axis] = float(np.mean(x))
                sums[axis, 0] += float(np.dot(w, x - origin[axis]))
    if n == 0 or weight_sum <= 0:
        raise ValueError("The selection has no particle with a finite position and mass.")
    centre = []
    for axis, period in enumerate(periods):
        if period:
            xi, zeta = sums[axis, 0] / weight_sum, sums[axis, 1] / weight_sum
            angle = math.atan2(-zeta, -xi) + math.pi
            centre.append(float(period * angle / (2.0 * math.pi)) % period)
        else:
            centre.append(float(origin[axis] + sums[axis, 0] / weight_sum))
    return {"centre": centre, "rows": n, "mass": weight_sum}


def _max_distance(passes: _Passes, pos_fields, mass_field, centre,
                  periods) -> tuple[float, float, int]:
    """(largest, smallest positive) distance of a usable row from ``centre``,
    and the number of usable rows."""
    largest, smallest, n = 0.0, math.inf, 0
    for columns in passes.run():
        keep, _ = _usable(columns, pos_fields, mass_field)
        if not keep.any():
            continue
        n += int(keep.sum())
        delta = _separations({f: np.asarray(columns[f])[keep] for f in pos_fields},
                             pos_fields, centre, periods)
        d = np.sqrt(delta[0] ** 2 + delta[1] ** 2 + delta[2] ** 2)
        largest = max(largest, float(d.max()))
        positive = d[d > 0]
        if positive.size:
            smallest = min(smallest, float(positive.min()))
    if n == 0:
        raise ValueError("The selection has no particle with a finite position and mass.")
    return largest, smallest, n


def shrinking_sphere_centre(chunks: Chunks, pos_fields, mass_field: str, periods, *,
                            start=None, initial_radius: float | None = None,
                            shrink: float = 0.975, min_particles: int = 1000,
                            memory_rows: int = CENTRE_MEMORY_ROWS,
                            stop=None, progress=None) -> dict:
    """The shrinking-sphere centre of the selection (see the module text).

    Step 0 takes the mass-weighted centre of the particles within
    ``initial_radius`` of ``start``; step k that of the particles of step k-1
    within ``shrink``**k times the initial radius of the centre of step k-1
    (nested spheres). Out of core, the members of the current sphere are
    kept as one bit per row and chunk, so each step reads the table once and
    tests one sphere; once a sphere holds at most ``memory_rows`` particles
    they are kept in memory, collected in the same pass, with the same
    result. The answer is the centre of the last sphere with at least
    ``n_min = min(min_particles, max(100, ceil(1 % of the selection)))``
    particles; a start sphere holding fewer is refused."""
    if not 0.5 <= shrink < 1.0:
        raise ValueError("The shrink factor must be in [0.5, 1).")

    def report(fraction):
        if progress is not None:
            progress(fraction)

    passes = _Passes(chunks, stop, lambda f: report(0.1 * f), expected=2)
    total = mass_centre(chunks, pos_fields, mass_field, periods, passes)
    if total["rows"] < MIN_SELECTION:
        raise ValueError(f"A shrinking-sphere centre needs at least {MIN_SELECTION} "
                         f"particles; the selection has {total['rows']}.")
    centre = np.asarray(start if start is not None else total["centre"], dtype=np.float64)
    radius = initial_radius
    if radius is None:
        radius = _max_distance(passes, pos_fields, mass_field, centre, periods)[0]
        radius = radius * (1.0 + 1e-12) or 1.0
    if not (math.isfinite(radius) and radius > 0):
        raise ValueError("The initial radius must be positive.")
    passes.progress = None
    n_min = min(int(min_particles), max(MIN_SELECTION, math.ceil(0.01 * total["rows"])))
    start_centre, r0 = centre.copy(), float(radius)
    masks: list[np.ndarray] | None = None
    members = None
    steps, n_inside, r_used = 0, 0, r0
    log_span = math.log(max(total["rows"], n_min + 1) / n_min)

    while True:
        r_next = r0 if steps == 0 else r_used * shrink
        collected = None
        if members is None:
            n, w_sum, s = 0, 0.0, np.zeros(3)
            new_masks, kept_pos, kept_w = [], [], []
            for index, columns in enumerate(passes.run()):
                keep, weight = _usable(columns, pos_fields, mass_field)
                if masks is not None:
                    keep &= np.unpackbits(masks[index], count=keep.size).astype(bool)
                delta = _separations(columns, pos_fields, centre, periods)
                keep &= delta[0] ** 2 + delta[1] ** 2 + delta[2] ** 2 <= r_next * r_next
                new_masks.append(np.packbits(keep))
                if keep.any():
                    n += int(keep.sum())
                    w = weight[keep]
                    w_sum += float(w.sum())
                    s += [float(np.dot(w, d[keep])) for d in delta]
                    if n <= memory_rows:
                        kept_pos.append(np.column_stack(
                            [np.asarray(columns[f], dtype=np.float64)[keep]
                             for f in pos_fields]))
                        kept_w.append(w)
            if n <= memory_rows:
                collected = (np.concatenate(kept_pos) if kept_pos else np.empty((0, 3)),
                             np.concatenate(kept_w) if kept_w else np.empty(0))
        else:
            pos, weight = members
            delta = _separations(dict(zip(pos_fields, pos.T)), pos_fields, centre, periods)
            keep = delta[0] ** 2 + delta[1] ** 2 + delta[2] ** 2 <= r_next * r_next
            n = int(keep.sum())
            w = weight[keep]
            w_sum = float(w.sum())
            s = np.array([float(np.dot(w, d[keep])) for d in delta])
            collected = (pos[keep], w)
        if stop is not None and stop.is_set():
            raise ProfileCancelled("Cancelled by client.")
        if n < n_min or w_sum <= 0:
            if steps == 0:
                raise ValueError(
                    f"The start sphere (radius {r0:.6g}) holds {n} particles, fewer than "
                    f"the {n_min} a centre needs: give a larger initial radius.")
            break
        new_centre = centre + s / w_sum
        for axis, period in enumerate(periods):
            if period:
                new_centre[axis] %= period
        centre, n_inside, r_used, steps = new_centre, n, r_next, steps + 1
        if members is None:
            masks = new_masks
        if collected is not None:
            members, masks = collected, None
        report(min(0.99, 0.1 + 0.89 * (1.0 - math.log(max(n, n_min) / n_min) / log_span)))
    return {"centre": [float(v) for v in centre], "radius": float(r_used),
            "particles_inside": int(n_inside), "steps": steps, "n_min": n_min,
            "start": [float(v) for v in start_centre], "initial_radius": r0,
            "shrink_factor": float(shrink), "selected_particles": total["rows"],
            "out_of_core_passes": passes.done}


def _spherical(delta, velocity):
    """Radial, polar and azimuthal velocity components and the distance."""
    x, y, z = delta
    vx, vy, vz = velocity
    d = np.sqrt(x * x + y * y + z * z)
    cyl = np.sqrt(x * x + y * y)
    with np.errstate(invalid="ignore", divide="ignore"):
        v_r = np.where(d > 0, (x * vx + y * vy + z * vz) / d, 0.0)
        v_theta = np.where((d > 0) & (cyl > 0), (z * (x * vx + y * vy) / cyl - cyl * vz) / d, 0.0)
        v_phi = np.where(cyl > 0, (x * vy - y * vx) / cyl, 0.0)
    return d, v_r, v_theta, v_phi


class Geometry:
    """Centre, centre velocity and periods shared by the derived columns."""

    def __init__(self, pos_fields, vel_fields, centre, periods, centre_velocity=None):
        self.pos_fields = list(pos_fields)
        self.vel_fields = list(vel_fields or [])
        self.centre = np.asarray(centre, dtype=np.float64)
        self.periods = list(periods)
        self.centre_velocity = (np.asarray(centre_velocity, dtype=np.float64)
                                if centre_velocity is not None else None)

    def derive(self, columns: dict) -> dict:
        """``columns`` with "radius" and, when velocities and a centre
        velocity are known, "radial_velocity" plus the private components."""
        delta = _separations(columns, self.pos_fields, self.centre, self.periods)
        out = dict(columns)
        out["_delta"] = delta
        if self.vel_fields and self.centre_velocity is not None:
            vel = [np.asarray(columns[f], dtype=np.float64) - c
                   for f, c in zip(self.vel_fields, self.centre_velocity)]
            d, v_r, v_t, v_p = _spherical(delta, vel)
            out.update(radius=d, radial_velocity=v_r, _v_theta=v_t, _v_phi=v_p, _vel=vel)
        else:
            out["radius"] = np.sqrt(delta[0] ** 2 + delta[1] ** 2 + delta[2] ** 2)
        return out


def centre_velocity(passes: _Passes, geometry: Geometry, mass_field: str,
                    r_inner: float, r_max: float) -> dict:
    """Mass-weighted mean velocity inside ``r_inner`` (a tenth of the outer
    radius), widened to the smallest radius holding
    CENTRE_VELOCITY_PARTICLES particles, at most ``r_max``: one pass, with
    the sums kept per log-spaced radius bin."""
    edges = np.concatenate([[0.0], np.geomspace(r_inner, r_max, 65)])
    counts = np.zeros(65)
    masses = np.zeros(65)
    momenta = np.zeros((3, 65))
    for columns in passes.run():
        keep, weight = _usable(columns, geometry.pos_fields, mass_field)
        for f in geometry.vel_fields:
            keep &= np.isfinite(columns[f])
        if not keep.any():
            continue
        sub = {f: np.asarray(columns[f], dtype=np.float64)[keep]
               for f in geometry.pos_fields + geometry.vel_fields}
        delta = _separations(sub, geometry.pos_fields, geometry.centre, geometry.periods)
        d = np.sqrt(delta[0] ** 2 + delta[1] ** 2 + delta[2] ** 2)
        index = np.searchsorted(edges, d, side="right") - 1
        inside = index < 65
        w = weight[keep][inside]
        index = index[inside]
        counts += np.bincount(index, minlength=65)
        masses += np.bincount(index, weights=w, minlength=65)
        for axis, f in enumerate(geometry.vel_fields):
            momenta[axis] += np.bincount(index, weights=w * sub[f][inside], minlength=65)
    cum_n, cum_m = np.cumsum(counts), np.cumsum(masses)
    chosen = next((k for k in range(65)
                   if cum_n[k] >= CENTRE_VELOCITY_PARTICLES and cum_m[k] > 0), 64)
    if cum_m[chosen] <= 0:
        raise ValueError("No particle with a finite velocity inside the profile.")
    velocity = np.cumsum(momenta, axis=1)[:, chosen] / cum_m[chosen]
    return {"velocity": [float(v) for v in velocity], "radius": float(edges[chosen + 1]),
            "particles": int(cum_n[chosen])}


def default_centre_velocity(chunks: Chunks, pos_fields, vel_fields, mass_field: str, centre,
                            periods, region_limit: float | None = None, stop=None,
                            progress=None) -> dict:
    """The centre velocity a radius-velocity diagram uses when none is
    given: as for a profile, with the outer radius the selection's extent
    (bounded by the selected region and half the period)."""
    passes = _Passes(chunks, stop, progress, expected=2)
    centre = np.asarray(centre, dtype=np.float64)
    largest = _max_distance(passes, pos_fields, mass_field, centre, periods)[0]
    outer = min([largest] + ([region_limit] if region_limit else [])
                + [0.5 * p for p in periods if p])
    if not outer > 0:
        raise ValueError("Every selected particle sits at the centre.")
    return centre_velocity(passes, Geometry(pos_fields, vel_fields, centre, periods),
                           mass_field, 0.1 * outer, outer)


def _quantile_scale(lo: np.ndarray, hi: np.ndarray) -> np.ndarray:
    """Per shell: read the quantiles on log10 when every value is positive
    and they span more than a factor 100."""
    with np.errstate(divide="ignore", invalid="ignore"):
        return (lo > 0) & (hi / lo > 100.0)


def _transform(values, shell, use_log, t_lo, t_hi):
    """Each value's position on its shell's quantile axis and coarse bin."""
    with np.errstate(divide="ignore", invalid="ignore"):
        t = np.where(use_log[shell], np.log10(np.where(values > 0, values, np.nan)), values)
        width = t_hi[shell] - t_lo[shell]
        frac = np.where(width > 0, (t - t_lo[shell]) / width, 0.0)
    coarse = np.clip(np.floor(frac * QUANTILE_BINS), 0, QUANTILE_BINS - 1).astype(np.int64)
    return t, coarse


def radial_profile(chunks: Chunks, *, pos_fields, mass_field: str, vel_fields, columns,
                   centre, periods, centre_velocity_value=None, r_min=None, r_max=None,
                   bins: int = 32, region_limit: float | None = None,
                   stop=None, progress=None) -> dict:
    """Log-spaced shells [r_lo, r_hi) around ``centre`` (see module text).

    Per shell: particle count, mass, density (mass over the shell volume),
    enclosed mass M(<r_hi) (every particle closer than r_hi, the ones inside
    r_min too), mass-weighted mean radius, and for each of ``columns``
    (table columns or "radial_velocity") the mass-weighted mean, standard
    deviation and 16/50/84 % quantiles. With velocities: mean radial and
    azimuthal velocity, sigma_r, sigma_theta, sigma_phi, the 1-D dispersion
    sqrt((sigma_r^2 + sigma_theta^2 + sigma_phi^2) / 3) (each about its shell
    mean, so infall and rotation are not dispersion) and the anisotropy beta
    = 1 - (sigma_theta^2 + sigma_phi^2) / (2 sigma_r^2); the Cartesian
    dispersion, which counts streaming, is given apart."""
    if not 4 <= bins <= 256:
        raise ValueError("A profile has between 4 and 256 shells.")
    with_velocity = bool(vel_fields)
    passes = _Passes(chunks, stop, progress,
                     expected=4 + int(with_velocity and centre_velocity_value is None))
    centre = np.asarray(centre, dtype=np.float64)
    largest, smallest, _ = _max_distance(passes, pos_fields, mass_field, centre, periods)
    if largest <= 0:
        raise ValueError("Every selected particle sits at the centre.")
    limits = [largest * (1.0 + 1e-12)]
    if region_limit is not None and region_limit > 0:
        limits.append(region_limit)
    half_periods = [0.5 * p for p in periods if p]
    limits += half_periods
    if r_max is None:
        r_max = min(limits)
    if r_min is None:
        r_min = max(smallest if math.isfinite(smallest) else 0.0, r_max * 1e-3)
    if not (math.isfinite(r_min) and math.isfinite(r_max) and 0 < r_min < r_max):
        raise ValueError("The profile needs 0 < r_min < r_max.")
    complete_radius = min([math.inf if region_limit is None else region_limit] + half_periods)
    edges = np.geomspace(r_min, r_max, bins + 1)

    velocity_info = None
    if with_velocity and centre_velocity_value is None:
        velocity_info = centre_velocity(passes, Geometry(pos_fields, vel_fields, centre, periods),
                                        mass_field, 0.1 * r_max, r_max)
        centre_velocity_value = velocity_info["velocity"]
    geometry = Geometry(pos_fields, vel_fields, centre, periods,
                        centre_velocity_value if with_velocity else None)
    columns = list(columns)
    if "radial_velocity" in columns and not with_velocity:
        raise ValueError("radial_velocity needs the velocity fields.")
    vel_keys = (["radial_velocity", "_v_theta", "_v_phi", "_vx", "_vy", "_vz"]
                if with_velocity else [])

    def shells_of(chunk):
        keep, weight = _usable(chunk, pos_fields, mass_field)
        sub = {k: np.asarray(v, dtype=np.float64)[keep] for k, v in chunk.items()}
        derived = geometry.derive(sub)
        if with_velocity:
            derived["_vx"], derived["_vy"], derived["_vz"] = derived["_vel"]
        d = derived["radius"]
        shell = np.searchsorted(edges, d, side="right") - 1
        return derived, weight[keep], d, shell

    n_inner = 0
    m_inner = 0.0
    n_outer = 0
    count = np.zeros(bins)
    mass = np.zeros(bins)
    radius_sum = np.zeros(bins)
    col_w = {c: np.zeros(bins) for c in columns}
    col_sum = {c: np.zeros(bins) for c in columns}
    col_lo = {c: np.full(bins, np.inf) for c in columns}
    col_hi = {c: np.full(bins, -np.inf) for c in columns}
    vel_sum = {k: np.zeros(bins) for k in vel_keys}
    vel_w = np.zeros(bins)
    selected = 0
    for chunk in passes.run():
        selected += len(chunk[pos_fields[0]])
        derived, w, d, shell = shells_of(chunk)
        inner = d < r_min
        n_inner += int(inner.sum())
        m_inner += float(w[inner].sum())
        outer = shell >= bins
        n_outer += int(outer.sum())
        ok = (shell >= 0) & ~outer
        s, ws = shell[ok], w[ok]
        count += np.bincount(s, minlength=bins)
        mass += np.bincount(s, weights=ws, minlength=bins)
        radius_sum += np.bincount(s, weights=ws * d[ok], minlength=bins)
        if with_velocity:
            moving = np.isfinite(derived["_vx"][ok]) & np.isfinite(derived["_vy"][ok]) \
                & np.isfinite(derived["_vz"][ok])
            sv, wv = s[moving], ws[moving]
            vel_w += np.bincount(sv, weights=wv, minlength=bins)
            for k in vel_keys:
                vel_sum[k] += np.bincount(sv, weights=wv * derived[k][ok][moving],
                                          minlength=bins)
        for c in columns:
            v = derived[c][ok]
            fin = np.isfinite(v)
            if not fin.any():
                continue
            sf, vf, wf = s[fin], v[fin], ws[fin]
            col_w[c] += np.bincount(sf, weights=wf, minlength=bins)
            col_sum[c] += np.bincount(sf, weights=wf * vf, minlength=bins)
            np.minimum.at(col_lo[c], sf, vf)
            np.maximum.at(col_hi[c], sf, vf)
    with np.errstate(invalid="ignore", divide="ignore"):
        col_mean = {c: np.where(col_w[c] > 0, col_sum[c] / col_w[c], np.nan) for c in columns}
        vel_mean = {k: np.where(vel_w > 0, vel_sum[k] / vel_w, np.nan) for k in vel_keys}
    use_log = {c: _quantile_scale(col_lo[c], col_hi[c]) for c in columns}
    t_lo, t_hi = {}, {}
    for c in columns:
        with np.errstate(divide="ignore", invalid="ignore"):
            t_lo[c] = np.where(use_log[c], np.log10(col_lo[c]), col_lo[c])
            t_hi[c] = np.where(use_log[c], np.log10(col_hi[c]), col_hi[c])

    col_dev = {c: np.zeros(bins) for c in columns}
    vel_dev = {k: np.zeros(bins) for k in vel_keys}
    coarse = {c: np.zeros(bins * QUANTILE_BINS) for c in columns}
    for chunk in passes.run():
        derived, w, d, shell = shells_of(chunk)
        ok = (shell >= 0) & (shell < bins)
        s, ws = shell[ok], w[ok]
        for k in vel_keys:
            moving = np.isfinite(derived[k][ok])
            dev = derived[k][ok][moving] - vel_mean[k][s[moving]]
            vel_dev[k] += np.bincount(s[moving], weights=ws[moving] * dev * dev,
                                      minlength=bins)
        for c in columns:
            v = derived[c][ok]
            fin = np.isfinite(v)
            if not fin.any():
                continue
            sf, vf, wf = s[fin], v[fin], ws[fin]
            dev = vf - col_mean[c][sf]
            col_dev[c] += np.bincount(sf, weights=wf * dev * dev, minlength=bins)
            _, cb = _transform(vf, sf, use_log[c], t_lo[c], t_hi[c])
            coarse[c] += np.bincount(sf * QUANTILE_BINS + cb, weights=wf,
                                     minlength=bins * QUANTILE_BINS)

    brackets = {}
    for c in columns:
        hist = coarse[c].reshape(bins, QUANTILE_BINS)
        cum = np.cumsum(hist, axis=1)
        total = cum[:, -1]
        found = np.zeros((bins, len(QUANTILES)), dtype=np.int64)
        before = np.zeros((bins, len(QUANTILES)))
        for qi, q in enumerate(QUANTILES):
            target = q * total
            k = np.array([int(np.searchsorted(cum[i], target[i], side="left"))
                          for i in range(bins)])
            k = np.clip(k, 0, QUANTILE_BINS - 1)
            found[:, qi] = k
            before[:, qi] = np.where(k > 0, cum[np.arange(bins), np.maximum(k - 1, 0)], 0.0)
        brackets[c] = (found, before, total)

    fine = {c: np.zeros(bins * len(QUANTILES) * QUANTILE_BINS) for c in columns}
    for chunk in passes.run():
        derived, w, d, shell = shells_of(chunk)
        ok = (shell >= 0) & (shell < bins)
        s, ws = shell[ok], w[ok]
        for c in columns:
            v = derived[c][ok]
            fin = np.isfinite(v)
            if not fin.any():
                continue
            sf, vf, wf = s[fin], v[fin], ws[fin]
            t, cb = _transform(vf, sf, use_log[c], t_lo[c], t_hi[c])
            found = brackets[c][0]
            step = (t_hi[c] - t_lo[c]) / QUANTILE_BINS
            for qi in range(len(QUANTILES)):
                inside = cb == found[sf, qi]
                if not inside.any():
                    continue
                si = sf[inside]
                start = t_lo[c][si] + found[si, qi] * step[si]
                with np.errstate(invalid="ignore", divide="ignore"):
                    frac = np.where(step[si] > 0, (t[inside] - start) / step[si], 0.0)
                sub_bin = np.clip(np.floor(frac * QUANTILE_BINS), 0,
                                  QUANTILE_BINS - 1).astype(np.int64)
                fine[c] += np.bincount((si * len(QUANTILES) + qi) * QUANTILE_BINS + sub_bin,
                                       weights=wf[inside],
                                       minlength=bins * len(QUANTILES) * QUANTILE_BINS)

    quantiles = {}
    for c in columns:
        found, before, total = brackets[c]
        hist = fine[c].reshape(bins, len(QUANTILES), QUANTILE_BINS)
        step = (t_hi[c] - t_lo[c]) / QUANTILE_BINS
        out = np.full((bins, len(QUANTILES)), np.nan)
        for i in range(bins):
            if not total[i] > 0:
                continue
            for qi, q in enumerate(QUANTILES):
                target = q * total[i] - before[i, qi]
                cum = np.cumsum(hist[i, qi])
                g = min(int(np.searchsorted(cum, target, side="left")), QUANTILE_BINS - 1)
                below = cum[g - 1] if g > 0 else 0.0
                inside = hist[i, qi, g]
                frac = (target - below) / inside if inside > 0 else 0.0
                t = (t_lo[c][i] + found[i, qi] * step[i]
                     + (g + min(max(frac, 0.0), 1.0)) * step[i] / QUANTILE_BINS)
                value = 10.0 ** t if use_log[c][i] else t
                out[i, qi] = min(max(value, col_lo[c][i]), col_hi[c][i])
        quantiles[c] = out

    if progress is not None:
        progress(1.0)
    volume = 4.0 / 3.0 * math.pi * (edges[1:] ** 3 - edges[:-1] ** 3)
    with np.errstate(invalid="ignore", divide="ignore"):
        mean_radius = np.where(mass > 0, radius_sum / mass, np.nan)
        result = {
            "r_lo": edges[:-1].tolist(), "r_hi": edges[1:].tolist(),
            "r_mid": np.sqrt(edges[:-1] * edges[1:]).tolist(),
            "r_mean": _finite_list(mean_radius),
            "complete": (edges[1:] <= complete_radius * (1 + 1e-12)).tolist(),
            "count": count.astype(np.int64).tolist(), "mass": mass.tolist(),
            "density": (mass / volume).tolist(),
            "enclosed_mass": (m_inner + np.cumsum(mass)).tolist(),
            "inside_r_min": {"count": n_inner, "mass": m_inner},
            "outside_r_max": {"count": n_outer},
            "selected_rows": selected, "r_min": float(r_min), "r_max": float(r_max),
            "complete_radius": None if math.isinf(complete_radius) else float(complete_radius),
            "columns": {},
        }
        for c in columns:
            std = np.sqrt(np.where(col_w[c] > 0, col_dev[c] / col_w[c], np.nan))
            result["columns"][c] = {
                "mean": _finite_list(col_mean[c]), "std": _finite_list(std),
                "p16": _finite_list(quantiles[c][:, 0]),
                "median": _finite_list(quantiles[c][:, 1]),
                "p84": _finite_list(quantiles[c][:, 2]),
                "weight": col_w[c].tolist(),
                "quantile_axis": ["log" if x else "linear" for x in use_log[c]],
            }
        if with_velocity:
            var = {k: np.where(vel_w > 0, vel_dev[k] / vel_w, np.nan) for k in vel_keys}
            sigma_t2 = var["_v_theta"] + var["_v_phi"]
            result["velocity"] = {
                "centre_velocity": [float(v) for v in geometry.centre_velocity],
                "centre_velocity_from": velocity_info,
                "mean_radial": _finite_list(vel_mean["radial_velocity"]),
                "mean_azimuthal": _finite_list(vel_mean["_v_phi"]),
                "sigma_r": _finite_list(np.sqrt(var["radial_velocity"])),
                "sigma_theta": _finite_list(np.sqrt(var["_v_theta"])),
                "sigma_phi": _finite_list(np.sqrt(var["_v_phi"])),
                "sigma_1d": _finite_list(np.sqrt((var["radial_velocity"] + sigma_t2) / 3.0)),
                "beta": _finite_list(1.0 - sigma_t2 / (2.0 * var["radial_velocity"])),
                "sigma_cartesian_with_streaming": _finite_list(
                    np.sqrt((var["_vx"] + var["_vy"] + var["_vz"]) / 3.0)),
            }
    return result


def _finite_list(values) -> list:
    return [float(v) if np.isfinite(v) else None for v in np.asarray(values, dtype=np.float64)]


def _axis_edges(lo: float, hi: float, bins: int, log: bool) -> np.ndarray:
    if log:
        return np.geomspace(lo, hi, bins + 1)
    return np.linspace(lo, hi, bins + 1)


def phase_diagram(chunks: Chunks, *, x: str, y: str, x_log: bool, y_log: bool,
                  x_bins: int, y_bins: int, x_range=None, y_range=None,
                  weight_field: str = "", derive=None, stop=None, progress=None) -> dict:
    """2-D histogram of the selected rows over columns ``x`` and ``y``.

    Bins are [lo, hi) except the last, which holds its upper edge (as
    numpy's). A row is binned, outside the range (``outside``), or dropped:
    a non-finite value, or a value <= 0 on a log axis. Rows with a weight
    that is not finite and >= 0 are counted apart (``invalid_weight_rows``)
    and carry no weight. So ``binned + outside + dropped`` equals the
    selected weight (up to float64 rounding of the sums), and
    ``binned + outside + dropped + invalid_weight`` the selected rows."""
    for bins in (x_bins, y_bins):
        if not 4 <= bins <= 512:
            raise ValueError("A phase-diagram axis has between 4 and 512 bins.")
    need_range = x_range is None or y_range is None
    passes = _Passes(chunks, stop, progress, expected=1 + int(need_range))
    derive = derive or (lambda columns: columns)

    def prepare(chunk):
        cols = derive(chunk)
        n = len(cols[x])
        w = (np.asarray(cols[weight_field], dtype=np.float64) if weight_field
             else np.ones(n))
        good_w = np.isfinite(w) & (w >= 0)
        xv = np.asarray(cols[x], dtype=np.float64)
        yv = np.asarray(cols[y], dtype=np.float64)
        placeable = np.isfinite(xv) & np.isfinite(yv)
        if x_log:
            placeable &= xv > 0
        if y_log:
            placeable &= yv > 0
        return xv, yv, w, good_w, placeable

    if need_range:
        lo = [np.inf, np.inf]
        hi = [-np.inf, -np.inf]
        for chunk in passes.run():
            xv, yv, _, good_w, placeable = prepare(chunk)
            ok = placeable & good_w
            if ok.any():
                for axis, v in enumerate((xv[ok], yv[ok])):
                    lo[axis] = min(lo[axis], float(v.min()))
                    hi[axis] = max(hi[axis], float(v.max()))
        if not np.isfinite(lo[0]):
            raise ValueError("No selected row has values that can be placed on both axes.")
        for axis, log in enumerate((x_log, y_log)):
            if lo[axis] >= hi[axis]:
                if log:
                    lo[axis], hi[axis] = lo[axis] / 2.0, hi[axis] * 2.0
                else:
                    pad = max(abs(lo[axis]) * 0.5, 0.5)
                    lo[axis], hi[axis] = lo[axis] - pad, hi[axis] + pad
        x_range = x_range or (lo[0], hi[0])
        y_range = y_range or (lo[1], hi[1])
    for (a, b), log in ((x_range, x_log), (y_range, y_log)):
        if not (math.isfinite(a) and math.isfinite(b) and a < b and (a > 0 or not log)):
            raise ValueError("An axis range needs two finite increasing values, positive "
                             "on a log axis.")
    x_edges = _axis_edges(float(x_range[0]), float(x_range[1]), x_bins, x_log)
    y_edges = _axis_edges(float(y_range[0]), float(y_range[1]), y_bins, y_log)

    counts = np.zeros((x_bins, y_bins))
    weights = np.zeros((x_bins, y_bins))
    tally = {"selected": [0, 0.0], "outside": [0, 0.0], "dropped": [0, 0.0]}
    invalid_weight_rows = 0
    for chunk in passes.run():
        xv, yv, w, good_w, placeable = prepare(chunk)
        invalid_weight_rows += int((~good_w).sum())
        w = np.where(good_w, w, 0.0)
        tally["selected"][0] += int(good_w.size)
        tally["selected"][1] += float(w.sum())
        drop = good_w & ~placeable
        tally["dropped"][0] += int(drop.sum())
        tally["dropped"][1] += float(w[drop].sum())
        ok = good_w & placeable
        xi = _bin_index(xv[ok], x_edges)
        yi = _bin_index(yv[ok], y_edges)
        inside = (xi >= 0) & (yi >= 0)
        tally["outside"][0] += int((~inside).sum())
        tally["outside"][1] += float(w[ok][~inside].sum())
        flat = xi[inside] * y_bins + yi[inside]
        counts += np.bincount(flat, minlength=x_bins * y_bins).reshape(x_bins, y_bins)
        weights += np.bincount(flat, weights=w[ok][inside],
                               minlength=x_bins * y_bins).reshape(x_bins, y_bins)
    if progress is not None:
        progress(1.0)
    return {"x_edges": x_edges.tolist(), "y_edges": y_edges.tolist(),
            "counts": counts.astype(np.int64).tolist(), "weights": weights.tolist(),
            "selected_rows": tally["selected"][0], "selected_weight": tally["selected"][1],
            "binned_rows": int(counts.sum()), "binned_weight": float(weights.sum()),
            "outside_rows": tally["outside"][0], "outside_weight": tally["outside"][1],
            "dropped_rows": tally["dropped"][0], "dropped_weight": tally["dropped"][1],
            "invalid_weight_rows": invalid_weight_rows}


def _bin_index(values: np.ndarray, edges: np.ndarray) -> np.ndarray:
    """Bin of each value, -1 outside [edges[0], edges[-1]] (the last bin
    holds its upper edge)."""
    index = np.searchsorted(edges, values, side="right") - 1
    index[values == edges[-1]] = len(edges) - 2
    index[(index < 0) | (index >= len(edges) - 1)] = -1
    return index
