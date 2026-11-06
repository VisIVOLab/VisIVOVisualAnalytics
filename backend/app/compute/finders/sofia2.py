"""SoFiA-2 as a finder plugin: a child process of the event loop.

The one validated way parameters reach SoFiA-2: :func:`refused` checks them
and :func:`par_lines` writes the .par, for this task finder and for the
synchronous ``/v1/sourcefind`` route (:func:`app.compute.sofia.worker_sofia`)
alike. A task needs more than the route: the run must stop when the task is
cancelled, and its log must be kept. So the process is started with asyncio
here (see ``_process``), killed on cancellation or on the timeout, and its
output is written next to the products; :func:`summarise_log` reads the
reliability counts back out of it.
"""
from __future__ import annotations

import asyncio
import difflib
import math
import re
from pathlib import Path
from typing import Any

from ..sofia import _parse_votable_rows, _sofia_value, resolve_sofia_bin
from ._process import ChildTimeout, run_child

# SoFiA-2 parameters a caller may set, with what they mean (the names are
# SoFiA's own; see its documentation for the full list and the defaults).
PARAMETERS = {
    "scfind.threshold": "S/N threshold of the smooth-and-clip finder (default 5.0)",
    "scfind.kernelsXY": "spatial smoothing kernels in pixels, e.g. '0, 3, 6'",
    "scfind.kernelsZ": "spectral smoothing kernels in channels, e.g. '0, 3, 7'",
    "linker.minSizeXY": "minimum spatial size of a source in pixels",
    "linker.minSizeZ": "minimum spectral size of a source in channels",
    "reliability.enable": "true to filter detections by reliability",
    "reliability.threshold": "reliability threshold (default 0.9)",
    "scaleNoise.enable": "true to normalise the noise (scaleNoise.mode spectral or local)",
    "filter.minSNR": "minimum integrated S/N of a source (default 3.0)",
    "contsub.enable": "true to subtract residual continuum (order in contsub.order)",
    "input.invert": "true to search the inverted cube (absorption)",
    "input.region": "sub-cube to search: x_min, x_max, y_min, y_max, z_min, z_max (pixels from 0)",
}

# What a caller may set: the pipeline's own switches and the flag, continuum,
# noise-scaling, finder, linker, reliability, filter, dilation and
# parametrisation sections, plus two input keys that name no file
# (``input.invert``, ``input.region``). Not the rest of the input section nor
# the output section, which the service sets, and not the few keys elsewhere
# that name a file for SoFiA to read: a parameter must never make the node open
# a path of the caller's choosing. Every key must be one SoFiA-2 2.7 knows:
# SoFiA stops on an unknown one (``pipeline.pedantic``), and a name close to a
# real one (``scfind.kernelXY``) is answered here with the right spelling
# rather than by SoFiA's exit code 7 once the run has started.
# (_ALLOWED_INPUT_KEYS, _FILE_INPUTS, _SPEC below.)

# Memory per byte of cube: SoFiA-2 holds the cube, the mask and the copy the
# smooth-and-clip finder smooths. Measured on the SKAVA testbed: 2.25x the
# cube's bytes of anonymous memory at the peak (a 512 MB cube runs under a
# 1280 MB cgroup limit and is killed under 1024 MB); a little on top for the
# process itself and the catalogue it writes.
MEMORY_FACTOR = 2.3
MEMORY_FLOOR_BYTES = 128 << 20

#: SoFiA-2's exit code when nothing is left after filtering (ERR_NO_SRC_FOUND).
EXIT_NO_SOURCES = 8

_ALLOWED_INPUT_KEYS = frozenset({"input.invert", "input.region"})
_FILE_INPUTS = frozenset({"flag.cube", "flag.catalog", "reliability.catalog"})
_KEY = re.compile(r"^[A-Za-z]+\.[A-Za-z0-9]+$")
_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_RULE = re.compile(r"[─│┌┐└┘]+|_{3,}")

# Every SoFiA-2 2.7 parameter of the sections a caller may set, with how
# SoFiA reads its value and its built-in default (the template .par of the
# ``sofiapipeline/sofia2`` image). SoFiA checks no value it reads: a switch is
# on only when it reads exactly "true", a number is what strtod/strtol make of
# the text (0 for "abc"), and an unknown word picks a fallback
# (``scfind.statistic = bogus`` runs with the standard deviation). So a value
# is checked here against how SoFiA reads it, before the run.
_BOOL, _INT, _FLOAT, _INTS, _FLOATS, _TEXT = "bool", "int", "float", "ints", "floats", "text"
_STATISTIC = ("std", "mad", "gauss")
_FLUX_RANGE = ("negative", "positive", "full")
_REL_PARAMETERS = ("peak", "sum", "mean", "chan", "pix", "fill", "std", "skew", "kurt")

#: key -> (how SoFiA reads it, its default, the words it knows or None).
_SPEC: dict[str, tuple[str, str, tuple[str, ...] | None]] = {
    "pipeline.verbose": (_BOOL, "false", None),
    "pipeline.pedantic": (_BOOL, "true", None),
    "pipeline.threads": (_INT, "0", None),
    "input.invert": (_BOOL, "false", None),
    "input.region": (_INTS, "", None),
    "flag.region": (_INTS, "", None),
    "flag.cube": (_TEXT, "", None),
    "flag.catalog": (_TEXT, "", None),
    "flag.radius": (_INT, "5", None),
    "flag.auto": (_TEXT, "false", ("false", "true", "channels", "pixels")),
    "flag.threshold": (_FLOAT, "5.0", None),
    "flag.log": (_BOOL, "false", None),
    "contsub.enable": (_BOOL, "false", None),
    "contsub.order": (_INT, "0", None),
    "contsub.threshold": (_FLOAT, "2.0", None),
    "contsub.shift": (_INT, "4", None),
    "contsub.padding": (_INT, "3", None),
    "scaleNoise.enable": (_BOOL, "false", None),
    "scaleNoise.mode": (_TEXT, "spectral", ("spectral", "spatial", "local")),
    "scaleNoise.statistic": (_TEXT, "mad", _STATISTIC),
    "scaleNoise.fluxRange": (_TEXT, "negative", _FLUX_RANGE),
    "scaleNoise.windowXY": (_INTS, "25", None),
    "scaleNoise.windowZ": (_INTS, "15", None),
    "scaleNoise.gridXY": (_INTS, "0", None),
    "scaleNoise.gridZ": (_INTS, "0", None),
    "scaleNoise.interpolate": (_BOOL, "false", None),
    "scaleNoise.scfind": (_BOOL, "false", None),
    "scfind.enable": (_BOOL, "true", None),
    "scfind.kernelsXY": (_FLOATS, "0, 3, 6", None),   # read as doubles
    "scfind.kernelsZ": (_INTS, "0, 3, 7, 15", None),
    "scfind.threshold": (_FLOAT, "5.0", None),
    "scfind.replacement": (_FLOAT, "2.0", None),
    "scfind.statistic": (_TEXT, "mad", _STATISTIC),
    "scfind.fluxRange": (_TEXT, "negative", _FLUX_RANGE),
    "threshold.enable": (_BOOL, "false", None),
    "threshold.threshold": (_FLOAT, "5.0", None),
    "threshold.mode": (_TEXT, "relative", ("absolute", "relative")),
    "threshold.statistic": (_TEXT, "mad", _STATISTIC),
    "threshold.fluxRange": (_TEXT, "negative", _FLUX_RANGE),
    "linker.enable": (_BOOL, "true", None),
    "linker.radiusXY": (_INT, "1", None),
    "linker.radiusZ": (_INT, "1", None),
    "linker.minSizeXY": (_INT, "5", None),
    "linker.minSizeZ": (_INT, "5", None),
    "linker.maxSizeXY": (_INT, "0", None),
    "linker.maxSizeZ": (_INT, "0", None),
    "linker.minPixels": (_INT, "0", None),
    "linker.maxPixels": (_INT, "0", None),
    "linker.minFill": (_FLOAT, "0.0", None),
    "linker.maxFill": (_FLOAT, "0.0", None),
    "linker.positivity": (_BOOL, "false", None),
    "reliability.enable": (_BOOL, "false", None),
    "reliability.parameters": (_TEXT, "peak, sum, mean", _REL_PARAMETERS),
    "reliability.threshold": (_FLOAT, "0.9", None),
    "reliability.scaleKernel": (_FLOAT, "0.4", None),
    "reliability.autoKernel": (_BOOL, "false", None),
    "reliability.iterations": (_INT, "30", None),
    "reliability.tolerance": (_FLOAT, "0.05", None),
    "reliability.catalog": (_TEXT, "", None),
    "reliability.plot": (_BOOL, "true", None),
    "reliability.debug": (_BOOL, "false", None),
    "filter.discardNegative": (_BOOL, "true", None),
    "filter.minSNR": (_FLOAT, "3.0", None),
    "filter.minPixels": (_INT, "0", None),
    "dilation.enable": (_BOOL, "false", None),
    "dilation.iterationsXY": (_INT, "10", None),
    "dilation.iterationsZ": (_INT, "5", None),
    "dilation.threshold": (_FLOAT, "0.001", None),
    "parameter.enable": (_BOOL, "true", None),
    "parameter.wcs": (_BOOL, "true", None),
    "parameter.physical": (_BOOL, "false", None),
    "parameter.prefix": (_TEXT, "SoFiA", None),
    "parameter.offset": (_BOOL, "false", None),
}

KNOWN_KEYS = frozenset(_SPEC)

#: SoFiA-2 2.7's built-in default of every parameter a caller may set ("" when
#: it has none, as for input.region).
DEFAULTS: dict[str, str] = {key: spec[1] for key, spec in _SPEC.items()
                            if key not in _FILE_INPUTS}

#: The sections of a SoFiA-2 2.7 .par that this service does not offer.
_OTHER_SECTIONS = ("output", "background")

# ASCII only: Python's \d also matches other scripts' digits, which C's
# strtod/strtol read as nothing.
_INT_TEXT = re.compile(r"^[+-]?[0-9]+$")
_UINT_TEXT = re.compile(r"^[0-9]+$")
_FLOAT_TEXT = re.compile(r"^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$")
_PREFIX = re.compile(r"^[A-Za-z0-9_]+$")


def _is_number(text: str) -> bool:
    """``text`` is a number strtod reads as written: not one that overflows
    to infinity (``1e999``)."""
    return bool(_FLOAT_TEXT.match(text)) and math.isfinite(float(text))


def _list_items(value: str) -> list[str]:
    """The items SoFiA reads from a list: it splits on "," (strtok) and reads
    each with strtod, which skips the white space before it; so spaces and tabs
    around an item are fine and are not separators."""
    return [item.strip(" \t") for item in value.split(",")]


def _value_refused(name: str, text: str) -> str | None:
    """Why ``text`` is not a value SoFiA-2 reads as meant for ``name``."""
    kind, _default, words = _SPEC[name]
    value = text.strip()
    if not value:
        if name in ("input.region", "flag.region"):
            return None   # no region: the whole cube
        return f"parameter {name!r} has no value (leave it out to keep SoFiA's default)"
    if kind == _BOOL:
        if value.lower() not in ("true", "false"):
            return f"parameter {name!r} must be true or false, not {value!r}"
    elif kind == _INT:
        if not _INT_TEXT.match(value):
            return f"parameter {name!r} must be a whole number, not {value!r}"
    elif kind == _FLOAT:
        if not _is_number(value):
            return f"parameter {name!r} must be a number, not {value!r}"
    elif kind == _FLOATS:
        if not all(_is_number(item) for item in _list_items(value)):
            return f"parameter {name!r} must be numbers separated by commas, not {value!r}"
    elif kind == _INTS:
        items = _list_items(value)
        count = len(items) if all(_UINT_TEXT.match(item) for item in items) else 0
        if name == "input.region" and count != 6:
            return ("parameter 'input.region' must be six pixel indices: "
                    "x_min, x_max, y_min, y_max, z_min, z_max")
        if name == "flag.region" and (not count or count % 6):
            return ("parameter 'flag.region' must be groups of six pixel indices: "
                    "x_min, x_max, y_min, y_max, z_min, z_max")
        if not count:
            return (f"parameter {name!r} must be whole numbers separated by commas, "
                    f"not {value!r}")
    elif name == "reliability.parameters":
        # SoFiA splits this one on commas and spaces (strtok ", ").
        if not re.fullmatch(r"[A-Za-z, ]+", value):
            return ("parameter 'reliability.parameters' must be names separated by commas, "
                    f"from {', '.join(words)}")
        names = [n for n in re.split(r"[, ]+", value) if n]
        unknown = [n for n in names if n not in words]
        if unknown:
            return (f"parameter 'reliability.parameters' does not know {unknown[0]!r}; "
                    f"it takes two or more of {', '.join(words)}")
        if len(set(names)) < 2:
            return ("parameter 'reliability.parameters' needs at least two of "
                    f"{', '.join(words)}")
    elif words is not None:
        if value not in words:
            return f"parameter {name!r} must be one of {', '.join(words)}, not {value!r}"
    elif name == "parameter.prefix":
        if not _PREFIX.match(value):
            return "parameter 'parameter.prefix' must be letters, digits or underscores"
    return None


def _par_value(name: str, value: Any) -> str:
    """``value`` as the .par line carries it: a switch in lower case, the one
    spelling SoFiA reads as on."""
    text = _sofia_value(value).strip()
    if name in _SPEC and _SPEC[name][0] == _BOOL:
        return text.lower()
    return text


def info():
    from . import FinderInfo

    binary = resolve_sofia_bin()
    return FinderInfo(
        name="sofia2",
        kinds=("cube",),
        description="SoFiA-2 smooth-and-clip source finder for spectral-line cubes.",
        parameters=PARAMETERS,
        available=bool(binary),
        reason="" if binary else "SoFiA-2 executable not found (set VISIVO_SOFIA_BIN or put sofia2 on PATH)",
        memory_factor=MEMORY_FACTOR,
        memory_floor_bytes=MEMORY_FLOOR_BYTES,
    )


def refused(params: dict[str, Any]) -> str | None:
    """Why a parameter set is not accepted, or None when it is."""
    for key, value in params.items():
        name = str(key).strip()
        if not _KEY.match(name):
            return f"parameter {name!r} is not a SoFiA-2 parameter name"
        section = name.split(".", 1)[0].lower()
        if section == "input":
            if name not in _ALLOWED_INPUT_KEYS:
                return f"parameter {name!r} is not accepted: the input files are set by the service"
        elif section == "output":
            return f"parameter {name!r} is not accepted: the output section is set by the service"
        elif section in _OTHER_SECTIONS:
            return f"parameter {name!r} is not accepted: this service does not offer the {section} section"
        if name in _FILE_INPUTS:
            return f"parameter {name!r} is not accepted: it names a file for SoFiA-2 to read"
        if name not in KNOWN_KEYS:
            close = difflib.get_close_matches(name, sorted(KNOWN_KEYS), n=1, cutoff=0.8)
            hint = f" (did you mean {close[0]!r}?)" if close else ""
            return f"parameter {name!r} is not a SoFiA-2 2.7 parameter{hint}"
        text = _sofia_value(value)
        if "\n" in text or "\r" in text or len(text) > 500:
            return f"parameter {name!r} has an invalid value"
        if why := _value_refused(name, text):
            return why
    return None


def par_lines(path: str, out_dir: Path | str, params: dict[str, Any]) -> list[str]:
    """The .par of a run of ``path`` into ``out_dir``: the service's input and
    output settings, then the caller's ``params``, which must have passed
    :func:`refused`. ValueError for a path SoFiA-2 cannot be given."""
    for text in (str(path), str(out_dir)):
        if "\n" in text or "\r" in text:
            # A pathname can hold a newline; in the .par it would be a second line.
            raise ValueError("the dataset path contains a line break, which SoFiA-2 cannot be given")
    lines = [
        f"input.data = {path}",
        f"output.directory = {out_dir}",
        "output.filename = sofia_out",
        "output.writeCatXML = true",
        "output.writeMask = true",
        "output.overwrite = true",
    ]
    lines += [f"{str(key).strip()} = {_par_value(str(key).strip(), value)}"
              for key, value in params.items()]
    return lines


def summarise_log(text: str) -> dict[str, Any]:
    """What a run's log says about the noise and the reliability filter.

    ``n_positive``/``n_negative``: the detections the reliability filter
    modelled ("Found N positive and M negative sources", only when it ran);
    ``rho``: SoFiA's Pearson coefficient of adjacent channels;
    ``n_after_filter``: sources left after reliability and filtering;
    ``warnings``: the first sentence of each of SoFiA's warnings (the text
    inside a boxed one); ``errors``: SoFiA's ERROR messages (exit 8, "No
    sources left after filtering", is one: an empty result, not a failure);
    ``note``: the sentence the client shows next to the suggestion.
    """
    text = _ANSI.sub("", text or "")
    out: dict[str, Any] = {"n_positive": None, "n_negative": None, "rho": None,
                           "n_after_filter": None, "warnings": [], "errors": [], "note": ""}
    if m := re.search(r"Found\s+(\d+)\s+positive and\s+(\d+)\s+negative sources", text):
        out["n_positive"], out["n_negative"] = int(m.group(1)), int(m.group(2))
    if m := re.search(r"Pearson coefficient[^:\n]*:\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)", text):
        out["rho"] = float(m.group(1))
    if m := re.search(r"(\d+)\s+sources? left after reliability analysis and filtering", text):
        out["n_after_filter"] = int(m.group(1))
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.startswith("WARNING:"):
            message = line[len("WARNING:"):].strip(" \t│┌└─┐┘")
            if not message:
                # A boxed warning: its text is on the lines inside the box.
                inside = []
                for follow in lines[i + 1:]:
                    if "└" in follow or not follow.strip():
                        break
                    inside.append(follow.strip(" \t│"))
                message = " ".join(inside).split(". ")[0].strip()
            if message and message not in out["warnings"]:
                out["warnings"].append(message)
        elif line.startswith("ERROR:"):
            message = line[len("ERROR:"):].strip()
            if message and message not in out["errors"]:
                out["errors"].append(message)
    if out["n_negative"] is not None:
        out["note"] = (f"Reliability ran on {out['n_negative']} negative and "
                       f"{out['n_positive']} positive detections.")
    elif any("No sources left" in e for e in out["errors"]):
        out["note"] = "No sources left after filtering."
    return out


def failure_detail(out: str, err: str, limit: int = 1200) -> str:
    """What a failed run says about why, bounded to ``limit`` characters.

    SoFiA-2 prints everything, its errors included, on stdout, and a run
    often opens with a long warning box (HCG 44's CELLSCAL), so the head of
    the output says nothing. Its ``ERROR:`` lines (with their indented
    continuation lines) come first, then the last lines of the output.
    """
    text = _ANSI.sub("", (err or "") + ("\n" if err and out else "") + (out or ""))
    lines = [line.rstrip() for line in text.splitlines()]
    errors: list[str] = []
    for i, line in enumerate(lines):
        if line.startswith("ERROR:"):
            block = [line[len("ERROR:"):].strip()]
            for follow in lines[i + 1:]:
                if not follow.startswith((" ", "\t")) or not follow.strip():
                    break
                block.append(follow.strip())
            message = " ".join(block)
            if message not in errors:
                errors.append(message)
    # SoFiA's rules and boxes (rows of "_" or of box-drawing characters) say
    # nothing and would take the room of the lines that do.
    said = [t for t in (_RULE.sub(" ", line).strip() for line in lines) if t]
    tail = " | ".join(said)[-max(200, limit // 3):]
    detail = "; ".join(errors)
    if detail:
        detail = detail[: limit - len(tail) - 16] + " … last output: " + tail
    else:
        detail = tail
    return detail[:limit]


def read_summary(log_path: Path | str) -> dict[str, Any]:
    """:func:`summarise_log` of a log file (an empty summary when it cannot be read)."""
    try:
        return summarise_log(Path(log_path).read_text(errors="replace"))
    except OSError:
        return summarise_log("")


async def run(path: str, out_dir: Path, params: dict[str, Any], timeout: float) -> dict[str, Any]:
    from . import FinderError, FinderUnavailable

    sofia_bin = resolve_sofia_bin()
    if not sofia_bin:
        raise FinderUnavailable("SoFiA-2 executable not found")
    why = refused(params)
    if why is not None:
        raise FinderError(why)
    try:
        lines = par_lines(path, out_dir, params)
    except ValueError as exc:
        raise FinderError(str(exc)) from exc
    par_path = out_dir / "sofia.par"
    par_path.write_text("\n".join(lines) + "\n")

    log_path = out_dir / "sofia.log"
    try:
        code, out, err = await run_child([sofia_bin, str(par_path)], out_dir, timeout, log_path)
    except ChildTimeout as exc:
        raise FinderError(f"SoFiA-2 {exc}") from exc
    if code not in (0, EXIT_NO_SOURCES):
        # Killed by SIGKILL: on a node under a memory limit, that is the
        # kernel's out-of-memory killer, not a SoFiA-2 error.
        killed = code in (-9, 137)
        hint = (f" (killed; most often the node ran out of memory, SoFiA-2 needs about "
                f"{MEMORY_FACTOR:g}x the cube's size)" if killed else "")
        raise FinderError(f"SoFiA-2 failed (exit {code}){hint}: {failure_detail(out, err)}")
    # Exit 8 is SoFiA's "no sources": a search that found nothing, which is a
    # result (the inversion test of a reliable setting expects it), not a
    # failure. Its log and .par stay, with n_sources 0.

    cat_path = out_dir / "sofia_out_cat.xml"
    mask_path = out_dir / "sofia_out_mask.fits"
    sources = await asyncio.to_thread(_parse_votable_rows, cat_path) if cat_path.exists() else []
    return {
        "n_sources": len(sources),
        "sources": sources,
        "mask_path": str(mask_path) if mask_path.exists() else "",
        "catalog_path": str(cat_path) if cat_path.exists() else "",
        "log_path": str(log_path),
        "exit_code": code,
        "summary": await asyncio.to_thread(read_summary, log_path),
    }
