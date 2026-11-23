# Scientific Copilot

The Copilot answers questions about the open data in plain language ("what are
the statistics of this cube?", "are there HI sources?"). A language model
chooses which analysis tools to call and words the answer; every number in the
answer comes from one of the backend's analysis tools, and the calls are shown
under it. Open a dataset, choose the *Copilot* tab in the Inspector (beside
*Properties*, *Analysis* and *Parameters*), type in the box at the bottom and
press *Send*.

## Panel

- *Send*: sends the question. While the answer is coming, the button reads
  *Stop* and interrupts it, and the status line names the tool running
  (*Running cube_statistics…*).
- *＋ New chat*: starts a new conversation. Within one conversation you can ask
  follow-ups (*"and the maximum?"*, *"now show that as a moment map"*).
- Starter chips: on an empty conversation, a few example questions for the
  current viewer. Click one to send it.
- *↩ Undo latest change*: appears after the Copilot changes the view, and
  reverts that change in every window it touched.
- Provider line: at the bottom, the model in use and whether it is ready
  (*Provider: server's configuration (self-hosted, model qwen) · ready*).
- A red message above the conversation says why the Copilot cannot answer (see
  [When the Copilot cannot answer](#when-the-copilot-cannot-answer)).

## What it can do in each viewer

Changes to the view are display-only and can be undone.

| Viewer | Analysis tools | Changes it can make |
|---|---|---|
| Cube | `dataset_metadata`, `cube_statistics`, `moment_map_summary`, `hi_products`, `noise_estimate`, `aperture_photometry`, `source_gaussfit`, `fit_line`, `find_sources` (SoFiA-2), `cross_match` | colormap, threshold, volume or isosurface, Composite, MIP or MinIP, go to channel, reset camera, compute and show a moment map, show SoFiA-2 detections, pin a spectrum |
| Image | `dataset_metadata`, `cube_statistics`, `aperture_photometry`, `source_gaussfit`, `angular_power_spectrum`, `cross_match` | colormap, scaling (linear, log, sqrt, square, power), reset camera |
| Catalogue 3D | `catalogue_summary`, `cosmology_distance` | colour scale, colour by field, reset camera |
| VBT | `vbt_summary` | colormap or colour scale, blend mode (volumes), colour by field, reset camera |

With several datasets open (a cube and an image of the same field), ask it to
*list the open datasets*, then refer to any of them; each request goes to the
right window.

## Example questions

- *Give me the statistics of this cube.*: `dataset_metadata` and
  `cube_statistics` (units, beam, pixel scale, min, max, mean, median, σ, rms,
  MAD, pixel counts).
- *What's the noise level, and what threshold should I use?*: `noise_estimate`
  (σ per channel), then sets the threshold at a few σ.
- *Measure the flux of the source at RA 10.0 Dec 20.0.*: `aperture_photometry`;
  *and fit it*: `source_gaussfit` (peak, integrated flux, FWHM, position angle,
  beam-deconvolved size). RA and Dec are ICRS and converted to the image's frame.
  On a cube with a Stokes axis, both measure Stokes I of the requested channel.
- *Fit the line at RA 10.0 Dec 20.0 over a 5-arcsec aperture.*: `fit_line`
  (centre, σ and FWHM in the axis unit, integrated flux, equivalent width).
- *Plot the spectrum at …*: pins the aperture spectrum as a panel, optionally
  with the fitted Gaussian.
- *Find all the sources in this cube.*: `find_sources` runs SoFiA-2 (it must be
  installed on the backend); *Show me the detections.* overlays the mask in 3-D.
- *What known sources are in this field?*: `cross_match` against SIMBAD, 2MASS,
  NVSS or FIRST.
- *What is the comoving distance at redshift 0.1?*: `cosmology_distance`
  (Planck18).
- In the Catalogue 3D window, *What is the redshift range?*: `catalogue_summary`.
- On a Gadget-4 SubFind catalogue, *Are the subhalo velocities peculiar?*: `catalogue_summary` with `subfind_velocity_check`, against the snapshot beside it.

## Evidence under each answer

- Provenance: each tool call with its JSON result, under a line starting
  *✓ provenance · grounded in tool call(s):* (without the tick when the answer is
  not fully grounded in them).
- Reproducibility script: a short Python snippet that recomputes every number by
  calling the same backend code, with no language model involved.

## Choosing a provider

*Settings ▸ Copilot* (the *Scientific Copilot* group): *Provider*, with the
*API key*, *Server URL* and *Model* fields.

| Provider | What it is | You supply |
|---|---|---|
| *Use the server's configuration* (default) | Whatever the backend's operator configured, or no model. | Nothing |
| *Claude (Anthropic)* | Anthropic's cloud models. | An Anthropic API key |
| *ChatGPT (OpenAI)* | OpenAI's cloud models. | An OpenAI API key |
| *Your server (OpenAI-compatible)* | An OpenAI-compatible endpoint you run (vLLM, Ollama). Data and prompts stay on your infrastructure. | The URL (for example `http://your-gpu-host:8000/v1`), the key if required, and the *Model* |
| *Off* | No language model: the tools still run and the answer is their raw output. | Nothing |

Any choice other than the first applies to your session only; other users of the
same backend keep theirs. Keys are held in the backend's memory only, never
written to disk or sent back to the client.

For sensitive or embargoed data, use *Your server*: nothing then leaves hardware
you control. To set up such a server, or to configure the backend for all its
users, see [Self-hosted copilot LLM](../../technical/deployment/copilot-self-hosted.md).

## When the Copilot cannot answer

The reason appears in red above the conversation and under the provider fields in
*Settings ▸ Copilot*, naming whose configuration it concerns.

- No model set, and the server serves none or several: the message lists the
  served models. With *Your server*, enter one in *Model*; with the server's
  configuration, the operator sets `VISIVO_ASSISTANT_MODEL`, or you switch to
  *Your server*. This appears after the first question.
- The operator's `VISIVO_LLM_BASE_URL` was refused (not an http(s) URL, or with
  credentials, a query or a fragment): appears at connection. Any explicit
  provider choice still works.

The message goes away once the backend no longer reports the problem, for
example after a model is set and *Settings* is closed with *OK*.

Details: [Assistant API](../../technical/backend-api/assistant.md) and
[Copilot backend design](../../technical/design/copilot-backend.md).
