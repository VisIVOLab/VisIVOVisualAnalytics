# Default 3-D threshold from the noise (issue #93)

| Cube | Reference | Rule | Threshold | k (σ) | Noise voxels | Purity | Flux kept |
|---|---|---|---|---|---|---|---|
| HGC 44 | SoFiA-2 | p99 (before #93) | 0.000527 | 2.34 | 747,391 | 0.03 | 90.3 % |
| HGC 44 | SoFiA-2 | median + 3σ | 0.000675 | 3.00 | 107,994 | 0.17 | 84.2 % |
| HGC 44 | SoFiA-2 | median + 4σ | 0.0009 | 4.00 | 2,772 | 0.86 | 75.4 % |
| HGC 44 | SoFiA-2 | ~100 noise voxels (#93) | 0.00108 | 4.79 | 90 | 0.99 | 69.1 % |
| HGC 44 | Serra+2013 | p99 (before #93) | 0.000527 | 2.34 | 743,865 | 0.04 | 78.4 % |
| HGC 44 | Serra+2013 | median + 3σ | 0.000675 | 3.00 | 106,898 | 0.18 | 70.9 % |
| HGC 44 | Serra+2013 | median + 4σ | 0.0009 | 4.00 | 2,755 | 0.86 | 62.2 % |
| HGC 44 | Serra+2013 | ~100 noise voxels (#93) | 0.00108 | 4.79 | 84 | 0.99 | 57.0 % |
| WALLABY | SoFiA-2 | p99 (before #93) | 0.00577 | 2.87 | 9,947 | 0.83 | 78.2 % |
| WALLABY | SoFiA-2 | median + 3σ | 0.00603 | 3.00 | 6,503 | 0.88 | 77.1 % |
| WALLABY | SoFiA-2 | median + 4σ | 0.00802 | 4.00 | 161 | 1.00 | 70.3 % |
| WALLABY | SoFiA-2 | ~100 noise voxels (#93) | 0.00835 | 4.17 | 82 | 1.00 | 69.3 % |
| 3D_Ape-DR1_200426041_AP_B021_HI_image_cube0.fits | none | p99 (before #93) | 0.00454 | 2.40 | 5,238,679 above, 5,237,425 mirrored below | – | – |
| 3D_Ape-DR1_200426041_AP_B021_HI_image_cube0.fits | none | median + 3σ | 0.00566 | 3.00 | 1,292,099 above, 1,290,129 mirrored below | – | – |
| 3D_Ape-DR1_200426041_AP_B021_HI_image_cube0.fits | none | median + 4σ | 0.00755 | 4.00 | 154,147 above, 153,148 mirrored below | – | – |
| 3D_Ape-DR1_200426041_AP_B021_HI_image_cube0.fits | none | ~100 noise voxels (#93) | 0.0139 | 7.35 | 118 above, 115 mirrored below | – | – |
