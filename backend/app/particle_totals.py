"""Physical totals of the exact selected population of a particle table
(issue #426): count, mass, centre-of-mass velocity, momentum, kinetic and
thermal energy.

The selection is read through the chunk factory the profiles use
(app/particle_profiles.py), once, so memory is one chunk whatever the
selection's size and a run is cancellable between two chunks.

Kinetic energy in the centre-of-mass frame is 0.5 sum m |v - v_com|^2. It is
not computed as sum m v^2 - M v_com^2 (two large numbers whose difference is
small for a halo whose bulk motion dominates). Each chunk gives its own mass,
momentum and second moment about its own mean velocity, and the chunks are
merged with the pairwise update of Chan, Golub & LeVeque (1983): for parts A
and B, S = S_A + S_B + M_A M_B / (M_A + M_B) |v_A - v_B|^2. The total
kinetic energy, 0.5 sum m v^2, has only positive terms and is summed
directly; the two agree through 0.5 sum m v^2 = KE_com + 0.5 M |v_com|^2.

Everything is first computed in the table's own units. A value is converted
to physical units (Msun, km/s, Msun km/s, erg) with the reader's factors
(``Snapshot.physical()``) only when every field it needs has a declared
factor in the unit the formula expects; otherwise ``value`` is None and
``reason`` says what is missing, while ``stored`` still gives the value in
the table's own units. A quantity whose field is absent has neither, and
says so. Velocities are the peculiar velocities the reader converts them to
(no Hubble flow), so the energies are those of the peculiar motion in the
simulation frame.
"""
from __future__ import annotations

import math

import numpy as np

from .gadget_snapshot import MSUN_G
from .particle_profiles import Chunks, _Passes

#: cm^2/s^2 in a (km/s)^2.
KMS2_CM2S2 = 1e10


def _quantity(stored, stored_unit: str, factor, unit: str, data_reason, unit_reason) -> dict:
    """One total. ``stored``: in the table's own units, None when its data
    are missing (``data_reason`` says why). ``value``: physical, in ``unit``,
    None when ``factor`` could not be determined (``unit_reason``)."""
    def scale(x, f):
        if isinstance(x, np.ndarray):
            return [float(v) * f for v in x]
        return float(x) * f
    if data_reason is not None:
        return {"value": None, "unit": unit, "stored": None, "reason": data_reason}
    own = [float(v) for v in stored] if isinstance(stored, np.ndarray) else float(stored)
    if unit_reason is not None or factor is None:
        return {"value": None, "unit": unit, "stored": {"value": own, "unit": stored_unit},
                "reason": unit_reason}
    return {"value": scale(stored, factor), "unit": unit,
            "stored": {"value": own, "unit": stored_unit}, "reason": None}


def _factor_of(fields, unit: str, physical: dict, stored_units: dict):
    """(common physical factor of ``fields`` in ``unit``, None) or (None, why)."""
    found = []
    for f in fields:
        entry = physical.get(f)
        if entry is None:
            return None, (f"The physical unit of {f!r} is not declared by this dataset "
                          f"(stored as {stored_units.get(f, 'unit not declared')}).")
        if entry["unit"] != unit:
            return None, f"{f!r} is declared in {entry['unit']}, not the {unit} this total needs."
        found.append(float(entry["factor"]))
    if len(found) > 1 and not np.allclose(found, found[0], rtol=1e-12):
        return None, f"The fields {', '.join(fields)} have different physical factors."
    return found[0], None


def selection_totals(chunks: Chunks, *, mass_field: str, vel_fields, energy_field: str,
                     stored_units: dict, physical: dict,
                     missing: dict | None = None, stop=None, progress=None) -> dict:
    """The totals of the rows ``chunks`` yields. ``mass_field`` "" and empty
    ``vel_fields`` / ``energy_field`` mean the table has none (``missing``
    gives the reason per "mass", "velocity", "energy"). ``stored_units`` maps
    each field to the unit the table states, ``physical`` each field to
    {"factor", "unit"} (Snapshot.physical()).

    A row whose mass is not finite and >= 0 carries no weight and counts only
    in ``rows.selected``. Velocity quantities use the rows that also have
    finite velocities, the thermal energy those with a finite internal
    energy >= 0; ``rows`` says how many each used, and ``mass_with_velocity``
    is the mass of the first group (the centre-of-mass velocity is relative
    to it, not to the mass of every row)."""
    missing = missing or {}
    vel_fields = list(vel_fields or [])
    passes = _Passes(chunks, stop, progress, expected=1)
    selected = with_mass = with_vel = with_energy = 0
    mass_all = mass_v = thermal = second = twice_ke = 0.0
    momentum = np.zeros(3)
    mean_v = np.zeros(3)              # running mass-weighted mean, for the merge
    for columns in passes.run():
        n = len(next(iter(columns.values())))
        selected += n
        if not mass_field or n == 0:
            continue
        weight = np.asarray(columns[mass_field], dtype=np.float64)
        good = np.isfinite(weight) & (weight >= 0)
        with_mass += int(good.sum())
        mass_all += float(weight[good].sum())
        if vel_fields:
            vel = [np.asarray(columns[f], dtype=np.float64) for f in vel_fields]
            ok = good & np.isfinite(vel[0]) & np.isfinite(vel[1]) & np.isfinite(vel[2])
            if ok.any():
                w = weight[ok]
                v = [c[ok] for c in vel]
                m_c = float(w.sum())
                p_c = np.array([float(np.dot(w, c)) for c in v])
                with_vel += int(ok.sum())
                twice_ke += float(np.dot(w, v[0] ** 2 + v[1] ** 2 + v[2] ** 2))
                if m_c > 0:
                    v_c = p_c / m_c
                    s_c = float(np.dot(w, (v[0] - v_c[0]) ** 2 + (v[1] - v_c[1]) ** 2
                                       + (v[2] - v_c[2]) ** 2))
                    gap = v_c - mean_v
                    second += s_c + (mass_v * m_c / (mass_v + m_c) * float(gap @ gap)
                                     if mass_v > 0 else 0.0)
                    mean_v = (momentum + p_c) / (mass_v + m_c)
                mass_v += m_c
                momentum += p_c
        if energy_field:
            u = np.asarray(columns[energy_field], dtype=np.float64)
            ok = good & np.isfinite(u) & (u >= 0)
            with_energy += int(ok.sum())
            thermal += float(np.dot(weight[ok], u[ok]))
    if progress is not None:
        progress(1.0)

    def unit_of(field):
        return str(stored_units.get(field, "unit not declared"))

    # Why a quantity has no stored value (data) or no physical one (unit).
    no_mass = missing.get("mass") or ("The dataset has no mass field, so no mass-weighted "
                                      "total is defined.")
    no_vel = missing.get("velocity") or "No velocity fields: this total needs vx, vy, vz."
    no_energy = missing.get("energy") or "No internal-energy field in this dataset."
    mass_data = None if mass_field else no_mass
    vel_data = mass_data or (no_vel if not vel_fields else
                             None if mass_v > 0 else
                             "No selected row has finite velocities and a positive mass.")
    energy_data = mass_data or (no_energy if not energy_field else None)
    f_mass, mass_unit_why = _factor_of([mass_field], "Msun", physical, stored_units) \
        if mass_field else (None, None)
    f_vel, vel_unit_why = _factor_of(vel_fields, "km/s", physical, stored_units) \
        if vel_fields else (None, None)
    f_u, u_unit_why = _factor_of([energy_field], "erg/g", physical, stored_units) \
        if energy_field else (None, None)
    both_why = mass_unit_why or vel_unit_why
    f_kms2 = None if both_why or f_mass is None or f_vel is None else \
        f_mass * f_vel ** 2 * MSUN_G * KMS2_CM2S2
    f_momentum = None if both_why or f_mass is None or f_vel is None else f_mass * f_vel
    thermal_why = mass_unit_why or u_unit_why
    f_thermal = None if thermal_why or f_mass is None or f_u is None else f_mass * f_u * MSUN_G
    m_unit = unit_of(mass_field) if mass_field else ""
    v_unit = unit_of(vel_fields[0]) if vel_fields else ""
    have_v = vel_data is None
    com = momentum / mass_v if have_v else None
    q = {
        "mass": _quantity(mass_all, m_unit, f_mass, "Msun", mass_data, mass_unit_why),
        "centre_of_mass_velocity": _quantity(com, v_unit, f_vel, "km/s", vel_data,
                                             vel_unit_why),
        "momentum": _quantity(momentum, f"{m_unit} {v_unit}", f_momentum, "Msun km/s",
                              vel_data, both_why),
        "momentum_magnitude": _quantity(float(np.linalg.norm(momentum)), f"{m_unit} {v_unit}",
                                        f_momentum, "Msun km/s", vel_data, both_why),
        "kinetic_energy": _quantity(0.5 * twice_ke, f"{m_unit} ({v_unit})^2", f_kms2, "erg",
                                    vel_data, both_why),
        "kinetic_energy_com_frame": _quantity(0.5 * second, f"{m_unit} ({v_unit})^2", f_kms2,
                                              "erg", vel_data, both_why),
        "velocity_dispersion_1d": _quantity(
            math.sqrt(second / mass_v / 3.0) if have_v else 0.0, v_unit, f_vel, "km/s",
            vel_data, vel_unit_why),
        "thermal_energy": _quantity(
            thermal, f"{m_unit} {unit_of(energy_field)}" if energy_field else "", f_thermal,
            "erg", energy_data, thermal_why),
    }
    return {"count": selected,
            "rows": {"selected": selected, "with_mass": with_mass,
                     "with_velocity": with_vel, "with_energy": with_energy},
            "mass_with_velocity": mass_v if vel_fields and mass_field else None,
            "quantities": q}
