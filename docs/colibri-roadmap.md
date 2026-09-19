# Colibri-Native Harness: Implementation Roadmap & Acceptance Criteria

> Status: ratified spec. Code untouched by this document.
> Supersedes the gap list sketched against `VISION.md` for the C++ lane;
> Parts A–E of `docs/budget-agi-host-middleware-spec.md` remain the
> normative rule source — this roadmap binds them to the Colibri engine.
> Flaw register F1–F10 audited against live code on 2026-09-19
> (commit a64c8c6); resolutions ratified by Khalid.

## Vision statement

A fully private, local Budget-AGI stack where a Colibri-served frontier
MoE (GLM-class) acts as the central brain, local light models serve as
the worker pool, and a C++ harness provides zero-trust process
sandboxing, host-enforced verification gates, and wall-clock/token
firewalling to ensure maximum reasoning throughput without wasting
NVMe disk-bound cycles.

## Ownership decision (resolves F10)

**dsh-lite-cpp is the sole execution engine for Colibri-native runs.**

1. *Alignment with Gap 2*: guard logic (`maxNudgeDepth=3`, retry caps,
   stall detection) is ported into C++, making the C++ binary the only
   place gates run. No gate may live in a prompt or a sidecar process.
2. *Schema & ledger integrity*: the C++ runtime emits `ledger.jsonl` v2
   directly, capturing wall-clock metrics (`latencyMs`, `tokPerSec`,
   NVMe disk-bound usage) natively at the socket/stream boundary with
   zero TS process overhead.
3. *Zero-network isolation*: the TS judge hook (`makeNodeJudgeHook` →
   remote Jev/Typesafe API) is eliminated from this lane; verification
   gates execute as local C++ code. `TYPESAFE_API_KEY` is scrubbed from
   every Colibri-native run (resolves F9).

`harness/loop.ts` remains the reference implementation of the TS lane
and the source of the guard constants being ported; it is not modified.

---

## Flaw register (audited before execution, per standing rule)

| # | Location | Defect | Ratified resolution |
|---|---|---|---|
| F1 | `brain.hpp:68` | Roadmap named a `std::deque<Message>` context; code has `std::vector<Message> history_`. | Compaction targets the vector. No migration; nothing justifies a container change. |
| F2 | `llm_client.hpp:14-15` | "Fallback to primary engine" would land stalled worker traffic on the GLM brain — destroys heterogeneous pool (VISION.md constraint 2), burns frontier wall-clock on leaf labor. | Fallback is **worker → worker (different family)** or escalate. Never to the brain engine. |
| F3 | `llm_client.cpp:96` | Port-only routing table ignores the verbatim `--model-id` rule (engine 404s anything else). | Routing table maps **role → (endpoint, model-id) pairs**. A port without the right model-id is a config error, rejected at load. |
| F4 | Gap 3 draft | "<0.1 tok/s hard abort" kills legitimate cold starts (Colibri's documented floor is 0.05–0.1 tok/s cold; `.coli_usage` pinning takes turns to warm). | Velocity floor is **per-engine-profile** and **warmup-exempt** (first N turns, configurable). |
| F5 | Gap 3 draft | `.coli_usage` format is undocumented; the file lives in the engine's model dir and races with engine writes. | **Probe task first**: inspect a real `.coli_usage` after a few turns; heat monitoring is best-effort until the format is confirmed parsable. Never block a run on heat data. |
| F6 | Gap 2 draft | Wall-clock stall timeouts can't distinguish stalled from slow-but-alive on disk-bound engines (60 s watchdog vs 0.1 tok/s). | Stall = **no bytes on the wire** over an interval (streaming liveness), not elapsed time. Nudge loop inherits I6/I7 caps (`MAX_ROUNDS_PER_TASK=2`, `maxNudgeDepth=3`, reset on user turn). |
| F7 | Gap 2 draft | Payload schema validation presumes a tool-call wire format dsh-lite doesn't have (plain role/content messages). | Structured JSON payloads are a **prerequisite task** for the pre-execution gate; Colibri `GRAMMAR=<file>.gbnf` forced drafts are the intended mechanism for constrained-JSON leaves. |
| F8 | Gap 4 draft | "~10 GB RAM bound" conflates harness and engine: GLM-5.2 dense alone is 9.9 GB; Colibri's table says 16 GB min / 24 GB comfortable. | Two separate bounds: **harness-process RSS < 50 MB**; engine residency bounded per-model-profile (name a small target, e.g. OLMoE ~8 GB, for memory-bounded runs). |
| F9 | `brain.hpp:39-47` | Default judge hook shells to `node harness/loop.ts judge`, which calls remote Jev when `TYPESAFE_API_KEY` is present. | Key scrubbed in Colibri-native runs; judge replaced by local C++ verification gates (see Ownership decision). |
| F10 | `harness/ledger.jsonl` | Ledger cost fields are `{input_tokens, output_tokens}` (historically 0); no latency/velocity; TS-vs-C++ ownership unstated. | `ledger.jsonl` **v2 schema** (below), emitted only by the C++ runtime; ownership resolved to dsh-lite-cpp. |

### ledger.jsonl v2 schema (C++-emitted)

One JSON object per line. v1 event types preserved (`split | report |
verify`), plus `DENY | nudge | route` lines:

```jsonc
{
  "type": "report",              // split|report|verify|DENY|nudge|route
  "schema": 2,
  "task_id": "R1.2",
  "parent_id": "R1",
  "depth": 1,
  "role": "worker",              // brain|worker|verifier
  "model": "qwen3.8-flash-next-colibri",
  "endpoint": "127.0.0.1:8081",  // loopback only in this lane
  "ts": "2026-09-19T12:00:00.000Z",
  "cost": {
    "prompt_tokens": 812,        // from engine usage block (real, not 0)
    "completion_tokens": 240,
    "latency_ms": 41200,         // socket-boundary wall clock
    "tok_per_sec": 5.8,          // completion_tokens / decode wall clock
    "ttft_ms": 1600              // when streaming is on; null otherwise
  },
  "cache": { "warm": true },     // best-effort .coli_usage heat (F5)
  "verdict": "pass",             // verify lines
  "detail": "…",                 // sanitized (Module 1), <= 4096 chars
  "resplit_count": 0
}
```

`route` lines record every router decision incl. fallback re-routes
(from/to model-id, reason: `timeout|no-bytes|http-5xx`). `nudge` lines
record stall detections and re-prompts with `nudge_depth`. `DENY` lines
carry `{gate, code}` exactly as spec A.7.

---

## Gap 1 — Multi-Engine Local Router (`llm_client.hpp/cpp`)

Objective: multi-model worker/verifier topologies across distinct local
`coli serve` instances (e.g. :8080 GLM-5.2 brain, :8081+ worker
families).

Acceptance criteria:

- **G1.1** Routing table maps each role (`brain`, `worker`, `verifier`)
  to an ordered list of **(endpoint, model-id)** pairs (F3). Config is
  validated at load: every entry's model-id must match the family the
  endpoint serves (probe via a 1-token request or documented
  `--model-id`); mismatch = hard config error, no silent 404 at
  runtime.
- **G1.2** Worker/verifier pools each contain **>= 2 distinct model
  families** (heterogeneity is load-bearing, VISION.md constraint 2).
  A single-family pool is a config warning surfaced on every report.
- **G1.3** Fallback: on worker stall/timeout/5xx, the router re-routes
  to the **next worker-family entry**, never to the brain endpoint
  (F2). Exhausted pool => escalate, `route` ledger line with reason.
  The brain endpoint is reachable only by role `brain`.
- **G1.4** Thread-safe concurrent dispatch: one `LlmClient` per
  endpoint (isolated connection + token state), fan-out via
  `postAsync`; per-endpoint mutex-guarded usage totals (existing
  pattern, `llm_client.cpp:160-166`). Race test: N threads x M
  endpoints, token totals exact.
- **G1.5** All endpoints loopback or explicitly allowlisted non-
  loopback; non-loopback still requires a key (existing rule,
  `llm_client.cpp:87-90`). Test suite uses the loopback stub server
  pattern from `tests/test_llm.cpp` — one stub per simulated engine,
  distinct ports, distinct served model-ids.

## Gap 2 — Host-Enforced Verification & Nudge Loop (C++, Parts D/E)

Objective: stall detection, re-prompting, and proposal verification as
compiled C++ in the harness process — never prompt-space.

Acceptance criteria:

- **G2.1 Stall detector (streaming liveness, F6)**: with `coli serve`
  streaming enabled, stall = zero bytes received for
  `stallNoBytesMs` (default 120_000, per-endpoint configurable).
  Wall-clock-only timeouts remain as the outer watchdog, set per
  engine profile (never 60 s flat against a 0.1 tok/s engine).
- **G2.2 Nudge turn**: on stall, host re-prompts the same (task,
  attempt) once; `nudge` ledger line with `nudge_depth`. Caps ported
  verbatim from the TS lane: `maxNudgeDepth=3` (I7, reset on user
  turn), `MAX_ROUNDS_PER_TASK=2` (I6), 3rd attempt must be narrower
  or re-routed to a different family (Gap 1 fallback), else
  stop+report. Uncapped nudge is a build-failing condition (test
  asserts the 4th nudge never fires).
- **G2.3 Repetitive-output detection**: leaf output matching the
  sanitizer's degenerate-repeat pattern (same line >= k times) is
  treated as a stall variant (`reason: repeat-loop`), not a success.
- **G2.4 Pre-execution gate (F7 prerequisite)**: leaves emit
  structured tool payloads as JSON, grammar-forced where the engine
  supports `GRAMMAR=*.gbnf`. Host validates schema + policy
  (destructive verbs => propose-only, per spec A.3) **before**
  `SwarmSpawner::spawn`. Invalid payload => retry within I6 budget,
  then escalate. No payload reaches a subprocess unvalidated.
- **G2.5 Result vector compaction (F1)**: multi-turn tool outputs are
  compacted to the spec E.2.3 result vector (taskId, status, summary
  1–5 lines, changedFiles, gitDiffHash, exportedState <= 4 KiB) before
  re-entering `BrainLoop::history_` (a `std::vector<Message>` — no
  deque exists or is introduced). Compaction runs through Module 1
  sanitizer first. Vector bound tests: oversized `exportedState` =>
  refuse (`BUS_VALUE_TOO_LARGE` semantics), never truncate.
- **G2.6 Local verification gate (F9)**: the C++ lane executes the
  check itself (exit code of the spawned verification command inside
  the task workspace) — sycophancy-immune per spec D.4. Judge hook
  replaced; `TYPESAFE_API_KEY` asserted absent from the harness
  environment at startup in this lane.

## Gap 3 — Wall-Clock & Local Token Firewall

Objective: replace request-count budgets with I/O-aware token &
wall-clock firewalling for disk-bound MoE streaming.

Acceptance criteria:

- **G3.1 Velocity accounting**: every `report`/`route` ledger line
  carries `latency_ms`, `tok_per_sec`, `ttft_ms` (v2 schema). Rates
  computed at the socket boundary in the C++ client.
- **G3.2 Per-profile velocity floor (F4)**: `minTokPerSec` is a field
  of the engine profile in the routing table (e.g. brain/GLM warm:
  1.0; small-model worker: 0.5; cold-start profile: exempt). Abort =>
  task fails with code `VELOCITY_FLOOR`, `DENY` ledger line, router
  may re-route per G1.3. **Warmup exemption**: the first
  `warmupTurns` (default 5, configurable) per engine are never
  aborted on velocity; warm-cache ramp (`.coli_usage` pinning) is
  expected physics, not a fault.
- **G3.3 Warm-cache awareness (F5)**: probe task P1 = run a real
  `coli serve`, exercise 3+ turns, inspect `.coli_usage` (format,
  mtime cadence, parse safety while the engine writes). Until P1
  lands: heat is recorded best-effort (`cache.warm: true|false|null`)
  and never gates anything. After P1: budget thresholds may scale
  with observed cache-hit rate; any such scaling is A/B'd against the
  static floor before becoming default.
- **G3.4 Budget currency**: the token firewall estimates cost in
  **tokens and wall-seconds** (breadth^depth x per-engine measured
  tok/s), not requests/day. The spec A.3 estimator formula is
  unchanged; its denominator becomes measured engine velocity.
  Hard-deny semantics (`BUDGET_EXCEEDED`) preserved.

## Gap 4 — Live Uncapped Benchmarking & Ledger

Objective: deep, unconstrained agent loops on local engines, producing
the ledger data VISION.md's falsifiable predictions need (Zipfian
depths, power-law atomicity residence times).

Acceptance criteria:

- **G4.1 Execution trace**: full v2 ledger for every run — depth per
  event, verifier-vs-worker turn ratios computable from `role` fields,
  spiral recovery rate from `resplit_count` + `SPIRAL_STOP` lines.
  Ledger is append-only, emitted by the C++ runtime only.
- **G4.2 Zero external network**: verified two ways — (a) loopback
  stub test suite (G1.5) passes with no routable endpoints configured;
  (b) during live runs, active interface monitoring (e.g. `nettop` /
  pf rule blocking non-loopback egress for the harness PID) shows zero
  non-loopback connections. `TYPESAFE_API_KEY`, `OPENROUTER_API_KEY`,
  `COLI_API_KEY` scrubbed from the spawned environment (F9); the
  scrubbed-env guarantee is already spawner law (`spawner.hpp:25-27`)
  — test asserts it.
- **G4.3 Memory bounds (F8)**: harness-process RSS < 50 MB across
  extended turns (sampled per turn into ledger `detail` or a `gauge`
  line). Engine residency is NOT harness-accounted; memory-bounded
  benchmark runs name their engine profile (e.g. OLMoE, ~8 GB total
  box budget) and assert against `ps` for the engine PID separately.
- **G4.4 Uncapped depth, bounded damage**: with `memberMaxDepth`
  removed (the fork experiment), all Gap 2/3 gates remain active —
  spiral breaker (>= 3 re-splits), budget firewall, nudge caps, retry
  caps. The run may go arbitrarily deep; it may not go infinitely
  wide or silently expensive. Every `DENY` is auditable.
- **G4.5 Regression honesty**: each new gate ships with a test that
  fails against the pre-gate code (standing rule). Benches follow
  `bench/README.md` discipline: outputs to `/tmp/bench-*` only, never
  touch prod ledger, non-discriminating metrics labeled as such.

---

## Sequencing

| Order | Item | Why first |
|---|---|---|
| 1 | G1 router + loopback multi-stub tests | Everything downstream needs >= 2 engines to be meaningful. |
| 2 | F5 probe task P1 (`.coli_usage` inspection) | Cheap, unblocks G3.3 design with facts instead of guesses. |
| 3 | G3 velocity accounting + per-profile floor | Must exist before uncapped runs, or G4 has no damage bound. |
| 4 | G2 stall/nudge/gate port to C++ | Guard constants ported from `harness/loop.ts` (reference impl). |
| 5 | G4 live uncapped benchmark | The experiment the whole vision exists to run. |

G2.4 (structured payloads / grammar forcing) may slip past step 5 for
the first benchmark if leaves stay text-report-only — the gate applies
to tool payloads, and the first run can restrict leaves to
report-shaped output, which G2.5 already compacts.

---

## G2.4 addendum — grammar-forced drafts (implemented, F45-F50)

Audited against the LIVE wire (coli serve + openai_server.py:2658-2689,
schema_gbnf.h, family_registry.py, docs/grammar-draft.md) before coding:

- **F45 (premise correction):** colibri's grammar is a speculative DRAFT
  SOURCE, never a sampling constraint — forced spans are verified by the
  target model, so no grammar can guarantee "100% strict JSON". The G2
  payload gate remains the ONLY hard enforcement; grammar reduces nudge
  frequency, not gate necessity. `parseStrictPayload` is whole-string,
  no repair/no substring extraction; failures feed the nudge loop.
- **F46 (capability):** `grammar_payload` is per-family — glm=True only;
  olmoe/qwen/deepseek/kimi/inkling=False ⇒ gateway HTTP 400
  `unsupported_parameter` (verbatim signature captured live). Typed
  `GrammarUnsupportedError` ⇒ router attempt outcome
  "grammar-unsupported" ⇒ fallthrough to a capable family; all-refusing
  pool ⇒ exhausted throw CITES the grammar (fail-loud, no masking).
  `solicitToolPayload` retries once unconstrained so solicitation is
  never impossible on an incapable family.
- **F47 (local mirror of gateway 400s):** type/schema-shape/1 MiB/NUL/
  missing root-rule and empty-tools all rejected at config time.
- **F48 (seam):** `ILlmPoster::postConstrained` defaulted virtual
  forwarding to `post()` — every existing fake keeps compiling; fakes
  ignoring the grammar is F45-honest.
- **F49/F50 (schema subset laws from schema_gbnf.h):** compiler accepts
  only object+properties(+required listing EVERY property)/string(+enum,
  const)/number/integer/boolean/null/array+items(+minItems 0|1);
  anything else fail-closed ⇒ silent no-grammar. No anyOf ⇒ multi-tool
  payloads constrain the tool-name enum span ONLY; args constrained only
  in the single-tool case. Bare `{"type":"object"}` and empty-properties
  are compiler traps — never emitted.
- Wire: `response_format` ∈ {text(omitted), json_object, json_schema
  (wrapped under json_schema.schema), gbnf (raw, root rule required)}.
- Verified live: constrained request to the olmoe engine produced the
  typed error from the REAL gateway; plain post unaffected (PONG).
- Tests: test-grammar 26 checks (wire shapes, F47 validation, F49/F50
  builders, body-capture passing, F46 typed refusal + router
  fallthrough + fail-loud exhaustion, F45 strict parse, solicit
  composition incl. gate-receives-structure and nudge-line emission).

### G2.4-in-runner addendum (F51-F59, live trace wiring)

- **F51 (typed-throw gap, BLOCKER found in audit):** through a router,
  an all-refusing pool threw plain runtime_error — solicitToolPayload's
  F46 fallback (catches GrammarUnsupportedError) would never fire and
  the solicit would crash. Fix: the router's grammar-exhaustion throw IS
  a GrammarUnsupportedError (derives from runtime_error; existing catch
  sites unaffected).
- **F52 (role pinning):** g4-run's poster was Brain-pinned; leaf
  solicitation would have violated role separation. Fix: role-
  parameterized RouterPoster; brain turns -> Brain pool, leaf solicits
  -> Worker pool, separate ledger tags (g4g.brain.* / g4g.leaf.*).
- **F53 (no-retry law):** solicitToolPayload never loops by design; the
  runner owns a bounded retry (cap = kMaxNudgeDepth 3) counting format
  nudges per arm.
- **F54 (honest measurement):** OLMoE lacks grammar_payload, so the
  grammar arm runs the F46 fallback on this hardware — the A/B measures
  strict-parse+nudge quality, NOT grammar acceleration. Summary says so
  explicitly; grammar_reached_capable_engine reported per run.
- **F55 (trace tags):** g4g.* run tag keeps ledger deltas separable
  from g4.* (earlier live) and g4s.* (stub stress) lines.
- **F56 (streaming 400 body swallowed — product bug, regression-
  proven):** on the streaming path httplib hands the non-200 body to
  the SSE content receiver, leaving res->body EMPTY; the F46 substring
  classification silently degraded to generic http-4xx, breaking the
  typed fallback in exactly the configuration g4-run uses (stream=
  true). Fix: classify from sse.partial when streaming + body empty.
  test-grammar §9 fails against pre-fix code (verified by temporary
  revert) and passes after.
- **F57 (nudges must carry information):** retry re-sends the prompt
  PLUS the strict-parse rejection reason; identical re-sends against a
  deterministic failure mode would burn the cap with zero information.
- **F58 (surface-form imitation, live finding):** OLMoE fenced its
  reply (```json ... ```) on 6/6 attempts when the prompt showed the
  schema inline in prose, even when told "no fences"; a BARE unfenced
  example line elicited raw JSON first-try. Strict parse rightly
  refused the fences (no fence-stripping repair by law) — the prompt
  must model the wanted surface form. Runner prompt + nudge text fixed
  accordingly; after the fix: both arms ok=1/1, nudges=0, gate
  received structured JSON, task verified.
- **F59 (counter honesty):** gotGrammar counts only SERVED constrained
  calls (200 through the grammar path), per-arm delta — a refused
  grammar followed by fallback must never report "grammar reached the
  engine".

### G1.2 dual-family + GLM grammar-wire addendum (F60-F61)

- **F60 (capability quirk):** registry family glm53 (GLM-5.3-Flash,
  321B) has grammar_payload=False; only family glm (GLM-5.2/5.3, 744B)
  is True. deriveFamily collapses both ids to "glm", so
  modelSupportsGrammar(modelId) is the id-accurate check (informational
  pre-warn only — the gateway's typed 400 remains authoritative).
- **F61 (GLM streaming quirk):** the gateway splits GLM's <think> span
  into reasoning_content deltas (#597 item 4). Client law: reasoning is
  counted (reasoningDeltas) but NEVER appended to content — payload
  purity for the strict parser; reasoning bytes still reset stall
  liveness and never inflate the F33 token estimate.
- **Live-hardware blocker (honest):** the only grammar-capable family
  (glm 744B) does not fit local disk (int4 ~370 GB vs 224 GB free);
  glm53 321B fits but is NOT grammar-capable. Live grammar acceleration
  therefore remains stub-proven (test-grammar §10 F61 GLM stream shape,
  test-router §11 constrained dispatch) until a glm checkpoint exists.
- g4-run gained dual-leaf mode: optional leaf2 (different family) puts
  two families in worker/verifier pools — G1.2 warnings clear, grammar
  solicits fall through to the capable family when one is present.
- Live dual-id trace (real OLMoE engines, honest ids): brain olmoe +
  leaves {glm-5.2-colibri, olmoe-leaf} — warnings cleared, role
  isolation held, 2-turn A/B ok=2/2 both arms, nudges=0, both gated
  tasks verified; ledger lines carry per-model attribution.
