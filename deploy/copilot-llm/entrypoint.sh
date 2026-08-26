#!/bin/sh
# Assemble the vLLM OpenAI-server arguments from the environment (docker-compose
# can't do conditional flags cleanly). All values come from .env.
set -eu

ARGS="--model ${LLM_MODEL} --served-model-name ${SERVED_NAME}"
ARGS="${ARGS} --gpu-memory-utilization ${GPU_UTIL} --max-model-len ${MAX_MODEL_LEN}"

# Tool-calling: required for the copilot. The parser MUST match the model family
# (hermes → Qwen; llama3_json → Llama 3.x; mistral → Mistral; …).
if [ "${ENABLE_TOOLS:-1}" = "1" ]; then
  ARGS="${ARGS} --enable-auto-tool-choice --tool-call-parser ${TOOL_PARSER}"
fi

# Require an API key from clients (paste the same value in the VisIVO copilot).
if [ -n "${VLLM_API_KEY:-}" ]; then
  ARGS="${ARGS} --api-key ${VLLM_API_KEY}"
fi

# Optional quantisation to fit a bigger model (e.g. awq / gptq for a 70B).
if [ -n "${QUANTIZATION:-}" ]; then
  ARGS="${ARGS} --quantization ${QUANTIZATION}"
fi

echo "[copilot-llm] starting vLLM: ${ARGS} ${EXTRA_ARGS:-}"
# shellcheck disable=SC2086
exec python3 -m vllm.entrypoints.openai.api_server ${ARGS} ${EXTRA_ARGS:-}
