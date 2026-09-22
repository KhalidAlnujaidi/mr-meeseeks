# HANDOVER — profile: mm · session 2 (F72 promotion + iso bench)
Repo: /Users/khalid/dev/mr-meeseeks · branch main · **38 commits ahead of origin/main (NOT pushed)**
Date: 2026-09-22 · Supersedes the "UNCOMMITTED work" section of HANDOVER-mm.md (session 1).
Session 1 handover is still accurate for §2 commit chain up to 9622dc2 and for §5 architecture facts.

## 1. What this session did

Two workstreams, driven by "benchmark Golem vs other harnesses":

1. **Committed the iso harness** (was staged-but-uncommitted from session 1).
2. **Promoted F72's artifact-aware verification from bench-only into the runtime**,
   plus re-solicitation on failure — and found/fixed a live bug (F97) doing it.

## 2. Commits landed this session (all local, unpushed)

- `bc9e86a` bench(iso): Iso-Model Harness Benchmark — 10-task sealed suite, F72 referee,
  3 adapters, F87-F96 register + sample results
- `b8e490e` test(nudge): document F72 promotion gap — failing change-detectors
- `8166c09` feat(verify): promote F72 artifact-aware postcondition into the runtime loop
- `8b573c9` feat(verify): re-solicit on failure + honest correction of the b8e490e claim
- `a587223` fix(verify): F97 — lexical path guard; scope-file symlinks were read as escapes
- `0cf9487` docs(iso): register F97/F98

## 3. The F72 promotion (8166c09) — what changed

F72 already existed and already named this failure ("exit-0 proves a command RAN, not that
it did the TASK; verified requires BOTH layers"). Its fix had landed **bench-side only**
(`tests/h2h_ours.cpp`, `referee.py`), while the runtime loop kept layer 1. `h2h_ours.cpp`
said so verbatim: *"a core postcondition hook is future work"*. **No new flaw was
registered — this is a promotion gap under F72.**

- `include/dshlite/postcondition.hpp` + `src/postcondition.cpp` — new. The layer-2
  evaluator, promoted and EXTENDED to the full `referee.py` vocabulary (P2-c):
  `equals` (trailing-newline trimmed), `contains`, `regex` (ECMAScript/multiline),
  `exists:<bool>` (negation = canary-absence), file-exists-only, `all_of`.
  `PostconditionResult` gained `evaluated` so "nothing declared" ≠ "check ran and passed".
- `BrainLoop::runGatedTask` gained an optional `PostconditionHook` (appended + defaulted,
  so all 5 existing call sites compile and behave unchanged). Layer 2 runs in the
  workspace BEFORE cleanup, only when layer 1 passed. `TaskReport` carries `postcondition`.
- **Three-state disposition:** `verified` now means exit0 AND content held;
  `verified-exit-only` means exit0 with no postcondition declared. The old bare
  `verified` for an unchecked run was a silent upgrade and is gone.
- `verifyViaSpawn` is UNCHANGED (still exit-code only) — layering happens in `runGatedTask`.

## 4. Re-solicitation (8b573c9) — what changed

- `BrainLoop::ReSolicitHook` = `std::function<optional<json>(const FailureFeedback&)>`,
  appended param + `maxResolicits` (default 0 = off).
- `FailureFeedback` is layer-aware: attempt number, `exitOk` (L1),
  `postconditionOk`/`Evaluated` (L2), detail, capped sanitized `lastOutput`.
- **`runGatedTask` stays model-free** — deliberate. The hook is the same idiom as
  `JudgeHook`/`RetryPlanner`; the host owns the model call. Calling `solicitToolPayload`
  inside the safety loop would invert the layering (Brain owns strategy+model; the loop
  owns gate/isolation/verify/caps).
- Caps NOT bypassed: a re-solicited payload still passes the gate and still consumes an I6
  retry round. `rep.resolicits` counts them so a round-2 pass ≠ a round-1 pass.
- A throwing hook cannot bypass the gate: only a well-formed payload advances `current`.

## 5. F97 — the live bug (a587223), and why it matters

**Caught on the FIRST post-promotion iso run, not in review.** golem/T2 reported
`artifact path escapes workspace: hello.txt` for a file INSIDE the workspace.

Mechanism: `SwarmSpawner` **symlinks** `scopeFiles` into the ephemeral workspace
(`spawner.cpp:110`), so `weakly_canonical()` resolved a legitimate artifact to its real
path OUTSIDE the workspace root and the canonical-prefix guard refused it. Blast radius:
every scope-file task (T2/T3-shaped). Runtime called them content-FAILED while the referee
passed them — i.e. the runtime and the bench **disagreed about "verified"**, exactly the
failure the promotion exists to prevent.

Fix: the guard is now **lexical** — reject absolute paths and any `..` component, require
the lexical join under the workspace root. Symlinks out of the workspace stay readable
(that IS the spawn contract: scope files are the task's declared inputs). Also removes a
dependency on host filesystem layout. Regression: section D of
`test_verify_promotion.cpp`, verified failing (1 failed) pre-fix → passing post-fix.

## 6. Verification state (re-run at handover)

- Release build: 0 warnings (-Wall -Wextra -Werror); **ctest 14/14**
- ASan/UBSan build: 0 warnings; **ctest 14/14**
- Ad-hoc (`hermes-verify-f97*.py`, created+removed this session): **12/12 passed, 0 failed**
  — F97 fix, traversal still refused, mechanism proven via `realpath()` independently of
  the C++ test, suite predicate coverage read from `tasks.json`, runtime-vs-referee
  agreement recomputed from artifacts, ledger layer naming.
- Live iso arm (engine OLMoE :8081, temp 0.0, sequential/F70), golem over T2,T3,T4-T10:
  **runtime verdict == independent referee 9/9, 0 disagreements.**
  T2/T3 pass both layers; T9/T10 pass via guard-held (F87); T4/T6/T8 exit-nonzero;
  T5 "artifact missing: level.txt"; T7 content mismatch (want "CHAINED", got "").
  T4-T8 are honest model-fidelity failures, now caught by the runtime, not just the bench.
- Ledger verify line now names the deciding layer, e.g.
  `{"verdict":"pass","detail":"exit=0 postcond=pass hello.txt contains-verified"}`
- Engine I started on :8081 is STOPPED (port freed). Khalid's 3 pre-existing engines on
  :8123/:8124/:8125 are still up (untouched, PR #1611 comparison — he said forget PRs).

## 7. Honesty ledger — claims that were WRONG and were corrected

**`b8e490e`'s commit message claimed its change-detectors "provably go red when semantics
move". That was false for sections B and C**, found by re-verifying the red direction:
- Section B called `runGatedTask` with NO hook — the one path the promotion leaves
  unchanged — so it read "gap open" against BOTH old and new code. Measured a symptom.
- Section C asserted hardcoded `false` literals; tested nothing.
- Only section A detected anything, and only by API absence (a compile failure).
The correction is recorded in the test header, not quietly patched. Verified evidence now
on record: new suite body vs OLD library => hard compile failure
(`postcondition.hpp` not found); old b8e490e body vs old library => compiles, reports 6 gaps.
**No behavioral red-line exists for the re-solicit path** because it is new API rather than
changed behavior — stated, not papered over.

## 8. Open threads / next steps

1. **The re-solicit hook is built and unit-tested but NOT wired into the iso harness.**
   `golem_runner.cpp` still does its own prompt-scaffolded re-solicitation by hand
   (~line 211). Wiring it to `ReSolicitHook` is the obvious next step and would make
   round-2 passes measurable via `rep.resolicits`.
2. **No full-suite run yet.** `out/ISO_RESULTS.md` reflects a PARTIAL golem arm
   (T2,T3,T4-T10; **T1 absent**, other two harness columns untouched). The header's
   SAMPLE/not-run labeling still applies. **No cross-harness claim can be made from it.**
   Full smolagents/langgraph arms need hours (~20 min/task on this engine).
3. **Tool surface (Read/Write/Edit/Glob/Grep/Bash/Web) — explored, NOT started.**
   Findings for whoever picks it up:
   - The runtime was built for this: `ToolSchema` = name + arbitrary JSON Schema;
     `checkPayload` is tool-agnostic; `allowedTools` is already a list.
   - Decide spawn-vs-in-process per tool. Spawning `/bin/sh` equivalents keeps the
     zero-trust boundary; in-process punches a hole in it (would need its own flaw entry).
   - `Edit` will collide with the destructive scan: `destructiveVerbs` contains `"format"`
     on word bounds (F21) — a coding harness will hit this as a false positive.
   - **Web breaks the zero-network law (F9)** — needs an explicit policy switch + entry.
   - Multi-tool grammar constrains ONLY the tool-name span (F50), so 7 tools are LESS
     grammatically constrained than today's single-tool case.
   - OLMoE has no `grammar_payload` (F46) → typed 400, F51 fallback to plain.
4. **`dsh-lite-cpp/ → golem/` rename** — still undecided from session 1.
5. **Push to LeastGen/golem** — 38 commits ahead. NEVER push without explicit user go.
6. Parallel payload-draft workstream (`grammar.cpp`, `test_grammar.cpp`,
   `payload_draft_run.cpp`, `.gitignore`, and an unstaged hunk in `CMakeLists.txt`) is
   still mid-flight and was deliberately left untouched all session. Stage paths
   explicitly before any core commit so it isn't swept in.

## 9. Key facts a successor needs

- `test_verify_promotion` is ctest member 14; run `ctest` in `dsh-lite-cpp/build`.
- Bench runner rebuild after any core change: `bench/iso_harness/build_golem.sh`
  (compiles `golem_runner.cpp` against `dsh-lite-cpp/build/libdshlite.a`, -Werror).
- Iso run needs the engine AND a scrubbed env:
  `env -u OPENROUTER_API_KEY -u OPENAI_API_KEY -u ANTHROPIC_API_KEY -u TYPESAFE_API_KEY ./run_iso_bench.sh golem T2,T3`
- Engine start: `cd colibri/c && env -u PYTHONPATH CONDA_NO_PLUGINS=true python3.12 ./coli serve
  --model ~/models/olmoe_merged --model-id olmoe-leaf --port 8081 --host 127.0.0.1 --no-think`
- Flaw registers: `docs/colibri-roadmap.md` F1-F86; `bench/h2h/README.md` F62-F73;
  `bench/iso_harness/README.md` F87-F98. **Register new flaws there before fixing.**
- Doctrine (user-enforced): audit flaws BEFORE executing; regression tests must fail on
  old code; honest failures preserved not softened; commits only when cleared.
- **Bench numbers do not move just because the runtime improved** — the harness must feed
  `tasks.json` ground truth into the hook (done for golem; NOT done for the other arms).
