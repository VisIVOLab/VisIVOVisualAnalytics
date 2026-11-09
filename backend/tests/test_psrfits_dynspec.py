"""PSRFITS conversion and dynspec folding on synthetic files.

Each file built here is a minimal valid PSRFITS (primary header with
OBS_MODE, SUBINT table with the definition's columns and keywords), so the
tests read the layout the way a real PSRCHIVE / PRESTO / SKA PST file
declares it, not the way the converter used to guess it.
"""
from __future__ import annotations

import ast
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app.dynspec_tools import (
    worker_dynspec_dedisperse, worker_dynspec_pulse_profile,
)
from app.psrfits import convert_psrfits_to_sidecar_fits


def _write_psrfits(path: Path, *, mode: str, data: np.ndarray, data_format: str,
                   tdim: str | None, nbin: int, nchan: int, npol: int,
                   nsblk: int, nbits: int, tbin: float = 1e-3,
                   scl: np.ndarray | None = None, offs: np.ndarray | None = None,
                   wts: np.ndarray | None = None, pol_type: str = "INTEN",
                   tsubint: float | None = None, zero_off: float | None = None,
                   freqs_mhz: np.ndarray | None = None,
                   offs_sub: np.ndarray | None = None,
                   signint: int | None = None) -> Path:
    nsub = data.shape[0]
    if tsubint is None:  # a search row lasts NSBLK samples
        tsubint = 10.0 if mode != "SEARCH" else nsblk * tbin
    if offs_sub is None:
        offs_sub = (np.arange(nsub) + 0.5) * tsubint
    if freqs_mhz is None:
        freqs_mhz = 1400.0 + np.arange(nchan, dtype=np.float64)
    if scl is None:
        scl = np.ones((nsub, nchan * npol), np.float32)
    if offs is None:
        offs = np.zeros((nsub, nchan * npol), np.float32)
    if wts is None:
        wts = np.ones((nsub, nchan), np.float32)
    primary = fits.PrimaryHDU()
    primary.header["HDRVER"] = "6.1"
    primary.header["OBS_MODE"] = mode
    primary.header["TELESCOP"] = "SYNTH"
    primary.header["OBSFREQ"] = float(freqs_mhz.mean())
    primary.header["OBSBW"] = float(nchan)
    cols = [
        fits.Column(name="TSUBINT", format="1D", unit="s",
                    array=np.full(nsub, tsubint)),
        fits.Column(name="OFFS_SUB", format="1D", unit="s",
                    array=offs_sub),
        fits.Column(name="DAT_FREQ", format=f"{nchan}D", unit="MHz",
                    array=np.tile(freqs_mhz, (nsub, 1))),
        fits.Column(name="DAT_WTS", format=f"{nchan}E", array=wts),
        fits.Column(name="DAT_OFFS", format=f"{nchan * npol}E", array=offs),
        fits.Column(name="DAT_SCL", format=f"{nchan * npol}E", array=scl),
        fits.Column(name="DATA", format=data_format, dim=tdim, array=data),
    ]
    subint = fits.BinTableHDU.from_columns(cols, name="SUBINT")
    h = subint.header
    h["INT_TYPE"] = "TIME"
    h["NPOL"] = npol
    h["POL_TYPE"] = pol_type
    h["TBIN"] = tbin
    h["NBIN"] = nbin
    h["NCHAN"] = nchan
    h["CHAN_BW"] = 1.0
    h["NSBLK"] = nsblk
    h["NBITS"] = nbits
    if zero_off is not None:
        h["ZERO_OFF"] = zero_off
    if signint is not None:
        h["SIGNINT"] = signint
    fits.HDUList([primary, subint]).writeto(path, overwrite=True)
    return path


def _fold_file(path: Path, data: np.ndarray, **kw) -> Path:
    """``data`` is ``(nsub, npol, nchan, nbin)``, the numpy view of the
    definition's ``(NBIN, NCHAN, NPOL)`` TDIM."""
    nsub, npol, nchan, nbin = data.shape
    return _write_psrfits(
        path, mode="PSR", data=data.astype(np.int16),
        data_format=f"{nbin * nchan * npol}I", tdim=f"({nbin},{nchan},{npol})",
        nbin=nbin, nchan=nchan, npol=npol, nsblk=1, nbits=16, **kw)


def _search_file(path: Path, data: np.ndarray, **kw) -> Path:
    """``data`` is ``(nsub, nsblk, npol, nchan)`` 8-bit samples."""
    nsub, nsblk, npol, nchan = data.shape
    return _write_psrfits(
        path, mode="SEARCH", data=data.astype(np.uint8),
        data_format=f"{nsblk * npol * nchan}B", tdim=f"({nchan},{npol},{nsblk})",
        nbin=1, nchan=nchan, npol=npol, nsblk=nsblk, nbits=8, **kw)


def _read_sidecar(src: Path) -> tuple[np.ndarray, fits.Header]:
    out = convert_psrfits_to_sidecar_fits(src)
    with fits.open(out) as hdul:
        return np.asarray(hdul[0].data), hdul[0].header.copy()


# ── Bug 1: the header is written at all, and only in ASCII ─────────────────

class TestSidecarHeaderAscii:
    def test_fold_file_converts_and_header_is_ascii(self, tmp_path):
        data = np.full((2, 1, 3, 4), 10)
        src = _fold_file(tmp_path / "fold.fits", data)
        out = convert_psrfits_to_sidecar_fits(src)
        raw = Path(out).read_bytes()
        header_block = raw[:raw.index(b"END" + b" " * 77) + 80]
        header_block.decode("ascii")  # raises on any non-ASCII byte
        assert fits.getheader(out)["CTYPE1"] == "TIME"

    def test_hdf5_converter_header_is_ascii(self, tmp_path):
        h5py = pytest.importorskip("h5py")
        from app.hdf5_dynspec import convert_hdf5_to_sidecar_fits
        src = tmp_path / "dyn.h5"
        with h5py.File(src, "w") as h5:
            h5.create_dataset("dynspec", data=np.ones((4, 8), np.float32))
            h5.attrs["TSAMP"] = 0.01
        out = convert_hdf5_to_sidecar_fits(src)
        hdr = fits.getheader(out)
        assert hdr.tostring().isascii()

    def test_no_literal_non_ascii_text_reaches_a_fits_header(self):
        """Static guard over the whole backend: a string literal assigned to
        a header card, or passed to add_history/add_comment/set/append on a
        header, must be ASCII, or astropy refuses to write the file."""
        app_dir = Path(__file__).resolve().parents[1] / "app"

        def headerish(node: ast.AST) -> bool:
            text = ast.unparse(node).lower()
            return "hdr" in text or "header" in text

        offenders = []
        for path in app_dir.rglob("*.py"):
            tree = ast.parse(path.read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                values: list[ast.AST] = []
                if isinstance(node, ast.Assign):
                    if any(isinstance(t, ast.Subscript) and headerish(t.value)
                           for t in node.targets):
                        values.append(node.value)
                elif (isinstance(node, ast.Call)
                      and isinstance(node.func, ast.Attribute)
                      and node.func.attr in ("add_history", "add_comment",
                                             "set", "append", "insert")
                      and headerish(node.func.value)):
                    values.extend(node.args)
                    values.extend(k.value for k in node.keywords)
                for value in values:
                    for sub in ast.walk(value):
                        if (isinstance(sub, ast.Constant)
                                and isinstance(sub.value, str)
                                and not sub.value.isascii()):
                            offenders.append(
                                f"{path.relative_to(app_dir)}:{sub.lineno}: "
                                f"{sub.value!r}")
        assert offenders == []


# ── Bug 2: DAT_SCL / DAT_OFFS / DAT_WTS ─────────────────────────────────────

class TestScaleOffsetWeights:
    def test_sample_10_scale_2_offset_100_is_120(self, tmp_path):
        nsub, npol, nchan, nbin = 2, 1, 3, 4
        data = np.full((nsub, npol, nchan, nbin), 10)
        src = _fold_file(
            tmp_path / "f.fits", data,
            scl=np.full((nsub, nchan * npol), 2.0, np.float32),
            offs=np.full((nsub, nchan * npol), 100.0, np.float32))
        image, _ = _read_sidecar(src)
        assert image.shape == (nchan, nsub)
        np.testing.assert_allclose(image, 120.0)

    def test_scale_is_per_row_channel_and_polarisation(self, tmp_path):
        """DAT_SCL holds NCHAN*NPOL values with the channel varying fastest:
        index pol*NCHAN + chan. With POL_TYPE=AABB, I = AA + BB."""
        nsub, npol, nchan, nbin = 2, 2, 3, 2
        data = np.ones((nsub, npol, nchan, nbin))
        scl = np.zeros((nsub, nchan * npol), np.float32)
        for row in range(nsub):
            for pol in range(npol):
                for chan in range(nchan):
                    scl[row, pol * nchan + chan] = (row + 1) * (10 ** pol) * (chan + 1)
        src = _fold_file(tmp_path / "f.fits", data, scl=scl, pol_type="AABB")
        image, _ = _read_sidecar(src)
        expected = np.array([[(row + 1) * 11 * (chan + 1) for row in range(nsub)]
                             for chan in range(nchan)], dtype=np.float32)
        np.testing.assert_allclose(image, expected)

    def test_zero_weight_channel_is_nan(self, tmp_path):
        nsub, npol, nchan, nbin = 3, 1, 4, 2
        data = np.full((nsub, npol, nchan, nbin), 5)
        wts = np.ones((nsub, nchan), np.float32)
        wts[:, 2] = 0.0         # channel 2 flagged in every row
        wts[1, 0] = 0.0         # channel 0 flagged in row 1 only
        wts[0, 1] = 0.25        # a non-zero weight is not a rescale
        src = _fold_file(tmp_path / "f.fits", data, wts=wts)
        image, header = _read_sidecar(src)
        assert np.isnan(image[2]).all()
        assert np.isnan(image[0, 1]) and np.isfinite(image[0, [0, 2]]).all()
        assert image[1, 0] == 5.0
        assert "1 channel(s) have zero weight" in "".join(header["HISTORY"])

    def test_search_mode_applies_zero_off(self, tmp_path):
        nsub, nsblk, npol, nchan = 1, 4, 1, 2
        data = np.full((nsub, nsblk, npol, nchan), 130)
        src = _search_file(
            tmp_path / "s.fits", data, zero_off=128.0,
            scl=np.full((nsub, nchan), 3.0, np.float32),
            offs=np.full((nsub, nchan), 1.0, np.float32))
        image, _ = _read_sidecar(src)
        np.testing.assert_allclose(image, (130 - 128) * 3.0 + 1.0)


# ── Bug 3: layout from the headers ──────────────────────────────────────────

class TestSearchModeLayout:
    def test_reviewer_case_2_subints_16_samples_1_pol_3_channels(self, tmp_path):
        nsub, nsblk, npol, nchan = 2, 16, 1, 3
        data = np.zeros((nsub, nsblk, npol, nchan))
        for row in range(nsub):
            for s in range(nsblk):
                for chan in range(nchan):
                    data[row, s, 0, chan] = row * 100 + s + 50 * chan
        src = _search_file(tmp_path / "s.fits", data, tbin=64e-6)
        image, header = _read_sidecar(src)
        assert image.shape == (nchan, nsub * nsblk)
        assert header["CDELT1"] == pytest.approx(64e-6)
        # Sample centres from the observation start: row 0 covers
        # 0..1.024 ms, so the first centre is 32 us and sample 16 (the
        # first of row 1) is at 1.056 ms.
        assert header["CRVAL1"] == pytest.approx(32e-6)
        assert header["CRVAL1"] + 16 * header["CDELT1"] == pytest.approx(1.056e-3)
        t = np.arange(nsub * nsblk)
        for chan in range(nchan):
            np.testing.assert_allclose(
                image[chan], (t // nsblk) * 100 + t % nsblk + 50 * chan)

    def test_equal_axis_lengths_follow_the_header(self, tmp_path):
        """2 samples, 1 pol, 2 channels: the row shape (2, 1, 2) also reads
        as a folded (npol=2, nchan=1, nbin=2) row; OBS_MODE decides."""
        data = np.array([[[[1, 2]], [[3, 4]]]])  # (1 sub, 2 samples, 1 pol, 2 chan)
        src = _search_file(tmp_path / "s.fits", data)
        image, _ = _read_sidecar(src)
        np.testing.assert_allclose(image, [[1, 3], [2, 4]])

    def test_fold_with_equal_npol_nchan_nbin(self, tmp_path):
        nsub, npol, nchan, nbin = 1, 2, 2, 2
        data = np.zeros((nsub, npol, nchan, nbin))
        data[0, 0, 0] = [1, 3]   # pol 0 (I), chan 0 -> mean 2
        data[0, 0, 1] = [5, 7]   # pol 0 (I), chan 1 -> mean 6
        data[0, 1] = 1000        # pol 1 (Q) must not leak in
        src = _fold_file(tmp_path / "f.fits", data, pol_type="IQUV")
        image, header = _read_sidecar(src)
        np.testing.assert_allclose(image, [[2.0], [6.0]])
        assert header["CDELT1"] == pytest.approx(10.0)  # TSUBINT

    def test_four_bit_search_samples_are_unpacked(self, tmp_path):
        nsub, nsblk, npol, nchan = 1, 4, 1, 2
        samples = np.arange(nsblk * npol * nchan) % 16  # 0..7
        packed = (samples[0::2] << 4) | samples[1::2]   # earlier sample high
        src = _write_psrfits(
            tmp_path / "s4.fits", mode="SEARCH",
            data=packed.astype(np.uint8).reshape(nsub, -1),
            data_format=f"{packed.size}B", tdim=None, nbin=1, nchan=nchan,
            npol=npol, nsblk=nsblk, nbits=4)
        image, _ = _read_sidecar(src)
        np.testing.assert_allclose(image, samples.reshape(nsblk, nchan).T)

    def test_signed_samples_follow_signint(self, tmp_path):
        nsub, nsblk, npol, nchan = 1, 2, 1, 1
        data = np.array([255, 128]).reshape(nsub, nsblk, npol, nchan)
        src = _search_file(tmp_path / "s8.fits", data, signint=1)
        image, _ = _read_sidecar(src)
        np.testing.assert_allclose(image, [[-1, -128]])
        packed = np.array([(0xF << 4) | 0x7], np.uint8).reshape(1, 1)  # -1, 7
        src = _write_psrfits(
            tmp_path / "s4.fits", mode="SEARCH", data=packed,
            data_format="1B", tdim=None, nbin=1, nchan=1, npol=1, nsblk=2,
            nbits=4, signint=1)
        image, _ = _read_sidecar(src)
        np.testing.assert_allclose(image, [[-1, 7]])

    def test_dropped_rows_leave_nan_columns(self, tmp_path):
        """Rows centred at 5 s and 25 s of 10 s each: the gap is a NaN
        column, not a shift of the second row to 15 s."""
        data = np.full((2, 1, 1, 2), 3)
        src = _fold_file(tmp_path / "f.fits", data,
                         offs_sub=np.array([5.0, 25.0]))
        image, header = _read_sidecar(src)
        assert image.shape == (1, 3)
        assert header["CRVAL1"] == pytest.approx(5.0)
        assert header["CDELT1"] == pytest.approx(10.0)
        assert image[0, 0] == 3 and np.isnan(image[0, 1]) and image[0, 2] == 3
        assert "1 missing column(s) left NaN" in "".join(header["HISTORY"])

    def test_irregular_rows_are_concatenated_and_said_so(self, tmp_path):
        data = np.full((3, 1, 1, 2), 3)
        src = _fold_file(tmp_path / "f.fits", data,
                         offs_sub=np.array([5.0, 15.0, 28.0]))
        image, header = _read_sidecar(src)
        assert image.shape == (1, 3)
        assert "OFFS_SUB irregular" in "".join(header["HISTORY"])

    def test_gap_limit_boundary(self, tmp_path):
        """Two 10 s rows may span at most 4 x 2 = 8 columns: centres 5 s and
        75 s (8 columns) keep their gap, 5 s and 85 s (9) are joined."""
        data = np.full((2, 1, 1, 2), 3)
        src = _fold_file(tmp_path / "in.fits", data,
                         offs_sub=np.array([5.0, 75.0]))
        image, _ = _read_sidecar(src)
        assert image.shape == (1, 8)
        src = _fold_file(tmp_path / "out.fits", data,
                         offs_sub=np.array([5.0, 85.0]))
        image, header = _read_sidecar(src)
        assert image.shape == (1, 2)
        assert "would need 9 columns for 2 samples" in "".join(header["HISTORY"])

    def test_minimal_fold_file_without_optional_columns(self, tmp_path):
        """Only DATA, one channel, TBIN='*': unit scale, no flags, times
        from 1 s sub-integrations, frequency from OBSFREQ/OBSBW."""
        primary = fits.PrimaryHDU()
        primary.header["OBS_MODE"] = "PSR"
        primary.header["OBSFREQ"] = 1400.0
        primary.header["OBSBW"] = 64.0
        subint = fits.BinTableHDU.from_columns(
            [fits.Column(name="DATA", format="4I", dim="(4,1,1)",
                         array=np.array([[[[1, 3, 5, 7]]], [[[2, 2, 2, 2]]]],
                                        np.int16))], name="SUBINT")
        for key, value in (("NPOL", 1), ("NCHAN", 1), ("NBIN", 4),
                           ("NSBLK", 1), ("NBITS", 1), ("TBIN", "*")):
            subint.header[key] = value
        src = tmp_path / "min.fits"
        fits.HDUList([primary, subint]).writeto(src)
        image, header = _read_sidecar(src)
        np.testing.assert_allclose(image, [[4.0, 2.0]])
        assert header["CDELT1"] == 1.0
        assert header["CRVAL2"] == pytest.approx(1400e6)
        assert header["CDELT2"] == pytest.approx(64e6)

    def test_nbits_must_match_the_stored_type(self, tmp_path):
        src = _write_psrfits(
            tmp_path / "s.fits", mode="SEARCH",
            data=np.zeros((1, 4), np.uint8), data_format="4B", tdim=None,
            nbin=1, nchan=2, npol=1, nsblk=2, nbits=16)
        with pytest.raises(ValueError, match="NBITS=16 but DATA is stored as 8-bit"):
            convert_psrfits_to_sidecar_fits(src)

    def test_twelve_bits_is_refused(self, tmp_path):
        src = _write_psrfits(
            tmp_path / "s.fits", mode="SEARCH",
            data=np.zeros((1, 4), np.int16), data_format="4I", tdim=None,
            nbin=1, nchan=2, npol=1, nsblk=2, nbits=12)
        with pytest.raises(ValueError, match="NBITS=12 is not supported"):
            convert_psrfits_to_sidecar_fits(src)

    def test_unsupported_bit_width_is_refused(self, tmp_path):
        src = _write_psrfits(
            tmp_path / "s3.fits", mode="SEARCH",
            data=np.zeros((1, 3), np.uint8), data_format="3B", tdim=None,
            nbin=1, nchan=2, npol=1, nsblk=4, nbits=3)
        with pytest.raises(ValueError, match="NBITS=3 is not supported"):
            convert_psrfits_to_sidecar_fits(src)

    def test_row_size_mismatch_is_refused(self, tmp_path):
        data = np.zeros((1, 16, 1, 3))
        src = _search_file(tmp_path / "s.fits", data)
        with fits.open(src, mode="update") as hdul:
            hdul["SUBINT"].header["NCHAN"] = 4
        with pytest.raises(ValueError, match="needs 64"):
            convert_psrfits_to_sidecar_fits(src)


# ── Bug 4: a decreasing time axis folds and dedisperses the same ────────────

def _write_dynspec(path: Path, data: np.ndarray, crval1: float, cdelt1: float,
                   f0_hz: float = 1.4e9, df_hz: float = 1e6) -> str:
    header = fits.Header()
    header["CTYPE1"], header["CUNIT1"] = "TIME", "s"
    header["CRPIX1"], header["CRVAL1"], header["CDELT1"] = 1.0, crval1, cdelt1
    header["CTYPE2"], header["CUNIT2"] = "FREQ", "Hz"
    header["CRPIX2"], header["CRVAL2"], header["CDELT2"] = 1.0, f0_hz, df_hz
    fits.PrimaryHDU(data=data.astype(np.float32), header=header).writeto(path)
    return str(path)


class TestTimeAxisOrder:
    def test_pulse_profile_does_not_depend_on_time_order(self, tmp_path):
        # 16 samples of 0.25 s centred mid-bin, period 1 s, pulse in the
        # first quarter of the period.
        n, dt = 16, 0.25
        times = (np.arange(n) + 0.5) * dt
        series = np.where(np.isclose(np.mod(times, 1.0), 0.125), 1.0, 0.0)
        data = np.tile(series, (2, 1))
        inc = _write_dynspec(tmp_path / "inc.fits", data, times[0], dt)
        dec = _write_dynspec(tmp_path / "dec.fits", data[:, ::-1], times[-1], -dt)
        p_inc = worker_dynspec_pulse_profile(inc, period_s=1.0, n_bins=4)
        p_dec = worker_dynspec_pulse_profile(dec, period_s=1.0, n_bins=4)
        assert p_inc["profile"] == [1.0, 0.0, 0.0, 0.0]
        assert p_dec["profile"] == p_inc["profile"]

    def test_dedispersion_does_not_depend_on_time_order(self, tmp_path):
        n, dt, dm = 128, 1e-3, 5.0
        freqs_hz = np.array([400e6, 500e6, 600e6])
        delays = 4.148808e3 * dm * (1 / (freqs_hz / 1e6) ** 2
                                    - 1 / (freqs_hz.max() / 1e6) ** 2)
        data = np.zeros((3, n))
        for c, d in enumerate(delays):
            data[c, 10 + int(round(d / dt))] = 1.0
        inc = _write_dynspec(tmp_path / "inc.fits", data, 0.0, dt, 400e6, 100e6)
        dec = _write_dynspec(tmp_path / "dec.fits", data[:, ::-1],
                             (n - 1) * dt, -dt, 400e6, 100e6)
        worker_dynspec_dedisperse(inc, str(tmp_path / "inc_dm.fits"), dm)
        worker_dynspec_dedisperse(dec, str(tmp_path / "dec_dm.fits"), dm)
        out_inc = fits.getdata(tmp_path / "inc_dm.fits")
        out_dec = fits.getdata(tmp_path / "dec_dm.fits")
        assert (out_inc[:, 10] == 1.0).all()
        np.testing.assert_array_equal(out_dec[:, ::-1], out_inc)

    def test_pulse_profile_dedisperses_a_reversed_file(self, tmp_path):
        n, dt, dm = 128, 1e-3, 5.0
        freqs_hz = np.array([400e6, 500e6, 600e6])
        delays = 4.148808e3 * dm * (1 / (freqs_hz / 1e6) ** 2
                                    - 1 / (freqs_hz.max() / 1e6) ** 2)
        data = np.zeros((3, n))
        for c, d in enumerate(delays):
            data[c, 8 + int(round(d / dt))] = 1.0
        # Sample centres half a sample off the bin edges, so the two
        # orders' float rounding cannot move a sample across a bin.
        inc = _write_dynspec(tmp_path / "inc.fits", data, 0.5 * dt, dt, 400e6, 100e6)
        dec = _write_dynspec(tmp_path / "dec.fits", data[:, ::-1],
                             (n - 0.5) * dt, -dt, 400e6, 100e6)
        kw = dict(period_s=0.016, n_bins=16, dm_pc_cm3=dm)
        p_inc = worker_dynspec_pulse_profile(inc, **kw)
        p_dec = worker_dynspec_pulse_profile(dec, **kw)
        assert p_inc["profile"] == p_dec["profile"]
        assert int(np.argmax(p_inc["profile"])) == 8


def test_search_psrfits_opens_as_dynspec(tmp_path):
    """The open path classifies a search-mode PSRFITS as a dynamic spectrum
    of NSUB*NSBLK time samples (it used to fail in the converter)."""
    from app.dependencies import _fits_metadata
    src = _search_file(tmp_path / "obs.fits", np.zeros((2, 16, 1, 3)))
    kind, geometry = _fits_metadata(src)
    assert kind == "dynspec"
    assert Path(geometry["resolved_path"]).name == "obs.fits.dynspec.fits"
    assert fits.getdata(geometry["resolved_path"]).shape == (3, 32)
