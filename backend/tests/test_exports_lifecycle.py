"""End-to-end tests for the Workspace Exports lifecycle:

  * helper safety  (make_workspace_path / resolve_workspace_path)
  * GET    /v1/exports/list
  * GET    /v1/exports/download
  * DELETE /v1/exports/{filename}

The workspace dir is monkey-patched to a per-test tmp_path so the tests
never touch the user's real ``~/.visivo/exports/``.
"""
from __future__ import annotations

from pathlib import Path

import pytest
import pytest_asyncio
from httpx import AsyncClient


# ── Fixture: redirect the workspace to a tmpdir for every test ──────────────

@pytest_asyncio.fixture
async def isolated_workspace(monkeypatch, tmp_path) -> Path:
    """Point _EXPORTS_DIR and the helper accessors at a tmpdir so tests
    operate on a clean, empty workspace."""
    from app import dependencies
    workspace = tmp_path / "exports"
    workspace.mkdir()
    monkeypatch.setattr(dependencies, "_EXPORTS_DIR", workspace)
    # Some callers import the accessor directly — patch it too so they
    # return the same redirected directory regardless of import order.
    monkeypatch.setattr(dependencies, "get_exports_dir", lambda: workspace)
    yield workspace


# ── make_workspace_path: collision handling + sanitisation ──────────────────

class TestMakeWorkspacePath:
    def test_basic_basename(self, isolated_workspace):
        from app.dependencies import make_workspace_path
        p = make_workspace_path("cube.fits")
        assert p == isolated_workspace / "cube.fits"

    def test_collision_appends_numeric_suffix(self, isolated_workspace):
        from app.dependencies import make_workspace_path
        (isolated_workspace / "cube.fits").touch()
        p = make_workspace_path("cube.fits")
        assert p.name == "cube_1.fits"
        p.touch()
        assert make_workspace_path("cube.fits").name == "cube_2.fits"

    def test_no_extension_defaults_to_fits(self, isolated_workspace):
        from app.dependencies import make_workspace_path
        assert make_workspace_path("noext").name == "noext.fits"

    def test_path_traversal_stripped(self, isolated_workspace):
        """Slashes / leading dots in the basename must NOT escape the
        workspace; the result must always live under it."""
        from app.dependencies import make_workspace_path
        p = make_workspace_path("../outside.fits")
        assert p.parent == isolated_workspace
        assert p.name == "outside.fits"
        p2 = make_workspace_path("subdir/inside.fits")
        assert p2.parent == isolated_workspace
        assert p2.name == "inside.fits"

    def test_empty_basename_falls_back(self, isolated_workspace):
        from app.dependencies import make_workspace_path
        assert make_workspace_path("").name == "export.fits"
        assert make_workspace_path("   ").name == "export.fits"

    def test_without_reserve_the_name_is_only_promised(self, isolated_workspace):
        """The default must not litter: a caller that writes nothing leaves nothing."""
        from app.dependencies import make_workspace_path
        p = make_workspace_path("promised.fits")
        assert not p.exists()

    def test_the_name_is_reserved_so_two_exports_cannot_collide(self, isolated_workspace):
        """Two callers asking at once must not be handed the same path.

        The name is claimed with an empty file, atomically, instead of being
        checked and returned — otherwise both exports write to it and the one
        that finishes second silently replaces the first, with both registered
        dataset ids pointing at one file.
        """
        from app.dependencies import make_workspace_path
        first = make_workspace_path("map.fits", reserve=True)
        second = make_workspace_path("map.fits", reserve=True)
        assert first.name == "map.fits"
        assert second.name == "map_1.fits"
        assert first.exists() and second.exists()   # reserved, not merely promised


# ── resolve_workspace_path: refuses traversal ───────────────────────────────

class TestResolveWorkspacePath:
    def test_simple_basename(self, isolated_workspace):
        from app.dependencies import resolve_workspace_path
        assert resolve_workspace_path("a.fits") == isolated_workspace / "a.fits"

    def test_traversal_rejected(self, isolated_workspace):
        from app.dependencies import resolve_workspace_path
        with pytest.raises(ValueError):
            resolve_workspace_path("../../etc/passwd")
        with pytest.raises(ValueError):
            resolve_workspace_path("sub/a.fits")
        with pytest.raises(ValueError):
            resolve_workspace_path(".")
        with pytest.raises(ValueError):
            resolve_workspace_path("")


# ── /v1/exports/list ────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_exports_list_empty(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    resp = await client.get("/v1/exports/list", headers=auth_headers)
    assert resp.status_code == 200
    body = resp.json()
    assert body["valid"] is True
    assert body["entries"] == []
    assert body["workspace_dir"] == str(isolated_workspace)


@pytest.mark.asyncio
async def test_exports_list_populated_and_sorted(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    """Listing reports each regular file with size + mtime, sorted
    newest-first."""
    import os
    import time
    a = isolated_workspace / "older.fits"; a.write_bytes(b"AAAA")
    time.sleep(0.05)
    b = isolated_workspace / "newer.fits"; b.write_bytes(b"BBBBBB")
    # Touch with explicit mtimes so the ordering check is deterministic
    os.utime(a, (a.stat().st_atime, 1_000_000_000))
    os.utime(b, (b.stat().st_atime, 2_000_000_000))

    resp = await client.get("/v1/exports/list", headers=auth_headers)
    assert resp.status_code == 200
    entries = resp.json()["entries"]
    names = [e["filename"] for e in entries]
    assert names == ["newer.fits", "older.fits"]
    assert entries[0]["size"] == 6
    assert entries[1]["size"] == 4


# ── /v1/exports/download ────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_exports_download_streams_bytes(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    f = isolated_workspace / "moment.fits"
    payload = b"FITS PAYLOAD CONTENT"
    f.write_bytes(payload)
    resp = await client.get(
        "/v1/exports/download",
        params={"filename": "moment.fits"},
        headers=auth_headers,
    )
    assert resp.status_code == 200
    assert resp.content == payload
    assert resp.headers["content-type"] == "application/fits"


@pytest.mark.asyncio
async def test_exports_download_traversal_rejected(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    """A relative-path / absolute-path filename must NOT be honoured."""
    resp = await client.get(
        "/v1/exports/download",
        params={"filename": "../sensitive.fits"},
        headers=auth_headers,
    )
    assert resp.status_code == 400


@pytest.mark.asyncio
async def test_exports_download_missing_returns_404(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    resp = await client.get(
        "/v1/exports/download",
        params={"filename": "doesnotexist.fits"},
        headers=auth_headers,
    )
    assert resp.status_code == 404


# ── DELETE /v1/exports/{name} ───────────────────────────────────────────────

@pytest.mark.asyncio
async def test_exports_delete_removes_file(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    f = isolated_workspace / "todelete.fits"
    f.write_bytes(b"x")
    assert f.exists()
    resp = await client.delete(
        "/v1/exports/todelete.fits", headers=auth_headers,
    )
    assert resp.status_code == 200
    body = resp.json()
    assert body["valid"] is True
    assert body["deleted_filename"] == "todelete.fits"
    assert not f.exists()


@pytest.mark.asyncio
async def test_exports_delete_idempotent_on_missing(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    """Deleting a non-existent file returns a 200 with valid=false (no
    HTTP exception) so the client can keep its UI flow consistent."""
    resp = await client.delete(
        "/v1/exports/never_existed.fits", headers=auth_headers,
    )
    assert resp.status_code == 200
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_exports_delete_traversal_rejected(
    client: AsyncClient, auth_headers: dict, isolated_workspace,
):
    """Path-segment filenames are sanitised → traversal cannot happen."""
    resp = await client.delete(
        "/v1/exports/..%2Fdangerous.fits", headers=auth_headers,
    )
    # FastAPI may URL-decode and route this differently; either 400 (our
    # validator refuses) or 404 (file genuinely missing) is acceptable —
    # the absolute requirement is that nothing OUTSIDE the workspace gets
    # touched.
    assert resp.status_code in (400, 404)


# ── The generic 2-D map export ───────────────────────────────────────────────
#
# The path every client-computed map leaves by: polarised intensity, position
# angle, Faraday depth, line-width. Unlike the moment exporter it does not
# recompute anything — the pixels come up with the request — so what has to be
# right is the HEADER it builds around them.

class TestSaveMap2d:

    @staticmethod
    def _source_cube(tmp_path: Path) -> str:
        """A small cube with a celestial WCS plus spectral and Stokes axes."""
        import numpy as np
        from astropy.io import fits

        data = np.zeros((1, 4, 20, 30), dtype=np.float32)
        hdu = fits.PrimaryHDU(data)
        h = hdu.header
        h["CTYPE1"], h["CRVAL1"], h["CRPIX1"], h["CDELT1"] = "GLON-SIN", 0.0, 15.0, -4.166667e-4
        h["CTYPE2"], h["CRVAL2"], h["CRPIX2"], h["CDELT2"] = "GLAT-SIN", 2.5, 10.0, 4.166667e-4
        h["CTYPE3"], h["CRVAL3"], h["CRPIX3"], h["CDELT3"] = "FREQ", 1.4e9, 1.0, 1e6
        h["CTYPE4"], h["CRVAL4"], h["CRPIX4"], h["CDELT4"] = "STOKES", 1.0, 1.0, 1.0
        h["BUNIT"], h["OBJECT"], h["TELESCOP"] = "Jy/beam", "TESTFIELD", "MeerKAT"
        path = tmp_path / "source_cube.fits"
        hdu.writeto(path, overwrite=True)
        return str(path)

    @staticmethod
    def _map(h: int, w: int):
        import base64
        import numpy as np
        pixels = np.arange(h * w, dtype="<f4").reshape(h, w)
        return base64.b64encode(pixels.tobytes()).decode(), pixels

    def test_non_ascii_units_and_provenance_do_not_break_the_export(self, tmp_path):
        """The units these maps actually carry are not ASCII.

        A FITS card holds printable ASCII only, and astropy refuses the whole
        header otherwise — so "rad m⁻²" and a HISTORY line containing "φ" used
        to fail the export with a 422 for every polarimetry product, which is
        every product this path exists for. The earlier tests passed because
        they used ASCII substitutes.
        """
        from astropy.io import fits
        from app.compute import worker_save_map_2d

        src = self._source_cube(tmp_path)
        payload, _ = self._map(5, 6)
        out = str(tmp_path / "unicode.fits")
        worker_save_map_2d(
            src, out, payload, 6, 5, "rad m\u207b\u00b2",
            ["Peak |F(\u03c6)| over \u03c6 -80\u202680 rad m\u207b\u00b2",
             "\u03c3\u00b2_QU = 1.2e-6"], None)

        with fits.open(out) as hdul:
            header = hdul[0].header
        assert header["BUNIT"] == "rad m-2"
        history = "\n".join(header["HISTORY"])
        assert "phi" in history
        assert "sigma2_QU" in history
        assert history.isascii()

    def test_oversized_map_is_refused(self, tmp_path):
        from app.compute import worker_save_map_2d
        src = self._source_cube(tmp_path)
        with pytest.raises(ValueError, match="pixel limit"):
            worker_save_map_2d(src, str(tmp_path / "huge.fits"), "", 100000, 100000,
                               "", [], None)

    def test_payload_larger_than_the_declared_shape_is_refused_before_decoding(self, tmp_path):
        """A caller announcing 1x1 and sending megabytes must not allocate them."""
        from app.compute import worker_save_map_2d
        src = self._source_cube(tmp_path)
        big, _ = self._map(200, 200)
        with pytest.raises(ValueError, match="larger than"):
            worker_save_map_2d(src, str(tmp_path / "lie.fits"), big, 1, 1, "", [], None)

    def test_writes_a_2d_fits_with_the_sources_celestial_wcs(self, tmp_path):
        from astropy.io import fits
        from app.compute import worker_save_map_2d

        src = self._source_cube(tmp_path)
        payload, pixels = self._map(20, 30)
        out = str(tmp_path / "derived.fits")
        worker_save_map_2d(src, out, payload, 30, 20, "rad m-2",
                           ["RM synthesis, phi -80..80"], None)

        with fits.open(out) as hdul:
            header, data = hdul[0].header, hdul[0].data
        # NAXIS=2: the spectral and Stokes axes are projected away, not left
        # degenerate, so the image viewer opens it without a stub axis.
        assert header["NAXIS"] == 2
        assert (header["NAXIS1"], header["NAXIS2"]) == (30, 20)
        assert header["CTYPE1"].startswith("GLON")
        assert header["CTYPE2"].startswith("GLAT")
        assert "CTYPE3" not in header
        # The derived unit wins over the source's — a Faraday depth is not Jy/beam.
        assert header["BUNIT"] == "rad m-2"
        assert header["OBJECT"] == "TESTFIELD"
        assert "RM synthesis, phi -80..80" in "\n".join(header["HISTORY"])
        assert data.shape == (20, 30)
        assert data[0, 1] == pytest.approx(1.0)

    def test_region_origin_shifts_crpix_to_the_right_sky(self, tmp_path):
        """A cropped map must not claim the whole field's astrometry."""
        from astropy.io import fits
        from astropy.wcs import WCS
        from app.compute import worker_save_map_2d

        src = self._source_cube(tmp_path)
        payload, _ = self._map(5, 6)
        out = str(tmp_path / "cropped.fits")
        worker_save_map_2d(src, out, payload, 6, 5, "", [], (8, 3))

        with fits.open(src) as hdul:
            src_wcs = WCS(hdul[0].header, relax=True).celestial
        with fits.open(out) as hdul:
            out_wcs = WCS(hdul[0].header)
        # Pixel (0,0) of the crop is pixel (8,3) of the source, on the sky.
        assert out_wcs.wcs_pix2world([[0, 0]], 0) == pytest.approx(
            src_wcs.wcs_pix2world([[8, 3]], 0), abs=1e-9)

    def test_payload_that_does_not_match_the_shape_is_refused(self, tmp_path):
        """Silently writing a reshaped or truncated map would be worse."""
        from app.compute import worker_save_map_2d

        src = self._source_cube(tmp_path)
        payload, _ = self._map(5, 6)
        with pytest.raises(ValueError, match="bytes"):
            worker_save_map_2d(src, str(tmp_path / "bad.fits"), payload, 7, 5, "", [], None)

    def test_sip_distortion_survives_a_4d_source(self, tmp_path):
        """A SIP cube cannot be built as an N-D WCS at all.

        astropy refuses (SIP is 2-D only), and the plain `.celestial` path
        therefore fell into the error branch and exported a map with NO
        astrometry — silently, because the file still looked fine. Selecting
        two axes up front works and keeps the distortion coefficients.
        """
        import numpy as np
        from astropy.io import fits
        from app.compute import worker_save_map_2d

        data = np.zeros((1, 2, 10, 10), dtype=np.float32)
        hdu = fits.PrimaryHDU(data)
        h = hdu.header
        h["CTYPE1"], h["CRVAL1"], h["CRPIX1"], h["CDELT1"] = "RA---TAN-SIP", 83.6, 5.0, -1e-4
        h["CTYPE2"], h["CRVAL2"], h["CRPIX2"], h["CDELT2"] = "DEC--TAN-SIP", 22.0, 5.0, 1e-4
        h["CTYPE3"], h["CRVAL3"], h["CRPIX3"], h["CDELT3"] = "FREQ", 1.4e9, 1.0, 1e6
        h["CTYPE4"], h["CRVAL4"], h["CRPIX4"], h["CDELT4"] = "STOKES", 1.0, 1.0, 1.0
        h["A_ORDER"], h["A_2_0"], h["B_ORDER"], h["B_0_2"] = 2, 1e-6, 2, 1e-6
        src = tmp_path / "sip_cube.fits"
        hdu.writeto(src, overwrite=True)

        payload, _ = self._map(10, 10)
        out = str(tmp_path / "sip_map.fits")
        worker_save_map_2d(str(src), out, payload, 10, 10, "", [], None)

        with fits.open(out) as hdul:
            header = hdul[0].header
        assert header["CTYPE1"].startswith("RA---TAN")
        assert "A_2_0" in header, "the distortion coefficients must survive the export"

    def test_a_source_with_no_sky_axes_says_so_in_the_file(self, tmp_path):
        """has_celestial False raises nothing, so this used to export silently."""
        import numpy as np
        from astropy.io import fits
        from app.compute import worker_save_map_2d

        hdu = fits.PrimaryHDU(np.zeros((4, 6), dtype=np.float32))
        hdu.header["CTYPE1"], hdu.header["CTYPE2"] = "FREQ", "STOKES"
        src = tmp_path / "no_sky.fits"
        hdu.writeto(src, overwrite=True)

        payload, _ = self._map(5, 6)
        out = str(tmp_path / "no_sky_map.fits")
        worker_save_map_2d(str(src), out, payload, 6, 5, "", [], None)

        with fits.open(out) as hdul:
            history = "\n".join(hdul[0].header["HISTORY"])
        assert "no celestial axes" in history

    def test_a_source_without_a_wcs_still_produces_a_file(self, tmp_path):
        """No celestial WCS is a degraded export, not a failed one."""
        import numpy as np
        from astropy.io import fits
        from app.compute import worker_save_map_2d

        plain = tmp_path / "no_wcs.fits"
        fits.PrimaryHDU(np.zeros((5, 6), dtype=np.float32)).writeto(plain, overwrite=True)
        payload, _ = self._map(5, 6)
        out = str(tmp_path / "from_plain.fits")
        result = worker_save_map_2d(str(plain), out, payload, 6, 5, "K", [], None)
        assert result["width"] == 6
        with fits.open(out) as hdul:
            assert hdul[0].header["BUNIT"] == "K"
