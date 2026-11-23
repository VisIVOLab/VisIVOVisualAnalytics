"""Radial profiles and phase diagrams of the selected rows of a particle
table (issues #426, #414), as the exact selection diagnostics are computed
(routers/catalogue.py): the same filters, the same out-of-core reading of a
Gadget snapshot or chunked reading of a loaded table, and the same task
protocol (``GET /v1/tasks/{id}`` for progress and result, ``DELETE`` to stop
between two chunks). The science is in app/particle_profiles.py."""
from __future__ import annotations

import asyncio
import math
import threading

import numpy as np
from fastapi import APIRouter, Depends, HTTPException

from .. import particle_profiles as pp
from .. import particle_totals as pt
from ..auth import verify_token
from ..dependencies import (
    _apply_tabular_query, _require_dataset, acquire_task_slot, task_slot_busy_error,
)
from ..periodic import box_inside, separation
from ..schemas import (
    ParticleProfileRequest, ParticleProfileResponse, ParticleTotalsRequest,
    ParticleTotalsResponse, PhaseDiagramRequest, PhaseDiagramResponse, ProfileCentre,
    TaskCreateResponse,
)
from ..sessions import Session, get_session
from ..tasks import TASKS
from . import catalogue as _catalogue
from .catalogue import (
    DiagnosticsCancelled, _diagnostic_selection,
    _entry_periods, _filter_fields, _particle_matcher,
)

router = APIRouter(tags=["catalogue"])
_auth = Depends(verify_token)

_NOT_DECLARED = "unit not declared"


def _field_names(entry) -> set[str]:
    return {str(field.get("name", "")) for field in entry.get("fields", ())}


def _require_catalogue(entry) -> None:
    if entry["kind"] != "catalogue":
        raise HTTPException(422, "This endpoint requires a catalogue dataset.")


def _selection_chunks(entry, filters, columns):
    """A chunk factory over the selected rows of ``entry`` (see
    app/particle_profiles.py): the columns read as float64, an out-of-core
    particle type through its matcher, a loaded table a million rows at a
    time with the filters applied to each slice."""
    from ..point_table import is_particle_format  # noqa: PLC0415
    columns = list(dict.fromkeys(columns))
    if is_particle_format(entry.get("format")) and entry.get("dataframe") is None:
        from .. import gadget_snapshot as gs  # noqa: PLC0415
        component = gs.resolve(entry["path"])
        matching = _particle_matcher(component, filters)
        n = max(1, component.num_rows)

        def particle_chunks():
            for start in range(0, component.num_rows, _catalogue.PARTICLE_CHUNK_ROWS):
                stop = min(component.num_rows, start + _catalogue.PARTICLE_CHUNK_ROWS)
                rows = matching(start, stop)
                yield ({c: np.asarray(component.take(c, rows), dtype=np.float64)
                        if rows.size else np.empty(0) for c in columns}, stop / n)
        return particle_chunks
    frame = entry["dataframe"]
    needed = [name for name in frame.columns
              if name in _filter_fields(filters) or name in columns]
    periods = _entry_periods(entry)
    n = max(1, len(frame))

    def frame_chunks():
        for start in range(0, len(frame), _catalogue.DIAGNOSTIC_CHUNK_ROWS):
            stop = min(len(frame), start + _catalogue.DIAGNOSTIC_CHUNK_ROWS)
            part, _ = _apply_tabular_query(frame.iloc[start:stop][needed], filters,
                                           None, 0, 0, periods=periods)
            yield ({c: part[c].to_numpy(dtype=np.float64, na_value=np.nan) for c in columns},
                   stop / n)
    return frame_chunks


def _check_numeric(entry, names) -> None:
    """Unknown columns are refused; so are text columns of a loaded table."""
    fields = _field_names(entry)
    frame = entry.get("dataframe")
    for name in names:
        if name not in fields:
            raise ValueError(f"Unknown field: {name!r}")
        if frame is not None and name in frame.columns and frame[name].dtype.kind not in "biuf":
            raise ValueError(f"Field {name!r} is not numeric.")


def _resolve_fields(entry, request) -> tuple[list[str], list[str], str]:
    """(position, velocity, mass) fields: the defaults are x/y/z, vx/vy/vz
    when present, and "mass" when present ("" = unit weights)."""
    fields = _field_names(entry)
    pos = list(request.position_fields)
    if len(pos) != 3:
        raise ValueError("Three position fields are needed.")
    if request.velocity_fields is None:
        vel = ["vx", "vy", "vz"] if {"vx", "vy", "vz"} <= fields else []
    else:
        vel = list(request.velocity_fields)
        if vel and len(vel) != 3:
            raise ValueError("Give three velocity fields, or none.")
    mass = ("mass" if "mass" in fields else "") if request.mass_field is None \
        else request.mass_field
    _check_numeric(entry, pos + vel + ([mass] if mass else []))
    return pos, vel, mass


def _periods(entry, request, pos) -> list[float | None]:
    if request.period is not None:
        if not math.isfinite(request.period) or request.period < 0:
            raise ValueError("The period must be a finite length, or 0 for none.")
        return [float(request.period) or None] * 3
    own = _entry_periods(entry)
    return [own.get(f) for f in pos]


def _region_limit(filters, pos, centre, periods) -> float | None:
    """Distance from ``centre`` to the nearest boundary of the region the
    selection keeps on the position fields: shells beyond it are only partly
    sampled. Inside boxes and spheres bound it from within, comparisons on a
    position field (x > 10, ...) are half-spaces, and outside boxes and
    spheres are holes whose nearest point bounds it. None without such a
    boundary; 0 when the centre itself is not kept. A comparison compares raw
    values, as the filter does, so on a periodic axis the kept slab is
    bounded by the compared value and by a face of the box."""
    axis_of = {f: k for k, f in enumerate(pos)}
    limits = []
    for item in filters:
        spatial = item.box if item.box is not None else item.sphere
        if spatial is None:
            k = axis_of.get(item.field)
            if k is None or item.op not in ("<", "<=", ">", ">="):
                continue
            try:
                value = float(item.value)
            except ValueError:
                continue
            period = periods[k]
            c = float(centre[k]) % period if period else float(centre[k])
            below = item.op.startswith("<")
            kept = {"<": c < value, "<=": c <= value, ">": c > value, ">=": c >= value}[item.op]
            if not kept:
                limits.append(0.0)
            elif period:
                limits.append(min(c, value - c) if below else min(c - value, period - c))
            else:
                limits.append(abs(c - value))
            continue
        if list(spatial.fields) != list(pos) or item.op not in ("inside", "outside"):
            continue
        if item.sphere is not None:
            d = math.sqrt(sum(float(separation(c, s, p)) ** 2 for c, s, p
                              in zip(centre, item.sphere.center, periods)))
            radius = float(item.sphere.radius)
            limits.append(max(0.0, radius - d) if item.op == "inside" else max(0.0, d - radius))
            continue
        gaps, inside_all = [], True
        for k, (lo, hi) in enumerate(zip(item.box.min, item.box.max)):
            p = periods[k]
            to_faces = min(abs(float(separation(centre[k], lo, p))),
                           abs(float(separation(hi, centre[k], p))))
            if bool(box_inside(np.array([centre[k]]), lo, hi, p)[0]):
                gaps.append((True, to_faces))
            else:
                inside_all = False
                gaps.append((False, to_faces))
        if item.op == "inside":
            limits.append(min(g for _, g in gaps) if inside_all else 0.0)
        else:
            limits.append(0.0 if inside_all else
                          math.sqrt(sum(g * g for inside, g in gaps if not inside)))
    return min(limits) if limits else None


def _target_position(session: Session, centre: ProfileCentre, pos) -> list[float]:
    """The position columns of one row of another open catalogue."""
    entry = _require_dataset(session, centre.target_dataset_id)
    _require_catalogue(entry)
    row = centre.target_row
    if row is None or row < 0:
        raise ValueError("A target centre needs target_row.")
    _check_numeric(entry, pos)
    frame = entry.get("dataframe")
    if frame is not None:
        if row >= len(frame):
            raise ValueError(f"The target catalogue has {len(frame)} rows.")
        values = [float(frame[f].iloc[row]) for f in pos]
    else:
        from .. import gadget_snapshot as gs  # noqa: PLC0415
        component = gs.resolve(entry["path"])
        if row >= component.num_rows:
            raise ValueError(f"The target catalogue has {component.num_rows} rows.")
        values = [float(np.asarray(component.take(f, np.array([row])))[0]) for f in pos]
    if not all(math.isfinite(v) for v in values):
        raise ValueError("The target row has no finite position.")
    return values


def _three_finite(values) -> bool:
    return len(values) == 3 and all(math.isfinite(v) for v in values)


def _check_centre_velocity(centre: ProfileCentre | None, vel) -> None:
    """Refuse a bad centre velocity before any pass is read."""
    if centre is not None and centre.velocity is not None:
        if not _three_finite(centre.velocity):
            raise ValueError("A centre velocity needs three finite values.")
        if not vel:
            raise ValueError("A centre velocity needs the velocity fields.")


def _centre(session, entry, filters, centre: ProfileCentre, pos, mass, periods,
            stop, progress) -> dict:
    """The centre the request asks for, with what it was found from."""
    if centre.mode == "point":
        if centre.point is None or not _three_finite(centre.point):
            raise ValueError("A point centre needs three finite coordinates.")
        return {"mode": "point", "centre": [float(v) for v in centre.point]}
    if centre.mode == "target":
        found = _target_position(session, centre, pos)
        return {"mode": "target", "centre": found, "target_dataset_id": centre.target_dataset_id,
                "target_row": int(centre.target_row),
                "note": "Target position read from the catalogue's position columns, taken "
                        "to be in the particles' coordinate units."}
    if centre.start is not None and not _three_finite(centre.start):
        raise ValueError("The shrinking-sphere start needs three finite coordinates.")
    found = pp.shrinking_sphere_centre(
        _selection_chunks(entry, filters, pos + ([mass] if mass else [])), pos, mass, periods,
        start=centre.start, initial_radius=centre.initial_radius,
        shrink=centre.shrink_factor, min_particles=centre.min_particles,
        stop=stop, progress=progress)
    return {"mode": "shrinking_sphere", **found,
            "weighting": f"mass-weighted by {mass}" if mass else "unweighted (unit masses)",
            "method": "Power et al. (2003) nested shrinking spheres; periodic centre of mass "
                      "of Bai & Breen (2008) as the default start"}


def _units(entry, pos, vel, mass, columns) -> tuple[dict, dict]:
    """Units of the result as the table states them (a Gadget snapshot's
    code units), and the factors to physical units the reader declares."""
    component = entry.get("component") or {}
    declared = component.get("units") or {}
    physical = component.get("physical") or {}

    def unit(name):
        return str(declared.get(name, _NOT_DECLARED)) if name else ""

    length, mass_unit = unit(pos[0]), (unit(mass) if mass else "particle")
    units = {"radius": length, "height": length, "mass": mass_unit,
             "density": f"{mass_unit} / ({length})^3",
             "surface_density": f"{mass_unit} / ({length})^2",
             "linear_density": f"{mass_unit} / {length}",
             **({"velocity": unit(vel[0])} if vel else {}),
             "columns": {c: (unit(vel[0]) if c == "radial_velocity" else
                             length if c == "radius" else unit(c)) for c in columns}}
    phys = {}
    if pos[0] in physical:
        phys["radius"] = phys["height"] = physical[pos[0]]
        if mass and mass in physical:
            phys["mass"] = physical[mass]
            for name, power in (("density", 3), ("surface_density", 2), ("linear_density", 1)):
                phys[name] = {
                    "factor": physical[mass]["factor"] / physical[pos[0]]["factor"] ** power,
                    "unit": f"{physical[mass]['unit']}/{physical[pos[0]]['unit']}"
                            + (f"^{power}" if power > 1 else "")}
    if vel and vel[0] in physical:
        phys["velocity"] = physical[vel[0]]
    for c in columns:
        source = vel[0] if c == "radial_velocity" and vel else pos[0] if c == "radius" else c
        if source in physical:
            phys.setdefault("columns", {})[c] = physical[source]
    return units, phys


def _provenance(entry, filters, pos, vel, mass, periods) -> dict:
    snapshot = entry.get("snapshot") or {}
    return {
        "dataset": entry.get("path", ""),
        "selection": _diagnostic_selection(filters),
        "position_fields": pos, "velocity_fields": vel,
        "weights": f"mass-weighted by {mass}" if mass else "unweighted (every particle 1)",
        "frame": ("the table's own coordinates"
                  + ("; minimum-image distances with periods "
                     + str({f: p for f, p in zip(pos, periods) if p}) if any(periods) else
                     "; no periodic wrapping")),
        "code_units_note": ("Computed in the table's stored units; a cosmological Gadget "
                            "snapshot stores comoving lengths (length/h in Hubble units), "
                            "masses (mass/h), densities in those units ((mass/h)/(length/h)^3, "
                            "so h^2 overall) and velocities as v/sqrt(a). 'physical' "
                            "gives the reader's factors to physical units."),
        **({"snapshot": {k: snapshot[k] for k in ("redshift", "time", "hubble_param",
                                                   "box_size") if k in snapshot}}
           if snapshot else {}),
    }


def _stages(progress, lo, hi):
    if progress is None:
        return None
    return lambda fraction: progress(lo + (hi - lo) * float(fraction))


def _monotonic(progress):
    """``progress`` that never goes back: the stages of a run report from
    passes whose count is only known as they go."""
    if progress is None:
        return None
    best = [0.0]

    def report(fraction):
        best[0] = max(best[0], float(fraction))
        progress(best[0])
    return report


def _shape_definitions(geometry: str, profile: dict) -> dict:
    complete = ("bin inside the region the selection samples fully (the selected region and "
                "half the period, as a sphere about the centre)")
    if geometry == "spherical":
        return {
            "density": "shell mass / (4/3 pi (r_hi^3 - r_lo^3))",
            "enclosed_mass": "mass of every particle closer than r_hi (r_min included)",
            "complete": "shell inside the selected region and within half the period"}
    if geometry == "cylindrical":
        return {
            "surface_density": "annulus mass / (pi (r_hi^2 - r_lo^2)): the selection "
                               "projected along the axis, over the heights it spans",
            "density": ("annulus mass / (pi (r_hi^2 - r_lo^2) (z_max - z_min)) within the "
                        "given slab" if profile["z_range"] else
                        "null: give z_min and z_max to bound the heights"),
            "enclosed_mass": "mass of every particle with R < r_hi (R < r_min included), "
                             "inside the slab when one is given",
            "outside_z_range": "particles outside the slab, in no bin",
            "complete": complete + "; the farthest corner of the annulus's cylinder must be "
                        "inside it, and without a slab only when nothing bounds the "
                        "selection"}
    return {
        "linear_density": "slab mass / (z_hi - z_lo)",
        "density": ("slab mass / (pi r_max^2 (z_hi - z_lo)) for the particles with R < r_max"
                    if profile["r_limit"] else
                    "null: give r_max to bound the distance from the axis"),
        "enclosed_mass": "mass below z_hi, from the particles below z_min up (within R < "
                         "r_max when given)",
        "complete": complete + "; the farthest corner of the slab's cylinder must be inside "
                    "it, and without r_max only when nothing bounds the selection"}


def _run_profile(session, entry, request: ParticleProfileRequest,
                 progress=None, stop=None) -> ParticleProfileResponse:
    _require_catalogue(entry)
    pos, vel, mass = _resolve_fields(entry, request)
    columns = list(dict.fromkeys(request.columns))
    _check_numeric(entry, [c for c in columns if c not in pp.DERIVED_COLUMNS])
    if "radius" in columns:
        raise ValueError("radius is the profile's axis, not a profiled column.")
    if "radial_velocity" in columns and not vel:
        raise ValueError("radial_velocity needs the velocity fields.")
    _check_centre_velocity(request.centre, vel)
    if request.axis is not None and (not _three_finite(request.axis)
                                     or not any(request.axis)):
        raise ValueError("The polar axis needs three finite values, not all zero.")
    if request.axis is not None and not vel and request.geometry == "spherical":
        raise ValueError("A polar axis needs the velocity fields.")
    periods = _periods(entry, request, pos)
    searching = request.centre.mode == "shrinking_sphere"
    centre = _centre(session, entry, request.filters, request.centre, pos, mass, periods,
                     stop, _stages(progress, 0.0, 0.4 if searching else 0.0))
    read = pos + vel + ([mass] if mass else []) + [c for c in columns
                                                   if c not in pp.DERIVED_COLUMNS]
    profile = pp.radial_profile(
        _selection_chunks(entry, request.filters, read), pos_fields=pos, mass_field=mass,
        vel_fields=vel, columns=columns, centre=centre["centre"], periods=periods,
        centre_velocity_value=request.centre.velocity, axis=request.axis,
        r_min=request.r_min, r_max=request.r_max, bins=request.bins,
        geometry=request.geometry, z_min=request.z_min, z_max=request.z_max,
        region_limit=_region_limit(request.filters, pos, centre["centre"], periods),
        stop=stop, progress=_stages(progress, 0.4 if searching else 0.0, 1.0))
    units, physical = _units(entry, pos, vel, mass, columns)
    provenance = _provenance(entry, request.filters, pos, vel, mass, periods)
    if request.geometry == "axial":
        cut = ("" if request.r_max is None else
               f", particles within R < {request.r_max:.6g} of the axis")
        provenance["binning"] = (f"{request.bins} linear slabs [z_lo, z_hi) in the signed "
                                 f"height along the axis, from {profile['z_min']:.6g} to "
                                 f"{profile['z_max']:.6g}{cut}")
    elif request.geometry == "cylindrical":
        slab = ("" if profile["z_range"] is None else
                f", within heights {profile['z_range'][0]:.6g} .. {profile['z_range'][1]:.6g}")
        provenance["binning"] = (f"{request.bins} log-spaced annuli [r_lo, r_hi) in the "
                                 f"distance R from the axis, from {profile['r_min']:.6g} to "
                                 f"{profile['r_max']:.6g}{slab}")
    else:
        provenance["binning"] = (f"{request.bins} log-spaced shells [r_lo, r_hi) from "
                                 f"{profile['r_min']:.6g} to {profile['r_max']:.6g}")
    if request.geometry != "spherical":
        provenance["axis"] = {
            **profile["axis"],
            "frame": "the table's coordinate frame, through the centre; R and z are taken "
                     "from the minimum-image vector from the centre (z signed along the "
                     "axis vector, R the distance from the line)",
            "default": "the angular momentum of the particles of the smallest sphere about "
                       "the centre holding 1000 particles, in the centre-velocity frame "
                       "(needs velocities); the table's z without them or without net "
                       "rotation"}
    provenance["definitions"] = {
        **_shape_definitions(request.geometry, profile),
        "columns": "mass-weighted mean and standard deviation; 16/50/84 % mass-weighted "
                   "quantiles from a 1024-bin histogram per shell refined once in the "
                   "bin of the quantile (log10 axis when the shell's values are positive "
                   "and span over a factor 100)",
        "velocity": "peculiar velocities (no Hubble flow H(a) a r added) relative to the "
                    "centre velocity, the mass-weighted mean velocity of the smallest "
                    "sphere about the centre holding 1000 particles unless given; "
                    "spherical components about the centre with the polar axis "
                    "velocity.axis (by default the angular momentum of those particles); "
                    + ("sigma_1d = sqrt((sigma_R^2 + sigma_phi^2 + sigma_z^2)/3) of the "
                       "cylindrical components v_R, v_phi, v_z about the axis, each about "
                       "its bin mean; "
                       if request.geometry != "spherical" else
                       "sigma_1d = sqrt((sigma_r^2 + sigma_theta^2 + sigma_phi^2)/3), "
                       "each about its shell mean; beta = 1 - (sigma_theta^2 + sigma_phi^2) / "
                       "(2 sigma_r^2); ")
                    + "sigma_cartesian_with_streaming includes infall and rotation",
    }
    return ParticleProfileResponse(valid=True, error="", profile=profile, centre=centre,
                                   units={**units, "physical": physical},
                                   provenance=provenance)


def _resolve_totals_fields(entry, request) -> tuple[list[str], str, str, dict]:
    """(velocity, mass, energy) fields of a totals request and, for each one
    the table lacks by default or by request, the reason it is missing. A
    field the request names that the table does not have is refused."""
    fields = _field_names(entry)
    missing: dict[str, str] = {}
    if request.velocity_fields is None:
        vel = ["vx", "vy", "vz"] if {"vx", "vy", "vz"} <= fields else []
        if not vel:
            missing["velocity"] = "The dataset has no velocity fields (vx, vy, vz)."
    else:
        vel = list(request.velocity_fields)
        if vel and len(vel) != 3:
            raise ValueError("Give three velocity fields, or none.")
        if not vel:
            missing["velocity"] = "No velocity fields were requested."
    if request.mass_field is None:
        mass = "mass" if "mass" in fields else ""
        if not mass:
            missing["mass"] = ("The dataset has no mass field, so no mass-weighted total "
                               "is defined.")
    else:
        mass = request.mass_field
        if not mass:
            missing["mass"] = ("No mass field was requested, so no mass-weighted total "
                               "is defined.")
    if request.energy_field is None:
        energy = "u" if "u" in fields else ""
        if not energy:
            missing["energy"] = ("The dataset has no internal-energy field ('u'); only gas "
                                 "particles carry one.")
    else:
        energy = request.energy_field
        if not energy:
            missing["energy"] = "No internal-energy field was requested."
    _check_numeric(entry, vel + ([mass] if mass else []) + ([energy] if energy else []))
    return vel, mass, energy, missing


def _run_totals(entry, request: ParticleTotalsRequest, progress=None,
                stop=None) -> ParticleTotalsResponse:
    _require_catalogue(entry)
    vel, mass, energy, missing = _resolve_totals_fields(entry, request)
    read = vel + ([mass] if mass else []) + ([energy] if energy else [])
    if not read:
        counting = [f for f in request.position_fields if f in _field_names(entry)]
        if not counting:
            raise ValueError("The dataset has none of the position fields to count rows with.")
        read = counting[:1]
        _check_numeric(entry, read)
    component = entry.get("component") or {}
    totals = pt.selection_totals(
        _selection_chunks(entry, request.filters, read), mass_field=mass, vel_fields=vel,
        energy_field=energy,
        stored_units={k: str(v) for k, v in (component.get("units") or {}).items()},
        physical=component.get("physical") or {}, missing=missing, stop=stop,
        progress=progress)
    snapshot = entry.get("snapshot") or {}
    provenance = {
        "dataset": entry.get("path", ""),
        "selection": _diagnostic_selection(request.filters),
        "fields": {"mass": mass or None, "velocity": vel, "internal_energy": energy or None},
        "definitions": {
            "count": "rows the selection keeps",
            "mass": "sum of the finite, non-negative masses of those rows",
            "centre_of_mass_velocity": "mass-weighted mean velocity of the rows with finite "
                                       "velocities (mass_with_velocity is their mass)",
            "momentum": "sum of m v over those rows; momentum_magnitude is its norm",
            "kinetic_energy": "0.5 sum m |v|^2 in the frame the velocities are given in "
                              "(peculiar velocities of the simulation frame)",
            "kinetic_energy_com_frame": "0.5 sum m |v - v_com|^2, summed as per-chunk second "
                                        "moments merged pairwise (no sum m v^2 - M v_com^2 "
                                        "cancellation); equals kinetic_energy - 0.5 M "
                                        "|v_com|^2",
            "velocity_dispersion_1d": "sqrt(2 kinetic_energy_com_frame / (3 M)) of the rows "
                                      "with velocities",
            "thermal_energy": "sum m u over the rows with finite u >= 0 (u: specific "
                              "internal energy)",
            "units": "value is physical, in unit, from the reader's declared factors; null "
                     "with a reason when a field or its unit is missing or undeclared; "
                     "stored is the value in the table's own units",
            "velocity": "peculiar velocities (no Hubble flow); a cosmological Gadget "
                        "snapshot stores v/sqrt(a), which the factor converts",
        },
        **({"snapshot": {k: snapshot[k] for k in ("redshift", "time", "hubble_param",
                                                   "box_size") if k in snapshot}}
           if snapshot else {}),
    }
    return ParticleTotalsResponse(valid=True, error="", totals=totals, provenance=provenance)


def _run_phase(session, entry, request: PhaseDiagramRequest,
               progress=None, stop=None) -> PhaseDiagramResponse:
    _require_catalogue(entry)
    axes = (request.x, request.y)
    table = _field_names(entry)
    derived = [a.field for a in axes if a.field in pp.DERIVED_COLUMNS
               and (request.centre is not None or a.field not in table)]
    plain = [a.field for a in axes if a.field not in derived]
    _check_numeric(entry, plain + ([request.weight_field] if request.weight_field else []))
    for axis in axes:
        if (axis.min is None) != (axis.max is None):
            raise ValueError(f"Give both ends of the {axis.field} range, or neither.")
    centre, derive, pos, vel, mass, periods = {}, None, [], [], "", [None] * 3
    if derived:
        if request.centre is None:
            raise ValueError("radius and radial_velocity need a centre.")
        pos, vel, mass = _resolve_fields(entry, request)
        if "radial_velocity" in derived and not vel:
            raise ValueError("radial_velocity needs the velocity fields.")
        use_vel = "radial_velocity" in derived
        if use_vel:
            _check_centre_velocity(request.centre, vel)
        periods = _periods(entry, request, pos)
        searching = request.centre.mode == "shrinking_sphere"
        centre = _centre(session, entry, request.filters, request.centre, pos, mass, periods,
                         stop, _stages(progress, 0.0, 0.4 if searching else 0.0))
        velocity = request.centre.velocity if use_vel else None
        if use_vel and velocity is None:
            probe = pp.default_centre_velocity(
                _selection_chunks(entry, request.filters, pos + vel + ([mass] if mass else [])),
                pos, vel, mass, centre["centre"], periods,
                region_limit=_region_limit(request.filters, pos, centre["centre"], periods),
                stop=stop, progress=_stages(progress, 0.4 if searching else 0.0, 0.6))
            velocity = probe["velocity"]
            centre["centre_velocity_from"] = probe
        if use_vel:
            centre["centre_velocity"] = velocity
        geometry = pp.Geometry(pos, vel if use_vel else [], centre["centre"], periods,
                               velocity if use_vel else None)
        derive = geometry.derive
    read = plain + ([request.weight_field] if request.weight_field else [])
    if derived:
        read += pos + (vel if "radial_velocity" in derived else [])
    start = (0.6 if derived and "radial_velocity" in derived and request.centre.velocity is None
             else 0.4 if derived and request.centre.mode == "shrinking_sphere" else 0.0)
    diagram = pp.phase_diagram(
        _selection_chunks(entry, request.filters, read), x=request.x.field, y=request.y.field,
        x_log=request.x.scale == "log", y_log=request.y.scale == "log",
        x_bins=request.x.bins, y_bins=request.y.bins,
        x_range=(request.x.min, request.x.max) if request.x.min is not None else None,
        y_range=(request.y.min, request.y.max) if request.y.min is not None else None,
        weight_field=request.weight_field, derive=derive, stop=stop,
        progress=_stages(progress, start, 1.0))
    columns = [request.x.field, request.y.field]
    units, physical = _units(entry, pos or ["x"], vel, request.weight_field, columns)
    provenance = _provenance(entry, request.filters, pos, vel, request.weight_field, periods)
    provenance["binning"] = {
        a: {"field": axis.field, "scale": axis.scale, "bins": axis.bins,
            "range": "given" if axis.min is not None else "the selection's finite extent"}
        for a, axis in (("x", request.x), ("y", request.y))}
    provenance["definitions"] = {
        "bins": "[lo, hi) except the last, which holds its upper edge",
        "dropped": "non-finite values, or values <= 0 on a log axis",
        "outside": "values outside a given range",
        "closure": "binned + outside + dropped = selected weight; binned + outside + "
                   "dropped + invalid_weight = selected rows (invalid_weight_rows: a weight "
                   "that is not finite and >= 0)",
        "derived": "radius and radial_velocity (peculiar, without the Hubble flow) are "
                   "measured from the centre; with a centre they take the place of table "
                   "columns of the same name",
    }
    return PhaseDiagramResponse(valid=True, error="", diagram=diagram, centre=centre,
                                units={"x": units["columns"][request.x.field],
                                       "y": units["columns"][request.y.field],
                                       "weight": units["mass"] if request.weight_field
                                       else "count",
                                       "physical": physical.get("columns", {})},
                                provenance=provenance)


def _task(session: Session, token: str, kind: str, compute, failure) -> TaskCreateResponse:
    """Start ``compute(progress, stop)`` as a task holding one of the
    session's compute slots until the computation itself ends."""
    task = TASKS.create(kind, owner_token=token)
    TASKS.update(task.task_id, status="running", progress=0.0)
    stop = threading.Event()

    def progress(fraction: float) -> None:
        TASKS.update(task.task_id, progress=min(0.99, max(0.0, float(fraction))))

    async def runner() -> None:
        work = asyncio.ensure_future(asyncio.to_thread(compute, progress, stop))

        def ended(future) -> None:
            session.release_task_slot()
            if not future.cancelled():
                future.exception()
        work.add_done_callback(ended)
        TASKS.update(task.task_id, work_handle=work)
        try:
            result = await asyncio.shield(work)
            TASKS.update(task.task_id, status="completed", progress=1.0,
                         result=result.model_dump())
        except asyncio.CancelledError:
            stop.set()
            TASKS.update(task.task_id, status="cancelled", error="Cancelled by client.")
        except (pp.ProfileCancelled, DiagnosticsCancelled):
            TASKS.update(task.task_id, status="cancelled", error="Cancelled by client.")
        except Exception as exc:   # noqa: BLE001 - reported in the result
            TASKS.update(task.task_id, status="completed", progress=1.0,
                         result=failure(str(exc)).model_dump())

    handle = asyncio.create_task(runner())
    TASKS.update(task.task_id, asyncio_handle=handle)
    return TaskCreateResponse(valid=True, error="", task_id=task.task_id, status="running")


@router.post("/v1/catalogue/profile", response_model=ParticleProfileResponse,
             tags=["catalogue"])
async def catalogue_profile(
    request: ParticleProfileRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ParticleProfileResponse:
    """Radial profile of the selected particles around a centre, answered
    when done; /v1/tasks/catalogue_profile runs it as a task."""
    try:
        entry = _require_dataset(session, request.dataset_id)
        return await asyncio.to_thread(_run_profile, session, entry, request)
    except HTTPException:
        raise
    except Exception as exc:   # noqa: BLE001 - reported in the response
        return ParticleProfileResponse(valid=False, error=str(exc))


@router.post("/v1/tasks/catalogue_profile", response_model=TaskCreateResponse, tags=["tasks"])
async def create_catalogue_profile_task(
    request: ParticleProfileRequest,
    token: str = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    """/v1/catalogue/profile as a task: progress over the centre search and
    the passes, cancellation between two chunks; a refused request still
    creates the task and its result says why (``valid: false``)."""
    entry = _require_dataset(session, request.dataset_id)
    _require_catalogue(entry)
    if not await acquire_task_slot(session):
        raise task_slot_busy_error()
    return _task(session, token, "catalogue_profile",
                 lambda progress, stop: _run_profile(session, entry, request,
                                                     _monotonic(progress), stop),
                 lambda error: ParticleProfileResponse(valid=False, error=error))


@router.post("/v1/catalogue/totals", response_model=ParticleTotalsResponse,
             tags=["catalogue"])
async def catalogue_totals(
    request: ParticleTotalsRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> ParticleTotalsResponse:
    """Physical totals of the selected particles (count, mass, centre-of-mass
    velocity, momentum, kinetic and thermal energy), answered when done;
    /v1/tasks/catalogue_totals runs it as a task."""
    try:
        entry = _require_dataset(session, request.dataset_id)
        return await asyncio.to_thread(_run_totals, entry, request)
    except HTTPException:
        raise
    except Exception as exc:   # noqa: BLE001 - reported in the response
        return ParticleTotalsResponse(valid=False, error=str(exc))


@router.post("/v1/tasks/catalogue_totals", response_model=TaskCreateResponse, tags=["tasks"])
async def create_catalogue_totals_task(
    request: ParticleTotalsRequest,
    token: str = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    """/v1/catalogue/totals as a task (see the profile task)."""
    entry = _require_dataset(session, request.dataset_id)
    _require_catalogue(entry)
    if not await acquire_task_slot(session):
        raise task_slot_busy_error()
    return _task(session, token, "catalogue_totals",
                 lambda progress, stop: _run_totals(entry, request, _monotonic(progress), stop),
                 lambda error: ParticleTotalsResponse(valid=False, error=error))


@router.post("/v1/catalogue/phase_diagram", response_model=PhaseDiagramResponse,
             tags=["catalogue"])
async def catalogue_phase_diagram(
    request: PhaseDiagramRequest,
    _: None = _auth,
    session: Session = Depends(get_session),
) -> PhaseDiagramResponse:
    """2-D histogram of two columns of the selected rows, answered when
    done; /v1/tasks/catalogue_phase_diagram runs it as a task."""
    try:
        entry = _require_dataset(session, request.dataset_id)
        return await asyncio.to_thread(_run_phase, session, entry, request)
    except HTTPException:
        raise
    except Exception as exc:   # noqa: BLE001 - reported in the response
        return PhaseDiagramResponse(valid=False, error=str(exc))


@router.post("/v1/tasks/catalogue_phase_diagram", response_model=TaskCreateResponse,
             tags=["tasks"])
async def create_catalogue_phase_diagram_task(
    request: PhaseDiagramRequest,
    token: str = _auth,
    session: Session = Depends(get_session),
) -> TaskCreateResponse:
    """/v1/catalogue/phase_diagram as a task (see the profile task)."""
    entry = _require_dataset(session, request.dataset_id)
    _require_catalogue(entry)
    if not await acquire_task_slot(session):
        raise task_slot_busy_error()
    return _task(session, token, "catalogue_phase_diagram",
                 lambda progress, stop: _run_phase(session, entry, request,
                                                   _monotonic(progress), stop),
                 lambda error: PhaseDiagramResponse(valid=False, error=error))
