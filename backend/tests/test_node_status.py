"""The node routes (/v1/ping, /v1/node/status, /v1/metrics) and the cgroup readings behind them."""
from pathlib import Path

import pytest

from app import node_status


@pytest.mark.asyncio
async def test_ping_needs_no_token(client):
    resp = await client.get("/v1/ping")
    assert resp.status_code == 200
    assert resp.json() == {"ok": True}


@pytest.mark.asyncio
async def test_status_is_public_and_names_no_user(client, monkeypatch):
    monkeypatch.setenv("VISIVO_NODE_CODE", "TEST")
    resp = await client.get("/v1/node/status")
    assert resp.status_code == 200
    body = resp.json()
    assert body["node_code"] == "TEST"
    assert body["cpu"]["cpus"] > 0
    assert body["workers"]["total"] >= 1
    assert set(body["tasks"]) == {"running", "queued"}
    assert "image" in body["capabilities"]["kinds"]
    # Capacity figures only: nothing that identifies a user, a dataset or a path.
    text = resp.text.lower()
    for word in ("token", "session_id", "dataset_id", "path"):
        assert word not in text


@pytest.mark.asyncio
async def test_status_can_require_a_token(client, auth_headers, monkeypatch):
    monkeypatch.setenv("VISIVO_NODE_STATUS_PUBLIC", "0")
    assert (await client.get("/v1/node/status")).status_code == 401
    assert (await client.get("/v1/metrics")).status_code == 401
    assert (await client.get("/v1/node/status", headers=auth_headers)).status_code == 200
    # Liveness stays open: it says nothing.
    assert (await client.get("/v1/ping")).status_code == 200


@pytest.mark.asyncio
async def test_metrics_are_prometheus_text(client):
    resp = await client.get("/v1/metrics")
    assert resp.status_code == 200
    assert resp.headers["content-type"].startswith("text/plain")
    assert "# TYPE visivo_cpus gauge" in resp.text
    assert "\nvisivo_tasks_running " in "\n" + resp.text


def _fake_cgroup(root: Path, *, max_mem="4294967296", current="1073741824",
                 inactive="268435456", cpu_max="200000 100000",
                 cpu_pressure="some avg10=49.53 avg60=35.26 avg300=11.25 total=1") -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "memory.max").write_text(max_mem + "\n")
    (root / "memory.current").write_text(current + "\n")
    (root / "memory.stat").write_text(f"anon 1\ninactive_file {inactive}\nactive_file 5\n")
    (root / "cpu.max").write_text(cpu_max + "\n")
    (root / "cpu.pressure").write_text(cpu_pressure + "\nfull avg10=1.00 avg60=0 avg300=0 total=0\n")
    (root / "cpu.stat").write_text("usage_usec 1000000\n")


def _no_affinity(pid):
    raise AttributeError("no sched_getaffinity on this platform")


@pytest.fixture(autouse=True)
def _host_affinity_hidden(monkeypatch):
    # The fake cgroups are the node; the test host's own CPU mask must not leak in.
    monkeypatch.setattr(node_status.os, "sched_getaffinity", _no_affinity, raising=False)


_REAL_HOST_MEMORY = node_status._host_memory


@pytest.fixture(autouse=True)
def _host_memory_unknown(monkeypatch):
    # Nor the test host's memory: a cgroup's headroom is capped by what the
    # host has left, which here is unknown (the usual case off Linux).
    monkeypatch.setattr(node_status, "_host_memory", lambda: (None, None))


def _collect():
    return node_status.collect(workers=2, running_tasks=1, queued_tasks=3, render_active=0,
                               render_capacity=4, workspace=Path("."), finders=[], version="t")


def test_cgroup_limits_are_the_node_limits(tmp_path, monkeypatch):
    _fake_cgroup(tmp_path)
    monkeypatch.setattr(node_status, "_CGROUP", tmp_path)
    monkeypatch.setattr(node_status, "_last_cpu_sample", None)
    status = _collect()
    assert status["cpu"]["cpus"] == 2.0 and status["cpu"]["limited"] is True
    assert status["cpu"]["pressure_pct"] == pytest.approx(49.53)
    # Working set = current − clean file cache (inactive and active alike);
    # available = limit − working set.
    assert status["memory"]["total_bytes"] == 4294967296
    assert status["memory"]["available_bytes"] == 4294967296 - (1073741824 - 268435456 - 5)
    assert status["tasks"] == {"running": 1, "queued": 3}


def test_unlimited_cgroup_falls_back_to_the_machine(tmp_path, monkeypatch):
    _fake_cgroup(tmp_path, max_mem="max", cpu_max="max 100000")
    monkeypatch.setattr(node_status, "_CGROUP", tmp_path)
    status = _collect()
    assert status["cpu"]["limited"] is False
    assert status["memory"]["total_bytes"] is None or status["memory"]["total_bytes"] > 0


def test_cpu_utilisation_is_measured_between_calls(tmp_path, monkeypatch):
    _fake_cgroup(tmp_path)
    monkeypatch.setattr(node_status, "_CGROUP", tmp_path)
    monkeypatch.setattr(node_status, "_last_cpu_sample", None)
    clock = iter([100.0, 101.0])
    monkeypatch.setattr(node_status.time, "monotonic", lambda: next(clock))
    assert node_status._cpu_utilisation(2.0) is None          # first sample: no interval yet
    (tmp_path / "cpu.stat").write_text("usage_usec 2000000\n")  # +1 CPU-second in 1 s
    assert node_status._cpu_utilisation(2.0) == pytest.approx(50.0)


def test_missing_cgroup_files_do_not_fail(tmp_path, monkeypatch):
    monkeypatch.setattr(node_status, "_CGROUP", tmp_path / "absent")
    status = _collect()
    assert status["cpu"]["pressure_pct"] is None
    assert status["cpu"]["cpus"] > 0


def test_cpuset_caps_the_granted_cpus(tmp_path, monkeypatch):
    # Unlimited quota on a 32-CPU host, but a two-CPU cpuset: two CPUs.
    _fake_cgroup(tmp_path, cpu_max="max 100000")
    (tmp_path / "cpuset.cpus.effective").write_text("0-1\n")
    monkeypatch.setattr(node_status, "_CGROUP", tmp_path)
    monkeypatch.setattr(node_status.os, "cpu_count", lambda: 32)
    status = _collect()
    assert status["cpu"]["cpus"] == 2.0 and status["cpu"]["limited"] is True
    # A quota below the cpuset wins.
    (tmp_path / "cpuset.cpus.effective").write_text("0-3,8\n")
    (tmp_path / "cpu.max").write_text("150000 100000\n")
    assert _collect()["cpu"]["cpus"] == 1.5


@pytest.mark.asyncio
async def test_running_tasks_exclude_the_pending_ones(client, monkeypatch):
    from app.routers import node as node_router

    monkeypatch.setattr(node_router.TASKS, "snapshot",
                        lambda recent_limit=0: {"running_count": 3, "pending_count": 1})
    body = (await client.get("/v1/node/status")).json()
    assert body["tasks"] == {"running": 2, "queued": 1}


def test_own_cgroup_follows_proc_self(tmp_path, monkeypatch):
    monkeypatch.delenv("VISIVO_CGROUP_ROOT", raising=False)
    real_read = node_status.Path.read_text

    def fake_read(self, *a, **kw):
        if str(self) == "/proc/self/cgroup":
            return "0::/system.slice/visivo.service\n"
        return real_read(self, *a, **kw)

    real_is_dir = node_status.Path.is_dir
    monkeypatch.setattr(node_status.Path, "read_text", fake_read)
    monkeypatch.setattr(node_status.Path, "is_dir",
                        lambda self: str(self).endswith("visivo.service") or real_is_dir(self))
    assert node_status._own_cgroup() == Path("/sys/fs/cgroup/system.slice/visivo.service")
    monkeypatch.setenv("VISIVO_CGROUP_ROOT", str(tmp_path))
    assert node_status._own_cgroup() == tmp_path


def test_affinity_narrower_than_the_cpuset_wins(tmp_path, monkeypatch):
    _fake_cgroup(tmp_path, cpu_max="max 100000")
    (tmp_path / "cpuset.cpus.effective").write_text("0-31\n")
    monkeypatch.setattr(node_status, "_CGROUP", tmp_path)
    monkeypatch.setattr(node_status.os, "sched_getaffinity", lambda pid: {0, 1}, raising=False)
    assert node_status._cpuset_count() == 2


def test_ancestor_limits_bind_the_node(tmp_path, monkeypatch):
    # The parent slice grants 2 CPUs and has 2 GiB left; the leaf is unlimited.
    parent, leaf = tmp_path, tmp_path / "visivo.service"
    _fake_cgroup(parent, max_mem=str(8 << 30), current=str(6 << 30), inactive="0",
                 cpu_max="200000 100000")
    _fake_cgroup(leaf, max_mem="max", current=str(1 << 30), cpu_max="max 100000")
    (parent / "cgroup.controllers").write_text("cpu memory\n")
    monkeypatch.setattr(node_status, "_CGROUP", leaf)
    assert node_status._cpu_quota() == 2.0
    memory = node_status._memory()
    assert memory["total_bytes"] == 8 << 30
    # The parent's 5 bytes of active file cache are reclaimable, so they count as free.
    assert memory["available_bytes"] == (2 << 30) + 5


def test_dirty_cache_is_not_counted_as_available(tmp_path, monkeypatch):
    # Pages still to be written cannot be dropped: they stay in the working set.
    _fake_cgroup(tmp_path)
    (tmp_path / "memory.stat").write_text("anon 1\ninactive_file 1000\nactive_file 500\n"
                                          "file_dirty 200\nfile_writeback 100\n")
    monkeypatch.setattr(node_status, "_CGROUP", tmp_path)
    monkeypatch.setattr(node_status, "_last_cpu_sample", None)
    status = _collect()
    assert status["memory"]["available_bytes"] == 4294967296 - (1073741824 - (1500 - 300))


def test_status_states_what_each_finder_needs(tmp_path, monkeypatch):
    monkeypatch.setattr(node_status, "_CGROUP", tmp_path)
    status = node_status.collect(workers=1, running_tasks=0, queued_tasks=0, render_active=0, render_capacity=1,
                                 workspace=Path("."), finders=["sofia2"], version="t",
                                 finder_memory={"sofia2": {"factor": 2.5, "floor_bytes": 64}})
    assert status["capabilities"]["finder_memory"] == {"sofia2": {"factor": 2.5, "floor_bytes": 64}}
    assert _collect()["capabilities"]["finder_memory"] == {}


def test_host_pressure_caps_a_cgroup_headroom(tmp_path, monkeypatch):
    # An 8 GiB limit with 1 GiB used advertises 7 GiB, unless the host has less left.
    _fake_cgroup(tmp_path, max_mem=str(8 << 30), current=str(1 << 30), inactive="0")
    monkeypatch.setattr(node_status, "_CGROUP", tmp_path)
    monkeypatch.setattr(node_status, "_last_cpu_sample", None)
    monkeypatch.setattr(node_status, "_host_memory", lambda: (16 << 30, 262144 * 1024))
    status = _collect()
    assert status["memory"]["total_bytes"] == 8 << 30
    assert status["memory"]["available_bytes"] == 262144 * 1024


def test_host_memory_is_read_from_meminfo(monkeypatch):
    real_read = node_status._read

    def read(path):
        if str(path) == "/proc/meminfo":
            return "MemTotal:       16777216 kB\nMemFree:          100000 kB\nMemAvailable:     262144 kB\n"
        return real_read(path)

    monkeypatch.setattr(node_status, "_read", read)
    assert _REAL_HOST_MEMORY() == (16777216 * 1024, 262144 * 1024)
    monkeypatch.setattr(node_status, "_read", lambda path: None)
    assert _REAL_HOST_MEMORY() == (None, None)
