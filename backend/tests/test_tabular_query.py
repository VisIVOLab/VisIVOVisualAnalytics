"""
test_tabular_query.py – unit tests for _apply_tabular_query() in app/main.py.

Covers:
  1. No filters — all rows returned, order preserved
  2. Comparison operators (=, !=, >, >=, <, <=) on numeric and string columns
  3. `contains` operator — case-insensitive substring match
  4. `in` operator — list membership for numbers and strings
  5. Multiple simultaneous filters (implicit AND)
  6. Sort — ascending / descending on numeric and string columns
  7. Limit / offset — pagination, over-offset, over-limit
  8. Coercion / robustness — string column numeric filter, invalid operator, bad value
  9. Filter producing zero rows — empty result with correct metadata
 10. Empty input DataFrame — no crash, coherent output

Run with:
    cd backend
    pytest tests/test_tabular_query.py -v
"""

from __future__ import annotations

import pytest
import pandas as pd


# ── Lazy import helpers ───────────────────────────────────────────────────────
# Imported inside helpers so conftest.py has already set VISIVO_TOKEN.


def _fn():
    from app.dependencies import _apply_tabular_query
    return _apply_tabular_query


def _models():
    from app.schemas import TabularFilter, TabularSortSpec
    return TabularFilter, TabularSortSpec


def apply(df, filters=None, sort=None, limit=0, offset=0):
    """Thin wrapper that calls _apply_tabular_query with sensible defaults."""
    return _fn()(df, filters or [], sort, limit, offset)


def flt(field: str, op: str, value: str):
    """Construct a TabularFilter."""
    TabularFilter, _ = _models()
    return TabularFilter(field=field, op=op, value=value)


def srt(field: str, direction: str = "asc"):
    """Construct a TabularSortSpec."""
    _, TabularSortSpec = _models()
    return TabularSortSpec(field=field, direction=direction)


# ── Shared fixture ────────────────────────────────────────────────────────────


@pytest.fixture()
def df() -> pd.DataFrame:
    return pd.DataFrame(
        {
            "id":    [1,       2,      3,       4,       5],
            "name":  ["alpha", "beta", "gamma", "delta", "epsilon"],
            "value": [10.0,    20.0,   30.0,    40.0,    50.0],
            "score": [1.5,     2.5,    3.5,     4.5,     5.5],
        }
    )


# ═══════════════════════════════════════════════════════════════════════════════
# 1. No filters
# ═══════════════════════════════════════════════════════════════════════════════


def test_no_filters_returns_all_rows(df):
    result, total = apply(df.copy())
    assert total == 5
    assert len(result) == 5
    assert list(result["id"]) == [1, 2, 3, 4, 5]


def test_no_filters_preserves_column_set(df):
    result, _ = apply(df.copy())
    assert set(result.columns) == set(df.columns)


# ═══════════════════════════════════════════════════════════════════════════════
# 2. Comparison operators — numeric column
# ═══════════════════════════════════════════════════════════════════════════════


def test_eq_numeric(df):
    result, total = apply(df.copy(), [flt("value", "=", "30.0")])
    assert total == 1
    assert list(result["id"]) == [3]


def test_neq_numeric(df):
    result, total = apply(df.copy(), [flt("value", "!=", "30.0")])
    assert total == 4
    assert 3 not in list(result["id"])


def test_gt_numeric(df):
    result, total = apply(df.copy(), [flt("value", ">", "30.0")])
    assert total == 2
    assert list(result["id"]) == [4, 5]


def test_gte_numeric(df):
    result, total = apply(df.copy(), [flt("value", ">=", "30.0")])
    assert total == 3
    assert list(result["id"]) == [3, 4, 5]


def test_lt_numeric(df):
    result, total = apply(df.copy(), [flt("value", "<", "30.0")])
    assert total == 2
    assert list(result["id"]) == [1, 2]


def test_lte_numeric(df):
    result, total = apply(df.copy(), [flt("value", "<=", "30.0")])
    assert total == 3
    assert list(result["id"]) == [1, 2, 3]


# ── 2b. Comparison operators — string column ─────────────────────────────────


def test_eq_string(df):
    result, total = apply(df.copy(), [flt("name", "=", "beta")])
    assert total == 1
    assert list(result["id"]) == [2]


def test_neq_string(df):
    result, total = apply(df.copy(), [flt("name", "!=", "beta")])
    assert total == 4
    assert 2 not in list(result["id"])


def test_gt_string_lexicographic(df):
    # "gamma" > "beta" lexicographically — should match gamma, delta, epsilon
    result, total = apply(df.copy(), [flt("name", ">", "beta")])
    assert "beta" not in list(result["name"])
    assert "alpha" not in list(result["name"])


# ═══════════════════════════════════════════════════════════════════════════════
# 3. `contains` operator
# ═══════════════════════════════════════════════════════════════════════════════


def test_contains_found(df):
    result, total = apply(df.copy(), [flt("name", "contains", "lph")])
    assert total == 1
    assert list(result["id"]) == [1]


def test_contains_case_insensitive(df):
    # Implementation uses case=False
    result, total = apply(df.copy(), [flt("name", "contains", "GAMMA")])
    assert total == 1
    assert list(result["id"]) == [3]


def test_contains_not_found(df):
    result, total = apply(df.copy(), [flt("name", "contains", "xyz")])
    assert total == 0
    assert len(result) == 0


def test_contains_multiple_matches(df):
    # 'a' appears in: alpha, beta (pos 3), gamma, delta — not epsilon
    result, total = apply(df.copy(), [flt("name", "contains", "a")])
    names = list(result["name"])
    assert "alpha"   in names
    assert "beta"    in names    # b-e-t-a — contains 'a'
    assert "gamma"   in names    # g-a-m-m-a — contains 'a'
    assert "delta"   in names    # d-e-l-t-a — contains 'a'
    assert "epsilon" not in names
    assert total == 4
    assert total == len(result)


# ═══════════════════════════════════════════════════════════════════════════════
# 4. `in` operator
# ═══════════════════════════════════════════════════════════════════════════════


def test_in_numeric(df):
    result, total = apply(df.copy(), [flt("value", "in", "10.0, 30.0, 50.0")])
    assert total == 3
    assert list(result["id"]) == [1, 3, 5]


def test_in_string(df):
    result, total = apply(df.copy(), [flt("name", "in", "alpha, gamma")])
    assert total == 2
    assert set(result["id"]) == {1, 3}


def test_in_no_match(df):
    result, total = apply(df.copy(), [flt("value", "in", "99.0, 100.0")])
    assert total == 0


def test_in_partial_match(df):
    result, total = apply(df.copy(), [flt("value", "in", "10.0, 999.0")])
    assert total == 1
    assert list(result["id"]) == [1]


# ═══════════════════════════════════════════════════════════════════════════════
# 5. Multiple filters (implicit AND)
# ═══════════════════════════════════════════════════════════════════════════════


def test_two_numeric_filters(df):
    filters = [flt("value", ">", "15.0"), flt("value", "<", "45.0")]
    result, total = apply(df.copy(), filters)
    assert total == 3
    assert list(result["id"]) == [2, 3, 4]


def test_numeric_and_string_filter(df):
    filters = [flt("value", ">", "20.0"), flt("name", "contains", "lt")]
    # value > 20 → rows 3,4,5 ; "lt" in name → delta (4)
    result, total = apply(df.copy(), filters)
    assert total == 1
    assert list(result["id"]) == [4]


def test_three_filters(df):
    filters = [
        flt("value", ">", "10.0"),
        flt("value", "<=", "40.0"),
        flt("name", "!=", "gamma"),
    ]
    result, total = apply(df.copy(), filters)
    assert 1 not in list(result["id"])   # value == 10.0 excluded
    assert 3 not in list(result["id"])   # gamma excluded
    assert 5 not in list(result["id"])   # value == 50.0 excluded
    assert set(result["id"]) == {2, 4}


# ═══════════════════════════════════════════════════════════════════════════════
# 6. Sort
# ═══════════════════════════════════════════════════════════════════════════════


def test_sort_numeric_ascending(df):
    result, _ = apply(df.copy(), sort=srt("value", "asc"))
    assert list(result["value"]) == [10.0, 20.0, 30.0, 40.0, 50.0]


def test_sort_numeric_descending(df):
    result, _ = apply(df.copy(), sort=srt("value", "desc"))
    assert list(result["value"]) == [50.0, 40.0, 30.0, 20.0, 10.0]


def test_sort_string_ascending(df):
    result, _ = apply(df.copy(), sort=srt("name", "asc"))
    names = list(result["name"])
    assert names == sorted(names)


def test_sort_string_descending(df):
    result, _ = apply(df.copy(), sort=srt("name", "desc"))
    names = list(result["name"])
    assert names == sorted(names, reverse=True)


def test_sort_nonexistent_field_is_ignored(df):
    """Sort on an unknown column must not raise — silently ignored."""
    result, total = apply(df.copy(), sort=srt("nonexistent", "asc"))
    assert total == 5


def test_sort_with_filter(df):
    filters = [flt("value", ">", "15.0")]
    result, _ = apply(df.copy(), filters, sort=srt("value", "desc"))
    assert list(result["value"]) == [50.0, 40.0, 30.0, 20.0]


# ═══════════════════════════════════════════════════════════════════════════════
# 7. Limit / offset
# ═══════════════════════════════════════════════════════════════════════════════


def test_limit_two(df):
    result, total = apply(df.copy(), limit=2)
    assert total == 5          # total before pagination
    assert len(result) == 2
    assert list(result["id"]) == [1, 2]


def test_offset_two(df):
    result, total = apply(df.copy(), offset=2)
    assert total == 5
    assert len(result) == 3
    assert list(result["id"]) == [3, 4, 5]


def test_limit_and_offset(df):
    result, total = apply(df.copy(), limit=2, offset=1)
    assert total == 5
    assert list(result["id"]) == [2, 3]


def test_offset_beyond_total(df):
    result, total = apply(df.copy(), offset=100)
    assert total == 5
    assert len(result) == 0


def test_limit_beyond_remaining(df):
    result, total = apply(df.copy(), limit=100, offset=3)
    assert total == 5
    assert list(result["id"]) == [4, 5]


def test_zero_limit_means_no_limit(df):
    """limit=0 → no slicing applied → all rows returned."""
    result, total = apply(df.copy(), limit=0)
    assert total == 5
    assert len(result) == 5


def test_limit_one_offset_zero(df):
    result, total = apply(df.copy(), limit=1, offset=0)
    assert len(result) == 1
    assert list(result["id"]) == [1]


def test_pagination_total_reflects_filtered_rows(df):
    """total must be rows matching filters, before pagination."""
    filters = [flt("value", ">", "15.0")]   # matches 4 rows
    result, total = apply(df.copy(), filters, limit=2, offset=0)
    assert total == 4
    assert len(result) == 2


# ═══════════════════════════════════════════════════════════════════════════════
# 8. Coercion / robustness
# ═══════════════════════════════════════════════════════════════════════════════


def test_numeric_filter_on_string_column_uses_string_comparison():
    """String column: comparison operators compare as strings, not floats."""
    df = pd.DataFrame({"val": ["10", "20", "5"]})
    # "5" > "20" lexicographically (string "5" > "2")
    result, total = apply(df.copy(), [flt("val", ">", "20")])
    assert "5" in list(result["val"])


def test_unknown_field_raises_value_error(df):
    with pytest.raises(ValueError, match="Unknown field"):
        apply(df.copy(), [flt("nonexistent", "=", "1")])


def test_unknown_operator_raises_value_error(df):
    with pytest.raises(ValueError, match="Unknown filter operator"):
        apply(df.copy(), [flt("value", "LIKE", "10")])


def test_non_numeric_value_on_numeric_column_raises(df):
    with pytest.raises(ValueError, match="Cannot convert"):
        apply(df.copy(), [flt("value", "=", "not_a_number")])


def test_in_operator_with_non_numeric_list_on_numeric_column(df):
    """'in' with unparseable values: falls back to string isin."""
    result, total = apply(df.copy(), [flt("value", "in", "ten, twenty")])
    # None of "ten"/"twenty" match the string representation of 10.0/20.0
    assert total == 0


# ═══════════════════════════════════════════════════════════════════════════════
# 9. Filter producing zero rows
# ═══════════════════════════════════════════════════════════════════════════════


def test_filter_zero_results(df):
    result, total = apply(df.copy(), [flt("value", "=", "999.0")])
    assert total == 0
    assert len(result) == 0


def test_zero_result_preserves_schema(df):
    """An empty result must still carry the original column names."""
    result, _ = apply(df.copy(), [flt("value", "=", "999.0")])
    assert set(result.columns) == set(df.columns)


def test_filter_then_offset_on_zero_results(df):
    result, total = apply(df.copy(), [flt("value", "=", "999.0")], offset=5)
    assert total == 0
    assert len(result) == 0


# ═══════════════════════════════════════════════════════════════════════════════
# 10. Empty input DataFrame
# ═══════════════════════════════════════════════════════════════════════════════


def test_empty_dataframe_no_filters():
    empty = pd.DataFrame({"id": pd.Series([], dtype=int), "value": pd.Series([], dtype=float)})
    result, total = apply(empty.copy())
    assert total == 0
    assert len(result) == 0


def test_empty_dataframe_with_numeric_filter():
    empty = pd.DataFrame({"id": pd.Series([], dtype=int), "value": pd.Series([], dtype=float)})
    result, total = apply(empty.copy(), [flt("value", ">", "0")])
    assert total == 0
    assert len(result) == 0


def test_empty_dataframe_with_limit_offset():
    empty = pd.DataFrame({"id": pd.Series([], dtype=int), "value": pd.Series([], dtype=float)})
    result, total = apply(empty.copy(), limit=10, offset=5)
    assert total == 0
    assert len(result) == 0


def test_empty_dataframe_unknown_field_raises():
    """Even on an empty DataFrame, an unknown field must still raise."""
    empty = pd.DataFrame({"id": pd.Series([], dtype=int)})
    with pytest.raises(ValueError, match="Unknown field"):
        apply(empty.copy(), [flt("nonexistent", "=", "0")])


def test_an_even_sample_covers_the_table_and_its_phases_fill_it_in():
    """A simulation's table is stored by region: its first rows are a few
    blocks of space. An even sample of a page covers it all; the next phases
    add the rows between, until every row is there once."""
    import numpy as np
    import pandas as pd

    from app.dependencies import _apply_tabular_query, sample_step
    from app.schemas import TabularFilter

    frame = pd.DataFrame({"X": np.arange(1000.0), "M": np.arange(1000.0) % 7})
    page, total = _apply_tabular_query(frame, [], None, limit=100, offset=0, even_sample=True)
    assert total == 1000 and sample_step(total, 100) == 10
    assert page["X"].tolist() == list(range(0, 1000, 10))
    seen = set()
    for phase in range(10):
        part, _ = _apply_tabular_query(frame, [], None, limit=100, offset=0,
                                       even_sample=True, phase=phase)
        seen.update(part["X"].astype(int))
    assert seen == set(range(1000))
    # Over the rows a filter keeps (the fast numeric path and the general one).
    kept = [TabularFilter(field="M", op="=", value="3")]
    part, total = _apply_tabular_query(frame, kept, None, limit=20, offset=0, even_sample=True)
    # One in ceil(143 / 20) = 8: a sample never larger than the page.
    assert total == 143 and len(part) == 18 and (part["M"] == 3).all()
    assert part["X"].iloc[-1] > 900          # spread over the table, not its start
    text = [TabularFilter(field="M", op="in", value="3")]
    part, _ = _apply_tabular_query(frame, text, None, limit=20, offset=0, even_sample=True)
    assert len(part) == 18 and part["X"].iloc[-1] > 900
    # Fewer rows than the page: all of them, as before.
    small, total = _apply_tabular_query(frame.iloc[:50], [], None, limit=100, offset=0,
                                        even_sample=True)
    assert total == 50 and len(small) == 50
