#!/usr/bin/env bash
# G3 corrected experiment: strict zero-count cold vs matched-count uniform,
# 6-ref widened holdout, on qwen38 (block-FP8 48x512). Reuses the proven
# usage.txt from the main run — only candidates + eval refs are new.
set -euo pipefail
cd ~/bench/prune-spike
W=runs/qwen38_zeroonly
M=~/models/Qwen3.8-Flash-Next-FP8
echo "[g3] $(date) init workdir"
mkdir -p $W
cp runs/qwen38_flash_next/usage.txt $W/usage.txt
[ -f $W/usage.txt ] && [ "$(wc -l < $W/usage.txt)" -gt 20000 ] || { echo "[g3] ABORT usage"; exit 1; }

echo "[g3] $(date) candidates (zero-only + matched-count uniform seed 1972)"
/usr/bin/python3 gen_zeroonly_cands.py $W/usage.txt --outdir $W
n_c=$(grep -vc '^#' $W/candidates_cold.txt); n_r=$(grep -vc '^#' $W/candidates_rand.txt)
[ "$n_c" -eq 1694 ] && [ "$n_r" -eq 1694 ] || { echo "[g3] ABORT candidate counts cold=$n_c rand=$n_r"; exit 1; }

echo "[g3] $(date) eval refs (corpus lines 13+, 210-tok window like main run)"
# corpus yields 17 tokenizer-usable lines; heat took the first 12 -> 5 eval
# refs are the honest max (design wanted 6; disclose in the writeup)
/usr/bin/python3 make_ppl_refs.py --model $M \
  --corpus runs/qwen38_flash_next/corpus.txt --prefix $W/eval_ref \
  --skip 12 --count 6 --max-tokens 210
NREF=$(ls $W/eval_ref_*.json 2>/dev/null | wc -l)
[ "$NREF" -ge 5 ] || { echo "[g3] ABORT refs (got $NREF)"; exit 1; }
echo "[g3] eval refs: $NREF"
grep -q schema_version $W/eval_ref_1.json || { echo "[g3] ABORT schema"; exit 1; }

echo "[g3] $(date) eval (seq_arms: ctl rebuild re-proves gate, then cold, rand)"
/usr/bin/python3 run_experiment.py --profile profiles/qwen38_zeroonly.enigma.json --steps eval
echo "[g3] $(date) report"
/usr/bin/python3 run_experiment.py --profile profiles/qwen38_zeroonly.enigma.json --steps report
echo "[g3] $(date) ALL DONE"
