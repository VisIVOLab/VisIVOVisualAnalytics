"""IVOA SODA on the node: /soda/sync and the UWS jobs of /soda/async."""
import asyncio
import io
import xml.etree.ElementTree as ET

import pytest
from astropy.io import fits

UWS = "{http://www.ivoa.net/xml/UWS/v1.0}"
POS = "CIRCLE 83.82208 -5.39111 0.003"


def _id(path):
    return f"file://{path}"


async def _wait(client, url, headers, done=("COMPLETED", "ERROR", "ABORTED")):
    for _ in range(200):
        phase = (await client.get(url + "/phase", headers=headers)).text
        if phase in done:
            return phase
        await asyncio.sleep(0.05)
    raise AssertionError(f"job did not finish, last phase {phase}")


@pytest.mark.asyncio
async def test_sync_cutout_is_fits(client, auth_headers, synthetic_fits_cube):
    resp = await client.get("/soda/sync", params={"ID": _id(synthetic_fits_cube), "POS": POS},
                            headers=auth_headers)
    assert resp.status_code == 200, resp.text
    assert resp.headers["content-type"] == "application/fits"
    assert "pos" in resp.headers["x-soda-applied"]
    with fits.open(io.BytesIO(resp.content)) as hdul:
        assert hdul[0].data.shape[0] == 32 and hdul[0].data.shape[1] < 64


@pytest.mark.asyncio
async def test_sync_accepts_post_form_and_case_insensitive_names(client, auth_headers, synthetic_fits_cube):
    resp = await client.post("/soda/sync", data={"id": _id(synthetic_fits_cube), "pos": POS},
                             headers=auth_headers)
    assert resp.status_code == 200, resp.text


@pytest.mark.asyncio
@pytest.mark.parametrize("params, fragment", [
    ({}, "ID is required"),
    ({"POS": "CIRCLE 1 2"}, "CIRCLE"),
    ({"RESPONSEFORMAT": "text/csv"}, "RESPONSEFORMAT"),
    ({"FOO": "1"}, "not understood"),
    ({"OPERATION": "MOMENT", "MOMENT": "0"}, "/soda/async"),
])
async def test_sync_usage_errors_are_dali_text(client, auth_headers, synthetic_fits_cube, params, fragment):
    query = {"ID": _id(synthetic_fits_cube), **params} if params else {}
    resp = await client.get("/soda/sync", params=query, headers=auth_headers)
    assert resp.status_code == 400
    assert resp.headers["content-type"].startswith("text/plain")
    assert resp.text.startswith("UsageError:") and fragment in resp.text


@pytest.mark.asyncio
async def test_sync_refuses_ids_outside_the_data_roots(client, auth_headers, synthetic_fits_cube, tmp_path,
                                                       monkeypatch):
    (tmp_path / "jail").mkdir()
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(tmp_path / "jail"))
    resp = await client.get("/soda/sync", params={"ID": _id(synthetic_fits_cube)}, headers=auth_headers)
    assert resp.status_code == 400 and "cannot be served" in resp.text


@pytest.mark.asyncio
async def test_sync_needs_the_token(client, synthetic_fits_cube):
    resp = await client.get("/soda/sync", params={"ID": _id(synthetic_fits_cube)})
    assert resp.status_code == 401


@pytest.mark.asyncio
async def test_async_cutout_job_lifecycle(client, auth_headers, synthetic_fits_cube):
    created = await client.post("/soda/async", data={"ID": _id(synthetic_fits_cube), "POS": POS},
                                headers=auth_headers)
    assert created.status_code == 303
    job = created.headers["location"]
    assert (await client.get(job + "/phase", headers=auth_headers)).text == "PENDING"

    started = await client.post(job + "/phase", data={"PHASE": "RUN"}, headers=auth_headers)
    assert started.status_code == 303
    assert await _wait(client, job, auth_headers) == "COMPLETED"

    doc = ET.fromstring((await client.get(job, headers=auth_headers)).text)
    assert doc.find(f"{UWS}phase").text == "COMPLETED"
    result = doc.find(f"{UWS}results/{UWS}result")
    href = result.get("{http://www.w3.org/1999/xlink}href")
    fits_resp = await client.get(href, headers=auth_headers)
    assert fits_resp.status_code == 200
    with fits.open(io.BytesIO(fits_resp.content)) as hdul:
        assert hdul[0].data.ndim == 3

    as_json = (await client.get(job, headers={**auth_headers, "Accept": "application/json"})).json()
    assert as_json["phase"] == "COMPLETED" and as_json["output_path"].endswith(".fits")
    assert as_json["parameters"]["POS"] == POS


@pytest.mark.asyncio
async def test_async_moment_and_pv_products(client, auth_headers, synthetic_fits_cube):
    moment = await client.post("/soda/async", headers=auth_headers, data={
        "ID": _id(synthetic_fits_cube), "OPERATION": "MOMENT", "MOMENT": "0",
        "CHANNELS": "10 20", "PHASE": "RUN"})
    pv = await client.post("/soda/async", headers=auth_headers, data={
        "ID": _id(synthetic_fits_cube), "OPERATION": "PV",
        "PVPATH": "83.826 -5.391 83.818 -5.391", "PVWIDTH": "3", "PHASE": "RUN"})
    for created, ndim in ((moment, 2), (pv, 2)):
        job = created.headers["location"]
        assert await _wait(client, job, auth_headers) == "COMPLETED", \
            (await client.get(job + "/error", headers=auth_headers)).text
        product = await client.get(job + "/results/result", headers=auth_headers)
        with fits.open(io.BytesIO(product.content)) as hdul:
            image = next(h for h in hdul if h.data is not None)
            assert image.data.ndim == ndim


@pytest.mark.asyncio
@pytest.mark.parametrize("params, fragment", [
    ({"OPERATION": "MOMENT"}, "MOMENT=<order>"),
    ({"OPERATION": "MOMENT", "MOMENT": "0", "CHANNELS": "9 3"}, "CHANNELS"),
    ({"OPERATION": "PV", "PVPATH": "1 2"}, "two"),
    ({"OPERATION": "PV", "PVPATH": "1 2 3 4", "POS": POS}, "not understood"),
    ({"OPERATION": "SMOOTH"}, "OPERATION"),
    ({"PHASE": "SUSPEND"}, "PHASE"),
])
async def test_async_rejects_bad_jobs_at_creation(client, auth_headers, synthetic_fits_cube, params, fragment):
    resp = await client.post("/soda/async", data={"ID": _id(synthetic_fits_cube), **params},
                             headers=auth_headers)
    assert resp.status_code == 400 and fragment in resp.text


@pytest.mark.asyncio
async def test_abort_delete_and_ownership(client, auth_headers, synthetic_fits_cube, monkeypatch):
    created = await client.post("/soda/async", data={"ID": _id(synthetic_fits_cube)}, headers=auth_headers)
    job = created.headers["location"]
    aborted = await client.post(job + "/phase", data={"PHASE": "ABORT"}, headers=auth_headers)
    assert aborted.status_code == 303
    assert (await client.get(job + "/phase", headers=auth_headers)).text == "ABORTED"
    # A job that is not running cannot be started again.
    assert (await client.post(job + "/phase", data={"PHASE": "RUN"}, headers=auth_headers)).status_code == 400

    listing = (await client.get("/soda/async", headers={**auth_headers, "Accept": "application/json"})).json()
    assert any(job.endswith(j["job_id"]) for j in listing)

    # Another user's token cannot see it (multi-user backend).
    monkeypatch.setenv("VISIVO_TOKENS", "other-user-token")
    other = {"X-Visivo-Token": "other-user-token"}
    assert (await client.get(job, headers=other)).status_code == 404
    monkeypatch.delenv("VISIVO_TOKENS")

    deleted = await client.delete(job, headers=auth_headers)
    assert deleted.status_code == 303
    assert (await client.get(job, headers=auth_headers)).status_code == 404


@pytest.mark.asyncio
async def test_no_overlap_is_an_empty_204(client, auth_headers, synthetic_fits_cube):
    resp = await client.get("/soda/sync", params={"ID": _id(synthetic_fits_cube), "POS": "CIRCLE 180 0 0.001"},
                            headers=auth_headers)
    assert resp.status_code == 204 and resp.content == b""


@pytest.mark.asyncio
async def test_circle_and_polygon_parameters(client, auth_headers, synthetic_fits_cube):
    circle = await client.get("/soda/sync", headers=auth_headers,
                              params={"ID": _id(synthetic_fits_cube), "CIRCLE": "83.82208 -5.39111 0.003"})
    assert circle.status_code == 200 and "pos" in circle.headers["x-soda-applied"]
    polygon = await client.get("/soda/sync", headers=auth_headers, params={
        "ID": _id(synthetic_fits_cube), "POLYGON": "83.818 -5.395 83.826 -5.395 83.822 -5.387"})
    assert polygon.status_code == 200
    both = await client.get("/soda/sync", headers=auth_headers,
                            params={"ID": _id(synthetic_fits_cube), "POS": POS, "CIRCLE": "1 2 3"})
    assert both.status_code == 400 and "one spatial constraint" in both.text


@pytest.mark.asyncio
@pytest.mark.parametrize("time, status", [("not a time", 400), ("60010 60000", 400),
                                          ("50000 50001", 204), ("59999 60001", 200)])
async def test_time_is_checked_against_the_observation(client, auth_headers, tmp_path, time, status):
    import numpy as np

    cube = tmp_path / "timed.fits"
    hdu = fits.PrimaryHDU(np.ones((4, 8, 8), dtype=np.float32))
    for key, value in (("CTYPE1", "RA---SIN"), ("CTYPE2", "DEC--SIN"), ("CTYPE3", "FREQ"),
                       ("CRVAL1", 10.0), ("CRVAL2", 10.0), ("CRVAL3", 1e9), ("CDELT1", -1e-3),
                       ("CDELT2", 1e-3), ("CDELT3", 1e6), ("CRPIX1", 4), ("CRPIX2", 4), ("CRPIX3", 1),
                       ("MJD-OBS", 60000.0), ("EXPTIME", 3600.0)):
        hdu.header[key] = value
    hdu.writeto(cube)
    resp = await client.get("/soda/sync", params={"ID": _id(cube), "TIME": time}, headers=auth_headers)
    assert resp.status_code == status, resp.text
    if status == 200:
        assert "time" in resp.headers["x-soda-applied"]


@pytest.mark.asyncio
async def test_malformed_channels_are_a_usage_error(client, auth_headers, synthetic_fits_cube):
    resp = await client.post("/soda/async", headers=auth_headers, data={
        "ID": _id(synthetic_fits_cube), "OPERATION": "MOMENT", "MOMENT": "0", "CHANNELS": "--1 2"})
    assert resp.status_code == 400 and resp.text.startswith("UsageError")


@pytest.mark.asyncio
async def test_moment_without_channels_uses_the_whole_axis(client, auth_headers, synthetic_fits_cube):
    async def product(extra):
        created = await client.post("/soda/async", headers=auth_headers, data={
            "ID": _id(synthetic_fits_cube), "OPERATION": "MOMENT", "MOMENT": "0", "PHASE": "RUN", **extra})
        job = created.headers["location"]
        assert await _wait(client, job, auth_headers) == "COMPLETED"
        with fits.open(io.BytesIO((await client.get(job + "/results/result", headers=auth_headers)).content)) as h:
            return next(x for x in h if x.data is not None).data.copy()

    import numpy as np

    whole, first = await product({}), await product({"CHANNELS": "0 0"})
    # The bright line sits in channels 14-17: only the whole-axis map sees it.
    assert np.nanmax(whole) > 2 * np.nanmax(first)


def test_pv_path_is_icrs_on_a_galactic_cube(tmp_path):
    import numpy as np
    from astropy.coordinates import SkyCoord
    from astropy.wcs import WCS

    from app.routers.soda import _pv_pixels

    w = WCS(naxis=3)
    w.wcs.ctype = ["GLON-TAN", "GLAT-TAN", "FREQ"]
    w.wcs.crval, w.wcs.crpix, w.wcs.cdelt = [0, 0, 1e9], [32, 32, 1], [-0.01, 0.01, 1e6]
    cube = tmp_path / "gal.fits"
    fits.PrimaryHDU(np.zeros((2, 64, 64), dtype=np.float32), w.to_header()).writeto(cube)
    centre = SkyCoord(l=0, b=0, unit="deg", frame="galactic").icrs
    pixels = _pv_pixels(cube, f"{centre.ra.deg} {centre.dec.deg} {centre.ra.deg} {centre.dec.deg}")
    assert pixels[0] == pytest.approx([31.0, 31.0], abs=1e-6)


@pytest.mark.asyncio
async def test_abort_discards_the_late_output_and_delete_reclaims(client, auth_headers, synthetic_fits_cube,
                                                                   tmp_path, monkeypatch):
    from app.routers import soda

    release = asyncio.Event()

    async def slow_execute(operation, path, values, out_dir, task_id=None):
        await release.wait()
        try:
            (out_dir / "late.fits").write_bytes(b"late")
        except OSError:
            pass
        return {"output_path": str(out_dir / "late.fits"), "applied": ""}

    with monkeypatch.context() as patched:
        patched.setattr(soda, "_execute", slow_execute)
        created = await client.post("/soda/async", headers=auth_headers,
                                    data={"ID": _id(synthetic_fits_cube), "PHASE": "RUN"})
        job = created.headers["location"]
        job_dir = soda._job_dir(job.rsplit("/", 1)[1])
        late = job_dir / "late.fits"
        await client.post(job + "/phase", data={"PHASE": "ABORT"}, headers=auth_headers)
        assert (await client.get(job + "/phase", headers=auth_headers)).text == "ABORTED"
        # The job's directory is gone, so the abandoned computation's late write fails.
        assert not job_dir.exists()
        # A computation that does not poll the stop file (this one) runs to
        # its end: until then the node still counts it as running (it holds
        # the worker), and the record is not evicted however old it gets.
        from app.tasks import TASKS
        assert TASKS.snapshot()["running_count"] == 1
        status = (await client.get("/v1/node/status")).json()
        assert status["tasks"]["running"] == 1
        release.set()
        await asyncio.sleep(0.05)
        assert not late.exists()
        assert TASKS.snapshot()["running_count"] == 0

        # DELETE of a running job forgets the job, not the work: the worker is
        # still counted until it ends.
        release.clear()
        created = await client.post("/soda/async", headers=auth_headers,
                                    data={"ID": _id(synthetic_fits_cube), "PHASE": "RUN"})
        job = created.headers["location"]
        await asyncio.sleep(0.02)
        assert (await client.delete(job, headers=auth_headers)).status_code == 303
        assert (await client.get(job, headers=auth_headers)).status_code == 404
        assert TASKS.snapshot()["running_count"] == 1
        release.set()
        await asyncio.sleep(0.05)
        assert TASKS.snapshot()["running_count"] == 0

    # DELETE of a completed job removes its product.
    done = await client.post("/soda/async", headers=auth_headers,
                             data={"ID": _id(synthetic_fits_cube), "POS": POS, "PHASE": "RUN"})
    job = done.headers["location"]
    assert await _wait(client, job, auth_headers) == "COMPLETED"
    output = (await client.get(job, headers={**auth_headers, "Accept": "application/json"})).json()["output_path"]
    assert (await client.delete(job, headers=auth_headers)).status_code == 303
    from pathlib import Path
    assert not Path(output).exists()


@pytest.mark.asyncio
async def test_uws_job_document_is_complete(client, auth_headers, synthetic_fits_cube):
    created = await client.post("/soda/async", headers=auth_headers,
                                data={"ID": _id(synthetic_fits_cube), "POS": POS, "RUNID": "paper-e4"})
    job = created.headers["location"]
    doc = ET.fromstring((await client.get(job, headers=auth_headers)).text)
    tags = [child.tag.replace(UWS, "") for child in doc]
    assert tags == ["jobId", "runId", "ownerId", "phase", "creationTime", "startTime", "endTime",
                    "executionDuration", "destruction", "parameters", "results"]
    assert doc.find(f"{UWS}runId").text == "paper-e4"
    for resource in ("parameters", "executionduration", "destruction", "owner", "quote"):
        assert (await client.get(f"{job}/{resource}", headers=auth_headers)).status_code == 200
    assert (await client.get(f"{job}/quote", headers=auth_headers)).text == ""


@pytest.mark.asyncio
async def test_vosi_capabilities_and_availability(client):
    caps = await client.get("/soda/capabilities")
    assert caps.status_code == 200
    assert "ivo://ivoa.net/std/SODA#sync-1.0" in caps.text and "ivo://ivoa.net/std/SODA#async-1.0" in caps.text
    avail = await client.get("/soda/availability")
    assert "<vosi:available>true</vosi:available>" in avail.text


@pytest.mark.asyncio
async def test_open_band_and_projection_edges(client, auth_headers, synthetic_fits_cube):
    base = {"ID": _id(synthetic_fits_cube)}
    whole = await client.get("/soda/sync", params={**base, "BAND": "-Inf +Inf"}, headers=auth_headers)
    assert whole.status_code == 200
    with fits.open(io.BytesIO(whole.content)) as hdul:
        assert hdul[0].data.shape[0] == 32
    off = await client.get("/soda/sync", params={**base, "POS": "RANGE 180 181 0 1"}, headers=auth_headers)
    assert off.status_code == 204


@pytest.mark.asyncio
async def test_async_empty_selection_has_an_explaining_result(client, auth_headers, synthetic_fits_cube):
    created = await client.post("/soda/async", headers=auth_headers, data={
        "ID": _id(synthetic_fits_cube), "POS": "CIRCLE 180 0 0.001", "PHASE": "RUN"})
    job = created.headers["location"]
    assert await _wait(client, job, auth_headers) == "COMPLETED"
    result = await client.get(job + "/results/result", headers=auth_headers)
    assert result.headers["content-type"].startswith("text/plain") and result.text.startswith("No data")


@pytest.mark.asyncio
async def test_pyvo_reads_the_job_and_vosi_documents(client, auth_headers, synthetic_fits_cube):
    pyvo_uws = pytest.importorskip("pyvo.io.uws")
    from pyvo.io.vosi import parse_availability, parse_capabilities

    created = await client.post("/soda/async", headers=auth_headers,
                                data={"ID": _id(synthetic_fits_cube), "POS": "nonsense", "PHASE": "RUN"})
    job = created.headers["location"]
    assert await _wait(client, job, auth_headers) == "ERROR"
    parsed = pyvo_uws.parse_job(io.BytesIO((await client.get(job, headers=auth_headers)).content))
    assert parsed.phase == "ERROR" and parsed.errorsummary.has_detail
    assert parse_availability(io.BytesIO((await client.get("/soda/availability")).content)).available
    caps = parse_capabilities(io.BytesIO((await client.get("/soda/capabilities")).content))
    assert any("SODA#async-1.0" in c.standardid for c in caps)


@pytest.mark.asyncio
async def test_repeated_parameter_error_type(client, auth_headers, synthetic_fits_cube):
    resp = await client.get(f"/soda/sync?ID={_id(synthetic_fits_cube)}&POS={POS}&POS={POS}", headers=auth_headers)
    assert resp.status_code == 400 and resp.text.startswith("MultiValuedParamNotSupported")


def test_expired_jobs_release_their_storage(tmp_path, monkeypatch):
    from app.routers import soda
    from app.tasks import TaskRegistry

    registry = TaskRegistry(ttl_seconds=1)
    registry.add_eviction_listener(soda._on_evict)
    record = registry.create("soda-cutout")
    registry.update(record.task_id, result={"soda": {"operation": "CUTOUT"}})
    job_dir = soda._job_dir(record.task_id)
    job_dir.mkdir(parents=True)
    (job_dir / "product.fits").write_bytes(b"x")
    record.last_touched_monotonic -= 10
    registry.stats()  # any access evicts
    assert not job_dir.exists()


@pytest.mark.asyncio
@pytest.mark.parametrize("pos", ["RANGE 0 360 -90 90", "RANGE 0 359 -20 20", "RANGE 80 90 -6 -5",
                                 "RANGE 350 85 -90 90"])
async def test_large_and_wrapping_ranges_contain_the_cube(client, auth_headers, synthetic_fits_cube, pos):
    resp = await client.get("/soda/sync", params={"ID": _id(synthetic_fits_cube), "POS": pos}, headers=auth_headers)
    assert resp.status_code == 200, (pos, resp.status_code)


@pytest.mark.asyncio
async def test_a_tiny_range_inside_the_image_is_found(client, auth_headers, synthetic_fits_cube):
    resp = await client.get("/soda/sync", headers=auth_headers, params={
        "ID": _id(synthetic_fits_cube), "POS": "RANGE 83.8220 83.8222 -5.3912 -5.3910"})
    assert resp.status_code == 200
    with fits.open(io.BytesIO(resp.content)) as hdul:
        # The box lies inside one pixel; the bound is conservative by a cell each side.
        assert hdul[0].data.shape[1] <= 5 and hdul[0].data.shape[2] <= 5


@pytest.mark.asyncio
async def test_a_job_whose_directory_cannot_be_made_fails_cleanly(client, auth_headers, synthetic_fits_cube,
                                                                   tmp_path, monkeypatch):
    from app.routers import soda

    blocked = tmp_path / "occupied"
    blocked.write_bytes(b"a file where the directory should go")
    with monkeypatch.context() as patched:
        patched.setattr(soda, "_job_dir", lambda job_id: blocked)
        created = await client.post("/soda/async", headers=auth_headers,
                                    data={"ID": _id(synthetic_fits_cube), "PHASE": "RUN"})
        job = created.headers["location"]
        assert await _wait(client, job, auth_headers) == "ERROR"


@pytest.mark.parametrize("ra1, ra2", [(0, 360), (0, 200), (95, 105), (350, 120)])
def test_a_thin_strip_crossing_the_image_is_bounded(ra1, ra2):
    from astropy.wcs import WCS

    from app.soda_cutout import Range, _range_to_pixel_bbox

    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---CAR", "DEC--CAR"]
    w.wcs.crval, w.wcs.crpix, w.wcs.cdelt = [100, 0], [1025, 1025], [-0.001, 0.001]
    dec = float(w.pixel_to_world(1024, 101).dec.deg)
    (y0, y1), (x0, x1) = _range_to_pixel_bbox(w, Range(ra1, ra2, dec - 0.0001, dec + 0.0001), (2048, 2048))
    assert y0 <= 101 <= y1 and y1 - y0 <= 16          # the strip, within a few 4-pixel cells
    assert x0 == 0 and x1 == 2047                     # all of it: the RA range covers the image


def test_failed_removals_are_retried_on_a_timer(tmp_path, monkeypatch):
    from app.routers import soda

    async def scenario():
        job_dir = tmp_path / "job"
        job_dir.mkdir()
        attempts = {"n": 0}
        real = soda.shutil.rmtree

        def stubborn(path, *a, **kw):
            attempts["n"] += 1
            if attempts["n"] == 1:
                return               # first removal fails silently, as on Windows
            return real(path, *a, **kw)

        with monkeypatch.context() as patched:
            patched.setattr(soda, "_job_dir", lambda job_id: job_dir)
            patched.setattr(soda.shutil, "rmtree", stubborn)
            patched.setattr(soda, "_RETRY_SECONDS", 0.01)
            patched.setattr(soda, "_UNRECLAIMED", set())   # nothing left over from other tests
            soda._reclaim("job")
            assert job_dir.exists() and "job" in soda._UNRECLAIMED
            await asyncio.sleep(0.05)
            assert not job_dir.exists() and "job" not in soda._UNRECLAIMED

    asyncio.run(scenario())


def test_a_polar_cap_containing_the_pole_is_found():
    from astropy.wcs import WCS

    from app.soda_cutout import Range, _range_to_pixel_bbox

    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crval, w.wcs.crpix, w.wcs.cdelt = [0, 90], [1026, 1026], [-0.001, 0.001]
    (y0, y1), (x0, x1) = _range_to_pixel_bbox(w, Range(0, 360, 89.9999, 90), (2048, 2048))
    assert y0 <= 1025 <= y1 and x0 <= 1025 <= x1


def test_a_polar_cap_with_an_offset_pole_keeps_its_edge_pixels():
    from astropy.wcs import WCS

    from app.soda_cutout import Range, _range_to_pixel_bbox

    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crval, w.wcs.crpix, w.wcs.cdelt = [0, 90], [1028, 1027], [-0.001, 0.001]
    (y0, y1), (x0, x1) = _range_to_pixel_bbox(w, Range(0, 360, 89.9979, 90), (2048, 2048))
    assert x0 <= 1029 <= x1 and y0 <= 1026 <= y1


def test_uws_timestamps_keep_milliseconds():
    # endTime minus startTime is a job's own duration: whole seconds would not do.
    from datetime import datetime, timezone

    from app.routers.soda import _as_utc, _utc

    stamp = datetime(2026, 10, 28, 15, 4, 5, 678900, tzinfo=timezone.utc)
    assert _utc(stamp) == "2026-10-28T15:04:05.678Z"
    assert _as_utc("2026-10-28T15:04:05.678900") == "2026-10-28T15:04:05.678Z"
