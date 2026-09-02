# Scientific Copilot

The **Copilot** is an AI assistant built into the cube viewer's Inspector. You
ask a question in natural language ("what are the statistics of this cube?",
"are there HI sources?") and it answers in prose — but every number it states
comes from a **real backend analysis tool**, never from the language model's
imagination. The LLM only decides *which* tools to call and how to phrase the
result; the figures are computed by the same deterministic workers the rest of
VisIVO uses.

This "provenance-bound" design means the copilot is safe for science: it cannot
invent a flux, a beam size, or a moment value. If it quotes a number, a tool
produced it, and the exact tool call is shown.

## Where it is

Open a cube, then in the **Inspector** (right panel) switch to the **Copilot**
tab (alongside Properties / Analysis / Provenance). Type in the box at the
bottom and press **Send**.

The provider status is shown at the bottom of the panel, e.g.
`Provider: self-hosted (ready)` or `Provider: null (not configured)`.

The Copilot is available in several viewers, each with the analysis tools and
reversible actions that make sense for its data:

| Viewer | Grounded analysis tools | Actions it can take |
| --- | --- | --- |
| **Cube** (3-D spectral cube) | `dataset_metadata`, `cube_statistics`, `moment_map_summary`, `hi_products`, `noise_estimate`, `aperture_photometry`, `source_gaussfit`, `fit_line`, `find_sources` (SoFiA-2), `cross_match` | colormap, threshold, render mode (volume/isosurface), blend mode (composite/MIP/MinIP), go to channel, reset camera, **compute & show a moment map**, **show source detections (SoFiA-2 overlay)**, **pin a spectrum panel** |
| **Image** (2-D) | `dataset_metadata`, `cube_statistics`, `aperture_photometry`, `source_gaussfit`, `angular_power_spectrum`, `cross_match` | colormap, scaling (linear/log/sqrt/square/power), reset camera |
| **Cosmology catalogue** (3-D point cloud — the *Catalogue 3D* window) | `catalogue_summary` (columns, coordinate system, per-column ranges), `cosmology_distance` (redshift → comoving/luminosity/angular-diameter distance) | colour scale (linear/log/…), colour-by-field, reset camera |
| **VBT** (point / volume binary table) | `vbt_summary` (rows, fields, per-field ranges) | point view: colormap, colour-by-field, reset camera · volume view: colour scale (linear/log/…), blend mode (composite/MIP/MinIP), colour-by-field, reset camera |

Actions are **display-only and reversible**: after the copilot changes the view
it logs an **↩ Undo latest change** link that reverts exactly that step.

## Working across several open views

When more than one dataset is open — say a spectral **cube** and a 2-D **image** of
the same field — the copilot can reason about and act on **all of them**. Ask it to
*"list the open datasets"* and it enumerates them (id, kind, shape); then refer to
another view naturally (*"and what's the noise in the continuum image?"*, *"recolour
the cube to match"*). Each analysis or visualisation directive is routed to the
right window automatically — so a single request can, e.g., compute a moment on the
cube **and** adjust the image. Undo reverts the last change across whichever views
it touched.

## Conversations & follow-ups

The copilot is **conversational**: it remembers the current exchange, so you can
ask follow-up questions that build on the previous answer — *"and the maximum?"*,
*"now show that as a moment map"*, *"fit it with an aperture instead"* — without
repeating the context. Each message is shown as a chat bubble (your questions on
the right, the copilot on the left), with the tool-call provenance folded under
each answer. Click **＋ New chat** (top-right) to start a fresh conversation and
drop the follow-up context.

While the copilot works, the status line shows **which analysis is running live**
(*"Running cube_statistics…"*, *"Running compute_moment…"*) — streamed from the
backend as each tool executes — so a multi-step answer isn't a blank spinner.

On an empty conversation the panel shows a few **starter-question chips** tailored
to the current viewer (a cube offers *"Find the sources"*, an image *"Measure the
flux at RA … Dec …"*, a catalogue *"What's the redshift range?"*, …) — click one
to send it and see what the copilot can do.

## Choosing a provider (⚙)

Natural-language answers need a Large Language Model. The tool catalogue works
**without** one — but then the copilot just runs the tools and reports the raw
numbers instead of a prose summary. Click the **⚙ (gear)** in the Copilot panel
to choose where the language model runs:

```{list-table}
:header-rows: 1
:widths: 18 42 40

* - Provider
  - What it is
  - You supply
* - **Claude**
  - Anthropic's cloud models.
  - An Anthropic API key.
* - **ChatGPT**
  - OpenAI's cloud models.
  - An OpenAI API key.
* - **Your server**
  - Any **OpenAI-compatible** endpoint you run yourself (vLLM / Ollama / …),
    so your data and prompts never leave your infrastructure and there is no
    per-token cost.
  - The endpoint **URL** (e.g. `http://your-gpu-host:8000/v1`) and, if it
    requires one, its **key**.
* - **None**
  - No LLM — tools still run, answers are raw tool output.
  - Nothing.
```

Keys are held in the backend's memory only; they are **never** written to disk
or returned to the client. The self-hosted endpoint key is kept strictly
separate from any cloud key, so a stored cloud key can never be redirected to an
arbitrary URL.

:::{tip}
Running your own model? See **[Self-hosted copilot LLM](copilot-self-hosted)**
for a turnkey container that serves an OpenAI-compatible endpoint on your GPU
server.
:::

## How an answer is grounded

Every copilot answer carries its evidence. Below the prose you get:

- **Provenance** — an expandable panel listing each tool call and its JSON
  result, e.g.

  ```text
  dataset_metadata({}) → {"bunit":"Jy/beam","beam_major":0.00836…, "ctype":["RA---SIN","DEC--SIN","VOPT"], …}
  cube_statistics({})  → {"min":-0.0100…,"max":0.0692…,"mean":0.000165…,"rms":0.00262…,"valid_count":5918292, …}
  ```

  The header reads `Provenance (<provider>) · <configured|not configured> · with
  tool call(s)`.

- **Reproducibility script** — a short Python snippet that regenerates every
  number **deterministically, with no LLM involved**, by calling the same
  backend workers directly. Paste it into a notebook to reproduce or extend the
  analysis in a paper.

The tools the copilot can call are grounded, read-only summaries over the loaded
dataset — dataset metadata, cube statistics, moment-map summaries, HI products
(column density / mass), cosmology distances, and so on. The LLM orchestrates
them; it cannot fabricate their outputs.

## Typical questions

- *"Give me the statistics of this cube."* → `dataset_metadata` + `cube_statistics`,
  summarised (units, beam, pixel scale, min/max/mean/median/σ/rms/MAD, pixel counts).
- *"Are there HI sources?"* → reasons over the statistics and can suggest / run a
  moment-0 map to integrate the emission.
- *"What's the noise level, and what threshold should I use?"* → `noise_estimate`
  (per-channel σ), then a `set_threshold` action at a few × σ.
- *"Measure the flux of the source at RA 10.0 Dec 20.0."* → `aperture_photometry`
  (raw sum + Jy when beam-corrected); *"and fit it"* → `source_gaussfit` (peak,
  integrated flux, FWHM, position angle, beam-deconvolved size).
- *"Fit the line at RA 10.0 Dec 20.0 over a 5-arcsec aperture."* → `fit_line`
  extracts the aperture spectrum and fits a Gaussian (center/σ/FWHM in the axis
  unit, integrated flux, equivalent width).
- *"Find all the sources in this cube."* → `find_sources` runs SoFiA-2 and reports
  the detection catalogue (requires the SoFiA-2 binary on the server).
- *"What known sources are in this field?"* → `cross_match` queries SIMBAD / 2MASS
  / NVSS / FIRST over the footprint and lists the catalogued matches.
- *"Show me the detections."* → `show_sources` opens SoFiA-2 for the cube and
  overlays the detection mask in the 3-D viewer.
- *"Plot the spectrum at RA 10.0 Dec 20.0."* → `plot_spectrum` extracts and pins
  the line profile as a 1-D panel; with `fit` it also overlays the fitted Gaussian
  curve on the profile (use `fit_line` for the numeric fit parameters).
- *"What is the comoving distance at redshift 0.1?"* → `cosmology_distance` with
  the honest (Planck18) constants.
- *(in the Catalogue 3D window)* *"What columns does this catalogue have, and what
  is the redshift range?"* → `catalogue_summary`; follow up with *"how far away is
  the most distant source?"* to chain `cosmology_distance`.
- *(in the VBT window)* *"How many points are in this table and what fields does it
  carry?"* → `vbt_summary`; *"colour it by the density field"* recolours the cloud.

## Privacy

With **Your server** (self-hosted), the whole loop — your FITS data summaries,
your prompts, and the model — stays on hardware you control. Nothing is sent to
a third-party cloud. This is the recommended mode for sensitive or embargoed
data. See **[Self-hosted copilot LLM](copilot-self-hosted)**.
