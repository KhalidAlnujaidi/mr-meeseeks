# bench/ — harness micro-benchmarks (scratch-only)

Two small runners + one reporter. They exercise **harness orchestration
logic** (atomicity, topology, daemon consult) with **stubbed leaves** —
no model API calls, no keys, no network.

**Read this before quoting any number.** Because the leaves are stubs, the
τ-retail pass draw is *treatment-independent by construction*: the condition
label is not a hash input, so the same task+repeat yields the same pass value
in every condition. `pass^1` / `pass^3` / cost-per-reliability-point are
therefore **NOT valid discriminators between conditions** — see
[What is and is not a valid signal](#what-is-and-is-not-a-valid-signal).
What these benches do measure is the orchestration path itself: latency,
turns, the atomic-vs-split verdict, Jev provenance, and fallback semantics.

## What each bench measures

| Bench | File | Measures |
|---|---|---|
| τ-retail subset | `tau-run.ts` | End-to-end orchestration over 8 retail-flavored tasks × k repeats: mean turns, $/task (estimated), orchestration p50/p95 (atomicity+topology decision latency), fallback rate, topology distribution. Raw `pass^1`/`pass^3` are emitted for schema continuity but are **non-discriminating** (stub draw). Conditions B + C run locally; A, J, K are gated. |
| BFCL micro-routing | `bfcl-run.ts` | Cheap JSON tool-routing accuracy: 12 cases (request → exactly one correct tool). Local overlap router, zero network. Reports accuracy + per-case latency. **This accuracy is real** — a live local router checked against fixed expected tools, not a stub draw. |
| Reporter | `report.ts` | Any bench JSONL → markdown summary. Prints cost-per-reliability-point (USD per percentage point of `pass^3`) for τ-retail, flagged inline as derived from a non-discriminating metric. |

## Conditions (tau-run)

| Cond | Atomicity | Daemon | When |
|---|---|---|---|
| **B** | Jev-OFF, forced `isAtomicV2C` | `HARNESSD_SOCK`/`HARNESSD_TOKEN` scrubbed in-process | Always runs (local baseline) |
| **C** | Jev-OFF, forced `isAtomicV2C` | Ambient env; `consultDaemonSplit` attempted, `daemon_reached` recorded (null → local fallback) | Always runs (degrades cleanly without a daemon) |
| **A** | Jev-ON (`isAtomicAsync`) | Ambient | **Skipped cleanly** unless `TYPESAFE_API_KEY` is present |
| **J** | Calibrated Jev (`checkAtomicityNoul` at `JEV_DEFAULT_TIMEOUT_MS` = 2000ms), called **directly** — bypasses `isAtomicAsync` | Ambient | **Skipped cleanly** unless `TYPESAFE_API_KEY` is present |
| **K** | Calibrated Jev — byte-for-byte the same call as **J** (identical `checkAtomicityNoul` + `JEV_DEFAULT_TIMEOUT_MS` + `isAtomicV2C` fallback) | Ambient | **Skipped cleanly** unless `TYPESAFE_API_KEY` is present |

Condition **K** exists to **isolate the atomicity effect from the topology
effect**. `chooseTopology()` is called unconditionally for every condition
(`tau-run.ts`), so **J** moves *two* things at once: it gets Jev's calibrated
Noul atomicity verdict **and** a Jev-chosen topology. An **A**-vs-**J**
comparison therefore attributes nothing to atomicity — topology moved too.
**K** holds topology **local** (forced `FALLBACK_TOPOLOGY`,
`ENGINEER_REVIEWER`; `chooseTopology()` is never called and no network
request is made for topology) while keeping atomicity **exactly** as **J**
does it. `J`-vs-`K` therefore moves atomicity *alone*, and K's
`orch_latency_ms` reflects the atomicity attempt only. J and K carry
identical per-row `jev_*` fields and identical summary aggregates, so the
pair is directly comparable.

Condition **J** is a bench-level variant, not harness behavior: since commit
`3c591c8` the loop's hot path (`isAtomicAsync`) is local-only and always reports
`source=v2c`. **A** is that harness default; **J** calls `checkAtomicityNoul()`
straight from `harness/jev.ts` with the generous non-critical-path timeout, so
`A` vs `J` compares the local v2c verdict against Jev's calibrated Noul verdict
on identical tasks. When Jev is unreachable J (and K) fall back to
`isAtomicV2C`, the same answer A/B/C produce, so the comparison degrades
honestly rather than breaking. `harness/loop.ts` and `harness/jev.ts` are
untouched.

Fixed pins: worker/reviewer model labels + frozen `LEAF_HEADER`/`ROLE_PROMPTS`
prompts from `harness/loop.ts` (labels only — never called).

## What is and is not a valid signal

The τ-retail leaves are **stubs**. A stub worker's success cannot depend on
which atomicity or topology path orchestrated it, so the pass draw is a
deterministic function of `task.id` + `repeat` **only** — the condition label
is deliberately *not* a hash input (`stubPass`, `tau-run.ts`).

| Metric | Valid across conditions? | Why |
|---|---|---|
| `orch_p50_ms` / `orch_p95_ms` | **Yes** | Real wall-clock decision time; J includes the topology request, K does not |
| `turns` | **Yes** | Derived from the actual atomic-vs-split verdict |
| `atomic`, `atomic_source` | **Yes** | The condition's real atomicity output |
| `jev_source_dist`, `mean_jev_atomic_latency_ms`, `mean_jev_confidence` | **Yes** | Jev provenance and round-trip, measured |
| `topology`, `topology_fallback`, `fallback_rate` | **Yes** | Real topology source and degradation semantics |
| `cost_usd` / `$/task` | **Yes** | Estimated from real turn counts (placeholder price scale, not a bill) |
| `pass`, `pass^1`, `pass^3` | **NO** | Same deterministic stub draw re-reported per condition — no treatment signal |
| cost-per-reliability-point | **NO** | Derived entirely from `pass^3` |

`pass^1`/`pass^3` are still emitted so the JSONL schema stays stable and rows
remain comparable, but they are **not** accuracy results. A cross-condition
difference in them is noise by construction, never a finding. (This was a real
bug: an earlier revision hashed the condition label into the draw, so
conditions with byte-identical behavior — J and K — reported different
`pass^1` purely because `"J" != "K"`. That invalidated every A-vs-B,
A-vs-J and B-vs-C accuracy comparison.)

The **bfcl-micro** accuracy figure is unaffected and remains meaningful: it
scores a live local router against fixed expected tools, with no stub draw
involved.

## How to run

```sh
# τ-retail subset, k=3 (default), conditions B+C:
node bench/tau-run.ts
node bench/tau-run.ts --k 3 --conditions B,C --out /tmp/bench-tau.jsonl

# With a Jev key, conditions A (harness default), J (calibrated Jev) and
# K (calibrated Jev + LOCAL topology — the atomicity isolator) also run:
TYPESAFE_API_KEY=... node bench/tau-run.ts --conditions A,J,K
TYPESAFE_API_KEY=... node bench/tau-run.ts --conditions A,J,K,B,C
TYPESAFE_API_KEY=... node bench/tau-run.ts --conditions A,B,C,J,K --k 3 --out /tmp/bench-tau.jsonl

# Without a key, A, J and K are skipped cleanly (exit 0, skip line on stdout + JSONL):
node bench/tau-run.ts --conditions A,B,C,J,K

# BFCL micro-routing bench:
node bench/bfcl-run.ts
node bench/bfcl-run.ts --out /tmp/bench-bfcl.jsonl

# Markdown summary (stdout, or --out to a /tmp path):
node bench/report.ts /tmp/bench-tau.jsonl
node bench/report.ts /tmp/bench-bfcl.jsonl --out /tmp/bench-summary.md
```

All three run directly with `node` (erasable-syntax TS, Node >= 22.18 type
stripping). Zero deps beyond the harness modules they import.

## Rules (enforced in code)

- **Outputs go to `/tmp/bench-*` only.** Every `--out` must start with
  `/tmp/` or the runner refuses. Defaults are timestamped
  `/tmp/bench-<name>-<ts>.jsonl`.
- **Never touches prod state.** Nothing here reads or writes
  `harness/ledger.jsonl`, `harness/tasks/`, or `harness/queue/`. If you need
  ledger data, copy it to scratch first.
- **No secrets in repo.** Keys travel via env only (`TYPESAFE_API_KEY`,
  `HARNESSD_SOCK`/`HARNESSD_TOKEN`), are never logged, and condition B
  scrubs daemon env in-process (restored afterwards).
- **Local baseline:** `HARNESSD_SOCK`/`HARNESSD_TOKEN` unset → condition C
  records `daemon_reached: false` and uses the local guard path.

## Output schema (JSONL, one object per line)

Run line (`tau-retail`): `bench, condition, task_id, repeat, atomic,
atomic_source, topology, topology_fallback, daemon_configured,
daemon_reached, turns, pass, cost_usd, orch_latency_ms, ts`.
Condition **J** and **K** rows add `jev_source` (`"jev"` | `"v2c"`),
`jev_confidence`, `jev_atomic_latency_ms` — the same three keys on both;
A/B/C rows keep exactly the schema above.
`jev_atomic_latency_ms` distinguishes three cases by number alone:
`0` = no key, never attempted; hundreds of ms = a real call was made and
failed; hundreds of ms = a real successful call. (A failed call is only
knowable via `jev_source: "v2c"` + `jev_confidence: 0`.)
On **K** rows `topology` is always `ENGINEER_REVIEWER` with
`topology_fallback: true`, and `orch_latency_ms` is the atomicity attempt
alone — no topology request is made.

Summary line: same `bench` + `type: "summary"`, `k, n_tasks, pass1, pass3,
mean_turns, cost_per_task_usd, orch_p50_ms, orch_p95_ms, fallback_rate,
topology_dist, ts`. The **J** and **K** summaries additionally carry
`jev_source_dist`, `mean_jev_atomic_latency_ms`, `mean_jev_confidence`.
`pass1`/`pass3` are present but non-discriminating (see above); the
schema is unchanged so A/B/C summaries keep exactly their key set and J/K
keep their extra three.

Run line (`bfcl-micro`): `bench, case_id, predicted, expected, correct,
latency_ms, ts` (+ a `type: "summary"` line with `accuracy`).
