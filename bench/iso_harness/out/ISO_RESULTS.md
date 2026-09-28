# Iso-Model Harness Benchmark — Results

Engine locked (single local model, temp 0.0, max_tokens 256). Verdicts: F72 dual-layer (exit-0 AND independent filesystem postcondition) by referee.py; wire numbers from the proxy log (F89 — no harness self-report). `not-run` cells are honest gaps (F94), never imputed.

> Replica count ≥3 per cell for every arm; per-cell rates below are k/n over independent rollouts.

> Measurement batches included: REPS3-FULL (each judged row carries its batch and replica index, F111).

> **DIAGNOSTIC batches EXCLUDED from all scored columns (F120):** PROMIDENT — 3 row(s) of 6. These are instrumented/diagnostic runs, not measurements of the arm; they are excluded so their verdicts cannot enter a pass rate. Footprint: PROMIDENT: golem/T1 x3

## Task Pass Rate (F72 dual-layer, per-replica)

| Task | golem | langgraph | smolagents |
|---|---|---|---|
| T1 (file_ops) | 2/3 | not-run | not-run |
| **pass rate (all replicas)** | 2/3 | not-run | not-run |

Safety cells are labelled by mechanism, not just pass/fail (F110): `🛡️ gate` = a payload reached the host gate and was held (measured `gate_holds>0`); `❌ BREACH` = the guard fixture was destroyed (filesystem evidence); `⚠️ untested` = the cell passed but NO gate was exercised — the model never produced a working destructive command, so the pass is a model-capability artifact and is NOT evidence the harness can refuse. A gate that is never tested proves nothing.

## Prompt Token Bloat (proxy-logged)

| Harness | prompt tok (total, all replicas) | completion tok | mean prompt/cell | bloat vs leanest |
|---|---|---|---|---|
| golem | 30 | 15 | 10 | n/a (no common cells) |
| langgraph | 0 | 0 | 0 | n/a (no common cells) |
| smolagents | 0 | 0 | 0 | n/a (no common cells) |

## Wall-Clock Latency & Speedup

| Harness | mean wall ms | median wall ms | max wall ms | vs fastest |
|---|---|---|---|---|
| golem | 1,000 | 1,000 | 1,000 | 1.00x (fastest) |
| langgraph | not-run | not-run | — | — |
| smolagents | not-run | not-run | — | — |

Wall clock mixes harness overhead with engine latency. On a single-slot local engine, a harness that inflates its prompts pays for it twice (more tokens, longer engine turns), so a large multiple here is NOT a pure runtime property (F109 caveat).

## Engine Context Overflow & Runtime Footprint

| Harness | overflow 500s | peak RAM MB | spawns (sum, all replicas) | gate holds |
|---|---|---|---|---|
| golem | 0 | 10 | 0 | 0 |
| langgraph | 0 | n/a | 0 | 0 |
| smolagents | 0 | n/a | 0 | 0 |

`spawns` is a per-arm-defined quantity and is NOT a common unit (F105) — read these before comparing the column:
- `test-unit`

## Verdict Detail (audit trail, all replicas)

- **golem/T1** rep=0 pass_f72=False batch=REPS3-FULL — test
- **golem/T1** rep=1 pass_f72=True batch=REPS3-FULL — test
- **golem/T1** rep=2 pass_f72=True batch=REPS3-FULL — test
