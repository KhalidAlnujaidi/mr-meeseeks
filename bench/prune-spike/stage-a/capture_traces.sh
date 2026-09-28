#!/usr/bin/env bash
# Capture ROUTE_TRACE for every Stage-A ref, one cold engine process per ref.
#   usage: capture_traces.sh <binary> <refs-dir> <traces-dir>
# One process per ref is the point: policy state is reset between files, so each
# trace is an independent held-out sample rather than a continuation.
set -eu
BIN=${1:?binary}
REFS=${2:?refs dir}
OUT=${3:?traces dir}
MODEL=${COLI_MODEL:-$HOME/models/olmoe_merged}
CAP=${CAP:-64}
mkdir -p "$OUT"
for ref in "$REFS"/*.json; do
  name=$(basename "$ref" .json)
  if [ -s "$OUT/$name.trace" ]; then echo "skip $name (exists)"; continue; fi
  SNAP="$MODEL" PPL=1 ROUTE_TRACE="$OUT/$name.trace" CAP_UNUSED=1 \
    "$BIN" "$CAP" 8 "$ref" >"$OUT/$name.log" 2>&1 || { echo "FAIL $name"; continue; }
  lines=$(wc -l <"$OUT/$name.trace" | tr -d ' ')
  ppl=$(sed -n 's/^TF-NLL: \([0-9.]*\).*/\1/p' "$OUT/$name.log")
  printf '%-26s %6s lines  nll=%s\n' "$name" "$lines" "$ppl"
done
echo "--- captured: $(ls "$OUT"/*.trace 2>/dev/null | wc -l | tr -d ' ') traces, $(cat "$OUT"/*.trace | wc -l | tr -d ' ') records total ---"
