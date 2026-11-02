"""A volume read where it is: an HDU of a FITS file, or a field of a VisIVO
Binary Table volume (issue #83).

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
from pathlib import Path
from typing import Iterator

import numpy as np


def parse_vbt_header(head_path: str | Path) -> dict:
    """Parse a VisIVO Binary Table .head file and return metadata dict."""
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
        "file_endian": file_endian,
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


def vbt_volume_header(path: str | Path) -> dict:
    header = parse_vbt_header(vbt_head(path))
    if header["kind"] != "volume":
        raise ValueError(f"{path} is a VBT of points, not a volume.")
    return header


def vbt_field(path: str | Path, field: int) -> np.memmap:
    """Field ``field`` of the VBT volume ``path``, memory-mapped in place as a
    (z, y, x) array of the file's own type and byte order."""
    header = vbt_volume_header(path)
    if not 0 <= field < header["field_count"]:
        raise ValueError(f"field {field} outside 0..{header['field_count'] - 1} of {path}")
    nx, ny, nz = header["dimensions"]
    dtype = np.dtype(header["np_endian_code"])
    expected = header["field_count"] * header["num_rows"] * dtype.itemsize
    size = Path(path).stat().st_size
    if size != expected:
        raise ValueError(f"{path} holds {size} bytes; its header describes {expected}.")
    return np.memmap(path, dtype=dtype, mode="r", shape=(nz, ny, nx),
                     offset=field * header["num_rows"] * dtype.itemsize)


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
    return f"{path}{REF_SEP}{int(field)}"


def split_ref(path: str | Path) -> tuple[str, int | None]:
    """``(data path, field)`` of a field reference; ``(path, None)`` else."""
    text = str(path)
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
    if is_vbt(path):
        return str(path), 0 if hdu is None else int(hdu)
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


def open_fits(path: str | Path, **kwargs):
    """``fits.open(path, **kwargs)``; for a field reference, an HDUList with
    the field memory-mapped in place as its primary HDU's data (no copy)."""
    from astropy.io import fits  # noqa: PLC0415

    base, field = split_ref(path)
    if field is None:
        return fits.open(str(path), **kwargs)
    hdu = fits.PrimaryHDU(data=vbt_field(base, field), header=vbt_fits_header(base, field),
                          do_not_scale_image_data=True)
    return fits.HDUList([hdu])


def volume_shape(path: str | Path, hdu: int | None = None) -> tuple[tuple[int, ...], int]:
    """The full shape of the volume and the HDU (field, for a VBT) it is in;
    ``hdu`` None: a FITS file's first image HDU, a VBT's first field. Read
    from the header: no data is touched."""
    vbt = resolve_vbt(path, hdu)
    if vbt is not None:
        header = vbt_volume_header(vbt[0])
        nx, ny, nz = header["dimensions"]
        return (nz, ny, nx), vbt[1]
    from astropy.io import fits  # noqa: PLC0415

    from .fits_dataset import image_hdu_index  # noqa: PLC0415
    with fits.open(str(path), memmap=True) as hdul:
        index = image_hdu_index(hdul) if hdu is None else int(hdu)
        # From the header: touching .data would decompress a whole
        # tile-compressed cube here.
        return tuple(int(n) for n in (hdul[index].shape or ())), index


@contextlib.contextmanager
def volume_data(path: str | Path, hdu: int | None = None) -> Iterator[np.ndarray]:
    """The volume's data, memory-mapped, with its leading axes beyond three
    reduced to their first plane: (z, y, x). Read-only, and possibly of the
    file's own type: copy what is to be changed or kept."""
    vbt = resolve_vbt(path, hdu)
    if vbt is not None:
        # Not closed on the way out: an array read from it without a copy (the
        # file's own type) is still a view of the map, and numpy closes the
        # map when the last view is gone.
        yield vbt_field(*vbt)
        return
    from astropy.io import fits  # noqa: PLC0415

    from .fits_dataset import image_hdu_index  # noqa: PLC0415
    with fits.open(str(path), memmap=True) as hdul:
        data = hdul[image_hdu_index(hdul) if hdu is None else int(hdu)].data
        while data.ndim > 3:
            data = data[0]
        yield data
