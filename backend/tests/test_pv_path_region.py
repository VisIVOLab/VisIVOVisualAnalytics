"""A PV path and the polygon of its width, written as regions (issue #520).

The desktop puts the path in the region list as a line of N vertices (and its
width as a polygon); the backend's region export is what writes them.
"""
from __future__ import annotations

from app import regions as R

PATH = {"shape": "line", "frame": "image", "unit": "pix",
        "points": [[2.0, 10.0], [12.5, 10.0], [12.5, 15.25]]}


def test_a_pv_path_is_exported_as_ds9_segment_and_crtf_lines():
    ds9 = R.export_ds9([PATH])
    assert "segment(2,10,12.5,10,12.5,15.25)" in ds9
    # ds9 gives the path back as one line of three vertices.
    (back,) = R.parse_regions(ds9, "ds9")
    assert back["shape"] == "line" and back["points"] == PATH["points"]
    # CRTF has no polyline (its pixels count from 0, the canonical frame from 1): one line per segment, in order.
    crtf = R.export_crtf([PATH]).splitlines()
    assert crtf[0] == "#CRTF" and len(crtf) == 3
    assert crtf[1].startswith("line[[1pix, 9pix], [11.5pix, 9pix]]")
    assert crtf[2].startswith("line[[11.5pix, 9pix], [11.5pix, 14.25pix]]")
    # A plain two-vertex line is unchanged.
    two = R.export_crtf([{**PATH, "points": PATH["points"][:2]}]).splitlines()
    assert len(two) == 2


def test_segments_keep_the_properties_of_the_path():
    path = {**PATH, "props": {"color": "red", "include": False}}
    lines = R.export_crtf([path]).splitlines()[1:]
    assert len(lines) == 2 and all(l.startswith("-line[") and "color=red" in l for l in lines)


def test_the_width_polygon_of_a_path_is_exported():
    band = {"shape": "polygon", "frame": "image", "unit": "pix",
            "points": [[2, 9], [12, 9], [12, 11], [2, 11]]}
    assert "polygon(2,9,12,9,12,11,2,11)" in R.export_ds9([band])
    assert R.export_crtf([band]).splitlines()[1].startswith("poly[[1pix, 8pix]")


async def test_the_export_route_takes_a_path(client, auth_headers):
    resp = await client.post("/v1/regions/export", json={"format": "crtf", "regions": [PATH]},
                             headers=auth_headers)
    body = resp.json()
    assert body["valid"], body
    assert body["text"].count("line[") == 2
