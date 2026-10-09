# Building the paper's figures

The A&C manuscript has seven figures, all still placeholders. This is the
recipe for each one: what it has to show, what already exists, the exact
command, and what is genuinely missing.

They are listed **by cost**, not by number. Two are one command away, two are a
GUI session, one is a drawing, and two need data or science that is not on disk
yet.

## Conventions

Put the images in a `figures/` directory next to `main.tex`. The manuscript
already loads `graphicx`, so replacing a placeholder means swapping the `\fbox`
block for an `\includegraphics` and keeping the caption and label:

```latex
\begin{figure*}[t]
\centering
\includegraphics[width=0.96\linewidth]{figures/fig3_lasso.png}
\caption{… unchanged …}
\label{fig:lasso}
\end{figure*}
```

Keep `figure*` (two-column span) where the placeholder used it and `figure`
where it did not — Figs. 6 and 7 are explicitly meant to stay small.

The validation scripts read their inputs from environment variables, so set
these once per shell:

```bash
cd <repo>
export VISIVO_VALIDATION_CUBE=~/Downloads/DemoData/hgc44_3axes.fits
export VISIVO_VALIDATION_FIELD=~/Downloads/CF4_new_64-z008_velocity.fits
export VISIVO_DEMODATA=~/Downloads/DemoData
PY=backend/.venv/bin/python
```

---

## Fig. 7 — cosmic flows (`fig:flows`) — **one command**

*Shows:* the CF4 reconstruction as streamlines coloured by speed, with the
gravitational basins of attraction, as the "does this generalise beyond PPV"
demo. Must stay small.

```bash
$PY validation/velocity_field_demo.py     # → validation/out/velocity_field.png
```

Already produced and ready. The only thing the caption still asks for is the
**galaxy-catalogue overlay** on top of the field; if you decide the streamlines
plus basins make the point on their own, drop that clause from the caption and
this figure is done.

## Fig. 3 — the lasso (`fig:lasso`) — **one command + two screenshots**

*Shows:* six panels on a real HI galaxy — (a) the seed double-click on the
volume render, (b) the grown selection, (c) the same seed at a lower $\lambda_v$,
(d) per-channel polygon refinement, (e) *isolate*, (f) *remove*.

```bash
$PY validation/figure3_lasso_hgc44.py     # → validation/out/figure3_lasso.png
```

That gives you **(b), (c), (e), (f)** on HGC 44, computed through the real
endpoints. Verified by running it: panel (b) is the grown selection outlined on
moment-0 — and it visibly *excludes* the companion galaxy to the left, which
panel (f) then shows surviving in the residual. That pair is the strongest
evidence in the figure and is worth saying in the caption.

**Two mismatches with the current caption, to fix before you paste it in:**

- The caption describes (c) as "same seed, lower velocity coupling: the
  selection follows the rotation" — a *picture*. What the script produces is a
  *plot*: selected pixels per channel for $\lambda_v = 0.35$, 1.0 and 1.6
  (16136 / 13216 / 12792 voxels total). It makes the same point quantitatively
  and is arguably the better panel, but the caption has to describe it.
- The PNG carries its own title line ("Kinematic lasso on an HGC 44 HI
  galaxy…"). In a journal figure that duplicates the caption; either strip it in
  the script (`fig.suptitle`) or crop it.

**(a) and (d) have to be screenshots**, because they show the *gesture*, not the
result:

- (a) open `hgc44_3axes.fits`, 3-D view, Volume rendering, rotate until the
  target galaxy is clear, arm *Tools → Kinematic Lasso*, and capture the frame
  with the cursor over the source. The yellow seed marker is now transient — it
  is visible while the grow runs, so capture during the grow, or capture the
  moment of the click.
- (d) with a selection live, press *Refine on 2-D map…*, draw a polygon on the
  channel map, and capture with the polygon half-drawn so the interaction reads.

Use a fixed window size for both so the panels sit together; macOS
`⇧⌘4` then Space captures a single window.

## Fig. 2 — architecture (`fig:architecture`) — **a drawing**

*Shows:* desktop client (Qt/VTK, GPU) ↔ authenticated REST ↔ compute service in
its three deployments (embedded / remote server / HPC via Apptainer+SLURM), plus
the persistent-product workspace and the VLKB/SODA path.

Nothing to compute — this is a diagram. Two things make it accurate rather than
decorative, and both are already true of the code:

- the *same* client talks to all three deployments over the same REST contract,
  so draw one arrow, not three variants;
- the workspace is on the **service** side (`~/.visivo/exports`), which is why a
  product survives the session — put it inside the service box, not between the
  two.

`docs/architecture-note.md` and `apptainer/backend.def` have the deployment
details if you want the box labels to match reality.

## Fig. 1 — the storyboard (`fig:workflow`) — **a GUI session**

*Shows:* eight screenshots in one row — cube → explore → lasso click → isolated
cube → moment 0 → tilted ring → residual → movie/export — plus an inset with the
REST call that regenerates the mask.

This is the figure a reader looks at first, and it is the one that most needs to
be a *single continuous session* rather than eight unrelated captures. Suggested
run, on HGC 44 so it matches Fig. 3:

1. Open the cube — capture the first view.
2. Rotate / change the threshold — capture.
3. Arm the lasso, double-click the galaxy — capture during the grow.
4. *Isolate → new cube*, open the product — capture.
5. On the isolated cube, *Moment Map* order 0 — capture.
6. *Kinematic Model Overlay* with the ring parameters — capture.
7. *Remove → new cube* on the original, open it — capture the residual.
8. *Export Movie* dialog or a frame of the result — capture.

For the inset, the honest version of the REST call is the one the client
actually sends:

```
POST /v1/cube/mask_apply_bin?dataset_id=…&origin_x=…&mode=blank_outside
```

`mask_selection` re-grows from the seeds, `mask_apply_bin` applies the exact
displayed mask. The caption claims reproducibility, so quote the one that
reproduces *what was on screen* — and say which seeds and parameters produced
it, since that is the claim.

## Fig. 5 — state transitions (`fig:transitions`) — **assembled from Fig. 1's session**

*Shows:* the chain Cube → Mask → Masked cube → Moments → Model → Residual →
Publication, each node a thumbnail plus a product card (size, provenance,
parameters, and the API call of the transition).

Do this **immediately after Fig. 1**, from the same session: the thumbnails are
the same captures, and the cards are real. The card contents come from the
Inspector's **Provenance** tab, which records exactly these fields for every
product — so the figure is a screenshot-plus-layout job, not an invention. The
caption currently admits the values are illustrative; with the real session they
need not be, and that is worth the half hour.

## Fig. 4 — tilted-ring model (`fig:model`) — **needs a fit you do not have yet**

*Shows:* three channel maps (approaching, systemic, receding) with tilted-ring
model contours overlaid, flat disc versus warped rings, plus a frame of the
model-vs-data movie.

The obstacle is stated in the paper itself: the tilted-ring tool **overlays a
model, it does not fit one**. So the figure needs ring parameters from
somewhere:

- take them from a published fit of whatever galaxy you use (then cite it, and
  say in the caption that the parameters are external), or
- fit them yourself with `3DBarolo`/`TiRiFiC` and present VisIVO as the
  comparator it is.

Either is defensible; what is not defensible is implying the tool produced the
fit. Pick the galaxy first — if it is the HGC 44 source used elsewhere, the
figure ties the whole paper to one dataset, which is worth more than variety.

## Fig. 6 — polarimetry (`fig:polarimetry`) — **needs data that is not here**

*Shows:* MeerKAT 1.3 GHz Bulge survey — Stokes I with polarisation vectors, and
the per-pixel Faraday RM map from the $(Q,U,\nu)$ fit. Must stay small.

Nothing under `~/Downloads/DemoData` matches; you need the Stokes I/Q/U cubes of
that survey (SARAO archive). Two honest options:

- fetch the data and produce it, remembering the paper's own caveat that the RM
  fit is valid at low RM;
- **cut the figure** and keep the polarimetry paragraph as text. The submission
  checklist already lists "too much on the plate" as a referee risk, with
  polarimetry named as the thinnest section. A missing small figure costs less
  than a weak one.

---

## Order of work

1. `velocity_field_demo.py` and `figure3_lasso_hgc44.py` — today, one command each.
2. One recording session on HGC 44 → Figs. 1, 5 and the two missing panels of 3.
3. The architecture diagram.
4. Decide Fig. 4 (whose fit) and Fig. 6 (fetch or cut).

Figures 3 and 7 carry the paper's two measured claims; they are also the two
that are essentially done.
