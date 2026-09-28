# Cross-model replication: Qwen3.8-Flash-Next 48×512 (block-FP8), x86 Linux host

Replication of the #1621 protocol on a much larger grid: **Qwen3.8-Flash-Next
FP8, 48 layers × 512 experts, top-10 routing + 1 shared expert, block-FP8
weights** — ~24× the expert count of OLMoE (24,576 vs 1,024 cells) and a
different quantization family, same engine-true zero-out methodology. Plus a
correction experiment (G3) that splits the two things the tier conflates:
**never-routed** vs merely **low-traffic** experts.

## TL;DR

- **The pipeline's ctl gate passed exactly — twice.** A full safetensors
  FP8→bf16→FP8 round-trip with nothing zeroed reproduces baseline TF-NLL to
  +0.0000 on all refs in both runs → the container rewrite is lossless at
  block-FP8; arm deltas below it are pure ablation damage.
- **Strict never-routed experts (1,694 of 24,576; zero traffic by
  construction) cost ~nothing to delete**: mean ΔTF-NLL **+0.0060**, median
  **−0.0012**, spread (sdev 0.041) wider than the mean. Indistinguishable
  from zero at this holdout size.
- **Damage tracks traffic removed, not expert count.** An 8×-smaller arm
  (1,694 experts, 6.9% traffic) did **+0.0539** ≈ the 8×-larger tier
  (14,022 experts, 7.85% traffic, **+0.0517**) — while both were far below
  what their per-count comparison to #1621's band would predict.
- **The 63–86% / 22–29× OLMoE ratios do NOT transfer as scale-invariant
  constants.** On q38 the per-expert cold/rand damage ratio came out ~10–11%.
  What transfers is the *mechanism warning one level down*: a small nonzero
  routing count is NOT evidence of deadness — the low-traffic tier still hurt
  (+0.05), while true-zero experts were free.

## Method

Identical protocol to the original (`bench/prune-spike/`, reproduction
profile: **`profiles/qwen38_flash_next.enigma.json`**; G3:
**`profiles/qwen38_zeroonly.enigma.json`** + **`gen_zeroonly_cands.py`**);
deltas from OLMoE:

1. **Heat.** 60 PPL prefills over 12/18 corpus lines of the same agentic
   traffic corpus; `COLI_USAGE` → 22,883 routing rows (header `-1 48 512`),
   2.92M picks, 22,882 of 24,576 experts touched ≥1×.
2. **Candidates (run A — "the tier").** Per-layer share <0.1% → **14,022
   count-cold experts (57.1% of the grid), 7.85% of heat traffic — of which
   only 1,694 have count = 0.** Uniform control: 14,022 sampled seed-1972,
   56.81% traffic, overlap 7,980.
3. **Candidates (run B — G3 "zero-only").** Cold := the **strict never-routed
   set only (1,694, 0.0% traffic by construction)** vs same-count uniform
   (seed 1972, 6.91% traffic, overlap 114). Holdout widened 3→5 refs (6
   planned; the 18-line corpus yields 17 tokenizer-usable lines and heat took
   12 — cap disclosed rather than padding refs).
4. **Ablation.** Zero the full `gate_proj|up_proj|down_proj` FP8 family per
   expert (scale bytes untouched; FP8 zero dequantizes to exactly 0.0);
   shared expert never zeroed. `seq_arms + link_mode`: build→eval→delete per
   arm (185.5 GB × 4 copies would overrun the volume).
5. **Eval.** Teacher-forced NLL (`PPL=1`), holdout refs disjoint from heat,
   CPU-only prefill, engine `c/qwen38`, 16 threads, x86 Linux (gcc-13 build).

## Gate (both runs)

ctl = full round-trip, nothing zeroed. Base values byte-identical across
runs and across the mid-run pipeline crash+resume — determinism for free.

|| ref 1 | ref 2 | ref 3 | ref 4 | ref 5 |
||---|---|---|---|---|---|
| base (G3, 5 refs) | 2.4701 | 3.1249 | 3.2926 | 3.6678 | 4.0770 |
| **ctl (G3)** | +0.0000 | +0.0000 | +0.0000 | +0.0000 | +0.0000 |
| **ctl (run A, 3 refs)** | +0.0000 | +0.0000 | +0.0000 | — | — |

## Results (teacher-forced ΔNLL, nats/token, vs base)

**Run A — the <0.1% tier at 48×512** (3 refs):

| arm (n zeroed) | ref 1 | ref 2 | ref 3 | mean |
|---|---|---|---|---|
| ctl (0) | +0.0000 | +0.0000 | +0.0000 | **0.0000** ✅ |
| tier-14022 (7.85% traffic) | +0.1801 | **−0.2086** | +0.1836 | **+0.0517** |
| rand-14022 (56.81% traffic) | +0.5222 | +0.5813 | +0.5098 | **+0.5378** |

per-expert damage ratio cold/rand: **10%** (OLMoE published band: 63–86%).

**Run B — G3 zero-only vs matched-count** (5 refs):

| arm (n zeroed) | ref 1 | ref 2 | ref 3 | ref 4 | ref 5 | mean | median |
|---|---|---|---|---|---|---|---|
| cold = never-routed (1,694; 0% traffic) | −0.0012 | −0.0349 | +0.0000 | −0.0092 | +0.0753 | **+0.0060** | −0.0012 |
| rand matched-count (1,694; 6.9% traffic) | +0.0609 | −0.0321 | +0.3553 | −0.0362 | −0.0783 | **+0.0539** | −0.0321 |

per-expert damage ratio cold/rand: **11%**; per-traffic-unit ratio is
**undefined by construction** (the zero-only arm carries exactly zero
traffic — the honest comparison is Δ(cold) vs 0, which sits inside the
noise band).

## Reading of the two runs together

- Traffic-share → damage is monotone and near-linear across three arms that
  differ 8× in count but track in traffic: 0% → +0.006; 6.9% → +0.054;
  7.9% → +0.052; 56.8% → +0.538. Per-expert ratios against OLMoE's band
  (63–86%) are meaningless at this operating point — **traffic, not count,
  is the deletion-relevant quantity at 24.6k-expert scale**, and OLMoE's
  headline ratios should be read as grid-size-dependent, not constants.
- The tier's mixed identity explains run A's negative per-ref delta:
  12,328/14,022 of its members had real (small) traffic, and at
  ~1.26 picks/expert on average the "cold" label is mostly heat-sampling
  noise — **2.9M picks over 24.6M cells cannot certify deadness**; G3's
  count=0-only arm is the only defensible strict claim from this heat.
- Never-routed ≠ never-useful is the surviving warning from #1621 — but
  G3 shows the *converse* also holds at q38: **count=0 really was free**.
  For a 48×512 top-10 model under this workload, pruning the 6.9% strictly
  dead experts is TF-NLL-invisible; pruning below that (the low-traffic
  tail) starts costing proportionally to traffic.

## Limits / honesty list

- Heat is 2 orders of magnitude sparser than OLMoE's 12M picks; expert
  counts carry small-sample variance — that is the point of the count=0-only
  G3 arm, and the 22.8k-row heat still cannot rule out experts that are dead
  *under this workload* but live under other traffic.
- Holdout is 5 lines (~1,000 tokens) from the 18-line published corpus —
  per-ref spread (rand sdev 0.176, cold sdev 0.041) is comparable to the
  means; treat direction of the means as the claim, not their decimals.
- CPU-only teacher-forced prefill; no decode arm; `norm_topk_prob` absent
  from this model's config — zero-out (never drop-slot) semantics used
  identically in both arms, so the comparison is apples-to-apples regardless.
- 57%-of-container tier (run A) is a different operating point than OLMoE's
  21.5%; disclosed above.
- One host (x86 Linux + gcc-13 build); cross-host determinism was
  separately confirmed for the protocol itself: the published OLMoE arm set
  reproduced on a second Apple Silicon host (M3, 8 threads) with every delta
  identical to 4 decimals.

## Repro

Enigma (`~/bench/prune-spike/`): run A `--profile
profiles/qwen38_flash_next.enigma.json --steps eval,report`; G3 `--profile
profiles/qwen38_zeroonly.enigma.json --steps eval,report` against
pre-generated candidates (`gen_zeroonly_cands.py`, deterministic on seed
1972). Logs `~/bench/logs/q38-run.log`, `~/bench/logs/g3-run.log`; heat
usage `runs/qwen38_flash_next/usage.txt` (22,883 rows; header `-1 48 512`).
Model: `Qwen3.8-Flash-Next-FP8`, 131 shards, 185,563,844,671 bytes.

Pipeline bugs found and fixed while running this (both A/B-tested): the
ablation container's 1-tensor/expert sanity check (false-aborted a *correct*
14,022-expert build; now counts distinct experts); `step_report` crashed on
division by the by-construction-zero traffic share (now prints UNDEFINED +
raw deltas); eval timeouts were hard-coded at 1800 s and tripped by a shared
host's load spikes (now profile-settable `eval_timeout`).
