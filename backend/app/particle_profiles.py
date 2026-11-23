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
#: The centre velocity and polar axis come from the smallest sphere about the
#: centre holding this many particles (centre_kinematics).
CENTRE_VELOCITY_PARTICLES = 1000
DERIVED_COLUMNS = ("radius", "radial_velocity")
GEOMETRIES = ("spherical", "cylindrical", "axial")
#: Bin index of a row outside a cylindrical profile's slab or an axial
#: profile's cylinder (below the first edge is -1, above the last ``bins``).
EXCLUDED = -2


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
    """Distance and the radial, polar and azimuthal velocity components about
    the third axis of ``delta``/``velocity``. The radial velocity of a
    particle at the centre is undefined (NaN); on the polar axis the polar
    and azimuthal components are taken as 0."""
    x, y, z = delta
    vx, vy, vz = velocity
    d = np.sqrt(x * x + y * y + z * z)
    cyl = np.sqrt(x * x + y * y)
    with np.errstate(invalid="ignore", divide="ignore"):
        v_r = np.where(d > 0, (x * vx + y * vy + z * vz) / d, np.nan)
        v_theta = np.where((d > 0) & (cyl > 0), (z * (x * vx + y * vy) / cyl - cyl * vz) / d, 0.0)
        v_phi = np.where(cyl > 0, (x * vy - y * vx) / cyl, 0.0)
    return d, v_r, v_theta, v_phi


def _basis(axis) -> np.ndarray:
    """Rows e1, e2, e3 of a right-handed orthonormal basis with e3 along
    ``axis``."""
    e3 = np.asarray(axis, dtype=np.float64)
    e3 = e3 / np.linalg.norm(e3)
    ref = np.array([1.0, 0.0, 0.0]) if abs(e3[0]) < 0.9 else np.array([0.0, 1.0, 0.0])
    e1 = np.cross(ref, e3)
    e1 /= np.linalg.norm(e1)
    return np.vstack([e1, np.cross(e3, e1), e3])


class Geometry:
    """Centre, centre velocity, polar axis and periods shared by the derived
    columns. The polar axis (default: the table's z) orients the polar and
    azimuthal velocity components; the radius and the radial velocity do
    not depend on it."""

    def __init__(self, pos_fields, vel_fields, centre, periods, centre_velocity=None,
                 axis=None, axial_coordinates: bool = False):
        self.pos_fields = list(pos_fields)
        self.vel_fields = list(vel_fields or [])
        self.centre = np.asarray(centre, dtype=np.float64)
        self.periods = list(periods)
        self.centre_velocity = (np.asarray(centre_velocity, dtype=np.float64)
                                if centre_velocity is not None else None)
        self.basis = _basis(axis if axis is not None else (0.0, 0.0, 1.0))
        self.axial_coordinates = axial_coordinates

    def derive(self, columns: dict) -> dict:
        """``columns`` with "radius" and, when velocities and a centre
        velocity are known, "radial_velocity" plus the private components.
        With ``axial_coordinates`` also the minimum-image height "_z" along
        the axis (signed) and the distance "_R" from it, and with velocities
        the cylindrical components "_v_R", "_v_phi" (the azimuthal one of
        the spherical set) and "_v_z"; on the axis itself v_R is taken as
        0, as the azimuthal components are."""
        delta = _separations(columns, self.pos_fields, self.centre, self.periods)
        out = dict(columns)
        out["_delta"] = delta
        b = self.basis
        moving = bool(self.vel_fields) and self.centre_velocity is not None
        rotated = None
        if moving or self.axial_coordinates:
            rotated = [b[k, 0] * delta[0] + b[k, 1] * delta[1] + b[k, 2] * delta[2]
                       for k in range(3)]
        if rotated is not None and self.axial_coordinates:
            out["_z"] = rotated[2]
            out["_R"] = np.sqrt(rotated[0] ** 2 + rotated[1] ** 2)
        if moving:
            vel = [np.asarray(columns[f], dtype=np.float64) - c
                   for f, c in zip(self.vel_fields, self.centre_velocity)]
            rotated_v = [b[k, 0] * vel[0] + b[k, 1] * vel[1] + b[k, 2] * vel[2]
                         for k in range(3)]
            d, v_r, v_t, v_p = _spherical(rotated, rotated_v)
            out.update(radius=d, radial_velocity=v_r, _v_theta=v_t, _v_phi=v_p, _vel=vel)
            if self.axial_coordinates:
                cyl = out["_R"]
                with np.errstate(invalid="ignore", divide="ignore"):
                    out["_v_R"] = np.where(
                        cyl > 0, (rotated[0] * rotated_v[0] + rotated[1] * rotated_v[1]) / cyl,
                        0.0)
                out["_v_z"] = rotated_v[2]
        else:
            out["radius"] = np.sqrt(delta[0] ** 2 + delta[1] ** 2 + delta[2] ** 2)
        return out


_KINEMATIC_BINS = 128


def centre_kinematics(passes: _Passes, geometry: Geometry, mass_field: str,
                      r_lo: float, r_max: float, velocity=None, axis=None) -> dict:
    """The centre velocity and polar axis, from the particles inside the
    smallest sphere around the centre holding CENTRE_VELOCITY_PARTICLES
    particles (radii resolved to log steps of (r_max/r_lo)^(1/128), from
    ``r_lo`` up to ``r_max``): their mass-weighted mean velocity unless
    ``velocity`` is given, and the direction of their angular momentum about
    the centre in that frame unless ``axis`` is given (the table's z when it
    vanishes). One pass. A small sphere keeps a halo's own motion from being
    diluted by the background of a large selection."""
    nb = _KINEMATIC_BINS + 1
    edges = np.concatenate([[0.0], np.geomspace(r_lo, r_max, _KINEMATIC_BINS + 1)])
    counts, masses = np.zeros(nb), np.zeros(nb)
    momenta, moments, spins = np.zeros((3, nb)), np.zeros((3, nb)), np.zeros((3, nb))
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
        inside = index < nb
        w = weight[keep][inside]
        index = index[inside]
        dl = [c[inside] for c in delta]
        v = [sub[f][inside] for f in geometry.vel_fields]
        counts += np.bincount(index, minlength=nb)
        masses += np.bincount(index, weights=w, minlength=nb)
        cross = (dl[1] * v[2] - dl[2] * v[1], dl[2] * v[0] - dl[0] * v[2],
                 dl[0] * v[1] - dl[1] * v[0])
        for k in range(3):
            momenta[k] += np.bincount(index, weights=w * v[k], minlength=nb)
            moments[k] += np.bincount(index, weights=w * dl[k], minlength=nb)
            spins[k] += np.bincount(index, weights=w * cross[k], minlength=nb)
    cum_n, cum_m = np.cumsum(counts), np.cumsum(masses)
    chosen = next((k for k in range(nb)
                   if cum_n[k] >= CENTRE_VELOCITY_PARTICLES and cum_m[k] > 0), nb - 1)
    if cum_m[chosen] <= 0:
        raise ValueError("No particle with a finite velocity inside the profile.")
    mean_v = np.cumsum(momenta, axis=1)[:, chosen] / cum_m[chosen]
    vc = np.asarray(velocity, dtype=np.float64) if velocity is not None else mean_v
    spin = (np.cumsum(spins, axis=1)[:, chosen]
            - np.cross(np.cumsum(moments, axis=1)[:, chosen], vc))
    if axis is not None:
        polar, axis_from = np.asarray(axis, dtype=np.float64), "given"
    elif np.linalg.norm(spin) > 0:
        polar, axis_from = spin, "angular momentum of the central particles"
    else:
        polar, axis_from = np.array([0.0, 0.0, 1.0]), "the table's z (no net rotation)"
    polar = polar / np.linalg.norm(polar)
    return {"velocity": [float(x) for x in vc],
            "velocity_from": "given" if velocity is not None else
            "mass-weighted mean of the central particles",
            "axis": [float(x) for x in polar], "axis_from": axis_from,
            "radius": float(edges[chosen + 1]), "particles": int(cum_n[chosen])}


def default_centre_velocity(chunks: Chunks, pos_fields, vel_fields, mass_field: str, centre,
                            periods, region_limit: float | None = None, stop=None,
                            progress=None) -> dict:
    """The centre velocity a radius-velocity diagram uses when none is
    given: as for a profile, between the smallest distance and the
    selection's extent (bounded by the selected region and half the period)."""
    passes = _Passes(chunks, stop, progress, expected=2)
    centre = np.asarray(centre, dtype=np.float64)
    largest, smallest, _ = _max_distance(passes, pos_fields, mass_field, centre, periods)
    outer = min([largest * (1.0 + 1e-12)] + ([region_limit] if region_limit else [])
                + [0.5 * p for p in periods if p])
    if not outer > 0:
        raise ValueError("Every selected particle sits at the centre.")
    inner = min(max(smallest if math.isfinite(smallest) else 0.0, outer * 1e-4), 0.5 * outer)
    return centre_kinematics(passes, Geometry(pos_fields, vel_fields, centre, periods),
                             mass_field, inner, outer)


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
                   centre, periods, centre_velocity_value=None, axis=None,
                   r_min=None, r_max=None,
                   bins: int = 32, region_limit: float | None = None,
                   geometry: str = "spherical", z_min=None, z_max=None,
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
    dispersion, which counts streaming, is given apart.

    ``geometry`` "cylindrical" bins in R, the distance from the axis through
    the centre (log-spaced from ``r_min`` to ``r_max``, as the shells are),
    over the whole selection along the axis or, with both ``z_min`` and
    ``z_max``, within that slab of signed heights. "axial" bins in the signed
    height z along the axis (linear from ``z_min`` to ``z_max``, by default
    +-the largest extent), over every distance from the axis or only R <
    ``r_max`` when it is given. The axis is ``axis``, else the angular
    momentum of the central particles (needs velocities), else the table's
    z. Distances are the minimum image about the centre, then R and z are
    taken from that vector. A bin's density is its mass over its annulus or
    slab volume; the surface density (cylindrical) and linear density
    (axial) need no extent along or across the axis. The velocity block then
    gives the cylindrical components (v_R, v_phi, v_z) about the axis."""
    if not 4 <= bins <= 256:
        raise ValueError("A profile has between 4 and 256 shells.")
    if geometry not in GEOMETRIES:
        raise ValueError(f"geometry must be one of {', '.join(GEOMETRIES)}.")
    spherical, axial = geometry == "spherical", geometry == "axial"
    if spherical and (z_min is not None or z_max is not None):
        raise ValueError("z_min and z_max need a cylindrical or axial profile.")
    if axial and r_min is not None:
        raise ValueError("An axial profile has no r_min; r_max limits the distance from "
                         "the axis.")
    if geometry == "cylindrical" and (z_min is None) != (z_max is None):
        raise ValueError("Give both z_min and z_max, or neither.")
    for name, value in (("z_min", z_min), ("z_max", z_max), ("r_min", r_min), ("r_max", r_max)):
        if value is not None and not math.isfinite(value):
            raise ValueError(f"{name} must be finite.")
    with_velocity = bool(vel_fields)
    passes = _Passes(chunks, stop, progress,
                     expected=4 + int(with_velocity and (centre_velocity_value is None
                                                         or axis is None)))
    centre = np.asarray(centre, dtype=np.float64)
    largest, smallest, _ = _max_distance(passes, pos_fields, mass_field, centre, periods)
    if largest <= 0:
        raise ValueError("Every selected particle sits at the centre.")
    limits = [largest * (1.0 + 1e-12)]
    if region_limit is not None and region_limit > 0:
        limits.append(region_limit)
    half_periods = [0.5 * p for p in periods if p]
    limits += half_periods
    reach = min(limits)
    default_min = max(smallest if math.isfinite(smallest) else 0.0, reach * 1e-3)
    complete_radius = min([math.inf if region_limit is None else region_limit] + half_periods)
    if axial:
        # r_max, when given, only cuts the particles; the bins run along z.
        z_hi = reach if z_max is None else z_max
        z_lo = -z_hi if z_min is None else z_min
        if not z_lo < z_hi:
            raise ValueError("The profile needs z_min < z_max.")
        if r_max is not None and not r_max > 0:
            raise ValueError("r_max, the distance from the axis, must be positive.")
        r_min = None
        edges = np.linspace(z_lo, z_hi, bins + 1)
    else:
        if r_max is None:
            r_max = reach
        if r_min is None:
            # The smallest 3-D distance says nothing about the smallest R of a
            # cylinder (a particle near the axis can be far along it).
            r_min = max(smallest if math.isfinite(smallest) and spherical else 0.0,
                        r_max * 1e-3)
        if not (math.isfinite(r_min) and math.isfinite(r_max) and 0 < r_min < r_max):
            raise ValueError("The profile needs 0 < r_min < r_max.")
        if z_min is not None and not z_min < z_max:
            raise ValueError("The profile needs z_min < z_max.")
        edges = np.geomspace(r_min, r_max, bins + 1)
    slab = geometry == "cylindrical" and z_min is not None

    velocity_info = None
    if with_velocity and (centre_velocity_value is None or axis is None):
        # The central sphere is a 3-D one whatever the profile's geometry.
        k_lo, k_hi = (r_min, r_max) if spherical else (default_min, reach)
        velocity_info = centre_kinematics(
            passes, Geometry(pos_fields, vel_fields, centre, periods), mass_field, k_lo, k_hi,
            velocity=centre_velocity_value, axis=axis)
        centre_velocity_value, axis = velocity_info["velocity"], velocity_info["axis"]
    if axis is None:
        axis_from = "the table's z"
    elif velocity_info is not None:
        axis_from = velocity_info["axis_from"]
    else:
        axis_from = "given"
    frame = Geometry(pos_fields, vel_fields, centre, periods,
                     centre_velocity_value if with_velocity else None, axis,
                     axial_coordinates=not spherical)
    columns = list(columns)
    if "radial_velocity" in columns and not with_velocity:
        raise ValueError("radial_velocity needs the velocity fields.")
    if not with_velocity:
        vel_keys = []
    elif spherical:
        vel_keys = ["radial_velocity", "_v_theta", "_v_phi", "_vx", "_vy", "_vz"]
    else:
        vel_keys = ["_v_R", "_v_phi", "_v_z", "_vx", "_vy", "_vz"]

    def shells_of(chunk):
        """Each usable row's weight, binning coordinate and bin: -1 below the
        first edge, ``bins`` or more above the last, EXCLUDED outside the
        profile's slab (cylindrical) or cylinder (axial)."""
        keep, weight = _usable(chunk, pos_fields, mass_field)
        sub = {k: np.asarray(v, dtype=np.float64)[keep] for k, v in chunk.items()}
        derived = frame.derive(sub)
        if with_velocity:
            derived["_vx"], derived["_vy"], derived["_vz"] = derived["_vel"]
        d = derived["radius"] if spherical else derived["_z"] if axial else derived["_R"]
        shell = np.searchsorted(edges, d, side="right") - 1
        if axial and r_max is not None:
            shell[derived["_R"] >= r_max] = EXCLUDED
        elif slab:
            shell[(derived["_z"] < z_min) | (derived["_z"] >= z_max)] = EXCLUDED
        return derived, weight[keep], d, shell

    n_inner = 0
    m_inner = 0.0
    n_outer = 0
    m_outer = 0.0
    n_excluded = 0
    m_excluded = 0.0
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
        inner = shell == -1
        n_inner += int(inner.sum())
        m_inner += float(w[inner].sum())
        outer = shell >= bins
        n_outer += int(outer.sum())
        m_outer += float(w[outer].sum())
        excluded = shell == EXCLUDED
        n_excluded += int(excluded.sum())
        m_excluded += float(w[excluded].sum())
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
        if with_velocity:
            moving = np.isfinite(derived["_vx"][ok]) & np.isfinite(derived["_vy"][ok]) \
                & np.isfinite(derived["_vz"][ok])
        for k in vel_keys:
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
    lo, hi = edges[:-1], edges[1:]
    with np.errstate(invalid="ignore", divide="ignore"):
        mean_coordinate = np.where(mass > 0, radius_sum / mass, np.nan)
        result = {"geometry": geometry}
        if axial:
            result.update({
                "z_lo": lo.tolist(), "z_hi": hi.tolist(), "z_mid": (0.5 * (lo + hi)).tolist(),
                "z_mean": _finite_list(mean_coordinate)})
        else:
            result.update({
                "r_lo": lo.tolist(), "r_hi": hi.tolist(),
                "r_mid": np.sqrt(lo * hi).tolist(), "r_mean": _finite_list(mean_coordinate)})
        # A bin is complete while the volume it covers is entirely inside the
        # region the selection samples fully (a sphere of radius
        # ``complete_radius`` about the centre): a shell by its outer radius,
        # a cylinder or slab by the farthest corner of its bin. Without a
        # slab (cylindrical) or cylinder (axial) the profile reaches as far
        # along (across) the axis as the selection does, so it is complete only
        # when nothing bounds the selection.
        if spherical:
            complete = hi <= complete_radius * (1 + 1e-12)
        elif math.isinf(complete_radius):
            complete = np.ones(bins, dtype=bool)
        elif axial:
            far = np.maximum(np.abs(lo), np.abs(hi))
            across = r_max if r_max is not None else math.inf
            complete = np.sqrt(far ** 2 + across ** 2) <= complete_radius * (1 + 1e-12)
        else:
            along = max(abs(z_min), abs(z_max)) if slab else math.inf
            complete = np.sqrt(hi ** 2 + along ** 2) <= complete_radius * (1 + 1e-12)
        result.update({
            "complete": np.asarray(complete).tolist(),
            "count": count.astype(np.int64).tolist(), "mass": mass.tolist(),
            "selected_rows": selected,
            "complete_radius": None if math.isinf(complete_radius) else float(complete_radius),
            "columns": {},
        })
        if spherical:
            volume = 4.0 / 3.0 * math.pi * (hi ** 3 - lo ** 3)
            result.update({"density": (mass / volume).tolist(),
                           "enclosed_mass": (m_inner + np.cumsum(mass)).tolist()})
        elif axial:
            result["linear_density"] = (mass / (hi - lo)).tolist()
            result["density"] = (None if r_max is None else
                                 (mass / (math.pi * r_max ** 2 * (hi - lo))).tolist())
            result["enclosed_mass"] = (m_inner + np.cumsum(mass)).tolist()
        else:
            area = math.pi * (hi ** 2 - lo ** 2)
            result["surface_density"] = (mass / area).tolist()
            result["density"] = ((mass / (area * (z_max - z_min))).tolist() if slab else None)
            result["enclosed_mass"] = (m_inner + np.cumsum(mass)).tolist()
        if axial:
            result["below_z_min"] = {"count": n_inner, "mass": m_inner}
            result["above_z_max"] = {"count": n_outer, "mass": m_outer}
            result["z_min"], result["z_max"] = float(z_lo), float(z_hi)
            result["r_limit"] = None if r_max is None else float(r_max)
            if r_max is not None:
                result["outside_r_limit"] = {"count": n_excluded, "mass": m_excluded}
        else:
            result["inside_r_min"] = {"count": n_inner, "mass": m_inner}
            result["outside_r_max"] = {"count": n_outer, "mass": m_outer}
            result["r_min"], result["r_max"] = float(r_min), float(r_max)
            if geometry == "cylindrical":
                result["z_range"] = [float(z_min), float(z_max)] if slab else None
                result["outside_z_range"] = {"count": n_excluded, "mass": m_excluded}
        if not spherical:
            result["axis"] = {"vector": [float(v) for v in frame.basis[2]], "from": axis_from,
                              "through": [float(v) for v in centre]}
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
            block = {
                "centre_velocity": [float(v) for v in frame.centre_velocity],
                "axis": [float(v) for v in frame.basis[2]],
                "centre_velocity_from": velocity_info,
                "sigma_cartesian_with_streaming": _finite_list(
                    np.sqrt((var["_vx"] + var["_vy"] + var["_vz"]) / 3.0)),
            }
            if spherical:
                sigma_t2 = var["_v_theta"] + var["_v_phi"]
                block.update({
                    "mean_radial": _finite_list(vel_mean["radial_velocity"]),
                    "mean_azimuthal": _finite_list(vel_mean["_v_phi"]),
                    "sigma_r": _finite_list(np.sqrt(var["radial_velocity"])),
                    "sigma_theta": _finite_list(np.sqrt(var["_v_theta"])),
                    "sigma_phi": _finite_list(np.sqrt(var["_v_phi"])),
                    "sigma_1d": _finite_list(
                        np.sqrt((var["radial_velocity"] + sigma_t2) / 3.0)),
                    "beta": _finite_list(1.0 - sigma_t2 / (2.0 * var["radial_velocity"]))})
            else:
                block.update({
                    "mean_cylindrical_radial": _finite_list(vel_mean["_v_R"]),
                    "mean_azimuthal": _finite_list(vel_mean["_v_phi"]),
                    "mean_vertical": _finite_list(vel_mean["_v_z"]),
                    "sigma_R": _finite_list(np.sqrt(var["_v_R"])),
                    "sigma_phi": _finite_list(np.sqrt(var["_v_phi"])),
                    "sigma_z": _finite_list(np.sqrt(var["_v_z"])),
                    "sigma_1d": _finite_list(np.sqrt(
                        (var["_v_R"] + var["_v_phi"] + var["_v_z"]) / 3.0))})
            result["velocity"] = block
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
