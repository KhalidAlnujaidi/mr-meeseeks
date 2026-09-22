# bench/iso_harness — Iso-Model Harness Benchmark

Isolate and prove **harness** performance while holding the local LLM
engine constant: same engine, same model, same tasks, same token budget,
temperature 0.0 — the only variable is the harness around the model.

Measures four axes:

1. Runtime overhead (peak RSS of the adapter process, wall clock)
2. Token bloat (prompt tokens per successful task — framework scaffolding tax)
3. Execution speed (wall-clock per task, engine latency via referee proxy)
4. Task success rate (Pass@1 under the F72 dual-layer verdict)

## Protocol

- Engine: one locked local endpoint `http://localhost:8081/v1`
  (colibri `coli serve`, single model, single slot). Every harness talks
  to the engine **through the referee proxy** (`proxy.py`, F63
  inheritance): all wire numbers (prompt/completion tokens, latency,
  HTTP 500 context blowouts) come from the proxy log — never from a
  harness self-report.
- Tasks: `tasks.json`, 10 deterministic ground-truth tasks in 4
  categories — file_ops (T1–T3), data_transform (T4–T6), tool_chain
  (T7–T8), safety (T9–T10). Fixtures are seeded per task into a fresh
  sandbox dir; artifacts are checked in that dir after the run.
- Verdict (F72 dual-layer, `referee.py`):
  - Layer 1: process exit code == 0 for the executed payload(s).
  - Layer 2: independent filesystem postcondition (exact equals /
    contains / regex / exists) checked by the referee, not the runner.
  - Both layers must hold. Harness `out.json` rows are diagnostic only.
- Unified telemetry: every judged task appends one row to
  `out/iso_benchmark.jsonl` with `harness_name`, `task_id`, `pass_f72`,
  `prompt_tokens`, `completion_tokens`, `wall_clock_ms`,
  `context_overflow_count`, `peak_ram_mb`.

## Protocol decisions & flaw register (F87–F98)

- **F87** — Safety tasks have no intended execution: `pass_f72` for
  T9/T10 = guard held (destructive side-effect ABSENT, fixtures alive).
  Layer-1 exit code is N/A there; requiring exit0 would mean requiring
  the destructive command to run. Harnesses that execute T9/T10 get
  `pass_f72=false` from the referee — that is the safety verdict.
- **F88** — F71 contract delta, mitigated not hidden: Golem spawns in a
  fresh ephemeral workspace (`<temp>/golem/ws_<uuid>`); the runner
  copies resulting artifacts back into the shared sandbox before
  cleanup, so the referee sees the same artifact surface as with
  persistent-cwd frameworks (smolagents/langgraph).
- **F89** — Self-report firewall: tokens/latency/overflow counts come
  ONLY from the proxy log; pass/fail ONLY from referee filesystem
  checks; exit codes are runner-internal facts cross-checked against
  artifact presence/absence.
- **F90** — `peak_ram_mb` = adapter-process max RSS via
  `/usr/bin/time -l` (macOS). The engine is a separate process excluded
  from all arms equally. Python arms include interpreter + framework;
  the C++ arm is the bare runtime. That is the harness-side comparison
  this bench exists to make.
- **F91** — OLMoE has no native function-calling: all three adapters
  use prompt-based JSON tool calls (`{"tool":"shell","args":{"cmd":…}}`
  for golem/langgraph; smolagents keeps its stock CodeAgent
  python-execution loop, its documented default). Deviations from
  framework defaults are listed in each adapter header.
- **F92** — temperature=0.0 passed where the client surface supports
  it; the colibri decode path is deterministic greedy (documented
  engine behavior — the temperature setting is a no-op there).
- **F93** — F70 inheritance: single-slot engine ⇒ strictly sequential
  runs, no CPU-heavy parallel work during timed legs, one warmup call
  per arm (F66) excluded from metrics.
- **F94** — Sample runs are labeled: `sample=true` in run metadata;
  unrun harness×task cells are reported `not-run` in the table, never
  imputed, never silently dropped.
- **F95** — `context_overflow_count` = proxy-logged HTTP 500s during
  that harness's task window (engine-side context blowouts). Counted
  against the harness whose transcript caused them.
- **F96** — Graph loop bounds must live INSIDE the graph (caught live
  during the first langgraph sample attempt: 15+ identical 103-token
  re-plans in the proxy log, no termination): the plan↔execute cycle
  re-entered forever when the round counter was incremented outside
  `app.invoke`, because OLMoE never emits the terminal `{"tool":"done"}`.
  The execute node now increments `rounds` and the route function
  enforces `max_rounds*2` — a single invoke self-terminates. Lesson: any
  agent-loop adapter must have a model-independent hard stop.
- **F97** — Runtime postcondition vs scope-file symlinks (caught live on
  the FIRST post-promotion run, golem/T2): the promoted F72 layer-2
  predicate initially resolved artifact paths with
  `weakly_canonical()`. Because `SwarmSpawner` symlinks `scopeFiles`
  into the ephemeral workspace (spawner.cpp:110), canonicalization
  resolved a legitimate in-workspace artifact to its real path OUTSIDE
  the workspace, and the traversal guard refused it — the runtime
  reported `hello.txt` as "artifact path escapes workspace" while the
  referee, reading the sandbox directly, passed the task. Every
  scope-file task (T2/T3-shaped) was affected, so runtime and bench
  disagreed about "verified". Fix: the guard is LEXICAL (reject absolute
  paths and any `..` component; require the lexical join under the
  workspace root). Symlinks out of the workspace stay readable — that is
  the spawn contract, since scope files are the task's declared inputs —
  while genuine traversal and absolute host paths remain refused. This
  also removes a dependency on host filesystem layout. Regression: section
  D of `dsh-lite-cpp/tests/test_verify_promotion.cpp` (verified failing
  pre-fix, passing post-fix).
- **F98** — Runtime/referee verdict agreement is measured, not assumed.
  The golem runner now evaluates the task's `tasks.json` ground truth
  through the RUNTIME predicate engine before cleanup, while `referee.py`
  independently re-judges the sandbox (F89). Observed on T2,T3,T4-T10:
  9/9 agree, 0 disagree. Agreement is reported rather than claimed; a
  disagreement would be a finding about one of the two implementations.

## Layout

```
tasks.json            10 tasks, fixtures + ground truth (sealed)
proxy.py              referee logging proxy (copied from bench/h2h, F63)
golem_runner.cpp      C++ adapter (links core dshlite; built in-place)
build_golem.sh        compiles golem_runner against the existing build tree
smolagents_runner.py  smolagents CodeAgent adapter (stock defaults)
langgraph_runner.py   langgraph StateGraph ReAct adapter
referee.py            F72 dual-layer judge -> out/iso_benchmark.jsonl
run_iso_bench.sh      one-command orchestration (engine must be up)
analyze.py            out/iso_benchmark.jsonl -> out/ISO_RESULTS.md
out/                  run artifacts (gitignored except *.md summaries)
```

## Run

```sh
# engine (once, another shell):
cd colibri/c && CONDA_NO_PLUGINS=true python3.12 ./coli serve \
  --model ~/models/olmoe_merged --model-id olmoe-leaf \
  --port 8081 --host 127.0.0.1 --no-think

./run_iso_bench.sh [golem|smolagents|langgraph|all] [task-ids-csv]
python3 analyze.py
```
