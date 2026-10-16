"""Validation cubes and their reference SoFiA-2 masks.

R-18 asked for a second cube: one cube with five sources cannot tell a property
of the method from a property of the observation. The registry keeps the two
apart from the scripts, so adding a third is a table entry rather than a fork.

Both entries are run through SoFiA-2 with the SAME detection parameters (see
``sofia_*.par``). That matters: if the reference finder were tuned per cube, the
agreement we report would partly be a property of the tuning.

Cubes live outside the repo (they are large). Each entry names the environment
variable that points at it, and the scripts exit with that name in the message
when the file is missing.
"""
from __future__ import annotations

import argparse
import math
import os
from dataclasses import dataclass
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"


@dataclass(frozen=True)
class Dataset:
    name: str
    env: str            # environment variable pointing at the cube
    default_path: str   # the author's local path, so it runs here with no setup
    mask: str           # reference SoFiA-2 mask, relative to sofia_out/
    title: str          # human label for figures and tables
    instrument: str
    note: str           # what makes this cube a DIFFERENT test, for the paper

    @property
    def cube(self) -> str:
        return os.environ.get(self.env, self.default_path)

    @property
    def mask_path(self) -> Path:
        return HERE / "sofia_out" / self.mask

    def suffix(self) -> str:
        """Output-name suffix. The primary cube keeps the unsuffixed names the
        paper and INDEX.md already cite; further cubes are suffixed."""
        return "" if self.name == DEFAULT else f"_{self.name}"

    def out(self, stem: str, ext: str, variant: str = "") -> Path:
        """`variant` names a run that must not overwrite the default one
        (`_smooth1-3-3`, `_ref-serra2013_hcg44`); it goes between the stem and
        the dataset suffix, which is the naming every artefact in out/ follows."""
        return OUT / f"{stem}{variant}{self.suffix()}.{ext}"


C_KMS = 299792.458


def spectral_axis_kms(header) -> tuple[np.ndarray, float]:
    """Per-channel velocity (km/s, optical convention) and the channel width.

    A frequency axis (the CASDA WALLABY cubelets) is converted with the rest
    frequency as v = c (nu0/nu - 1); a velocity axis (VOPT/VELO/VRAD, m/s or
    km/s) is read as it is. The CASDA cubelet and its relabelled copy in the
    2022 demo data therefore give the same axis, to 0.15 km/s."""
    nz = int(header["NAXIS3"])
    ch = np.arange(1, nz + 1, dtype=np.float64)
    axis = float(header["CRVAL3"]) + (ch - float(header["CRPIX3"])) * float(header["CDELT3"])
    ctype = str(header.get("CTYPE3", "")).upper()
    unit = str(header.get("CUNIT3", "")).strip().lower()
    if ctype.startswith("FREQ"):
        rest = float(header.get("RESTFRQ", header.get("RESTFREQ", 1420405751.79)))
        scale = {"hz": 1.0, "khz": 1e3, "mhz": 1e6, "ghz": 1e9}.get(unit, 1.0)
        vel = C_KMS * (rest / (axis * scale) - 1.0)
    elif ctype.startswith(("VOPT", "VELO", "VRAD", "VEL")):
        vel = axis / (1.0 if unit.startswith("km") else 1000.0)
    else:
        raise ValueError(f"spectral axis {ctype!r} is neither frequency nor velocity")
    return vel, float(np.mean(np.abs(np.diff(vel))))


def rank_sources(mask: np.ndarray, n: int | None = None):
    """Labelled sources by size, largest first: `(labels, counts, order)` with
    `order` the indices of the `n` largest. One ranking for every script, so row
    k of one table is the same object as row k of another."""
    labels, counts = np.unique(mask[mask > 0], return_counts=True)
    order = np.argsort(counts)[::-1][:n]
    return labels, counts, order


def robust_sigma(data: np.ndarray, source_free: np.ndarray | None = None) -> float:
    """Noise scale (1.4826·MAD) over finite values, from EMISSION-FREE voxels when
    a mask is given: including the sources biases the MAD high (0.05 % on
    HGC 44, 1.7 % on WALLABY), and excluding them costs nothing."""
    sample = data if source_free is None else data[source_free]
    finite = np.asarray(sample[np.isfinite(sample)], dtype=np.float64)
    if finite.size == 0:
        return 0.0
    if finite.size > 5_000_000:
        finite = finite[:: finite.size // 5_000_000]
    return float(1.4826 * np.median(np.abs(finite - np.median(finite))))


def parse_floats(spec: str, n: int, name: str, example: str,
                 lo: float | None = None, hi: float | None = None) -> tuple[float, ...]:
    """`n` comma-separated finite numbers, optionally bounded — for argparse
    `type=` and for hand parsing alike; the error names the option."""
    try:
        values = tuple(float(x) for x in spec.split(","))
    except ValueError:
        values = ()
    ok = (len(values) == n and all(math.isfinite(v) for v in values)
          and (lo is None or min(values) >= lo) and (hi is None or max(values) <= hi))
    if not ok:
        bounds = "" if lo is None and hi is None else f" in [{'-inf' if lo is None else lo:g}, {'inf' if hi is None else hi:g}]"
        raise argparse.ArgumentTypeError(f"{name} wants {n} finite numbers{bounds}, e.g. {example}")
    return values


DATASETS: dict[str, Dataset] = {
    "hgc44": Dataset(
        name="hgc44",
        env="VISIVO_VALIDATION_CUBE",
        default_path="/Users/fvitello/downloads/DemoData/hgc44_3axes.fits",
        mask="sofia_hgc44_mask.fits",
        title="HGC 44",
        instrument="HI observation, VELO-HEL",
        note="compact group: several galaxies of comparable brightness in one field",
    ),
    "wallaby": Dataset(
        name="wallaby",
        env="VISIVO_VALIDATION_CUBE_2",
        default_path="/Users/fvitello/downloads/DemoData/WALLABY_J100903-290239_Hydra_TR2_cube.fits",
        mask="sofia_wallaby_mask.fits",
        title="WALLABY",
        instrument="ASKAP, RA---SIN / FREQ",
        note="different instrument, projection and spectral convention; one large "
             "velocity-gradient source plus faint compact ones",
    ),
}
DEFAULT = "hgc44"


def add_dataset_arg(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--dataset", choices=sorted(DATASETS), default=DEFAULT,
        help=f"validation cube to run against (default: {DEFAULT})")


def resolve(name: str) -> Dataset:
    """Return the dataset, exiting with an actionable message if its inputs are
    missing — a co-author reproducing the paper must not get an astropy stack
    trace when they have simply not set the environment variable."""
    ds = DATASETS[name]
    missing = []
    if not Path(ds.cube).is_file():
        missing.append(f"  cube:  {ds.cube}\n         (set ${ds.env})")
    if not ds.mask_path.is_file():
        missing.append(f"  mask:  {ds.mask_path}\n         (reproduce it with "
                       f"sofia_{ds.name}.par — see validation/README.md)")
    if missing:
        raise SystemExit(f"error: inputs for dataset '{name}' not found:\n"
                         + "\n".join(missing))
    return ds
