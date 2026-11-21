"""Exact selected-population diagnostics (issue #426)."""

from __future__ import annotations

import numpy as np
import pytest


def _calculate(values, weights=None, bins=4, field_range=None):
    from app.routers.catalogue import _diagnostic_accumulate

    values = np.asarray(values, dtype=np.float64)
    weights = None if weights is None else np.asarray(weights, dtype=np.float64)

    def chunks():
        # Deliberately split the input: results must not depend on chunking.
        yield values[:3], None if weights is None else weights[:3]
        yield values[3:], None if weights is None else weights[3:]

    return _diagnostic_accumulate(chunks, field_range, bins, weights is not None)


def test_exact_histogram_and_moments_use_every_finite_selected_value():
    result = _calculate([0, 1, 2, 3, 4, np.nan], bins=4, field_range=(0, 4))
    assert result["selected_rows"] == 6
    assert result["finite_rows"] == 5
    assert result["counts"] == [1, 1, 1, 2]
    assert result["mean"] == pytest.approx(2.0)
    assert result["stddev"] == pytest.approx(np.sqrt(2.0))


def test_weighted_histogram_and_mean_state_the_actual_weight_sum():
    result = _calculate([0, 1, 2, 3], [1, 1, 2, 6], bins=2, field_range=(0, 4))
    assert result["counts"] == [2, 8]
    assert result["weight_sum"] == pytest.approx(10.0)
    assert result["weighted_mean"] == pytest.approx(2.3)


def test_nonfinite_values_and_invalid_weights_are_excluded_consistently():
    result = _calculate([1, np.nan, 3, 4], [2, 9, -1, np.inf], bins=2)
    assert result["selected_rows"] == 4
    assert result["finite_rows"] == 1
    assert result["minimum"] == result["maximum"] == 1
    assert sum(result["counts"]) == pytest.approx(2.0)


def test_empty_finite_population_has_an_actionable_error():
    with pytest.raises(ValueError, match="no finite values"):
        _calculate([np.nan, np.inf], bins=8)
