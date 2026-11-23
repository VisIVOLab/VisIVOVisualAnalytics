"""The spectral axis of a cube as a table, in another quantity or frame
(issue #496).

A cube's spectral axis is a linear WCS in one quantity (frequency, radio or
optical velocity) and one reference frame (``SPECSYS``). Converting it to
another quantity needs a rest frequency; to another frame, the observer and
the target, which astropy builds from the header (``MJD-OBS``/``DATE-OBS``,
``OBSGEO-*``, the celestial reference). Velocity changes are not linear in
frequency, so the answer is one value per channel, not a new CDELT.
"""

from __future__ import annotations

import warnings
from typing import Any

import numpy as np

#: FITS SPECSYS name -> what ``SpectralCoord.with_observer_stationary_relative_to``
#: takes ("TOPOCENT" is handled apart: at rest on the rotating Earth).
FRAMES = {
    "BARYCENT": "icrs",
    "LSRK": "lsrk",
    "LSRD": "lsrd",
    "GEOCENTR": "gcrs",
    "GALACTOC": "galactocentric",
    "TOPOCENT": None,
}
QUANTITIES = ("native", "frequency", "vrad", "vopt")


def _spectral_coord(dataset, nspec: int):
    """The SpectralCoord of every channel at the celestial reference pixel."""
    from astropy.coordinates import SpectralCoord  # noqa: PLC0415
    from astropy.wcs import WCS  # noqa: PLC0415

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        wcs = WCS(dataset.header)
    if not wcs.has_spectral:
        raise ValueError("This cube has no spectral axis in its WCS.")
    spectral_index = wcs.wcs.spec               # 0-based WCS axis
    pixels = []
    for axis in range(wcs.pixel_n_dim):
        if axis == spectral_index:
            pixels.append(np.arange(nspec, dtype=np.float64))
        else:
            ref = float(dataset.header.get(f"CRPIX{axis + 1}", 1.0)) - 1.0
            size = int(dataset.header.get(f"NAXIS{axis + 1}", 1))
            pixels.append(np.full(nspec, ref if size > 1 else 0.0))
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            world = wcs.pixel_to_world(*pixels)
    except NotImplementedError:
        # No (or an unknown) SPECSYS: astropy will not attach a frame. The
        # values are the linear axis all the same, without observer or target.
        import astropy.units as u  # noqa: PLC0415
        spectral = wcs.sub(["spectral"])
        values = spectral.all_pix2world(np.arange(nspec, dtype=np.float64), 0)[0]
        unit = u.Unit(spectral.wcs.cunit[0] or "")
        rest = dataset._rest_freq_hz()
        ctype = str(spectral.wcs.ctype[0]).upper()
        convention = ("optical" if ctype.startswith(("VOPT", "ZOPT", "WAVE"))
                      else "radio" if ctype.startswith("VRAD") else None)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            return SpectralCoord(values * unit,
                                 doppler_rest=rest * u.Hz if rest else None,
                                 doppler_convention=convention)
    for item in world if isinstance(world, (list, tuple)) else [world]:
        if isinstance(item, SpectralCoord):
            return item
    raise ValueError("The WCS gives no spectral coordinate for this cube.")


def _obstime(header):
    from astropy.time import Time  # noqa: PLC0415
    if header.get("MJD-OBS") is not None:
        return Time(float(header["MJD-OBS"]), format="mjd", scale="utc")
    if header.get("DATE-OBS"):
        return Time(str(header["DATE-OBS"]), scale="utc")
    return None


def _at_rest(frame: str, header):
    """A coordinate at rest (zero velocity) in ``frame``: the observer of a
    spectrum in that frame. The barycentre, the LSR and the Galactic centre
    need nothing from the header; the geocentre and the telescope are where
    they are at the observation time (an observer at rest in a rotating frame
    is at rest only at one place), so those need the date, and the telescope
    its position."""
    import astropy.units as u  # noqa: PLC0415
    from astropy.coordinates import (GCRS, ICRS, ITRS, LSRD, LSRK,  # noqa: PLC0415
                                     CartesianDifferential, CartesianRepresentation,
                                     EarthLocation, Galactocentric)
    still = CartesianDifferential([0.0, 0.0, 0.0] * (u.km / u.s))
    origin = CartesianRepresentation([0.0, 0.0, 0.0] * u.m).with_differentials(still)
    simple = {"BARYCENT": ICRS, "LSRK": LSRK, "LSRD": LSRD, "GALACTOC": Galactocentric}
    if frame in simple:
        return simple[frame](origin)
    obstime = _obstime(header)
    if obstime is None:
        raise ValueError("A geocentric or topocentric spectral frame needs the observation "
                         "date in the header (MJD-OBS or DATE-OBS).")
    if frame == "GEOCENTR":
        return GCRS(origin, obstime=obstime)
    if not all(header.get(k) is not None for k in ("OBSGEO-X", "OBSGEO-Y", "OBSGEO-Z")):
        raise ValueError("A topocentric spectral frame needs the observatory's position in "
                         "the header (OBSGEO-X/Y/Z).")
    site = EarthLocation.from_geocentric(*(float(header[k]) for k in
                                           ("OBSGEO-X", "OBSGEO-Y", "OBSGEO-Z")), unit=u.m)
    position = site.get_itrs(obstime).cartesian.without_differentials()
    return ITRS(position.with_differentials(still), obstime=obstime)


def _with_observer(sc, dataset, native_frame: str):
    """``sc`` with an observer at rest in the file's frame and the target at
    the celestial reference, when astropy attached none (it does only with a
    date and an observatory position, which a barycentric or LSR cube does
    not need)."""
    from astropy.coordinates import ICRS, SpectralCoord  # noqa: PLC0415
    from astropy.wcs import WCS  # noqa: PLC0415

    observer = _at_rest(native_frame, dataset.header)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        celestial = WCS(dataset.header).celestial
        if celestial.naxis != 2:
            raise ValueError("Converting the spectral frame needs the cube's sky position, "
                             "and its WCS has no celestial axes.")
        target = celestial.pixel_to_world(*(c - 1.0 for c in celestial.wcs.crpix))
        target = target.transform_to(ICRS())
        return SpectralCoord(sc.quantity, observer=observer, target=target,
                             doppler_rest=sc.doppler_rest,
                             doppler_convention=sc.doppler_convention)


def spectral_axis(path: str, quantity: str = "native", frame: str = "native",
                  rest_freq_hz: float | None = None) -> dict[str, Any]:
    """One value per channel of the cube's spectral axis.

    ``quantity``: ``native`` (the file's), ``frequency`` (Hz), ``vrad`` or
    ``vopt`` (km/s, radio or optical convention). ``frame``: ``native`` or a
    FITS SPECSYS name of ``FRAMES``. ``rest_freq_hz`` replaces the header's
    rest frequency for the velocities (the frequencies do not depend on it)."""
    import astropy.units as u  # noqa: PLC0415

    from .fits_dataset import ScientificFitsDataset  # noqa: PLC0415

    quantity = (quantity or "native").lower()
    if quantity not in QUANTITIES:
        raise ValueError(f"Unknown spectral quantity '{quantity}'.")
    frame = (frame or "native").upper() if frame and frame.lower() != "native" else "native"
    if frame != "native" and frame not in FRAMES:
        raise ValueError(f"Unknown spectral frame '{frame}'; one of {', '.join(FRAMES)}.")

    dataset = ScientificFitsDataset(path)
    nspec = int(dataset.geometry_metadata()["depth"])
    native_frame = str(dataset.header.get("SPECSYS", "") or "").strip().upper()
    header_rest = dataset._rest_freq_hz()
    sc = _spectral_coord(dataset, nspec)
    native_unit = str(sc.unit)
    native_ctype = str(dataset.header.get(
        f"CTYPE{dataset.display_fits_axes()[2]}", "")).strip()[:4].upper()

    note = []
    out_frame = native_frame or "unknown"
    if frame != "native" and frame != native_frame:
        if not native_frame:
            raise ValueError("The header does not say which frame its spectral axis is in "
                             "(no SPECSYS), so it cannot be converted to another one.")
        if native_frame not in FRAMES:
            raise ValueError(f"The file's spectral frame '{native_frame}' cannot be converted.")
        if sc.observer is None or sc.target is None:
            sc = _with_observer(sc, dataset, native_frame)
        at_rest = _at_rest(frame, dataset.header)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            sc = sc.with_observer_stationary_relative_to(at_rest)
        out_frame = frame
    rest = rest_freq_hz if rest_freq_hz and rest_freq_hz > 0 else header_rest

    if quantity == "native" or (quantity == "frequency" and sc.unit.physical_type == "frequency"):
        values, unit = sc.to_value(sc.unit), native_unit
        ctype = native_ctype or "FREQ"
        if quantity == "frequency":
            values, unit, ctype = sc.to_value(u.Hz), "Hz", "FREQ"
    elif quantity == "frequency":
        if sc.doppler_rest is None:
            raise ValueError("A velocity axis needs a rest frequency to be shown as frequency; "
                             "the header has none.")
        values, unit, ctype = sc.to_value(u.Hz), "Hz", "FREQ"
    else:
        if not rest:
            raise ValueError("A velocity needs a rest frequency; the header has none: give one.")
        convention = "radio" if quantity == "vrad" else "optical"
        # Through frequency, so a native velocity axis takes the new rest frequency too.
        freq = sc.to(u.Hz) if sc.unit.physical_type != "frequency" else sc
        values = freq.to_value(u.km / u.s, doppler_convention=convention,
                               doppler_rest=rest * u.Hz)
        unit, ctype = "km/s", "VRAD" if quantity == "vrad" else "VOPT"
        if rest_freq_hz and header_rest and abs(rest_freq_hz - header_rest) > 1e-6 * header_rest:
            note.append(f"rest frequency {rest_freq_hz:.9g} Hz instead of the header's "
                        f"{header_rest:.9g} Hz")
    return {
        "values": [float(v) for v in np.asarray(values, dtype=np.float64)],
        "unit": unit, "ctype": ctype, "frame": out_frame, "native_frame": native_frame,
        "native_unit": native_unit, "native_ctype": native_ctype,
        "rest_freq_hz": float(rest) if rest else None,
        "header_rest_freq_hz": float(header_rest) if header_rest else None,
        "note": "; ".join(note),
    }
