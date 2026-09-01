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

## Rootless Podman on RHEL / AlmaLinux — the setup that actually works ⭐

Battle-tested on AlmaLinux 9 + rootless Podman 5 + an RTX PRO 6000 (Blackwell).
On this stack the Docker Compose path is fragile; use the **direct `podman run`**
command below. Do these one-time host steps FIRST, then run:

```bash
# 1) GPU via CDI (root, once) — the Swarm-style compose GPU block is ignored by
#    podman-compose, so Podman needs a CDI device instead:
sudo dnf install -y nvidia-container-toolkit
sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
nvidia-ctk cdi list        # must show nvidia.com/gpu=all

# 2) Keep the container alive after you log out (rootless Podman otherwise KILLS
#    all your processes when the last SSH session closes — this is the #1 gotcha):
loginctl enable-linger "$USER"   # no root needed; verify: loginctl show-user "$USER" -p Linger

# 3) Open the API port in the firewall (root, once):
sudo firewall-cmd --add-port=8000/tcp --permanent && sudo firewall-cmd --reload

# 4) Config + run (from deploy/copilot-llm, with .env set — see below):
cp .env.example .env      # set VLLM_API_KEY; set HUGGING_FACE_HUB_TOKEN (see note)
podman run -d --name visivo-copilot-llm --restart unless-stopped \
  --network=host \                       # avoids the flaky rootless port-forwarder (pasta)
  --device nvidia.com/gpu=all \          # CDI GPU
  --security-opt label=disable \         # SELinux would deny the container NVML access
  --ipc=host \
  --env-file "$PWD/.env" \
  -v "$PWD/entrypoint.sh:/entrypoint.sh:ro,Z" \
  -v "$PWD/hf-cache:/root/.cache/huggingface:Z" \
  --entrypoint /bin/sh docker.io/vllm/vllm-openai:latest /entrypoint.sh
#  ^ fully-qualified image name: short-name-mode=enforcing can't prompt over SSH.
podman logs -f visivo-copilot-llm        # wait for "Application startup complete"
./validate.sh
```

Then point the copilot ⚙ → "Your server" at `http://<server-ip>:8000/v1` with your
`VLLM_API_KEY`.

**Two `.env` notes that bit us:**
- **`HUGGING_FACE_HUB_TOKEN`** — set it (free HF account → Settings → Access
  Tokens, `read`). Unauthenticated bulk downloads of a 32B model get rate-limited
  by HF and **stall** even on a fast link; a token fixes it. (Qwen2.5-32B is
  ungated, so the token is only for the rate limit.)
- **No inline comments on value lines** in `.env` — the env-file parser folds them
  into the value (e.g. `QUANTIZATION=  # note` breaks vLLM's args).

**Startup speed:** the first start compiles CUDA graphs for the GPU, which on a
brand-new arch (Blackwell) can take 15–20 min and doesn't persist across restarts.
For a copilot (low throughput) add **`EXTRA_ARGS=--enforce-eager`** to `.env` to
skip compilation — startup drops to ~2 min with a negligible latency cost.

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

## Full stack — backend + LLM together (one command)
If you want the **VisIVO backend and the LLM on the same server**, pre-wired so
the copilot works with no extra config:
```bash
cd deploy/copilot-llm
cp .env.example .env               # set VLLM_API_KEY and VISIVO_TOKEN
DATA_DIR=/path/to/fits docker compose -f docker-compose.full.yml up -d --build
```
Then point the **VisIVO desktop app** at `http://<this-server-ip>:8080` with the
`VISIVO_TOKEN` you set. The copilot is already connected to the internal LLM.

**Coexistence:** the LLM uses the GPU; the backend runs **CPU-only** compute
(moments / spectral / photometry / copilot), so they share the machine fine.
Server-side *rendering* (the GPU WebRTC path) is **not** enabled in this backend
image — if you need it, run the backend on the host with the EGL build instead,
and keep only the LLM in a container. The first `--build` is large (the
scientific stack: VTK, numba, astropy, dask, …) and the first `up` downloads the
model — both are one-time.

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
- **(rootless Podman) container exits within seconds; logs show `acquiring lock N:
  file exists`** — the SHM lock segment is stale/corrupt. Fix:
  `rm -f /dev/shm/libpod_rootless_lock_* && podman system renumber`.
- **(rootless Podman) container dies the moment your SSH session closes** (often
  seen as an unexplained `SIGTERM` / "terminated" mid-startup) — you didn't enable
  linger. Run `loginctl enable-linger "$USER"` and restart the container.
- **External `curl http://host:8000/health` resets the connection, but it works
  from inside the container** (`podman exec … curl localhost:8000/health`) — the
  rootless port-forwarder (pasta) is failing. Use `--network=host` (host binds the
  port directly). Also check the firewall (`firewall-cmd --add-port=8000/tcp`).
- **Model download stalls at a few MB** despite good bandwidth — HF is rate-limiting
  the unauthenticated download. Set `HUGGING_FACE_HUB_TOKEN` in `.env`.
- **OOM / won't load** — lower `MAX_MODEL_LEN`, raise `GPU_UTIL` toward 0.95, or
  pick a smaller/quantized model.
- **`validate.sh` step 3 warns "no tool_calls"** — fix `TOOL_PARSER` for your
  model and ensure `ENABLE_TOOLS=1`.
- **GPU not visible** — install nvidia-container-toolkit and
  `sudo nvidia-ctk runtime configure --runtime=docker && sudo systemctl restart docker`.
