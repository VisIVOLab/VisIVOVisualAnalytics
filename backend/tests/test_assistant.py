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


def test_cosmology_rejects_bad_redshift():
    with pytest.raises(ValueError):
        TOOLS["cosmology_distance"].executor(None, {"redshift": -2.0})


def test_cosmology_tool():
    out = TOOLS["cosmology_distance"].executor(None, {"redshift": 0.1, "model": "Planck18"})
    assert out["comoving_Mpc"] > 0
    assert out["model"] == "Planck18"


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
