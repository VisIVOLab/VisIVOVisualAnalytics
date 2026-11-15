"""The headset's variable picker (issue #317): reduced_info lists the fields
of a multi-field source (a PLUTO output's variables present on disk, a VBT
volume's fields) and names the one on show, whichever way the dataset was
opened; any other cube has none."""
from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
import pytest

from app import pluto_source as ps
from app.volume_source import field_listing, vbt_field_ref

_GEN = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"


def _write_pluto():
    spec = importlib.util.spec_from_file_location("generate_demo_data", _GEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.write_pluto


SHAPE = (6, 8, 10)


def _fields(names=("rho", "vx1", "prs")) -> dict[str, np.ndarray]:
    k, j, i = np.meshgrid(*(np.arange(n, dtype=np.float64) for n in SHAPE), indexing="ij")
    out = {"rho": 1.0 + i + 10.0 * j + 100.0 * k, "vx1": np.sin(i) - j, "prs": 2.0 + np.cos(k)}
    return {n: out[n] for n in names}


def _write_vbt(tmp_path: Path) -> Path:
    nz, ny, nx = SHAPE
    data = tmp_path / "v.bin"
    with open(data, "wb") as fh:
        for f in (np.ones(SHAPE), np.arange(np.prod(SHAPE)).reshape(SHAPE)):
            f.astype("<f4").tofile(fh)
    (tmp_path / "v.bin.head").write_text(f"float\n2\n{nx * ny * nz} {nx} {ny} {nz} 1 1 1\nlittle\nrho\nvx\n")
    return data


def test_field_listing_of_pluto_vbt_and_other_paths(tmp_path):
    first = _write_pluto()(tmp_path / "p", _fields(), mode="multiple_files", precision="flt", endian="little")
    refs = ps.pluto_fields(first)
    fields, name = field_listing(refs[1]["path"])
    assert [f["name"] for f in fields] == ["rho", "vx1", "prs"]
    assert [f["path"] for f in fields] == [r["path"] for r in refs]
    assert name == "vx1"
    vbt = _write_vbt(tmp_path)
    fields, name = field_listing(vbt_field_ref(vbt, 1))
    assert [f["name"] for f in fields] == ["rho", "vx"] and name == "vx"
    assert fields[0]["path"] == vbt_field_ref(vbt, 0)
    assert field_listing(str(tmp_path / "cube.fits")) == ([], "")


def test_a_pluto_output_copied_one_variable_at_a_time_has_one_field(tmp_path):
    """multiple_files with only the density on disk (a 2048³ run is often
    copied one variable at a time): one field, so the picker is hidden."""
    first = _write_pluto()(tmp_path / "p", _fields(), mode="multiple_files", precision="flt", endian="little")
    for name in ("vx1", "prs"):
        next(Path(first).parent.glob(f"{name}.*.flt")).unlink()
    ref = ps.pluto_fields(first)[0]["path"]
    fields, name = field_listing(ref)
    assert [f["name"] for f in fields] == ["rho"] and name == "rho"


@pytest.mark.asyncio
async def test_reduced_info_lists_the_fields_and_names_the_one_on_show(client, auth_headers, tmp_path):
    first = _write_pluto()(tmp_path / "p", _fields(), mode="multiple_files", precision="flt", endian="little")
    # Opened by its data file, as the desktop does: the reference is resolved on open.
    opened = await client.post("/v1/datasets/open", json={"path": str(first)}, headers=auth_headers)
    assert opened.status_code == 200, opened.text
    body = opened.json()
    info = await client.post("/v1/cube/reduced_info", json={"dataset_id": body["dataset_id"], "max_dim": 64},
                             headers=auth_headers)
    assert info.status_code == 200, info.text
    got = info.json()
    assert [f["name"] for f in got["fields"]] == ["rho", "vx1", "prs"]
    assert got["field_name"] == "rho" == body["field_name"]
    assert [f["path"] for f in got["fields"]] == [f["path"] for f in body["fields"]]
    # Each listed path opens that variable, with its own name.
    other = await client.post("/v1/datasets/open", json={"path": got["fields"][2]["path"]}, headers=auth_headers)
    info2 = await client.post("/v1/cube/reduced_info", json={"dataset_id": other.json()["dataset_id"], "max_dim": 64},
                              headers=auth_headers)
    assert info2.json()["field_name"] == "prs"
    assert info2.json()["full_shape"] == got["full_shape"]


@pytest.mark.asyncio
async def test_a_plain_cube_has_no_fields(client, opened_cube):
    info = await client.post("/v1/cube/reduced_info", json={"dataset_id": opened_cube["dataset_id"]},
                             headers=opened_cube["headers"])
    assert info.status_code == 200, info.text
    assert info.json()["fields"] == [] and info.json()["field_name"] == ""
