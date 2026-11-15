"""The version has one source, backend/app/VERSION: the backend reports it,
CMakeLists.txt reads it, and CITATION.cff must agree (the release workflow
also refuses a tag that differs)."""
from __future__ import annotations

import re
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


@pytest.mark.asyncio
async def test_vr_info_reports_it_to_an_accepted_token(client, auth_headers, monkeypatch) -> None:
    monkeypatch.delenv("VISIVO_VERSION", raising=False)
    r = await client.get("/v1/vr/info", headers={"X-Visivo-Token": auth_headers["X-Visivo-Token"]})
    assert r.status_code == 200 and r.json().get("backend_version") == V.__version__
