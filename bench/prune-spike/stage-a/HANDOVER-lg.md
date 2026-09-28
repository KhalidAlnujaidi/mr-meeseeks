# HANDOVER — Colibri predictive caching (Markov transition tables)

**Hermes profile: `lg`**
Profile directory: `/Users/khalid/.hermes/profiles/lg`
Upstream: https://github.com/JustVugg/colibri
Local clone: `/Users/khalid/dev/mr-meeseeks/colibri`
Session date: 2026-09-20 → 2026-09-21

---

## 1. One-paragraph state

Stage B (OLMoE route-trace emission) is **done and merged upstream into `dev`**
(PR #1630, merge commit `32d513d`, merged by JustVugg 2026-09-21T03:54:14Z, CI
green). Stage A (offline order-1 Markov tables + Phase-0 gate evaluation) is
**complete and closed NEGATIVE** — the predictor is real and large, but the
existing Phase-0 gate structurally cannot measure it, so it is not cleared.
**Stage C was never started and must not be**, per the decision rule: a table
that fails the gate does not get a runtime path. Stage A commit `8e03dd3` is
pushed to the fork branch `feat/markov-transition-tables`; no PR was opened for
it, deliberately (see §5).

---

## 2. Where everything lives

| what | path |
|---|---|
| upstream clone (branch `feat/markov-transition-tables`) | `/Users/khalid/dev/mr-meeseeks/colibri` |
| Stage A campaign + all evidence | `/Users/khalid/dev/mr-meeseeks/bench/prune-spike/stage-a/` |
| Stage A result write-up | `bench/prune-spike/stage-a/FINDINGS.md` |
| how to verify Stage A | `bench/prune-spike/stage-a/VERIFYING.md` |
| OLMoE model container | `~/models/olmoe_merged` (6.46 GB experts, int8) |
| instrumented engine binary used for capture | `/tmp/olmoe_pr1` (ephemeral — rebuild it) |

Fork: `fork` remote = `https://github.com/KhalidAlnujaidi/colibri.git`.
`origin` = upstream `JustVugg/colibri`.

---

## 3. Stage B — what shipped (MERGED)

Two edits in `c/olmoe.c` inside `moe()`:

1. The activation-heatmap loop became one `if (!m->hot_pinned) rt_route(layer, s,
   idx, val, K);` — same counters, same placement, plus the trace line.
2. `rt_trace_end();` outside the row loop, once per `moe()` invocation, so the
   call counter advances even for a zero-row batch.

Plus a `ROUTE_TRACE` row in the engine's env-var header and two paragraph fixes
in `docs/experiments/cnre-offline-simulator.md`.

**Why it mattered:** OLMoE called `rt_init()` (which opens the trace file) but
never `rt_trace()`, so it announced `ROUTE_TRACE=` and wrote a **0-byte file**.
Proven with two independent builds before fixing.

**Evidence:** 3344 lines / 292,917 bytes for one 202-token ref; 3232 consecutive
call ids; one layer per call cycling 0..15; rows 0..7. `TF-NLL 3.8370` **identical**
with tracing off, tracing on, and pre-change — the token-exactness invariant.
4 in-repo test binaries pass (`test_olmoe_cache_index`, `test_olmoe_serve_framing`,
`test_olmoe_matmul_q`, `test_route_trace`).

**Known caveat:** `mergeStateStatus` was never cleanly readable while open, and
the PR merged into `dev` rather than `main`. The fork branch is now behind
`origin/dev`; if anyone touches Stage B's files, start from `origin/dev`.

---

## 4. Stage A — what was built and what it found

### Built (all additive; **no engine code changed in Stage A**)

| file | role |
|---|---|
| `c/tools/stage_a_build_corpus.py` | real DSH session text → engine refs, split **by source session** |
| `c/tools/route_markov.py` | builds the `COLIMARKOV` order-1 table + held-out recall report |
| `c/tools/residency_sim.py` | added `markov` policy class, `--markov-table`, `--markov-confidence` |
| `c/tests/test_residency_sim.py` | +4 tests (39 total, all green) |
| `stage-a/capture_traces.sh` | one cold engine process per ref → ROUTE_TRACE |
| `stage-a/olmoe-residency.json` | per-layer geometry from the engine's own slot arithmetic |
| `stage-a/run_olmoe_gate.py` | drives the Phase-0 gate on OLMoE traces |
| `stage-a/validate_felt_cost.py` | tests the gate's per-miss cost assumption |

### Corpus

32 refs, 4 workload categories, by-session split (8 train / 24 held-out).
32 traces, **107,008 records**, 32 distinct held-out NLLs (1.458–5.248) so every
ref is a genuinely different workload. Table: **960 entries, 51 KB, order-1, n=8**.

### The headline number (this part of the hypothesis HOLDS)

Held-out recall of the true next-layer expert set, 24 traces / 601,920 selections,
budget 8 candidates per conditioning expert:

| predictor | recall | candidates/selection |
|---|---|---|
| marginal heat (the `.coli_usage` view) | 39.80% | 1.0 |
| **order-1 Markov** | **82.53%** | 2.5 |
| | **+42.7pp** | |

It transfers across held-out **sessions** and across workload **categories**.

### The gate result (NEGATIVE)

`markov` and `half-pinned` post **identical misses, identical felt wait, identical
verdicts** at every budget. The predictor contributes exactly zero to the metric
the gate scores.

| budget | lru hit% | half-pinned / markov hit% | gate verdict |
|---|---|---|---|
| 0.5 GB (4 slots) | 16.65 | 22.33 | PASS (mean 13.34%, worst 10.65%) |
| 1 GB (9 slots) | 43.61 | 44.13 | fail (mean 6.74%, worst 3.20%) |
| 2 GB (19 slots) | 65.22 | 67.17 | PASS (mean 10.41%, worst 7.94%) |
| 4 GB (39 slots) | 87.79 | 89.12 | PASS (mean 18.75%, worst 15.69%) |

Those PASSes belong to the **pinning rule**, not the predictor. At 0.5 GB
`markov` is not even the best policy — `frequency` is (26.15% vs 22.33%).

### Why (measured, not inferred)

- `BasePolicy.access` hands the policy the per-`(call, layer)` **union** (mean
  8.11 experts). A position's prediction set is therefore ~19.7 distinct experts.
- At 2 GB a layer has 19 slots, 9 pinned → **10 adaptive slots**. A 19.7-expert
  prediction cannot discriminate within 10 slots.
- The class's only lever is "evict an unpredicted resident first" — but the
  prediction is derived from the very union being demanded, so almost every
  resident is predicted and the rule rarely fires.
- **The structural reason:** `docs/experiments/cnre-offline-simulator.md` states
  the tool "does not model new prefetch reads". Prefetch recall is the lever this
  table moves; a demand-residency gate has no term for a hidden read.

### Second finding: the gate's cost model is weak on this box

`validate_felt_cost.py`, 6 cache sizes on one held-out ref (`felt-cost.txt`):

| cap | hit% | misses | tok/s | us/miss | ppl |
|---|---|---|---|---|---|
| 8 | 27.30 | 19444 | 10.78 | 962 | 3.8370 |
| 24 | 71.40 | 7657 | 13.67 | 1933 | 3.8370 |
| 48 | 92.00 | 2134 | 14.38 | 6560 | 3.8370 |
| 64 | 96.30 | 980 | **12.65** | 16327 | 3.8370 |

- Cost per miss **rises** with residency (962 → 16,327 µs/miss); fit is
  `seconds = 14.17 + 203.5µs·misses`, RMS residual 0.87 s on a 4.70 s range.
- `cap=64` has the **best hit rate and is slower than cap=48**. A felt-wait
  ranking is therefore **not** a tok/s ranking on this machine.
- PPL is 3.8370 at every cap — placement never changes semantics.

---

## 5. Decisions a successor must not silently reverse

1. **Stage C is off.** The gate was not cleared. Adding `COLI_PREDICT=markov` now
   would be optimising a mechanism the evidence says is not the lever, and any
   PASS would be the pinning rule's.
2. **No PR was opened for Stage A yet.** It is a negative result whose three new
   files are research tooling on top of a gate upstream owns. Worth offering to
   upstream as an issue/comment *after* the maintainer's reaction to #1630, not
   before. The merge of #1630 into `dev` is the natural prompt.
3. **The empty-table control is load-bearing.** `markov` with an empty table must
   reproduce `half-pinned` byte-for-byte; that is asserted in the test suite. My
   first implementation failed it (15344 vs 15324 misses over 4000 events) from
   pure LRU bookkeeping. Do not "simplify" `admit_batch` and re-break it.
4. **Don't chase the eviction tiebreaker to win the gate.** Documented in
   FINDINGS.md as explicitly not recommended.
5. **Held-out discipline is by SESSION, not by line.** Routing is autocorrelated
   within a run; a line-level split leaks.

---

## 6. Verification status (honest)

- **In-repo suite: GREEN.** `cd colibri/c && python3 -m unittest
  tests.test_residency_sim` → **39 tests, OK** (35 upstream + 4 new).
- **Ad-hoc, 63 checks, all passing** (two scripts in `stage-a/`, see VERIFYING.md):
  `hermes-verify-stage-a.py` (31) and `hermes-verify-stage-a-drivers.py` (32).
- **`make check` was NEVER run** at any stage — it wipes every build artifact and
  rebuilds the whole tree. Both stages owe it. State this plainly if asked.
- Also unverified: the `OLMOE_NO_MAIN` segment build path, the CUDA flag matrix
  (Stage A touched no engine code).

A verifier bug worth remembering: an H5 check anchored on the last column of
`validate_felt_cost`'s table (the NLL) and asserted a false failure. A verifier
that lies is worse than none — fix the check, don't drop it.

---

## 7. Reproduction commands

```sh
cd /Users/khalid/dev/mr-meeseeks/colibri/c
python3 -m unittest tests.test_residency_sim          # 39 tests, OK

cd /Users/khalid/dev/mr-meeseeks/bench/prune-spike/stage-a
python3 hermes-verify-stage-a.py                      # 31 checks
python3 hermes-verify-stage-a-drivers.py              # 32 checks

# full campaign
python3 stage_a_build_corpus.py --out traces --per-category 8
./capture_traces.sh /path/to/olmoe traces/refs traces/raw   # ~50 min, 1 process/ref
python3 /Users/khalid/dev/mr-meeseeks/colibri/c/tools/route_markov.py markov.table \
        --train $(cat train.txt) --score $(cat heldout.txt) --report --topn 8
python3 run_olmoe_gate.py --traces traces/raw --manifest olmoe-residency.json \
        --markov-table markov.table --budgets-gb 0.5 1 2 4 --read-gbps 4.64
python3 validate_felt_cost.py /path/to/olmoe <ref.json> 8 16 24 32 48 64
```

`train.txt` / `heldout.txt` are generated from `traces/split.json` (one path per
line; the last line has no trailing newline so `wc -l` under-reports by one —
harmless, `run_olmoe_gate.py` reads `split.json`, not these files).

Note: use the repo venv interpreter (`colibri/.venv/bin/python`, 3.12). The
system `python3` is 3.9.6 and `c/coli` fails on it (`dataclass(slots=True)`).

---

## 8. Environment facts worth carrying forward

- Machine: Apple M5 Pro, 24 GB, macOS 26.5.1, ~203 GB free. **Only OLMoE fits.**
  GLM-5.2 needs ~370 GB → its 0.05–0.1 tok/s floor is not reachable here, so
  "regime 2" (RAM-rich, pinning-heavy) was never available on this box.
- Measured NVMe: **4.64 GB/s** sequential F_NOCACHE → 1.36 ms per 6.3 MB expert.
- Expert geometry (engine's own arithmetic): `hidden*inter*3 + (inter*2+hidden)*4`
  = **6,307,840 bytes**.
- DSH corpus: `~/.dsh/sessions`, 1004 sessions, 2,192,964 records, 2.27 GB
  decompressed. Session dir names carry the workload category.
- **Corpus trap:** the first records of every DSH session are identical injected
  context (`Current runtime context.`, `<system-reminder>`,
  `<hindsight_knowledge>`). Taking "the first long-enough record" per session
  extracts the *same text* from every session and silently collapses the campaign
  to one workload — 5 of 32 refs were byte-identical. Reject boilerplate and
  assert no duplicate ref content.
- **Trace-parser trap:** `call` advances once per **layer**, so keying positions
  on `(call, row)` yields zero transitions. Key on `(forward, row)`, detecting the
  forward boundary where the layer index wraps.
- Bash-invoked background jobs in this environment emit unrelated conda
  shell-init noise on every poll. It is not from the job.

---

## 9. Next actions, in order

1. **Watch #1630's merge.** It merged into `dev` — check whether the maintainer
   wants anything else (and whether `dev`→`main` carries it).
2. **Then** offer Stage A upstream: an issue or a comment with the +42.7pp recall
   result and the specific structural finding (the gate cannot score prefetch).
   That is the contribution with real value — "this lever is real and your gate
   can't see it" is useful to them.
3. If they want it scored: a **prefetch term** in the simulator (a hidden-read
   fraction so a precached expert costs service but not felt wait). That is a
   simulator change, not an engine change, and it is the honest fix.
4. Only after a prefetch-aware gate passes: consider Stage C as a runtime A/B
   (`COLI_PREDICT=markov` feeding the existing PILOT ring) for real tok/s/TTFT.
   That is what the gate doc itself calls "the necessary qualification".
5. Housekeeping: `colibri/.venv/` is untracked and expected; the branch
   `feat/olmoe-route-trace` is stale after the `dev` merge — don't build on it.
