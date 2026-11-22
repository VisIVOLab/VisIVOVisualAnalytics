"""Gadget snapshots read in place as tables of particles (issue #31).

Synthetic snapshots written as Gadget writes them (scripts/generate_demo_data.py:
write_gadget) in format 1 and 2, both byte orders, one file or several, and
HDF5: every value read back per block, type and file; the mass table; the
derived temperature and entropy against the analytic gas sphere; and the
snapshot going through the same routes as a VBT point table (classification,
catalogue open, points info and samples, series, the render route, the
ray-traced particle render, the copilot), with the same values."""
from __future__ import annotations

import asyncio
import importlib.util
import struct
from pathlib import Path

import numpy as np
import pytest

from app import gadget_snapshot as gs

_GEN_PATH = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"


def _generator():
    spec = importlib.util.spec_from_file_location("generate_demo_data_gadget", _GEN_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


G = _generator()
MASS_TABLE = [0.0, G.GADGET_DM_MASS, 0.0, 0.0, 0.0, 0.0]


@pytest.fixture(autouse=True)
def _no_units_env(monkeypatch):
    monkeypatch.delenv(gs.UNITS_ENV, raising=False)


def _parts(seed=5, **kw):
    return G.gadget_halo_particles(np.random.default_rng(seed), **kw)


def _snapshot(tmp_path, *, fmt=2, endian="<", num_files=3, name="snap_002", parts=None,
              folder="snapdir_002", **kw):
    parts = parts or _parts()
    split = G.gadget_split(parts) if num_files == 3 else None
    files = G.write_gadget(tmp_path / folder / name, parts, fmt=fmt, endian=endian,
                           num_files=num_files, split=split, mass_table=MASS_TABLE, **kw)
    return files, parts


def _expected(parts, ptype, block, dtype="f4"):
    return np.asarray(parts[ptype][block]).astype(dtype)


# ── Reading every block, type and file ────────────────────────────────────────

@pytest.mark.parametrize("fmt,endian,num_files", [(2, "<", 3), (2, ">", 3), (1, "<", 1),
                                                  (1, ">", 1), (2, ">", 1), (1, "<", 3)])
def test_every_block_reads_back_per_type(tmp_path, fmt, endian, num_files):
    files, parts = _snapshot(tmp_path, fmt=fmt, endian=endian, num_files=num_files)
    assert gs.detect(files[0]) == (fmt, endian)
    snap = gs.open_snapshot(files[-1])          # any file of it opens the whole
    assert snap.path == str(files[0]) and len(snap.files) == num_files
    assert snap.counts() == (2000, 4000, 0, 0, 500, 2)
    assert snap.header.time == pytest.approx(0.5) and snap.header.redshift == pytest.approx(1.0)
    assert snap.header.box_size == 1000.0 and snap.header.hubble == pytest.approx(0.7)
    for t in (0, 1, 4, 5):
        c = snap.component(t)
        pos = _expected(parts, t, "POS")
        vel = _expected(parts, t, "VEL")
        assert c.num_rows == len(pos)
        for k, axis in enumerate("xyz"):
            assert np.array_equal(c.column(axis), pos[:, k])
            assert np.array_equal(c.column("v" + axis), vel[:, k])
        assert np.array_equal(c.column("id"), _expected(parts, t, "ID", "u4"))
    gas = snap.component(0)
    for block, col in (("U", "u"), ("RHO", "rho"), ("NE", "ne"), ("MASS", "mass")):
        assert np.array_equal(gas.column(col), _expected(parts, 0, block)), block
    if fmt == 2:
        # Named blocks: HSML and the black holes' BHMA, which format 1 has no name for.
        assert np.array_equal(gas.column("hsml"), _expected(parts, 0, "HSML"))
        assert np.array_equal(snap.component(5).column("bh_mass"), _expected(parts, 5, "BHMA"))
        assert snap.blocks["BHMA"].types == (5,)
    else:
        assert np.array_equal(gas.column("hsml"), _expected(parts, 0, "HSML"))
        assert "BHMA" not in snap.blocks
    # Stars' and black holes' masses from the MASS block, dark matter's from the table.
    assert np.array_equal(snap.component(4).column("mass"), _expected(parts, 4, "MASS"))
    assert snap.blocks["MASS"].types == (0, 4, 5)
    dm = snap.component(1)
    assert "mass" in dm.names and dm.describe()["mass_from_table"]
    assert np.all(dm.column("mass") == np.float32(G.GADGET_DM_MASS))


def test_a_single_file_is_read_in_place(tmp_path):
    files, parts = _snapshot(tmp_path, num_files=1, name="snap_000", folder=".")
    c = gs.open_snapshot(files[0]).component(1)
    x = c.column("x")
    # A view of the memory map: no copy, not writeable.
    assert isinstance(x.base, np.memmap) or isinstance(x, np.memmap) or not x.flags.owndata
    assert not x.flags.writeable


def test_multi_file_stitching_follows_the_files(tmp_path):
    files, parts = _snapshot(tmp_path)
    snap = gs.open_snapshot(files[0])
    split = G.gadget_split(parts)
    # Each file's rows of a type, in file order: the pieces of the snapshot.
    for t in (0, 1, 4):
        rows = [p.rows for p in snap.pieces[("POS", t)]]
        assert rows == [s[t] for s in split if s[t]]
    # Black holes only in the first file: the other files have no BHMA block.
    assert [len([b for b in f.blocks if b.name == "BHMA"]) for f in snap.files] == [1, 0, 0]
    # No stars in the last: no AGE-like gap, the stars' rows come from two files.
    assert len(snap.pieces[("POS", 4)]) == 2


def test_the_header_counts_with_the_high_word():
    raw = bytearray(256)
    struct.pack_into("<6i", raw, 0, 1, 2, 0, 0, 0, 0)
    struct.pack_into("<6I", raw, 96, 5, 7, 0, 0, 0, 0)
    struct.pack_into("<2i", raw, 120, 0, 4)
    struct.pack_into("<6I", raw, 168, 0, 3, 0, 0, 0, 0)
    h = gs.parse_header(bytes(raw), "<")
    assert h.npart == (1, 2, 0, 0, 0, 0) and h.num_files == 4
    assert h.npart_total[1] == 7 + (3 << 32)


def test_a_missing_file_or_wrong_total_is_refused(tmp_path):
    files, _ = _snapshot(tmp_path)
    files[1].unlink()
    with pytest.raises(ValueError, match="missing"):
        gs.open_snapshot(files[0])
    files2, parts = _snapshot(tmp_path, folder="other",
                              header_npart_total=[1999, 4000, 0, 0, 500, 2])
    with pytest.raises(ValueError, match="announces"):
        gs.open_snapshot(files2[0])


def test_a_broken_record_marker_is_refused(tmp_path):
    files, _ = _snapshot(tmp_path, num_files=1, folder=".", name="snap_009")
    raw = bytearray(files[0].read_bytes())
    # The closing marker of the POS block (format 2: name record, then POS).
    pos_len = struct.unpack_from("<i", raw, 8 + 264 + 16)[0]
    struct.pack_into("<i", raw, 8 + 264 + 16 + 4 + pos_len, pos_len + 4)
    bad = tmp_path / "snap_bad"
    bad.write_bytes(bytes(raw))
    with pytest.raises(ValueError, match="closing length"):
        gs.scan_file(bad)


def test_unknown_binary_blocks_are_not_guessed_as_float(tmp_path):
    parts = _parts()
    rng = np.random.default_rng(9)
    parts[0]["XTRA"] = rng.random((2000, 2))          # gas, two values each
    parts[4]["FOO"] = rng.random(500)                 # stars
    parts[0]["ZZ"] = rng.random(2000)                 # gas and stars, one each:
    parts[4]["ZZ"] = rng.random(500)                  # told apart by the files
    files, _ = _snapshot(tmp_path, parts=parts)
    snap = gs.open_snapshot(files[0])
    assert {"XTRA", "FOO", "ZZ"} <= set(snap.skipped)
    assert "whether values are integers or floats" in dict(snap.skip_reasons)["XTRA"]
    assert "xtra" not in snap.component(0).names


def test_info_block_safely_declares_custom_float_and_integer_blocks(tmp_path):
    e = "<"
    npart = [3, 0, 0, 0, 0, 0]
    head = G._gadget_header(npart, [0.0] * 6, 1.0, 0.0, npart, 1,
                            100.0, 0.0, 0.0, 0.7, e)

    def info(name, kind, dims, types):
        flags = [int(index in types) for index in range(6)]
        return (name.ljust(4).encode("ascii") + kind.ljust(8).encode("ascii")
                + struct.pack(e + "i6i", dims, *flags))

    pos = np.arange(9, dtype=e + "f4").reshape(3, 3)
    flag = np.array([1, 2**31 + 7, 9], dtype=e + "u4")
    custom = np.arange(6, dtype=e + "f4").reshape(3, 2)
    schema = (info("POS", "FLOATN", 3, (0,))
              + info("FLAG", "LONG", 1, (0,))
              + info("XTRA", "FLOATN", 2, (0,)))
    path = tmp_path / "snap_info"
    path.write_bytes(G._gadget_block("HEAD", head, e)
                     + G._gadget_block("INFO", schema, e)
                     + G._gadget_block("POS", pos.tobytes(), e)
                     + G._gadget_block("FLAG", flag.tobytes(), e)
                     + G._gadget_block("XTRA", custom.tobytes(), e))
    snap = gs.open_snapshot(path)
    gas = snap.component(0)
    assert not snap.skipped and "INFO" not in snap.blocks
    assert np.array_equal(gas.column("flag"), flag)
    assert np.array_equal(gas.column("xtra_1"), custom[:, 1])


def test_known_most_bound_ids_are_read_as_integers(tmp_path):
    e = "<"
    npart = [0, 3, 0, 0, 0, 0]
    head = G._gadget_header(npart, [0.0] * 6, 0.5, 1.0, npart, 1,
                            1000.0, 0.3, 0.7, 0.7, e)
    ids = np.array([17, 2**31 + 5, 99], dtype=e + "u4")
    path = tmp_path / "subhalo_ids"
    path.write_bytes(G._gadget_block("HEAD", head, e)
                     + G._gadget_block("MBID", ids.tobytes(), e))
    values = gs.open_snapshot(path).component(1).column("most_bound_id")
    assert values.dtype.kind == "u"
    assert np.array_equal(values, ids)


def test_double_precision_and_long_ids(tmp_path):
    files, parts = _snapshot(tmp_path, num_files=2, double=True, long_ids=True, folder="d")
    snap = gs.open_snapshot(files[0])
    assert snap.blocks["POS"].dtype == "f8" and snap.blocks["ID"].dtype == "u8"
    assert np.array_equal(snap.component(1).column("x"), parts[1]["POS"][:, 0])


def test_hdf5_snapshot_reads_the_same(tmp_path):
    h5py = pytest.importorskip("h5py")
    parts = _parts()
    names = {"POS": "Coordinates", "VEL": "Velocities", "ID": "ParticleIDs", "MASS": "Masses",
             "U": "InternalEnergy", "RHO": "Density", "NE": "ElectronAbundance",
             "HSML": "SmoothingLength", "BHMA": "BH_Mass", "NH": "NeutralHydrogenAbundance"}
    split = [[1000, 2000, 0, 0, 500, 2], [1000, 2000, 0, 0, 0, 0]]
    start = [0] * 6
    for f, npart in enumerate(split):
        with h5py.File(tmp_path / f"snap_004.{f}.hdf5", "w") as h:
            hd = h.create_group("Header")
            hd.attrs["NumPart_ThisFile"] = np.array(npart, dtype=np.int32)
            hd.attrs["NumPart_Total"] = np.array([2000, 4000, 0, 0, 500, 2], dtype=np.uint32)
            hd.attrs["NumPart_Total_HighWord"] = np.zeros(6, dtype=np.uint32)
            hd.attrs["MassTable"] = np.array(MASS_TABLE)
            hd.attrs["Time"], hd.attrs["Redshift"] = 0.5, 1.0
            hd.attrs["BoxSize"], hd.attrs["HubbleParam"] = 1000.0, 0.7
            hd.attrs["NumFilesPerSnapshot"] = 2
            for t in range(6):
                if not npart[t]:
                    continue
                g = h.create_group(f"PartType{t}")
                for block, arr in parts[t].items():
                    if t == 1 and block == "MASS":
                        continue
                    data = np.asarray(arr)[start[t]:start[t] + npart[t]]
                    kw = {"compression": "gzip"} if block == "VEL" else {}
                    g.create_dataset(names[block], data=data.astype(
                        "u8" if block == "ID" else "f4"), **kw)
        start = [start[t] + npart[t] for t in range(6)]
    first = tmp_path / "snap_004.0.hdf5"
    assert gs.detect(first) == (0, "")
    snap = gs.open_snapshot(first)
    assert snap.fmt == "Gadget HDF5" and len(snap.files) == 2
    gas = snap.component(0)
    assert np.array_equal(gas.column("x"), parts[0]["POS"][:, 0].astype("f4"))
    # A compressed dataset is read; a contiguous one memory-mapped.
    assert np.array_equal(gas.column("vy"), parts[0]["VEL"][:, 1].astype("f4"))
    assert snap.pieces[("POS", 0)][0].offset is not None
    assert snap.pieces[("VEL", 0)][0].offset is None
    assert np.allclose(gas.column("temperature"), snap.component(0).temperature())
    assert np.all(snap.component(1).column("mass") == G.GADGET_DM_MASS)
    assert np.array_equal(snap.component(5).column("bh_mass"), parts[5]["BHMA"].astype("f4"))


def _hdf5_header(handle, this_file, total, num_files=1):
    hd = handle.create_group("Header")
    hd.attrs["NumPart_ThisFile"] = np.array(this_file, dtype=np.int32)
    hd.attrs["NumPart_Total"] = np.array(total, dtype=np.uint32)
    hd.attrs["NumPart_Total_HighWord"] = np.zeros(6, dtype=np.uint32)
    hd.attrs["MassTable"] = np.zeros(6)
    hd.attrs["NumFilesPerSnapshot"] = num_files


def test_hdf5_same_field_may_have_a_different_shape_per_particle_type(tmp_path):
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "snap_heterogeneous.hdf5"
    with h5py.File(path, "w") as h:
        _hdf5_header(h, [2, 2, 0, 0, 0, 0], [2, 2, 0, 0, 0, 0])
        gas = h.create_group("PartType0")
        gas.create_dataset("Tracer", data=np.array([3.0, 4.0], dtype="f4"))
        dark = h.create_group("PartType1")
        dark.create_dataset("Tracer", data=np.arange(6, dtype="f8").reshape(2, 3))
    snap = gs.open_snapshot(path)
    assert snap.component(0).names == ["tracer"]
    assert snap.component(1).names == ["tracer_0", "tracer_1", "tracer_2"]
    assert np.array_equal(snap.component(0).column("tracer"), [3.0, 4.0])
    assert np.array_equal(snap.component(1).column("tracer_2"), [2.0, 5.0])
    assert snap.describe()["blocks"]["Tracer"]["by_type"]["1"] == {
        "dims": 3, "dtype": "f8"}


def test_hdf5_multifile_rejects_a_field_missing_from_one_shard(tmp_path):
    h5py = pytest.importorskip("h5py")
    total = [4, 0, 0, 0, 0, 0]
    for index in range(2):
        with h5py.File(tmp_path / f"snap_missing.{index}.hdf5", "w") as h:
            _hdf5_header(h, [2, 0, 0, 0, 0, 0], total, 2)
            gas = h.create_group("PartType0")
            gas.create_dataset("Coordinates", data=np.zeros((2, 3), dtype="f4"))
            if index == 0:
                gas.create_dataset("Density", data=np.ones(2, dtype="f4"))
    with pytest.raises(ValueError, match=r"snap_missing\.1\.hdf5.*missing RHO"):
        gs.open_snapshot(tmp_path / "snap_missing.0.hdf5")


@pytest.mark.parametrize("second,problem", [
    (np.ones((2, 2), dtype="f4"), "shape"),
    (np.ones(2, dtype="f8"), "dtype"),
])
def test_hdf5_multifile_rejects_inconsistent_field_schema(tmp_path, second, problem):
    h5py = pytest.importorskip("h5py")
    total = [4, 0, 0, 0, 0, 0]
    for index, density in enumerate((np.ones(2, dtype="f4"), second)):
        with h5py.File(tmp_path / f"snap_schema.{index}.hdf5", "w") as h:
            _hdf5_header(h, [2, 0, 0, 0, 0, 0], total, 2)
            gas = h.create_group("PartType0")
            gas.create_dataset("Density", data=density)
    with pytest.raises(ValueError, match=problem):
        gs.open_snapshot(tmp_path / "snap_schema.0.hdf5")


def test_hdf5_rejects_a_dataset_row_count_different_from_its_header(tmp_path):
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "snap_bad_rows.hdf5"
    with h5py.File(path, "w") as h:
        _hdf5_header(h, [2, 0, 0, 0, 0, 0], [2, 0, 0, 0, 0, 0])
        gas = h.create_group("PartType0")
        gas.create_dataset("Coordinates", data=np.zeros((1, 3), dtype="f4"))
    with pytest.raises(ValueError, match=r"Coordinates has 1 rows.*announces 2"):
        gs.open_snapshot(path)


# ── Units and derived gas quantities ──────────────────────────────────────────

def test_temperature_and_entropy_follow_the_analytic_sphere(tmp_path):
    files, parts = _snapshot(tmp_path)
    gas = gs.open_snapshot(files[0]).component(0)
    pos = np.stack([gas.column(a) for a in "xyz"], axis=1).astype(float)
    r = np.linalg.norm(pos - G.GADGET_HALO["centre"], axis=1)
    t_true = G.gadget_temperature(r)
    assert np.allclose(gas.column("temperature"), t_true, rtol=2e-4)
    # K = k T / n_e^(2/3), n_e = NE X rho / m_p, rho physical (h^2 / a^3).
    a, h, x = 0.5, 0.7, gs.HYDROGEN_MASS_FRACTION
    rho = G.gadget_density(r) * 1e10 * gs.MSUN_G / gs.KPC_CM ** 3 * h ** 2 / a ** 3
    ne = G.GADGET_HALO["ne"] * x * rho / gs.PROTON_MASS_G
    k_true = gs.BOLTZMANN_ERG_K * t_true / gs.KEV_ERG / ne ** (2 / 3)
    assert np.allclose(gas.column("entropy"), k_true, rtol=5e-4)
    assert gas.units()["temperature"] == "K" and gas.units()["entropy"] == "keV cm^2"
    assert gas.derived["temperature"] == "from U and NE"


def test_without_ne_the_gas_is_fully_ionised_and_says_so(tmp_path):
    parts = _parts()
    del parts[0]["NE"], parts[0]["NH"]
    files, _ = _snapshot(tmp_path, parts=parts, cooling=0)
    gas = gs.open_snapshot(files[0]).component(0)
    assert "fully ionised" in gas.derived["temperature"]
    # The same gas with NE = the fully ionised abundance: the same temperature.
    pos = np.stack([gas.column(a) for a in "xyz"], axis=1).astype(float)
    r = np.linalg.norm(pos - G.GADGET_HALO["centre"], axis=1)
    assert np.allclose(gas.column("temperature"), G.gadget_temperature(r), rtol=2e-4)


def test_units_from_the_run_and_the_environment(tmp_path, monkeypatch):
    files, _ = _snapshot(tmp_path)
    snap = gs.open_snapshot(files[0])
    assert snap.units.describe()["length"] == "kpc" and snap.units.comoving
    assert snap.units.source == "Gadget defaults; comoving (cosmological header)"
    phys = snap.component(1).physical()
    # Lengths to physical kpc (x a / h), velocities peculiar (x sqrt(a)).
    assert phys["x"]["factor"] == pytest.approx(0.5 / 0.7) and phys["x"]["unit"] == "kpc"
    assert phys["vx"]["factor"] == pytest.approx(np.sqrt(0.5))
    assert phys["mass"]["factor"] == pytest.approx(1e10 / 0.7)
    # The run's own parameter file, one folder up.
    (tmp_path / "parameters-usedvalues").write_text(
        "UnitLength_in_cm   3.085678e24\nUnitMass_in_g 1.989e43 % comment\n"
        "UnitVelocity_in_cm_per_s 1e5\nComovingIntegrationOn 1\n")
    gs._cached_snapshot.cache_clear()
    snap = gs.open_snapshot(files[0])
    assert snap.units.describe()["length"] == "Mpc"
    assert "parameters-usedvalues" in snap.units.source
    assert snap.component(0).units()["x"] == "Mpc/h comoving"
    monkeypatch.setenv(gs.UNITS_ENV, "length_cm=3.085678e21,comoving=0")
    gs._cached_snapshot.cache_clear()
    snap = gs.open_snapshot(files[0])
    assert snap.units.describe()["length"] == "kpc" and not snap.units.comoving
    assert snap.scale_factor == 1.0
    assert snap.component(0).physical()["vx"]["factor"] == pytest.approx(1.0)


def test_the_subfind_like_group_file_parses_with_the_same_blocks(tmp_path):
    path = G.write_subfind(tmp_path / "groups_002" / "sub_002.0", [540.0, 500.0, 500.0],
                           250.0, 120.0)
    snap = gs.open_snapshot(path)
    assert snap.blocks["GPOS"].types == (0,) and snap.blocks["SPOS"].types == (1,)
    groups = snap.component(0)
    assert snap.is_subfind and snap.describe()["catalogue_kind"] == "subfind"
    assert groups.name == "FoF groups"
    assert groups.column("x")[0] == 540.0 and groups.column("rvir")[0] == 250.0
    assert groups.column("mvir")[0] == 120.0
    assert groups.column("r500")[0] == 150.0
    assert groups.units()["rvir"] == "kpc/h comoving"
    assert groups.physical()["rvir"]["unit"] == "kpc"
    subhalos = snap.component(1)
    assert subhalos.name == "subhalos" and subhalos.column("x")[0] == 540.0
    assert subhalos.column("vx")[0] == 120.0
    assert subhalos.column("subhalo_mass")[0] == 30.0
    assert gs.gadget_components(path)[0]["name"] == "FoF groups"
    assert gs.gadget_components(path)[1]["name"] == "subhalos"


# ── Classification and the series ────────────────────────────────────────────

def test_classification_by_content_not_name(tmp_path):
    from app.classify import classify_path
    files, _ = _snapshot(tmp_path)
    for target in (files[0], files[2], files[0].parent):
        v = classify_path(target)
        assert v["kind"] == "particles" and v["opener"] == "catalogue" and v["confident"]
        assert v["open_path"] == f"{files[0]}{gs.REF_SEP}0"
        assert [c["name"] for c in v["components"]] == ["gas", "dark matter", "stars",
                                                        "black holes"]
        assert v["components"][1]["num_rows"] == 4000
    f1, _ = _snapshot(tmp_path, fmt=1, endian=">", num_files=1, folder="f1",
                      name="anything.dat")
    assert classify_path(f1[0])["kind"] == "particles"
    other = tmp_path / "snap_noise"
    other.write_bytes(np.random.default_rng(1).bytes(4096))
    assert classify_path(other)["kind"] != "particles"
    files[1].unlink()
    v = classify_path(files[0])
    assert v["opener"] == "unsupported" and "missing" in v["detail"]


def test_series_over_snapdirs(tmp_path):
    from app.snapshot_series import snapshot_series
    for k in range(3):
        G.write_gadget(tmp_path / f"snapdir_{k:03d}" / f"snap_{k:03d}", _parts(seed=k),
                       num_files=2, mass_table=MASS_TABLE)
    s = snapshot_series(str(tmp_path / "snapdir_001" / "snap_001.0"))
    assert s["count"] == 3 and s["index"] == 1 and s["numbers"] == [0, 1, 2]
    assert s["paths"][2] == str(tmp_path / "snapdir_002" / "snap_002.0")
    # Single files in one folder: the existing series.
    for k in range(2):
        G.write_gadget(tmp_path / "flat" / f"snap_{k:03d}", _parts(seed=k), mass_table=MASS_TABLE)
    assert snapshot_series(str(tmp_path / "flat" / "snap_000"))["count"] == 2


# ── The same routes as a VBT point table ──────────────────────────────────────

def _vbt_of(component, path: Path) -> str:
    """The component written as a VBT point table: the equivalent dataset."""
    names = ["x", "y", "z", "mass", "u"] if "u" in component.names else ["x", "y", "z", "mass"]
    data = np.stack([np.asarray(component.column(n), dtype="<f4") for n in names])
    data.tofile(path)
    Path(str(path) + ".head").write_text(
        f"float\n{len(names)}\n{component.num_rows}\nlittle\n" + "\n".join(names) + "\n")
    return str(path) + ".head"


def test_point_table_and_samples_equal_the_equivalent_vbt(tmp_path):
    from app.point_sample import sample_points
    from app.point_table import open_point_table
    files, _ = _snapshot(tmp_path)
    ref = f"{files[0]}{gs.REF_SEP}0"
    table = open_point_table(ref)
    vbt = open_point_table(_vbt_of(table_component := gs.resolve(ref), tmp_path / "gas.bin"))
    assert table.axes == ("x", "y", "z") and table.num_rows == vbt.num_rows == 2000
    a = sample_points(table, 500, colour_field="u", colour_log=True)
    b = sample_points(vbt, 500, colour_field="u", colour_log=True)
    assert a.encode() == b.encode() and a.bounds == b.bounds
    assert table_component.name == "gas"


@pytest.mark.asyncio
async def test_routes_open_and_draw_a_snapshot(client, auth_headers, tmp_path):
    files, parts = _snapshot(tmp_path / "run" / "a", folder="snapdir_001", name="snap_001")
    _snapshot(tmp_path / "run" / "a", folder="snapdir_002", name="snap_002", parts=_parts(seed=2))
    first = files[0]
    r = (await client.post("/v1/files/classify", json={"path": str(first.parent)},
                           headers=auth_headers)).json()
    assert r["opener"] == "catalogue" and len(r["components"]) == 4
    # Opened by its file, with the format a client guesses from a name: the
    # content decides.
    cat = (await client.post("/v1/catalogue/open", json={"path": str(first), "format": "csv"},
                             headers=auth_headers)).json()
    assert cat["valid"], cat
    assert cat["format"] == "gadget" and cat["particles"] and cat["num_rows"] == 2000
    assert cat["path"] == f"{first}{gs.REF_SEP}0"
    assert cat["coordinate_fields"]["system"] == "cartesian"
    assert cat["snapshot"]["redshift"] == pytest.approx(1.0)
    assert cat["component"]["units"]["x"] == "kpc/h comoving"
    assert {f["name"] for f in cat["fields"]} >= {"x", "u", "temperature", "entropy"}
    did = cat["dataset_id"]
    # Another particle type by its reference (a Session Data row, Recent).
    dm_ref = cat["components"][1]["path"]
    r = (await client.post("/v1/files/classify", json={"path": dm_ref}, headers=auth_headers)).json()
    assert r["opener"] == "catalogue" and r["open_path"] == dm_ref
    dm = (await client.post("/v1/catalogue/open", json={"path": dm_ref, "format": "csv"},
                            headers=auth_headers)).json()
    assert dm["valid"] and dm["num_rows"] == 4000
    bad = (await client.post("/v1/catalogue/open",
                             json={"path": f"{first}{gs.REF_SEP}2", "format": "csv"},
                             headers=auth_headers)).json()
    assert not bad["valid"]
    # The VR page's routes.
    info = (await client.post("/v1/points/info", json={"dataset_id": did},
                              headers=auth_headers)).json()
    assert info["particles"] and info["format"] == "gadget" and info["axes"] == ["x", "y", "z"]
    assert info["units"]["temperature"] == "K"
    r = await client.post("/v1/points/sample_bin", json={"dataset_id": did, "max_points": 1000,
                                                          "color_field": "temperature"},
                          headers=auth_headers)
    assert r.status_code == 200 and len(r.content) == 1000 * 8
    s = (await client.post("/v1/points/series", json={"dataset_id": did},
                           headers=auth_headers)).json()
    assert s["count"] == 2 and s["index"] == 0
    assert s["paths"][1] == str(tmp_path / "run" / "a" / "snapdir_002" / "snap_002.0") + gs.REF_SEP + "0"
    # An analysis route: the catalogue query (filters on a derived column).
    q = (await client.post("/v1/catalogue/query", json={
        "dataset_id": did, "filters": [{"field": "temperature", "op": ">", "value": "2e7"}],
        "limit": 10}, headers=auth_headers)).json()
    assert q["valid"], q
    assert 0 < q["total_rows"] < 2000


@pytest.mark.asyncio
async def test_normal_particle_open_and_query_never_build_the_full_frame(
        client, auth_headers, tmp_path, monkeypatch):
    from app import point_table
    files, _ = _snapshot(tmp_path)
    monkeypatch.setattr(point_table, "particle_frame",
                        lambda *_args, **_kw: (_ for _ in ()).throw(
                            AssertionError("full particle frame materialised")))
    opened = (await client.post(
        "/v1/catalogue/open", json={"path": str(files[0]), "format": "gadget"},
        headers=auth_headers)).json()
    assert opened["valid"], opened
    queried = (await client.post(
        "/v1/catalogue/query",
        json={"dataset_id": opened["dataset_id"], "limit": 100,
              "random_sample": True,
              "fields": ["x", "y", "z", "temperature"],
              "filters": [{"field": "x", "op": ">", "value": "0"}]},
        headers=auth_headers)).json()
    assert queried["valid"] and queried["returned_rows"] == 100
    assert set(queried["rows"][0]) == {"x", "y", "z", "temperature"}

    # Sorting is out-of-core too: particle_frame remains forbidden by the
    # monkeypatch above, while the requested page is correctly ordered.
    sorted_query = (await client.post(
        "/v1/catalogue/query",
        json={"dataset_id": opened["dataset_id"], "limit": 100,
              "fields": ["x", "rho"],
              "filters": [{"field": "rho", "op": ">", "value": "0"}],
              "sort": {"field": "x", "direction": "asc"}},
        headers=auth_headers)).json()
    assert sorted_query["valid"], sorted_query
    values = [row["x"] for row in sorted_query["rows"]]
    assert values == sorted(values)
    assert set(sorted_query["rows"][0]) == {"x", "rho"}


def test_the_render_route_opens_a_remote_session_on_a_particle_type(monkeypatch):
    from app.routers import render as render_router
    seen = {}

    class _Sess:
        session_id = "s" * 32

        class view:
            width, height = 800, 600

        async def render_frame(self):
            return b"jpeg"

        def levels(self):
            return {"threshold": None, "data_min": None, "data_max": None, "pending": False}

    class _Manager:
        async def start(self):
            pass

        async def create_session(self, **kw):
            seen.update(kw)
            return _Sess()

    ref = "/data/snapdir_002/snap_002.0::gadget-type=1"

    class _VisivoSession:
        datasets = {"cat_1": {"kind": "catalogue", "format": "gadget", "path": ref}}

    monkeypatch.setattr(render_router, "server_side_rendering_available", lambda: True)
    monkeypatch.setattr(render_router, "get_render_manager", lambda: _Manager())
    reply = asyncio.new_event_loop().run_until_complete(render_router.create_session(
        render_router.CreateRenderSessionRequest(dataset_id="cat_1"), token="tok",
        session=_VisivoSession()))
    assert reply.valid
    assert seen["dataset_path"] == ref
    assert seen["factory"].endswith(":PointRenderSession")


def test_the_ray_traced_render_equals_the_equivalent_vbt(tmp_path):
    pytest.importorskip("numba")
    from app.routers import particle_render as router
    files, _ = _snapshot(tmp_path)
    ref = f"{files[0]}{gs.REF_SEP}0"
    head = _vbt_of(gs.resolve(ref), tmp_path / "gas.bin")
    cam = {"position": (500, 500, 2500), "focal_point": (500, 500, 500), "view_up": (0, 1, 0)}
    req = router.ParticleRenderRequest(dataset_id="d", width=96, height=64, cameras=[cam],
                                       colour={"field": "u", "log": True})
    frames = []
    for entry in ({"kind": "catalogue", "format": "gadget", "path": ref},
                  {"kind": "catalogue", "format": "vbt", "path": head}):
        particles = router._prepare(entry, req)
        frames.append(router._frame(particles, req, req.cameras[0], 96, 64, req.tone, None)[1])
    assert frames[0].sum() > 0
    assert np.array_equal(frames[0], frames[1])


def test_the_point_session_opens_a_particle_type(tmp_path):
    pytest.importorskip("vtk")
    from app.render_session.points import PointRenderSession
    files, _ = _snapshot(tmp_path)
    s = PointRenderSession("g" * 32, f"{files[0]}{gs.REF_SEP}4")
    s._open_table()
    assert s._num_rows == 500 and (s.view.x, s.view.y, s.view.z) == ("x", "y", "z")
    assert "mass" in s._columns


def test_the_copilot_sees_the_particle_type(tmp_path):
    from app.assistant.tools import _catalogue_summary, _list_datasets
    from app.dependencies import _catalogue_metadata
    from app.point_table import particle_components
    files, _ = _snapshot(tmp_path)
    ref = f"{files[0]}{gs.REF_SEP}0"
    frame, meta = _catalogue_metadata(Path(ref), "gadget")
    entry = {"path": ref, "paths": [str(files[0])], "dataframe": frame, **meta,
             "components": particle_components(ref)}
    out = _catalogue_summary(ref, {}, entry)
    assert out["format"] == "gadget" and out["particles"]
    assert out["snapshot"]["redshift"] == pytest.approx(1.0)
    assert out["units"]["temperature"] == "K"
    assert out["component"]["name"] == "gas"
    assert [c["name"] for c in out["other_components"]][:2] == ["gas", "dark matter"]
    listing = _list_datasets(None, {}, {"_datasets": {"cat_1": {**entry, "kind": "catalogue"}},
                                        "_active_id": "cat_1"})
    row = listing["datasets"][0]
    assert row["format"] == "gadget" and row["component"] == "gas" and row["num_rows"] == 2000


def test_the_product_cache_keys_on_every_file_of_the_snapshot(tmp_path):
    from app.product_cache import file_identity
    files, _ = _snapshot(tmp_path)
    ref = f"{files[0]}{gs.REF_SEP}0"
    before = file_identity(ref)
    assert before.count("|") == 2
    import os
    st = os.stat(files[2])
    os.utime(files[2], ns=(st.st_atime_ns, st.st_mtime_ns + 10**9))
    assert file_identity(ref) != before


def test_the_remote_point_session_draws_it_as_the_equivalent_vbt(tmp_path):
    """The remote 3-D view (a point session on the backend) of a particle type
    and of the same particles as a VBT: the same frame."""
    pytest.importorskip("vtk")
    import io
    import subprocess
    import sys

    from PIL import Image

    from tests.test_point_render import _RENDER_PROBE, _session
    probe = subprocess.run([sys.executable, "-c", _RENDER_PROBE], capture_output=True,
                           timeout=60, cwd=Path(__file__).resolve().parents[1])
    if probe.returncode != 0:
        pytest.skip("no offscreen OpenGL here (no GPU or display)")
    files, _ = _snapshot(tmp_path)
    ref = f"{files[0]}{gs.REF_SEP}0"
    head = _vbt_of(gs.resolve(ref), tmp_path / "gas.bin")
    frames = []
    for path in (ref, head):
        s, run, loop = _session(path)
        try:
            run(s.apply_command({"type": "set_color", "field": "u", "lut": "viridis"}))
            frames.append(run(s.render_frame()))
        finally:
            run(s.close())
            loop.close()
    a, b = (np.asarray(Image.open(io.BytesIO(f)).convert("RGB"), dtype=float) for f in frames)
    assert a.shape == b.shape and a.std() > 0
    assert np.abs(a - b).max() <= 2.0


@pytest.mark.asyncio
async def test_an_unreadable_snapshot_says_why(client, auth_headers, tmp_path):
    files, _ = _snapshot(tmp_path)
    files[2].unlink()
    for path in (str(files[0]), f"{files[0]}{gs.REF_SEP}1"):
        r = (await client.post("/v1/catalogue/open", json={"path": path, "format": "csv"},
                               headers=auth_headers)).json()
        assert not r["valid"] and "missing" in r["error"] and "snap_002.2" in r["error"]
    v = (await client.post("/v1/files/classify", json={"path": f"{files[0]}{gs.REF_SEP}1"},
                           headers=auth_headers)).json()
    assert v["opener"] == "unsupported" and "missing" in v["detail"]
    v = (await client.post("/v1/files/classify",
                           json={"path": str(tmp_path / "nope" / f"snap_0.0{gs.REF_SEP}1")},
                           headers=auth_headers)).json()
    assert not v["valid"]


def _counts(gas, dm, stars=0, bh=0, seed=7):
    """Particles of each type with POS VEL ID only, so blocks are added by hand."""
    rng = np.random.default_rng(seed)
    out = {}
    for t, n in ((0, gas), (1, dm), (4, stars), (5, bh)):
        if n:
            out[t] = {"POS": rng.random((n, 3)) * 10, "VEL": rng.random((n, 3)),
                      "ID": np.arange(n) + 1000 * t}
    return out


def test_a_known_multi_value_block_reads_with_its_own_types(tmp_path):
    # Gadget-3's Zs: gas and stars, 11 metals each; 2 black holes, which
    # divide almost any size, must not be taken for its owners.
    parts = _counts(600, 900, 150, 2)
    rng = np.random.default_rng(1)
    parts[0]["Zs"] = rng.random((600, 11))
    parts[4]["Zs"] = rng.random((150, 11))
    files, _ = _snapshot(tmp_path, parts=parts, num_files=1, folder="zs", name="snap_000")
    snap = gs.open_snapshot(files[0])
    spec = snap.blocks["Zs"]
    assert spec.types == (0, 4) and spec.dims == 11 and not spec.inferred
    assert np.array_equal(snap.component(4).column("metals_10"),
                          parts[4]["Zs"][:, 10].astype("f4"))


def test_an_unknown_block_is_left_out_when_its_size_is_ambiguous(tmp_path):
    # Equal gas and dark matter in one file: a gas block of 2 values reads as
    # well as a gas+dark-matter block of 1. Left out, with the reason.
    parts = _counts(500, 500, 0, 2)
    parts[0]["QQ"] = np.random.default_rng(2).random((500, 2))
    files, _ = _snapshot(tmp_path, parts=parts, num_files=1, folder="amb", name="snap_000")
    snap = gs.open_snapshot(files[0])
    assert "QQ" not in snap.blocks and "QQ" in snap.skipped
    assert "integers or floats" in dict(snap.skip_reasons)["QQ"]
    assert "QQ" in snap.describe()["skipped_reasons"]


def test_unknown_blocks_are_not_guessed_even_when_types_fit(tmp_path):
    # The same kinds of block in three files with different mixtures of
    # types: only one reading fits them all.
    parts = _counts(600, 600, 150, 2)
    rng = np.random.default_rng(3)
    parts[0]["QQ"] = rng.random((600, 2))                 # gas, 2 values
    parts[0]["GS3"] = rng.random((600, 3))                # gas+stars, 3 values
    parts[4]["GS3"] = rng.random((150, 3))
    parts[5]["BHX"] = rng.random(2)                       # black holes
    split = [[300, 100, 0, 0, 150, 2], [200, 300, 0, 0, 0, 0], [100, 200, 0, 0, 0, 0]]
    files = G.write_gadget(tmp_path / "multi" / "snap_000", parts, num_files=3, split=split,
                           mass_table=MASS_TABLE)
    snap = gs.open_snapshot(files[0])
    assert {"QQ", "GS3", "BHX"} <= set(snap.skipped)
    assert all("integers or floats" in dict(snap.skip_reasons)[name]
               for name in ("QQ", "GS3", "BHX"))


@pytest.mark.parametrize("time", [0.0, 2.0])
def test_a_run_that_is_not_cosmological_is_not_comoving(tmp_path, time):
    # An isolated galaxy: the header's time is a time (0 for the initial
    # conditions), no matter density, no redshift.
    files, parts = _snapshot(tmp_path, num_files=1, folder=f"iso{time}", name="snap_000",
                             time=time, redshift=0.0, omega0=0.0, omega_l=0.0)
    snap = gs.open_snapshot(files[0])
    assert snap.units.comoving is False and snap.scale_factor == 1.0
    gas = snap.component(0)
    phys = gas.physical()
    assert phys["x"]["factor"] == pytest.approx(1 / 0.7) and phys["vx"]["factor"] == 1.0
    assert phys["rho"]["factor"] == pytest.approx(1e10 * gs.MSUN_G / gs.KPC_CM ** 3 * 0.49)
    assert gas.units()["x"] == "kpc/h"
    assert np.all(np.isfinite(gas.column("entropy")))
    from app.point_table import particle_frame
    assert len(particle_frame(f"{files[0]}{gs.REF_SEP}0")) == 2000


def test_a_cosmological_header_without_parameters_is_comoving(tmp_path):
    files, _ = _snapshot(tmp_path, num_files=1, folder="cosmo", name="snap_000", time=0.25)
    snap = gs.open_snapshot(files[0])
    assert snap.units.comoving and snap.scale_factor == 0.25
    # The parameter file says otherwise: it wins.
    (tmp_path / "cosmo" / "parameters-usedvalues").write_text("ComovingIntegrationOn 0\n")
    assert gs.open_snapshot(files[0]).units.comoving is False


def test_the_parameter_file_is_part_of_the_cache_keys(tmp_path):
    import os

    from app.product_cache import file_identity
    files, _ = _snapshot(tmp_path, num_files=1, folder="p", name="snap_000")
    ref = f"{files[0]}{gs.REF_SEP}0"
    params = tmp_path / "p" / "parameters-usedvalues"
    params.write_text("UnitLength_in_cm 3.085678e21\n")
    before = file_identity(ref)
    assert gs.open_snapshot(files[0]).units.describe()["length"] == "kpc"
    params.write_text("UnitLength_in_cm 3.085678e24\n")
    st = params.stat()
    os.utime(params, ns=(st.st_atime_ns, st.st_mtime_ns + 10 ** 9))
    # No cache_clear: the snapshot is scanned again for its new units.
    assert gs.open_snapshot(files[0]).units.describe()["length"] == "Mpc"
    assert file_identity(ref) != before


def test_a_compressed_hdf5_dataset_is_read_once_for_its_three_columns(tmp_path, monkeypatch):
    h5py = pytest.importorskip("h5py")
    path = tmp_path / "snap_007.hdf5"
    with h5py.File(path, "w") as h:
        hd = h.create_group("Header")
        hd.attrs["NumPart_ThisFile"] = np.array([0, 100, 0, 0, 0, 0], dtype=np.int32)
        hd.attrs["NumPart_Total"] = np.array([0, 100, 0, 0, 0, 0], dtype=np.uint32)
        hd.attrs["MassTable"] = np.array(MASS_TABLE)
        hd.attrs["NumFilesPerSnapshot"] = 1
        g = h.create_group("PartType1")
        g.create_dataset("Coordinates", data=np.random.default_rng(0).random((100, 3)),
                         compression="gzip")
    reads = []
    real = gs.Piece.array
    monkeypatch.setattr(gs.Piece, "array", lambda self: reads.append(self) or real(self))
    dm = gs.open_snapshot(path).component(1)
    for axis in "xyz":
        dm.column(axis)
    assert len(reads) == 1


def test_records_past_2_and_4_gib_are_read_by_their_true_length(tmp_path):
    # Sparse files: a POS block of 3 GiB (its marker has the high bit set) and
    # one past 4 GiB (its marker wraps). Only the markers are read.
    for n in ((3 << 30) // 12, (1 << 32) // 12 + 1000):
        nbytes = 12 * n
        e = "<"
        head = G._gadget_header([0, n, 0, 0, 0, 0], MASS_TABLE, 0.5, 1.0, [0, n, 0, 0, 0, 0],
                                1, 1000.0, 0.3, 0.7, 0.7, e)
        path = tmp_path / f"big_{n}"
        marker = np.array([nbytes % (1 << 32)], dtype="<u4").tobytes()
        label = b"POS " + np.array([(nbytes + 8) % (1 << 32)], dtype="<u4").tobytes()
        with open(path, "wb") as fh:
            fh.write(G._gadget_block("HEAD", head, e))
            fh.write(G._gadget_record(label, e))
            fh.write(marker)
            fh.seek(nbytes, 1)
            fh.write(marker)
        layout = gs.scan_file(path)
        assert [(b.name, b.nbytes) for b in layout.blocks] == [("POS", nbytes)]


def test_initial_conditions_with_no_file_count_and_nul_padded_names(tmp_path):
    parts = _counts(0, 50)
    files, _ = _snapshot(tmp_path, parts=parts, num_files=1, folder="ic", name="ics")
    raw = bytearray(files[0].read_bytes())
    struct.pack_into("<i", raw, 8 + 8 + 4 + 124, 0)          # num_files = 0
    raw[16 + 264 + 4 + 3] = 0                                # "POS " -> "POS\0"
    files[0].write_bytes(bytes(raw))
    snap = gs.open_snapshot(files[0])
    assert snap.header.num_files == 1 and "POS" in snap.blocks


def test_a_snapshot_file_outside_the_data_roots_is_refused(tmp_path, monkeypatch):
    from app.dependencies import _within_data_roots
    from app.point_table import particle_dataset_path
    root = tmp_path / "root"
    files, _ = _snapshot(root)
    outside = tmp_path / "outside"
    outside.mkdir()
    moved = outside / files[2].name
    files[2].rename(moved)
    files[2].symlink_to(moved)
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(root))
    with pytest.raises(PermissionError):
        particle_dataset_path(str(files[0]), _within_data_roots)


def test_classifying_a_directory_does_not_follow_an_external_snapshot_symlink(
        tmp_path, monkeypatch):
    from app.classify import classify_path
    from app.dependencies import _within_data_roots
    outside = tmp_path / "outside"
    files, _ = _snapshot(outside, num_files=1, folder=".", name="snap_external")
    root = tmp_path / "root"
    root.mkdir()
    (root / "snap_external").symlink_to(files[0])
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(root))
    verdict = classify_path(root, _within_data_roots)
    assert verdict["kind"] == "directory"
    assert "external" not in str(verdict)


@pytest.mark.asyncio
async def test_classify_endpoint_does_not_leak_an_external_snapshot_symlink(
        client, auth_headers, tmp_path, monkeypatch):
    outside = tmp_path / "outside"
    files, _ = _snapshot(outside, num_files=1, folder=".", name="snap_secret")
    root = tmp_path / "root"
    root.mkdir()
    (root / "snap_secret").symlink_to(files[0])
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(root))
    reply = (await client.post("/v1/files/classify", json={"path": str(root)},
                               headers=auth_headers)).json()
    assert reply["valid"]
    assert reply["kind"] == "directory"
    assert "secret" not in str(reply)


def test_a_type_without_mass_block_or_table_has_no_mass_column(tmp_path):
    parts = _parts()
    del parts[4]["MASS"]
    files, _ = _snapshot(tmp_path, parts=parts)
    stars = gs.open_snapshot(files[0]).component(4)
    assert "mass" not in stars.names


# ── Review of the out-of-core reader (issue #445) ────────────────────────────

def _info_entry(name, kind, dims, types, e="<"):
    flags = [int(index in types) for index in range(6)]
    return (name.ljust(4).encode("ascii") + kind.ljust(8).encode("ascii")
            + struct.pack(e + "i6i", dims, *flags))


def _info_snapshot(path, schema, blocks, npart=(3, 0, 0, 0, 0, 0), e="<"):
    npart = list(npart)
    head = G._gadget_header(npart, [0.0] * 6, 1.0, 0.0, npart, 1, 100.0, 0.0, 0.0, 0.7, e)
    raw = G._gadget_block("HEAD", head, e) + G._gadget_block("INFO", schema, e)
    for name, values in blocks:
        raw += G._gadget_block(name, values.tobytes(), e)
    path.write_bytes(raw)
    return gs.open_snapshot(path)


def test_info_float_takes_its_size_from_the_block_beside_double_positions(tmp_path):
    # OpenGadget3 / P-Gadget3 with double positions: INFO says DOUBLEN for POS
    # and FLOAT for RHO, which is single precision.
    pos = np.arange(9, dtype="<f8").reshape(3, 3)
    rho = np.array([1.5, 2.5, 3.5], dtype="<f4")
    hsml = np.array([0.25, 0.5, 0.75], dtype="<f8")       # a FLOAT block in double
    snap = _info_snapshot(
        tmp_path / "mixed",
        _info_entry("POS", "DOUBLEN", 3, (0,)) + _info_entry("RHO", "FLOAT", 1, (0,))
        + _info_entry("HSML", "FLOAT", 1, (0,)),
        [("POS", pos), ("RHO", rho), ("HSML", hsml)])
    gas = snap.component(0)
    assert not snap.skipped
    assert snap.blocks["POS"].dtype == "f8" and snap.blocks["RHO"].dtype == "f4"
    assert snap.blocks["HSML"].dtype == "f8"
    assert np.array_equal(gas.column("rho"), rho)
    assert np.array_equal(gas.take("rho", [2, 0]), rho[[2, 0]])
    assert np.array_equal(gas.column("hsml"), hsml)
    assert np.array_equal(gas.column("z"), pos[:, 2])


def test_an_info_entry_that_does_not_fit_costs_its_block_not_the_snapshot(tmp_path):
    pos = np.arange(9, dtype="<f4").reshape(3, 3)
    odd = np.arange(3, dtype="<f4")
    snap = _info_snapshot(
        tmp_path / "odd",
        _info_entry("POS", "FLOATN", 3, (0,)) + _info_entry("RHO", "DOUBLE", 1, (0,))
        + _info_entry("QUUX", "COMPLEX", 1, (0,)) + _info_entry("HSML", "FLOAT", 1, (0,)),
        [("POS", pos), ("RHO", odd), ("QUUX", odd), ("HSML", np.arange(6, dtype="<u1"))])
    reasons = dict(snap.skip_reasons)
    assert set(snap.skipped) == {"RHO", "QUUX", "HSML"}
    assert "requiring 24 bytes; the block has 12" in reasons["RHO"]
    assert "'COMPLEX'" in reasons["QUUX"]
    assert "neither float nor double" in reasons["HSML"]
    assert snap.component(0).names == ["x", "y", "z"]
    # The file browser says which blocks were left out and why.
    from app.classify import classify_path
    detail = classify_path(tmp_path / "odd")["detail"]
    assert "RHO (INFO declares f8" in detail and "QUUX (" in detail


def _compressed_hdf5(folder, n_per_file, num_files, chunk):
    h5py = pytest.importorskip("h5py")
    folder.mkdir(parents=True, exist_ok=True)
    for i in range(num_files):
        with h5py.File(folder / f"snap.{i}.hdf5", "w") as h:
            _hdf5_header(h, [n_per_file, 0, 0, 0, 0, 0],
                         [n_per_file * num_files, 0, 0, 0, 0, 0], num_files)
            g = h.create_group("PartType0")
            rng = np.random.default_rng(i)
            g.create_dataset("Coordinates", data=rng.random((n_per_file, 3), dtype="f4"),
                             compression="gzip", chunks=(chunk, 3))
            g.create_dataset("Density", data=rng.random(n_per_file, dtype="f4"),
                             compression="gzip", chunks=(chunk,))
    return gs.gadget_ref(folder / "snap.0.hdf5", 0)


def test_compressed_hdf5_through_take_decompresses_each_shard_once(tmp_path, monkeypatch):
    """A filter walking the table in chunks, a sample placing x, y and z in
    slices and a sorted page all read each compressed shard once: before
    #445 every take decompressed the whole shard again (48 reads for 4)."""
    from app import point_sample
    from app.point_table import open_point_table
    from app.routers import catalogue as cat
    ref = _compressed_hdf5(tmp_path / "h5", 6000, 2, 512)
    gs.PIECE_CACHE.clear()
    monkeypatch.delenv(gs.PIECE_CACHE_ENV, raising=False)
    reads = []
    real = gs.Piece.array
    monkeypatch.setattr(gs.Piece, "array",
                        lambda self: reads.append((self.path, self.dataset)) or real(self))
    monkeypatch.setattr(cat, "PARTICLE_CHUNK_ROWS", 1000)
    monkeypatch.setattr(point_sample, "_POS_CHUNK", 700)
    component = gs.resolve(ref)
    expected_x = gs.resolve(ref).column("x")
    density = gs.resolve(ref).column("rho")
    reads.clear()

    rows = np.random.default_rng(3).permutation(component.num_rows)[:9000]
    placed = point_sample.place_rows(open_point_table(ref).columns, ("x", "y", "z"), rows)
    assert np.allclose(placed.xyz[:, 0] + placed.origin[0], expected_x[placed.rows], atol=1e-6)
    filt = type("F", (), {"field": "rho", "op": ">", "value": "0.5", "box": None,
                          "sphere": None})()
    frame, total, idx = cat._particle_filtered_page(ref, [filt], 50, 0, True, False, 0,
                                                    ["x", "rho"])
    assert (frame["rho"] > 0.5).all() and len(frame) == 50
    srt = type("S", (), {"field": "rho", "direction": "desc"})()
    frame, _, _ = cat._particle_sorted_page(ref, [], srt, 20, 30, ["rho"])
    assert np.array_equal(frame["rho"].to_numpy(), np.sort(density)[::-1][30:50])
    # 2 files x (Coordinates, Density): each decompressed once in all.
    assert len(reads) == len(set(reads)) == 4


def test_hdf5_larger_than_the_cache_is_read_by_bounded_windows(tmp_path, monkeypatch):
    ref = _compressed_hdf5(tmp_path / "big", 5000, 2, 256)
    gs.PIECE_CACHE.clear()
    monkeypatch.setenv(gs.PIECE_CACHE_ENV, "0")
    monkeypatch.setattr(gs, "WINDOW_BYTES", 1024 * 4)         # 1024 rows of one column
    windows = []
    real = gs._read_window
    monkeypatch.setattr(gs, "_read_window", lambda d, a, b, c: windows.append((a, b))
                        or real(d, a, b, c))
    component = gs.resolve(ref)
    full = gs.resolve(ref).column("rho")
    x_full = gs.resolve(ref).column("x")
    windows.clear()
    # A contiguous slice reads its own rows only, never the whole shard.
    assert np.array_equal(component.take("rho", np.arange(4800, 5300)), full[4800:5300])
    assert sum(b - a for a, b in windows) == 500
    assert all(b - a <= 1024 for a, b in windows)
    windows.clear()
    rows = np.random.default_rng(1).integers(0, component.num_rows, 3000)
    assert np.array_equal(component.take("x", rows), x_full[rows])
    # Coordinates: 12 bytes a row, 4 KiB windows -> one 256-row HDF5 chunk
    # each; every window read once per shard, none twice.
    assert all(b - a <= 256 for a, b in windows)
    assert len(windows) <= 2 * -(-5000 // 256)


def test_take_equals_column_for_every_field_derived_ones_too(tmp_path):
    for endian in "<>":
        files, _ = _snapshot(tmp_path / f"e{endian == '<'}", endian=endian)
        snap = gs.open_snapshot(files[0])
        for ptype in (0, 1, 4, 5):
            component = snap.component(ptype)
            rows = np.random.default_rng(ptype).permutation(component.num_rows)
            for name in component.names:
                full = component.column(name)
                got = component.take(name, rows)
                assert got.dtype == full.dtype.newbyteorder("="), (ptype, name)
                assert np.array_equal(got, full[rows], equal_nan=True), (ptype, name)
        assert {"temperature", "entropy"} <= set(snap.component(0).names)


def test_a_segmented_column_indexes_as_numpy_does(tmp_path):
    files, _ = _snapshot(tmp_path)
    component = gs.open_snapshot(files[0]).component(0)
    seg, full = component.columns()["x"], component.column("x")
    mask = np.zeros(component.num_rows, bool)
    mask[[5, 10]] = True
    assert np.array_equal(seg[mask], full[mask])
    assert seg[-1] == full[-1] and seg[3] == full[3]
    assert np.array_equal(seg[[-1, 0, -2]], full[[-1, 0, -2]])
    assert np.array_equal(seg[-3:], full[-3:])
    assert seg[[]].size == 0
    for bad in (np.array([1.0, 2.0]), mask[:-1], component.num_rows, -component.num_rows - 1):
        with pytest.raises(IndexError):
            seg[bad]


def _sorted_pages(ref, direction, page, chunk, monkeypatch, filters=()):
    from app.routers import catalogue as cat
    monkeypatch.setattr(cat, "PARTICLE_CHUNK_ROWS", chunk)
    sort = type("S", (), {"field": "mass", "direction": direction})()
    seen, total = [], None
    offset = 0
    while True:
        _, total, rows = cat._particle_sorted_page(ref, list(filters), sort, page, offset,
                                                   ["mass"])
        if not len(rows):
            return seen, total
        seen.extend(int(r) for r in rows)
        offset += page


@pytest.mark.parametrize("direction", ["asc", "desc"])
def test_sorted_pages_on_ties_are_slices_of_one_sequence(tmp_path, monkeypatch, direction):
    files, _ = _snapshot(tmp_path)
    ref = gs.gadget_ref(files[0], 1)                 # dark matter: one mass for all
    seen, total = _sorted_pages(ref, direction, 500, 700, monkeypatch)
    assert total == 4000 and len(seen) == 4000
    assert seen == list(range(4000))                 # ties by row, ascending, both ways
    # Distinct values and ties together (gas masses rounded), across pages.
    gas = gs.gadget_ref(files[0], 0)
    mass = gs.resolve(gas).column("mass")
    seen, _ = _sorted_pages(gas, direction, 300, 450, monkeypatch)
    key = np.round(mass, 7)
    assert len(seen) == len(set(seen)) == len(mass)
    values = mass[seen]
    assert list(values) == sorted(values, reverse=direction == "desc")


def test_sorted_order_puts_nan_last_and_breaks_ties_by_row():
    from app.routers.catalogue import _sorted_order
    values = np.array([1.0, np.nan, 2.0, 1.0, 2.0, np.nan])
    rows = np.array([10, 11, 12, 13, 14, 15])
    assert rows[_sorted_order(values, rows, False)].tolist() == [10, 13, 12, 14, 11, 15]
    assert rows[_sorted_order(values, rows, True)].tolist() == [12, 14, 10, 13, 11, 15]
    ids = np.array([3, 1, 3, 2], dtype=np.uint64)
    assert _sorted_order(ids, np.arange(4), True).tolist() == [0, 2, 3, 1]


@pytest.mark.asyncio
async def test_in_and_contains_filters_match_the_loaded_catalogue(client, auth_headers, tmp_path):
    from app.dependencies import _apply_tabular_query
    from app.point_table import particle_frame
    from app.schemas import TabularFilter
    files, _ = _snapshot(tmp_path)
    ref = gs.gadget_ref(files[0], 0)
    opened = (await client.post("/v1/catalogue/open", json={"path": ref, "format": "gadget"},
                                headers=auth_headers)).json()
    assert opened["valid"], opened
    frame = particle_frame(ref)
    for flt in ({"field": "id", "op": "in", "value": "5, 17,1999"},
                {"field": "id", "op": "in", "value": "5,abc"},
                {"field": "id", "op": "contains", "value": "99"}):
        reply = (await client.post("/v1/catalogue/query", json={
            "dataset_id": opened["dataset_id"], "filters": [flt], "limit": 0,
            "fields": ["id"]}, headers=auth_headers)).json()
        assert reply["valid"], reply
        expected, total = _apply_tabular_query(frame, [TabularFilter(**flt)], None, 0, 0)
        assert reply["total_rows"] == total > 0 or flt["value"] == "5,abc"
        assert [row["id"] for row in reply["rows"]] == expected["id"].tolist()
    bad = (await client.post("/v1/catalogue/query", json={
        "dataset_id": opened["dataset_id"], "limit": 10,
        "filters": [{"field": "id", "op": "~", "value": "1"}]}, headers=auth_headers)).json()
    assert not bad["valid"] and "Unknown filter operator" in bad["error"]


def test_the_copilot_summarises_an_out_of_core_particle_type(tmp_path):
    from app.assistant.tools import _catalogue_summary
    from app.dependencies import _catalogue_metadata
    from app.point_table import particle_catalogue_metadata
    files, _ = _snapshot(tmp_path)
    ref = gs.gadget_ref(files[0], 0)
    entry = {"path": ref, "dataframe": None, **particle_catalogue_metadata(ref)}
    streamed = _catalogue_summary(ref, {}, entry)
    frame, meta = _catalogue_metadata(Path(ref), "gadget")
    loaded = _catalogue_summary(ref, {}, {"path": ref, "dataframe": frame, **meta})
    assert streamed["num_rows"] == loaded["num_rows"] == 2000
    assert streamed["particles"] and streamed["snapshot"]["redshift"] == pytest.approx(1.0)
    assert streamed["units"]["temperature"] == "K"
    assert [c["name"] for c in streamed["columns"]] == [c["name"] for c in loaded["columns"]]
    for got, want in zip(streamed["columns"], loaded["columns"]):
        assert got["finite_count"] == want["finite_count"]
        for key in ("min", "max", "mean"):
            assert got[key] == pytest.approx(want[key], rel=1e-9), (got["name"], key)
    # Without the session entry (a path alone), it still works.
    assert _catalogue_summary(ref, {"max_columns": 2}, None)["columns_truncated"]


@pytest.mark.asyncio
async def test_a_snapshot_file_reaching_outside_the_roots_classifies_as_not_found(
        client, auth_headers, tmp_path, monkeypatch):
    root = tmp_path / "root"
    files, _ = _snapshot(root)
    outside = tmp_path / "outside_secret"
    outside.mkdir()
    moved = outside / files[2].name
    files[2].rename(moved)
    files[2].symlink_to(moved)
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(root))
    reply = (await client.post("/v1/files/classify", json={"path": str(files[0])},
                               headers=auth_headers)).json()
    assert reply == {**reply, "valid": False, "error": "File not found."}
    assert "outside_secret" not in str(reply)
    # Its folder is listed as a plain folder, naming nothing beyond the roots.
    reply = (await client.post("/v1/files/classify", json={"path": str(files[0].parent)},
                               headers=auth_headers)).json()
    assert reply["valid"] and reply["kind"] == "directory"
    assert "outside_secret" not in str(reply)
