"""SubFind follow-ups (issue #448): folders holding a group catalogue,
Gadget-4 lightcone group catalogues, and the check of a catalogue's
velocity convention against the particles of the snapshot written with it
(Gadget-4 sorts that snapshot by group)."""
from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
import pytest

from app import gadget_snapshot as gs

h5py = pytest.importorskip("h5py")

_GEN_PATH = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"


def _generator():
    spec = importlib.util.spec_from_file_location("generate_demo_data_followups", _GEN_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


G = _generator()
A = 0.25
H = 0.7


@pytest.fixture(autouse=True)
def _no_units_env(monkeypatch):
    monkeypatch.delenv(gs.UNITS_ENV, raising=False)
    monkeypatch.delenv("VISIVO_DATA_ROOTS", raising=False)


def _params(h, config=None, comoving=True):
    p = h.create_group("Parameters").attrs
    p["ComovingIntegrationOn"] = np.int32(comoving)
    p["HubbleParam"] = H
    p["Omega0"] = 0.3
    p["OmegaLambda"] = 0.7
    if config is not None:
        c = h.create_group("Config")
        for key in config:
            c.attrs[key] = 1


#: Two subhalos of a snapshot sorted by group: subhalo 0 owns gas rows
#: 0..39 and dark-matter rows 0..99, subhalo 1 dark-matter rows 100..159.
LEN_TYPE = np.array([[40, 100, 0, 0, 0, 0], [0, 60, 0, 0, 0, 0]])
OFF_TYPE = np.array([[0, 0, 0, 0, 0, 0], [40, 100, 0, 0, 0, 0]])
N_GAS, N_DM, DM_MASS = 40, 180, 0.5


def _particles(seed=7):
    """Peculiar velocities (km/s) and gas masses: each subhalo moves as a
    whole, with a dispersion around it."""
    rng = np.random.default_rng(seed)
    gas_v = np.array([300.0, -120.0, 40.0]) + rng.normal(0, 30, (N_GAS, 3))
    dm_v = np.empty((N_DM, 3))
    dm_v[:100] = np.array([250.0, -100.0, 60.0]) + rng.normal(0, 50, (100, 3))
    dm_v[100:160] = np.array([-80.0, 200.0, 10.0]) + rng.normal(0, 40, (60, 3))
    dm_v[160:] = rng.normal(0, 100, (N_DM - 160, 3))
    gas_m = rng.uniform(0.05, 0.15, N_GAS)
    return gas_v, gas_m, dm_v


def _subhalo_velocities(gas_v, gas_m, dm_v):
    out = []
    for s in range(2):
        m = np.concatenate([gas_m[OFF_TYPE[s, 0]:OFF_TYPE[s, 0] + LEN_TYPE[s, 0]],
                            np.full(LEN_TYPE[s, 1], DM_MASS)])
        v = np.concatenate([gas_v[OFF_TYPE[s, 0]:OFF_TYPE[s, 0] + LEN_TYPE[s, 0]],
                            dm_v[OFF_TYPE[s, 1]:OFF_TYPE[s, 1] + LEN_TYPE[s, 1]]])
        out.append((m[:, None] * v).sum(axis=0) / m.sum())
    return np.array(out, dtype=np.float32)


def write_snapshot(path, a=A, velocities=True, comoving=True):
    """A Gadget-4 HDF5 snapshot sorted by group, velocities stored with
    Gadget's sqrt(a) (v_peculiar / sqrt(a)) in a comoving run."""
    gas_v, gas_m, dm_v = _particles()
    if not comoving:
        gas_v, dm_v = gas_v * np.sqrt(a), dm_v * np.sqrt(a)
    with h5py.File(path, "w") as h:
        hd = h.create_group("Header").attrs
        hd["NumPart_ThisFile"] = np.array([N_GAS, N_DM, 0, 0, 0, 0], np.int32)
        hd["NumPart_Total"] = np.array([N_GAS, N_DM, 0, 0, 0, 0], np.uint32)
        hd["MassTable"] = np.array([0, DM_MASS, 0, 0, 0, 0], np.float64)
        hd["Time"] = a
        hd["Redshift"] = 1 / a - 1
        hd["NumFilesPerSnapshot"] = 1
        hd["BoxSize"] = 1000.0
        _params(h, comoving=comoving)
        g = h.create_group("PartType0")
        g["Coordinates"] = np.full((N_GAS, 3), 500.0, "f4")
        if velocities:
            g["Velocities"] = (gas_v / np.sqrt(a)).astype("f4")
        g["Masses"] = gas_m.astype("f4")
        d = h.create_group("PartType1")
        d["Coordinates"] = np.full((N_DM, 3), 500.0, "f4")
        if velocities:
            d["Velocities"] = (dm_v / np.sqrt(a)).astype("f4")
    return path


def write_catalogue(path, *, a=A, stored_power=0.0, config=None, offsets=OFF_TYPE,
                    comoving=True):
    """A Gadget-4 catalogue of those two subhalos (and one group), with
    SubhaloVel = peculiar x a**(-stored_power): 0 peculiar, 0.5 the
    particles' convention, -1 the internal a^2 dx/dt."""
    vel = _subhalo_velocities(*_particles()) * a ** (-stored_power)
    with h5py.File(path, "w") as h:
        hd = h.create_group("Header").attrs
        for key, value in (("Ngroups_ThisFile", 1), ("Nsubhalos_ThisFile", 2),
                           ("Ngroups_Total", 1), ("Nsubhalos_Total", 2), ("Nids_ThisFile", 0)):
            hd[key] = np.uint64(value)
        hd["NumFiles"] = np.int32(1)
        hd["Time"] = a
        hd["Redshift"] = 1 / a - 1
        hd["BoxSize"] = 1000.0
        _params(h, config, comoving)
        g = h.create_group("Group")
        g["GroupPos"] = np.full((1, 3), 500.0, "f4")
        g["GroupVel"] = np.full((1, 3), 40.0, "f4")
        g["GroupMass"] = np.full(1, 120.0, "f4")
        s = h.create_group("Subhalo")
        s["SubhaloPos"] = np.full((2, 3), 500.0, "f4")
        s["SubhaloVel"] = vel.astype("f4")
        s["SubhaloMass"] = np.array([54.0, 30.0], "f4")
        s["SubhaloHalfmassRad"] = np.array([6.0, 4.0], "f4")
        s["SubhaloLenType"] = LEN_TYPE.astype("i4")
        s["SubhaloOffsetType"] = offsets.astype("i8")
    return path


# ── Folders ──────────────────────────────────────────────────────────────────

def test_a_folder_holding_only_a_catalogue_opens_it(tmp_path):
    from app.classify import classify_path
    folder = tmp_path / "groups_004"
    folder.mkdir()
    cat = write_catalogue(folder / "fof_subhalo_tab_004.hdf5")
    assert gs.first_in_dir(folder) == cat
    assert gs.catalogue_in_dir(folder, cat) is None
    assert gs.open_snapshot(folder).is_subfind
    verdict = classify_path(folder)
    assert verdict["kind"] == "particles" and verdict["opener"] == "catalogue"
    assert verdict["open_path"] == gs.gadget_ref(cat, 0)
    assert [c["name"] for c in verdict["components"]] == ["FoF groups", "subhalos"]


def test_a_folder_with_a_snapshot_and_its_catalogue_opens_the_snapshot(tmp_path):
    from app.classify import classify_path
    snap = write_snapshot(tmp_path / "snapshot_004.hdf5")
    cat = write_catalogue(tmp_path / "fof_subhalo_tab_004.hdf5")
    write_catalogue(tmp_path / "fof_subhalo_tab_003.hdf5")
    write_snapshot(tmp_path / "snapshot-prevmostboundonly_004.hdf5")
    assert gs.first_in_dir(tmp_path) == snap
    assert gs.catalogue_in_dir(tmp_path, snap) == cat
    verdict = classify_path(tmp_path)
    assert verdict["open_path"] == gs.gadget_ref(snap, 0)
    assert "fof_subhalo_tab_004.hdf5" in verdict["detail"]
    assert "_003" not in verdict["detail"]


def test_a_snapshot_without_its_catalogue_names_none(tmp_path):
    from app.classify import classify_path
    snap = write_snapshot(tmp_path / "snapshot_000.hdf5")
    write_catalogue(tmp_path / "fof_subhalo_tab_007.hdf5")
    assert gs.catalogue_in_dir(tmp_path, snap) is None
    assert "catalogue" not in classify_path(tmp_path)["detail"]


def test_a_single_file_snapshot_with_a_numbered_suffix_still_opens(tmp_path):
    snap = write_snapshot(tmp_path / "snapshot.100")
    assert gs.first_in_dir(tmp_path) == snap


def test_a_binary_snapshot_beside_its_binary_catalogue_opens_first(tmp_path):
    parts = G.gadget_halo_particles(np.random.default_rng(1))
    files = G.write_gadget(tmp_path / "snap_002", parts,
                           mass_table=[0.0, G.GADGET_DM_MASS, 0, 0, 0, 0])
    sub = G.write_subfind(tmp_path / "sub_002", [540.0, 500.0, 500.0], 250.0, 120.0)
    assert gs.first_in_dir(tmp_path) == files[0]
    assert gs.catalogue_in_dir(tmp_path, files[0]) == sub


def test_later_files_of_a_multi_file_snapshot_are_not_probed(tmp_path):
    parts = G.gadget_halo_particles(np.random.default_rng(2))
    files = G.write_gadget(tmp_path / "snap_002", parts, num_files=3,
                           split=G.gadget_split(parts),
                           mass_table=[0.0, G.GADGET_DM_MASS, 0, 0, 0, 0])
    assert gs.first_in_dir(tmp_path) == files[0]
    assert [p for p, _ in gs._gadget_files_in_dir(tmp_path)] == [files[0]]


# ── Lightcone catalogues ─────────────────────────────────────────────────────

def test_a_lightcone_catalogue_keeps_its_velocities_and_does_not_wrap(
        tmp_path, monkeypatch):
    folder = tmp_path / "lc_groups_004"
    folder.mkdir()
    path = write_catalogue(folder / "lc_fof_subhalo_tab_004.hdf5",
                           config=["LIGHTCONE", "LIGHTCONE_PARTICLES_GROUPS", "PERIODIC"])
    monkeypatch.setenv(gs.UNITS_ENV, "svel=snapshot")
    snap = gs.open_snapshot(path)
    assert snap.lightcone and snap.fmt == "Gadget-4 SubFind HDF5, lightcone"
    assert snap.describe()["lightcone"] is True
    groups, subs = snap.component(0), snap.component(1)
    assert groups.physical()["vx"]["factor"] == pytest.approx(1.0)
    assert groups.units()["vx"] == "km/s (peculiar (lightcone))"
    assert subs.physical()["vx"]["factor"] == pytest.approx(1.0)
    assert subs.units()["vx"] == "km/s (peculiar (lightcone))"
    assert subs.column("half_mass_radius")[0] == pytest.approx(6.0 / A)
    assert subs.physical()["half_mass_radius"]["factor"] == pytest.approx(A / H)
    assert groups.periods() == {} and subs.periods() == {}


@pytest.mark.parametrize("folder,name,config", [
    ("groups_004", "fof_subhalo_tab_004.hdf5", ["LIGHTCONE_PARTICLES_GROUPS"]),
    ("lc_groups_004", "lc_fof_subhalo_tab_004.hdf5", ["LIGHTCONE"])])
def test_only_a_lightcone_named_catalogue_of_a_lightcone_run_is_one(
        tmp_path, folder, name, config):
    (tmp_path / folder).mkdir()
    snap = gs.open_snapshot(write_catalogue(tmp_path / folder / name, config=config))
    assert not snap.lightcone
    assert snap.component(0).physical()["vx"]["factor"] == pytest.approx(1 / A)


# ── The velocity check ───────────────────────────────────────────────────────

@pytest.mark.parametrize("power,convention", [
    (0.0, "peculiar"), (0.5, "snapshot"), (-1.0, "canonical")])
def test_the_check_finds_the_convention_the_catalogue_was_written_with(
        tmp_path, power, convention):
    from app.subfind_check import subfind_velocity_check
    write_snapshot(tmp_path / "snapshot_004.hdf5")
    folder = tmp_path / "groups_004"
    folder.mkdir()
    cat = write_catalogue(folder / "fof_subhalo_tab_004.hdf5", stored_power=power)
    out = subfind_velocity_check(cat)
    assert out["snapshot"] == "snapshot_004.hdf5" and out["scale_factor"] == pytest.approx(A)
    assert out["median_ratio"] == pytest.approx(A ** power, rel=1e-4)
    assert out["convention"] == convention and out["svel_in_use"] == "peculiar"
    assert [s["subhalo"] for s in out["subhalos"]] == [0, 1]
    assert not out["sampled"]


def test_the_check_samples_a_large_subhalo(tmp_path, monkeypatch):
    from app import subfind_check
    monkeypatch.setattr(subfind_check, "MAX_MEMBER_ROWS", 7)
    monkeypatch.setattr(subfind_check, "CHUNK_ROWS", 3)
    snap = write_snapshot(tmp_path / "snapshot_004.hdf5")
    cat = write_catalogue(tmp_path / "fof_subhalo_tab_004.hdf5")
    out = subfind_check.subfind_velocity_check(cat, snap, subhalos=1)
    assert out["sampled"] and len(out["subhalos"]) == 1
    assert out["median_ratio"] == pytest.approx(1.0, rel=0.3)


def test_a_run_that_is_not_comoving_has_no_sqrt_a_in_its_particles(tmp_path):
    from app.subfind_check import subfind_velocity_check
    snap = write_snapshot(tmp_path / "snapshot_004.hdf5", a=2.0, comoving=False)
    cat = write_catalogue(tmp_path / "fof_subhalo_tab_004.hdf5", a=2.0, comoving=False)
    out = subfind_velocity_check(cat, snap)
    assert out["scale_factor"] == 1.0
    assert out["median_ratio"] == pytest.approx(1.0, rel=1e-4)
    assert out["convention"] is None and "cannot tell" in out["reason"]


def test_a_mispaired_snapshot_gives_no_convention(tmp_path):
    from app.subfind_check import subfind_velocity_check
    snap = write_snapshot(tmp_path / "snapshot_004.hdf5")
    cat = write_catalogue(tmp_path / "fof_subhalo_tab_004.hdf5",
                          offsets=np.array([[0, 60, 0, 0, 0, 0], [40, 0, 0, 0, 0, 0]]))
    out = subfind_velocity_check(cat, snap)
    assert out["convention"] is None and out["ratio_mad"] is not None


def test_the_check_refuses_what_it_cannot_measure(tmp_path):
    from app.subfind_check import subfind_velocity_check
    sub = G.write_subfind(tmp_path / "groups_002" / "sub_002.0", [540.0, 500.0, 500.0],
                          250.0, 120.0)
    with pytest.raises(ValueError, match="Gadget-4 HDF5"):
        subfind_velocity_check(sub)
    lonely = tmp_path / "alone"
    lonely.mkdir()
    with pytest.raises(ValueError, match="no particle snapshot"):
        subfind_velocity_check(write_catalogue(lonely / "fof_subhalo_tab_004.hdf5"))
    other = tmp_path / "other"
    other.mkdir()
    write_snapshot(other / "snapshot_004.hdf5", a=0.5)
    with pytest.raises(ValueError, match="time"):
        subfind_velocity_check(write_catalogue(other / "fof_subhalo_tab_004.hdf5"))
    bad = tmp_path / "bad"
    bad.mkdir()
    write_snapshot(bad / "snapshot_004.hdf5")
    with pytest.raises(ValueError, match="outside the snapshot"):
        subfind_velocity_check(write_catalogue(bad / "fof_subhalo_tab_004.hdf5",
                                               offsets=OFF_TYPE + 500))
    still = tmp_path / "still"
    still.mkdir()
    write_snapshot(still / "snapshot_004.hdf5", velocities=False)
    with pytest.raises(ValueError, match="no vx"):
        subfind_velocity_check(write_catalogue(still / "fof_subhalo_tab_004.hdf5"))
    orphan = tmp_path / "orphan"
    orphan.mkdir()
    write_snapshot(orphan / "snapshot-prevmostboundonly_004.hdf5")
    with pytest.raises(ValueError, match="no particle snapshot"):
        subfind_velocity_check(write_catalogue(orphan / "fof_subhalo_tab_004.hdf5"))


def test_the_check_needs_a_snapshot_within_the_data_roots(tmp_path, monkeypatch):
    from app.subfind_check import snapshot_for_catalogue
    write_snapshot(tmp_path / "snapshot_004.hdf5")
    folder = tmp_path / "groups_004"
    folder.mkdir()
    cat = gs.open_snapshot(write_catalogue(folder / "fof_subhalo_tab_004.hdf5"))
    assert snapshot_for_catalogue(cat) == tmp_path / "snapshot_004.hdf5"
    monkeypatch.setenv("VISIVO_DATA_ROOTS", str(folder))
    assert snapshot_for_catalogue(cat) is None
    monkeypatch.delenv("VISIVO_DATA_ROOTS")
    assert snapshot_for_catalogue(cat, lambda p: ".hidden" not in p) is not None
    assert snapshot_for_catalogue(cat, lambda p: "snapshot_004" not in p) is None


def test_the_copilot_runs_the_check_on_a_loaded_catalogue(tmp_path):
    from app.assistant.tools import _catalogue_summary
    write_snapshot(tmp_path / "snapshot_004.hdf5")
    cat = write_catalogue(tmp_path / "fof_subhalo_tab_004.hdf5", stored_power=0.5)
    ref = gs.gadget_ref(cat, 1)
    out = _catalogue_summary(ref, {"subfind_velocity_check": 2}, None)
    assert out["subfind_velocity_check"]["convention"] == "snapshot"
    assert "subfind_velocity_check" not in _catalogue_summary(ref, {}, None)
    assert "subfind_velocity_check" not in _catalogue_summary(
        ref, {"subfind_velocity_check": False}, None)
    flag = _catalogue_summary(ref, {"subfind_velocity_check": True}, None)
    assert flag["subfind_velocity_check"]["convention"] == "snapshot"
    for bad in ("5", 0, -3, 2.5):
        out = _catalogue_summary(ref, {"subfind_velocity_check": bad}, None)
        assert "error" in out["subfind_velocity_check"] and out["num_rows"] == 2
    binary = G.write_subfind(tmp_path / "g" / "sub_002.0", [1.0, 1.0, 1.0], 2.0, 3.0)
    out = _catalogue_summary(gs.gadget_ref(binary, 1), {"subfind_velocity_check": 1}, None)
    assert "Gadget-4" in out["subfind_velocity_check"]["error"]
