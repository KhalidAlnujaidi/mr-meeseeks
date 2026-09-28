A follow-up to the placement-policy discussion (#708) and the REAP support line (#1254, #1615): **can routing counts (.coli_usage heat) identify removable noise experts?** Short answer, measured: no — count-cold experts are specialists, and zeroing them costs quality even when the tail carries 0.66% of traffic. Full protocol + manifest below; negative/no-change results included per the README's request.


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

Note ref A/B are two distinct held-out sequences (a fixture slip duplicated
one), so treat sequence spread as directional at n=2 — the monotonicity across
tiers within each sequence is the load-bearing part.

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
2. **Count-cold ≠ causal-cold.** The coldest 5% of experts carry 0.03% of
   routing traffic yet cost +0.034 to +0.117 nats/token when zeroed, and the
   coldest 21.5% (0.66% of traffic) costs +30% perplexity. Damage is also
   **sequence-dependent** (the same tier costs 3.5× more on a runbook-heavy
   sequence than an email-heavy one). These look like specialists: rarely
   asked, right when asked. (A uniform-random-drop control arm would sharpen
   the per-byte comparison; not run here.)
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
```

Caveats: single model (OLMoE-1B-7B int8, 64 experts/layer, top-8, no gate
renorm); workload is one user's agent logs (English, code/ops-heavy); ablation
zeroes weights (router still *can* pick them, and does — that's where the
damage comes from; a slot-ejection prune would additionally renormalize-free
fail differently); TF-NLL on 2×202 held-out tokens is a proxy for coherence,
not a task benchmark. A `coli bench` run on the tiers would strengthen it.

## Manifest (passes `python3 c/experiment_manifest.py`: `ok (ablated_expert_set, no-change)`)

```json
{
  "version": 1,
  "hypothesis": "Zeroing the 220 experts whose routing counts are below 0.1% of their layer (0.66% of traffic at 12M picks) leaves end-to-end decode speed unchanged (warm, page-cached) while degrading quality; if speed is unchanged, count-cold experts are not free to remove for performance reasons, and the quality delta measures their causal value.",
  "commit": "a8f2ca623ffe9de9df11d56f34d11d2d501493d3",
  "model": "OLMoE-1B-7B-0125-Instruct, colibri merged int8 container (6.9 GB, 16 layers x 64 experts, top-8, norm_topk_prob=false); baseline container vs same container with 220 expert merged_weight tensors zeroed via safetensors round-trip (control arm proved round-trip loss-free: TF-NLL bit-identical to baseline)",
  "command": "make -C c olmoe; bash paired_speed.sh (warm-up x1 per container, then ABABAB interleaved: SNAP=<container> PPL=1 ./olmoe 64 8 eval_ref_1.json, OMP_NUM_THREADS=12); quality: python3 tier_eval.py (202-token teacher-forced NLL, 2 reps, all four arms)",
  "prompt_hash": "0b69362b5bf8d78d9f47e803b9d0feb9bf81e5e1c9e915064f5b2820c668d025",
  "hardware": {
    "cpu": "Apple M5 Pro, 18 logical cores, 12 OMP threads",
    "ram": "24 GB unified memory (containers 6.9 GB, warm page-cache after untimed warm-up)",
    "storage": "internal Apple SSD, APFS; no O_DIRECT; identical file layout and byte counts in both containers",
    "os": "macOS 26.5.1 (Darwin 25.5.1), Apple clang 21, Homebrew libomp, arm64 default build"
  },
  "warmup_runs": 1,
  "changed_variables": ["ablated_expert_set"],
  "baseline": {
    "config": {
      "ablated_expert_set": "none",
      "engine_build": "make -C c olmoe @ a8f2ca6, default flags",
      "cap": "64",
      "threads": "12",
      "prompt": "eval_ref_1.json (202 scored held-out agentic-traffic tokens, sha in prompt_hash)",
      "mode": "PPL=1 teacher-forced, cache fully warm",
      "cache_state": "warm (one untimed run per container before the timed block)"
    },
    "samples": { "tok_s": [12.20, 13.85, 15.66] },
    "median_tok_s": 13.85,
    "quality": {
      "method": "TF-NLL bit-stability across reps (3.8370 x3) + round-trip control container TF-NLL-identical to pristine baseline",
      "passed": true
    },
    "evidence": {
      "uri": "file:///Users/khalid/dev/mr-meeseeks/bench/prune-spike/paired_speed_warm.log (baseline lines)",
      "sha256": "9adf74f548ebc0c2ab60dca6668267209e88bea568578e3b338c074170748086"
    }
  },
  "trial": {
    "config": {
      "ablated_expert_set": "cold_tail_220 (candidates_full.txt: 218 experts below 0.1% of layer traffic + 2 zero-route; weights zeroed, byte layout unchanged)",
      "engine_build": "make -C c olmoe @ a8f2ca6, default flags",
      "cap": "64",
      "threads": "12",
      "prompt": "eval_ref_1.json (202 scored held-out agentic-traffic tokens, sha in prompt_hash)",
      "mode": "PPL=1 teacher-forced, cache fully warm",
      "cache_state": "warm (one untimed run per container before the timed block)"
    },
    "samples": { "tok_s": [14.41, 13.94, 15.74] },
    "median_tok_s": 14.41,
    "quality": {
      "method": "TF-NLL bit-stability across reps (4.0857 x3, vs baseline 3.8370: +0.2487 nats/token, +30% ppl); ablation verified as exactly-220-zeroed by per-tensor audit",
      "passed": true
    },
    "evidence": {
      "uri": "file:///Users/khalid/dev/mr-meeseeks/bench/prune-spike/paired_speed_warm.log (t220 lines); raw NLL: file:///Users/khalid/dev/mr-meeseeks/bench/prune-spike/tier_eval_results.json",
      "sha256": "833505589bc0185920b24a3dac0741d5c1ed036329881fd5e0a2893727336ec3"
    }
  },
  "outcome": "no-change"
}
```

Evidence files are local to the reporting machine and pinned by sha256 in the manifest; happy to attach or re-run on request. The heat workload is personal agent-session logs (not publishable), so reproduce with any plain-text corpus through the scripts above. Cross-refs: quantifies why #1254-style REAP uses activation-importance reconstruction instead of hit counts; speaks to placement-vs-removal in #708 and the fragility framing of #883.
