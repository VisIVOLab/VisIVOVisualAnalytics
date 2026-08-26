# VisIVO copilot — self-hosted LLM server (turnkey)

Run an **open-weight LLM on your own GPU server** and point the VisIVO scientific
copilot at it. Your data and prompts never leave your infrastructure, there's no
per-token cost, and the model is pinned/reproducible.

The copilot talks to any **OpenAI-compatible** endpoint, so this package ships a
one-command container that serves one. Two flavours:

| | Image | Best for | Endpoint |
|---|---|---|---|
| **vLLM** (recommended) | `vllm/vllm-openai` | throughput, production | `http://host:8000/v1` |
| **Ollama** (easiest) | `ollama/ollama` | quick start, prototyping | `http://host:11434/v1` |

## Prerequisites (on the GPU host)
- NVIDIA driver + **nvidia-container-toolkit** (`nvidia-ctk`), so containers see the GPU.
- **Docker** with the Compose plugin.
- Disk for the model cache (a 32–70B model is ~30–70 GB).

## Quick start — vLLM
```bash
cd deploy/copilot-llm
cp .env.example .env          # edit: set VLLM_API_KEY (change-me → your secret)
docker compose up -d          # first run downloads the model into ./hf-cache
docker compose logs -f        # watch until "Application startup complete"
./validate.sh                 # confirms /models, a chat completion, AND a tool call
```
Then in **VisIVO → Copilot panel → ⚙ → "Your server"**:
- **URL:** `http://<this-server-ip>:8000/v1`
- **Key:** the `VLLM_API_KEY` you set

Ask a question about a loaded cube — it should call a tool and cite the number.

## Quick start — Ollama (simplest)
```bash
cd deploy/copilot-llm
docker compose -f docker-compose.ollama.yml up -d
docker exec -it visivo-copilot-ollama ollama pull qwen2.5:32b
```
Copilot ⚙ → "Your server" → `http://<server>:11434/v1` (leave Key blank).

## Which model on a single RTX PRO 6000 (96 GB)?
The default (`Qwen/Qwen2.5-32B-Instruct`) is **ungated, fits in bf16, and does
tool-calling well** — a safe starting point. To go bigger, edit `.env`:

| Model | `.env` changes | VRAM | Notes |
|---|---|---|---|
| Qwen2.5-32B-Instruct *(default)* | — | ~65 GB | ungated, strong tools |
| Llama-3.3-70B-Instruct (AWQ) | `LLM_MODEL=casperhansen/llama-3.3-70b-instruct-awq` · `TOOL_PARSER=llama3_json` · `QUANTIZATION=awq` | ~40 GB | excellent tools; base model is gated (AWQ repos usually aren't) |
| gpt-oss-120b | `LLM_MODEL=openai/gpt-oss-120b` · `TOOL_PARSER=…` | ~60 GB (MXFP4) | Apache-2, native tools |
| Llama-3.3-70B-Instruct (bf16) | `LLM_MODEL=meta-llama/Llama-3.3-70B-Instruct` · `TOOL_PARSER=llama3_json` + set `HUGGING_FACE_HUB_TOKEN` | ~140 GB | won't fit unquantised on 96 GB |

**`TOOL_PARSER` must match the model family** (`hermes` for Qwen, `llama3_json`
for Llama 3.x, `mistral` for Mistral) or `tool_choice=auto` won't emit tool calls
— and the copilot *needs* tool calls (every number it states comes from one).

## Security
- `VLLM_API_KEY` is a shared secret the endpoint requires (`--api-key`). The
  copilot stores it in the backend's memory only and never returns it. **Change
  the default.**
- Don't expose the port to the public internet. Keep it on the same private
  network as the VisIVO backend, or reach it over the same SSH tunnel.
- The VisIVO copilot keeps this self-hosted key **separate** from any cloud
  OpenAI/Claude key, so one can never be sent to the other endpoint.

## Troubleshooting
- **Container unhealthy for a while on first run** — normal: it's downloading the
  model (`start_period` is 15 min). Watch `docker compose logs -f`.
- **OOM / won't load** — lower `MAX_MODEL_LEN`, raise `GPU_UTIL` toward 0.95, or
  pick a smaller/quantized model.
- **`validate.sh` step 3 warns "no tool_calls"** — fix `TOOL_PARSER` for your
  model and ensure `ENABLE_TOOLS=1`.
- **GPU not visible** — install nvidia-container-toolkit and
  `sudo nvidia-ctk runtime configure --runtime=docker && sudo systemctl restart docker`.
