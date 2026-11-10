"""A VBT whose ``.head`` declares the wrong byte order (issue #165).

Found on PLEIADI: ``changavbtDARK.bin.head`` says ``float / 9 / 51200000 /
little`` but the data are big-endian (ChaNGa/Tipsy snapshots are XDR). Read as
declared, MASS was -1.273e24 on every row instead of 2.450e-5. The open routes
now sample the values, warn when the other order is the plausible one, and
accept the order to read the file in; the file is never rewritten.
"""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np
import pytest

from app import vbt_byte_order_store
from app.vbt_byte_order import check_byte_order, implausible_share, open_with_byte_order
from app.volume_source import (
    byte_order_override, open_fits, parse_vbt_header, vbt_field, vbt_field_ref,
)


@pytest.fixture(autouse=True)
def _no_overrides():
    """The choices are kept in a store every process reads: none may leak
    between tests."""
    vbt_byte_order_store.clear()
    yield
    vbt_byte_order_store.clear()


def _sph_fields(rows: int, cgs: bool = False, seed: int = 0) -> dict[str, np.ndarray]:
    """A dark-matter/gas particle table as simulations write it: positions in
    a box, Gaussian velocities, constant mass and softening, a density over
    several decades. ``cgs``: the same in grams and centimetres, with
    magnitudes (1e24 cm, 2e33 g, 1e-24 g/cm^3) an absolute threshold would
    call absurd."""
    rng = np.random.default_rng(seed)
    box, mass, eps, rho = (3.0e24, 2.0e33, 3.0e21, 1.0e-24) if cgs else (50.0, 2.45e-5, 1e-3, 1.0)
    return {
        "X": rng.uniform(0, box, rows), "Y": rng.uniform(0, box, rows),
        "Z": rng.uniform(0, box, rows),
        "VX": rng.normal(0, 300, rows), "VY": rng.normal(0, 300, rows),
        "VZ": rng.normal(0, 300, rows),
        "MASS": np.full(rows, mass), "EPS": np.full(rows, eps),
        "RHO": rho * rng.lognormal(0.0, 2.0, rows),
    }


def _write(tmp_path: Path, fields: dict[str, np.ndarray], written: str, declared: str,
           scalar: str = "float", dims: tuple[int, int, int] | None = None,
           name: str = "t") -> Path:
    """A VBT (``name.bin`` + ``name.bin.head``) with its data in ``written``
    order and its header saying ``declared``; returns the ``.bin``."""
    code = ("<" if written == "little" else ">") + ("f4" if scalar == "float" else "f8")
    data = tmp_path / f"{name}.bin"
    rows = len(next(iter(fields.values())))
    with open(data, "wb") as fh:
        for values in fields.values():
            np.asarray(values).astype(code).tofile(fh)
    spec = str(rows) if dims is None else f"{rows} {dims[0]} {dims[1]} {dims[2]} 1 1 1"
    (tmp_path / f"{name}.bin.head").write_text(
        "\n".join([scalar, str(len(fields)), spec, declared, *fields]) + "\n")
    return data


def _check(data: Path) -> dict | None:
    return check_byte_order(data, parse_vbt_header(str(data) + ".head"))


# ── the check ────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("scalar", ["float", "double"])
@pytest.mark.parametrize("cgs", [False, True])
@pytest.mark.parametrize("written", ["little", "big"])
def test_a_correct_header_is_never_flagged(tmp_path, scalar, cgs, written):
    data = _write(tmp_path, _sph_fields(20_000, cgs), written, written, scalar)
    assert _check(data) is None


@pytest.mark.parametrize("scalar", ["float", "double"])
@pytest.mark.parametrize("cgs", [False, True])
@pytest.mark.parametrize("written", ["little", "big"])
def test_a_wrong_header_is_flagged_with_the_other_order(tmp_path, scalar, cgs, written):
    declared = "big" if written == "little" else "little"
    data = _write(tmp_path, _sph_fields(20_000, cgs), written, declared, scalar)
    warning = _check(data)
    assert warning is not None
    assert (warning["declared"], warning["suggested"]) == (declared, written)
    assert warning["confidence"] == "overwhelming"
    evidence = warning["evidence"]
    assert evidence["implausible_declared"] >= 0.4
    assert evidence["implausible_suggested"] == 0.0
    by_name = {f["name"]: f for f in evidence["fields"]}
    assert float(by_name["X"]["first_suggested"]) >= 0


def test_the_pleiadi_case_reads_its_mass_back(tmp_path):
    """changavbtDARK: 9 fields, little declared, big written; the first MASS
    read either way is in the evidence, as in the issue's table."""
    data = _write(tmp_path, _sph_fields(50_000), "big", "little")
    warning = _check(data)
    mass = {f["name"]: f for f in warning["evidence"]["fields"]}["MASS"]
    assert mass["first_suggested"] == "2.45e-05"
    swapped = float(np.array([2.45e-5], ">f4").view("<f4")[0])
    assert float(mass["first_declared"]) == pytest.approx(swapped, rel=1e-3)


def test_a_volume_is_checked_as_a_table(tmp_path):
    rng = np.random.default_rng(3)
    shape = (16, 20, 24)
    fields = {"RHO": rng.lognormal(0, 1, shape).ravel(), "T": 1e4 * rng.lognormal(0, 1, shape).ravel()}
    good = _write(tmp_path, fields, "big", "big", dims=(24, 20, 16), name="good")
    bad = _write(tmp_path, fields, "big", "little", dims=(24, 20, 16), name="bad")
    assert _check(good) is None
    assert _check(bad)["suggested"] == "big"


@pytest.mark.parametrize("written", ["little", "big"])
def test_what_cannot_be_judged_is_not_flagged(tmp_path, written):
    declared = "big" if written == "little" else "little"
    # Constant fields whose value is a normal float either way (7.0 is not:
    # swapped, it is subnormal, which IS evidence).
    constant = {"A": np.full(5000, 1e-3), "B": np.full(5000, 0.3)}
    assert _check(_write(tmp_path, constant, written, declared, name="c")) is None
    # A few rows: too little to judge.
    tiny = {k: v[:10] for k, v in _sph_fields(10).items()}
    assert _check(_write(tmp_path, tiny, written, declared, name="t")) is None
    # Mostly NaN read correctly: odd either way, so no verdict against it.
    rng = np.random.default_rng(1)
    nan = rng.normal(0, 1, 5000)
    nan[rng.random(5000) < 0.8] = np.nan
    assert _check(_write(tmp_path, {"A": nan, "B": nan}, written, written, name="n")) is None


def _detects(tmp_path, name, fields, scalar):
    for written in ("little", "big"):
        other = "big" if written == "little" else "little"
        good = _write(tmp_path, fields, written, written, scalar, name=f"{name}g{written}")
        bad = _write(tmp_path, fields, written, other, scalar, name=f"{name}b{written}")
        assert _check(good) is None, (name, scalar, written)
        warning = _check(bad)
        assert warning is not None and warning["suggested"] == written, (name, scalar, written)


@pytest.mark.parametrize("scalar", ["float", "double"])
def test_fields_spanning_many_decades_or_masked_are_judged(tmp_path, scalar):
    """A density over 33 decades, a velocity cube 60% NaN, a 12-field gas
    table: each read right is plausible and read wrong is not."""
    rng = np.random.default_rng(7)
    n = 5000
    _detects(tmp_path, "rho", {"RHO": 10 ** rng.uniform(-30, 3, n)}, scalar)
    _detects(tmp_path, "rhot", {"RHO": 10 ** rng.uniform(-30, 3, n),
                                "T": 10 ** rng.uniform(1, 8, n)}, scalar)
    v = rng.normal(0, 100, n)
    v[rng.random(n) < 0.6] = np.nan
    _detects(tmp_path, "nan", {"VX": v, "VY": v.copy()}, scalar)
    gas = {"X": rng.normal(0, 30, n), "Y": rng.normal(0, 30, n), "Z": rng.normal(0, 0.3, n),
           "VX": rng.normal(0, 200, n), "VY": rng.normal(0, 200, n), "VZ": rng.normal(0, 20, n),
           "MASS": rng.choice([1e-6, 8e-6, 6.4e-5], n), "ID": np.arange(1, n + 1, dtype=float),
           "TEMP": 10 ** rng.uniform(1, 8, n), "RHO": 10 ** rng.uniform(-30, 3, n),
           "Z_MET": np.where(rng.random(n) < 0.9, 0.0, 10 ** rng.uniform(-5, -1.5, n)),
           "POT": -10 ** rng.uniform(3, 6, n)}
    _detects(tmp_path, "gas", gas, scalar)
    _detects(tmp_path, "ids", {"ID": np.arange(10 ** 9, 10 ** 9 + n, dtype=float),
                               "X": rng.uniform(0, 1, n)}, scalar)
    _detects(tmp_path, "small", {"DIV": rng.normal(0, 1, n) * 10 ** rng.uniform(-15, 0, n)},
             scalar)


def test_legitimate_extremes_are_plausible():
    """Scale-free: cgs magnitudes score as well as unit ones."""
    rng = np.random.default_rng(2)
    for values in (2e33 * rng.lognormal(0, 0.3, 4000), 1e-28 * rng.lognormal(0, 3, 4000),
                   rng.normal(0, 1, 4000), np.zeros(4000)):
        assert implausible_share(values.astype(np.float32)) == 0.0
        assert implausible_share(values.astype(np.float64)) == 0.0


def test_sampling_a_51_million_point_table_is_fast(tmp_path):
    """The size of changavbtDARK.bin (51 200 000 points, 9 float fields,
    1.8 GB), sparse on disk: the check seeks, it never reads the file."""
    rows, nfields = 51_200_000, 9
    data = tmp_path / "big.bin"
    with open(data, "wb") as fh:
        fh.truncate(rows * nfields * 4)
        rng = np.random.default_rng(0)
        for f in range(nfields):   # real values at the sampled places' neighbourhoods
            for start in np.linspace(0, rows - 4096, 16).astype(np.int64):
                fh.seek((f * rows + int(start)) * 4)
                rng.uniform(0, 50, 4096).astype(">f4").tofile(fh)
    (tmp_path / "big.bin.head").write_text(
        "\n".join(["float", str(nfields), str(rows), "little",
                   *[f"F{i}" for i in range(nfields)]]) + "\n")
    header = parse_vbt_header(tmp_path / "big.bin.head")
    check_byte_order(data, header)          # warm the imports
    t0 = time.perf_counter()
    warning = check_byte_order(data, header)
    elapsed = time.perf_counter() - t0
    assert warning is not None and warning["suggested"] == "big"
    assert elapsed < 0.1, f"{elapsed * 1e3:.1f} ms"


# ── the override ─────────────────────────────────────────────────────────────

def test_the_override_is_read_by_every_reader_and_the_file_is_untouched(tmp_path):
    from app.dependencies import _read_vbt_points
    from app.point_table import open_point_table
    from app.product_cache import file_identity

    fields = _sph_fields(5000)
    data = _write(tmp_path, fields, "big", "little")
    head = Path(str(data) + ".head")
    before = (data.read_bytes(), head.read_bytes())
    identity = file_identity(str(data))

    found = open_with_byte_order(head, None)
    assert found["byte_order"] == "little" and found["byte_order_warning"]["suggested"] == "big"

    found = open_with_byte_order(head, "big")
    assert found == {"byte_order": "big", "byte_order_declared": "little",
                     "byte_order_warning": None, "changed": True}
    assert byte_order_override(data) == "big"
    np.testing.assert_allclose(_read_vbt_points(head)["MASS"].to_numpy(), fields["MASS"],
                               rtol=1e-6)
    table = open_point_table(str(head))
    np.testing.assert_allclose(np.asarray(table.columns["X"]), fields["X"], rtol=1e-6)
    assert file_identity(str(data)) != identity
    assert (data.read_bytes(), head.read_bytes()) == before

    # Not asking keeps the choice, and warns again (the header is still
    # wrong), so that a fresh open lets the user reconsider; asking for the
    # declared order drops the choice.
    again = open_with_byte_order(data, None)
    assert again["byte_order"] == "big" and again["byte_order_warning"]["suggested"] == "big"
    assert file_identity(str(head)).endswith("byte-order=big")
    assert file_identity(str(data)).endswith("byte-order=big")
    found = open_with_byte_order(data, "little")
    assert found["byte_order"] == "little" and found["byte_order_warning"] is None
    assert byte_order_override(data) == ""
    assert file_identity(str(data)) == identity


def test_correcting_the_header_retires_the_choice(tmp_path):
    data = _write(tmp_path, _sph_fields(500), "big", "little")
    open_with_byte_order(data, "big")
    assert byte_order_override(data) == "big"
    head = Path(str(data) + ".head")
    head.write_text(head.read_text().replace("little", "big"))
    assert byte_order_override(data) == ""
    assert parse_vbt_header(head)["file_endian"] == "big"


def test_a_volume_field_and_its_pyramid_follow_the_override(tmp_path):
    from app.brick_pyramid import SourceSignature, default_pyramid_path

    rng = np.random.default_rng(4)
    values = rng.lognormal(0, 1, (8, 10, 12)).astype(np.float32)
    data = _write(tmp_path, {"RHO": values.ravel()}, "big", "little", dims=(12, 10, 8))
    declared_path = default_pyramid_path(data, 0)
    assert SourceSignature.of(data, 0).byte_order == ""
    open_with_byte_order(vbt_field_ref(data, 0), "big")
    np.testing.assert_array_equal(np.asarray(vbt_field(data, 0)), values)
    assert SourceSignature.of(data, 0).byte_order == "big"
    assert default_pyramid_path(data, 0) != declared_path


def test_a_pyramid_built_in_the_other_order_is_stale(tmp_path, monkeypatch):
    pytest.importorskip("h5py")
    from app.brick_pyramid import BrickPyramid, StalePyramid, build_pyramid

    rng = np.random.default_rng(5)
    values = rng.lognormal(0, 1, (16, 16, 16)).astype(np.float32)
    data = _write(tmp_path, {"RHO": values.ravel()}, "big", "little", dims=(16, 16, 16))
    out = tmp_path / "p.pyr.h5"
    open_with_byte_order(data, "big")
    build_pyramid(data, out, hdu=0, brick=8)
    BrickPyramid(out, source=data).close()
    open_with_byte_order(data, "little")
    with pytest.raises(StalePyramid, match="byte order"):
        BrickPyramid(out, source=data)


# ── across processes ─────────────────────────────────────────────────────────

def _volume(tmp_path, shape=(16, 16, 16)):
    rng = np.random.default_rng(8)
    values = (1.0 + rng.random(shape)).astype(np.float32)   # 1 .. 2
    return values, _write(tmp_path, {"RHO": values.ravel()}, "big", "little",
                          dims=(shape[2], shape[1], shape[0]))


@pytest.mark.asyncio
async def test_a_spawned_pool_worker_reads_the_chosen_order(tmp_path):
    """The cube tools run in a spawned worker pool, which never shared the
    memory of the process that took the choice."""
    from app.compute.cube import worker_cube_slice
    from app.dependencies import _run_in_pool

    values, data = _volume(tmp_path)
    ref = vbt_field_ref(data, 0)
    open_with_byte_order(ref, "big")
    got = await _run_in_pool(worker_cube_slice, ref, 3)
    assert got["range_min"] == pytest.approx(float(values[3].min()))
    assert got["range_max"] == pytest.approx(float(values[3].max()))
    # A worker started before a later change sees it too.
    open_with_byte_order(ref, "little")
    got = await _run_in_pool(worker_cube_slice, ref, 3)
    assert not got["range_max"] == pytest.approx(float(values[3].max()))


@pytest.mark.asyncio
async def test_a_spawned_process_reads_a_point_table_in_its_open_order(tmp_path):
    """What a render session process does first, without OpenGL: open the
    point table it is given (routers/render.point_table_spec), in a spawned
    process, after the store has changed again."""
    from app.dependencies import _run_in_pool
    from app.point_table import open_point_table
    from app.routers.render import point_table_spec

    fields = _sph_fields(300)
    data = _write(tmp_path, fields, "big", "little")
    head = str(data) + ".head"
    order = open_with_byte_order(head, "big")["byte_order"]
    spec = point_table_spec({"kind": "catalogue", "format": "vbt", "path": head,
                             "byte_order": order})
    open_with_byte_order(head, "little")
    table = await _run_in_pool(open_point_table, spec)
    np.testing.assert_allclose(np.asarray(table.columns["MASS"]), fields["MASS"], rtol=1e-6)


def test_a_render_session_process_reads_the_chosen_order(tmp_path):
    pytest.importorskip("vtk")
    import asyncio

    from app.render_session._process import RenderSessionProcess

    rng = np.random.default_rng(9)
    n = 2000
    fields = {"X": rng.normal(0, 1, n), "Y": rng.normal(0, 1, n), "Z": rng.normal(0, 1, n),
              "Mass": np.concatenate([np.full(n // 2, 1.0), np.full(n // 2, 100.0)])}
    data = _write(tmp_path, fields, "big", "little")
    head = str(data) + ".head"
    open_with_byte_order(head, "big")

    async def scenario():
        s = RenderSessionProcess("b" * 32, head,
                                 factory="app.render_session.points:PointRenderSession")
        try:
            await s.initialize()
        except Exception as exc:   # noqa: BLE001 - no offscreen OpenGL here
            pytest.skip(f"no render session here: {exc}")
        try:
            await s.apply_command({"type": "set_color", "field": "Mass", "lut": "Viridis"})
            await s.refresh_levels()
            return s.levels()
        finally:
            await s.close()

    levels = asyncio.new_event_loop().run_until_complete(scenario())
    assert (levels["data_min"], levels["data_max"]) == (1.0, 100.0)


def test_a_pyramid_built_in_its_own_process_is_ready_and_not_rebuilt(tmp_path, monkeypatch):
    pytest.importorskip("h5py")
    from app import pyramid_builds

    monkeypatch.setenv("VISIVO_PYRAMID_DIR", str(tmp_path / "pyr"))
    values, data = _volume(tmp_path, (32, 32, 32))
    open_with_byte_order(data, "big")
    deadline = time.monotonic() + 120
    status = pyramid_builds.ensure_pyramid(str(data), 0)
    while status.state == "building" and time.monotonic() < deadline:
        time.sleep(0.2)
        status = pyramid_builds.ensure_pyramid(str(data), 0)
    assert status.state == "ready", status
    assert pyramid_builds.pyramid_ready(str(data), 0)
    from app.brick_pyramid import BrickPyramid, default_pyramid_path
    with BrickPyramid(default_pyramid_path(data, 0), source=data) as pyramid:
        assert pyramid.source.byte_order == "big"
    assert pyramid_builds.ensure_pyramid(str(data), 0).state == "ready"


def test_datasets_open_in_another_order_are_closed_in_every_session(tmp_path):
    from app.sessions import REGISTRY
    from app.volume_source import with_order

    data = _write(tmp_path, _sph_fields(500), "big", "little")
    one, two = REGISTRY.create(), REGISTRY.create()
    one.datasets["cat_old"] = {"path": str(data) + ".head", "byte_order": "little"}
    two.datasets["vol_old"] = {"path": with_order(vbt_field_ref(data, 0), "little"),
                               "byte_order": "little"}
    two.datasets["cat_same"] = {"path": str(data), "byte_order": "big"}
    two.datasets["cat_other"] = {"path": str(tmp_path / "x.bin.head"), "byte_order": "little"}
    open_with_byte_order(data, "big")
    assert "cat_old" not in one.datasets and "vol_old" not in two.datasets
    assert {"cat_same", "cat_other"} <= set(two.datasets)


def test_render_sessions_drawing_the_old_order_are_picked():
    """Which running render sessions an order change closes (the closing
    itself is the render manager's own close_session)."""
    import tempfile
    from types import SimpleNamespace

    from app.vbt_byte_order import sessions_read_otherwise
    from app.volume_source import with_order

    with tempfile.TemporaryDirectory() as d:
        data = _write(Path(d), _sph_fields(500), "big", "little")
        head = str(data) + ".head"
        s = [SimpleNamespace(session_id="a", dataset_path=head),                       # store: little
             SimpleNamespace(session_id="b", dataset_path=with_order(head, "big")),
             SimpleNamespace(session_id="c", dataset_path=with_order(head, "little")),
             SimpleNamespace(session_id="d", dataset_path='{"paths": ["x.csv"]}'),
             SimpleNamespace(session_id="e", dataset_path=str(Path(d) / "other.fits"))]
        assert sessions_read_otherwise(s, data, "big") == ["a", "c"]


def test_a_symlinked_table_keeps_its_choice(tmp_path):
    """link.bin -> elsewhere/t.bin, with its own .head beside the link: the
    .head the parser reads is the one the choice is kept for."""
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    fields = _sph_fields(500)
    data = _write(elsewhere, fields, "big", "little")
    (elsewhere / "t.bin.head").unlink()
    link = tmp_path / "link.bin"
    link.symlink_to(data)
    (tmp_path / "link.bin.head").write_text("\n".join(["float", str(len(fields)), "500",
                                                         "little", *fields]) + "\n")
    found = open_with_byte_order(link, "big")
    assert found["byte_order"] == "big"
    assert byte_order_override(link) == "big"
    assert parse_vbt_header(tmp_path / "link.bin.head")["file_endian"] == "big"


def test_an_order_that_cannot_be_kept_is_an_error(tmp_path):
    from app.vbt_byte_order_store import ByteOrderNotStored, set_override

    with pytest.raises(ByteOrderNotStored):
        set_override(tmp_path / "gone.bin", "big", "little")


def test_a_file_system_without_flock_still_stores(tmp_path, monkeypatch):
    fcntl = pytest.importorskip("fcntl")

    def refuse(*_args):
        raise OSError(38, "Function not implemented")

    monkeypatch.setattr(fcntl, "flock", refuse)
    data = _write(tmp_path, _sph_fields(500), "big", "little")
    assert open_with_byte_order(data, "big")["byte_order"] == "big"
    assert byte_order_override(data) == "big"
    assert not (vbt_byte_order_store.store_path().with_name(
        vbt_byte_order_store.store_path().name + ".lockdir")).exists()


def _write_orders(args):
    """In a process of its own: choose the order of one table, many times."""
    from app.vbt_byte_order_store import set_override
    path, order = args
    for _ in range(10):
        set_override(path, order, "little", by="test")
    return path


def test_writers_in_several_processes_lose_nothing(tmp_path):
    import concurrent.futures as cf
    import multiprocessing as mp

    tables = [str(_write(tmp_path, _sph_fields(200), "big", "little", name=f"t{i}"))
              for i in range(6)]
    with cf.ProcessPoolExecutor(6, mp_context=mp.get_context("spawn")) as pool:
        list(pool.map(_write_orders, [(t, "big") for t in tables]))
    assert all(byte_order_override(t) == "big" for t in tables)


def test_a_flip_during_a_task_does_not_reach_it(tmp_path):
    """An open dataset's path carries its order: blocks read after the store
    changes are read as the first ones, and the product cache key holds."""
    from app.compute.moment import _FitsCubeSource
    from app.product_cache import file_identity
    from app.volume_source import with_order

    values, data = _volume(tmp_path)
    ref = with_order(vbt_field_ref(data, 0), open_with_byte_order(data, "big")["byte_order"])
    key = file_identity(ref)
    source = _FitsCubeSource(ref, 0, 0, values.shape, np.float32)
    first = np.asarray(source[0:8])
    open_with_byte_order(data, "little")          # another open changes the store
    second = np.asarray(source[8:16])
    np.testing.assert_array_equal(first, values[0:8])
    np.testing.assert_array_equal(second, values[8:16])
    assert file_identity(ref) == key
    with open_fits(ref) as hdul:
        np.testing.assert_array_equal(np.asarray(hdul[0].data), values)


@pytest.mark.parametrize("start", ["data", "head", "ref", "data+order", "ref+order"])
@pytest.mark.parametrize("field", [0, 3])
def test_a_field_reference_keeps_its_order_at_the_end(tmp_path, start, field):
    from app.vbt_byte_order_store import head_of
    from app.volume_source import ref_order, split_ref, vbt_field_ref, with_order

    data = str(tmp_path / "t.bin")
    given = {"data": data, "head": data, "ref": vbt_field_ref(data, 1),
             "data+order": with_order(data, "big"),
             "ref+order": with_order(vbt_field_ref(data, 1), "big")}[start]
    ref = vbt_field_ref(given, field)
    assert split_ref(ref) == (data, field)
    assert head_of(ref) == data + ".head"
    assert ref_order(ref) == ("big" if start.endswith("+order") else "")
    if start.endswith("+order"):
        assert ref == f"{data}::vbt-field={field}::vbt-order=big"


def test_a_colour_field_is_read_in_the_order_of_the_brightness(tmp_path):
    from types import SimpleNamespace

    from app.routers.volume_render import _paths
    from app.volume_source import ref_order, with_order

    rng = np.random.default_rng(10)
    shape = (8, 8, 8)
    data = _write(tmp_path, {"A": rng.random(shape).ravel(), "B": rng.random(shape).ravel()},
                  "big", "little", dims=(8, 8, 8))
    open_with_byte_order(data, "little")       # the store: the header's order
    path = with_order(vbt_field_ref(data, 1), "big")
    req = SimpleNamespace(colour_by="colour", colour_field=0)
    brightness, colour = _paths({"kind": "cube", "path": path}, req)
    assert brightness == path
    assert ref_order(colour) == "big"


@pytest.mark.asyncio
async def test_the_session_listing_and_snapshots_show_the_path_opened(client, auth_headers,
                                                                     tmp_path):
    values, data = _volume(tmp_path)
    ref = vbt_field_ref(data, 0)
    body = (await client.post("/v1/datasets/open", json={"path": ref, "byte_order": "big"},
                              headers=auth_headers)).json()
    assert body["valid"], body["error"]
    headers = {**auth_headers, "X-Visivo-Session": body["session_id"]}
    listing = (await client.get(f"/v1/sessions/{body['session_id']}/datasets",
                                headers=headers)).json()
    mine = next(d for d in listing["datasets"] if d["dataset_id"] == body["dataset_id"])
    assert mine["path"] == ref and mine["byte_order"] == "big"
    snap = (await client.post("/v1/reproducibility/snapshot", json={"label": "t"},
                              headers=headers)).json()
    state = snap.get("state") or snap.get("snapshot", {}).get("state") or snap
    recorded = _find_dataset(state, body["dataset_id"])
    assert recorded["path"] == ref
    assert (recorded["byte_order"], recorded["byte_order_declared"]) == ("big", "little")
    prov = (await client.get("/v1/reproducibility/provenance", headers=headers)).json()
    assert prov["prov"]["entity"][f"visivo:{body['dataset_id']}"]["visivo:path"] == ref


def _find_dataset(tree, dataset_id):
    """The snapshot's record of ``dataset_id``, wherever the answer nests it."""
    if isinstance(tree, dict):
        if dataset_id in tree and isinstance(tree[dataset_id], dict):
            return tree[dataset_id]
        for value in tree.values():
            found = _find_dataset(value, dataset_id)
            if found is not None:
                return found
    return None


@pytest.mark.asyncio
async def test_a_samp_subset_is_read_in_the_order_the_window_opened(client, auth_headers,
                                                                    tmp_path):
    from astropy.table import Table

    from app.samp_bridge import get_samp_file_path

    fields = _sph_fields(300)
    data = _write(tmp_path, fields, "big", "little")
    head = str(data) + ".head"
    body = (await client.post("/v1/catalogue/open",
                              json={"path": head, "format": "vbt", "byte_order": "big"},
                              headers=auth_headers)).json()
    assert body["valid"], body["error"]
    # The store changes afterwards, as another backend sharing it would do
    # (without closing this backend's dataset).
    vbt_byte_order_store.set_override(head, "little", "little", by="another backend")
    resp = await client.post("/v1/samp/send-catalogue",
                             json={"path": head, "format": "vbt", "deliver": "return",
                                   "row_indices": [0, 5]},
                             headers={**auth_headers, "X-Visivo-Session": body["session_id"]})
    sent = resp.json()
    assert sent["valid"], sent
    token = sent["message"]["params"]["url"].rsplit("/", 1)[-1]
    table = Table.read(get_samp_file_path(token), format="votable")
    np.testing.assert_allclose(np.asarray(table["MASS"]), fields["MASS"][[0, 5]], rtol=1e-6)


def test_an_unknown_order_is_refused(tmp_path):
    data = _write(tmp_path, _sph_fields(100), "big", "little")
    with pytest.raises(ValueError, match="little"):
        open_with_byte_order(data, "middle")


# ── the routes ───────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_catalogue_open_warns_then_opens_in_the_order_asked(client, auth_headers, tmp_path):
    fields = _sph_fields(5000)
    data = _write(tmp_path, fields, "big", "little")
    head = str(data) + ".head"
    body = (await client.post("/v1/catalogue/open", json={"path": head, "format": "vbt"},
                              headers=auth_headers)).json()
    assert body["valid"], body["error"]
    assert body["byte_order"] == body["byte_order_declared"] == "little"
    assert body["byte_order_warning"]["suggested"] == "big"
    assert body["byte_order_warning"]["confidence"] == "overwhelming"

    body = (await client.post("/v1/catalogue/open",
                              json={"path": head, "format": "vbt", "byte_order": "big"},
                              headers=auth_headers)).json()
    assert body["valid"], body["error"]
    assert (body["byte_order"], body["byte_order_declared"]) == ("big", "little")
    assert body["byte_order_warning"] is None
    headers = {**auth_headers, "X-Visivo-Session": body["session_id"]}
    rows = (await client.post("/v1/catalogue/query",
                              json={"dataset_id": body["dataset_id"], "limit": 3},
                              headers=headers)).json()["rows"]
    assert rows[0]["MASS"] == pytest.approx(2.45e-5, rel=1e-6)
    prov = (await client.get("/v1/reproducibility/provenance", headers=headers)).json()
    entity = prov["prov"]["entity"][f"visivo:{body['dataset_id']}"]
    assert (entity["visivo:byte_order"], entity["visivo:byte_order_declared"]) == ("big", "little")

    # A correct table: no warning.
    good = _write(tmp_path, fields, "big", "big", name="good")
    body = (await client.post("/v1/catalogue/open",
                              json={"path": str(good) + ".head", "format": "vbt"},
                              headers=auth_headers)).json()
    assert body["valid"] and body["byte_order_warning"] is None
    assert body["byte_order"] == "big"


@pytest.mark.asyncio
async def test_byte_order_is_refused_off_a_vbt_or_out_of_range(client, auth_headers, tmp_path):
    csv = tmp_path / "c.csv"
    csv.write_text("x,y,z\n1,2,3\n")
    body = (await client.post("/v1/catalogue/open",
                              json={"path": str(csv), "format": "csv", "byte_order": "big"},
                              headers=auth_headers)).json()
    assert body["valid"] is False and "VisIVO Binary Table" in body["error"]
    resp = await client.post("/v1/catalogue/open",
                             json={"path": str(csv), "format": "csv", "byte_order": "middle"},
                             headers=auth_headers)
    assert resp.status_code == 422


@pytest.mark.asyncio
async def test_datasets_open_of_a_volume_field_warns_and_takes_the_override(client, auth_headers,
                                                                           tmp_path):
    rng = np.random.default_rng(6)
    values = rng.lognormal(0, 1, (8, 10, 12)).astype(np.float32)
    data = _write(tmp_path, {"RHO": values.ravel(), "T": 1e4 * values.ravel()}, "big", "little",
                  dims=(12, 10, 8))
    ref = vbt_field_ref(data, 1)
    body = (await client.post("/v1/datasets/open", json={"path": ref},
                              headers=auth_headers)).json()
    assert body["valid"], body["error"]
    assert body["byte_order_warning"]["suggested"] == "big"
    body = (await client.post("/v1/datasets/open", json={"path": ref, "byte_order": "big"},
                              headers=auth_headers)).json()
    assert body["valid"], body["error"]
    assert (body["byte_order"], body["byte_order_declared"]) == ("big", "little")
    assert body["byte_order_warning"] is None
    # A FITS file takes no byte order.
    fits_body = (await client.post("/v1/datasets/open",
                                   json={"path": str(tmp_path / "x.fits"), "byte_order": "big"},
                                   headers=auth_headers)).json()
    assert fits_body["valid"] is False


@pytest.mark.asyncio
async def test_vbt_open_warns_and_takes_the_override(client, auth_headers, tmp_path):
    fields = _sph_fields(5000)
    data = _write(tmp_path, fields, "little", "big")
    head = str(data) + ".head"
    body = (await client.post("/v1/vbt/open", json={"path": head},
                              headers=auth_headers)).json()
    assert body["valid"], body["error"]
    assert body["byte_order_warning"]["suggested"] == "little"
    body = (await client.post("/v1/vbt/open", json={"path": head, "byte_order": "little"},
                              headers=auth_headers)).json()
    assert body["valid"] and body["byte_order"] == "little"
    assert body["byte_order_warning"] is None
