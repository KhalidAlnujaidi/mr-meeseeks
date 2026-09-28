#!/usr/bin/env bash
# audit_capture_run.sh — INSTRUMENTED short golem rerun for the prompt-identity
# audit (plan 2026-09-26_211500, Task 1.2).
#
# Why this exists as a script rather than a shell one-liner: the harness's own
# zero-network law (F93) requires the API-key env scrubbed, and ISO_PROXY_CAPTURE=1
# must be set on the PROXY process (bodies are never retroactive — see the audit's
# FAIL branch). Both are easy to get wrong in an ad-hoc invocation, and a captured
# log with missing req_body silently degrades the audit to a no-measurement.
#
# Usage: IDS=T1,T9,T10 ISO_REPS=3 ./audit_capture_run.sh
#   Outputs: out/runner-audit-<TID>-<batch>.json   (per-replica windows)
#            out/proxy-audit-<batch>.jsonl         (req_body capture)
#   F125: both are BATCH-SUFFIXED; a colliding batch name REFUSES rather than
#   truncating (the old `: >` here would destroy the previous capture).
#   Then:    python3 audit_replica_prompt_identity.py \
#              out/runner-audit-<TID>-<batch>.json out/proxy-audit-<batch>.jsonl
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
ENGINE_PORT="${ENGINE_PORT:-8081}"
MODEL_ID="${MODEL_ID:-olmoe-leaf}"
PP="${AUDIT_PROXY_PORT:-8097}"
IDS="${IDS:-T1,T9,T10}"
REPS="${ISO_REPS:-3}"
SANDBOX="$OUT/sandbox-audit"

export ISO_REPS="$REPS"
export ISO_RUN_BATCH="${ISO_RUN_BATCH:-AUDIT-PROMPTID-$(date -u +%Y%m%dT%H%M%SZ)}"
export ISO_SAMPLE="${ISO_SAMPLE:-0}"

# F125: batch-suffixed evidence; a colliding batch name refuses. The old
# `: >` here truncated the capture in place — a second run would have
# destroyed the only surviving F124 audit capture.
PLOG="$OUT/proxy-audit-$ISO_RUN_BATCH.jsonl"
if [ -e "$PLOG" ]; then
  echo "[audit-run] REFUSING: batch evidence already exists: $PLOG" >&2
  echo "  (F125: choose a new ISO_RUN_BATCH, or move the old files aside.)" >&2
  exit 1
fi
: > "$PLOG"
echo "[audit-run] ids=$IDS reps=$REPS batch=$ISO_RUN_BATCH proxy=:$PP"

# Golem owns the replica loop internally (F114) => one invocation per TASK,
# ISO_REPS replicas inside. Do NOT loop replicas here (that is the F114 bug).
env -u PYTHONPATH -u OPENROUTER_API_KEY -u OPENAI_API_KEY -u ANTHROPIC_API_KEY \
    -u TYPESAFE_API_KEY ISO_PROXY_CAPTURE=1 \
    python3 "$HERE/proxy.py" "$PP" "$ENGINE_PORT" "$PLOG" \
    > "$OUT/proxy-audit-$ISO_RUN_BATCH.err" 2>&1 &
PXY=$!
sleep 1
curl -s -m 3 "http://127.0.0.1:$PP/v1/models" -o /dev/null || {
  echo "[audit-run] proxy failed to start"; cat "$OUT/proxy-audit-$ISO_RUN_BATCH.err"; exit 1; }

for tid in $(echo "$IDS" | tr ',' ' '); do
  python3 "$HERE/seed_sandbox.py" "$HERE/tasks.json" "$tid" "$SANDBOX"
  rout="$OUT/runner-audit-$tid-$ISO_RUN_BATCH.json"
  if [ -e "$rout" ] || [ -e "$rout.crashed" ]; then
    echo "[audit-run] REFUSING: evidence for batch $ISO_RUN_BATCH already exists: $rout" >&2
    exit 1
  fi
  ISO_WARMUP=1 env -u OPENROUTER_API_KEY -u OPENAI_API_KEY -u ANTHROPIC_API_KEY \
      -u TYPESAFE_API_KEY \
      "$HERE/golem-runner" "http://127.0.0.1:$PP/v1/chat/completions" "$MODEL_ID" \
      "$SANDBOX" "$HERE/tasks.json" "$rout" "$tid" \
      > "$OUT/audit-$tid-$ISO_RUN_BATCH-stdout.log" 2>&1
  rc=$?
  n=$(python3 -c "import json;print(len(json.load(open('$rout'))['results']))" 2>/dev/null || echo NA)
  echo "[audit-run] $tid runner exit=$rc rows=$n"
done

kill $PXY 2>/dev/null || true
wait $PXY 2>/dev/null || true
echo "[audit-run] done; chat entries in proxy log: $(grep -c chat/completions "$PLOG")"
