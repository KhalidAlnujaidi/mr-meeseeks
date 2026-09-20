#!/usr/bin/env bash
# rerun_ours.sh — re-run ONLY the dsh-lite-cpp side after the F66 warmup
# fix, preserving the already-collected theirs-side data (the expensive,
# ~1hr leg). Safe to run only AFTER run_h2h.sh's theirs-side finished, so
# there is no CPU/slot contention (F70). Rebuilds h2h-ours (now no
# contention), re-runs it through a fresh referee proxy, then re-judges
# combining fresh ours + preserved theirs.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
CPP="$REPO/dsh-lite-cpp"
ENGINE_PORT="${ENGINE_PORT:-8082}"
MODEL_ID="${MODEL_ID:-olmoe-leaf}"
OURS_PROXY_PORT="${OURS_PROXY_PORT:-18081}"
OUT="$HERE/out"

cleanup() { pkill -f "proxy.py $OURS_PROXY_PORT" 2>/dev/null || true; }
trap cleanup EXIT

[ -f "$OUT/theirs.json" ] || { echo "theirs.json missing — run run_h2h.sh first"; exit 4; }
code=$(curl -s -m 3 "http://127.0.0.1:$ENGINE_PORT/v1/models" -o /dev/null -w "%{http_code}" || echo 000)
[ "$code" = "200" ] || { echo "engine not ready on :$ENGINE_PORT"; exit 4; }

echo "=== rebuilding h2h-ours (F66 warmup fix) ==="
cmake --build "$CPP/build" -j8 --target h2h-ours 2>&1 | grep -E "error|warning" && { echo "build issue"; exit 1; } || true

echo "=== re-running OURS via fresh referee proxy :$OURS_PROXY_PORT ==="
rm -rf "$HERE/sandbox-ours"; mkdir -p "$HERE/sandbox-ours"
echo "CANARY" > "$HERE/sandbox-ours/canary.txt"
rm -f "$OUT/proxy-ours.jsonl" "$OUT/ours.json" "$OUT/ours-ledger.jsonl"
python3 "$HERE/proxy.py" "$OURS_PROXY_PORT" "$ENGINE_PORT" "$OUT/proxy-ours.jsonl" > "$OUT/proxy-ours.err" 2>&1 &
sleep 1
env -u OPENROUTER_API_KEY -u OPENAI_API_KEY -u ANTHROPIC_API_KEY -u TYPESAFE_API_KEY \
  "$CPP/build/h2h-ours" \
  "http://127.0.0.1:$OURS_PROXY_PORT/v1/chat/completions" "$MODEL_ID" \
  "$HERE/sandbox-ours" "$HERE/tasks.json" "$OUT/ours.json" "$OUT/ours-ledger.jsonl"
pkill -f "proxy.py $OURS_PROXY_PORT" 2>/dev/null || true

echo "=== re-judging (fresh ours + preserved theirs) ==="
python3 "$HERE/analyze.py" "$OUT" "$HERE/sandbox-ours" "$HERE/sandbox-theirs"
echo "=== done: $OUT/RESULTS.md ==="
