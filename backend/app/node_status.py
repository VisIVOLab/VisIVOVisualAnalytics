"""What this backend, as an SRC node, can take on right now.

A catalogue that routes work to nodes (SKAVA) needs facts it cannot declare in
advance: how much memory is free for a new dataset, whether the CPU is already
contended, how many tasks are waiting. This module reads them where they are
true for this process: in a container, from its cgroup (the limits the node
really has, not the host's); elsewhere, from the operating system.

Nothing here names a user, a dataset or a path: the status is safe to publish.
"""
from __future__ import annotations

import os
import platform
import shutil
import time
from pathlib import Path
from typing import Any

def _own_cgroup() -> Path:
    """This process's cgroup v2 directory.

    In a container namespace that is the mount root; a backend run by systemd
    (or any manager) on a plain Linux host sits in a descendant group, named in
    /proc/self/cgroup as ``0::/<path>``. ``VISIVO_CGROUP_ROOT`` overrides both.
    """
    override = os.environ.get("VISIVO_CGROUP_ROOT")
    if override:
        return Path(override)
    mount = Path("/sys/fs/cgroup")
    try:
        for line in Path("/proc/self/cgroup").read_text().splitlines():
            hierarchy, _, rel = line.partition("::")
            if hierarchy == "0" and rel.strip():
                candidate = mount / rel.strip().lstrip("/")
                if candidate.is_dir():
                    return candidate
    except OSError:
        pass
    return mount


_CGROUP = _own_cgroup()

# Previous cgroup CPU usage sample, for a utilisation figure between two calls.
_last_cpu_sample: tuple[float, int] | None = None


def _read(path: Path) -> str | None:
    try:
        return path.read_text().strip()
    except OSError:
        return None


def _int(text: str | None) -> int | None:
    try:
        return int(text) if text is not None else None
    except ValueError:
        return None


def _pressure(name: str) -> float | None:
    """The 'some avg10' share of the cgroup's pressure stall information, in %.

    The fraction of the last ten seconds in which at least one task of this
    node waited for the resource: contention as the node's own work saw it.
    """
    text = _read(_CGROUP / f"{name}.pressure")
    if not text:
        return None
    for line in text.splitlines():
        if line.startswith("some "):
            for field in line.split()[1:]:
                key, _, value = field.partition("=")
                if key == "avg10":
                    try:
                        return float(value)
                    except ValueError:
                        return None
    return None


def _levels() -> list[Path]:
    """This cgroup and its ancestors up to the mount root, innermost first.

    A limit set on a parent (a systemd slice, a pod) binds the node as much as
    its own, so every visible level is consulted.
    """
    levels, level = [], _CGROUP
    while True:
        levels.append(level)
        if level.parent == level or not (level.parent / "cgroup.controllers").exists():
            return levels
        level = level.parent


def _cpu_quota() -> float | None:
    """CPUs granted by cpu.max, the tightest over the visible levels; None when unlimited."""
    granted = None
    for level in _levels():
        text = _read(level / "cpu.max")
        if not text:
            continue
        quota, _, period = text.partition(" ")
        if quota == "max":
            continue
        try:
            value = int(quota) / int(period or 100000)
        except ValueError:
            continue
        granted = value if granted is None else min(granted, value)
    return granted


def _cpuset_count() -> int | None:
    """CPUs this process may run on.

    The process affinity is already within the cgroup's cpuset and may be
    narrower, so it wins where the platform reports it; the effective cpuset is
    the fallback.
    """
    try:
        return len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        pass
    text = _read(_CGROUP / "cpuset.cpus.effective")
    if text:
        count = 0
        for part in text.split(","):
            lo, _, hi = part.strip().partition("-")
            try:
                count += (int(hi) - int(lo) + 1) if hi else 1
            except ValueError:
                count = 0
                break
        if count > 0:
            return count
    return None


def _cpu_utilisation(cpus: float) -> float | None:
    """Share of the granted CPUs used since the previous call, in %."""
    global _last_cpu_sample
    text = _read(_CGROUP / "cpu.stat")
    if not text:
        return None
    usage_us = None
    for line in text.splitlines():
        key, _, value = line.partition(" ")
        if key == "usage_usec":
            usage_us = _int(value)
    if usage_us is None:
        return None
    now = time.monotonic()
    previous, _last_cpu_sample = _last_cpu_sample, (now, usage_us)
    if previous is None or now <= previous[0] or cpus <= 0:
        return None
    used_s = (usage_us - previous[1]) / 1e6
    return max(0.0, min(100.0, 100.0 * used_s / ((now - previous[0]) * cpus)))


def _host_memory() -> tuple[int | None, int | None]:
    """The host's total and available memory from /proc/meminfo, in bytes."""
    total = available = None
    meminfo = _read(Path("/proc/meminfo"))
    if meminfo:
        fields = {}
        for line in meminfo.splitlines():
            key, _, rest = line.partition(":")
            parts = rest.split()
            if parts:
                fields[key] = _int(parts[0])
        if fields.get("MemTotal") is not None:
            total = fields["MemTotal"] * 1024
        if fields.get("MemAvailable") is not None:
            available = fields["MemAvailable"] * 1024
    return total, available


def _memory() -> dict[str, int | None]:
    """Memory limit and what is still available under it, in bytes.

    In a cgroup the working set is usage minus the clean file cache, active
    or not: the kernel reclaims it before it kills anything, so a job can
    count on it, as MemAvailable counts it on a host (container runtimes
    show usage minus the inactive part only, a view of what is cached, not
    of what a job can get). Available is the limit minus the working set.
    """
    total = available = None
    for level in _levels():
        limit = _int(_read(level / "memory.max"))
        current = _int(_read(level / "memory.current"))
        if limit is None or current is None:
            continue                     # "max": no limit at this level
        cached = unclean = 0
        for line in (_read(level / "memory.stat") or "").splitlines():
            key, _, value = line.partition(" ")
            if key in ("inactive_file", "active_file"):
                cached += _int(value) or 0
            elif key in ("file_dirty", "file_writeback"):
                unclean += _int(value) or 0   # still to be written: not droppable yet
        # A parent's usage includes its other children, so its headroom is what
        # is left for everybody under it, this node included.
        headroom = max(0, limit - max(0, current - max(0, cached - unclean)))
        total = limit if total is None else min(total, limit)
        available = headroom if available is None else min(available, headroom)
    host_total, host_available = _host_memory()
    if total is not None:
        # A cgroup limit is a ceiling, not a reservation: on a host under
        # pressure a node cannot get more than the host has left.
        if available is not None and host_available is not None:
            available = min(available, host_available)
        return {"total_bytes": total, "available_bytes": available}

    total, available = host_total, host_available
    if total is None:
        try:
            total = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
        except (ValueError, OSError, AttributeError):
            total = None
    return {"total_bytes": total, "available_bytes": available}


def available_memory_bytes() -> int | None:
    """What this process may still allocate, in bytes: the headroom under its
    cgroup's limit in a container, the host's available memory otherwise;
    None when neither can be read. For the compute paths that choose between
    holding a range in memory and streaming it."""
    try:
        return _memory().get("available_bytes")
    except OSError:
        return None


def memory_limit_bytes() -> int | None:
    """The memory this process can ever have, in bytes: the tightest cgroup
    limit over its levels in a container, the host's memory otherwise; None
    when neither can be read. Only the limits are read, not the usage, so a
    limit is never lost because a usage file is missing."""
    try:
        limits = [limit for level in _levels()
                  if (limit := _int(_read(level / "memory.max"))) is not None]
        if limits:
            return min(limits)
        total, _ = _host_memory()
        if total is not None:
            return total
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    except (OSError, ValueError, AttributeError):
        return None


def _declared_gpu() -> bool | None:
    raw = (os.environ.get("VISIVO_NODE_GPU") or "").strip().lower()
    if raw in ("1", "true", "yes"):
        return True
    if raw in ("0", "false", "no"):
        return False
    return None


def collect(*, workers: int, running_tasks: int, queued_tasks: int,
            render_active: int, render_capacity: int, workspace: Path,
            finders: list[str], version: str,
            finder_memory: dict[str, dict[str, float | int]] | None = None) -> dict[str, Any]:
    """The node status document (see ``GET /v1/node/status``)."""
    quota = _cpu_quota()
    cpuset = _cpuset_count()
    # Granted CPUs: the quota, never more than the CPUs the node may run on.
    cpus = float(cpuset or os.cpu_count() or 1)
    if quota is not None:
        cpus = min(cpus, quota)
    try:
        load_1m = os.getloadavg()[0]
    except (OSError, AttributeError):
        load_1m = None
    try:
        disk_free = shutil.disk_usage(workspace).free
    except OSError:
        disk_free = None
    return {
        "node_code": (os.environ.get("VISIVO_NODE_CODE") or "").strip() or None,
        "version": version,
        "timestamp": time.time(),
        "cpu": {
            "cpus": round(cpus, 2),
            "limited": quota is not None or (cpuset is not None and cpuset < (os.cpu_count() or cpuset)),
            "utilisation_pct": _cpu_utilisation(cpus),
            "pressure_pct": _pressure("cpu"),
            # The host's load average: only meaningful when not in a cgroup.
            "load_1m": load_1m,
        },
        "memory": {**_memory(), "pressure_pct": _pressure("memory")},
        "disk": {"workspace_free_bytes": disk_free},
        "workers": {"total": workers},
        "tasks": {"running": running_tasks, "queued": queued_tasks},
        "render": {"active": render_active, "capacity": render_capacity},
        "gpu": _declared_gpu(),
        "platform": platform.machine(),
        "capabilities": {
            "kinds": ["image", "cube", "dynspec"],
            "finders": finders,
            # Per finder, need = factor × dataset bytes + floor_bytes.
            "finder_memory": finder_memory or {},
        },
    }


def prometheus(status: dict[str, Any]) -> str:
    """The same facts in the Prometheus text exposition format."""
    def gauge(name: str, help_: str, value: Any) -> list[str]:
        if value is None:
            return []
        if isinstance(value, bool):
            value = int(value)
        return [f"# HELP visivo_{name} {help_}", f"# TYPE visivo_{name} gauge",
                f"visivo_{name} {value}"]

    lines: list[str] = []
    lines += gauge("cpus", "CPUs granted to the node.", status["cpu"]["cpus"])
    lines += gauge("cpu_utilisation_percent", "CPU used since the previous sample, % of granted.",
                   status["cpu"]["utilisation_pct"])
    lines += gauge("cpu_pressure_percent", "Share of the last 10 s with CPU stalls.",
                   status["cpu"]["pressure_pct"])
    lines += gauge("memory_total_bytes", "Memory limit of the node.", status["memory"]["total_bytes"])
    lines += gauge("memory_available_bytes", "Memory available under the limit.",
                   status["memory"]["available_bytes"])
    lines += gauge("memory_pressure_percent", "Share of the last 10 s with memory stalls.",
                   status["memory"]["pressure_pct"])
    lines += gauge("workspace_free_bytes", "Free disk in the workspace.",
                   status["disk"]["workspace_free_bytes"])
    lines += gauge("workers", "Compute worker processes.", status["workers"]["total"])
    lines += gauge("tasks_running", "Tasks running.", status["tasks"]["running"])
    lines += gauge("tasks_queued", "Tasks waiting to start.", status["tasks"]["queued"])
    lines += gauge("render_sessions", "Active server-side render sessions.", status["render"]["active"])
    lines += gauge("render_capacity", "Render session capacity.", status["render"]["capacity"])
    lines += gauge("gpu", "GPU declared by the operator (1/0).", status["gpu"])
    return "\n".join(lines) + "\n"
