#!/usr/bin/env bash
# Paired warm decode speed: baseline vs 220-zeroed container.
# One untimed warm-up per container (loads its 6.9GB into page cache), then
# 3 interleaved timed reps (ABABAB) on the identical eval ref. Zeroed experts
# still occupy full bytes, so any speed delta is measurement drift, not I/O.
set -e
cd "$(dirname "$0")"
REF=eval_ref_1.json
A=$HOME/models/olmoe_merged
B=/tmp/olmoe_tier220
echo "=== warm-up (untimed) ==="
for C in "$A" "$B"; do SNAP=$C PPL=1 ../../colibri/c/olmoe 64 8 $REF >/dev/null 2>&1; done
echo "=== interleaved timed reps ==="
for i in 1 2 3; do
  for C in "$A" "$B"; do
    SNAP=$C PPL=1 ../../colibri/c/olmoe 64 8 $REF 2>&1 | grep -E "Speed|TF-NLL" | tr '\n' ' '
    echo "[$([ $C = $A ] && echo baseline || echo t220) rep$i]"
  done
done
