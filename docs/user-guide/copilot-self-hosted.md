# Self-hosted copilot LLM

Run an **open-weight LLM on your own GPU server** and point the VisIVO
[Copilot](copilot) at it. Your data and prompts never leave your infrastructure,
there is no per-token cost, and the model is pinned and reproducible.

The copilot talks to any **OpenAI-compatible** endpoint, so the repository ships
a turnkey package under `deploy/copilot-llm/` that serves one with a single
container. Two flavours:

| | Image | Best for | Endpoint |
|---|---|---|---|
| **vLLM** (recommended) | `vllm/vllm-openai` | throughput, production | `http://host:8000/v1` |
| **Ollama** (easiest) | `ollama/ollama` | quick start, prototyping | `http://host:11434/v1` |

## Prerequisites (on the GPU host)

- NVIDIA driver + **nvidia-container-toolkit** (`nvidia-ctk`).
- **Docker** *or* rootless **Podman** with a compose provider.
- Disk for the model cache (a 32–70 B model is ≈ 30–70 GB).

## Quick start — Docker + vLLM

```bash
cd deploy/copilot-llm
cp .env.example .env          # set VLLM_API_KEY; set HUGGING_FACE_HUB_TOKEN (see below)
docker compose up -d          # first run downloads the model into ./hf-cache
docker compose logs -f        # wait for "Application startup complete"
./validate.sh                 # checks /models, a chat completion, AND a tool call
```

Then in **Copilot ⚙ → "Your server"**: URL `http://<server-ip>:8000/v1`, Key = the
`VLLM_API_KEY` you set.

## Rootless Podman on RHEL / AlmaLinux — the setup that actually works ⭐

On rootless Podman the Swarm-style compose GPU block is ignored and the
port-forwarder is flaky, so use the **direct `podman run`** below after the
one-time host setup. These are the exact steps validated on AlmaLinux 9 +
Podman 5 + an RTX PRO 6000 (Blackwell):

```bash
# 1) GPU via CDI (root, once) — Podman passes GPUs through the Container Device
#    Interface, not the compose deploy block:
sudo dnf install -y nvidia-container-toolkit
sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
nvidia-ctk cdi list                     # must show nvidia.com/gpu=all

# 2) Keep the container alive after you log out — rootless Podman otherwise KILLS
#    all your processes when the last SSH session closes (the #1 gotcha):
loginctl enable-linger "$USER"          # no root needed
systemctl --user enable --now podman-restart.service   # also restart it on reboot

# 3) Open the API port in the firewall (root, once):
sudo firewall-cmd --add-port=8000/tcp --permanent && sudo firewall-cmd --reload

# 4) Config + run (from deploy/copilot-llm, with .env set):
cp .env.example .env                    # set VLLM_API_KEY and HUGGING_FACE_HUB_TOKEN
podman run -d --name visivo-copilot-llm --restart unless-stopped \
  --network=host \                      # avoids the flaky rootless port-forwarder
  --device nvidia.com/gpu=all \         # CDI GPU
  --security-opt label=disable \        # SELinux would deny the container NVML access
  --ipc=host \
  --env-file "$PWD/.env" \
  -v "$PWD/entrypoint.sh:/entrypoint.sh:ro,Z" \
  -v "$PWD/hf-cache:/root/.cache/huggingface:Z" \
  --entrypoint /bin/sh docker.io/vllm/vllm-openai:latest /entrypoint.sh
#  ^ fully-qualified image name: short-name-mode=enforcing can't prompt over SSH.
./validate.sh
```

## Two `.env` notes that matter

- **`HUGGING_FACE_HUB_TOKEN`** — set it (free HuggingFace account → Settings →
  Access Tokens, `read`). Unauthenticated bulk downloads of a large model get
  rate-limited by HuggingFace and **stall** even on a fast link; a token fixes
  it. (Ungated models like Qwen2.5 need the token only for the rate limit.)
- **No inline comments on value lines** in `.env` — the env-file parser folds
  them into the value (e.g. `QUANTIZATION=  # note` breaks the launch args).

**Startup speed:** the first start compiles CUDA graphs, which on a brand-new GPU
architecture (Blackwell) can take 15–20 min and does not persist across restarts.
For a copilot (low throughput) add **`EXTRA_ARGS=--enforce-eager`** to `.env` to
skip compilation — startup drops to ~2 min at a negligible latency cost.

## Which model?

The default `Qwen/Qwen2.5-32B-Instruct` is ungated, fits an RTX PRO 6000 (96 GB)
in bf16, and does tool-calling well. Larger options (70 B AWQ, gpt-oss-120b) are
in the package README. The **`TOOL_PARSER` must match the model family**
(`hermes` for Qwen, `llama3_json` for Llama 3.x, `mistral` for Mistral) or
`tool_choice=auto` won't emit tool calls — and the copilot *needs* tool calls.

## Full stack (backend + LLM together)

`docker-compose.full.yml` runs the VisIVO backend and the LLM on the same server,
pre-wired so the copilot works with no extra config. The LLM uses the GPU; the
backend runs CPU-only compute, so they share the machine fine.

## Backend requirement

The **VisIVO backend** that talks to your endpoint needs the `openai` Python SDK
installed (`pip install openai`, and it is in `backend/requirements.txt`). It is
the client for every OpenAI-compatible provider — cloud OpenAI *and* your own
server. Without it the "Your server" / ChatGPT providers silently fall back to
*not configured*.

## Troubleshooting

```{list-table}
:header-rows: 1
:widths: 45 55

* - Symptom
  - Fix
* - Copilot shows **Provider: null** after configuring
  - Install `openai` in the backend venv and restart the backend.
* - Model download **stalls** at a few MB
  - Set `HUGGING_FACE_HUB_TOKEN` (HuggingFace rate-limits unauthenticated bulk downloads).
* - (rootless Podman) container **exits within seconds**; logs show `acquiring lock N: file exists`
  - Stale SHM lock: `rm -f /dev/shm/libpod_rootless_lock_* && podman system renumber`.
* - (rootless Podman) container **dies when your SSH session closes**
  - `loginctl enable-linger "$USER"`, then restart the container.
* - External `curl http://host:8000/health` **resets/times out** but works from inside the container
  - Use `--network=host` (bypasses the pasta port-forwarder); open the firewall port.
* - `validate.sh` step 3 warns **no tool_calls**
  - Fix `TOOL_PARSER` for your model family and ensure `ENABLE_TOOLS=1`.
```

See `deploy/copilot-llm/README.md` in the repository for the full reference.
