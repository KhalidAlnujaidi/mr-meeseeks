# Routing-count coldness badly overestimates expert prunability — measured on OLMoE

Raw evidence: `usage_full.txt`, `candidates_full.txt`, `tier_eval_results.json`,
scripts in this directory. Engines: colibri `main` @ a8f2ca6 (v1.11.0), `c/olmoe`
default build, macOS 26.5.1, Apple M5 Pro (12 OMP threads), 24 GB.

## Question

`.coli_usage` tells you which experts the router rarely picks. The tempting move
is to treat the rare tail as removable noise — the REAP-style question. This is
a controlled measurement of whether routing **count** predicts causal
**importance**, on a model small enough to ablate every candidate engine-true.

## Method

1. **Workload.** 10,579 OLMoE tokens of real agentic traffic — user/assistant/
   tool text extracted from 4 local agent session logs (deduped, per-session
   capped, deterministic selection: `extract_corpus.py`).
2. **Heat.** Fed through `c/olmoe` CHAT mode, `MAX_NEW=1`, one `/reset` per
   line — 11,993,600 routing decisions across 16 layers × 64 experts
   (`run_heat.sh` → `.coli_usage` via `COLI_USAGE=`).
3. **Candidates.** Ranked by count; zero-route set + sub-0.1%-of-layer tail
   (`analyze_candidates.py`).
4. **Causal ablation.** Candidate experts' `merged_weight` tensors zeroed in a
   full container copy (int8 zeros dequantize to exactly 0.0; OLMoE has
   `norm_topk_prob=false` so no renormalization side-effects — `ablate_container.py`).
   Tier sizes 51 / 102 / 220.
5. **Eval.** Teacher-forced NLL (`PPL=1`) on held-out corpus lines, 202 scored
   tokens each, all containers through the same safetensors round-trip, plus a
   **control container (round-trip, nothing zeroed)** to prove the pipeline is
   loss-free. 4 arms × 3 refs × 2 reps, process rotation per block
   (`tier_eval.py`).
6. **Uniform control arm (added after the first post).** 220 experts drawn
   uniformly at random, seed 1972, without consulting the counts
   (`make_uniform_arm.py` → `/tmp/olmoe_uniform`, audited 220/220 zeroed);
   same refs, same containers-pipeline, base/cold/rand rotated
   (`armc_eval.py` → `armc_results.json`).

## Results

Count distribution at 12.0M picks:

| set | size | share of all routing traffic |
|---|---|---|
| zero-route (truly count-dead) | **2 / 1024** | 0% |
| coldest 51 (<0.03% band) | 5.0% of container | 0.03% |
| coldest 102 | 10% | 0.14% |
| coldest 220 (<0.1% tail) | 21.5% | 0.66% |

(Pilot with only 24 routed tokens "found" 140 zero-route experts — a pure
small-sample artifact; at 12M picks 1022/1024 experts route at least once.)

Causal TF-NLL deltas vs baseline, per 202-token sequence (all values bit-stable
across reps/processes — greedy TF is deterministic, so these deltas are exact,
not noisy):

| tier | ref A | ref B | mean ΔNLL (nats/token) |
|---|---|---|---|
| control (0 zeroed) | ±0.0000 | ±0.0000 | 0.0000 |
| coldest 51 | +0.0335 | +0.1170 | +0.061 |
| coldest 102 | +0.0636 | +0.2049 | +0.111 |
| coldest 220 | +0.2487 | +0.3302 | +0.276 |
| **220 uniform-random (added control)** | **+0.3932** | **+0.3841** | **+0.389** |

Note ref A/B are two distinct held-out sequences (a fixture slip duplicated
one — ref1≡ref2; the table uses ref1 and ref3), so treat sequence spread as
directional at n=2 — the monotonicity across tiers within each sequence is the
load-bearing part. Uniform arm: seed 1972, drawn without consulting counts;
carries 22.51% of measured traffic (`arm_shares.json`, `armc_results.json`);
50 experts overlap the cold arm — a no-op for same-base comparisons.

## Findings

0. **Warm decode speed is unchanged** by zeroing 220 experts (baseline
   [12.20, 13.85, 15.66] vs t220 [14.41, 13.94, 15.74] tok/s, ABABAB
   interleaved, warm page cache): medians 13.85 vs 14.41, distributions
   overlap rep-by-rep → `no-change`. Zeroed experts keep full byte layout, so
   the ablation isolates *information loss* from *I/O savings* — the quality
   deltas below are pure causality, and any real pruning win must come from
   actually removing bytes (slot ejection / REAP-style re-layout), not from
   this arm.
1. **Count-dead ≈ doesn't exist.** 2/1024 experts never routed under 12M
   picks of real agentic traffic. Pruning by "never seen" removes ~0.2% of the
   container, not 10-20%.
2. **Count-cold ≠ causal-cold.** Per expert removed, the cold tail costs
   63–86% of what a uniformly-random expert costs (+0.2487/+0.3302 vs
   +0.3932/+0.3841) while each cold expert carries only ~3% of the traffic
   share of a random one (0.66% vs 22.5% arm totals). Per unit of routing
   traffic, the cold tail is **22–29× more load-bearing than the average
   expert**. Specialists: rarely asked, right when asked. (The
   sequence-dependence holds across arms: cold +0.25→+0.33, rand
   +0.39→+0.38 between refs A and B.)
3. **The count is a placement signal, not a deletion signal** — which is
   exactly how colibri uses it (PIN/auto-pin/AUTOPIN). Nothing here says MoE
   models have no removable noise; it says *routing counts, even at 12M
   picks, are the wrong lens*, consistent with why REAP uses activation-
   importance reconstruction rather than hit counts.
4. **For a disk-streamed model, the cheap win is the other direction:** those
   220 tail experts cost real streaming bandwidth on the rare hits. A tier
   that keeps them on the slowest storage (or, as here, proves they cannot be
   deleted, informing the `coli plan` side) is more defensible than pruning.

## Reproduce

```sh
python3 extract_corpus.py --limit 40 --per-file 3 --max-tokens 10000 --out corpus.txt
bash run_heat.sh                                  # ~15 min on M5 Pro; writes usage_full.txt
python3 analyze_candidates.py usage_full.txt --out candidates_full.txt
bash tier_ablation.sh                             # builds /tmp/olmoe_tier{51,102,220}, TF-NLLs each
python3 tier_eval.py                              # paired 4-arm eval -> tier_eval_results.json
python3 make_uniform_arm.py                       # seed-1972 uniform control list + traffic shares
python3 ablate_container.py --candidates candidates_uniform.txt --out /tmp/olmoe_uniform
python3 armc_eval.py                              # base/cold/rand -> armc_results.json
```

Caveats: single model (OLMoE-1B-7B int8, 64 experts/layer, top-8, no gate
renorm); workload is one user's agent logs (English, code/ops-heavy); ablation
zeroes weights (router still *can* pick them, and does — that's where the
damage comes from; a slot-ejection prune would additionally renormalize-free
fail differently); TF-NLL on 2×202 held-out tokens is a proxy for coherence,
not a task benchmark. A `coli bench` run on the tiers would strengthen it.
