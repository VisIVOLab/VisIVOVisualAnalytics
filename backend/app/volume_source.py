"""A volume read where it is: an HDU of a FITS file, a field of a VisIVO
Binary Table volume (issue #83), or a variable of a PLUTO binary output
(issue #315, app/pluto_source.py), or a raw grid of the legacy VisIVO
Importer's descriptor (issue #551, app/raw_descriptor.py), or a dataset of a
generic HDF5 file (issue #552, app/hdf5_source.py). The last four are raw
fields (:class:`RawField`): samples at an offset of a file, read in place.

A VBT volume is a cube already: each field is ``nx * ny * nz`` scalars, x
fastest, one field after the other in the ``.bin`` beside its ``.head``. It is
read in place, memory-mapped at the field's offset, and takes the cube path
(brick pyramid, render sessions in core, chunked or out of core) without being
converted: where that path names an HDU, a VBT names its field's index.

A VBT is the ``.bin`` file with a ``.head`` beside it (``volume.bin`` and
``volume.bin.head``); the pyramid's signature is the ``.bin``'s, the data.
"""
from __future__ import annotations

import contextlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import numpy as np

from . import hdf5_source, raw_descriptor


# ── The byte order a VBT is read in (issue #165) ─────────────────────────────
#
# A header may declare the wrong byte order (a converter wrote big-endian data
# and a "little" header). The user can then open the table in the other order:
# the file is never rewritten, the choice is kept in a small store every
# backend process reads (app/vbt_byte_order_store.py: render sessions, worker
# pools, pyramid builds are other processes), and parse_vbt_header() hands it
# to every reader (catalogue, render sessions, ray-traced particles, field
# memory maps, pyramids, moments, previews). Pyramids and cached products key
# on it too (brick_pyramid.SourceSignature, product_cache.file_identity).

BYTE_ORDERS = ("little", "big")


def set_byte_order_override(path: str | Path, order: str | None, declared: str,
                            by: str = "") -> tuple[str, str]:
    """Read the VBT ``path`` (data, ``.head`` or field reference) in byte
    order ``order`` from now on, in every process; ``order`` equal to the
    ``declared`` one, or None, drops the choice. Returns the order read in
    before and after."""
    from .vbt_byte_order_store import set_override  # noqa: PLC0415
    return set_override(path, order, declared, by)


def byte_order_override(path: str | Path) -> str:
    """The byte order the VBT ``path`` is read in against its header, or "":
    the order an open dataset's path carries (:func:`with_order`), else the
    store's choice for the file. "" when it is the header's own."""
    frozen = ref_order(path)
    if frozen:
        from .vbt_byte_order_store import head_of  # noqa: PLC0415
        declared = parse_vbt_header(head_of(path), frozen)["declared_endian"]
        return "" if frozen == declared else frozen
    from .vbt_byte_order_store import override  # noqa: PLC0415
    return override(path)


def read_order(path: str | Path) -> str:
    """The byte order the VBT ``path`` is read in: its frozen order, else the
    store's, else the header's."""
    from .vbt_byte_order_store import head_of  # noqa: PLC0415
    return parse_vbt_header(head_of(path), ref_order(path) or None)["file_endian"]


#: An open dataset's path ends in ORDER_SEP + the order it was opened in: every
#: reader of that dataset (other processes, a task running while the choice
#: changes, another backend sharing the store) reads that order, whatever the
#: store says later. The store decides only new opens.
ORDER_SEP = "::vbt-order="


def split_order(path: str | Path) -> tuple[str, str]:
    """``(path without its order, order)``; ``(path, "")`` when it has none."""
    text = str(path)
    base, sep, order = text.rpartition(ORDER_SEP)
    if sep and order in BYTE_ORDERS:
        return base, order
    return text, ""


def ref_order(path: str | Path) -> str:
    """The order ``path`` was frozen in (:func:`with_order`), or ""."""
    return split_order(path)[1]


def with_order(path: str | Path, order: str) -> str:
    """``path`` (a field reference, a ``.head``) frozen in byte order ``order``."""
    return f"{split_order(path)[0]}{ORDER_SEP}{order}"


def parse_vbt_header(head_path: str | Path, order: str | None = None) -> dict:
    """Parse a VisIVO Binary Table .head file and return metadata dict.

    ``file_endian`` (and ``np_endian_code``) is the order the values are READ
    in: ``order`` when given (an open dataset's), else the header's, unless
    the user chose the other one for this file (:func:`set_byte_order_override`);
    ``declared_endian`` is the header's and ``byte_order_override`` says
    whether they differ. ``head_path`` may carry an order (:func:`with_order`)."""
    head_path, frozen = split_order(head_path)
    order = order or frozen or None
    with open(head_path, "r") as fh:
        lines = [ln.strip() for ln in fh.readlines() if ln.strip()]
    if len(lines) < 4:
        raise ValueError(f"VBT header too short: expected at least 4 lines, got {len(lines)}")

    scalar_str = lines[0].lower()
    if scalar_str in ("f", "float"):
        scalar_type = "float32"
        np_itemcode = "f4"
    elif scalar_str in ("d", "double"):
        scalar_type = "float64"
        np_itemcode = "f8"
    else:
        raise ValueError(f"Unknown VBT scalar type: '{lines[0]}'")

    try:
        field_count = int(lines[1])
    except ValueError:
        raise ValueError(f"Invalid field count: '{lines[1]}'")
    if field_count <= 0:
        raise ValueError(f"Field count must be positive, got {field_count}")

    tokens = lines[2].split()
    if len(tokens) == 1:
        kind = "point"
        num_rows = int(tokens[0])
        dimensions: list[int] | None = None
        spacing: list[float] | None = None
    elif len(tokens) == 7:
        kind = "volume"
        num_rows = int(tokens[0])
        nx, ny, nz = int(tokens[1]), int(tokens[2]), int(tokens[3])
        sx, sy, sz = float(tokens[4]), float(tokens[5]), float(tokens[6])
        if num_rows != nx * ny * nz:
            raise ValueError(
                f"VBT voxel count mismatch: header says {num_rows}, dims give {nx * ny * nz}"
            )
        dimensions = [nx, ny, nz]
        spacing = [sx, sy, sz]
    else:
        raise ValueError(f"Unexpected token count on row-spec line: {len(tokens)}")

    endian_str = lines[3].lower()
    if endian_str in ("l", "little"):
        file_endian = "little"
    elif endian_str in ("b", "big"):
        file_endian = "big"
    else:
        raise ValueError(f"Unknown VBT endian: '{lines[3]}'")

    # The byte order was parsed and then thrown away: the numpy code was fixed
    # to "<" by the scalar type above, so EVERY VBT was read as little-endian
    # and a big-endian one came back as garbage — NaNs and values at the
    # float32 limit, with nothing to say why. The header line exists precisely
    # to be obeyed.
    declared_endian = file_endian
    if order is not None and order not in BYTE_ORDERS:
        raise ValueError(f"byte order must be 'little' or 'big', not {order!r}")
    file_endian = order or byte_order_override(head_path) or declared_endian
    np_endian_code = ("<" if file_endian == "little" else ">") + np_itemcode

    if len(lines) < 4 + field_count:
        raise ValueError(
            f"VBT header has {len(lines)} lines but expected at least {4 + field_count}"
        )
    field_names = lines[4 : 4 + field_count]

    return {
        "kind": kind,
        "scalar_type": scalar_type,
        "np_endian_code": np_endian_code,  # numpy dtype string including endian mark
        "file_endian": file_endian,          # the order the values are read in
        "declared_endian": declared_endian,  # the order the .head declares
        "byte_order_override": file_endian != declared_endian,
        "field_count": field_count,
        "num_rows": num_rows,
        "dimensions": dimensions,
        "spacing": spacing,
        "field_names": field_names,
    }


def vbt_head(path: str | Path) -> Path:
    return Path(str(path) + ".head")


def is_vbt(path: str | Path) -> bool:
    """Whether ``path`` is the data of a VBT: a ``.head`` beside it, and not
    a FITS file (which begins with ``SIMPLE``)."""
    if not vbt_head(path).is_file():
        return False
    try:
        with open(path, "rb") as fh:
            return fh.read(6) != b"SIMPLE"
    except OSError:
        return False


def vbt_volume_header(path: str | Path, order: str | None = None) -> dict:
    header = parse_vbt_header(vbt_head(path), order)
    if header["kind"] != "volume":
        raise ValueError(f"{path} is a VBT of points, not a volume.")
    return header


def vbt_field(path: str | Path, field: int, order: str | None = None) -> np.memmap:
    """Field ``field`` of the VBT volume ``path``, memory-mapped in place as a
    (z, y, x) array of the file's own type, in byte order ``order`` (an open
    dataset's, :func:`ref_order`) or else as the header and the store say."""
    return _vbt_raw(path, field, order).memmap()


def _vbt_raw(path: str | Path, field: int, order: str | None = None) -> "RawField":
    header = vbt_volume_header(path, order or None)
    if not 0 <= field < header["field_count"]:
        raise ValueError(f"field {field} outside 0..{header['field_count'] - 1} of {path}")
    nx, ny, nz = header["dimensions"]
    dtype = np.dtype(header["np_endian_code"])
    expected = header["field_count"] * header["num_rows"] * dtype.itemsize
    size = Path(path).stat().st_size
    if size != expected:
        raise ValueError(f"{path} holds {size} bytes; its header describes {expected}.")
    return RawField(str(path), int(field), field * header["num_rows"] * dtype.itemsize,
                    dtype, (nz, ny, nx), (str(vbt_head(path)),), "vbt")


# ── A raw field: a VBT volume's field or a PLUTO variable ────────────────────
#
# Both are raw samples at an offset of a file, (z, y, x) with x fastest, read
# in place. Everything that reads a cube by its samples rather than as FITS
# (the pyramid, the moments' explicit reads, the product cache's identity, the
# size a dataset reports) asks raw_field() what the bytes are, so a third such
# format is one more resolver here, not one more branch in each of them.

@dataclass(frozen=True)
class RawField:
    """Where a field's samples are and how they read."""
    path: str                       # the data file
    index: int                      # the field in it: a VBT field, a PLUTO variable
    offset: int                     # bytes from the start of the file
    dtype: np.dtype                 # sample type, byte order included
    shape: tuple[int, int, int]     # (z, y, x), x fastest
    #: The text files whose every change is a change of how the bytes read (a
    #: VBT's .head): one edited beside the same data is another source. Not a
    #: PLUTO .out, which a running simulation appends a line to at every
    #: output: its own line is in :attr:`layout`.
    sidecars: tuple[str, ...] = ()
    kind: str = ""                  # "vbt" | "pluto" | "rawgrid" | "hdf5"

    @property
    def layout(self) -> str:
        """What the values are read as (type, byte order, offset, shape): what
        a source keyed by it must match besides the data file."""
        return f"{self.dtype.str}@{self.offset}:{'x'.join(str(n) for n in self.shape)}"

    @property
    def nbytes(self) -> int:
        return int(np.prod(self.shape)) * self.dtype.itemsize

    def memmap(self) -> np.memmap:
        return np.memmap(self.path, dtype=self.dtype, mode="r", shape=self.shape,
                         offset=self.offset)


def raw_field(path: str | Path, hdu: int | None = None) -> RawField | None:
    """The raw field ``path`` stands for: a field reference (VBT or PLUTO),
    or the data file of either with ``hdu`` its field. None for anything else
    (a FITS file); an error for a field that cannot be read as described."""
    vbt = resolve_vbt(path, hdu)
    if vbt is not None:
        return _vbt_raw(*vbt, order=ref_order(path) or None)
    from . import pluto_source  # noqa: PLC0415
    var = pluto_source.resolve(path, hdu)
    if var is not None:
        return RawField(var.data_path, var.index, var.offset, var.dtype, var.shape,
                        (), "pluto")
    if raw_descriptor.is_grid_ref(path):
        rf = raw_descriptor.grid_file(path)
        # The descriptor is a sidecar: a type or size edited beside the same
        # data is another source.
        return RawField(rf.path, rf.index, 0, rf.desc.dtype, rf.shape, (rf.desc.path,),
                        "rawgrid")
    if hdf5_source.is_volume_ref(path):
        # A dataset of a generic HDF5 file (issue #552): mapped in place, or a
        # raw copy of it when it is chunked, compressed or a hyperslab.
        placed = hdf5_source.volume_placement(path)
        return RawField(placed.path, 0, placed.offset, placed.dtype, placed.shape,
                        placed.sidecars, "hdf5")
    return None


def is_field_ref(path: str | Path) -> bool:
    """Whether ``path`` is a reference to a field (VBT or PLUTO), from the
    string alone."""
    from .pluto_source import split_ref as split_pluto  # noqa: PLC0415
    return (split_ref(path)[1] is not None or split_pluto(path)[1] is not None
            or raw_descriptor.is_grid_ref(path) or hdf5_source.is_volume_ref(path))


def field_data_file(path: str | Path) -> str:
    """The file a field reference reads (its data file), from the string
    alone; ``path`` itself for anything else."""
    from .pluto_source import split_ref as split_pluto  # noqa: PLC0415
    base, field = split_ref(path)
    if field is not None:
        return base
    base, name = split_pluto(path)
    if name is not None:
        return base
    if hdf5_source.is_volume_ref(path):
        return hdf5_source.ref_base(path)
    # A raw grid's reference begins with its descriptor: the file the data
    # roots check; the data file it lists is checked when it is opened.
    return raw_descriptor.split_grid_ref(path)[0]


def field_file(path: str | Path) -> str:
    """The file a field reference's samples are in, to open or stat: its data
    file. :func:`field_data_file` is the reference's leading path, which the
    routes check against the data roots; the two differ for a raw grid, whose
    reference begins with its descriptor."""
    if raw_descriptor.is_grid_ref(path) or hdf5_source.is_volume_ref(path):
        return raw_field(path).path
    return field_data_file(path)


def field_dataset_path(path: str | Path, allowed=None) -> str | None:
    """The reference a field reference (VBT, PLUTO or raw grid) opens by,
    checked; None when ``path`` is no field reference or names a field that is
    not there. ``allowed`` (path -> bool): the data file a raw grid's
    descriptor lists must pass it too (None, as a refused path, when it does
    not)."""
    from . import pluto_source  # noqa: PLC0415
    if raw_descriptor.is_grid_ref(path):
        try:
            return raw_descriptor.grid_dataset_path(path, allowed)
        except (OSError, ValueError):
            return None
    if hdf5_source.is_volume_ref(path):
        try:
            return hdf5_source.canonical_ref(path, "volume")
        except (OSError, ValueError, RuntimeError):
            return None
    base, field = split_ref(path)
    if field is not None:
        return vbt_dataset_path(base, field)
    base, name = pluto_source.split_ref(path)
    if name is not None:
        return pluto_source.pluto_dataset_path(base, name)
    return None


def field_listing(path: str | Path) -> tuple[list[dict], str]:
    """``(fields, name)`` for a field reference: every field of its source
    (:func:`field_siblings`, ``{name, path}``) and the name of the one
    ``path`` names; ``([], "")`` for any other path. The headset's variable
    picker reads it from reduced_info (issue #317), whichever way the dataset
    was opened."""
    from . import pluto_source  # noqa: PLC0415
    if not is_field_ref(path):
        return [], ""
    fields = field_siblings(path)
    _base, index = split_ref(path)
    if raw_descriptor.is_grid_ref(path) or hdf5_source.is_volume_ref(path):
        name = next((f["name"] for f in fields if f["path"] == path), "")
    elif index is not None:
        name = fields[index]["name"] if 0 <= index < len(fields) else ""
    else:
        name = pluto_source.split_ref(path)[1] or ""
    return fields, name


def field_siblings(path: str | Path) -> list[dict]:
    """Every field beside the one ``path`` names, ``{name, path}``: a VBT
    volume's fields, a PLUTO output's variables present on disk."""
    from . import pluto_source  # noqa: PLC0415
    base, field = split_ref(path)
    if field is not None:
        return vbt_fields(base)
    if hdf5_source.is_volume_ref(path):
        return hdf5_source.volume_siblings(path)
    if pluto_source.split_ref(path)[1] is not None:
        return pluto_source.pluto_fields(path)
    if raw_descriptor.is_grid_ref(path):
        # The descriptor's list: one dataset for each file (a time step).
        try:
            return [{"name": f["name"], "path": f["path"]}
                    for f in raw_descriptor.listing(path)]
        except (OSError, ValueError):
            return []
    return []


# ── A field as a cube dataset ────────────────────────────────────────────────
#
# A field of a VBT volume opens as a cube (/v1/datasets/open), and every tool of
# the cube viewer reads it as it reads a FITS cube: its path is a reference to
# the field, and open_fits() gives for it an HDUList built in memory around the
# field's memory map, with a linear header from the VBT's grid. Nothing is
# written: the values are the file's.

#: ``<data>.bin`` + REF_SEP + field index.
REF_SEP = "::vbt-field="


def vbt_field_ref(path: str | Path, field: int) -> str:
    """The reference to field ``field`` of the VBT ``path``; an order ``path``
    carries (:func:`with_order`) stays at the end, where it is read."""
    base, order = split_order(path)
    base = split_ref(base)[0]
    ref = f"{base}{REF_SEP}{int(field)}"
    return with_order(ref, order) if order else ref


def split_ref(path: str | Path) -> tuple[str, int | None]:
    """``(data path, field)`` of a field reference; ``(path, None)`` else.
    An order the reference carries (:func:`with_order`) is dropped: read it
    with :func:`ref_order`."""
    text = split_order(path)[0]
    base, sep, field = text.rpartition(REF_SEP)
    if sep and field.isdigit():
        return base, int(field)
    return text, None


def resolve_vbt(path: str | Path, hdu: int | None = None) -> tuple[str, int] | None:
    """The VBT data file and field ``path`` (a field reference, or the data
    of a VBT with ``hdu`` its field) stands for; None for anything else."""
    base, field = split_ref(path)
    if field is not None:
        return base, field
    if is_vbt(base):
        return base, 0 if hdu is None else int(hdu)
    return None


def vbt_dataset_path(path: str | Path, field: int | None = None) -> str | None:
    """The reference /v1/datasets/open registers for a VBT volume given by
    its data or its ``.head`` (field ``field``, the first by default); None
    when ``path`` is not a VBT volume."""
    text = str(path)
    data = text[: -len(".head")] if text.endswith(".head") else text
    if not is_vbt(data):
        return None
    try:
        header = vbt_volume_header(data)
    except (OSError, ValueError):
        return None
    field = 0 if field is None else int(field)
    if not 0 <= field < header["field_count"]:
        return None
    return vbt_field_ref(data, field)


def vbt_fields(path: str | Path) -> list[dict]:
    """Every field of the VBT volume ``path`` with the reference opening it."""
    names = vbt_volume_header(path)["field_names"]
    return [{"name": name, "path": vbt_field_ref(path, i)} for i, name in enumerate(names)]


def vbt_fits_header(path: str | Path, field: int):
    """The header a field is read with as a FITS cube: its grid as linear X,
    Y, Z axes, spaced as the VBT says, from 0 at the first voxel."""
    from astropy.io import fits  # noqa: PLC0415

    header = vbt_volume_header(path)
    nx, ny, nz = header["dimensions"]
    hdr = fits.Header()
    hdr["SIMPLE"] = True
    hdr["BITPIX"] = -32 if header["scalar_type"] == "float32" else -64
    hdr["NAXIS"] = 3
    for axis, (n, step, name) in enumerate(zip((nx, ny, nz), header["spacing"], "XYZ"), start=1):
        hdr[f"NAXIS{axis}"] = n
        hdr[f"CTYPE{axis}"] = name
        hdr[f"CRPIX{axis}"] = 1.0
        hdr[f"CRVAL{axis}"] = 0.0
        hdr[f"CDELT{axis}"] = float(step)
    hdr["OBJECT"] = header["field_names"][field]
    hdr["VBTFIELD"] = (header["field_names"][field], "field of the VisIVO Binary Table")
    return hdr


def raw_grid_fits_header(rf):
    """The header a raw grid is read with as a FITS cube: its axes as linear X,
    Y, Z, one unit a cell from 0 at the first (the format states no box), as a
    VBT volume's; the descriptor's variable, time tag and file as cards."""
    from astropy.io import fits  # noqa: PLC0415

    hdr = fits.Header()
    hdr["SIMPLE"] = True
    hdr["BITPIX"] = -32 if rf.desc.dtype.itemsize == 4 else -64
    hdr["NAXIS"] = 3
    for axis, (n, name) in enumerate(zip(rf.shape[::-1], "XYZ"), start=1):
        hdr[f"NAXIS{axis}"] = int(n)
        hdr[f"CTYPE{axis}"] = name
        hdr[f"CRPIX{axis}"] = 1.0
        hdr[f"CRVAL{axis}"] = 0.0
        hdr[f"CDELT{axis}"] = 1.0
    hdr["OBJECT"] = rf.desc.name
    hdr["RAWVAR"] = (rf.desc.name[:68], "raw grid variable")
    hdr["RAWTIME"] = (rf.time, "time tag in the descriptor")
    hdr["RAWSTEP"] = (rf.index + 1, "place in the descriptor's list")
    return hdr


def hdf5_fits_header(ref, shape):
    """The header a dataset of an HDF5 file is read with as a FITS cube: its
    axes as linear X, Y, Z, one unit a sample from 0 at the first (the file
    states no grid), its path as a card and its unit attribute as BUNIT."""
    from astropy.io import fits  # noqa: PLC0415

    hdr = fits.Header()
    hdr["SIMPLE"] = True
    hdr["BITPIX"] = -32
    hdr["NAXIS"] = 3
    for axis, (n, name) in enumerate(zip(shape[::-1], "XYZ"), start=1):
        hdr[f"NAXIS{axis}"] = int(n)
        hdr[f"CTYPE{axis}"] = name
        hdr[f"CRPIX{axis}"] = 1.0
        hdr[f"CRVAL{axis}"] = 0.0
        hdr[f"CDELT{axis}"] = 1.0
    name = hdf5_source.split_volume_ref(ref)[1] or ""
    hdr["OBJECT"] = name[-68:]
    hdr["H5DSET"] = (name[-68:], "dataset of the HDF5 file")
    unit = hdf5_source.volume_units(ref)
    if unit and unit.isascii():
        hdr["BUNIT"] = unit[:68]
    return hdr


def open_fits(path: str | Path, **kwargs):
    """``fits.open(path, **kwargs)``; for a field reference (a VBT field, a
    PLUTO variable, a raw grid), an HDUList with the field memory-mapped in place as its
    primary HDU's data (no copy)."""
    from astropy.io import fits  # noqa: PLC0415

    base, field = split_ref(path)
    if field is not None:
        hdu = fits.PrimaryHDU(data=vbt_field(base, field, ref_order(path)),
                              header=vbt_fits_header(base, field),
                              do_not_scale_image_data=True)
        return fits.HDUList([hdu])
    from . import pluto_source  # noqa: PLC0415
    if pluto_source.split_ref(path)[1] is not None:
        var = pluto_source.resolve(path)
        hdu = fits.PrimaryHDU(
            data=np.memmap(var.data_path, dtype=var.dtype, mode="r", shape=var.shape,
                           offset=var.offset),
            header=pluto_source.pluto_fits_header(var), do_not_scale_image_data=True)
        return fits.HDUList([hdu])
    if raw_descriptor.is_grid_ref(path):
        rf = raw_descriptor.grid_file(path)
        hdu = fits.PrimaryHDU(data=raw_field(path).memmap(), header=raw_grid_fits_header(rf),
                              do_not_scale_image_data=True)
        return fits.HDUList([hdu])
    if hdf5_source.is_volume_ref(path):
        raw = raw_field(path)
        hdu = fits.PrimaryHDU(data=raw.memmap(), header=hdf5_fits_header(path, raw.shape),
                              do_not_scale_image_data=True)
        return fits.HDUList([hdu])
    return fits.open(str(path), **kwargs)


def volume_shape(path: str | Path, hdu: int | None = None) -> tuple[tuple[int, ...], int]:
    """The full shape of the volume and the HDU (field, for a VBT or a PLUTO
    output) it is in; ``hdu`` None: a FITS file's first image HDU, a VBT's
    first field, a PLUTO file's own variable. Read from the header: no data
    is touched."""
    vbt = resolve_vbt(path, hdu)
    if vbt is not None:
        header = vbt_volume_header(vbt[0])
        nx, ny, nz = header["dimensions"]
        return (nz, ny, nx), vbt[1]
    if hdf5_source.is_volume_ref(path):
        # From the file's metadata: a chunked dataset is not unpacked to say its shape.
        return hdf5_source.volume_shape(path), 0
    raw = raw_field(path, hdu)
    if raw is not None:
        return raw.shape, raw.index
    from astropy.io import fits  # noqa: PLC0415

    from .fits_dataset import image_hdu_index  # noqa: PLC0415
    with fits.open(str(path), memmap=True) as hdul:
        index = image_hdu_index(hdul) if hdu is None else int(hdu)
        # From the header: touching .data would decompress a whole
        # tile-compressed cube here.
        return tuple(int(n) for n in (hdul[index].shape or ())), index


def volume_cube_shape(path: str | Path, full, hdu: int | None = None) -> tuple[int, int, int]:
    """The (z, y, x) shape :func:`volume_data` yields for a volume whose
    ``volume_shape`` is ``full``: a raw field (VBT, PLUTO) is already a cube,
    a FITS array is cut to ``fits_dataset.cube_axes`` (an image one plane
    deep)."""
    if raw_field(path, hdu) is not None:
        return tuple(int(n) for n in full)  # type: ignore[return-value]
    from .fits_dataset import cube_shape  # noqa: PLC0415

    dims = cube_shape(full)
    return (1, *dims) if len(dims) == 2 else dims  # type: ignore[return-value]


@contextlib.contextmanager
def volume_data(path: str | Path, hdu: int | None = None) -> Iterator[np.ndarray]:
    """The volume's data, memory-mapped, as the cube ``fits_dataset.cube_axes``
    picks: (z, y, x), an image one plane deep. Read-only, and possibly of the
    file's own type: copy what is to be changed or kept."""
    raw = raw_field(path, hdu)
    if raw is not None:
        # Not closed on the way out: an array read from it without a copy (the
        # file's own type) is still a view of the map, and numpy closes the
        # map when the last view is gone.
        yield raw.memmap()
        return
    from astropy.io import fits  # noqa: PLC0415

    from .fits_dataset import cube_index, image_hdu_index  # noqa: PLC0415
    with fits.open(str(path), memmap=True) as hdul:
        data = hdul[image_hdu_index(hdul) if hdu is None else int(hdu)].data
        data = data[cube_index(data.shape)]   # a view: nothing is read
        yield data[np.newaxis] if data.ndim == 2 else data
