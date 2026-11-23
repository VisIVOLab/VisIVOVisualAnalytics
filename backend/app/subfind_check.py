"""Which velocity convention a SubFind catalogue uses, measured on the data
(issue #448).

Gadget-4 writes a snapshot sorted by group beside its group catalogue, and
the catalogue's ``SubhaloOffsetType``/``SubhaloLenType`` give each subhalo's
particles as one run of rows per type. A subhalo's velocity is its members'
mass-weighted mean (subfind_properties.cc), so for the most massive
subhalos the ratio of that mean (peculiar: the snapshot's velocities x
sqrt(a), fixed by Gadget-4's snap_io.cc) to the stored ``SubhaloVel`` says
how the catalogue stores velocities: 1 when peculiar, sqrt(a) when with the
particles' convention, 1/a when the internal a^2 dx/dt. Both come from the
same particles at the same time, so the ratio is exact to the precision of
the files. A binary P-Gadget3 catalogue indexes its own ID list instead of
the snapshot, an Arepo one keeps its offsets in separate files: neither can
be checked this way.
"""
from __future__ import annotations

import math
import re
from pathlib import Path

import numpy as np

from . import gadget_snapshot as gs

#: Subhalos checked when the caller does not say, and at most.
DEFAULT_SUBHALOS = 10
MAX_SUBHALOS = 50
#: Rows of one type of one subhalo read at most: a larger one is read every
#: k-th row (the mass-weighted mean of an even sample).
MAX_MEMBER_ROWS = 4_000_000
#: Rows read at a time.
CHUNK_ROWS = 1_000_000
#: A convention is named when the median ratio is within this factor of
#: its value, no other is within twice that, and at least AGREEING of the
#: subhalos' own ratios are within it too (a mis-paired snapshot scatters).
TOLERANCE = 1.02
AGREEING = 0.8

_NUMBER = re.compile(r"_(\d{3,})(\.\d+)?\.(hdf5|h5)$")


def snapshot_for_catalogue(catalogue: gs.Snapshot, allowed=None) -> Path | None:
    """The particle snapshot written with a Gadget-4 catalogue: same output
    number, in the catalogue's folder or the one above it (``snapdir_NNN``,
    ``snapshot_NNN.hdf5``, ``snap_NNN.0.hdf5`` ...), with every file it reads
    passing ``allowed`` (path -> bool; default: within VISIVO_DATA_ROOTS).
    The most-bound-only companion of an orphan-tracking run
    (``...-prevmostboundonly_NNN``) is not the snapshot."""
    if allowed is None:
        def allowed(p):
            return gs._within_env_roots(Path(p))
    first = Path(catalogue.path)
    m = _NUMBER.search(first.name)
    if m is None:
        return None
    number = m.group(1)
    name = re.compile(rf"_{number}(\.0)?\.(hdf5|h5)$")
    for folder in (first.parent, first.parent.parent):
        if not allowed(str(folder)):
            continue
        try:
            entries = sorted(folder.iterdir())
        except OSError:
            continue
        for entry in entries:
            if not allowed(str(entry)):
                continue
            if entry.is_dir() and entry.name == f"snapdir_{number}":
                candidate = gs.first_in_dir(entry, allowed)
            elif (entry.is_file() and name.search(entry.name)
                  and not gs.is_companion_name(entry.name)):
                candidate = entry
            else:
                continue
            if (candidate is not None and gs.is_gadget(candidate)
                    and not gs.is_group_catalogue(candidate)
                    and _all_allowed(candidate, allowed)):
                return candidate
    return None


def _all_allowed(first: Path, allowed) -> bool:
    """Whether every file the snapshot reads (its files, its parameter
    file) passes ``allowed``, as for a snapshot the user opens."""
    try:
        files = gs.snapshot_files(first)
    except (OSError, ValueError):
        return False
    params = gs.parameter_file(files[0])
    return all(allowed(str(f)) for f in [*files, *([params] if params else [])])


def _member_mean(snapshot: gs.Snapshot, offsets, lengths) -> tuple[float, np.ndarray, bool]:
    """Total mass, mass-weighted peculiar velocity (km/s) of one subhalo's
    particles, and whether they were sampled."""
    factor = snapshot.units.velocity_cm_s / 1e5 * math.sqrt(snapshot.scale_factor)
    mass, momentum, sampled = 0.0, np.zeros(3), False
    for t in range(gs.NTYPES):
        start, count = int(offsets[t]), int(lengths[t])
        if count <= 0:
            continue
        comp = snapshot.component(t)
        if start < 0 or start + count > comp.num_rows:
            raise ValueError(f"subhalo members of type {t} (rows {start}..{start + count}) "
                             f"lie outside the snapshot's {comp.num_rows}: not the snapshot "
                             "this catalogue was written with, or not sorted by group")
        missing = [c for c in ("mass", "vx", "vy", "vz") if c not in comp.names]
        if missing:
            raise ValueError(f"the snapshot's type {t} has no " + ", ".join(missing))
        step = max(1, math.ceil(count / MAX_MEMBER_ROWS))
        sampled |= step > 1
        rows = np.arange(start, start + count, step, dtype=np.int64)
        for at in range(0, rows.size, CHUNK_ROWS):
            chunk = rows[at:at + CHUNK_ROWS]
            m = np.asarray(comp.take("mass", chunk), dtype=np.float64)
            v = np.stack([np.asarray(comp.take(c, chunk), dtype=np.float64)
                          for c in ("vx", "vy", "vz")], axis=1) * factor
            mass += float(m.sum())
            momentum += (m[:, None] * v).sum(axis=0)
    return mass, (momentum / mass if mass > 0 else momentum), sampled


def expected_ratios(a: float) -> dict[str, float]:
    """The ratio each stored convention gives (svel= names)."""
    return {name: a ** power for name, (power, _) in gs.SUBFIND_VEL_FACTORS.items()
            if name != "comoving"}


def _verdict(median: float, ratios: list[float], a: float) -> tuple[str | None, str]:
    """The convention the ratios imply, and why (None: undecided)."""
    if not (median > 0 and math.isfinite(median)):
        return None, "the median ratio is not positive"
    tol = math.log(TOLERANCE)
    distance = sorted((abs(math.log(median / value)), name)
                      for name, value in expected_ratios(a).items())
    (best, name), (second, _) = distance[0], distance[1]
    if best > tol:
        return None, (f"the median ratio {median:.4g} matches no convention within "
                      f"{TOLERANCE - 1:.0%}: not the snapshot written with this catalogue?")
    if second < 2 * tol:
        return None, (f"the median ratio {median:.4g} cannot tell the conventions apart "
                      f"at a = {a:.4g}")
    value = expected_ratios(a)[name]
    agreeing = sum(1 for r in ratios if r > 0 and abs(math.log(r / value)) <= tol)
    if agreeing < AGREEING * len(ratios):
        return None, (f"only {agreeing} of {len(ratios)} subhalos agree with {name!r}: "
                      "the members do not match the stored velocities")
    return name, (f"the median ratio {median:.4g} is {name!r}'s {value:.4g}, "
                  f"for {agreeing} of {len(ratios)} subhalos")


def subfind_velocity_check(catalogue_path: str | Path, snapshot_path: str | Path | None = None,
                           subhalos: int = DEFAULT_SUBHALOS, allowed=None) -> dict:
    """Compare ``SubhaloVel`` with the mass-weighted velocity of the members
    of the ``subhalos`` most massive subhalos (Gadget-4 HDF5 only). Returns
    the per-subhalo and median ratios (members / stored, both in km/s), their
    spread, the convention they imply and the ``svel=`` setting in use.
    ``allowed`` decides which snapshot files may be read when the snapshot
    is looked for (:func:`snapshot_for_catalogue`)."""
    catalogue = gs.open_snapshot(gs.split_ref(catalogue_path)[0])
    if catalogue.producer != "gadget4":
        raise ValueError("the velocity check needs a Gadget-4 HDF5 group catalogue, whose "
                         "snapshot is sorted by group; a binary catalogue indexes its own "
                         "ID list, an Arepo one keeps its offsets in separate files")
    if catalogue.lightcone:
        raise ValueError("a lightcone group catalogue has no single snapshot to check against")
    subs = catalogue.component(1)
    needed = [f"subhalooffsettype_{t}" for t in range(gs.NTYPES)] + \
        [f"subhalolentype_{t}" for t in range(gs.NTYPES)] + ["subhalo_mass", "vx"]
    missing = [c for c in needed if c not in subs.names]
    if missing:
        raise ValueError("the catalogue lacks " + ", ".join(missing[:4]))
    if snapshot_path is None:
        snapshot_path = snapshot_for_catalogue(catalogue, allowed)
        if snapshot_path is None:
            raise ValueError("no particle snapshot with this catalogue's output number beside "
                             "it (or one folder up) that this backend may read")
    snapshot = gs.open_snapshot(snapshot_path)
    a_cat, a_snap = catalogue.header.time, snapshot.header.time
    if not math.isclose(a_cat, a_snap, rel_tol=1e-5):
        raise ValueError(f"the snapshot is at time {a_snap:.6g}, the catalogue at {a_cat:.6g}")
    n = max(1, min(int(subhalos), MAX_SUBHALOS))
    masses = np.asarray(subs.column("subhalo_mass"), dtype=np.float64)
    order = np.argsort(-masses, kind="stable")[:n]
    offsets = np.stack([np.asarray(subs.take(f"subhalooffsettype_{t}", order))
                        for t in range(gs.NTYPES)], axis=1)
    lengths = np.stack([np.asarray(subs.take(f"subhalolentype_{t}", order))
                        for t in range(gs.NTYPES)], axis=1)
    kms = catalogue.units.velocity_cm_s / 1e5
    stored = np.stack([np.asarray(subs.take(c, order), dtype=np.float64)
                       for c in ("vx", "vy", "vz")], axis=1) * kms
    rows, ratios, sampled_any = [], [], False
    for k, index in enumerate(order):
        mass, members, sampled = _member_mean(snapshot, offsets[k], lengths[k])
        sampled_any |= sampled
        s = stored[k]
        norm = float(s @ s)
        ratio = float(members @ s) / norm if norm > 0 and mass > 0 else None
        if ratio is not None:
            ratios.append(ratio)
        rows.append({"subhalo": int(index), "members_km_s": [float(x) for x in members],
                     "stored_km_s": [float(x) for x in s], "ratio": ratio,
                     "sampled": sampled})
    a = catalogue.scale_factor
    median = float(np.median(ratios)) if ratios else None
    spread = float(np.median(np.abs(np.asarray(ratios) - median))) if ratios else None
    convention, why = (_verdict(median, ratios, a) if median is not None
                       else (None, "no subhalo with members and a velocity"))
    return {"catalogue": Path(catalogue.path).name, "snapshot": Path(snapshot.path).name,
            "scale_factor": a, "subhalos": rows, "median_ratio": median,
            "ratio_mad": spread, "convention": convention, "reason": why,
            "expected": expected_ratios(a), "svel_in_use": catalogue.units.subfind_vel,
            "sampled": sampled_any,
            "note": "ratio = (members' mass-weighted peculiar velocity) . v / |v|^2, v the "
                    "stored SubhaloVel in km/s: 1 for peculiar, sqrt(a) for 'snapshot', "
                    "1/a for 'canonical' (svel= in VISIVO_GADGET_UNITS)"}
