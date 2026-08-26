#!/usr/bin/env bash
# Sanity-check the self-hosted copilot LLM endpoint: it is up, lists the model,
# answers a chat completion, AND performs a tool call (the copilot needs tools).
#
# Usage:  ./validate.sh              # reads .env for URL + key
#         BASE_URL=http://host:8000/v1 API_KEY=xxx ./validate.sh
set -euo pipefail

if [ -f .env ]; then
  # shellcheck disable=SC1091
  set -a; . ./.env; set +a
fi
BASE_URL="${BASE_URL:-http://localhost:${LLM_PORT:-8000}/v1}"
API_KEY="${API_KEY:-${VLLM_API_KEY:-}}"
MODEL="${MODEL:-${SERVED_NAME:-visivo-copilot}}"
AUTH=(-H "Authorization: Bearer ${API_KEY}")

echo "→ endpoint: ${BASE_URL}  model: ${MODEL}"

echo "1) /models"
curl -fsS "${AUTH[@]}" "${BASE_URL}/models" | grep -q '"id"' \
  && echo "   ok" || { echo "   FAILED — is the server up / model loaded?"; exit 1; }

echo "2) chat completion"
curl -fsS "${AUTH[@]}" -H "Content-Type: application/json" \
  "${BASE_URL}/chat/completions" -d @- <<JSON | grep -q '"content"' \
  && echo "   ok" || { echo "   FAILED"; exit 1; }
{ "model": "${MODEL}", "max_tokens": 32,
  "messages": [{"role":"user","content":"Reply with the single word: ready"}] }
JSON

echo "3) tool call (the copilot depends on this)"
RESP=$(curl -fsS "${AUTH[@]}" -H "Content-Type: application/json" \
  "${BASE_URL}/chat/completions" -d @- <<JSON
{ "model": "${MODEL}", "max_tokens": 128, "tool_choice": "auto",
  "messages": [{"role":"user","content":"What is the comoving distance at redshift 0.1? Use the tool."}],
  "tools": [{"type":"function","function":{
    "name":"cosmology_distance",
    "description":"Distance for a redshift.",
    "parameters":{"type":"object","properties":{"redshift":{"type":"number"}},"required":["redshift"]}}}] }
JSON
)
if echo "$RESP" | grep -q 'tool_calls'; then
  echo "   ok — model emits tool_calls"
else
  echo "   WARNING — no tool_calls returned. Check --enable-auto-tool-choice and"
  echo "   that TOOL_PARSER matches the model family. The copilot needs tool use."
fi
echo "All checks done."
