"""Real outputs of other codes through the routes (issue #554), when yt's sample
data is at hand: set VISIVO_YT_SAMPLES to the folder holding them (see
tests/test_yt_output.py for the synthetic checks that always run)."""
from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from app import amr_source
from app import yt_output as yo

needs_yt = pytest.mark.skipif(not yo.yt_available(), reason="yt is not installed")
SAMPLES = Path(os.environ.get("VISIVO_YT_SAMPLES", "/nonexistent"))
needs_samples = pytest.mark.skipif(not SAMPLES.is_dir(), reason="VISIVO_YT_SAMPLES is not set")


@needs_yt
@needs_samples
@pytest.mark.asyncio
async def test_a_real_uniform_grid_opens_as_a_cube_of_its_density(client, auth_headers):
    import yt
    path = SAMPLES / "DensTurbMag/DensTurbMag/DensTurbMag_hdf5_plt_cnt_0015"     # FLASH, unigrid
    h = amr_source.open_hierarchy(str(path))
    assert h.uniform_level() == 2 and h.leaf_counts() == {2: 32 ** 3}
    from app.classify import classify_path
    verdict = classify_path(path)
    assert (verdict["kind"], verdict["opener"]) == ("cube", "dataset")
    opened = (await client.post("/v1/datasets/open", json={"path": str(path)},
                                headers=auth_headers)).json()
    assert opened["valid"], opened["error"]
    assert (opened["width"], opened["height"], opened["depth"]) == (32, 32, 32)
    # The cube is yt's own grid of the density, cell for cell (z, y, x).
    made = await client.post("/v1/simulations/amr/uniform", json={
        "path": str(path), "field": "density", "level": 2}, headers=auth_headers)
    assert made.status_code == 200, made.text
    with fits.open(made.json()["outputs"][0]["path"]) as hdul:
        cube, header = hdul[0].data, hdul[0].header
    assert header["AMRCODE"] == "yt:flash" and header["AMRUNIT"] == "g/cm**3"
    ds = yt.load(str(path))
    grid = ds.covering_grid(2, ds.domain_left_edge, ds.domain_dimensions * 4)[("gas", "density")]
    np.testing.assert_allclose(cube, np.transpose(np.asarray(grid.to_value("g/cm**3")), (2, 1, 0)),
                               rtol=1e-6)
    # A second open reuses the cube.
    again = (await client.post("/v1/datasets/open", json={"path": str(path)},
                               headers=auth_headers)).json()
    assert again["valid"]


@needs_yt
@needs_samples
@pytest.mark.asyncio
async def test_a_real_refined_mesh_opens_as_cells_and_serves_the_routes(client, auth_headers):
    from app.classify import classify_path
    path = SAMPLES / "Sedov3D/Sedov_3d/sedov_hdf5_chk_0003"
    r = classify_path(path)
    assert (r["kind"], r["opener"]) == ("particles", "catalogue")
    assert r["components"][0]["num_rows"] == 835584
    opened = (await client.post("/v1/catalogue/open", json={"path": str(path), "format": "hdf5"},
                                headers=auth_headers)).json()
    assert opened["valid"], opened["error"]
    assert opened["num_rows"] == 835584 and opened["format"] == "yt"
    plan = (await client.post("/v1/simulations/amr/plan", json={"path": str(path),
                                                                  "field": "density"},
                              headers=auth_headers)).json()
    assert plan["valid"] and plan["code"] == "yt:flash" and plan["plan"]["level"] == 4


@needs_yt
@needs_samples
def test_an_amrex_plot_opens_by_its_header_file_as_by_its_folder():
    # The open dialog picks files, not folders: the plot's Header stands for it.
    folder = SAMPLES / "nyx_sedov_plt00086/nyx_sedov_plt00086"
    header = folder / "Header"
    assert yo.mesh_path(header) == str(folder)
    from app.classify import classify_path
    by_header, by_folder = classify_path(header), classify_path(folder)
    assert by_header["kind"] == by_folder["kind"] and by_header["kind"] != "directory"
    assert amr_source.open_hierarchy(str(header)).leaf_counts() == \
        amr_source.open_hierarchy(str(folder)).leaf_counts()
