"""The version has one source, backend/app/VERSION: the backend reports it,
CMakeLists.txt reads it, and CITATION.cff must agree (the release workflow
also refuses a tag that differs)."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from app import version as V
from app.main import app

ROOT = Path(__file__).resolve().parents[2]


def test_the_backend_reports_the_version_file() -> None:
    v = V.VERSION_FILE.read_text(encoding="utf-8").strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+", v)
    assert V.__version__ == v and app.version == v


def test_a_missing_or_bad_file_gives_unknown(tmp_path: Path) -> None:
    assert V.read_version(tmp_path / "none") == V.UNKNOWN
    (tmp_path / "VERSION").write_text("1.2\n", encoding="utf-8")
    assert V.read_version(tmp_path / "VERSION") == V.UNKNOWN
    (tmp_path / "VERSION").write_text(" 2.3.4 \n", encoding="utf-8")
    assert V.read_version(tmp_path / "VERSION") == "2.3.4"


@pytest.mark.skipif(not (ROOT / "CMakeLists.txt").is_file(), reason="not a source checkout")
def test_cmake_reads_the_file_and_citation_agrees() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/backend/app/VERSION" VISIVO_PROJECT_VERSION' in cmake
    assert re.search(r"^\s*VERSION\s+\$\{VISIVO_PROJECT_VERSION\}", cmake, re.M)
    citation = re.search(r'^version:\s*"?([^"\s]+)"?\s*$', (ROOT / "CITATION.cff").read_text(encoding="utf-8"), re.M)
    assert citation and citation.group(1) == V.__version__


#: VERSION files and what every reader must make of them (None: refused).
VARIANTS = [(b"1.0.12\n", "1.0.12"), (b"1.0.12 \n", "1.0.12"), (b"\xef\xbb\xbf1.0.12\n", "1.0.12"),
            (b"\xef\xbb\xbf  1.0.12 \t\r\n", "1.0.12"), (b"1.0.12", "1.0.12"), (b"1.0\n", None),
            (b"1.0.12\n1\n", None), (b"v1.0.12\n", None)]


@pytest.mark.parametrize("raw, want", VARIANTS)
def test_the_backend_reads_the_file_as_cmake_and_the_release_do(tmp_path: Path, raw: bytes, want) -> None:
    (tmp_path / "VERSION").write_bytes(raw)
    assert V.read_version(tmp_path / "VERSION") == (want or V.UNKNOWN)


@pytest.mark.skipif(not (ROOT / "CMakeLists.txt").is_file() or shutil.which("cmake") is None,
                    reason="no source checkout or no cmake")
@pytest.mark.parametrize("raw, want", VARIANTS)
def test_cmake_reads_the_file_as_the_backend_does(tmp_path: Path, raw: bytes, want) -> None:
    """The project's own lines up to project(), configured with no language."""
    lines = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8").splitlines()
    end = next(i for i, line in enumerate(lines) if line.startswith(")"))
    stub = "\n".join(lines[:end + 1]).replace("LANGUAGES CXX C", "LANGUAGES NONE")
    (tmp_path / "src" / "backend" / "app").mkdir(parents=True)
    (tmp_path / "src" / "CMakeLists.txt").write_text(stub + '\nmessage(STATUS "GOT [${PROJECT_VERSION}]")\n')
    (tmp_path / "src" / "backend" / "app" / "VERSION").write_bytes(raw)
    r = subprocess.run(["cmake", "-S", str(tmp_path / "src"), "-B", str(tmp_path / "b")],
                       capture_output=True, text=True, timeout=120, check=False)
    got = re.search(r"GOT \[([^\]]*)\]", r.stdout)
    assert (got.group(1) if got and r.returncode == 0 else None) == want, r.stdout + r.stderr


@pytest.mark.skipif(not (ROOT / ".github" / "workflows" / "release.yml").is_file() or shutil.which("bash") is None,
                    reason="no source checkout or no bash")
@pytest.mark.parametrize("raw, want", VARIANTS)
def test_the_release_check_reads_the_file_as_the_backend_does(tmp_path: Path, raw: bytes, want) -> None:
    yaml = pytest.importorskip("yaml")
    steps = yaml.safe_load((ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8"))
    script = next(s["run"] for s in steps["jobs"]["setup"]["steps"] if s.get("name", "").startswith("Tag, VERSION"))
    (tmp_path / "backend" / "app").mkdir(parents=True)
    (tmp_path / "backend" / "app" / "VERSION").write_bytes(raw)
    shutil.copy(ROOT / "CMakeLists.txt", tmp_path / "CMakeLists.txt")
    (tmp_path / "CITATION.cff").write_text(f"cff-version: 1.2.0\nversion: {want or '1.0.12'}\n")
    env = {**os.environ, "IS_TAG": "true", "TAG": f"v{want or '1.0.12'}"}
    r = subprocess.run(["bash", "-c", script], cwd=tmp_path, env=env, capture_output=True, text=True,
                       timeout=30, check=False)
    assert (r.returncode == 0) == (want is not None), r.stdout + r.stderr


@pytest.mark.asyncio
async def test_vr_info_reports_it_to_an_accepted_token(client, auth_headers, monkeypatch) -> None:
    monkeypatch.delenv("VISIVO_VERSION", raising=False)
    r = await client.get("/v1/vr/info", headers={"X-Visivo-Token": auth_headers["X-Visivo-Token"]})
    assert r.status_code == 200 and r.json().get("backend_version") == V.__version__
