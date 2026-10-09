# Copilot backend & engine

Developer reference for the scientific copilot's server side. The user-facing
guide is [Scientific Copilot](user-guide/copilot); this page documents the code
in `backend/app/assistant/` and the `/v1/assistant/*` routes.

## Design: provenance-bound tool use

The copilot is a **bounded tool-use agent** over the existing analysis workers,
not a free-form chatbot. The LLM only decides *which* tool to call and how to
phrase the result; every number comes from a real worker. The grounding rule is
enforced two ways:

1. The **system prompt** (`engine.SYSTEM_PROMPT`) forbids stating any numeric
   result that did not come from a tool call, requires establishing units/frame
   first (`dataset_metadata`), and lists the science caveats (moment-2 is σ not
   variance, fluxes are Jy only when beam-corrected, frequency needs a rest
   frequency for km/s, …).
2. Every tool call is **recorded** and surfaced as provenance + a reproducibility
   script, so an answer is auditable.

The flag is named `tool_calls_recorded` (not `grounded`) on purpose: it is a
factual trace flag — "≥1 tool ran" — **not** a guarantee that every number in the
prose is tool-backed, which would need answer-level citations or a verifier. The
name keeps the UI from overclaiming.

## Package layout

| Module | Responsibility |
|--------|----------------|
| `assistant/tools.py` | The grounded, read-only tool catalogue + JSON specs. |
| `assistant/provider.py` | The LLM provider abstraction + runtime config. |
| `assistant/engine.py` | The tool-use loop (`AssistantEngine`). |
| `routers/assistant.py` | The HTTP routes + admission control. |

## The engine loop

`AssistantEngine.run(question, dataset_path, dataset_id)` (`engine.py`) runs a
bounded conversation:

- History is a **neutral, provider-agnostic list** of `{role: user|assistant|tool}`
  turns. Each provider translates it to its own wire shape (Anthropic
  `tool_use`/`tool_result` blocks, OpenAI `tool_calls`/`tool` messages), so the
  engine is identical across Claude / OpenAI / a self-hosted OpenAI-compatible LLM.
- Each round calls `provider.complete(SYSTEM_PROMPT, history, specs)`:
  - If the response has **tool calls**, each is executed against the real worker
    (`_execute` → `tools.TOOLS[name].executor(dataset_path, args)`), the result
    (or error) is appended as a `tool` turn, and the loop continues.
  - Only a response with **no tool calls** ends the loop and yields the final
    answer. Text emitted *alongside* a tool-use turn is deliberately **not**
    adopted as the answer, so an exhausted loop never returns half a sentence.
- The loop is capped at `_MAX_ROUNDS = 6`. Exhaustion returns an explicit
  "stopped after the maximum number of tool rounds" message.
- Tool exceptions are **not** swallowed — they are returned to the model as an
  `is_error` tool result so it can recover or report the failure.

The result (`AssistantResult.to_dict()`) carries `answer`, `provider`,
`configured`, `tool_calls_recorded`, the list of `tool_calls`
(name/arguments/result/error), `rounds`, and a **reproducibility script**
(`_repro_script`) — a runnable Python sketch that regenerates the tool calls
headless via the same workers, with no LLM involved.

## Providers

`provider.py` defines `AssistantProvider` (ABC) with `complete(system, history,
tools) -> ProviderResponse` and two flags — `name` and `configured`:

| Provider | `configured` | Notes |
|----------|--------------|-------|
| `NullProvider` | `False` | No LLM; returns the canned "not configured" note. Tools still run. |
| `AnthropicProvider` | `True` | Anthropic cloud (`anthropic` SDK). |
| `OpenAIProvider` | `True` | OpenAI cloud **or** any OpenAI-compatible server — the only difference is `base_url`. Uses the `openai` SDK. |

Each provider owns the translation from the neutral history to its API and back
to a `ProviderResponse(tool_calls, final_text)`. `OpenAIProvider` therefore
serves ChatGPT and a self-hosted vLLM/Ollama endpoint with the same code path.

:::{note}
The backend needs the `openai` SDK installed (`backend/requirements.txt`) for the
OpenAI / "Your server" providers. Without it, `OpenAIProvider.__init__` raises on
`import openai` and `provider_from_env()` degrades to `NullProvider` — the copilot
then shows *Provider: null* even after you configure a URL/key.
:::

## Runtime configuration & dispatch

Config lives in a module-level `_CONFIG` dict (seeded from env vars) and is
changed at runtime by `configure(**kwargs)`:

- `provider` — `auto | anthropic | openai | null`.
- `anthropic_key` — Claude.
- **`openai_key`** — the CLOUD OpenAI key.
- **`base_url`** + **`base_url_key`** — a self-hosted / OpenAI-compatible endpoint
  and *its own* key.

The cloud key and the self-hosted key are kept **strictly separate**: the cloud
key is used only when there is no `base_url`, and a self-hosted endpoint only ever
receives its `base_url_key`. This prevents an authenticated caller from setting an
arbitrary `base_url` and having a stored cloud key sent there. `base_url` is
validated by `_sanitize_base_url` (must be http(s), no embedded credentials, no
query/fragment).

`provider_from_env()` builds the active provider: in `auto` it prefers a
self-hosted/OpenAI endpoint or key, then Anthropic, else `NullProvider`; on **any**
construction error it degrades to `NullProvider` (never crashes the route).
`public_config()` returns the non-secret view for the UI (keys reported only as
`*_set` booleans; `base_url` sanitised).

The active provider is **module-cached** (`_get_provider`) so `/status` and
`/query` don't rebuild an SDK client each call; `POST /config` calls
`_invalidate_provider()` so the next call rebuilds it.

## Tools

`tools.py` exposes read-only, grounded summaries over the loaded dataset. Each
`AssistantTool` has a `name`, `description`, a JSON-schema `parameters`, and an
`executor(path, params)`. Current read catalogue: `dataset_metadata`,
`cube_statistics`, `moment_map_summary`, `hi_products`, `noise_estimate`,
`aperture_photometry`, `source_gaussfit`, `fit_line` (spectrum extraction +
Gaussian fit), `find_sources` (SoFiA-2), `cross_match` (SIMBAD/2MASS/NVSS/FIRST),
`angular_power_spectrum`,
`cosmology_distance`, `catalogue_summary` (cosmology / point catalogues),
`vbt_summary` (VBT point/volume tables). `tool_specs()` renders them as
`{name, description, input_schema}` for the provider. Executors call the same
workers the REST API uses, so the numbers are identical to the interactive tools.

A tool that must operate on the **in-session** data rather than re-reading a file
sets `wants_context=True`; the engine then passes it the resolved session dataset
entry as a third argument (`executor(path, params, context)`). `catalogue_summary`
uses this to read the already-concatenated catalogue DataFrame, and `vbt_summary`
to read the memory-mapped VBT binary — neither has (or needs) a single file path.

Tools whose description starts with `ACTION:` (`set_colormap`, `set_threshold`,
`set_render_mode`, `set_blend_mode`, `goto_channel`, `reset_camera`,
`compute_moment`) don't return data — they emit a viewer directive the desktop
app applies to the active viewer (display-only and reversible). Which directives
a given viewer honours depends on its data; see the table in the
[Scientific Copilot](user-guide/copilot) user guide.

## HTTP routes & admission control

`routers/assistant.py`:

| Route | Purpose |
|-------|---------|
| `GET /v1/assistant/status` | Active provider + `public_config()` for the UI. |
| `GET /v1/assistant/tools` | The tool catalogue. |
| `POST /v1/assistant/config` | Choose the provider at runtime (422 on a bad `base_url`); rebuilds the provider. |
| `POST /v1/assistant/query` | Run one copilot query. |

`/query` guards against LLM cost-DoS:

- The prompt is bounded (`question: Field(max_length=4000)`).
- A **dedicated** semaphore (`_ASSISTANT_SLOTS`, default 2) — *not* the global
  heavy-compute semaphore, because a query spends most of its time on LLM network
  I/O and holding a heavy slot would starve real moment/isosurface compute.
- A per-session task slot is taken, and both slots are **held until the worker
  thread settles**: on timeout (`_ASSISTANT_TIMEOUT_S`, default 120 s) the client
  gets a 504 immediately, but the slots are released only by an
  `add_done_callback` when the still-running thread finishes — so admission
  control stays accurate and slots can't leak. A pre-acquire cancel guard
  releases the session slot if the request is cancelled before the semaphore is
  taken.

## Security summary

- Keys are held in server **memory only** — never persisted, logged, or returned
  (`public_config` exposes only `*_set` booleans).
- Cloud key and self-hosted key are never interchanged.
- `base_url` is validated (no credentials / query / fragment).
- The tool catalogue is read-only; the loop is bounded in rounds, prompt size,
  concurrency, and wall-clock time.
