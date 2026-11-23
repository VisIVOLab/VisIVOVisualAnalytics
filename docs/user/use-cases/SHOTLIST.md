---
orphan: true
---

# Use-case screenshots: shot list

The screenshots the use-case pages expect, for capture in the real
application for 1.2.0. Each file goes to `docs/user/use-cases/img/` under the
name given. Capture the window only (no desktop, no other windows), on macOS
in the light theme, at 2× (Retina) and the window size given. Crop nothing
inside the window unless the row says so. Where a row says to highlight
something, the highlight must come from the application itself (a selection,
a hover, an armed tool), never a drawn overlay.

Demo files are in the demo data folder (`DemoData/`); the PLUTO density is
`STEP62/rho.0000.flt` with its `flt.out` and `grid.out`. Unless a row says
otherwise, use the local backend.

## 01-first-steps.md

| File | What must be on screen |
|---|---|
| `01-first-steps-1.png` | The *Open Dataset* file browser (*File ▸ Open…*), title naming *Backend: Local*, listing `DemoData/` with `WALLABY.fits` selected; the *Preview* shows its FITS header (NAXIS1 196, NAXIS2 280, NAXIS3 109, CTYPE3 VOPT visible). *File type* on *All supported files*. Dialog at its default size over a 1600 × 1000 main window; capture the dialog only. |
| `01-first-steps-2.png` | Cube window, 1600 × 1000, `WALLABY.fits` at full resolution. *Two panes*: left *3D View* (Volume, *Composite*, default colour map), right *2D Slice* at a channel through the middle of the galaxy's emission (about channel 55). Camera: default view turned about 30° about the vertical axis so the cutting plane is seen at an angle. Left dock with *Session Data* and *DISPLAY, 3D* visible; Inspector open on *Properties*. Status bar showing *Full resolution*. |
| `01-first-steps-3.png` | Same window and layout, *Tools ▸ Spectral ▸ Extract Spectrum* armed: cross-hair on the galaxy's brighter side in the *2D Slice*, the third pane (layout grown to *Four panes* or a *Two panes* layout with the spectrum replacing the 3-D view, whichever the tool does) showing the live spectrum with its header *pixel (x, y) · live*. The double-horned profile must be visible. |
| `01-first-steps-4.png` | Same window after pinning: *Session Data* with the saved spectrum row selected (*Spectrum 1* or similar), the Inspector on *Parameters* showing the pixel it was probed at. The spectrum visible in a pane. Crop to the left dock, the spectrum pane and the Inspector if the full window is too wide to read at page width. |

## 02-find-and-measure-a-source.md

| File | What must be on screen |
|---|---|
| `02-find-and-measure-a-source-1.png` | The *SoFiA-2 Source Finding* dialog on `hgc44.fits` (full resolution loaded) after the suggestion arrived: *Data type* and *Line widths* filled in with their reasons, the measurements block (noise σ, beam in pixels, channel width), and the parameter table with *Value*, *Status*, *SoFiA default* and *Why* columns, a few bold (non-default) rows. No run yet. Dialog at its default size; capture the dialog only. Needs SoFiA-2 2.7 on the backend for the later steps, not for this shot. |
| `02-find-and-measure-a-source-2.png` | Cube window, 1600 × 1000, `hgc44.fits` at full resolution, *Two panes*: *3D View* (left) and *2D Slice* (right). *Kinematic Lasso (click to select)* on, NGC 3187 selected by a double-click: green selection surface in the 3-D view, its outline on the slice at a channel crossing the galaxy. Camera rotated so NGC 3187 is not behind NGC 3190. Inspector on *Parameters* showing *Reach*, *Velocity coupling*, *Threshold*, *Smoothing*, the voxel count (*N voxels selected*) and the per-channel profile. |
| `02-find-and-measure-a-source-3.png` | Same window after *Kinematic Model Overlay… ▸ Fit from the lasso selection* on that selection (dialog closed): *Session Data* showing the new rows (velocity field, model, residual, *Integrated spectrum (selection)*, *Source properties: … Jy km/s*), the *Source properties* row selected, the Inspector showing *Flux*, *w20 / w50*, *V_sys (w20 midpoint)*, *PA (moment 0)* and the inclination. The integrated spectrum in a pane. |

## 03-moments-and-pv.md

| File | What must be on screen |
|---|---|
| `03-moments-and-pv-1.png` | The *Moment Map Settings* dialog on `WALLABY.fits`: *Moment order* on *Moment 0: Integrated intensity (M0)*, a *Channel range* restricted to the galaxy's channels (not the full cube), *Enable threshold mask* on, *Auto threshold (≈ 3σ from MAD)* chosen. Capture the dialog only, default size. |
| `03-moments-and-pv-2.png` | Cube window, 1600 × 1000, *Four panes*: *3D View* top left, then the M0, M1 and M2 maps of `WALLABY.fits` (each from *Session Data*). Default colour maps (Inferno, CoolWarm, Viridis). The galaxy's rotation gradient clearly visible in M1. Left dock *Session Data* showing the three moment rows. |
| `03-moments-and-pv-3.png` | Cube window, 1600 × 1000, *Two panes*: left the M1 map with a two-vertex PV path drawn along the kinematic major axis (handles visible, *Extract PV Diagram…* still on), right the *PV Diagram* pane showing an S-shaped rotation curve, with the footer (*Width*, *Smooth curve*, *New path*, *Save as FITS…*, *Save as PNG…*) and the colour bar visible. |

## 04-regions.md

Before the shots, save the `hcg44.reg` file of the page next to `hgc44.fits`.

| File | What must be on screen |
|---|---|
| `04-regions-1.png` | Cube window, 1600 × 1000, `hgc44.fits` at full resolution, *Two panes*: *2D Slice* left at a channel with HI in NGC 3190, the region's mean spectrum right. A circle region drawn on a galaxy, selected (thicker outline, white handles). |
| `04-regions-2.png` | The *Region Properties* dialog next to the slice: *Frame* on *ICRS, sexagesimal*, centre RA `10:18:05.6` Dec `+21:49:56`, radius 90 arcsec, after *Apply*; the circle visible on NGC 3190 in the slice behind it. Capture the window with the dialog placed beside the slice so both read. |
| `04-regions-3.png` | After *Import Region (CRTF/DS9)…* of `hcg44.reg`: the slice with the imported circle, ellipse (cyan), annulus and point, their `text` labels drawn, and the non-modal *Region Import: hcg44.reg* window beside the viewer listing the `panda` line with its *Line*, *Text* and *Reason*. The status bar message with the counts visible. |
| `04-regions-4.png` | *3D View* of `hgc44.fits` with the imported regions drawn on the cutting plane (needs PR #353 merged). Camera rotated about 30° so the plane is seen at an angle; *Cutting plane* opacity around 0.7. Same channel as `04-regions-3.png`. |

## 05-large-cubes-and-simulations.md

Use a backend that has not yet built the pyramid of `rho.0000.flt` for shot 1 and the start of shot 2 (or delete its pyramid from the backend's pyramid folder first). Log scale on in every shot.

| File | What must be on screen |
|---|---|
| `05-large-cubes-and-simulations-1.png` | Cube window, 1600 × 1000, `rho.0000.flt` just opened, one pane *3D View*, *Local*: the strided preview, log colour scale (LUT editor *Scale: Log* applied), the status line naming the preview (*Preview: every Nth voxel*), and the banner offering *Load full resolution* and *Switch to Remote*. Default camera. |
| `05-large-cubes-and-simulations-2.png` | Same window after *Remote* and the pyramid build: the remote view of the full cube, *Remote* selected in the pane header's *Auto / Local / Remote*, status bar naming remote rendering. Same camera as shot 1 so the two compare. |
| `05-large-cubes-and-simulations-3.png` | Back on *Local* with the pyramid built: camera zoomed onto a dense filament (about a quarter of the cube across the view) and settled, the refined part visibly sharper than the block-mean preview around it, the status line reading *Detail: level … in x …, y …, z …; elsewhere block means, pyramid level 3*. |

Pending VR shots, not yet referenced by the pages (add the figure directives to steps 13 to 15 of 05 and step 13 of 08 once captured):

| File | What must be on screen |
|---|---|
| `05-large-cubes-and-simulations-4.png` | In the headset, `rho.0000.flt` sent with *Show on Headset*, log scale: the clipping plane *On*, tilted about 30° from the X axis through the middle of the box, the cut face showing the density; the *Clipping plane* tool in hand, the hand menu closed. |
| `05-large-cubes-and-simulations-5.png` | In the headset, same volume: two isosurfaces (*Surfaces ▸*, *+ Add* twice) at two levels a decade apart, in two colours, the outer one at about 40 % opacity, *With the volume*; the *Surfaces ▸* menu open beside the box with the chips and *Level* visible. |
| `05-large-cubes-and-simulations-6.png` | In the headset, same volume exploded: *Explode ▸* with *Pieces* *Value ranges*, *3*, *Layout* *Row*, after *Explode*: three pieces side by side, the menu visible. |
| `08-vr-from-the-desktop-5.png` | In the headset, a simulation volume with two or three fields (a PLUTO output with several variables, or a VBT volume with several fields) under *Overlay ▸* *On*, *Sum*: the fields in their own colour maps, one colour bar per field. |

## 06-catalogues-hips-crossmatch.md

| File | What must be on screen |
|---|---|
| `06-catalogues-hips-crossmatch-1.png` | **Re-capture**: the current shot predates the new splat size and opacity and shows the old *Color* label (now *Colour by*). 3-D catalogue viewer, 1600 × 1000, `NKATZ_2.3M.tbl` with *Load all* done, shape *Gaussian splats*, coloured by `LDensity` (log scale, Inferno or the default map), colour bar visible, one point selected with its ring and the *Source Info* page showing its row. Check that positions come from `X`, `Y`, `Z` (the *AXIS MAPPING* section visible in the dock). |
| `06-catalogues-hips-crossmatch-2.png` | Main window, 1600 × 1000, *HiPS Viewer* tab: a Herschel 250 µm survey (chosen with *Browse…* and `herschel 250`) or DSS colour, centred on a resolved target typed in *Target:* (`M31` works for DSS; a Galactic-plane target such as `W43` for Herschel), the order and field of view visible in the toolbar. |
| `06-catalogues-hips-crossmatch-3.png` | Image viewer, 1400 × 1000, `vlkb-cutout_2023-09-08_04-44-11_833602_continuum-higal_mosaic_mosaic_wgls_m030039_PSW.fits` after *Tools ▸ Catalogue ▸ Cross-match with Catalogue…* with `simbad`, radius 60: markers with names on the image, *Session Data* showing *Cross-match with simbad*. Needs network access on the backend. |

## 07-polarimetry.md

| File | What must be on screen |
|---|---|
| `07-polarimetry-1.png` | Cube window, 1600 × 1000, `polarimetry/DEMO_I.fits` with Q, U and V companions loaded, *Two panes* showing *Polarised intensity (ch N)* and *Polarisation angle (ch N)* for one sub-band (channel 3 or later), *Session Data* listing the two products. |
| `07-polarimetry-2.png` | The *Rotation Measure Synthesis* dialog with *φ min* -80, *φ max* 80, *φ step* 1 and the sample count (161) shown. Capture the dialog only. |
| `07-polarimetry-3.png` | Cube window, 1600 × 1000, *Two panes*: *Faraday depth (peak \|F\| > 6σ)* left, *Peak \|F(φ)\|* right, for the synthetic field: the left-to-right gradient and the two blobs (near pixels (180, 150) and (330, 340)) visible, colour bars visible. |

## 08-vr-from-the-desktop.md

Headset shots come from the Quest 3's own capture (or *Watch the Headset* on the desktop), cropped to one eye, 1600 × 1000 or the capture's native size. Use the hosted pairing service.

| File | What must be on screen |
|---|---|
| `08-vr-from-the-desktop-1.png` | The *Open in VR Headset* dialog for `hgc44.fits`, waiting for the headset: the large pairing code, the countdown (*Waiting for the headset. The code expires in 4:5x.*), the QR code. Capture the dialog only. Blur nothing: the code expires in five minutes. |
| `08-vr-from-the-desktop-2.png` | In the headset, HCG 44 in front of you, the hand menu open (X) on the *Display* tab: colour map swatches, *Threshold* with its σ value, *Colour max*, *Opacity*. The cube fully visible beside the menu. |
| `08-vr-from-the-desktop-3.png` | In the headset, the tool palette open (A), the ray on the *Kinematic Lasso* icon so its description line shows under the icons. |
| `08-vr-from-the-desktop-4.png` | In the headset, `rho.0000.flt` sent with *Show on Headset*: the density on a log scale, the colour bar beside the box in values, a *Spatial crop* of about a quarter of each axis settled so the label over the box reads *the crop binned 2× (level 1)* (or the level shown). |

## 09-backend-on-a-cluster.md

Use a real cluster profile (PLEIADI or another SLURM site). Replace the user name and paths in the profile with neutral ones (`user`, `/scratch/user/...`) before capturing, or capture on a test account.

| File | What must be on screen |
|---|---|
| `09-backend-on-a-cluster-1.png` | The *Connect to a cluster* dialog with a complete profile: *Name*, *Frontend*, *User*, *VisIVO clone*, *Partition* `gpu`, *GPUs (gres)* `gpu:1`, *Memory*, *Time limit*, *Backend image*, *Data root*, *Workspace*. Capture the dialog only. |
| `09-backend-on-a-cluster-2.png` | The cluster window's *Open Dataset* browser listing the data root on the cluster with a cube selected, title naming *Backend: NAME*; the status bar and backend chip of the window behind it naming the cluster. 1600 × 1000. |
| `09-backend-on-a-cluster-3.png` | Cube window on the cluster, 1600 × 1000, a large cube (several GB) rendered with *Remote* selected in the 3-D pane header, the backend chip naming the cluster. |

## 10-hi-in-a-galaxy-group.md

Before the shots, save the `hcg44_group.reg` file of the page in a folder of your own. All shots on `hgc44.fits` at full resolution, local backend.

| File | What must be on screen |
|---|---|
| `10-hi-in-a-galaxy-group-1.png` | The *Moment Map Settings* dialog on `hgc44.fits`: *Moment order* on *Moment 0: Integrated intensity (M0)*, *Start → End* 21 → 101 with the hint *81 of 158 channels selected*, *Enable threshold mask* off. Capture the dialog only, default size. |
| `10-hi-in-a-galaxy-group-2.png` | Cube window, 1600 × 1000, *Two panes*: left the *Moment 0* map (channels 21 to 101, Inferno), right the *2D Slice* or *3D View*. The five imported regions on the moment map with their labels: green circles on NGC 3190, NGC 3187 and NGC 3185, the cyan circle on NGC 3193, the yellow *Background* annulus around the central pair. Left dock with *Session Data* expanded to *Regions* ▸ `hcg44_group.reg (5)` and its five rows (name, shape, colour swatch), the *NGC 3190 · Circle* row selected and the circle drawn selected (thicker, white handles) on the map. |
| `10-hi-in-a-galaxy-group-3.png` | Same window after marking *Background* as *Background Region* and setting *Tools ▸ Regions ▸ HI Mass Distance…* to `25`, then right-click *NGC 3190* row ▸ *Analyse* with the *Moment 0* pane active (mask off): the moment map left with NGC 3190 selected, the *NGC 3190 spectrum* (mean per channel) in the right pane showing the double-horned profile, *Session Data* with the new *NGC 3190 spectrum* row, and the Inspector on *Parameters* showing *2D stats from: Moment 0 (pane 1)*, *Valid pixels 706 / 706*, *Integrated flux* `3.32 ± 0.27 Jy km/s`, *Flux error from* naming the background annulus (2186 px, its mean and σ, mean subtracted), *Beam area* `30.66 pixels (53.0″ × 32.7″)` and *HI mass* `(4.89 ± 0.39) × 10^8 M☉ at 25 Mpc`. No *Flux warning* line. Widen the Inspector if needed so these lines read in full. |
| `10-hi-in-a-galaxy-group-4.png` | The *Estimate Noise* result for the box *X (columns)* 50 → 200, *Y (rows)* 50 → 200, channels 1 → 158: σ and MAD per channel as *Mean / Median / Min / Max*, the median σ about 2.24e-04. The amber noise box visible in the lower-left corner of the slice beside it. Capture the window with the result panel, or the panel and the slice if the window is too wide to read at page width. |
| `10-hi-in-a-galaxy-group-5.png` | Same window, *Moment 0* map with the five regions: the context menu of the *Background* row in *Session Data* (right-click), with *Background Region* ticked, and the row behind it reading *Background · Annulus · background*. Crop to the left dock and the map if the full window is too wide to read at page width. Step 5 of the page has a hidden comment for it; turn it into a figure once captured. |

## 11-cosmology.md

Local backend. `volumesnap63.bin` from `DemoData/`; the moment and its FITS export go to the Workspace Exports, nothing is written next to the demo file.

| File | What must be on screen |
|---|---|
| `11-cosmology-1.png` | Cube window, 1600 × 1000, `volumesnap63.bin` (field `CDM-Density`), *Two panes*: left *3D View* on the default Log scale, right the *Moment 0* map (column density) computed over all 512 planes with *Enable threshold mask* unticked, on a Log colour scale with its colour bar, filaments and haloes visible. *Session Data* lists the moment product; the status bar reads *Moment map: … \| Mask: …* with the mask off. |
| `11-cosmology-2.png` | The power spectrum window opened by *Compute P(ℓ)* (64 bins) on that moment, at its default size: log axes, x label *Wavenumber k (pixel⁻¹): no celestial pixel scale, ℓ not defined*, blue bins in the fit and grey ones out, error bars, the fitted line, and the slope line *Slope n = -2.008 ± 0.081 (P ∝ kⁿ) over k 0.008166–0.4793, 46 bins, each weighted by 1/σ² of log P. One power law does not fit these bins (χ²/dof = 47.2)…*. *Fit k from* and *to* on *auto*. Capture the window only. |
| `11-cosmology-3.png` | Same window after typing 0.02 in *Fit k from* and 0.2 in *to* (Enter after each): the bins below 0.02 and above 0.2 grey, the line through the middle range, the slope line *Slope n = -1.200 ± 0.034 (P ∝ kⁿ) over k 0.02118–0.1848, 26 bins, each weighted by 1/σ² of log P (χ²/dof = 1.36)*. Capture the window only. |
| `11-cosmology-4.png` | *Tools ▸ Cosmology Calculator…* from the main window, *Model* WMAP9, redshifts `0.1, 0.5, 1, 2, 3`, after *Compute*: five rows, D_C 422.982 at z = 0.1 and 6503.902 at z = 3, D_L and *Proper scale (kpc/arcsec)* filled, the D_A, lookback and age columns empty. Widen the dialog until every column header reads in full. Capture the dialog only. |

## 13-molecular-cloud.md

Local backend with network access. Before the shots, save the `chimps_g40.reg` file of the page in a folder of your own and fetch the C¹⁸O cutout from the *VLKB* tab as in step 2 (the backend downloads it; nothing is written to `DemoData/`). The ¹³CO cube is `vlkb-cutout_2019-04-11_13-06-35_409523_CHIMPS_CHIMPS_13CO_40p00+0p33.fits` from `DemoData/`.

| File | What must be on screen |
|---|---|
| `13-molecular-cloud-1.png` | Main window, 1600 × 1000, *VLKB* tab after *Query* with *glon* 40.0, *glat* 0.228, *dl* 0.352, *db* 0.144, the line under the fields reading `RANGE 39.824000 40.176000 0.156000 0.300000`. Filter box `CHIMPS`, so only the five CHIMPS rows show; the C¹⁸O 3-2 row whose *Identifier* ends in `CHIMPS_C18O_40p00+0p33` selected, its *Coverage* `full` in green, the two C¹⁸O `40p33+0p33` rows `partial`. |
| `13-molecular-cloud-2.png` | ¹³CO cube window, 1600 × 1000, *Two panes*: left *Moment 0* over channels 130 to 139 (mask off) with the clump bright near the lower-left corner; right *Moment 1* over the same channels with the auto mask, diverging colour map, the clump at about 84 km/s. *Session Data* lists both moment rows; the status bar names the order, the unit and the channel range. |
| `13-molecular-cloud-3.png` | Same window: left *Moment 0* over channels 280 to 288 with *Extract PV Diagram* armed and its two-vertex path along the filament at l ≈ 40.07°, from the bottom edge of the map to b ≈ 0.22°; right the PV diagram with *Width* 3, the filament a horizontal ridge at about 9 km/s from about 30″ to 220″ along the offset axis. |
| `13-molecular-cloud-4.png` | C¹⁸O cube window (the `CHIMPS_C18O_40p00+0p33` cutout), 1600 × 1000, *Two panes*: left *Moment 0* over channels 129 to 138, turned by 90° against the ¹³CO map, with the imported *Clump* and *Filament* circles and their labels, *Clump* selected; right the *Clump spectrum* after *Fit Gaussian*, the fitted curve on the line at 84 km/s and the label *Gaussian fit · peak 0.61… · centre 84.1… · FWHM 1.75… · ∫ 1.14…*. *Session Data* with `chimps_g40.reg (2)` expanded and the *Clump spectrum* row; Inspector on *Parameters* reading *2D stats from: Moment 0 (pane 1)*, *Valid pixels 29 / 29*, *Mean* about 1.145. |
