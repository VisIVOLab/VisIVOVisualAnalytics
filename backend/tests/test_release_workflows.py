"""The release workflows' own logic (issue #305): the headset app is attached
to a release only on a tag push, retried, and the job fails when it cannot be
attached. The step's script is run with a stand-in ``gh`` and ``sleep``."""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / ".github" / "workflows" / "quest-app.yml"

pytestmark = pytest.mark.skipif(not QUEST.is_file() or shutil.which("bash") is None,
                                reason="not a source checkout, or no bash")


def _upload_step() -> dict:
    yaml = pytest.importorskip("yaml")
    steps = yaml.safe_load(QUEST.read_text(encoding="utf-8"))["jobs"]["build"]["steps"]
    return next(s for s in steps if s.get("name") == "Add it to the tag's release")


def test_the_upload_runs_on_a_tag_push_only() -> None:
    cond = " ".join(_upload_step()["if"].split())
    assert "github.event_name == 'push'" in cond and "startsWith(github.ref, 'refs/tags/')" in cond


def _run(tmp_path: Path, fail_first: int) -> subprocess.CompletedProcess:
    """The step's script with a ``gh`` whose first ``fail_first`` calls fail."""
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    (bin_dir / "gh").write_text(
        '#!/usr/bin/env bash\n'
        'n=$(( $(cat "$COUNT" 2>/dev/null || echo 0) + 1 )); echo $n > "$COUNT"\n'
        'echo "gh $*" >> "$CALLS"\n'
        '[ "$n" -gt "$FAIL_FIRST" ]\n', encoding="utf-8")
    (bin_dir / "sleep").write_text("#!/usr/bin/env bash\necho \"sleep $*\" >> \"$CALLS\"\n", encoding="utf-8")
    for f in bin_dir.iterdir():
        f.chmod(0o755)
    env = {**os.environ, "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}", "TAG": "1.1.0",
           "RUNNER_TEMP": str(tmp_path), "COUNT": str(tmp_path / "count"), "CALLS": str(tmp_path / "calls"),
           "FAIL_FIRST": str(fail_first)}
    return subprocess.run(["bash", "-c", _upload_step()["run"]], env=env, capture_output=True, text=True,
                          timeout=30, check=False)


def test_a_transient_failure_is_retried(tmp_path: Path) -> None:
    r = _run(tmp_path, fail_first=2)      # view fails twice, then view + upload succeed
    assert r.returncode == 0, r.stdout + r.stderr
    calls = (tmp_path / "calls").read_text().splitlines()
    assert [c for c in calls if c.startswith("sleep")] == ["sleep 10", "sleep 10"]
    assert calls[-1].startswith("gh release upload 1.1.0") and "--clobber" in calls[-1]


def test_the_job_fails_when_the_apk_cannot_be_attached(tmp_path: Path) -> None:
    r = _run(tmp_path, fail_first=1000)
    assert r.returncode == 1
    assert "::error::" in r.stdout and "5 attempts" in r.stdout
    calls = (tmp_path / "calls").read_text().splitlines()
    assert sum(c.startswith("gh release view") for c in calls) == 5
    assert sum(c.startswith("sleep") for c in calls) == 4
