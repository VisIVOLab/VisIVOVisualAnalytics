"""
Tests for Domain R – Reproducibility & Provenance (R-01, R-02, R-03).

R-01: CARTA script export.
R-02: PROV-JSON provenance export.
R-03: Parameter snapshots.
"""
from __future__ import annotations

import os
import tempfile
import unittest

import numpy as np


_TEST_TOKEN = "test-token-visivo-12345"


def _make_2d_fits(ny: int = 16, nx: int = 16) -> str:
    from astropy.io import fits
    data = np.random.default_rng(0).standard_normal((ny, nx)).astype(np.float32)
    hdu = fits.PrimaryHDU(data)
    h = hdu.header
    h["CTYPE1"] = "RA---SIN"; h["CRPIX1"] = nx / 2; h["CRVAL1"] = 83.8
    h["CDELT1"] = -1e-4;       h["CUNIT1"] = "deg"
    h["CTYPE2"] = "DEC--SIN"; h["CRPIX2"] = ny / 2; h["CRVAL2"] = -5.4
    h["CDELT2"] = 1e-4;        h["CUNIT2"] = "deg"
    tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
    fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
    tmp.close()
    return tmp.name


class ReproducibilityTestBase(unittest.TestCase):
    """Base class: creates a TestClient with a pre-opened dataset."""

    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
        cls.fits_path = _make_2d_fits()
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": _TEST_TOKEN}
        r = cls.client.post("/v1/datasets/open",
                            json={"path": cls.fits_path}, headers=cls._auth)
        data = r.json()
        cls.dataset_id = data["dataset_id"]
        cls.session_id = data.get("session_id", "")

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.fits_path)

    def _headers(self):
        h = dict(self._auth)
        if self.session_id:
            h["X-Visivo-Session"] = self.session_id
        return h


# ============================================================================
# R-01: CARTA script export
# ============================================================================

class TestCartaExport(ReproducibilityTestBase):
    """POST /v1/reproducibility/carta_script returns a CARTA Python script."""

    def test_returns_200(self):
        r = self.client.post("/v1/reproducibility/carta_script",
                             json={}, headers=self._headers())
        self.assertEqual(r.status_code, 200)

    def test_response_valid(self):
        r = self.client.post("/v1/reproducibility/carta_script",
                             json={}, headers=self._headers())
        data = r.json()
        self.assertTrue(data["valid"])

    def test_script_contains_import(self):
        """Script must import the carta module."""
        r = self.client.post("/v1/reproducibility/carta_script",
                             json={}, headers=self._headers())
        script = r.json()["script"]
        self.assertIn("import carta", script)

    def test_script_contains_open_file(self):
        """Script must call cc.open_file for the opened dataset."""
        r = self.client.post("/v1/reproducibility/carta_script",
                             json={}, headers=self._headers())
        script = r.json()["script"]
        self.assertIn("open_file", script)
        self.assertIn(self.fits_path, script)

    def test_script_n_datasets(self):
        r = self.client.post("/v1/reproducibility/carta_script",
                             json={}, headers=self._headers())
        data = r.json()
        self.assertGreaterEqual(data["n_datasets"], 1)

    def test_explicit_dataset_ids(self):
        """Passing an explicit list should only export those datasets."""
        r = self.client.post("/v1/reproducibility/carta_script",
                             json={"dataset_ids": [self.dataset_id]},
                             headers=self._headers())
        data = r.json()
        self.assertTrue(data["valid"])
        self.assertEqual(data["n_datasets"], 1)

    def test_unknown_dataset_id_ignored(self):
        """Unknown dataset_ids should be silently skipped (n_datasets=0)."""
        r = self.client.post("/v1/reproducibility/carta_script",
                             json={"dataset_ids": ["nonexistent_id"]},
                             headers=self._headers())
        data = r.json()
        self.assertTrue(data["valid"])
        self.assertEqual(data["n_datasets"], 0)

    def test_script_is_string(self):
        r = self.client.post("/v1/reproducibility/carta_script",
                             json={}, headers=self._headers())
        self.assertIsInstance(r.json()["script"], str)

    def test_get_returns_plain_text(self):
        """GET /v1/reproducibility/carta_script must return text/plain."""
        r = self.client.get("/v1/reproducibility/carta_script",
                            headers=self._headers())
        self.assertEqual(r.status_code, 200)
        self.assertIn("text/plain", r.headers.get("content-type", ""))

    def test_get_has_content_disposition(self):
        """GET endpoint must include Content-Disposition attachment header."""
        r = self.client.get("/v1/reproducibility/carta_script",
                            headers=self._headers())
        disp = r.headers.get("content-disposition", "")
        self.assertIn("attachment", disp)
        self.assertIn(".carta.py", disp)


# ============================================================================
# R-02: PROV-JSON export
# ============================================================================

class TestProvJson(ReproducibilityTestBase):
    """GET /v1/reproducibility/provenance returns a W3C PROV-JSON document."""

    def test_returns_200(self):
        r = self.client.get("/v1/reproducibility/provenance",
                            headers=self._headers())
        self.assertEqual(r.status_code, 200)

    def test_response_valid(self):
        r = self.client.get("/v1/reproducibility/provenance",
                            headers=self._headers())
        self.assertTrue(r.json()["valid"])

    def test_prov_has_required_keys(self):
        r = self.client.get("/v1/reproducibility/provenance",
                            headers=self._headers())
        prov = r.json()["prov"]
        for key in ("prefix", "entity", "activity",
                    "wasGeneratedBy", "used", "wasDerivedFrom"):
            self.assertIn(key, prov, f"Missing PROV-JSON key: {key}")

    def test_entity_contains_dataset(self):
        """The opened dataset should appear as an entity."""
        r = self.client.get("/v1/reproducibility/provenance",
                            headers=self._headers())
        entities = r.json()["prov"]["entity"]
        # Entity keys are prefixed: visivo:{dataset_id}
        entity_ids = {k.split(":")[-1] for k in entities}
        self.assertIn(self.dataset_id, entity_ids)

    def test_entity_has_path(self):
        """Each entity must record the file path."""
        r = self.client.get("/v1/reproducibility/provenance",
                            headers=self._headers())
        entities = r.json()["prov"]["entity"]
        for entity in entities.values():
            self.assertIn("visivo:path", entity)

    def test_prefix_contains_visivo(self):
        r = self.client.get("/v1/reproducibility/provenance",
                            headers=self._headers())
        prefix = r.json()["prov"]["prefix"]
        self.assertIn("visivo", prefix)

    def test_activity_after_snapshot(self):
        """After recording a snapshot the prov activity list should be non-empty."""
        # Record a snapshot first
        self.client.post("/v1/reproducibility/snapshots",
                         json={"operation": "moment", "dataset_id": self.dataset_id,
                               "params": {"order": 0}},
                         headers=self._headers())
        r = self.client.get("/v1/reproducibility/provenance",
                            headers=self._headers())
        activities = r.json()["prov"]["activity"]
        self.assertGreater(len(activities), 0)

    def test_was_derived_from_edge_present(self):
        """When a result_dataset_id is recorded, wasDerivedFrom must be populated."""
        # Manually record a history entry with both dataset_id and result_dataset_id
        import json, hmac, hashlib
        # We inject via the snapshot endpoint which stores into session.history
        self.client.post("/v1/reproducibility/snapshots",
                         json={"operation": "baseline",
                               "dataset_id": self.dataset_id,
                               "result_dataset_id": "ds_result_abc",
                               "params": {}},
                         headers=self._headers())
        r = self.client.get("/v1/reproducibility/provenance",
                            headers=self._headers())
        prov = r.json()["prov"]
        # wasDerivedFrom should have at least one entry
        self.assertGreater(len(prov["wasDerivedFrom"]), 0)

    def test_used_edge_present(self):
        """When an input dataset_id is recorded, used must be populated."""
        self.client.post("/v1/reproducibility/snapshots",
                         json={"operation": "stack",
                               "dataset_id": self.dataset_id,
                               "params": {}},
                         headers=self._headers())
        r = self.client.get("/v1/reproducibility/provenance",
                            headers=self._headers())
        prov = r.json()["prov"]
        self.assertGreater(len(prov["used"]), 0)


# ============================================================================
# R-01: Auto-recording of compute endpoint calls
# ============================================================================

class TestAutoHistory(ReproducibilityTestBase):
    """Verify that compute endpoints automatically append to session history (R-01)."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        # Open a spectral cube so we can call /v1/products/moment
        from astropy.io import fits
        import tempfile
        import numpy as np
        data = np.random.default_rng(7).standard_normal((4, 8, 8)).astype(np.float32)
        hdu = fits.PrimaryHDU(data)
        h = hdu.header
        h["NAXIS"] = 3
        h["CTYPE1"] = "RA---SIN";  h["CRPIX1"] = 4; h["CRVAL1"] = 83.8; h["CDELT1"] = -1e-4
        h["CTYPE2"] = "DEC--SIN"; h["CRPIX2"] = 4; h["CRVAL2"] = -5.4;  h["CDELT2"] = 1e-4
        h["CTYPE3"] = "FREQ";      h["CRPIX3"] = 1; h["CRVAL3"] = 1.4e9; h["CDELT3"] = 1e6
        tmp = tempfile.NamedTemporaryFile(suffix=".fits", delete=False)
        fits.HDUList([hdu]).writeto(tmp.name, overwrite=True)
        tmp.close()
        cls.cube_path = tmp.name
        # Open cube in the SAME session as the 2-D fits (pass session header)
        open_headers = dict(cls._auth)
        if cls.session_id:
            open_headers["X-Visivo-Session"] = cls.session_id
        r = cls.client.post("/v1/datasets/open",
                            json={"path": cls.cube_path}, headers=open_headers)
        cls.cube_id = r.json()["dataset_id"]

    def test_moment_auto_records_history(self):
        """Calling /v1/products/moment should append an entry to session history."""
        n_before = self.client.get(
            "/v1/reproducibility/snapshots", headers=self._headers()
        ).json()["n_entries"]

        resp = self.client.post("/v1/products/moment",
                                json={"dataset_id": self.cube_id, "moment_order": 0,
                                      "channel_start": 0, "channel_end": 3},
                                headers=self._headers())
        # Ensure moment succeeded before checking history
        self.assertTrue(resp.json().get("valid", False),
                        f"Moment call failed: {resp.json()}")

        n_after = self.client.get(
            "/v1/reproducibility/snapshots", headers=self._headers()
        ).json()["n_entries"]
        self.assertGreater(n_after, n_before)

    def test_moment_history_entry_has_endpoint(self):
        """The auto-recorded entry must include the endpoint path."""
        self.client.post("/v1/products/moment",
                         json={"dataset_id": self.cube_id, "order": 0,
                               "channel_start": 0, "channel_end": 3},
                         headers=self._headers())
        r = self.client.get("/v1/reproducibility/snapshots", headers=self._headers())
        history = r.json()["history"]
        endpoints = [e.get("endpoint", "") for e in history]
        self.assertIn("/v1/products/moment", endpoints)

    def test_moment_history_entry_has_dataset_id(self):
        """Auto-recorded entry must include the source dataset_id."""
        self.client.post("/v1/products/moment",
                         json={"dataset_id": self.cube_id, "order": 0,
                               "channel_start": 0, "channel_end": 3},
                         headers=self._headers())
        r = self.client.get("/v1/reproducibility/snapshots", headers=self._headers())
        history = r.json()["history"]
        ds_ids = [e.get("dataset_id", "") for e in history]
        self.assertIn(self.cube_id, ds_ids)

    def test_moment_history_entry_has_timestamp(self):
        """Auto-recorded entry must have an ISO-8601 timestamp."""
        self.client.post("/v1/products/moment",
                         json={"dataset_id": self.cube_id, "order": 0,
                               "channel_start": 0, "channel_end": 3},
                         headers=self._headers())
        r = self.client.get("/v1/reproducibility/snapshots", headers=self._headers())
        history = r.json()["history"]
        ts_list = [e.get("timestamp", "") for e in history if e.get("endpoint") == "/v1/products/moment"]
        self.assertTrue(any(ts for ts in ts_list), "timestamp missing in auto-recorded entry")


# ============================================================================
# R-03: Parameter snapshots
# ============================================================================

class TestSnapshots(ReproducibilityTestBase):
    """GET/POST /v1/reproducibility/snapshots."""

    def test_list_returns_200(self):
        r = self.client.get("/v1/reproducibility/snapshots",
                            headers=self._headers())
        self.assertEqual(r.status_code, 200)

    def test_list_valid(self):
        r = self.client.get("/v1/reproducibility/snapshots",
                            headers=self._headers())
        self.assertTrue(r.json()["valid"])

    def test_record_snapshot_returns_200(self):
        r = self.client.post("/v1/reproducibility/snapshots",
                             json={"operation": "gaussfit",
                                   "dataset_id": self.dataset_id,
                                   "params": {"box_arcsec": 30.0}},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200)

    def test_recorded_snapshot_visible_in_list(self):
        """A snapshot posted via POST should appear in the GET list."""
        self.client.post("/v1/reproducibility/snapshots",
                         json={"operation": "linewidth_test",
                               "dataset_id": self.dataset_id,
                               "params": {"channel_start": 0, "channel_end": 10}},
                         headers=self._headers())
        r = self.client.get("/v1/reproducibility/snapshots",
                            headers=self._headers())
        data = r.json()
        ops = [e["operation"] for e in data["history"]]
        self.assertIn("linewidth_test", ops)

    def test_n_entries_increases_after_record(self):
        r0 = self.client.get("/v1/reproducibility/snapshots",
                             headers=self._headers())
        n0 = r0.json()["n_entries"]
        self.client.post("/v1/reproducibility/snapshots",
                         json={"operation": "stack_test",
                               "params": {"method": "mean"}},
                         headers=self._headers())
        r1 = self.client.get("/v1/reproducibility/snapshots",
                             headers=self._headers())
        n1 = r1.json()["n_entries"]
        self.assertEqual(n1, n0 + 1)

    def test_snapshot_timestamp_auto_filled(self):
        """If timestamp is not provided it should be auto-filled by the server."""
        r = self.client.post("/v1/reproducibility/snapshots",
                             json={"operation": "ts_test", "params": {}},
                             headers=self._headers())
        history = r.json()["history"]
        ts_entry = next((e for e in history if e["operation"] == "ts_test"), None)
        self.assertIsNotNone(ts_entry)
        self.assertNotEqual(ts_entry.get("timestamp", ""), "")

    def test_snapshot_params_stored(self):
        """Params dict must be stored verbatim."""
        params = {"order": 2, "threshold": 0.5, "label": "test"}
        r = self.client.post("/v1/reproducibility/snapshots",
                             json={"operation": "params_test", "params": params},
                             headers=self._headers())
        history = r.json()["history"]
        entry = next((e for e in history if e["operation"] == "params_test"), None)
        self.assertIsNotNone(entry)
        self.assertEqual(entry["params"], params)

    def test_history_ordering_is_chronological(self):
        """Entries should appear in insertion order (oldest first)."""
        ops = ["op_alpha", "op_beta", "op_gamma"]
        for op in ops:
            self.client.post("/v1/reproducibility/snapshots",
                             json={"operation": op, "params": {}},
                             headers=self._headers())
        r = self.client.get("/v1/reproducibility/snapshots",
                            headers=self._headers())
        all_ops = [e["operation"] for e in r.json()["history"]]
        # Verify relative order (there may be earlier entries from other tests)
        idxs = [all_ops.index(op) for op in ops if op in all_ops]
        self.assertEqual(idxs, sorted(idxs), "History not in insertion order")


# ============================================================================
# R-03 enhanced: HMAC-signed named snapshots with UUID storage
# ============================================================================

class TestHmacSnapshot(ReproducibilityTestBase):
    """POST /v1/reproducibility/snapshot + GET /v1/reproducibility/snapshot/{id}."""

    def test_create_snapshot_returns_200(self):
        r = self.client.post("/v1/reproducibility/snapshot",
                             json={"label": "test_snap"},
                             headers=self._headers())
        self.assertEqual(r.status_code, 200)

    def test_create_snapshot_response_valid(self):
        r = self.client.post("/v1/reproducibility/snapshot",
                             json={"label": "test_snap2"},
                             headers=self._headers())
        data = r.json()
        self.assertTrue(data["valid"])

    def test_create_snapshot_has_uuid(self):
        r = self.client.post("/v1/reproducibility/snapshot",
                             json={"label": "uuid_test"},
                             headers=self._headers())
        snap_id = r.json()["snapshot_id"]
        import re
        uuid_re = re.compile(
            r"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"
        )
        self.assertRegex(snap_id, uuid_re, "snapshot_id must be a UUID4")

    def test_create_snapshot_has_hmac(self):
        r = self.client.post("/v1/reproducibility/snapshot",
                             json={"label": "hmac_test"},
                             headers=self._headers())
        data = r.json()
        self.assertIsInstance(data["hmac"], str)
        self.assertGreater(len(data["hmac"]), 0)

    def test_retrieve_snapshot_by_id(self):
        r_create = self.client.post("/v1/reproducibility/snapshot",
                                    json={"label": "retrieve_test"},
                                    headers=self._headers())
        snap_id = r_create.json()["snapshot_id"]
        r_get = self.client.get(f"/v1/reproducibility/snapshot/{snap_id}",
                                headers=self._headers())
        self.assertEqual(r_get.status_code, 200)
        self.assertTrue(r_get.json()["valid"])

    def test_retrieve_snapshot_hmac_matches(self):
        """Retrieved HMAC must equal the one returned at creation."""
        r_create = self.client.post("/v1/reproducibility/snapshot",
                                    json={"label": "hmac_match"},
                                    headers=self._headers())
        data_create = r_create.json()
        snap_id = data_create["snapshot_id"]
        hmac_create = data_create["hmac"]

        r_get = self.client.get(f"/v1/reproducibility/snapshot/{snap_id}",
                                headers=self._headers())
        hmac_retrieve = r_get.json()["hmac"]
        self.assertEqual(hmac_create, hmac_retrieve)

    def test_tampered_hmac_returns_403(self):
        """Providing a wrong HMAC should return HTTP 403."""
        r_create = self.client.post("/v1/reproducibility/snapshot",
                                    json={"label": "tamper_test"},
                                    headers=self._headers())
        snap_id = r_create.json()["snapshot_id"]
        r_get = self.client.get(
            f"/v1/reproducibility/snapshot/{snap_id}",
            params={"provided_hmac": "deadbeefdeadbeef" * 4},
            headers=self._headers(),
        )
        self.assertEqual(r_get.status_code, 403)

    def test_correct_hmac_returns_200(self):
        """Providing the correct HMAC should succeed."""
        r_create = self.client.post("/v1/reproducibility/snapshot",
                                    json={"label": "correct_hmac"},
                                    headers=self._headers())
        data = r_create.json()
        snap_id = data["snapshot_id"]
        correct_hmac = data["hmac"]

        r_get = self.client.get(
            f"/v1/reproducibility/snapshot/{snap_id}",
            params={"provided_hmac": correct_hmac},
            headers=self._headers(),
        )
        self.assertEqual(r_get.status_code, 200)

    def test_unknown_snapshot_id_returns_404(self):
        r = self.client.get("/v1/reproducibility/snapshot/nonexistent-uuid",
                            headers=self._headers())
        self.assertEqual(r.status_code, 404)

    def test_snapshot_state_contains_datasets(self):
        r_create = self.client.post("/v1/reproducibility/snapshot",
                                    json={"label": "state_test"},
                                    headers=self._headers())
        snap_id = r_create.json()["snapshot_id"]
        r_get = self.client.get(f"/v1/reproducibility/snapshot/{snap_id}",
                                headers=self._headers())
        state = r_get.json()["state"]
        self.assertIn("datasets", state)


# ============================================================================
# C-02: GET /v1/cosmology/distances with custom cosmology
# ============================================================================

class TestCosmologyDistances(unittest.TestCase):
    """Tests for the spec-conformant GET /v1/cosmology/distances endpoint."""

    _TEST_TOKEN = "test-token-visivo-12345"

    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("VISIVO_TOKEN", cls._TEST_TOKEN)
        from fastapi.testclient import TestClient
        from app.main import app
        cls.client = TestClient(app, raise_server_exceptions=True)
        cls._auth = {"X-Visivo-Token": cls._TEST_TOKEN}

    def _get(self, **params):
        return self.client.get("/v1/cosmology/distances",
                               params=params, headers=self._auth)

    def test_z0_comoving_zero(self):
        """z=0 → comoving distance = 0 Mpc."""
        r = self._get(z=0.0)
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["valid"])
        self.assertAlmostEqual(data["comoving_Mpc"], 0.0, places=3)

    def test_z0_luminosity_zero(self):
        """z=0 → luminosity distance = 0 Mpc."""
        r = self._get(z=0.0)
        self.assertAlmostEqual(r.json()["luminosity_Mpc"], 0.0, places=3)

    def test_response_has_all_required_fields(self):
        r = self._get(z=1.0)
        data = r.json()
        for field in ("comoving_Mpc", "luminosity_Mpc", "angular_diameter_Mpc",
                      "lookback_time_Gyr", "age_Gyr"):
            self.assertIn(field, data, f"Missing required field: {field}")
            self.assertIsNotNone(data[field])

    def test_z1_standard_lcdm_reference(self):
        """z=1 with Planck18 cosmology should match astropy to 0.1%."""
        try:
            from astropy.cosmology import Planck18
            d_c_ref = float(Planck18.comoving_distance(1.0).to("Mpc").value)
        except ImportError:
            self.skipTest("astropy not available")
        r = self._get(z=1.0, H0=Planck18.H0.value, Om0=Planck18.Om0,
                      Ode0=Planck18.Ode0)
        data = r.json()
        self.assertTrue(data["valid"])
        self.assertAlmostEqual(data["comoving_Mpc"], d_c_ref,
                               delta=0.001 * d_c_ref,
                               msg="Comoving distance not within 0.1% of Planck18 reference")

    def test_invalid_z_too_large_returns_422(self):
        r = self._get(z=25.0)
        self.assertEqual(r.status_code, 422)

    def test_invalid_h0_too_low_returns_422(self):
        r = self._get(z=1.0, H0=30.0)
        self.assertEqual(r.status_code, 422)

    def test_invalid_om0_ode0_sum_returns_422(self):
        r = self._get(z=1.0, Om0=0.7, Ode0=0.7)
        self.assertEqual(r.status_code, 422)

    def test_lookback_time_positive_for_z_gt_0(self):
        r = self._get(z=1.0)
        self.assertGreater(r.json()["lookback_time_Gyr"], 0.0)

    def test_age_decreases_with_z(self):
        """Age of universe at z=2 should be less than at z=0.5."""
        r0 = self._get(z=0.5)
        r1 = self._get(z=2.0)
        self.assertGreater(r0.json()["age_Gyr"], r1.json()["age_Gyr"])


if __name__ == "__main__":
    unittest.main()
