"""Physical units of a projected particle map (issue #393).

A projected map (particle_raytrace.projected_maps) holds, per pixel, the
weight the particles' kernels put in it and the weighted mean of a quantity.
A Gadget snapshot states what its columns are (gadget_snapshot.Component
physical and units), so its maps can be written in physical units: the
surface density in Msun/kpc² when the weight is the particles' mass (a number
per kpc² with no weight), the mean in the quantity's own unit (a temperature
in K, an entropy in keV cm², a velocity dispersion in km/s), the pixels in
physical kpc. Lengths and masses in h units and comoving lengths are made
physical as the reader does (x a / h, / h), so a surface density in code
units is multiplied by h / a². Any other table states no units: its maps
stay relative.
"""
from __future__ import annotations

import io
import math

import numpy as np

RELATIVE = "relative"


def map_units(component, *, axes, weight: str | None, power: float, clipped: int,
              light_unit: float, colour_by: str, value_field: str | None,
              velocity: tuple[str, str, str] | None, log10: bool = False) -> dict:
    """What a projected map's two planes are and the factors that make them
    physical, for a table of points (``component`` a Gadget snapshot's
    particle type, or None for any other table).

    ``axes``: the position columns (lengths only when all three are),
    ``weight``: the column the particles are weighted by (None: one each),
    ``power`` and ``clipped`` as the light was made of it
    (particle_raytrace.normalised_light), ``light_unit`` the column's value
    the light is 1 at, ``log10`` whether the column was read as its log10.
    The keys: ``density_unit`` and ``density_factor`` (plane 1 times the
    factor, over a pixel's area in kpc², is in that unit), ``value_unit``
    and ``value_factor`` (plane 2 times the factor), ``weighting``,
    ``power``, ``clipped_rows``, and for a snapshot ``kpc_per_unit``
    (physical kpc per table unit, when the positions are lengths),
    ``comoving``, ``redshift`` (a comoving run's), ``hubble``,
    ``hubble_units``, ``scale_factor``."""
    out = {"density_unit": RELATIVE, "density_factor": 1.0, "value_unit": RELATIVE,
           "value_factor": 1.0, "weighting": weight or "number",
           "power": float(power) if weight else 1.0, "clipped_rows": int(clipped)}
    if component is None:
        return out
    physical, stored = component.physical(), component.units()
    snapshot = component.snapshot
    lengths = {physical.get(a, {}).get("unit") for a in axes}
    if lengths == {"kpc"}:
        out["kpc_per_unit"] = float(physical[axes[0]]["factor"])
        if weight is None:
            out["density_unit"] = "kpc^-2"
        elif (physical.get(weight, {}).get("unit") == "Msun" and power == 1.0
              and not clipped and not log10 and math.isfinite(light_unit)
              and light_unit > 0):
            out["density_unit"] = "Msun/kpc^2"
            out["density_factor"] = float(light_unit) * float(physical[weight]["factor"])
    out["value_unit"], out["value_factor"] = _value_unit(colour_by, value_field, velocity,
                                                         physical, stored)
    out.update({"comoving": bool(snapshot.units.comoving),
                "scale_factor": float(snapshot.scale_factor),
                "hubble": float(snapshot.header.hubble),
                "hubble_units": bool(snapshot.units.hubble_units)})
    if snapshot.units.comoving:
        out["redshift"] = float(snapshot.header.redshift)
    return out


def pixel_size(camera, height: int) -> float:
    """A projected map's pixel, in the camera's units: its view at the
    focal plane (VTK's camera: position, focal_point, view_angle, parallel,
    parallel_scale) over ``height`` pixels."""
    pos = np.asarray(camera.position, dtype=np.float64)
    fp = np.asarray(camera.focal_point, dtype=np.float64)
    distance = float(np.linalg.norm(fp - pos)) or 1.0
    half = (camera.parallel_scale if camera.parallel
            else distance * math.tan(math.radians(camera.view_angle) / 2.0))
    return 2.0 * half / height


def physical_planes(maps: np.ndarray, units: dict, camera,
                    height: int) -> tuple[np.ndarray, np.ndarray]:
    """The map's two planes in ``units``' units (float64): the surface
    density over a pixel's area (in kpc² when the table is a snapshot's),
    the mean times its factor."""
    pixel = pixel_size(camera, height) * units.get("kpc_per_unit", 1.0)
    density = np.asarray(maps[..., 0], dtype=np.float64) * units["density_factor"]
    return density / (pixel * pixel), np.asarray(maps[..., 1],
                                                 dtype=np.float64) * units["value_factor"]


#: Units as FITS writes them (the FITS standard's names).
_FITS_UNITS = {"Msun/kpc^2": "solMass kpc-2", "kpc^-2": "kpc-2", "K": "K",
               "keV cm^2": "keV cm2", "km/s": "km s-1", "g/cm^3": "g cm-3",
               "erg/g": "erg g-1", "kpc": "kpc", "Msun": "solMass", "dimensionless": ""}


def projected_fits(maps: np.ndarray, units: dict, camera, width: int, height: int,
                   slab: float | None) -> bytes:
    """A projected map as FITS: the surface density in the primary HDU, the
    weighted mean in the ``MEAN`` extension, each in its unit, rows bottom
    up as FITS shows them, on a linear grid centred on the focal point:
    physical kpc for a snapshot (comoving kpc too), the table's units for
    any other table. ``camera`` in the table's coordinates; a perspective
    camera's map is drawn without perspective, as seen at its focal plane."""
    from astropy.io import fits  # noqa: PLC0415
    pos = np.asarray(camera.position, dtype=np.float64)
    fp = np.asarray(camera.focal_point, dtype=np.float64)
    forward = fp - pos
    forward /= float(np.linalg.norm(forward)) or 1.0
    right = np.cross(forward, np.asarray(camera.view_up, dtype=np.float64))
    right /= float(np.linalg.norm(right)) or 1.0
    up = np.cross(right, forward)
    per_pixel = pixel_size(camera, height)
    kpc = units.get("kpc_per_unit")
    planes = physical_planes(maps, units, camera, height)
    hdus = [fits.PrimaryHDU(np.ascontiguousarray(planes[0][::-1]).astype(np.float32)),
            fits.ImageHDU(np.ascontiguousarray(planes[1][::-1]).astype(np.float32),
                          name="MEAN")]
    vector = lambda v: " ".join(f"{x + 0.0:.6g}" for x in v)  # noqa: E731
    for hdu, unit, what in ((hdus[0], units["density_unit"], "projected surface density"),
                            (hdus[1], units["value_unit"],
                             f"mean weighted by {units['weighting']}")):
        h = hdu.header
        if unit in (RELATIVE, "none"):
            h["BUNIT"] = ("", f"{what}, {unit}")
        else:
            h["BUNIT"] = (_FITS_UNITS.get(unit, unit), what)
        for axis, n in ((1, width), (2, height)):
            h[f"CTYPE{axis}"] = ("LINEAR", "offset from the focal point in the view plane")
            h[f"CRPIX{axis}"] = (n / 2.0 + 0.5, "the focal point")
            h[f"CRVAL{axis}"] = 0.0
            if kpc:
                h[f"CDELT{axis}"] = (per_pixel * kpc, "physical kpc per pixel")
                h[f"CUNIT{axis}"] = "kpc"
            else:
                h[f"CDELT{axis}"] = (per_pixel, "table units per pixel")
        if kpc and units.get("comoving"):
            h["CDELTCOM"] = (per_pixel * kpc / units["scale_factor"],
                             "comoving kpc per pixel, h removed")
        if "comoving" in units:
            h["COMOVING"] = (bool(units["comoving"]), "snapshot lengths comoving")
        if "redshift" in units:
            h["REDSHIFT"] = units["redshift"]
        if "hubble" in units:
            h["HUBBLE"] = (units["hubble"], "h, H0 / (100 km/s/Mpc)")
            h["HUNITS"] = (bool(units.get("hubble_units")), "T: stored per h, h divided out")
            h["SCALEFAC"] = (units["scale_factor"], "a, 1 when not comoving")
        h["PROJAXIS"] = (vector(forward), "line of sight, table axes")
        h["XAXIS"] = (vector(right), "image +x, table axes")
        h["YAXIS"] = (vector(up), "image +y, table axes")
        h["FOCALX"], h["FOCALY"], h["FOCALZ"] = ((float(v), "focal point, table units")
                                                 for v in fp)
        if slab:
            h["DEPTH"] = ((slab * kpc, "slab thickness, physical kpc") if kpc
                          else (slab, "slab thickness, table units"))
        else:
            h["DEPTH"] = ("all", "the whole selection along the line of sight")
        h["WEIGHT"] = (units["weighting"], "what each particle carries")
        h["POWER"] = (units.get("power", 1.0), "the weight raised to this power")
        h["CLIPPED"] = (units.get("clipped_rows", 0), "rows whose weight was capped")
        h["ORIGIN"] = "VisIVO Visual Analytics"
    buf = io.BytesIO()
    fits.HDUList(hdus).writeto(buf)
    return buf.getvalue()


def _value_unit(colour_by, field, velocity, physical, stored) -> tuple[str, float]:
    """Plane 2's unit and factor: the colour column's, or the velocity
    columns' for a dispersion; none for a map coloured by its density."""
    if colour_by == "velocity_dispersion":
        field = (velocity or ("vx",))[0]
    elif colour_by != "colour" or not field:
        return "none", 1.0
    if field in physical:
        return physical[field]["unit"], float(physical[field]["factor"])
    unit = stored.get(field)
    if unit in ("K", "keV cm^2", "dimensionless"):
        return unit, 1.0
    return RELATIVE, 1.0
