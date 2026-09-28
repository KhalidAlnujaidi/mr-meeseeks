# Stage A result: the order-1 Markov table is real, the Phase-0 gate cannot see it

> **Status handover:** `HANDOVER-lg.md` in this directory — profile `lg`, current
> PR/commit state, what merged, what must not be silently reversed.

Status: **the gate is NOT cleared by the predictor.** Report the negative result and
stop. No Stage C.

## What was built (all additive, no engine change)

| piece | purpose |
|---|---|
| `c/tools/stage_a_build_corpus.py` | real agentic text -> engine refs, split by SOURCE SESSION |
| `stage-a/capture_traces.sh` | one cold engine process per ref -> ROUTE_TRACE |
| `stage-a/olmoe-residency.json` | per-layer geometry from the engine's own slot arithmetic |
| `c/tools/route_markov.py` | COLIMARKOV order-1 table + held-out recall report |
| `c/tools/residency_sim.py` | `markov` policy class + `--markov-table` (upstream gate) |
| `stage-a/run_olmoe_gate.py` | drives the Phase-0 gate on OLMoE traces |
| `stage-a/validate_felt_cost.py` | tests the gate's per-miss cost assumption |

## Corpus (held-out discipline)

32 refs, 4 workload categories, **split by source session** — no session on both
sides. Traces: 107,008 records, 32 distinct held-out NLLs (1.458–5.248) so each
ref is a genuinely different workload. 8 train / 24 held-out.

Three extraction bugs were caught and fixed by hashing refs, not by reading code:

1. Taking the first long-enough record per session extracted the same DSH
   preamble (`Current runtime context...`, `<system-reminder>...`,
   `<hindsight_knowledge>...`) from every session — 5 of 32 refs were byte-identical.
   Fix: boilerplate rejection + a hard duplicate check that aborts the build.
2. Quota allocated by iteration order gave 24 train / 0 held-out, then 0/32.
   Fix: explicit per-split quotas + assertions on the split.
3. `route_markov.read_trace` keyed positions on `(call, row)`. Since `call`
   advances once per LAYER, this produced 3344 single-layer "positions" and
   **zero transitions**. Fix: key on (forward, row), detecting the forward
   boundary where the layer index wraps — the same regrouping `route_pairs.py` does.

## Table

`markov.table`: **960 conditioning entries, 51 KB, n=8, order-1.**

Held-out recall of the true next-layer expert set, 24 traces / 601,920 selections,
budget 8 candidates per conditioning expert:

| predictor | recall | candidates/selection |
|---|---|---|
| marginal heat (the `.coli_usage` view) | 39.80% | 1.0 |
| **order-1 Markov** | **82.53%** | 2.5 |
| | **+42.7pp** | |

So the routing signal is strong and it transfers across held-out sessions and
across workload categories. That part of the hypothesis holds.

## Phase-0 gate: not cleared

`markov` vs `half-pinned` at every budget — **identical misses, identical
felt_wait, identical gate verdict**, because the predictor changes nothing:

| budget | lru hit% | half-pinned / markov hit% | vs lru (felt) | gate verdict |
|---|---|---|---|---|
| 0.5 GB (4 slots) | 16.65 | 22.33 | +6.75% | PASS (mean 13.34%, worst 10.65%) |
| 1 GB (9 slots) | 43.61 | 44.13 | +0.92% | fail (mean 6.74%, worst 3.20%) |
| 2 GB (19 slots) | 65.22 | 67.17 | +5.61% | PASS (mean 10.41%, worst 7.94%) |
| 4 GB (39 slots) | 87.79 | 89.12 | +10.94% | PASS (mean 18.75%, worst 15.69%) |

`markov` and `half-pinned` are identical to the last decimal at all four budgets
(same misses, same felt wait, same verdicts), and `markov` is not the best
policy at 0.5 GB — `frequency` is (26.15% hit vs 22.33%). The PASS at 0.5/2/4 GB
belongs to the **pinning rule**, not the predictor.

### Why the predictor contributes zero in this model

Measured, not inferred:

- `BasePolicy.access` hands the policy the per-`(call, layer)` **union** — mean
  8.11 experts, max 45. The conditioning set is therefore already the layer's
  full demand for that step, and the size of a *position's* prediction set is
  ~19.7 distinct experts.
- At 2 GB a layer has 19 slots, 9 of them pinned, leaving **10 adaptive slots**.
  A 19.7-expert prediction cannot discriminate within 10 slots.
- The only lever the class has is "evict an unpredicted resident first". Since
  the prediction is derived from the very union being demanded, almost every
  resident is predicted, so the rule rarely fires and when it does it is close
  to arbitrary.

### The deeper reason: the gate cannot test this lever

`docs/experiments/cnre-offline-simulator.md` states the tool **does not model
prefetch reads** ("A predicted reduction in felt wait is a gate for a runtime
A/B, not the result of one"). The Markov table's measured value is *prefetch
recall* — which will be *prefetched* early, not *which resident to evict*.
Phase 0 has no term for a hidden read, so it structurally cannot score the
mechanism the table is good at. The four policies it can score are all
demand-residency policies; `markov` as implemented is a fifth one, and in that
role it adds nothing beyond the pins it was given.

## Also measured: the gate's core assumption is weak on this box

`validate_felt_cost.py` fits `seconds = a + b*misses` across 6 cache sizes on one
held-out ref (`felt-cost.txt`):

| cap | hit% | misses | seconds | tok/s | us/miss | ppl |
|---|---|---|---|---|---|---|
| 8 | 27.30 | 19444 | 18.70 | 10.78 | 962 | 3.8370 |
| 16 | 56.70 | 11589 | 16.40 | 12.33 | 1415 | 3.8370 |
| 24 | 71.40 | 7657 | 14.80 | 13.67 | 1933 | 3.8370 |
| 32 | 80.20 | 5301 | 14.70 | 13.72 | 2773 | 3.8370 |
| 48 | 92.00 | 2134 | 14.00 | 14.38 | 6560 | 3.8370 |
| 64 | 96.30 | 980 | 16.00 | 12.65 | 16327 | 3.8370 |

Two things the Phase-0 model does not express:

- The marginal cost per miss **rises** with residency (962 -> 16327 us/miss). The
  simulator prices every miss identically, so its total is a poor fit here:
  `seconds = 14.17 + 203.5us*misses`, RMS residual 0.87 s on a 4.70 s range.
- `cap=64` is the **fastest hit rate (96.3%) and slower than cap=48** (12.65 vs
  14.38 tok/s). A ranking by misses puts it wrong. A felt-wait ranking is
  therefore not a tok/s ranking on this machine, which is worth saying before
  anyone reads the table above as a predicted speedup.

PPL is 3.8370 at every cap: placement changes I/O, never semantics, which is the
token-exactness invariant holding under every policy tested.

## Conclusion

- Order-1 Markov prediction of the next layer's experts is **real and large**
  (+42.7pp recall held-out, 51 KB table, transfers across sessions and categories).
- The existing Phase-0 gate **cannot see that signal** and reports the predictor
  as worth exactly nothing, which is the correct answer *to the question that
  gate asks*.
- Therefore **Stage A fails the gate as written, and Stage C is not started.**

## What would settle it, in order of cost

1. Add a prefetch term to the simulator (a hidden-read fraction, so a precached
   expert costs service but not felt wait). This is the honest fix and it is a
   simulator change, not an engine change.
2. Or go straight to a runtime A/B in the engine (`COLI_PREDICT=markov` feeding
   the existing PILOT ring), which measures tok/s/TTFT directly and is what the
   gate doc calls "the necessary qualification".
3. Not recommended: tuning the eviction tiebreaker to win the gate. It would be
   optimising a mechanism the evidence says is not the lever, and any PASS would
   come from the pinning rule already present in the baseline.
