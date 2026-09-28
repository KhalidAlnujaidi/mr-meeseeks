# DESIGN — F121 residual / runner boundary race: call→row attribution

Task: design call on the runner boundary race (F121 residual): "disjoint
stamps (cheap) vs explicit per-replica call indices (better)".
Author: design-race (design-reviewer), team mm-handover-s8, 2026-09-28.
Status: **Phase 1 IMPLEMENTED** (2026-09-28; commits `d9abea6` + `b8fa80d` +
`1e5ddd7`) — see the implementation note at the end; Phase 2 not started.
Companion register entries: `README.md` F124 (+ F126 for the estimate-bound
correction found in verification).

## 0. The call (30 seconds)

**Per-replica call indices — not disjoint stamps.** But the honest cheap limb
of the space is not disjoint stamps either: attribute by *request-receipt*
time (stopgap, consumer-side, no rebuild) and carry explicit
(replica, call_index, phase) identity in **request headers** (structural fix,
two phases, one coordination point with the dsh-lite-cpp stream). Disjoint
stamps are REJECTED (three independent reasons, §3).

Phasing (staged explicitly so no core commit rides along unreviewed):

1. **Phase 1 — receipt-time attribution** (landable now, touches only
   `proxy.py`, `audit_replica_prompt_identity.py`, `referee.py`): log
   `req_ts_ms` at request receipt; attribute half-open `[start, finish)` on
   receipt time; orphan/pre-span calls (warmup) reported and excluded;
   ties that remain ambiguous are NAMED in output and a trace verdict refuses
   exit 0 while any are unresolved.
2. **Phase 2 — explicit call indices** (coordinate first: the header hook
   lives in `dsh-lite-cpp/include/dshlite/llm_client.hpp`, the same tree the
   payload-draft stream is holding uncommitted): runners tag every LLM call
   with `X-Iso-Replica` / `X-Iso-Call-Index` / `X-Iso-Phase`
   (`solicitation|warmup`) in **request headers — never the request body**
   (the audit hashes `req_body`; a body field would fabricate PROMPT-VARIANT
   on every capture). The proxy copies the headers into log entries.
   Consumers group by tag first; the time windows are demoted to a
   tie-tolerant cross-check whose disagreement is an exit-2 FINDING
   ("runner bookkeeping race"), never a silent reassignment.

Legacy captures (no `req_ts_ms`, no tags): fall back to the
`ts_ms − latency_ms` receipt estimate, print
`attribution-mode=window-inferred (estimate)`, count `tie_ambiguous: N`, and
do not certify traces while ambiguous ties remain.

## 1. Evidence (all measured on the real capture, reproduced today)

Data: `out/runner-audit-T10.json` + `out/proxy-audit.jsonl` (audit-T10,
ISO_REPS=3, 2026-09-28T13:35Z).

Row windows as the runner declared them:

| replica | started_at_ms | finished_at_ms | wall_ms |
|---|---|---|---|
| 0 | 1790591606133 | 1790591645517 | 39383 |
| 1 | 1790591645517 | 1790591685059 | 39542 |
| 2 | 1790591685059 | 1790591725593 | 40533 |

**Both boundaries tie exactly**: `finished_0 == started_1 == 1790591645517`
and `finished_1 == started_2 == 1790591685059`. Adjacent row stamps are
taken microseconds apart and truncate into the same millisecond — the tie is
structural, not unlucky.

Three attribution defects follow from that tie (plus the stamp-semantics
correction), all on 12 proxy calls:

1. **Trailing-call steal (the registered residual).** The `parser-retry`
   logged at `ts_ms = 1790591685059` (= `finished_1` = `started_2`) was
   issued at `…670158` = `ts_ms − latency(14901)`, i.e. 15 s *inside*
   replica 1's row, one ms after replica 1's previous call completed. It is
   provably replica 1's call, but the audit's half-open `[start, finish)`
   rule assigns it to replica 2 (whose window starts at the tie). Current
   output reproduces the register's symptom exactly:
   `per-replica call classes: (2, ['parser-retry','solicit','parser-retry','parser-retry'])`,
   with replica 1 meanwhile ground down to 2 calls (it made 3). The audit
   prints calls/replica `[4, 2, 4]`; the truth is `[3, 3, 3]` (see 3).
   *(The F121 residual paragraph says "half-open attribution assigns the call
   correctly (rep1)" — the code does not: `start <= ts < finish` at a tie
   `ts == finish_1 == start_2` gives the call to replica 2, which is exactly
   why the same paragraph's printed list has the stray leading
   `parser-retry`. The recorded mechanism is self-contradictory on this one
   clause; the symptom text is right, the assignment claim is not.)*
2. **Leading-edge dual case (kills every tie-break-only fix).** The warmup
   call (`req_body = {"user":"Reply with the single word: warm"}`, owner: NO
   replica — F93: unjudged pre-roll) completed at `ts_ms = 1790591606133` =
   replica 0's `started_at_ms`, exactly `+0` from the row edge. Any window
   rule — half-open or right-closed — must claim it for replica 0 or drop
   it, and timestamps alone cannot tell "pre-roll call finished as the row
   began" from "the row's first call". Both tie shapes sit in the SAME
   capture. No stamp arithmetic distinguishes them (§3).
3. **The verdict flips — to a refusal.** Re-attributed by request receipt
   (`ts_ms − latency_ms`), every replica's true trace is
   `[solicit, parser-retry, parser-retry]` — same number, same sequence,
   and byte-identical bodies per index
   (`60f41a02778c0a4c`, `24b095448083b9f2`, `24b095448083b9f2`) with the
   warmup orphaned pre-span, and the two manufactured `STRUCTURAL(...)` /
   call-count findings are artifacts of defects 1–2 (deleted). But two
   receipts (`solicit@…645517` ON the shared rep0/rep1 boundary,
   `solicit@…606133` ON `started_0`) sit within the ±1 ms estimate window
   of a row boundary — their ownership is unprovable from ms stamps — so
   the honest landed verdict is `TRACE-NOT-CERTIFIABLE`, exit 1, with both
   NAMED (1 tie + 1 boundary-ms receipt; F126). *(Amended 2026-09-28: the
   original reading — `PROMPT-IDENTICAL (raw)`, exit 0 — rested on a
   one-sided estimate bound that verification refuted; the refusal is the
   honest verdict. See README F126.)*
   *(T1 keeps its genuine `TRACE-VARIANT` under true attribution — its
   replica traces are 2/2/3 calls, real F113 cascade variance — so the F121
   regression on T1 survives; only T10's expectation flips, and it flips
   toward the more honest reading.)*
4. **Wire sums double-count (same race, second consumer — blast radius
   beyond the audit).** `referee.py:218-221` attributes proxy entries to row
   windows `[t0−500, t1+500]` **inclusive both ends** (the code comment says
   `[started_at_ms, finished_at_ms]` — the slop is undocumented in the
   comment). Measured on the same capture:

   | row | calls in window | prompt_tok | completion_tok | truth | what leaked in |
   |---|---|---|---|---|---|
   | rep0 | 4 | 427 | 69 | 3 calls / 407 / 67 | warmup (20/2) |
   | rep1 | 4 | 565 | 98 | 3 calls / 407 / 79 | rep0's trailing call (158/19) |
   | rep2 | 4 | 565 | 88 | 3 calls / 407 / 68 | rep1's trailing call (158/20) |

   `calls claimed by >1 row window: {…645514: 2, …685059: 2}` — two calls
   counted twice, one call counted that belongs to no row. These sums feed
   the **published cost axes** (`analyze.py:214-221` averages
   `prompt_tokens` per cell): an ISO_REPS=3 T10-shaped cell would publish
   `519` prompt-token average against a true `407` (+27 %). The F121
   handover line "affects future capture attribution, **not** published
   numbers" is true for gate/spawn/exit columns and **false for the
   token/cost columns** at ISO_REPS>1.
   This is also an **F112 recurrence at the attribution layer**: F112's fix
   rescheduled warmup (`first=1` per replica), but no filter excludes a
   warmup entry from a row's ±500 ms window, so the warmup's tokens still
   land in whichever judged row abuts it (measured: rep0, +20/+2).
   *(Amended 2026-09-28 — measured, not projected: the published-row leak is
   +20 prompt tokens (golem/T1 rep0: published 424 vs receipt-truth 404);
   the "+27 %" cell figure above is a projection from the audit capture,
   not a measured published row. The golem/langgraph token columns are not
   re-derivable at all — their proxy logs are gone from the tree (README
   F125) — while smolagents re-derives exactly. Do not quote the ISO_REPS>1
   token/cost columns until a clean re-run.)*
   Scheduling cannot fix an attribution filter.
5. **Stamp-semantics correction to the register.** F121's mechanism sentence
   says `ts_ms` "is stamped at request receipt". `proxy.py` stamps it **after
   the response has been written back to the client** (`_forward`: body read
   → `wfile.write` → entry built → `log`). So the boundary tie is a three-way
   race between the proxy's post-write stamp and the runner's
   row-end/row-start `system_clock` stamps, and the true ordering of the
   three events is unobservable at ms truncation. Consequence: *any*
   completion-side attribution (with any tie rule) is guessing at exactly the
   points that matter; request-receipt side is causally clean (each request
   is received while exactly one row scope is live).

Side note while in the code: the audit's partition doubles-check uses
`if sum(1 for _, w in windows if e in w) > 1` — dict *equality* rather than
identity, so two byte-equal log entries would be indistinguishable. Unlikely
(two fields differ per entry) but it is the same disease: evidence items
carry no identity (fixed for good by Phase-2 ids).

## 2. Root cause

Call→row ownership is a **fact known exactly at exactly one place** — the
runner issues the call from inside one row's scope — and the pipeline throws
that knowledge away. Ownership is later *inferred* from ms-truncated wall
clock stamps taken in three different processes at three different event
points (proxy post-write, row end, row start). Adjacent rows provably touch
at the same millisecond (2 of 2 boundaries in one capture), and every
consumer (audit windows; referee ±500 ms slop windows) re-infers ownership
with its own rule, so each boundary call can be stolen, dropped, or
double-counted per consumer.

## 3. Why NOT disjoint stamps

"Disjoint stamps" = make adjacent windows not share an endpoint (e.g. stamp
`started_N+1 = finished_N + 1`, or fake a ≥1 ms gap). Rejected:

1. **It falsifies honest data.** `started_at_ms`/`finished_at_ms` would stop
   being observations and become annotations (`wall_ms` is measured on
   `steady_clock`, so the two timing lines would then disagree by
   construction — a trap for the next auditor). Doctrine: honest data is
   preserved, not softened; a stamp that is deliberately wrong is exactly
   the family of F121-defect-2 (a stored field read as if it were measured).
2. **It cannot disambiguate — it just moves the ambiguity.** Timestamps are
   blind at ties: the warmup-at-`started_0` case (§1.2) and a hypothetical
   sub-ms first-call-at-`finished_N` case are **indistinguishable on the
   clock**. Inflating the gap chooses which edge gets arbitrarily right or
   wrong; it never learns ownership.
3. **It does nothing for the real money surface.** The referee's window is
   `[t0−500, t1+500]`. A 1 ms disjoint gap changes nothing there (the slop
   swallows it); making the slop "work" would require faking ≥500 ms gaps —
   an outright fabrication of every replica's start time — and even then the
   warmup entry would still fall inside a −500 ms window. Partition-correct
   sums need ownership, not spacing.

Cost is not an argument for it either: the honest receipt-time interim fix
(Phase 1) is *cheaper* (consumer-side only, no rebuild of
`golem-runner`/SDK) than the falsifying one (which must touch the C++ runner
and re-pin its timing tests).

## 4. Why call indices win

Phase-2 tags give **ground-truth ownership** and several invariants that are
unreachable from clocks:

- exact per-row call lists and per-index byte-sha comparison (the audit's
  actual payload), warmup excluded structurally (`phase=warmup`) — F112's
  attribution-layer recurrence closes here;
- exact per-row token sums in the referee (double-count impossible);
- **dup/missing-call detection**: a repeated or gapped `call_index` is a
  FINDING (double-issue / dropped call) — invisible to time windows;
- the window cross-check becomes a genuine **consistency assertion**:
  tag-vs-window disagreement (beyond ±2 ms) = exit-2 finding "runner
  bookkeeping race". The F121 residual class of bug becomes a *detected
  runner defect* instead of a silent misattribution — failing loudly is the
  doctrine's preferred direction.

Placement subtleties (why the design is shaped this way):

- **Headers, not body.** The audit's claim-bearing measurement is
  `req_body` byte-identity; a per-call JSON field in the body would make
  every call unique by construction (PROMPT-VARIANT on 100 % of captures).
  `X-Iso-*` request headers leave the hashed bytes pristine and are copied
  verbatim by `proxy.py` into log entries (+4 lines).
- **Coordination point:** the C++ side's only request entry is
  `LlmClient::post(messages)` (`dsh-lite-cpp/include/dshlite/llm_client.hpp`;
  `postConstrained` from that same file is the API the dsh-lite-cpp
  payload-draft stream is holding uncommitted). The header hook must land
  with that stream coordinated — the handover's "stage paths explicitly, no
  core commit" applies. Until then Phase 1 (this design's interim) needs no
  SDK change at all. The Python arms (`langgraph_runner.py`,
  `smolagents_runner.py`) are a ~3-line headers change each.

## 5. Implementation plan + regressions (must FAIL on old code)

Phase 1 (`proxy.py`, `audit_replica_prompt_identity.py`, `referee.py`):

- `proxy.py`: `entry["req_ts_ms"] = int(t0 * 1000)` (floor at arrival).
- audit: attribute by `req_ts_ms` (fallback `ts_ms − latency_ms`, flagged),
  half-open per row; pre-span orphans listed (`orphan/pre-span: [warmup]`)
  and excluded from replica traces; `tie_ambiguous` counter named in output;
  while `tie_ambiguous > 0` **or any boundary-ms receipt remains (F126)**, a
  trace verdict still exits 1 and says the trace check was not certifiable
  (input verdicts unaffected);
- referee: same receipt-window rule for wire sums; entries outside all rows
  (warmup) counted in **neither**; ±500 slop removed (it existed to paper
  over exactly this race).

Regression suite (new test file, e.g. `test_call_attribution.py`):

| # | case | old code | new code |
|---|---|---|---|
| R1 | synthetic: touching windows + trailing-call tie at `f1==s2` + warmup tie at `s0` | `[4,2,4]`, warmup in rep0, steal rep1→rep2 | `[3,3,3]`, `[solicit,parser-retry,parser-retry]`×3, warmup orphaned |
| R2 | same fixture → verdict | `TRACE-VARIANT` exit 1 | `PROMPT-IDENTICAL (raw)` exit 0 |
| R3 | **real** `runner-audit-T10.json`+`proxy-audit.jsonl` | prints `TRACE-VARIANT`, 3 manufactured findings | zero manufactured findings, traces byte-identical — but `TRACE-NOT-CERTIFIABLE` exit 1 (1 tie + 1 boundary-ms receipt NAMED; amended 2026-09-28, F126) |
| R4 | **real** golem-T1 capture (F121 regression) | `TRACE-VARIANT` exit 1 | unchanged `TRACE-VARIANT` exit 1 (2/2/3 genuine) |
| R5 | referee sums on R3 data | 427/565/565 prompt, warmup counted | 407/407/407, `calls claimed >1: {}` |
| R6 | legacy capture (no `req_ts_ms`) | n/a | `attribution-mode=window-inferred`, ties named, no exit 0 while ties > 0 |
| R6f | synthetic outer-edge fixture (F126) | certifies `PROMPT-IDENTICAL (raw)` exit 0 — a false certification | NAMED as boundary-ms + refuses (`TRACE-NOT-CERTIFIABLE`) |

Phase 2 (coordinated): R-tag1 tag/window consistency fixture — tag says
replica 1, stamped window lands in replica 2 → exit 2 finding (this fixture
encodes today's bug as tomorrow's detector); R-tag2 gapped `call_index` →
finding; R-tag3 warmup phase excluded from referee sums.

R3 carries an **expectation change on real data** (`TRACE-VARIANT` →
refusal-by-tie) — recorded loudly at implementation: the three manufactured
findings are deleted (the true finding on T10 is "identical everywhere"; the
variance was manufactured by the attribution bugs), and certification is
REFUSED rather than granted while the two boundary receipts' ownership is
unprovable from ms stamps *(amended 2026-09-28: the original reading,
`PROMPT-IDENTICAL` exit 0, rested on a one-sided estimate bound that
verification refuted — README F126)*. R1–R6 verifiably fail against the
pre-fix code (R1/R2/R3/R5 by measurement above; R4 already green and stays
green); R6f (added with F126) fails against the pre-F126 code.

## 6. What this design does NOT establish

- It does not re-audit the **already-published** token/cost columns for
  ISO_REPS>1 batches. §1.4 shows the double-count mechanism live; whether any
  published row in `out/ISO_RESULTS.md` inherited inflated tokens should be
  its own verification pass (same method as F122's catch: re-derive from raw
  telemetry, don't trust the tree).
  *(Done 2026-09-28, two independent passes: no inherited inflation is
  demonstrated beyond the +20-token warmup leak on golem/T1 rep0; the
  golem/langgraph token columns are unauditable — their raw logs are gone
  (README F125); smolagents re-derives exactly. See README F124's token/cost
  paragraph.)*
- It does not fix F112's *scheduling* semantics (warmup per arm/replica) —
  only its attribution-layer recurrence (warmup never enters a row).
- It does not touch T9-CLAIM.md adjudication or the colibri rescue tracks.
- F109/F113/F115 semantics are untouched: T1 stays TRACE-VARIANT, and the
  "same prompt / identical inputs" claim keeps its byte-level evidence.

## 7. Register entry

Appended to `README.md` as **F124** (flaw registered before any fix, per
doctrine). Entry text lives there; this document is its evidence appendix.

---

## Implementation note (Phase 1 landed 2026-09-28)

Commits: `d9abea6` (proxy `req_ts_ms` + audit + referee receipt-time
attribution, first landing) → `b8fa80d` (t1 correction: receipts ON a shared
boundary refuse certification; test expectations aligned) → `1e5ddd7` (F126:
boundary-ms receipts block; the two-sided ±1 ms bound corrected in code +
docs). Files: `proxy.py`, `audit_replica_prompt_identity.py`, `referee.py`,
`test_call_attribution.py` (25 checks, RED-first). Suites: 25/25 + 8/8 +
21/21 + 15/15; `out/iso_benchmark.jsonl` sha256 unchanged throughout.

Verification (two independent seats, both bound to committed hashes):
1. team mm-f124's `verifier-claim` — audited `d9abea6` (attempt 2): the
   mechanism works; FAIL on the §1.3-vs-§5 contradiction (T10's promised
   `PROMPT-IDENTICAL` vs refuse-while-ties); recommended the refuse-on-boundary
   policy — adopted in `b8fa80d`. It also flagged two process problems: the
   fix was committed before clearance, and the tree moved mid-audit.
2. an fe-profile adversarial verifier — verdict **REFUTED** on the original
   claim set, with the findings that became F126 (two-sided bound; the
   outer-edge false certification), the F125 audit trail, and the claim
   corrections now recorded in README F124 (T10 verdict, RED procedure,
   token-column scope).

Coordination record (honest): two writers shared this tree today. The
fe-profile implementation (written first) was absorbed into `d9abea6`; the
team's correction (`b8fa80d`) was frozen by the fe profile while the team's
captain was idle, to protect it from resets; `1e5ddd7` and the register/docs
updates are fe-profile work on top. The team's t2 re-audit (attempt 3) had
NOT run when this note was written — it should bind to `HEAD` as pushed.