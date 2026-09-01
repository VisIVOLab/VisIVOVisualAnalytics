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
- *"What is the comoving distance at redshift 0.1?"* → `cosmology_distance` with
  the honest (Planck18) constants.

## Privacy

With **Your server** (self-hosted), the whole loop — your FITS data summaries,
your prompts, and the model — stays on hardware you control. Nothing is sent to
a third-party cloud. This is the recommended mode for sensitive or embargoed
data. See **[Self-hosted copilot LLM](copilot-self-hosted)**.
