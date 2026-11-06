"""SODA BAND on an Obit MFImage cube (CTYPE3 = 'SPECLNMF'), issue #115.

The cube is a small synthetic copy of the MeerKAT Milky Way Bulge layout:
NTERM fitted planes, then NSPEC sub-bands whose frequencies are only in
FREQnnnn / FRELnnnn / FREHnnnn, while CRVAL3 / CDELT3 describe a linear grid
that has nothing to do with them (as in the release).
"""
from __future__ import annotations

import io
import xml.etree.ElementTree as ET

import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS

from app.soda_cutout import CutoutError, CutoutNoOverlap, describe, make_cutout, stokes_states

C = 299792458.0
VOT = "{http://www.ivoa.net/xml/VOTable/v1.3}"
# Sub-band centres in Hz: the first three evenly spaced (as in the release),
# the rest not. Edges are the midpoints, the outer ones half a step out.
FREQ = np.array([908.0, 952.0, 996.0, 1043.0, 1093.0, 1145.0]) * 1e6
EDGES = np.concatenate(([FREQ[0] - 22e6], (FREQ[1:] + FREQ[:-1]) / 2, [FREQ[-1] + 26e6]))
N_TERM = 2


def _band(f_hi_hz, f_lo_hz):
    """The SODA BAND (metres) of a frequency interval."""
    return f"{C / f_hi_hz!r} {C / f_lo_hz!r}"


def _subband_cube(path, *, edges=True, freq_keys=True, n_term=N_TERM, freqs=FREQ, nstokes=3):
    """(stokes, plane, y, x): fitted planes hold 100 + term, sub-band k (1-based)
    holds k + 10 × Stokes code, so every plane says what it is."""
    n_spec = len(freqs)
    data = np.empty((nstokes, n_term + n_spec, 6, 6), np.float32)
    for s in range(nstokes):
        data[s, :n_term] = (100.0 + np.arange(n_term))[:, None, None]
        data[s, n_term:] = (np.arange(1, n_spec + 1) + 10.0 * (s + 1))[:, None, None]
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    for key, value in (("CTYPE1", "GLON-SIN"), ("CTYPE2", "GLAT-SIN"), ("CTYPE3", "SPECLNMF"),
                       ("CTYPE4", "STOKES"), ("CRVAL1", 0.0), ("CRVAL2", 2.5),
                       ("CRVAL3", 1.283791015625e9), ("CRVAL4", 1.0), ("CDELT1", -4.166667e-4),
                       ("CDELT2", 4.166667e-4), ("CDELT3", 5.755108e7), ("CDELT4", 1.0),
                       ("CRPIX1", 3.0), ("CRPIX2", 3.0), ("CRPIX3", 1.0), ("CRPIX4", 1.0),
                       ("NTERM", n_term), ("NSPEC", n_spec), ("ORIGIN", "Obit")):
        h[key] = value
    for i, f in enumerate(freqs, start=1):
        if freq_keys:
            h[f"FREQ{i:04d}"] = float(f)
        if edges:
            h[f"FREL{i:04d}"] = float(EDGES[i - 1])
            h[f"FREH{i:04d}"] = float(EDGES[i])
    hdu.writeto(path)
    return path


def _open(fits_bytes, index=0):
    with fits.open(io.BytesIO(fits_bytes)) as hdul:
        return hdul[index].data.copy(), hdul[index].header.copy(), len(hdul)


def _freq_keys(header):
    return [header[f"FREQ{i:04d}"] for i in range(1, header["NSPEC"] + 1)]


# ── selection ───────────────────────────────────────────────────────────────


@pytest.mark.parametrize("f_lo, f_hi, picked", [
    (900e6, 1000e6, [1, 2, 3]),
    (952e6, 1043e6, [2, 3, 4]),          # centres on the bounds are in
    (970e6, 1050e6, [3, 4]),             # clips sub-band 2's upper edge, not its centre
    (1100e6, 2000e6, [6]),
    (1e6, 1e12, [1, 2, 3, 4, 5, 6]),
])
def test_band_keeps_the_subbands_whose_centre_is_inside(tmp_path, f_lo, f_hi, picked):
    path = _subband_cube(tmp_path / "c.fits")
    result = make_cutout(str(path), band=_band(f_hi, f_lo))
    data, header, _ = _open(result.fits_bytes)
    assert result.applied["band"] and result.applied["subbands"] == picked
    # Stokes I: plane value is the sub-band number + 10; no fitted plane.
    assert data[0, :, 0, 0].tolist() == [k + 10.0 for k in picked]
    assert header["NTERM"] == 0 and header["NSPEC"] == len(picked)
    assert _freq_keys(header) == [FREQ[k - 1] for k in picked]
    assert [header[f"FREL{j:04d}"] for j in range(1, len(picked) + 1)] == [EDGES[k - 1] for k in picked]
    assert [header[f"FREH{j:04d}"] for j in range(1, len(picked) + 1)] == [EDGES[k] for k in picked]
    assert f"FREQ{len(picked) + 1:04d}" not in header


def test_open_ended_band(tmp_path):
    path = _subband_cube(tmp_path / "c.fits")
    upper = make_cutout(str(path), band=f"-Inf {C / 1000e6!r}")      # every frequency above 1 GHz
    assert upper.applied["subbands"] == [4, 5, 6]
    lower = make_cutout(str(path), band=f"{C / 960e6!r} +Inf")
    assert lower.applied["subbands"] == [1, 2]


@pytest.mark.parametrize("f_lo, f_hi", [
    (1000e6, 1040e6),        # between sub-bands 3 and 4, inside both their edges
    (2e9, 3e9),              # above
    (100e6, 800e6),          # below
])
def test_band_without_a_centre_is_no_overlap(tmp_path, f_lo, f_hi):
    path = _subband_cube(tmp_path / "c.fits")
    with pytest.raises(CutoutNoOverlap, match="centre of any sub-band"):
        make_cutout(str(path), band=_band(f_hi, f_lo))
    with pytest.raises(CutoutNoOverlap):
        make_cutout(str(path), band="-2 -1")                          # no positive wavelength


def test_without_band_the_cube_is_unchanged(tmp_path):
    path = _subband_cube(tmp_path / "c.fits")
    data, header, _ = _open(make_cutout(str(path), pol="I").fits_bytes)
    assert data.shape[1] == N_TERM + len(FREQ) and header["NTERM"] == N_TERM
    assert header["CTYPE3"] == "SPECLNMF" and _freq_keys(header) == list(FREQ)


def test_a_speclnmf_axis_without_frequencies_refuses_band(tmp_path):
    path = _subband_cube(tmp_path / "c.fits", freq_keys=False)
    with pytest.raises(CutoutError, match="no usable per-sub-band frequencies"):
        make_cutout(str(path), band="0.2 0.4")
    assert describe(str(path)).band_m is None
    # Keywords that do not fill the axis are not trusted either.
    bad = _subband_cube(tmp_path / "bad.fits", n_term=3)
    with fits.open(bad, mode="update") as hdul:
        hdul[0].header["NTERM"] = 1
    with pytest.raises(CutoutError):
        make_cutout(str(bad), band="0.2 0.4")


# ── the cutout header ───────────────────────────────────────────────────────


def test_evenly_spaced_subbands_become_a_freq_axis(tmp_path):
    path = _subband_cube(tmp_path / "c.fits")
    data, header, _ = _open(make_cutout(str(path), band=_band(1000e6, 900e6)).fits_bytes)
    assert header["CTYPE3"] == "FREQ" and header["CUNIT3"] == "Hz"
    wcs = WCS(header)
    np.testing.assert_allclose(wcs.spectral.pixel_to_world_values(np.arange(3)), FREQ[:3], atol=1e-3)


def test_one_subband_is_a_freq_axis_its_width_wide(tmp_path):
    path = _subband_cube(tmp_path / "c.fits")
    _, header, _ = _open(make_cutout(str(path), band=_band(1100e6, 1080e6)).fits_bytes)
    assert header["CTYPE3"] == "FREQ" and header["CRVAL3"] == FREQ[4]
    assert header["CDELT3"] == pytest.approx(EDGES[5] - EDGES[4])
    # Without edges the release's own increment stays.
    plain = _subband_cube(tmp_path / "plain.fits", edges=False)
    _, header, _ = _open(make_cutout(str(plain), band=_band(1100e6, 1080e6)).fits_bytes)
    assert header["CDELT3"] == 5.755108e7 and "FREL0001" not in header


def test_uneven_subbands_stay_speclnmf_and_cut_again(tmp_path):
    path = _subband_cube(tmp_path / "c.fits")
    first = make_cutout(str(path), band=_band(1200e6, 990e6))
    assert first.applied["subbands"] == [3, 4, 5, 6]
    _, header, _ = _open(first.fits_bytes)
    assert header["CTYPE3"] == "SPECLNMF" and header["CRVAL3"] == FREQ[2]
    assert header["CDELT3"] == pytest.approx((FREQ[5] - FREQ[2]) / 3)
    # The cutout is itself a valid MFImage cube: describe and BAND read it.
    again_path = tmp_path / "again.fits"
    again_path.write_bytes(first.fits_bytes)
    assert describe(str(again_path)).band_m == pytest.approx((C / EDGES[6], C / EDGES[2]))
    second = make_cutout(str(again_path), band=_band(1100e6, 1040e6))
    data, header, _ = _open(second.fits_bytes)
    assert second.applied["subbands"] == [2, 3]                     # its own numbering
    assert data[0, :, 0, 0].tolist() == [14.0, 15.0] and _freq_keys(header) == list(FREQ[3:5])


def test_rm_synthesis_reads_the_cutout_frequencies(tmp_path):
    from app.compute.polarisation import _mfimage_subbands
    from app.fits_dataset import ScientificFitsDataset

    path = _subband_cube(tmp_path / "c.fits", nstokes=1)
    out = tmp_path / "out.fits"
    out.write_bytes(make_cutout(str(path), band=_band(1200e6, 990e6)).fits_bytes)
    offset, freqs = _mfimage_subbands(ScientificFitsDataset(str(out)))
    assert offset == 0 and freqs.tolist() == list(FREQ[2:])


def test_band_with_pol_split_into_hdus(tmp_path):
    path = _subband_cube(tmp_path / "c.fits", nstokes=4)
    result = make_cutout(str(path), band=_band(1000e6, 900e6), pol=["I", "Q", "V"])
    assert result.applied["hdus"] == 3                      # I, Q, V: not one linear axis
    for index, (code, state) in enumerate(((1, "I"), (2, "Q"), (4, "V"))):
        data, header, _ = _open(result.fits_bytes, index)
        assert stokes_states(header) == [state]
        assert data[0, :, 0, 0].tolist() == [10.0 * code + k for k in (1, 2, 3)]
        assert header["CTYPE3"] == "FREQ" and _freq_keys(header) == list(FREQ[:3])


def test_describe_advertises_the_subband_edges(tmp_path):
    path = _subband_cube(tmp_path / "c.fits")
    assert describe(str(path)).band_m == pytest.approx((C / EDGES[-1], C / EDGES[0]))
    plain = _subband_cube(tmp_path / "plain.fits", edges=False)
    assert describe(str(plain)).band_m == pytest.approx((C / FREQ[-1], C / FREQ[0]))


# ── routes ──────────────────────────────────────────────────────────────────


def _id(path):
    return f"file://{path}"


@pytest.mark.asyncio
async def test_sync_band_on_a_subband_cube(client, auth_headers, tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    path = _subband_cube(tmp_path / "c.fits")
    ok = await client.get("/soda/sync", params=[("ID", _id(path)), ("BAND", _band(1000e6, 900e6)),
                                                ("POL", "Q"), ("POL", "U")], headers=auth_headers)
    assert ok.status_code == 200, ok.text
    assert set(ok.headers["x-soda-applied"].split(",")) == {"band", "pol"}
    data, header, _ = _open(ok.content)
    assert data.shape[:2] == (2, 3) and stokes_states(header) == ["Q", "U"]
    gap = await client.get("/soda/sync", params={"ID": _id(path), "BAND": _band(1040e6, 1000e6)},
                           headers=auth_headers)
    assert gap.status_code == 204


@pytest.mark.asyncio
async def test_links_and_pyvo_on_a_subband_cube(client, auth_headers, tmp_path, monkeypatch):
    adhoc = pytest.importorskip("pyvo.dal.adhoc")
    import astropy.units as u
    from astropy.io.votable import parse

    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path))
    path = _subband_cube(tmp_path / "c.fits")
    resp = await client.get("/soda/links", params={"ID": _id(path)}, headers=auth_headers)
    assert resp.status_code == 200
    root = ET.fromstring(resp.text)
    service = next(r for r in root.iter(f"{VOT}RESOURCE") if r.get("ID") == "soda-sync-0")
    band = next(p for p in service.iter(f"{VOT}PARAM") if p.get("name") == "BAND")
    lo = float(band.find(f"{VOT}VALUES/{VOT}MIN").get("value"))
    hi = float(band.find(f"{VOT}VALUES/{VOT}MAX").get("value"))
    assert (lo, hi) == pytest.approx((C / EDGES[-1], C / EDGES[0]))

    links = adhoc.DatalinkResults(parse(io.BytesIO(resp.content)))
    proc = links.get_first_proc()
    query = adhoc.SodaQuery.from_resource(proc, links.get_adhocservice_by_id(proc.service_def))
    query.band = (C / 1000e6, C / 900e6) * u.m
    query.pol.add("U")
    sent = await client.get("/soda/sync", params=[("ID", query["ID"]), ("BAND", query["BAND"])]
                            + [("POL", s) for s in query["POL"]], headers=auth_headers)
    assert sent.status_code == 200, sent.text
    data, header, _ = _open(sent.content)
    assert stokes_states(header) == ["U"] and data[0, :, 0, 0].tolist() == [31.0, 32.0, 33.0]


@pytest.mark.parametrize("matrix", ["pc-scale", "pc-coupled", "cd", "cd-without-spectral-diagonal"])
def test_the_rewritten_axis_ignores_the_old_linear_transformation(tmp_path, matrix):
    path = _subband_cube(tmp_path / "c.fits")
    with fits.open(path, mode="update") as hdul:
        h = hdul[0].header
        if matrix == "pc-scale":
            h["PC3_3"] = 2.0
        elif matrix == "pc-coupled":
            h["PC3_4"] = 0.5
        else:
            for i in (1, 2, 3, 4):
                if matrix == "cd" or i != 3:
                    h[f"CD{i}_{i}"] = h[f"CDELT{i}"]
    _, header, _ = _open(make_cutout(str(path), band=_band(1000e6, 900e6)).fits_bytes)
    spectral = WCS(header).spectral.pixel_to_world_values(np.arange(3))
    np.testing.assert_allclose(spectral, FREQ[:3], atol=1e-3)
    assert _freq_keys(header) == list(FREQ[:3])


def test_a_freq_axis_cutout_is_cut_again_by_subband(tmp_path):
    path = _subband_cube(tmp_path / "c.fits")
    linear = tmp_path / "linear.fits"
    linear.write_bytes(make_cutout(str(path), band=_band(1000e6, 900e6)).fits_bytes)
    assert fits.getheader(linear)["CTYPE3"] == "FREQ"
    assert describe(str(linear)).band_m == pytest.approx((C / EDGES[3], C / EDGES[0]))
    with pytest.raises(CutoutNoOverlap):                 # between centres 908 and 952 MHz
        make_cutout(str(linear), band=_band(930e6, 920e6))
    again = make_cutout(str(linear), band=_band(960e6, 940e6))
    data, header, _ = _open(again.fits_bytes)
    assert again.applied["subbands"] == [2] and data[0, :, 0, 0].tolist() == [12.0]
    assert header["NSPEC"] == 1 and _freq_keys(header) == [FREQ[1]] and header["CRVAL3"] == FREQ[1]
