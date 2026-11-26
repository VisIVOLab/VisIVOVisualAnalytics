from __future__ import annotations

import hashlib
import json
import logging
import os
import threading
from collections import OrderedDict
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

logger = logging.getLogger("visivo.cache")


def file_identity(path: str | os.PathLike) -> str:
    """The version of the file ``path`` names, as a short string for a cache key.

    A product cached for a path must not be served once the file has been
    replaced or rewritten (a re-reduced cube saved over the old one, a FITS
    edited in place): the key therefore carries device, inode, size and the
    modification and change times of the file. ``ctime`` is there for a
    rewrite that restores ``mtime`` (``touch -r``, some copy tools); a chmod
    or a rename also moves it, which costs only a recomputation. A VBT field
    reference (``data::field``) is the data file, and a ``.head`` beside the
    data is part of its version: its values are read as the header says. A
    PLUTO variable reference (``data::pluto-var=rho``) is its data file, its
    ``grid.out`` and what its line of the ``.out`` says it is (not the
    ``.out``'s own version: a running simulation appends to it).
    Empty for a path that cannot be stat'ed (an in-memory or remote source),
    which keys as before.
    """
    from .pluto_source import GRID_NAME  # noqa: PLC0415
    from .pluto_source import split_ref as split_pluto  # noqa: PLC0415
    from .point_table import component_format, split_component  # noqa: PLC0415
    from .volume_source import (byte_order_override, field_data_file,  # noqa: PLC0415
                                raw_field, vbt_head)

    if component_format(path) == "ramses":
        # A RAMSES family (issue #549): the output's info, header and
        # descriptors and its particle files; the mesh files change nothing
        # a particle table reads. The gas cells (issue #550) are the mesh
        # and its hydro files.
        from .ramses_output import CELLS, open_output, split_ref  # noqa: PLC0415
        kinds = ("amr", "hydro") if split_ref(path)[1] == CELLS else ("part",)
        try:
            stats = [os.stat(f) for f in open_output(split_component(path)[0]).files(kinds)]
        except (OSError, ValueError):
            return ""
        return "|".join(f"{st.st_dev}.{st.st_ino}.{st.st_size}.{st.st_mtime_ns}.{st.st_ctime_ns}"
                        for st in stats)

    if component_format(path) == "yt":
        # The gas cells of a mesh of another code read through yt (issue
        # #554): every file of the mesh (its siblings and levels included).
        from .yt_output import files  # noqa: PLC0415
        try:
            stats = [os.stat(f) for f in files(split_component(path)[0])]
        except OSError:
            return ""
        return "|".join(f"{st.st_dev}.{st.st_ino}.{st.st_size}.{st.st_mtime_ns}.{st.st_ctime_ns}"
                        for st in stats)

    if component_format(path) == "gadget":
        # A snapshot's particle type (issue #31): every file of the snapshot,
        # and the run's parameter file and the environment's units, which
        # change what its derived columns are.
        from .gadget_snapshot import UNITS_ENV, parameter_file, snapshot_files  # noqa: PLC0415
        try:
            files = snapshot_files(split_component(path)[0])
            params = parameter_file(files[0])
            stats = [os.stat(f) for f in [*files, *([params] if params else [])]]
        except (OSError, ValueError):
            return ""
        units = os.environ.get(UNITS_ENV, "")
        return "|".join([f"{st.st_dev}.{st.st_ino}.{st.st_size}.{st.st_mtime_ns}.{st.st_ctime_ns}"
                         for st in stats] + ([f"units={units}"] if units else []))

    from . import raw_descriptor  # noqa: PLC0415
    if raw_descriptor.is_grid_ref(path) or raw_descriptor.is_points_ref(path):
        # A file of a legacy-importer descriptor (issue #551): its data file
        # and its descriptor, which says how the data read.
        files = raw_descriptor.identity_files(path)
        if not files:
            return ""
        parts = []
        for candidate in files:
            try:
                st = os.stat(candidate)
            except OSError:
                return ""
            parts.append(f"{st.st_dev}.{st.st_ino}.{st.st_size}."
                         f"{st.st_mtime_ns}.{st.st_ctime_ns}")
        return "|".join(parts + [f"ref={path}"])

    base = field_data_file(path)
    sidecars = [vbt_head(base)]
    layout = ""
    if split_pluto(path)[1] is not None:
        sidecars = [os.path.join(os.path.dirname(base), GRID_NAME)]
        try:
            layout = raw_field(path).layout
        except (OSError, ValueError):
            return ""
    parts = []
    for candidate, required in ((base, True), *((side, False) for side in sidecars)):
        try:
            st = os.stat(candidate)
        except OSError:
            if required:
                return ""
            continue
        parts.append(f"{st.st_dev}.{st.st_ino}.{st.st_size}."
                     f"{st.st_mtime_ns}.{st.st_ctime_ns}")
    # A VBT read against its header's byte order (issue #165) has other
    # values: another version, whichever of its names (data, .head, field
    # reference) the key was made from.
    if layout:
        parts.append(f"layout={layout}")
    order = byte_order_override(path) if not layout else ""
    if order:
        parts.append(f"byte-order={order}")
    return "|".join(parts)


def dataset_identity(entry: dict[str, Any]) -> str:
    """:func:`file_identity` of every file a registered dataset reads: its
    ``path``, the ``paths`` of a catalogue opened from several files and the
    ``head_path`` and ``bin_path`` of a VBT opened by /v1/vbt/open. For a
    cache keyed by dataset id rather than path, which otherwise keeps serving
    what an earlier version of the file gave."""
    names = [entry.get("path"), *(entry.get("paths") or []),
             entry.get("head_path"), entry.get("bin_path")]
    return "|".join(file_identity(str(name)) for name in names if name)


@dataclass
class CachedProductRecord:
    key: str
    operation: str
    dataset_path: str
    parameter_hash: str
    payload: dict[str, Any]
    scientific_metadata: dict[str, Any]
    provenance_metadata: dict[str, Any]
    created_at: str


class ProductCache:
    def __init__(self, max_entries: int = 32) -> None:
        self._max_entries = max(1, max_entries)
        self._entries: OrderedDict[str, CachedProductRecord] = OrderedDict()
        self._lock = threading.Lock()

    @staticmethod
    def make_key(dataset_path: str, operation: str, params: dict[str, Any]) -> tuple[str, str]:
        """``(key, parameter hash)`` of a product. The key carries the file's
        version (:func:`file_identity`), so a file replaced on disk is a miss;
        it is taken before the computation, so a result of a file changed
        while it was computed is stored under a version no request asks for.
        """
        encoded = json.dumps(params, sort_keys=True, separators=(",", ":"), default=str)
        param_hash = hashlib.sha256(encoded.encode("utf-8")).hexdigest()[:16]
        version = hashlib.sha256(file_identity(dataset_path).encode("utf-8")).hexdigest()[:16]
        return f"{operation}:{dataset_path}:{version}:{param_hash}", param_hash

    def get(self, key: str) -> CachedProductRecord | None:
        with self._lock:
            record = self._entries.get(key)
            if record is None:
                logger.info("[cache] miss key=%s", key)
                return None
            self._entries.move_to_end(key)
            logger.info("[cache] hit key=%s op=%s", key, record.operation)
            return record

    def put(
        self,
        *,
        key: str,
        operation: str,
        dataset_path: str,
        parameter_hash: str,
        payload: dict[str, Any],
        scientific_metadata: dict[str, Any],
        provenance_metadata: dict[str, Any],
    ) -> CachedProductRecord:
        record = CachedProductRecord(
            key=key,
            operation=operation,
            dataset_path=dataset_path,
            parameter_hash=parameter_hash,
            payload=payload,
            scientific_metadata=scientific_metadata,
            provenance_metadata=provenance_metadata,
            created_at=datetime.now(tz=timezone.utc).isoformat(),
        )
        with self._lock:
            self._entries[key] = record
            self._entries.move_to_end(key)
            while len(self._entries) > self._max_entries:
                self._entries.popitem(last=False)
        logger.info("[cache] insert key=%s op=%s", key, operation)
        return record

    def stats(self) -> dict[str, Any]:
        with self._lock:
            return {
                "entries": len(self._entries),
                "max_entries": self._max_entries,
            }


PRODUCT_CACHE = ProductCache(max_entries=int(os.environ.get("VISIVO_PRODUCT_CACHE_ENTRIES", "32")))
