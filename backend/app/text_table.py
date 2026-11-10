"""Delimited text tables: which separator a file uses, and reading it with it.

The classifier (:mod:`app.classify`) calls a text file a catalogue when its
lines parse into the same number of fields with a comma, a tab or a semicolon.
Every reader used to follow that with a plain ``pd.read_csv(path)``, which
splits on commas only: a tab-separated catalogue opened as one column (issue
#155). Both now ask :func:`sniff_separator` the same question about the same
first bytes, so what the classifier calls a table is read with the separator
it saw.

The detection, shared:
  - lines starting with ``#`` and blank lines do not vote;
  - the fields are PARSED with :mod:`csv` (quotes honoured), not counted as
    characters, so a quoted field may contain any separator;
  - a separator is taken when the first lines (up to ten) all parse into the
    same number of fields, at least two; comma first, then tab, then semicolon.

The reading (:func:`read_delimited_table`), with N the field count of the
first line that does not start with ``#``:
  - the header is that line, except when the ``#`` line right above it (blank
    lines between allowed), with its ``#`` and surrounding spaces removed,
    parses into N non-empty fields none of which is a number, and that first
    line has a number among its fields: then the ``#`` line is the header
    (``#id,ra,dec`` over ``1,10,20``) and the first line is data;
  - below the header, a line starting with ``#`` is a comment only when it does
    NOT parse into N fields; one that does is a row (a source named ``#S5``);
  - a file no separator makes consistent is read as ``pd.read_csv`` always read
    it (comma, short rows padded) when that gives a number somewhere: ragged
    rows, a trailing comma, a single column of numbers; otherwise it is refused.
"""
from __future__ import annotations

import csv
import io
import re
from pathlib import Path

import pandas as pd

#: How much of a file the separator is judged on (the classifier's sniff).
SNIFF_BYTES = 8192

#: Separators tried, in order: the first consistent one wins.
SEPARATORS = (",", "\t", ";")

#: A wider look for the reader only, when the sniff held fewer than two whole
#: lines (a header of hundreds of columns). The classifier has already said
#: "unknown" for such a file, so the two cannot disagree about it.
_WIDE_SNIFF_BYTES = 1 << 20

_SAMPLE_LINES = 10

_NUMBER = re.compile(r"^\s*[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?\s*$")
# A semicolon table's numeric field written with a decimal comma, or a whole
# number; and a number written with a decimal point.
_COMMA_OR_INTEGER = re.compile(r"^\s*[+-]?\d+(,\d+)?([eE][+-]?\d+)?\s*$")
_DECIMAL_POINT = re.compile(r"^\s*[+-]?\d*\.\d+([eE][+-]?\d+)?\s*$")


# What may begin a comment line: "#" right after a line break, or a line that
# starts with a space or a tab (then the per-row check decides). Searched with
# bytes.find, chunk by chunk: a regular expression, or mmap.find, over the
# same bytes costs as much as the parse.
_LINE_STARTS = (b"\n#", b"\n ", b"\n\t")
_CR_LINE_STARTS = (b"\r#", b"\r ", b"\r\t")
_CHUNK_BYTES = 8 << 20


def _is_significant(line: str) -> bool:
    # A byte-order mark is not part of the line: "\ufeff# note" is a comment.
    line = line.lstrip("\ufeff")
    return bool(line.strip()) and not line.lstrip().startswith("#")


def _significant_lines(text: str, truncated: bool) -> list[str]:
    lines = text.splitlines()
    if truncated and lines:
        # A read cut at a byte budget ends in half a row.
        lines = lines[:-1]
    return [line for line in lines if _is_significant(line)]


def _sample_rows(lines: list[str], separator: str) -> list[list[str]] | None:
    try:
        return [row for row in csv.reader(io.StringIO("\n".join(lines)), delimiter=separator)
                if row]
    except csv.Error:
        return None


def _fields(line: str, separator: str) -> list[str]:
    """One line's fields, quotes honoured; [] when it does not parse."""
    rows = _sample_rows([line], separator)
    return rows[0] if rows else []


def sniff_separator(text: str, truncated: bool = False) -> str | None:
    """The separator that makes ``text`` a table, or None.

    ``truncated`` says ``text`` is the start of a longer file, so its last line
    is probably incomplete and is not judged. One line with commas is a
    sentence; ten lines that parse into the same number of fields is a table.
    """
    lines = _significant_lines(text, truncated)
    if len(lines) < 2:
        return None
    sample = lines[:_SAMPLE_LINES]
    for separator in SEPARATORS:
        rows = _sample_rows(sample, separator)
        if rows is None:
            continue
        counts = [len(row) for row in rows]
        if len(counts) >= 2 and counts[0] >= 2 and len(set(counts)) == 1:
            return separator
    return None


def describe_table(text: str, truncated: bool = False) -> tuple[str, int] | None:
    """``(separator, columns)`` when ``text`` is a delimited table, else None.

    The file browser's preview names the table it shows with the separator the
    readers would use (:func:`sniff_separator`) and the field count of the
    first line that votes, so preview and reader cannot disagree.
    """
    # Judge the same head the classifier and the readers judge (SNIFF_BYTES),
    # even when the preview shows more of the file.
    head = text.encode("utf-8")
    if len(head) > SNIFF_BYTES:
        text = head[:SNIFF_BYTES].decode("utf-8", errors="ignore")
        truncated = True
    separator = sniff_separator(text, truncated)
    if separator is None:
        return None
    return separator, len(_fields(_significant_lines(text, truncated)[0], separator))


def _open_text(path: Path):
    """A binary stream of the file's content, through its compression.

    ``pd.read_csv`` infers compression from the name; the sniff must read the
    same bytes the parser will, not the compressed ones.
    """
    name = path.name.lower()
    if name.endswith(".gz"):
        import gzip  # noqa: PLC0415
        return gzip.open(path, "rb")
    if name.endswith(".bz2"):
        import bz2  # noqa: PLC0415
        return bz2.open(path, "rb")
    if name.endswith(".xz"):
        import lzma  # noqa: PLC0415
        return lzma.open(path, "rb")
    return open(path, "rb")


def _text_lines(path: Path):
    """The file's physical lines, decoded as the sniff decodes them."""
    return io.TextIOWrapper(_open_text(path), encoding="utf-8", errors="replace",
                            newline="")


def read_head(path: str | Path, nbytes: int = SNIFF_BYTES) -> tuple[str, bool]:
    """``(text, truncated)``: the first ``nbytes`` of the file, decoded as the
    classifier decodes them, and whether the file goes on past them."""
    with _open_text(Path(path)) as handle:
        raw = handle.read(nbytes + 1)
    truncated = len(raw) > nbytes
    return raw[:nbytes].decode("utf-8", errors="replace"), truncated


def _header_only_separator(lines: list[str]) -> str | None:
    """A table with a header and no rows yet: the first separator that splits
    the header into two names or more."""
    for separator in SEPARATORS:
        rows = _sample_rows(lines[:1], separator)
        if rows and len(rows[0]) >= 2:
            return separator
    return None


def _uses_decimal_comma(data_lines: list[str]) -> bool:
    """Whether a semicolon table writes its numbers with a decimal comma, as a
    spreadsheet in a European locale exports them.

    "1,2" alone is ambiguous (a decimal, or a list of two identifiers), so it
    takes TWO columns of the sampled rows that read entirely as numbers with
    decimal commas (whole numbers allowed, one comma at least), and no number
    written with a decimal point anywhere in the sample.
    """
    rows = _sample_rows(data_lines[:_SAMPLE_LINES], ";") or []
    if not rows or any(_DECIMAL_POINT.match(f) for row in rows for f in row):
        return False
    width = max(len(row) for row in rows)
    qualifying = 0
    for index in range(width):
        values = [row[index] for row in rows if index < len(row) and row[index].strip()]
        if (values and all(_COMMA_OR_INTEGER.match(v) for v in values)
                and any("," in v for v in values)):
            qualifying += 1
    return qualifying >= 2


class _NotATable(ValueError):
    """No separator gives a consistent table; ``has_rows`` says whether there
    were uncommented lines at all (only then may the comma reading be tried)."""

    def __init__(self, message: str, has_rows: bool):
        super().__init__(message)
        self.has_rows = has_rows


def _detect(path: Path) -> str:
    for budget in (SNIFF_BYTES, _WIDE_SNIFF_BYTES):
        text, truncated = read_head(path, budget)
        separator = sniff_separator(text, truncated)
        if separator is not None:
            return separator
        lines = _significant_lines(text, truncated)
        if len(lines) >= 2 or not truncated:
            break
    if not lines:
        if not text.strip():
            raise _NotATable(f"{path.name} is empty.", False)
        raise _NotATable(f"{path.name} holds no table: every line is a '#' comment.",
                         False)
    if len(lines) == 1 and not truncated:
        separator = _header_only_separator(lines)
        if separator is not None:
            return separator
    raise _NotATable(
        f"{path.name} is not a delimited text table: no separator (comma, tab or "
        "semicolon) splits its first lines into the same number of fields, two or "
        "more. A whitespace-aligned table opens as IPAC (.tbl).", True)


def detect_separator(path: str | Path) -> str:
    """The separator of a delimited text table, as the classifier sees it.

    Raises ``ValueError`` saying why when no separator gives a table.
    """
    return _detect(Path(path))


def _layout(path: Path, separator: str) -> tuple[int, int, list[str] | None]:
    """``(first, nfields, names)``: the physical index of the first line not
    starting with ``#``, its field count, and the header names taken from the
    ``#`` line above it when that line is the header (else None)."""
    candidate: str | None = None
    with _text_lines(path) as lines:
        for index, raw in enumerate(lines):
            if index == 0:
                raw = raw.lstrip("\ufeff")
            if not raw.strip():
                continue                      # blanks keep the candidate
            if raw.lstrip().startswith("#"):
                candidate = raw
                continue
            first = _fields(raw, separator)
            names = None
            if candidate is not None and any(_NUMBER.match(f) for f in first):
                header = _fields(candidate.lstrip().lstrip("#").strip(), separator)
                header = [h.strip() for h in header]
                if (len(header) == len(first) and all(header)
                        and not any(_NUMBER.match(h) for h in header)):
                    names = header
            return index, len(first), names
    return 0, 0, None


def _holds_hash_after(path: Path, offset: int) -> bool:
    """Whether a line past the first ``offset`` may start with "#": one does,
    or one starts with a space or a tab.

    A byte search in large chunks (``bytes.find``, a C search): a table with
    no such line, nearly every one, then skips the per-row check below, which
    costs half the parse on a column of names. A compressed file is not
    searched and is always checked.
    """
    if path.name.lower().endswith((".gz", ".bz2", ".xz")):
        return True
    with open(path, "rb") as handle:
        for _ in range(offset):
            if not handle.readline().endswith(b"\n"):
                # Lines counted on "\r" alone (an old Mac file): do not guess.
                return True
        # From the header on; the header itself does not start with "#".
        previous = b""
        while True:
            chunk = handle.read(_CHUNK_BYTES)
            if not chunk:
                return False
            window = previous + chunk
            # Lines end in "\n" (or "\r\n"); only an old Mac file breaks on "\r".
            starts = _LINE_STARTS if window.find(b"\n") >= 0 else _CR_LINE_STARTS
            # No "#" at all (a one-byte memchr, far faster than the two-byte
            # searches): no comment can start in this chunk.
            if window.find(b"#") >= 0 and any(window.find(token) >= 0
                                               for token in starts):
                return True
            previous = window[-1:]   # a line break that ends the chunk


def _may_hold_comment_rows(frame: pd.DataFrame) -> bool:
    """Whether a comment line further down may have been read as a row.

    A comment is a "#" line with fewer or more fields than the table; with
    more, pandas raises. With fewer, its text lands in the first column and
    the row is padded, so its last column is empty. A "#" row with every field
    filled is data and does not count.
    """
    if frame.empty or len(frame.columns) < 2:
        return False
    first = frame.iloc[:, 0]
    if pd.api.types.is_numeric_dtype(first):
        return False
    hashed = first.astype(str).str.lstrip().str.startswith("#")
    return bool((hashed & frame.iloc[:, -1].isna()).any())


def _without_comment_lines(path: Path, first: int, nfields: int, separator: str) -> str:
    """The table from line ``first`` on, without its comment lines.

    Records are followed by their quotes, so a quoted field spanning lines is
    kept whole and a line inside it is never taken for a comment. A line that
    starts a record with "#" is a comment when it does not parse into
    ``nfields`` fields. Only read when the table has such a line (rare): the
    whole file is then held as text once.
    """
    kept: list[str] = []
    in_quotes = False
    with _text_lines(path) as lines:
        for index, raw in enumerate(lines):
            if index < first:
                continue
            if (not in_quotes and raw.lstrip().startswith("#")
                    and len(_fields(raw.rstrip("\r\n"), separator)) != nfields):
                continue
            kept.append(raw)
            if raw.count('"') % 2:
                in_quotes = not in_quotes
    return "".join(kept)


def _comma_reading(path: Path) -> pd.DataFrame | None:
    """The file as ``pd.read_csv(path)`` read it before #155 (leading comment
    and blank lines skipped), when that gives a table with a number in it:
    ragged rows, a trailing comma on the rows, a single column of numbers.
    Prose read this way is all text, and is refused (None)."""
    first, _, _ = _layout(path, ",")
    try:
        # index_col=False: a trailing comma on the rows only must not turn
        # the first column into the index and shift every value one column.
        frame = pd.read_csv(path, skiprows=first, index_col=False)
    except (pd.errors.ParserError, pd.errors.EmptyDataError, ValueError):
        return None
    for name in frame.columns:
        column = frame[name]
        if (pd.api.types.is_numeric_dtype(column) and not pd.api.types.is_bool_dtype(column)
                and column.notna().any()):
            return frame
    return None


def read_delimited_table(path: str | Path) -> pd.DataFrame:
    """A comma-, tab- or semicolon-separated table as a DataFrame.

    The separator is :func:`detect_separator`'s; quoting is the csv standard
    (``"`` quotes, doubled to escape), so a quoted field may hold the other
    separators. Header and comment lines follow the rules in the module
    docstring. A comma file without ``#`` lines reads as ``pd.read_csv(path)``
    read it.
    """
    path = Path(path)
    try:
        separator = _detect(path)
    except _NotATable as exc:
        frame = _comma_reading(path) if exc.has_rows else None
        if frame is None:
            raise ValueError(str(exc)) from None
        return frame
    first, nfields, names = _layout(path, separator)
    options: dict = {"sep": separator, "index_col": False}
    if names is not None:
        options.update(header=None, names=names)
    if separator == ";":
        text, truncated = read_head(path)
        data = _significant_lines(text, truncated)
        if _uses_decimal_comma(data if names is not None else data[1:]):
            options["decimal"] = ","
    try:
        frame = pd.read_csv(path, skiprows=first, **options)
    except pd.errors.ParserError:
        frame = None
    if frame is None or (_holds_hash_after(path, first) and _may_hold_comment_rows(frame)):
        # A comment line among the rows (rare): read again without it. A
        # ParserError with no comment to blame is the file's own fault and is
        # reported as pandas says it.
        cleaned = _without_comment_lines(path, first, nfields, separator)
        frame = pd.read_csv(io.StringIO(cleaned), **options)
    if options.get("decimal") == ",":
        frame = _numbers_in_either_notation(frame)
    return frame


# A number with a decimal comma or a decimal point, or a whole number.
_EITHER_NOTATION = r"[+-]?(\d+([.,]\d*)?|[.,]\d+)([eE][+-]?\d+)?"


def _numbers_in_either_notation(frame: pd.DataFrame) -> pd.DataFrame:
    """Columns pandas left as text because the decimal-comma decision, made on
    the first rows, met a decimal point further down ("10,5" then "20.5"):
    numeric when every value is a number in one notation or the other. Real
    text columns stay text."""
    for name in frame.columns:
        column = frame[name]
        # Text is object, or pandas 3's own string dtype.
        if not (column.dtype == object or pd.api.types.is_string_dtype(column)):
            continue
        values = column.dropna().astype(str).str.strip()
        values = values[values != ""]
        if values.empty or not values.str.fullmatch(_EITHER_NOTATION).all():
            continue
        text = column.astype(str).str.strip().str.replace(",", ".", regex=False)
        frame[name] = pd.to_numeric(text.where(column.notna()), errors="coerce")
    return frame
