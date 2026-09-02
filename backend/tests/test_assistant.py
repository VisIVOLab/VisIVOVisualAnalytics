"""Copilot (R-22) tests.

Cover the deterministic parts that must work with NO LLM key: the tool
catalogue, tool execution against real workers, the agent loop (driven by a
scripted fake provider), provenance capture, and the NullProvider fallback.
"""
from __future__ import annotations

import os

import numpy as np
import pytest
from astropy.io import fits

from app.assistant import AssistantEngine
from app.assistant.provider import (
    AssistantProvider,
    NullProvider,
    ProviderResponse,
    ToolCall,
    provider_from_env,
)
from app.assistant.tools import TOOLS, tool_specs

_TEST_TOKEN = "test-token-visivo-12345"


def _write_cube(path, nz=16, ny=8, nx=8):
    rng = np.random.default_rng(0)
    data = rng.standard_normal((nz, ny, nx)).astype(np.float32)
    data[6:10, 2:6, 2:6] += 5.0
    h = fits.Header()
    h["NAXIS"] = 3; h["NAXIS1"] = nx; h["NAXIS2"] = ny; h["NAXIS3"] = nz
    h["CTYPE1"] = "RA---SIN"; h["CTYPE2"] = "DEC--SIN"; h["CTYPE3"] = "FREQ"
    h["CRVAL1"] = 10.0; h["CRVAL2"] = 20.0; h["CRVAL3"] = 1.4e9
    h["CRPIX1"] = 4.0; h["CRPIX2"] = 4.0; h["CRPIX3"] = 8.0
    h["CDELT1"] = -1e-3; h["CDELT2"] = 1e-3; h["CDELT3"] = 1e6
    h["CUNIT1"] = "deg"; h["CUNIT2"] = "deg"; h["CUNIT3"] = "Hz"
    h["BUNIT"] = "Jy/beam"; h["BMAJ"] = 3e-3; h["BMIN"] = 2e-3; h["BPA"] = 0.0
    fits.PrimaryHDU(data, header=h).writeto(path, overwrite=True)


# ── Tool catalogue + direct execution ─────────────────────────────────────────

def test_tool_specs_are_wellformed():
    specs = tool_specs()
    assert specs and all("name" in s and "input_schema" in s for s in specs)
    assert {"dataset_metadata", "cube_statistics", "moment_map_summary",
            "cosmology_distance"} <= set(TOOLS)


def test_dataset_metadata_tool(tmp_path):
    path = str(tmp_path / "c.fits")
    _write_cube(path)
    md = TOOLS["dataset_metadata"].executor(path, {})
    assert md["bunit"] == "Jy/beam"
    assert md["ctype"][2] == "FREQ"
    assert md["beam_major"] == pytest.approx(3e-3)


def test_moment_summary_tool_reports_dispersion(tmp_path):
    path = str(tmp_path / "c2.fits")
    _write_cube(path)
    out = TOOLS["moment_map_summary"].executor(path, {"order": 2, "channel_end": 15})
    assert out["order"] == 2
    assert "dispersion" in (out["note"] or "")


def test_moment_summary_defaults_to_full_depth(tmp_path):
    """No channel range → whole spectral axis, and the EFFECTIVE range is reported."""
    path = str(tmp_path / "cfd.fits")
    _write_cube(path, nz=16)
    out = TOOLS["moment_map_summary"].executor(path, {"order": 0})
    assert out["channel_start"] == 0
    assert out["channel_end"] == 15
    assert out["n_channels_in_cube"] == 16


def test_moment_summary_rejects_bad_order(tmp_path):
    path = str(tmp_path / "cbo.fits")
    _write_cube(path)
    with pytest.raises(ValueError):
        TOOLS["moment_map_summary"].executor(path, {"order": 7})


def test_noise_estimate_tool(tmp_path):
    path = str(tmp_path / "cn.fits")
    _write_cube(path)
    out = TOOLS["noise_estimate"].executor(path, {})   # full-cube default
    assert out["bunit"] == "Jy/beam"
    assert out["num_channels"] == 16
    assert out["sigma_median"] is not None and out["sigma_median"] > 0


def test_aperture_photometry_tool(tmp_path):
    path = str(tmp_path / "ca.fits")
    _write_cube(path)
    # Source sits at the reference sky position (CRVAL 10/20), channel 6-9.
    out = TOOLS["aperture_photometry"].executor(
        path, {"ra_deg": 10.0, "dec_deg": 20.0, "aperture_arcsec": 7.0, "channel": 7})
    assert out["flux_unit"] and out["n_pix_aperture"] > 0
    assert "flux" in out and out["bunit"] == "Jy/beam"


def test_aperture_photometry_requires_positive_aperture(tmp_path):
    path = str(tmp_path / "ca2.fits")
    _write_cube(path)
    with pytest.raises((ValueError, KeyError)):
        TOOLS["aperture_photometry"].executor(path, {"ra_deg": 10.0, "dec_deg": 20.0})


def test_set_scaling_action_validates_and_builds_directive():
    out = TOOLS["set_scaling"].executor(None, {"scale": "LOG"})
    assert out["status"] == "queued"
    assert out["action"] == {"type": "set_scaling", "scale": "log"}
    with pytest.raises(ValueError):
        TOOLS["set_scaling"].executor(None, {"scale": "rainbow"})


def test_set_color_field_action_validates_and_builds_directive():
    out = TOOLS["set_color_field"].executor(None, {"field": "density"})
    assert out["action"] == {"type": "set_color_field", "field": "density"}
    with pytest.raises(ValueError):
        TOOLS["set_color_field"].executor(None, {"field": "  "})


def test_measurement_tools_reject_non_integral_and_bad_params(tmp_path):
    path = str(tmp_path / "cv.fits")
    _write_cube(path)
    # A fractional channel would silently measure a different plane — reject it.
    with pytest.raises(ValueError):
        TOOLS["aperture_photometry"].executor(
            path, {"ra_deg": 10.0, "dec_deg": 20.0, "aperture_arcsec": 7.0, "channel": 7.5})
    # Annulus must be a proper ring.
    with pytest.raises(ValueError):
        TOOLS["aperture_photometry"].executor(
            path, {"ra_deg": 10.0, "dec_deg": 20.0, "aperture_arcsec": 7.0,
                   "annulus_inner_arcsec": 9.0, "annulus_outer_arcsec": 8.0})
    # Non-finite position.
    with pytest.raises(ValueError):
        TOOLS["source_gaussfit"].executor(
            path, {"ra_deg": float("nan"), "dec_deg": 20.0, "box_arcsec": 22.0})


def test_source_gaussfit_tool(tmp_path):
    path = str(tmp_path / "cg.fits")
    _write_cube(path)
    out = TOOLS["source_gaussfit"].executor(
        path, {"ra_deg": 10.0, "dec_deg": 20.0, "box_arcsec": 22.0, "channel": 7})
    assert "peak_jy_beam" in out and "major_arcsec" in out and "pa_deg" in out


def test_angular_power_spectrum_tool(tmp_path):
    path = str(tmp_path / "cp.fits")
    _write_cube(path)
    out = TOOLS["angular_power_spectrum"].executor(path, {"channel": 7, "n_bins": 4})
    assert isinstance(out, dict) and out


def test_fit_line_tool_probe(tmp_path):
    path = str(tmp_path / "cfit.fits")
    _write_cube(path)   # bright blob at x,y 2..5, channels 6..9
    out = TOOLS["fit_line"].executor(path, {"x": 3, "y": 3})
    assert out["extraction"] == "probe"
    assert "fwhm" in out and "center" in out and "integrated_flux" in out
    assert out["n_channels"] == 16


def test_fit_line_requires_position(tmp_path):
    path = str(tmp_path / "cfit2.fits")
    _write_cube(path)
    with pytest.raises(ValueError):
        TOOLS["fit_line"].executor(path, {})


def test_fit_line_radec_without_wcs_falls_back(tmp_path):
    # ra/dec on the WCS cube resolves to a pixel and fits (source at CRVAL 10/20).
    path = str(tmp_path / "cfit3.fits")
    _write_cube(path)
    out = TOOLS["fit_line"].executor(path, {"ra_deg": 10.0, "dec_deg": 20.0})
    assert "fwhm" in out


def test_fit_line_rejects_off_cube_position(tmp_path):
    path = str(tmp_path / "cfit4.fits")
    _write_cube(path)   # 8×8 spatial
    with pytest.raises(ValueError, match="outside"):
        TOOLS["fit_line"].executor(path, {"x": 99, "y": 3, "radius_arcsec": 5.0})


def test_cross_match_rejects_unknown_catalogue(tmp_path):
    path = str(tmp_path / "cxm.fits")
    _write_cube(path)
    with pytest.raises(ValueError, match="catalogue"):
        TOOLS["cross_match"].executor(path, {"catalogue": "gaia"})


def test_cross_match_wraps_worker(tmp_path, monkeypatch):
    # Avoid the network: stub the worker and check the tool's summary + nan scrub.
    import app.compute as _compute
    def _fake(path, radius, catalogue, max_sources):
        return {"valid": True, "n_sources": 2, "centre_ra_deg": 10.0,
                "centre_dec_deg": 20.0, "truncated": False,
                "sources": [{"name": "A", "flux": float("nan")}, {"name": "B", "flux": 1.5}]}
    monkeypatch.setattr(_compute, "worker_crossmatch", _fake, raising=True)
    path = str(tmp_path / "cxm2.fits")
    _write_cube(path)
    out = TOOLS["cross_match"].executor(path, {"catalogue": "NVSS", "radius_arcsec": 30.0})
    assert out["catalogue"] == "nvss" and out["n_sources"] == 2
    assert out["sources"][0]["flux"] is None      # nan scrubbed
    assert out["sources"][1]["flux"] == 1.5


def test_find_sources_without_sofia_raises(tmp_path, monkeypatch):
    import app.compute as _compute
    monkeypatch.setattr(_compute, "resolve_sofia_bin", lambda: None, raising=True)
    path = str(tmp_path / "csf.fits")
    _write_cube(path)
    with pytest.raises(ValueError, match="SoFiA"):
        TOOLS["find_sources"].executor(path, {})


def test_cosmology_rejects_bad_redshift():
    with pytest.raises(ValueError):
        TOOLS["cosmology_distance"].executor(None, {"redshift": -2.0})


def test_cosmology_tool():
    out = TOOLS["cosmology_distance"].executor(None, {"redshift": 0.1, "model": "Planck18"})
    assert out["comoving_Mpc"] > 0
    assert out["model"] == "Planck18"


def test_catalogue_summary_tool(tmp_path):
    path = tmp_path / "cat.csv"
    path.write_text("RA,DEC,z\n10.0,-5.0,0.1\n20.0,5.0,0.3\n", encoding="utf-8")
    out = TOOLS["catalogue_summary"].executor(str(path), {})
    assert out["kind"] == "catalogue"
    assert out["num_rows"] == 2
    assert out["coordinate_fields"]["system"] == "sky"
    zcol = next(c for c in out["columns"] if c["name"] == "z")
    assert zcol["min"] == 0.1 and zcol["max"] == 0.3


def test_catalogue_summary_rejects_unknown_extension(tmp_path):
    path = tmp_path / "cat.dat"
    path.write_text("nope", encoding="utf-8")
    with pytest.raises(ValueError):
        TOOLS["catalogue_summary"].executor(str(path), {})


def test_catalogue_summary_prefers_cached_dataframe():
    # Multi-file catalogues are concatenated in-session; the tool must summarise
    # the CACHED frame (correct row count), not re-read one path. It also reports
    # finite_count=0 with None stats for an all-NaN numeric column.
    import numpy as _np
    import pandas as _pd

    frame = _pd.DataFrame({"RA": [1.0, 2.0, 3.0], "DEC": [0.0, 0.0, 0.0],
                           "flux": [_np.nan, _np.nan, _np.nan]})
    context = {"kind": "catalogue", "format": "csv", "path": "/nonexistent.csv",
               "dataframe": frame}
    out = TOOLS["catalogue_summary"].executor(None, {}, context)
    assert out["num_rows"] == 3
    flux = next(c for c in out["columns"] if c["name"] == "flux")
    assert flux["finite_count"] == 0
    assert flux["min"] is None and flux["mean"] is None


def test_vbt_summary_tool_reads_memmap():
    # VBT datasets expose a field-major (field_count, num_rows) array via the
    # session entry — no filesystem path. vbt_summary must stat it from context.
    arr = np.array([[1.0, 2.0, 3.0, 4.0], [10.0, np.nan, 30.0, 40.0]], dtype="float64")
    header = {"field_names": ["x", "flux"], "field_count": 2, "num_rows": 4,
              "scalar_type": "float64", "kind": "point"}
    context = {"kind": "vbt", "header": header, "memmap": arr}
    out = TOOLS["vbt_summary"].executor(None, {}, context)
    assert out["vbt_kind"] == "point"
    assert out["num_rows"] == 4
    flux = next(f for f in out["fields"] if f["name"] == "flux")
    assert flux["finite_count"] == 3
    assert flux["min"] == 10.0 and flux["max"] == 40.0


def test_vbt_summary_requires_vbt_context():
    with pytest.raises(ValueError):
        TOOLS["vbt_summary"].executor(None, {}, {"kind": "catalogue"})


class _CatalogueProvider(AssistantProvider):
    """Fake LLM: calls catalogue_summary once, then answers."""

    name = "scripted"
    configured = True

    def __init__(self):
        self._turn = 0

    def complete(self, system, messages, tools):
        self._turn += 1
        if self._turn == 1:
            return ProviderResponse(tool_calls=[
                ToolCall(id="c1", name="catalogue_summary", arguments={})])
        return ProviderResponse(final_text="The catalogue has 3 rows.")


def test_engine_passes_dataset_entry_to_context_tool():
    # A wants_context tool must receive the cached session entry (dataframe), so
    # catalogue_summary reports the in-session row count — not a re-read of path.
    import pandas as _pd

    frame = _pd.DataFrame({"RA": [1.0, 2.0, 3.0], "DEC": [0.0, 1.0, 2.0]})
    entry = {"kind": "catalogue", "format": "csv", "path": "/nonexistent.csv",
             "dataframe": frame}
    engine = AssistantEngine(_CatalogueProvider())
    res = engine.run("Describe the catalogue.", "/nonexistent.csv",
                     dataset_id="cat1", dataset_entry=entry)
    assert res.tool_calls[0].name == "catalogue_summary"
    assert res.tool_calls[0].error is None
    assert res.tool_calls[0].result["num_rows"] == 3


# ── Agent loop with a scripted fake provider ──────────────────────────────────

class _ScriptedProvider(AssistantProvider):
    """Fake LLM: first turn asks for a tool, second turn answers using the result."""

    name = "scripted"
    configured = True

    def __init__(self):
        self._turn = 0

    def complete(self, system, messages, tools):
        self._turn += 1
        if self._turn == 1:
            return ProviderResponse(tool_calls=[
                ToolCall(id="t1", name="dataset_metadata", arguments={})])
        return ProviderResponse(final_text="The cube's BUNIT is Jy/beam.")


def test_engine_runs_tool_then_answers(tmp_path):
    path = str(tmp_path / "c3.fits")
    _write_cube(path)
    engine = AssistantEngine(_ScriptedProvider())
    res = engine.run("What are the units?", path, dataset_id="ds1")
    assert res.answer == "The cube's BUNIT is Jy/beam."
    assert len(res.tool_calls) == 1
    assert res.tool_calls[0].name == "dataset_metadata"
    assert res.tool_calls[0].result["bunit"] == "Jy/beam"
    assert res.tool_calls[0].error is None
    assert "dataset_metadata" in res.reproducibility_script
    assert res.tool_calls_recorded


class _ActionProvider(AssistantProvider):
    """Fake LLM that drives two visualisation actions, then answers."""

    name = "scripted"
    configured = True

    def __init__(self):
        self._turn = 0

    def complete(self, system, messages, tools):
        self._turn += 1
        if self._turn == 1:
            return ProviderResponse(tool_calls=[
                ToolCall(id="a1", name="set_colormap", arguments={"name": "inferno"}),
                ToolCall(id="a2", name="set_blend_mode", arguments={"mode": "mip"})])
        return ProviderResponse(final_text="Set inferno + MIP to highlight the source.")


def test_engine_collects_visualisation_actions(tmp_path):
    path = str(tmp_path / "cact.fits")
    _write_cube(path)
    res = AssistantEngine(_ActionProvider()).run("show this source better", path, dataset_id="ds1")
    assert res.actions == [
        {"type": "set_colormap", "name": "inferno"},
        {"type": "set_blend_mode", "mode": "mip"},
    ]
    # The action ack (not data) is fed back to the model.
    assert res.tool_calls[0].result == {"action": {"type": "set_colormap", "name": "inferno"},
                                        "status": "queued"}
    assert res.to_dict()["actions"] == res.actions


def test_action_tool_validates_params(tmp_path):
    from app.assistant.tools import TOOLS as _T
    with pytest.raises(ValueError):
        _T["set_render_mode"].executor(None, {"mode": "bogus"})
    with pytest.raises(ValueError):
        _T["set_blend_mode"].executor(None, {"mode": "bogus"})
    # a valid one returns the queued directive envelope
    assert _T["goto_channel"].executor(None, {"index": 42}) == {
        "action": {"type": "goto_channel", "index": 42}, "status": "queued"}


def test_compute_moment_action_builds_directive(tmp_path):
    from app.assistant.tools import TOOLS as _T
    # full args → all forwarded into the directive
    out = _T["compute_moment"].executor(None, {
        "order": 0, "channel_start": 5, "channel_end": 90,
        "mask_enabled": True, "threshold_auto": True})
    assert out["status"] == "queued"
    d = out["action"]
    assert d == {"type": "compute_moment", "order": 0, "channel_start": 5,
                 "channel_end": 90, "mask_enabled": True, "threshold_auto": True}
    # minimal args → just order; a bad order is rejected
    assert _T["compute_moment"].executor(None, {"order": 2})["action"] == {
        "type": "compute_moment", "order": 2}
    with pytest.raises(ValueError):
        _T["compute_moment"].executor(None, {"order": 7})


def test_engine_surfaces_tool_error_to_model(tmp_path):
    class _BadToolProvider(_ScriptedProvider):
        def complete(self, system, messages, tools):
            self._turn += 1
            if self._turn == 1:
                return ProviderResponse(tool_calls=[
                    ToolCall(id="t1", name="cube_statistics", arguments={})])
            return ProviderResponse(final_text="done")

    # No dataset path → cube_statistics raises; the engine records the error
    # rather than crashing, and feeds it back to the model.
    engine = AssistantEngine(_BadToolProvider())
    res = engine.run("stats?", None)
    assert res.tool_calls[0].error is not None


# ── Provider selection (Claude / OpenAI / self-hosted / null) ─────────────────

def test_null_provider_when_nothing_configured():
    from app.assistant.provider import configure, provider_from_env as pfe
    configure(provider="auto", anthropic_key="", openai_key="", base_url="")
    assert isinstance(pfe(), NullProvider)


def test_provider_resolution_auto():
    from app.assistant import provider as P
    # base_url (self-hosted) → openai path
    P.configure(provider="auto", anthropic_key="", openai_key="", base_url="http://gpu:8000/v1")
    assert P._resolve_provider_name() == "openai"
    # only anthropic key → anthropic
    P.configure(provider="auto", anthropic_key="sk-ant", openai_key="", base_url="")
    assert P._resolve_provider_name() == "anthropic"
    # explicit choice wins
    P.configure(provider="null")
    assert P._resolve_provider_name() == "null"
    P.configure(provider="auto", anthropic_key="", openai_key="", base_url="")  # reset


def test_public_config_hides_secrets():
    from app.assistant import provider as P
    P.configure(provider="anthropic", anthropic_key="SECRET123", base_url="")
    cfg = P.public_config()
    assert cfg["provider"] == "anthropic"
    assert cfg["anthropic_key_set"] is True
    assert "SECRET123" not in str(cfg)          # the key value is never exposed
    assert "anthropic" in cfg["available"] and "openai" in cfg["available"]
    P.configure(provider="auto", anthropic_key="", openai_key="", base_url="")


def test_openai_message_and_tool_translation():
    """OpenAIProvider translates the neutral history + tools to OpenAI shape
    (no SDK needed — the translators are static)."""
    from app.assistant.provider import OpenAIProvider, ToolCall
    history = [
        {"role": "user", "text": "hi"},
        {"role": "assistant", "tool_calls": [ToolCall(id="t1", name="foo", arguments={"a": 1})]},
        {"role": "tool", "results": [{"id": "t1", "name": "foo", "content": "{}", "is_error": False}]},
    ]
    msgs = OpenAIProvider._to_messages("SYS", history)
    assert msgs[0] == {"role": "system", "content": "SYS"}
    assert msgs[1] == {"role": "user", "content": "hi"}
    assert msgs[2]["role"] == "assistant" and msgs[2]["tool_calls"][0]["function"]["name"] == "foo"
    assert msgs[2]["tool_calls"][0]["function"]["arguments"] == '{"a": 1}'  # JSON string
    assert msgs[3] == {"role": "tool", "tool_call_id": "t1", "content": "{}"}
    otools = OpenAIProvider._to_tools(tool_specs())
    assert otools[0]["type"] == "function" and "parameters" in otools[0]["function"]


def test_base_url_validation_rejects_secrets():
    import pytest as _pytest
    from app.assistant import provider as P
    with _pytest.raises(ValueError):
        P.configure(base_url="http://user:pass@host:8000/v1")   # embedded creds
    with _pytest.raises(ValueError):
        P.configure(base_url="http://host:8000/v1?token=abc")    # query secret
    with _pytest.raises(ValueError):
        P.configure(base_url="ftp://host/v1")                    # bad scheme
    P.configure(base_url="")  # reset


def test_key_separation_cloud_key_not_used_for_self_hosted():
    """SECURITY: a stored CLOUD openai_key must never be paired with a base_url
    (self-hosted) endpoint — the self-hosted path uses only base_url_key."""
    from app.assistant import provider as P
    P.configure(provider="openai", openai_key="CLOUD_SECRET",
                base_url="http://gpu:8000/v1", base_url_key="LOCAL")
    assert P._resolve_provider_name() == "openai"
    # The config keeps them separate; the resolver's self-hosted branch reads
    # base_url_key, never openai_key.
    assert P._CONFIG["openai_key"] == "CLOUD_SECRET"
    assert P._CONFIG["base_url_key"] == "LOCAL"
    cfg = P.public_config()
    assert "CLOUD_SECRET" not in str(cfg) and "LOCAL" not in str(cfg)
    P.configure(provider="auto", openai_key="", base_url="", base_url_key="")


def test_config_endpoint_selects_and_hides_secrets():
    client, auth = _client_and_auth()
    r = client.post("/v1/assistant/config", json={"provider": "null"}, headers=auth)
    assert r.status_code == 200 and r.json()["config"]["provider"] == "null"
    # Self-hosted server with its own key — status reflects the URL, no key leaks.
    r = client.post("/v1/assistant/config",
                    json={"provider": "openai", "base_url": "http://gpu:8000/v1",
                          "base_url_key": "SECRET"}, headers=auth)
    assert r.status_code == 200
    assert r.json()["config"]["base_url"] == "http://gpu:8000/v1"
    assert r.json()["config"]["base_url_key_set"] is True
    assert "SECRET" not in r.text
    # A bad base_url is rejected with 422, not stored.
    r = client.post("/v1/assistant/config",
                    json={"provider": "openai", "base_url": "http://u:p@gpu/v1"}, headers=auth)
    assert r.status_code == 422
    # Reset.
    client.post("/v1/assistant/config",
                json={"provider": "auto", "anthropic_key": "", "openai_key": "",
                      "base_url": "", "base_url_key": ""}, headers=auth)


def _client_and_auth():
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient
    from app.main import app
    return TestClient(app), {"X-Visivo-Token": _TEST_TOKEN}


def test_engine_with_null_provider_explains(tmp_path):
    engine = AssistantEngine(NullProvider())
    res = engine.run("hi", None)
    assert not res.configured
    assert "not configured" in res.answer.lower()


# ── HTTP surface ──────────────────────────────────────────────────────────────

def test_assistant_endpoints():
    os.environ.setdefault("VISIVO_TOKEN", _TEST_TOKEN)
    from fastapi.testclient import TestClient
    from app.main import app

    client = TestClient(app)
    auth = {"X-Visivo-Token": _TEST_TOKEN}
    r = client.get("/v1/assistant/status", headers=auth)
    assert r.status_code == 200
    assert "configured" in r.json()
    r = client.get("/v1/assistant/tools", headers=auth)
    assert r.status_code == 200 and r.json()["tools"]
    r = client.post("/v1/assistant/query", json={"question": "hello"}, headers=auth)
    assert r.status_code == 200
    body = r.json()
    assert "answer" in body and "reproducibility_script" in body
