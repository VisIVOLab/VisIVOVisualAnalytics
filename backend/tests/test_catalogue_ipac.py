"""Tests for IPAC ASCII table (.tbl) catalogue support — Cartesian x/y/z + attrs."""
import numpy as np
import pytest

from app.dependencies import _catalogue_load_dataframe, _catalogue_metadata_from_dataframe, _read_ipac


# Minimal IPAC table: '\' keyword/comment lines, then '|'-delimited name/type/unit
# header rows, then whitespace-aligned data. Mirrors the cosmological-snapshot
# subtables (x/y/z in Mpc + velocity + attribute columns).
IPAC = """\
\\ snapshot subtable 2_1_3
\\ xlimits:  23.810  47.619
|    x     |    y     |    z     |   vx     | LogMass  |
|    r     |    r     |    r     |    r     |    r     |
|   Mpc    |   Mpc    |   Mpc    |  km.s    |   Msun   |
     26.370      1.654     56.809    -29.572      7.067
     26.407      1.660     56.813    -28.109      7.067
     26.461      1.723     56.902    -41.407      7.071
"""


def _write_ipac(tmp_path):
    p = tmp_path / "snap_2_1_3.tbl"
    p.write_text(IPAC)
    return p


def test_read_ipac_columns_and_rows(tmp_path):
    df = _read_ipac(_write_ipac(tmp_path))
    assert list(df.columns) == ["x", "y", "z", "vx", "LogMass"]
    assert len(df) == 3
    np.testing.assert_allclose(df.iloc[0].to_numpy(), [26.370, 1.654, 56.809, -29.572, 7.067])


def test_ipac_load_via_format_dispatch(tmp_path):
    # Both "tbl" and "ipac" format strings route to the IPAC reader.
    for fmt in ("tbl", "ipac"):
        df = _catalogue_load_dataframe(_write_ipac(tmp_path), fmt)
        assert list(df.columns)[:3] == ["x", "y", "z"]


def test_ipac_cartesian_metadata(tmp_path):
    df = _read_ipac(_write_ipac(tmp_path))
    meta = _catalogue_metadata_from_dataframe(df, "ipac")
    cf = meta["coordinate_fields"]
    assert cf["system"] == "cartesian"
    # lowercase x/y/z are detected (normalised to X/Y/Z aliases)
    assert (cf["x"], cf["y"], cf["z"]) == ("x", "y", "z")
    assert cf["ra"] == "" and cf["dec"] == ""
    assert meta["num_rows"] == 3


def test_ipac_router_open(tmp_path):
    import os
    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)
    p = _write_ipac(tmp_path)
    r = client.post("/v1/catalogue/open", json={"path": str(p), "format": "tbl"}, headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"] is True, body
    assert body["coordinate_fields"]["system"] == "cartesian"


def test_ipac_router_open_multifile(tmp_path):
    """Several .tbl files open as ONE concatenated catalogue (snapshot subtables)."""
    import os
    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)
    p1 = tmp_path / "a.tbl"
    p1.write_text(IPAC)
    p2 = tmp_path / "b.tbl"
    p2.write_text(IPAC)
    r = client.post(
        "/v1/catalogue/open",
        json={"path": str(p1), "format": "tbl", "paths": [str(p1), str(p2)]},
        headers=auth,
    )
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"] is True, body
    assert body["num_rows"] == 6   # 3 + 3 concatenated
    assert body["coordinate_fields"]["system"] == "cartesian"


def test_ipac_multifile_rejects_column_mismatch(tmp_path):
    """Multi-file open refuses files whose columns differ (no silent NaN fill)."""
    import os
    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)
    p1 = tmp_path / "a.tbl"
    p1.write_text(IPAC)
    # A second table with DIFFERENT columns (x/y/z/foo, not x/y/z/vx/LogMass).
    other = (
        "|    x     |    y     |    z     |   foo    |\n"
        "|    r     |    r     |    r     |    r     |\n"
        "|   Mpc    |   Mpc    |   Mpc    |   unk    |\n"
        "     1.0       2.0       3.0       4.0\n"
    )
    p2 = tmp_path / "b.tbl"
    p2.write_text(other)
    r = client.post(
        "/v1/catalogue/open",
        json={"path": str(p1), "format": "tbl", "paths": [str(p1), str(p2)]},
        headers=auth,
    )
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"] is False
    assert "mismatch" in body["error"].lower()
