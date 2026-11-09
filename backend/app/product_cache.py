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
    data is part of its version: its values are read as the header says.
    Empty for a path that cannot be stat'ed (an in-memory or remote source),
    which keys as before.
    """
    from .volume_source import split_ref, vbt_head  # noqa: PLC0415

    base, _field = split_ref(path)
    parts = []
    for candidate, required in ((base, True), (vbt_head(base), False)):
        try:
            st = os.stat(candidate)
        except OSError:
            if required:
                return ""
            continue
        parts.append(f"{st.st_dev}.{st.st_ino}.{st.st_size}."
                     f"{st.st_mtime_ns}.{st.st_ctime_ns}")
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
