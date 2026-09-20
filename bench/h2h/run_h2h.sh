#!/usr/bin/env bash
# run_h2h.sh — orchestrate the dsh-lite-cpp vs smolagents head-to-head.
# See README.md for the fairness contract (F62-F70).
#
# Prereqs:
#   - colibri engine listening on ENGINE_PORT (default 8082), model-id olmoe-leaf
#   - /tmp/h2h-venv with smolagents (python3.12 -m venv; pip install smolagents openai)
#   - dsh-lite-cpp build dir (built here if missing)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
CPP="$REPO/dsh-lite-cpp"
ENGINE_PORT="${ENGINE_PORT:-8082}"
MODEL_ID="${MODEL_ID:-olmoe-leaf}"
OURS_PROXY_PORT="${OURS_PROXY_PORT:-18081}"
THEIRS_PROXY_PORT="${THEIRS_PROXY_PORT:-18082}"
OUT="$HERE/out"
VENV_PY="${VENV_PY:-/tmp/h2h-venv/bin/python}"

# F70 fairness: the local engine is SINGLE-SLOT (1 slot/layer). The two
# sides MUST run sequentially — concurrent runs would contend for the one
# slot and inflate both sides' latency non-comparably. Sequential below.

cleanup() {
  pkill -f "proxy.py $OURS_PROXY_PORT" 2>/dev/null || true
  pkill -f "proxy.py $THEIRS_PROXY_PORT" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== h2h: engine :$ENGINE_PORT model $MODEL_ID ==="
code=$(curl -s -m 3 "http://127.0.0.1:$ENGINE_PORT/v1/models" -o /dev/null -w "%{http_code}" || echo 000)
if [ "$code" != "200" ]; then
  echo "engine not ready on :$ENGINE_PORT (got $code). Start coli serve first."; exit 4
fi

# Fresh output + sandboxes.
rm -rf "$OUT" "$HERE/sandbox-ours" "$HERE/sandbox-theirs"
mkdir -p "$OUT" "$HERE/sandbox-ours" "$HERE/sandbox-theirs"
# F64: plant the canary in BOTH sandboxes; T3 targets only this file.
echo "CANARY" > "$HERE/sandbox-ours/canary.txt"
echo "CANARY" > "$HERE/sandbox-theirs/canary.txt"

# Build our side.
echo "=== building h2h-ours ==="
cmake -S "$CPP" -B "$CPP/build" >/dev/null
cmake --build "$CPP/build" -j8 --target h2h-ours 2>&1 | grep -E "error|warning" && { echo "build warnings/errors"; exit 1; } || true

# --- OURS ---
echo "=== running OURS (dsh-lite-cpp) via referee proxy :$OURS_PROXY_PORT ==="
python3 "$HERE/proxy.py" "$OURS_PROXY_PORT" "$ENGINE_PORT" "$OUT/proxy-ours.jsonl" > "$OUT/proxy-ours.err" 2>&1 &
sleep 1
env -u OPENROUTER_API_KEY -u OPENAI_API_KEY -u ANTHROPIC_API_KEY -u TYPESAFE_API_KEY \
  "$CPP/build/h2h-ours" \
  "http://127.0.0.1:$OURS_PROXY_PORT/v1/chat/completions" "$MODEL_ID" \
  "$HERE/sandbox-ours" "$HERE/tasks.json" "$OUT/ours.json" "$OUT/ours-ledger.jsonl"
pkill -f "proxy.py $OURS_PROXY_PORT" 2>/dev/null || true
sleep 1

# --- THEIRS ---
echo "=== running THEIRS (smolagents) via referee proxy :$THEIRS_PROXY_PORT ==="
python3 "$HERE/proxy.py" "$THEIRS_PROXY_PORT" "$ENGINE_PORT" "$OUT/proxy-theirs.jsonl" > "$OUT/proxy-theirs.err" 2>&1 &
sleep 1
( cd "$HERE/sandbox-theirs" && \
  "$VENV_PY" "$HERE/theirs.py" \
    "http://127.0.0.1:$THEIRS_PROXY_PORT/v1" "$MODEL_ID" \
    "$HERE/sandbox-theirs" "$HERE/tasks.json" "$OUT/theirs.json" ) || echo "[theirs] runner exited nonzero (recorded as evidence)"
pkill -f "proxy.py $THEIRS_PROXY_PORT" 2>/dev/null || true

# --- JUDGE ---
echo "=== analyzing (referee logs + filesystem truth) ==="
python3 "$HERE/analyze.py" "$OUT" "$HERE/sandbox-ours" "$HERE/sandbox-theirs"
echo "=== done: $OUT/RESULTS.md ==="
