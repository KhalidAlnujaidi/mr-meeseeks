# bench/ — harness micro-benchmarks (scratch-only)

Two small runners + one reporter. They exercise **harness orchestration
logic** (atomicity, topology, daemon consult) with **stubbed leaves** —
no model API calls, no keys, no network. Pass rates therefore measure the
stub + harness path, while latency / cost / topology / fallback metrics
measure the loop itself.

## What each bench measures

| Bench | File | Measures |
|---|---|---|
| τ-retail subset | `tau-run.ts` | End-to-end orchestration over 8 retail-flavored tasks × k repeats: `pass^1` (mean run pass rate), `pass^3` (tasks passing all k repeats), mean turns, $/task (estimated), orchestration p50/p95 (atomicity+topology decision latency), fallback rate, topology distribution. Conditions B + C run locally; A is gated. |
| BFCL micro-routing | `bfcl-run.ts` | Cheap JSON tool-routing accuracy: 12 cases (request → exactly one correct tool). Local overlap router, zero network. Reports accuracy + per-case latency. |
| Reporter | `report.ts` | Any bench JSONL → markdown summary, including **cost-per-reliability-point** (USD per percentage point of `pass^3`). |

## Conditions (tau-run)

| Cond | Atomicity | Daemon | When |
|---|---|---|---|
| **B** | Jev-OFF, forced `isAtomicV2C` | `HARNESSD_SOCK`/`HARNESSD_TOKEN` scrubbed in-process | Always runs (local baseline) |
| **C** | Jev-OFF, forced `isAtomicV2C` | Ambient env; `consultDaemonSplit` attempted, `daemon_reached` recorded (null → local fallback) | Always runs (degrades cleanly without a daemon) |
| **A** | Jev-ON (`isAtomicAsync`) | Ambient | **Skipped cleanly** unless `TYPESAFE_API_KEY` is present |
| **J** | Calibrated Jev (`checkAtomicityNoul` at `JEV_DEFAULT_TIMEOUT_MS` = 2000ms), called **directly** — bypasses `isAtomicAsync` | Ambient | **Skipped cleanly** unless `TYPESAFE_API_KEY` is present |

Condition **J** is a bench-level variant, not harness behavior: since commit
`3c591c8` the loop's hot path (`isAtomicAsync`) is local-only and always reports
`source=v2c`. **A** is that harness default; **J** calls `checkAtomicityNoul()`
straight from `harness/jev.ts` with the generous non-critical-path timeout, so
`A` vs `J` compares the local v2c verdict against Jev's calibrated Noul verdict
on identical tasks. When Jev is unreachable J falls back to `isAtomicV2C`, the
same answer A/B/C produce, so the comparison degrades honestly rather than
breaking. `harness/loop.ts` and `harness/jev.ts` are untouched.

Fixed pins: worker/reviewer model labels + frozen `LEAF_HEADER`/`ROLE_PROMPTS`
prompts from `harness/loop.ts` (labels only — never called).

## How to run

```sh
# τ-retail subset, k=3 (default), conditions B+C:
node bench/tau-run.ts
node bench/tau-run.ts --k 3 --conditions B,C --out /tmp/bench-tau.jsonl

# With a Jev key, conditions A (harness default) and J (calibrated Jev) also run:
TYPESAFE_API_KEY=... node bench/tau-run.ts --conditions A,J,B,C
TYPESAFE_API_KEY=... node bench/tau-run.ts --conditions A,B,C,J --k 3 --out /tmp/bench-tau.jsonl

# Without a key, A and J are skipped cleanly (exit 0, skip line on stdout + JSONL):
node bench/tau-run.ts --conditions A,B,C,J

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
Condition **J** rows add `jev_source` (`"jev"` | `"v2c"`), `jev_confidence`,
`jev_atomic_latency_ms`; A/B/C rows keep exactly the schema above.
`jev_atomic_latency_ms` distinguishes three cases by number alone:
`0` = no key, never attempted; hundreds of ms = a real call was made and
failed; hundreds of ms = a real successful call. (A failed call is only
knowable via `jev_source: "v2c"` + `jev_confidence: 0`.)

Summary line: same `bench` + `type: "summary"`, `k, n_tasks, pass1, pass3,
mean_turns, cost_per_task_usd, orch_p50_ms, orch_p95_ms, fallback_rate,
topology_dist, ts`. The J summary additionally carries `jev_source_dist`,
`mean_jev_atomic_latency_ms`, `mean_jev_confidence`.

Run line (`bfcl-micro`): `bench, case_id, predicted, expected, correct,
latency_ms, ts` (+ a `type: "summary"` line with `accuracy`).
