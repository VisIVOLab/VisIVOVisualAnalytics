"""Routes that no test exercised (E-08).

Measured rather than guessed: extracting every ``@router`` path and grepping the
suite (excluding the contract snapshot, which lists them all by construction)
showed 37 of 129 routes — 29 % — with no test at all. This module takes the ones
where a test can actually find something.

Two of them matter more than the rest. ``kinematic_levels_bin`` is a route added
recently whose compute layer is thoroughly tested while its HTTP contract — the
binary layout and the headers a client parses — was not. And the SAMP endpoints
are the largest untested cluster AND the ones put behind an operator gate for
multi-user isolation; a gate that is never exercised is a gate nobody knows
works.
"""

from __future__ import annotations

import os

import numpy as np
import pytest
from httpx import AsyncClient

# Every SAMP route. The hub itself cannot be stood up in a test, but the
# authorisation decision in front of it can, and that is the part isolation
# depends on.
SAMP_ROUTES = [
    ("post", "/v1/samp/send", {}),
    ("post", "/v1/samp/receive", {}),
    ("get", "/v1/samp/pending", None),
    ("get", "/v1/samp/inbox", None),
    ("get", "/v1/samp/status", None),
    ("post", "/v1/samp/hub-status", {}),
    ("post", "/v1/samp/connect", {}),
    ("post", "/v1/samp/files/register", {}),
    ("post", "/v1/samp/import-url", {}),
    ("post", "/v1/samp/send-fits", {}),
    ("post", "/v1/samp/send-catalogue", {}),
    ("post", "/v1/samp/upload-file", {"filename": "x.txt", "content_base64": "WA=="}),
]


# ── the route added with the geodesic level field ────────────────────────────

@pytest.mark.asyncio
async def test_kinematic_levels_bin_layout(client: AsyncClient, opened_cube: dict) -> None:
    """The HTTP contract a desktop client parses: two uint8 fields of identical
    geometry, add then sub, with the ROI geometry in headers."""
    resp = await client.post(
        "/v1/cube/kinematic_levels_bin",
        json={
            "dataset_id": opened_cube["dataset_id"],
            "seed_z": 16, "seed_y": 32, "seed_x": 32,
            "reach": 8.0, "reach_max": 16.0, "lambda_v": 1.0, "roi_radius": 20,
        },
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200, resp.text
    w = int(resp.headers["X-Visivo-Width"])
    h = int(resp.headers["X-Visivo-Height"])
    d = int(resp.headers["X-Visivo-Depth"])
    assert w > 0 and h > 0 and d > 0
    assert resp.headers["X-Visivo-Scalar-Type"] == "uint8"
    assert resp.headers["X-Visivo-Components"] == "2"
    assert len(resp.content) == 2 * w * h * d, "add and sub fields, same geometry"
    # The budget the field can answer for, and the sentinel a client must exclude.
    assert int(resp.headers["X-Visivo-Range-Max"]) == 16
    assert int(resp.headers["X-Visivo-Peak"]) == 255

    add = np.frombuffer(resp.content[: w * h * d], dtype=np.uint8)
    sub = np.frombuffer(resp.content[w * h * d:], dtype=np.uint8)
    assert add.min() == 0, "the seed must be level 0"
    assert add.max() == 255, "voxels beyond the budget carry the sentinel"
    assert (sub == 255).all(), "no carve seeds were given, so nothing is subtracted"
    # No real level may exceed the requested budget.
    assert add[add != 255].max() <= 16


@pytest.mark.asyncio
async def test_kinematic_levels_bin_rejects_an_unrepresentable_budget(
        client: AsyncClient, opened_cube: dict) -> None:
    """reach_max is capped at 254 because 255 is the unreachable sentinel."""
    resp = await client.post(
        "/v1/cube/kinematic_levels_bin",
        json={"dataset_id": opened_cube["dataset_id"],
              "seed_z": 16, "seed_y": 32, "seed_x": 32, "reach_max": 300.0},
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 422


@pytest.mark.asyncio
async def test_kinematic_levels_bin_rejects_a_seed_outside_the_cube(
        client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/kinematic_levels_bin",
        json={"dataset_id": opened_cube["dataset_id"],
              "seed_z": 9999, "seed_y": 32, "seed_x": 32, "reach_max": 16.0},
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 422, "a bad seed is user input, not a server error"


# ── SAMP: the operator gate that multi-user isolation rests on ───────────────

@pytest.mark.asyncio
@pytest.mark.parametrize(("verb", "path", "body"), SAMP_ROUTES)
async def test_samp_routes_require_authentication(
        client: AsyncClient, verb: str, path: str, body: dict | None) -> None:
    call = getattr(client, verb)
    resp = await (call(path) if body is None else call(path, json=body))
    assert resp.status_code == 401, f"{path} answered {resp.status_code} unauthenticated"


@pytest.mark.asyncio
@pytest.mark.parametrize(("verb", "path", "body"), SAMP_ROUTES)
async def test_samp_routes_are_operator_only_in_multi_user_mode(
        client: AsyncClient, auth_headers: dict, monkeypatch, verb: str,
        path: str, body: dict | None) -> None:
    """The SAMP hub is one per machine and talks to local desktop apps, so on a
    shared backend only the operator may reach it. With a single token everyone
    IS the operator; the gate only bites once per-user tokens exist.
    """
    monkeypatch.setenv("VISIVO_TOKENS", "alice-token-aaaaaaaaaaaaaaaaaaaa")
    call = getattr(client, verb)
    headers = {"X-Visivo-Token": "alice-token-aaaaaaaaaaaaaaaaaaaa"}
    resp = await (call(path, headers=headers) if body is None
                  else call(path, json=body, headers=headers))
    assert resp.status_code == 403, (
        f"{path} let a non-operator token through with {resp.status_code}")

    # The operator's own token still gets past the gate. Whatever happens after
    # depends on there being a hub, so anything except 403 is acceptable here —
    # the gate is what is under test.
    resp = await (call(path, headers=auth_headers) if body is None
                  else call(path, json=body, headers=auth_headers))
    assert resp.status_code != 403, f"{path} refused the operator"


# ── VBT: VisIVO's own binary table format ────────────────────────────────────

@pytest.mark.asyncio
async def test_vbt_open_rejects_a_missing_header_file(
        client: AsyncClient, auth_headers: dict, tmp_path) -> None:
    resp = await client.post("/v1/vbt/open",
                             json={"path": str(tmp_path / "nope.head")},
                             headers=auth_headers)
    assert resp.status_code == 200
    body = resp.json()
    assert body["valid"] is False and "not found" in body["error"].lower()


@pytest.mark.asyncio
async def test_vbt_open_reports_a_truncated_header(
        client: AsyncClient, auth_headers: dict, tmp_path) -> None:
    """A .head with fewer than the four required lines must be reported, not
    raise — the file is user input."""
    head = tmp_path / "bad.head"
    head.write_text("float\n3\n")
    (tmp_path / "bad").write_bytes(b"\x00" * 16)
    resp = await client.post("/v1/vbt/open", json={"path": str(head)},
                             headers=auth_headers)
    assert resp.status_code == 200
    assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_vbt_round_trip(client: AsyncClient, auth_headers: dict, tmp_path) -> None:
    """Open a real point-table VBT and pull a subset back."""
    # The real .head layout: scalar type, field count, row count, endianness,
    # then one line per field. My first attempt omitted the endianness line and
    # the test skipped itself — a skip that hides the format being wrong is worse
    # than no test, so the fixture is written from the parser's own contract.
    n = 64
    head = tmp_path / "pts.bin.head"
    head.write_text(f"float\n3\n{n}\nlittle\nX\nY\nZ\n")
    rng = np.random.default_rng(3)
    cols = [rng.normal(0.0, 1.0, n).astype("<f4") for _ in range(3)]
    (tmp_path / "pts.bin").write_bytes(b"".join(c.tobytes() for c in cols))

    resp = await client.post("/v1/vbt/open", json={"path": str(head)},
                             headers=auth_headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"] is True, body.get("error")
    assert body["num_rows"] == n
    # The route returns richer field descriptors than the parser's bare names.
    names = [(f["name"] if isinstance(f, dict) else f).lower() for f in body["fields"]]
    assert names == ["x", "y", "z"]

    headers = dict(auth_headers)
    if body.get("session_id"):
        headers["X-Visivo-Session"] = body["session_id"]
    # The request model takes max_rows and field_name — not a `fields` list, which
    # the first version of this test sent and which was silently ignored, leaving
    # an assertion on the HTTP status and nothing else.
    subset = await client.post(
        "/v1/vbt/subset",
        json={"dataset_id": body["dataset_id"], "max_rows": 10},
        headers=headers)
    assert subset.status_code == 200, subset.text
    sub = subset.json()
    assert sub["valid"] is True, sub.get("error")
    assert sub["kind"] == "point"
    assert sub["num_rows"] == 10, "max_rows must be honoured"
    # Columns come back base64-encoded float64-LE. Compare against the VALUES the
    # fixture wrote: size-and-finiteness alone would pass on a wrong endianness,
    # a wrong column, or a transposed layout.
    import base64
    for name, written in (("X", cols[0]), ("Y", cols[1]), ("Z", cols[2])):
        got = np.frombuffer(base64.b64decode(sub["columns"][name]), dtype="<f8")
        assert got.size == 10
        np.testing.assert_allclose(got, written[:10].astype(np.float64), rtol=1e-6)


# ── cube write paths ─────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_save_channel_2d_writes_a_readable_file(
        client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/save_channel_2d",
        json={"dataset_id": opened_cube["dataset_id"], "channel": 4},
        headers=opened_cube["headers"],
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"] is True, body.get("error")
    out = body.get("output_path") or body.get("path")
    assert out and os.path.isfile(out)
    from astropy.io import fits
    with fits.open(out) as hdul:
        assert hdul[0].data.ndim == 2, "a saved channel must be a 2-D image"


@pytest.mark.asyncio
async def test_save_channel_2d_rejects_a_channel_outside_the_cube(
        client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/save_channel_2d",
        json={"dataset_id": opened_cube["dataset_id"], "channel": 99999},
        headers=opened_cube["headers"],
    )
    assert resp.status_code in (200, 422)
    if resp.status_code == 200:
        assert resp.json()["valid"] is False


# ── tabular routes: catalogue and VBT share the query model ──────────────────

@pytest.fixture
async def opened_catalogue(client: AsyncClient, auth_headers: dict, tmp_path):
    """A small CSV catalogue, opened."""
    csv = tmp_path / "cat.csv"
    csv.write_text("ra,dec,flux\n10.0,2.0,1.5\n10.1,2.1,2.5\n10.2,2.2,0.5\n")
    resp = await client.post("/v1/catalogue/open", json={"path": str(csv)},
                             headers=auth_headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"] is True, body.get("error")
    headers = dict(auth_headers)
    if body.get("session_id"):
        headers["X-Visivo-Session"] = body["session_id"]
    return {"dataset_id": body["dataset_id"], "headers": headers}


@pytest.mark.asyncio
async def test_catalogue_subset_returns_rows(client: AsyncClient, opened_catalogue) -> None:
    resp = await client.post(
        "/v1/catalogue/subset",
        json={"dataset_id": opened_catalogue["dataset_id"], "max_rows": 2},
        headers=opened_catalogue["headers"])
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"] is True, body.get("error")
    # The response carries `rows`, not a count: asserting on a missing count key
    # with a default would have passed on an empty result.
    rows = body["rows"]
    assert len(rows) == 2, "max_rows must be honoured, and must return rows"
    assert {"ra", "dec", "flux"} <= set(rows[0])


@pytest.mark.asyncio
async def test_catalogue_query_filters_and_sorts(client: AsyncClient, opened_catalogue) -> None:
    """The filter/sort path, which nothing exercised — a query language that is
    never run is a query language nobody knows parses."""
    resp = await client.post(
        "/v1/catalogue/query",
        json={"dataset_id": opened_catalogue["dataset_id"], "limit": 10,
              "filters": [{"field": "flux", "op": ">", "value": "1.0"}],
              # `direction`, not `ascending` — an unknown key is ignored by the
              # model, so the first version of this test silently sorted ascending
              # and asserted nothing about order.
              "sort": {"field": "flux", "direction": "desc"}},
        headers=opened_catalogue["headers"])
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"] is True, body.get("error")
    assert body["returned_rows"] == 2, "two of the three rows have flux > 1"
    assert body["total_rows"] == 2, "the filter applies before the limit"
    fluxes = [float(r["flux"]) for r in body["rows"]]
    assert fluxes == [2.5, 1.5], "descending sort must be applied"


@pytest.mark.asyncio
async def test_catalogue_query_rejects_an_unknown_field(
        client: AsyncClient, opened_catalogue) -> None:
    resp = await client.post(
        "/v1/catalogue/query",
        json={"dataset_id": opened_catalogue["dataset_id"],
              "filters": [{"field": "no_such_column", "op": ">", "value": "1.0"}]},
        headers=opened_catalogue["headers"])
    assert resp.status_code in (200, 422)
    if resp.status_code == 200:
        assert resp.json()["valid"] is False


# ── cosmology batch ──────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_cosmology_distance_batch(client: AsyncClient, auth_headers: dict) -> None:
    resp = await client.post("/v1/cosmology/distance/batch",
                             json={"redshifts": [0.0, 0.1, 1.0], "model": "Planck18"},
                             headers=auth_headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"] is True, body.get("error")
    d0, d1, d2 = body["distances_Mpc"]
    # z = 0 is a real answer of zero distance; distance must grow with redshift.
    assert d0 == pytest.approx(0.0, abs=1e-6)
    assert d0 < d1 < d2


@pytest.mark.asyncio
async def test_cosmology_distance_batch_nulls_an_invalid_redshift(
        client: AsyncClient, auth_headers: dict) -> None:
    """A bad entry must not become 0.0 Mpc — indistinguishable from a genuine
    z = 0 at the observer. The single-redshift endpoint answers "Redshift z must
    be >= 0"; the batch tolerates the row but reports null for it, which is what
    the response model has always documented.
    """
    resp = await client.post(
        "/v1/cosmology/distance/batch",
        json={"redshifts": [0.0, -1.0, 0.5]},
        headers=auth_headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"] is True, "one bad row must not fail the whole request"
    d = body["distances_Mpc"]
    assert d[0] == pytest.approx(0.0, abs=1e-6), "z=0 is a real zero, not a null"
    assert d[1] is None, "a negative redshift must be null"
    assert d[2] > 0.0
    # Non-finite cannot travel over HTTP — JSON has no representation for it — so
    # the ROUTE is called directly. Asserting only that a Pydantic model can hold a
    # NaN, as the first version did, would have passed against the old
    # zero-filling implementation too.
    from app.routers.cosmology import CosmologyBatchRequest, cosmology_distance_batch
    direct = await cosmology_distance_batch(
        CosmologyBatchRequest(redshifts=[float("nan"), 0.5]))
    assert direct.valid is True
    assert direct.distances_Mpc[0] is None, "a non-finite redshift must be null"
    assert direct.distances_Mpc[1] > 0.0


# ── cube and product write paths ─────────────────────────────────────────────

@pytest.mark.asyncio
async def test_save_subregion_writes_the_requested_box(
        client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/save_subregion",
        json={"dataset_id": opened_cube["dataset_id"],
              "x0": 8, "x1": 23, "y0": 8, "y1": 23, "z0": 2, "z1": 9},
        headers=opened_cube["headers"])
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"] is True, body.get("error")
    out = body.get("output_path") or body.get("path")
    assert out and os.path.isfile(out)
    from astropy.io import fits
    with fits.open(out) as hdul:
        data = np.squeeze(hdul[0].data)
    assert data.shape == (8, 16, 16), "inclusive bounds, (z, y, x)"


@pytest.mark.asyncio
async def test_save_subregion_rejects_an_inverted_box(
        client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/cube/save_subregion",
        json={"dataset_id": opened_cube["dataset_id"],
              "x0": 20, "x1": 5, "y0": 0, "y1": 10, "z0": 0, "z1": 4},
        headers=opened_cube["headers"])
    assert resp.status_code in (200, 422)
    if resp.status_code == 200:
        assert resp.json()["valid"] is False


@pytest.mark.asyncio
async def test_moment_fits_writes_a_2d_map(client: AsyncClient, opened_cube: dict) -> None:
    resp = await client.post(
        "/v1/products/moment_fits",
        json={"dataset_id": opened_cube["dataset_id"], "moment_order": 0,
              "channel_start": 0, "channel_end": 15},
        headers=opened_cube["headers"])
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["valid"] is True, body.get("error")
    out = body.get("output_path") or body.get("path")
    assert out and os.path.isfile(out)
    from astropy.io import fits
    with fits.open(out) as hdul:
        assert np.squeeze(hdul[0].data).ndim == 2
