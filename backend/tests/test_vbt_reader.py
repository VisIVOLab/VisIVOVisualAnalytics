"""VisIVO Binary Table reading: byte order, and the size of a point subset.

Both cases here came from one real file — mergerSTAR.bin, a 21-million-row,
11-field star table from a merger simulation — that would not open at all. The
client reported "connection refused", which pointed at the network and not at
either of the two defects that actually caused it.
"""
from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import pytest


def _write_vbt(tmp_path: Path, values: np.ndarray, endian: str,
               field_names: list[str]) -> Path:
    """Write a point VBT (.bin + .bin.head). `values` is (fields, rows)."""
    fields, rows = values.shape
    code = ("<" if endian == "little" else ">") + "f4"
    bin_path = tmp_path / "t.bin"
    values.astype(code).tofile(bin_path)
    head = tmp_path / "t.bin.head"
    head.write_text("\n".join(["float", str(fields), str(rows), endian, *field_names]) + "\n")
    return head


class TestByteOrder:
    """The header's byte-order line has to be obeyed.

    It was parsed into `file_endian` and then ignored: the numpy code was fixed
    to "<" by the scalar type, so every VBT was read little-endian. A
    big-endian file came back as NaNs and values at the float32 limit, with
    nothing anywhere to say why.
    """

    @staticmethod
    def _values() -> np.ndarray:
        # Deliberately values whose byte-swapped reading is obviously wrong:
        # swapping these gives ~1e-41 and ~1e38, not small tidy numbers.
        return np.array([[1.5, 2.5, 3.5], [-0.25, 0.75, 100.0]], dtype=np.float32)

    @pytest.mark.parametrize("endian", ["little", "big"])
    def test_values_survive_a_round_trip_in_either_order(self, tmp_path, endian):
        from app.dependencies import _parse_vbt_header

        values = self._values()
        head = _write_vbt(tmp_path, values, endian, ["A", "B"])
        header = _parse_vbt_header(head)

        assert header["file_endian"] == endian
        assert header["np_endian_code"] == ("<" if endian == "little" else ">") + "f4"

        arr = np.memmap(str(head)[: -len(".head")], dtype=header["np_endian_code"],
                        mode="r", shape=(2, 3))
        np.testing.assert_allclose(np.asarray(arr), values, rtol=1e-6)

    def test_reading_a_big_endian_file_as_little_is_visibly_wrong(self, tmp_path):
        """Pins WHY this matters: the failure mode is garbage, not an error."""
        values = self._values()
        head = _write_vbt(tmp_path, values, "big", ["A", "B"])
        wrong = np.memmap(str(head)[: -len(".head")], dtype="<f4", mode="r", shape=(2, 3))
        # Nothing raises; the numbers are simply absurd. Byte-swapping a small
        # tidy float gives either a denormal or something near the type's
        # limit — never a plausible measurement — which is why this failure
        # mode reaches the user as "the data looks wrong" and not as an error.
        swapped = np.abs(np.asarray(wrong, dtype=np.float64))
        assert not np.allclose(swapped, values)
        assert np.all((swapped < 1e-30) | (swapped > 1e30))


class TestPointSubsetSize:
    """A point subset must refuse to be enormous rather than kill the service.

    Every field is returned as a base64 float64 column, so the reply costs
    8 bytes per value plus a third again — and the arrays, the encoded strings
    and the serialised JSON are alive at once. max_rows = 0 means "everything",
    and the desktop client sent exactly that: on the real file that is 1.8 GB of
    arrays and 2.5 GB of base64, and the process died.
    """

    def test_the_limit_is_stated_in_rows_the_caller_can_use(self):
        from app.routers.vbt import _POINT_SUBSET_LIMIT_BYTES

        fields, rows = 11, 20_946_288
        payload = rows * fields * 8 * 4 // 3
        assert payload > _POINT_SUBSET_LIMIT_BYTES, "the real file must exceed the budget"

        fits = (_POINT_SUBSET_LIMIT_BYTES * 3 // 4) // (fields * 8)
        # The number offered back has to be usable: within the budget, and not
        # so conservative as to be useless.
        assert fits * fields * 8 * 4 // 3 <= _POINT_SUBSET_LIMIT_BYTES
        assert fits > 1_000_000

    def test_a_modest_table_is_not_refused(self, tmp_path):
        from app.routers.vbt import _POINT_SUBSET_LIMIT_BYTES

        fields, rows = 11, 200_000
        payload = rows * fields * 8 * 4 // 3
        assert payload < _POINT_SUBSET_LIMIT_BYTES
