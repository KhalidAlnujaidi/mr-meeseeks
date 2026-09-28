# qwen38 run 2026-09-20 — crash notes (monitor subagent)

## Classification

**failed — driver crash during the cold-arm container build.** Not a stall:
`pgrep -f run_experiment.py` empty, log ends in a Python traceback at
10:18 +03 (first poll found it dead at ~10:31, dead ~13 min). No
`results.json`, no report. **However the ctl gate arm completed before the
crash and PASSED**, and all heat/candidates data is intact.

## Timeline (enigma, +03)

- 08:58:34 — download verified (131 shards, 185.56 GB), fixtures, heat starts
- 10:01:42 — heat done (60 PPL prefills; usage.txt = 22,883 routing rows);
  cands + uniform; eval step starts (seq_arms: build→eval→delete per arm)
- 10:0x — base eval: `eval_ref_1 2.4701 | eval_ref_2 3.1249 | eval_ref_3 3.2926`
- ctl build (`--zero-all` full round-trip, no links): `zeroed 0/0` →
  ctl eval: `2.4701 / 3.1249 / 3.2926` — **identical to base at engine
  precision on all 3 refs → ctl delta = +0.0000/+0.0000/+0.0000. GATE PASS.**
  The FP8→bf16→FP8 `save_file` container rewrite is lossless on this model.
- cold build: `14022 candidate experts to zero` → `DONE: zeroed
  42066/14022 expert tensors -> /home/enigma/models/q38_tier_cold` →
  `WARNING: -28044 candidates not found in shards` → `ablate_container.py`
  exits 1 → `run_experiment.py step_eval` `subprocess.run(check=True)` raises
  CalledProcessError → **driver dies. Cold was never evaluated; rand never built.**

## Root cause (tooling bug, not a data/ablation problem)

`ablate_container.py` line ~90:

```python
print(f"DONE: zeroed {zeroed}/{len(targets)} expert tensors -> {a.out}")
if zeroed != len(targets):
    sys.exit(f"WARNING: {len(targets)-zeroed} candidates not found in shards")
```

`zeroed` increments once per matched **tensor name** (gate_proj + up_proj +
down_proj = 3 per expert candidate), while `len(targets)` counts **candidate
(l,e) pairs**. The check was written when the only supported family was
OLMoE's single `merged_weight` tensor (1:1), and silently breaks for every
multi-tensor family in `--tensor-res`: for any complete run it computes
`len(targets) - zeroed = -(t-1)·N = -28044` and hard-fails.

The zeroing itself was **100% correct**: 42066 = 3 × 14022 exactly, i.e. every
candidate's full gate/up/down family was found and zeroed; `-28044` is the
unit-mismatch, not missing tensors.

### Suggested fix (NOT applied — pipeline code is off-limits per task constraints)

Track distinct candidate hits instead of tensor counts:

```python
zeroed = 0; hits = set()
...
    if m and (int(m.group(1)), int(m.group(2))) in targets:
        tensors[k] = torch.zeros_like(v)
        zeroed += 1; touched += 1
        hits.add((int(m.group(1)), int(m.group(2))))
...
if len(hits) != len(targets):
    sys.exit(f"WARNING: {len(targets)-len(hits)} candidates not found in shards")
```

## State left on enigma (untouched; I am read-only there)

- `~/models/q38_tier_cold/` — **complete, correctly zeroed cold container**
  (131 shards + config.json). `run_experiment.step_eval` skips the build when
  `config.json` exists, so a re-run reuses it — no rebuild needed.
- `~/bench/prune-spike/runs/qwen38_flash_next/` — usage.txt,
  candidates_cold.txt (14,022), candidates_rand.txt (14,022, seed 1972,
  overlap 7,980), arm_shares.json, all 12 heat refs + 3 eval refs intact.
- Disk: 247 G free.

### Recovery (for the driver owner, not me)

Fix `ablate_container.py:90-91` on the enigma copy, then re-run only
`--steps eval,report` (heat/cands/uniform must NOT rerun). Cost: base+ctl
re-eval, ctl full rewrite ~4 min, cold eval 3 refs (build skipped, container
reused), rand build+eval. From observed pacing, roughly ≤1 h total.

## Data caveats found for the eventual draft

1. **Cold tier here is much fatter than OLMoE's.** `pct=0.1` per-layer-share
   threshold on a 48×512 grid selects 14,022/24,576 experts = **57% of the
   container**, carrying 7.85% of heat traffic. Expected share per expert at
   this density is 0.195%, so "<0.1%" catches ~half the grid. OLMoE's coldest-220
   was 21.5% of container at 0.66% traffic. Per-expert ratios vs the published
   63–86% are NOT like-for-like unless tier size % of container is disclosed;
   the matched-count cold-vs-uniform comparison (both n=14,022) remains valid.
2. Heat = 22,883 routing rows only (~60 prefills); "count-cold" at this sample
   size is noisier than OLMoE's 12M-pick heat. Disclose.
3. Eval = 3 refs of holdout-by-line from the 18-line corpus (12 heat / 3 eval),
   CPU-only prefill, teacher-forced. Disclose per profile notes.

## Evidence pulled locally

- `bench/prune-spike/out/enigma-qwen38-flash-next/q38-run.log.crash` (full 345-line log)
- `bench/prune-spike/out/enigma-qwen38-flash-next/arm_shares.json.crash`
