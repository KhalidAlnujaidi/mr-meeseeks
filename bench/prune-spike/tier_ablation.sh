#!/usr/bin/env bash
# Ablation tiers on OLMoE: zero candidate lists of increasing depth, TF-NLL each.
# Tier sizes from the traffic-share table: coldest 51 / 102 / 220 (cap-free list).
set -e
cd "$(dirname "$0")"
REF=../../colibri/ref.json
for N in 51 102 220; do
  /opt/homebrew/bin/python3.12 - "$N" <<'PY'
import sys
n = int(sys.argv[1])
rows = [ln.split() for ln in open('candidates_full.txt') if not ln.startswith('#')]
rows = sorted(((int(c), int(l), int(e)) for l, e, c in rows))[:n]   # global-count order
open(f'/tmp/cand_tier_{n}.txt','w').write(''.join(f"{l} {e} {c}\n" for c, l, e in rows))
PY
  ../../colibri/.venv/bin/python ablate_container.py --candidates /tmp/cand_tier_$N.txt \
    --out /tmp/olmoe_tier$N > /tmp/tier_build_$N.log 2>&1
  echo "=== tier $N experts zeroed ==="
  SNAP=/tmp/olmoe_tier$N PPL=1 ../../colibri/c/olmoe 64 8 $REF 2>&1 | grep TF-NLL
done
