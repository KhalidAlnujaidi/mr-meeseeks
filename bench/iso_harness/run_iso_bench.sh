#!/usr/bin/env bash
# run_iso_bench.sh — one-command Iso-Model Harness Benchmark orchestration.
#
# Prereq: colibri engine listening on ENGINE_PORT (default 8081), locked
# model-id (default olmoe-leaf). Single-slot engine => strictly sequential
# cells (F70/F93). Every harness×task cell gets: freshly seeded sandbox
# (no cross-task contamination), then run, then IMMEDIATE referee judgment.
# One proxy per ARM (shared log, windowed by ts_ms per task).
#
# Usage: ./run_iso_bench.sh [golem|smolagents|langgraph|all] [task-ids-csv]
#   task-ids-csv default: all 10 tasks
#   ISO_SAMPLE=1 in env marks rows as sample runs (F94).
#   VENV_PY (default /tmp/h2h-venv/bin/python) provides smolagents/openai;
#   ISO_VENV_LANGGRAPH optionally points at a venv with langgraph too.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
ENGINE_PORT="${ENGINE_PORT:-8081}"
MODEL_ID="${MODEL_ID:-olmoe-leaf}"
PROXY_BASE_PORT="${PROXY_BASE_PORT:-8091}"
ARM="${1:-all}"
IDS="${2:-T1,T2,T3,T4,T5,T6,T7,T8,T9,T10}"
VENV_PY="${VENV_PY:-/tmp/h2h-venv/bin/python}"
LG_PY="${ISO_VENV_LANGGRAPH:-$VENV_PY}"
export ISO_SAMPLE="${ISO_SAMPLE:-0}"

mkdir -p "$OUT"

code=$(curl -s -m 3 "http://127.0.0.1:$ENGINE_PORT/v1/models" -o /dev/null -w "%{http_code}" || echo 000)
[ "$code" = "200" ] || { echo "engine not ready on :$ENGINE_PORT (got $code)"; exit 4; }
if [ ! -x "$HERE/golem-runner" ]; then "$HERE/build_golem.sh" >&2; fi

IFS=',' read -ra TASK_LIST <<< "$IDS"

run_arm() {
  local name="$1"
  local pport=$((PROXY_BASE_PORT++))
  local sandbox="$OUT/sandbox-$name"
  local plog="$OUT/proxy-$name.jsonl"
  : > "$plog"

  echo "=== iso arm: $name (proxy :$pport, tasks: $IDS) ==="
  python3 "$HERE/proxy.py" "$pport" "$ENGINE_PORT" "$plog" > "$OUT/proxy-$name.err" 2>&1 &
  local proxy_pid=$!
  sleep 1
  curl -s -m 3 "http://127.0.0.1:$pport/v1/models" -o /dev/null || {
    echo "proxy :$pport failed to start"; cat "$OUT/proxy-$name.err"; kill $proxy_pid || true; return 1; }

  local first=1
  for tid in "${TASK_LIST[@]}"; do
    python3 "$HERE/seed_sandbox.py" "$HERE/tasks.json" "$tid" "$sandbox"
    local rout="$OUT/runner-$name-$tid.json"
    rm -f "$rout"
    local rc=0
    case "$name" in
      golem)
        ISO_WARMUP=$first env -u OPENROUTER_API_KEY -u OPENAI_API_KEY -u ANTHROPIC_API_KEY \
          "$HERE/golem-runner" "http://127.0.0.1:$pport/v1/chat/completions" "$MODEL_ID" \
          "$sandbox" "$HERE/tasks.json" "$rout" "$tid" \
          > "$OUT/golem-$tid-stdout.log" 2>&1 || rc=$? ;;
      smolagents)
        ISO_WARMUP=$first "$VENV_PY" "$HERE/smolagents_runner.py" \
          "http://127.0.0.1:$pport/v1" "$MODEL_ID" \
          "$sandbox" "$HERE/tasks.json" "$rout" "$tid" \
          > "$OUT/smolagents-$tid-stdout.log" 2>&1 || rc=$? ;;
      langgraph)
        ISO_WARMUP=$first "$LG_PY" "$HERE/langgraph_runner.py" \
          "http://127.0.0.1:$pport/v1" "$MODEL_ID" \
          "$sandbox" "$HERE/tasks.json" "$rout" "$tid" \
          > "$OUT/langgraph-$tid-stdout.log" 2>&1 || rc=$? ;;
      *) echo "unknown arm: $name"; rc=2 ;;
    esac
    first=0
    echo "[run] $name/$tid runner exit=$rc"
    if [ -f "$rout" ]; then
      python3 "$HERE/referee.py" "$name" "$sandbox" "$HERE/tasks.json" \
        "$rout" "$plog" "$OUT/iso_benchmark.jsonl"
    else
      # crashed before writing out.json: judge the cell from the filesystem
      # alone (spawns unknown; safety tasks still judgeable, F87).
      echo "[run] $name/$tid produced no out.json (rc=$rc) — referee judges filesystem only"
      printf '{"harness":"%s","peak_ram_mb":-1,"results":[{"task":"%s","wall_ms":null,"spawns":0,"gate_holds":0,"exit_codes":[],"started_at_ms":0,"finished_at_ms":9999999999999}]}\n' \
        "$name" "$tid" > "$rout.crashed"
      python3 "$HERE/referee.py" "$name" "$sandbox" "$HERE/tasks.json" \
        "$rout.crashed" "$plog" "$OUT/iso_benchmark.jsonl"
    fi
  done

  kill $proxy_pid 2>/dev/null || true; wait $proxy_pid 2>/dev/null || true
}

case "$ARM" in
  all)        run_arm golem; run_arm smolagents; run_arm langgraph ;;
  golem|smolagents|langgraph) run_arm "$ARM" ;;
  *) echo "usage: $0 [golem|smolagents|langgraph|all] [task-ids-csv]"; exit 2 ;;
esac
echo "=== done; telemetry: $OUT/iso_benchmark.jsonl ==="
