"""The finder plugins and the source-finding task."""
import asyncio
import io
import os
import stat
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits


async def _wait(client, headers, task_id, done=("completed", "failed", "cancelled")):
    for _ in range(400):
        body = (await client.get(f"/v1/tasks/{task_id}", headers=headers)).json()
        if body["status"] in done:
            return body
        await asyncio.sleep(0.05)
    raise AssertionError(f"task did not finish: {body}")


@pytest.fixture()
def image_with_sources(tmp_path):
    """A 128x128 image: Gaussian noise and two bright Gaussian sources."""
    rng = np.random.default_rng(3)
    data = rng.normal(0.0, 1.0, (128, 128)).astype(np.float32)
    yy, xx = np.mgrid[0:128, 0:128]
    for x0, y0, peak in ((40.0, 50.0, 40.0), (90.0, 80.0, 25.0)):
        data += (peak * np.exp(-0.5 * ((xx - x0) ** 2 + (yy - y0) ** 2) / 2.5 ** 2)).astype(np.float32)
    data[5:8, 5:8] = np.nan
    hdu = fits.PrimaryHDU(data)
    for key, value in (("CTYPE1", "RA---TAN"), ("CTYPE2", "DEC--TAN"), ("CRVAL1", 150.0), ("CRVAL2", -30.0),
                       ("CRPIX1", 64.0), ("CRPIX2", 64.0), ("CDELT1", -1e-3), ("CDELT2", 1e-3),
                       ("CUNIT1", "deg"), ("CUNIT2", "deg")):
        hdu.header[key] = value
    path = tmp_path / "sources.fits"
    hdu.writeto(path)
    return path


async def _open(client, headers, path):
    body = (await client.post("/v1/datasets/open", json={"path": str(path)}, headers=headers)).json()
    assert body["valid"], body["error"]
    return body["dataset_id"], {**headers, "X-Visivo-Session": body["session_id"]}


@pytest.mark.asyncio
async def test_finders_are_listed_with_availability(client, auth_headers, monkeypatch):
    monkeypatch.delenv("VISIVO_SOFIA_BIN", raising=False)
    monkeypatch.setenv("PATH", "/nonexistent")
    body = (await client.get("/v1/finders", headers=auth_headers)).json()
    by_name = {f["name"]: f for f in body["finders"]}
    assert by_name["photutils"]["available"] is True and by_name["photutils"]["kinds"] == ["image"]
    assert by_name["sofia2"]["available"] is False and "not found" in by_name["sofia2"]["reason"]
    assert "scfind.threshold" in by_name["sofia2"]["parameters"]
    status = (await client.get("/v1/node/status")).json()
    assert status["capabilities"]["finders"] == ["photutils"]
    # The memory need is stated per available finder: factor × dataset bytes + floor.
    stated = status["capabilities"]["finder_memory"]
    assert list(stated) == ["photutils"]
    assert stated["photutils"]["factor"] > 1.0 and stated["photutils"]["floor_bytes"] > 0


@pytest.mark.asyncio
async def test_photutils_task_finds_the_sources_and_writes_products(client, auth_headers, image_with_sources):
    dataset_id, headers = await _open(client, auth_headers, image_with_sources)
    created = await client.post("/v1/tasks/sourcefind", headers=headers,
                                json={"dataset_id": dataset_id, "finder": "photutils",
                                      "params": {"nsigma": 5, "npixels": 5}})
    assert created.status_code == 200, created.text
    body = await _wait(client, headers, created.json()["task_id"])
    assert body["status"] == "completed", body
    result = body["result"]
    assert result["finder"] == "photutils" and result["n_sources"] == 2
    xs = sorted(round(s["x"]) for s in result["sources"])
    assert xs == [40, 90]
    assert all(abs(s["ra"] - 150.0) < 0.1 and abs(s["dec"] + 30.0) < 0.1 for s in result["sources"])
    with fits.open(result["mask_path"]) as hdul:
        labels = hdul[0].data
        assert labels.shape == (128, 128) and labels[50, 40] > 0 and labels[80, 90] > 0
        assert labels[50, 40] != labels[80, 90] and labels[0, 0] == 0
    from astropy.io.votable import parse_single_table

    table = parse_single_table(io.BytesIO(Path(result["catalog_path"]).read_bytes())).to_table()
    assert len(table) == 2 and {"label", "x", "y", "ra", "dec", "area", "flux", "peak"} <= set(table.colnames)
    # The products sit in the workspace, under the task.
    assert Path(result["mask_path"]).parent.name == created.json()["task_id"]


@pytest.mark.asyncio
async def test_bad_parameters_fail_the_task_not_the_service(client, auth_headers, image_with_sources):
    dataset_id, headers = await _open(client, auth_headers, image_with_sources)
    created = await client.post("/v1/tasks/sourcefind", headers=headers,
                                json={"dataset_id": dataset_id, "finder": "photutils", "params": {"nsigma": "lots"}})
    body = await _wait(client, headers, created.json()["task_id"])
    assert body["status"] == "failed" and "nsigma" in body["error"]


@pytest.mark.asyncio
async def test_finder_and_kind_are_checked_at_creation(client, auth_headers, synthetic_fits_cube, monkeypatch):
    monkeypatch.delenv("VISIVO_SOFIA_BIN", raising=False)
    monkeypatch.setenv("PATH", "/nonexistent")
    dataset_id, headers = await _open(client, auth_headers, synthetic_fits_cube)
    unknown = await client.post("/v1/tasks/sourcefind", headers=headers,
                                json={"dataset_id": dataset_id, "finder": "aegean"})
    assert unknown.status_code == 404
    wrong_kind = await client.post("/v1/tasks/sourcefind", headers=headers,
                                   json={"dataset_id": dataset_id, "finder": "photutils"})
    assert wrong_kind.status_code == 422
    missing = await client.post("/v1/tasks/sourcefind", headers=headers,
                                json={"dataset_id": dataset_id, "finder": "sofia2"})
    assert missing.status_code == 503


def _fake_sofia(tmp_path, body: str) -> str:
    script = tmp_path / "sofia2"
    script.write_text("#!/bin/sh\n" + body)
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return str(script)


@pytest.mark.asyncio
async def test_sofia2_task_reads_the_outputs(client, auth_headers, synthetic_fits_cube, tmp_path, monkeypatch):
    # A stand-in for SoFiA-2: it writes a one-row catalogue where the .par says.
    votable = ('<VOTABLE><RESOURCE><TABLE><FIELD name="id"/><FIELD name="x"/><FIELD name="f_sum"/>'
               '<DATA><TABLEDATA><TR><TD>1</TD><TD>32.5</TD><TD>7.25</TD></TR></TABLETDATA></DATA>'
               '</TABLE></RESOURCE></VOTABLE>').replace("TABLETDATA", "TABLEDATA")
    monkeypatch.setenv("VISIVO_SOFIA_BIN", _fake_sofia(tmp_path, f'''
d=$(dirname "$1")
grep -q "output.directory = $d" "$1" || exit 3
grep -q "scfind.threshold = 4.0" "$1" || exit 4
printf '%s' '{votable}' > "$d/sofia_out_cat.xml"
echo "fake sofia ran"
'''))
    dataset_id, headers = await _open(client, auth_headers, synthetic_fits_cube)
    created = await client.post("/v1/tasks/sourcefind", headers=headers,
                                json={"dataset_id": dataset_id, "finder": "sofia2",
                                      "params": {"scfind.threshold": 4.0}})
    assert created.status_code == 200, created.text
    body = await _wait(client, headers, created.json()["task_id"])
    assert body["status"] == "completed", body
    result = body["result"]
    assert result["n_sources"] == 1 and result["sources"][0]["f_sum"] == 7.25
    assert result["mask_path"] == "" and result["catalog_path"].endswith("sofia_out_cat.xml")
    assert "fake sofia ran" in Path(result["log_path"]).read_text()


@pytest.mark.asyncio
@pytest.mark.parametrize("params", [
    {"output.directory": "/tmp/elsewhere"},
    {"input.mask": "/etc/passwd"},
    {"flag.catalog": "/etc/passwd"},
    {"reliability.catalog": "/data/other.xml"},
    {"scfind.threshold": "4\noutput.directory = /tmp/x"},
    {"weird key": 1},
])
async def test_sofia2_parameters_cannot_name_files_or_escape(client, auth_headers, synthetic_fits_cube,
                                                              tmp_path, monkeypatch, params):
    monkeypatch.setenv("VISIVO_SOFIA_BIN", _fake_sofia(tmp_path, "exit 0\n"))
    dataset_id, headers = await _open(client, auth_headers, synthetic_fits_cube)
    created = await client.post("/v1/tasks/sourcefind", headers=headers,
                                json={"dataset_id": dataset_id, "finder": "sofia2", "params": params})
    body = await _wait(client, headers, created.json()["task_id"])
    assert body["status"] == "failed" and "not accepted" in body["error"] or "invalid value" in body["error"] \
        or "not a SoFiA-2 parameter" in body["error"]


def test_sofia2_allowlist_accepts_the_documented_sections():
    from app.compute.finders.sofia2 import refused

    assert refused({"scfind.threshold": 4.0, "linker.minSizeXY": 3, "reliability.enable": True,
                    "pipeline.threads": 2, "flag.region": "0,10,0,10,0,5"}) is None


@pytest.mark.asyncio
async def test_a_helper_left_behind_by_the_finder_is_killed_too(client, auth_headers, synthetic_fits_cube,
                                                                 tmp_path, monkeypatch):
    # The fake finder starts a helper and exits at once; the helper keeps the
    # pipes open. The timeout must still end the task, and the helper with it.
    pidfile = tmp_path / "helper.pid"
    monkeypatch.setenv("VISIVO_SOFIA_BIN", _fake_sofia(tmp_path, f'sleep 60 &\necho $! > "{pidfile}"\nexit 0\n'))
    monkeypatch.setenv("VISIVO_FINDER_TIMEOUT_S", "1")
    dataset_id, headers = await _open(client, auth_headers, synthetic_fits_cube)
    created = await client.post("/v1/tasks/sourcefind", headers=headers,
                                json={"dataset_id": dataset_id, "finder": "sofia2"})
    body = await _wait(client, headers, created.json()["task_id"])
    assert body["status"] == "failed" and "time limit" in body["error"]
    pid = int(pidfile.read_text())
    for _ in range(40):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            break
        await asyncio.sleep(0.05)
    else:
        raise AssertionError("the helper survived")


def test_photutils_mask_keeps_the_celestial_wcs_of_a_degenerate_image(tmp_path):
    from astropy.wcs import WCS

    from app.compute.finders.photutils_finder import segment

    rng = np.random.default_rng(42)
    data = rng.normal(0, 1, (40, 1, 40)).astype(np.float32)
    data[18:23, 0, 18:23] += 30.0
    w = WCS(naxis=3)
    w.wcs.ctype = ["RA---TAN", "STOKES", "DEC--TAN"]
    w.wcs.crpix, w.wcs.crval, w.wcs.cdelt = [20, 1, 20], [30.0, 1.0, 40.0], [-0.01, 1.0, 0.01]
    path = tmp_path / "degenerate.fits"
    fits.writeto(path, data, w.to_header())
    out = tmp_path / "out"
    out.mkdir()
    result = segment(str(path), out, {})
    assert result["n_sources"] == 1
    header = fits.getheader(result["mask_path"])
    assert header["CTYPE1"] == "RA---TAN" and header["CTYPE2"] == "DEC--TAN"
    assert WCS(header).celestial.naxis == 2
    assert abs(result["sources"][0]["ra"] - 30.0) < 0.05 and abs(result["sources"][0]["dec"] - 40.0) < 0.05


@pytest.mark.asyncio
async def test_cancelling_the_task_kills_sofia2(client, auth_headers, synthetic_fits_cube, tmp_path, monkeypatch):
    pidfile = tmp_path / "pid"
    monkeypatch.setenv("VISIVO_SOFIA_BIN", _fake_sofia(tmp_path, f'echo $$ > "{pidfile}"\nsleep 60\n'))
    dataset_id, headers = await _open(client, auth_headers, synthetic_fits_cube)
    created = await client.post("/v1/tasks/sourcefind", headers=headers,
                                json={"dataset_id": dataset_id, "finder": "sofia2"})
    task_id = created.json()["task_id"]
    for _ in range(100):
        if pidfile.exists() and pidfile.read_text().strip():
            break
        await asyncio.sleep(0.05)
    pid = int(pidfile.read_text())
    cancelled = await client.delete(f"/v1/tasks/{task_id}", headers=headers)
    assert cancelled.status_code == 200
    body = await _wait(client, headers, task_id)
    assert body["status"] == "cancelled"
    for _ in range(40):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            break
        await asyncio.sleep(0.05)
    else:
        raise AssertionError("the SoFiA-2 process survived the cancellation")
    # Its products are gone from the workspace (the registry reports the
    # cancellation at once; the runner's cleanup follows a moment later).
    from app.dependencies import get_exports_dir

    out_dir = get_exports_dir() / "sourcefind" / task_id
    for _ in range(100):
        if not out_dir.exists():
            break
        await asyncio.sleep(0.05)
    assert not out_dir.exists()


@pytest.mark.asyncio
async def test_sofia2_timeout_fails_the_task(client, auth_headers, synthetic_fits_cube, tmp_path, monkeypatch):
    monkeypatch.setenv("VISIVO_SOFIA_BIN", _fake_sofia(tmp_path, "sleep 30\n"))
    monkeypatch.setenv("VISIVO_FINDER_TIMEOUT_S", "1")
    dataset_id, headers = await _open(client, auth_headers, synthetic_fits_cube)
    created = await client.post("/v1/tasks/sourcefind", headers=headers,
                                json={"dataset_id": dataset_id, "finder": "sofia2"})
    body = await _wait(client, headers, created.json()["task_id"])
    assert body["status"] == "failed" and "time limit" in body["error"]


def test_photutils_mask_keeps_distortion_terms(tmp_path):
    from astropy.wcs import WCS, Sip

    from app.compute.finders.photutils_finder import segment

    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN-SIP", "DEC--TAN-SIP"]
    w.wcs.crpix, w.wcs.crval, w.wcs.cdelt = [20, 20], [30.0, 40.0], [-0.01, 0.01]
    a, b = np.zeros((3, 3)), np.zeros((3, 3))
    a[2, 0], b[0, 2] = 0.005, 0.005
    w.sip = Sip(a, b, None, None, w.wcs.crpix)
    data = np.random.default_rng(42).normal(0, 1, (40, 40)).astype(np.float32)
    data[29:34, 29:34] += 30.0
    path = tmp_path / "sip.fits"
    fits.writeto(path, data, w.to_header(relax=True))
    out = tmp_path / "out"
    out.mkdir()
    result = segment(str(path), out, {})
    mask_wcs = WCS(fits.getheader(result["mask_path"]))
    assert mask_wcs.sip is not None
    source = result["sources"][0]
    ra, dec = mask_wcs.all_pix2world([[source["x"], source["y"]]], 0)[0]
    assert abs(ra - source["ra"]) < 1e-6 and abs(dec - source["dec"]) < 1e-6


def test_run_child_writes_the_log_while_running(tmp_path):
    import sys

    from app.compute.finders._process import ChildTimeout, run_child

    async def scenario():
        log = tmp_path / "child.log"
        with pytest.raises(ChildTimeout):
            await run_child([sys.executable, "-c", "import time; print('diagnostic', flush=True); time.sleep(10)"],
                            tmp_path, 0.5, log)
        assert "diagnostic" in log.read_text()

    asyncio.run(scenario())


def test_run_child_times_out_when_only_a_helper_remains(tmp_path):
    # The child exits at once but leaves a helper holding the pipes: the run
    # is not over, the timeout applies, and the helper is killed with the group.
    import sys

    from app.compute.finders._process import ChildTimeout, run_child

    pidfile = tmp_path / "helper.pid"
    code = ("import subprocess, sys, pathlib; "
            "c = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)']); "
            f"pathlib.Path({str(pidfile)!r}).write_text(str(c.pid))")

    async def scenario():
        with pytest.raises(ChildTimeout):
            await run_child([sys.executable, "-c", code], tmp_path, 0.5)
        pid = int(pidfile.read_text())
        for _ in range(40):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            await asyncio.sleep(0.05)
        raise AssertionError("the helper survived")

    asyncio.run(scenario())


def test_run_child_stops_a_noisy_child_promptly(tmp_path):
    # A child flooding its output is killed at the timeout, its pipes are
    # drained to the end, and the log holds what it wrote.
    import sys
    import time

    from app.compute.finders._process import ChildTimeout, run_child

    async def scenario():
        log = tmp_path / "noisy.log"
        start = time.monotonic()
        with pytest.raises(ChildTimeout):
            await run_child([sys.executable, "-c", "while True: print('x' * 1000, flush=True)"],
                            tmp_path, 0.2, log)
        assert time.monotonic() - start < 3.0
        assert log.stat().st_size > 1000

    asyncio.run(scenario())
