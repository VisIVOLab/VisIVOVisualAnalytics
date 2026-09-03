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
import os
from dataclasses import dataclass
from pathlib import Path

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

    def out(self, stem: str, ext: str) -> Path:
        return OUT / f"{stem}{self.suffix()}.{ext}"


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
        default_path="/Users/fvitello/downloads/DemoData/WALLABY.fits",
        mask="sofia_wallaby_mask.fits",
        title="WALLABY",
        instrument="ASKAP, RA---SIN / VOPT",
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
