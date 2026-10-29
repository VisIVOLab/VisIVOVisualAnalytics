"""The full-resolution threshold follows the backend's memory.

A node with 8 GB was killed serving a 3 GB cube at full resolution: the
client loads anything below the threshold in full, and the threshold was a
fixed 4 GiB whatever the node had.
"""
from __future__ import annotations

import pytest

from app import node_status
from app.fits_dataset import ScientificFitsDataset

GiB = 1024 ** 3


def _threshold() -> int:
    # The method reads only the environment and the memory limit.
    return ScientificFitsDataset._full_res_threshold_bytes(object.__new__(ScientificFitsDataset))


@pytest.fixture(autouse=True)
def _no_explicit(monkeypatch):
    monkeypatch.delenv("VISIVO_FULL_RES_THRESHOLD_BYTES", raising=False)


def test_a_small_node_lowers_the_threshold(monkeypatch):
    monkeypatch.setattr(node_status, "memory_limit_bytes", lambda: 8 * GiB)
    assert _threshold() == 2 * GiB


def test_a_large_host_keeps_four_gib(monkeypatch):
    monkeypatch.setattr(node_status, "memory_limit_bytes", lambda: 64 * GiB)
    assert _threshold() == 4 * GiB


def test_unknown_memory_keeps_four_gib(monkeypatch):
    monkeypatch.setattr(node_status, "memory_limit_bytes", lambda: None)
    assert _threshold() == 4 * GiB


def test_the_environment_sets_it_outright(monkeypatch):
    monkeypatch.setattr(node_status, "memory_limit_bytes", lambda: 8 * GiB)
    monkeypatch.setenv("VISIVO_FULL_RES_THRESHOLD_BYTES", str(6 * GiB))
    assert _threshold() == 6 * GiB


def test_a_cgroup_limit_counts_without_its_usage_file(monkeypatch, tmp_path):
    # memory.current unreadable: the limit alone still bounds the node.
    level = tmp_path / "cg"
    level.mkdir()
    (level / "memory.max").write_text(str(8 * GiB))
    monkeypatch.setattr(node_status, "_levels", lambda: [level])
    monkeypatch.setattr(node_status, "_host_memory", lambda: (64 * GiB, 32 * GiB))
    assert node_status.memory_limit_bytes() == 8 * GiB
    assert _threshold() == 2 * GiB


def test_without_a_cgroup_limit_the_host_memory_counts(monkeypatch, tmp_path):
    level = tmp_path / "cg"
    level.mkdir()
    (level / "memory.max").write_text("max")
    monkeypatch.setattr(node_status, "_levels", lambda: [level])
    monkeypatch.setattr(node_status, "_host_memory", lambda: (12 * GiB, 6 * GiB))
    assert node_status.memory_limit_bytes() == 12 * GiB
    assert _threshold() == 3 * GiB
