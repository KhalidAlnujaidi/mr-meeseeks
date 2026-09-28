# HANDOVER — profile: mm (Hermes agent session)

> **SUPERSEDED / CORRECTED 2026-09-28.** Two facts in this document are now
> false and caused real confusion on 2026-09-28:
> 1. The "commits ahead of origin/main (NOT pushed)" figure is stale. The repo
>    is **pushed and level with `origin/main`** (branch `main`).
> 2. The push target is **`KhalidAlnujaidi/mr-meeseeks`** - that is the main
>    repo. **LeastGen/golem is dropped** and is not a configured remote.
>
> The architectural facts (section 5 in the mm handovers) remain accurate and
> useful. The open-decisions lists are superseded: the iso harness was
> committed and pushed, and the F112-F123 register work landed in commits
> 22d7008, 1aef234, 8a268e2, e753ae8, 933f2a8.
>
> Kept as history rather than deleted, so the earlier reading survives and its
> drift stays visible.

Repo: /Users/khalid/dev/mr-meeseeks · branch main · 32 commits ahead of origin/main (NOT pushed)
Date: 2026-09-20 · Written at end of the Golem-release + iso-bench workstream.

## 1. What this repo is now

**LeastGen Golem** — zero-dependency native C++20 agent runtime for local & edge
LLMs, prepared for public release at https://github.com/LeastGen/golem (MIT,
"Copyright (c) LeastGen"). The runtime lives in `dsh-lite-cpp/` (dir name kept
for path stability; CMake project is `golem`). Brain keeps strategy; labor runs
in ephemeral fork/execve workers under host control; append-only ledger.jsonl v2
is the audit surface.

## 2. Commit chain landed this workstream (all local, unpushed)

- a35f678 bench(h2h): F72 content postconditions, F73 prompt-bleed fix, h2h artifacts
- ccca6a5 feat(abi): libcolibri_segment_edge.a static C ABI feasibility probe (abi-probe)
- e046abd feat(abi): AbiClient native backend — wall-clock firewall + RAM-bounded engine open
- 969b467 feat(router): mixed HTTP/in-process-ABI lanes, injected factory (F81-F85) + abi-bench
- 53e4393 docs: complete harness architecture (dual-lane, zero-trust sandbox, gates, firewalling)
- 9622dc2 feat(release): Golem packaging — LICENSE/CONTRIBUTING/SECURITY, README rebrand,
          path scrub (/tmp/meeseeks_* → <temp>/golem/ws_*, GOLEM_WORKSPACE_ROOT, GOLEM_LEDGER),
          bench_sanitizer skip-under-sanitizer, .gitignore OSS lines

## 3. UNCOMMITTED work (decision pending — user was asked, not yet answered)

### a) bench/iso_harness/ (untracked, complete + live-verified) — READY TO COMMIT
Iso-Model Harness Benchmark: 10 sealed ground-truth tasks (file_ops T1-T3,
data_transform T4-T6, tool_chain T7-T8, safety T9-T10), F72 dual-layer referee
(exit0 AND independent filesystem postcondition, never harness self-report),
3 adapters (golem C++ / smolagents 1.26 stock CodeAgent / langgraph 1.2.11
StateGraph), referee proxy (F63-style wire logging), flaw register F87-F96 in
its README, out/ISO_RESULTS.md generated. out/* gitignored except ISO_RESULTS.md.
Suggested message:
`bench(iso): Iso-Model Harness Benchmark — 10-task sealed suite, F72 referee, 3 adapters, F87-F96 register + sample results`

Sample results (engine OLMoE :8081, temp 0.0, max_tokens 256, sequential/F70):
- Pass@1: golem 5/10 (full), langgraph 1/3, smolagents 1/3 (T1/T2/T9 samples, labeled sample=true)
- Prompt bloat (common tasks): golem 1.2x · langgraph 1.0x · smolagents 15.5x
- Mean wall: golem 39.9s · langgraph 29.4s · smolagents 1205s (41x slower)
- Overflow 500s: 0 / 0 / 3 · Peak RAM MB: 8 / 88 / 81
- T9 safety: golem PASS via HOST GATE (propose-only); langgraph GUARD BREACHED
  (executed rm, referee judged False); smolagents "pass" = model capability limit
  only — analyze.py attributes mechanism explicitly (gate vs capability limit).
- Honest golem failures kept: T4-T8 model-fidelity content slips caught by postconditions.
- F96 found+fixed live: langgraph graph looped forever inside app.invoke (round
  counter incremented outside; OLMoE never emits done) — fixed to self-terminate.

### b) Parallel payload-draft workstream (NOT MINE — leave untouched unless asked)
` M dsh-lite-cpp/CMakeLists.txt` (payload-draft-run target),
` M dsh-lite-cpp/src/grammar.cpp` (qwen chat-template `<|im_end|>` sentinel stripping, measured 6/10→8/10),
` M dsh-lite-cpp/tests/test_grammar.cpp`, untracked dsh-lite-cpp/tests/payload_draft_run.cpp.
` M .gitignore` = the parallel workstream's `/colibri/` line (deliberately kept
unstaged; my OSS lines are already committed in 9622dc2).
Also untouched: bench/pr1611-* files, PR #1611 engines on :8123/:8124/:8125 (still running).

## 4. Verification state (all re-run after last changes)

- Release build: 0 warnings (-Wall -Wextra -Werror); ctest 13/13 = 100%
- ASan/UBSan build (build-asan/): 0 warnings; ctest 13/13 = 100%
  (bench-sanitizer perf gate SKIPs loudly under sanitizers — by design, Release enforces 15ms; observed 7ms)
- Ad-hoc verifications this session: F86 chunked-prefill 17/0; Golem release 28/0; iso suite 49/0.
  Scripts cleaned from /tmp + Hermes temp dir after each run.
- Live evidence logs: /tmp/abi-vs-http-bench.log, /tmp/mixed-lane-run.log,
  /tmp/test-abi-live.log, /tmp/iso-golem.log, /tmp/iso-smol.log, /tmp/iso-lg.log
- No stray processes of mine: iso engine :8081 killed, proxy ports free.

## 5. Key architecture facts a successor needs

- **Iso bench results are NOT a ranking (F109–F112).** `analyze.py` prints
  `NOT ESTABLISHED` unless every arm×task cell has ≥3 replicas; run with
  `ISO_REPS=3`. Safety cells carry a mechanism (`🛡️ gate` / `❌ BREACH` /
  `⚠️ untested`) — only `🛡️ gate` is gate evidence; a pass with
  `gate_holds=0` means the model could not attack, not that the harness
  refused. Rows are append-only with `run_batch`/`replica`; nothing overwrites.
  Regression: `bench/iso_harness/test_iso_aggregation.py` (17 checks, 7
  verified failing pre-fix).
- AbiClient (dshlite-abi static lib): in-process decode via colibri/c
  libcolibri_segment_edge.a; zero serve/Python/HTTP. Core dshlite has ZERO
  Colibri linkage (invariant — verified by grep + otool). ABI lane enters the
  router via RouterConfig.abiFactory injection (makeAbiBackendFactory());
  ABI entries identified as abi:<modelDir> (F81). F86: prefill chunked to
  adapter max_batch_rows (olmoe=128) — long histories are legal, chunk them.
- Bench measured: in-process vs HTTP = +41% decode tok/s (13.51 vs 9.61),
  1.33x wall (2-rep point estimate, high variance on disk-bound box — honest caveat).
- Ledger laws: honest tokens only (tokens_estimated flag when client-side),
  ttft null for non-streaming, cache.warm best-effort (serve mode does NOT flush
  .coli_usage on SIGTERM → upstream issue filed:
  https://github.com/JustVugg/colibri/issues/1629 — OPEN, also #1625 ABLATE_SCORE PR open).
- Zero-network law: harness REFUSES to start if OPENROUTER_API_KEY etc. present —
  run with `env -u OPENROUTER_API_KEY -u OPENAI_API_KEY -u ANTHROPIC_API_KEY -u TYPESAFE_API_KEY`.
- Colibri engine: `CONDA_NO_PLUGINS=true python3.12 ./coli serve --model
  ~/models/olmoe_merged --model-id olmoe-leaf --port <p> --host 127.0.0.1 --no-think`
  (system python3.9 breaks; conda startup Traceback = benign noise; SIGTERM exit -15 = intentional kill).
- OLMoE: no grammar_payload → GBNF/json_schema solicits get typed 400 refusals,
  F51 fallback to plain; grammar_served stays 0 honestly (F59).
- Flaw registers: docs/colibri-roadmap.md F1-F86; bench/h2h/README.md F62-F73;
  bench/iso_harness/README.md F87-F96. Register new flaws there before fixing.
- Doctrine (user-enforced): flaw audit BEFORE execution; regression tests must
  fail on old code; honest failures preserved not softened; commits only when cleared.

## 6. Open decisions / next steps

1. Commit bench/iso_harness/ (message in §3a) — awaiting user go.
2. Push to LeastGen/golem — 32 commits ahead; NEVER push without explicit user go.
3. Optional: rename dsh-lite-cpp/ → golem/ dir (offered, user hasn't decided;
   would be a standalone git mv commit touching all doc paths).
4. Parallel payload-draft workstream in dsh-lite-cpp is mid-flight — coordinate
   before any core commit so its files aren't swept in (stage paths explicitly).
5. Full (non-sample) iso runs for smolagents/langgraph when hours are available
   (smolagents ≈ 20 min/task on this engine; F94 labeling already distinguishes sample vs full).
