"""Delimited text catalogues: comma, tab or semicolon, read as classified (issue #155).

The classifier called a tab- or semicolon-separated file a catalogue, and every
reader then split it on commas only, so it opened as one column. One reader
now detects the separator the classifier's way, on the same bytes.
"""
from __future__ import annotations

import gzip

import numpy as np
import pandas as pd
import pytest

from app.classify import classify_path
from app.dependencies import (
    _catalogue_csv_metadata,
    _catalogue_load_dataframe,
    _catalogue_overlay_sync,
    _load_overlay_csv,
)
from app.text_table import detect_separator, read_delimited_table, sniff_separator

ROWS = [("M31", 10.6847, 41.2690, 3.44), ("M33", 23.4621, 30.6599, 5.72),
        ("NGC 253", 11.8880, -25.2883, 7.1)]
COLUMNS = ["name", "ra", "dec", "mag"]


def _table(separator: str) -> str:
    lines = [separator.join(COLUMNS)]
    lines += [separator.join(str(v) for v in row) for row in ROWS]
    return "\n".join(lines) + "\n"


def _write(tmp_path, name: str, text: str):
    path = tmp_path / name
    path.write_text(text, encoding="utf-8")
    return path


@pytest.fixture(autouse=True)
def _fresh_overlay_cache():
    _load_overlay_csv.cache_clear()
    yield
    _load_overlay_csv.cache_clear()


@pytest.mark.parametrize("separator,name", [(",", "cat.csv"), ("\t", "cat.tsv"),
                                            (";", "cat.txt"), ("\t", "cat.csv")])
def test_each_separator_reads_as_the_comma_version(tmp_path, separator, name):
    reference = pd.read_csv(_write(tmp_path, "reference.csv", _table(",")))
    path = _write(tmp_path, name, _table(separator))
    assert detect_separator(path) == separator
    frame = read_delimited_table(path)
    pd.testing.assert_frame_equal(frame, reference)
    # The classifier agrees: same bytes, same verdict.
    assert classify_path(path)["opener"] == "catalogue"


def test_quoted_fields_hold_the_other_separators(tmp_path):
    text = ('name\tcontour\tra\n'
            '"a, b; c"\t"10.4_10.6,0.0_0.2"\t1.5\n'
            '"tab\there"\tplain\t2.5\n')
    path = _write(tmp_path, "quoted.tsv", text)
    assert detect_separator(path) == "\t"
    frame = read_delimited_table(path)
    assert list(frame.columns) == ["name", "contour", "ra"]
    assert frame["name"].tolist() == ["a, b; c", "tab\there"]
    assert frame["contour"].tolist() == ["10.4_10.6,0.0_0.2", "plain"]
    np.testing.assert_allclose(frame["ra"], [1.5, 2.5])


def test_quoted_commas_in_a_semicolon_file(tmp_path):
    path = _write(tmp_path, "q.csv", 'name;ra;dec\n"Smith, J.";1.0;2.0\n"Doe, A.";3.0;4.0\n')
    frame = read_delimited_table(path)
    assert frame["name"].tolist() == ["Smith, J.", "Doe, A."]
    np.testing.assert_allclose(frame["dec"], [2.0, 4.0])


def test_comment_and_blank_lines_are_skipped(tmp_path):
    text = ("# exported by some tool\n#   columns: name ra dec mag\n\n"
            + _table("\t").replace("M33", "M33", 1))
    lines = text.splitlines()
    # A comment and a blank line among the rows, too.
    lines.insert(5, "# a note between rows, with\ttabs\tand,commas")
    lines.insert(6, "")
    path = _write(tmp_path, "commented.tsv", "\n".join(lines) + "\n")
    frame = read_delimited_table(path)
    reference = pd.read_csv(_write(tmp_path, "reference.csv", _table(",")))
    pd.testing.assert_frame_equal(frame, reference)


def test_a_hash_inside_a_field_is_data_not_a_comment(tmp_path):
    path = _write(tmp_path, "hash.csv", 'name,ra\n"#1 brightest",1.0\nsrc#2,2.0\n')
    frame = read_delimited_table(path)
    assert frame["name"].tolist() == ["#1 brightest", "src#2"]


def test_a_text_file_that_is_not_a_table_is_refused_clearly(tmp_path):
    path = _write(tmp_path, "notes.txt",
                  "These are my notes about the observation.\n"
                  "The weather was good, the seeing poor; nothing else.\n"
                  "Another line with no structure at all\n")
    with pytest.raises(ValueError, match="not a delimited text table"):
        read_delimited_table(path)
    assert classify_path(path)["opener"] != "catalogue"


def test_empty_and_comment_only_files_say_so(tmp_path):
    with pytest.raises(ValueError, match="empty"):
        read_delimited_table(_write(tmp_path, "empty.csv", "\n\n"))
    with pytest.raises(ValueError, match="comment"):
        read_delimited_table(_write(tmp_path, "comments.csv", "# a\n# b\n"))


@pytest.mark.parametrize("separator", [",", "\t", ";"])
def test_a_header_only_file_is_an_empty_table(tmp_path, separator):
    path = _write(tmp_path, "header.csv", separator.join(COLUMNS) + "\n")
    frame = read_delimited_table(path)
    assert list(frame.columns) == COLUMNS
    assert len(frame) == 0


def test_semicolon_with_decimal_commas(tmp_path):
    path = _write(tmp_path, "eu.csv", "ra;dec\n10,5;-20,25\n11,0;1,5e1\n")
    frame = read_delimited_table(path)
    np.testing.assert_allclose(frame["ra"], [10.5, 11.0])
    np.testing.assert_allclose(frame["dec"], [-20.25, 15.0])


def test_semicolon_with_decimal_points_is_left_alone(tmp_path):
    path = _write(tmp_path, "pt.csv", "ra;label\n10.5;1,2\n11.0;3,4\n")
    frame = read_delimited_table(path)
    np.testing.assert_allclose(frame["ra"], [10.5, 11.0])
    assert frame["label"].tolist() == ["1,2", "3,4"]


def test_gzipped_table_is_sniffed_through_the_compression(tmp_path):
    path = tmp_path / "cat.tsv.gz"
    with gzip.open(path, "wt", encoding="utf-8") as handle:
        handle.write(_table("\t"))
    assert detect_separator(path) == "\t"
    assert read_delimited_table(path)["ra"].tolist() == [r[1] for r in ROWS]


def test_classifier_and_reader_judge_the_same_truncated_head():
    # A cut half row at the end of an 8 KB sniff is not judged.
    text = "a,b,c\n1,2,3\n4,5,6\n7,8"
    assert sniff_separator(text, truncated=True) == ","
    assert sniff_separator(text, truncated=False) is None


def test_wide_header_beyond_the_sniff_is_read(tmp_path):
    names = [f"col_{i:05d}" for i in range(1500)]   # a header of ~15 KB
    text = "\t".join(names) + "\n" + "\t".join("1" for _ in names) + "\n"
    path = _write(tmp_path, "wide.tsv", text * 1)
    frame = read_delimited_table(path)
    assert frame.shape == (1, 1500)


def test_catalogue_readers_use_the_detected_separator(tmp_path):
    path = _write(tmp_path, "cat.tsv", _table("\t"))
    meta = _catalogue_csv_metadata(path)
    assert [f["name"] for f in meta["fields"]] == COLUMNS
    assert meta["coordinate_fields"]["ra"] == "ra"
    frame = _catalogue_load_dataframe(path, "csv")
    assert list(frame.columns) == COLUMNS


def test_overlay_reads_a_semicolon_catalogue(tmp_path):
    path = _write(tmp_path, "cat.txt", _table(";"))
    result = _catalogue_overlay_sync(str(path), 10.68, 41.27, 2.0, "", "", 100)
    assert result["valid"] is True, result["error"]
    assert result["total_in_view"] == 1


def test_overlay_reports_a_non_table_clearly(tmp_path):
    path = _write(tmp_path, "notes.txt", "just one line of prose\n")
    result = _catalogue_overlay_sync(str(path), 0.0, 0.0, 2.0, "", "", 100)
    assert result["valid"] is False
    assert "not a delimited text table" in result["error"]


@pytest.mark.asyncio
async def test_tsv_opens_through_the_catalogue_route(client, auth_headers, tmp_path):
    path = _write(tmp_path, "cat.tsv", _table("\t"))
    response = await client.post("/v1/catalogue/open",
                                 json={"path": str(path), "format": "csv"},
                                 headers=auth_headers)
    body = response.json()
    assert body["valid"] is True, body["error"]
    assert body["num_rows"] == 3
    assert [f["name"] for f in body["fields"]] == COLUMNS
    assert body["coordinate_fields"]["ra"] == "ra"
    assert body["coordinate_fields"]["dec"] == "dec"


@pytest.mark.asyncio
async def test_tsv_opens_through_the_overlay_route(client, auth_headers, tmp_path):
    path = _write(tmp_path, "cat.tsv", _table("\t"))
    response = await client.post(
        "/v1/hips/catalogue_overlay",
        json={"catalogue_path": str(path), "ra_center": 11.0, "dec_center": 35.0,
              "fov_deg": 20.0},
        headers=auth_headers)
    body = response.json()
    assert body["valid"] is True, body["error"]
    assert body["total_in_view"] == 2
    assert sorted(round(s["ra"]) for s in body["sources"]) == [11, 23]


# ── Header and comment rules; comma files read as before ─────────────────────


@pytest.mark.parametrize("header", ["#id,ra,dec", "# id,ra,dec", "#  id , ra , dec"])
def test_a_hash_header_above_the_data_is_the_header(tmp_path, header):
    path = _write(tmp_path, "h.csv",
                  f"# written by some tool\n{header}\n\n1,10,20\n2,11,21\n3,12,22\n")
    frame = read_delimited_table(path)
    assert list(frame.columns) == ["id", "ra", "dec"]
    assert frame["id"].tolist() == [1, 2, 3]


def test_a_hash_header_in_a_tab_file(tmp_path):
    path = _write(tmp_path, "h.tsv", "#id\tra\tdec\n1\t10\t20\n2\t11\t21\n")
    assert list(read_delimited_table(path).columns) == ["id", "ra", "dec"]


def test_a_descriptive_comment_above_a_real_header_is_not_the_header(tmp_path):
    path = _write(tmp_path, "d.csv", "# Sources, version 2, J2000\nid,ra,dec\n1,10,20\n")
    frame = read_delimited_table(path)
    assert list(frame.columns) == ["id", "ra", "dec"]
    assert len(frame) == 1


def test_a_commented_out_row_above_the_header_is_not_the_header(tmp_path):
    path = _write(tmp_path, "c.csv", "# 9,99,999\nid,ra,dec\n1,10,20\n")
    assert list(read_delimited_table(path).columns) == ["id", "ra", "dec"]


def test_rows_whose_first_field_starts_with_a_hash_are_data(tmp_path):
    path = _write(tmp_path, "ids.csv", "id,ra\n#1,10\n#2,11\n")
    assert read_delimited_table(path)["id"].tolist() == ["#1", "#2"]
    rows = ["name,ra,dec"] + [f"S{i},{i},{i}" for i in range(500)]
    rows[6] = "#S5,5,5"
    big = _write(tmp_path, "big.csv", "\n".join(rows) + "\n")
    frame = read_delimited_table(big)
    assert len(frame) == 500
    assert frame["name"].iloc[5] == "#S5"


def test_a_comment_after_a_multiline_quoted_field(tmp_path):
    path = _write(tmp_path, "m.csv", 'a,b\n1,"x\ny"\n# note\n3,4\n')
    frame = read_delimited_table(path)
    assert frame["a"].tolist() == [1, 3]
    assert frame["b"].tolist() == ["x\ny", "4"]


def test_a_hash_line_inside_a_quoted_field_is_kept(tmp_path):
    path = _write(tmp_path, "q.csv", 'a,b\n1,"x\n# y"\n# note\n2,z\n')
    assert read_delimited_table(path)["b"].tolist() == ["x\n# y", "z"]


@pytest.mark.parametrize("text", [
    "a,b,c\n1,2\n3,4,5\n",          # a short row
    "a,b\n1,2,3\n4,5\n",            # a long row: the extra field is dropped
    "a,b\n1,2,\n3,4,\n",            # a trailing comma on the rows only
    "ra\n1\n2\n",                   # one column
])
def test_comma_files_no_separator_makes_consistent_read_as_before(tmp_path, text):
    # As pd.read_csv reads them, except that the first column never becomes
    # the index (which shifted every value one column on longer rows).
    path = _write(tmp_path, "r.csv", text)
    pd.testing.assert_frame_equal(read_delimited_table(path),
                                  pd.read_csv(path, index_col=False))


def test_a_trailing_comma_on_the_rows_does_not_shift_the_columns(tmp_path):
    path = _write(tmp_path, "t.csv", "ra,dec\n1,2,\n3,4,\n")
    frame = read_delimited_table(path)
    assert list(frame.columns) == ["ra", "dec"]
    assert frame["ra"].tolist() == [1, 3] and frame["dec"].tolist() == [2, 4]


def test_a_decimal_point_after_the_sampled_rows_keeps_the_column_numeric(tmp_path):
    # The decision is made on the first ten rows; a later "20.5" made pandas
    # leave RA as text with "10,5" in it.
    rows = [f"{chr(97 + i)};{10 + i},5;{20 + i},5" for i in range(10)]
    text = "ID;RA;DEC\n" + "\n".join(rows) + "\nk;20.5;30\nl;;31,25\n"
    frame = read_delimited_table(_write(tmp_path, "late.csv", text))
    assert pd.api.types.is_float_dtype(frame["RA"])
    assert pd.api.types.is_float_dtype(frame["DEC"])
    assert frame["RA"].iloc[0] == 10.5 and frame["RA"].iloc[10] == 20.5
    assert np.isnan(frame["RA"].iloc[11])
    assert frame["DEC"].tolist()[-2:] == [30.0, 31.25]
    assert frame["ID"].tolist()[:2] == ["a", "b"]     # text stays text


def test_a_comma_list_column_in_a_semicolon_table_stays_text(tmp_path):
    path = _write(tmp_path, "l.csv", "id;ranges;flux\n1;1,2;10\n2;3,4;11\n")
    frame = read_delimited_table(path)
    assert frame["ranges"].tolist() == ["1,2", "3,4"]
    assert frame["flux"].tolist() == [10, 11]


@pytest.mark.parametrize("raw", [b"# c\ra,b\r1,2\r# note\r3,4\r",
                                 b"a,b\r1,2\r# note\r3,4\r",
                                 b"# c\r\na,b\r\n1,2\r\n# note\r\n3,4\r\n"])
def test_comments_in_cr_and_crlf_files(tmp_path, raw):
    path = tmp_path / "cr.csv"
    path.write_bytes(raw)
    frame = read_delimited_table(path)
    assert list(frame.columns) == ["a", "b"]
    assert frame.values.tolist() == [[1, 2], [3, 4]]


def test_a_comment_after_a_byte_order_mark(tmp_path):
    path = tmp_path / "bom.csv"
    path.write_bytes("﻿# c\na,b\n1,2\n3,4\n".encode("utf-8"))
    assert list(read_delimited_table(path).columns) == ["a", "b"]
    assert classify_path(path)["opener"] == "catalogue"


def test_a_pandas_index_column_is_a_table_for_both(tmp_path):
    # to_csv with its index writes a header starting with a tab (an empty name).
    path = tmp_path / "idx.tsv"
    pd.DataFrame({"ra": [1.0, 2.0], "dec": [3.0, 4.0]}).to_csv(path, sep="\t")
    assert classify_path(path)["opener"] == "catalogue"
    assert list(read_delimited_table(path).columns)[1:] == ["ra", "dec"]


def test_classify_a_file_whose_sniff_cuts_a_long_row(tmp_path):
    # Rows of ~1 KB: the 8 KB sniff holds eight whole lines and a piece of the
    # ninth, which parses into fewer fields. Judging that piece would call this
    # ordinary catalogue unknown.
    field = "x" * 200
    lines = [",".join(f"c{i}" + field for i in range(5))]
    lines += [",".join(field for _ in range(5)) for _ in range(12)]
    path = _write(tmp_path, "long.csv", "\n".join(lines) + "\n")
    head = path.read_bytes()[:8192].decode().splitlines()
    assert len(head) <= 10 and len(head[-1].split(",")) < 5
    assert classify_path(path)["opener"] == "catalogue"
    assert read_delimited_table(path).shape == (12, 5)
