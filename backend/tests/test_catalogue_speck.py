"""Tests for Cartesian (3-D) catalogue support — Partiview .speck + X/Y/Z (F4)."""
import numpy as np
import pytest

from app.dependencies import (
    _catalogue_metadata_from_dataframe,
    _read_speck,
)


SPECK = """\
# a tiny Partiview point file
datavar 0 mass
datavar 1 sfr
texture -M 1 halo.sgi
10.0 20.0 30.0 1.5 0.2
-5.0  0.0 12.0 2.0 0.1 text label ignored
3.0  -3.0  9.0 0.7 0.9
"""


def _write_speck(tmp_path):
    p = tmp_path / "cat.speck"
    p.write_text(SPECK)
    return p


def test_read_speck_columns_and_rows(tmp_path):
    df = _read_speck(_write_speck(tmp_path))
    assert list(df.columns) == ["X", "Y", "Z", "mass", "sfr"]
    assert len(df) == 3
    np.testing.assert_allclose(df.iloc[0].to_numpy(), [10.0, 20.0, 30.0, 1.5, 0.2])
    # trailing text on row 2 is ignored, numeric values still parsed
    np.testing.assert_allclose(df.iloc[1].to_numpy(), [-5.0, 0.0, 12.0, 2.0, 0.1])


def test_cartesian_metadata_no_radec(tmp_path):
    df = _read_speck(_write_speck(tmp_path))
    meta = _catalogue_metadata_from_dataframe(df, "speck")
    cf = meta["coordinate_fields"]
    assert cf["system"] == "cartesian"
    assert (cf["x"], cf["y"], cf["z"]) == ("X", "Y", "Z")
    assert cf["ra"] == "" and cf["dec"] == ""
    assert meta["num_rows"] == 3


def test_sky_metadata_still_works():
    import pandas as pd
    df = pd.DataFrame({"RA": [10.0, 11.0], "DEC": [-5.0, -4.0], "flux": [1.0, 2.0]})
    meta = _catalogue_metadata_from_dataframe(df, "csv")
    assert meta["coordinate_fields"]["system"] == "sky"
    assert meta["coordinate_fields"]["ra"] == "RA"


def test_catalogue_requires_coordinates():
    import pandas as pd
    df = pd.DataFrame({"flux": [1.0], "name": ["x"]})
    with pytest.raises(ValueError):
        _catalogue_metadata_from_dataframe(df, "csv")


def test_speck_router_open(tmp_path):
    import os
    os.environ.setdefault("VISIVO_TOKEN", "test-token-visivo-12345")
    from fastapi.testclient import TestClient
    from app.main import app
    auth = {"X-Visivo-Token": "test-token-visivo-12345"}
    client = TestClient(app, raise_server_exceptions=True)
    p = _write_speck(tmp_path)
    r = client.post("/v1/catalogue/open", json={"path": str(p), "format": "speck"}, headers=auth)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["valid"] is True, body
    assert body["coordinate_fields"]["system"] == "cartesian"
