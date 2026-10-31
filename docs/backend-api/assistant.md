# Assistant

The routes of the scientific copilot (`backend/app/routers/assistant.py`): a
tool-calling assistant over the backend's own analysis workers, where every
number in an answer comes from a recorded tool call. The design, the tool
catalogue and the engine loop are described in
[Copilot backend](../copilot-backend.md).

The language-model provider is configured per backend session, so users
sharing one backend each choose their own provider and keys. With no provider
configured the routes still work: `query` explains how to enable one, and the
tool catalogue is available. Every route needs the token; all except `tools`
use the session from `X-Visivo-Session`.

## `GET /v1/assistant/status`
The provider active for this session:

```json
{ "configured": false, "provider": "null", "n_tools": 25,
  "config": { "provider": "auto", "anthropic_key_set": false,
              "openai_key_set": false, "base_url": "",
              "base_url_key_set": false, "model": "",
              "available": ["anthropic", "openai", "null"] } }
```

`config` never contains a key, only whether each one is set; `base_url` is
returned without embedded credentials.

## `POST /v1/assistant/config`
Choose the provider for this session. Every field is optional: an omitted
field is left unchanged and an empty string clears it.

```json
{ "provider": "openai", "base_url": "http://llm.internal:8000/v1",
  "base_url_key": "…", "model": "…" }
```

| Field | Meaning |
|---|---|
| `provider` | `auto`, `anthropic`, `openai` or `null`. `auto` picks `openai` when a `base_url` or `openai_key` is set, else `anthropic` when its key is set, else `null` |
| `anthropic_key`, `openai_key` | Keys of the hosted providers |
| `base_url` | URL of a self-hosted, OpenAI-compatible server |
| `base_url_key` | Key for that server, kept apart from `openai_key` |
| `model` | Model name to request |

Keys are held in memory on the session only: never persisted, logged or
returned. Response: `{valid, configured, provider, config}`. An invalid value
is 422.

## `GET /v1/assistant/tools`
The tool catalogue, `{tools: [...]}`, one provider-ready JSON specification
per tool. No session needed.

## `POST /v1/assistant/query`
Answer a question about the data.

```json
{ "question": "What is the peak flux in channel 40?",
  "dataset_id": "ds_…",
  "history": [{ "role": "user", "text": "…" }, { "role": "assistant", "text": "…" }] }
```

| Field | Limit |
|---|---|
| `question` | Required, non-empty, at most 4000 characters |
| `dataset_id` | Optional; the dataset the question is about, which must be open in the session |
| `history` | Earlier turns for follow-up questions, at most 20, each `text` at most 4000 characters, `role` `user` or `assistant` |

Response: `answer`, `provider`, `configured`, `tool_calls_recorded` (at least
one tool ran; not a guarantee that every number in the text is tool-backed),
`rounds`, `tool_calls` (`[{name, arguments, result, error}]`),
`reproducibility_script` and `actions`.

Admission control: a query takes one of the session's compute-task slots
(429 when none is free) and one of `VISIVO_ASSISTANT_SLOTS` (default 2)
backend-wide assistant slots, and is limited to `VISIVO_ASSISTANT_TIMEOUT_S`
(default 120 s; 504 beyond it). After a timeout both slots stay taken until
the underlying computation actually finishes.

## `POST /v1/assistant/stream`
The same request and admission control as `query`, answered as
Server-Sent Events (`text/event-stream`) so a client can show progress:

| Event | Data |
|---|---|
| `tool` | `{"tool": "<name>"}`: a tool has started |
| `thinking` | `{}`: the model is working |
| `result` | The `query` response |
| `error` | `{"error": "…"}`: failure, cancellation or the time budget exceeded |

The stream ends after `result` or `error`. Refusals (empty question, unknown
dataset, no free slot) are ordinary HTTP errors sent before the stream starts.
