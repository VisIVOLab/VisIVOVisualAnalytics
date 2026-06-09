"""Background FITS cube reader — Phase 4 Path 1 (decouple I/O from render).

The default ``RenderSession`` initialisation loads the FITS file
synchronously on the VTK executor thread: ``fits.open(memmap=True)``
followed by ``np.asarray(data, dtype=float32)`` that materialises the
ENTIRE cube into RAM before VTK can start.

For a 22 GB WALLABY-DR1 sized cube on /mnt/beegfs that single call
blocks for **2-3 minutes**. While it blocks:

  * the executor thread is busy → no rendering happens, the first
    frame stays away
  * the GIL is held → aiortc's asyncio loop on the main thread loses
    ICE keepalive cadence and the negotiation drifts
  * the user stares at a "Connecting…" banner for minutes

This module flips the architecture: a SEPARATE Python process owns
the FITS read, writes z-slab chunks into a shared-memory buffer, and
posts progress events on a multiprocessing.Queue. The render process
maps the same shared memory as a NumPy view and wraps it with
``vtkImageData`` via ``numpy_to_vtk(deep=False)`` — VTK sees the
buffer directly, no copy. After each chunk lands, the render process
calls ``vtkImageData.Modified()`` and the next frame reflects the
freshly-loaded slab.

Net effect:
  * first VTK render happens after the FIRST chunk (~5-10 s on 22 GB)
  * the cube progressively fills in as further chunks arrive
  * the asyncio loop stays responsive — the reader process has its
    own GIL, doesn't block aiortc
  * shared memory keeps the cross-process cost at one ``mmap``
    syscall, NO marshalling of GB of float32

Cleanup is critical: a stale ``/dev/shm/visivo-cube-*`` segment from
a crashed reader would leak the entire cube. ``CubeReader.close()``
both terminates the reader and ``unlink``s the shared memory; the
context-manager protocol in ``__enter__/__exit__`` makes that
exception-safe.
"""
from __future__ import annotations

import logging
import time
from multiprocessing import Process, Queue, get_context
from multiprocessing.shared_memory import SharedMemory
from typing import Optional, Tuple

import numpy as np

logger = logging.getLogger("visivo.render.reader")


def _reader_main(
    path: str,
    shm_name: str,
    shape: Tuple[int, int, int],
    chunks: list[Tuple[int, int]],
    progress_q: "Queue",
) -> None:
    """Reader-process entry point.

    Runs in a freshly-spawned Python interpreter with ``spawn`` start
    method (set by ``CubeReader.__init__``); imports happen lazily
    here so the reader doesn't pay the cost of importing every
    backend module that the FastAPI parent already has loaded.

    Each iteration:
      1. ``data[z_start:z_end]`` triggers astropy's memmap to page
         in only that z-slab from disk
      2. ``np.asarray(..., dtype=float32)`` coerces to the layout
         the render side expects (most FITS are float32 already so
         this is a no-op in practice — but it normalises if not)
      3. NaN sanitisation in-place (we use 0.0 as the sentinel; the
         render side will re-clamp later via vmin/vmax once the full
         cube has been seen)
      4. ``cube[z_start:z_end] = slab`` writes into shared memory —
         this is the actual cross-process handoff, ~50 ms for a
         ~500 MB slab on /mnt/beegfs

    Progress events posted to ``progress_q``:

        {"event": "start",    "shape": (nz, ny, nx)}
        {"event": "chunk",    "z_range": (z0, z1), "vmin": ..., "vmax": ..., "elapsed_ms": ...}
        {"event": "complete", "vmin": ..., "vmax": ...}
        {"event": "error",    "message": "..."}
    """
    try:
        from astropy.io import fits  # noqa: PLC0415 — lazy in subprocess
    except ImportError as e:
        progress_q.put({"event": "error", "message": f"astropy import failed: {e}"})
        return

    try:
        shm = SharedMemory(name=shm_name)
        cube = np.ndarray(shape, dtype=np.float32, buffer=shm.buf)
        progress_q.put({"event": "start", "shape": shape})

        global_vmin = float("inf")
        global_vmax = float("-inf")

        with fits.open(path, memmap=True) as hdul:
            data = hdul[0].data
            # Strip degenerate axes (FITS Stokes/etc).
            while data.ndim > 3 and data.shape[0] == 1:
                data = data[0]
            if data.ndim != 3:
                progress_q.put({
                    "event": "error",
                    "message": f"FITS data has {data.ndim} dims after squeeze",
                })
                return
            # data is typically (nz, ny, nx) — matches our ``shape`` arg.
            if data.shape != shape:
                progress_q.put({
                    "event": "error",
                    "message": f"FITS shape {data.shape} != expected {shape}",
                })
                return

            for z0, z1 in chunks:
                t0 = time.perf_counter()
                slab = np.asarray(data[z0:z1], dtype=np.float32)
                # NaN → 0.0 in-place. The render side will re-mask to
                # vmin (its own sanitisation) once it sees a chunk
                # with valid data; using 0 here keeps the cube
                # well-defined for partial renders.
                np.nan_to_num(slab, nan=0.0, posinf=0.0, neginf=0.0, copy=False)
                cube[z0:z1] = slab
                # Track running vmin/vmax — useful for the render side
                # to bootstrap its LUT range without waiting for the
                # whole cube to land.
                if slab.size:
                    chunk_vmin = float(slab.min())
                    chunk_vmax = float(slab.max())
                    if chunk_vmin < global_vmin:
                        global_vmin = chunk_vmin
                    if chunk_vmax > global_vmax:
                        global_vmax = chunk_vmax
                elapsed_ms = (time.perf_counter() - t0) * 1000.0
                progress_q.put({
                    "event": "chunk",
                    "z_range": (z0, z1),
                    "vmin": chunk_vmin,
                    "vmax": chunk_vmax,
                    "elapsed_ms": elapsed_ms,
                })

        progress_q.put({
            "event": "complete",
            "vmin": global_vmin if global_vmin != float("inf") else 0.0,
            "vmax": global_vmax if global_vmax != float("-inf") else 0.0,
        })
    except Exception as e:
        progress_q.put({"event": "error", "message": f"{type(e).__name__}: {e}"})
    finally:
        try:
            shm.close()
        except Exception:
            pass


class CubeReader:
    """Background FITS reader backed by a shared-memory cube buffer.

    Usage from ``RenderSession``::

        reader = CubeReader(path, shape=(nz, ny, nx))
        reader.start()
        # cube is reader.cube — a numpy view into shared memory
        # build vtkImageData with numpy_to_vtk(reader.cube.ravel(), deep=False)
        # in your render loop:
        for ev in reader.poll_progress():
            if ev["event"] == "chunk":
                self._image_data.Modified()
                self._vmin, self._vmax = ev["vmin"], ev["vmax"]

    Memory: ``shape[0] * shape[1] * shape[2] * 4`` bytes of POSIX
    shared memory are allocated on construction; freed on
    ``close()`` / ``__exit__``.
    """

    # Use ``spawn`` for CUDA-safety even though we don't initialise
    # CUDA in the reader — better to be safe if downstream consumers
    # want to add GPU-side decompression later.
    _MP_CTX = get_context("spawn")

    def __init__(
        self,
        path: str,
        shape: Tuple[int, int, int],
        chunk_size_z: Optional[int] = None,
    ) -> None:
        """
        Args:
            path:         FITS file path (must be readable by the worker
                          process — typically a /mnt/beegfs/... path that
                          works in the parent already).
            shape:        Cube dimensions as (nz, ny, nx). MUST match
                          the on-disk cube shape after degenerate axes
                          are squeezed; reader will error otherwise.
            chunk_size_z: How many z-slices per chunk. ``None`` ⇒ pick a
                          schedule of ~32 chunks (good trade-off between
                          progress granularity and per-chunk overhead).
                          Override for tiny / huge cubes.
        """
        self.path = path
        self.shape = shape
        nz, ny, nx = shape
        if chunk_size_z is None:
            # Aim for ~32 chunks: enough progress events to feel
            # responsive, few enough to keep per-chunk overhead low.
            chunk_size_z = max(1, nz // 32)
        self.chunks = [(z, min(z + chunk_size_z, nz))
                       for z in range(0, nz, chunk_size_z)]
        nbytes = int(np.prod(shape)) * 4  # float32 = 4 bytes/voxel
        self.shm = SharedMemory(create=True, size=nbytes)
        self.cube = np.ndarray(shape, dtype=np.float32, buffer=self.shm.buf)
        # Zero-fill so a partially-loaded cube doesn't show garbage
        # in the unloaded z-slabs.
        self.cube.fill(0.0)
        self.progress_q: "Queue" = self._MP_CTX.Queue()
        self._process: Optional[Process] = None
        logger.info(
            "[reader] CubeReader allocated %.2f MB shared mem (shm=%s) "
            "for shape=%s in %d chunks",
            nbytes / (1024 * 1024), self.shm.name, shape, len(self.chunks),
        )

    def start(self) -> None:
        """Spawn the reader process. Non-blocking — progress on
        ``poll_progress()``."""
        if self._process is not None:
            return
        self._process = self._MP_CTX.Process(
            target=_reader_main,
            args=(self.path, self.shm.name, self.shape, self.chunks,
                  self.progress_q),
            daemon=True,
            name=f"visivo-cube-reader-{self.shm.name[-8:]}",
        )
        self._process.start()
        logger.info("[reader] reader process pid=%d started", self._process.pid)

    def poll_progress(self) -> list[dict]:
        """Drain pending progress events. Non-blocking; safe to call
        every render frame."""
        events: list[dict] = []
        while True:
            try:
                events.append(self.progress_q.get_nowait())
            except Exception:
                break  # Queue empty
        return events

    def is_alive(self) -> bool:
        return self._process is not None and self._process.is_alive()

    def close(self) -> None:
        """Terminate the reader, release shared memory. Idempotent."""
        if self._process is not None and self._process.is_alive():
            self._process.terminate()
            self._process.join(timeout=5.0)
        self._process = None
        try:
            # ``close`` on this side; ``unlink`` removes the segment
            # from /dev/shm so it doesn't leak.
            self.shm.close()
            self.shm.unlink()
        except Exception as e:
            logger.warning("[reader] shm cleanup failed: %s", e)

    def __enter__(self) -> "CubeReader":
        self.start()
        return self

    def __exit__(self, *exc) -> None:
        self.close()
