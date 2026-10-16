#!/usr/bin/env python
"""Turn a binary source mask into the labelled mask ``lasso_vs_sofia.py`` reads.

The second reference of the paper's lasso validation is the mask the published
science of HCG 44 was done with (Serra et al. 2013), which is binary: 1 on
emission, 0 elsewhere. ``lasso_vs_sofia.py`` expects a SoFiA-style mask with one
integer id per source, so this labels the 6-connected components (face
adjacency in z, y, x; scipy's default 3-D structure) in raster order and writes
them as int32 under the input's own header. Component ids are therefore
deterministic, which is what the paper's row mapping (published components
13, 1, 8, 3, 7 for rows A-E) relies on.

Run:  backend/.venv/bin/python validation/label_published_mask.py \
          /path/to/hcg44_cube_3.0_binmask.fits validation/sofia_out/serra2013_hcg44_mask.fits
      backend/.venv/bin/python validation/lasso_vs_sofia.py validation/sofia_out/serra2013_hcg44_mask.fits
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from astropy.io import fits
from scipy import ndimage

if len(sys.argv) != 3:
    sys.exit(__doc__)
src, dst = Path(sys.argv[1]), Path(sys.argv[2])
data, header = fits.getdata(src, header=True)
data = np.squeeze(data)
if data.ndim != 3:
    sys.exit(f"{src}: expected a 3-D mask, got shape {data.shape}")
labels, n = ndimage.label(data > 0)          # 6-connected, raster order: ids are deterministic
header["BUNIT"] = "source id"
header["HISTORY"] = f"label_published_mask.py: {n} 6-connected components of {src.name}"
dst.parent.mkdir(parents=True, exist_ok=True)
fits.writeto(dst, labels.astype(np.int32, copy=False), header, overwrite=True)
sizes = np.bincount(labels.ravel())[1:]
print(f"{n} components; largest {np.sort(sizes)[::-1][:5].tolist()} voxels; written {dst}")
