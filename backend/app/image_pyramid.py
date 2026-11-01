"""2-D images larger than memory: tiles from an averaged pyramid (issue #85).

``/v1/image/tile`` reads a tile of level ``L`` as every ``2**L``-th pixel of
the file. That needs no preparation, but a coarse tile shows one pixel in
``4**L`` (noise and small sources alias), and it touches pages across the
whole file. An image larger than ``VISIVO_IMAGE_PYRAMID_BYTES`` (float32,
256 MiB by default) gets a pyramid instead: the brick pyramid of the cubes
(``app.brick_pyramid``), one plane deep, where each level is the mean of 2x2
pixels of the one below over the finite ones. It is built in the background
the first time a tile of the image is asked for (``app.pyramid_builds``, as
for a cube's remote view), and until it is ready tiles keep coming from the
file, sampled. Each tile says which it is (``averaged``), because the two
place their pixels differently: a sampled pixel ``i`` of level ``L`` is source
pixel ``i * 2**L``, an averaged one stands for source pixels
``i * 2**L .. (i + 1) * 2**L - 1``, centred half a cell further on. (Its value
is the mean of the 2x2 means of the level below, which is the mean of those
source pixels where they are all finite.) A build that fails is not retried
for the same version of the image.
"""
from __future__ import annotations

import logging
import os
from dataclasses import dataclass

import numpy as np

logger = logging.getLogger("visivo.backend")

#: Images above this many float32 bytes are served from an averaged pyramid.
DEFAULT_IMAGE_PYRAMID_BYTES = 256 * 1024 ** 2


def image_pyramid_bytes() -> int:
    try:
        return int(os.environ.get("VISIVO_IMAGE_PYRAMID_BYTES", DEFAULT_IMAGE_PYRAMID_BYTES))
    except ValueError:
        return DEFAULT_IMAGE_PYRAMID_BYTES


@dataclass(frozen=True)
class TileSource:
    """Where an image's tiles come from now: ``state`` is ``"none"`` (a small
    image, read from the file), ``"building"``, ``"failed"`` (both read from
    the file meanwhile) or ``"ready"`` (from ``pyramid``)."""
    state: str
    progress: float = 0.0
    pyramid: str | None = None
    error: str = ""


# Pyramids found ready, by the path of the image's pyramid (which names the
# image's size and modification time, so a changed image is looked up anew):
# a burst of tiles opens the pyramid once, not once per tile. Likewise the
# image's HDU and shape, by its path, size and modification time.
_ready: set[str] = set()
# Builds that failed, by pyramid path: not tried again for this version of the
# image (a whole-file compressed image, a disk without room). A changed image
# has another pyramid path, and a restarted backend tries again.
_failed: dict[str, str] = {}
_layout: dict[tuple[str, int, int], tuple[int, tuple[int, int, int]]] = {}


def _hdu_and_shape(path: str) -> tuple[int, tuple[int, int, int]]:
    from astropy.io import fits  # noqa: PLC0415

    from .brick_pyramid import source_shape  # noqa: PLC0415
    from .fits_dataset import image_hdu_index  # noqa: PLC0415

    st = os.stat(path)
    key = (os.path.realpath(path), st.st_size, st.st_mtime_ns)
    if key not in _layout:
        with fits.open(path, memmap=True) as hdul:
            hdu = image_hdu_index(hdul)
        _layout[key] = (hdu, source_shape(path, hdu))
    return _layout[key]


def tile_source(path: str) -> TileSource:
    """Where the tiles of the image at ``path`` come from, starting the build of
    its pyramid when the image is large and has none (in the server process:
    the build is tracked there). Never raises: whatever stops the pyramid, the
    tiles are still read from the file."""
    try:
        return _tile_source(path)
    except Exception as exc:  # noqa: BLE001 - e.g. a pyramid directory that cannot be created
        logger.warning("image pyramid of %s unavailable: %s", path, exc)
        return TileSource("failed", 0.0, None, f"{type(exc).__name__}: {exc}")


def _tile_source(path: str) -> TileSource:
    from .brick_pyramid import default_pyramid_path  # noqa: PLC0415
    from .pyramid_builds import ensure_pyramid  # noqa: PLC0415

    hdu, (nz, ny, nx) = _hdu_and_shape(path)
    if nz != 1 or ny * nx * 4 <= image_pyramid_bytes():
        return TileSource("none")
    pyramid = str(default_pyramid_path(path, hdu))
    if pyramid in _ready and os.path.exists(pyramid):
        return TileSource("ready", 1.0, pyramid)
    if pyramid in _failed:
        return TileSource("failed", 0.0, None, _failed[pyramid])
    status = ensure_pyramid(path, hdu)
    if status.state == "ready":
        _ready.add(pyramid)
        return TileSource("ready", 1.0, pyramid)
    if status.state == "failed":
        _failed[pyramid] = status.error
        logger.warning("image pyramid of %s failed: %s", path, status.error)
    return TileSource(status.state, status.progress, None, status.error)


def read_level(pyramid: str, level: int, y0: int, y1: int, x0: int, x1: int) -> np.ndarray:
    """Pixels ``[y0:y1, x0:x1]`` of ``level`` (in that level's pixels) of an
    image pyramid. Levels beyond the pyramid's last (a viewer's tiles may be
    smaller than its bricks) are halved from the last one, which fits in one
    brick."""
    import h5py  # noqa: PLC0415

    from .brick_pyramid import _halve  # noqa: PLC0415

    with h5py.File(pyramid, "r") as h5:
        levels = int(h5.attrs["levels"])
        if level < levels:
            return np.asarray(h5[f"level{level}/data"][0, y0:y1, x0:x1], dtype=np.float32)
        top = np.asarray(h5[f"level{levels - 1}/data"][...], dtype=np.float32)
    for _ in range(level - levels + 1):
        top = _halve(top)
    return top[0, y0:y1, x0:x1]
