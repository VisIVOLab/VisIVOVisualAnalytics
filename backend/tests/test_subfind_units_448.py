"""SubFind unit conventions (issue #448, item 4): subhalo velocities are
peculiar km/s, Vmax is physical km/s, an HDF5 catalogue's GroupVel carries
1/a, and the Gadget-4 / Arepo HDF5 catalogues open with the same columns as
the binary ones. The conventions follow the public writers: Gadget-4
(subfind_properties.cc, fof.cc, fof_io.cc, subfind_so.cc) and Arepo
(subfind_properties.c)."""
from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
import pytest

from app import gadget_snapshot as gs

_GEN_PATH = Path(__file__).resolve().parents[1] / "scripts" / "generate_demo_data.py"


def _generator():
    spec = importlib.util.spec_from_file_location("generate_demo_data_subfind", _GEN_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


G = _generator()


@pytest.fixture(autouse=True)
def _no_units_env(monkeypatch):
    monkeypatch.delenv(gs.UNITS_ENV, raising=False)


def _binary(tmp_path, a=0.25):
    return G.write_subfind(tmp_path / "groups_004" / "sub_004.0", [540.0, 500.0, 500.0],
                           250.0, 120.0, time=a, vmax=210.0)


# ── Binary catalogues ────────────────────────────────────────────────────────

def test_subhalo_velocity_is_peculiar_and_vmax_physical_by_default(tmp_path):
    snap = gs.open_snapshot(_binary(tmp_path))
    assert snap.units.comoving and snap.scale_factor == pytest.approx(0.25)
    subhalos = snap.component(1)
    phys = subhalos.physical()
    assert phys["vx"]["factor"] == pytest.approx(1.0)
    assert phys["vmax"] == {"factor": pytest.approx(1.0), "unit": "km/s"}
    units = subhalos.units()
    assert units["vx"] == "km/s (peculiar (SubFind convention))"
    assert units["vmax"] == "km/s physical"
    assert subhalos.column("vx")[0] * phys["vx"]["factor"] == pytest.approx(120.0)
    assert phys["half_mass_radius"]["factor"] == pytest.approx(0.25 / 0.7)


@pytest.mark.parametrize("svel,factor,label", [
    ("snapshot", 0.5, "internal, x sqrt(a)"),
    ("comoving", 4.0, "a^2 dx/dt, x 1/a"),
    ("canonical", 4.0, "a^2 dx/dt, x 1/a"),
    ("PECULIAR", 1.0, "peculiar (SubFind convention)")])
def test_the_environment_states_the_subhalo_velocity_convention(
        tmp_path, monkeypatch, svel, factor, label):
    monkeypatch.setenv(gs.UNITS_ENV, f"svel={svel}")
    subhalos = gs.open_snapshot(_binary(tmp_path)).component(1)
    assert subhalos.physical()["vx"]["factor"] == pytest.approx(factor)
    assert subhalos.units()["vz"] == f"km/s ({label})"
    assert subhalos.physical()["vmax"]["factor"] == pytest.approx(1.0)


def test_srad_reads_physical_subhalo_radii_as_comoving(tmp_path, monkeypatch):
    monkeypatch.setenv(gs.UNITS_ENV, "srad=physical")
    snap = gs.open_snapshot(_binary(tmp_path))
    subhalos = snap.component(1)
    assert subhalos.column("half_mass_radius")[0] == pytest.approx(25.0 / 0.25)
    assert subhalos.take("half_mass_radius", [0])[0] == pytest.approx(100.0)
    assert subhalos.physical()["half_mass_radius"]["factor"] == pytest.approx(0.25 / 0.7)
    assert subhalos.units()["half_mass_radius"] == \
        "kpc/h comoving (stored physical, x 1/a on read)"
    assert snap.component(0).physical()["rvir"]["factor"] == pytest.approx(0.25 / 0.7)


@pytest.mark.parametrize("env", ["svel=physical", "srad=proper", "sveldisp=1"])
def test_unknown_conventions_are_refused(tmp_path, monkeypatch, env):
    monkeypatch.setenv(gs.UNITS_ENV, env)
    with pytest.raises(ValueError, match=gs.UNITS_ENV):
        gs.open_snapshot(_binary(tmp_path))


def test_particle_velocities_keep_the_internal_sqrt_a(tmp_path, monkeypatch):
    parts = G.gadget_halo_particles(np.random.default_rng(3))
    files = G.write_gadget(tmp_path / "snap_004", parts, time=0.25,
                           mass_table=[0.0, G.GADGET_DM_MASS, 0, 0, 0, 0])
    dm = gs.open_snapshot(files[0]).component(1)
    assert dm.physical()["vx"]["factor"] == pytest.approx(0.5)
    assert dm.units()["vx"] == "km/s (internal, x sqrt(a))"
    monkeypatch.setenv(gs.UNITS_ENV, "svel=comoving")
    assert gs.open_snapshot(files[0]).component(1).physical()["vx"]["factor"] == \
        pytest.approx(0.5)


def test_a_non_comoving_run_has_no_a_in_any_velocity(tmp_path, monkeypatch):
    monkeypatch.setenv(gs.UNITS_ENV, "comoving=0,svel=comoving")
    subhalos = gs.open_snapshot(_binary(tmp_path)).component(1)
    assert subhalos.physical()["vx"]["factor"] == pytest.approx(1.0)
    assert subhalos.units()["vx"] == "km/s"


# ── HDF5 catalogues ──────────────────────────────────────────────────────────

def _hdf5_catalogue(path, *, producer="gadget4", ngroups=2, nsub=3, num_files=1,
                    totals=None, a=0.25):
    h5py = pytest.importorskip("h5py")
    sub = "Nsubhalos" if producer == "gadget4" else "Nsubgroups"
    totals = totals or (ngroups, nsub)
    with h5py.File(path, "w") as h:
        hd = h.create_group("Header")
        hd.attrs["Ngroups_ThisFile"] = np.uint64(ngroups)
        if producer != "gadget4-fof":
            hd.attrs[f"{sub}_ThisFile"] = np.uint64(nsub)
            hd.attrs[f"{sub}_Total"] = np.uint64(totals[1])
        hd.attrs["Nids_ThisFile"] = np.uint64(0)
        hd.attrs["Ngroups_Total"] = np.uint64(totals[0])
        hd.attrs["NumFiles"] = np.int32(num_files)
        hd.attrs["Time"] = a
        hd.attrs["Redshift"] = 1 / a - 1
        hd.attrs["BoxSize"] = 1000.0
        params = h.create_group("Parameters").attrs
        target = hd.attrs if producer == "arepo" else params
        target["HubbleParam"] = 0.7
        target["Omega0"] = 0.3
        target["OmegaLambda"] = 0.7
        params["ComovingIntegrationOn"] = np.int32(1)
        if ngroups:
            g = h.create_group("Group")
            g["GroupPos"] = np.full((ngroups, 3), 500.0, "f4")
            g["GroupVel"] = np.full((ngroups, 3), 40.0, "f4")
            g["GroupMass"] = np.full(ngroups, 12.0, "f4")
            g["GroupLen"] = np.full(ngroups, 1000, "i4")
            g["GroupNsubs"] = np.full(ngroups, 1, "i4")
            g["Group_M_TopHat200"] = np.full(ngroups, 11.0, "f4")
            g["Group_R_TopHat200"] = np.full(ngroups, 250.0, "f4")
            g["Group_M_Crit500"] = np.full(ngroups, 8.0, "f4")
            g["Group_R_Crit500"] = np.full(ngroups, 150.0, "f4")
            g["Group_R_Crit200"] = np.full(ngroups, 200.0, "f4")
        if nsub:
            s = h.create_group("Subhalo")
            s["SubhaloPos"] = np.full((nsub, 3), 501.0, "f4")
            s["SubhaloVel"] = np.tile(np.array([120.0, -30.0, 8.0], "f4"), (nsub, 1))
            s["SubhaloMass"] = np.full(nsub, 3.0, "f4")
            s["SubhaloVmax"] = np.full(nsub, 210.0, "f4")
            s["SubhaloVmaxRad"] = np.full(nsub, 9.0, "f4")
            s["SubhaloHalfmassRad"] = np.full(nsub, 6.0, "f4")
            s["SubhaloLenType"] = np.ones((nsub, 6), "i4")
            s["SubhaloHalfmassRadType"] = np.full((nsub, 6), 6.0, "f4")
            s["SubhaloSpin"] = np.ones((nsub, 3), "f4")
            s["SubhaloVelDisp"] = np.full(nsub, 80.0, "f4")
            s["SubhaloGroupNr" if producer == "gadget4" else "SubhaloGrNr"] = \
                np.zeros(nsub, "i8")
    return path


def test_a_gadget4_hdf5_catalogue_opens_like_a_binary_one(tmp_path):
    path = _hdf5_catalogue(tmp_path / "fof_subhalo_tab_004.hdf5")
    assert gs.is_gadget(path)
    snap = gs.open_snapshot(path)
    assert snap.is_subfind and snap.fmt == "Gadget-4 SubFind HDF5"
    assert snap.counts()[:2] == (2, 3) and snap.units.comoving
    assert [c["name"] for c in gs.gadget_components(path)] == ["FoF groups", "subhalos"]
    groups, subhalos = snap.component(0), snap.component(1)
    for col in ("x", "vx", "group_mass", "mvir", "rvir", "m500", "r500",
                "group_length", "num_subhalos", "group_r_crit200"):
        assert col in groups.names
    for col in ("x", "vx", "subhalo_mass", "vmax", "rmax", "half_mass_radius",
                "group_number", "subhalolentype_0", "spin_x", "subhaloveldisp"):
        assert col in subhalos.names
    assert groups.column("rvir")[0] == 250.0 and subhalos.column("vmax")[0] == 210.0
    assert subhalos.column("half_mass_radius")[0] == pytest.approx(6.0 / 0.25)
    assert subhalos.column("subhalohalfmassradtype_1")[0] == pytest.approx(6.0 / 0.25)
    assert subhalos.take("rmax", [2])[0] == pytest.approx(9.0 / 0.25)
    gp, sp = groups.physical(), subhalos.physical()
    assert gp["vx"]["factor"] == pytest.approx(4.0)
    assert groups.units()["vx"] == "km/s (a^2 dx/dt, x 1/a)"
    assert sp["vx"]["factor"] == pytest.approx(1.0) and sp["vmax"]["factor"] == 1.0
    assert gp["rvir"]["factor"] == pytest.approx(0.25 / 0.7)
    assert gp["group_r_crit200"]["factor"] == pytest.approx(0.25 / 0.7)
    assert sp["half_mass_radius"]["factor"] == pytest.approx(0.25 / 0.7)
    assert sp["subhalohalfmassradtype_1"]["factor"] == pytest.approx(0.25 / 0.7)
    assert sp["spin_x"] == {"factor": pytest.approx(1 / 0.7), "unit": "kpc km/s"}
    assert sp["subhaloveldisp"] == {"factor": pytest.approx(1.0), "unit": "km/s"}
    described = groups.describe()
    assert described["catalogue_kind"] == "subfind" and described["object_kind"] == "group"
    assert "critical" in described["radius_definitions"]["r500"]
    assert "Bryan & Norman" in described["radius_definitions"]["rvir"]
    assert "critical" in described["radius_definitions"]["rvir"]


def test_an_arepo_hdf5_catalogue_keeps_comoving_subhalo_radii(tmp_path):
    path = _hdf5_catalogue(tmp_path / "fof_subhalo_tab_004.hdf5", producer="arepo")
    snap = gs.open_snapshot(path)
    assert snap.fmt == "Arepo SubFind HDF5" and snap.hubble == pytest.approx(0.7)
    assert snap.units.comoving and "HDF5" in snap.units.source
    subhalos = snap.component(1)
    assert "group_number" in subhalos.names
    assert subhalos.column("half_mass_radius")[0] == pytest.approx(6.0)
    assert subhalos.physical()["half_mass_radius"]["factor"] == pytest.approx(0.25 / 0.7)
    assert subhalos.physical()["vx"]["factor"] == pytest.approx(1.0)


def test_a_multi_file_hdf5_catalogue_joins_its_files(tmp_path):
    folder = tmp_path / "groups_004"
    folder.mkdir()
    _hdf5_catalogue(folder / "fof_subhalo_tab_004.0.hdf5", ngroups=2, nsub=3,
                    num_files=2, totals=(3, 3))
    _hdf5_catalogue(folder / "fof_subhalo_tab_004.1.hdf5", ngroups=1, nsub=0,
                    num_files=2, totals=(3, 3))
    snap = gs.open_snapshot(folder / "fof_subhalo_tab_004.1.hdf5")
    assert len(snap.files) == 2 and snap.counts()[:2] == (3, 3)
    assert len(snap.component(0).column("rvir")) == 3
    assert len(snap.component(1).column("vmax")) == 3


def test_a_catalogue_without_groups_is_still_a_subfind_catalogue(tmp_path):
    path = _hdf5_catalogue(tmp_path / "fof_subhalo_tab_000.hdf5", ngroups=0, nsub=0)
    snap = gs.open_snapshot(path)
    assert snap.is_subfind and snap.describe()["catalogue_kind"] == "subfind"
    assert snap.component(0).name == "FoF groups"


def test_a_gadget4_fof_only_catalogue_is_labelled_gadget4(tmp_path):
    path = _hdf5_catalogue(tmp_path / "fof_tab_004.hdf5", producer="gadget4-fof", nsub=0)
    snap = gs.open_snapshot(path)
    assert snap.fmt == "Gadget-4 SubFind HDF5" and snap.counts()[:2] == (2, 0)
    assert snap.component(0).physical()["vx"]["factor"] == pytest.approx(4.0)


def test_the_binary_catalogue_keeps_its_own_definitions_and_no_spin_units(tmp_path):
    snap = gs.open_snapshot(_binary(tmp_path))
    assert snap.component(0).describe()["radius_definitions"]["rvir"] == \
        "mean background density"
    assert "spin_x" not in snap.component(1).physical()
